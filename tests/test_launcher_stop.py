from paths import ROOT
import ctypes
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher/uncharted_launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


def window(process=None, job=None):
    instance = launcher.Launcher.__new__(launcher.Launcher)
    instance.process = process
    instance.job = job
    instance.stopping = False
    instance.stop_button = mock.Mock()
    instance.status = mock.Mock()
    instance.messagebox = mock.Mock()
    return instance


class GameJobTests(unittest.TestCase):
    def kernel(self, handle=123, assigned=True, terminated=True):
        kernel32 = mock.Mock()
        kernel32.CreateJobObjectW.return_value = handle
        kernel32.AssignProcessToJobObject.return_value = assigned
        kernel32.TerminateJobObject.return_value = terminated
        return kernel32

    def test_assigned_job_terminates_and_closes(self):
        kernel32 = self.kernel()
        process = mock.Mock(_handle=456)
        with mock.patch.object(launcher.ctypes, 'windll', create=True) as windll:
            windll.kernel32 = kernel32
            job = launcher.GameJob(process)
            self.assertTrue(job.terminate())
            job.close()
        kernel32.AssignProcessToJobObject.assert_called_once()
        kernel32.TerminateJobObject.assert_called_once()
        kernel32.CloseHandle.assert_called_once()
        self.assertIsNone(job.handle)

    def test_failed_assignment_releases_handle_and_disables_job(self):
        kernel32 = self.kernel(assigned=False)
        process = mock.Mock(_handle=456)
        with mock.patch.object(launcher.ctypes, 'windll', create=True) as windll:
            windll.kernel32 = kernel32
            job = launcher.GameJob(process)
            self.assertFalse(job.terminate())
        kernel32.CloseHandle.assert_called_once()
        kernel32.TerminateJobObject.assert_not_called()
        self.assertIsNone(job.handle)

    def test_failed_job_termination_is_reported(self):
        kernel32 = self.kernel(terminated=False)
        process = mock.Mock(_handle=456)
        with mock.patch.object(launcher.ctypes, 'windll', create=True) as windll:
            windll.kernel32 = kernel32
            job = launcher.GameJob(process)
            self.assertFalse(job.terminate())

    @unittest.skipUnless(os.name == 'nt', 'Windows job-object integration test')
    def test_real_job_stops_parent_and_child(self):
        child_code = 'import time; time.sleep(60)'
        parent_code = (
            'import subprocess, sys, time; sys.stdin.readline(); '
            'child=subprocess.Popen([sys.executable, "-c", sys.argv[1]]); '
            'print(child.pid, flush=True); time.sleep(60)')
        parent = subprocess.Popen(
            [sys.executable, '-c', parent_code, child_code], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), text=True)
        job = launcher.GameJob(parent)
        child_handle = None
        try:
            self.assertTrue(job.handle, 'Windows refused the job assignment')
            parent.stdin.write('\n')
            parent.stdin.flush()
            child_pid = int(parent.stdout.readline().strip())
            child_handle = ctypes.windll.kernel32.OpenProcess(0x00100000, False, child_pid)
            self.assertTrue(child_handle)
            self.assertTrue(job.terminate())
            parent.wait(timeout=5)
            result = ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(child_handle), 5000)
            self.assertEqual(result, 0, 'the job left the child process running')
        finally:
            if child_handle:
                ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(child_handle))
            if parent.poll() is None:
                launcher.terminate_process_tree(parent)
            job.close()
            parent.stdin.close()
            parent.stdout.close()
            parent.stderr.close()


class StopActionTests(unittest.TestCase):
    def test_job_termination_updates_launcher_state(self):
        job = mock.Mock()
        job.terminate.return_value = True
        process = mock.Mock()
        launcher_window = window(process, job)
        with mock.patch.object(launcher, 'terminate_process_tree') as fallback:
            launcher_window.stop()
        fallback.assert_not_called()
        self.assertTrue(launcher_window.stopping)
        launcher_window.stop_button.configure.assert_called_once_with(state='disabled')
        launcher_window.status.configure.assert_called_once_with(
            text='Stopping the game…', fg=launcher.GOLD)

    def test_failed_job_uses_process_tree_fallback(self):
        job = mock.Mock()
        job.terminate.return_value = False
        process = mock.Mock()
        launcher_window = window(process, job)
        with mock.patch.object(launcher, 'terminate_process_tree') as fallback:
            launcher_window.stop()
        fallback.assert_called_once_with(process)
        self.assertTrue(launcher_window.stopping)

    def test_missing_job_uses_process_tree_fallback(self):
        process = mock.Mock()
        launcher_window = window(process)
        with mock.patch.object(launcher, 'terminate_process_tree') as fallback:
            launcher_window.stop()
        fallback.assert_called_once_with(process)
        self.assertTrue(launcher_window.stopping)

    def test_failed_fallback_keeps_stop_available_and_reports_error(self):
        process = mock.Mock()
        launcher_window = window(process)
        with mock.patch.object(launcher, 'terminate_process_tree', side_effect=OSError('access denied')):
            launcher_window.stop()
        self.assertFalse(launcher_window.stopping)
        self.assertEqual([mock.call(state='disabled'), mock.call(state='normal')],
                         launcher_window.stop_button.configure.call_args_list)
        launcher_window.messagebox.showerror.assert_called_once_with(
            launcher.NAME, 'Could not stop the game: access denied')

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher-stop integration test')
    def test_stop_fallback_ends_real_process_tree(self):
        child_code = 'import time; time.sleep(60)'
        parent_code = (
            'import subprocess, sys, time; '
            'child=subprocess.Popen([sys.executable, "-c", sys.argv[1]]); '
            'print(child.pid, flush=True); time.sleep(60)')
        parent = subprocess.Popen(
            [sys.executable, '-c', parent_code, child_code],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), text=True)
        child_handle = None
        try:
            child_pid = int(parent.stdout.readline().strip())
            child_handle = ctypes.windll.kernel32.OpenProcess(0x00100000, False, child_pid)
            self.assertTrue(child_handle)
            job = mock.Mock()
            job.terminate.return_value = False
            launcher_window = window(parent, job)
            launcher_window.stop()
            parent.wait(timeout=5)
            result = ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(child_handle), 5000)
            self.assertEqual(result, 0, 'Stop left the child process running')
            self.assertTrue(launcher_window.stopping)
            launcher_window.messagebox.showerror.assert_not_called()
        finally:
            if child_handle:
                ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(child_handle))
            if parent.poll() is None:
                launcher.terminate_process_tree(parent)
            parent.stdout.close()
            parent.stderr.close()

    def test_no_process_is_a_no_op(self):
        launcher_window = window()
        launcher_window.stop()
        launcher_window.stop_button.configure.assert_not_called()


class ProcessTreeFallbackTests(unittest.TestCase):
    def test_already_exited_process_needs_no_taskkill(self):
        process = mock.Mock()
        process.poll.return_value = 0
        with mock.patch.object(launcher.subprocess, 'run') as run:
            launcher.terminate_process_tree(process)
        run.assert_not_called()

    def test_taskkill_targets_pid_and_descendants(self):
        process = mock.Mock(pid=2468)
        process.poll.return_value = None
        completed = mock.Mock(returncode=0, stdout=b'')
        with mock.patch.object(launcher.subprocess, 'run', return_value=completed) as run, \
                mock.patch.dict(launcher.os.environ, {'SystemRoot': r'C:\Windows'}):
            launcher.terminate_process_tree(process)
        command = run.call_args.args[0]
        self.assertEqual(Path(command[0]), Path(r'C:\Windows\System32\taskkill.exe'))
        self.assertEqual(command[1:], ['/PID', '2468', '/T', '/F'])
        self.assertEqual(run.call_args.kwargs['timeout'], 10)

    def test_taskkill_failure_raises_its_output(self):
        process = mock.Mock(pid=2468)
        process.poll.return_value = None
        completed = mock.Mock(returncode=1, stdout=b'Access is denied.')
        with mock.patch.object(launcher.subprocess, 'run', return_value=completed):
            with self.assertRaisesRegex(OSError, 'Access is denied'):
                launcher.terminate_process_tree(process)

    @unittest.skipUnless(os.name == 'nt', 'Windows process-tree integration test')
    def test_real_taskkill_stops_parent_and_child(self):
        child_code = 'import time; time.sleep(60)'
        parent_code = (
            'import subprocess, sys, time; '
            'child=subprocess.Popen([sys.executable, "-c", sys.argv[1]]); '
            'print(child.pid, flush=True); time.sleep(60)')
        parent = subprocess.Popen(
            [sys.executable, '-c', parent_code, child_code],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), text=True)
        child_handle = None
        try:
            child_pid = int(parent.stdout.readline().strip())
            child_handle = ctypes.windll.kernel32.OpenProcess(0x00100000, False, child_pid)
            self.assertTrue(child_handle)
            launcher.terminate_process_tree(parent)
            parent.wait(timeout=5)
            result = ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(child_handle), 5000)
            self.assertEqual(result, 0, 'taskkill left the child process running')
        finally:
            if child_handle:
                ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(child_handle))
            if parent.poll() is None:
                launcher.terminate_process_tree(parent)
            parent.stdout.close()
            parent.stderr.close()


if __name__ == '__main__':
    unittest.main()

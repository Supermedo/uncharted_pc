"""Windows restart tests for run.py --after."""
from contextlib import redirect_stderr
import ctypes
import importlib.util
import io
import subprocess
import sys
import unittest
from unittest import mock

from paths import ROOT


spec = importlib.util.spec_from_file_location('bb_run', ROOT / 'run.py')
bb_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bb_run)


@unittest.skipUnless(sys.platform == 'win32', 'Windows process handles only')
class PreviousProcessWaitTests(unittest.TestCase):
    def test_waits_for_a_real_child_to_exit(self):
        with subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(0.2)']) as child:
            bb_run.wait_for(child.pid)
            self.assertEqual(child.poll(), 0)

    def test_missing_process_allows_restart(self):
        with mock.patch('_winapi.OpenProcess', side_effect=ctypes.WinError(87)), \
                mock.patch('_winapi.WaitForSingleObject') as wait:
            bb_run.wait_for(4321)
            wait.assert_not_called()

    def test_invalid_pid_is_rejected_before_opening(self):
        for pid in (0, -1, 2**32):
            with self.subTest(pid=pid), mock.patch('_winapi.OpenProcess') as open_process:
                with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    bb_run.wait_for(pid)
                open_process.assert_not_called()

    def test_missing_or_malformed_pid_stops_restart(self):
        for args in (['--after'], ['--after', 'abc']):
            with self.subTest(args=args), mock.patch.object(sys, 'argv', ['run.py', *args]), \
                    mock.patch.object(bb_run, 'wait_for') as wait:
                with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    bb_run.main()
                wait.assert_not_called()

    def test_access_denied_stops_restart(self):
        stderr = io.StringIO()
        with mock.patch('_winapi.OpenProcess', side_effect=ctypes.WinError(5)), \
                redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            bb_run.wait_for(4321)
        self.assertEqual(raised.exception.code, 1)
        self.assertIn('WinError 5', stderr.getvalue())

    def test_unsuccessful_wait_closes_handle_and_stops_before_preparation(self):
        for outcome, diagnostic in [(258, '30 seconds'), (ctypes.WinError(6), 'WinError 6'),
                                    (128, '128')]:
            with self.subTest(outcome=outcome):
                stderr = io.StringIO()
                with mock.patch.object(sys, 'argv', ['run.py', '--after', '4321']), \
                        mock.patch('_winapi.OpenProcess', return_value=123), \
                        mock.patch('_winapi.WaitForSingleObject', side_effect=[outcome]) as wait, \
                        mock.patch('_winapi.CloseHandle') as close, \
                        mock.patch.object(bb_run, 'prepare') as prepare, \
                        redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
                    bb_run.main()
                self.assertEqual(raised.exception.code, 1)
                self.assertIn(diagnostic, stderr.getvalue())
                wait.assert_called_once_with(123, 30000)
                close.assert_called_once_with(123)
                prepare.assert_not_called()


if __name__ == '__main__':
    unittest.main()

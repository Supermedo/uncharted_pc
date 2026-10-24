"""Tests for waiting on the previous Windows game process."""
from contextlib import redirect_stderr
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


class FakeKernel32:
    def __init__(self, handle=123, wait_result=0, error=0):
        self.handle = handle
        self.wait_result = wait_result
        self.error = error
        self.open_calls = []
        self.wait_calls = []
        self.close_calls = []

    def OpenProcess(self, access, inherit, pid):
        self.open_calls.append((access, inherit, pid))
        return self.handle

    def WaitForSingleObject(self, handle, timeout):
        self.wait_calls.append((handle, timeout))
        return self.wait_result

    def CloseHandle(self, handle):
        self.close_calls.append(handle)
        return True

    def GetLastError(self):
        return self.error


class PreviousProcessWaitTests(unittest.TestCase):
    def run_wait(self, kernel32, pid=4321):
        with mock.patch.object(bb_run.ctypes, 'windll', create=True) as windll:
            windll.kernel32 = kernel32
            bb_run.wait_for(pid)

    def assert_wait_fails(self, kernel32, message):
        stderr = io.StringIO()
        with self.assertRaises(SystemExit) as raised, redirect_stderr(stderr):
            self.run_wait(kernel32)
        self.assertEqual(raised.exception.code, 1)
        self.assertIn(message, stderr.getvalue())

    def test_waits_for_the_previous_process_and_closes_its_handle(self):
        kernel32 = FakeKernel32()

        self.run_wait(kernel32)

        self.assertEqual(kernel32.open_calls, [(0x00100000, False, 4321)])
        self.assertEqual(kernel32.wait_calls, [(123, 30000)])
        self.assertEqual(kernel32.close_calls, [123])

    def test_process_that_already_exited_needs_no_wait(self):
        kernel32 = FakeKernel32(handle=0, error=87)

        self.run_wait(kernel32)

        self.assertEqual(kernel32.wait_calls, [])
        self.assertEqual(kernel32.close_calls, [])

    def test_open_failure_is_reported(self):
        kernel32 = FakeKernel32(handle=0, error=5)

        self.assert_wait_fails(kernel32, 'OpenProcess failed (Windows error 5)')

    def test_timeout_prevents_a_second_instance(self):
        kernel32 = FakeKernel32(wait_result=258)

        self.assert_wait_fails(kernel32, 'did not exit within 30 seconds')
        self.assertEqual(kernel32.close_calls, [123])

    def test_wait_failure_is_reported_and_the_handle_is_closed(self):
        kernel32 = FakeKernel32(wait_result=0xffffffff, error=6)

        self.assert_wait_fails(kernel32, 'WaitForSingleObject failed (Windows error 6)')
        self.assertEqual(kernel32.close_calls, [123])

    def test_unexpected_wait_status_is_reported(self):
        kernel32 = FakeKernel32(wait_result=128)

        self.assert_wait_fails(kernel32, 'unexpected status 128')
        self.assertEqual(kernel32.close_calls, [123])

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process handles only')
    def test_waits_for_a_real_process(self):
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(0.1)'])
        try:
            bb_run.wait_for(process.pid)
            self.assertEqual(process.wait(timeout=1), 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == '__main__':
    unittest.main()

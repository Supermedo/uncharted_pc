import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
from uncharted_launcher import select_run_problem


class LauncherDiagnosticsTests(unittest.TestCase):
    def test_fault_replaces_earlier_feature_warning(self):
        warning = 'DLSS: warning: Feature requirement query failed'
        fault = 'Fault: exception 0xc000001d'

        self.assertEqual(fault, select_run_problem(warning, fault))

    def test_terminal_reason_replaces_fault(self):
        fault = 'Fault: read access to ffffffffffffffff'
        stop = 'STOP: invalid render frame parameters'

        self.assertEqual(stop, select_run_problem(fault, stop))

    def test_generic_failure_remains_as_fallback(self):
        warning = 'DLSS: warning: Feature requirement query failed'

        self.assertEqual(warning, select_run_problem('', warning))
        self.assertEqual(warning, select_run_problem(warning, 'ordinary log message'))
        self.assertEqual(warning, select_run_problem(warning, 'GPU [Render] upload failed'))

    def test_latest_reason_wins_at_the_same_priority(self):
        first = 'ERROR first failure'
        latest = 'Fault: exception 0xc000001d'

        self.assertEqual(latest, select_run_problem(first, latest))


if __name__ == '__main__':
    unittest.main()

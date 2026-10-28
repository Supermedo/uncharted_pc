"""The brightness flicker fix (BB_READBACK_SMALL_BUFFERS) is on by default for Uncharted 2 only."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT))
import run  # noqa: E402

U1, U2, U3 = run.EXECUTABLES['u1'], run.EXECUTABLES['u2'], run.EXECUTABLES['u3']


class BrightnessFixDefault(unittest.TestCase):
    def test_uncharted_2_gets_the_fix_by_default(self):
        self.assertEqual(run.brightness_fix_default(None, U2), '1')

    def test_an_explicit_player_value_wins(self):
        self.assertEqual(run.brightness_fix_default('0', U2), '0')
        self.assertEqual(run.brightness_fix_default('1', U3), '1')
        self.assertEqual(run.brightness_fix_default('0', U1), '0')

    def test_uncharted_1_and_3_stay_unchanged(self):
        self.assertIsNone(run.brightness_fix_default(None, U1))
        self.assertIsNone(run.brightness_fix_default(None, U3))

    def test_unknown_executable_stays_unchanged(self):
        self.assertIsNone(run.brightness_fix_default(None, 'unknown.elf'))


if __name__ == '__main__':
    unittest.main()

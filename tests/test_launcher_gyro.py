"""Launcher gyro settings (Controls page): read from and written to bbport.ini, which the game reads."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


class GyroIniTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / 'bbport.ini'
        self.original = LAUNCHER.ini_path
        LAUNCHER.ini_path = lambda: self.path

    def tearDown(self):
        LAUNCHER.ini_path = self.original
        self.directory.cleanup()

    def test_reads_the_games_values(self):
        self.path.write_text('show_fps=1\ngyro_aim=1\ngyro_builtin=0\ngyro_button=-3\ngyro_button_mode=2\n'
                             'gyro_sensitivity=2.70\ngyro_invert_x=1\ngyro_invert_y=0\n'
                             'gyro_speed_deadzone=0.50\ngyro_precision_speed=1.25\n', encoding='utf-8')
        self.assertEqual(LAUNCHER.gyro_from_ini(), {
            'gyro_aim': True, 'gyro_builtin': False, 'gyro_button': '-3', 'gyro_button_mode': '2',
            'gyro_sensitivity': '2.70', 'gyro_invert_x': True, 'gyro_invert_y': False,
            'gyro_speed_deadzone': '0.50', 'gyro_precision_speed': '1.25'})

    def test_missing_file_or_bad_values_leave_the_defaults(self):
        self.assertEqual(LAUNCHER.gyro_from_ini(), {})
        self.path.write_text('gyro_sensitivity=fast\ngyro_button=L2\n', encoding='utf-8')
        self.assertEqual(LAUNCHER.gyro_from_ini(), {})

    def test_writes_every_key_and_keeps_the_rest(self):
        self.path.write_text('# the game\nshow_fps=1\ngyro_sensitivity=1.00\ngyro_bias=0.1 0.2 0.3\n',
                             encoding='utf-8')
        LAUNCHER.write_gyro_ini({**LAUNCHER.APP_DEFAULTS, 'gyro_sensitivity': '2.7', 'gyro_aim': True,
                                 'gyro_button': '9', 'gyro_precision_speed': '0.8'})
        lines = self.path.read_text(encoding='utf-8').splitlines()
        self.assertEqual(lines[:4], ['# the game', 'show_fps=1', 'gyro_sensitivity=2.70', 'gyro_bias=0.1 0.2 0.3'])
        for line in ('gyro_aim=1', 'gyro_builtin=1', 'gyro_button=9', 'gyro_button_mode=0', 'gyro_invert_x=0',
                     'gyro_invert_y=0', 'gyro_speed_deadzone=0.36', 'gyro_precision_speed=0.80'):
            self.assertIn(line, lines)
        self.assertEqual(sum(line.startswith('gyro_sensitivity=') for line in lines), 1)

    def test_written_values_stay_in_the_games_ranges(self):
        LAUNCHER.write_gyro_ini({**LAUNCHER.APP_DEFAULTS, 'gyro_sensitivity': '50', 'gyro_speed_deadzone': '-1',
                                 'gyro_precision_speed': 'x', 'gyro_button_mode': '7'})
        lines = self.path.read_text(encoding='utf-8').splitlines()
        for line in ('gyro_sensitivity=10.00', 'gyro_speed_deadzone=0.00', 'gyro_precision_speed=0.75',
                     'gyro_button_mode=0'):
            self.assertIn(line, lines)

    def test_conversion_style_reads_writes_and_stays_in_range(self):
        self.path.write_text('gyro_space=4\ngyro_local_angle=30.5\ngyro_complement=0.25\n', encoding='utf-8')
        values = LAUNCHER.gyro_from_ini()
        self.assertEqual((values['gyro_space'], values['gyro_local_angle'], values['gyro_complement']),
                         ('4', '30.50', '0.25'))
        LAUNCHER.write_gyro_ini({**LAUNCHER.APP_DEFAULTS, 'gyro_space': '9', 'gyro_local_angle': '-200',
                                 'gyro_complement': '300'})
        lines = self.path.read_text(encoding='utf-8').splitlines()
        for line in ('gyro_space=4', 'gyro_local_angle=-180.00', 'gyro_complement=100.00'):
            self.assertIn(line, lines)
        LAUNCHER.write_gyro_ini({**LAUNCHER.APP_DEFAULTS, 'gyro_rotate': '-200'})
        self.assertIn('gyro_rotate=-180.00', self.path.read_text(encoding='utf-8').splitlines())
        LAUNCHER.write_gyro_ini({**LAUNCHER.APP_DEFAULTS, 'gyro_space': '5'})
        self.assertIn('gyro_space=5', self.path.read_text(encoding='utf-8').splitlines())

    def test_conversion_styles_match_the_games_numbers(self):
        self.assertEqual([value for value, _name in LAUNCHER.GYRO_SPACES], ['0', '1', '2', '3', '4', '5'])
        self.assertEqual(LAUNCHER.APP_DEFAULTS['gyro_space'], '4')  # Player Space
        self.assertEqual(LAUNCHER.APP_DEFAULTS['gyro_sensitivity'], '2.70')
        self.assertEqual(LAUNCHER.GYRO_NUMBERS['gyro_sensitivity'][2], '2.70')

    def test_steam_dots_per_360_follows_the_mouse_sensitivity(self):
        self.assertEqual(LAUNCHER.steam_dots_per_360({'mouse_sensitivity': '1.0'}), 2400)
        self.assertEqual(LAUNCHER.steam_dots_per_360({'mouse_sensitivity': '2.0'}), 1200)
        self.assertEqual(LAUNCHER.steam_dots_per_360({'mouse_sensitivity': 'x'}), 2400)


if __name__ == '__main__':
    unittest.main()

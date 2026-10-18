from paths import ROOT
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher/uncharted_launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


def settings(folder):
    return {**launcher.APP_DEFAULTS, 'game_dir': str(folder)}


class InputConfigurationFailureTests(unittest.TestCase):
    def test_game_environment_writes_and_exports_input_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'controls/input.ini'
            with mock.patch.dict(os.environ, {'BB_INPUT_CONFIG': str(path)}):
                env = launcher.game_environment(settings('game'))
            self.assertEqual(env['BB_INPUT_CONFIG'], str(path))
            self.assertIn('mouse_look=1\n', path.read_text(encoding='utf-8'))

    def test_game_environment_propagates_input_write_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            blocker = Path(directory) / 'not-a-directory'
            blocker.write_text('file', encoding='utf-8')
            with mock.patch.dict(os.environ, {'BB_INPUT_CONFIG': str(blocker / 'input.ini')}):
                with self.assertRaises(OSError):
                    launcher.game_environment(settings('game'))

    def test_launcher_reports_failure_without_starting_game(self):
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            (game / 'eboot.bin').touch()
            app = settings(game)
            window = launcher.Launcher.__new__(launcher.Launcher)
            window.process = None
            window.app = app
            window.collect = mock.Mock()
            window.set_log = mock.Mock()
            window.messagebox = mock.Mock()
            with mock.patch.object(launcher, 'write_input_ini', side_effect=OSError('access denied')), \
                    mock.patch.object(launcher.subprocess, 'Popen') as popen:
                window.play()
            popen.assert_not_called()
            window.messagebox.showerror.assert_called_once_with(
                launcher.NAME, 'Could not save the input configuration: access denied')

    def test_play_role_logs_failure_without_starting_game(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            app = settings(data)
            app['user_dir'] = str(data / 'user')
            stderr = io.StringIO()
            with mock.patch.object(launcher, 'attach_stdio'), \
                    mock.patch.object(launcher, 'write_input_ini', side_effect=OSError('access denied')), \
                    mock.patch.object(launcher.subprocess, 'Popen') as popen, \
                    mock.patch.object(launcher.sys, 'stderr', stderr):
                self.assertEqual(launcher.play_without_window(app), 1)
            popen.assert_not_called()
            message = 'Could not save the input configuration: access denied\n'
            self.assertEqual((data / 'user/last_run.log').read_text(encoding='utf-8'), message)
            self.assertEqual(stderr.getvalue(), message)


if __name__ == '__main__':
    unittest.main()

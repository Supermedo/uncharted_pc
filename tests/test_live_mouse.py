"""Live launcher mouse controls: migration, atomic writes and running-game updates."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from paths import ROOT

SPEC = importlib.util.spec_from_file_location('live_mouse_launcher', ROOT / 'launcher/uncharted_launcher.py')
launcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(launcher)


class LiveMouseTests(unittest.TestCase):
    def test_legacy_sensitivity_is_inherited_by_all_four_axes(self):
        with mock.patch.object(launcher, 'load_json', return_value={'mouse_sensitivity': '1.50'}):
            settings = launcher.load_settings()
        self.assertEqual([settings[key] for key in launcher.MOUSE_SENS_KEYS], ['1.50'] * 4)

    def test_explicit_axes_override_the_legacy_value(self):
        saved = {'mouse_sensitivity': '1.50', 'mouse_look_sensitivity_x': '2.5',
                 'mouse_aim_sensitivity_y': '0.25'}
        with mock.patch.object(launcher, 'load_json', return_value=saved):
            settings = launcher.load_settings()
        self.assertEqual([settings[key] for key in launcher.MOUSE_SENS_KEYS],
                         ['2.50', '1.50', '1.50', '0.25'])

    def test_four_axes_are_written_independently(self):
        settings = dict(zip(launcher.MOUSE_SENS_KEYS, ('2.0', '3.0', '0.5', '4.0')))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            with mock.patch.object(launcher, 'input_ini_path', return_value=path):
                launcher.write_input_ini(settings)
            text = path.read_text(encoding='utf-8')
        for key, value in zip(launcher.MOUSE_SENS_KEYS, ('2.00', '3.00', '0.50', '4.00')):
            self.assertIn(f'{key}={value}\n', text)

    def test_invalid_and_non_finite_sensitivities_have_safe_values(self):
        for value in ('nan', 'inf', '-inf', '', None, '-1', '0'):
            with self.subTest(value=value):
                self.assertEqual(launcher.mouse_sensitivity(
                    {'mouse_sensitivity': '1.5', 'mouse_aim_sensitivity_x': value},
                    'mouse_aim_sensitivity_x'), '1.50')
        self.assertEqual(launcher.mouse_sensitivity({'mouse_sensitivity': '0.01'}), '0.10')
        self.assertEqual(launcher.mouse_sensitivity({'mouse_sensitivity': '100'}), '10.00')

    def test_failed_atomic_replace_keeps_the_previous_complete_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            path.write_text('previous complete controls\n', encoding='utf-8')
            with mock.patch.object(launcher.os, 'replace', side_effect=PermissionError('busy')):
                with self.assertRaises(PermissionError):
                    launcher.atomic_write_text(path, 'new controls\n')
            self.assertEqual(path.read_text(encoding='utf-8'), 'previous complete controls\n')
            self.assertEqual(list(Path(directory).iterdir()), [path])

    def window(self, settings):
        window = launcher.Launcher.__new__(launcher.Launcher)
        window.app = dict(launcher.APP_DEFAULTS)
        window.current = mock.Mock(return_value={**window.app, **settings})
        window.mouse_apply_job = None
        window.mouse_status = mock.Mock()
        window.root = mock.Mock()
        window.process = mock.Mock()  # a running game must not be restarted by a slider
        return window

    def test_running_game_gets_updates_and_settings_persist_without_restart(self):
        settings = dict(zip(launcher.MOUSE_SENS_KEYS, ('2', '3', '.5', '4')))
        window = self.window(settings)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            saved = Path(directory) / 'settings.json'
            with mock.patch.object(launcher, 'input_ini_path', return_value=path), \
                    mock.patch.object(launcher, 'CONFIG_FILE', saved), \
                    mock.patch.object(launcher.subprocess, 'Popen') as popen:
                window.apply_mouse_settings()
                text = path.read_text(encoding='utf-8')
                self.assertIn('mouse_aim_sensitivity_x=0.50\n', text)
                persisted = json.loads(saved.read_text(encoding='utf-8'))
                self.assertEqual(persisted['mouse_look_sensitivity_y'], '3.00')
                settings['mouse_aim_sensitivity_x'] = '1.75'
                window.current.return_value.update(settings)
                window.apply_mouse_settings()
                self.assertIn('mouse_aim_sensitivity_x=1.75\n', path.read_text(encoding='utf-8'))
                popen.assert_not_called()
        window.process.terminate.assert_not_called()
        window.process.kill.assert_not_called()
        self.assertEqual(window.app['mouse_aim_sensitivity_x'], '1.75')

    def test_drag_updates_are_throttled_without_waiting_for_drag_end(self):
        window = self.window({})
        window.root.after.return_value = 'pending'
        for _ in range(10):
            window.schedule_mouse_settings()
        window.root.after.assert_called_once_with(40, window.apply_mouse_settings)
        window.root.after_cancel.assert_not_called()

    def test_temporary_invalid_entry_does_not_replace_live_settings(self):
        window = self.window({'mouse_aim_sensitivity_y': ''})
        with mock.patch.object(launcher, 'write_input_ini') as write:
            window.apply_mouse_settings()
            write.assert_not_called()

    def test_busy_file_is_retried_without_restarting_the_game(self):
        window = self.window({})
        with mock.patch.object(launcher, 'write_input_ini', side_effect=PermissionError('busy')):
            window.apply_mouse_settings(retries=1)
        self.assertEqual(window.root.after.call_args.args[0], 50)
        self.assertIn('busy', window.mouse_status.configure.call_args.kwargs['text'])
        window.process.terminate.assert_not_called()


if __name__ == '__main__':
    unittest.main()

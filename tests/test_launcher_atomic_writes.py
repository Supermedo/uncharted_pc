"""Atomic launcher configuration write tests."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


class AtomicWriteTests(unittest.TestCase):
    def test_complete_file_replaces_previous_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            path.write_text('old', encoding='utf-8')

            LAUNCHER.atomic_write_text(path, 'new\n')

            self.assertEqual(path.read_text(encoding='utf-8'), 'new\n')
            self.assertEqual(list(path.parent.glob(f'.{path.name}.*.tmp')), [])

    def test_replace_failure_preserves_previous_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            path.write_text('old', encoding='utf-8')

            with mock.patch.object(LAUNCHER.os, 'replace', side_effect=OSError('busy')):
                with self.assertRaises(OSError):
                    LAUNCHER.atomic_write_text(path, 'new\n')

            self.assertEqual(path.read_text(encoding='utf-8'), 'old')
            self.assertEqual(list(path.parent.glob(f'.{path.name}.*.tmp')), [])

    def test_write_failure_before_replace_removes_temporary_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            path.write_bytes(b'old')
            with self.assertRaises(UnicodeEncodeError):
                LAUNCHER.atomic_write_text(path, '\ud800')
            self.assertEqual(path.read_bytes(), b'old')
            self.assertEqual(list(path.parent.iterdir()), [path])

    def test_ini_read_failure_preserves_existing_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bbport.ini'
            path.write_bytes(b'sensitivity=2\n')
            with mock.patch.object(LAUNCHER, 'ini_path', return_value=path), \
                    mock.patch.object(Path, 'read_text', side_effect=PermissionError):
                with self.assertRaises(PermissionError):
                    LAUNCHER.set_ini('show_fps', '1')
            self.assertEqual(path.read_bytes(), b'sensitivity=2\n')

    def test_ini_writer_uses_atomic_replace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bbport.ini'
            with mock.patch.object(LAUNCHER, 'ini_path', return_value=path), \
                    mock.patch.object(LAUNCHER, 'atomic_write_text') as write:
                LAUNCHER.set_ini('show_fps', '1')
        write.assert_called_once_with(path, 'show_fps=1\n')

    def test_input_writer_uses_atomic_replace(self):
        path = Path('input.ini')
        with mock.patch.object(LAUNCHER, 'input_ini_path', return_value=path), \
                mock.patch.object(LAUNCHER, 'atomic_write_text') as write:
            self.assertEqual(LAUNCHER.write_input_ini({}), path)
        self.assertEqual(write.call_args.args[0], path)
        self.assertIn('mouse_look=1\n', write.call_args.args[1])

    def test_launcher_settings_use_atomic_replace(self):
        window = LAUNCHER.Launcher.__new__(LAUNCHER.Launcher)
        window.vars = {}
        window.app = {'show_fps': False}
        with mock.patch.object(LAUNCHER, 'atomic_write_text') as write, \
                mock.patch.object(LAUNCHER, 'set_ini'):
            self.assertTrue(window.collect())
        write.assert_called_once_with(
            LAUNCHER.CONFIG_FILE, '{\n  "show_fps": false\n}\n')

    @unittest.skipUnless(os.name == 'nt', 'Windows file sharing')
    def test_play_reports_locked_file_and_can_retry_after_unlock(self):
        for name in ('settings.json', 'bbport.ini', 'input.ini'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                folder = Path(directory)
                (folder / 'eboot.bin').touch()
                window = LAUNCHER.Launcher.__new__(LAUNCHER.Launcher)
                window.app = {**LAUNCHER.APP_DEFAULTS, 'game_dir': str(folder)}
                window.vars = {}
                window.process = None
                window.messagebox = mock.Mock()
                window.set_log = mock.Mock()
                locked = folder / name
                locked.write_bytes(b'keep=1\n')
                with mock.patch.object(LAUNCHER, 'CONFIG_FILE', folder / 'settings.json'), \
                        mock.patch.dict(os.environ, {'BB_CONFIG': str(folder / 'bbport.ini'),
                                                     'BB_INPUT_CONFIG': str(folder / 'input.ini')}), \
                        mock.patch.object(LAUNCHER.subprocess, 'Popen') as spawn:
                    with locked.open('rb'):
                        window.play()
                    spawn.assert_not_called()
                    window.messagebox.showerror.assert_called_once()
                    self.assertIn(name, window.messagebox.showerror.call_args.args[1])
                    self.assertEqual(locked.read_bytes(), b'keep=1\n')
                    self.assertEqual(list(folder.glob('.*.tmp')), [])
                    self.assertTrue(window.collect())
                    LAUNCHER.write_input_ini(window.app)
                    self.assertNotEqual(locked.read_bytes(), b'keep=1\n')
                    if name == 'bbport.ini':
                        self.assertIn('keep=1', locked.read_text(encoding='utf-8').splitlines())


if __name__ == '__main__':
    unittest.main()

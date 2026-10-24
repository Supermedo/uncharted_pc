"""Atomic launcher configuration write tests."""
import importlib.util
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
            window.collect()
        write.assert_called_once_with(
            LAUNCHER.CONFIG_FILE, '{\n  "show_fps": false\n}\n')


if __name__ == '__main__':
    unittest.main()

"""Launcher input.ini generation tests."""
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


class InputConfigTests(unittest.TestCase):
    def test_capture_menu_and_brackets_in_both_binding_slots(self):
        window = LAUNCHER.Launcher.__new__(LAUNCHER.Launcher)
        window.root = Mock()
        window.key_buttons = {}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            for sym, code, binding in [('Menu', 93, 'Application'),
                                       ('[', 219, '['), (']', 221, ']'),
                                       ('bracketleft', 219, '['), ('bracketright', 221, ']')]:
                for slot, expected in enumerate((binding, f'Space|{binding}')):
                    with self.subTest(keysym=sym, slot=slot):
                        window.app = {}
                        window.capturing = ('cross', slot)
                        window.captured_key(SimpleNamespace(keysym=sym, keycode=code))
                        with patch.object(LAUNCHER, 'input_ini_path', return_value=path):
                            LAUNCHER.write_input_ini(window.app)
                        self.assertIn(f'key.cross={expected}', path.read_text().splitlines())

    def test_controller_vibration_defaults_on_and_can_be_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            original = LAUNCHER.input_ini_path
            LAUNCHER.input_ini_path = lambda: path
            try:
                LAUNCHER.write_input_ini({})
                text = path.read_text(encoding='utf-8')
                self.assertIn('pad_vibration=1\n', text)
                self.assertIn('pad_vibration_strength=100\n', text)

                LAUNCHER.write_input_ini({'pad_vibration': False, 'pad_vibration_strength': '45'})
                text = path.read_text(encoding='utf-8')
                self.assertIn('pad_vibration=0\n', text)
                self.assertIn('pad_vibration_strength=45\n', text)
            finally:
                LAUNCHER.input_ini_path = original

    def test_vibration_strength_is_clamped(self):
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': '-5'}), 0)
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': '250'}), 100)
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': 'invalid'}), 100)

    def test_controller_probe_response(self):
        info = LAUNCHER.parse_controller_probe(
            'connected=1\nname=DualSense Wireless Controller\nrumble=1\ntested=1\n')
        self.assertEqual(info, {'connected': True, 'name': 'DualSense Wireless Controller',
                                'rumble': True, 'tested': True})


if __name__ == '__main__':
    unittest.main()

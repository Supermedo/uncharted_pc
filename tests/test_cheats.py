"""Cheat toggles: launcher settings to environment, and the per-game filter in run.py."""
from paths import ROOT
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class CheatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.input_dir = tempfile.TemporaryDirectory()
        os.environ['BB_INPUT_CONFIG'] = str(Path(cls.input_dir.name) / 'input.ini')
        sys.path.insert(0, str(ROOT / 'launcher'))
        cls.launcher = load('uncharted_launcher', 'launcher/uncharted_launcher.py')
        cls.runner = load('uncharted_run', 'run.py')

    @classmethod
    def tearDownClass(cls):
        os.environ.pop('BB_INPUT_CONFIG', None)
        cls.input_dir.cleanup()

    def settings(self, **on):
        return {**self.launcher.APP_DEFAULTS, **on}

    def test_all_six_default_to_off(self):
        keys = [k for k in self.launcher.APP_DEFAULTS if '_infinite_' in k]
        self.assertEqual(len(keys), 6)
        self.assertFalse(any(self.launcher.APP_DEFAULTS[k] for k in keys))
        self.assertEqual(self.launcher.game_environment(self.settings())['BB_CHEATS'], '')

    def test_environment_lists_enabled_cheats_by_game(self):
        env = self.launcher.game_environment(self.settings(u2_infinite_ammo=True, u1_infinite_health=True))
        self.assertEqual(env['BB_CHEATS'], 'u1:health,u2:ammo')

    def test_run_keeps_only_the_running_games_cheats(self):
        cheats = 'u1:health,u2:ammo,u3:health,u3:ammo'
        self.assertEqual(self.runner.game_cheats(cheats, 'eboot.bin'), 'health')
        self.assertEqual(self.runner.game_cheats(cheats, 'big2-ps4_Shipping.elf'), 'ammo')
        self.assertEqual(self.runner.game_cheats(cheats, 'big3-ps4_Shipping.elf'), 'health,ammo')
        self.assertEqual(self.runner.game_cheats('', 'eboot.bin'), '')


def tiny_self(code):
    data = bytearray(0x1000 + 0x40)
    data[:4] = b'O='
    struct.pack_into('<H', data, 24, 1)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 0x40, 0x40)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 64,
                     b'ELF	', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 128, 1, 5, 0x1000, 0x100, 0, 0x40, 0x40, 0x1000)
    data[0x200:0x200 + len(code)] = code
    return bytes(data)


class PatchFileTests(unittest.TestCase):
    ORIGINAL, PATCHED, ADDRESS = '7535', 'eb35', 0x110

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / 'scripts'))
        cls.cheats = load('uncharted_cheats', 'scripts/cheats.py')

    def run_cheats(self, code, names='health', extra=None):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'game.elf'
            exe.write_bytes(tiny_self(code))
            sha = hashlib.sha256(exe.read_bytes()).hexdigest()
            table = {sha: {'health': (self.ADDRESS, self.ORIGINAL, self.PATCHED), **(extra or {})}}
            self.cheats.PATCHES, saved = table, self.cheats.PATCHES
            try:
                out = self.cheats.build(exe, directory, [n for n in names.split(',') if n])
            finally:
                self.cheats.PATCHES = saved
            self.assertEqual(out is not None, (Path(directory) / 'cheats.bin').exists())
            return out.read_bytes() if out else None

    def code(self, at, data):
        code = bytearray(0x40)
        code[at - 0x100:at - 0x100 + len(data)] = data
        return bytes(code)

    def test_expected_bytes_are_replaced_in_the_patch_file(self):
        blob = self.run_cheats(self.code(self.ADDRESS, bytes.fromhex(self.ORIGINAL)))
        self.assertEqual(blob, b'BBPATCH2' + struct.pack('<QQQQ', 0, 1, self.ADDRESS, 2) + bytes.fromhex(self.PATCHED))

    def test_unexpected_bytes_write_nothing(self):
        self.assertIsNone(self.run_cheats(self.code(self.ADDRESS, b't5')))

    def test_mismatched_cheat_does_not_drop_the_other(self):
        blob = self.run_cheats(self.code(self.ADDRESS, bytes.fromhex(self.ORIGINAL)), 'health,ammo',
                               {'ammo': (0x120, 'e800000000', '9090909090')})
        self.assertEqual(blob, b'BBPATCH2' + struct.pack('<QQQQ', 0, 1, self.ADDRESS, 2) + bytes.fromhex(self.PATCHED))

    def test_disabled_cheat_and_unknown_cheat_write_nothing(self):
        code = self.code(self.ADDRESS, bytes.fromhex(self.ORIGINAL))
        self.assertIsNone(self.run_cheats(code, ''))
        self.assertIsNone(self.run_cheats(code, 'ammo'))

    def test_unknown_executable_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'game.elf'
            exe.write_bytes(tiny_self(bytes(0x40)))
            self.assertIsNone(self.cheats.build(exe, directory, ['health', 'ammo']))
            self.assertFalse((Path(directory) / 'cheats.bin').exists())

    def test_stale_patch_file_is_removed_when_cheats_are_off(self):
        with tempfile.TemporaryDirectory() as directory:
            stale = Path(directory) / 'cheats.bin'
            stale.write_bytes(b'old')
            self.assertIsNone(self.cheats.build(Path(directory) / 'missing.elf', directory, []))
            self.assertFalse(stale.exists())

    def test_run_calls_the_imported_module_not_a_script_path(self):
        runner = load('uncharted_run', 'run.py')
        self.assertTrue(callable(runner.cheats.build))
        self.assertNotIn("run_script('cheats", (ROOT / 'run.py').read_text(encoding='utf-8'))

    def test_real_table_is_consistent(self):
        for sha, table in self.cheats.PATCHES.items():
            spans = []
            for name, (address, original, replacement) in table.items():
                self.assertIn(name, ('health', 'ammo'))
                self.assertEqual(len(bytes.fromhex(original)), len(bytes.fromhex(replacement)))
                self.assertNotEqual(original, replacement)
                spans.append((address, address + len(bytes.fromhex(original))))
            spans.sort()
            self.assertTrue(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), sha)
        self.assertEqual(len(self.cheats.PATCHES), 6)


if __name__ == '__main__':
    unittest.main()

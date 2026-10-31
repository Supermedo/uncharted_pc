"""Native startup arguments must preserve the loader's argv[0] and LoadExec options."""
from paths import ROOT
from pathlib import Path
import runpy
import tempfile
import unittest


class StartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = runpy.run_path(str(ROOT / 'run.py'))

    def arguments(self, env, exe, inherited=None):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source = None
            if inherited is not None:
                source = folder / 'loadexec.txt'
                source.write_text('\n'.join(inherited) + '\n', encoding='utf-8')
            result = self.runner['startup_arguments'](env, exe, folder, source)
            return result.read_text(encoding='utf-8').splitlines() if result else None

    def test_supported_initial_launch(self):
        for exe in ('eboot.bin', 'big2-ps4_Shipping.elf'):
            self.assertEqual(self.arguments({}, exe),
                             [f'/app0/{exe}', 'eboot.bin', 'bp_skipintromovie'])

    def test_loadexec_preserves_level_and_options(self):
        original = ['/app0/big2-ps4_Shipping.elf', 'eboot.bin', 'level=test', 'foo=bar']
        self.assertEqual(self.arguments({}, 'big2-ps4_Shipping.elf', original),
                         original + ['bp_skipintromovie'])
        self.assertEqual(self.arguments({'BB_SKIP_INTRO': '0'},
                                        'big2-ps4_Shipping.elf', original), original)

    def test_skip_is_not_duplicated(self):
        original = ['/app0/eboot.bin', 'eboot.bin', 'bp_skipintromovie']
        self.assertEqual(self.arguments({}, 'eboot.bin', original), original)

    def test_target_only_loadexec_adds_argv_zero(self):
        self.assertEqual(self.arguments({}, 'eboot.bin', ['/app0/eboot.bin']),
                         ['/app0/eboot.bin', 'eboot.bin', 'bp_skipintromovie'])

    def test_disabled_and_unsupported(self):
        self.assertIsNone(self.arguments({'BB_SKIP_INTRO': '0'}, 'eboot.bin'))
        self.assertIsNone(self.arguments({}, 'big3-ps4_Shipping.elf'))


if __name__ == '__main__':
    unittest.main()

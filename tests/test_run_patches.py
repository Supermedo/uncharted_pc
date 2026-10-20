"""run.py hands patches.py the collection's executable name of the game being run."""
from paths import ROOT
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('bbrun', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class PatchArguments(unittest.TestCase):
    def test_a_game_dumped_on_its_own_keeps_its_collection_name(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            u2 = root / 'u2'; u2.mkdir(); (u2 / 'eboot.bin').write_bytes(b'')
            patches = root / 'patches'; patches.mkdir()
            env = {'BB_GAME_DIR_U2': str(u2), 'BB_PATCHES_DIR': str(patches)}
            exe = run.EXECUTABLES['u2']
            game, file = run.locate(env, exe)
            self.assertEqual(file, 'eboot.bin')  # the per-game dump's own executable file
            args = run.patch_arguments(env, exe, game, root / 'out')
            self.assertEqual(args[args.index('--app-elf') + 1], 'big2-ps4_Shipping.elf')

    def test_no_patch_folder_means_no_patches(self):
        self.assertEqual(run.patch_arguments({}, 'eboot.bin', Path('.'), Path('.')), [])
        self.assertEqual(run.patch_arguments({'BB_PATCHES_DIR': '/nonexistent'}, 'eboot.bin', Path('.'), Path('.')), [])


class StalePatchFile(unittest.TestCase):
    def test_a_patch_file_from_an_earlier_launch_is_not_kept(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out = root / 'out'; out.mkdir()
            (out / 'patches.bin').write_bytes(b'junk from the last launch')
            seen = []

            def compiler(name, *args, **kwargs):
                seen.append((out / 'patches.bin').exists())  # patches.py appends to what it finds
                (out / 'patches.bin').write_bytes(b'fresh')

            env = {'BB_CONFIG': str(root / 'bbport.ini'), 'BB_RENDER_RES': '1920x1080'}
            with patch.object(run, 'run_script', compiler):
                run.apply_patches(root, out, env, root / 'caps')
            self.assertEqual(seen, [False])
            self.assertEqual((out / 'patches.bin').read_bytes(), b'fresh')


if __name__ == '__main__':
    unittest.main()

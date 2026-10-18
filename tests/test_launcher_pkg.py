from paths import ROOT
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

# The launcher is a Tkinter application; importing the module must not need a display,
# which these tests rely on to stay runnable in CI.
spec = importlib.util.spec_from_file_location("uncharted_launcher",
                                              ROOT / "launcher/uncharted_launcher.py")
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

# Reuse the fixture builder so the launcher is tested against the same .pkg images the
# extractor is.
_pkg_spec = importlib.util.spec_from_file_location("pkg", ROOT / "scripts/pkg.py")
pkg_module = importlib.util.module_from_spec(_pkg_spec)
_pkg_spec.loader.exec_module(pkg_module)


def build_pkg(files, **kwargs):
    sys.path.insert(0, str(ROOT / "tests"))
    from test_pkg import build_pkg as builder
    return builder(files, **kwargs)


SAMPLE = {'sce_sys/param.sfo': b'\x00\x00\x00\x00' * 8, 'eboot.bin': b'original executable',
          'u1data/asset.dcx': b'level data'}


class PkgDetectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_folder_with_a_pkg_and_no_game_is_detected(self):
        (self.root / "EP9000-CUSA02343_00-UNCHARTERED4.pkg").write_bytes(b'\x7f' + b'\x00' * 64)
        self.assertTrue(launcher.looks_like_pkg(self.root))

    def test_extracted_folder_is_not_flagged(self):
        (self.root / "eboot.bin").write_bytes(b'original executable')
        (self.root / "EP9000-CUSA02343_00-UNCHARTERED4.pkg").write_bytes(b'\x7f')
        # The executable wins: a leftover .pkg beside a real dump is not a problem.
        self.assertFalse(launcher.looks_like_pkg(self.root))

    def test_plain_extracted_folder_is_not_flagged(self):
        (self.root / "sce_sys").mkdir()
        (self.root / "sce_sys/param.sfo").write_bytes(b'fixture')
        (self.root / "eboot.bin").write_bytes(b'original executable')
        self.assertFalse(launcher.looks_like_pkg(self.root))

    def test_extension_is_matched_case_insensitively(self):
        (self.root / "game.PKG").write_bytes(b'\x7f')
        self.assertTrue(launcher.looks_like_pkg(self.root))

    def test_a_directory_named_pkg_does_not_count(self):
        (self.root / "something.pkg").mkdir()
        self.assertFalse(launcher.looks_like_pkg(self.root))

    def test_empty_folder_and_missing_folder_are_not_flagged(self):
        self.assertFalse(launcher.looks_like_pkg(self.root))
        self.assertFalse(launcher.looks_like_pkg(self.root / 'not-there'))
        self.assertFalse(launcher.looks_like_pkg(None))

    def test_the_pkg_file_itself_is_not_a_game_folder(self):
        # Picking the .pkg itself is the other way this goes wrong.
        pkg = self.root / "game.pkg"
        pkg.write_bytes(b'\x7f' * 128)
        self.assertIsNone(launcher.game_info(pkg))


class UnpackTests(unittest.TestCase):
    """unpack_pkgs(): what the launcher's "unpack now" action does."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # unpack_pkgs imports pkg from PORT_DIR/scripts; point that at the repo.
        self.saved = launcher.PORT_DIR
        launcher.PORT_DIR = ROOT
        self.addCleanup(setattr, launcher, 'PORT_DIR', self.saved)

    def test_find_pkgs_is_sorted_and_only_pkg_files(self):
        (self.root / 'B.pkg').write_bytes(b'x')
        (self.root / 'A.PKG').write_bytes(b'x')
        (self.root / 'notes.txt').write_bytes(b'x')
        (self.root / 'dir.pkg').mkdir()
        found = [p.name for p in launcher.find_pkgs(self.root)]
        self.assertEqual(found, ['A.PKG', 'B.pkg'])

    def test_find_pkgs_on_a_missing_folder_is_empty(self):
        self.assertEqual(launcher.find_pkgs(self.root / 'nope'), [])

    def test_unpacking_writes_a_folder_next_to_the_package(self):
        (self.root / 'game.pkg').write_bytes(build_pkg(SAMPLE))
        out = launcher.unpack_pkgs(self.root)
        self.assertIsNotNone(out)
        self.assertEqual(out.name, 'game')
        self.assertEqual((out / 'eboot.bin').read_bytes(), b'original executable')
        self.assertTrue(launcher.game_info(out), 'the result must look like a game folder')

    def test_unpacking_logs_what_happened(self):
        (self.root / 'game.pkg').write_bytes(build_pkg(SAMPLE))
        lines = []
        launcher.unpack_pkgs(self.root, log=lines.append)
        text = ''.join(lines)
        self.assertIn('Unpacking game.pkg', text)
        self.assertIn('3 files into', text)

    def test_a_fully_encrypted_package_yields_no_folder(self):
        (self.root / 'enc.pkg').write_bytes(build_pkg(SAMPLE, encrypt=set(SAMPLE)))
        lines = []
        self.assertIsNone(launcher.unpack_pkgs(self.root, log=lines.append))

    def test_a_delta_package_yields_no_folder_and_says_why(self):
        (self.root / 'patch.pkg').write_bytes(build_pkg(SAMPLE, content_type=0x1E))
        lines = []
        self.assertIsNone(launcher.unpack_pkgs(self.root, log=lines.append))
        self.assertIn('delta', ''.join(lines).lower())

    def test_no_packages_is_not_an_error(self):
        self.assertIsNone(launcher.unpack_pkgs(self.root))

    def test_a_corrupt_package_does_not_stop_the_others(self):
        (self.root / 'a-broken.pkg').write_bytes(b'not a package at all')
        (self.root / 'b-good.pkg').write_bytes(build_pkg(SAMPLE))
        lines = []
        out = launcher.unpack_pkgs(self.root, log=lines.append)
        self.assertIsNotNone(out, 'the readable package must still be extracted')
        self.assertIn('a-broken.pkg', ''.join(lines))
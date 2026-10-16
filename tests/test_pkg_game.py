"""PS4 package source: package checks, layering, prepared-game reuse and the launcher's package settings.

Synthetic packages cover the checks and need no keys. The real packages are checked only when
UNCHARTED_BASE_PKG and UNCHARTED_UPDATE_PKG name them; the unpack checks also need cryptography.
"""
from paths import ROOT
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / "launcher"))
import pkg_game as g
import pkg_info
import uncharted_launcher

TITLE = g.TITLE
PROFILE = g.PROFILES[TITLE]
CONTENT_ID = PROFILE["content_id"]
REQUIRED = PROFILE["required_files"]
BASE_EBOOT = b"base eboot"
UPDATE_EBOOT = b"update eboot"


def sfo_bytes(values):
    keys, data, index = b"", b"", b""
    for key, value in values.items():
        raw = value.encode() + b"\0"
        index += struct.pack("<HHIII", len(keys), 0x0204, len(raw), len(raw), len(data))
        keys += key.encode() + b"\0"
        data += raw
    key_table = 20 + len(index)
    data_table = key_table + len(keys)
    return struct.pack("<4sIIII", b"\0PSF", 0x0101, key_table, data_table, len(values)) + index + keys + data


def pkg_bytes(*, title=TITLE, content_id=CONTENT_ID, category="gd", app_ver="01.00", fake=True):
    """A PKG header, entry table and param.sfo: enough for pkg_info, with no encrypted data."""
    sfo = sfo_bytes({"TITLE": "Uncharted", "TITLE_ID": title, "CONTENT_ID": content_id,
                     "CATEGORY": category, "APP_VER": app_ver, "VERSION": "01.00"})
    entries = [(0x1000, sfo, 0), (0x20, b"", (3 if fake else 0) << 12)]
    header = bytearray(0x1000)
    struct.pack_into(">I", header, 0, 0x7F434E54)
    struct.pack_into(">I", header, 0x10, len(entries))
    struct.pack_into(">I", header, 0x18, 0x1000)
    header[0x40:0x64] = content_id.encode().ljust(0x24, b"\0")
    table, blobs = b"", b""
    start = 0x1000 + 32 * len(entries)
    for entry_id, blob, flags2 in entries:
        table += struct.pack(">IIIIII", entry_id, 0, 0, flags2, start + len(blobs), len(blob)) + bytes(8)
        blobs += blob
    return bytes(header) + table + blobs


def sfo_for(app_ver, sfo_version):
    return sfo_bytes({"TITLE_ID": TITLE, "CONTENT_ID": CONTENT_ID, "APP_VER": app_ver, "VERSION": sfo_version})


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def fill_base(layer):
    for name in REQUIRED:
        write(layer / name, BASE_EBOOT if name == "eboot.bin" else b"base " + name.encode())
    write(layer / "sce_sys" / "param.sfo", sfo_for("01.00", "01.00"))


def fill_update(layer):
    write(layer / "eboot.bin", UPDATE_EBOOT)
    write(layer / "big2-ps4_Shipping.elf", b"update big2")
    write(layer / "sce_sys" / "param.sfo", sfo_for("01.02", "01.01"))


class PackageSelectionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def package(self, name, data=None, **fields):
        path = self.dir / name
        path.write_bytes(data if data is not None else pkg_bytes(**fields))
        return str(path)

    def assert_rejected(self, text, *args):
        with self.assertRaises(g.PackageError) as caught:
            g.select_packages(*args)
        self.assertIn(text, str(caught.exception))

    def test_base_alone_is_version_1_00(self):
        selection = g.select_packages(self.package("base.pkg"))
        self.assertEqual(selection.version, "1.00")
        self.assertIsNone(selection.update)

    def test_base_and_update_are_version_1_02(self):
        selection = g.select_packages(self.package("base.pkg"), self.package("upd.pkg", category="gp", app_ver="01.02"))
        self.assertEqual(selection.version, "1.02")
        self.assertEqual(selection.sources["update"]["app_ver"], "01.02")
        self.assertEqual(selection.sources["base"]["content_id"], CONTENT_ID)

    def test_missing_base_is_rejected(self):
        self.assert_rejected("The base package was not found", str(self.dir / "missing.pkg"), None)

    def test_missing_update_is_rejected(self):
        self.assert_rejected("The update package was not found", self.package("base.pkg"), str(self.dir / "no.pkg"))

    def test_empty_base_asks_for_the_base_package(self):
        self.assert_rejected("Choose the base package", "", None)

    def test_file_that_is_not_a_pkg_is_rejected(self):
        self.assert_rejected("not a readable PS4 package", self.package("x.pkg", data=b"\0" * 8192), None)

    def test_other_title_is_rejected(self):
        self.assert_rejected("is for CUSA03173", self.package("b.pkg", title="CUSA03173"), None)
        self.assert_rejected("supports CUSA02320", self.package("b2.pkg", title="CUSA03173"), None)

    def test_patch_given_as_base_is_rejected(self):
        self.assert_rejected("is not a base game package", self.package("p.pkg", category="gp", app_ver="01.02"), None)

    def test_other_edition_is_rejected(self):
        self.assert_rejected("has content ID UP9000-OTHER",
                             self.package("b.pkg", content_id="UP9000-OTHER"), None)

    def test_wrong_base_version_is_rejected(self):
        self.assert_rejected("The base package is version 01.01", self.package("b.pkg", app_ver="01.01"), None)

    def test_retail_package_is_rejected(self):
        self.assert_rejected("not a fake PKG", self.package("b.pkg", fake=False), None)

    def test_base_given_as_update_is_rejected(self):
        base = self.package("base.pkg")
        self.assert_rejected("is not an update package", base, base)

    def test_update_for_another_game_is_rejected(self):
        self.assert_rejected("not for the same game",
                             self.package("base.pkg"),
                             self.package("u.pkg", category="gp", app_ver="01.02", content_id="UP9000-OTHER"))

    def test_wrong_update_version_is_rejected(self):
        self.assert_rejected("The update package is version 01.03",
                             self.package("base.pkg"), self.package("u.pkg", category="gp", app_ver="01.03"))


class PreparedGameTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        update_sha = hashlib.sha256(UPDATE_EBOOT).hexdigest()
        patcher = mock.patch.dict(PROFILE["versions"]["1.02"], {"eboot_sha256": update_sha})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.base_pkg = self.dir / "base.pkg"
        self.update_pkg = self.dir / "update.pkg"
        self.base_pkg.write_bytes(pkg_bytes())
        self.update_pkg.write_bytes(pkg_bytes(category="gp", app_ver="01.02"))
        self.root = self.dir / "game" / TITLE
        self.layers = [self.root / "extracted" / "base", self.root / "extracted" / "update"]
        fill_base(self.layers[0])
        fill_update(self.layers[1])
        self.selection = g.select_packages(str(self.base_pkg), str(self.update_pkg))
        self.view = self.root / "prepared"

    def build(self, selection=None):
        selection = selection or self.selection
        stats = g.build_view(self.layers, self.view, "test")
        g.finish_view(self.view, selection, stats)

    def test_update_replaces_base_files_without_touching_the_layers(self):
        self.build()
        self.assertEqual((self.view / "eboot.bin").read_bytes(), UPDATE_EBOOT)
        self.assertEqual((self.layers[0] / "eboot.bin").read_bytes(), BASE_EBOOT)
        self.assertEqual(g.view_problems(self.view, self.selection), ([], []))
        self.assertEqual(pkg_info.parse_sfo((self.view / "sce_sys" / "param.sfo").read_bytes())["APP_VER"], "01.02")

    def test_finished_view_is_reused(self):
        self.build()
        self.assertIsNone(g.check_prepared(self.root, self.selection))

    def test_view_is_not_reused_after_the_update_changes(self):
        self.build()
        stat = self.update_pkg.stat()
        os.utime(self.update_pkg, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10 ** 9))
        changed = g.select_packages(str(self.base_pkg), str(self.update_pkg))
        self.assertEqual(g.check_prepared(self.root, changed), "the packages changed since the game was extracted")

    def test_unfinished_view_is_not_reused(self):
        stats = g.build_view(self.layers, self.view, "test")
        self.assertEqual(g.check_prepared(self.root, self.selection), "the last extraction did not finish")
        g.finish_view(self.view, self.selection, stats)
        self.assertIsNone(g.check_prepared(self.root, self.selection))

    def test_missing_required_file_is_reported(self):
        self.build()
        (self.view / "u2data" / "data.psarc").unlink()
        self.assertEqual(g.view_problems(self.view, self.selection), (["u2data/data.psarc"], []))
        self.assertEqual(g.check_prepared(self.root, self.selection), "the extracted files are incomplete or changed")

    def test_wrong_version_identity_in_the_view_is_reported(self):
        self.build()
        write(self.view / "sce_sys" / "param.sfo", sfo_for("01.00", "01.00"))
        missing, mismatches = g.view_problems(self.view, self.selection)
        self.assertEqual(missing, [])
        self.assertTrue(any("APP_VER is 01.00, not 01.02" in item for item in mismatches), mismatches)

    def test_wrong_eboot_is_reported(self):
        self.build()
        write(self.view / "eboot.bin", BASE_EBOOT)
        _missing, mismatches = g.view_problems(self.view, self.selection)
        self.assertEqual(mismatches, ["eboot.bin is not the 1.02 build"])

    def test_layer_action_follows_the_marker(self):
        layer = self.layers[0]
        source = self.selection.base.fingerprint
        self.assertEqual(g.layer_action(layer, source), "fresh")
        g.write_json(layer / g.LAYER_MARKER, {"source": source, "complete": False})
        self.assertEqual(g.layer_action(layer, source), "resume")
        g.write_json(layer / g.LAYER_MARKER, {"source": source, "complete": True})
        self.assertEqual(g.layer_action(layer, source), "ready")
        self.assertEqual(g.layer_action(layer, {**source, "mtime_ns": 1}), "fresh")

    def test_folder_the_launcher_did_not_make_is_kept(self):
        foreign = self.dir / "foreign"
        write(foreign / "notes.txt", b"mine")
        with self.assertRaises(g.PrepareError):
            g.remove_owned(foreign, g.LAYER_MARKER)
        self.assertEqual((foreign / "notes.txt").read_bytes(), b"mine")
        with self.assertRaises(g.PrepareError):
            g.build_view(self.layers, foreign, "test")
        self.assertTrue((foreign / "notes.txt").is_file())

    def test_launcher_made_view_is_rebuilt(self):
        self.build()
        g.build_view(self.layers, self.view, "again")
        self.assertEqual(g.check_prepared(self.root, self.selection), "the last extraction did not finish")
        self.assertEqual((self.view / "eboot.bin").read_bytes(), UPDATE_EBOOT)

    def test_unusable_prepared_folder_is_reported(self):
        blocker = self.dir / "file"
        blocker.write_bytes(b"x")
        with self.assertRaises(g.PrepareError) as caught:
            g.ensure_writable(blocker / "game")
        self.assertIn("not available", str(caught.exception))

    def test_free_space_is_checked_before_unpacking(self):
        with self.assertRaises(g.PrepareError) as caught:
            g.ensure_space(self.dir, 1 << 62, "base")
        self.assertIn("Not enough free space", str(caught.exception))

    def test_prepare_returns_a_finished_view_without_unpacking(self):
        self.build()
        with mock.patch.object(g, "unpack_layer", side_effect=AssertionError("must not unpack")):
            self.assertEqual(g.prepare(self.selection, self.root, log=lambda _t: None), self.view)


def extractor():
    try:
        import pkg_extract
        return pkg_extract
    except ImportError:
        return None


class BuiltInKeysTests(unittest.TestCase):
    def test_fake_pkg_keysets_are_built_in(self):
        import pkg_keys
        for name in ("FAKEKEYSET", "PKGDERIVEDKEY3KEYSET"):
            modulus, exponent = getattr(pkg_keys, name + "_N"), getattr(pkg_keys, name + "_D")
            self.assertEqual(modulus.bit_length(), 2048)
            self.assertEqual(pow(pow(0x1234567, 65537, modulus), exponent, modulus), 0x1234567)

    @unittest.skipUnless(extractor(), "needs cryptography")
    def test_extractor_uses_the_built_in_keys(self):
        self.assertIs(extractor().pkg_keys, __import__("pkg_keys"))


class LauncherSettingsTests(unittest.TestCase):
    def test_active_folder_follows_the_source(self):
        folder = {"game_source": "folder", "game_dir": "D:/games/folder", "base_pkg": "B.pkg", "update_pkg": ""}
        self.assertEqual(uncharted_launcher.active_game_dir(folder), "D:/games/folder")
        packages = {**folder, "game_source": "pkg"}
        self.assertEqual(uncharted_launcher.active_game_dir(packages), str(g.view_dir(uncharted_launcher.DATA_DIR)))

    def test_switching_source_keeps_the_other_paths(self):
        saved = {**uncharted_launcher.APP_DEFAULTS, "game_source": "pkg", "game_dir": "D:/games/folder",
                 "base_pkg": "E:/base.pkg", "update_pkg": "E:/update.pkg"}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "settings.json"
            path.write_text(json.dumps(saved), encoding="utf-8")
            loaded = {**uncharted_launcher.APP_DEFAULTS, **uncharted_launcher.load_json(path, {})}
        loaded["game_source"] = "folder"
        self.assertEqual(loaded["game_dir"], "D:/games/folder")
        self.assertEqual(uncharted_launcher.package_paths(loaded), ("E:/base.pkg", "E:/update.pkg"))

    def test_decrypted_folder_is_still_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "eboot.bin").write_bytes(b"x")
            info = uncharted_launcher.game_info(folder)
        self.assertIsNotNone(info)
        self.assertEqual(info["games"], ["u1"])

    def test_package_paths_drop_blank_update(self):
        self.assertEqual(uncharted_launcher.package_paths({"base_pkg": " a.pkg ", "update_pkg": "  "}), ("a.pkg", None))


REAL_BASE = os.environ.get("UNCHARTED_BASE_PKG", "")
REAL_UPDATE = os.environ.get("UNCHARTED_UPDATE_PKG", "")


@unittest.skipUnless(REAL_BASE and REAL_UPDATE, "set UNCHARTED_BASE_PKG and UNCHARTED_UPDATE_PKG")
class RealPackageTests(unittest.TestCase):
    def test_real_packages_select_version_1_02(self):
        selection = g.select_packages(REAL_BASE, REAL_UPDATE)
        self.assertEqual((selection.title, selection.version), (TITLE, "1.02"))

    @unittest.skipUnless(extractor(), "needs cryptography")
    def test_real_eboot_files_match_the_known_builds(self):
        pkg_extract = extractor()
        for package, version in ((REAL_BASE, "1.00"), (REAL_UPDATE, "1.02")):
            with tempfile.TemporaryDirectory() as directory:
                pkg_extract.extract(package, directory, None, ["eboot.bin"])
                digest = hashlib.sha256((Path(directory) / "eboot.bin").read_bytes()).hexdigest()
            self.assertEqual(digest, PROFILE["versions"][version]["eboot_sha256"], version)

    @unittest.skipUnless(extractor(), "needs cryptography")
    def test_extracted_sizes_match_the_packages(self):
        pkg_extract = extractor()
        with tempfile.TemporaryDirectory() as directory:
            base, _remaining = pkg_extract.plan(REAL_BASE, directory)
            update, _remaining = pkg_extract.plan(REAL_UPDATE, directory)
        self.assertEqual(PROFILE["versions"]["1.00"]["extracted_bytes"], base)
        self.assertEqual(PROFILE["versions"]["1.02"]["extracted_bytes"], base + update)

    @unittest.skipUnless(extractor(), "needs cryptography")
    def test_required_files_are_in_the_packages(self):
        pkg_extract = extractor()
        names = set()
        for package in (REAL_BASE, REAL_UPDATE):
            with pkg_extract.Pkg(package) as pkg:
                names.update(rel for rel, _ino, _t in pkg_extract.listing(pkg))
        for name in REQUIRED:
            if name != "sce_sys/param.sfo":  # written from the PKG metadata
                self.assertIn(name, names)


if __name__ == "__main__":
    unittest.main()

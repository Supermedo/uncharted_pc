from paths import ROOT
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('pkg', ROOT / 'scripts/pkg.py')
pkg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pkg)

# Fixed by the format, not by this project: the header is little-endian, the entry
# table is big-endian, and file names live in the entry with id 0x200.
HEADER_SIZE = 0x1000
ENTRY_SIZE = 0x20


def build_pkg(files, *, encrypt=(), entry_count_extra=0, content_type=0x1A,
              content_flags=0, drm_type=0x0, entry_table_offset=None):
    """A minimal but structurally faithful .pkg image.

    Everything is big-endian, including the header -- verified against a real
    retail package (entry count, table offset, drm_type and content_type all read
    as sensible values only big-endian).
    `files` is {relative name: bytes}. `encrypt` names entries to mark encrypted.
    Layout: header (0x1000), names blob, entry table, then the file bodies.
    """
    names_blob = bytearray()
    name_offsets = {}
    for name in files:
        name_offsets[name] = len(names_blob)
        names_blob += name.encode() + b'\0'

    names_offset = HEADER_SIZE
    if entry_table_offset is None:
        entry_table_offset = names_offset + len(names_blob)

    table_size = (len(files) + 1) * ENTRY_SIZE
    body_start = entry_table_offset + table_size
    body = bytearray()
    bodies = {}
    for name, blob in files.items():
        bodies[name] = (body_start + len(body), len(blob))
        body += blob

    entries = [(0x200, 0, 0, 0, names_offset, len(names_blob))]
    for name in files:
        offset, size = bodies[name]
        flags1 = pkg.FLAG_ENCRYPTED if name in encrypt else 0
        entries.append((0x1000, name_offsets[name], flags1, 0, offset, size))

    header = bytearray(HEADER_SIZE)
    header[0:4] = pkg.PKG_MAGIC
    struct.pack_into('>I', header, 0x10, len(entries) + entry_count_extra)
    struct.pack_into('>I', header, 0x18, entry_table_offset)
    struct.pack_into('>I', header, 0x70, drm_type)
    struct.pack_into('>I', header, 0x74, content_type)
    struct.pack_into('>I', header, 0x78, content_flags)
    header[0x40:0x40 + len(b'EP9000-CUSA02343')] = b'EP9000-CUSA02343'

    table = bytearray()
    for id, name_offset, flags1, flags2, offset, size in entries:
        table += struct.pack('>IIIIIIQ', id, name_offset, flags1, flags2, offset, size, 0)

    image = bytearray(header)
    image += names_blob
    assert len(image) == entry_table_offset, (len(image), entry_table_offset)
    image += table
    image += body
    return bytes(image)


SAMPLE = {
    'sce_sys/param.sfo': b'\x00\x00\x00\x00' * 16,
    'eboot.bin': b'original executable',
    'u1data/asset.dcx': b'level data',
}


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / 'out'

    def test_round_trip_recovers_every_file_and_its_bytes(self):
        image = build_pkg(SAMPLE)
        info, written = pkg.extract(image, self.out)
        self.assertEqual(written, 3)
        for name, blob in SAMPLE.items():
            self.assertEqual((self.out / name).read_bytes(), blob, name)
        self.assertEqual(info['header']['content_id'], 'EP9000-CUSA02343')

    def test_structural_entries_are_not_written(self):
        # Entry 0x200 carries the names; it must not land in the output folder.
        _, written = pkg.extract(build_pkg(SAMPLE), self.out)
        self.assertFalse((self.out / '.entry_names').exists())
        self.assertEqual(written, len(SAMPLE))

    def test_encrypted_entries_are_skipped_and_reported(self):
        image = build_pkg(SAMPLE, encrypt={'u1data/asset.dcx'})
        info, written = pkg.extract(image, self.out)
        self.assertEqual(written, 2)
        self.assertEqual(len(info['encrypted']), 1)
        self.assertEqual(info['encrypted'][0].name, 'u1data/asset.dcx')
        self.assertFalse((self.out / 'u1data/asset.dcx').exists())

    def test_an_entirely_encrypted_package_yields_nothing(self):
        image = build_pkg(SAMPLE, encrypt=set(SAMPLE))
        info, written = pkg.extract(image, self.out)
        self.assertEqual(written, 0)
        self.assertEqual(len(info['encrypted']), 3)

    def test_delta_patch_is_refused_rather_than_half_applied(self):
        image = build_pkg(SAMPLE, content_type=pkg.CONTENT_TYPE_DELTA_PATCH)
        with self.assertRaises(pkg.PkgError) as caught:
            pkg.extract(image, self.out)
        self.assertIn('delta', str(caught.exception).lower())
        self.assertFalse(self.out.exists(), 'nothing should be written for a delta package')

    def test_cumulative_patch_extracts_normally(self):
        image = build_pkg(SAMPLE, content_flags=pkg.CONTENT_FLAG_CUMULATIVE)
        info, written = pkg.extract(image, self.out)
        self.assertFalse(info['delta'])
        self.assertEqual(written, 3)

    def test_bad_magic_is_reported(self):
        with self.assertRaises(pkg.PkgError):
            pkg.extract(b'NOTAPKG' + b'\0' * 4096, self.out)

    def test_truncated_package_is_reported(self):
        image = build_pkg(SAMPLE)
        with self.assertRaises(pkg.PkgError):
            pkg.extract(image[:HEADER_SIZE + 8], self.out)

    def test_entry_pointing_past_the_end_is_reported(self):
        image = bytearray(build_pkg(SAMPLE))
        table_offset = struct.unpack_from('>I', image, 0x18)[0]
        # Second entry (the first real file): push its size past the end.
        struct.pack_into('>I', image, table_offset + ENTRY_SIZE + 0x14, 0x7FFFFFFF)
        with self.assertRaises(pkg.PkgError):
            pkg.extract(bytes(image), self.out)


class PathSafetyTests(unittest.TestCase):
    def test_traversal_names_are_refused(self):
        for name in ['../escape.bin', 'a/../../escape.bin', '/abs.bin', 'C:/x.bin',
                     '..\\windows.bin']:
            self.assertIsNone(pkg.safe_relative(name), name)

    def test_ordinary_names_are_accepted(self):
        self.assertEqual(pkg.safe_relative('sce_sys/param.sfo'), Path('sce_sys/param.sfo'))
        self.assertEqual(pkg.safe_relative('a\\b.bin'), Path('a/b.bin'))
        self.assertEqual(pkg.safe_relative('./a.bin'), Path('a.bin'))

    def test_empty_and_dot_names_are_refused(self):
        for name in ['', '.', None, '///']:
            self.assertIsNone(pkg.safe_relative(name), repr(name))

    def test_traversal_entries_are_not_written(self):
        image = build_pkg({'../escape.bin': b'nope'})
        _, written = pkg.extract(image, Path(tempfile.gettempdir()))
        self.assertEqual(written, 0)


class BigEndianTests(unittest.TestCase):
    def test_entry_fields_are_read_big_endian(self):
        # A big-endian 0x01020304 is 0x04030201 in little-endian: get this wrong
        # and every offset points into the wrong place.
        entry = pkg.Entry(0x1000, 0, 0, 0, 0x01020304, 0x05060708)
        self.assertEqual(entry.offset, 0x01020304)
        self.assertEqual(entry.size, 0x05060708)

    def test_header_fields_are_read_big_endian(self):
        # Checked against a real retail package (CUSA02344 1.02): entry_count,
        # entry_table_offset, drm_type and content_type are all sensible only when
        # read big-endian. Reading them little-endian gives 0x33000000 entries and a
        # 3 GB table offset, which fails on the first bounds check.
        head = bytearray(HEADER_SIZE)
        head[0:4] = pkg.PKG_MAGIC
        struct.pack_into('>I', head, 0x10, 0x33)
        struct.pack_into('>I', head, 0x18, 0x2AC0)
        struct.pack_into('>I', head, 0x70, pkg.DRM_TYPE_PS4)
        struct.pack_into('>I', head, 0x74, 0x1A)
        header = pkg.read_header(pkg.BytesSource(bytes(head)))
        self.assertEqual(header['entry_count'], 0x33)
        self.assertEqual(header['entry_table_offset'], 0x2AC0)
        self.assertEqual(header['drm_type'], pkg.DRM_TYPE_PS4)
        self.assertEqual(header['content_type'], 0x1A)

    def test_magic_is_the_raw_four_bytes(self):
        # b'\x7fCNT'. Reading it as a little-endian uint32 gives 0x544E437F, which
        # matches nothing; as big-endian it is 0x7F434E54.
        self.assertEqual(pkg.PKG_MAGIC, b'\x7fCNT')
        self.assertEqual(int.from_bytes(pkg.PKG_MAGIC, 'big'), 0x7F434E54)

    def test_encryption_flags(self):
        self.assertTrue(pkg.Entry(0x1000, 0, pkg.FLAG_ENCRYPTED, 0, 0, 0).encrypted)
        self.assertFalse(pkg.Entry(0x1000, 0, 0, 0, 0, 0).encrypted)
        entry = pkg.Entry(0x1000, 0, 0, 0x3000, 0, 0)
        self.assertEqual(entry.key_index, 3)


class SourceTests(unittest.TestCase):
    """Reading a package from disk by seeking, not by loading it whole."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_file_and_bytes_sources_agree(self):
        image = build_pkg(SAMPLE)
        path = self.root / 'game.pkg'
        path.write_bytes(image)
        with pkg.FileSource(path) as source:
            from_file = pkg.analyse(source)
        from_bytes = pkg.analyse(image)
        self.assertEqual(from_file['header'], from_bytes['header'])
        self.assertEqual([e.name for e in from_file['files']], [e.name for e in from_bytes['files']])

    def test_extraction_from_a_path_works(self):
        path = self.root / 'game.pkg'
        path.write_bytes(build_pkg(SAMPLE))
        out = self.root / 'out'
        _info, written = pkg.extract(path, out)
        self.assertEqual(written, len(SAMPLE))
        self.assertEqual((out / 'eboot.bin').read_bytes(), b'original executable')

    def test_reading_past_the_end_is_refused(self):
        path = self.root / 'small.pkg'
        path.write_bytes(build_pkg(SAMPLE))
        with pkg.FileSource(path) as source:
            with self.assertRaises(pkg.PkgError):
                source.read(source.total - 4, 64)

    def test_an_encrypted_package_is_reported_not_written(self):
        path = self.root / 'enc.pkg'
        path.write_bytes(build_pkg(SAMPLE, encrypt=set(SAMPLE), drm_type=pkg.DRM_TYPE_PS4))
        lines = []
        out = self.root / 'out'
        info, written = pkg.extract(path, out, report=lines.append)
        self.assertEqual(written, 0)
        self.assertEqual(info['header']['drm_type'], pkg.DRM_TYPE_PS4)
        self.assertTrue(any('encrypted' in line for line in lines))


if __name__ == '__main__':
    unittest.main()
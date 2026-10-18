from paths import ROOT
import struct
import unittest
import tempfile
from pathlib import Path
from prepare import parse_self, inspect_libc, nid


def fixture():
    data = bytearray(0x210)
    data[:4] = b'O\x15=\x1d'
    struct.pack_into('<H', data, 24, 1)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 16, 16)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 64,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 128, 1, 5, 0x1000, 0, 0, 16, 32, 0x1000)
    data[0x200:] = bytes(range(16))
    return data


class SelfTests(unittest.TestCase):
    def test_libc_ret_contract_is_verified_from_symbol_and_code(self):
        def libc(instruction):
            data=bytearray(0x320)
            data[:4]=b'O\x15=\x1d'
            struct.pack_into('<H',data,24,2)
            struct.pack_into('<QQQQ',data,32,0x800,0x200,16,16)
            struct.pack_into('<QQQQ',data,64,0x100800,0x220,256,256)
            struct.pack_into('<16sHHIQQQIHHHHHH',data,96,b'\x7fELF\x02\x01\x01\x09',0xfe18,62,1,0,64,0,0,64,56,3,0,0,0)
            struct.pack_into('<IIQQQQQQ',data,160,1,5,0x1000,0,0,16,16,0x1000)
            struct.pack_into('<IIQQQQQQ',data,216,0x61000000,4,0x2000,0,0,256,0,16)
            struct.pack_into('<IIQQQQQQ',data,272,2,4,0x20b0,0,0,80,80,8)
            data[0x200]=instruction
            name=b'bzQExy189ZI#C#A\0'
            data[0x220:0x220+len(name)]=name
            struct.pack_into('<IBBHQQ',data,0x260,0,0x12,0,1,0,1)
            for i,(tag,value) in enumerate([(0x61000035,0),(0x61000037,32),(0x61000039,64),(0x6100003f,24),(0,0)]):
                struct.pack_into('<QQ',data,0x2d0+16*i,tag,value)
            return data
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'libc.prx'
            path.write_bytes(libc(0xc3))
            proof=inspect_libc(path)
            self.assertTrue(proof['init_env_is_ret'])
            self.assertEqual(proof['bytes'],'c3')
            path.write_bytes(libc(0x90))
            self.assertFalse(inspect_libc(path)['init_env_is_ret'])
        self.assertEqual(nid('_init_env'),'bzQExy189ZI')

    def test_file_offset_is_not_virtual_address(self):
        elf, header, ph, segments, missing = parse_self(fixture())
        self.assertEqual(elf[0x1000:0x1010], bytes(range(16)))
        self.assertEqual(ph[0]['vaddr'], 0)
        self.assertEqual(missing, [])

    def test_truncated_payload_rejected(self):
        with self.assertRaisesRegex(ValueError, 'out of bounds'):
            parse_self(fixture()[:-1])

    def test_encrypted_or_compressed_payload_rejected(self):
        for flag in (2, 8):
            data = fixture()
            struct.pack_into('<Q', data, 32, 0x800 | flag)
            with self.assertRaisesRegex(ValueError, 'encrypted/compressed'):
                parse_self(data)

    def test_missing_required_segment_rejected(self):
        data = fixture()
        struct.pack_into('<Q', data, 32, 0)
        with self.assertRaisesRegex(ValueError, 'required segment'):
            parse_self(data)

    def test_bad_program_header_index_rejected(self):
        data = fixture()
        struct.pack_into('<Q', data, 32, 0x100800)
        with self.assertRaisesRegex(ValueError, 'segment index'):
            parse_self(data)


def make_test_sfo(title):
    key = b'TITLE\0'
    val = title.encode('utf-8') + b'\0'
    count = 1
    hdr = struct.pack('<4sIIII', b'\0PSF', 0x101, 20 + 16, 20 + 16 + len(key), count)
    entry = struct.pack('<HHIII', 0, 0x204, len(val), len(val), 0)
    return hdr + entry + key + val


def make_test_libc():
    data = bytearray(0x320)
    data[:4] = b'O\x15=\x1d'
    struct.pack_into('<H', data, 24, 2)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 16, 16)
    struct.pack_into('<QQQQ', data, 64, 0x100800, 0x220, 256, 256)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 96, b'\x7fELF\x02\x01\x01\x09', 0xfe18, 62, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 160, 1, 5, 0x1000, 0, 0, 16, 16, 0x1000)
    struct.pack_into('<IIQQQQQQ', data, 216, 0x61000000, 4, 0x2000, 0, 0, 256, 0, 16)
    struct.pack_into('<IIQQQQQQ', data, 272, 2, 4, 0x20b0, 0, 0, 80, 80, 8)
    data[0x200] = 0xc3
    name = b'bzQExy189ZI#C#A\0'
    data[0x220:0x220 + len(name)] = name
    struct.pack_into('<IBBHQQ', data, 0x260, 0, 0x12, 0, 1, 0, 1)
    for i, (tag, value) in enumerate([(0x61000035, 0), (0x61000037, 32), (0x61000039, 64), (0x6100003f, 24), (0, 0)]):
        struct.pack_into('<QQ', data, 0x2d0 + 16 * i, tag, value)
    return data


def make_test_eboot():
    data = bytearray(0x600)
    data[:4] = b'O\x15=\x1d'
    struct.pack_into('<H', data, 24, 3)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 16, 16)
    struct.pack_into('<QQQQ', data, 64, 0x100800, 0x220, 0x200, 0x200)
    struct.pack_into('<QQQQ', data, 96, 0x200800, 0x420, 0x100, 0x100)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 128, b'\x7fELF\x02\x01\x01\x09', 0xfe18, 62, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 192, 1, 5, 0x1000, 0, 0, 16, 16, 0x1000)
    struct.pack_into('<IIQQQQQQ', data, 248, 0x61000000, 4, 0x2000, 0, 0, 0x200, 0, 16)
    struct.pack_into('<IIQQQQQQ', data, 304, 2, 4, 0x2200, 0, 0, 0x100, 0x100, 8)
    data[0x200] = 0xc3
    tags = [
        (0x61000035, 0), (0x61000037, 32),
        (0x61000039, 32), (0x6100003f, 0),
        (0x61000029, 32), (0x6100002d, 0),
        (0x6100002f, 32), (0x61000031, 0),
        (0, 0)
    ]
    for i, (tag, val) in enumerate(tags):
        struct.pack_into('<QQ', data, 0x420 + 16 * i, tag, val)
    return data


class PrepareEncodingTests(unittest.TestCase):
    def test_cp950_stdout_with_trademark_title_succeeds(self):
        import io
        import json
        import sys
        from prepare import prepare

        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            out = Path(tmp) / 'out'
            (game / 'sce_sys').mkdir(parents=True)
            (game / 'sce_module').mkdir(parents=True)
            (game / 'dvdroot_ps4').mkdir(parents=True)
            (game / 'eboot.bin').write_bytes(make_test_eboot())
            (game / 'sce_module' / 'libc.prx').write_bytes(make_test_libc())
            title = 'UNCHARTED: The Nathan Drake Collection\u2122'
            (game / 'sce_sys' / 'param.sfo').write_bytes(make_test_sfo(title))

            # Simulate a strict cp950 stdout stream (which cannot encode \u2122)
            raw = io.BytesIO()
            fake_stdout = io.TextIOWrapper(raw, encoding='cp950', errors='strict')
            old_stdout = sys.stdout
            try:
                sys.stdout = fake_stdout
                prepare(game, out, 'eboot.bin')
                fake_stdout.flush()
            finally:
                sys.stdout = old_stdout

            output = raw.getvalue().decode('cp950')
            self.assertIn('UNCHARTED: The Nathan Drake Collection', output)
            self.assertNotIn('\u2122', output)

            # Check that analysis.json was generated and preserved full UTF-8 title
            analysis = json.loads((out / 'analysis.json').read_text(encoding='utf-8'))
            self.assertEqual(analysis['sfo']['TITLE'], title)

    def test_unencodable_character_fallback_does_not_crash(self):
        import io
        import sys
        from prepare import prepare

        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            out = Path(tmp) / 'out'
            (game / 'sce_sys').mkdir(parents=True)
            (game / 'sce_module').mkdir(parents=True)
            (game / 'dvdroot_ps4').mkdir(parents=True)
            (game / 'eboot.bin').write_bytes(make_test_eboot())
            (game / 'sce_module' / 'libc.prx').write_bytes(make_test_libc())
            # Unicode character that cannot be encoded in ASCII or CP950
            title = 'Uncharted \u2605 Special'
            (game / 'sce_sys' / 'param.sfo').write_bytes(make_test_sfo(title))

            raw = io.BytesIO()
            fake_stdout = io.TextIOWrapper(raw, encoding='ascii', errors='strict')
            old_stdout = sys.stdout
            try:
                sys.stdout = fake_stdout
                prepare(game, out, 'eboot.bin')
                fake_stdout.flush()
            finally:
                sys.stdout = old_stdout

            output = raw.getvalue().decode('ascii')
            self.assertIn('Uncharted ? Special', output)


if __name__ == '__main__':
    unittest.main()

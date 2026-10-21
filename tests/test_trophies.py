from paths import ROOT
from pathlib import Path
import struct
import tempfile
import unittest
import trophies


def cbc_encrypt(aes, data, iv):
    out, previous = [], iv
    for i in range(0, len(data), 16):
        block = bytes(a ^ b for a, b in zip(data[i:i + 16], previous))
        previous = aes.encrypt_block(block)
        out.append(previous)
    return b''.join(out)


def esfm(npcommid, xml):
    """An encrypted trophy text entry as a trophy pack holds it: IV, then CBC of a zero block and the xml."""
    aes = trophies.title_key(npcommid)
    body = bytes(16) + xml.encode('utf-8')
    body += bytes(-len(body) % 16)
    cipher = cbc_encrypt(aes, body, bytes(16))
    return cipher[:16] + cipher[16:]  # the first cipher block doubles as the IV of the rest


def make_trp(entries):
    """entries: [(name, flag, bytes)] -> bytes of a trophy pack."""
    table, data = b'', b''
    offset = 0x60 + 64 * len(entries)
    for name, flag, blob in entries:
        table += name.encode().ljust(32, b'\0') + struct.pack('>QQI', offset + len(data), len(blob), flag) + bytes(12)
        data += blob
    header = struct.pack('>IIQIII', trophies.TRP_MAGIC, 3, 0x60 + len(table) + len(data), len(entries), 64, 0)
    return header.ljust(0x60, b'\0') + table + data


CONF = ('<trophyconf version="1.1"><npcommid>{id}</npcommid><group id="001"/>'
        '<trophy id="000" hidden="no" ttype="P" pid="-1"/><trophy id="001" hidden="no" ttype="B" pid="000"/>'
        '<trophy id="002" hidden="yes" ttype="G" pid="000"/></trophyconf>')
TEXTS = ('<trophyconf><npcommid>{id}</npcommid><title-name>Uncharted\u2122 Test</title-name>'
         '<title-detail>A game</title-detail><group id="001"><name>G</name></group>'
         '<trophy id="000" ttype="P"><name>Platinum</name><detail>All of them</detail></trophy>'
         '<trophy id="001" ttype="B"><name>First</name><detail>Do the first thing</detail></trophy>'
         '<trophy id="002" hidden="yes" ttype="G"><name>Secret</name><detail>Do\ta\nthing</detail></trophy></trophyconf>')
PNG = b'\x89PNG\r\n\x1a\nnot really a picture'


def fixture(folder, npcommid='NPWR09798_00', npbind=False):
    sys_dir = Path(folder) / 'sce_sys'
    (sys_dir / 'trophy').mkdir(parents=True)
    pack = make_trp([('TROPCONF.ESFM', 3, esfm(npcommid, CONF.format(id=npcommid))),
                     ('TROP.ESFM', 3, esfm(npcommid, TEXTS.format(id=npcommid))),
                     ('TROP_01.ESFM', 3, esfm(npcommid, TEXTS.format(id=npcommid).replace('First', 'Erster'))),
                     ('TROP001.PNG', 0, PNG), ('ICON0.PNG', 0, PNG), ('GR001.PNG', 0, PNG)])
    (sys_dir / 'trophy' / 'trophy01.trp').write_bytes(pack)
    if npbind:
        (sys_dir / 'npbind.dat').write_bytes(b'\0' * 7 + npcommid.encode() + b'\0' * 9)


class AesTests(unittest.TestCase):
    def test_fips_197_vector(self):
        aes = trophies.AES128(bytes(range(16)))
        plain = bytes.fromhex('00112233445566778899aabbccddeeff')
        cipher = bytes.fromhex('69c4e0d86a7b0430d8cdb78070b4c55a')
        self.assertEqual(aes.encrypt_block(plain), cipher)
        self.assertEqual(aes.decrypt_block(cipher), plain)

    def test_cbc_round_trip(self):
        aes = trophies.AES128(b'0123456789abcdef')
        data = bytes(range(64))
        self.assertEqual(aes.cbc_decrypt(cbc_encrypt(aes, data, b'i' * 16), b'i' * 16), data)


class PackTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name) / 'game'

    def test_load_with_known_id(self):
        fixture(self.game)
        info = trophies.load_trp(self.game / 'sce_sys/trophy/trophy01.trp', self.game, 8)
        self.assertEqual(info['npcommid'], 'NPWR09798_00')
        self.assertEqual(info['title'], 'Uncharted\u2122 Test')
        self.assertEqual(sorted(info['trophies']), [0, 1, 2])
        self.assertEqual(info['trophies'][0]['grade'], 'P')
        self.assertTrue(info['trophies'][2]['hidden'])
        self.assertEqual(info['trophies'][1]['name'], 'First')
        self.assertEqual(info['groups'], 1)
        self.assertEqual(set(info['icons']), {'TROP001.PNG', 'ICON0.PNG'})

    def test_language_text(self):
        fixture(self.game)
        info = trophies.load_trp(self.game / 'sce_sys/trophy/trophy01.trp', self.game, 1)
        self.assertEqual(info['trophies'][1]['name'], 'Erster')

    def test_id_from_npbind_and_by_search(self):
        fixture(self.game, 'NPWR00123_00', npbind=True)
        pack = self.game / 'sce_sys/trophy/trophy01.trp'
        self.assertEqual(trophies.load_trp(pack, self.game, 1)['npcommid'], 'NPWR00123_00')
        (self.game / 'sce_sys/npbind.dat').unlink()
        self.assertEqual(trophies.load_trp(pack, self.game, 1)['npcommid'], 'NPWR00123_00')

    def test_not_a_pack(self):
        fixture(self.game)
        broken = self.game / 'broken.trp'
        broken.write_bytes(b'nope' * 100)
        with self.assertRaises(trophies.TrophyError):
            trophies.read_trp(broken)

    def test_cache_round_trip_and_reuse(self):
        fixture(self.game)
        out = Path(self.temp.name) / 'out'
        self.assertEqual(trophies.prepare(self.game, 'u1', out, 8), out)
        info = trophies.read_cache(out)
        self.assertEqual(info['npcommid'], 'NPWR09798_00')
        self.assertEqual(info['trophies'][2], (2, 'G', True, 'Secret', 'Do\ta\nthing'))
        self.assertEqual((out / 'TROP001.PNG').read_bytes(), PNG)
        stamp = (out / 'trophies.txt').stat().st_mtime_ns
        trophies.prepare(self.game, 'u1', out, 8)
        self.assertEqual((out / 'trophies.txt').stat().st_mtime_ns, stamp)  # same pack, same language: kept
        trophies.prepare(self.game, 'u1', out, 1)
        self.assertEqual(trophies.read_cache(out)['trophies'][1][3], 'Erster')

    def test_game_without_a_pack_has_no_cache(self):
        out = Path(self.temp.name) / 'out'
        out.mkdir()
        (out / 'trophies.txt').write_text('stale', encoding='utf-8')
        self.assertIsNone(trophies.prepare(self.game, 'u2', out))
        self.assertFalse((out / 'trophies.txt').exists())

    def test_text_escapes(self):
        self.assertEqual(trophies.unescape(trophies.escape('a\tb\nc\\d')), 'a\tb\nc\\d')


class UnlockTests(unittest.TestCase):
    def test_log_keeps_earliest_and_skips_garbage(self):
        with tempfile.TemporaryDirectory() as folder:
            log = trophies.unlock_log(folder, 'NPWR09798_00')
            log.parent.mkdir()
            log.write_text('5 200\n5 100\nbroken\n7 300 extra\n\n', encoding='utf-8')
            self.assertEqual(trophies.read_unlocks(folder, 'NPWR09798_00'), {5: 100, 7: 300})
            self.assertEqual(trophies.read_unlocks(folder, 'NPWR09799_00'), {})

    def test_progress(self):
        info = {'trophies': [(0, 'P', False, 'p', ''), (1, 'B', False, 'b', ''), (2, 'G', False, 'g', ''),
                             (3, 'B', False, 'b', '')]}
        progress = trophies.progress(info, {1: 5, 2: 6})
        self.assertEqual((progress['count'], progress['percent']), (2, 50))
        self.assertEqual(progress['earned'], {'P': 0, 'G': 1, 'S': 0, 'B': 1})
        self.assertEqual(progress['total'], {'P': 1, 'G': 1, 'S': 0, 'B': 2})


GAME = Path(r'D:\Games\Uncharted\CUSA02320')


@unittest.skipUnless((GAME / 'sce_sys/trophy/trophy01.trp').is_file(), 'needs the collection dump')
class DumpTests(unittest.TestCase):
    def test_the_three_sets(self):
        for key, (name, npcommid) in trophies.GAMES.items():
            info = trophies.load_trp(GAME / 'sce_sys/trophy' / name, GAME, 1, npcommid)
            self.assertEqual(info['npcommid'], npcommid)
            self.assertGreater(len(info['trophies']), 40)
            self.assertEqual(info['trophies'][0]['grade'], 'P')


if __name__ == '__main__':
    unittest.main()

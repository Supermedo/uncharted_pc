#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Trophy sets of the collection, read from the game's own trophy files.

sce_sys/trophy/trophy01.trp (Drake's Fortune), trophy02.trp (Among Thieves) and trophy03.trp
(Drake's Deception) are PS4 trophy packs: a small container with the trophy icons (PNG) and the
texts as ESFM files, which are AES-128-CBC encrypted. The key of a set comes from the console's
trophy key and the set's NP communication id (NPWRxxxxx_00), the same way shadPS4 reads them.

run.py calls this before a game starts and the runtime (src/runtime_trophy.c) reads what it
leaves in <out>/trophy/: trophies.txt and the icons. The launcher's Trophies page reads the same
files. Unlocks are kept in <saves folder>/trophy/<NPWR id>.log, one "id unix-time" per line.

    python trophies.py <game folder> --out <dir> --exe u1|u2|u3 [--lang 1] [--own]
"""
import argparse
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# The console's trophy key (public: PS4 tools and shadPS4 use the same one).
TROPHY_KEY = bytes.fromhex('21F41A6BAD8A1D3ECA7AD586C101B7A9')
# The collection's three sets. The id is looked for in npbind.dat first, then here, then by trying
# every NPWR number (slow, a few seconds, once: the answer is kept).
GAMES = {'u1': ('trophy01.trp', 'NPWR09798_00'), 'u2': ('trophy02.trp', 'NPWR09799_00'),
         'u3': ('trophy03.trp', 'NPWR09801_00')}
FORMAT = 2  # bump when trophies.txt changes: caches of older scripts are rebuilt
TRP_MAGIC = 0xDCA24D00
FLAG_ENCRYPTED = 3
GRADE_NAMES = {'P': 'Platinum', 'G': 'Gold', 'S': 'Silver', 'B': 'Bronze'}


# ---- AES-128 (table based; the key schedule and one block at a time is all this needs) ----------

def _tables():
    sbox, p, q = [0] * 256, 1, 1
    while True:
        p = (p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q
        for shift in (1, 2, 3, 4):
            x ^= ((q << shift) | (q >> (8 - shift))) & 0xFF
        sbox[p] = x ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    inverse = [0] * 256
    for i, value in enumerate(sbox):
        inverse[value] = i

    def mul(a, b):
        out = 0
        while b:
            if b & 1:
                out ^= a
            a = ((a << 1) ^ (0x11B if a & 0x80 else 0)) & 0x1FF
            b >>= 1
        return out

    def ror(word, bits):
        return ((word >> bits) | (word << (32 - bits))) & 0xFFFFFFFF
    te0 = [mul(s, 2) << 24 | s << 16 | s << 8 | mul(s, 3) for s in sbox]
    td0 = [mul(s, 14) << 24 | mul(s, 9) << 16 | mul(s, 13) << 8 | mul(s, 11) for s in map(inverse.__getitem__, range(256))]
    te = [te0] + [[ror(w, 8 * i) for w in te0] for i in (1, 2, 3)]
    td = [td0] + [[ror(w, 8 * i) for w in td0] for i in (1, 2, 3)]
    return sbox, inverse, te, td


SBOX, INV_SBOX, TE, TD = _tables()


def _expand(key):
    words = list(struct.unpack('>4I', key))
    rcon = 1
    for i in range(4, 44):
        w = words[i - 1]
        if i % 4 == 0:
            w = ((w << 8) | (w >> 24)) & 0xFFFFFFFF
            w = SBOX[w >> 24] << 24 | SBOX[(w >> 16) & 255] << 16 | SBOX[(w >> 8) & 255] << 8 | SBOX[w & 255]
            w ^= rcon << 24
            rcon = (rcon << 1) ^ (0x11B if rcon & 0x80 else 0)
        words.append(words[i - 4] ^ w)
    return words


class AES128:
    def __init__(self, key):
        self.enc = _expand(key)
        # Equivalent inverse cipher: round keys reversed, the middle ones through InvMixColumns.
        dec = []
        for r in range(10, -1, -1):
            dec += self.enc[4 * r:4 * r + 4]
        for i in range(4, 40):
            w = dec[i]
            dec[i] = (TD[0][SBOX[w >> 24]] ^ TD[1][SBOX[(w >> 16) & 255]] ^
                      TD[2][SBOX[(w >> 8) & 255]] ^ TD[3][SBOX[w & 255]])
        self.dec = dec

    def encrypt_block(self, block):
        k = self.enc
        s0, s1, s2, s3 = (w ^ k[i] for i, w in enumerate(struct.unpack('>4I', block)))
        t0, t1, t2, t3 = TE
        for r in range(1, 10):
            n = 4 * r
            s0, s1, s2, s3 = (
                t0[s0 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s3 & 255] ^ k[n],
                t0[s1 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s0 & 255] ^ k[n + 1],
                t0[s2 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s1 & 255] ^ k[n + 2],
                t0[s3 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s2 & 255] ^ k[n + 3])
        sb = SBOX
        return struct.pack('>4I',
                           (sb[s0 >> 24] << 24 | sb[(s1 >> 16) & 255] << 16 | sb[(s2 >> 8) & 255] << 8 | sb[s3 & 255]) ^ k[40],
                           (sb[s1 >> 24] << 24 | sb[(s2 >> 16) & 255] << 16 | sb[(s3 >> 8) & 255] << 8 | sb[s0 & 255]) ^ k[41],
                           (sb[s2 >> 24] << 24 | sb[(s3 >> 16) & 255] << 16 | sb[(s0 >> 8) & 255] << 8 | sb[s1 & 255]) ^ k[42],
                           (sb[s3 >> 24] << 24 | sb[(s0 >> 16) & 255] << 16 | sb[(s1 >> 8) & 255] << 8 | sb[s2 & 255]) ^ k[43])

    def decrypt_block(self, block):
        k = self.dec
        s0, s1, s2, s3 = (w ^ k[i] for i, w in enumerate(struct.unpack('>4I', block)))
        t0, t1, t2, t3 = TD
        for r in range(1, 10):
            n = 4 * r
            s0, s1, s2, s3 = (
                t0[s0 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s1 & 255] ^ k[n],
                t0[s1 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s2 & 255] ^ k[n + 1],
                t0[s2 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s3 & 255] ^ k[n + 2],
                t0[s3 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s0 & 255] ^ k[n + 3])
        ib = INV_SBOX
        return struct.pack('>4I',
                           (ib[s0 >> 24] << 24 | ib[(s3 >> 16) & 255] << 16 | ib[(s2 >> 8) & 255] << 8 | ib[s1 & 255]) ^ k[40],
                           (ib[s1 >> 24] << 24 | ib[(s0 >> 16) & 255] << 16 | ib[(s3 >> 8) & 255] << 8 | ib[s2 & 255]) ^ k[41],
                           (ib[s2 >> 24] << 24 | ib[(s1 >> 16) & 255] << 16 | ib[(s0 >> 8) & 255] << 8 | ib[s3 & 255]) ^ k[42],
                           (ib[s3 >> 24] << 24 | ib[(s2 >> 16) & 255] << 16 | ib[(s1 >> 8) & 255] << 8 | ib[s0 & 255]) ^ k[43])

    def cbc_decrypt(self, data, iv):
        out, previous = [], iv
        for i in range(0, len(data) - len(data) % 16, 16):
            block = data[i:i + 16]
            plain = self.decrypt_block(block)
            out.append(bytes(a ^ b for a, b in zip(plain, previous)))
            previous = block
        return b''.join(out)


# ---- The trophy pack -----------------------------------------------------------------------

class TrophyError(Exception):
    pass


_MASTER = None


def title_key(npcommid):
    """AES key of a set: its NP communication id (padded to a block) encrypted with the trophy key."""
    global _MASTER
    _MASTER = _MASTER or AES128(TROPHY_KEY)
    return AES128(_MASTER.encrypt_block(npcommid.encode('ascii').ljust(16, b'\0')))


def read_trp(path):
    """{entry name: (flag, bytes)} of a .trp file."""
    data = Path(path).read_bytes()
    if len(data) < 0x60 or struct.unpack('>I', data[:4])[0] != TRP_MAGIC:
        raise TrophyError(f'{path}: not a trophy pack')
    count, entry_size = struct.unpack('>II', data[16:24])
    entries = {}
    for i in range(count):
        at = 0x60 + i * entry_size
        name = data[at:at + 32].split(b'\0')[0].decode('ascii', 'replace')
        offset, size, flag = struct.unpack('>QQI', data[at + 32:at + 52])
        if offset + size <= len(data):
            entries[name] = (flag, data[offset:offset + size])
    return entries


def decrypt_esfm(raw, key):
    """The XML text of an ESFM entry (first block = IV; the plain text starts after a zero block)."""
    plain = key.cbc_decrypt(raw[16:], raw[:16])
    end = plain.rfind(b'>')
    if end < 0:
        raise TrophyError('trophy text did not decrypt')
    return plain[:end + 1].decode('utf-8', 'replace')


def key_fits(raw, key):
    """The first plain block of an ESFM entry is all zero when the key is right."""
    return key.decrypt_block(raw[:16]) == bytes(16)


def find_npcommid(raw, game_dir, wanted):
    """The NP communication id whose key opens `raw` (an ESFM entry): ids in npbind.dat, the
    collection's own, then every number."""
    candidates = []
    try:
        npbind = (Path(game_dir) / 'sce_sys' / 'npbind.dat').read_bytes()
        candidates += [m.decode() for m in re.findall(rb'NPWR\d{5}_\d\d', npbind)]
    except OSError:
        pass
    candidates += [wanted] + [i for _f, i in GAMES.values()]
    for npcommid in dict.fromkeys(candidates):
        if key_fits(raw, title_key(npcommid)):
            return npcommid
    master = _MASTER or AES128(TROPHY_KEY)
    for number in range(100000):
        npcommid = f'NPWR{number:05d}_00'
        if key_fits(raw, AES128(master.encrypt_block(npcommid.encode().ljust(16, b'\0')))):
            return npcommid
    raise TrophyError('no trophy key fits this pack')


def parse_xml(text):
    """{'title', 'detail', 'trophies': {id: {...}}} of a decrypted TROPCONF/TROP xml."""
    root = ET.fromstring(text)
    result = {'npcommid': (root.findtext('npcommid') or '').strip(), 'title': (root.findtext('title-name') or '').strip(),
              'detail': (root.findtext('title-detail') or '').strip(), 'groups': len(root.findall('group')),
              'trophies': {}}
    for node in root.iter('trophy'):
        try:
            number = int(node.get('id'))
        except (TypeError, ValueError):
            continue
        result['trophies'][number] = {
            'grade': node.get('ttype', 'B'), 'hidden': node.get('hidden') == 'yes',
            'group': node.get('gid', ''), 'name': (node.findtext('name') or '').strip(),
            'detail': (node.findtext('detail') or '').strip()}
    return result


def load_trp(trp_path, game_dir=None, lang=1, wanted=''):
    """The set in a trophy pack: {'npcommid', 'title', 'detail', 'trophies': {id: {...}}, 'icons': {name: bytes}}.
    Texts are in the console language when the pack has it (TROP_<lang>.ESFM), else the default."""
    entries = read_trp(trp_path)
    conf = entries.get('TROPCONF.ESFM')
    if not conf:
        raise TrophyError(f'{trp_path}: no TROPCONF.ESFM')
    npcommid = find_npcommid(conf[1], game_dir or Path(trp_path).parent.parent.parent, wanted)
    key = title_key(npcommid)
    structure = parse_xml(decrypt_esfm(conf[1], key))
    texts = entries.get(f'TROP_{int(lang):02d}.ESFM') or entries.get('TROP.ESFM')
    if texts:
        named = parse_xml(decrypt_esfm(texts[1], key))
        structure['title'], structure['detail'] = named['title'], named['detail']
        for number, trophy in named['trophies'].items():
            structure['trophies'].setdefault(number, trophy).update(name=trophy['name'], detail=trophy['detail'])
    structure['npcommid'] = npcommid
    structure['icons'] = {name: blob for name, (flag, blob) in entries.items()
                          if re.fullmatch(r'(TROP\d+|ICON0)\.PNG', name)}
    return structure


# ---- The cache the runtime and the launcher read ----------------------------------------------

def escape(text):
    return text.replace('\\', '\\\\').replace('\t', '\\t').replace('\r', '').replace('\n', '\\n')


def unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            out.append({'t': '\t', 'n': '\n'}.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def trp_for(game_dir, key, own=False):
    """The trophy pack of one game of the collection in `game_dir`. A game dumped on its own may
    have it as trophy00.trp."""
    names = [GAMES[key][0]] + (['trophy00.trp'] if own else [])
    for name in names:
        path = Path(game_dir) / 'sce_sys' / 'trophy' / name
        if path.is_file():
            return path
    return None


def write_cache(info, out, trp_stamp):
    out.mkdir(parents=True, exist_ok=True)
    lines = ['format\t%d' % FORMAT, 'stamp\t' + trp_stamp, 'npcommid\t' + info['npcommid'],
             'title\t' + escape(info['title']), 'detail\t' + escape(info['detail']),
             'groups\t%d' % info.get('groups', 0)]
    for number, t in sorted(info['trophies'].items()):
        lines.append('\t'.join(['trophy', str(number), t['grade'], '1' if t['hidden'] else '0', escape(t['name']),
                                escape(t['detail'])]))
    for name, blob in info['icons'].items():
        (out / name).write_bytes(blob)
    (out / 'trophies.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')


def read_cache(out):
    """{'npcommid', 'title', 'detail', 'trophies': [(id, grade, hidden, name, detail)]} of a cache folder, or None."""
    try:
        lines = (Path(out) / 'trophies.txt').read_text(encoding='utf-8').splitlines()
    except OSError:
        return None
    info = {'npcommid': '', 'title': '', 'detail': '', 'trophies': []}
    for line in lines:
        part = line.split('\t')
        if part[0] == 'format' and (len(part) < 2 or part[1] != str(FORMAT)):
            return None
        if part[0] in ('npcommid', 'title', 'detail') and len(part) > 1:
            info[part[0]] = unescape(part[1])
        elif part[0] == 'trophy' and len(part) >= 6 and part[1].isdigit():
            info['trophies'].append((int(part[1]), part[2], part[3] == '1', unescape(part[4]), unescape(part[5])))
    return info if info['trophies'] else None


def prepare(game_dir, key, out, lang=1, own=False):
    """Writes the cache of one game into `out` (made again when the pack or the language changed).
    Returns the folder, or None when the game has no readable trophy pack (the cache is removed then)."""
    out = Path(out)
    trp = trp_for(game_dir, key, own)
    if not trp:
        (out / 'trophies.txt').unlink(missing_ok=True)
        return None
    stat = trp.stat()
    stamp = f'{trp.resolve()}|{stat.st_size}|{stat.st_mtime_ns}|{int(lang)}'
    try:
        current = next((line.split('\t', 1)[1] for line in (out / 'trophies.txt').read_text(encoding='utf-8').splitlines()
                        if line.startswith('stamp\t')), '')
    except OSError:
        current = ''
    if current == stamp and read_cache(out):
        return out
    try:
        info = load_trp(trp, game_dir, lang, GAMES[key][1])
        write_cache(info, out, stamp)
    except (TrophyError, ET.ParseError, OSError, struct.error) as error:
        print(f'Trophies: {key}: {error}')
        (out / 'trophies.txt').unlink(missing_ok=True)
        return None
    print(f'Trophies: {info["npcommid"]}, {len(info["trophies"])} trophies')
    return out


# ---- Unlocks ---------------------------------------------------------------------------------

def unlock_log(user_dir, npcommid):
    return Path(user_dir) / 'trophy' / f'{npcommid}.log'


def read_unlocks(user_dir, npcommid):
    """{trophy id: earliest unlock time} from the game's log; broken lines are skipped."""
    unlocked = {}
    try:
        text = unlock_log(user_dir, npcommid).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return unlocked
    for line in text.splitlines():
        part = line.split()
        if len(part) >= 2 and part[0].isdigit() and part[1].isdigit():
            number, when = int(part[0]), int(part[1])
            if number not in unlocked or when < unlocked[number]:
                unlocked[number] = when
    return unlocked


def progress(info, unlocked):
    """How much of a set is earned: {'count', 'percent', 'earned': {grade: n}, 'total': {grade: n}}."""
    total, earned, count = dict.fromkeys('PGSB', 0), dict.fromkeys('PGSB', 0), 0
    for number, grade, _hidden, _name, _detail in info['trophies']:
        total[grade if grade in total else 'B'] += 1
        if number in unlocked:
            earned[grade if grade in earned else 'B'] += 1
            count += 1
    everything = len(info['trophies'])
    return {'count': count, 'percent': count * 100 // everything if everything else 0, 'earned': earned, 'total': total}


def main():
    for stream in (sys.stdout, sys.stderr):  # titles carry a trademark sign; the console may be GBK
        if stream:
            stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--exe', choices=sorted(GAMES), required=True)
    parser.add_argument('--lang', type=int, default=1)
    parser.add_argument('--own', action='store_true', help='the folder holds this game alone')
    args = parser.parse_args()
    try:
        prepare(args.game, args.exe, args.out, args.lang, args.own)
    except Exception as error:  # trophies are optional: never keep a game from starting
        print(f'Trophies: {error}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

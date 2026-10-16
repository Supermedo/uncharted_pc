"""Read-only PS4 PKG metadata inspection (standard library only).

Reads the header, entry table and param.sfo; never decrypts the PFS image and never writes.

Usage: python scripts/pkg_info.py <file.pkg> [--json]
"""
import argparse
import json
import os
import struct
import sys

PKG_MAGIC = 0x7F434E54
ENTRY_PARAM_SFO = 0x1000
ENTRY_IMAGE_KEY = 0x20
ENTRY_TARGET_DELTA = 0x407
ENTRY_ORIGIN_DELTA = 0x408
FAKE_IMAGE_KEY_INDEX = 3

SFO_KEYS = ('TITLE', 'TITLE_ID', 'CONTENT_ID', 'CATEGORY', 'APP_VER', 'VERSION', 'SYSTEM_VER',
            'PUBTOOLINFO', 'ATTRIBUTE', 'PARENTAL_LEVEL')


def parse_sfo(data):
    magic, _version, key_table, data_table, count = struct.unpack_from('<4sIIII', data, 0)
    if magic != b'\0PSF':
        raise ValueError('bad param.sfo signature')
    values = {}
    for i in range(count):
        key_offset, fmt, length, _capacity, data_offset = struct.unpack_from('<HHIII', data, 0x14 + i * 16)
        key = data[key_table + key_offset:data.index(0, key_table + key_offset)].decode('ascii')
        raw = data[data_table + data_offset:data_table + data_offset + length]
        values[key] = struct.unpack('<I', raw)[0] if fmt == 0x0404 else raw.rstrip(b'\0').decode('utf-8', 'replace')
    return values


def inspect(path):
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        header = f.read(0x1000)
        if len(header) < 0x1000 or struct.unpack_from('>I', header, 0)[0] != PKG_MAGIC:
            raise ValueError('not a PS4 PKG (bad magic)')
        be32 = lambda offset: struct.unpack_from('>I', header, offset)[0]
        be64 = lambda offset: struct.unpack_from('>Q', header, offset)[0]
        info = {
            'file': os.path.basename(path),
            'file_size': size,
            'pkg_type': be32(4),
            'entry_count': be32(0x10),
            'entry_table_offset': be32(0x18),
            'content_id': header[0x40:0x64].rstrip(b'\0').decode('ascii', 'replace'),
            'drm_type': be32(0x70),
            'content_type': be32(0x74),
            'content_flags': be32(0x78),
            'pfs_offset': be64(0x410),
            'pfs_size': be64(0x418),
            'declared_package_size': be64(0x430),
        }
        info['size_matches_header'] = info['declared_package_size'] == size
        info['pfs_within_file'] = info['pfs_offset'] + info['pfs_size'] <= size

        f.seek(info['entry_table_offset'])
        table = f.read(info['entry_count'] * 32)
        if len(table) != info['entry_count'] * 32:
            raise ValueError('entry table truncated')
        entries = {}
        for i in range(info['entry_count']):
            entry_id, _name, flags1, flags2, offset, length = struct.unpack_from('>IIIIII', table, i * 32)
            entries[entry_id] = dict(flags1=flags1, flags2=flags2, offset=offset, size=length)
        info['entries_within_file'] = all(e['offset'] + e['size'] <= size for e in entries.values())
        info['entry_ids'] = sorted(entries)

        key = entries.get(ENTRY_IMAGE_KEY)
        info['image_key_index'] = (key['flags2'] >> 12) & 0xF if key else None
        info['fake_pkg'] = info['image_key_index'] == FAKE_IMAGE_KEY_INDEX
        info['has_delta_info'] = ENTRY_TARGET_DELTA in entries or ENTRY_ORIGIN_DELTA in entries

        sfo = entries.get(ENTRY_PARAM_SFO)
        info['param_sfo'] = {}
        if sfo and not sfo['flags1'] & 0x80000000:
            f.seek(sfo['offset'])
            values = parse_sfo(f.read(sfo['size']))
            info['param_sfo'] = {k: values[k] for k in SFO_KEYS if k in values}
        elif sfo:
            info['param_sfo_encrypted'] = True
    return info


def classify(info):
    sfo = info['param_sfo']
    category = sfo.get('CATEGORY')
    if category == 'gp':
        return 'patch'
    if category == 'gd':
        return 'base'
    if category:
        return 'other (%s)' % category
    return 'unknown'


def title_id(info):
    sfo = info['param_sfo']
    if sfo.get('TITLE_ID'):
        return sfo['TITLE_ID']
    content_id = info['content_id']
    return content_id.split('-')[1].split('_')[0] if '-' in content_id else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pkg')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    try:
        info = inspect(args.pkg)
    except (OSError, ValueError, struct.error) as error:
        print(f'{args.pkg}: {error}', file=sys.stderr)
        return 1
    info['kind'] = classify(info)
    info['title_id'] = title_id(info)
    if args.json:
        print(json.dumps(info, indent=2))
        return 0
    sfo = info['param_sfo']
    print(f"file          : {info['file']}")
    print(f"size          : {info['file_size']} bytes (header says {info['declared_package_size']}: "
          f"{'match' if info['size_matches_header'] else 'MISMATCH'})")
    print(f"content id    : {info['content_id']}")
    print(f"title id      : {info['title_id']}")
    print(f"title         : {sfo.get('TITLE', '?')}")
    print(f"kind          : {info['kind']} (CATEGORY={sfo.get('CATEGORY', '?')}, content_type={info['content_type']:#x}, "
          f"drm_type={info['drm_type']:#x}, content_flags={info['content_flags']:#x})")
    print(f"APP_VER       : {sfo.get('APP_VER', '?')}   VERSION: {sfo.get('VERSION', '?')}   "
          f"SYSTEM_VER: {sfo['SYSTEM_VER']:#x}" if 'SYSTEM_VER' in sfo else
          f"APP_VER       : {sfo.get('APP_VER', '?')}   VERSION: {sfo.get('VERSION', '?')}")
    print(f"entries       : {info['entry_count']} (within file: {info['entries_within_file']}), "
          f"delta info: {info['has_delta_info']}")
    print(f"PFS image     : {info['pfs_size']} bytes at {info['pfs_offset']:#x} (within file: {info['pfs_within_file']})")
    print(f"image key idx : {info['image_key_index']} -> "
          f"{'fake PKG (supported)' if info['fake_pkg'] else 'not a fake PKG (unsupported)'}")
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the files from a PS4 .pkg container into a plain game folder.

    python scripts/pkg.py EP9000-CUSA02344_00-UNCHARTEDTRILOGY.pkg --out CUSA02344

Scope, and what this deliberately does not do:

  * This unpacks the *container* only. A .pkg as downloaded from PSN, or ripped from
    a disc, is encrypted (drm_type 0x0F): turning it into plaintext needs the console's
    keys. That is decryption, not unpacking, and it is out of scope. Encrypted entries
    are detected from the entry table and skipped with a count, so the result never
    pretends to be a game folder it is not.
  * A delta patch package (content type 0x1E / DELTA_PATCH) stores *differences*
    against a base package. There is no merge here: such a package is reported as
    unsupported rather than half-applied, because a partial merge produces a folder
    that looks fine and crashes later.
  * Cumulative patches and plain base packages extract normally. Extracting several
    packages into the same --out folder in order gives the newest file per path.

Everything in a .pkg is big-endian: the magic is the bytes 7F 43 4E 54 ("\x7fCNT"),
and both the header fields and the 32-byte entry table are read big-endian. This is
easy to get backwards and hard to notice, so tests/test_pkg.py pins it.

The container is read by seeking, never loaded whole: a retail game package is tens of
gigabytes, far more than this port's machines have spare.

Format references: psdevwiki.com/ps4/PKG_files.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    _reconfigure = getattr(_stream, 'reconfigure', None)
    if callable(_reconfigure):
        try:
            _reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError, OSError):
            pass

PKG_MAGIC = b'\x7fCNT'
HEADER_SIZE = 0x1000
ENTRY_SIZE = 0x20
ENTRY_NAMES_ID = 0x200

# Entry ids below this are the package's own metadata (digests, entry keys, the names
# blob) rather than game files. Everything at or above is content, and the fixed
# sce_sys entries (param.sfo 0x1000, icon0.png 0x1200, ...) live there too.
STRUCTURAL_ID_MAX = 0x1000

# Entry flags (big-endian)
FLAG_ENCRYPTED = 0x80000000
KEY_INDEX_MASK = 0xF000

DRM_TYPE_NONE = 0x0
DRM_TYPE_PS4 = 0xF

CONTENT_TYPE_DELTA_PATCH = 0x1E
CONTENT_FLAG_DELTA_PATCH = 0x41000000
CONTENT_FLAG_CUMULATIVE = 0x60000000


class PkgError(ValueError):
    """The file is not a .pkg we can read."""


class BytesSource:
    """An in-memory image, for tests and for already-loaded data."""

    def __init__(self, data):
        self.data = data
        self.total = len(data)

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.total:
            raise PkgError(f'out of bounds: {offset:#x}+{size:#x} / {self.total:#x}')
        return self.data[offset:offset + size]


class FileSource:
    """A package opened from disk: only the parts we need are ever read."""

    def __init__(self, path):
        self.path = path
        self._file = open(path, 'rb')
        self.total = os.fstat(self._file.fileno()).st_size

    def close(self):
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.total:
            raise PkgError(f'out of bounds: {offset:#x}+{size:#x} / {self.total:#x}')
        self._file.seek(offset)
        data = self._file.read(size)
        if len(data) != size:
            raise PkgError(f'short read at {offset:#x}: wanted {size:#x}, got {len(data):#x}')
        return data


def be32(data, offset):
    """Big-endian uint32 at `offset`. Every field in a .pkg is big-endian."""
    if offset + 4 > len(data):
        raise PkgError(f'truncated header at {offset:#x}')
    return struct.unpack_from('>I', data, offset)[0]


class Entry:
    __slots__ = ('id', 'name_offset', 'flags1', 'flags2', 'offset', 'size', 'name')

    def __init__(self, id, name_offset, flags1, flags2, offset, size):
        self.id = id
        self.name_offset = name_offset
        self.flags1 = flags1
        self.flags2 = flags2
        self.offset = offset
        self.size = size
        self.name = None

    @property
    def encrypted(self):
        return bool(self.flags1 & FLAG_ENCRYPTED)

    @property
    def key_index(self):
        return (self.flags2 & KEY_INDEX_MASK) >> 12

    def __repr__(self):
        label = self.name or f'0x{self.id:08x}'
        return f'<Entry {label} id=0x{self.id:08x} size={self.size}{" encrypted" if self.encrypted else ""}>'


def read_header(source):
    """The fields we act on. Big-endian."""
    head = source.read(0, HEADER_SIZE)
    if head[:4] != PKG_MAGIC:
        raise PkgError(f'not a PS4 .pkg (magic {head[:4].hex(" ")}, expected {PKG_MAGIC.hex(" ")})')
    return {
        'entry_count': be32(head, 0x10),
        'entry_table_offset': be32(head, 0x18),
        'content_id': head[0x40:0x64].split(b'\0', 1)[0].decode('ascii', 'replace'),
        'drm_type': be32(head, 0x70),
        'content_type': be32(head, 0x74),
        'content_flags': be32(head, 0x78),
    }


def read_entries(source, header):
    """Every table entry, with its name attached when it has one."""
    count = header['entry_count']
    if count == 0:
        raise PkgError('package has no entries')
    if count > 1 << 20:
        raise PkgError(f'unreasonable entry count: {count}')
    table = source.read(header['entry_table_offset'], count * ENTRY_SIZE)

    entries = []
    for index in range(count):
        id, name_offset, flags1, flags2, offset, size = struct.unpack_from(
            '>IIIIII', table, index * ENTRY_SIZE)
        entries.append(Entry(id, name_offset, flags1, flags2, offset, size))

    # The entry with id 0x200 holds every file name as NUL-separated strings.
    names = next((e for e in entries if e.id == ENTRY_NAMES_ID), None)
    if names:
        blob = source.read(names.offset, names.size)
        for entry in entries:
            # Offset 0 is valid (it is the first name in the blob); an entry with no
            # name either points past the blob or at an empty string.
            if entry.name_offset < len(blob):
                entry.name = blob[entry.name_offset:].split(b'\0', 1)[0].decode('utf-8', 'replace')
    return entries


def safe_relative(name):
    """The entry name as a relative path, or None when it must not be written.

    Entry names come from the file and are untrusted input: they may be absolute, may
    escape with '..', or may carry a Windows drive. Anything that would write outside
    the output folder is refused, not sanitised quietly.
    """
    if not name:
        return None
    normalised = name.replace('\\', '/')
    if normalised.startswith('/') or ':' in normalised.split('/')[0]:
        return None
    parts = []
    for part in normalised.split('/'):
        if part in ('', '.'):
            continue
        if part == '..' or ':' in part:
            return None
        parts.append(part)
    return Path(*parts) if parts else None


def as_source(image_or_path):
    """Accept a bytes-like image or a path; always return a source to read from."""
    if isinstance(image_or_path, (bytes, bytearray, memoryview)):
        return BytesSource(bytes(image_or_path))
    return FileSource(image_or_path)


def analyse(image_or_path):
    """{'header', 'entries', 'files', 'encrypted', 'delta'} for a package image."""
    if isinstance(image_or_path, (BytesSource, FileSource)):
        source = image_or_path
        close = False
    else:
        source, close = as_source(image_or_path), True
    try:
        return _analyse(source)
    finally:
        if close:
            source.close()


def _analyse(source):
    header = read_header(source)
    entries = read_entries(source, header)

    files, encrypted = [], []
    for entry in entries:
        if entry.id < STRUCTURAL_ID_MAX:
            continue  # digests, keys, names, metadata: part of the container
        if not entry.name:
            continue
        if safe_relative(entry.name) is None:
            continue  # refuses to write outside the folder
        (encrypted if entry.encrypted else files).append(entry)

    delta = (header['content_type'] == CONTENT_TYPE_DELTA_PATCH or
             (header['content_flags'] & CONTENT_FLAG_DELTA_PATCH) == CONTENT_FLAG_DELTA_PATCH)
    return {'header': header, 'entries': entries, 'files': files,
            'encrypted': encrypted, 'delta': delta}


def extract(image_or_path, out, report=None):
    """Write every unencrypted named entry under `out`. Returns (analysis, count)."""
    report = report or (lambda _message: None)
    if isinstance(image_or_path, (BytesSource, FileSource)):
        source, close = image_or_path, False
    else:
        source, close = as_source(image_or_path), True
    try:
        return _extract(source, out, report)
    finally:
        if close:
            source.close()


def _extract(source, out, report):
    info = analyse(source)
    header = info['header']
    if info['delta']:
        raise PkgError('this is a delta patch package (differences against a base game), '
                       'which cannot be unpacked on its own. Extract the base package, or '
                       'use a cumulative patch, or dump the game from the console.')

    out = Path(out)
    written = 0
    for entry in info['files']:
        if entry.size == 0:
            continue
        target = out / safe_relative(entry.name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read(entry.offset, entry.size))
        written += 1
        report(f'  {entry.name} ({entry.size} bytes)')

    if header['drm_type'] == DRM_TYPE_PS4:
        report('  note: drm_type 0x0F: this is a retail package. Only its plaintext'
               ' metadata (sce_sys, icons, trophies) is in the outer container; the game'
               ' itself sits in an encrypted PFS image inside it and cannot be read here.')
    if info['encrypted']:
        report(f'  skipped {len(info["encrypted"])} encrypted entries')
    return info, written


# What makes a folder a playable game rather than bare metadata. param.sfo is
# deliberately not here: every retail package has one, so counting it would make a
# metadata-only extraction look like a successful game.
GAME_MARKERS = {'eboot.bin', 'eboot.elf', 'eboot.self',
                'big2-ps4_shipping.elf', 'big3-ps4_shipping.elf'}


def game_available(name):
    """True when a package entry name is one of the files a game folder needs."""
    return bool(name) and name.split('/')[-1].lower() in GAME_MARKERS


def describe(info):
    header = info['header']
    lines = [f'{header["content_id"]}  type={header["content_type"]:#x} '
             f'flags={header["content_flags"]:#x} drm={header["drm_type"]:#x}',
             f'{len(info["entries"])} entries: {len(info["files"])} named files, '
             f'{len(info["encrypted"])} encrypted']
    if info['delta']:
        lines.append('delta patch package: cannot be applied on its own')
    lines.extend(f'  {e.name} ({e.size} bytes)' for e in info['files'][:40])
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description='Extract a PS4 .pkg into a game folder.')
    parser.add_argument('pkg', type=Path, help='the .pkg file')
    parser.add_argument('--out', type=Path, help='folder to write the game into')
    parser.add_argument('--list', action='store_true',
                        help='list what is inside without writing anything')
    args = parser.parse_args(argv)
    if not args.list and args.out is None:
        parser.error('--out is required unless --list is given')

    try:
        with FileSource(args.pkg) as source:
            if args.list:
                info = analyse(source)
                for line in describe(info):
                    print(line)
                return 1 if info['delta'] or (not info['files'] and info['encrypted']) else 0

            info, written = extract(source, args.out, report=lambda m: print(m))
    except (PkgError, ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')

    header = info['header']
    print(f'Extracted {written} files to {args.out} ({header["content_id"]})')
    if info['encrypted']:
        print(f'{len(info["encrypted"])} entries were encrypted and were skipped.')

    # A retail package yields its metadata and nothing else: the game is in an
    # encrypted PFS image. Say so, rather than leaving an icons-only folder that
    # looks like a successful extraction.
    has_game = any(game_available(entry.name) for entry in info['files'])
    if not has_game:
        if header['drm_type'] == DRM_TYPE_PS4:
            print('\nThis is a retail (encrypted) package: what came out is only the metadata '
                  'inside the container. The game itself is in an encrypted PFS image and '
                  'cannot be unpacked here.\n'
                  'Use your own decrypted dump of the game folder (eboot.bin, sce_sys, u1data...).')
            return 1
        if not written:
            print('\nNothing was extracted. A .pkg downloaded from the PS Store, or ripped from '
                  'a disc, is encrypted and has to be decrypted before it can be used. This tool '
                  'only opens packages that are already unencrypted.')
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
"""Unpack a PS4 fake PKG (fPKG) into a game folder (eboot.bin, sce_module, sce_sys, ...).

Follows shadPS4's PKG::Extract. Only fake PKGs (image key index 3) are supported.

Usage:
  python scripts/pkg_extract.py <game.pkg> --list
  python scripts/pkg_extract.py <game.pkg> <out_dir>                    # resumable
  python scripts/pkg_extract.py <game.pkg> <out_dir> --only "*.prx" eboot.bin
"""
import argparse
import fnmatch
import hashlib
import hmac
import os
import struct
import sys
import time
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

import pkg_keys

PKG_MAGIC = 0x7F434E54
PFSC_MAGIC = b"PFSC"
XTS_SECTOR = 0x1000
PFS_FILE, PFS_DIR, PFS_CUR, PFS_PARENT = 2, 3, 4, 5
INODE_SIZE = 0xA8
CHUNK_BLOCKS = 256  # 16 MB of decompressed data per read

ENTRY_NAMES = {
    0x0001: "digests", 0x0010: "entry_keys", 0x0020: "image_key", 0x0080: "general_digests",
    0x0100: "metas", 0x0200: "entry_names", 0x0400: "license.dat", 0x0401: "license.info",
    0x0402: "nptitle.dat", 0x0403: "npbind.dat", 0x0404: "selfinfo.dat", 0x0406: "imageinfo.dat",
    0x0407: "target-deltainfo.dat", 0x0408: "origin-deltainfo.dat", 0x0409: "psreserved.dat",
    0x1000: "param.sfo", 0x1001: "playgo-chunk.dat", 0x1002: "playgo-chunk.sha",
    0x1003: "playgo-manifest.xml", 0x1004: "pronunciation.xml", 0x1005: "pronunciation.sig",
    0x1006: "pic1.png", 0x1007: "pubtoolinfo.dat", 0x1008: "app/playgo-chunk.dat",
    0x1009: "app/playgo-chunk.sha", 0x100A: "app/playgo-manifest.xml", 0x100B: "shareparam.json",
    0x100C: "shareoverlayimage.png", 0x100D: "save_data.png", 0x100E: "shareprivacyguardimage.png",
    0x1200: "icon0.png", 0x1220: "pic0.png", 0x1240: "snd0.at9", 0x1280: "icon0.dds",
    0x12A0: "pic0.dds", 0x12C0: "pic1.dds", 0x1400: "trophy/trophy00.trp",
}


def rsa_pkcs1_decrypt(c: bytes, n: int, d: int) -> bytes:
    m = pow(int.from_bytes(c, "big"), d, n).to_bytes(256, "big")
    if m[0] != 0 or m[1] != 2:
        raise ValueError("RSA padding check failed (not a fake PKG?)")
    return m[m.index(0, 2) + 1:]


class Pkg:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        try:
            self._open()
        except BaseException:
            self.f.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.f.close()

    def _open(self):
        h = self.f.read(0x1000)
        be32 = lambda o: struct.unpack_from(">I", h, o)[0]
        be64 = lambda o: struct.unpack_from(">Q", h, o)[0]
        if len(h) < 0x1000 or be32(0) != PKG_MAGIC:
            raise ValueError("not a PS4 PKG")
        self.content_id = h[0x40:0x64].rstrip(b"\0").decode()
        self.entry_count = be32(0x10)
        self.table_offset = be32(0x18)
        self.pfs_offset = be64(0x410)
        self.pfs_size = be64(0x418)

        self.f.seek(self.table_offset)
        table = self.f.read(self.entry_count * 32)
        self.entries = []
        for i in range(self.entry_count):
            raw = table[i * 32:(i + 1) * 32]
            eid, _fn, fl1, fl2, off, size = struct.unpack_from(">IIIIII", raw)
            self.entries.append(dict(id=eid, flags1=fl1, flags2=fl2, offset=off, size=size, raw=raw))
        self._derive_keys()
        self._open_pfsc()

    def read_raw(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def entry_data(self, e):
        return self.read_raw(e["offset"], e["size"])

    def _derive_keys(self):
        by_id = {e["id"]: e for e in self.entries}
        ek = self.entry_data(by_id[0x10])
        key1_3 = ek[32 + 7 * 32 + 3 * 256:32 + 7 * 32 + 4 * 256]
        self.dk3 = rsa_pkcs1_decrypt(key1_3, pkg_keys.PKGDERIVEDKEY3KEYSET_N, pkg_keys.PKGDERIVEDKEY3KEYSET_D)[:32]
        ik = by_id[0x20]
        if (ik["flags2"] >> 12) & 0xF != 3:
            raise ValueError("image key index %d: only fake PKGs (index 3) can be unpacked" % ((ik["flags2"] >> 12) & 0xF))
        self.entry_ivkey = lambda e: hashlib.sha256(e["raw"] + self.dk3).digest()
        ivkey = self.entry_ivkey(ik)
        dec = Cipher(algorithms.AES(ivkey[16:32]), modes.CBC(ivkey[:16])).decryptor()
        imgkey = dec.update(self.entry_data(ik)) + dec.finalize()
        self.ekpfs = rsa_pkcs1_decrypt(imgkey, pkg_keys.FAKEKEYSET_N, pkg_keys.FAKEKEYSET_D)[:32]
        seed = self.read_raw(self.pfs_offset + 0x370, 16)
        d = hmac.new(self.ekpfs, struct.pack("<I", 1) + seed, hashlib.sha256).digest()
        self.tweak_key, self.data_key = d[:16], d[16:32]
        self.xts_key = self.data_key + self.tweak_key

    def decrypt_entry(self, e):
        data = self.entry_data(e)
        if not (e["flags1"] & 0x80000000):
            return data
        ivkey = self.entry_ivkey(e)
        n = len(data) & ~15
        dec = Cipher(algorithms.AES(ivkey[16:32]), modes.CBC(ivkey[:16])).decryptor()
        return dec.update(data[:n]) + dec.finalize() + data[n:]

    # Outer PFS image: AES-XTS with 0x1000-byte sectors numbered from the image start.
    def read_pfs(self, off, size):
        start = off & ~(XTS_SECTOR - 1)
        end = (off + size + XTS_SECTOR - 1) & ~(XTS_SECTOR - 1)
        enc = self.read_raw(self.pfs_offset + start, end - start)
        out = bytearray(len(enc))
        sector = start // XTS_SECTOR
        for i in range(0, len(enc), XTS_SECTOR):
            d = Cipher(algorithms.AES(self.xts_key), modes.XTS((sector + i // XTS_SECTOR).to_bytes(16, "little"))).decryptor()
            out[i:i + XTS_SECTOR] = d.update(enc[i:i + XTS_SECTOR])
        return bytes(out[off - start:off - start + size])

    def _open_pfsc(self):
        self.pfsc_offset = None
        for o in range(0x20000, min(self.pfs_size, 0x4000000), 0x10000):
            if self.read_pfs(o, 4) == PFSC_MAGIC:
                self.pfsc_offset = o
                break
        if self.pfsc_offset is None:
            raise ValueError("PFSC image not found")
        hdr = self.read_pfs(self.pfsc_offset, 0x30)
        _m, _u4, _u8, _bs, self.block_size, block_offsets, _data_start, data_length = struct.unpack_from("<iiiiqqQq", hdr)
        self.num_blocks = data_length // self.block_size
        raw = self.read_pfs(self.pfsc_offset + block_offsets, (self.num_blocks + 1) * 8)
        self.sector_map = struct.unpack("<%dQ" % (self.num_blocks + 1), raw)

    def _inflate_block(self, comp):
        if len(comp) == self.block_size:
            return comp
        out = zlib.decompress(comp)
        return out.ljust(self.block_size, b"\0")

    def read_blocks(self, first, count):
        """Inner PFS blocks [first, first+count), decompressed."""
        lo, hi = self.sector_map[first], self.sector_map[first + count]
        comp = self.read_pfs(self.pfsc_offset + lo, hi - lo)
        parts = []
        for b in range(first, first + count):
            s, e = self.sector_map[b] - lo, self.sector_map[b + 1] - lo
            parts.append(self._inflate_block(comp[s:e]))
        return b"".join(parts)

    # Inner PFS
    def load_fs(self):
        h = self.read_blocks(0, 1)
        mode, = struct.unpack_from("<H", h, 0x1C)
        self.fs_block = struct.unpack_from("<i", h, 0x20)[0]
        dinode_count, = struct.unpack_from("<q", h, 0x30)
        dinode_blocks, = struct.unpack_from("<q", h, 0x40)
        self.superroot, = struct.unpack_from("<q", h, 0x48)
        if self.fs_block != self.block_size:
            raise ValueError("unexpected PFS block size %#x" % self.fs_block)
        # Unsigned 32-bit dinodes (0x64 header + 12 direct + 5 indirect u32 = 0xA8); fPKG inner PFS.
        if mode & 0x3:
            raise ValueError("unexpected PFS mode %#x (expected unsigned 32-bit inodes)" % mode)
        self.inodes = []
        data = self.read_blocks(1, dinode_blocks)
        for b in range(dinode_blocks):
            blk = data[b * self.block_size:(b + 1) * self.block_size]
            for p in range(0, self.block_size - INODE_SIZE + 1, INODE_SIZE):
                m, _nl, flags, size = struct.unpack_from("<HHIq", blk, p)
                if m == 0:
                    break
                blocks, loc = struct.unpack_from("<II", blk, p + 0x60)
                self.inodes.append(dict(mode=m, flags=flags, size=size, blocks=blocks, loc=loc))
        if len(self.inodes) < dinode_count:
            raise ValueError("read %d of %d inodes" % (len(self.inodes), dinode_count))

    def read_dir(self, ino):
        node = self.inodes[ino]
        data = self.read_blocks(node["loc"], node["blocks"])
        out = []
        for b in range(0, len(data), self.block_size):
            p = b
            while p + 16 <= b + self.block_size:
                d_ino, d_type, nlen, esize = struct.unpack_from("<iiii", data, p)
                if d_ino == 0 or esize <= 0:
                    break
                name = data[p + 16:p + 16 + nlen].decode("utf-8", "replace")
                out.append((name, d_ino, d_type))
                p += esize
        return out

    def walk(self):
        """Yield (relative_path, inode, type) for everything under uroot."""
        root = dict((n, (i, t)) for n, i, t in self.read_dir(self.superroot))
        if "uroot" not in root:
            raise ValueError("no uroot in PFS")
        stack = [("", root["uroot"][0])]
        while stack:
            prefix, ino = stack.pop()
            for name, child, t in self.read_dir(ino):
                if t in (PFS_CUR, PFS_PARENT):
                    continue
                rel = prefix + "/" + name if prefix else name
                yield rel, child, t
                if t == PFS_DIR:
                    stack.append((rel, child))


def matches(rel, patterns):
    rel = rel.lower()
    return any(fnmatch.fnmatchcase(rel, p.lower()) for p in patterns)


def target_parts(rel):
    parts = rel.split("/")
    if any(part in ("", ".", "..") or ":" in part or "\\" in part for part in parts):
        raise ValueError("unsafe path in the package: %r" % rel)
    return parts


def listing(pkg, only=None):
    """Sorted (rel, inode, type) of what `extract` writes; `only`: case-insensitive globs."""
    if not hasattr(pkg, "inodes"):
        pkg.load_fs()
    items = sorted(pkg.walk())
    if only:
        items = [(rel, ino, t) for rel, ino, t in items if t == PFS_FILE and matches(rel, only)]
    for rel, _ino, _t in items:
        target_parts(rel)
    return items


def is_complete(out, rel, size):
    path = os.path.join(out, *target_parts(rel))
    return os.path.isfile(path) and os.path.getsize(path) == size


def plan(pkg_path, out, only=None):
    """(total, remaining) bytes of the files `extract` writes into `out`; `remaining` leaves out complete files."""
    with Pkg(pkg_path) as pkg:
        total = remaining = 0
        for rel, ino, t in listing(pkg, only):
            if t != PFS_FILE:
                continue
            size = pkg.inodes[ino]["size"]
            total += size
            if not is_complete(out, rel, size):
                remaining += size
    return total, remaining


def write_metadata(pkg, out):
    """PKG metadata entries go to sce_sys (as shadPS4 does); the PFS copy wins where both exist."""
    for e in pkg.entries:
        if e["id"] < 0x400:
            continue
        dst = os.path.join(out, "sce_sys", ENTRY_NAMES.get(e["id"], str(e["id"])))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fo:
            fo.write(pkg.decrypt_entry(e))


def extract(pkg_path, out, progress=None, only=None):
    """Unpacks the package into `out`; resumable (complete files are kept).
    `only`: case-insensitive globs on the package path (None = everything).
    progress(done_bytes, total_bytes, mb_per_s) is called as data is written."""
    out = os.path.abspath(out)
    with Pkg(pkg_path) as pkg:
        items = listing(pkg, only)
        total = sum(pkg.inodes[i]["size"] for _, i, t in items if t == PFS_FILE)
        os.makedirs(os.path.join(out, "sce_sys"), exist_ok=True)
        write_metadata(pkg, out)

        done = 0
        t0 = time.time()
        for rel, ino, t in items:
            dst = os.path.join(out, *target_parts(rel))
            if t == PFS_DIR:
                os.makedirs(dst, exist_ok=True)
                continue
            node = pkg.inodes[ino]
            if node["flags"] & 0x1:
                raise ValueError("%s: PFS-compressed file not supported" % rel)
            if is_complete(out, rel, node["size"]):
                done += node["size"]
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + ".part"
            remaining = node["size"]
            with open(tmp, "wb") as fo:
                b = 0
                while remaining > 0:
                    n = min(CHUNK_BLOCKS, node["blocks"] - b)
                    data = pkg.read_blocks(node["loc"] + b, n)
                    w = data[:remaining]
                    fo.write(w)
                    remaining -= len(w)
                    done += len(w)
                    b += n
                    if progress:
                        progress(done, total, done / 1048576 / max(time.time() - t0, 1e-6))
            os.replace(tmp, dst)
    return out


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit)
        n /= 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pkg")
    parser.add_argument("out", nargs="?")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--only", nargs="+", metavar="PATTERN", help="extract only paths matching these globs (case-insensitive)")
    args = parser.parse_args()

    if args.list or not args.out:
        with Pkg(args.pkg) as pkg:
            for rel, ino, t in listing(pkg, args.only):
                print("%s %12d  %s" % ("d" if t == PFS_DIR else "-", pkg.inodes[ino]["size"] if t == PFS_FILE else 0, rel))
        return

    last = [-1]

    def report(done, total, rate):
        percent = 100 * done // max(total, 1)
        if percent == last[0]:
            return
        last[0] = percent
        sys.stdout.write("\r  %3d%%  %s / %s  %.0f MB/s" % (percent, human(done), human(total), rate))
        sys.stdout.flush()

    extract(args.pkg, args.out, report, args.only)
    print("\ndone: %s -> %s" % (args.pkg, os.path.abspath(args.out)))


if __name__ == "__main__":
    main()

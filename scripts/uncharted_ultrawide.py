#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Wide-screen (21:9, 32:9, 16:10, ...) game patches for the Uncharted executables.

Writes out/<exe>/patches.bin (BBPATCH2, read by bb-probe --patches) for a requested render size.
The patches are applied in memory at start; no game file is changed.

What is patched (Uncharted: Drake's Fortune, eboot.bin; see SITES):
  1. The game's default video configuration (function 0x8d0710 fills a global at 0x1276e74):
     the 1920x1080 size fields and the two 16:9 aspect floats become width x height and width/height.
  2. The camera's field of view. Camera::Setup (0x6cb1b0) stores x_scale = 1 / tan(fov / 2) and
     y_scale = x_scale * aspect, so a wider aspect alone only crops the top and bottom. Scaling the
     numerator of x_scale by 1/k, k = aspect / (16/9), keeps the vertical view of 16:9 and widens the
     horizontal one (Hor+). That numerator is a constant (1.0 at 0xbd1be4) used by Setup alone.

  3. The memory budget. The game sizes its Render Target heap (ALLOCATION_RENDER_TARGET, 497 MiB at
     0xd358b8) for 1080p and aborts with "HeapArena::Allocate: out of memory" at larger sizes. The heap
     is enlarged in proportion to the pixel count, and the GPU memory pool that holds the budgets
     (2988 MiB at 0xd34ab8) grows by the same amount so they still fit. BB_ULTRAWIDE_RT_MIB sets the
     heap size in MiB directly (for testing when the automatic size is not enough). The startup check
   of the memory contexts adds the pool up against a fixed total (4.5 GiB for kAllocSystemDirect, an
   immediate at 0x4000cb), so that total grows by the same amount. The game's heap arena setup also
   truncates an area's size to 32 bits (`mov eax,eax` at 0x850459), so pools of 4 GiB and more get two
   NOPs there.

Every site is verified byte for byte against the executable before anything is written. A different
build (other dump version) is left unpatched, with a message, and the game starts as usual.

Usage: uncharted_ultrawide.py --exe eboot.bin --elf out/eboot/eboot.elf --size 2560x1080 --out out/eboot/patches.bin
       --size off (or 1920x1080) removes the patch file.
"""
import argparse
import math
import os
import struct
import sys
from pathlib import Path

EBOOT_BASE = 0x400000
NATIVE = (1920, 1080)
NATIVE_ASPECT = 16 / 9
MIB = 1 << 20
DMEM_DEFAULT_MIB = 5056  # the port's direct memory size (BB_DMEM_MB), as on a retail PS4
DMEM_MAX_MIB = 16384
MEMORY_MARGIN = 1.3  # extra room over a plain pixel-count scaling of the render target heap

# Sites per executable: (runtime address, expected instruction bytes, offset of the immediate in it).
# Runtime address = ELF virtual address + EBOOT_BASE (the addresses of patches/*.xml).
SITES = {
    'eboot.bin': {
        'width': [(0x8d0733, 'c7400880070000', 3), (0x8d0748, 'c7401880070000', 3),
                  (0x8d075d, 'c7402480070000', 3), (0x8d07a3, 'c7403880070000', 3)],
        'height': [(0x8d073a, 'c7400c38040000', 3), (0x8d074f, 'c7401c38040000', 3),
                   (0x8d0764, 'c7402838040000', 3), (0x8d07aa, 'c7403c38040000', 3)],
        'aspect': [(0x8d0756, 'c74020398ee33f', 3), (0x8d076b, 'c7402c398ee33f', 3)],
        # Camera::Setup: x_scale = NUMERATOR / tan(fov * 0.5); the constant 1.0 has this one user.
        'fov_numerator': (0xbd1be4, '0000803f', 0),
        # HUD: the HUD is laid out in a virtual 1280x720 space that maps to the whole screen, so on a wider screen
        # it is stretched. The x scale 2/1280 (to clip space; x * 2/1280 - 1) is a float constant used by the 2D
        # vertex transforms; it becomes 2/(1280 k), the virtual space 1280 k wide. Constants that are 16 byte
        # vectors (x scale, y scale, -1, 1) or splats are patched at their x lanes.
        'hud_scale': [0xc045e4, 0xc045f4, 0xbff1f0, 0xbff1f4, 0xbff1f8, 0xbff1fc, 0xc00f20, 0xc00f30, 0xc00f40,
                      0xc04ad8, 0xc04ae4, 0xc04b78, 0xc04b84],
        # Absolutely placed elements (menus, crosshair) stay a centred 16:9 area: the x offset -1 of the transform
        # becomes -1/k (the vector constants' offset lanes, and the scalar one in the transform at 0x8da708, whose
        # operand is pointed at the first vector's lane). Anchored elements keep the screen edges: the safe area
        # (filled by 0x8d06d0) is widened by half of the extra width on both sides, left 50 and right 1229.
        'hud_offset': [0xc00f28, 0xc00f38, 0xc00f48],
        'hud_offset_scalar': (0x8da708, 'c5fa5805d89e3200', 4, 0xc045e8),
        'hud_left': [(0x8d06d0, 'c7475032000000', 3)],
        'hud_right': [(0x8d06de, 'c74758cd040000', 3), (0x8d06ec, '48b89c040000a4020000', 2),
                      (0x8d06fa, '48b8ce040000ba020000', 2)],
        # Memory budgets (u64 sizes in 32-byte records of the game's pool tables, in the read-only data).
        'rt_heap': (0xd358b8, '00bc101f00000000'),     # ALLOCATION_RENDER_TARGET: 521190400
        'rt_small': (0xd35898, '0040bc0800000000'),     # the other ALLOCATION_RENDER_TARGET ("Render Target"): 146554880
        'gpu_pool': (0xd34ab8, '0000c0ba00000000'),    # direct memory type 3 (GPU): 3133145088
        # The startup check of the memory contexts (0x853d40) compares each context's entries with a fixed total.
        # kAllocSystemDirect (20 MiB + 1600 MiB CPU + the GPU pool) expects `movabs rdx, 0x120000000` here.
        'context_total': (0x4000cb, '48ba0000002001000000', 2),
        # The heap arena initializers (0x8503a0 and the variant at 0x8505xx) cut the arena size to 32 bits with
        # `mov eax,eax`, so a GPU pool of 4 GiB or more shrinks by 4 GiB. Two NOPs keep all 64 bits.
        'arena_truncation': [(0x850459, '89c0'), (0x85058b, '89c0')],
    },
}


class Mismatch(Exception):
    pass


def parse_size(text):
    try:
        w, h = (int(v) for v in text.lower().replace('×', 'x').split('x'))
    except ValueError:
        raise ValueError(f'bad size {text!r}, expected WIDTHxHEIGHT (for example 2560x1080)')
    if not (640 <= w <= 16384 and 360 <= h <= 16384 and w % 2 == 0 and h % 2 == 0):
        raise ValueError(f'unsupported size {w}x{h}')
    return w, h


def load_segments(elf):
    """[(vaddr, file offset, filesz)] of the ELF's loaded segments (PT_LOAD and the SCE relro data)."""
    if elf[:4] != b'\x7fELF':
        raise Mismatch('not an ELF image')
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    out = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if kind in (1, 0x61000010) and filesz:
            out.append((vaddr, offset, filesz))
    return out


def read_at(elf, segments, address, length):
    vaddr = address - EBOOT_BASE
    for start, offset, filesz in segments:
        if start <= vaddr and vaddr + length <= start + filesz:
            return elf[offset + vaddr - start:offset + vaddr - start + length]
    raise Mismatch(f'address {address:#x} is not in the executable')


def check(elf, segments, address, expected_hex):
    expected = bytes.fromhex(expected_hex)
    found = read_at(elf, segments, address, len(expected))
    if found != expected:
        raise Mismatch(f'{address:#x}: expected {expected.hex()}, found {found.hex()}')


def build_writes(exe, elf, width, height):
    """[(image offset, bytes)] for a render size; Mismatch when the executable is not the expected build."""
    sites = SITES.get(exe)
    if not sites:
        raise Mismatch(f'no ultrawide patch for {exe} yet')
    segments = load_segments(elf)
    aspect = width / height
    k = max(1.0, aspect / NATIVE_ASPECT)  # taller than 16:9: the horizontal view is kept, nothing to scale
    values = {'width': struct.pack('<I', width), 'height': struct.pack('<I', height),
              'aspect': struct.pack('<f', aspect)}
    writes = []
    for kind, data in values.items():
        for address, expected, imm in sites[kind]:
            check(elf, segments, address, expected)
            writes.append((address + imm - EBOOT_BASE, data))
    address, expected, imm = sites['fov_numerator']
    check(elf, segments, address, expected)
    if k != 1.0:
        writes.append((address + imm - EBOOT_BASE, struct.pack('<f', 1.0 / k)))
    if k != 1.0 and 'hud_scale' in sites:
        scale_old = struct.pack('<f', 2 / 1280)
        for address in sites['hud_scale']:
            check(elf, segments, address, scale_old.hex())
            writes.append((address - EBOOT_BASE, struct.pack('<f', 2 / (1280 * k))))
        for address in sites['hud_offset']:
            check(elf, segments, address, struct.pack('<f', -1.0).hex())
            writes.append((address - EBOOT_BASE, struct.pack('<f', -1.0 / k)))
        address, expected, imm, old_target = sites['hud_offset_scalar']
        check(elf, segments, address, expected)
        next_ip = address + len(expected) // 2
        assert next_ip + struct.unpack('<i', bytes.fromhex(expected)[imm:])[0] == old_target
        writes.append((address + imm - EBOOT_BASE, struct.pack('<i', sites['hud_offset'][0] - next_ip)))
        half = round(640 * (k - 1.0))
        for address, expected, imm in sites['hud_left']:
            check(elf, segments, address, expected)
            old = struct.unpack('<i', bytes.fromhex(expected)[imm:imm + 4])[0]
            writes.append((address + imm - EBOOT_BASE, struct.pack('<i', old - half)))
        for address, expected, imm in sites['hud_right']:
            check(elf, segments, address, expected)
            old = struct.unpack('<I', bytes.fromhex(expected)[imm:imm + 4])[0]
            writes.append((address + imm - EBOOT_BASE, struct.pack('<I', old + half)))
    # Memory: the render target heap grows with the pixel count (plus a margin); the GPU pool by the same amount.
    rt_address, rt_expected = sites['rt_heap']
    pool_address, pool_expected = sites['gpu_pool']
    check(elf, segments, rt_address, rt_expected)
    check(elf, segments, pool_address, pool_expected)
    rt_old = struct.unpack('<Q', bytes.fromhex(rt_expected))[0]
    pool_old = struct.unpack('<Q', bytes.fromhex(pool_expected))[0]
    total_address, total_expected, total_imm = sites['context_total']
    check(elf, segments, total_address, total_expected)
    total_old = struct.unpack('<Q', bytes.fromhex(total_expected)[total_imm:])[0]
    ratio = (width * height) / (NATIVE[0] * NATIVE[1])
    forced = os.environ.get('BB_ULTRAWIDE_RT_MIB', '').strip()
    if forced:
        rt_new = int(forced) * MIB
    else:
        rt_new = rt_old + math.ceil(max(ratio - 1.0, 0.0) * rt_old * MEMORY_MARGIN / MIB) * MIB
    rt_new = max(rt_new, rt_old)
    # The game asserts that every memory area is a multiple of 2 MiB (IsSizeAligned(sizeMB, 2)): grow by whole 2 MiB.
    grow = -(-(rt_new - rt_old) // (2 * MIB)) * 2 * MIB
    rt_new = rt_old + grow
    # The second, smaller render target heap holds the 4 byte per pixel targets and grows the same way.
    small_address, small_expected = sites['rt_small']
    check(elf, segments, small_address, small_expected)
    small_old = struct.unpack('<Q', bytes.fromhex(small_expected))[0]
    small_grow = -(-math.ceil(max(ratio - 1.0, 0.0) * small_old * MEMORY_MARGIN) // (2 * MIB)) * 2 * MIB
    extra = grow + small_grow  # all that the GPU memory context (and the pool holding it) gains
    pool_new = pool_old + extra
    dmem_mib = DMEM_DEFAULT_MIB + math.ceil(extra / MIB)  # direct memory the port must offer
    if dmem_mib > DMEM_MAX_MIB:
        raise Mismatch(f'{width}x{height} needs {dmem_mib} MiB of direct memory; the port allows {DMEM_MAX_MIB} MiB '
                       '(try a smaller size, or BB_ULTRAWIDE_RT_MIB)')
    if pool_new >= 1 << 32:
        for address, expected in sites['arena_truncation']:
            check(elf, segments, address, expected)
            writes.append((address - EBOOT_BASE, b'\x66\x90'))
    if extra:
        writes.append((rt_address - EBOOT_BASE, struct.pack('<Q', rt_new)))
        if small_grow:
            writes.append((small_address - EBOOT_BASE, struct.pack('<Q', small_old + small_grow)))
        writes.append((pool_address - EBOOT_BASE, struct.pack('<Q', pool_new)))
        writes.append((total_address + total_imm - EBOOT_BASE, struct.pack('<Q', total_old + extra)))
    return writes, aspect, k, (rt_old, rt_new), dmem_mib


def write_patch_file(path, writes):
    blob = struct.pack('<8sQQ', b'BBPATCH2', EBOOT_BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    Path(path).write_bytes(blob)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', required=True, help='executable name: eboot.bin, big2-ps4_Shipping.elf, ...')
    parser.add_argument('--elf', required=True, help='the prepared ELF (out/<exe>/eboot.elf)')
    parser.add_argument('--size', default='off', help='render size WIDTHxHEIGHT, or off')
    parser.add_argument('--out', required=True, help='patch file to write (removed when off)')
    a = parser.parse_args()
    out = Path(a.out)
    out.unlink(missing_ok=True)
    memory = out.with_suffix('.dmem')  # direct memory (MiB) the port must offer; run.py raises BB_DMEM_MB to it
    memory.unlink(missing_ok=True)
    if a.size.lower() in ('', 'off', '0', 'none'):
        return 0
    try:
        width, height = parse_size(a.size)
        if (width, height) == NATIVE:
            return 0
        writes, aspect, k, (rt_old, rt_new), dmem_mib = build_writes(a.exe, Path(a.elf).read_bytes(), width, height)
    except (ValueError, Mismatch, OSError) as error:
        print(f'Ultrawide: not applied: {error}', file=sys.stderr)
        return 0  # the game still starts, unpatched
    write_patch_file(out, writes)
    memory.write_text(f'{dmem_mib}\n', encoding='utf-8')
    print(f'Ultrawide: {width}x{height} (aspect {aspect:.4f}, field of view x{k:.4f}, render target heap '
          f'{rt_old / MIB:.0f} -> {rt_new / MIB:.0f} MiB): {len(writes)} writes')
    if dmem_mib > DMEM_DEFAULT_MIB + 230:  # the default leaves about 450 MiB of slack over the 1080p needs
        print(f'Ultrawide: this size needs {dmem_mib} MiB of direct memory (BB_DMEM_MB, raised automatically; '
              f'the default is {DMEM_DEFAULT_MIB})')
    return 0


if __name__ == '__main__':
    sys.exit(main())

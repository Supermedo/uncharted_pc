"""Wide-screen field of view and UI proportions for Uncharted: Drake's Fortune 1.02 (eboot.bin).

BB_GAME_RES (runtime_display.c) gives the game a wider size and aspect ratio. Two things in the game still
assume 16:9 and are fixed here, as byte patches in memory (no game file changes):

1. Field of view. Camera::Setup stores x_scale = 1 / tan(fov / 2) and y_scale = x_scale * aspect, so a wider
   aspect alone crops the top and bottom. The numerator of x_scale (the constant 1.0, used by Setup alone)
   becomes 1 / k, k = aspect / (16/9): the vertical view of 16:9 is kept and the horizontal one widens.
2. The UI. It is laid out in a virtual 1280x720 space that the 2D vertex transforms (x * 2/1280 - 1) stretch
   over the whole screen, so on a wide screen everything is too wide (round prompts become ellipses).
   - The x scale constants become 2/(1280 k): the layout space is 1280 k wide and keeps its proportions.
   - The x offset -1 becomes -1/k: absolutely placed elements (menus, the crosshair) stay a centred 16:9 area.
   - The safe area the game fills at start (function 0x8d06d0: left 50, right 1229 and the rectangles that
     end near it) moves out by half of the extra width on each side, so the elements anchored to the screen
     edges (the left, centre and right anchors of the HUD) keep their place at the edges.
   The weapon and ammo HUD is placed at fixed positions, so it stays in the 16:9 area.

Only the executable with the SHA-256 below is patched and every site is checked byte for byte; anything else
is reported and skipped.
"""
import math
import struct
from pathlib import Path

import cheats
import prepare

SHA256 = cheats.V102['eboot.bin']
NATIVE_ASPECT = 16 / 9
BASE = 0x400000   # the addresses below are the runtime addresses of patches/*.xml; the image offset is address - BASE
F_2_1280 = struct.pack('<f', 2 / 1280).hex()
F_M1 = struct.pack('<f', -1.0).hex()

FOV_NUMERATOR = 0xbd1be4                      # 1.0, used by Camera::Setup (0x6cb1b0) only
# 2D vertex transforms: x * [scale] + [offset]. Scale constants: scalars, a splat and 16 byte vectors
# (x scale, y scale, -1, 1); the offset is the -1 lane of those vectors.
SCALES = [0xc045e4, 0xc045f4, 0xbff1f0, 0xbff1f4, 0xbff1f8, 0xbff1fc, 0xc00f20, 0xc00f30, 0xc00f40,
          0xc04ad8, 0xc04ae4, 0xc04b78, 0xc04b84]
OFFSETS = [0xc00f28, 0xc00f38, 0xc00f48]
# `vaddss xmm0, xmm0, [0xc045e8]` (the scalar -1 offset of the transform at 0x8da6a0): its operand points at
# the first vector's offset lane instead. (address, bytes, displacement offset, old target)
SCALAR_OFFSET = (0x8da708, 'c5fa5805d89e3200', 4, 0xc045e8)
# The safe area (0x8d06d0): left edge, right edge and the rectangles that end near it: (address, bytes, offset
# of the 32-bit x in them)
LEFT = [(0x8d06d0, 'c7475032000000', 3)]
RIGHT = [(0x8d06de, 'c74758cd040000', 3), (0x8d06ec, '48b89c040000a4020000', 2),
         (0x8d06fa, '48b8ce040000ba020000', 2)]


class Mismatch(Exception):
    pass


def plan(read, width, height):
    """[(address, bytes)] for a game size; read(address, length) gives the executable's bytes there."""
    k = (width / height) / NATIVE_ASPECT
    if k <= 1.0001:
        return []
    writes = []

    def check(address, expected_hex):
        expected = bytes.fromhex(expected_hex)
        found = read(address, len(expected))
        if found != expected:
            raise Mismatch(f'{address:#x}: expected {expected.hex()}, found {None if found is None else found.hex()}')

    check(FOV_NUMERATOR, struct.pack('<f', 1.0).hex())
    writes.append((FOV_NUMERATOR, struct.pack('<f', 1.0 / k)))
    for address in SCALES:
        check(address, F_2_1280)
        writes.append((address, struct.pack('<f', 2 / (1280 * k))))
    for address in OFFSETS:
        check(address, F_M1)
        writes.append((address, struct.pack('<f', -1.0 / k)))
    address, expected, at, target = SCALAR_OFFSET
    check(address, expected)
    next_ip = address + len(expected) // 2
    if next_ip + struct.unpack('<i', bytes.fromhex(expected)[at:])[0] != target:
        raise Mismatch(f'{address:#x}: not the transform expected')
    writes.append((address + at, struct.pack('<i', OFFSETS[0] - next_ip)))
    half = round(640 * (k - 1.0))
    for address, expected, at in LEFT:
        check(address, expected)
        old = struct.unpack('<i', bytes.fromhex(expected)[at:at + 4])[0]
        writes.append((address + at, struct.pack('<i', old - half)))
    for address, expected, at in RIGHT:
        check(address, expected)
        old = struct.unpack('<I', bytes.fromhex(expected)[at:at + 4])[0]
        writes.append((address + at, struct.pack('<I', old + half)))
    return writes


def parse_size(text):
    try:
        width, height = (int(part) for part in text.lower().replace('×', 'x').split('x'))
        return width, height
    except ValueError:
        return None


def compile_ui(path, size):
    """[(image offset, bytes)] for BB_GAME_RES `size` ('2560x1080') and the executable at `path`; reasons to stdout."""
    parsed = parse_size(size or '')
    if not parsed or parsed[0] / parsed[1] <= NATIVE_ASPECT * 1.0001:
        return []
    import hashlib
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        print('Ultrawide UI: this executable is not supported; the game keeps its own layout')
        return []
    elf, _header, program_headers, _segments, _missing = prepare.parse_self(data)
    try:
        writes = plan(lambda a, n: cheats.image_bytes(elf, program_headers, a - BASE, n), *parsed)
    except Mismatch as error:
        print(f'Ultrawide UI: not applied ({error})')
        return []
    print(f'Ultrawide UI: field of view and layout for {parsed[0]}x{parsed[1]}: {len(writes)} writes')
    return [(address - BASE, data) for address, data in writes]

"""Compile the enabled cheats of one executable into cheats.bin for bb-probe --patches.

Cheats are "health" and "ammo". A patch is written only when the executable's SHA-256 is listed and the bytes at its
address are the expected ones; anything else is reported and skipped, never an error.
Addresses are image offsets (virtual addresses of the SELF's load segments).
"""
import hashlib
from pathlib import Path
import struct

import prepare

NOP = '90'
V100 = {'eboot.bin': '8953b6ed5cf4d1d26aa3aa4cd1e0c72817ea155f6a5774fb17f6fec7d787f953',
        'big2-ps4_Shipping.elf': '61c0456314a8de2dbd32e1c426f7828092de7d25a58b83386ced1a48d7765ebe',
        'big3-ps4_Shipping.elf': 'acc952e5a9ac7062f8ecfc52700faa599d5f29deb1e24f9e81e6de832326d1d0'}
V102 = {'eboot.bin': 'dc017b45da8978bb11a0a11d940e96d98fa07f3c066a3debba043714432212f8',
        'big2-ps4_Shipping.elf': '5047b693b1c6fd06ccb1eacecacee60b6183bedc18d023cac276e1ec6009dfe2',
        'big3-ps4_Shipping.elf': '02c111aa7e73e445d71a1ad861702811f31d471abc61caf01732d55a77eb1c38'}

# sha256 -> cheat -> (address, original bytes, replacement)
PATCHES = {
    # health: jump to the engine's own no-damage path. ammo: the clip decrement is shared with NPC weapons, so U1/U2
    # drop the player Fire's call to it and U3 forces its player-held branch.
    V100['eboot.bin']: {'health': (0x261edb, '7535', 'eb35'),
                        'ammo': (0x71b003, 'e8c8acc2ff', NOP * 5)},
    V102['eboot.bin']: {'health': (0x26244b, '7535', 'eb35'),
                        'ammo': (0x730f73, 'e8c854c1ff', NOP * 5)},
    V100['big2-ps4_Shipping.elf']: {'health': (0x35de73, '0f84bc010000', 'e9cf02000090'),
                                    'ammo': (0xa4fd8d, 'e87e8aa6ff', NOP * 5)},
    V100['big3-ps4_Shipping.elf']: {'health': (0x33f87b, '0f84ca020000', 'e96a0b000090'),
                                    'ammo': (0x46a1f9, '4c8d3dc0660501', '41b401eb729090')},
    V102['big2-ps4_Shipping.elf']: {'health': (0x7f0143, '0f843c050000', 'e95f06000090'),
                                    'ammo': (0x961ac8, 'e853851800', NOP * 5)},
    V102['big3-ps4_Shipping.elf']: {'health': (0x9f9295, '4180bfa7000000007436', 'e9320300009090909090'),
                                    'ammo': (0xd517ac, '4c8d257d92aa0066418b442462', '40b701e9110100009090909090')},
}


def image_bytes(elf, program_headers, address, size):
    for p in program_headers:
        if p['type'] in (1, 0x61000010) and p['vaddr'] <= address and address + size <= p['vaddr'] + p['filesz']:
            start = p['offset'] + address - p['vaddr']
            return bytes(elf[start:start + size])
    return None


def compile_cheats(path, cheats):
    """[(address, replacement bytes)] for the enabled cheats; reasons go to stdout."""
    data = Path(path).read_bytes()
    table = PATCHES.get(hashlib.sha256(data).hexdigest())
    if table is None:
        print(f'Cheats: {Path(path).name} is not a supported version; none applied')
        return []
    elf, _header, program_headers, _segments, _missing = prepare.parse_self(data)
    writes = []
    for name in cheats:
        if name not in table:
            continue
        address, original, replacement = table[name]
        original, replacement = bytes.fromhex(original), bytes.fromhex(replacement)
        if image_bytes(elf, program_headers, address, len(original)) != original:
            print(f'Cheats: {name} skipped, unexpected code at {address:#x}')
            continue
        writes.append((address, replacement))
        print(f'Cheats: {name} on')
    return writes


def write_patch_file(out, writes):
    with open(out, 'wb') as f:
        f.write(b'BBPATCH2' + struct.pack('<QQ', 0, len(writes)))
        for address, data in writes:
            f.write(struct.pack('<QQ', address, len(data)) + data)


def build(path, out_dir, cheats, extra_writes=()):
    """Path of out_dir/cheats.bin for the enabled cheats (and extra_writes: [(address, bytes)] of other patches that
    share the one patch file), or None when nothing applies."""
    out = Path(out_dir) / 'cheats.bin'
    try:
        out.unlink(missing_ok=True)
        writes = (compile_cheats(path, cheats) if cheats else []) + list(extra_writes)
        if writes:
            write_patch_file(out, writes)
            return out
    except (OSError, ValueError, KeyError, IndexError) as error:
        print(f'Cheats: not applied ({error})')
    return None

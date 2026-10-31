# SPDX-License-Identifier: GPL-2.0-or-later
"""Guarded patches to the loaded game image; original SELF/ELF files stay unchanged.

Capacity/pointer arithmetic is 64 bit. Existing 32-bit query interfaces and the
legacy global usage counter saturate at UINT32_MAX instead of wrapping.
"""
import json
from pathlib import Path
import struct

MANIFEST = Path(__file__).with_name('arena64-manifest.json')
PAGE = 0x10000


class Stub:
    def __init__(self, base):
        self.base, self.data = base, bytearray()

    def raw(self, value):
        self.data.extend(bytes.fromhex(value))

    def rip(self, opcode, target):
        self.raw(opcode)
        self.data.extend(struct.pack('<i', target - (self.base + len(self.data) + 4)))

    def jump(self, target):
        self.rip('e9', target)


def apply_arena64(image, segments, source_sha256):
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    build = manifest['builds'].get(source_sha256)
    if build is None:
        return dict(version=manifest['version'], source_sha256=source_sha256, patches=[], supported=False)
    # Validate ALL regions before mutating any bytes.
    for patch in build['patches']:
        address = patch['context_address']
        expected = bytes.fromhex(patch['context'])
        if image[address:address + len(expected)] != expected:
            raise ValueError(f"Arena64: unexpected {patch['role']} instructions at {address:#x}")
    stub_base = (len(image) + PAGE - 1) & -PAGE
    stubs = bytearray()
    applied = []
    for patch in build['patches']:
        address = patch['address']
        length = len(bytes.fromhex(patch['expected']))
        if 'replacement' in patch:
            replacement = bytes.fromhex(patch['replacement'])
        else:
            stub = Stub(stub_base + len(stubs))
            if patch['role'] == 'capacity_query':
                stub.raw('488b87c8000000 baffffffff 4839d0 480f47c2 c3')
            elif patch['role'] == 'free_query':
                stub.raw('488b4758 482b4750 baffffffff 4839d0 480f47c2 c3')
            elif patch['role'] == 'gpu_usage':
                targets = patch['globals']
                stub.rip('488b0d', targets['current'])
                stub.rip('482b0d', targets['stack'])
                stub.rip('488b35', targets['packed'])
                stub.raw('48c1ee02 4801f1 baffffffff 4839d1 480f47ca 39c1 0f42c8')
                stub.rip('890d', targets['usage'])
                stub.jump(address + length)
            elif patch['role'] == 'movie_cpu_total_validation':
                # runtime_display.c first adds GPU growth to MOVABS RDX. Add the
                # movie CPU growth here, preserving its GPU-budget patch signature.
                stub.raw('4881c2')
                stub.data.extend(struct.pack('<I', patch['extra_cpu_bytes']))
                stub.raw('31c9 4889df')
                stub.jump(address + length)
            else:
                raise ValueError('Arena64: unknown stub role')
            replacement = b'\xe9' + struct.pack('<i', stub.base - address - 5)
            replacement += b'\x90' * (length - len(replacement))
            stubs.extend(stub.data)
            stubs.extend(b'\x90' * (-len(stubs) % 16))
        if len(replacement) != length:
            raise ValueError('Arena64: replacement length mismatch')
        image[address:address + length] = replacement
        applied.append(dict(role=patch['role'], address=hex(address), bytes=length))
    image.extend(b'\0' * (stub_base - len(image)))
    image.extend(stubs)
    stub_size = (len(stubs) + PAGE - 1) & -PAGE
    image.extend(b'\0' * (stub_size - len(stubs)))
    segments.append((stub_base, stub_size, 5))
    report = dict(version=manifest['version'], source_sha256=source_sha256,
                  executable=build['file'], patches=applied, supported=True,
                  stub_base=hex(stub_base), stub_bytes=len(stubs),
                  legacy_queries='uint32 saturation', arena_capacity='64-bit',
                  movie_cpu_mib=128, movie_cpu_backing_extra_mib=54)
    print(f"Arena64: {build['file']}: {len(applied)} guarded patches; 64-bit capacity/high-water, saturated legacy queries", flush=True)
    print('Movie memory: CPU reservation 128 MiB; backing and parent CPU budgets +54 MiB; bounds checks retained', flush=True)
    return report

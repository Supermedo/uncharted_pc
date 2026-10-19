"""List the guest stores that run below the red zone on Windows: patches/redzone.json.

Windows writes an exception's frame right below rsp (on Intel 12th gen and later down to the
next 64-byte boundary, AVX state in use or not), over the System V red zone, so a leaf
function's store that faults on a GPU-tracked page and is resumed loses the locals the function
keeps there (link_modules.protect_red_zones). A site is a run of a red-zone leaf's instructions,
at least 5 bytes for a jmp, holding its stores through non-stack pointers; nothing inside it may
be a branch target, rip-relative, segment-prefixed or change rsp. Functions with jump tables or
with a runtime guest patch (src/runtime_pad.c guest_callbacks) are left out.

One table per executable, keyed by the sha256 of its code segment; running the tool again for
an executable replaces its table. From the Bloodborne port (bbw tools/redzone_sites.py).

Developer tool (pip install capstone):
  python tools/redzone_sites.py <game dir> [--exe eboot.bin big2-ps4_Shipping.elf ...]
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import capstone
from capstone import x86

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare import parse_self, sfo  # noqa: E402

EXECUTABLES = ('eboot.bin', 'big2-ps4_Shipping.elf', 'big3-ps4_Shipping.elf')
RSP, RBP, RIP = x86.X86_REG_RSP, x86.X86_REG_RBP, x86.X86_REG_RIP
STACK = {RSP, x86.X86_REG_ESP, x86.X86_REG_SP, x86.X86_REG_SPL}
FLOW = {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL, capstone.CS_GRP_RET, capstone.CS_GRP_INT, capstone.CS_GRP_IRET}
NO_ACCESS = ('lea', 'nop', 'prefetch')
RED_ZONE_RSP = re.compile(rb'[\x44\x4c\x54\x5c\x64\x6c\x74\x7c]\x24[\x80-\xff]', re.S)  # [rsp-disp8]
FRAME_POINTER = bytes.fromhex('4889e5')  # mov rbp, rsp


def text_sha256(elf, ph):
    """The executable's code segment as in the file: link_modules picks its table by it."""
    text = next(p for p in ph if p['type'] == 1 and p['flags'] & 1)
    return hashlib.sha256(elf[text['offset']:text['offset'] + text['filesz']]).hexdigest()


def load(path):
    elf, _, ph, _, _ = parse_self(path.read_bytes())
    loads = [p for p in ph if p['type'] == 1]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = elf[p['offset']:p['offset'] + p['filesz']]
    text = next(p for p in loads if p['flags'] & 1)
    hdr = next(p for p in ph if p['type'] == 0x6474e550)['vaddr']
    if image[hdr + 2] != 0x03 or image[hdr + 3] != 0x3b:
        raise ValueError('unexpected .eh_frame_hdr encoding')
    count, = struct.unpack_from('<I', image, hdr + 8)
    starts = sorted({hdr + struct.unpack_from('<i', image, hdr + 12 + 8 * i)[0] for i in range(count)})
    end = text['vaddr'] + text['filesz']
    return bytes(image), [s for s in starts if text['vaddr'] <= s < end] + [end], text_sha256(elf, ph)


def patched_addresses():
    """Guest code the runtime patches at load time (by signature, in whichever game matches)."""
    source = (ROOT / 'src' / 'runtime_pad.c').read_text(encoding='utf-8')
    return sorted(int(m, 16) for m in re.findall(r'\{"[^"]+",(0x[0-9a-f]+),', source))


def stack_layout(insns):
    """rsp after the prologue and rbp as a frame pointer, both relative to the entry rsp."""
    rsp, rbp = 0, None
    for i in insns:
        ops = i.operands
        if i.mnemonic == 'push':
            rsp -= 8
        elif i.mnemonic == 'sub' and ops[0].type == x86.X86_OP_REG and ops[0].reg == RSP and ops[1].type == x86.X86_OP_IMM:
            rsp -= ops[1].imm
        elif i.mnemonic == 'mov' and ops[0].type == x86.X86_OP_REG and ops[0].reg == RBP and \
                ops[1].type == x86.X86_OP_REG and ops[1].reg == RSP:
            rbp = rsp
        else:
            break
    return rsp, rbp


def movable(i):
    _, written = i.regs_access()
    return not FLOW & set(i.groups) and not STACK & set(written) and i.prefix[1] not in (0x64, 0x65) and \
        not any(op.type == x86.X86_OP_MEM and op.mem.base == RIP for op in i.operands)


def analyse(md, image, start, end):
    """(sites, uncovered stores) of one function; None when it keeps nothing below rsp."""
    insns = []
    for i in md.disasm(image[start:end], start):
        if i.group(capstone.CS_GRP_CALL):
            return None  # not a leaf: no red zone
        insns.append(i)
    if not insns:
        return None
    body_rsp, rbp = stack_layout(insns)
    targets, red_zone, stores = set(), False, []
    for i in insns:
        if i.group(capstone.CS_GRP_JUMP):
            op = i.operands[0]
            if op.type == x86.X86_OP_IMM:
                targets.add(op.imm)
            elif not (op.type == x86.X86_OP_MEM and op.mem.base == RIP):
                return [], ['jump table']  # targets unknown: left alone
        for op in i.operands:
            if op.type != x86.X86_OP_MEM:
                continue
            if op.mem.base == RSP and op.mem.disp < 0:
                red_zone = True
            if rbp is not None and op.mem.base == RBP and rbp + op.mem.disp < body_rsp:
                red_zone = True
            if op.access & capstone.CS_AC_WRITE and not i.mnemonic.startswith(NO_ACCESS) and \
                    op.mem.base not in (RSP, RIP) and not (rbp is not None and op.mem.base == RBP):
                stores.append(i)
    if not red_zone or not stores:
        return None
    # Runs of movable instructions; a branch target starts a new run.
    runs, run = [], []
    for i in insns:
        if not movable(i) or (i.address in targets and run):
            if run:
                runs.append(run)
            run = [i] if movable(i) else []
            continue
        run.append(i)
    if run:
        runs.append(run)
    store_at = {i.address for i in stores}
    sites = []
    for run in runs:
        marked = [k for k, i in enumerate(run) if i.address in store_at]
        if not marked:
            continue
        first, last = marked[0], marked[-1]
        while sum(i.size for i in run[first:last + 1]) < 5 and (last + 1 < len(run) or first > 0):
            if last + 1 < len(run):
                last += 1
            else:
                first -= 1
        span = run[first:last + 1]
        if sum(i.size for i in span) >= 5:
            sites.append([span[0].address, b''.join(i.bytes for i in span).hex(),
                          [-i.size if i.address in store_at else i.size for i in span]])
    covered = {a for s in sites for a in range(s[0], s[0] + len(s[1]) // 2)}
    return sites, [f'{i.address:#x}' for i in stores if i.address not in covered]


def table(path, label):
    image, starts, digest = load(path)
    patched = patched_addresses()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    sites, functions, skipped, uncovered = [], 0, [], []
    for start, end in zip(starts, starts[1:]):
        code = image[start:end]
        if not (RED_ZONE_RSP.search(code) or FRAME_POINTER in code):
            continue
        result = analyse(md, image, start, end)
        if result is None:
            continue
        if any(start <= q < end for q in patched):
            skipped.append(f'{start:#x} (runtime patch)')
            continue
        found, missing = result
        if missing == ['jump table']:
            skipped.append(f'{start:#x} (jump table)')
            continue
        functions += 1
        sites += found
        uncovered += missing
    print(f'{label}: {len(sites)} sites in {functions} red-zone leaf functions; {len(uncovered)} stores '
          f'uncovered; {len(skipped)} functions skipped')
    return dict(executable=label, text_sha256=digest, functions=functions, skipped=skipped,
                uncovered_stores=uncovered, sites=sites)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path)
    p.add_argument('--exe', nargs='*', default=[e for e in EXECUTABLES])
    p.add_argument('-o', '--out', type=Path, default=ROOT / 'patches' / 'redzone.json')
    a = p.parse_args()
    info = sfo((a.game / 'sce_sys' / 'param.sfo').read_bytes())
    tables = json.loads(a.out.read_text())['tables'] if a.out.is_file() else []
    for exe in a.exe:
        if not (a.game / exe).is_file():
            continue
        new = table(a.game / exe, f"{exe} {info.get('TITLE_ID')} {info.get('APP_VER')}")
        tables = [t for t in tables if t['text_sha256'] != new['text_sha256']] + [new]
    a.out.write_text(json.dumps(dict(tables=tables), separators=(',', ':')) + '\n', encoding='ascii')
    print(f'{len(tables)} tables -> {a.out}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Recover DirectFB interface vtable layouts from a stripped Thumb-2 libdirectfb.

DirectFB fills each interface in IDirectFB*_Construct with a run of
    ldr rX, [pc, #imm]      ; literal at Align(L + 4, 4) + imm
    add rX, pc              ; at address A: rX = literal + A + 4
    str rX, [rThiz, #off]
This script finds those runs, then names each stored function by the first
"IDirectFB..." string it references (DirectFB's debug messages carry
__FUNCTION__). Output: one block per run, slot -> offset -> name.

usage: vtables.py LIB DISASM   (DISASM = llvm-objdump -d output, ARM mode)
"""
import re
import struct
import sys

lib_path, dis_path = sys.argv[1], sys.argv[2]
data = open(lib_path, 'rb').read()

# program headers: map VA -> file offset
e_phoff, = struct.unpack_from('<I', data, 0x1c)
e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x2a)
loads = []
for i in range(e_phnum):
    p_type, p_off, p_va, _, p_filesz, _, _, _ = struct.unpack_from('<8I', data, e_phoff + i * e_phentsize)
    if p_type == 1:
        loads.append((p_va, p_off, p_filesz))


def rd32(va):
    for v, o, n in loads:
        if v <= va < v + n - 3:
            return struct.unpack_from('<I', data, o + va - v)[0]
    return None


def cstr(va, maxlen=80):
    for v, o, n in loads:
        if v <= va < v + n:
            off = o + va - v
            end = data.find(b'\0', off, off + maxlen)
            if end < 0:
                return None
            s = data[off:end]
            if s and all(32 <= c < 127 for c in s):
                return s.decode()
    return None


line_re = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4} ?){1,2})\s+(\S+)\s*(.*)$')
insns = []
for line in open(dis_path, errors='replace'):
    m = line_re.match(line)
    if m:
        insns.append((int(m.group(1), 16), m.group(3), m.group(4).split('@')[0].strip()))

text_lo = min(a for a, _, _ in insns)
text_hi = max(a for a, _, _ in insns)

ldr_pc = re.compile(r'^(r\d+|r1[0-2]|lr|ip), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]$')
add_pc = re.compile(r'^(r\d+|lr), pc$')
str_off = re.compile(r'^(r\d+|lr), \[(r\d+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]$')

regs = {}
stores = []        # (addr, base, off, target)
strrefs = []       # (addr, string)
func_starts = []
for a, mn, ops in insns:
    if (mn.startswith('push') or mn.startswith('stmdb')) and 'lr' in ops:
        func_starts.append(a)
    m = ldr_pc.match(ops) if mn in ('ldr', 'ldr.w') else None
    if m:
        lit = rd32(((a + 4) & ~3) + int(m.group(2), 0))
        regs[m.group(1)] = ('lit', lit)
        continue
    m = add_pc.match(ops) if mn == 'add' else None
    if m and regs.get(m.group(1), (None,))[0] == 'lit' and regs[m.group(1)][1] is not None:
        val = (regs[m.group(1)][1] + a + 4) & 0xffffffff
        regs[m.group(1)] = ('abs', val)
        s = cstr(val)
        if s:
            strrefs.append((a, s))
        continue
    m = str_off.match(ops) if mn in ('str', 'str.w') else None
    if m and regs.get(m.group(1), (None,))[0] == 'abs':
        tgt = regs[m.group(1)][1] & ~1
        if text_lo <= tgt <= text_hi:
            stores.append((a, m.group(2), int(m.group(3) or '0', 0), tgt))
        continue
    if mn in ('bl', 'blx', 'bx', 'b', 'b.w') or mn.startswith('pop') or mn.startswith('ldm'):
        # calls clobber only caller-saved registers; keep r4-r11 for the
        # literal/add pairs the compiler splits around a call
        regs = {k: v for k, v in regs.items() if k in ('r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11')}             if mn in ('bl', 'blx') else {}

func_starts.sort()


def func_of(addr):
    import bisect
    i = bisect.bisect_right(func_starts, addr) - 1
    return func_starts[i] if i >= 0 else None


# name each function by the first interface-ish string it references
from collections import Counter
votes = {}
for a, s in strrefs:
    f = func_of(a)
    if f is None:
        continue
    # MStar's build tags functions with "_" + __FUNCTION__
    if re.match(r'^_?I[A-Z][A-Za-z]+_[A-Z][A-Za-z0-9]+$', s):
        votes.setdefault(f, Counter())[s.lstrip('_')] += 1
names = {f: c.most_common(1)[0][0] for f, c in votes.items()}


def name_of(tgt):
    # targets are function entries; allow the push to sit a few insns in
    for d in range(0, 16, 4):
        if tgt + d in names:
            return names[tgt + d]
    f = func_of(tgt + 12)
    return names.get(f, '?')


# group stores into runs: same base register, within one function
runs = {}
for a, base, off, tgt in stores:
    runs.setdefault((func_of(a), base), []).append((off, tgt, a))

for (f, base), items in sorted(runs.items(), key=lambda kv: kv[0][0] or 0):
    if len(items) < 8:
        continue
    items.sort()
    owner = names.get(f, '?')
    print(f'== run in function {f:#x} ({owner}) base {base}, {len(items)} slots')
    for off, tgt, a in items:
        print(f'  {off:#05x} slot {off // 4:3d}  {tgt:#x}  {name_of(tgt)}')

if '-v' in sys.argv:
    print(f'insns={len(insns)} stores={len(stores)} strrefs={len(strrefs)} funcs={len(func_starts)} names={len(names)}', file=sys.stderr)

#!/usr/bin/env python3
"""Map modeljs methods to the kilby model keys they use, from libcobalt.so.

Each cobalt::hisense::modeljs_atsc::<Class>::<Method> loads its model key with
an ARM PC-relative pair (ldr rX, [pc, #imm]; add rX, pc, rX) near its start.
Given the library and its dynamic symbol list (llvm-nm -D output), print the
first kilby string each method loads.

usage: modeljs_keys.py LIBCOBALT NM_D_OUTPUT DISASM Class::Method [...]
       (DISASM = llvm-objdump -d --triple=armv7 over the methods' ranges)
"""
import re
import struct
import sys

lib, nm_path, dis_path = sys.argv[1:4]
wanted = sys.argv[4:]
d = open(lib, 'rb').read()
phoff, = struct.unpack_from('<I', d, 0x1c)
phes, phn = struct.unpack_from('<HH', d, 0x2a)
loads = []
for i in range(phn):
    t, o, va, _, fs, _, _, _ = struct.unpack_from('<8I', d, phoff + i * phes)
    if t == 1:
        loads.append((va, o, fs))


def off(va):
    for v, o, n in loads:
        if v <= va < v + n:
            return o + va - v
    return None


def rd(va):
    o = off(va)
    return struct.unpack_from('<I', d, o)[0] if o is not None else None


def cs(va):
    o = off(va)
    if o is None:
        return None
    e = d.find(b'\0', o, o + 200)
    return d[o:e].decode('latin1') if e > o else None


addr = {}
for line in open(nm_path):
    parts = line.split()
    if len(parts) == 3:
        m = re.match(r'_ZN6cobalt7hisense12modeljs_atsc(\d+)(\w+?)(\d+)(\w+?)E', parts[2])
        if m:
            cls = m.group(2)[:int(m.group(1)) - 0] if False else None
        # parse length-prefixed names properly
        s = parts[2]
        p = '_ZN6cobalt7hisense12modeljs_atsc'
        if s.startswith(p):
            rest = s[len(p):]
            names = []
            while rest and rest[0].isdigit():
                n = re.match(r'(\d+)', rest).group(1)
                rest = rest[len(n):]
                names.append(rest[:int(n)])
                rest = rest[int(n):]
            if len(names) >= 2:
                addr.setdefault('::'.join(names[:2]), int(parts[0], 16))

line_re = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$')
insns = []
for l in open(dis_path, errors='replace'):
    m = line_re.match(l)
    if m:
        insns.append((int(m.group(1), 16), m.group(2), m.group(3).split('@')[0].strip()))
insns.sort()

import bisect
keys = [a for a, _, _ in insns]
for w in wanted:
    a0 = addr.get(w)
    if a0 is None:
        print(f'{w}: no symbol')
        continue
    i = bisect.bisect_left(keys, a0)
    regs = {}
    found = []
    for a, mn, ops in insns[i:i + 60]:
        mm = re.match(r'^(r\d+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]$', ops)
        if mn == 'ldr' and mm:
            regs[mm.group(1)] = rd(a + 8 + int(mm.group(2), 0))
            continue
        mm = re.match(r'^(r\d+), pc, (r\d+)$', ops)
        if mn == 'add' and mm and regs.get(mm.group(2)) is not None:
            s = cs((regs[mm.group(2)] + a + 8) & 0xffffffff)
            if s and s.startswith('kilby'):
                found.append(s)
                break
    print(f'{w}: {found[0] if found else "?"}')

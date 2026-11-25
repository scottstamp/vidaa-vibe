#!/usr/bin/env python3
"""Compare recovered vtables (vtables.py output) with a directfb.h.

For each interface whose *_Construct run was found, walk the header's method
list (AddRef, Release, then the DEFINE_INTERFACE body) with the MStar layout:
priv @0, magic @4, one extra word @8, AddRef @0xc. Print every slot whose
recovered function name differs from the header's method name.

usage: check_abi.py VTABLES_OUT DIRECTFB_H [Interface ...]
"""
import re
import sys

vt_path, hdr_path = sys.argv[1], sys.argv[2]
want = sys.argv[3:]

runs, cur = {}, None
for line in open(vt_path):
    m = re.match(r'== run in function (0x[0-9a-f]+) \((\S+)\)', line)
    if m:
        cur = None
        continue
    m = re.match(r'\s+(0x[0-9a-f]+) slot\s+\d+\s+0x[0-9a-f]+\s+(\S+)', line)
    if m:
        off, name = int(m.group(1), 16), m.group(2)
        iface = name.split('_')[0] if name != '?' else None
        if cur is None and iface:
            cur = iface
            runs.setdefault(cur, {})
        if cur:
            runs[cur][off] = name

hdr = open(hdr_path, errors='replace').read()
ok_all = True
for iface in (want or sorted(runs)):
    m = re.search(r'DEFINE_INTERFACE\(\s*' + iface + r',(.*?)\n\)', hdr, re.S)
    if not m or iface not in runs:
        print(f'{iface}: not found ({"header" if not m else "vtable"})')
        continue
    methods = ['AddRef', 'Release'] + re.findall(r'\(\*\s*([A-Za-z0-9_]+)\s*\)', m.group(1))
    got = runs[iface]
    bad = []
    for i, meth in enumerate(methods):
        off = 0xc + 4 * i
        name = got.get(off)
        if name is None:
            bad.append(f'{off:#x} {meth}: not recovered')
        elif meth.startswith('_mstar_'):
            continue  # vendor slot, any name
        elif name != f'{iface}_{meth}':
            bad.append(f'{off:#x} {meth}: library has {name}')
    extra = sorted(o for o in got if o > 0xc + 4 * (len(methods) - 1))
    print(f'{iface}: {len(methods)} header methods, {len(bad)} mismatches, '
          f'{len(extra)} appended slots')
    for b in bad:
        print('   ', b)
    if extra:
        print('    appended:', ', '.join(got[o].split('_', 1)[-1] for o in extra))
    ok_all &= not bad
sys.exit(0 if ok_all else 1)

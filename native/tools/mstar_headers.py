#!/usr/bin/env python3
"""Patch DirectFB 1.4.2 public headers to the MStar layout on the 58A51HUV.

Measured from the TV's libdirectfb-1.4.so.0.2.0 (vtables.py + check_abi.py,
2026-11-25). Upstream 1.4.2 differs from the vendor build in four places:

  every interface   one extra word at 0x08, between magic and AddRef
  IDirectFBSurface  two slots before Lock, one after Flip
  IDirectFBDisplayLayer  two slots before SetOpacity
  IDirectFBWindow   two slots before SetProperty

Methods the vendor appended at the end of an interface are left out: the
structs are only ever reached through pointers the library hands out, so a
shorter declaration is safe. The inserted slots are declared as
_mstar_<Iface>_<n> and must not be called (their signatures are unknown).

usage: mstar_headers.py INCLUDE_DIR   (patches directfb.h, direct/interface.h)
"""
import os
import re
import sys

inc = sys.argv[1]


def patch(path, fn):
    s = open(path, encoding='latin-1').read()
    if 'MSTAR-LAYOUT' in s:
        return
    s = fn(s)
    open(path, 'w', encoding='latin-1', newline='\n').write(s)


def interface_h(s):
    old = """          void          *priv;                    \\
          int            magic;                   \\
"""
    new = """          void          *priv;                    \\
          int            magic;                   \\
          void          *mstar_reserved;          /* MSTAR-LAYOUT */ \\
"""
    assert old in s, 'DEFINE_INTERFACE layout not found'
    return s.replace(old, new)


INSERTS = [
    # (interface, method the slots go before or after, 'before'|'after', count)
    ('IDirectFBSurface', 'Lock', 'before', 2),
    ('IDirectFBSurface', 'Flip', 'after', 1),
    ('IDirectFBDisplayLayer', 'SetOpacity', 'before', 2),
    ('IDirectFBWindow', 'SetProperty', 'before', 2),
]


def directfb_h(s):
    s = '/* MSTAR-LAYOUT: patched by native/tools/mstar_headers.py */\n' + s
    for iface, meth, where, count in INSERTS:
        m = re.search(r'DEFINE_INTERFACE\(\s*' + iface + r',\n(.*?)\n\)', s, re.S)
        assert m, iface
        body = m.group(1)
        # a method declaration: "     DFBResult (*Name) (\n ... \n     );\n"
        mm = re.search(r'\n(     DFBResult \(\*' + meth + r'\) \(.*?\n     \);\n)', body, re.S)
        assert mm, f'{iface}.{meth}'
        n0 = len(re.findall(r'_mstar_' + iface, body))
        slots = ''.join(f'     void (*_mstar_{iface}_{n0 + i})( void );  /* vendor slot, do not call */\n'
                        for i in range(count))
        if where == 'before':
            body = body[:mm.start(1)] + slots + '\n' + body[mm.start(1):]
        else:
            body = body[:mm.end(1)] + '\n' + slots + body[mm.end(1):]
        s = s[:m.start(1)] + body + s[m.end(1):]
    return s


patch(os.path.join(inc, 'direct', 'interface.h'), interface_h)
patch(os.path.join(inc, 'directfb.h'), directfb_h)
print('patched', inc)

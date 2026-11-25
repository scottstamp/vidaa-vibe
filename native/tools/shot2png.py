#!/usr/bin/env python3
"""Turn a vibe-ui screenshot (/tmp/vibe-shot.raw on the TV) into a PNG.

The raw file is "W H\\n" followed by W*H little-endian ARGB words
(non-premultiplied). The UI layer is mostly transparent over the HDMI video,
so the PNG composites it over a stand-in background: a soft diagonal
gradient, roughly like a mid-tone picture.

usage: shot2png.py SHOT.raw OUT.png [scale]   (scale 0.5 = half size)
No third-party modules: PNG written with zlib.
"""
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
data = open(src, 'rb').read()
nl = data.index(b'\n')
W, H = map(int, data[:nl].split())
px = data[nl + 1:]
step = max(1, int(round(1 / scale)))
ow, oh = W // step, H // step

rows = []
for oy in range(oh):
    y = oy * step
    row = bytearray(b'\0')                      # filter type 0
    base = y * W * 4
    for ox in range(ow):
        x = ox * step
        b, g, r, a = px[base + 4 * x: base + 4 * x + 4]
        # stand-in video: diagonal gradient, teal to dusk
        t = (x / W + y / H) / 2
        br, bg, bb = int(40 + 60 * t), int(70 + 30 * (1 - t)), int(90 + 40 * t)
        row += bytes((
            (r * a + br * (255 - a)) // 255,
            (g * a + bg * (255 - a)) // 255,
            (b * a + bb * (255 - a)) // 255,
        ))
    rows.append(bytes(row))


def chunk(tag, body):
    c = struct.pack('>I', len(body)) + tag + body
    return c + struct.pack('>I', zlib.crc32(tag + body) & 0xffffffff)


png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6))
png += chunk(b'IEND', b'')
open(dst, 'wb').write(png)
print(f'{W}x{H} -> {dst} ({ow}x{oh})')

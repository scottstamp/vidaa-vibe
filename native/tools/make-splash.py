"""Make a boot animation folder for /usr/local/animation/hisense's bind mount.

  python native/tools/make-splash.py test     # black frames, a white bar growing (proves the frames are ours)
  python native/tools/make-splash.py from DIR # DIR/0.png..N.png (740x300 PNG, checked), copied in order

Writes native/splash/animation.cfg and native/splash/part1/0.png..N.png in
the format boot_animation reads (docs/findings/06-native-route.md, "Boot
splashes"): 740x300 PNG frames at 20 fps. Real artwork can replace part1/
(same names, 0..N.png); then set N in animation.cfg ("endIndex").
No third-party modules: PNGs are written by hand (8-bit RGB).
"""
import os
import struct
import sys
import zlib

W, H, N, FPS = 740, 300, 30, 20
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'splash')


def png(path, rows):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    raw = b''.join(b'\x00' + r for r in rows)
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(data)


def test_frames():
    black = b'\x00\x00\x00' * W
    for i in range(N):
        bw = (i + 1) * W // N
        bar = b'\xff\xff\xff' * bw + b'\x00\x00\x00' * (W - bw)
        rows = [bar if 147 <= y < 153 else black for y in range(H)]
        png(os.path.join(OUT, 'part1', '%d.png' % i), rows)


def from_dir(src):
    """copy DIR/<n>.png in numeric order; return the frame count"""
    names = sorted((f for f in os.listdir(src) if f.endswith('.png') and f[:-4].isdigit()),
                   key=lambda f: int(f[:-4]))
    for i, f in enumerate(names):
        data = open(os.path.join(src, f), 'rb').read()
        if data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
            raise SystemExit('%s: not a PNG' % f)
        w, h = struct.unpack('>II', data[16:24])
        if (w, h) != (W, H) or data[28] != 0:
            raise SystemExit('%s: %dx%d (interlace %d), want %dx%d, not interlaced' % (f, w, h, data[28], W, H))
        with open(os.path.join(OUT, 'part1', '%d.png' % i), 'wb') as o:
            o.write(data)
    return len(names)


def main():
    global N
    if not (len(sys.argv) == 2 and sys.argv[1] == 'test') and not (len(sys.argv) == 3 and sys.argv[1] == 'from'):
        print(__doc__)
        return 2
    part = os.path.join(OUT, 'part1')
    os.makedirs(part, exist_ok=True)
    for f in os.listdir(part):                 # the previous set
        if f.endswith('.png'):
            os.remove(os.path.join(part, f))
    if sys.argv[1] == 'test':
        test_frames()
    else:
        N = from_dir(sys.argv[2])
        if N < 1:
            raise SystemExit('no frames')
    # the stock file's layout, frames 0..N-1, looping, 35 s at most
    with open(os.path.join(OUT, 'animation.cfg'), 'w', newline='\n') as f:
        f.write('#fps\n%d\n#type width height isPlay circle startIndex endIndex maxTime partChildPath\n'
                'png %d %d 1 1 0 %d 35000 part1\n' % (FPS, W, H, N - 1))
    print('wrote', os.path.normpath(OUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())

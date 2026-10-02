"""Makes the exe's icon from the game's own lives icon (Bruce's portrait, texture 68 in bits.psx,
32 x 23) - read from the disc at build time, so no game art is kept in the repository.

  python tools/make_icon.py [WAD_DIR] [OUT.ico]      (default: extracted/wad generated/apocalypse.ico)

The portrait is cut to a square around the face and scaled to the usual icon sizes; each size is
stored as a PNG inside the .ico (Windows Vista and later).
"""
import os
import struct
import sys

from psx_textures import decode, png_bytes, textures

TEXTURE = 68           # bits.psx: the lives icon
CROP_X, CROP = 3, 23   # the face: a 23 x 23 square from x = 3
SIZES = (16, 24, 32, 48, 64, 128, 256)


def scale(rgba, w, n):
    """The square `rgba` (w x w) as n x n: each pixel averages 4 x 4 samples (smooth when shrinking,
    blocky like the game's own pixels when growing)."""
    out = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            acc = [0, 0, 0, 0]
            for sy in range(4):
                for sx in range(4):
                    px = min(w - 1, int((x + (sx + 0.5) / 4) * w / n))
                    py = min(w - 1, int((y + (sy + 0.5) / 4) * w / n))
                    i = (py * w + px) * 4
                    a = rgba[i + 3]
                    for k in range(3):
                        acc[k] += rgba[i + k] * a
                    acc[3] += a
            o = (y * n + x) * 4
            if acc[3]:
                out[o:o + 3] = bytes(acc[k] // acc[3] for k in range(3))
            out[o + 3] = acc[3] // 16
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    wad = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'extracted', 'wad')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'generated', 'apocalypse.ico')
    tex, pals = textures(open(os.path.join(wad, 'bits.psx'), 'rb').read())
    entry = next(e for e in tex if e[0][3] == TEXTURE)
    w, h = entry[0][4], entry[0][5]
    rgba = decode(entry, pals, pals)
    if rgba is None or h < CROP or w < CROP_X + CROP:
        sys.exit('bits.psx: texture %d is not the expected portrait' % TEXTURE)
    face = bytearray()
    for y in range(CROP):
        face += rgba[(y * w + CROP_X) * 4:(y * w + CROP_X + CROP) * 4]
    images = [png_bytes(n, n, scale(face, CROP, n)) for n in SIZES]
    # ICONDIR, then one ICONDIRENTRY per image (0 = 256), then the PNGs
    data = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for n, img in zip(SIZES, images):
        data += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(data + b''.join(images))
    print('icon: %s (%s)' % (out, ', '.join(str(n) for n in SIZES)))


if __name__ == '__main__':
    main()

"""Builds PNGs of the game's textures from what a level load sends to video memory.

  python tools/extract_textures.py UPLOAD_DIR OUT_DIR RAM_OR_STATE...

UPLOAD_DIR holds one file per VRAM upload of a run (APOC_UPLOAD_DUMP=dir: "SEQ_FRAME_X_Y_WxH.bin",
the uploaded 16-bit words); then RAM images to look for texture words in: .ram files
(APOC_RAW_DUMP=FRAME:prefix) or save states (.state: RAM at byte 556), e.g. every chapter of
the level, so the polygons of all its sections count (a level uploads all its textures at its
start).
Textures and palettes are uploaded separately; which palette and colour depth a texture is drawn
with is in the level's polygons, which hold PS1 texture words: (clut << 16 | v << 8 | u) followed
by (tpage << 16 | v << 8 | u). Every pair in RAM whose palette was uploaded and whose texel lies in
an uploaded texture pairs them. One PNG per (texture, depth, palette), named by a hash of its
texels and palette; colour 0 is see-through (alpha 0). Files already in OUT_DIR are skipped.
"""
import hashlib
import os
import re
import struct
import sys
import zlib
from collections import Counter


def png(path, w, h, rgba):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def colour(c):
    if c == 0:
        return (0, 0, 0, 0)
    r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
    return (r | r >> 5, g | g >> 5, b | b >> 5, 255)


def main():
    up_dir, out_dir, ram_files = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out_dir, exist_ok=True)
    uploads = []
    for f in sorted(os.listdir(up_dir)):
        m = re.match(r'(\d+)_(\d+)_(\d+)_(\d+)_(\d+)x(\d+)\.bin$', f)
        if not m:
            continue
        _, _, x, y, w, h = map(int, m.groups())
        if (w == 24 and h == 240) or w >= 320 or x < 512:  # movie strips, screens, frame buffers
            continue
        data = struct.unpack('<%dH' % (w * h), open(os.path.join(up_dir, f), 'rb').read())
        uploads.append((x, y, w, h, data))
    # palettes: 16 or 256 colours in one row (later uploads replace earlier ones)
    pal = {}
    for x, y, w, h, data in uploads:
        if h == 1 and w in (16, 256):
            pal[(x, y)] = data
    tex = {}
    for x, y, w, h, data in uploads:
        if h > 1:
            tex[(x, y, w, h)] = data
    pairs = Counter()
    for ram_file in ram_files:
        ram = open(ram_file, 'rb').read()
        if ram_file.endswith('.state'):
            ram = ram[556:556 + 0x200000]
        scan(struct.unpack('<%dI' % (len(ram) // 4), ram), pal, tex, pairs)
    write(tex, pal, pairs, out_dir)


def scan(words, pal, tex, pairs):
    for i in range(len(words) - 3):
        c = words[i] >> 16
        cx, cy = (c & 63) * 16, (c >> 6) & 511
        if (cx, cy) not in pal:
            continue
        for k in (1, 2):
            t = words[i + k] >> 16
            if t >> 9:
                continue
            depth = (t >> 7) & 3
            if depth > 1:
                continue
            px, py = (t & 15) * 64, ((t >> 4) & 1) * 256
            u, v = words[i + k] & 255, (words[i + k] >> 8) & 255
            tx, ty = px + (u // 4 if depth == 0 else u // 2), py + v
            for (x, y, w, h) in tex:
                if x <= tx < x + w and y <= ty < y + h and len(pal[(cx, cy)]) == (16 if depth == 0 else 256):
                    pairs[((x, y, w, h), depth, (cx, cy))] += 1


def write(tex, pal, pairs, out_dir):
    # per texture: its most used colour depth, and the palettes used at least a fifth as often as
    # its most used one (some textures are drawn with several); rarer matches are chance
    best = {}
    for (rect, depth, cl), n in pairs.items():
        if n > best.get(rect, (0, 0))[0]:
            best[rect] = (n, depth)
    written = 0
    for (rect, depth, cl), n in pairs.items():
        top, top_depth = best[rect]
        if n < 2 or depth != top_depth or n * 5 < top:
            continue
        x, y, w, h = rect
        data, p = tex[rect], pal[cl]
        per = 4 if depth == 0 else 2
        tw = w * per
        rgba = bytearray(tw * h * 4)
        for yy in range(h):
            for xx in range(tw):
                word = data[yy * w + xx // per]
                idx = (word >> (4 * (xx & 3))) & 15 if depth == 0 else (word >> (8 * (xx & 1))) & 255
                rgba[(yy * tw + xx) * 4:(yy * tw + xx) * 4 + 4] = bytes(colour(p[idx]))
        name = hashlib.sha1(struct.pack('<%dH' % len(data), *data) + struct.pack('<%dH' % len(p), *p) +
                            bytes([depth])).hexdigest()[:16]
        path = os.path.join(out_dir, '%s.png' % name)
        if not os.path.exists(path):
            png(path, tw, h, rgba)
            written += 1
    paired = len(best)
    print('%s: %d textures, %d palettes, %d paired, %d new PNGs' % (out_dir, len(tex), len(pal), paired, written))


if __name__ == '__main__':
    main()

"""Exports every texture in the game's data files (CD.WAD .psx files) as PNG, read straight from
the files (python tools/discfs.py extract first).

  python tools/psx_textures.py [WAD_DIR] [OUT_DIR]      (default: extracted/wad assets/extracted/textures)

A .psx file (a level's library, its objects, a model like bruce.psx; level sections have none)
holds a texture section:
  u32 n16;  per palette: u32 id, u16 colours[16]           (PS1 15-bit colours, 0 = see-through)
  u32 n256; per palette: u32 id, u16 colours[256]
  u32 textures; u32 offset[textures]                       (from the start of the file)
  at each offset: u32 flag, u32 colours (16 or 256), u32 palette id, u32 index,
                  u16 width, u16 height (texels), then the texels (8 or 4 bits each; each
                  row padded to whole 16-bit words, the whole texture to 4 bytes)
The section is found by its shape: a texture count followed by offsets that all point at valid
texture headers, right after the two palette tables, which end exactly there (of the readings
that fit, the one that holds most of the textures' palettes). Palettes are also looked up in the
other files (some objects use their level library's).
Output: OUT_DIR/<file>/<index>.png (index = the texture's number in its file).

Full-screen pictures (loading screens, logos) go to OUT_DIR/screens/<file>.png:
  .bmr  512 x 240 15-bit pixels (+ 8 bytes of padding)
  .rle  "_RLE_16_", u32 unpacked size, then u16 commands: bit 15 set = repeat the next u16
        (count & 0x7FFF) times, else copy the next `count` u16; unpacks to 512-wide rows (+ 4 words)
"""
import os
import struct
import sys
import zlib


def png_bytes(w, h, rgba):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def png(path, w, h, rgba):
    with open(path, 'wb') as f:
        f.write(png_bytes(w, h, rgba))


def rgba_of(c):
    # 0 is see-through on the PS1; pure magenta (7C1F) is the game's own see-through colour (its
    # palettes hold it where VRAM gets 0000)
    if c == 0 or (c & 0x7FFF) == 0x7C1F:
        return (0, 0, 0, 0)
    r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
    return (r | r >> 5, g | g >> 5, b | b >> 5, 255)


def stride(ncol, w):  # bytes per row: rows are whole 16-bit words
    return (w + 1) // 2 * 2 if ncol == 256 else (w + 3) // 4 * 2


def header(b, o):
    if o < 0 or o + 20 > len(b):
        return None
    flag, ncol, pid, idx, w, h = struct.unpack_from('<IIIIHH', b, o)
    if flag > 1 or ncol not in (16, 256) or not (0 < w <= 1024 and 0 < h <= 512):
        return None
    size = stride(ncol, w) * h
    if o + 20 + size > len(b):
        return None
    return flag, ncol, pid, idx, w, h, size


def find_table(b):
    """Offset of the texture table, or None."""
    for o in range(0, len(b) - 8, 4):
        n = struct.unpack_from('<I', b, o)[0]
        if not 0 < n <= 512 or o + 4 + 4 * n > len(b):
            continue
        offs = struct.unpack_from('<%dI' % n, b, o + 4)
        if offs[0] == o + 4 + 4 * n and all(header(b, x) is not None for x in offs):
            return o
    return None


def palettes(b, o, wanted):
    """The palette tables ending at o: {id: colours}, the reading that holds most of `wanted`."""
    best = {}
    for n256 in range(0, 512):
        p256 = o - 4 - n256 * 516
        if p256 < 4:
            break
        if struct.unpack_from('<I', b, p256)[0] != n256:
            continue
        for n16 in range(0, 1024):
            p16 = p256 - 4 - n16 * 36
            if p16 < 0:
                break
            if struct.unpack_from('<I', b, p16)[0] != n16:
                continue
            pals = {}
            for i in range(n16):
                q = p16 + 4 + i * 36
                pals[struct.unpack_from('<I', b, q)[0]] = struct.unpack_from('<16H', b, q + 4)
            for i in range(n256):
                q = p256 + 4 + i * 516
                pals[struct.unpack_from('<I', b, q)[0]] = struct.unpack_from('<256H', b, q + 4)
            if len(wanted & pals.keys()) > len(wanted & best.keys()):
                best = pals
    return best


def textures(b):
    """[(header, data)] and the file's palettes."""
    o = find_table(b)
    if o is None:
        return [], {}
    n = struct.unpack_from('<I', b, o)[0]
    tex = []
    for x in struct.unpack_from('<%dI' % n, b, o + 4):
        h = header(b, x)
        tex.append((h, b[x + 20:x + 20 + h[6]]))
    return tex, palettes(b, o, set(h[2] for h, _ in tex))


def decode(entry, pals, all_pals):
    """One texture as RGBA bytes (None without its palette)."""
    (flag, ncol, pid, idx, w, h, size), data = entry
    pal = pals.get(pid) or all_pals.get(pid)
    if pal is None or len(pal) != ncol:
        return None
    rgba = bytearray(w * h * 4)
    row = stride(ncol, w)
    for y in range(h):
        for x in range(w):
            v = data[y * row + x] if ncol == 256 else (data[y * row + (x >> 1)] >> (4 * (x & 1))) & 15
            i = y * w + x
            rgba[i * 4:i * 4 + 4] = bytes(rgba_of(pal[v]))
    return rgba


def export(tex, pals, all_pals, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    written = 0
    for entry in tex:
        rgba = decode(entry, pals, all_pals)
        if rgba is None:
            continue
        idx, w, h = entry[0][3], entry[0][4], entry[0][5]
        png(os.path.join(out_dir, '%03d.png' % idx), w, h, rgba)
        written += 1
    return written


def screen(words, path):
    h = len(words) // 512
    rgba = bytearray(512 * h * 4)
    for i in range(512 * h):
        c = words[i]
        r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
        rgba[i * 4:i * 4 + 4] = bytes((r | r >> 5, g | g >> 5, b | b >> 5, 255))
    png(path, 512, h, rgba)


def unrle(b):
    size, o, out = struct.unpack_from('<I', b, 8)[0], 12, []
    while o + 2 <= len(b) and len(out) * 2 < size:
        w = struct.unpack_from('<H', b, o)[0]
        o += 2
        if w & 0x8000:
            out += [struct.unpack_from('<H', b, o)[0]] * (w & 0x7FFF)
            o += 2
        else:
            out += struct.unpack_from('<%dH' % w, b, o)
            o += 2 * w
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    wad = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'extracted', 'wad')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'assets', 'extracted', 'textures')
    files = {}
    all_pals = {}
    for f in sorted(os.listdir(wad)):
        if f.lower().endswith('.psx'):
            files[f] = textures(open(os.path.join(wad, f), 'rb').read())
            all_pals.update(files[f][1])
    total = 0
    for f, (tex, pals) in files.items():
        if not tex:
            continue
        w = export(tex, pals, all_pals, os.path.join(out, os.path.splitext(f)[0]))
        total += w
        print('%-16s %3d textures, %3d written' % (f, len(tex), w))
    os.makedirs(os.path.join(out, 'screens'), exist_ok=True)
    for f in sorted(os.listdir(wad)):
        name, ext = os.path.splitext(f)
        b = open(os.path.join(wad, f), 'rb').read()
        if ext.lower() == '.bmr':
            screen(struct.unpack_from('<%dH' % (512 * 240), b), os.path.join(out, 'screens', name + '.png'))
        elif ext.lower() == '.rle' and b[:8] == b'_RLE_16_':
            screen(unrle(b), os.path.join(out, 'screens', name + '.png'))
        else:
            continue
        total += 1
    print('%d PNGs in %s' % (total, out))


if __name__ == '__main__':
    main()

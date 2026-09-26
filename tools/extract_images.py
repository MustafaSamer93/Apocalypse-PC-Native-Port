"""Extracts the game's full-screen pictures (loading screens, title, logos, legal and warning
screens) from extracted/wad/ to PNG files. Textures (inside the .psx level files) are not included.

  python tools/extract_images.py [OUT_DIR]      (default: assets/extracted/images)

Formats:
  .bmr  raw 15-bit PS1 pixels (bits 0-4 red, 5-9 green, 10-14 blue), 512 wide, then 8 zero bytes
  .rle  "_RLE_16_", u32 decoded size, then 16-bit control words: bit 15 set = the next word
        repeated (control & 0x7FFF) times, clear = that many words follow as they are. The
        decoded data is a .bmr.
Every picture is saved at its full size in the file (512 x 240; warning.rle is 512 x 205).
"""
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WAD = ROOT / "extracted" / "wad"
ROW = 512  # pixels per row in the files (the width of the PS1 frame buffer area they fill)


def rle_decode(data: bytes) -> bytes:
    if data[:8] != b"_RLE_16_":
        raise ValueError("not an _RLE_16_ file")
    size = struct.unpack_from("<I", data, 8)[0]
    out = bytearray()
    p = 12
    while len(out) < size and p + 2 <= len(data):
        c = struct.unpack_from("<H", data, p)[0]
        p += 2
        n = c & 0x7FFF
        if c & 0x8000:
            out += data[p:p + 2] * n
            p += 2
        else:
            out += data[p:p + 2 * n]
            p += 2 * n
    if len(out) != size:
        raise ValueError(f"decoded {len(out)} bytes, expected {size}")
    return bytes(out)


def to_rgb(pixels: bytes) -> tuple[list[bytes], int, int]:
    count = len(pixels) // 2
    height = count // ROW
    words = struct.unpack_from(f"<{height * ROW}H", pixels)
    width = ROW
    out = []
    for y in range(height):
        b = bytearray()
        for w in words[y * ROW:(y + 1) * ROW]:
            r, g, bl = w & 31, (w >> 5) & 31, (w >> 10) & 31
            b += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (bl << 3) | (bl >> 2)))
        out.append(bytes(b))
    return out, width, height


def write_png(path: Path, rows: list[bytes], width: int, height: int):
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    raw = b"".join(b"\x00" + r for r in rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "assets" / "extracted" / "images"
    out_dir.mkdir(parents=True, exist_ok=True)
    files = sorted(WAD.glob("*.bmr")) + sorted(WAD.glob("*.rle"))
    if not files:
        sys.exit(f"no .bmr / .rle files in {WAD} (run tools/discfs.py extract first)")
    for f in files:
        data = f.read_bytes()
        pixels = rle_decode(data) if f.suffix == ".rle" else data
        rows, w, h = to_rgb(pixels)
        dest = out_dir / f"{f.stem}.png"
        write_png(dest, rows, w, h)
        print(f"{f.name:14s} -> {dest.name} ({w}x{h})")
    print(f"{len(files)} pictures in {out_dir}")


if __name__ == "__main__":
    main()

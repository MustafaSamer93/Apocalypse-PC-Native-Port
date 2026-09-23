"""Disc access for the Apocalypse (USA) image: ISO9660 over raw MODE2/2352 sectors,
plus the game's CD.HED/CD.WAD archive.

Usage:
    python tools/discfs.py extract [--cue rom/Apocalypse (USA).cue] [--out extracted]
"""
from __future__ import annotations

import argparse
import os
import struct
from dataclasses import dataclass
from pathlib import Path

RAW_SECTOR = 2352
USER_OFFSET = 24  # 12 sync + 4 header + 8 subheader (MODE2 form 1)
USER_SIZE = 2048


@dataclass
class IsoEntry:
    path: str
    lba: int
    size: int
    is_dir: bool


class Disc:
    def __init__(self, bin_path: str | os.PathLike):
        self.path = Path(bin_path)
        self.f = open(self.path, "rb")

    def close(self):
        self.f.close()

    def read_sector(self, lba: int) -> bytes:
        self.f.seek(lba * RAW_SECTOR + USER_OFFSET)
        return self.f.read(USER_SIZE)

    def read_file(self, lba: int, size: int) -> bytes:
        out = bytearray()
        while len(out) < size:
            out += self.read_sector(lba)
            lba += 1
        return bytes(out[:size])

    def list_files(self) -> list[IsoEntry]:
        pvd = self.read_sector(16)
        if pvd[1:6] != b"CD001":
            raise ValueError("no ISO9660 primary volume descriptor at LBA 16")
        root_lba, root_size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        entries: list[IsoEntry] = []
        self._walk(root_lba, root_size, "", entries)
        return entries

    def _walk(self, lba: int, size: int, prefix: str, out: list[IsoEntry]):
        data = self.read_file(lba, size)
        pos = 0
        while pos < len(data):
            rec_len = data[pos]
            if rec_len == 0:
                # records never straddle sectors; skip to next sector
                pos = (pos // USER_SIZE + 1) * USER_SIZE
                continue
            ext_lba = struct.unpack_from("<I", data, pos + 2)[0]
            ext_size = struct.unpack_from("<I", data, pos + 10)[0]
            flags = data[pos + 25]
            name_len = data[pos + 32]
            raw_name = data[pos + 33 : pos + 33 + name_len]
            pos += rec_len
            if raw_name in (b"\x00", b"\x01"):
                continue
            name = raw_name.decode("ascii").split(";")[0]
            path = f"{prefix}/{name}" if prefix else name
            is_dir = bool(flags & 2)
            out.append(IsoEntry(path, ext_lba, ext_size, is_dir))
            if is_dir:
                self._walk(ext_lba, ext_size, path, out)

    def find(self, path: str) -> IsoEntry:
        want = path.upper().lstrip("/\\").replace("\\", "/")
        for e in self.list_files():
            if e.path.upper() == want:
                return e
        raise FileNotFoundError(path)


@dataclass
class WadEntry:
    name: str
    offset: int
    size: int


def parse_hed(hed: bytes) -> list[WadEntry]:
    """CD.HED: repeated {name\\0 padded to 4 bytes, u32 offset, u32 size}, terminated by 0xFF."""
    entries = []
    pos = 0
    while pos < len(hed) and hed[pos] != 0xFF:
        end = hed.index(b"\x00", pos)
        name = hed[pos:end].decode("ascii")
        pos = (end + 1 + 3) & ~3
        offset, size = struct.unpack_from("<II", hed, pos)
        pos += 8
        entries.append(WadEntry(name, offset, size))
    return entries


def bin_from_cue(cue: Path) -> Path:
    """Return the first data track's .bin referenced by a .cue sheet."""
    for line in cue.read_text().splitlines():
        line = line.strip()
        if line.upper().startswith("FILE"):
            name = line[line.index('"') + 1 : line.rindex('"')]
            return cue.parent / name
    raise ValueError(f"no FILE line in {cue}")


def cmd_extract(args):
    cue = Path(args.cue)
    out = Path(args.out)
    disc = Disc(bin_from_cue(cue))
    out.mkdir(parents=True, exist_ok=True)

    files = disc.list_files()
    for e in files:
        print(f"{'DIR ' if e.is_dir else '    '}{e.path:<32} lba={e.lba:<7} size={e.size}")

    # Small, code-relevant ISO files only; the multi-megabyte .STR/.XA streams are read
    # straight from the disc image by the runtime.
    for name in ("SYSTEM.CNF", "SLUS_003.73", "CD.HED"):
        e = disc.find(name)
        (out / name).write_bytes(disc.read_file(e.lba, e.size))

    wad_e = disc.find("CD.WAD")
    wad = disc.read_file(wad_e.lba, wad_e.size)
    hed = parse_hed((out / "CD.HED").read_bytes())
    wad_dir = out / "wad"
    wad_dir.mkdir(exist_ok=True)
    for e in hed:
        if e.offset + e.size > len(wad):
            raise ValueError(f"{e.name} extends past end of CD.WAD")
        (wad_dir / e.name).write_bytes(wad[e.offset : e.offset + e.size])
    print(f"extracted {len(hed)} WAD entries to {wad_dir}")
    disc.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    ex = sub.add_parser("extract")
    ex.add_argument("--cue", default="rom/Apocalypse (USA).cue")
    ex.add_argument("--out", default="extracted")
    ex.set_defaults(func=cmd_extract)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

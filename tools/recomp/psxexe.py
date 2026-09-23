"""PS-X EXE loader."""
from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass
class PsxExe:
    pc0: int
    gp0: int
    text_addr: int
    text: bytes
    bss_addr: int
    bss_size: int
    sp: int

    @classmethod
    def load(cls, path) -> "PsxExe":
        data = open(path, "rb").read()
        if data[:8] != b"PS-X EXE":
            raise ValueError(f"{path}: not a PS-X EXE")
        pc0, gp0, t_addr, t_size = struct.unpack_from("<IIII", data, 0x10)
        b_addr, b_size = struct.unpack_from("<II", data, 0x28)
        sp_base, sp_off = struct.unpack_from("<II", data, 0x30)
        text = data[0x800 : 0x800 + t_size]
        return cls(pc0, gp0, t_addr, text, b_addr, b_size, sp_base + sp_off)

    @property
    def text_end(self) -> int:
        return self.text_addr + len(self.text)

    def contains(self, addr: int) -> bool:
        return self.text_addr <= addr < self.text_end

    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.text, addr - self.text_addr)[0]

"""MIPS R3000A (PS1 CPU) instruction decoder."""
from __future__ import annotations

from dataclasses import dataclass, field

REG_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]
RA = 31
SP = 29

# Instruction kinds, used by analysis and the emitter.
K_ALU = "alu"
K_LOAD = "load"
K_STORE = "store"
K_BRANCH = "branch"      # conditional, PC-relative
K_JUMP = "jump"          # j
K_CALL = "call"          # jal / bltzal / bgezal
K_JR = "jr"
K_JALR = "jalr"
K_SYSCALL = "syscall"
K_BREAK = "break"
K_COP0 = "cop0"
K_GTE = "gte"            # cop2 moves and commands
K_GTE_MEM = "gte_mem"    # lwc2 / swc2
K_RFE = "rfe"
K_INVALID = "invalid"


def sext16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


@dataclass
class Insn:
    addr: int
    word: int
    op: str                 # mnemonic
    kind: str
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0            # sign- or zero-extended per instruction
    target: int | None = None   # branch / jump destination
    reads: tuple = ()
    writes: tuple = ()
    extra: dict = field(default_factory=dict)

    @property
    def has_delay_slot(self) -> bool:
        return self.kind in (K_BRANCH, K_JUMP, K_CALL, K_JR, K_JALR)

    def __str__(self) -> str:
        return f"{self.addr:08X}: {self.word:08X}  {disasm(self)}"


_SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
    0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor",
    0x2A: "slt", 0x2B: "sltu",
}
_PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu",
    0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x32: "lwc2", 0x3A: "swc2",
}
LOADS = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr"}
STORES = {"sb", "sh", "swl", "sw", "swr"}


def decode(addr: int, word: int) -> Insn:
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    funct = word & 63
    imm16 = word & 0xFFFF

    def mk(name, kind, **kw):
        return Insn(addr, word, name, kind, rs=rs, rt=rt, rd=rd, sa=sa, **kw)

    if op == 0x00:
        name = _SPECIAL.get(funct)
        if name is None:
            return mk("invalid", K_INVALID)
        if name in ("sll", "srl", "sra"):
            return mk(name, K_ALU, reads=(rt,), writes=(rd,))
        if name in ("sllv", "srlv", "srav"):
            return mk(name, K_ALU, reads=(rt, rs), writes=(rd,))
        if name == "jr":
            return mk(name, K_JR, reads=(rs,))
        if name == "jalr":
            return mk(name, K_JALR, reads=(rs,), writes=(rd,))
        if name == "syscall":
            return mk(name, K_SYSCALL, imm=(word >> 6) & 0xFFFFF)
        if name == "break":
            return mk(name, K_BREAK, imm=(word >> 6) & 0xFFFFF)
        if name in ("mfhi", "mflo"):
            return mk(name, K_ALU, reads=(), writes=(rd,))
        if name in ("mthi", "mtlo"):
            return mk(name, K_ALU, reads=(rs,))
        if name in ("mult", "multu", "div", "divu"):
            return mk(name, K_ALU, reads=(rs, rt))
        return mk(name, K_ALU, reads=(rs, rt), writes=(rd,))

    if op == 0x01:  # REGIMM: bltz/bgez/bltzal/bgezal (R3000A decodes loosely on rt bits)
        link = (rt & 0x1E) == 0x10
        ge = rt & 1
        name = ("bgez" if ge else "bltz") + ("al" if link else "")
        tgt = (addr + 4 + (sext16(imm16) << 2)) & 0xFFFFFFFF
        if link:
            return mk(name, K_CALL, target=tgt, reads=(rs,), writes=(RA,))
        return mk(name, K_BRANCH, target=tgt, reads=(rs,))

    if op in (0x02, 0x03):
        tgt = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        if op == 0x02:
            return mk("j", K_JUMP, target=tgt)
        return mk("jal", K_CALL, target=tgt, writes=(RA,))

    if op in (0x04, 0x05, 0x06, 0x07):
        name = _PRIMARY[op]
        tgt = (addr + 4 + (sext16(imm16) << 2)) & 0xFFFFFFFF
        reads = (rs, rt) if op in (0x04, 0x05) else (rs,)
        return mk(name, K_BRANCH, target=tgt, reads=reads)

    if op in (0x08, 0x09, 0x0A, 0x0B):
        return mk(_PRIMARY[op], K_ALU, imm=sext16(imm16), reads=(rs,), writes=(rt,))
    if op in (0x0C, 0x0D, 0x0E):
        return mk(_PRIMARY[op], K_ALU, imm=imm16, reads=(rs,), writes=(rt,))
    if op == 0x0F:
        return mk("lui", K_ALU, imm=imm16, writes=(rt,))

    if op == 0x10:  # COP0
        sub = rs
        if sub == 0x00:
            return mk("mfc0", K_COP0, writes=(rt,))
        if sub == 0x04:
            return mk("mtc0", K_COP0, reads=(rt,))
        if sub == 0x10 and funct == 0x10:
            return mk("rfe", K_RFE)
        return mk("invalid", K_INVALID)

    if op == 0x12:  # COP2 / GTE
        sub = rs
        if sub & 0x10:
            return mk("cop2", K_GTE, imm=word & 0x1FFFFFF)
        if sub == 0x00:
            return mk("mfc2", K_GTE, writes=(rt,))
        if sub == 0x02:
            return mk("cfc2", K_GTE, writes=(rt,))
        if sub == 0x04:
            return mk("mtc2", K_GTE, reads=(rt,))
        if sub == 0x06:
            return mk("ctc2", K_GTE, reads=(rt,))
        return mk("invalid", K_INVALID)

    name = _PRIMARY.get(op)
    if name in LOADS:
        # lwl/lwr merge with the old rt value
        reads = (rs, rt) if name in ("lwl", "lwr") else (rs,)
        return mk(name, K_LOAD, imm=sext16(imm16), reads=reads, writes=(rt,))
    if name in STORES:
        return mk(name, K_STORE, imm=sext16(imm16), reads=(rs, rt))
    if name in ("lwc2", "swc2"):
        return mk(name, K_GTE_MEM, imm=sext16(imm16), reads=(rs,))
    return mk("invalid", K_INVALID)


def disasm(i: Insn) -> str:
    r = REG_NAMES
    n = i.op
    if i.word == 0:
        return "nop"
    if n in ("sll", "srl", "sra"):
        return f"{n} {r[i.rd]}, {r[i.rt]}, {i.sa}"
    if n in ("sllv", "srlv", "srav"):
        return f"{n} {r[i.rd]}, {r[i.rt]}, {r[i.rs]}"
    if n == "jr":
        return f"jr {r[i.rs]}"
    if n == "jalr":
        return f"jalr {r[i.rd]}, {r[i.rs]}"
    if n in ("mfhi", "mflo"):
        return f"{n} {r[i.rd]}"
    if n in ("mthi", "mtlo"):
        return f"{n} {r[i.rs]}"
    if n in ("mult", "multu", "div", "divu"):
        return f"{n} {r[i.rs]}, {r[i.rt]}"
    if i.kind == K_ALU and n in _SPECIAL.values():
        return f"{n} {r[i.rd]}, {r[i.rs]}, {r[i.rt]}"
    if n == "lui":
        return f"lui {r[i.rt]}, 0x{i.imm:04X}"
    if n in ("andi", "ori", "xori"):
        return f"{n} {r[i.rt]}, {r[i.rs]}, 0x{i.imm:04X}"
    if i.kind == K_ALU:
        return f"{n} {r[i.rt]}, {r[i.rs]}, {i.imm}"
    if i.kind in (K_LOAD, K_STORE):
        return f"{n} {r[i.rt]}, {i.imm}({r[i.rs]})"
    if i.kind == K_GTE_MEM:
        return f"{n} gte{i.rt}, {i.imm}({r[i.rs]})"
    if n in ("beq", "bne"):
        return f"{n} {r[i.rs]}, {r[i.rt]}, 0x{i.target:08X}"
    if i.kind in (K_BRANCH, K_CALL) and n != "jal":
        return f"{n} {r[i.rs]}, 0x{i.target:08X}"
    if n in ("j", "jal"):
        return f"{n} 0x{i.target:08X}"
    if n in ("mfc0", "mtc0", "mfc2", "mtc2", "cfc2", "ctc2"):
        return f"{n} {r[i.rt]}, ${i.rd}"
    if n == "cop2":
        return f"cop2 0x{i.imm:07X}"
    if n in ("syscall", "break"):
        return f"{n} 0x{i.imm:X}"
    return n

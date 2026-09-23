"""Control-flow analysis: function discovery, per-function block tracing, jump tables.

Every function is traced independently from its entry point. Code reachable from two
entries (shared epilogues, jal into the middle of a function) is simply emitted twice,
which keeps each generated C function self-contained.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from mips import (
    K_BRANCH, K_CALL, K_INVALID, K_JALR, K_JR, K_JUMP, K_LOAD, K_BREAK, RA, Insn, decode,
)


class CodeImage:
    """A contiguous region of MIPS code/data at a fixed virtual address."""

    def __init__(self, base: int, data: bytes, name: str):
        self.base = base
        self.data = data
        self.name = name
        self._cache: dict[int, Insn] = {}

    @property
    def end(self) -> int:
        return self.base + len(self.data)

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.end

    def word(self, addr: int) -> int:
        o = addr - self.base
        return int.from_bytes(self.data[o : o + 4], "little")

    def insn(self, addr: int) -> Insn:
        i = self._cache.get(addr)
        if i is None:
            i = decode(addr, self.word(addr))
            self._cache[addr] = i
        return i


@dataclass
class JumpTable:
    jr_addr: int
    table_addr: int
    targets: list[int]


@dataclass
class Function:
    entry: int
    image: CodeImage
    addrs: set[int] = field(default_factory=set)       # every instruction address traced
    labels: set[int] = field(default_factory=set)      # addresses that need a C label
    calls: set[int] = field(default_factory=set)       # direct jal targets
    tail_calls: set[int] = field(default_factory=set)  # j to another function's entry
    jump_tables: dict[int, JumpTable] = field(default_factory=dict)  # jr addr -> table
    indirect_jr: set[int] = field(default_factory=set) # jr reg that isn't ra or a table
    errors: list[str] = field(default_factory=list)

    @property
    def name(self) -> str:
        return f"f_{self.entry:08X}"


class Analyzer:
    def __init__(self, image: CodeImage, extra_images: list[CodeImage] = ()):
        self.image = image
        self.extra_images = list(extra_images)  # e.g. the main EXE, when analysing an overlay
        self.entries: set[int] = set()
        self.functions: dict[int, Function] = {}
        self.warnings: list[str] = []

    # ---------------------------------------------------------------- tracing

    def trace(self, entry: int) -> Function:
        img = self.image
        fn = Function(entry, img)
        fn.labels.add(entry)
        work = [entry]
        seen_blocks: set[int] = set()
        while work:
            pc = work.pop()
            if pc in seen_blocks:
                continue
            seen_blocks.add(pc)
            while True:
                if not img.contains(pc):
                    fn.errors.append(f"ran off image at {pc:08X}")
                    break
                i = img.insn(pc)
                fn.addrs.add(pc)
                if i.kind == K_INVALID:
                    fn.errors.append(f"invalid instruction {i.word:08X} at {pc:08X}")
                    break
                if i.has_delay_slot:
                    fn.addrs.add(pc + 4)
                    ds = img.insn(pc + 4) if img.contains(pc + 4) else None
                    if ds is None or ds.kind == K_INVALID:
                        fn.errors.append(f"invalid delay slot at {pc + 4:08X}")
                        break
                    if ds.has_delay_slot:
                        fn.errors.append(f"branch in delay slot at {pc + 4:08X}")
                if i.kind == K_BRANCH:
                    # beq zero,zero is an unconditional branch ("b")
                    fn.labels.add(i.target)
                    work.append(i.target)
                    if i.op == "beq" and i.rs == 0 and i.rt == 0:
                        break
                    fn.labels.add(pc + 8)
                    pc += 8
                    continue
                if i.kind == K_CALL:
                    if i.op == "jal":
                        fn.calls.add(i.target)
                    else:  # bltzal/bgezal: conditional call, rare
                        fn.calls.add(i.target)
                    fn.labels.add(pc + 8)
                    pc += 8
                    continue
                if i.kind == K_JALR:
                    fn.labels.add(pc + 8)
                    pc += 8
                    continue
                if i.kind == K_JUMP:
                    if self._is_tail_call(fn, i.target):
                        fn.tail_calls.add(i.target)
                    else:
                        fn.labels.add(i.target)
                        work.append(i.target)
                    break
                if i.kind == K_JR:
                    if i.rs != RA:
                        jt = self._find_jump_table(pc, i.rs)
                        if jt:
                            fn.jump_tables[pc] = jt
                            for t in jt.targets:
                                fn.labels.add(t)
                                work.append(t)
                        else:
                            fn.indirect_jr.add(pc)
                    break
                if i.kind == K_BREAK and i.imm not in (6, 7):
                    # break 6/7 are GCC's div-overflow / div-by-zero traps and fall through
                    pass
                pc += 4
        self.functions[entry] = fn
        return fn

    def _is_tail_call(self, fn: Function, target: int) -> bool:
        if not self.image.contains(target):
            return True
        if target == fn.entry:
            return False  # self-loop via j
        return target in self.entries

    def _find_jump_table(self, jr_addr: int, reg: int) -> JumpTable | None:
        """Match GCC's switch lowering, searching backwards from the jr:

            sltiu  at, idx, N        ; bound check (optional but usual)
            beq    at, zero, default
            sll    t, idx, 2
            lui    at, %hi(table)
            addu   at, at, t
            lw     reg, %lo(table)(at)
            ...
            jr     reg
        """
        img = self.image
        load_addr = None
        lo = base_reg = None
        for back in range(1, 12):
            a = jr_addr - 4 * back
            if not img.contains(a):
                return None
            i = img.insn(a)
            if i.op == "lw" and i.rt == reg:
                load_addr, lo, base_reg = a, i.imm, i.rs
                break
            if reg in i.writes:
                return None
        if load_addr is None:
            return None
        # base_reg = addu(idx, base) where `base` holds %hi(table) [+ %lo(table)].
        # First find the addu, then resolve its operands to a constant. The base is often
        # hoisted into a callee-saved register at the top of the function, so the
        # constant search looks a long way back.
        table_base = None
        for back in range(1, 12):
            a = load_addr - 4 * back
            if not img.contains(a):
                break
            i = img.insn(a)
            if i.op == "addu" and i.rd == base_reg:
                for r in (i.rs, i.rt):
                    v = self._resolve_const(a, r, 256)
                    if v is not None and self._image_for(v) is not None:
                        table_base = v
                        break
                break
            if base_reg in i.writes:
                break
        if table_base is None:
            return None
        table = (table_base + lo) & 0xFFFFFFFF
        bound = self._find_bound(load_addr)
        targets = []
        n = 0
        while True:
            ta = table + 4 * n
            img_t = self._image_for(ta)
            if img_t is None:
                break
            t = img_t.word(ta)
            if bound is not None:
                if n >= bound:
                    break
            elif not (img.contains(t) and t & 3 == 0) or n >= 512:
                break
            if not img.contains(t):
                self.warnings.append(f"jump table {table:08X}[{n}] -> {t:08X} outside image")
                break
            targets.append(t)
            n += 1
        if not targets:
            return None
        return JumpTable(jr_addr, table, targets)

    def _resolve_const(self, before: int, reg: int, limit: int) -> int | None:
        """Value of `reg` just before `before`, if a linear backward scan finds it built
        from lui/addiu/ori (ignores control flow; good enough for hoisted table bases)."""
        img = self.image
        add = 0
        for back in range(1, limit):
            a = before - 4 * back
            if not img.contains(a):
                return None
            i = img.insn(a)
            if reg not in i.writes:
                continue
            if i.op == "lui":
                return ((i.imm << 16) + add) & 0xFFFFFFFF
            if i.op == "addiu":
                if i.rs == reg:
                    add += i.imm
                    continue
                base = self._resolve_const(a, i.rs, limit - back)
                return None if base is None else (base + i.imm + add) & 0xFFFFFFFF
            if i.op == "ori" and i.rs == reg:
                # only valid when the low half was zero (lui+ori idiom)
                base = self._resolve_const(a, reg, limit - back)
                return None if base is None else ((base | i.imm) + add) & 0xFFFFFFFF
            return None
        return None

    def _find_bound(self, load_addr: int) -> int | None:
        img = self.image
        for back in range(1, 16):
            a = load_addr - 4 * back
            if not img.contains(a):
                return None
            i = img.insn(a)
            if i.op == "sltiu":
                return i.imm & 0xFFFF
        return None

    def _image_for(self, addr: int) -> CodeImage | None:
        if self.image.contains(addr):
            return self.image
        for im in self.extra_images:
            if im.contains(addr):
                return im
        return None

    # ---------------------------------------------------------------- discovery

    def discover(self, seeds: set[int]) -> None:
        """Trace from seeds, adding every direct call / tail-call target, to a fixpoint.

        Tail-call detection depends on the set of known entries, so once the set stops
        growing we retrace everything once more with the final set.
        """
        self.entries |= {s for s in seeds if self.image.contains(s)}
        while True:
            before = len(self.entries)
            todo = [e for e in sorted(self.entries) if e not in self.functions]
            for e in todo:
                fn = self.trace(e)
                for t in fn.calls | fn.tail_calls:
                    if self.image.contains(t):
                        self.entries.add(t)
            if len(self.entries) == before and not todo:
                break
        self.functions.clear()
        for e in sorted(self.entries):
            self.trace(e)

    def pointer_seeds(self) -> set[int]:
        """Code addresses materialised by lui/addiu pairs or stored as words in data:
        callbacks, vtables and function pointer tables that only jalr reaches."""
        img = self.image
        cands: set[int] = set()
        # 1) aligned words anywhere in the image that point at plausible code
        for a in range(img.base, img.end, 4):
            w = img.word(a)
            if img.contains(w) and w & 3 == 0:
                cands.add(w)
        # 2) lui rX, hi ; addiu rY, rX, lo
        for a in range(img.base, img.end - 4, 4):
            i = img.insn(a)
            if i.op != "lui":
                continue
            for k in range(1, 6):
                b = a + 4 * k
                if not img.contains(b):
                    break
                j = img.insn(b)
                if j.op == "addiu" and j.rs == i.rt:
                    cands.add(((i.imm << 16) + j.imm) & 0xFFFFFFFF)
                    break
                if i.rt in j.writes:
                    break
        good = set()
        for c in cands:
            if c in self.entries:
                good.add(c)
                continue
            if self._looks_like_function_start(c):
                ok = not self.trace(c).errors
                del self.functions[c]  # trial trace only; discover() retraces
                if ok:
                    good.add(c)
        return good

    def gap_seeds(self) -> set[int]:
        """First instruction of each untraced, code-looking run between known functions.

        Catches functions reachable only through tables the pointer scan missed. A
        spurious extra function is harmless: it is only ever run via dispatch.
        """
        img = self.image
        covered: set[int] = set()
        for f in self.functions.values():
            covered |= f.addrs
        seeds = set()
        a = img.base
        while a < img.end:
            if a in covered or img.word(a) == 0:
                a += 4
                continue
            start = a
            while a < img.end and a not in covered:
                a += 4
            if img.insn(start).kind == K_INVALID or start in self.entries:
                continue
            fn = self.trace(start)
            del self.functions[start]
            if not fn.errors and max(fn.addrs) < a + 4:
                seeds.add(start)
        return seeds

    def _looks_like_function_start(self, addr: int) -> bool:
        img = self.image
        if not img.contains(addr):
            return False
        if addr in self.entries:
            return True
        # previous instruction should end a function (jr ra + delay, or j + delay),
        # and the entry itself should decode and not be padding
        first = img.insn(addr)
        if first.kind == K_INVALID or first.word == 0:
            return False
        if img.contains(addr - 8):
            prev = img.insn(addr - 8)
            if not ((prev.kind == K_JR and prev.rs == RA) or prev.kind == K_JUMP):
                # also allow nop padding after a function
                if img.insn(addr - 4).word != 0:
                    return False
        # trace a short distance: must hit a return/jump before an invalid insn
        pc = addr
        for _ in range(4096):
            if not img.contains(pc):
                return False
            i = img.insn(pc)
            if i.kind == K_INVALID:
                return False
            if (i.kind == K_JR and i.rs == RA) or i.kind == K_JUMP:
                return True
            pc += 4
        return False


def load_delay_hazards(fn: Function) -> list[int]:
    """Addresses of loads whose next instruction reads the loaded register.

    On the R3000A the old register value is still visible in the load delay slot.
    """
    out = []
    for a in sorted(fn.addrs):
        i = fn.image.insn(a)
        if i.kind != K_LOAD or i.rt == 0:
            continue
        if a + 4 not in fn.addrs:
            continue
        n = fn.image.insn(a + 4)
        if i.op in ("lwl", "lwr") and n.op in ("lwl", "lwr") and n.rt == i.rt:
            continue  # unaligned pair: the hardware forwards between lwl/lwr
        if i.rt in n.reads:
            out.append(a)
    return out

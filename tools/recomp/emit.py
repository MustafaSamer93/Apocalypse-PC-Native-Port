"""C code emitter: one C function per traced MIPS function.

Generated code targets the runtime ABI in runtime/include/recomp.h.
"""
from __future__ import annotations

from analysis import Function
from mips import (
    K_ALU, K_BRANCH, K_BREAK, K_CALL, K_COP0, K_GTE, K_GTE_MEM, K_JALR, K_JR, K_JUMP,
    K_LOAD, K_RFE, K_STORE, K_SYSCALL, RA, Insn, disasm,
)


def R(n: int) -> str:
    return "0u" if n == 0 else f"c->r[{n}]"


def S(n: int) -> str:
    return "0" if n == 0 else f"(int32_t)c->r[{n}]"


def lbl(a: int) -> str:
    return f"L_{a:08X}"


# Runtime hooks emitted at the entry of specific functions.
#   8008632C VSync: save-state safe point (see runtime/src/savestate.cpp)
#   80030AD4 the level's main loop: a level has started (auto-save)
#   800307F0 the level's render: extra frames between simulation steps (smooth.cpp)
#   8002FF90 the level's tick: 60 Hz simulation prototype (sim60.cpp)
#   800189EC the game records progress (level, restart point, health, weapons): auto-save
#   8005F688 the player's update: every jump rises the full height (sim60.cpp)
#   80063CC4 / 80063CF0 around the player's update in a tick: what the player itself changed
#            (sim60.cpp: retimed) and what other objects did to it (kept)
#   8003A288 / 8003A5CC / 8003A76C a force zone (currents, the chute) pushes the objects of a
#            list: 60 Hz for the player (sim60.cpp)
ENTRY_HOOKS = {
    0x8002FF90: "if (psx_hook_tick(c)) return;",
    0x800189EC: "psx_hook_progress(c);",
    0x8005F688: "psx_hook_player(c);",
    0x80063CC4: "psx_hook_contacts(c, 0);",  # just before the player's update in a tick (sim60.cpp)
    0x80063CF0: "psx_hook_contacts(c, 1);",  # just after it
    # the damage methods (vtable +0x34) of every class but the player's: one hit kill cheat
    **{a: "psx_hook_enemy_damage(c);" for a in (
        0x8003C490, 0x8003E354, 0x800422C0, 0x80045B14, 0x80048428, 0x8004A25C, 0x8004F5C8, 0x80054714,
        0x80056C50, 0x80058A14, 0x800593CC, 0x8005A7B0, 0x8005B864, 0x800736AC, 0x800747A8, 0x80075930)},
    # VSync's wait (target count, timeout): the timeout is a loop count, which this faster CPU
    # used up before the VBlank came -> "VSync: timeout", a GPU reset: movies started at 7.5 fps
    # and showed a frame of green stripes. 16x the loops (still ends if VBlanks stop).
    0x800864A4: "c->r[5] <<= 4;",
    0x80062044: "if (psx_hook_player_use(c)) return;",  # a button fires (sim60.cpp: once at 60 Hz)
    0x80061D8C: "if (psx_hook_player_use(c)) return;",  # a wheel fires
    0x8003A288: "psx_hook_zone(c);",  # a force zone pushes a list's objects (sim60.cpp)
    0x8003A5CC: "psx_hook_zone(c);",
    0x8003A76C: "psx_hook_zone(c);",
    0x8008632C: "psx_safepoint(c, 0x8008632Cu);",
    0x80030AD4: "psx_hook_level_start();",
    0x800307F0: "if (psx_hook_render(c)) return;",
    0x8007E63C: "if (psx_hook_frustum(c)) return;",  # culling planes: widescreen (drawdist.cpp)
    0x800817FC: "if (psx_hook_mesh(c)) return;",  # the level mesh renderer: native (mesh.cpp)
    # the render's 2D part (widescreen: HUD at its own proportions, gpu.cpp)
    0x8006D91C: "psx_hook_2d(c, 0);",  # HUD
    0x80016884: "psx_hook_2d(c, 1);",
    0x8001B8BC: "psx_hook_2d(c, 2);",
    0x8001C004: "psx_hook_2d(c, 3);",  # full-screen tint
    0x8006D3C0: "psx_hook_2d(c, 4);",  # boss health bar (part of the HUD, centred)
    # menu / HUD drawing routines: their packets are 2D (gpu.cpp)
    0x8001AA28: "if (psx_hook_2d_call(c, 0)) return;",  # text
    0x8006D028: "if (psx_hook_2d_call(c, 1)) return;",  # icons (button symbols, ...)
    0x8006D0D4: "if (psx_hook_2d_call(c, 2)) return;",
    # the game's menus: size, and Options -> Enhancements (game_menu.cpp)
    0x80015228: "psx_hook_menu_init(c);",
    0x80015474: "psx_hook_menu_add(c);",
    0x800152C4: "psx_hook_menu_free(c);",
    0x80015BA4: "if (psx_hook_menu_update(c)) return;",
    0x800156E0: "if (psx_hook_menu_draw(c)) return;",
    0x80017364: "if (psx_hook_menu_keys(c)) return;",
    0x800119C4: "if (psx_hook_menu_camera(c)) return;",  # once per front end loop (menus at 60 Hz)
    # pickup voice lines: more of them, and one for Extra Life (game_menu.cpp)
    0x8002FC64: "if (psx_hook_voice(c)) return;",
    0x8005E260: "psx_hook_weapon_pickup(c);",  # cheats.cpp
    0x8001B788: "psx_hook_message(c);",
}

# Runtime hooks emitted just before specific instructions (not delay slots).
#   800376F8 the glow renderer (8003736C) turned the glow's depth into its ordering-table slot
#            (v0): sort glows nearer (sim60.cpp psx_hook_glow_sort)
INSN_HOOKS = {
    0x800376F8: "psx_hook_glow_sort(c);",
    0x80080D38: "psx_hook_strip_corner(c);",  # a strip corner (lamps): exact position (gte.cpp)
    0x800312A0: "psx_hook_game_over(c);",  # lives left (v1) after a death: 0 restarts the level (savestate.cpp)
    0x800776B4: "psx_hook_camera(c);",  # the render camera: smoothed view (gte.cpp)
    0x8005CDA0: "psx_hook_aim(c);",  # player fire vector +471/+472 set: exact stick/mouse angle (host_sdl.cpp)
    # 800664FC: the VBlank wait's loop (800664E4: until the VBlank callback has counted a0 more):
    # straight to the next event instead of running the loop through the idle cycles (with the
    # 8x CPU of 60 Hz mode that was ~10% of the host's time)
    0x800664FC: "psx_idle_until_event(c);",
}

# jal sites whose call can be switched off at runtime: address -> runtime flag name.
# 80030BCC: the main loop's second VBlank wait (the game's 30 fps cap).
# 80030B80 / 80030BBC: the loop's second tick and its extra VBlank wait (sim60.cpp:
#   one tick and one render per VBlank).
OPTIONAL_CALLS = {
    0x80030BCC: "psx_opt_skip_frame_wait",
    0x80030B80: "psx_opt_sim60",
    0x80030BBC: "psx_opt_sim60",
    0x800145BC: "psx_opt_menu60",  # the front end's (menus) second VBlank wait
    # front end at 60 Hz, every other loop: no pad read, no controller / memory card check
    0x80012F10: "psx_opt_menu_odd",
    0x80012F18: "psx_opt_menu_odd",
}


class FunctionEmitter:
    def __init__(self, fn: Function, known_entries: set[int], debug_ra: bool = True):
        self.fn = fn
        self.img = fn.image
        self.known = known_entries
        self.debug_ra = debug_ra
        self.lines: list[str] = []
        self.used_labels: set[int] = set()
        self.label_positions: dict[int, int] = {}
        self.pending_cycles = 0

    # ------------------------------------------------------------ helpers

    def out(self, s: str):
        self.lines.append("    " + s)

    def goto(self, target: int) -> str:
        self.used_labels.add(target)
        return f"goto {lbl(target)};"

    def flush_cycles(self):
        if self.pending_cycles:
            self.out(f"ADD_CYCLES({self.pending_cycles});")
            self.pending_cycles = 0

    def call(self, target: int) -> str:
        if target in self.known:
            return f"f_{target:08X}(c);"
        return f"call_indirect(c, 0x{target:08X}u);"

    # ------------------------------------------------------------ simple instructions

    def simple(self, i: Insn) -> list[str]:
        """C statements for a non-control-flow instruction."""
        n, rs, rt, rd, sa, imm = i.op, i.rs, i.rt, i.rd, i.sa, i.imm
        k = i.kind

        def set_(reg: int, expr: str) -> list[str]:
            return [] if reg == 0 else [f"c->r[{reg}] = {expr};"]

        if i.word == 0:
            return []
        # register moves carry the precise-vertex shadow along (PGXP_* in recomp.h)
        if k == K_ALU and rd != 0 and n in ("addu", "or") and (rs == 0 or rt == 0) and rs != rt:
            return [f"c->r[{rd}] = {R(rs or rt)}; PGXP_MOVE({rd}, {rs or rt});"]
        if k == K_ALU and rt != 0 and n in ("addiu", "addi", "ori") and imm == 0 and rs != 0:
            return [f"c->r[{rt}] = {R(rs)}; PGXP_MOVE({rt}, {rs});"]
        if k == K_ALU:
            if n == "sll":   return set_(rd, f"{R(rt)} << {sa}")
            if n == "srl":   return set_(rd, f"{R(rt)} >> {sa}")
            if n == "sra":   return set_(rd, f"(uint32_t)({S(rt)} >> {sa})")
            if n == "sllv":  return set_(rd, f"{R(rt)} << ({R(rs)} & 31)")
            if n == "srlv":  return set_(rd, f"{R(rt)} >> ({R(rs)} & 31)")
            if n == "srav":  return set_(rd, f"(uint32_t)({S(rt)} >> ({R(rs)} & 31))")
            if n == "mfhi":  return set_(rd, "c->hi")
            if n == "mflo":  return set_(rd, "c->lo")
            if n == "mthi":  return [f"c->hi = {R(rs)};"]
            if n == "mtlo":  return [f"c->lo = {R(rs)};"]
            if n == "mult":  return [f"MULT(c, {S(rs)}, {S(rt)});"]
            if n == "multu": return [f"MULTU(c, {R(rs)}, {R(rt)});"]
            if n == "div":   return [f"DIV(c, {S(rs)}, {S(rt)});"]
            if n == "divu":  return [f"DIVU(c, {R(rs)}, {R(rt)});"]
            # add/sub/addi overflow traps are not emulated; GCC never relies on them
            if n in ("add", "addu"):  return set_(rd, f"{R(rs)} + {R(rt)}")
            if n in ("sub", "subu"):  return set_(rd, f"{R(rs)} - {R(rt)}")
            if n == "and":   return set_(rd, f"{R(rs)} & {R(rt)}")
            if n == "or":    return set_(rd, f"{R(rs)} | {R(rt)}")
            if n == "xor":   return set_(rd, f"{R(rs)} ^ {R(rt)}")
            if n == "nor":   return set_(rd, f"~({R(rs)} | {R(rt)})")
            if n == "slt":   return set_(rd, f"{S(rs)} < {S(rt)}")
            if n == "sltu":  return set_(rd, f"{R(rs)} < {R(rt)}")
            if n in ("addi", "addiu"): return set_(rt, f"{R(rs)} + (uint32_t){imm}")
            if n == "slti":  return set_(rt, f"{S(rs)} < {imm}")
            if n == "sltiu": return set_(rt, f"{R(rs)} < (uint32_t){imm}")
            if n == "andi":  return set_(rt, f"{R(rs)} & 0x{imm:X}u")
            if n == "ori":   return set_(rt, f"{R(rs)} | 0x{imm:X}u")
            if n == "xori":  return set_(rt, f"{R(rs)} ^ 0x{imm:X}u")
            if n == "lui":   return set_(rt, f"0x{(imm << 16) & 0xFFFFFFFF:08X}u")
        addr = f"{R(rs)} + (uint32_t){imm}" if imm else R(rs)
        if k == K_LOAD:
            if n == "lwl":  return set_(rt, f"MEM_LWL(c, {addr}, {R(rt)})")
            if n == "lwr":  return set_(rt, f"MEM_LWR(c, {addr}, {R(rt)})")
            expr = {
                "lb":  f"(uint32_t)(int8_t)MEM_R8(c, {addr})",
                "lbu": f"MEM_R8(c, {addr})",
                "lh":  f"(uint32_t)(int16_t)MEM_R16(c, {addr})",
                "lhu": f"MEM_R16(c, {addr})",
                "lw":  f"MEM_R32(c, {addr})",
            }[n]
            if rt == 0:
                return [f"(void){expr};"]  # keep side effects of I/O reads
            if n == "lw":  # the address is taken before rt (which may be rs) changes
                return [f"{{ const uint32_t a_ = {addr}; c->r[{rt}] = MEM_R32(c, a_); PGXP_LW({rt}, a_); }}"]
            if n in ("lh", "lhu"):  # a word's low half carries its shadow (exact UVs, gte.cpp)
                return [f"{{ const uint32_t a_ = {addr}; c->r[{rt}] = {expr.replace(addr, 'a_')}; PGXP_LH({rt}, a_); }}"]
            return set_(rt, expr)
        if k == K_STORE:
            fn = {"sb": "MEM_W8", "sh": "MEM_W16", "sw": "MEM_W32",
                  "swl": "MEM_SWL", "swr": "MEM_SWR"}[n]
            if n == "sw":
                return [f"{{ const uint32_t a_ = {addr}; MEM_W32(c, a_, {R(rt)}); PGXP_SW({rt}, a_); }}"]
            return [f"{fn}(c, {addr}, {R(rt)});"]
        if k == K_COP0:
            if n == "mfc0": return set_(rt, f"cop0_read(c, {rd})")
            if n == "mtc0": return [f"cop0_write(c, {rd}, {R(rt)});"]
        if k == K_RFE:
            return ["cop0_rfe(c);"]
        if k == K_GTE:
            sxy = 12 <= rd <= 15
            if n == "mfc2" and (sxy or rd == 22) and rt != 0:  # 22 = RGB2: exact UVs (gte.cpp)
                return [f"c->r[{rt}] = gte_read_data(c, {rd}); pgxp_gte_to_gpr({rt}, {rd});"]
            if n == "mtc2" and sxy:
                return [f"gte_write_data(c, {rd}, {R(rt)}); pgxp_gpr_to_gte({rd}, {rt});"]
            if n == "mfc2": return set_(rt, f"gte_read_data(c, {rd})")
            if n == "cfc2": return set_(rt, f"gte_read_ctrl(c, {rd})")
            if n == "mtc2": return [f"gte_write_data(c, {rd}, {R(rt)});"]
            if n == "ctc2": return [f"gte_write_ctrl(c, {rd}, {R(rt)});"]
            if n == "cop2": return [f"gte_command(c, 0x{imm:07X}u);"]
        if k == K_GTE_MEM:
            if 12 <= rt <= 15:  # SXY: the precise-vertex shadow goes along
                if n == "lwc2":
                    return [f"{{ const uint32_t a_ = {addr}; gte_write_data(c, {rt}, MEM_R32(c, a_)); pgxp_mem_to_gte({rt}, a_); }}"]
            if 12 <= rt <= 15 or rt == 22:  # 22 = RGB2: exact UVs (gte.cpp)
                if n == "swc2":
                    return [f"{{ const uint32_t a_ = {addr}; MEM_W32(c, a_, gte_read_data(c, {rt})); pgxp_gte_to_mem({rt}, a_); }}"]
            if n == "lwc2": return [f"gte_write_data(c, {rt}, MEM_R32(c, {addr}));"]
            if n == "swc2": return [f"MEM_W32(c, {addr}, gte_read_data(c, {rt}));"]
        if k == K_SYSCALL:
            return [f"psx_syscall(c, 0x{i.addr:08X}u);"]
        if k == K_BREAK:
            return [f"psx_break(c, 0x{i.addr:08X}u, {i.imm});"]
        raise ValueError(f"cannot emit {i}")

    def emit_simple(self, i: Insn):
        self.pending_cycles += 1
        for s in self.simple(i):
            self.out(s)

    # ------------------------------------------------------------ control flow

    def branch_cond(self, i: Insn) -> str:
        n = i.op
        if n == "beq":  return f"{R(i.rs)} == {R(i.rt)}"
        if n == "bne":  return f"{R(i.rs)} != {R(i.rt)}"
        if n == "blez": return f"{S(i.rs)} <= 0"
        if n == "bgtz": return f"{S(i.rs)} > 0"
        if n in ("bltz", "bltzal"): return f"{S(i.rs)} < 0"
        if n in ("bgez", "bgezal"): return f"{S(i.rs)} >= 0"
        raise ValueError(n)

    def emit_control(self, i: Insn):
        """Emit a control-flow instruction together with its delay slot."""
        pc = i.addr
        ds = self.img.insn(pc + 4)
        k = i.kind
        self.pending_cycles += 2

        if k == K_BRANCH:
            always = i.op == "beq" and i.rs == 0 and i.rt == 0 or (i.op == "bgez" and i.rs == 0)
            if not always:
                self.out(f"{{ const int cond = {self.branch_cond(i)};")
            for s in self.simple(ds):
                self.out("  " + s if not always else s)
            self.flush_cycles()
            back = i.target <= pc
            chk = "CHECK_EVENTS(c); " if back else ""
            if always:
                self.out(f"{chk}{self.goto(i.target)}")
            else:
                self.out(f"  if (cond) {{ {chk}{self.goto(i.target)} }} }}")
            return

        if k == K_CALL:
            if i.op == "jal":
                for s in self.simple(ds):
                    self.out(s)
                self.flush_cycles()
                call = f"c->r[31] = 0x{pc + 8:08X}u; {self.call(i.target)}"
                if pc in OPTIONAL_CALLS:
                    call = f"if (!{OPTIONAL_CALLS[pc]}) {{ {call} }}"
                self.out(call)
            else:  # bltzal / bgezal: ra is written whether or not taken
                self.out(f"{{ const int cond = {self.branch_cond(i)};")
                for s in self.simple(ds):
                    self.out("  " + s)
                self.flush_cycles()
                self.out(f"  c->r[31] = 0x{pc + 8:08X}u; if (cond) {{ {self.call(i.target)} }} }}")
            return

        if k == K_JUMP:
            for s in self.simple(ds):
                self.out(s)
            self.flush_cycles()
            if i.target in self.fn.tail_calls:
                self.out(f"{self.call(i.target)} return;")
            else:
                chk = "CHECK_EVENTS(c); " if i.target <= pc else ""
                self.out(f"{chk}{self.goto(i.target)}")
            return

        if k == K_JR:
            if i.rs == RA:
                ret_check = f"CHECK_RET(c, entry_ra, entry_sp, 0x{pc:08X}u); " if self.debug_ra else ""
                for s in self.simple(ds):
                    self.out(s)
                self.flush_cycles()
                self.out(f"{ret_check}return;")
                return
            self.out(f"{{ const uint32_t tgt = {R(i.rs)};")
            for s in self.simple(ds):
                self.out("  " + s)
            self.flush_cycles()
            jt = self.fn.jump_tables.get(pc)
            if jt:
                self.out("  switch (tgt) {")
                for t in sorted(set(jt.targets)):
                    self.out(f"    case 0x{t:08X}u: {self.goto(t)}")
                self.out(f"    default: psx_bad_jump(c, 0x{pc:08X}u, tgt); return;")
                self.out("  } }")
            else:
                # BIOS stubs (jr t2 = 0xA0/B0/C0) and function-pointer tail calls; an
                # unrecognised intra-function jump continues in the interpreter until
                # this function returns to its caller.
                self.out("  jump_indirect(c, tgt, entry_ra); return; }")
            return

        if k == K_JALR:
            self.out(f"{{ const uint32_t tgt = {R(i.rs)};")
            for s in self.simple(ds):
                self.out("  " + s)
            self.flush_cycles()
            link = f"c->r[{i.rd}] = 0x{pc + 8:08X}u; " if i.rd else ""
            self.out(f"  {link}call_indirect(c, tgt); }}")
            return

        raise ValueError(f"unhandled control flow {i}")

    # ------------------------------------------------------------ function body

    def emit(self) -> str:
        fn = self.fn
        addrs = sorted(fn.addrs)
        addr_set = fn.addrs
        # instructions consumed as delay slots of the preceding control-flow insn
        delay_slots = {a + 4 for a in addrs if self.img.insn(a).has_delay_slot}
        branch_targets = set(fn.labels)

        pos = 0
        prev_falls_to: int | None = None
        if addrs[0] != fn.entry:
            # code below the entry (e.g. a loop body placed before it) is emitted first
            self.out(self.goto(fn.entry))
        while pos < len(addrs):
            a = addrs[pos]
            if a in delay_slots and a not in branch_targets:
                pos += 1
                continue
            if prev_falls_to is not None and prev_falls_to != a:
                # flow continues somewhere not adjacent in emission order
                self.flush_cycles()
                self.out(self.goto(prev_falls_to))
            if a in branch_targets:
                self.flush_cycles()
                self.label_positions[a] = len(self.lines)
                self.lines.append(f"{lbl(a)}:;")
            i = self.img.insn(a)
            if a in INSN_HOOKS:
                self.flush_cycles()
                self.out(INSN_HOOKS[a])
            self.out(f"// {a:08X}: {disasm(i)}")
            if i.has_delay_slot:
                self.emit_control(i)
                unconditional_exit = (
                    i.kind in (K_JUMP, K_JR)
                    or (i.kind == K_BRANCH and i.op == "beq" and i.rs == 0 and i.rt == 0)
                )
                if unconditional_exit:
                    prev_falls_to = None
                else:
                    prev_falls_to = a + 8
                # a delay slot that is also a branch target is emitted separately
                if a + 4 in branch_targets and a + 4 in addr_set:
                    if prev_falls_to is not None:
                        self.out(self.goto(prev_falls_to))
                    prev_falls_to = None
                    pos += 1
                    continue
                pos += 2 if (pos + 1 < len(addrs) and addrs[pos + 1] == a + 4) else 1
                continue
            self.emit_simple(i)
            prev_falls_to = a + 4
            pos += 1

        if prev_falls_to is not None:
            self.flush_cycles()
            if prev_falls_to in addr_set:
                self.out(self.goto(prev_falls_to))
            else:
                self.out(f"psx_fell_off(c, 0x{prev_falls_to:08X}u); return;")

        # drop labels nothing jumps to (avoids unused-label warnings)
        body = []
        label_lines = {v: k for k, v in self.label_positions.items()}
        for idx, line in enumerate(self.lines):
            if idx in label_lines and label_lines[idx] not in self.used_labels:
                continue
            body.append(line)
        missing = self.used_labels - set(self.label_positions)
        if missing:
            raise ValueError(f"{fn.name}: goto targets without labels: "
                             + ", ".join(f"{m:08X}" for m in sorted(missing)))

        head = [f"void {fn.name}(R3000* c) {{",
                "    const uint32_t entry_ra = c->r[31]; (void)entry_ra;"]
        if self.debug_ra:
            head.append("    const uint32_t entry_sp = c->r[29]; (void)entry_sp;")
        if fn.entry in ENTRY_HOOKS:
            head.append("    " + ENTRY_HOOKS[fn.entry])
        return "\n".join(head + body + ["}", ""])

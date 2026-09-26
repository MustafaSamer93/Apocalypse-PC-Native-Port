// Fallback MIPS interpreter for code the static recompiler did not discover
// (unexpected function pointers, code copied to RAM at runtime).
//
// It runs one "function": it returns when execution jumps to the return address the
// function was entered with. Calls it makes go back through call_indirect, so
// recompiled code is used wherever it exists.
#include <unordered_set>

#include "psx.h"

namespace {

uint32_t fetch(R3000* c, uint32_t pc) { return MEM_R32(c, pc); }

int32_t simm(uint32_t w) { return (int16_t)(w & 0xFFFF); }

// Called functions that were not recompiled are appended to extra_seeds.txt;
// tools/recomp/main.py reads it so the next recompile makes them native. Jumps into the
// middle of a function are only logged: they are not function entry points.
void report_once(uint32_t pc, bool is_call) {
    static std::unordered_set<uint32_t> seen;
    if (!seen.insert(pc).second) return;
    log_msg(LogCat::Core, "interpreting unrecompiled code at %08X%s", pc, is_call ? "" : " (intra-function jump)");
    if (!is_call || (pc & 0x1FFFFFFF) < 0x10000) return;  // not code: a jump through a null pointer
    if (FILE* f = std::fopen("extra_seeds.txt", "a")) {
        std::fprintf(f, "%08X\n", pc);
        std::fclose(f);
    }
}

}  // namespace

// Executes one instruction that is not a branch/jump. Returns false if it is one.
static bool exec_simple(R3000* c, uint32_t pc, uint32_t w) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    uint32_t sa = (w >> 6) & 31, fn = w & 63;
    uint32_t* r = c->r;
    auto set = [&](uint32_t reg, uint32_t v) { if (reg) r[reg] = v; };
    uint32_t ea = r[rs] + (uint32_t)simm(w);
    switch (op) {
        case 0x00:
            switch (fn) {
                case 0x00: set(rd, r[rt] << sa); return true;
                case 0x02: set(rd, r[rt] >> sa); return true;
                case 0x03: set(rd, (uint32_t)((int32_t)r[rt] >> sa)); return true;
                case 0x04: set(rd, r[rt] << (r[rs] & 31)); return true;
                case 0x06: set(rd, r[rt] >> (r[rs] & 31)); return true;
                case 0x07: set(rd, (uint32_t)((int32_t)r[rt] >> (r[rs] & 31))); return true;
                case 0x0C: psx_syscall(c, pc); return true;
                case 0x0D: psx_break(c, pc, (w >> 6) & 0xFFFFF); return true;
                case 0x10: set(rd, c->hi); return true;
                case 0x11: c->hi = r[rs]; return true;
                case 0x12: set(rd, c->lo); return true;
                case 0x13: c->lo = r[rs]; return true;
                case 0x18: MULT(c, (int32_t)r[rs], (int32_t)r[rt]); return true;
                case 0x19: MULTU(c, r[rs], r[rt]); return true;
                case 0x1A: DIV(c, (int32_t)r[rs], (int32_t)r[rt]); return true;
                case 0x1B: DIVU(c, r[rs], r[rt]); return true;
                case 0x20: case 0x21: set(rd, r[rs] + r[rt]); return true;
                case 0x22: case 0x23: set(rd, r[rs] - r[rt]); return true;
                case 0x24: set(rd, r[rs] & r[rt]); return true;
                case 0x25: set(rd, r[rs] | r[rt]); return true;
                case 0x26: set(rd, r[rs] ^ r[rt]); return true;
                case 0x27: set(rd, ~(r[rs] | r[rt])); return true;
                case 0x2A: set(rd, (int32_t)r[rs] < (int32_t)r[rt]); return true;
                case 0x2B: set(rd, r[rs] < r[rt]); return true;
                case 0x08: case 0x09: return false;
            }
            break;
        case 0x08: case 0x09: set(rt, r[rs] + (uint32_t)simm(w)); return true;
        case 0x0A: set(rt, (int32_t)r[rs] < simm(w)); return true;
        case 0x0B: set(rt, r[rs] < (uint32_t)simm(w)); return true;
        case 0x0C: set(rt, r[rs] & (w & 0xFFFF)); return true;
        case 0x0D: set(rt, r[rs] | (w & 0xFFFF)); return true;
        case 0x0E: set(rt, r[rs] ^ (w & 0xFFFF)); return true;
        case 0x0F: set(rt, w << 16); return true;
        case 0x10:
            if (rs == 0) { set(rt, cop0_read(c, (int)rd)); return true; }
            if (rs == 4) { cop0_write(c, (int)rd, r[rt]); return true; }
            if (rs == 0x10 && fn == 0x10) { cop0_rfe(c); return true; }
            break;
        case 0x12:
            if (rs & 0x10) { gte_command(c, w & 0x1FFFFFF); return true; }
            if (rs == 0) { set(rt, gte_read_data(c, (int)rd)); return true; }
            if (rs == 2) { set(rt, gte_read_ctrl(c, (int)rd)); return true; }
            if (rs == 4) { gte_write_data(c, (int)rd, r[rt]); return true; }
            if (rs == 6) { gte_write_ctrl(c, (int)rd, r[rt]); return true; }
            break;
        case 0x20: set(rt, (uint32_t)(int8_t)MEM_R8(c, ea)); return true;
        case 0x21: set(rt, (uint32_t)(int16_t)MEM_R16(c, ea)); return true;
        case 0x22: set(rt, MEM_LWL(c, ea, r[rt])); return true;
        case 0x23: set(rt, MEM_R32(c, ea)); return true;
        case 0x24: set(rt, MEM_R8(c, ea)); return true;
        case 0x25: set(rt, MEM_R16(c, ea)); return true;
        case 0x26: set(rt, MEM_LWR(c, ea, r[rt])); return true;
        case 0x28: MEM_W8(c, ea, r[rt]); return true;
        case 0x29: MEM_W16(c, ea, r[rt]); return true;
        case 0x2A: MEM_SWL(c, ea, r[rt]); return true;
        case 0x2B: MEM_W32(c, ea, r[rt]); return true;
        case 0x2E: MEM_SWR(c, ea, r[rt]); return true;
        case 0x32: gte_write_data(c, (int)rt, MEM_R32(c, ea)); return true;
        case 0x3A: MEM_W32(c, ea, gte_read_data(c, (int)rt)); return true;
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
            return false;
    }
    fatal("interpreter: invalid instruction %08X at %08X", w, pc);
}

static void interp_loop(R3000* c, uint32_t pc, const uint32_t return_to);

// jal sites switched off at runtime; keep in sync with OPTIONAL_CALLS in tools/recomp/emit.py
static bool optional_call_skipped(uint32_t pc) {
    switch (pc) {
        case 0x80030BCC: return psx_opt_skip_frame_wait != 0;
        case 0x80030B80:
        case 0x80030BBC: return psx_opt_sim60 != 0;
        case 0x800145BC: return psx_opt_menu60 != 0;
        case 0x80012F10:
        case 0x80012F18: return psx_opt_menu_odd != 0;
        default: return false;
    }
}

void interp_run(R3000* c, uint32_t pc) {
    // The kernel's low memory holds no game code: this is a call through a null (or corrupt)
    // function pointer. Running it would only execute junk and hang; stop with the evidence.
    if ((pc & 0x1FFFFFFF) < 0x1000)
        fatal("call to %08X (a null function pointer) from ra=%08X a0=%08X", pc, c->r[31], c->r[4]);
    report_once(pc, true);
    interp_loop(c, pc, c->r[31]);
}

void interp_run_until(R3000* c, uint32_t pc, const uint32_t return_to) {
    report_once(pc, false);
    interp_loop(c, pc, return_to);
}

static void interp_loop(R3000* c, uint32_t pc, const uint32_t return_to) {
    for (;;) {
        uint32_t w = fetch(c, pc);
        if (exec_simple(c, pc, w)) {
            pc += 4;
            ADD_CYCLES(1);
            continue;
        }
        // Branch/jump: resolve target and condition before the delay slot runs.
        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
        uint32_t* r = c->r;
        uint32_t btarget = pc + 4 + ((uint32_t)simm(w) << 2);
        bool taken = false, link = false, reg_jump = false;
        uint32_t target = 0, link_reg = 31;
        switch (op) {
            case 0x00:  // jr / jalr
                target = r[rs];
                taken = true;
                reg_jump = true;
                if ((w & 63) == 0x09) { link = true; link_reg = rd; }
                break;
            case 0x01: {
                bool ge = rt & 1;
                taken = ge ? (int32_t)r[rs] >= 0 : (int32_t)r[rs] < 0;
                if ((rt & 0x1E) == 0x10) link = true;
                target = btarget;
                break;
            }
            case 0x02: case 0x03:
                target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFF) << 2);
                taken = true;
                link = op == 0x03;
                break;
            case 0x04: taken = r[rs] == r[rt]; target = btarget; break;
            case 0x05: taken = r[rs] != r[rt]; target = btarget; break;
            case 0x06: taken = (int32_t)r[rs] <= 0; target = btarget; break;
            case 0x07: taken = (int32_t)r[rs] > 0; target = btarget; break;
        }
        uint32_t ds = fetch(c, pc + 4);
        if (!exec_simple(c, pc + 4, ds)) fatal("interpreter: branch in delay slot at %08X", pc + 4);
        ADD_CYCLES(2);
        if (link) {
            if (link_reg) r[link_reg] = pc + 8;
            bool skip = optional_call_skipped(pc);
            if (taken && !skip) call_indirect(c, target);
            pc += 8;
            continue;
        }
        if (!taken) {
            pc += 8;
            continue;
        }
        if (reg_jump && target == return_to) return;  // function return
        if (op == 0x02 || reg_jump) {
            // j / jr leaving the function: a tail call if it lands on a known function
            if (dispatch_lookup(target) || (target & 0x1FFFFFFF) == 0xA0 || (target & 0x1FFFFFFF) == 0xB0 ||
                (target & 0x1FFFFFFF) == 0xC0) {
                call_indirect(c, target);
                return;
            }
        }
        if (target <= pc) CHECK_EVENTS(c);
        pc = target;
    }
}

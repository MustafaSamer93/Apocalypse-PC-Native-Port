// Runtime ABI for statically recompiled PS1 code.
// Generated C (generated/*.c) includes this; the runtime implements everything declared here.
#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define RECOMP_INLINE static __forceinline
#define RECOMP_LIKELY(x) (x)
#define RECOMP_UNLIKELY(x) (x)
#else
#define RECOMP_INLINE static inline __attribute__((always_inline))
#define RECOMP_LIKELY(x) __builtin_expect(!!(x), 1)
#define RECOMP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

typedef struct R3000 {
    uint32_t r[32];
    uint32_t hi, lo;
    uint64_t cycles;      // CPU cycles since boot (approximate: 1 per instruction + stalls)
    uint64_t next_event;  // when cycles >= next_event the scheduler must run
    uint32_t cop0[32];
    int32_t gte_data[32];
    int32_t gte_ctrl[32];
} R3000;

typedef void (*RecompFuncPtr)(R3000* c);
typedef struct RecompFunc {
    uint32_t addr;
    RecompFuncPtr fn;
} RecompFunc;

// ---------------------------------------------------------------- memory

#define PSX_RAM_SIZE 0x200000u
#define PSX_SCRATCH_SIZE 0x400u

extern uint8_t psx_ram[PSX_RAM_SIZE];
extern uint8_t psx_scratch[PSX_SCRATCH_SIZE];
extern int psx_cache_isolated;  // COP0 SR.IsC: stores go to the (unemulated) I-cache

uint32_t mem_read_slow(R3000* c, uint32_t addr, int size);
void mem_write_slow(R3000* c, uint32_t addr, uint32_t value, int size);

// Debug write watchpoint (--watch): physical word address, 0 = off.
extern uint32_t psx_watch_addr;
void psx_watch_hit(R3000* c, uint32_t addr, uint32_t value, int size);
#define PSX_WATCH_CHECK(c, a, v, n)                                                         \
    do {                                                                                    \
        if (RECOMP_UNLIKELY(psx_watch_addr) && (((a) & 0x1FFFFCu) == psx_watch_addr))      \
            psx_watch_hit((c), (a), (v), (n));                                              \
    } while (0)

// RAM is 2 MB mirrored four times in the low 8 MB of each segment (KUSEG/KSEG0/KSEG1).
#define PSX_IS_RAM(a) (((a) & 0x1F800000u) == 0)
#define PSX_IS_SCRATCH(a) (((a) & 0x1FFFFC00u) == 0x1F800000u)

#define DEFINE_MEM_READ(bits, type)                                              \
    RECOMP_INLINE uint32_t MEM_R##bits(R3000* c, uint32_t a) {                   \
        type v;                                                                  \
        if (RECOMP_LIKELY(PSX_IS_RAM(a))) {                                      \
            memcpy(&v, psx_ram + (a & (PSX_RAM_SIZE - 1)), sizeof v);            \
            return v;                                                            \
        }                                                                        \
        if (PSX_IS_SCRATCH(a)) {                                                 \
            memcpy(&v, psx_scratch + (a & (PSX_SCRATCH_SIZE - 1)), sizeof v);    \
            return v;                                                            \
        }                                                                        \
        return mem_read_slow(c, a, bits / 8);                                    \
    }
#define DEFINE_MEM_WRITE(bits, type)                                             \
    RECOMP_INLINE void MEM_W##bits(R3000* c, uint32_t a, uint32_t value) {       \
        type v = (type)value;                                                    \
        PSX_WATCH_CHECK(c, a, value, bits / 8);                                  \
        if (RECOMP_LIKELY(PSX_IS_RAM(a) && !psx_cache_isolated)) {               \
            memcpy(psx_ram + (a & (PSX_RAM_SIZE - 1)), &v, sizeof v);            \
            return;                                                              \
        }                                                                        \
        if (PSX_IS_SCRATCH(a) && !psx_cache_isolated) {                          \
            memcpy(psx_scratch + (a & (PSX_SCRATCH_SIZE - 1)), &v, sizeof v);    \
            return;                                                              \
        }                                                                        \
        mem_write_slow(c, a, value, bits / 8);                                   \
    }
DEFINE_MEM_READ(8, uint8_t)
DEFINE_MEM_READ(16, uint16_t)
DEFINE_MEM_READ(32, uint32_t)
DEFINE_MEM_WRITE(8, uint8_t)
DEFINE_MEM_WRITE(16, uint16_t)
DEFINE_MEM_WRITE(32, uint32_t)

// Unaligned word access (little-endian semantics).
RECOMP_INLINE uint32_t MEM_LWL(R3000* c, uint32_t a, uint32_t old) {
    uint32_t w = MEM_R32(c, a & ~3u);
    switch (a & 3) {
        case 0: return (old & 0x00FFFFFFu) | (w << 24);
        case 1: return (old & 0x0000FFFFu) | (w << 16);
        case 2: return (old & 0x000000FFu) | (w << 8);
        default: return w;
    }
}
RECOMP_INLINE uint32_t MEM_LWR(R3000* c, uint32_t a, uint32_t old) {
    uint32_t w = MEM_R32(c, a & ~3u);
    switch (a & 3) {
        case 0: return w;
        case 1: return (old & 0xFF000000u) | (w >> 8);
        case 2: return (old & 0xFFFF0000u) | (w >> 16);
        default: return (old & 0xFFFFFF00u) | (w >> 24);
    }
}
RECOMP_INLINE void MEM_SWL(R3000* c, uint32_t a, uint32_t v) {
    uint32_t aa = a & ~3u, w = MEM_R32(c, aa);
    switch (a & 3) {
        case 0: w = (w & 0xFFFFFF00u) | (v >> 24); break;
        case 1: w = (w & 0xFFFF0000u) | (v >> 16); break;
        case 2: w = (w & 0xFF000000u) | (v >> 8); break;
        default: w = v; break;
    }
    MEM_W32(c, aa, w);
}
RECOMP_INLINE void MEM_SWR(R3000* c, uint32_t a, uint32_t v) {
    uint32_t aa = a & ~3u, w = MEM_R32(c, aa);
    switch (a & 3) {
        case 0: w = v; break;
        case 1: w = (w & 0x000000FFu) | (v << 8); break;
        case 2: w = (w & 0x0000FFFFu) | (v << 16); break;
        default: w = (w & 0x00FFFFFFu) | (v << 24); break;
    }
    MEM_W32(c, aa, w);
}

// ---------------------------------------------------------------- precise vertices (PGXP)

// Every RAM/scratchpad word and CPU register has a shadow: the exact screen position and depth
// of the projected vertex (GTE SXY) it last held, and the whole-pixel word it was for (v). The
// generated code copies shadows on lw/sw, register moves and SXY transfers; nothing else
// clears them: whoever uses a shadow checks v against the word it has (gpu.cpp), so a
// shadow left behind by other code is simply not taken. w = 0: no exact position.
typedef struct PgxpVal {
    uint32_t v;
    float x, y, w;
} PgxpVal;
extern PgxpVal pgxp_gpr[32];
extern PgxpVal pgxp_ram[PSX_RAM_SIZE / 4];
extern PgxpVal pgxp_scratch[PSX_SCRATCH_SIZE / 4];

RECOMP_INLINE PgxpVal* PGXP_MEM(uint32_t a) {
    if (RECOMP_LIKELY(PSX_IS_RAM(a))) return &pgxp_ram[(a & (PSX_RAM_SIZE - 1)) >> 2];
    if (PSX_IS_SCRATCH(a)) return &pgxp_scratch[(a & (PSX_SCRATCH_SIZE - 1)) >> 2];
    return 0;
}
RECOMP_INLINE void PGXP_LW(int rt, uint32_t a) {
    const PgxpVal* m = PGXP_MEM(a);
    if (m) pgxp_gpr[rt] = *m;
    else pgxp_gpr[rt].w = 0;
}
// lh/lhu of a word's low half: its shadow (the exact UV behind the subdivision's UV halfword);
// the high half gets none
RECOMP_INLINE void PGXP_LH(int rt, uint32_t a) {
    const PgxpVal* m = (a & 2) ? 0 : PGXP_MEM(a);
    if (m) pgxp_gpr[rt] = *m;
    else pgxp_gpr[rt].w = 0;
}
RECOMP_INLINE void PGXP_SW(int rt, uint32_t a) {
    PgxpVal* m = PGXP_MEM(a);
    if (m) *m = pgxp_gpr[rt];
}
#define PGXP_MOVE(rd, rs) (pgxp_gpr[rd] = pgxp_gpr[rs])
// SXY registers (12..15) to and from CPU registers and memory (gte.cpp)
void pgxp_gte_to_gpr(int gpr, int reg);
void pgxp_gpr_to_gte(int reg, int gpr);
void pgxp_gte_to_mem(int reg, uint32_t a);
void pgxp_mem_to_gte(int reg, uint32_t a);

// ---------------------------------------------------------------- arithmetic

RECOMP_INLINE void MULT(R3000* c, int32_t a, int32_t b) {
    int64_t p = (int64_t)a * (int64_t)b;
    c->lo = (uint32_t)p;
    c->hi = (uint32_t)((uint64_t)p >> 32);
}
RECOMP_INLINE void MULTU(R3000* c, uint32_t a, uint32_t b) {
    uint64_t p = (uint64_t)a * (uint64_t)b;
    c->lo = (uint32_t)p;
    c->hi = (uint32_t)(p >> 32);
}
// R3000A results for division by zero and INT_MIN / -1 (no exception is raised).
RECOMP_INLINE void DIV(R3000* c, int32_t n, int32_t d) {
    if (d == 0) {
        c->hi = (uint32_t)n;
        c->lo = n >= 0 ? 0xFFFFFFFFu : 1u;
    } else if ((uint32_t)n == 0x80000000u && d == -1) {
        c->hi = 0;
        c->lo = 0x80000000u;
    } else {
        c->lo = (uint32_t)(n / d);
        c->hi = (uint32_t)(n % d);
    }
}
RECOMP_INLINE void DIVU(R3000* c, uint32_t n, uint32_t d) {
    if (d == 0) {
        c->hi = n;
        c->lo = 0xFFFFFFFFu;
    } else {
        c->lo = n / d;
        c->hi = n % d;
    }
}

// ---------------------------------------------------------------- timing / events

void psx_run_events(R3000* c);
// Emulated CPU speed: cycles charged per instruction in quarter cycles (default 8 = 2
// cycles; smaller values make the CPU faster, e.g. for the 60 Hz simulation), with the
// fraction carried over so nothing is lost to rounding.
extern uint32_t psx_cpi_q;
extern uint32_t psx_cycle_frac;
#define ADD_CYCLES(n)                                       do {                                                        psx_cycle_frac += (uint32_t)(n) * psx_cpi_q;            c->cycles += psx_cycle_frac >> 2;                       psx_cycle_frac &= 3;                                } while (0)
#define CHECK_EVENTS(c) do { if (RECOMP_UNLIKELY((c)->cycles >= (c)->next_event)) psx_run_events(c); } while (0)
// Average cost of an instruction including cache/memory stalls. Recompiled code runs
// far faster than this on the host; the value only paces emulated time.
#define PSX_CYCLES_PER_INSN 2

// ---------------------------------------------------------------- control flow

void call_indirect(R3000* c, uint32_t addr);

// Entry hooks (see ENTRY_HOOKS in tools/recomp/emit.py).
void psx_safepoint(R3000* c, uint32_t pc);
void psx_hook_level_start(void);
void psx_hook_progress(R3000* c);  // the game recorded a checkpoint (savestate.cpp)
void psx_hook_2d(R3000* c, int stage);        // the render's 2D routines start (gpu.cpp)
int psx_hook_2d_call(R3000* c, int which);  // nonzero: a 2D routine was run by the hook (gpu.cpp)
int psx_hook_render(R3000* c);  // nonzero: the render was done by the hook
int psx_hook_mesh(R3000* c);  // nonzero: the level mesh was drawn natively (mesh.cpp)
int psx_hook_frustum(R3000* c);  // nonzero: the culling planes were built by the hook (drawdist.cpp)
int psx_hook_tick(R3000* c);  // nonzero: the tick was handled (sim60.cpp)
void psx_hook_player(R3000* c);  // entry of the player's update, a0 = player (sim60.cpp)
void psx_hook_contacts(R3000* c, int after);  // just before (0) / after (1) the player's update in a tick (sim60.cpp)
void psx_hook_game_over(R3000* c);  // no lives left: restart the level (savestate.cpp)
void psx_hook_enemy_damage(R3000* c);  // one hit kill cheat (cheats.cpp)
int psx_hook_player_use(R3000* c);   // a switch the player used fires: skip on the odd tick (sim60.cpp)
void psx_hook_zone(R3000* c);  // a force zone pushes a list, a0 = zone, a1 = list head (sim60.cpp)
// A loop that only waits for an interrupt (reads memory an event handler changes): emulated time
// jumps to the next event, which the loop's CHECK_EVENTS then runs, as spinning to it would.
RECOMP_INLINE void psx_idle_until_event(R3000* c) {
    if (c->cycles < c->next_event) c->cycles = c->next_event;
}
void psx_hook_glow_sort(R3000* c);  // glow renderer: v0 = ordering-table slot, a1 = depth, s4 = glow (sim60.cpp)
void psx_hook_strip_corner(R3000* c);  // level transform: a strip corner is stored (gte.cpp)
void psx_hook_camera(R3000* c);        // render camera: smoothed view (gte.cpp)
void psx_hook_aim(R3000* c);           // player aim: s0 = player, fire vector +471/+472 just set (host_sdl.cpp)
// the game's menus (game_menu.cpp): add item, destroy, move selection, draw, read select/back
void psx_hook_menu_init(R3000* c);
void psx_hook_menu_add(R3000* c);
void psx_hook_menu_free(R3000* c);
int psx_hook_menu_update(R3000* c);
int psx_hook_menu_camera(R3000* c);
int psx_hook_menu_draw(R3000* c);  // nonzero: drawn by the hook
int psx_hook_menu_keys(R3000* c);  // nonzero: read by the hook
int psx_hook_voice(R3000* c);     // 8002FC64 play a voice clip (a0 block, a1 channel); nonzero: silent
void psx_hook_weapon_pickup(R3000* c);  // 8005E260 give a weapon (a0 player, a1 type)
void psx_hook_message(R3000* c);  // 8001B788 show a message (a0 text)

// Runtime switches for patched call sites (see OPTIONAL_CALLS in tools/recomp/emit.py).
extern int psx_opt_skip_frame_wait;  // 1 = skip the main loop's second VBlank wait (60 fps)
extern int psx_opt_sim60;            // 1 = 60 Hz simulation prototype (sim60.cpp)
extern int psx_opt_menu60;           // 1 = the front end (menus) runs at 60 Hz (game_menu.cpp)
extern int psx_opt_menu_odd;         // 1 = this front end loop is an in-between one (60 Hz)
// `jr reg` that is not a known switch: a tail call if `addr` is a function or BIOS
// vector, otherwise a jump inside the current function (interpreted until it returns
// to `func_ra`, the address the current function was called with).
void jump_indirect(R3000* c, uint32_t addr, uint32_t func_ra);
void psx_bad_jump(R3000* c, uint32_t pc, uint32_t target);
void psx_fell_off(R3000* c, uint32_t pc);
void psx_ra_mismatch(R3000* c, uint32_t pc, uint32_t expected, uint32_t actual);
void psx_sp_mismatch(R3000* c, uint32_t pc, uint32_t expected, uint32_t actual);
#ifdef RECOMP_RA_CHECK
// On `jr ra`: the function should return where it was called from, with sp restored.
#define CHECK_RET(c, entry_ra, entry_sp, pc)                                                        \
    do {                                                                                            \
        if ((c)->r[31] != (entry_ra)) psx_ra_mismatch((c), (pc), (entry_ra), (c)->r[31]);          \
        if ((c)->r[29] != (entry_sp)) psx_sp_mismatch((c), (pc), (entry_sp), (c)->r[29]);          \
    } while (0)
#else
#define CHECK_RET(c, entry_ra, entry_sp, pc) ((void)0)
#endif

// ---------------------------------------------------------------- coprocessors / traps

uint32_t cop0_read(R3000* c, int reg);
void cop0_write(R3000* c, int reg, uint32_t value);
void cop0_rfe(R3000* c);

uint32_t gte_read_data(R3000* c, int reg);
uint32_t gte_read_ctrl(R3000* c, int reg);
void gte_write_data(R3000* c, int reg, uint32_t value);
void gte_write_ctrl(R3000* c, int reg, uint32_t value);
void gte_command(R3000* c, uint32_t cmd);

void psx_syscall(R3000* c, uint32_t pc);
void psx_break(R3000* c, uint32_t pc, uint32_t code);

#ifdef __cplusplus
}
#endif

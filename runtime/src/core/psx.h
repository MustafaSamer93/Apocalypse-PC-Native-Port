// The emulated machine: logging, timing, interrupts, the hardware modules' interfaces, the BIOS
// and code dispatch. (The recompiled code's own ABI is runtime/include/recomp.h.)
#pragma once

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"

class StateIO;

// ---------------------------------------------------------------- logging

enum class LogCat { Core, Bios, Mem, Gpu, Cd, Spu, Dma, Irq, Pad, Mdec, Host };
void log_msg(LogCat cat, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
bool log_enabled(LogCat cat);
void log_enable(const char* list);  // comma-separated category names, or "all"
#define LOG(cat, ...) do { if (log_enabled(LogCat::cat)) log_msg(LogCat::cat, __VA_ARGS__); } while (0)
// Always-on ring of recent notable events, dumped when something goes wrong.
void trace(const char* fmt, ...);
void trace_dump();

// ---------------------------------------------------------------- clock

constexpr uint64_t PSX_CPU_HZ = 33868800;
constexpr uint64_t CYCLES_PER_FRAME_NTSC = PSX_CPU_HZ / 60;  // ~564480 (263 lines * 2146 cycles)
constexpr uint32_t CYCLES_PER_SCANLINE_NTSC = 2146;
constexpr uint32_t SCANLINES_NTSC = 263;

// ---------------------------------------------------------------- scheduler

enum class Ev : int { Scanline, Cdrom, CdromRead, DmaGpu, DmaCd, DmaSpu, DmaMdec, Spu, Timer0, Timer1, Timer2, Sio, Count };
void sched_init(R3000* c);
void sched_add(R3000* c, Ev ev, uint64_t cycles_from_now);
void sched_cancel(R3000* c, Ev ev);
void sched_request_check(R3000* c);  // forces psx_run_events at the next check point
void sched_refresh(R3000* c);        // recomputes the next event (after a state load)
uint32_t psx_current_scanline();

// ---------------------------------------------------------------- interrupts

enum Irq : uint32_t {
    IRQ_VBLANK = 1 << 0, IRQ_GPU = 1 << 1, IRQ_CDROM = 1 << 2, IRQ_DMA = 1 << 3,
    IRQ_TMR0 = 1 << 4, IRQ_TMR1 = 1 << 5, IRQ_TMR2 = 1 << 6, IRQ_SIO0 = 1 << 7,
    IRQ_SIO1 = 1 << 8, IRQ_SPU = 1 << 9, IRQ_LIGHTPEN = 1 << 10,
};
void irq_raise(R3000* c, uint32_t bits);
uint32_t irq_read(int reg);            // 0 = I_STAT, 1 = I_MASK
void irq_write(R3000* c, int reg, uint32_t v);
bool irq_should_take(R3000* c);        // I_STAT&I_MASK and COP0 enables

// ---------------------------------------------------------------- DMA, timers, controller port (hw.cpp)

void dma_init();
uint32_t dma_read(uint32_t addr);
void dma_write(R3000* c, uint32_t addr, uint32_t v);
void dma_event(R3000* c, int channel);

void timers_init();
uint32_t timers_read(R3000* c, uint32_t addr);
void timers_write(R3000* c, uint32_t addr, uint32_t v);
void timers_scanline(R3000* c, bool vblank_start, bool vblank_end);

void sio_init();
uint32_t sio_read(R3000* c, uint32_t addr, int size);
void sio_write(R3000* c, uint32_t addr, uint32_t v, int size);
void sio_event(R3000* c);

extern uint32_t g_gpu_ot_z;          // depth of the ordering-table slot being sent (0: none)
extern uint32_t g_gpu_packet_addr;   // RAM address of the GPU packet being processed
extern uint32_t g_gpu_word_addr;     // RAM address of the GP0 word being sent (0: not from a list)

// ---------------------------------------------------------------- GPU (gpu.cpp)

void gpu_init();
uint32_t gpu_read_data(R3000* c);
uint32_t gpu_read_stat(R3000* c);
void gpu_write_gp0(R3000* c, uint32_t v);
void gpu_write_gp1(R3000* c, uint32_t v);
void gpu_vblank();
struct GpuDisplay {
    const uint16_t* vram;  // 1024x512 16-bit
    int x, y, w, h;        // display area in VRAM pixels (w in 16-bit units for 15-bit mode)
    bool rgb24;
    bool enabled;
};
GpuDisplay gpu_display();
bool gpu_drawn_display(GpuDisplay* out);  // the frame just drawn, where the game will show it
uint32_t gpu_run_list(R3000* c, uint32_t addr);  // walks a GPU packet list (ordering table) into GP0
int gpu_set_renderer(int scale, int samples = 1);  // 0 = software, 1..9 = hardware at that scale; MSAA samples
// A movie (MDEC) is playing, or played within the last half second: movies are shown at
// 4:3 as they were made, everything else in the chosen aspect (gpu.cpp, mdec.cpp).
void gpu_note_movie_frame();
bool gpu_movie_playing();
void gpu_list_done();      // an ordering table was sent to the GPU (hw.cpp)
bool gpu_title_screen();   // the title screen (PRESS START) is up
extern bool g_display_flipped;   // the game set the display start since VBlank
extern uint64_t g_display_flips; // ... how many times so far
extern bool g_smooth_font;       // setting: the game's text and button icons with smoothed textures
extern uint64_t g_menu_drawn;    // the last frame the game drew one of its menus

// ---------------------------------------------------------------- GTE (gte.cpp)

// Widescreen: horizontal scale applied to GTE screen coordinates (1 = off).
extern float g_gte_widescreen;

// Pose blending between two renders (gte.cpp, sim60.cpp)
enum class GteBlendMode { Off, Record, Blend };
struct GteBlendVtx {
    int16_t vx, vy, vz;  // model-space input
    int32_t sxy, sz;     // screen output
    float px, py, pw;    // exact screen position and depth (pw 0: none)
};
extern GteBlendMode g_gte_blend;
void gte_blend_begin(GteBlendMode m);

// Precise vertices: the exact screen position (x, y) and view depth (w) of recent projections,
// found by the whole-pixel vertex word the GPU received. Different vertices can land on the
// same pixel: every one is kept, numbered in projection order (seq).
struct GtePrecise {
    float x, y, w;
    uint32_t seq;
};
extern bool g_gte_precise;  // setting: draw 3D polygons at their exact positions, perspective-correct
int gte_precise_find(uint32_t xy, GtePrecise* out, int max);  // candidates found (0: none)
void gte_precise_clear();
// PGXP: the exact vertex for a GPU vertex word read from RAM address a (false: none)
bool pgxp_find(uint32_t a, uint32_t word, GtePrecise* out);
bool pgxp_uv_find(uint32_t a, uint32_t word, float* u, float* v);  // exact texture coordinates
// GTE results a ghost render must not leave behind (core.cpp core_ghost_*)
extern PgxpVal g_rgb2_exact;
extern double g_mac_exact[4];
extern bool g_mac_exact_ok;
void gte_frame_stats();  // per-frame command counts (--log gpu)

// ---------------------------------------------------------------- CD-ROM (cdrom.cpp)

void cdrom_init(const std::string& cue_path);
uint8_t cdrom_read(R3000* c, uint32_t reg);
void cdrom_write(R3000* c, uint32_t reg, uint8_t v);
void cdrom_event(R3000* c);
void cdrom_read_event(R3000* c);
uint32_t cdrom_dma_read_word();
void cdrom_voice_poll();  // once a frame: the rest of a voice line a data read cut
// Direct file access for the BIOS HLE (FlushCache-free loaders, open("cdrom:...")).
bool cdrom_find_file(const char* path, uint32_t* lba, uint32_t* size);
bool cdrom_read_sector_user(uint32_t lba, uint8_t out[2048]);

// ---------------------------------------------------------------- SPU (spu.cpp)

void spu_init();
uint16_t spu_read(uint32_t addr);
void spu_write(R3000* c, uint32_t addr, uint16_t v);
void spu_dma_write(uint32_t word);
uint32_t spu_dma_read();
void spu_scanline();           // produces samples due so far (keeps sound events on time)
void spu_end_frame(R3000* c);  // hands this frame's samples to the host
void spu_push_cd_audio(const int16_t* stereo, int frames, int sample_rate);
size_t spu_cd_queued();  // CD audio frames not played yet

// ---------------------------------------------------------------- MDEC (mdec.cpp)

void mdec_init();
uint32_t mdec_read(uint32_t reg);
void mdec_write(uint32_t reg, uint32_t v);
void mdec_dma_in(uint32_t word);
uint32_t mdec_dma_out();
bool mdec_out_ready();

// ---------------------------------------------------------------- core (core.cpp)

extern R3000* g_cpu;
extern bool g_host_menu_open;  // a host menu is open: the game stops on purpose (watchdog)
void watchdog_start();         // freeze report thread
void core_early_cancel();      // a picture upload: forget the frame shown early
void core_frame_drawn(R3000* c);  // a frame's ordering table was drawn (gpu_run_list)

// Ghost mode (sim60.cpp): run game code outside emulated time, then roll memory,
// scheduler and interrupts back. Exceeding the cycle budget longjmps to `abort`.
void core_ghost_begin(R3000* c, uint64_t cycle_budget, std::jmp_buf* abort);
int core_ghost_end(R3000* c);  // returns how many sound/CD/movie writes were dropped
bool core_in_ghost();

void dispatch_register(const RecompFunc* table, uint32_t count);
RecompFuncPtr dispatch_lookup(uint32_t addr);
void interp_run(R3000* c, uint32_t pc);  // runs until jr to the entry ra
void interp_run_until(R3000* c, uint32_t pc, uint32_t return_to);

// Memory helpers for C++ code.
inline uint8_t* ram_ptr(uint32_t addr) { return psx_ram + (addr & (PSX_RAM_SIZE - 1)); }
std::string ram_cstr(uint32_t addr, size_t max = 256);

// ---------------------------------------------------------------- BIOS HLE (bios.cpp)

void bios_init(R3000* c);
void bios_call_a0(R3000* c);
void bios_call_b0(R3000* c);
void bios_call_c0(R3000* c);
void bios_take_interrupt(R3000* c);
void bios_vblank(R3000* c);          // BIOS-side pad/card polling at VBlank
void bios_pad_refresh(R3000* c);     // the pads into their BIOS buffers
void bios_deliver_event(R3000* c, uint32_t cls, uint32_t spec);
bool bios_in_exception();

// ---------------------------------------------------------------- save states
// Each module saves and loads its state through one function (state_io.h).

void core_state(StateIO& s);
void bios_state(StateIO& s);
void bios_ghost_state(StateIO& s);  // what a ghost render may change
void gpu_state(StateIO& s);
void gpu_regs_state(StateIO& s);
void hw_state(StateIO& s);
void cdrom_state(StateIO& s);
void spu_state(StateIO& s);
void mdec_state(StateIO& s);

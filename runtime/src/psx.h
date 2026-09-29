// Internal runtime interfaces shared by the hardware/BIOS modules.
#pragma once

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"

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
bool sched_pending(Ev ev);
void sched_request_check(R3000* c);  // forces psx_run_events at the next check point

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

// ---------------------------------------------------------------- subsystems

void dma_init();
uint32_t dma_read(uint32_t addr);
void dma_write(R3000* c, uint32_t addr, uint32_t v);
void dma_event(R3000* c, int channel);

void timers_init();
uint32_t timers_read(R3000* c, uint32_t addr);
void timers_write(R3000* c, uint32_t addr, uint32_t v);
void timers_scanline(R3000* c, bool vblank_start, bool vblank_end);

void gpu_init();
uint32_t gpu_read_data(R3000* c);
uint32_t gpu_read_stat(R3000* c);
void gpu_write_gp0(R3000* c, uint32_t v);
void gpu_write_gp1(R3000* c, uint32_t v);
void gpu_vblank();
bool gpu_is_interlaced_odd_field();
struct GpuDisplay {
    const uint16_t* vram;  // 1024x512 16-bit
    int x, y, w, h;        // display area in VRAM pixels (w in 16-bit units for 15-bit mode)
    bool rgb24;
    bool enabled;
};
GpuDisplay gpu_display();
bool gpu_drawn_display(GpuDisplay* out);  // the frame just drawn, where the game will show it
void core_frame_drawn(R3000* c);          // a frame's ordering table was drawn (gpu_run_list)
uint32_t gpu_run_list(R3000* c, uint32_t addr);  // walks a GPU packet list (ordering table) into GP0
void gpu_vram_save(int x, int y, int w, int h, std::vector<uint16_t>& out);
void gpu_vram_restore(int x, int y, int w, int h, const std::vector<uint16_t>& in);

// Ghost mode (smooth.cpp): run game code outside emulated time, then roll memory,
// scheduler and interrupts back. Exceeding the cycle budget longjmps to `abort`.
void core_ghost_begin(R3000* c, uint64_t cycle_budget, std::jmp_buf* abort);
int core_ghost_end(R3000* c);  // returns how many sound/CD/movie writes were dropped
bool core_in_ghost();
void smooth_on_flip();         // the game showed a new frame (GP1 display start)

// Pose blending between two renders (gte.cpp, sim60.cpp)
enum class GteBlendMode { Off, Record, Blend };
struct GteBlendVtx {
    int16_t vx, vy, vz;  // model-space input
    int32_t sxy, sz;     // screen output
    float px, py, pw;    // exact screen position and depth (pw 0: none)
};

// Precise vertices (gte.cpp): the exact screen position (x, y) and view depth (w) of recent
// projections, found by the whole-pixel vertex word the GPU received. Different vertices can
// land on the same pixel: every one is kept, numbered in projection order (seq).
struct GtePrecise {
    float x, y, w;
    uint32_t seq;
};
extern bool g_gte_precise;  // setting: draw 3D polygons at their exact positions, perspective-correct
int gte_precise_find(uint32_t xy, GtePrecise* out, int max);  // candidates found (0: none)
void gte_precise_clear();
// PGXP (gte.cpp): the exact vertex for a GPU vertex word read from RAM address a (0: none)
bool pgxp_find(uint32_t a, uint32_t word, GtePrecise* out);
extern uint32_t g_gpu_word_addr;  // RAM address of the GP0 word being sent (0: not from a list)
extern uint32_t g_precise_stats[8];
extern uint32_t g_corner_stats[2];  // level grid vertices rebuilt from exact corners, corner sets rejected
extern GteBlendMode g_gte_blend;
extern uint32_t g_blend_stats[3];
void gte_blend_begin(GteBlendMode m);

// Draw distance (drawdist.cpp): percent of the game's own view distance (100 = original)
extern int g_draw_distance;
extern bool g_smooth_font;  // setting: draw the game's text and button icons with smoothed textures (gpu.cpp)
extern int g_menu_zoom;     // setting: the game's menus at this percent of their size (game_menu.cpp)
// Enhancement settings, shared by F1 and the game's Options -> Enhancements (host_sdl.cpp).
enum EnhSetting { E_FPS, E_SIM, E_DRAWDIST, E_PRECISE, E_SMOOTH_FONT, E_FONT_SCALE, E_RENDERER, E_SCREEN, E_RES, E_ASPECT, E_DISPLAY, E_AUTOSAVE, E_SUBTITLES, E_AA,
                   E_CHEAT_HEALTH, E_CHEAT_LIVES, E_CHEAT_WEAPONS, E_CHEAT_AMMO, E_CHEAT_BOMBS,
                   E_FPS_COUNTER };
const char* enh_name(int id);
std::string enh_value(int id);
void enh_change(int id, int dir);
void game_menu_reset(bool state_loaded);  // a saved state was loaded / a level starts (game_menu.cpp)
// Shows `text` (at most 10 characters) the way the game shows a pickup (game_menu.cpp).
void game_message(R3000* c, const char* text);
// A subtitle for `seconds` on a random side, tilted (game_menu.cpp); subtitles.cpp decides when.
void game_subtitle(R3000* c, const char* text, float seconds);
void game_subtitle_keep(R3000* c);  // once per frame: shows a cleared subtitle again
void subtitle_note_xa(uint32_t lba, int chan);  // cdrom.cpp: an XA sector is being played
void subtitle_poll(R3000* c);                    // once per frame (savestate.cpp)
void game_voice_poll(R3000* c);                  // once per frame: queued pickup lines (game_menu.cpp)
float subtitle_tilt(uint32_t text);
void game_test_pickup(R3000* c, int what);
void game_test_health(R3000* c, int amount);
void game_test_grab(R3000* c);
void game_test_spawn(R3000* c, int type);
void game_test_spawn_poll(R3000* c);
// Cheats (cheats.cpp): 0 health, 1 lives, 2 weapons, 3 ammo, 4 smart bombs. Off at start, not saved.
bool cheat_on(int which);
void cheat_set(int which, bool on);
void cheats_frame(R3000* c);                     // once per frame (savestate.cpp)
bool cheat_quiet();
void weapons_check(R3000* c, uint32_t player);  // a freed weapon left in a slot is dropped (cheats.cpp)                              // the cheat is giving a weapon: no voice line
void cheat_note_pickup(uint32_t player, int type);  // a weapon pickup (8005E260)  // debug: APOC_TEST_PICKUP (game_menu.cpp)  // gpu.cpp: the angle to draw the text at `text` with (0 = level)
extern bool g_subtitles;  // setting (subtitles.cpp)
extern bool g_autosave;  // setting: auto-save at checkpoints (savestate.cpp)

// 60 Hz simulation (sim60.cpp)
extern int g_sim60_setting;    // 0 = 30 Hz, 1 = 60 Hz in level 1, 2 = 60 Hz in every level
extern bool g_sim60_force;     // --sim60
extern std::string g_sim60_level;
extern bool g_sim60_demo;
void sim60_apply();
void sim60_note_file(const std::string& file);
int gpu_set_renderer(int scale, int samples = 1);  // 0 = software, 1..8 = OpenGL at that scale; MSAA samples

// Widescreen hack: horizontal scale applied to GTE screen coordinates (1 = off).
extern float g_gte_widescreen;
// A movie (MDEC) is playing, or played within the last half second: movies are shown at
// 4:3 as they were made, everything else in the chosen aspect (gpu.cpp, mdec.cpp).
void gpu_note_movie_frame();
bool gpu_movie_playing();
void gpu_list_done();  // an ordering table was sent to the GPU (hw.cpp)
// Replacement pictures (replace.cpp): uploads and copies in VRAM, the whole VRAM re-sent, and the
// look for known pictures at each VBlank.
void replace_note_upload(int x, int y, int w, int h);
void replace_note_copy(int sx, int sy, int dx, int dy, int w, int h);
void replace_note_vram_reset();
void replace_check(const uint16_t* vram);
extern uint32_t g_gpu_packet_addr;  // RAM address of the GPU packet being processed (hw.cpp)

void cdrom_init(const std::string& cue_path);
uint8_t cdrom_read(R3000* c, uint32_t reg);
void cdrom_write(R3000* c, uint32_t reg, uint8_t v);
void cdrom_event(R3000* c);
void cdrom_read_event(R3000* c);
uint32_t cdrom_dma_read_word();
// Direct file access for the BIOS HLE (FlushCache-free loaders, open("cdrom:...")).
bool cdrom_find_file(const char* path, uint32_t* lba, uint32_t* size);
bool cdrom_read_sector_user(uint32_t lba, uint8_t out[2048]);

void spu_init();
uint16_t spu_read(uint32_t addr);
void spu_write(R3000* c, uint32_t addr, uint16_t v);
void spu_dma_write(uint32_t word);
uint32_t spu_dma_read();
void spu_scanline();           // produces samples due so far (keeps sound events on time)
void spu_end_frame(R3000* c);  // hands this frame's samples to the host
void spu_push_cd_audio(const int16_t* stereo, int frames, int sample_rate);

void mdec_init();
uint32_t mdec_read(uint32_t reg);
void mdec_write(uint32_t reg, uint32_t v);
void mdec_dma_in(uint32_t word);
uint32_t mdec_dma_out();
bool mdec_out_ready();

void sio_init();
uint32_t sio_read(R3000* c, uint32_t addr, int size);
void sio_write(R3000* c, uint32_t addr, uint32_t v, int size);
void sio_event(R3000* c);

// ---------------------------------------------------------------- pad / host input

struct PadState {
    uint16_t buttons = 0xFFFF;  // active low, PS1 digital pad bit order
    bool connected = true;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;  // analog sticks, 0x80 = centre
    bool has_sticks = false;    // a real gamepad with sticks is connected
};
// `analog` tells the host the game switched the pad to analog mode, so the left stick
// should stop doubling as the d-pad.
PadState host_pad(int port, bool analog = false);
void host_rumble(int port, uint8_t small_motor, uint8_t large_motor);
void host_poll();                    // pump OS events
uint64_t host_frame_count();
bool host_quit_requested();
bool host_multi_present();  // several images per emulated frame (smooth frame rate)
void host_request_quit();            // close the game (as closing the window does)
void host_present(const GpuDisplay& d);
void host_throttle_frame();          // sleep to hold 60 Hz
void host_audio_push(const int16_t* stereo, int frames);
int host_audio_queued_frames();

// ---------------------------------------------------------------- BIOS HLE / CPU glue

void bios_init(R3000* c);
void bios_call_a0(R3000* c);
void bios_call_b0(R3000* c);
void bios_call_c0(R3000* c);
void bios_take_interrupt(R3000* c);
void bios_vblank(R3000* c);          // BIOS-side pad/card polling at VBlank
void bios_pad_refresh(R3000* c);     // the pads into their BIOS buffers
extern bool g_display_flipped;       // the game set the display start since VBlank (gpu.cpp)
void bios_deliver_event(R3000* c, uint32_t cls, uint32_t spec);

void dispatch_register(const RecompFunc* table, uint32_t count);
void dispatch_unregister_range(uint32_t start, uint32_t end);
RecompFuncPtr dispatch_lookup(uint32_t addr);
void interp_run(R3000* c, uint32_t pc);  // runs until jr to the entry ra
void interp_run_until(R3000* c, uint32_t pc, uint32_t return_to);

// Memory helpers for C++ code (always go through the full address decode).
inline uint8_t* ram_ptr(uint32_t addr) { return psx_ram + (addr & (PSX_RAM_SIZE - 1)); }
uint32_t ram_r32(uint32_t addr);
void ram_w32(uint32_t addr, uint32_t v);
std::string ram_cstr(uint32_t addr, size_t max = 256);

extern R3000* g_cpu;
void psx_watch_dma(uint32_t phys, uint32_t len, const char* who);

// Internal runtime interfaces shared by the hardware/BIOS modules.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

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
};
PadState host_pad(int port);
void host_poll();                    // pump OS events
uint64_t host_frame_count();
bool host_quit_requested();
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

// Entry point: loads the game's executable from the user's disc image into emulated RAM,
// brings up the hardware and jumps to the recompiled entry point.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <csetjmp>

#include "memcard.h"
#include "psx.h"
#include "savestate.h"

extern std::jmp_buf g_resume_jmp;

extern "C" {
extern const RecompFunc main_func_table[];
extern const uint32_t main_func_count;
extern const uint32_t main_exe_text_addr;
extern const uint32_t main_exe_text_size;
extern const uint32_t main_exe_hash;
}

bool host_init(const char* title, int scale);
void host_shutdown();
bool host_parse_press_script(const char* spec);
void host_set_turbo(bool on);
bool host_open_wav(const char* path);
void host_close_wav();

namespace {

uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void usage() {
    std::printf(
        "usage: apocalypse [--cue PATH] [--memcard-dir DIR] [--scale N] [--log cats]\n"
        "  --cue PATH         disc image cue sheet (default: asset/Apocalypse (USA).cue)\n"
        "  --memcard-dir DIR  where memory card images are kept (default: saves)\n"
        "  --scale N          initial window scale (default 3)\n"
        "  --log cats         comma list: core,bios,mem,gpu,cd,spu,dma,irq,pad,mdec,host,all\n"
        "  --smooth K         K images per game step (1 = original 30 fps; default: match the display)\n"
        "  --refresh HZ       display refresh rate to assume (if the detected one is wrong)\n"
        "  --sim60            60 Hz simulation in every level (default: level 1 only; F1 menu)\n"
        "keys: arrows, Z=Cross X=Circle A=Square S=Triangle Q/W=L1/R1 1/2=L2/R2\n"
        "      Enter=Start Backspace=Select Tab=fast-forward F11=fullscreen Esc=quit\n");
}

// Returns the EXE's entry point.
uint32_t load_exe_from_disc(R3000* c) {
    uint32_t lba, size;
    if (!cdrom_find_file("cdrom:\\SLUS_003.73;1", &lba, &size))
        fatal("SLUS_003.73 not found on the disc: is this Apocalypse (USA)?");
    std::vector<uint8_t> exe(((size + 2047) / 2048) * 2048);
    for (uint32_t i = 0; i < exe.size() / 2048; i++) cdrom_read_sector_user(lba + i, &exe[i * 2048]);
    if (std::memcmp(exe.data(), "PS-X EXE", 8) != 0) fatal("SLUS_003.73 is not a PS-X EXE");
    uint32_t pc0, gp0, t_addr, t_size;
    std::memcpy(&pc0, &exe[0x10], 4);
    std::memcpy(&gp0, &exe[0x14], 4);
    std::memcpy(&t_addr, &exe[0x18], 4);
    std::memcpy(&t_size, &exe[0x1C], 4);
    if (t_addr != main_exe_text_addr || t_size != main_exe_text_size ||
        fnv1a(&exe[0x800], t_size) != main_exe_hash)
        fatal("this disc's executable does not match the one the code was recompiled from");
    std::memcpy(psx_ram + (t_addr & (PSX_RAM_SIZE - 1)), &exe[0x800], t_size);
    c->r[28] = gp0;
    return pc0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string cue = "asset/Apocalypse (USA).cue";
    std::string card_dir = "saves";
    int scale = 3;
    bool interp_only = false;  // debugging: run everything in the interpreter
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(1); }
            return argv[++i];
        };
        if (a == "--cue") cue = next();
        else if (a == "--memcard-dir") card_dir = next();
        else if (a == "--scale") scale = std::max(1, std::atoi(next().c_str()));
        else if (a == "--log") log_enable(next().c_str());
        else if (a == "--dump-ram") {  // --dump-ram CYCLES PATH
            extern uint64_t g_dump_ram_at;
            extern std::string g_dump_ram_path;
            g_dump_ram_at = std::strtoull(next().c_str(), nullptr, 10);
            g_dump_ram_path = next();
        }
        else if (a == "--turbo") host_set_turbo(true);
        else if (a == "--interp") interp_only = true;
        else if (a == "--fps60") psx_opt_skip_frame_wait = 1;
        else if (a == "--sim60") {  // 60 Hz simulation in any level (sim60.cpp)
            g_sim60_force = true;
            sim60_apply();
        }
        else if (a == "--refresh") {  // --refresh HZ: display refresh rate to assume
            extern float g_refresh_override;
            g_refresh_override = (float)std::atof(next().c_str());
        }
        else if (a == "--smooth") {  // --smooth K: K images per game step (1 = original 30 fps)
            extern int g_force_smooth;
            g_force_smooth = std::clamp(std::atoi(next().c_str()), 1, 5);
        }
        else if (a == "--states") {
            extern bool host_parse_state_script(const char*);
            if (!host_parse_state_script(next().c_str())) { usage(); return 1; }
        }
        else if (a == "--screenshot") {  // --screenshot FRAME[,FRAME...] PREFIX
            extern std::vector<uint64_t> g_shot_frames;
            extern std::string g_shot_prefix;
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                size_t e = list.find(',', p);
                if (e == std::string::npos) e = list.size();
                g_shot_frames.push_back(std::strtoull(list.substr(p, e - p).c_str(), nullptr, 10));
                p = e + 1;
            }
            g_shot_prefix = next();
        }
        else if (a == "--wav") {
            if (!host_open_wav(next().c_str())) { std::fprintf(stderr, "cannot write wav\n"); return 1; }
        }
        else if (a == "--trace-call") {
            extern uint32_t g_trace_call_addr;
            g_trace_call_addr = (uint32_t)std::strtoul(next().c_str(), nullptr, 16);
        }
        else if (a == "--gte-dump") {
            extern uint64_t g_gte_dump_frame;
            g_gte_dump_frame = std::strtoull(next().c_str(), nullptr, 10);
        }
        else if (a == "--dump-vram") {  // --dump-vram FRAME PATH
            extern uint64_t g_dump_vram_frame;
            extern std::string g_dump_vram_path;
            g_dump_vram_frame = std::strtoull(next().c_str(), nullptr, 10);
            g_dump_vram_path = next();
        }
        else if (a == "--press") {
            if (!host_parse_press_script(next().c_str())) { usage(); return 1; }
        }
        else if (a == "--watch") psx_watch_addr = (uint32_t)std::strtoul(next().c_str(), nullptr, 16) & 0x1FFFFC;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { usage(); return 1; }
    }

    static R3000 cpu;
    std::memset(&cpu, 0, sizeof cpu);
    g_cpu = &cpu;

    cdrom_init(cue);
    std::filesystem::create_directories(card_dir);
    memcard(0).load_or_create(card_dir + "/card1.mcr");
    memcard(1).load_or_create(card_dir + "/card2.mcr");

    if (!host_init("Apocalypse", scale)) fatal("could not initialise the window/audio");
    sched_init(&cpu);
    dma_init();
    timers_init();
    gpu_init();
    spu_init();
    mdec_init();
    sio_init();
    bios_init(&cpu);
    if (!interp_only) dispatch_register(main_func_table, main_func_count);

    uint32_t entry = load_exe_from_disc(&cpu);
    cpu.r[29] = 0x801FFF00;  // STACK from SYSTEM.CNF
    cpu.r[30] = cpu.r[29];
    cpu.r[31] = 0;
    sched_add(&cpu, Ev::Scanline, CYCLES_PER_SCANLINE_NTSC);

    // Loading a save state longjmps back here (from the VSync safe point) with the machine
    // restored; the game then continues from VSync's entry: VSync itself runs recompiled,
    // and everything above it (the callers' remaining code) is interpreted, while every
    // function those callers invoke is recompiled again.
    if (setjmp(g_resume_jmp) == 0) {
        call_indirect(&cpu, entry);
    } else {
        uint32_t pc;
        if (!state_take_resume(&pc)) fatal("resumed without a loaded state");
        uint32_t return_to = cpu.r[31];
        call_indirect(&cpu, pc);
        for (;;) {
            interp_run_until(&cpu, return_to, 0xFFFFFFFFu);  // never "returns" to the sentinel
            return_to = cpu.r[31];  // left through a tail call: continue where that returned
        }
    }

    log_msg(LogCat::Core, "game returned from its entry point");
    memcard(0).flush();
    memcard(1).flush();
    host_close_wav();
    host_shutdown();
    return 0;
}

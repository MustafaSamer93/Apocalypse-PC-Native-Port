// Entry point: loads the game's executable from the user's disc image into emulated RAM,
// brings up the hardware and jumps to the recompiled entry point.
#include <algorithm>
#include <cstring>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <csetjmp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // the Windows entry point for a windowed (non-console) exe

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

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
        "usage: repocalypse [--cue PATH] [--memcard-dir DIR] [--scale N] [--log cats]\n"
        "  --cue PATH         disc image .cue or .bin (default: the image in assets/image)\n"
        "  --memcard-dir DIR  where memory card images are kept (default: saves)\n"
        "  --scale N          window height 240*N (default: 1080 lines, or fullscreen / what fits)\n"
        "  --log cats         comma list: core,bios,mem,gpu,cd,spu,dma,irq,pad,mdec,host,all\n"
        "  --smooth K         K images per game step (1 = original 30 fps; default: match the display)\n"
        "  --refresh HZ       display refresh rate to assume (if the detected one is wrong)\n"
        "  --sim60            60 Hz simulation in every level (default: levels 1-4 only; F1 menu)\n"
        "  --chapter N        start at chapter N (saves/chapters; F2 or F1 -> Chapter lists them)\n"
        "  --renderer R       opengl or vulkan (default: the F1 setting, OpenGL at first)\n"
        "  --bios-intro       play the BIOS boot sequence (assets/bios) even with other options\n"
        "keys: arrows, Z=Cross X=Circle A=Square S=Triangle Q/W=L1/R1 1/2=L2/R2\n"
        "      Enter=Start Backspace=Select Tab=fast-forward F2=chapters F11=fullscreen Esc=quit\n");
}

// The assets folder: the game disc (required) and the optional extras, made at start so the
// player sees where things go.
void make_asset_folders() {
    std::error_code ec;
    for (const char* d : {"assets/image", "assets/bios", "assets/replacements/textures",
                          "assets/replacements/fmvs"})
        std::filesystem::create_directories(d, ec);
}

// The disc image in assets/image, whatever its files are called: the first .cue (by name), else
// the first .bin (cdrom.cpp then takes the folder's .bin files as the tracks).
std::string find_disc() {
    std::error_code ec;
    std::vector<std::filesystem::path> cues, bins;
    for (const auto& e : std::filesystem::directory_iterator("assets/image", ec)) {
        std::string ext = e.path().extension().string();
        for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
        if (ext == ".cue") cues.push_back(e.path());
        if (ext == ".bin") bins.push_back(e.path());
    }
    std::sort(cues.begin(), cues.end());
    std::sort(bins.begin(), bins.end());
    return !cues.empty() ? cues[0].string() : !bins.empty() ? bins[0].string() : std::string();
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

// The exe is a Windows app (no console window of its own). Its log output still reaches a
// file or pipe it is redirected to; started from a terminal, it writes there instead.
void attach_console() {
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;  // redirected
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

#ifdef _WIN32
// A crash in the runtime itself (not the game's code): write where it happened to the log, so a
// silent close can be tracked down (the address is relative to the exe; see the .map file).
static LONG WINAPI log_native_crash(EXCEPTION_POINTERS* e) {
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    std::fprintf(stderr, "NATIVE CRASH: exception %08lX at exe+%llX (address %p)\n", e->ExceptionRecord->ExceptionCode,
                 (unsigned long long)(at - base), (void*)at);
    // the stack at the crash: every word that points into the exe (return addresses among them)
    const auto* sp = (const uintptr_t*)e->ContextRecord->Rsp;
    const auto* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    int found = 0;
    for (int i = 0; i < 512 && found < 24; i++)
        if (sp[i] > base && sp[i] < end) std::fprintf(stderr, "  stack+%X: exe+%llX\n", i * 8, (unsigned long long)(sp[i] - base)), found++;
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void log_native_crash(int sig) {
    char msg[64];
    const int n = std::snprintf(msg, sizeof msg, "NATIVE CRASH: signal %d\n", sig);
    if (n > 0) (void)!write(STDERR_FILENO, msg, (size_t)n);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

// When a file was last written (0 = unknown).
static std::time_t file_mtime(const char* path) {
#ifdef _WIN32
    struct _stat64 st{};
    return _stat64(path, &st) == 0 ? (std::time_t)st.st_mtime : 0;
#else
    struct stat st{};
    return stat(path, &st) == 0 ? st.st_mtime : 0;
#endif
}

// Nobody sees stderr (started by a double-click / desktop launcher, nothing redirected).
static bool stderr_unseen() {
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    return !GetConsoleWindow() && (!err || err == INVALID_HANDLE_VALUE || GetFileType(err) == FILE_TYPE_UNKNOWN);
#else
    struct stat st{};  // a terminal, file or pipe is seen; /dev/null or a journal socket is not
    return !isatty(STDERR_FILENO) && (fstat(STDERR_FILENO, &st) != 0 || !(S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode)));
#endif
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(log_native_crash);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) std::signal(sig, log_native_crash);
#endif
    attach_console();
    // Everything the game uses sits next to the exe (the assets and saves folders,
    // settings.ini), wherever it was started from.
    if (const char* base = SDL_GetBasePath()) {
        std::error_code ec;
        std::filesystem::current_path(std::filesystem::u8path(base), ec);
    }
    // Started with no console and nothing redirected (a double-click): log to
    // debug/repocalypse.log (the report log testers send), with the previous runs' logs kept in
    // debug/logs (the newest 10, named by when they were written) and crashed runs' as
    // debug/crash_DATE_TIME.log.
    if (stderr_unseen()) {
        std::error_code ec;
        std::filesystem::create_directories("debug", ec);
        // a log that ended in a crash is kept (debug/crash_DATE_TIME.log) instead of being
        // rotated away by the next two runs
        for (const char* old : {"debug/repocalypse.old.log", "debug/repocalypse.log"}) {
            std::ifstream in(old);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            if (text.find("FATAL") == std::string::npos && text.find("NATIVE CRASH") == std::string::npos) continue;
            std::time_t t = file_mtime(old);
            if (!t) t = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof name, "debug/crash_%Y%m%d_%H%M%S.log", std::localtime(&t));
            std::filesystem::rename(old, name, ec);
        }
        if (std::filesystem::exists("debug/repocalypse.log", ec)) {
            std::filesystem::create_directories("debug/logs", ec);
            std::time_t t = file_mtime("debug/repocalypse.log");
            if (!t) t = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof name, "debug/logs/repocalypse_%Y%m%d_%H%M%S.log", std::localtime(&t));
            std::filesystem::rename("debug/repocalypse.log", name, ec);
            std::vector<std::filesystem::path> logs;  // names sort by date: drop all but the newest 10
            for (const auto& e : std::filesystem::directory_iterator("debug/logs", ec))
                if (e.path().extension() == ".log") logs.push_back(e.path());
            std::sort(logs.begin(), logs.end());
            for (size_t i = 0; i + 10 < logs.size(); i++) std::filesystem::remove(logs[i], ec);
        }
        if (std::freopen("debug/repocalypse.log", "a", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    std::string cue;  // --cue, else find_disc()
    std::string card_dir = "saves";
    int scale = 0;  // window: 0 = 1080 lines or as much as fits
    bool interp_only = false;  // debugging: run everything in the interpreter
    // The BIOS boot sequence plays on a plain start; scripted runs (chapters, presses, screenshots)
    // count frames from the game's start, so any option leaves it out unless --bios-intro is given.
    bool bios_intro = argc == 1 && !std::getenv("APOC_NO_BIOS_BOOT");
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
        else if (a == "--bios-intro") bios_intro = true;
        else if (a == "--fps60") psx_opt_skip_frame_wait = 1;
        else if (a == "--sim60") {  // 60 Hz simulation in any level (sim60.cpp)
            g_sim60_force = true;
            sim60_apply();
        }
        else if (a == "--renderer") {  // --renderer opengl|vulkan (default: the saved setting)
            extern int g_renderer_override;
            std::string r = next();
            g_renderer_override = r == "vulkan" ? 1 : r == "opengl" ? 0 : -1;
        }
        else if (a == "--refresh") {  // --refresh HZ: display refresh rate to assume
            extern float g_refresh_override;
            g_refresh_override = (float)std::atof(next().c_str());
        }
        else if (a == "--smooth") {  // --smooth K: K images per game step (1 = original 30 fps)
            extern int g_force_smooth;
            g_force_smooth = std::clamp(std::atoi(next().c_str()), 1, 5);
        }
        else if (a == "--chapter") {  // --chapter N: load chapter N (1 = the first) once running
            extern bool host_parse_state_script(const char*);
            const int n = std::atoi(next().c_str());
            if (n < 1 || !host_parse_state_script(("load" + std::to_string(kChapterBase + n - 1) + "@30").c_str())) {
                usage();
                return 1;
            }
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

    make_asset_folders();
    if (cue.empty()) cue = find_disc();
    if (cue.empty()) {
        const char* msg =
            "The game disc was not found.\n\nPut your own copy of Apocalypse (USA) in the \"assets\\image\" "
            "folder next to repocalypse.exe: its .cue and .bin files (any names), or just the .bin files.\n\n"
            "Everything else in assets (BIOS, replacements) is optional.";
        std::fprintf(stderr, "%s\n", msg);
        if (!std::getenv("APOC_NO_DIALOGS")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Repocalypse", msg, nullptr);
        return 1;
    }

    static R3000 cpu;
    std::memset(&cpu, 0, sizeof cpu);
    g_cpu = &cpu;

    cdrom_init(cue);
    std::filesystem::create_directories(card_dir);
    memcard(0).load_or_create(card_dir + "/card1.mcr");
    memcard(1).load_or_create(card_dir + "/card2.mcr");

    if (!host_init("Repocalypse", scale)) fatal("could not initialise the window/audio");
    watchdog_start();
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
    if (bios_intro && (argc > 1 || g_boot_sequence)) bios_boot_sequence(&cpu, entry);  // (--bios-intro: always)

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

#include "savestate.h"

#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

#include "memcard.h"
#include "psx.h"
#include "smooth.h"
#include "state_io.h"

extern "C" const uint32_t main_exe_hash;

void core_state(StateIO& s);
void bios_state(StateIO& s);
void gpu_state(StateIO& s);
void hw_state(StateIO& s);
void cdrom_state(StateIO& s);
void spu_state(StateIO& s);
void mdec_state(StateIO& s);
void sched_refresh(R3000* c);
bool bios_in_exception();
void host_audio_clear();
void host_toast(const std::string& text);

std::jmp_buf g_resume_jmp;  // set in main()

namespace {

constexpr uint32_t kMagic = 0x53504F41;  // "AOPS"
constexpr uint32_t kVersion = 2;  // 2: never taken inside an interrupt handler

int g_save_request = -1, g_load_request = -1;
bool g_resume_pending = false;
uint32_t g_resume_pc = 0;

// Auto-save: when the game records progress (800189EC: a checkpoint, or the start of the
// next level) with a level or restart point different from the last auto-save's. Dying and
// restarting at the same checkpoint records the same point again and does not save.
constexpr uint32_t kProgressLevel = 0x800A53D8;    // level file name ("prsn_t")
constexpr uint32_t kProgressRestart = 0x800A53E1;  // restart point ("Re_Cat1_Prsn")
bool g_progress_pending = false;
uint64_t g_progress_frame = 0;
std::string g_saved_level, g_saved_restart;  // the point of the last auto-save (or the start)
bool g_demo_loaded = false;          // a .dem file was loaded since the last level start
uint64_t g_last_load_frame = 0;      // last time the game loaded a file

std::string ram_string(uint32_t addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)psx_ram[(addr + i) & (PSX_RAM_SIZE - 1)];
        if (!ch) break;
        s += ch;
    }
    return s;
}

// The progress point the game has recorded: "level/restart".
void read_progress(std::string* level, std::string* restart) {
    *level = ram_string(kProgressLevel, 9);
    *restart = ram_string(kProgressRestart, 24);
}

std::string slot_path(int slot) {
    std::filesystem::create_directories("saves");
    return slot == kAutoSlot ? "saves/autosave.state" : "saves/slot" + std::to_string(slot) + ".state";
}

void serialize(StateIO& s, R3000* c, uint32_t& pc) {
    uint32_t magic = kMagic, version = kVersion, hash = main_exe_hash;
    s(magic);
    s(version);
    s(hash);
    if (s.loading() && (magic != kMagic || version != kVersion || hash != main_exe_hash)) {
        log_msg(LogCat::Core, "save state is from a different version or game");
        return;
    }
    s(pc);
    s(*c);
    core_state(s);
    bios_state(s);
    gpu_state(s);
    hw_state(s);
    cdrom_state(s);
    spu_state(s);
    mdec_state(s);
    s.marker(0x454E4421);  // "END!"
    // optional trailer (absent in older files): which level is running, for the 60 Hz mode
    if (!s.loading() || !s.at_end()) {
        s.marker(0x53363020);  // "S60 "
        s.str(g_sim60_level);
        s(g_sim60_demo);
        if (s.loading()) sim60_apply();
    }
}

bool save_to(int slot, R3000* c, uint32_t pc) {
    StateIO s(false);
    serialize(s, c, pc);
    std::string path = slot_path(slot), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(s.data().data(), 1, s.data().size(), f) == s.data().size();
    std::fclose(f);
    if (!ok) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    log_msg(LogCat::Core, "saved state to %s (%zu KB)", path.c_str(), s.data().size() / 1024);
    return !ec;
}

bool load_from(int slot, R3000* c, uint32_t* pc) {
    std::string path = slot_path(slot);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    // check the header before touching the machine, so a foreign file changes nothing
    {
        StateIO s(data);
        uint32_t magic = 0, version = 0, hash = 0;
        s(magic); s(version); s(hash);
        if (!s.ok() || magic != kMagic || version != kVersion || hash != main_exe_hash) {
            log_msg(LogCat::Core, "%s is not a compatible save state", path.c_str());
            return false;
        }
    }
    StateIO s(data);
    serialize(s, c, *pc);
    if (!s.ok()) {
        log_msg(LogCat::Core, "%s is damaged; the game state may be inconsistent", path.c_str());
        return false;
    }
    sched_refresh(c);
    return true;
}

}  // namespace

// Loading replaces the whole machine and abandons whatever is executing, so unlike saving
// it needs no safe point: it is also polled once per frame (the game does not call VSync
// while it loads a level from the disc).
void state_poll_load(R3000* c) {
    if (g_load_request < 0) return;
    int slot = g_load_request;
    g_load_request = -1;
    uint32_t resume_pc = 0;
    if (!load_from(slot, c, &resume_pc)) {
        host_toast("Could not load that save");
        return;
    }
    memcard(0).flush();
    host_audio_clear();
    host_toast(slot == kAutoSlot ? "Auto-save loaded" : "State " + std::to_string(slot) + " loaded");
    g_resume_pc = resume_pc;
    g_resume_pending = true;
    // the loaded game's progress point is where auto-saves continue from
    g_progress_pending = false;
    g_demo_loaded = false;
    read_progress(&g_saved_level, &g_saved_restart);
    smooth_reset();
    std::longjmp(g_resume_jmp, 1);  // unwind the native stack; main() resumes
}

// Called at the entry of VSync (see SAFEPOINT_FUNCS in tools/recomp/emit.py).
extern "C" void psx_safepoint(R3000* c, uint32_t pc) {
    // VSync can also be reached from an interrupt callback; the interrupted context then
    // lives in native frames a state cannot capture, so wait for the main loop's VSync
    if (bios_in_exception() || core_in_ghost()) return;
    uint64_t frame = host_frame_count();

    state_poll_load(c);

    if (g_save_request >= 0) {
        int slot = g_save_request;
        g_save_request = -1;
        bool ok = save_to(slot, c, pc);
        host_toast(ok ? "State " + std::to_string(slot) + " saved" : "Saving failed");
    }

    {  // Debug: APOC_TEST_CHECKPOINT=FRAME:NODE makes the restart point trigger node NODE (level 1:
       // 1 = start, 296 = Cat1, 52 = Trips) and records progress, as a checkpoint trigger does
        static const char* t = std::getenv("APOC_TEST_CHECKPOINT");
        static bool done = false;
        unsigned long long tf = 0;
        unsigned ti = 0;
        if (t && !done && std::sscanf(t, "%llu:%u", &tf, &ti) == 2 && frame >= tf) {
            done = true;
            const R3000 saved = *c;
            MEM_W32(c, 0x800FF620, ti);
            c->r[31] = 0;
            call_indirect(c, 0x800189EC);
            *c = saved;
        }
    }
    // Auto-save a moment after the game records a new progress point (once any level
    // loading has finished), never in attract-mode demos.
    if (g_progress_pending && frame >= std::max(g_progress_frame, g_last_load_frame) + 90) {
        g_progress_pending = false;
        std::string level, restart;
        read_progress(&level, &restart);
        if (g_demo_loaded || level.empty() || restart.empty()) return;
        if (g_saved_level.empty()) {  // the first point of this session (new game or loaded save)
            g_saved_level = level;
            g_saved_restart = restart;
            return;
        }
        if (level == g_saved_level && restart == g_saved_restart) return;  // respawn at the same point
        const bool new_level = level != g_saved_level;
        if (save_to(kAutoSlot, c, pc)) {
            host_toast(new_level ? "Level complete - auto-saved" : "Checkpoint - auto-saved");
            log_msg(LogCat::Core, "auto-save: %s %s", level.c_str(), restart.c_str());
            g_saved_level = level;
            g_saved_restart = restart;
        }
    }
}

extern "C" void psx_hook_progress(R3000* c) {
    if (core_in_ghost()) return;
    LOG(Core, "game records progress (ra=%08X)", c->r[31]);
    g_progress_pending = true;
    g_progress_frame = host_frame_count();
}

void state_request_save(int slot) { g_save_request = slot; }
void state_request_load(int slot) { g_load_request = slot; }

bool state_slot_exists(int slot) { return std::filesystem::exists(slot_path(slot)); }

std::string state_slot_label(int slot) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(slot_path(slot), ec);
    if (ec) return "empty";
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&tt));
    return buf;
}

extern "C" void psx_hook_level_start(void) { state_note_level_start(); }

void state_note_level_start() { smooth_reset(); }

void state_note_file_loaded(const std::string& name) {
    sim60_note_file(name);
    g_last_load_frame = host_frame_count();
    if (name.find(".dem") != std::string::npos || name.find(".DEM") != std::string::npos) g_demo_loaded = true;
    // level files (".trg" is loaded first for every level) start a new level context
    if (name.find("_t.trg") != std::string::npos || name.find("_T.TRG") != std::string::npos) g_demo_loaded = false;
}

bool state_take_resume(uint32_t* pc) {
    if (!g_resume_pending) return false;
    g_resume_pending = false;
    *pc = g_resume_pc;
    return true;
}

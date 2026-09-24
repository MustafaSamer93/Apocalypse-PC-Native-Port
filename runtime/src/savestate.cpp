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
constexpr uint64_t kAutosaveIntervalFrames = 60ull * 60 * 5;  // 5 minutes of play

int g_save_request = -1, g_load_request = -1;
bool g_resume_pending = false;
uint32_t g_resume_pc = 0;

// auto-save state
bool g_level_start_pending = false;
uint64_t g_level_start_frame = 0;
bool g_demo_loaded = false;          // a .dem file was loaded since the last level start
bool g_in_level = false;
uint64_t g_last_autosave_frame = 0;
uint64_t g_last_load_frame = 0;      // last time the game loaded a file

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
    // the loaded game is mid-play: keep the periodic auto-save going from here
    g_level_start_pending = false;
    g_demo_loaded = false;
    g_in_level = true;
    g_last_autosave_frame = host_frame_count();
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

    // Auto-save: once a level has started and finished loading (not for attract-mode
    // demos), then every few minutes of play.
    if (g_level_start_pending && frame >= std::max(g_level_start_frame, g_last_load_frame) + 90) {
        g_level_start_pending = false;
        if (!g_demo_loaded) {
            g_in_level = true;
            if (save_to(kAutoSlot, c, pc)) host_toast("Auto-saved");
            g_last_autosave_frame = frame;
        } else {
            g_in_level = false;
        }
    } else if (g_in_level && frame >= g_last_autosave_frame + kAutosaveIntervalFrames) {
        if (save_to(kAutoSlot, c, pc)) host_toast("Auto-saved");
        g_last_autosave_frame = frame;
    }
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

void state_note_level_start() {
    smooth_reset();
    g_level_start_pending = true;
    g_level_start_frame = host_frame_count();
}

void state_note_file_loaded(const std::string& name) {
    g_last_load_frame = host_frame_count();
    if (name.find(".dem") != std::string::npos || name.find(".DEM") != std::string::npos) g_demo_loaded = true;
    // level files (".trg" is loaded first for every level) start a new level context
    if (name.find("_t.trg") != std::string::npos || name.find("_T.TRG") != std::string::npos) {
        g_demo_loaded = false;
        g_in_level = false;
    }
}

bool state_take_resume(uint32_t* pc) {
    if (!g_resume_pending) return false;
    g_resume_pending = false;
    *pc = g_resume_pc;
    return true;
}

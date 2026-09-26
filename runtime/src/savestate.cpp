#include "savestate.h"

#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#include <SDL3/SDL_stdinc.h>

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
bool g_autosave = true;  // psx.h

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

// The auto-save's progress point ("sewr_t Re_Chute_Sewr"), kept next to it for the menus.
constexpr const char* kAutoInfo = "saves/autosave.txt";
std::string g_auto_info;
bool g_auto_info_read = false;

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

// Chapters: the state at each progress point the first time it is reached, in that order,
// as "saves/chapters/NN <level> <restart point>.state" (listed in the F1 menu).
constexpr int kChapterNew = kChapterBase - 1;  // the slot save_to writes a new chapter to
std::string g_new_chapter;

std::vector<std::filesystem::path> chapter_files() {
    std::vector<std::filesystem::path> v;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("saves/chapters", ec))
        if (e.path().extension() == ".state") v.push_back(e.path());
    std::sort(v.begin(), v.end());
    return v;
}

std::string slot_path(int slot) {
    std::filesystem::create_directories("saves");
    if (slot == kChapterNew) return g_new_chapter;
    if (slot >= kChapterBase) {
        auto v = chapter_files();
        return (size_t)(slot - kChapterBase) < v.size() ? v[slot - kChapterBase].string() : "saves/chapters/none";
    }
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

// Keeps the state as a chapter unless this progress point already has one.
void save_chapter(const std::string& level, const std::string& restart, R3000* c, uint32_t pc);

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

void save_chapter(const std::string& level, const std::string& restart, R3000* c, uint32_t pc) {
    const std::string key = level + " " + restart;
    auto v = chapter_files();
    for (const auto& f : v)
        if (f.stem().string().substr(3) == key) return;
    std::filesystem::create_directories("saves/chapters");
    char num[8];
    std::snprintf(num, sizeof num, "%02zu ", v.size() + 1);
    g_new_chapter = "saves/chapters/" + std::string(num) + key + ".state";
    save_to(kChapterNew, c, pc);
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
    host_toast(slot == kAutoSlot       ? "Auto-save loaded"
               : slot >= kChapterBase ? "Chapter " + std::to_string(slot - kChapterBase + 1) + " loaded"
                                      : "State " + std::to_string(slot) + " loaded");
    g_resume_pc = resume_pc;
    g_resume_pending = true;
    // the loaded game's progress point is where auto-saves continue from
    g_progress_pending = false;
    g_demo_loaded = false;
    read_progress(&g_saved_level, &g_saved_restart);
    smooth_reset();
    game_menu_reset(true);
    std::longjmp(g_resume_jmp, 1);  // unwind the native stack; main() resumes
}

// Called at the entry of VSync (see SAFEPOINT_FUNCS in tools/recomp/emit.py).
extern "C" void psx_safepoint(R3000* c, uint32_t pc) {
    // VSync can also be reached from an interrupt callback; the interrupted context then
    // lives in native frames a state cannot capture, so wait for the main loop's VSync
    if (bios_in_exception() || core_in_ghost()) return;
    uint64_t frame = host_frame_count();

    state_poll_load(c);
    subtitle_poll(c);
    game_voice_poll(c);
    cheats_frame(c);

    if (g_save_request >= 0) {
        int slot = g_save_request;
        g_save_request = -1;
        bool ok = save_to(slot, c, pc);
        host_toast(ok ? "State " + std::to_string(slot) + " saved" : "Saving failed");
    }

    {  // Debug: APOC_TEST_CHECKPOINT=FRAME:NODE[:respawn] makes the restart point trigger node NODE
       // and records progress, as a checkpoint trigger does. With ":respawn" the player then
       // respawns there (as after a death, without losing a life), which creates that chapter.
       // Level 1 restart nodes: 1 Start, 296 Cat1, 867 Cat1andhalf, 410 Cell, 52 Trips,
       // 495 Cat2, 1467 Cat2andhalf, 558 Prisoners, 739 Tank (type-8 nodes in prsn_t.trg).
       // Level 2 (sewr_t.trg), in play order: 1 Start, 54 CollectingTunnel, 159 FlameJets,
       // 207 Cavern2, 1062 Cavern3, 285 Rat, 740 Tunnel02, 1038 Chute, 1515 Croc.
       // Level 3 (city_t.trg): 0 Start, 79 City2, 150 City3, 217 City4, 239 City5, 318 City6,
       // 319 City7, 338 City8, 346 City8a, 615 City10; level 3a (death_t.trg): 0 Start.
       // Level 4 (roof_t.trg): 1183 Roof_Start, 56 Roof5, 590 Heli, 625 HeliAfter, 368 Roof12,
       // 515 Roof18, 543 Roof24, 674 Roof24a, 589 Roof25, 1379 Heli_Boss2, 914 De_Heli_Boss.
        static const char* t = std::getenv("APOC_TEST_CHECKPOINT");
        static bool done = false;
        unsigned long long tf = 0;
        unsigned ti = 0;
        char opt[16] = "";
        if (t && !done && std::sscanf(t, "%llu:%u:%15s", &tf, &ti, opt) >= 2 && frame >= tf) {
            done = true;
            const R3000 saved = *c;
            MEM_W32(c, 0x800FF620, ti);
            c->r[31] = 0;
            call_indirect(c, 0x800189EC);
            *c = saved;
            // the level loop (80030CB4) respawns at the recorded point once player flag
            // +0x4E bit 0x40 (death finished) is set; that costs a life (0x800FF874), so add one
            if (uint32_t player = MEM_R32(c, 0x800FF5A0); player && std::strcmp(opt, "respawn") == 0) {
                MEM_W32(c, 0x800FF874, MEM_R32(c, 0x800FF874) + 1);
                MEM_W16(c, player + 0x4E, MEM_R16(c, player + 0x4E) | 0x40);
            }
        }
    }
    {  // Debug: APOC_TEST_WARP=FRAME:LEVEL:RESTART (e.g. 200:city_t:Re_Start_City) starts another
       // level: it becomes the recorded progress point and the level loop's state is set to 3,
       // which loads the level of the progress point (80031424) as "continue" does. The first
       // point reached there is kept as a chapter; APOC_TEST_CHECKPOINT then warps within it.
        static const char* t = std::getenv("APOC_TEST_WARP");
        static bool done = false;
        unsigned long long tf = 0;
        char lv[16] = "", rs[32] = "";
        if (t && !done && std::sscanf(t, "%llu:%15[^:]:%31s", &tf, lv, rs) == 3 && frame >= tf) {
            done = true;
            for (uint32_t i = 0; i < 9; i++) MEM_W8(c, kProgressLevel + i, i < std::strlen(lv) ? (uint8_t)lv[i] : 0);
            for (uint32_t i = 0; i < 24; i++) MEM_W8(c, kProgressRestart + i, i < std::strlen(rs) ? (uint8_t)rs[i] : 0);
            MEM_W32(c, 0x800FEEBC + 0x430, 3);
        }
    }
    {  // Debug: APOC_TEST_SUBTITLE=FRAME shows a subtitle for 60 seconds from FRAME (timing tests)
        static const char* t = std::getenv("APOC_TEST_SUBTITLE");
        static bool done = false;
        if (t && !done && frame >= std::strtoull(t, nullptr, 10)) {
            done = true;
            game_subtitle(c, "Kill them all, let God sort them out!", 60.0f);
        }
    }
    {  // Debug: APOC_TEST_PICKUP=FRAME:N gives weapon pickup N (1-7) or, with 0, Extra Life's message
        static const char* t = std::getenv("APOC_TEST_PICKUP");
        static bool done = false;
        unsigned long long tf = 0;
        int what = 0;
        if (t && !done && std::sscanf(t, "%llu:%d", &tf, &what) == 2 && frame >= tf) {
            done = true;
            game_test_pickup(c, what);
        }
    }
    // Auto-save a moment after the game records a new progress point (once any level
    // loading has finished), never in attract-mode demos.
    if (g_progress_pending && frame >= std::max(g_progress_frame, g_last_load_frame) + 90) {
        g_progress_pending = false;
        std::string level, restart;
        read_progress(&level, &restart);
        if (g_demo_loaded || level.empty() || restart.empty()) return;
        save_chapter(level, restart, c, pc);
        if (g_saved_level.empty()) {  // the first point of this session (new game or loaded save)
            g_saved_level = level;
            g_saved_restart = restart;
            return;
        }
        if (level == g_saved_level && restart == g_saved_restart) return;  // respawn at the same point
        if (!g_autosave) return;  // switched off (Options -> Enhancements -> Autosave)
        if (save_to(kAutoSlot, c, pc)) {
            log_msg(LogCat::Core, "auto-save: %s %s", level.c_str(), restart.c_str());
            g_saved_level = level;
            g_saved_restart = restart;
            g_auto_info = level + " " + restart;
            g_auto_info_read = true;
            if (FILE* f = std::fopen(kAutoInfo, "w")) {
                std::fprintf(f, "%s\n", g_auto_info.c_str());
                std::fclose(f);
            }
            game_message(c, "Auto Saved");  // as the game shows a pickup (game_menu.cpp)
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

std::vector<std::string> state_chapter_names() {
    std::vector<std::string> names;
    for (const auto& f : chapter_files()) {
        // "03 prsn_t Re_Cat1_Prsn" -> "3: Cat1 (prsn)"
        std::string n = f.stem().string(), level = n.substr(3, n.find(' ', 3) - 3), point = n.substr(4 + level.size());
        if (level.size() > 2 && level.compare(level.size() - 2, 2, "_t") == 0) level.resize(level.size() - 2);
        if (point.compare(0, 3, "Re_") == 0) point.erase(0, 3);
        if (size_t u = point.rfind('_'); u != std::string::npos && SDL_strcasecmp(point.c_str() + u + 1, level.c_str()) == 0)
            point.resize(u);
        names.push_back(std::to_string(names.size() + 1) + ": " + point + " (" + level + ")");
    }
    return names;
}

std::string state_autosave_level() {
    if (!g_auto_info_read) {
        g_auto_info_read = true;
        if (FILE* f = std::fopen(kAutoInfo, "r")) {
            char line[128] = "";
            if (std::fgets(line, sizeof line, f)) g_auto_info = line;
            std::fclose(f);
        }
    }
    // level file name -> the name the game's own saves use
    static const struct { const char* file; const char* name; } kLevels[] = {
        {"prsn", "Prison"}, {"sewr", "Sewers"},   {"city", "City"},   {"death", "Death"},
        {"roof", "Rooftops"}, {"grav", "Graveyard"}, {"pest", "Plague"}, {"tube", "War Factory"},
        {"war", "War"},       {"wh", "White House"}, {"bst", "The Beast"},
    };
    const std::string file = g_auto_info.substr(0, g_auto_info.find('_'));
    for (const auto& l : kLevels)
        if (file == l.file) return l.name;
    return "";  // a save from before the level was recorded
}

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
    game_menu_reset(false);
}

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

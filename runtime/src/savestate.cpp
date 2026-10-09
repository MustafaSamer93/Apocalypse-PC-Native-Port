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
#include <SDL3/SDL_timer.h>

#include "memcard.h"
#include "psx.h"
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
// Boss checkpoint: when the boss (the one the health bar shows, gp+2224) is down to half its
// health (its second phase) the game is saved to kBossSlot; dying while it still lives loads
// that (with the lives left) instead of starting the fight again.
constexpr int kBossSlot = 98;
constexpr uint32_t kBoss = 0x800FF76C;
uint32_t g_boss = 0;
int g_boss_full = 0;
bool g_boss_saved = false;
int g_after_load_lives = -1;  // set the lives to this after the next state load
bool g_boss_reloaded = false;  // the checkpoint was just loaded: the same fight goes on
std::string g_start_level;  // the level of the first progress point seen in it, and its record
uint8_t g_start_record[136];
uint8_t g_restart_record[136];  // the record a game over goes back to (state_poll_restart)
uint64_t g_restart_at = 0;    // frame to load it at (0 = none)
constexpr uint32_t kDifficulty = 0x800FF384, kStartLives = 0x800EC522;  // 0 easy 1 normal 2 hard; the game's starting lives
constexpr uint32_t kLives = 0x800FF874, kRecordLives = 0x6C;  // (the death state keeps lives at record +0x6C)
uint64_t g_progress_frame = 0;
std::string g_saved_level, g_saved_restart;  // the point of the last auto-save (or the start)
bool g_demo_loaded = false;          // a .dem file was loaded since the last level start
uint64_t g_last_load_frame = 0;      // last time the game loaded a file

// The auto-save is the game's own save record, the one a memory card save holds: the progress
// point the game recorded (level, restart point, health, lives, weapons, options; word 0 = the
// XOR of the other 33 words). It is loaded as the game loads a card save (game_menu.cpp) or, in a
// level, as the game continues from its progress point (state 3 of the level loop, 80031424).
constexpr uint32_t kSaveRecord = 0x800A53D4;
constexpr const char* kAutoFile = "saves/autosave.sav";
bool g_autoload_request = false;

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
    return slot == kAutoSlot ? kAutoFile : "saves/slot" + std::to_string(slot) + ".state";
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

bool record_ok(const uint8_t* r) {
    uint32_t w[kSaveRecordSize / 4], x = 0;
    std::memcpy(w, r, sizeof w);
    for (size_t i = 1; i < std::size(w); i++) x ^= w[i];
    return w[0] == (x | 1) && r[4] != 0;  // the game's checksum (80018B44): XOR, bit 0 set; and a level name
}

void record_fix(uint8_t* r) {  // word 0 = XOR of words 1..33 with bit 0 set (80018B44)
    uint32_t w[kSaveRecordSize / 4], x = 0;
    std::memcpy(w, r, sizeof w);
    for (size_t i = 1; i < std::size(w); i++) x ^= w[i];
    x |= 1;
    std::memcpy(r, &x, 4);
}
void record_name(uint8_t* r, uint32_t addr, size_t n, const char* s) {  // a name field (RAM address)
    std::memset(r + (addr - kSaveRecord), 0, n);
    std::memcpy(r + (addr - kSaveRecord), s, std::min(n - 1, std::strlen(s)));
}

// The auto-save: the save record the game has just made for its progress point.
bool save_record(R3000* c) {
    uint8_t r[kSaveRecordSize];
    for (uint32_t i = 0; i < kSaveRecordSize; i++) r[i] = (uint8_t)MEM_R8(c, kSaveRecord + i);
    if (!record_ok(r)) return false;
    std::filesystem::create_directories("saves");
    const std::string tmp = std::string(kAutoFile) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(r, 1, sizeof r, f) == sizeof r;
    std::fclose(f);
    if (!ok) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, kAutoFile, ec);
    if (ec) {
        std::filesystem::remove(kAutoFile, ec);
        std::filesystem::rename(tmp, kAutoFile, ec);
    }
    return !ec;
}

// Loads the auto-save in a level: it becomes the game's progress point and the level loop
// continues from it (state 3: loads that level and restores the record, as after a Continue).
void load_record_in_level(R3000* c) {
    uint8_t r[kSaveRecordSize];
    if (!state_read_autosave(r)) {
        host_toast("Could not load the auto-save");
        return;
    }
    for (uint32_t i = 0; i < kSaveRecordSize; i++) MEM_W8(c, kSaveRecord + i, r[i]);
    MEM_W32(c, 0x800FEEBC + 0x430, 3);
    g_progress_pending = false;
    read_progress(&g_saved_level, &g_saved_restart);
    host_toast("Auto-save loaded");
}

}  // namespace

bool state_read_autosave(uint8_t* record) {
    FILE* f = std::fopen(kAutoFile, "rb");
    if (!f) return false;
    uint8_t r[kSaveRecordSize + 1];
    const size_t n = std::fread(r, 1, sizeof r, f);
    std::fclose(f);
    if (n != kSaveRecordSize || !record_ok(r)) return false;
    std::memcpy(record, r, kSaveRecordSize);
    return true;
}

uint64_t g_level_render_frame = 0;

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
    if (g_after_load_lives >= 0) {  // the boss checkpoint after a death
        MEM_W32(c, 0x800FF874, (uint32_t)g_after_load_lives);
        g_after_load_lives = -1;
        g_boss_reloaded = true;
        host_toast("Boss checkpoint");
    }
    texreplace_vram_changed();  // (VRAM came from the state)
    g_resume_pc = resume_pc;
    g_resume_pending = true;
    // the loaded game's progress point is where auto-saves continue from
    g_progress_pending = false;
    g_demo_loaded = false;
    read_progress(&g_saved_level, &g_saved_restart);
    game_menu_reset(true);
    std::longjmp(g_resume_jmp, 1);  // unwind the native stack; main() resumes
}

// Called at the entry of VSync (see SAFEPOINT_FUNCS in tools/recomp/emit.py).
void state_poll_restart(R3000* c);

extern "C" void psx_safepoint(R3000* c, uint32_t pc) {
    // VSync can also be reached from an interrupt callback; the interrupted context then
    // lives in native frames a state cannot capture, so wait for the main loop's VSync
    if (bios_in_exception() || core_in_ghost()) return;
    uint64_t frame = host_frame_count();

    state_poll_load(c);
    state_poll_restart(c);
    if (g_autoload_request) {  // F1 -> Load auto-save
        g_autoload_request = false;
        if (frame < g_level_render_frame + 30) load_record_in_level(c);
        else host_toast("Load the auto-save from Load Game");
    }
    subtitle_poll(c);
    game_voice_poll(c);
    cheats_frame(c);

    {  // boss checkpoint (see kBossSlot)
        const uint32_t boss = MEM_R32(c, kBoss), player = MEM_R32(c, 0x800FF5A0);
        const int hp = boss ? (int16_t)MEM_R16(c, boss + 0xDA) : 0;
        static int last_hp = 0;
        // another boss, or this one back at more health (the fight started again, a state loaded)
        if (g_boss_reloaded) g_boss_reloaded = false;
        else if (boss != g_boss || hp > last_hp) g_boss = boss, g_boss_full = hp, g_boss_saved = false;
        last_hp = hp;
        const bool alive = player && (int16_t)MEM_R16(c, player + 0xDA) > 0;
        if (boss && !g_boss_saved && alive && hp > 0 && hp * 2 <= g_boss_full) {
            g_boss_saved = save_to(kBossSlot, c, pc);
            log_msg(LogCat::Core, "boss checkpoint: boss %08X at %d of %d health", boss, hp, g_boss_full);
            if (g_boss_saved) game_message(c, "Checkpoint");
        }
    }

    if (g_save_request >= 0) {
        int slot = g_save_request;
        g_save_request = -1;
        bool ok = save_to(slot, c, pc);
        host_toast(ok ? "State " + std::to_string(slot) + " saved" : "Saving failed");
    }

    // Auto-save a moment after the game records a new progress point (once any level
    // loading has finished), never in attract-mode demos.
    // (and not while the player is dead, health +0xDA = 0: a checkpoint recorded before a
    // respawn is kept once the player is back)
    const uint32_t player = MEM_R32(c, 0x800FF5A0);
    const bool dead = player && MEM_R16(c, player + 0xDA) == 0;
    if (g_progress_pending && !dead && frame >= std::max(g_progress_frame, g_last_load_frame) + 90) {
        g_progress_pending = false;
        std::string level, restart;
        read_progress(&level, &restart);
        if (g_demo_loaded || level.empty() || restart.empty()) return;
        log_msg(LogCat::Core, "checkpoint: %s %s", level.c_str(), restart.c_str());
        if (level != g_start_level) {  // the level's first point: where a game over starts it again
            g_start_level = level;
            for (uint32_t i = 0; i < kSaveRecordSize; i++) g_start_record[i] = (uint8_t)MEM_R8(c, kSaveRecord + i);
        }
        save_chapter(level, restart, c, pc);
        if (g_saved_level.empty()) {  // the first point of this session (new game or loaded save)
            g_saved_level = level;
            g_saved_restart = restart;
            return;
        }
        if (level == g_saved_level && restart == g_saved_restart) return;  // respawn at the same point
        if (!g_autosave) return;  // switched off (Options -> Enhancements -> Autosave)
        if (save_record(c)) {
            log_msg(LogCat::Core, "auto-save: %s %s", level.c_str(), restart.c_str());
            g_saved_level = level;
            g_saved_restart = restart;
            game_message(c, "Auto Saved");  // as the game shows a pickup (game_menu.cpp)
        }
    }
}

// Game over (80031264, the death state: at 800312A0 v1 = lives left, 0 = game over): the level
// starts again from its first point with what the player had there (lives, health, weapons) -
// its record goes back into the progress record and the game takes its own respawn path.
extern "C" void psx_hook_game_over(R3000* c) {
    if (core_in_ghost()) return;
    {
        std::string level, restart;
        read_progress(&level, &restart);
        log_msg(LogCat::Core, "player died in %s (last checkpoint %s): %u lives left", level.c_str(), restart.c_str(), c->r[3]);
    }
    // a death in a boss fight past its checkpoint: back there, with the lives left
    const uint32_t boss = MEM_R32(c, kBoss);
    if (c->r[3] != 0 && g_boss_saved && boss == g_boss && (int16_t)MEM_R16(c, boss + 0xDA) > 0) {
        g_after_load_lives = (int)c->r[3];
        g_load_request = kBossSlot;
        log_msg(LogCat::Core, "death in the boss fight: back to its checkpoint with %u lives", c->r[3]);
        return;
    }
    if (c->r[3] != 0) return;
    // Game over: the player respawns here as for a death with a life left, then
    // state_poll_restart loads a level start through the game's continue path (level state 3, as
    // F1 -> Load auto-save; the respawn itself only goes to the current checkpoint).
    uint8_t rec[kSaveRecordSize];
    uint32_t lives;
    if (MEM_R32(c, kDifficulty) == 2) {
        // Hard: the start of the game with a new game's loadout; the current record for the
        // options it also holds
        for (uint32_t i = 0; i < kSaveRecordSize; i++) rec[i] = (uint8_t)MEM_R8(c, kSaveRecord + i);
        record_name(rec, kProgressLevel, 9, "prsn_t");
        record_name(rec, kProgressRestart, 24, "Re_Start_Prsn");
        lives = std::clamp<uint32_t>((uint32_t)(int16_t)MEM_R16(c, kStartLives), 1, 99);
        // a new game's (chapter 1 state): the gun only (1000 rounds), health 100, 3 smart bombs
        rec[66] = 1;
        rec[67] = 0;
        const uint32_t words[][2] = {{68, 1000}, {72, 0}, {76, 0}, {80, 0}, {84, 0}, {88, 0}, {92, 0}, {96, 0},
                                     {100, 100}, {104, 3}, {108, lives}};
        for (const auto& w : words) std::memcpy(rec + w[0], &w[1], 4);
    } else {
        // Easy / normal: this level's start with what the player had when it began
        if (g_start_level.empty()) {  // no point recorded since a save state was loaded: the current record
            std::string restart;
            read_progress(&g_start_level, &restart);
            for (uint32_t i = 0; i < kSaveRecordSize; i++) g_start_record[i] = (uint8_t)MEM_R8(c, kSaveRecord + i);
        }
        if (g_start_level.empty() || !record_ok(g_start_record)) return;
        std::memcpy(rec, g_start_record, sizeof rec);
        static const char* const kStarts[][2] = {
            {"prsn_t", "Re_Start_Prsn"}, {"sewr_t", "Re_Start_Sewer"}, {"city_t", "Re_Start_City"},
            {"death_t", "Re_Start_Death"}, {"roof_t", "Re_Roof_Start"}, {"grav_t", "Re_Grav_Start"},
            {"pest_t", "Re_Start_Pest"}, {"tube_t", "Re_Start_Tube"}, {"war_t", "Re_Start_War"},
            {"wh_t", "Re_WH_Start"}, {"bst_t", "Re_Bst_Start"}};
        for (const auto& st : kStarts)
            if (g_start_level == st[0]) record_name(rec, kProgressRestart, 24, st[1]);
        std::memcpy(&lives, rec + kRecordLives, 4);
        lives = std::clamp(lives, 1u, 99u);
    }
    record_fix(rec);
    std::memcpy(g_restart_record, rec, sizeof rec);
    g_restart_at = host_frame_count() + 30;
    MEM_W32(c, kLives, 1);
    c->r[3] = 1;
    g_progress_pending = false;
    log_msg(LogCat::Core, "game over: %s with %u lives", MEM_R32(c, kDifficulty) == 2 ? "hard, back to the start of the game"
                                                                                     : ("back to the start of " + g_start_level).c_str(), lives);
}

// Game over, a moment later: the restart record becomes the progress point and the level loop's
// state 3 loads its level (the game's continue picture waits for X first).
void state_poll_restart(R3000* c) {
    // once the respawn has finished (its section loaded, the level drawing, the player alive):
    // a state set while it runs is lost
    const uint64_t frame = host_frame_count();
    const uint32_t player = MEM_R32(c, 0x800FF5A0);
    if (!g_restart_at || frame < g_restart_at || core_in_ghost() || frame > g_level_render_frame + 2 || !player ||
        (int16_t)MEM_R16(c, player + 0xDA) <= 0)
        return;
    g_restart_at = 0;
    log_msg(LogCat::Core, "game over: loading %s %s", (const char*)g_restart_record + 4, (const char*)g_restart_record + 0xD);
    for (uint32_t i = 0; i < kSaveRecordSize; i++) MEM_W8(c, kSaveRecord + i, g_restart_record[i]);
    MEM_W32(c, 0x800FEEBC + 0x430, 3);
    g_progress_pending = false;
    g_start_level.clear();  // the level records its start point again when it loads
}

// Back at the main menu: the next level played (new game, load, level select) records its own start.
void state_note_main_menu() { g_start_level.clear(); }

extern "C" void psx_hook_progress(R3000* c) {
    if (core_in_ghost()) return;
    LOG(Core, "game records progress (ra=%08X)", c->r[31]);
    g_progress_pending = true;
    g_progress_frame = host_frame_count();
}

void state_request_save(int slot) { g_save_request = slot; }
void state_request_load(int slot) {
    if (slot == kAutoSlot) g_autoload_request = true;  // not a state: the game's save record
    else g_load_request = slot;
}

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
    uint8_t r[kSaveRecordSize];
    if (!state_read_autosave(r)) return "";
    std::string file;  // "sewr" of "sewr_t"
    for (uint32_t i = kProgressLevel - kSaveRecord; i < kSaveRecordSize && r[i] && r[i] != '_'; i++) file += (char)r[i];
    // level file name -> the name the game's own saves use
    static const struct { const char* file; const char* name; } kLevels[] = {
        {"prsn", "Prison"}, {"sewr", "Sewers"},   {"city", "City"},   {"death", "Death"},
        {"roof", "Rooftops"}, {"grav", "Graveyard"}, {"pest", "Plague"}, {"tube", "War Factory"},
        {"war", "War"},       {"wh", "White House"}, {"bst", "The Beast"},
    };
    for (const auto& l : kLevels)
        if (file == l.file) return l.name;
    return "";
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
    game_menu_reset(false);
}

void state_note_file_loaded(const std::string& name) {
    sim60_note_file(name);
    g_last_load_frame = host_frame_count();
    if (name.find(".dem") != std::string::npos || name.find(".DEM") != std::string::npos) g_demo_loaded = true;
    // level files (".trg" is loaded first for every level) start a new level context
    if (name.find("_t.trg") != std::string::npos || name.find("_T.TRG") != std::string::npos) {
        g_demo_loaded = false;
        const size_t a = name.find_first_not_of(" \r\n\t"), b = name.find_last_not_of(" \r\n\t");
        log_msg(LogCat::Core, "level loading: %s", a == std::string::npos ? "" : name.substr(a, b - a + 1).c_str());
    }
}

bool state_take_resume(uint32_t* pc) {
    if (!g_resume_pending) return false;
    g_resume_pending = false;
    *pc = g_resume_pc;
    return true;
}

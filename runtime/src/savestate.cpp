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
    return w[0] == x && r[4] != 0;  // (a level name)
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
            } else if (player && std::strcmp(opt, "kill") == 0) {
                // ":kill": the player dies as in a kill zone (8005E708) and respawns there after
                // the death animation (levels 5-11: the flag above alone did not respawn)
                MEM_W32(c, 0x800FF874, MEM_R32(c, 0x800FF874) + 1);
                const R3000 keep = *c;
                c->r[4] = player;
                c->r[31] = 0;
                call_indirect(c, 0x8005E708);
                *c = keep;
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
    {  // Debug: APOC_TEST_TELEPORT=FRAME:X:Y:Z puts the player at X Y Z (e.g. on a trigger)
        static const char* t = std::getenv("APOC_TEST_TELEPORT");
        static bool done = false;
        unsigned long long tf = 0;
        int x = 0, y = 0, z = 0;
        if (t && !done && std::sscanf(t, "%llu:%d:%d:%d", &tf, &x, &y, &z) == 4 && frame >= tf) {
            done = true;
            if (uint32_t player = MEM_R32(c, 0x800FF5A0)) {
                MEM_W32(c, player + 4, (uint32_t)x);
                MEM_W32(c, player + 8, (uint32_t)y);
                MEM_W32(c, player + 12, (uint32_t)z);
            }
        }
    }
    {  // Debug: APOC_TEST_SAVE=FRAME saves state slot 5
        static const char* t = std::getenv("APOC_TEST_SAVE");
        static bool done = false;
        if (t && !done && frame >= std::strtoull(t, nullptr, 10)) {
            done = true;
            g_save_request = 5;
        }
    }
    {  // Debug: APOC_TEST_AUTOLOAD=FRAME loads the auto-save as F1 -> Load auto-save does
        static const char* t = std::getenv("APOC_TEST_AUTOLOAD");
        static bool done = false;
        if (t && !done && frame >= std::strtoull(t, nullptr, 10)) {
            done = true;
            state_request_load(kAutoSlot);
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
    {  // Debug: APOC_TEST_BOSSHP=FRAME:HP sets the boss's health (boss at gp+2224, health +0xDA)
        static const char* t = std::getenv("APOC_TEST_BOSSHP");
        static bool done = false;
        unsigned long long tf = 0;
        int hp = 0;
        if (t && !done && std::sscanf(t, "%llu:%d", &tf, &hp) == 2 && frame >= tf) {
            const uint32_t boss = MEM_R32(c, 0x800FF76C);
            if (boss) MEM_W16(c, boss + 0xDA, (uint16_t)hp), done = true;
            LOG(Host, "boss %08X health set to %d at frame %llu", boss, hp, frame);
        }
    }
    if (static const char* t = std::getenv("APOC_TEST_MESSAGE"); t && frame == std::strtoull(t, nullptr, 10))
        game_message(c, "Auto Saved");  // Debug: APOC_TEST_MESSAGE=FRAME shows the auto-save message there
    if (static const char* t = std::getenv("APOC_RAW_DUMP"); t) {  // Debug: APOC_RAW_DUMP=FRAME:PREFIX -> PREFIX.ram / PREFIX.vram
        static bool done = false;
        unsigned long long tf = 0;
        char prefix[400] = "";
        if (!done && std::sscanf(t, "%llu:%399s", &tf, prefix) == 2 && frame >= tf) {
            done = true;
            if (FILE* f = std::fopen((std::string(prefix) + ".ram").c_str(), "wb")) std::fwrite(psx_ram, 1, PSX_RAM_SIZE, f), std::fclose(f);
            if (FILE* f = std::fopen((std::string(prefix) + ".vram").c_str(), "wb")) std::fwrite(gpu_display().vram, 2, 1024 * 512, f), std::fclose(f);
        }
    }
    if (static const char* t = std::getenv("APOC_TEST_FREEZE"); t && frame == std::strtoull(t, nullptr, 10))
        for (;;) SDL_Delay(100);  // Debug: APOC_TEST_FREEZE=FRAME hangs there (the freeze report, core.cpp)
    {  // Debug: APOC_TEST_GAMEOVER=FRAME[:LIVES]: the player dies there with LIVES (default 1: the last)
        static const char* t = std::getenv("APOC_TEST_GAMEOVER");
        static bool done = false;
        unsigned long long tf = 0;
        unsigned lives = 1;
        if (t && !done && std::sscanf(t, "%llu:%u", &tf, &lives) >= 1 && frame >= tf) {
            done = true;
            if (uint32_t player = MEM_R32(c, 0x800FF5A0)) {
                MEM_W32(c, kLives, lives);
                const R3000 keep = *c;
                c->r[4] = player;
                c->r[31] = 0;
                call_indirect(c, 0x8005E708);
                *c = keep;
            }
        }
    }
    {  // Debug: APOC_TEST_HEALTH=FRAME:PERIOD:COUNT health pickups (+20) every PERIOD frames from FRAME
        static const char* t = std::getenv("APOC_TEST_HEALTH");
        static int left = -1;
        static unsigned long long tf = 0, period = 0;
        if (t && left < 0 && std::sscanf(t, "%llu:%llu:%d", &tf, &period, &left) != 3) left = 0;
        if (left > 0 && frame >= tf) {
            left--;
            tf += period;
            LOG(Host, "test health pickup at frame %llu", frame);
            game_test_health(c, 20);
        }
    }
    {  // Debug: APOC_TEST_GRAB=FRAME:PERIOD:COUNT moves the player onto health pickups
        static const char* t = std::getenv("APOC_TEST_GRAB");
        static int left = -1;
        static unsigned long long tf = 0, period = 0;
        if (t && left < 0 && std::sscanf(t, "%llu:%llu:%d", &tf, &period, &left) != 3) left = 0;
        if (left > 0 && frame >= tf) {
            left--;
            tf += period;
            game_test_grab(c);
        }
    }
    {  // Debug: APOC_TEST_SPAWN=FRAME:PERIOD:T1,T2,... drops pickups of those types at the player
        static const char* t = std::getenv("APOC_TEST_SPAWN");
        static unsigned long long tf = 0, period = 0;
        static int n = 0;
        static const char* list = t && std::sscanf(t, "%llu:%llu:%n", &tf, &period, &n) >= 2 && n ? t + n : nullptr;
        if (t) game_test_spawn_poll(c);
        if (list && *list && frame >= tf) {
            tf += period;
            game_test_spawn(c, std::atoi(list));
            list = std::strchr(list, ',');
            list = list ? list + 1 : nullptr;
        }
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
    // a death in a boss fight past its checkpoint: back there, with the lives left
    const uint32_t boss = MEM_R32(c, kBoss);
    if (c->r[3] != 0 && g_boss_saved && boss == g_boss && (int16_t)MEM_R16(c, boss + 0xDA) > 0) {
        g_after_load_lives = (int)c->r[3];
        g_load_request = kBossSlot;
        log_msg(LogCat::Core, "death in the boss fight: back to its checkpoint with %u lives", c->r[3]);
        return;
    }
    if (c->r[3] != 0 || g_start_level.empty() || core_in_ghost() || !record_ok(g_start_record)) return;
    for (uint32_t i = 0; i < kSaveRecordSize; i++) MEM_W8(c, kSaveRecord + i, g_start_record[i]);
    uint32_t lives = 0;
    std::memcpy(&lives, g_start_record + kRecordLives, 4);
    lives = std::clamp(lives, 1u, 99u);
    MEM_W32(c, kLives, lives);
    c->r[3] = lives;
    g_progress_pending = false;
    log_msg(LogCat::Core, "game over: %s starts again with %u lives", g_start_level.c_str(), lives);
}

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

// The whole machine in memory (bios.cpp: the PlayStation boot sequence runs on a scratch machine
// and the game then starts from this copy).
std::vector<uint8_t> state_capture(R3000* c, uint32_t pc) {
    StateIO s(false);
    serialize(s, c, pc);
    return std::move(s.data());
}

void state_restore(const std::vector<uint8_t>& data, R3000* c) {
    StateIO s(data);
    uint32_t pc;
    serialize(s, c, pc);
    sched_refresh(c);
}

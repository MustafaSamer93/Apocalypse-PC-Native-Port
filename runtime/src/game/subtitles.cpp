// Subtitles for the voice lines (APOC.XA): when the first sector of a clip plays, its line from
// runtime/data/subtitles.txt (built into the exe) is shown at the next frame, on a random side of
// the screen in the game's font, tilted (game_menu.cpp game_subtitle).
//
// The clip table is in the EXE (see tools/extract_voice.py): 22 block starts at 0x800A5F80 (in
// sectors of one channel) and 22 x 16 clip lengths at 0x800A5FD8 (-1 = none); clip "bBB_cCC"
// starts at APOC.XA sector start * 16 + channel.
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <string>

#include "core/psx.h"
#include "game/game.h"
#include "host/host.h"

bool g_subtitles = true;  // psx.h

namespace {

constexpr uint32_t kXaLba = 15704;  // APOC.XA on the (USA) disc
constexpr uint32_t kPosTable = 0x800A5F80, kLenTable = 0x800A5FD8;
constexpr int kBlocks = 22, kChannels = 16;
const char* const kText =
#include "subtitles.txt.h"
    ;

// Clips that are the same recording (byte-identical audio on the USA disc): the first one's
// line is used for the others when they have none of their own.
const char* const kSameAs[][2] = {
    {"b01_c02", "b01_c01"}, {"b01_c03", "b01_c01"}, {"b01_c04", "b01_c01"}, {"b01_c05", "b01_c01"},
    {"b01_c06", "b01_c01"}, {"b01_c07", "b01_c01"},
    {"b03_c06", "b03_c05"}, {"b03_c07", "b03_c05"},
    {"b04_c04", "b04_c03"}, {"b04_c05", "b04_c03"}, {"b04_c06", "b04_c03"}, {"b04_c07", "b04_c03"},
    {"b05_c07", "b05_c06"},
    {"b15_c14", "b15_c13"}, {"b15_c15", "b15_c13"},
};

// b05_c00 is also played from blocks 6, 7, 9, 10 and 11 (every channel 0-7).
std::string same_recording(const std::string& key) {
    for (const auto& p : kSameAs)
        if (key == p[0]) return p[1];
    const int b = std::atoi(key.c_str() + 1);
    if (b == 6 || b == 7 || b == 9 || b == 10 || b == 11) return "b05_c00";
    return "";
}

std::map<std::string, std::string> g_lines;  // "b14_c08" -> text
bool g_loaded = false;
int g_pending_block = -1, g_pending_chan = -1;

uint32_t ram32(uint32_t a) {
    const uint8_t* p = psx_ram + (a & (PSX_RAM_SIZE - 1));
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void load_lines() {
    if (g_loaded) return;
    g_loaded = true;
    for (const char* p = kText; *p;) {
        const char* e = std::strchr(p, '\n');
        std::string line(p, e ? (size_t)(e - p) : std::strlen(p));
        p = e ? e + 1 : p + line.size();
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        std::string key = trim(line.substr(0, eq)), text = trim(line.substr(eq + 1));
        // The game's font (char map 800E6908) has letters, digits, space and ! % + - . ' only;
        // anything else draws as its empty box. Commas and '?' are left out ("?!" -> "!", a lone
        // "?" -> "!"), '#' (censoring) becomes '-', and the rest goes.
        std::string shown;
        for (size_t i = 0; i < text.size(); i++) {
            const char ch = text[i];
            if (std::isalnum((unsigned char)ch) || std::strchr(" !%+-.'", ch)) shown += ch;
            else if (ch == '#') shown += '-';
            else if (ch == '?' && (i + 1 >= text.size() || text[i + 1] != '!')) shown += '!';
        }
        text = trim(shown);
        if (!key.empty() && !text.empty()) g_lines[key] = text;
    }
    log_msg(LogCat::Host, "subtitles: %zu lines", g_lines.size());
}

}  // namespace

// cdrom.cpp: an XA sector of `chan` at `lba` is being played.
void subtitle_note_xa(uint32_t lba, int chan) {
    if (lba < kXaLba) return;
    const uint32_t off = lba - kXaLba;
    if ((int)(off % kChannels) != chan) return;
    const int32_t pos = (int32_t)(off / kChannels);
    for (int b = 0; b < kBlocks; b++)
        if ((int32_t)ram32(kPosTable + 4 * b) == pos && (int32_t)ram32(kLenTable + 4 * (b * kChannels + chan)) > 0) {
            g_pending_block = b, g_pending_chan = chan;  // shown at the next frame (subtitle_poll)
            return;
        }
}

// The voice clip the XA sector of `chan` at `lba` belongs to: *end = the disc sector after its last.
bool voice_clip_end(uint32_t lba, int chan, uint32_t* end) {
    if (lba < kXaLba) return false;
    const uint32_t off = lba - kXaLba;
    if ((int)(off % kChannels) != chan) return false;
    const int32_t p = (int32_t)(off / kChannels);
    for (int b = 0; b < kBlocks; b++) {
        const int32_t pos = (int32_t)ram32(kPosTable + 4 * b), len = (int32_t)ram32(kLenTable + 4 * (b * kChannels + chan));
        if (len > 0 && p >= pos && p < pos + len) {
            *end = kXaLba + (uint32_t)(pos + len) * kChannels + (uint32_t)chan;
            return true;
        }
    }
    return false;
}

// savestate.cpp psx_safepoint (once per frame, in the game's main loop): shows a pending line.
void subtitle_poll(R3000* c) {
    if (g_subtitles) game_subtitle_keep(c);
    if (g_pending_block < 0) return;
    const int b = g_pending_block, ch = g_pending_chan;
    g_pending_block = g_pending_chan = -1;
    LOG(Host, "voice clip b%02d_c%02d at frame %llu", b, ch, (unsigned long long)host_frame_count());
    if (!g_subtitles) return;
    load_lines();
    char key[16];
    std::snprintf(key, sizeof key, "b%02d_c%02d", b, ch);
    auto it = g_lines.find(key);
    if (it == g_lines.end()) it = g_lines.find(same_recording(key));
    if (it == g_lines.end()) {
        LOG(Host, "subtitle: none for %s", key);
        return;
    }
    // the clip's length: sectors of 4032 samples at 37800 Hz (mono)
    const int sectors = (int)ram32(kLenTable + 4 * (b * kChannels + ch));
    LOG(Host, "subtitle: \"%s\"", it->second.c_str());
    game_subtitle(c, it->second.c_str(), (float)sectors * 4032.0f / 37800.0f);
}

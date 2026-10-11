// CD-ROM controller (0x1F801800-0x1F801803) backed by the user's .cue/.bin image,
// including XA-ADPCM decoding and CD-DA playback into the SPU's CD input.
#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/psx.h"
#include "core/state_io.h"
#include "game/game.h"
#include "host/host.h"

namespace {

constexpr int RAW = 2352;

// ---------------------------------------------------------------- disc image

struct Track {
    int number;
    bool audio;
    std::string file;
    uint32_t file_lba;     // first sector of this track's file on the disc (absolute LBA)
    uint32_t start_lba;    // INDEX 01 (absolute)
    uint32_t sectors;      // sectors in the file
    FILE* fp = nullptr;
};
std::vector<Track> g_tracks;
uint32_t g_disc_end = 0;

uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

void lba_to_msf(uint32_t lba, uint8_t* m, uint8_t* s, uint8_t* f) {
    lba += 150;
    *m = bcd((int)(lba / (75 * 60)));
    *s = bcd((int)((lba / 75) % 60));
    *f = bcd((int)(lba % 75));
}

bool read_raw(uint32_t lba, uint8_t* out) {
    for (auto& t : g_tracks) {
        if (lba >= t.file_lba && lba < t.file_lba + t.sectors) {
            std::fseek(t.fp, (long)((lba - t.file_lba) * (uint64_t)RAW), SEEK_SET);
            return std::fread(out, 1, RAW, t.fp) == RAW;
        }
    }
    std::memset(out, 0, RAW);
    return false;
}

const Track* track_at(uint32_t lba) {
    const Track* best = nullptr;
    for (auto& t : g_tracks)
        if (lba >= t.file_lba) best = &t;
    return best;
}

// No .cue: the .bin files in the image's folder by name, the first the data track and the others
// audio (as "Track 1", "Track 2" ... dumps are named).
void load_bins(const std::filesystem::path& dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> bins;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
        if (ext == ".bin") bins.push_back(e.path());
    }
    std::sort(bins.begin(), bins.end());
    uint32_t base = 0;
    for (const auto& b : bins) {
        Track t{};
        t.number = (int)g_tracks.size() + 1;
        t.audio = !g_tracks.empty();
        t.file = b.string();
        t.fp = std::fopen(t.file.c_str(), "rb");
        if (!t.fp) fatal("The disc image could not be opened: %s", t.file.c_str());
        std::fseek(t.fp, 0, SEEK_END);
        t.sectors = (uint32_t)(std::ftell(t.fp) / RAW);
        t.file_lba = base;
        t.start_lba = base + (t.audio ? 150 : 0);  // an audio track's 2 s pregap is in its file
        base += t.sectors;
        g_tracks.push_back(t);
    }
}

// The file a .cue names, matched without case (cue sheets often say "GAME.BIN" for game.bin,
// which only Windows forgives).
std::string cue_file(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    if (std::filesystem::exists(dir / name, ec)) return (dir / name).string();
    auto lower = [](std::string t) {
        for (char& ch : t) ch = (char)std::tolower((unsigned char)ch);
        return t;
    };
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == lower(name)) return e.path().string();
    return (dir / name).string();
}

void load_cue(const std::string& cue_path) {
    std::string ext = std::filesystem::path(cue_path).extension().string();
    for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    if (ext == ".bin") {
        load_bins(std::filesystem::path(cue_path).parent_path());
        if (g_tracks.empty()) fatal("no disc image in %s", cue_path.c_str());
        g_disc_end = g_tracks.back().file_lba + g_tracks.back().sectors;
        for (auto& t : g_tracks)
            log_msg(LogCat::Cd, "track %d %s start=%u sectors=%u", t.number, t.audio ? "audio" : "data", t.start_lba, t.sectors);
        return;
    }
    std::ifstream in(cue_path);
    if (!in)
        fatal("The game's disc image could not be opened: %s", cue_path.c_str());
    auto dir = std::filesystem::path(cue_path).parent_path();
    std::string line, cur_file;
    uint32_t file_base = 0;
    while (std::getline(in, line)) {
        auto p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        line = line.substr(p);
        if (line.rfind("FILE", 0) == 0) {
            auto a = line.find('"'), b = line.rfind('"');
            cur_file = cue_file(dir, line.substr(a + 1, b - a - 1));
            if (!g_tracks.empty()) file_base = g_tracks.back().file_lba + g_tracks.back().sectors;
        } else if (line.rfind("TRACK", 0) == 0) {
            Track t{};
            t.number = std::atoi(line.c_str() + 6);
            t.audio = line.find("AUDIO") != std::string::npos;
            t.file = cur_file;
            t.file_lba = file_base;
            t.fp = std::fopen(cur_file.c_str(), "rb");
            if (!t.fp) fatal("The disc image is incomplete: %s is missing (assets\\image).", cur_file.c_str());
            std::fseek(t.fp, 0, SEEK_END);
            t.sectors = (uint32_t)(std::ftell(t.fp) / RAW);
            t.start_lba = file_base;
            g_tracks.push_back(t);
        } else if (line.rfind("INDEX 01", 0) == 0 && !g_tracks.empty()) {
            int m, s, f;
            if (std::sscanf(line.c_str() + 9, "%d:%d:%d", &m, &s, &f) == 3)
                g_tracks.back().start_lba = g_tracks.back().file_lba + (uint32_t)((m * 60 + s) * 75 + f);
        }
    }
    if (g_tracks.empty()) fatal("no tracks in %s", cue_path.c_str());
    g_disc_end = g_tracks.back().file_lba + g_tracks.back().sectors;
    for (auto& t : g_tracks)
        log_msg(LogCat::Cd, "track %d %s start=%u sectors=%u", t.number, t.audio ? "audio" : "data", t.start_lba, t.sectors);
}

// ---------------------------------------------------------------- controller state

enum : uint8_t {
    ST_ERROR = 0x01, ST_MOTOR = 0x02, ST_SEEKERR = 0x04, ST_IDERR = 0x08, ST_SHELL = 0x10,
    ST_READ = 0x20, ST_SEEK = 0x40, ST_PLAY = 0x80,
};

struct Response {
    uint8_t irq;
    std::vector<uint8_t> bytes;
    uint64_t due;  // absolute cycle
};

// The BIOS leaves all CD interrupt types enabled after boot and libcd relies on that.
uint8_t g_index = 0, g_int_enable = 0x1F, g_int_flag = 0;
std::vector<uint8_t> g_params;
std::deque<uint8_t> g_resp_fifo;
std::deque<Response> g_pending;
bool g_busy = false;

uint8_t g_stat = ST_MOTOR;
uint8_t g_mode = 0;
uint8_t g_filter_file = 0, g_filter_chan = 0;
bool g_muted = false;

uint32_t g_setloc = 0;
bool g_setloc_pending = false;
uint32_t g_pos = 0;       // current head position (LBA)
bool g_reading = false, g_playing = false;

uint8_t g_sector[RAW];     // last sector delivered with INT1
uint8_t g_last_header[8];  // for GetlocL
std::vector<uint8_t> g_data;
size_t g_data_pos = 0;

// audio volume matrix (L->L, L->R, R->R, R->L) and pending values
uint8_t g_vol[4] = {0x80, 0, 0x80, 0}, g_vol_pending[4] = {0x80, 0, 0x80, 0};

// XA decoder history per channel
int32_t g_xa_old[2] = {}, g_xa_older[2] = {};
// Voice lines: the game's data read routine (8006B234: section loads, a door button's event ...)
// stops a playing voice clip (the drive cannot stream both), which cut one-liners off. The clip
// being played (next sector of its channel, end) is noted; when a read starts before its end,
// the rest is streamed from the disc here instead, at the rate it plays (cdrom_voice_poll).
uint32_t g_voice_next = 0, g_voice_end = 0;
int g_voice_chan = 0;
int32_t g_voice_old[2] = {}, g_voice_older[2] = {};  // the clip's ADPCM history
uint32_t g_carry_next = 0, g_carry_end = 0;           // the rest being streamed (0 = none)
// The drive streams on past a clip's end into the next recording on its channel until the game's
// once-a-frame monitor (800662A0) sees the position and mutes: a frame or two of another line
// ended every one-liner. Past the clip's end nothing more is played until the next Setloc.
uint32_t g_voice_stop = 0;
int32_t g_carry_old[2] = {}, g_carry_older[2] = {};
// Pickup lines while the drive is busy (a data read, the game's queued reads, a level's own XA
// stream as in pest): the game refuses a voice then. The clip is played here straight from the
// disc image instead, beside the drive, mixed over the CD input (spu_push_side_audio).
struct {
    uint32_t next = 0, end = 0;  // next sector of the clip (0 = none), the sector after its last
    int chan = 0;
    int32_t old[2] = {}, older[2] = {};
} g_side;

// Data reads without XA audio run kFastData times faster than the drive: level section loads
// froze the picture for 1-2 s. XA (voices, movie sound) keeps the real rate.
constexpr uint64_t kFastData = 8;
bool fast_data() {
    return !(g_mode & 0x40);
}
uint64_t sector_period() {
    uint64_t p = (g_mode & 0x80) ? PSX_CPU_HZ / 150 : PSX_CPU_HZ / 75;
    return fast_data() ? p / kFastData : p;
}

void schedule_next(R3000* c) {
    if (g_pending.empty()) {
        sched_cancel(c, Ev::Cdrom);
        return;
    }
    uint64_t due = g_pending.front().due;
    sched_add(c, Ev::Cdrom, due > c->cycles ? due - c->cycles : 1);
}

void queue(R3000* c, uint8_t irq, std::vector<uint8_t> bytes, uint64_t delay) {
    uint64_t due = c->cycles + delay;
    if (!g_pending.empty()) due = std::max(due, g_pending.back().due);
    g_pending.push_back({irq, std::move(bytes), due});
    schedule_next(c);
}

void try_deliver(R3000* c) {
    if (g_pending.empty() || g_int_flag) return;
    Response& r = g_pending.front();
    if (r.due > c->cycles) {
        schedule_next(c);
        return;
    }
    g_resp_fifo.assign(r.bytes.begin(), r.bytes.end());
    g_int_flag = r.irq;
    g_pending.pop_front();
    g_busy = false;
    LOG(Cd, "INT%d [%zu bytes] stat=%02X", g_int_flag, g_resp_fifo.size(), g_stat);
    if (g_int_flag & g_int_enable) irq_raise(c, IRQ_CDROM);
    schedule_next(c);
}

// ---------------------------------------------------------------- XA ADPCM

const int kPos[4] = {0, 60, 115, 98};
const int kNeg[4] = {0, 0, -52, -55};

void xa_decode_block(const uint8_t* group, int block, int ch, int16_t* out) {
    uint8_t hdr = group[4 + block];
    int shift = hdr & 0xF;
    if (shift > 12) shift = 9;
    int filter = (hdr >> 4) & 3;
    int32_t& old = g_xa_old[ch];
    int32_t& older = g_xa_older[ch];
    for (int i = 0; i < 28; i++) {
        uint8_t byte = group[16 + (block >> 1) + i * 4];
        int nib = (block & 1) ? byte >> 4 : byte & 0xF;
        int32_t t = (int32_t)((uint32_t)nib << 28) >> 28;
        int32_t s = ((t << 12) >> shift) + ((old * kPos[filter] + older * kNeg[filter] + 32) >> 6);
        s = std::clamp(s, -32768, 32767);
        older = old;
        old = s;
        out[i] = (int16_t)s;
    }
}

// One XA audio sector -> interleaved stereo pcm at *rate (false: 8-bit XA, not used by this game)
bool xa_decode_sector(const uint8_t* raw, std::vector<int16_t>& pcm, int* rate) {
    uint8_t coding = raw[16 + 3];
    bool stereo = coding & 1;
    *rate = (coding & 4) ? 18900 : 37800;
    if (coding & 0x10) return false;
    const uint8_t* groups = raw + 24;
    pcm.clear();
    pcm.reserve(18 * 8 * 28 * 2);
    for (int g = 0; g < 18; g++) {
        const uint8_t* grp = groups + g * 128;
        if (stereo) {
            for (int b = 0; b < 8; b += 2) {
                int16_t l[28], r[28];
                xa_decode_block(grp, b, 0, l);
                xa_decode_block(grp, b + 1, 1, r);
                for (int i = 0; i < 28; i++) { pcm.push_back(l[i]); pcm.push_back(r[i]); }
            }
        } else {
            for (int b = 0; b < 8; b++) {
                int16_t m[28];
                xa_decode_block(grp, b, 0, m);
                for (int i = 0; i < 28; i++) { pcm.push_back(m[i]); pcm.push_back(m[i]); }
            }
        }
    }
    return true;
}

void xa_play_sector(const uint8_t* raw, bool side = false) {
    std::vector<int16_t> pcm;
    int rate;
    if (!xa_decode_sector(raw, pcm, &rate) || g_muted) return;
    // apply the CD audio volume matrix (0x80 = 100%)
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
        int l = pcm[i], r = pcm[i + 1];
        pcm[i] = (int16_t)std::clamp((l * g_vol[0] + r * g_vol[3]) >> 7, -32768, 32767);
        pcm[i + 1] = (int16_t)std::clamp((r * g_vol[2] + l * g_vol[1]) >> 7, -32768, 32767);
    }
    if (side) spu_push_side_audio(pcm.data(), (int)(pcm.size() / 2), rate);
    else spu_push_cd_audio(pcm.data(), (int)(pcm.size() / 2), rate);
}

// A sector of a clip streamed apart from the drive, with that stream's own ADPCM history.
void play_stream_sector(const uint8_t* raw, int32_t old[2], int32_t older[2], bool side) {
    int32_t keep[4] = {g_xa_old[0], g_xa_old[1], g_xa_older[0], g_xa_older[1]};
    std::memcpy(g_xa_old, old, sizeof g_xa_old);
    std::memcpy(g_xa_older, older, sizeof g_xa_older);
    xa_play_sector(raw, side);
    std::memcpy(old, g_xa_old, sizeof g_xa_old);
    std::memcpy(older, g_xa_older, sizeof g_xa_older);
    g_xa_old[0] = keep[0], g_xa_old[1] = keep[1], g_xa_older[0] = keep[2], g_xa_older[1] = keep[3];
}

void cdda_play_sector(const uint8_t* raw) {
    if (g_muted) return;
    int16_t pcm[588 * 2];
    std::memcpy(pcm, raw, sizeof pcm);
    for (int i = 0; i < 588 * 2; i += 2) {
        int l = pcm[i], r = pcm[i + 1];
        pcm[i] = (int16_t)std::clamp((l * g_vol[0] + r * g_vol[3]) >> 7, -32768, 32767);
        pcm[i + 1] = (int16_t)std::clamp((r * g_vol[2] + l * g_vol[1]) >> 7, -32768, 32767);
    }
    spu_push_cd_audio(pcm, 588, 44100);
}

// ---------------------------------------------------------------- commands

void apply_setloc() {
    if (g_setloc_pending) {
        g_pos = g_setloc;
        g_setloc_pending = false;
        g_voice_stop = 0;
        std::memset(g_xa_old, 0, sizeof g_xa_old);
        std::memset(g_xa_older, 0, sizeof g_xa_older);
    }
}

void start_read(R3000* c) {
    apply_setloc();
    // a data read before the voice clip ended: the rest of it plays on (cdrom_voice_poll). Not when
    // the game stopped the clip itself (it mutes first) or starts another voice (XA mode).
    if (g_voice_next && g_voice_next < g_voice_end && g_pos != g_voice_next && !(g_mode & 0x40) && !g_muted) {
        g_carry_next = g_voice_next, g_carry_end = g_voice_end;
        std::memcpy(g_carry_old, g_voice_old, sizeof g_carry_old);
        std::memcpy(g_carry_older, g_voice_older, sizeof g_carry_older);
        LOG(Host, "voice: a read cut the clip %u sectors before its end, it plays on", (g_voice_end - g_voice_next) / 16);
    }
    g_voice_next = 0;
    g_reading = true;
    g_playing = false;
    g_stat = (g_stat & ~(ST_PLAY | ST_SEEK)) | ST_READ | ST_MOTOR;
    sched_add(c, Ev::CdromRead, sector_period() + 20000);
}

void stop_all(R3000* c) {
    g_reading = g_playing = false;
    g_stat &= ~(ST_READ | ST_PLAY | ST_SEEK);
    sched_cancel(c, Ev::CdromRead);
}

void command(R3000* c, uint8_t cmd) {
    const uint64_t ACK = 25000;   // first response
    const uint64_t DONE = 60000;  // second response
    std::vector<uint8_t> p = g_params;
    g_params.clear();
    g_busy = true;
    LOG(Cd, "cmd %02X params=%zu [%02X %02X %02X] f=%llu", cmd, p.size(), p.size() > 0 ? p[0] : 0, p.size() > 1 ? p[1] : 0,
        p.size() > 2 ? p[2] : 0, (unsigned long long)host_frame_count());
    auto stat = [&] { return std::vector<uint8_t>{g_stat}; };
    switch (cmd) {
        case 0x01: queue(c, 3, stat(), ACK); g_stat &= ~ST_SHELL; break;  // Getstat
        case 0x02:  // Setloc
            if (p.size() >= 3) {
                int m = unbcd(p[0]), s = unbcd(p[1]), f = unbcd(p[2]);
                g_setloc = (uint32_t)((m * 60 + s) * 75 + f) - 150;
                g_setloc_pending = true;
            }
            queue(c, 3, stat(), ACK);
            break;
        case 0x03: {  // Play [track]
            if (!p.empty() && p[0]) {
                int tn = unbcd(p[0]);
                for (auto& t : g_tracks)
                    if (t.number == tn) { g_pos = t.start_lba; g_setloc_pending = false; }
            } else {
                apply_setloc();
            }
            g_reading = false;
            g_playing = true;
            g_stat = (g_stat & ~(ST_READ | ST_SEEK)) | ST_PLAY | ST_MOTOR;
            queue(c, 3, stat(), ACK);
            sched_add(c, Ev::CdromRead, PSX_CPU_HZ / 75);
            break;
        }
        case 0x04: case 0x05: queue(c, 3, stat(), ACK); break;  // Forward / Backward
        case 0x06: case 0x1B:  // ReadN / ReadS
            queue(c, 3, stat(), ACK);
            start_read(c);
            break;
        case 0x07: g_stat |= ST_MOTOR; queue(c, 3, stat(), ACK); queue(c, 2, stat(), DONE); break;  // MotorOn
        case 0x08:  // Stop
            stop_all(c);
            queue(c, 3, stat(), ACK);
            g_stat &= ~ST_MOTOR;
            queue(c, 2, stat(), DONE * 4);
            break;
        case 0x09: {  // Pause
            queue(c, 3, stat(), ACK);
            bool was_active = g_reading || g_playing;
            stop_all(c);
            queue(c, 2, stat(), was_active ? (g_mode & 0x80 ? 2100000 : 1060000) / 16 : 7000);
            break;
        }
        case 0x0A:  // Init
            g_mode = 0;
            stop_all(c);
            g_stat = ST_MOTOR;
            queue(c, 3, stat(), ACK);
            queue(c, 2, stat(), DONE);
            break;
        case 0x0B: g_muted = true; queue(c, 3, stat(), ACK); break;
        case 0x0C: g_muted = false; queue(c, 3, stat(), ACK); break;
        case 0x0D:  // Setfilter
            if (p.size() >= 2) { g_filter_file = p[0]; g_filter_chan = p[1]; }
            queue(c, 3, stat(), ACK);
            break;
        case 0x0E: if (!p.empty()) g_mode = p[0]; queue(c, 3, stat(), ACK); break;  // Setmode
        case 0x0F: queue(c, 3, {g_stat, g_mode, 0, g_filter_file, g_filter_chan}, ACK); break;
        case 0x10: queue(c, 3, std::vector<uint8_t>(g_last_header, g_last_header + 8), ACK); break;  // GetlocL
        case 0x11: {  // GetlocP
            const Track* t = track_at(g_pos);
            uint32_t rel = t && g_pos >= t->start_lba ? g_pos - t->start_lba : 0;
            // relative position within the track has no 2-second lead-in offset
            uint8_t m = bcd((int)(rel / 4500)), s = bcd((int)((rel / 75) % 60)), f = bcd((int)(rel % 75));
            uint8_t am, as, af;
            lba_to_msf(g_pos, &am, &as, &af);
            queue(c, 3, {bcd(t ? t->number : 1), 1, m, s, f, am, as, af}, ACK);
            break;
        }
        case 0x12: queue(c, 3, stat(), ACK); queue(c, 2, stat(), DONE); break;  // SetSession
        case 0x13: queue(c, 3, {g_stat, bcd(g_tracks.front().number), bcd(g_tracks.back().number)}, ACK); break;
        case 0x14: {  // GetTD(track)
            int tn = p.empty() ? 0 : unbcd(p[0]);
            uint32_t lba = g_disc_end;
            for (auto& t : g_tracks)
                if (t.number == tn) lba = t.start_lba;
            uint8_t m, s, f;
            lba_to_msf(lba, &m, &s, &f);
            queue(c, 3, {g_stat, m, s}, ACK);
            break;
        }
        case 0x15: case 0x16:  // SeekL / SeekP
            stop_all(c);
            apply_setloc();
            g_stat |= ST_SEEK;
            queue(c, 3, stat(), ACK);
            g_stat &= ~ST_SEEK;
            queue(c, 2, stat(), DONE * 2);
            break;
        case 0x19:  // Test
            if (!p.empty() && p[0] == 0x20) queue(c, 3, {0x94, 0x09, 0x19, 0xC0}, ACK);
            else queue(c, 3, stat(), ACK);
            break;
        case 0x1A:  // GetID: licensed, NTSC-U ("SCEA")
            queue(c, 3, stat(), ACK);
            queue(c, 2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}, DONE);
            break;
        case 0x1C: stop_all(c); g_mode = 0; g_stat = ST_MOTOR; queue(c, 3, stat(), ACK); break;  // Reset
        case 0x1E: queue(c, 3, stat(), ACK); queue(c, 2, stat(), DONE * 8); break;  // ReadTOC
        default:
            log_msg(LogCat::Cd, "unhandled CD command %02X", cmd);
            queue(c, 5, {(uint8_t)(g_stat | ST_ERROR), 0x40}, ACK);
            break;
    }
}

}  // namespace

// ================================================================ public

void cdrom_init(const std::string& cue_path) {
    load_cue(cue_path);
}

void cdrom_event(R3000* c) { try_deliver(c); }

void cdrom_read_event(R3000* c) {
    if (g_playing) {
        uint8_t raw[RAW];
        read_raw(g_pos, raw);
        cdda_play_sector(raw);
        g_pos++;
        const Track* t = track_at(g_pos);
        if (g_pos >= g_disc_end || ((g_mode & 2) && t && t->start_lba == g_pos)) {
            stop_all(c);
            queue(c, 4, {g_stat}, 100);  // data end
            return;
        }
        sched_add(c, Ev::CdromRead, PSX_CPU_HZ / 75);
        return;
    }
    if (!g_reading) return;
    sched_add(c, Ev::CdromRead, sector_period());
    // a fast read waits for the game to take the previous sector instead of dropping it
    if (fast_data() && (g_int_flag == 1 || (!g_pending.empty() && g_pending.back().irq == 1))) return;
    uint8_t raw[RAW];
    bool ok = read_raw(g_pos, raw);
    uint32_t lba = g_pos++;
    if (!ok) {
        queue(c, 5, {(uint8_t)(g_stat | ST_ERROR), 0x04}, 100);
        return;
    }
    movie_cd_sector(lba);  // a replacement movie starts or stops (movie.cpp)
    std::memcpy(g_last_header, raw + 12, 8);
    uint8_t file = raw[16], chan = raw[17], submode = raw[18];
    bool is_audio = (submode & 0x04) && (submode & 0x20);  // audio + form 2
    if ((g_mode & 0x40) && is_audio) {
        bool match = !(g_mode & 0x08) || (file == g_filter_file && chan == g_filter_chan);
        if (match && g_voice_stop && lba >= g_voice_stop) return;  // past the clip (see g_voice_stop)
        if (match) {
            subtitle_note_xa(lba, chan);
            g_carry_next = 0;  // the game plays a voice again
            xa_play_sector(raw);
            uint32_t end = 0;
            if (voice_clip_end(lba, chan, &end)) {
                g_voice_next = lba + 16, g_voice_end = end, g_voice_chan = chan;
                g_voice_stop = end;
                std::memcpy(g_voice_old, g_xa_old, sizeof g_voice_old);
                std::memcpy(g_voice_older, g_xa_older, sizeof g_voice_older);
            }
        }
        return;  // real-time audio never reaches the data path
    }
    if ((g_mode & 0x08) && is_audio) return;
    std::memcpy(g_sector, raw, RAW);
    // A new INT1 replaces an unacknowledged older one rather than queueing behind it.
    if (!g_pending.empty() && g_pending.back().irq == 1) {
        g_pending.pop_back();
        log_msg(LogCat::Cd, "sector %u dropped: previous INT1 not acknowledged in time", lba - 1);
    }
    if (g_int_flag == 1) log_msg(LogCat::Cd, "sector %u arrived while INT1 still set", lba);
    queue(c, 1, {g_stat}, 0);
    LOG(Cd, "sector %u ready", lba);
}

uint8_t cdrom_read(R3000* c, uint32_t reg) {
    (void)c;
    switch (reg) {
        case 0: {
            uint8_t s = g_index;
            s |= g_params.empty() ? 0x08 : 0;
            s |= g_params.size() < 16 ? 0x10 : 0;
            s |= g_resp_fifo.empty() ? 0 : 0x20;
            s |= g_data_pos < g_data.size() ? 0x40 : 0;
            s |= g_busy ? 0x80 : 0;
            return s;
        }
        case 1: {
            if (g_resp_fifo.empty()) return 0;
            uint8_t v = g_resp_fifo.front();
            g_resp_fifo.pop_front();
            return v;
        }
        case 2:
            LOG(Cd, "data port read at fifo pos %zu/%zu", g_data_pos, g_data.size());
            return g_data_pos < g_data.size() ? g_data[g_data_pos++] : 0;
        case 3: return (g_index & 1) ? (uint8_t)(g_int_flag | 0xE0) : (uint8_t)(g_int_enable | 0xE0);
    }
    return 0;
}

void cdrom_write(R3000* c, uint32_t reg, uint8_t v) {
    if (reg == 0) {
        g_index = v & 3;
        return;
    }
    LOG(Cd, "write idx%d reg%u = %02X", g_index, reg, v);
    switch (g_index << 4 | reg) {
        case 0x01: command(c, v); break;
        case 0x02: if (g_params.size() < 16) g_params.push_back(v); break;
        case 0x03:  // request register
            if (v & 0x80) {
                if (g_data_pos >= g_data.size()) {
                    bool whole = g_mode & 0x20;
                    const uint8_t* src = whole ? g_sector + 12 : g_sector + 24;
                    g_data.assign(src, src + (whole ? 0x924 : 0x800));
                    g_data_pos = 0;
                }
            } else {
                g_data.clear();
                g_data_pos = 0;
            }
            break;
        case 0x12:
            // the interrupt line is level-sensitive: enabling a pending type raises it
            g_int_enable = v & 0x1F;
            if (g_int_flag & g_int_enable) irq_raise(c, IRQ_CDROM);
            break;
        case 0x13:
            g_int_flag &= ~(v & 0x1F);
            if (v & 0x40) g_params.clear();
            if (!g_int_flag) {
                g_resp_fifo.clear();
                try_deliver(c);
            }
            break;
        case 0x11: break;  // sound map data out
        case 0x21: break;  // sound map coding info
        case 0x22: g_vol_pending[0] = v; break;
        case 0x23: g_vol_pending[1] = v; break;
        case 0x31: g_vol_pending[2] = v; break;
        case 0x32: g_vol_pending[3] = v; break;
        case 0x33: if (v & 0x20) std::memcpy(g_vol, g_vol_pending, 4); break;
        default: break;
    }
}

uint32_t cdrom_dma_read_word() {
    if (g_data_pos == 0 || g_data_pos == 12 || g_data_pos == 24 || g_data_pos == 44)
        LOG(Cd, "DMA word at fifo pos %zu/%zu", g_data_pos, g_data.size());
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = g_data_pos < g_data.size() ? g_data[g_data_pos++] : 0;
        w |= (uint32_t)b << (8 * i);
    }
    return w;
}

bool cdrom_read_sector_user(uint32_t lba, uint8_t out[2048]) {
    uint8_t raw[RAW];
    if (!read_raw(lba, raw)) return false;
    std::memcpy(out, raw + 24, 2048);
    return true;
}

bool cdrom_find_file(const char* path, uint32_t* lba_out, uint32_t* size_out) {
    // ISO9660 walk from the root; path like "\\DIR\\FILE.EXT;1"
    uint8_t sec[2048];
    if (!cdrom_read_sector_user(16, sec)) return false;
    uint32_t dir_lba, dir_size;
    std::memcpy(&dir_lba, sec + 156 + 2, 4);
    std::memcpy(&dir_size, sec + 156 + 10, 4);
    std::string p(path);
    for (auto& ch : p) {
        if (ch == '/') ch = '\\';
        ch = (char)std::toupper((unsigned char)ch);
    }
    if (auto colon = p.find(':'); colon != std::string::npos) p = p.substr(colon + 1);
    while (!p.empty() && p[0] == '\\') p.erase(0, 1);
    if (auto semi = p.find(';'); semi != std::string::npos) p = p.substr(0, semi);
    for (;;) {
        std::string part = p.substr(0, p.find('\\'));
        bool last = p.find('\\') == std::string::npos;
        bool found = false;
        for (uint32_t s = 0; s < (dir_size + 2047) / 2048 && !found; s++) {
            cdrom_read_sector_user(dir_lba + s, sec);
            for (int o = 0; o < 2048 && sec[o];) {
                int len = sec[o];
                std::string name((char*)sec + o + 33, sec[o + 32]);
                if (auto semi = name.find(';'); semi != std::string::npos) name = name.substr(0, semi);
                if (name == part) {
                    std::memcpy(&dir_lba, sec + o + 2, 4);
                    std::memcpy(&dir_size, sec + o + 10, 4);
                    found = true;
                    break;
                }
                o += len;
            }
        }
        if (!found) return false;
        if (last) {
            *lba_out = dir_lba;
            *size_out = dir_size;
            return true;
        }
        p = p.substr(part.size() + 1);
    }
}

void cdrom_voice_poll() {
    uint8_t raw[RAW];
    for (int guard = 0; g_carry_next && g_carry_next < g_carry_end && spu_cd_queued() < 8820 && guard < 8; guard++) {
        const bool ok = read_raw(g_carry_next, raw);
        g_carry_next += 16;
        if (!ok || !(raw[18] & 4) || raw[17] != g_voice_chan) continue;
        play_stream_sector(raw, g_carry_old, g_carry_older, false);
    }
    if (g_carry_next >= g_carry_end) g_carry_next = 0;
    for (int guard = 0; g_side.next && g_side.next < g_side.end && spu_side_queued() < 8820 && guard < 8; guard++) {
        const bool ok = read_raw(g_side.next, raw);
        g_side.next += 16;
        if (ok && (raw[18] & 4) && raw[17] == g_side.chan) play_stream_sector(raw, g_side.old, g_side.older, true);
    }
    if (g_side.next >= g_side.end) g_side.next = 0;
}

bool cdrom_side_voice(uint32_t lba, uint32_t end, int chan) {
    if (cdrom_side_busy()) return false;
    g_side = {};
    g_side.next = lba, g_side.end = end, g_side.chan = chan;
    subtitle_note_xa(lba, chan);  // its subtitle, as when the drive plays it
    cdrom_voice_poll();
    return true;
}

bool cdrom_side_busy() { return g_side.next || g_carry_next || spu_side_queued() > 0; }

// ================================================================ save states

void cdrom_state(StateIO& s) {
    if (s.loading()) g_voice_next = g_carry_next = g_voice_stop = g_side.next = 0;  // (not saved: a voice line going on)
    s.marker(0x43444452);  // "CDDR"
    s(g_index); s(g_int_enable); s(g_int_flag);
    s.vec(g_params);
    s.deq(g_resp_fifo);
    uint32_t n = (uint32_t)g_pending.size();
    s(n);
    if (s.loading()) g_pending.resize(n);
    for (auto& r : g_pending) {
        s(r.irq);
        s.vec(r.bytes);
        s(r.due);
    }
    s(g_busy); s(g_stat); s(g_mode); s(g_filter_file); s(g_filter_chan); s(g_muted);
    s(g_setloc); s(g_setloc_pending); s(g_pos); s(g_reading); s(g_playing);
    s.raw(g_sector, sizeof g_sector);
    s.raw(g_last_header, sizeof g_last_header);
    s.vec(g_data);
    s(g_data_pos);
    s.raw(g_vol, sizeof g_vol);
    s.raw(g_vol_pending, sizeof g_vol_pending);
    s.raw(g_xa_old, sizeof g_xa_old);
    s.raw(g_xa_older, sizeof g_xa_older);
}

// Texture replacements (F1 -> Texture replacements): textures\replacements\<file>\<index>.png,
// named as tools/psx_textures.py exports them (textures\files), drawn instead of the game's own.
//
// Matching: the .psx files named by the folders are read from the disc (CD.HED / CD.WAD) and
// their texture sections parsed (psx_textures.py has the format): each texture's texels are
// stored exactly as they go into VRAM (rows of 16-bit words). After VRAM changes (a level loads
// its textures, a saved state is loaded, the option is switched on) VRAM's texture area is
// scanned for them; each place found becomes a region.
//
// Drawing: replacements go into an HD copy of VRAM's texture area (x 512..1023), kScale times the
// texels, each at its own place - 4-bit textures in the top half (2048 x 512 texels), 8-bit ones
// in the bottom half (1024 x 512) - so polygons that span neighbouring textures (the game's art
// is often one picture stored as several textures) or repeat one with the texture window (floors)
// still line up. A polygon is drawn from it (HwPrim::hd; the shader samples it with the polygon's
// own texture coordinates and window) when every texel it may use is covered by regions.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"
#include "stb_truetype.h"  // (its code is in ui_text.cpp)

bool replace_load_image(const std::string& path, std::vector<uint32_t>& rgba, int& w, int& h);  // replace.cpp

bool g_tex_replace = true;  // setting (settings.ini "texreplace"; on: whatever is in the folder is used)
bool g_game_font = true;    // setting (settings.ini "gamefont"): the game's letters in assets/fonts' font

namespace {

constexpr int kScale = 4;  // HD texels per game texel (the shaders use the same)
// (APOC_TEXREPLACE_DIR: another folder, for tests - never the player's own)
const char* const kDir = std::getenv("APOC_TEXREPLACE_DIR") ? std::getenv("APOC_TEXREPLACE_DIR") : "assets/replacements/textures";

struct Tex {
    std::string png;             // the replacement
    std::vector<uint16_t> data;  // texels as in VRAM: rows of hw words
    int hw = 0, h = 0;           // size in VRAM words
    int texels_w = 0;            // width in texels
    int depth = 0;               // 0 = 4-bit, 1 = 8-bit
    std::vector<uint16_t> pal;   // its palette (16 or 256 colours)
    int key_dx = 0, key_dy = 0;  // where its first non-zero 4-word window is (the scan key)
    std::vector<uint32_t> hd[3];  // the replacement at kScale, kScale/2, 1 (mip levels; loaded when first used)
    bool tried = false;
    uint32_t glyph = 0;  // a letter of the game's fonts (bits.psx): its character, drawn in assets/fonts' font
};

// ---- the game's fonts in a TrueType font (Enhancements -> Font)
// Both fonts are textures in bits.psx, one per letter, cut to the letter's ink: the big bronze one
// (menus, titles) and the small white one (HUD, messages). Each letter is drawn again in the font
// from assets/fonts (the first .ttf/.otf there) to fill the same box, coloured row by row like the
// game's letter, and goes in as that texture's replacement: the game still places, spaces, tints and
// fades its letters itself.
uint32_t font_glyph(int idx) {
    static const char* const kBig = "-+%\xC4\xD6\xDC" "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789?\xC0!\xC9\xC8\xD4',-\xCA\xCC";  // 97..149
    static const char* const kSmall = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.'?!-0123456789";                         // 13..53
    if (idx >= 97 && idx <= 149) return (uint8_t)kBig[idx - 97];
    if (idx >= 13 && idx <= 53) return (uint8_t)kSmall[idx - 13];
    return 0;
}

stbtt_fontinfo g_ttf;
std::vector<uint8_t> g_ttf_data;
std::string g_ttf_name;  // the font file's name without extension ("" = none found)

bool font_loaded() {
    static bool tried = false;
    if (tried) return !g_ttf_name.empty();
    tried = true;
    std::error_code ec;
    std::vector<std::filesystem::path> found;
    for (const auto& e : std::filesystem::directory_iterator("assets/fonts", ec)) {
        std::string x = e.path().extension().string();
        for (char& ch : x) ch = (char)std::tolower((unsigned char)ch);
        if (x == ".ttf" || x == ".otf") found.push_back(e.path());
    }
    if (found.empty()) return false;
    std::sort(found.begin(), found.end());
    FILE* f = std::fopen(found[0].string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    g_ttf_data.resize((size_t)std::max(0L, std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool read = std::fread(g_ttf_data.data(), 1, g_ttf_data.size(), f) == g_ttf_data.size();
    std::fclose(f);
    if (!read || !stbtt_InitFont(&g_ttf, g_ttf_data.data(), stbtt_GetFontOffsetForIndex(g_ttf_data.data(), 0))) return false;
    g_ttf_name = found[0].stem().string();
    log_msg(LogCat::Host, "game font: %s", found[0].string().c_str());
    return true;
}

// Drawn in the TrueType font now (setting on, the font has the character).
bool glyph_on(const Tex& t) {
    return t.glyph && g_game_font && font_loaded() && stbtt_FindGlyphIndex(&g_ttf, (int)t.glyph) != 0;
}

// The letter at kScale (t.hd[0]): the font's glyph scaled to the height of the game letter's ink
// (narrowed if wider than it), centred in it. Only its shape counts: the shaders
// colour it from the game letter's own texels, in whatever palette the game draws it with.
bool render_glyph(Tex& t) {
    const int per = t.depth == 0 ? 4 : 2, dw = t.texels_w * kScale, dh = t.h * kScale;
    int x0 = t.texels_w, x1 = -1, y0 = t.h, y1 = -1;
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.texels_w; x++) {
            const uint16_t w = t.data[(size_t)y * t.hw + x / per];
            const int i = t.depth == 0 ? (w >> (x % 4 * 4)) & 15 : (w >> (x % 2 * 8)) & 255;
            const uint16_t c = i < (int)t.pal.size() ? t.pal[i] & 0x7FFF : 0;
            if (c && c != 0x7C1F)  // (0 and magenta: see-through)
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
    if (x1 < 0) return false;
    const int bw = (x1 - x0 + 1) * kScale, bh = (y1 - y0 + 1) * kScale;
    int gx0, gy0, gx1, gy1;
    stbtt_GetCodepointBitmapBox(&g_ttf, (int)t.glyph, 1, 1, &gx0, &gy0, &gx1, &gy1);
    if (gx1 <= gx0 || gy1 <= gy0) return false;
    const float sy = (float)bh / (float)(gy1 - gy0), sx = std::min(sy, (float)bw / (float)(gx1 - gx0));
    int ix0, iy0, ix1, iy1;
    stbtt_GetCodepointBitmapBox(&g_ttf, (int)t.glyph, sx, sy, &ix0, &iy0, &ix1, &iy1);
    const int gw = ix1 - ix0, gh = iy1 - iy0;
    if (gw <= 0 || gh <= 0) return false;
    std::vector<uint8_t> cov((size_t)gw * gh);
    stbtt_MakeCodepointBitmap(&g_ttf, cov.data(), gw, gh, gw, sx, sy, (int)t.glyph);
    const int ox = x0 * kScale + (bw - gw) / 2, oy = y0 * kScale + (bh - gh) / 2;
    t.hd[0].assign((size_t)dw * dh, 0);
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            const int px = ox + x, py = oy + y;
            const uint8_t a = cov[(size_t)y * gw + x];
            if (a && px >= 0 && py >= 0 && px < dw && py < dh) t.hd[0][(size_t)py * dw + px] = 0x00FFFFFFu | (uint32_t)a << 24;
        }
    return true;
}

struct Region {
    int x, y;  // VRAM words
    int tex;
};

bool g_loaded = false;
std::vector<Tex> g_tex;
std::unordered_multimap<uint64_t, int> g_keys;  // first non-zero window -> texture
// textures under 4 words wide (font letters, small icons): the first non-zero 4-row column instead
std::unordered_multimap<uint64_t, int> g_keys_v;
// a quick "no texture has this key" for the scans (most VRAM positions): one bit per key hash
std::vector<uint64_t> g_key_bits(1 << 12);  // 2^18 bits
uint32_t key_bit(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 46); }
bool maybe_key(uint64_t k) { const uint32_t b = key_bit(k); return g_key_bits[b >> 6] >> (b & 63) & 1; }
void add_key_bit(uint64_t k) { const uint32_t b = key_bit(k); g_key_bits[b >> 6] |= 1ull << (b & 63); }
std::vector<Region> g_regions;
std::vector<int> g_cells[16][32];            // 32 x 32-word cells -> regions overlapping them
std::vector<Region> g_placed;  // what the HD copy holds now (a later one may have overwritten part of an earlier one)
bool g_dirty = true;
// the part of VRAM changed since the last scan (words, exclusive max): only there can regions
// have come or gone (a level animating a texture uploads every other frame; a whole-VRAM scan
// each time cost ~20 ms a frame on a Steam Deck)
int g_ch_x0 = 0, g_ch_y0 = 0, g_ch_x1 = 1024, g_ch_y1 = 512;
int g_max_hw = 0, g_max_h = 0;  // the largest texture: how far from a change one may start

uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    uint32_t v = 0;
    if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
    return v;
}

bool read_disc_file(const char* name, std::vector<uint8_t>& out) {
    uint32_t lba = 0, size = 0;
    if (!cdrom_find_file(name, &lba, &size)) return false;
    out.resize((size + 2047) / 2048 * 2048);
    for (uint32_t s = 0; s < out.size() / 2048; s++)
        if (!cdrom_read_sector_user(lba + s, out.data() + s * 2048)) return false;
    out.resize(size);
    return true;
}

// A texture header (psx_textures.py header()): flag, colours, palette id, index, w, h.
struct Hdr {
    uint32_t ncol, pid, idx;
    int w, h, stride;
};
bool header(const std::vector<uint8_t>& b, size_t o, Hdr* hd) {
    if (o + 20 > b.size()) return false;
    const uint32_t flag = rd32(b, o), ncol = rd32(b, o + 4);
    int w = b[o + 16] | b[o + 17] << 8, h = b[o + 18] | b[o + 19] << 8;
    if (flag > 1 || (ncol != 16 && ncol != 256) || w <= 0 || w > 1024 || h <= 0 || h > 512) return false;
    const int stride = ncol == 256 ? (w + 1) / 2 * 2 : (w + 3) / 4 * 2;
    if (o + 20 + (size_t)stride * h > b.size()) return false;
    *hd = Hdr{ncol, rd32(b, o + 8), rd32(b, o + 12), w, h, stride};
    return true;
}

// The textures of one .psx file whose replacement exists.
void parse(const std::vector<uint8_t>& b, const std::string& dir, bool fonts) {
    // the texture table: a count, then offsets that all point at valid headers
    size_t table = 0;
    uint32_t n = 0;
    for (size_t o = 0; o + 8 <= b.size() && !table; o += 4) {
        n = rd32(b, o);
        if (n == 0 || n > 512 || o + 4 + 4 * (size_t)n > b.size() || rd32(b, o + 4) != o + 4 + 4 * n) continue;
        bool ok = true;
        Hdr hd;
        for (uint32_t i = 0; i < n && ok; i++) ok = header(b, rd32(b, o + 4 + 4 * i), &hd);
        if (ok) table = o;
    }
    if (!table) return;
    std::vector<Hdr> hdrs(n);
    std::vector<size_t> at(n);
    for (uint32_t i = 0; i < n; i++) at[i] = rd32(b, table + 4 + 4 * i), header(b, at[i], &hdrs[i]);
    // the palette tables end at the texture table: of the readings that fit, the one with most
    // of the textures' palettes
    std::unordered_map<uint32_t, std::vector<uint16_t>> best;
    size_t best_hits = 0;
    for (uint32_t n256 = 0; n256 < 512; n256++) {
        if (table < 4 + (size_t)n256 * 516 + 4) break;
        const size_t p256 = table - 4 - (size_t)n256 * 516;
        if (rd32(b, p256) != n256) continue;
        for (uint32_t n16 = 0; n16 < 1024; n16++) {
            if (p256 < 4 + (size_t)n16 * 36) break;
            const size_t p16 = p256 - 4 - (size_t)n16 * 36;
            if (rd32(b, p16) != n16) continue;
            std::unordered_map<uint32_t, std::vector<uint16_t>> pals;
            auto read = [&](size_t q, int colours) {
                std::vector<uint16_t> c(colours);
                std::memcpy(c.data(), b.data() + q + 4, colours * 2);
                pals[rd32(b, q)] = std::move(c);
            };
            for (uint32_t i = 0; i < n16; i++) read(p16 + 4 + i * 36, 16);
            for (uint32_t i = 0; i < n256; i++) read(p256 + 4 + i * 516, 256);
            size_t hits = 0;
            for (const Hdr& hd : hdrs) hits += pals.count(hd.pid);
            if (hits > best_hits) best_hits = hits, best = std::move(pals);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        const Hdr& hd = hdrs[i];
        char name[32];
        std::snprintf(name, sizeof name, "/%03u.png", hd.idx);
        const std::string png = dir + name;
        std::error_code ec;
        const bool have = !dir.empty() && std::filesystem::exists(png, ec);
        const uint32_t glyph = fonts ? font_glyph((int)hd.idx) : 0;
        if (!have && !glyph) continue;
        Tex t;
        t.png = have ? png : "";
        t.glyph = glyph;
        t.hw = hd.stride / 2;
        t.h = hd.h;
        t.texels_w = hd.w;
        t.depth = hd.ncol == 256 ? 1 : 0;
        auto pal = best.find(hd.pid);
        if (pal != best.end() && pal->second.size() == hd.ncol) t.pal = pal->second;
        t.data.resize((size_t)t.hw * t.h);
        std::memcpy(t.data.data(), b.data() + at[i] + 20, t.data.size() * 2);
        // the scan key: its first window of 4 words that is not all see-through
        bool found = false;
        for (int y = 0; y < t.h && !found; y++)
            for (int x = 0; x + 4 <= t.hw && !found; x++) {
                uint64_t k = 0;
                for (int j = 0; j < 4; j++) k = k << 16 | t.data[(size_t)y * t.hw + x + j];
                if (k) t.key_dx = x, t.key_dy = y, found = true, g_keys.emplace(k, (int)g_tex.size()), add_key_bit(k);
            }
        for (int x = 0; x < t.hw && !found; x++)
            for (int y = 0; y + 4 <= t.h && !found; y++) {
                uint64_t k = 0;
                for (int j = 0; j < 4; j++) k = k << 16 | t.data[(size_t)(y + j) * t.hw + x];
                if (k) t.key_dx = x, t.key_dy = y, found = true, g_keys_v.emplace(k, (int)g_tex.size()), add_key_bit(k);
            }
        if (found) {
            g_max_hw = std::max(g_max_hw, t.hw), g_max_h = std::max(g_max_h, t.h);
            g_tex.push_back(std::move(t));
        }
    }
}

void load_index() {
    g_loaded = true;
    std::error_code ec;
    std::vector<uint8_t> hed, wad;
    if (!read_disc_file("CD.HED", hed) || !read_disc_file("CD.WAD", wad)) return;
    // CD.HED: {name\0 padded to 4 bytes, u32 offset, u32 size}... ended by 0xFF
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> files;
    for (size_t p = 0; p < hed.size() && hed[p] != 0xFF;) {
        const size_t end = std::find(hed.begin() + p, hed.end(), 0) - hed.begin();
        std::string name(hed.begin() + p, hed.begin() + end);
        for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
        p = (end + 1 + 3) & ~(size_t)3;
        files[name] = {rd32(hed, p), rd32(hed, p + 4)};
        p += 8;
    }
    auto file = [&](const std::string& base, std::vector<uint8_t>& b) {
        auto it = files.find(base + ".psx");
        if (it == files.end() || (size_t)it->second.first + it->second.second > wad.size()) return false;
        b.assign(wad.begin() + it->second.first, wad.begin() + it->second.first + it->second.second);
        return true;
    };
    bool bits = false;  // (the game's fonts: also without a folder of replacements for bits.psx)
    for (const auto& e : std::filesystem::directory_iterator(kDir, ec)) {
        if (!e.is_directory()) continue;
        std::string base = e.path().filename().string();
        for (char& ch : base) ch = (char)std::tolower((unsigned char)ch);
        std::vector<uint8_t> b;
        if (!file(base, b)) continue;
        parse(b, std::string(kDir) + "/" + e.path().filename().string(), base == "bits");
        bits |= base == "bits";
    }
    if (std::vector<uint8_t> b; !bits && file("bits", b)) parse(b, "", true);
    font_loaded();  // (here, before the decoder threads ask)
    log_msg(LogCat::Host, "texture replacements: %zu textures", g_tex.size());
}

// Mip level `l` from level l-1 (w x h): 2x2 averages weighted by alpha, so see-through texels
// (black) don't darken the edges of cut-outs. Blocks stay inside one game texel up to level 2.
void make_mip(Tex& t, int l, int w, int h) {
    const std::vector<uint32_t>& s = t.hd[l - 1];
    std::vector<uint32_t>& d = t.hd[l];
    d.resize((size_t)(w / 2) * (h / 2));
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                const uint32_t px = s[(size_t)(y * 2 + k / 2) * w + x * 2 + k % 2];
                const int a = px >> 24;
                for (int c = 0; c < 3; c++) sum[c] += (int)((px >> (8 * c)) & 0xFF) * a;
                sum[3] += a;
            }
            uint32_t out = (uint32_t)((sum[3] + 2) / 4) << 24;
            if (sum[3])
                for (int c = 0; c < 3; c++) out |= (uint32_t)((sum[c] + sum[3] / 2) / sum[3]) << (8 * c);
            d[(size_t)y * (w / 2) + x] = out;
        }
}

// The replacement scaled to its texels x kScale (bilinear), and its mip levels.
// The replacement read and fitted (t.hd: the HD texels and their mips); false = unreadable. Runs on
// the decoder threads or the main thread, one texture at a time each (see load_hd).
bool decode_hd(Tex& t) {
    if (glyph_on(t)) return render_glyph(t) && (make_mip(t, 1, t.texels_w * kScale, t.h * kScale),
                                                 make_mip(t, 2, t.texels_w * kScale / 2, t.h * kScale / 2), true);
    if (t.png.empty()) return false;
    std::vector<uint32_t> src;
    int sw = 0, sh = 0;
    if (!replace_load_image(t.png, src, sw, sh) || sw <= 0 || sh <= 0) return false;
    // pure magenta is the game's see-through colour (its palettes hold 7C1F where VRAM gets 0000):
    // exports and their upscales have it opaque; near-magenta too (an upscaler's soft edges)
    for (uint32_t& px : src) {
        const int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
        if (r > 160 && b > 160 && g < 96 && std::abs(r - b) < 80) px = 0;
    }
    const int dw = t.texels_w * kScale, dh = t.h * kScale;
    auto mips = [&] {
        make_mip(t, 1, dw, dh);
        make_mip(t, 2, dw / 2, dh / 2);
        return true;
    };
    if (sw == dw && sh == dh) {
        t.hd[0] = std::move(src);
        return mips();
    }
    t.hd[0].resize((size_t)dw * dh);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            const float fx = std::clamp(((float)x + 0.5f) * sw / dw - 0.5f, 0.0f, (float)(sw - 1));
            const float fy = std::clamp(((float)y + 0.5f) * sh / dh - 0.5f, 0.0f, (float)(sh - 1));
            const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
            const float ax = fx - (float)x0, ay = fy - (float)y0;
            uint32_t out = 0;
            for (int c = 0; c < 4; c++) {
                auto ch = [&](int px, int py) { return (float)((src[(size_t)py * sw + px] >> (8 * c)) & 0xFF); };
                const float v = (ch(x0, y0) * (1 - ax) + ch(x1, y0) * ax) * (1 - ay) + (ch(x0, y1) * (1 - ax) + ch(x1, y1) * ax) * ay;
                out |= (uint32_t)std::clamp((int)(v + 0.5f), 0, 255) << (8 * c);
            }
            t.hd[0][(size_t)y * dw + x] = out;
        }
    return mips();
}

// Decoding a level's replacements (PNG reading, fitting, mips: ~4 ms each here, ~10 on a Steam Deck)
// when it first drew them cost 50-200 ms frames on a Deck: they are decoded on these threads as
// soon as a scan finds them in VRAM, so they are usually ready when the game draws them.
enum : uint8_t { kIdle, kQueued, kDecoding, kDone };
std::vector<uint8_t> g_state;  // per texture (under g_pool_mx)
std::vector<uint8_t> g_ok;     // decode_hd's result (kDone)
std::deque<int> g_queue;
std::mutex g_pool_mx;
std::condition_variable g_pool_cv, g_done_cv;
std::vector<std::thread> g_workers;
bool g_pool_quit = false;

void decoder() {
    for (;;) {
        int id;
        {
            std::unique_lock<std::mutex> lk(g_pool_mx);
            g_pool_cv.wait(lk, [] { return g_pool_quit || !g_queue.empty(); });
            if (g_pool_quit) return;
            id = g_queue.front();
            g_queue.pop_front();
            g_state[id] = kDecoding;
        }
        const bool ok = decode_hd(g_tex[id]);
        {
            std::lock_guard<std::mutex> lk(g_pool_mx);
            g_ok[id] = ok;
            g_state[id] = kDone;
        }
        g_done_cv.notify_all();
    }
}

// Quitting (std::exit): the decoders finish their texture and end before g_tex goes.
struct StopDecoders {
    ~StopDecoders() {
        {
            std::lock_guard<std::mutex> lk(g_pool_mx);
            g_pool_quit = true;
        }
        g_pool_cv.notify_all();
        for (std::thread& w : g_workers) w.join();
    }
} g_stop_decoders;

// Textures to decode ahead (the regions a scan found).
void queue_decode(const std::vector<int>& ids) {
    {
        std::lock_guard<std::mutex> lk(g_pool_mx);
        if (g_state.size() != g_tex.size()) g_state.assign(g_tex.size(), kIdle), g_ok.assign(g_tex.size(), 0);
        for (int id : ids)
            if (g_state[id] == kIdle && !g_tex[id].tried) g_state[id] = kQueued, g_queue.push_back(id);
        if (g_workers.empty() && !g_queue.empty()) {
            const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);  // (the game needs the rest)
            for (unsigned i = 0; i < n; i++) g_workers.emplace_back(decoder);
        }
    }
    g_pool_cv.notify_all();
}

// The replacement's HD texels, decoded by now: by a decoder thread (waiting for it if it is at it),
// else here.
bool load_hd(Tex& t) {
    if (t.tried) return !t.hd[0].empty();
    t.tried = true;
    const int id = (int)(&t - g_tex.data());
    bool ok = false, here = true;
    {
        std::unique_lock<std::mutex> lk(g_pool_mx);
        if (id < (int)g_state.size() && g_state[id] != kIdle) {
            if (g_state[id] == kQueued) {  // not started: take it
                g_queue.erase(std::find(g_queue.begin(), g_queue.end(), id));
                g_state[id] = kDecoding;
            } else {
                g_done_cv.wait(lk, [&] { return g_state[id] == kDone; });
                ok = g_ok[id], here = false;
            }
        }
    }
    if (here) {
        ok = decode_hd(t);
        std::lock_guard<std::mutex> lk(g_pool_mx);
        if (id < (int)g_state.size()) g_state[id] = kDone, g_ok[id] = ok;
    }
    if (!ok) {
        if (!t.png.empty()) log_msg(LogCat::Host, "texture replacements: cannot read %s", t.png.c_str());
        t.hd[0].clear();
    }
    return ok;
}

// texreplace_find's answers (a polygon's page, palette, texels and window -> drawn from the HD copy or
// not): a level draws the same textures over and over each frame, and finding the regions under
// every sampled texel (comparing palettes) cost ~15% of the CPU on a Steam Deck. They hold until
// their palette is written over, the regions change or the HD copy does.
struct FindKey {
    int v[13];
    bool operator==(const FindKey& o) const { return std::equal(v, v + 13, o.v); }
};
struct FindKeyHash {
    size_t operator()(const FindKey& k) const {
        uint64_t h = 0xCBF29CE484222325ull;
        for (int x : k.v) h = (h ^ (uint32_t)x) * 0x100000001B3ull;
        return (size_t)h;
    }
};
std::unordered_map<FindKey, int, FindKeyHash> g_found;

// Partial scans' results while only a few areas keep changing (animated textures: the game uploads
// their frames every other frame, in turns): (the changed area, the contents of all those areas) ->
// the regions found that overlap the change. Any other change starts over.
struct Rect {
    int x0, y0, x1, y1;
    bool operator==(const Rect& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};
std::vector<Rect> g_memo_rects;  // (at most 4)
std::unordered_map<uint64_t, std::vector<Region>> g_memo;

void scan(const uint16_t* vram) {
    g_dirty = false;
    const auto t0 = std::chrono::steady_clock::now();
    const int cx0 = std::max(g_ch_x0, 512), cy0 = std::max(g_ch_y0, 0), cx1 = std::min(g_ch_x1, 1024),
              cy1 = std::min(g_ch_y1, 512);
    auto changed = [&](int x, int y, int w, int h) { return x < cx1 && x + w > cx0 && y < cy1 && y + h > cy0; };
    // regions the change touched go (found again below if still there); the others stay
    const auto keep = std::stable_partition(g_regions.begin(), g_regions.end(), [&](const Region& r) {
        return !changed(r.x, r.y, g_tex[r.tex].hw, g_tex[r.tex].h);
    });
    std::vector<Region> removed(keep, g_regions.end());
    g_regions.erase(keep, g_regions.end());
    if (g_keys.empty() && g_keys_v.empty()) return;
    // every texture whose key is at (x, y), whose texels all match there and that overlaps the
    // change becomes a region
    std::vector<Region> found;
    auto try_keys = [&](const std::unordered_multimap<uint64_t, int>& keys, uint64_t k, int x, int y) {
        auto range = keys.equal_range(k);
        for (auto it = range.first; it != range.second; ++it) {
            const Tex& t = g_tex[it->second];
            const int ox = x - t.key_dx, oy = y - t.key_dy;
            if (ox < 512 || oy < 0 || ox + t.hw > 1024 || oy + t.h > 512) continue;
            if (!changed(ox, oy, t.hw, t.h)) continue;  // (kept above if it was there)
            bool same = true;
            for (int r = 0; r < t.h && same; r++)
                same = std::memcmp(vram + (oy + r) * 1024 + ox, t.data.data() + (size_t)r * t.hw, t.hw * 2) == 0;
            if (!same) continue;
            found.push_back({ox, oy, it->second});
        }
    };
    const bool whole = cx0 == 512 && cy0 == 0 && cx1 == 1024 && cy1 == 512;
    const Rect rect{cx0, cy0, cx1, cy1};
    if (std::find(g_memo_rects.begin(), g_memo_rects.end(), rect) == g_memo_rects.end()) {
        if (g_memo_rects.size() >= 4 || whole) g_memo_rects.clear();
        if (!whole) g_memo_rects.push_back(rect);
        g_memo.clear();  // (its keys did not cover this area's contents)
    }
    if (g_memo.size() > 256) g_memo.clear();
    uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a of the change and every tracked area's contents
    for (int v : {cx0, cy0, cx1, cy1}) hash = (hash ^ (uint32_t)v) * 0x100000001B3ull;
    if (!whole)
        for (const Rect& r : g_memo_rects)
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++) hash = (hash ^ vram[y * 1024 + x]) * 0x100000001B3ull;
    if (auto m = g_memo.find(hash); !whole && m != g_memo.end()) {
        found = m->second;
    } else {
        // a key lies inside its texture: within the largest texture's size of the change
        for (int y = std::max(cy0 - g_max_h, 0); y < std::min(cy1 + g_max_h, 512); y++)
            for (int x = std::max(cx0 - g_max_hw, 512); x < std::min(cx1 + g_max_hw, 1024); x++) {
                uint64_t k = 0;
                if (x + 4 <= 1024 && !g_keys.empty()) {
                    for (int i = 0; i < 4; i++) k = k << 16 | vram[y * 1024 + x + i];
                    if (k && maybe_key(k)) try_keys(g_keys, k, x, y);
                }
                if (y + 4 <= 512 && !g_keys_v.empty()) {
                    k = 0;
                    for (int i = 0; i < 4; i++) k = k << 16 | vram[(y + i) * 1024 + x];
                    if (k && maybe_key(k)) try_keys(g_keys_v, k, x, y);
                }
            }
        if (!whole) g_memo[hash] = found;
    }
    // in the order a scan of all of VRAM finds them (by key position, rows first; a 4-word key before
    // a 4-row one): which of two overlapping regions counts must not depend on what changed when
    auto order = [](const Region& r) {
        const Tex& t = g_tex[r.tex];
        return std::make_tuple(r.y + t.key_dy, r.x + t.key_dx, t.hw < 4, r.tex);
    };
    auto before = [&](const Region& a, const Region& b) { return order(a) < order(b); };
    std::sort(found.begin(), found.end(), before);
    auto same_region = [](const Region& a, const Region& b) { return a.x == b.x && a.y == b.y && a.tex == b.tex; };
    if (!std::equal(found.begin(), found.end(), removed.begin(), removed.end(), same_region)) {
        std::vector<int> ids;
        for (const Region& r : found) ids.push_back(r.tex);
        g_regions.insert(g_regions.end(), found.begin(), found.end());
        std::sort(g_regions.begin(), g_regions.end(), before);
        for (auto& row : g_cells)
            for (auto& c : row) c.clear();
        for (int id = 0; id < (int)g_regions.size(); id++) {
            const Region& r = g_regions[id];
            const Tex& t = g_tex[r.tex];
            for (int cy = r.y / 32; cy <= (r.y + t.h - 1) / 32; cy++)
                for (int cx = r.x / 32; cx <= (r.x + t.hw - 1) / 32; cx++) g_cells[cy][cx].push_back(id);
        }
        queue_decode(ids);
        g_found.clear();
    } else {
        g_regions.insert(g_regions.end(), removed.begin(), removed.end());  // (as they were: same order, same ids)
        std::inplace_merge(g_regions.begin(), g_regions.end() - (ptrdiff_t)removed.size(), g_regions.end(), before);
    }
    LOG(Host, "texture replacements: %zu found in VRAM (change %d,%d-%d,%d; %.2f ms)", g_regions.size(), cx0, cy0, cx1, cy1,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// The region of `depth` covering VRAM word (wx, wy) whose own palette is the one in use (the same
// texels can be in more than one file, or drawn with another palette: its colours are baked into
// the replacement). A texture whose palette was not found only counts if no other fits. -1: none.
int region_at(int wx, int wy, int depth, const uint16_t* clut) {
    if (wx < 512 || wx >= 1024 || wy < 0 || wy >= 512) return -1;
    int first = -1;
    for (int id : g_cells[wy / 32][wx / 32]) {
        const Region& r = g_regions[id];
        const Tex& t = g_tex[r.tex];
        if (t.depth != depth || wx < r.x || wy < r.y || wx >= r.x + t.hw || wy >= r.y + t.h) continue;
        if (!g_tex_replace && !glyph_on(t)) continue;  // (Texture replacements off: only the font)
        if (glyph_on(t)) return id;
        bool own = !t.pal.empty();
        for (size_t i = 0; i < t.pal.size() && own; i++) {
            // uploaded palettes get bit 15 set, and magenta (7C1F, the see-through colour) becomes 0
            const uint16_t want = (t.pal[i] & 0x7FFF) == 0x7C1F ? 0 : t.pal[i];
            own = ((clut[i] ^ want) & 0x7FFF) == 0;
        }
        if (own) return id;
        if (first < 0 && t.pal.empty()) first = id;
    }
    return first;
}

// The region's replacement into the HD copy at its place (once per texture and place).
bool place(int id) {
    const Region& r = g_regions[id];
    Tex& t = g_tex[r.tex];
    for (const Region& p : g_placed)
        if (p.x == r.x && p.y == r.y && p.tex == r.tex) return true;
    if (!load_hd(t)) return false;
    const int per = t.depth == 0 ? 4 : 2;
    const int ax = (r.x - 512) * per * kScale, ay = (r.y + (t.depth == 1 ? 512 : 0)) * kScale;
    for (int l = 0; l <= 2; l++)
        if (!hw_hd_upload(l, ax >> l, ay >> l, (t.texels_w * kScale) >> l, (t.h * kScale) >> l, t.hd[l].data())) return false;
    // the ones it drew over are no longer whole there
    g_placed.erase(std::remove_if(g_placed.begin(), g_placed.end(), [&](const Region& p) {
        const Tex& o = g_tex[p.tex];
        return o.depth == t.depth && p.x < r.x + t.hw && r.x < p.x + o.hw && p.y < r.y + t.h && r.y < p.y + o.h;
    }), g_placed.end());
    g_placed.push_back(r);
    g_found.clear();
    return true;
}

}  // namespace

// One file inside CD.WAD by name ("title.bmr"; replace.cpp: the full-screen pictures), read from
// the disc sector by sector.
bool texreplace_wad_file(const std::string& name, std::vector<uint8_t>& out) {
    std::vector<uint8_t> hed;
    uint32_t wad_lba = 0, wad_size = 0;
    if (!read_disc_file("CD.HED", hed) || !cdrom_find_file("CD.WAD", &wad_lba, &wad_size)) return false;
    for (size_t p = 0; p < hed.size() && hed[p] != 0xFF;) {
        const size_t end = std::find(hed.begin() + p, hed.end(), 0) - hed.begin();
        std::string n(hed.begin() + p, hed.begin() + end);
        for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
        p = (end + 1 + 3) & ~(size_t)3;
        const uint32_t off = rd32(hed, p), size = rd32(hed, p + 4);
        p += 8;
        if (n != name) continue;
        if ((uint64_t)off + size > wad_size) return false;
        std::vector<uint8_t> sec(2048);
        out.resize(size);
        for (uint32_t done = 0; done < size;) {
            const uint32_t at = off + done;
            if (!cdrom_read_sector_user(wad_lba + at / 2048, sec.data())) return false;
            const uint32_t n2 = std::min<uint32_t>(2048 - at % 2048, size - done);
            std::memcpy(out.data() + done, sec.data() + at % 2048, n2);
            done += n2;
        }
        return true;
    }
    return false;
}

void texreplace_vram_changed(int x, int y, int w, int h) {
    if (x + w > 1024 || y + h > 512) x = 0, y = 0, w = 1024, h = 512;  // (wrapped round: all of it)
    if (!g_dirty) g_ch_x0 = x, g_ch_y0 = y, g_ch_x1 = x + w, g_ch_y1 = y + h;
    else g_ch_x0 = std::min(g_ch_x0, x), g_ch_y0 = std::min(g_ch_y0, y), g_ch_x1 = std::max(g_ch_x1, x + w), g_ch_y1 = std::max(g_ch_y1, y + h);
    g_dirty = true;
}

// gpu_hw.cpp / gpu_vk.cpp: the renderer was shut down (its HD copy went with it): place again.
void texreplace_atlas_lost() { g_placed.clear(), g_found.clear(); }

void texreplace_vram_written(int x, int y, int w, int h) {
    if (g_found.empty()) return;
    if (x + w > 1024 || y + h > 512) return g_found.clear();  // (wrapped round)
    // only answers whose palette was written over can change (textures: through the regions)
    for (auto it = g_found.begin(); it != g_found.end();) {
        const int depth = it->first.v[2], cx = it->first.v[3] & 1023, cy = it->first.v[4] & 511;
        const bool hit = cy >= y && cy < y + h && cx < x + w && cx + (depth == 0 ? 16 : 256) > x;
        it = hit ? g_found.erase(it) : std::next(it);
    }
}

// gpu.cpp: a textured polygon or sprite of page (page_x, page_y, depth 0/1/2) and palette at
// (clut_x, clut_y) uses texels [u0, u1] x [v0, v1] of the page through the texture window tw
// (mask x, mask y, offset x, offset y): true if every texel it may use has a replacement (then it
// is drawn from the HD copy).
static int find_uncached(int page_x, int page_y, int depth, int per, const uint16_t* clut, float u0, float v0,
                         float u1, float v1, const int tw[4]);

int texreplace_find(int page_x, int page_y, int depth, int clut_x, int clut_y, float u0, float v0, float u1,
                    float v1, const int tw[4], const uint16_t* vram) {
    if ((!g_tex_replace && !g_game_font) || depth > 1) return 0;
    if (!g_loaded) load_index();
    if (g_tex.empty()) return 0;
    if (g_dirty) scan(vram);
    const int per = depth == 0 ? 4 : 2;
    if ((clut_x & 1023) + (depth == 0 ? 16 : 256) > 1024) return 0;
    const uint16_t* clut = vram + (clut_y & 511) * 1024 + (clut_x & 1023);
    const FindKey key{{page_x, page_y, depth, clut_x, clut_y, (int)std::floor(u0), (int)std::ceil(u1), (int)std::floor(v0),
                       (int)std::ceil(v1), tw[0], tw[1], tw[2], tw[3]}};
    if (auto it = g_found.find(key); it != g_found.end()) return it->second;
    if (g_found.size() > 8192) g_found.clear();
    const int hd = find_uncached(page_x, page_y, depth, per, clut, u0, v0, u1, v1, tw);
    g_found[key] = hd;  // (after: placing may have cleared the cache)
    return hd;
}

static int find_uncached(int page_x, int page_y, int depth, int per, const uint16_t* clut, float u0, float v0,
                         float u1, float v1, const int tw[4]) {
    // the texels to check: both ends and every 16th one between, through the window (the mask
    // bits of a coordinate take the offset's: a block of the page repeats)
    auto points = [](float a, float b, int mask, int off, std::vector<int>& out) {
        const int lo = (int)std::floor(a), hi = std::max(lo, (int)std::ceil(b) - 1);
        for (int t = lo;; t = std::min(t + 16, hi)) {
            int v = t & 255;
            v = (v & ~(mask * 8)) | ((off & mask) * 8);
            out.push_back(v);
            if (t == hi) break;
        }
    };
    static std::vector<int> us, vs, ids;
    us.clear(), vs.clear(), ids.clear();
    points(u0, u1, tw[0], tw[2], us);
    points(v0, v1, tw[1], tw[3], vs);
    for (int v : vs)
        for (int u : us) {
            const int id = region_at(page_x + u / per, page_y + v, depth, clut);
            if (id < 0) return 0;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    bool glyph = false;
    for (int id : ids) {
        if (!place(id)) return 0;
        glyph |= glyph_on(g_tex[g_regions[id].tex]);
    }
    return glyph ? 2 : 1;
}

// The Font setting changed: the letters are made again (or their replacement images read), and placed.
void texreplace_font_changed() {
    {
        std::lock_guard<std::mutex> lk(g_pool_mx);
        for (size_t id = 0; id < g_tex.size(); id++) {
            Tex& t = g_tex[id];
            if (!t.glyph || (id < g_state.size() && (g_state[id] == kQueued || g_state[id] == kDecoding))) continue;
            t.tried = false;
            for (auto& l : t.hd) l.clear();
            if (id < g_state.size()) g_state[id] = kIdle;
        }
    }
    g_placed.clear();
    g_found.clear();
}

std::string texreplace_font_name() { return font_loaded() ? g_ttf_name : ""; }

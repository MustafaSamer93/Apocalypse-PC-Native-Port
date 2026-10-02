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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"

bool replace_load_image(const std::string& path, std::vector<uint32_t>& rgba, int& w, int& h);  // replace.cpp

bool g_tex_replace = true;  // setting (settings.ini "texreplace"; on: whatever is in the folder is used)

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
};

struct Region {
    int x, y;  // VRAM words
    int tex;
};

bool g_loaded = false;
std::vector<Tex> g_tex;
std::unordered_multimap<uint64_t, int> g_keys;  // first non-zero window -> texture
// textures under 4 words wide (font letters, small icons): the first non-zero 4-row column instead
std::unordered_multimap<uint64_t, int> g_keys_v;
std::vector<Region> g_regions;
std::vector<int> g_cells[16][32];            // 32 x 32-word cells -> regions overlapping them
std::vector<Region> g_placed;  // what the HD copy holds now (a later one may have overwritten part of an earlier one)
bool g_dirty = true;

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
void parse(const std::vector<uint8_t>& b, const std::string& dir) {
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
        if (!std::filesystem::exists(png, ec)) continue;
        Tex t;
        t.png = png;
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
                if (k) t.key_dx = x, t.key_dy = y, found = true, g_keys.emplace(k, (int)g_tex.size());
            }
        for (int x = 0; x < t.hw && !found; x++)
            for (int y = 0; y + 4 <= t.h && !found; y++) {
                uint64_t k = 0;
                for (int j = 0; j < 4; j++) k = k << 16 | t.data[(size_t)(y + j) * t.hw + x];
                if (k) t.key_dx = x, t.key_dy = y, found = true, g_keys_v.emplace(k, (int)g_tex.size());
            }
        if (found) g_tex.push_back(std::move(t));
    }
}

void load_index() {
    g_loaded = true;
    std::error_code ec;
    if (!std::filesystem::is_directory(kDir, ec)) return;
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
    for (const auto& e : std::filesystem::directory_iterator(kDir, ec)) {
        if (!e.is_directory()) continue;
        std::string base = e.path().filename().string();
        for (char& ch : base) ch = (char)std::tolower((unsigned char)ch);
        auto it = files.find(base + ".psx");
        if (it == files.end() || (size_t)it->second.first + it->second.second > wad.size()) continue;
        std::vector<uint8_t> b(wad.begin() + it->second.first, wad.begin() + it->second.first + it->second.second);
        parse(b, std::string(kDir) + "/" + e.path().filename().string());
    }
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
bool load_hd(Tex& t) {
    if (t.tried) return !t.hd[0].empty();
    t.tried = true;
    std::vector<uint32_t> src;
    int sw = 0, sh = 0;
    if (!replace_load_image(t.png, src, sw, sh) || sw <= 0 || sh <= 0) {
        log_msg(LogCat::Host, "texture replacements: cannot read %s", t.png.c_str());
        return false;
    }
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

void scan(const uint16_t* vram) {
    g_dirty = false;
    g_regions.clear();
    for (auto& row : g_cells)
        for (auto& c : row) c.clear();
    if (g_keys.empty() && g_keys_v.empty()) return;
    // every texture whose key is at (x, y) and whose texels all match there becomes a region
    auto try_keys = [&](const std::unordered_multimap<uint64_t, int>& keys, uint64_t k, int x, int y) {
        auto range = keys.equal_range(k);
        for (auto it = range.first; it != range.second; ++it) {
            const Tex& t = g_tex[it->second];
            const int ox = x - t.key_dx, oy = y - t.key_dy;
            if (ox < 512 || oy < 0 || ox + t.hw > 1024 || oy + t.h > 512) continue;
            bool same = true;
            for (int r = 0; r < t.h && same; r++)
                same = std::memcmp(vram + (oy + r) * 1024 + ox, t.data.data() + (size_t)r * t.hw, t.hw * 2) == 0;
            if (!same) continue;
            const int id = (int)g_regions.size();
            g_regions.push_back({ox, oy, it->second});
            for (int cy = oy / 32; cy <= (oy + t.h - 1) / 32; cy++)
                for (int cx = ox / 32; cx <= (ox + t.hw - 1) / 32; cx++) g_cells[cy][cx].push_back(id);
        }
    };
    for (int y = 0; y < 512; y++)
        for (int x = 512; x < 1024; x++) {
            uint64_t k = 0;
            if (x + 4 <= 1024 && !g_keys.empty()) {
                for (int i = 0; i < 4; i++) k = k << 16 | vram[y * 1024 + x + i];
                if (k) try_keys(g_keys, k, x, y);
            }
            if (y + 4 <= 512 && !g_keys_v.empty()) {
                k = 0;
                for (int i = 0; i < 4; i++) k = k << 16 | vram[(y + i) * 1024 + x];
                if (k) try_keys(g_keys_v, k, x, y);
            }
        }
    LOG(Host, "texture replacements: %zu found in VRAM", g_regions.size());
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

void texreplace_vram_changed() { g_dirty = true; }

// gpu_hw.cpp / gpu_vk.cpp: the renderer was shut down (its HD copy went with it): place again.
void texreplace_atlas_lost() { g_placed.clear(); }

// gpu.cpp: a textured polygon or sprite of page (page_x, page_y, depth 0/1/2) and palette at
// (clut_x, clut_y) uses texels [u0, u1] x [v0, v1] of the page through the texture window tw
// (mask x, mask y, offset x, offset y): true if every texel it may use has a replacement (then it
// is drawn from the HD copy).
bool texreplace_find(int page_x, int page_y, int depth, int clut_x, int clut_y, float u0, float v0, float u1,
                     float v1, const int tw[4], const uint16_t* vram) {
    if (!g_tex_replace || depth > 1) return false;
    if (!g_loaded) load_index();
    if (g_tex.empty()) return false;
    if (g_dirty) scan(vram);
    const int per = depth == 0 ? 4 : 2;
    if ((clut_x & 1023) + (depth == 0 ? 16 : 256) > 1024) return false;
    const uint16_t* clut = vram + (clut_y & 511) * 1024 + (clut_x & 1023);
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
            if (id < 0) return false;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    for (int id : ids)
        if (!place(id)) return false;
    return true;
}

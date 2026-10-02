// Replacement pictures: a full-screen picture the game puts into VRAM (title, loading screens,
// logos: CD.WAD's .bmr / .rle files) is recognised by its contents and its replacement from
// assets/replacements/textures/screens/<name>.png (as tools/psx_textures.py names them) is drawn
// over it at full detail, so what the game draws on top (the blinking PRESS START) still appears.
// The originals are read from the disc at the first look, so any of them can be replaced.
//
// The replacement may have any size and shape: it is fitted to the shape the game's picture
// has on screen (4:3, or 16:9 / 16:10 in widescreen), cut at the edges where it is wider or
// taller. A 16:9 picture made from the original fills a widescreen display.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#else
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>
#endif

bool texreplace_wad_file(const std::string& name, std::vector<uint8_t>& out);

namespace {

struct Picture {
    std::string name;  // the original's file name without extension ("title", "load03")
    std::string file;  // its replacement
    uint32_t hash;     // FNV-1a of the original's 15-bit pixels (see content_hash)
    int w, h;          // the original's size in VRAM
    bool tried = false;
    int pw = 0, ph = 0;
    std::vector<uint32_t> rgba;  // loaded replacement (R G B A bytes)
    // fitted to the VRAM area at the last scale / screen shape
    int fit_scale = 0;
    float fit_aspect = 0;
    std::vector<uint32_t> fitted;
};

constexpr uint32_t kTitleHash = 0x865681EF;  // title.bmr, 512 x 240: "APOCALYPSE / PRESS START"
constexpr const char* kScreens = "assets/replacements/textures/screens";
std::vector<Picture> g_pictures;
bool g_scanned = false;

// The two frame buffers (y = 0 and 256): an upload or copy changed it since the last look (it
// may hold a new picture). Only writes into a buffer count: the picture is drawn once, and drawing
// on top of it (PRESS START) must stay.
bool g_check[2] = {};
bool g_replaced[2] = {};  // the buffer shows a replacement (a copy of it carries it along)
bool g_title[2] = {};     // the buffer holds the title picture (replaced or not)
// The buffer holds a full-screen picture the game uploaded (title, loading screen, logo), not a
// rendered scene: a few polygons on it (PRESS START, a loading bar) keep it, a 3D scene does not.
bool g_picture[2] = {};
int g_picture_polys[2] = {};
constexpr int kScenePolys = 200;

int buffer_of(int x, int y, int w, int h) {  // the frame buffer a rectangle touches, -1 = none
    for (int b = 0; b < 2; b++)
        if (x < 512 && x + w > 0 && y < b * 256 + 240 && y + h > b * 256) return b;
    return -1;
}

uint32_t content_hash(const uint16_t* vram, int x, int y, int w, int h) {
    uint32_t hash = 0x811C9DC5;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint16_t p = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] & 0x7FFF;
            hash = (hash ^ (p & 0xFF)) * 0x01000193;
            hash = (hash ^ (p >> 8)) * 0x01000193;
        }
    return hash;
}

#ifdef _WIN32
// PNG, JPEG, WebP, BMP ... through the Windows imaging component.
bool load_image(const std::wstring& path, std::vector<uint32_t>& out, int& w, int& h) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom));
    UINT uw = 0, uh = 0;
    if (ok) ok = SUCCEEDED(conv->GetSize(&uw, &uh)) && uw && uh && uw <= 16384 && uh <= 16384;
    if (ok) {
        out.resize((size_t)uw * uh);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, (UINT)(out.size() * 4), (BYTE*)out.data()));
        w = (int)uw;
        h = (int)uh;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
#else
// PNG, JPEG, BMP through stb_image (no WebP).
bool load_image(const std::wstring& path, std::vector<uint32_t>& out, int& w, int& h) {
    int n = 0;
    stbi_uc* px = stbi_load(std::filesystem::path(path).string().c_str(), &w, &h, &n, 4);
    if (!px) return false;
    const bool ok = w > 0 && h > 0 && w <= 16384 && h <= 16384;
    if (ok) out.assign((const uint32_t*)px, (const uint32_t*)px + (size_t)w * h);
    stbi_image_free(px);
    return ok;
}
#endif

bool load(Picture& p) {
    if (p.tried) return !p.rgba.empty();
    p.tried = true;
    if (load_image(std::filesystem::u8path(p.file).wstring(), p.rgba, p.pw, p.ph)) {
        log_msg(LogCat::Host, "replacement %s: %dx%d", p.file.c_str(), p.pw, p.ph);
        return true;
    }
    log_msg(LogCat::Host, "replacement %s: cannot read it", p.file.c_str());
    return false;
}

// The replacements in kScreens whose original is on the disc: its 15-bit pixels (.bmr raw
// 512 x 240; .rle "_RLE_16_", u32 size, u16 commands: bit 15 = repeat the next word, else copy
// that many) give the contents to look for.
void scan_pictures() {
    g_scanned = true;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(kScreens, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string(), name = e.path().stem().string();
        for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
        for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp" && ext != ".bmp") continue;
        std::vector<uint8_t> b;
        std::vector<uint16_t> words;
        if (texreplace_wad_file(name + ".bmr", b)) {
            words.resize(std::min<size_t>(b.size() / 2, 512 * 240));
            std::memcpy(words.data(), b.data(), words.size() * 2);
        } else if (texreplace_wad_file(name + ".rle", b) && b.size() >= 12 && std::memcmp(b.data(), "_RLE_16_", 8) == 0) {
            for (size_t o = 12; o + 2 <= b.size() && words.size() < 512 * 240;) {
                uint16_t c;
                std::memcpy(&c, b.data() + o, 2);
                o += 2;
                if (c & 0x8000) {
                    uint16_t v = 0;
                    if (o + 2 <= b.size()) std::memcpy(&v, b.data() + o, 2);
                    o += 2;
                    words.insert(words.end(), c & 0x7FFF, v);
                } else {
                    for (int i = 0; i < c && o + 2 <= b.size(); i++, o += 2) {
                        uint16_t v;
                        std::memcpy(&v, b.data() + o, 2);
                        words.push_back(v);
                    }
                }
            }
        }
        const int h = (int)std::min<size_t>(words.size() / 512, 240);
        if (h < 16) continue;  // not a screen on the disc
        std::vector<uint16_t> vram((size_t)1024 * h);  // as in VRAM: 1024-word rows
        for (int y = 0; y < h; y++) std::memcpy(&vram[(size_t)y * 1024], &words[(size_t)y * 512], 512 * 2);
        Picture p;
        p.name = name;
        p.file = e.path().u8string();
        p.hash = content_hash(vram.data(), 0, 0, 512, h);
        p.w = 512, p.h = h;
        g_pictures.push_back(std::move(p));
    }
    log_msg(LogCat::Host, "replacement screens: %zu", g_pictures.size());
}

// The replacement resampled to the VRAM area (w x h at `scale`) that shows at `aspect` on
// screen: the middle of the picture in that shape, area-averaged when shrinking, bilinear
// when enlarging.
void fit(Picture& p, int scale, float aspect) {
    if (p.fit_scale == scale && p.fit_aspect == aspect) return;
    p.fit_scale = scale;
    p.fit_aspect = aspect;
    const int ow = p.w * scale, oh = p.h * scale;
    // the whole height of the picture, centred: a narrower one (4:3 on a 16:9 screen) gets black
    // bars at the sides, a wider one loses its sides
    const float sh = (float)p.ph, sw = sh * aspect;  // the part of the source that is shown
    const float sx0 = ((float)p.pw - sw) * 0.5f, sy0 = 0.0f;
    const float fx = sw / (float)ow, fy = sh / (float)oh;  // source pixels per output pixel
    const int nx = std::max(1, (int)std::ceil(fx)), ny = std::max(1, (int)std::ceil(fy));
    p.fitted.assign((size_t)ow * oh, 0);
    auto src = [&](int x, int y, int c) {
        x = std::clamp(x, 0, p.pw - 1);
        y = std::clamp(y, 0, p.ph - 1);
        return (float)((p.rgba[(size_t)y * p.pw + x] >> (8 * c)) & 0xFF);
    };
    for (int j = 0; j < oh; j++) {
        for (int i = 0; i < ow; i++) {
            float acc[3] = {};
            for (int b = 0; b < ny; b++)
                for (int a = 0; a < nx; a++) {
                    // sample position in the source (pixel centres), then bilinear
                    float x = sx0 + ((float)i + ((float)a + 0.5f) / (float)nx) * fx - 0.5f;
                    if (x < -0.5f || x > (float)p.pw - 0.5f) continue;  // a side bar: black
                    float y = sy0 + ((float)j + ((float)b + 0.5f) / (float)ny) * fy - 0.5f;
                    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
                    float tx = x - (float)x0, ty = y - (float)y0;
                    for (int c = 0; c < 3; c++)
                        acc[c] += (src(x0, y0, c) * (1 - tx) + src(x0 + 1, y0, c) * tx) * (1 - ty) +
                                  (src(x0, y0 + 1, c) * (1 - tx) + src(x0 + 1, y0 + 1, c) * tx) * ty;
                }
            const float n = (float)(nx * ny);
            uint32_t px = 0;  // alpha 0: VRAM mask bit clear
            for (int c = 0; c < 3; c++) px |= (uint32_t)std::lround(std::clamp(acc[c] / n, 0.0f, 255.0f)) << (8 * c);
            p.fitted[(size_t)j * ow + i] = px;
        }
    }
}

}  // namespace

// texreplace.cpp: a replacement texture's pixels (R G B A bytes).
bool replace_load_image(const std::string& path, std::vector<uint32_t>& rgba, int& w, int& h) {
    return load_image(std::wstring(path.begin(), path.end()), rgba, w, h);
}

void note_upload(int x, int y, int w, int h, bool picture) {
    for (int b = 0; b < 2; b++)
        if (x < 512 && x + w > 0 && y < b * 256 + 240 && y + h > b * 256) {
            g_check[b] = true;
            g_replaced[b] = false;
            g_title[b] = false;
            g_picture[b] = picture;
            g_picture_polys[b] = 0;
        }
}
void replace_note_upload(int x, int y, int w, int h) { note_upload(x, y, w, h, h >= 200); }

// A VRAM copy: a whole replaced buffer copied onto the other keeps the replacement (and what
// the game then draws on it, PRESS START, must stay); other copies count as uploads.
void replace_note_copy(int sx, int sy, int dx, int dy, int w, int h) {
    const int from = buffer_of(sx, sy, w, h), to = buffer_of(dx, dy, w, h);
    // a copy onto itself changes nothing (the title does a 2x1 one after drawing PRESS START)
    if (to < 0 || (sx == dx && sy == dy)) return;
    const bool title = from >= 0 && from != to && g_title[from] && sx == 0 && dx == 0 && w >= 512 && h >= 240;
    if (from >= 0 && from != to && g_replaced[from] && sx == 0 && dx == 0 && sy % 256 == 0 && dy % 256 == 0 &&
        w >= 512 && h >= 240) {
        g_replaced[to] = true;
        g_check[to] = false;
        g_title[to] = title;
        g_picture[to] = g_picture[from], g_picture_polys[to] = g_picture_polys[from];
        return;
    }
    note_upload(dx, dy, w, h, from >= 0 && from != to && g_picture[from]);
    g_title[to] = title;
}

void replace_note_vram_reset() {
    g_check[0] = g_check[1] = true;
    g_replaced[0] = g_replaced[1] = false;
    g_title[0] = g_title[1] = false;
    g_picture[0] = g_picture[1] = false;
}

// The title screen's picture is in the frame buffer at `display_y` (host_sdl.cpp: the version label).
bool replace_title_shown(int display_y) { return g_title[(display_y >> 8) & 1]; }

// gpu.cpp: a polygon drawn with the drawing area at `area_y`.
void replace_note_poly(int area_y) {
    const int b = (area_y >> 8) & 1;
    if (g_picture[b] && ++g_picture_polys[b] > kScenePolys) g_picture[b] = false;
}

// The buffer at `display_y` shows one of the disc's own full-screen pictures (not replaced):
// it is shown at its own 4:3 shape, not stretched to a wide screen (host_sdl.cpp).
bool replace_original_picture_shown(int display_y) {
    const int b = (display_y >> 8) & 1;
    return g_picture[b] && !g_replaced[b];
}

// At each VBlank: after an upload into the frame buffers, look for a known picture there.
void replace_check(const uint16_t* vram) {
    if (!(g_check[0] || g_check[1]) || !hw_active()) return;
    // on screen the frame buffer has the display's shape: 4:3, widened in widescreen
    const float aspect = (4.0f / 3.0f) / g_gte_widescreen;
    for (int b = 0; b < 2; b++) {
        if (!g_check[b]) continue;
        g_check[b] = false;
        const int y = b * 256;
        const uint32_t full = content_hash(vram, 0, y, 512, 240);
        if (full == kTitleHash) g_title[b] = true;
        if (!g_tex_replace) continue;  // Texture replacements off: the disc's own pictures
        if (!g_scanned) scan_pictures();
        for (Picture& p : g_pictures) {
            if ((p.h == 240 ? full : content_hash(vram, 0, y, p.w, p.h)) != p.hash) continue;
            if (!load(p)) continue;
            fit(p, hw_scale(), aspect);
            hw_draw_picture(0, y, p.w, p.h, p.fitted.data());
            g_replaced[b] = true;
        }
    }
}

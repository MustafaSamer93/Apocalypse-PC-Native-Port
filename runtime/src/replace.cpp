// Replacement pictures: a full-screen picture the game puts into VRAM (the title screen) is
// recognised by its contents and a picture from assets\replace\ is drawn over it at full
// detail, so what the game draws on top (the blinking PRESS START) still appears.
//
// The replacement may have any size and shape: it is fitted to the shape the game's picture
// has on screen (4:3, or 16:9 / 16:10 in widescreen), cut at the edges where it is wider or
// taller. A 16:9 picture made from the original fills a widescreen display.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#endif

namespace {

struct Picture {
    const char* name;  // file name in assets\replace\ (without extension)
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

Picture g_pictures[] = {
    {"title", 0x865681EF, 512, 240},  // "APOCALYPSE / PRESS START"
};

// The two frame buffers (y = 0 and 256): an upload or copy changed it since the last look (it
// may hold a new picture). Only writes into a buffer count: the picture is drawn once, and drawing
// on top of it (PRESS START) must stay.
bool g_check[2] = {};
bool g_replaced[2] = {};  // the buffer shows a replacement (a copy of it carries it along)
bool g_title[2] = {};     // the buffer holds the title picture (replaced or not)

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
bool load_image(const std::wstring&, std::vector<uint32_t>&, int&, int&) { return false; }
#endif

bool load(Picture& p) {
    if (p.tried) return !p.rgba.empty();
    p.tried = true;
    for (const char* ext : {".png", ".webp", ".jpg", ".jpeg", ".bmp"}) {
        std::string file = std::string("assets/replacements/images/") + p.name + ext;
        if (load_image(std::wstring(file.begin(), file.end()), p.rgba, p.pw, p.ph)) {
            log_msg(LogCat::Host, "replacement %s: %dx%d", file.c_str(), p.pw, p.ph);
            return true;
        }
    }
    return false;
}

// The replacement resampled to the VRAM area (w x h at `scale`) that shows at `aspect` on
// screen: the middle of the picture in that shape, area-averaged when shrinking, bilinear
// when enlarging.
void fit(Picture& p, int scale, float aspect) {
    if (p.fit_scale == scale && p.fit_aspect == aspect) return;
    p.fit_scale = scale;
    p.fit_aspect = aspect;
    const int ow = p.w * scale, oh = p.h * scale;
    float sw = (float)p.pw, sh = (float)p.ph;  // the part of the source that is used
    if (sw / sh > aspect) sw = sh * aspect;
    else sh = sw / aspect;
    const float sx0 = ((float)p.pw - sw) * 0.5f, sy0 = ((float)p.ph - sh) * 0.5f;
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

void replace_note_upload(int x, int y, int w, int h) {
    for (int b = 0; b < 2; b++)
        if (x < 512 && x + w > 0 && y < b * 256 + 240 && y + h > b * 256) {
            g_check[b] = true;
            g_replaced[b] = false;
            g_title[b] = false;
        }
}

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
        return;
    }
    replace_note_upload(dx, dy, w, h);
    g_title[to] = title;
}

void replace_note_vram_reset() {
    g_check[0] = g_check[1] = true;
    g_replaced[0] = g_replaced[1] = false;
    g_title[0] = g_title[1] = false;
}

// The title screen's picture is in the frame buffer at `display_y` (host_sdl.cpp: the version label).
bool replace_title_shown(int display_y) { return g_title[(display_y >> 8) & 1]; }

// At each VBlank: after an upload into the frame buffers, look for a known picture there.
void replace_check(const uint16_t* vram) {
    if (!(g_check[0] || g_check[1]) || !hw_active()) return;
    // on screen the frame buffer has the display's shape: 4:3, widened in widescreen
    const float aspect = (4.0f / 3.0f) / g_gte_widescreen;
    for (int b = 0; b < 2; b++) {
        if (!g_check[b]) continue;
        g_check[b] = false;
        const int y = b * 256;
        for (Picture& p : g_pictures) {
            if (content_hash(vram, 0, y, p.w, p.h) != p.hash) continue;
            if (&p == &g_pictures[0]) g_title[b] = true;
            if (!load(p)) continue;
            fit(p, hw_scale(), aspect);
            hw_draw_picture(0, y, p.w, p.h, p.fitted.data());
            g_replaced[b] = true;
        }
    }
}

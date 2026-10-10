// Texture dump (Settings -> Dump textures): every texture page the game draws with - page, colour
// depth and palette - is written once as assets/dumps/textures/<level>/<hash>.png (colour 0 = see-through
// = alpha 0), for upscaling. The hash covers the page's texels and its palette, so the same
// picture always gets the same name. A page is looked at again only after an upload or copy
// changed VRAM there.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/psx.h"
#include "game/game.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#else
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#endif

bool g_tex_dump = std::getenv("APOC_TEX_DUMP") != nullptr;  // session only (not saved); APOC_TEX_DUMP=1 for tests

namespace {

uint32_t g_gen[16][2] = {};                       // per 64 x 256 VRAM page: bumped by uploads and copies
std::unordered_map<uint64_t, uint64_t> g_checked;  // (page, depth, palette) -> generations seen
std::unordered_map<uint64_t, bool> g_written;     // content hashes written this session
std::unordered_map<uint64_t, int> g_versions;     // (page, depth, palette) -> pictures written of it
// a page whose contents keep changing (an animated texture: menu flames, water, screens) was
// written again every frame of its animation; after this many versions it is left alone
constexpr int kMaxVersions = 3;
int g_count = 0;

#ifdef _WIN32
bool write_png(const std::wstring& path, const std::vector<uint32_t>& bgra, int w, int h) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize((UINT)w, (UINT)h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
              fmt == GUID_WICPixelFormat32bppBGRA &&
              SUCCEEDED(frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)(bgra.size() * 4), (BYTE*)bgra.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
#else
bool write_png(const std::wstring& path, const std::vector<uint32_t>& bgra, int w, int h) {
    std::vector<uint32_t> rgba(bgra.size());
    for (size_t i = 0; i < bgra.size(); i++)
        rgba[i] = (bgra[i] & 0xFF00FF00u) | (bgra[i] >> 16 & 0xFF) | (bgra[i] & 0xFF) << 16;
    return stbi_write_png(std::filesystem::path(path).string().c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}
#endif

uint32_t bgra_of(uint16_t c) {
    if (c == 0) return 0;  // see-through
    const uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return 0xFF000000u | (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
}

}  // namespace

// gpu.cpp: an upload or copy wrote VRAM here.
void texdump_vram_changed(int x, int y, int w, int h) {
    if (!g_tex_dump) return;
    for (int px = x / 64; px <= (x + w - 1) / 64 && px < 16; px++)
        for (int py = y / 256; py <= (y + h - 1) / 256 && py < 2; py++)
            if (px >= 0 && py >= 0) g_gen[px][py]++;
}

// gpu.cpp: a textured primitive uses this page (VRAM halfword units), depth 0/1/2 = 4/8/15 bit.
void texdump_note(int page_x, int page_y, int depth, int clut_x, int clut_y, const uint16_t* vram) {
    if (!g_tex_dump || page_x < 512) return;  // (x < 512: the frame buffers and movie pictures)
    const int words = depth == 0 ? 64 : depth == 1 ? 128 : 256, colours = depth == 0 ? 16 : depth == 1 ? 256 : 0;
    const int px = (page_x & 1023) / 64, py = (page_y & 511) / 256;
    uint64_t gens = 0;
    for (int i = 0; i < words / 64 && px + i < 16; i++) gens = gens * 1000003u + g_gen[px + i][py];
    const uint64_t key = (uint64_t)page_x | (uint64_t)page_y << 10 | (uint64_t)depth << 20 |
                         (uint64_t)(colours ? clut_x : 0) << 22 | (uint64_t)(colours ? clut_y : 0) << 32;
    auto it = g_checked.find(key);
    if (it != g_checked.end() && it->second == gens) return;
    g_checked[key] = gens;
    // the picture and its hash
    uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&](uint16_t v) { hash = (hash ^ v) * 0x100000001B3ull; };
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < words; x++) mix(vram[((page_y + y) & 511) * 1024 + ((page_x + x) & 1023)]);
    for (int i = 0; i < colours; i++) mix(vram[(clut_y & 511) * 1024 + ((clut_x + i) & 1023)]);
    mix((uint16_t)depth);
    if (g_written.count(hash) || g_versions[key] >= kMaxVersions) return;
    g_written[hash] = true;
    g_versions[key]++;
    std::vector<uint32_t> img(256 * 256);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            const uint16_t* row = vram + ((page_y + y) & 511) * 1024;
            uint16_t c;
            if (depth == 2) {
                c = row[(page_x + x) & 1023];
            } else {
                const uint16_t w = row[(page_x + (depth == 0 ? x / 4 : x / 2)) & 1023];
                const int idx = depth == 0 ? (w >> (4 * (x & 3))) & 15 : (w >> (8 * (x & 1))) & 255;
                c = vram[(clut_y & 511) * 1024 + ((clut_x + idx) & 1023)];
            }
            img[(size_t)y * 256 + x] = bgra_of(c);
        }
    const std::string dir = "assets/dumps/textures/" + (g_sim60_level.empty() ? std::string("menu") : g_sim60_level);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[64];
    std::snprintf(name, sizeof name, "/%016llx.png", (unsigned long long)hash);
    const std::string file = dir + name;
    if (std::filesystem::exists(file, ec)) return;
    if (write_png(std::wstring(file.begin(), file.end()), img, 256, 256) && ++g_count % 50 == 1)
        log_msg(LogCat::Host, "texture dump: %d pictures so far (%s)", g_count, file.c_str());
}

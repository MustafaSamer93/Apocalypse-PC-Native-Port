// Vulkan PS1 renderer: the same design as the OpenGL one (gpu_hw.cpp), whose batching and
// dirty-region logic it repeats. VRAM lives in two RGBA8 images of (1024*S)x(512*S): `draw`
// (render target; multisampled with MSAA) and `read` (sampled for textures and palettes, and
// shown). VRAM row 0 is image row 0 here (the OpenGL version keeps it at the bottom), so all
// rectangles, scissors and texel coordinates use the same numbers as the OpenGL code.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"
#include "render_backend.h"
#include "vk_context.h"

#include "copy.frag.h"
#include "copy.vert.h"
#include "vram.frag.h"
#include "vram.vert.h"

namespace {

int S = 0;  // scale; 0 = not initialised
int g_samples_wanted = 1, g_samples = 1;
bool g_filter = false;

VkImg g_draw, g_read, g_stage;  // stage: native 1024x512, for uploads
VkShaderModule g_vs, g_fs, g_copy_vs, g_copy_fs;
VkPipelineLayout g_layout = VK_NULL_HANDLE, g_copy_layout = VK_NULL_HANDLE;
VkPipeline g_pipes[5] = {};          // opaque, then semi modes 0..3
VkPipeline g_copy_pipe = VK_NULL_HANDLE;  // copy pass into `draw` (its sample count)
VkImg g_hd;  // replacement textures' atlas (texreplace.cpp), sampler set 1; made on first use
constexpr int kHdW = 8192, kHdH = 4096;  // VRAM's texture area x 4 (4-bit above, 8-bit below)
// Z-buffer: depth image beside `draw` (same size and sample count); HwPrim::ztest batches test
// and write it, cleared for each frame (vk_hw_depth_reset).
VkImg g_zbuf;
bool g_zbuf_clear = true;
void begin_draw() { vkc::begin_rendering(g_draw, g_zbuf.image ? &g_zbuf : nullptr); }

struct VkVtx {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t info0, info1;
    float w;  // 1, or view depth for perspective-correct texturing (HwVtx::w)
    uint32_t box;  // HwPrim::box
    float z;  // Z-buffer depth (HwVtx::z)
};
static_assert(sizeof(VkVtx) == 40, "vertex layout (vk_context.cpp kVtxStride)");

struct VramPush {
    int32_t scale, pass, set_mask, filter;
    int32_t texwin[4];
};
struct CopyPush {
    int32_t scale, pad, off[2];
};

// Primitives are batched while the state below stays the same (as gpu_hw.cpp).
struct BatchKey {
    bool semi = false;
    int semi_mode = 0;
    bool set_mask = false;
    int ztest = 0;      // HwPrim::ztest
    int tw[4] = {};
    int sc[4] = {};  // scissor x1,y1,x2,y2 (native, inclusive)
    bool operator==(const BatchKey& o) const {
        return semi == o.semi && (!semi || semi_mode == o.semi_mode) && set_mask == o.set_mask &&
               ztest == o.ztest &&
               std::memcmp(tw, o.tw, sizeof tw) == 0 && std::memcmp(sc, o.sc, sizeof sc) == 0;
    }
};
std::vector<VkVtx> g_batch;
BatchKey g_key;

bool g_dirty = false;  // region drawn since `read` was last synchronised (native, exclusive max)
int g_dx0, g_dy0, g_dx1, g_dy1;

uint32_t rgba_of(uint16_t p) {
    uint32_t r = (p & 31) * 255 / 31, g = ((p >> 5) & 31) * 255 / 31, b = ((p >> 10) & 31) * 255 / 31;
    uint32_t a = (p & 0x8000) ? 255 : 0;
    return r | g << 8 | b << 16 | a << 24;
}

void mark_dirty(int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, 1024); y1 = std::min(y1, 512);
    if (x0 >= x1 || y0 >= y1) return;
    if (!g_dirty) {
        g_dirty = true;
        g_dx0 = x0; g_dy0 = y0; g_dx1 = x1; g_dy1 = y1;
    } else {
        g_dx0 = std::min(g_dx0, x0); g_dy0 = std::min(g_dy0, y0);
        g_dx1 = std::max(g_dx1, x1); g_dy1 = std::max(g_dy1, y1);
    }
}

bool overlaps_dirty(int x0, int y0, int x1, int y1) {
    return g_dirty && x0 < g_dx1 && x1 > g_dx0 && y0 < g_dy1 && y1 > g_dy0;
}

VkCommandBuffer cb() { return vkc::cmd(); }

void set_viewport_scissor(int sx, int sy, int sw, int sh) {
    VkViewport vp{0, 0, (float)(1024 * S), (float)(512 * S), 0, 1};
    VkRect2D sc{{sx, sy}, {(uint32_t)std::max(sw, 0), (uint32_t)std::max(sh, 0)}};
    vkCmdSetViewport(cb(), 0, 1, &vp);
    vkCmdSetScissor(cb(), 0, 1, &sc);
}

// APOC_PROFILE: what the renderer did, logged every 2 seconds (vk_profile_frame).
struct Prof {
    uint64_t flushes, verts, syncs, sync_px, readbacks, read_px, uploads, up_px, copies, caps;
} g_prof;

void flush() {
    if (g_batch.empty()) return;
    g_prof.flushes++;
    g_prof.verts += g_batch.size();
    // textures come from `read`; once it is readable, batches follow each other in one rendering
    VkImg& tex = g_read;
    VkImg& hd = g_hd.image ? g_hd : tex;  // (set 1 must hold an image even with no replacements)
    for (VkImg* img : {&tex, &hd})
        if (img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            vkc::end_rendering();
            vkc::transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    begin_draw();
    const size_t bytes = g_batch.size() * sizeof(VkVtx);
    vkc::Staging st = vkc::stage(bytes);
    std::memcpy(st.ptr, g_batch.data(), bytes);
    VkCommandBuffer c = cb();
    if (g_zbuf.image) {
        const bool test = g_key.ztest != 0;
        if (test && g_zbuf_clear) {  // a new frame: far away everywhere
            VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
            ca.clearValue.depthStencil = {1.0f, 0};
            VkClearRect cr{{{0, 0}, {(uint32_t)(1024 * S), (uint32_t)(512 * S)}}, 0, 1};
            vkCmdClearAttachments(c, 1, &ca, 1, &cr);
            g_zbuf_clear = false;
        }
        vkCmdSetDepthTestEnable(c, test);
        vkCmdSetDepthWriteEnable(c, g_key.ztest == 1);
    }
    vkCmdBindVertexBuffers(c, 0, 1, &st.buffer, &st.offset);
    const VkDescriptorSet sets[2] = {vkc::sampler_set(tex, false), vkc::sampler_set(hd, true)};
    set_viewport_scissor(g_key.sc[0] * S, g_key.sc[1] * S, (g_key.sc[2] - g_key.sc[0] + 1) * S,
                         (g_key.sc[3] - g_key.sc[1] + 1) * S);
    VramPush pc{S, 0, g_key.set_mask ? 1 : 0, g_filter ? 1 : 0, {g_key.tw[0], g_key.tw[1], g_key.tw[2], g_key.tw[3]}};
    auto draw = [&](VkPipeline pipe, int pass) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 2, sets, 0, nullptr);
        pc.pass = pass;
        vkCmdPushConstants(c, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(c, (uint32_t)g_batch.size(), 1, 0, 0);
    };
    if (!g_key.semi) {
        draw(g_pipes[0], 0);
    } else {
        float k = g_key.semi_mode == 0 ? 0.5f : 0.25f;
        const float consts[4] = {0, 0, 0, k};
        vkCmdSetBlendConstants(c, consts);
        draw(g_pipes[0], 1);
        draw(g_pipes[1 + (g_key.semi_mode & 3)], 2);
    }
    g_batch.clear();
}

// Makes `read` match `draw` for the region drawn since the last sync.
void sync_read() {
    flush();
    if (!g_dirty) return;
    g_prof.syncs++;
    g_prof.sync_px += (uint64_t)(g_dx1 - g_dx0) * (g_dy1 - g_dy0);
    vkc::end_rendering();
    vkc::transition(g_draw, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkc::transition(g_read, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkOffset3D o{g_dx0 * S, g_dy0 * S, 0};
    VkExtent3D e{(uint32_t)((g_dx1 - g_dx0) * S), (uint32_t)((g_dy1 - g_dy0) * S), 1};
    VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (g_samples > 1) {
        VkImageResolve r{sub, o, sub, o, e};
        vkCmdResolveImage(cb(), g_draw.image, g_draw.layout, g_read.image, g_read.layout, 1, &r);
    } else {
        VkImageCopy r{sub, o, sub, o, e};
        vkCmdCopyImage(cb(), g_draw.image, g_draw.layout, g_read.image, g_read.layout, 1, &r);
    }
    g_dirty = false;
}

// Before a textured primitive: if it samples VRAM drawn since the last sync, sync first.
void prepare_textures(const HwPrim& p) {
    if (!p.textured || !g_dirty) return;
    int pw = p.depth == 0 ? 64 : p.depth == 1 ? 128 : 256;
    bool hit = overlaps_dirty(p.page_x, p.page_y, p.page_x + pw, p.page_y + 256);
    if (p.depth < 2) hit = hit || overlaps_dirty(p.clut_x, p.clut_y, p.clut_x + (p.depth == 0 ? 16 : 256), p.clut_y + 1);
    if (hit) sync_read();
}

void begin_prim(const HwPrim& p, const HwState& s) {
    prepare_textures(p);
    BatchKey k;
    k.semi = p.semi;
    k.semi_mode = p.semi_mode;
    k.set_mask = s.set_mask;
    k.ztest = p.ztest;
    k.tw[0] = s.tw_mask_x; k.tw[1] = s.tw_mask_y; k.tw[2] = s.tw_off_x; k.tw[3] = s.tw_off_y;
    k.sc[0] = s.area_x1; k.sc[1] = s.area_y1; k.sc[2] = s.area_x2; k.sc[3] = s.area_y2;
    if (!(k == g_key)) {
        flush();
        g_key = k;
    }
}

uint32_t info0(const HwPrim& p) {
    return (uint32_t)(p.page_x & 1023) | (uint32_t)(p.page_y & 511) << 10 | (uint32_t)p.depth << 20 |
           (uint32_t)p.textured << 22 | (uint32_t)p.raw << 23 | (uint32_t)p.smooth << 24 |
           (uint32_t)p.hd << 27 | (uint32_t)p.glyph << 28;
}
uint32_t info1(const HwPrim& p) { return (uint32_t)(p.clut_x & 1023) | (uint32_t)(p.clut_y & 511) << 10; }

void push(float x, float y, int r, int g, int b, float u, float v, const HwPrim& p, float w = 1.0f, float z = 0.0f) {
    g_batch.push_back(VkVtx{x, y, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255, u, v, info0(p), info1(p), w, p.box, z});
}

void draw_area_dirty(int x0, int y0, int x1, int y1, const HwState& s) {
    mark_dirty(std::max(x0, s.area_x1), std::max(y0, s.area_y1), std::min(x1, s.area_x2 + 1),
               std::min(y1, s.area_y2 + 1));
}

// Draws texels of `src` into `draw` over the native rectangle x, y, w, h: target pixel p takes
// texel (p - off) / scale (copy.frag). Used where a copy cannot write (a multisampled target).
void draw_copy(VkImg& src, int offx, int offy, int scale, int x, int y, int w, int h) {
    vkc::end_rendering();
    vkc::transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_draw();
    VkVtx q[6] = {};
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    float c[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 6; i++) { q[i].x = c[i][0]; q[i].y = c[i][1]; }
    vkc::Staging st = vkc::stage(sizeof q);
    std::memcpy(st.ptr, q, sizeof q);
    VkCommandBuffer cmd = cb();
    VkDescriptorSet set = vkc::sampler_set(src, false);
    set_viewport_scissor(0, 0, 1024 * S, 512 * S);
    if (g_zbuf.image) vkCmdSetDepthTestEnable(cmd, VK_FALSE), vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_copy_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_copy_layout, 0, 1, &set, 0, nullptr);
    CopyPush pc{scale, 0, {offx, offy}};
    vkCmdPushConstants(cmd, g_copy_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &st.buffer, &st.offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

// Copies a scaled rectangle between two single-sample images (native coordinates).
void copy_region(VkImg& src, int sx, int sy, VkImg& dst, int dx, int dy, int w, int h) {
    vkc::end_rendering();
    vkc::transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkc::transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageCopy r{sub, {sx, sy, 0}, sub, {dx, dy, 0}, {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyImage(cb(), src.image, src.layout, dst.image, dst.layout, 1, &r);
}

// Uploads a native rectangle of the CPU VRAM copy into both images.
void upload_rect(int x, int y, int w, int h, const uint16_t* vram) {
    vkc::Staging st = vkc::stage((VkDeviceSize)w * h * 4);
    uint32_t* px = (uint32_t*)st.ptr;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px[(size_t)j * w + i] = rgba_of(vram[(y + j) * 1024 + x + i]);
    vkc::end_rendering();
    vkc::transition(g_stage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic{};
    bic.bufferOffset = st.offset;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageOffset = {x, y, 0};
    bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb(), st.buffer, g_stage.image, g_stage.layout, 1, &bic);
    // nearest-neighbour upscale into `read` (and `draw` unless it is multisampled)
    vkc::transition(g_stage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageBlit blit{sub, {{x, y, 0}, {x + w, y + h, 1}}, sub, {{x * S, y * S, 0}, {(x + w) * S, (y + h) * S, 1}}};
    vkc::transition(g_read, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdBlitImage(cb(), g_stage.image, g_stage.layout, g_read.image, g_read.layout, 1, &blit, VK_FILTER_NEAREST);
    if (g_samples == 1) {
        vkc::transition(g_draw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(cb(), g_stage.image, g_stage.layout, g_draw.image, g_draw.layout, 1, &blit, VK_FILTER_NEAREST);
    } else {
        draw_copy(g_stage, 0, 0, S, x, y, w, h);
    }
}

// Capture slots for extra frames (smooth.cpp) and the stash they draw over.
struct Capture {
    VkImg img;
};
Capture g_caps[16];
Capture g_stash;
int g_stash_x, g_stash_y, g_stash_w, g_stash_h;

// Copies a native VRAM rectangle (as drawn so far) into `c`, resized as needed.
void copy_out(Capture& c, int x, int y, int w, int h) {
    sync_read();
    if (c.img.w != w * S || c.img.h != h * S) {
        vkc::destroy_image(c.img);
        vkc::create_image(c.img, w * S, h * S, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vkc::register_texture(&c.img);
    }
    copy_region(g_read, x * S, y * S, c.img, 0, 0, w * S, h * S);
    vkc::transition(c.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool clip_rect(int& x, int& y, int& w, int& h) {
    x = std::clamp(x, 0, 1023);
    y = std::clamp(y, 0, 511);
    w = std::min(w, 1024 - x);
    h = std::min(h, 512 - y);
    return w > 0 && h > 0;
}

void destroy_pipelines() {
    for (VkPipeline& p : g_pipes)
        if (p) vkDestroyPipeline(vkc::device, p, nullptr), p = VK_NULL_HANDLE;
    if (g_copy_pipe) vkDestroyPipeline(vkc::device, g_copy_pipe, nullptr), g_copy_pipe = VK_NULL_HANDLE;
}

}  // namespace

bool vk_hw_init(int scale) {
    if (!vkc::ready()) return false;
    if (!g_layout) {
        g_vs = vkc::shader(kSpv_vram_vert, sizeof kSpv_vram_vert);
        g_fs = vkc::shader(kSpv_vram_frag, sizeof kSpv_vram_frag);
        g_copy_vs = vkc::shader(kSpv_copy_vert, sizeof kSpv_copy_vert);
        g_copy_fs = vkc::shader(kSpv_copy_frag, sizeof kSpv_copy_frag);
        g_layout = vkc::make_layout(sizeof(VramPush), 2);
        g_copy_layout = vkc::make_layout(sizeof(CopyPush));
        vkc::create_image(g_stage, 1024, 512, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    vk_hw_shutdown();
    S = std::clamp(scale, 1, 9);
    const int W = 1024 * S, H = 512 * S;
    if (!vkc::create_image(g_read, W, H, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        log_msg(LogCat::Gpu, "Vulkan renderer: %dx VRAM image not supported", S);
        S = 0;
        return false;
    }
    vkc::register_texture(&g_read);
    // MSAA: lower the sample count if the GPU offers fewer or the image would take more than a
    // quarter of its video memory (at least 512 MB: integrated GPUs report little of their own)
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vkc::gpu, &props);
    const VkSampleCountFlags supported = props.limits.framebufferColorSampleCounts;
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(vkc::gpu, &mem);
    VkDeviceSize vram = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, mem.memoryHeaps[i].size);
    const uint64_t budget = std::max<uint64_t>(512ull << 20, vram / 4);
    log_msg(LogCat::Gpu, "Vulkan: %llu MB video memory, MSAA image up to %llu MB", (unsigned long long)(vram >> 20),
            (unsigned long long)(budget >> 20));
    int n = g_samples_wanted;
    while (n > 1 && (!(supported & (VkSampleCountFlags)n) || (uint64_t)W * H * 4 * (uint64_t)n > budget)) n /= 2;
    const VkImageUsageFlags draw_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    while (!vkc::create_image(g_draw, W, H, VK_FORMAT_R8G8B8A8_UNORM, draw_usage, (VkSampleCountFlagBits)n)) {
        if (n == 1) {
            log_msg(LogCat::Gpu, "Vulkan renderer: %dx draw image not supported", S);
            vkc::destroy_image(g_read);
            S = 0;
            return false;
        }
        n /= 2;
    }
    g_samples = n;
    const VkSampleCountFlagBits sc = (VkSampleCountFlagBits)n;
    if (!vkc::create_image(g_zbuf, W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, sc)) {
        g_zbuf = VkImg{};
        log_msg(LogCat::Gpu, "Vulkan renderer: no depth buffer (Z-buffer off)");
    }
    const VkFormat zfmt = g_zbuf.image ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
    g_zbuf_clear = true;
    const vkc::Blend modes[5] = {vkc::Blend::None, vkc::Blend::Half, vkc::Blend::Add, vkc::Blend::Subtract,
                                 vkc::Blend::AddQuarter};
    for (int i = 0; i < 5; i++)
        g_pipes[i] = vkc::make_pipeline({g_vs, g_fs, g_layout, VK_FORMAT_R8G8B8A8_UNORM, sc, modes[i], vkc::Vertices::Vram, zfmt});
    g_copy_pipe = vkc::make_pipeline(
        {g_copy_vs, g_copy_fs, g_copy_layout, VK_FORMAT_R8G8B8A8_UNORM, sc, vkc::Blend::None, vkc::Vertices::Position, zfmt});
    // start from black (the caller uploads the CPU copy of VRAM next)
    const VkClearColorValue black{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (VkImg* img : {&g_draw, &g_read}) {
        vkc::transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb(), img->image, img->layout, &black, 1, &all);
    }
    g_dirty = false;
    g_batch.clear();
    g_key = BatchKey{};
    log_msg(LogCat::Gpu, "Vulkan renderer: %dx (%dx%d VRAM), %dx MSAA", S, W, H, g_samples);
    return true;
}

void vk_hw_shutdown() {
    if (!S) return;
    vkc::submit_wait();
    vkDeviceWaitIdle(vkc::device);
    for (auto& c : g_caps) vkc::destroy_image(c.img);
    vkc::destroy_image(g_stash.img);
    vkc::destroy_image(g_draw);
    vkc::destroy_image(g_read);
    vkc::destroy_image(g_zbuf);
    if (g_hd.image) vkc::destroy_image(g_hd), texreplace_atlas_lost();
    destroy_pipelines();
    g_batch.clear();
    S = 0;
}

// A replacement texture into the atlas at (x, y) (texreplace.cpp places them).
bool vk_hw_hd_upload(int level, int x, int y, int w, int h, const uint32_t* rgba) {
    if (!S || level < 0 || level > 2 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > kHdW >> level || y + h > kHdH >> level)
        return false;
    flush();
    if (!g_hd.image && !vkc::create_image(g_hd, kHdW, kHdH, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, 3))
        return false;
    const VkDeviceSize row_bytes = (VkDeviceSize)w * 4;
    const int band = (int)std::max<VkDeviceSize>(1, vkc::stage_limit() / row_bytes);
    for (int r0 = 0; r0 < h; r0 += band) {
        const int rows = std::min(band, h - r0);
        vkc::Staging st = vkc::stage(row_bytes * rows);
        std::memcpy(st.ptr, rgba + (size_t)r0 * w, row_bytes * rows);
        vkc::end_rendering();
        vkc::transition(g_hd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic{};
        bic.bufferOffset = st.offset;
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1};
        bic.imageOffset = {x, y + r0, 0};
        bic.imageExtent = {(uint32_t)w, (uint32_t)rows, 1};
        vkCmdCopyBufferToImage(cb(), st.buffer, g_hd.image, g_hd.layout, 1, &bic);
    }
    return true;
}

bool vk_hw_active() { return S != 0; }
int vk_hw_scale() { return S; }
int vk_hw_samples() { return S ? g_samples : 0; }
void vk_hw_set_samples(int n) { g_samples_wanted = std::clamp(n, 1, 8); }
int vk_hw_samples_wanted() { return g_samples_wanted; }
void vk_hw_set_texture_filter(bool on) {
    if (on != g_filter) flush();
    g_filter = on;
}

// ================================================================ Z-buffer

void vk_hw_depth_reset() { g_zbuf_clear = true; }
bool vk_hw_zbuffer_ready() { return S && g_zbuf.image; }

// ================================================================ primitives

void vk_hw_triangle(const HwVtx* a, const HwVtx* b, const HwVtx* c, const HwPrim& p, const HwState& s) {
    begin_prim(p, s);
    for (const HwVtx* v : {a, b, c}) {
        const HwVtx* col = p.gouraud ? v : a;
        push((float)v->x, (float)v->y, col->r, col->g, col->b, (float)v->u, (float)v->v, p, v->w, v->z);
    }
    draw_area_dirty((int)std::floor(std::min({a->x, b->x, c->x})), (int)std::floor(std::min({a->y, b->y, c->y})),
                    (int)std::ceil(std::max({a->x, b->x, c->x})) + 1, (int)std::ceil(std::max({a->y, b->y, c->y})) + 1, s);
}

void vk_hw_rect(int x, int y, int w, int h, int u0, int v0, bool flip_x, bool flip_y, uint32_t color, const HwPrim& p,
                const HwState& s) {
    if (w <= 0 || h <= 0) return;
    begin_prim(p, s);
    int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    float ua = (float)u0, va = (float)v0;
    float ub = flip_x ? ua - (float)w : ua + (float)w, vb = flip_y ? va - (float)h : va + (float)h;
    push(x0, y0, r, g, b, ua, va, p);
    push(x1, y0, r, g, b, ub, va, p);
    push(x0, y1, r, g, b, ua, vb, p);
    push(x1, y0, r, g, b, ub, va, p);
    push(x1, y1, r, g, b, ub, vb, p);
    push(x0, y1, r, g, b, ua, vb, p);
    draw_area_dirty(x, y, x + w, y + h, s);
}

void vk_hw_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gouraud, const HwPrim& p,
                const HwState& s) {
    begin_prim(p, s);
    if (!gouraud) c1 = c0;
    int r0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = (c0 >> 16) & 0xFF;
    int r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    // a one-pixel-wide quad along the major axis, covering both end pixels
    float ax = (float)x0, ay = (float)y0, bx = (float)x1, by = (float)y1;
    bool xmajor = std::abs(x1 - x0) >= std::abs(y1 - y0);
    if (xmajor) {
        if (bx >= ax) bx += 1; else ax += 1;
        push(ax, ay, r0, g0, b0, 0, 0, p); push(bx, by, r1, g1, b1, 0, 0, p); push(ax, ay + 1, r0, g0, b0, 0, 0, p);
        push(bx, by, r1, g1, b1, 0, 0, p); push(bx, by + 1, r1, g1, b1, 0, 0, p); push(ax, ay + 1, r0, g0, b0, 0, 0, p);
    } else {
        if (by >= ay) by += 1; else ay += 1;
        push(ax, ay, r0, g0, b0, 0, 0, p); push(bx, by, r1, g1, b1, 0, 0, p); push(ax + 1, ay, r0, g0, b0, 0, 0, p);
        push(bx, by, r1, g1, b1, 0, 0, p); push(bx + 1, by, r1, g1, b1, 0, 0, p); push(ax + 1, ay, r0, g0, b0, 0, 0, p);
    }
    draw_area_dirty(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1, std::max(y0, y1) + 1, s);
}

// ================================================================ VRAM operations

void vk_hw_fill(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    flush();
    uint32_t c = rgba_of(color & 0x7FFF);
    begin_draw();
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color.float32[0] = (c & 0xFF) / 255.0f;
    ca.clearValue.color.float32[1] = ((c >> 8) & 0xFF) / 255.0f;
    ca.clearValue.color.float32[2] = ((c >> 16) & 0xFF) / 255.0f;
    ca.clearValue.color.float32[3] = 0.0f;
    VkClearRect cr{{{x * S, y * S}, {(uint32_t)(std::min(w, 1024 - x) * S), (uint32_t)(std::min(h, 512 - y) * S)}}, 0, 1};
    vkCmdClearAttachments(cb(), 1, &ca, 1, &cr);
    mark_dirty(x, y, x + w, y + h);
}

void vk_hw_upload(int x, int y, int w, int h, const uint16_t* vram) {
    g_prof.uploads++;
    g_prof.up_px += (uint64_t)w * h;
    flush();
    for (int yy = y; yy < y + h;) {  // the region may wrap around the edges of VRAM
        int ry = yy & 511, rh = std::min(y + h - yy, 512 - ry);
        for (int xx = x; xx < x + w;) {
            int rx = xx & 1023, rw = std::min(x + w - xx, 1024 - rx);
            if (overlaps_dirty(rx, ry, rx + rw, ry + rh)) sync_read();
            upload_rect(rx, ry, rw, rh, vram);
            xx += rw;
        }
        yy += rh;
    }
}

void vk_hw_upload_all(const uint16_t* vram) {
    flush();
    upload_rect(0, 0, 1024, 512, vram);
    g_dirty = false;
}

void vk_hw_draw_picture(int x, int y, int w, int h, const uint32_t* rgba) {
    if (!S || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > 1024 || y + h > 512) return;
    if (overlaps_dirty(x, y, x + w, y + h)) sync_read();
    else flush();
    VkImg pic;
    if (!vkc::create_image(pic, w * S, h * S, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return;
    // in bands of rows that fit the staging buffer (a big picture at 8x is larger than it)
    const VkDeviceSize row_bytes = (VkDeviceSize)w * S * 4;
    const int band = (int)std::max<VkDeviceSize>(1, vkc::stage_limit() / row_bytes);
    for (int r0 = 0; r0 < h * S; r0 += band) {
        const int rows = std::min(band, h * S - r0);
        vkc::Staging st = vkc::stage(row_bytes * rows);
        std::memcpy(st.ptr, rgba + (size_t)r0 * w * S, row_bytes * rows);
        vkc::end_rendering();
        vkc::transition(pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic{};
        bic.bufferOffset = st.offset;
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageOffset = {0, r0, 0};
        bic.imageExtent = {(uint32_t)(w * S), (uint32_t)rows, 1};
        vkCmdCopyBufferToImage(cb(), st.buffer, pic.image, pic.layout, 1, &bic);
    }
    draw_copy(pic, x * S, y * S, 1, x, y, w, h);
    vkc::destroy_image(pic);  // waits for the GPU (rare: when a replaced picture appears)
    mark_dirty(x, y, x + w, y + h);
}

void vk_hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    g_prof.copies++;
    if (w <= 0 || h <= 0) return;
    if (overlaps_dirty(sx, sy, sx + w, sy + h)) sync_read();
    else flush();
    w = std::min({w, 1024 - sx, 1024 - dx});
    h = std::min({h, 512 - sy, 512 - dy});
    if (w <= 0 || h <= 0) return;
    if (g_samples > 1) draw_copy(g_read, (dx - sx) * S, (dy - sy) * S, 1, dx, dy, w, h);
    else copy_region(g_read, sx * S, sy * S, g_draw, dx * S, dy * S, w * S, h * S);
    mark_dirty(dx, dy, dx + w, dy + h);
}

void vk_hw_readback(int x, int y, int w, int h, uint16_t* vram) {
    w = std::min(w, 1024 - x);
    h = std::min(h, 512 - y);
    if (w <= 0 || h <= 0) return;
    g_prof.readbacks++;
    g_prof.read_px += (uint64_t)w * h;
    sync_read();
    vkc::end_rendering();
    // The GPU shrinks the area to native size first (one pixel of each S x S block), so only that
    // is copied out and read: the staging memory is slow for the CPU to read, and reading all of
    // VRAM at 8x from it took 55 ms (saving a state froze the game for 5 frames).
    vkc::transition(g_read, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkc::transition(g_stage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageBlit blit{sub, {{x * S, y * S, 0}, {(x + w) * S, (y + h) * S, 1}}, sub, {{x, y, 0}, {x + w, y + h, 1}}};
    vkCmdBlitImage(cb(), g_read.image, g_read.layout, g_stage.image, g_stage.layout, 1, &blit, VK_FILTER_NEAREST);
    vkc::transition(g_stage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    vkc::Staging st = vkc::stage(bytes);
    VkBufferImageCopy bic{};
    bic.bufferOffset = st.offset;
    bic.imageSubresource = sub;
    bic.imageOffset = {x, y, 0};
    bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer(cb(), g_stage.image, g_stage.layout, st.buffer, 1, &bic);
    vkc::submit_wait();
    std::vector<uint32_t> px((size_t)w * h);
    std::memcpy(px.data(), st.ptr, (size_t)bytes);  // one sequential read
    auto q = [](uint32_t v) { return (uint16_t)((v * 31 + 127) / 255); };
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            const uint32_t c = px[(size_t)j * w + i];
            vram[(y + j) * 1024 + x + i] = (uint16_t)(q(c & 0xFF) | q((c >> 8) & 0xFF) << 5 | q((c >> 16) & 0xFF) << 10 |
                                                      ((c >> 24) > 127 ? 0x8000 : 0));
        }
    }
}

unsigned vk_hw_display_texture(int x, int y, int w, int h, int* px, int* py, int* pw, int* ph) {
    sync_read();
    vkc::end_rendering();
    vkc::transition(g_read, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    *px = x * S;
    *py = y * S;
    *pw = w * S;
    *ph = h * S;
    return g_read.id;
}

void vk_hw_capture(int slot, int x, int y, int w, int h) {
    g_prof.caps++;
    if (!S || slot < 0 || slot >= 16 || !clip_rect(x, y, w, h)) return;
    copy_out(g_caps[slot], x, y, w, h);
}

unsigned vk_hw_capture_texture(int slot, int* tex_w, int* tex_h) {
    if (!S || slot < 0 || slot >= 16 || !g_caps[slot].img.image) return 0;
    *tex_w = g_caps[slot].img.w;
    *tex_h = g_caps[slot].img.h;
    return g_caps[slot].img.id;
}

void vk_hw_stash(int x, int y, int w, int h) {
    g_stash_w = 0;
    if (!S || !clip_rect(x, y, w, h)) return;
    copy_out(g_stash, x, y, w, h);
    g_stash_x = x;
    g_stash_y = y;
    g_stash_w = w;
    g_stash_h = h;
}

void vk_hw_unstash() {
    if (!S || !g_stash_w) return;
    flush();
    if (g_samples > 1) {
        draw_copy(g_stash.img, g_stash_x * S, g_stash_y * S, 1, g_stash_x, g_stash_y, g_stash_w, g_stash_h);
    } else {
        copy_region(g_stash.img, 0, 0, g_draw, g_stash_x * S, g_stash_y * S, g_stash_w * S, g_stash_h * S);
        vkc::transition(g_stash.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    mark_dirty(g_stash_x, g_stash_y, g_stash_x + g_stash_w, g_stash_y + g_stash_h);  // read copy re-syncs
    g_stash_w = 0;
}

// Frees everything the renderer created (before the Vulkan device goes; present_vk.cpp).
void vk_renderer_destroy() {
    if (!vkc::ready()) return;
    vk_hw_shutdown();
    vkc::destroy_image(g_stage);
    for (VkShaderModule* m : {&g_vs, &g_fs, &g_copy_vs, &g_copy_fs})
        if (*m) vkDestroyShaderModule(vkc::device, *m, nullptr), *m = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&g_layout, &g_copy_layout})
        if (*l) vkDestroyPipelineLayout(vkc::device, *l, nullptr), *l = VK_NULL_HANDLE;
}

extern uint64_t g_prof_submits, g_prof_waits, g_prof_fence_ns, g_prof_present_ns, g_prof_gpu_ns;
extern uint64_t g_prof_input_ns, g_prof_input_n;

// APOC_PROFILE: logs the renderer's work per second every 120 frames (host_sdl.cpp).
void vk_profile_frame(uint64_t frame) {
    static const bool on = std::getenv("APOC_PROFILE") != nullptr;
    if (!on || frame % 120 != 0) return;
    const Prof& p = g_prof;
    std::fprintf(stderr,
                 "PROF frame %llu per s: %llu batches, %llu verts, %llu syncs (%llu kpx), %llu readbacks (%llu kpx), "
                 "%llu uploads (%llu kpx), %llu copies, %llu captures, %llu submits, %llu GPU waits\n",
                 (unsigned long long)frame, (unsigned long long)p.flushes / 2, (unsigned long long)p.verts / 2,
                 (unsigned long long)p.syncs / 2, (unsigned long long)p.sync_px / 2000, (unsigned long long)p.readbacks / 2,
                 (unsigned long long)p.read_px / 2000, (unsigned long long)p.uploads / 2, (unsigned long long)p.up_px / 2000,
                 (unsigned long long)p.copies / 2, (unsigned long long)p.caps / 2, (unsigned long long)g_prof_submits / 2,
                 (unsigned long long)g_prof_waits / 2);
    static uint64_t last = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (last)
        std::fprintf(stderr, "PROF time: %.1f ms a frame; GPU busy %.1f ms; waiting for GPU %.1f ms, for the display %.1f ms; "
                     "controller read to screen %.1f ms\n",
                     (double)(now - last) / 120e6, (double)g_prof_gpu_ns / 120e6, (double)g_prof_fence_ns / 120e6, (double)g_prof_present_ns / 120e6,
                     g_prof_input_n ? (double)g_prof_input_ns / 1e6 / (double)g_prof_input_n : 0.0);
    last = now;
    g_prof_input_ns = g_prof_input_n = 0;
    g_prof_fence_ns = g_prof_present_ns = g_prof_gpu_ns = 0;
    g_prof = Prof{};
    g_prof_submits = g_prof_waits = 0;
}

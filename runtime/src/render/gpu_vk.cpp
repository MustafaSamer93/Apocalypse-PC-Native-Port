// Vulkan PS1 renderer: the same design as the OpenGL one (gpu_gl.cpp), with the same batching
// and dirty-region logic (hw_common.cpp). VRAM lives in two RGBA8 images of (1024*S)x(512*S): `draw`
// (render target; multisampled with MSAA) and `read` (sampled for textures and palettes, and
// shown). VRAM row 0 is image row 0 here (the OpenGL version keeps it at the bottom), so all
// rectangles, scissors and texel coordinates use the same numbers as the OpenGL code.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "copy.frag.h"
#include "copy.vert.h"
#include "core/psx.h"
#include "game/game.h"
#include "render/gpu_hw.h"
#include "render/hw_common.h"
#include "render/render_backend.h"
#include "render/vk_context.h"
#include "vram.frag.h"
#include "vram.vert.h"

namespace {

using namespace hwc;

VkImg g_draw, g_read, g_stage;  // stage: native 1024x512, for uploads
VkShaderModule g_vs, g_fs, g_copy_vs, g_copy_fs;
VkPipelineLayout g_layout = VK_NULL_HANDLE, g_copy_layout = VK_NULL_HANDLE;
VkPipeline g_pipes[5] = {};          // opaque, then semi modes 0..3
VkPipeline g_copy_pipe = VK_NULL_HANDLE;  // copy pass into `draw` (its sample count)
VkImg g_hd;  // replacement textures' atlas (texreplace.cpp), sampler set 1; made on first use
// Z-buffer: depth image beside `draw` (same size and sample count); HwPrim::ztest batches test
// and write it, cleared for each frame (vk_hw_depth_reset).
VkImg g_zbuf;
bool g_zbuf_clear = true;
void begin_draw() { vkc::begin_rendering(g_draw, g_zbuf.image ? &g_zbuf : nullptr); }

struct VramPush {
    int32_t scale, pass, set_mask, filter;
    int32_t texwin[4];
    float origin[4];  // the VRAM rectangle the target covers (vram.vert)
};

// Frame interpolation (vk_hw_replay): the image a recorded frame is drawn again into, with its own
// depth image, multisampled like `draw` (then resolved into g_ip).
VkImg g_ip, g_ip_ms, g_ip_z;
struct CopyPush {
    int32_t scale, pad, off[2];
};

VkCommandBuffer cb() { return vkc::cmd(); }

void set_viewport_scissor(int sx, int sy, int sw, int sh, int vw = 0, int vh = 0) {
    VkViewport vp{0, 0, (float)(vw ? vw : 1024 * S), (float)(vh ? vh : 512 * S), 0, 1};
    VkRect2D sc{{sx, sy}, {(uint32_t)std::max(sw, 0), (uint32_t)std::max(sh, 0)}};
    vkCmdSetViewport(cb(), 0, 1, &vp);
    vkCmdSetScissor(cb(), 0, 1, &sc);
}

// The textures (`read`, the replacements' atlas) ready to be sampled; may end the rendering.
void textures_readable() {
    for (VkImg* img : {&g_read, &g_hd})
        if (img->image && img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            vkc::end_rendering();
            vkc::transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
}

// Draws n vertices with drawing state `key` into the target being rendered, which covers the VRAM
// rectangle `origin` (x, y, w, h); depth: the target has a depth image (tested as the key says).
void draw_vertices(const Vtx* v, size_t n, const BatchKey& key, bool filter, const int* origin, bool depth) {
    VkImg& tex = g_read;
    VkImg& hd = g_hd.image ? g_hd : tex;  // (set 1 must hold an image even with no replacements)
    const size_t bytes = n * sizeof(Vtx);
    vkc::Staging st = vkc::stage(bytes);
    std::memcpy(st.ptr, v, bytes);
    VkCommandBuffer c = cb();
    if (depth) {
        vkCmdSetDepthTestEnable(c, key.ztest != 0);
        vkCmdSetDepthWriteEnable(c, key.ztest == 1);
    }
    vkCmdBindVertexBuffers(c, 0, 1, &st.buffer, &st.offset);
    const VkDescriptorSet sets[2] = {vkc::sampler_set(tex, false), vkc::sampler_set(hd, true)};
    set_viewport_scissor((key.sc[0] - origin[0]) * S, (key.sc[1] - origin[1]) * S, (key.sc[2] - key.sc[0] + 1) * S,
                         (key.sc[3] - key.sc[1] + 1) * S, origin[2] * S, origin[3] * S);
    VramPush pc{S, 0, key.set_mask ? 1 : 0, filter ? 1 : 0, {key.tw[0], key.tw[1], key.tw[2], key.tw[3]},
                {(float)origin[0], (float)origin[1], (float)origin[2], (float)origin[3]}};
    auto draw = [&](VkPipeline pipe, int pass) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 2, sets, 0, nullptr);
        pc.pass = pass;
        vkCmdPushConstants(c, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(c, (uint32_t)n, 1, 0, 0);
    };
    if (!key.semi) {
        draw(g_pipes[0], 0);
    } else {
        float k = key.semi_mode == 0 ? 0.5f : 0.25f;
        const float consts[4] = {0, 0, 0, k};
        vkCmdSetBlendConstants(c, consts);
        draw(g_pipes[0], 1);
        draw(g_pipes[1 + (key.semi_mode & 3)], 2);
    }
}

void vk_flush() {
    if (g_batch.empty()) return;
    // textures come from `read`; once it is readable, batches follow each other in one rendering
    textures_readable();
    begin_draw();
    if (g_zbuf.image && g_key.ztest && g_zbuf_clear) {  // a new frame: far away everywhere
        VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        ca.clearValue.depthStencil = {1.0f, 0};
        VkClearRect cr{{{0, 0}, {(uint32_t)(1024 * S), (uint32_t)(512 * S)}}, 0, 1};
        vkCmdClearAttachments(cb(), 1, &ca, 1, &cr);
        g_zbuf_clear = false;
    }
    static const int kAll[4] = {0, 0, 1024, 512};
    draw_vertices(g_batch.data(), g_batch.size(), g_key, g_filter, kAll, g_zbuf.image != VK_NULL_HANDLE);
    g_batch.clear();
}

// Makes `read` match `draw` for the region drawn since the last sync.
void vk_sync_read() {
    flush();
    if (!g_dirty) return;
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

// Draws texels of `src` into `draw` over the native rectangle x, y, w, h: target pixel p takes
// texel (p - off) / scale (copy.frag). Used where a copy cannot write (a multisampled target).
void draw_copy(VkImg& src, int offx, int offy, int scale, int x, int y, int w, int h) {
    vkc::end_rendering();
    vkc::transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_draw();
    Vtx q[6] = {};
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

void destroy_pipelines() {
    for (VkPipeline& p : g_pipes)
        if (p) vkDestroyPipeline(vkc::device, p, nullptr), p = VK_NULL_HANDLE;
    if (g_copy_pipe) vkDestroyPipeline(vkc::device, g_copy_pipe, nullptr), g_copy_pipe = VK_NULL_HANDLE;
}

}  // namespace

bool vk_hw_init(int scale) {
    if (!vkc::ready()) return false;
    hwc::flush = vk_flush;
    hwc::sync_read = vk_sync_read;
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
    vkc::destroy_image(g_draw);
    vkc::destroy_image(g_read);
    vkc::destroy_image(g_zbuf);
    for (VkImg* img : {&g_ip, &g_ip_ms, &g_ip_z}) vkc::destroy_image(*img);
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

// ================================================================ Z-buffer

void vk_hw_depth_reset() { g_zbuf_clear = true; }
bool vk_hw_zbuffer_ready() { return S && g_zbuf.image; }

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

// ================================================================ frame interpolation

// The replay images for a w x h (native) frame.
static bool replay_target(int w, int h) {
    const int pw = w * S, ph = h * S;
    if (g_ip.image && g_ip.w == pw && g_ip.h == ph) return true;  // (vk_hw_init frees them)
    for (VkImg* img : {&g_ip, &g_ip_ms, &g_ip_z}) vkc::destroy_image(*img);
    const VkSampleCountFlagBits sc = (VkSampleCountFlagBits)g_samples;
    bool ok = vkc::create_image(g_ip, pw, ph, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (ok && g_samples > 1)
        ok = vkc::create_image(g_ip_ms, pw, ph, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, sc);
    if (ok && g_zbuf.image)
        ok = vkc::create_image(g_ip_z, pw, ph, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, sc);
    if (!ok) {
        log_msg(LogCat::Gpu, "frame interpolation: no %dx%d image", pw, ph);
        for (VkImg* img : {&g_ip, &g_ip_ms, &g_ip_z}) vkc::destroy_image(*img);
        return false;
    }
    vkc::register_texture(&g_ip);
    return true;
}

unsigned vk_hw_replay(int id, float t, int* tw, int* th) {
    const RecFrame* f = rec_frame(id);
    if (!S || !f || !replay_target(f->w, f->h)) return 0;
    flush();  // (the game's own pending primitives, into VRAM)
    textures_readable();
    VkImg& target = g_samples > 1 ? g_ip_ms : g_ip;
    vkc::end_rendering();
    vkc::begin_rendering(target, g_ip_z.image ? &g_ip_z : nullptr);
    VkCommandBuffer c = cb();
    auto clear = [&](VkImageAspectFlags aspect, int x, int y, int w, int h, uint32_t rgba) {
        VkClearAttachment ca{aspect, 0, {}};
        if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
            ca.clearValue.depthStencil = {1.0f, 0};
        } else {
            for (int i = 0; i < 3; i++) ca.clearValue.color.float32[i] = ((rgba >> (8 * i)) & 0xFF) / 255.0f;
            ca.clearValue.color.float32[3] = 0.0f;
        }
        VkClearRect cr{{{x, y}, {(uint32_t)w, (uint32_t)h}}, 0, 1};
        vkCmdClearAttachments(c, 1, &ca, 1, &cr);
    };
    clear(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, target.w, target.h, 0);
    if (g_ip_z.image) clear(VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, target.w, target.h, 0);
    const int origin[4] = {f->x, f->y, f->w, f->h};
    static std::vector<Vtx> moved;
    for (const RecOp& op : f->ops) {
        if (op.kind == RecOp::Depth) {
            if (g_ip_z.image) clear(VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, target.w, target.h, 0);
        } else if (op.kind == RecOp::Fill) {
            const int x0 = std::max(op.x, f->x), y0 = std::max(op.y, f->y);
            const int x1 = std::min(op.x + op.w, f->x + f->w), y1 = std::min(op.y + op.h, f->y + f->h);
            if (x0 < x1 && y0 < y1)
                clear(VK_IMAGE_ASPECT_COLOR_BIT, (x0 - f->x) * S, (y0 - f->y) * S, (x1 - x0) * S, (y1 - y0) * S,
                      rgba_of(op.color & 0x7FFF));
        } else {
            moved.resize(op.count);
            for (uint32_t i = 0; i < op.count; i++) moved[i] = rec_vertex(f->v[op.first + i], f->m[op.first + i], t);
            draw_vertices(moved.data(), moved.size(), op.key, op.filter, origin, g_ip_z.image != VK_NULL_HANDLE);
        }
    }
    vkc::end_rendering();
    if (g_samples > 1) {
        vkc::transition(g_ip_ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkc::transition(g_ip, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        VkImageResolve r{sub, {0, 0, 0}, sub, {0, 0, 0}, {(uint32_t)g_ip.w, (uint32_t)g_ip.h, 1}};
        vkCmdResolveImage(cb(), g_ip_ms.image, g_ip_ms.layout, g_ip.image, g_ip.layout, 1, &r);
    }
    vkc::transition(g_ip, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    *tw = g_ip.w, *th = g_ip.h;
    return g_ip.id;
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

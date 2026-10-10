// Vulkan presentation (see present.h; the OpenGL version is present_gl.cpp): swapchain, the game
// image with sharp or area-average scaling, the CRT and FXAA filters, the settings overlay,
// and screenshots.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/psx.h"
#include "render/present.h"
#include "render/present_common.h"
#include "render/render_backend.h"
#include "render/ui_text.h"
#include "render/vk_context.h"

#include "crt.frag.h"
#include "fxaa.frag.h"
#include "overlay.frag.h"
#include "plain.frag.h"
#include "quad.vert.h"

namespace {

using namespace pres;

VkSwapchainKHR g_swap = VK_NULL_HANDLE;
VkFormat g_swap_format = VK_FORMAT_B8G8R8A8_UNORM;
std::vector<VkImg> g_swap_imgs;
std::vector<VkSemaphore> g_done;  // one per swapchain image: rendering finished, can present
VkSemaphore g_acquired[2] = {};  // per in-flight slot (vkc::frame_slot)
uint32_t g_index = 0;
bool g_have_image = false;
bool g_vsync = true;
bool g_swap_dirty = false;  // recreate before the next frame (resize, vsync change)

VkShaderModule g_quad, g_plain_fs, g_crt_fs, g_fxaa_fs, g_overlay_fs;
VkPipelineLayout g_layout = VK_NULL_HANDLE, g_fxaa_layout = VK_NULL_HANDLE;
VkPipeline g_plain = VK_NULL_HANDLE, g_crt = VK_NULL_HANDLE, g_fxaa = VK_NULL_HANDLE, g_overlay = VK_NULL_HANDLE;
VkPipeline g_plain_aa = VK_NULL_HANDLE;  // plain, into the FXAA intermediate image

VkImg g_aa;       // the scaled image, for the FXAA pass
VkImg g_sw;       // the software renderer's picture
VkImg g_ov_img;   // the overlay (present_overlay_size)
VkImg g_badge[kBadgeSlots];  // present_badge's images

struct ImagePush {
    float rect[4];
    float tex_size[2];
    float out_size[2];
    float lines;
    float time;  // the CRT filter's (crt.frag)
};

VkCommandBuffer cb() { return vkc::cmd(); }

void destroy_swapchain() {
    for (VkImg& img : g_swap_imgs) vkc::destroy_image(img);
    g_swap_imgs.clear();
    for (VkSemaphore s : g_done) vkDestroySemaphore(vkc::device, s, nullptr);
    g_done.clear();
    if (g_swap) vkDestroySwapchainKHR(vkc::device, g_swap, nullptr);
    g_swap = VK_NULL_HANDLE;
}

bool create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkc::gpu, vkc::surface, &caps);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) ext = {(uint32_t)pw, (uint32_t)ph};
    if (ext.width == 0 || ext.height == 0) return false;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkc::gpu, vkc::surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkc::gpu, vkc::surface, &nf, formats.data());
    VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                             : formats[0];
    for (const auto& f : formats)  // UNORM like the OpenGL window's framebuffer
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            fmt = f;
            break;
        }
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vkc::gpu, vkc::surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vkc::gpu, vkc::surface, &nm, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vsync) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = vkc::surface;
    // With vsync, two images: a third only lets the CPU queue another refresh ahead of the
    // display (~17 ms more input lag; the GPU needs ~2 ms a frame, so it gains nothing).
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount && !g_vsync) ci.minImageCount = std::min(ci.minImageCount + 1, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(vkc::device, &ci, nullptr, &g_swap) != VK_SUCCESS) return false;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(vkc::device, g_swap, &n, nullptr);
    std::vector<VkImage> imgs(n);
    vkGetSwapchainImagesKHR(vkc::device, g_swap, &n, imgs.data());
    g_swap_imgs.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        VkImg& im = g_swap_imgs[i];
        im = VkImg{};
        im.image = imgs[i];
        im.format = fmt.format;
        im.w = (int)ext.width;
        im.h = (int)ext.height;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = imgs[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vkc::device, &vci, nullptr, &im.view);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(vkc::device, &sci, nullptr, &s);
        g_done.push_back(s);
    }
    if (fmt.format != g_swap_format || !g_plain) {  // pipelines follow the swapchain format
        for (VkPipeline* p : {&g_plain, &g_crt, &g_fxaa, &g_overlay})
            if (*p) vkDestroyPipeline(vkc::device, *p, nullptr), *p = VK_NULL_HANDLE;
        g_swap_format = fmt.format;
        const auto one = VK_SAMPLE_COUNT_1_BIT;
        g_plain = vkc::make_pipeline({g_quad, g_plain_fs, g_layout, g_swap_format, one, vkc::Blend::None, vkc::Vertices::None});
        g_crt = vkc::make_pipeline({g_quad, g_crt_fs, g_layout, g_swap_format, one, vkc::Blend::None, vkc::Vertices::None});
        g_fxaa = vkc::make_pipeline({g_quad, g_fxaa_fs, g_fxaa_layout, g_swap_format, one, vkc::Blend::None, vkc::Vertices::None});
        g_overlay = vkc::make_pipeline({g_quad, g_overlay_fs, g_layout, g_swap_format, one, vkc::Blend::Alpha, vkc::Vertices::None});
    }
    g_swap_dirty = false;
    return true;
}

bool recreate_swapchain() {
    vkc::submit_wait();
    vkDeviceWaitIdle(vkc::device);
    destroy_swapchain();
    return create_swapchain();
}

// The swapchain image of this frame, acquired on first use and cleared to black.
VkImg* frame() {
    if (g_have_image) return &g_swap_imgs[g_index];
    for (int attempt = 0; attempt < 3; attempt++) {
        if ((g_swap_dirty || !g_swap) && !recreate_swapchain()) return nullptr;
        VkResult r = vkAcquireNextImageKHR(vkc::device, g_swap, UINT64_MAX, g_acquired[vkc::frame_slot()], VK_NULL_HANDLE, &g_index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            g_swap_dirty = true;
            continue;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return nullptr;
        vkc::wait_next_submit(g_acquired[vkc::frame_slot()]);
        g_have_image = true;
        VkImg& im = g_swap_imgs[g_index];
        im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkc::begin_rendering(im);
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ca.clearValue.color.float32[3] = 1.0f;
        VkClearRect cr{{{0, 0}, {(uint32_t)im.w, (uint32_t)im.h}}, 0, 1};
        vkCmdClearAttachments(cb(), 1, &ca, 1, &cr);
        return &im;
    }
    return nullptr;
}

void viewport(float x, float y, float w, float h) {
    VkViewport vp{x, y, w, h, 0, 1};
    VkRect2D sc{{(int32_t)x, (int32_t)y}, {(uint32_t)std::max(w, 1.0f), (uint32_t)std::max(h, 1.0f)}};
    vkCmdSetViewport(cb(), 0, 1, &vp);
    vkCmdSetScissor(cb(), 0, 1, &sc);
}

void draw_quad(VkPipeline pipe, VkPipelineLayout layout, VkImg& src, const void* push, uint32_t push_bytes) {
    VkDescriptorSet set = vkc::sampler_set(src, true);
    vkCmdBindPipeline(cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (push_bytes)
        vkCmdPushConstants(cb(), layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes, push);
    vkCmdDraw(cb(), 4, 1, 0, 0);
}

void draw_image(VkImg& tex, int tex_w, int tex_h, int x, int y, int w, int h, int native_h, const PresentOptions& o) {
    VkImg* target = frame();
    if (!target) return;
    const int ww = target->w, wh = target->h;
    fit_image(o.aspect, ww, wh);
    const float cw = g_cw, ch = g_ch;
    ImagePush pc{{(float)x, (float)y, (float)w, (float)h}, {(float)tex_w, (float)tex_h}, {cw, ch}, (float)crt_lines(native_h), crt_time()};
    // the source is sampled: make sure it is readable (it is outside any rendering then)
    vkc::end_rendering();
    vkc::transition(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const bool fxaa = o.fxaa && !o.crt;
    if (fxaa) {
        int tw = std::max(1, (int)cw), th = std::max(1, (int)ch);
        if (g_aa.w != tw || g_aa.h != th) {
            vkc::submit_wait();
            vkc::destroy_image(g_aa);
            vkc::create_image(g_aa, tw, th, g_swap_format,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            if (!g_plain_aa)
                g_plain_aa = vkc::make_pipeline(
                    {g_quad, g_plain_fs, g_layout, g_swap_format, VK_SAMPLE_COUNT_1_BIT, vkc::Blend::None, vkc::Vertices::None});
            target = frame();
        }
        vkc::begin_rendering(g_aa);
        viewport(0, 0, (float)tw, (float)th);
        draw_quad(g_plain_aa, g_layout, tex, &pc, sizeof pc);
        vkc::end_rendering();
        vkc::transition(g_aa, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkc::begin_rendering(*target);
        viewport(g_cx, g_cy, cw, ch);
        float px[2] = {1.0f / (float)tw, 1.0f / (float)th};
        draw_quad(g_fxaa, g_fxaa_layout, g_aa, px, sizeof px);
        return;
    }
    vkc::begin_rendering(*target);
    viewport(g_cx, g_cy, cw, ch);
    draw_quad(o.crt ? g_crt : g_plain, g_layout, tex, &pc, sizeof pc);
}

}  // namespace

bool vk_present_init(SDL_Window* window) {
    g_win = window;
    if (!vkc::init(window)) return false;
    g_quad = vkc::shader(kSpv_quad_vert, sizeof kSpv_quad_vert);
    g_plain_fs = vkc::shader(kSpv_plain_frag, sizeof kSpv_plain_frag);
    g_crt_fs = vkc::shader(kSpv_crt_frag, sizeof kSpv_crt_frag);
    g_fxaa_fs = vkc::shader(kSpv_fxaa_frag, sizeof kSpv_fxaa_frag);
    g_overlay_fs = vkc::shader(kSpv_overlay_frag, sizeof kSpv_overlay_frag);
    g_layout = vkc::make_layout(sizeof(ImagePush));
    g_fxaa_layout = vkc::make_layout(2 * sizeof(float));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& s : g_acquired) vkCreateSemaphore(vkc::device, &sci, nullptr, &s);
    if (!create_swapchain()) {
        log_msg(LogCat::Host, "Vulkan: could not create the swapchain");
        return false;
    }
    return g_plain && g_crt && g_fxaa && g_overlay;
}

void vk_present_shutdown() {
    if (!vkc::ready()) {
        vkc::shutdown();
        return;
    }
    vkc::submit_wait();
    vkDeviceWaitIdle(vkc::device);
    vk_renderer_destroy();
    destroy_swapchain();
    for (VkImg* im : {&g_aa, &g_sw, &g_ov_img}) vkc::destroy_image(*im);
    for (VkImg& im : g_badge) vkc::destroy_image(im);
    for (VkPipeline* p : {&g_plain, &g_crt, &g_fxaa, &g_overlay, &g_plain_aa})
        if (*p) vkDestroyPipeline(vkc::device, *p, nullptr), *p = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&g_layout, &g_fxaa_layout})
        if (*l) vkDestroyPipelineLayout(vkc::device, *l, nullptr), *l = VK_NULL_HANDLE;
    for (VkShaderModule* m : {&g_quad, &g_plain_fs, &g_crt_fs, &g_fxaa_fs, &g_overlay_fs})
        if (*m) vkDestroyShaderModule(vkc::device, *m, nullptr), *m = VK_NULL_HANDLE;
    for (VkSemaphore& s : g_acquired)
        if (s) vkDestroySemaphore(vkc::device, s, nullptr), s = VK_NULL_HANDLE;
    overlay_shutdown();
    vkc::shutdown();
    g_have_image = false;
    g_last = LastImage{};
}

void vk_present_set_vsync(bool on) {
    if (on == g_vsync) return;
    g_vsync = on;
    g_swap_dirty = true;  // a different present mode needs a new swapchain
}

void vk_present_game_pixels(const uint32_t* pixels, int w, int h, const PresentOptions& o) {
    if (g_sw.w != w || g_sw.h != h) {
        vkc::submit_wait();
        vkc::destroy_image(g_sw);
        vkc::create_image(g_sw, w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vkc::register_texture(&g_sw);
    }
    vkc::Staging st = vkc::stage((VkDeviceSize)w * h * 4);
    std::memcpy(st.ptr, pixels, (size_t)w * h * 4);
    vkc::end_rendering();
    vkc::transition(g_sw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic{};
    bic.bufferOffset = st.offset;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb(), st.buffer, g_sw.image, g_sw.layout, 1, &bic);
    g_last = {true, g_sw.id, w, h, 0, 0, w, h, h};
    draw_image(g_sw, w, h, 0, 0, w, h, h, o);
}

void vk_present_game_texture(unsigned tex, int tex_w, int tex_h, int x, int y, int w, int h, int native_h,
                             const PresentOptions& o) {
    VkImg* img = vkc::texture(tex);
    if (!img) return;
    g_last = {true, tex, tex_w, tex_h, x, y, w, h, native_h};
    draw_image(*img, tex_w, tex_h, x, y, w, h, native_h, o);
}

void vk_present_redraw(const PresentOptions& o) {
    VkImg* img = g_last.valid ? vkc::texture(g_last.tex) : nullptr;
    if (!img) {
        frame();  // cleared to black
        return;
    }
    draw_image(*img, g_last.tex_w, g_last.tex_h, g_last.x, g_last.y, g_last.w, g_last.h, g_last.native_h, o);
}

SDL_Renderer* vk_present_overlay_begin() {
    bool remade;  // (the image follows the surface's size in vk_present_overlay_end)
    return overlay_begin(&remade);
}

void vk_present_overlay_end() {
    overlay_flush();
    VkImg* target = frame();
    if (!target) return;
    const int ow = g_ov_surf->w, oh = g_ov_surf->h;
    vkc::end_rendering();
    if (g_ov_img.w != ow || g_ov_img.h != oh) {
        vkc::destroy_image(g_ov_img);
        vkc::create_image(g_ov_img, ow, oh, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    vkc::Staging st = vkc::stage((VkDeviceSize)ow * oh * 4);
    for (int y = 0; y < oh; y++)
        std::memcpy(st.ptr + (size_t)y * ow * 4, (const uint8_t*)g_ov_surf->pixels + (size_t)y * g_ov_surf->pitch, (size_t)ow * 4);
    vkc::transition(g_ov_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic{};
    bic.bufferOffset = st.offset;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {(uint32_t)ow, (uint32_t)oh, 1};
    vkCmdCopyBufferToImage(cb(), st.buffer, g_ov_img.image, g_ov_img.layout, 1, &bic);
    vkc::transition(g_ov_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float ox, oy, cw, ch;
    overlay_area((float)target->w, (float)target->h, &ox, &oy, &cw, &ch);
    vkc::begin_rendering(*target);
    viewport(ox, oy, cw, ch);
    draw_quad(g_overlay, g_layout, g_ov_img, nullptr, 0);
}

void vk_present_badge(int slot, const uint32_t* px, int w, int h, int x, int margin, bool changed) {
    VkImg& badge = g_badge[std::clamp(slot, 0, kBadgeSlots - 1)];
    VkImg* target = frame();
    if (!target) return;
    vkc::end_rendering();
    if (badge.w != w || badge.h != h) {
        vkc::destroy_image(badge);
        vkc::create_image(badge, w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        target = frame();
        changed = true;
    }
    if (changed) {
        vkc::Staging st = vkc::stage((VkDeviceSize)w * h * 4);
        std::memcpy(st.ptr, px, (size_t)w * h * 4);
        vkc::transition(badge, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic{};
        bic.bufferOffset = st.offset;
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyBufferToImage(cb(), st.buffer, badge.image, badge.layout, 1, &bic);
    }
    vkc::transition(badge, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkc::begin_rendering(*target);
    viewport((float)x, (float)(target->h - margin - h), (float)w, (float)h);
    draw_quad(g_overlay, g_layout, badge, nullptr, 0);
}

void vk_present_swap() {
    VkImg* target = frame();
    if (!target) return;
    vkc::end_rendering();
    vkc::transition(*target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkc::submit_frame(g_done[g_index]);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_done[g_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &g_index;
    VkResult r = vkQueuePresentKHR(vkc::queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_swap_dirty = true;
    g_have_image = false;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    if (!g_swap_imgs.empty() && (pw != g_swap_imgs[0].w || ph != g_swap_imgs[0].h)) g_swap_dirty = true;
}

// --screenshot FRAME PATH support: saves the frame drawn so far as a BMP.
void vk_present_save_bmp(const char* path) {
    VkImg* target = frame();
    if (!target) return;
    const int w = target->w, h = target->h;
    vkc::end_rendering();
    vkc::transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkc::Staging st = vkc::stage((VkDeviceSize)w * h * 4);
    VkBufferImageCopy bic{};
    bic.bufferOffset = st.offset;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer(cb(), target->image, target->layout, st.buffer, 1, &bic);
    vkc::submit_wait();
    std::vector<uint8_t> px(st.ptr, st.ptr + (size_t)w * h * 4);
    if (g_swap_format == VK_FORMAT_R8G8B8A8_UNORM)  // BMP rows want B, G, R
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    save_bmp_bgrx(px.data(), w, h, path);
    // drawing can continue on this image (the overlay, for one)
    vkc::begin_rendering(*target);
}

#include "present.h"
#include "render_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gl_api.h"
#include "psx.h"
#include "ui_text.h"

namespace {

SDL_Window* g_win = nullptr;
SDL_GLContext g_ctx = nullptr;
GLuint g_prog_plain, g_prog_crt, g_prog_fxaa, g_vao, g_vbo;
GLuint g_aa_tex = 0, g_aa_fbo = 0;  // the scaled image, for the FXAA pass
int g_aa_w = 0, g_aa_h = 0;
GLuint g_sw_tex = 0;
int g_sw_w = 0, g_sw_h = 0;

// last game image, for redraws
struct Last {
    bool valid = false;
    unsigned tex = 0;
    int tex_w = 0, tex_h = 0, x = 0, y = 0, w = 0, h = 0, native_h = 240;
} g_last;

// where the game image went on the last frame (window pixels, y down)
float g_cx = 0, g_cy = 0, g_cw = 1, g_ch = 1;

SDL_Surface* g_ov_surf = nullptr;
SDL_Renderer* g_ov_ren = nullptr;
GLuint g_ov_tex = 0;
GLuint g_prog_overlay = 0;

const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;   // 0..1 across the viewport, y up
out vec2 v_uv;                        // 0..1, y down (image space)
void main() {
    gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(a_pos.x, 1.0 - a_pos.y);
}
)";

// Sharp bilinear: nearest within texels, linear only across texel edges.
const char* kPlainFS = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_tex_size;     // texture size in texels
uniform vec4 u_rect;         // x, y, w, h of the image in texels (y = top row)
uniform vec2 u_out_size;     // output size in pixels
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec2 texel = u_rect.xy + v_uv * u_rect.zw;
    // Image larger than the output (supersampling): average everything each output pixel
    // covers, up to 4x4 bilinear taps (each already a 2x2 average).
    vec2 fp = u_rect.zw / u_out_size;
    if (fp.x > 1.01 || fp.y > 1.01) {
        ivec2 n = ivec2(clamp(ceil(fp), vec2(1.0), vec2(4.0)));
        vec3 acc = vec3(0.0);
        for (int j = 0; j < n.y; j++)
            for (int i = 0; i < n.x; i++) {
                vec2 t = texel + ((vec2(i, j) + 0.5) / vec2(n) - 0.5) * fp;
                acc += texture(u_tex, t / u_tex_size).rgb;
            }
        o_color = vec4(acc / float(n.x * n.y), 1.0);
        return;
    }
    vec2 scale = max(floor(u_out_size / u_rect.zw), vec2(1.0));
    vec2 region = 0.5 - 0.5 / scale;
    vec2 f = fract(texel) - 0.5;
    vec2 off = (f - clamp(f, -region, region)) * scale + 0.5;
    vec2 p = floor(texel) + off;
    o_color = vec4(texture(u_tex, p / u_tex_size).rgb, 1.0);
}
)";

// The CRT filter: runtime/shaders/crt_body.glsl (shared with Vulkan's crt.frag).
const char* kCrtHead = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform vec4 u_rect;
uniform vec2 u_out_size;
uniform float u_lines, u_time;
in vec2 v_uv;
out vec4 o_color;
#define SRC u_tex
#define RECT u_rect
#define TEX_SIZE u_tex_size
#define OUT_SIZE u_out_size
#define LINES u_lines
#define TIME u_time
)";
const char* kCrtBody =
#include "crt_body.glsl.h"
    ;
const std::string kCrtFS = std::string(kCrtHead) + kCrtBody + "void main() { o_color = crt(v_uv, gl_FragCoord.x); }\n";

const char* kOverlayFS = R"(#version 330 core
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = texture(u_tex, v_uv); }
)";

// FXAA (Timothy Lottes' fast approximate anti-aliasing): softens stair-stepped edges
// along their direction, leaving flat areas and fine texture alone.
const char* kFxaaFS = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_px;           // 1 / texture size
in vec2 v_uv;
out vec4 o_color;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 p) { return texture(u_tex, p).rgb; }
void main() {
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);  // the source was rendered bottom-up
    vec3 m = at(uv);
    float lNW = luma(at(uv + vec2(-1.0, -1.0) * u_px)), lNE = luma(at(uv + vec2(1.0, -1.0) * u_px));
    float lSW = luma(at(uv + vec2(-1.0, 1.0) * u_px)), lSE = luma(at(uv + vec2(1.0, 1.0) * u_px));
    float lM = luma(m);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) { o_color = vec4(m, 1.0); return; }  // no edge
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * u_px;
    vec3 a = 0.5 * (at(uv + dir * (1.0 / 3.0 - 0.5)) + at(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (at(uv - dir * 0.5) + at(uv + dir * 0.5));
    float lB = luma(b);
    o_color = vec4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)";

GLuint make_texture(int w, int h, GLint filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

float aspect_value(Aspect a, int w, int h) {
    const float r = aspect_ratio(a);
    return r > 0 ? r : (float)w / (float)std::max(h, 1);
}

void draw_quad() {
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void draw_image(unsigned tex, int tex_w, int tex_h, int x, int y, int w, int h, int native_h, const PresentOptions& o) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    float ar = aspect_value(o.aspect, ww, wh);
    float cw = (float)ww, ch = cw / ar;
    if (ch > (float)wh) { ch = (float)wh; cw = ch * ar; }
    g_cx = ((float)ww - cw) * 0.5f;
    g_cy = ((float)wh - ch) * 0.5f;
    g_cw = cw;
    g_ch = ch;
    glViewport((GLint)g_cx, (GLint)g_cy, (GLsizei)cw, (GLsizei)ch);

    // FXAA: draw the scaled image into a target the size of the picture, then filter that
    // into the window. (Not with the CRT filter, whose scanlines it would soften.)
    const bool fxaa = o.fxaa && !o.crt;
    if (fxaa) {
        int tw = std::max(1, (int)cw), th = std::max(1, (int)ch);
        if (tw != g_aa_w || th != g_aa_h) {
            if (g_aa_fbo) glDeleteFramebuffers(1, &g_aa_fbo);
            if (g_aa_tex) glDeleteTextures(1, &g_aa_tex);
            g_aa_tex = make_texture(tw, th, GL_LINEAR);
            glGenFramebuffers(1, &g_aa_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_aa_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_aa_tex, 0);
            g_aa_w = tw;
            g_aa_h = th;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, g_aa_fbo);
        glViewport(0, 0, tw, th);
    }
    GLuint prog = o.crt ? g_prog_crt : g_prog_plain;
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(prog, "u_tex_size"), (float)tex_w, (float)tex_h);
    glUniform4f(glGetUniformLocation(prog, "u_rect"), (float)x, (float)y, (float)w, (float)h);
    glUniform2f(glGetUniformLocation(prog, "u_out_size"), cw, ch);
    if (o.crt) {
        glUniform1f(glGetUniformLocation(prog, "u_lines"), (float)crt_lines(native_h));
        glUniform1f(glGetUniformLocation(prog, "u_time"), crt_time());
    }
    draw_quad();
    if (fxaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport((GLint)g_cx, (GLint)g_cy, (GLsizei)cw, (GLsizei)ch);
        glUseProgram(g_prog_fxaa);
        glBindTexture(GL_TEXTURE_2D, g_aa_tex);
        glUniform1i(glGetUniformLocation(g_prog_fxaa, "u_tex"), 0);
        glUniform2f(glGetUniformLocation(g_prog_fxaa, "u_px"), 1.0f / (float)g_aa_w, 1.0f / (float)g_aa_h);
        draw_quad();
    }
}

}  // namespace

bool gl_present_init(SDL_Window* window) {
    g_win = window;
    g_ctx = SDL_GL_CreateContext(window);
    if (!g_ctx) {
        log_msg(LogCat::Host, "OpenGL context failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window, g_ctx);
    if (!gl_load()) return false;
    log_msg(LogCat::Host, "OpenGL: %s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    g_prog_plain = gl_program(kVS, kPlainFS);
    g_prog_crt = gl_program(kVS, kCrtFS.c_str());
    g_prog_fxaa = gl_program(kVS, kFxaaFS);
    g_prog_overlay = gl_program(kVS, kOverlayFS);
    if (!g_prog_plain || !g_prog_crt || !g_prog_fxaa || !g_prog_overlay) return false;

    const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    return true;
}

void gl_present_shutdown() {
    ui_text_reset(g_ov_ren);
    if (g_ov_ren) SDL_DestroyRenderer(g_ov_ren), g_ov_ren = nullptr;
    if (g_ov_surf) SDL_DestroySurface(g_ov_surf), g_ov_surf = nullptr;
    if (g_ctx) SDL_GL_DestroyContext(g_ctx);
}

void gl_present_set_vsync(bool on) { SDL_GL_SetSwapInterval(on ? 1 : 0); }

void gl_present_game_pixels(const uint32_t* pixels, int w, int h, const PresentOptions& o) {
    if (!g_sw_tex || w != g_sw_w || h != g_sw_h) {
        if (g_sw_tex) glDeleteTextures(1, &g_sw_tex);
        g_sw_tex = make_texture(w, h, GL_LINEAR);
        g_sw_w = w;
        g_sw_h = h;
    }
    glBindTexture(GL_TEXTURE_2D, g_sw_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    g_last = {true, g_sw_tex, w, h, 0, 0, w, h, h};
    draw_image(g_sw_tex, w, h, 0, 0, w, h, h, o);
}

void gl_present_game_texture(unsigned tex, int tex_w, int tex_h, int x, int y, int w, int h, int native_h,
                          const PresentOptions& o) {
    g_last = {true, tex, tex_w, tex_h, x, y, w, h, native_h};
    draw_image(tex, tex_w, tex_h, x, y, w, h, native_h, o);
}

void gl_present_redraw(const PresentOptions& o) {
    if (!g_last.valid) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, ww, wh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    draw_image(g_last.tex, g_last.tex_w, g_last.tex_h, g_last.x, g_last.y, g_last.w, g_last.h, g_last.native_h, o);
}

SDL_Renderer* gl_present_overlay_begin() {
    int ww, wh, ow, oh;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    present_overlay_size(ww, wh, &ow, &oh);
    if (!g_ov_surf || g_ov_surf->w != ow || g_ov_surf->h != oh) {  // (re)made at the window's size
        ui_text_reset(g_ov_ren);
        if (g_ov_ren) SDL_DestroyRenderer(g_ov_ren);
        if (g_ov_surf) SDL_DestroySurface(g_ov_surf);
        if (g_ov_tex) glDeleteTextures(1, &g_ov_tex);
        g_ov_surf = SDL_CreateSurface(ow, oh, SDL_PIXELFORMAT_ARGB8888);
        g_ov_ren = SDL_CreateSoftwareRenderer(g_ov_surf);
        g_ov_tex = make_texture(ow, oh, GL_LINEAR);
    }
    SDL_SetRenderScale(g_ov_ren, (float)oh / 480.0f, (float)oh / 480.0f);
    SDL_SetRenderDrawColor(g_ov_ren, 0, 0, 0, 0);
    SDL_SetRenderDrawBlendMode(g_ov_ren, SDL_BLENDMODE_NONE);
    SDL_RenderClear(g_ov_ren);
    return g_ov_ren;
}

void gl_present_overlay_end() {
    SDL_RenderPresent(g_ov_ren);  // software renderer: flushes into the surface
    glBindTexture(GL_TEXTURE_2D, g_ov_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, g_ov_surf->pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_ov_surf->w, g_ov_surf->h, GL_BGRA, GL_UNSIGNED_BYTE, g_ov_surf->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    int ww, wh;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    // the overlay covers the whole window, keeping 4:3
    float ch = (float)wh, cw = ch * 4.0f / 3.0f;
    if (cw > (float)ww) { cw = (float)ww; ch = cw * 0.75f; }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport((GLint)(((float)ww - cw) * 0.5f), (GLint)(((float)wh - ch) * 0.5f), (GLsizei)cw, (GLsizei)ch);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glUseProgram(g_prog_overlay);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(g_prog_overlay, "u_tex"), 0);
    draw_quad();
    glDisable(GL_BLEND);
}

GLuint g_badge_tex[3] = {};
int g_badge_w[3] = {}, g_badge_h[3] = {};

void gl_present_badge(int slot, const uint32_t* px, int w, int h, int x, int margin, bool changed) {
    slot = std::clamp(slot, 0, 2);
    GLuint& tex = g_badge_tex[slot];
    if (!tex || w != g_badge_w[slot] || h != g_badge_h[slot]) {
        if (tex) glDeleteTextures(1, &tex);
        tex = make_texture(w, h, GL_LINEAR);
        g_badge_w[slot] = w, g_badge_h[slot] = h;
        changed = true;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    if (changed) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(x, margin, w, h);  // (GL counts from the bottom)
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glUseProgram(g_prog_overlay);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(g_prog_overlay, "u_tex"), 0);
    draw_quad();
    glDisable(GL_BLEND);
}

// --screenshot FRAME PATH support: saves the back buffer as a BMP before swapping.
void gl_present_save_bmp(const char* path) {
    int w, h;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    std::vector<uint8_t> px((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    // GL rows are bottom-up
    std::vector<uint8_t> flipped(px.size());
    for (int y = 0; y < h; y++)
        std::memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    save_bmp_bgrx(flipped.data(), w, h, path);
}

// 24-bit, so the window's alpha (0 in the GL back buffer) cannot make viewers show it
// transparent or blank.
bool save_bmp_bgrx(const uint8_t* px, int w, int h, const char* path) {
    const uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, (uint32_t)w);
    put32(22, (uint32_t)-h);  // top-down rows
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * (uint32_t)h);
    FILE* f = std::fopen(path, "wb");
    bool ok = f && std::fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    std::vector<uint8_t> line(row, 0);
    for (int y = 0; ok && y < h; y++) {
        const uint8_t* src = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) std::memcpy(&line[(size_t)x * 3], src + (size_t)x * 4, 3);
        ok = std::fwrite(line.data(), 1, row, f) == row;
    }
    if (f && std::fclose(f) != 0) ok = false;
    if (ok) log_msg(LogCat::Host, "screenshot saved to %s", path);
    else log_msg(LogCat::Host, "screenshot %s failed", path);
    return ok;
}

void gl_present_swap() {
    extern uint64_t g_prof_present_ns;  // (present_vk.cpp: time waiting for the display)
    const uint64_t t0 = SDL_GetTicksNS();
    SDL_GL_SwapWindow(g_win);
    // Waits until the swap is done: the driver otherwise queues 1-2 more frames ahead of the
    // display, each one refresh of input lag.
    glFinish();
    const uint64_t t1 = SDL_GetTicksNS();
    g_prof_present_ns += t1 - t0;
    // APOC_PROFILE: controller read -> its frame swapped (average over 120 frames)
    extern uint64_t g_input_read_ns;
    static const bool prof = std::getenv("APOC_PROFILE") != nullptr;
    static uint64_t sum = 0, n = 0;
    if (prof && g_input_read_ns && t1 > g_input_read_ns) {
        sum += t1 - g_input_read_ns;
        if (++n == 120) {
            log_msg(LogCat::Host, "input to screen: %.1f ms", (double)sum / 1e6 / (double)n);
            sum = n = 0;
        }
    }
}

bool gl_present_window_to_game(float wx, float wy, float* x, float* y) {
    // SDL reports window coordinates; the viewport was set in pixels
    int ww, wh, pw, ph;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    float sx = ww ? (float)pw / (float)ww : 1.0f, sy = wh ? (float)ph / (float)wh : 1.0f;
    float px = wx * sx, py = wy * sy;
    *x = (px - g_cx) / g_cw * 640.0f;
    *y = (py - g_cy) / g_ch * 480.0f;
    return *x >= 0 && *x < 640 && *y >= 0 && *y < 480;
}

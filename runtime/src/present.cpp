#include "present.h"
#include "render_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gl_api.h"
#include "psx.h"

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

// CRT: gentle curvature, gaussian scanlines at the PS1 line rate, aperture-grille mask.
const char* kCrtFS = R"(#version 330 core
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform vec4 u_rect;
uniform vec2 u_out_size;
uniform float u_lines;       // PS1 lines in the image (240 or 480)
in vec2 v_uv;
out vec4 o_color;
vec3 lin(vec3 c) { return c * c; }
void main() {
    vec2 uv = v_uv * 2.0 - 1.0;
    uv *= 1.0 + vec2(uv.y * uv.y, uv.x * uv.x) * vec2(0.035, 0.045);
    vec2 pos = uv * 0.5 + 0.5;
    if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }

    float lines = u_lines > 300.0 ? u_lines * 0.5 : u_lines;
    float line_pos = pos.y * lines;
    float center = floor(line_pos) + 0.5;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = -1; i <= 1; i++) {
        float ly = center + float(i);
        vec2 t = u_rect.xy + vec2(pos.x, ly / lines) * u_rect.zw;
        vec3 c = lin(texture(u_tex, t / u_tex_size).rgb);
        float lum = dot(c, vec3(0.3, 0.6, 0.1));
        float width = mix(0.28, 0.46, lum);          // bright lines bloom wider
        float d = (line_pos - ly) / width;
        float w = exp(-0.5 * d * d);
        acc += c * w;
        wsum += w;
    }
    vec3 col = acc / max(wsum, 0.0001) * 1.0;
    float beam = clamp(wsum, 0.0, 1.0);
    col *= mix(0.55, 1.0, beam);

    int m = int(mod(gl_FragCoord.x, 3.0));
    vec3 mask = m == 0 ? vec3(1.0, 0.72, 0.72) : m == 1 ? vec3(0.72, 1.0, 0.72) : vec3(0.72, 0.72, 1.0);
    col *= mask * 1.35;

    float vig = 1.0 - 0.18 * dot(uv * vec2(0.8, 0.9), uv * vec2(0.8, 0.9));
    col *= vig;
    o_color = vec4(sqrt(clamp(col, 0.0, 1.0)), 1.0);
}
)";

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
    switch (a) {
        case Aspect::A4_3: return 4.0f / 3.0f;
        case Aspect::A16_9: return 16.0f / 9.0f;
        case Aspect::A16_10: return 16.0f / 10.0f;
        default: return (float)w / (float)std::max(h, 1);
    }
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
    if (o.crt) glUniform1f(glGetUniformLocation(prog, "u_lines"), (float)native_h);
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
    g_prog_crt = gl_program(kVS, kCrtFS);
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

    g_ov_surf = SDL_CreateSurface(640, 480, SDL_PIXELFORMAT_ARGB8888);
    g_ov_ren = SDL_CreateSoftwareRenderer(g_ov_surf);
    g_ov_tex = make_texture(640, 480, GL_LINEAR);
    return g_ov_surf && g_ov_ren;
}

void gl_present_shutdown() {
    if (g_ov_ren) SDL_DestroyRenderer(g_ov_ren);
    if (g_ov_surf) SDL_DestroySurface(g_ov_surf);
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
    SDL_SetRenderDrawColor(g_ov_ren, 0, 0, 0, 0);
    SDL_SetRenderDrawBlendMode(g_ov_ren, SDL_BLENDMODE_NONE);
    SDL_RenderClear(g_ov_ren);
    return g_ov_ren;
}

void gl_present_overlay_end() {
    SDL_RenderPresent(g_ov_ren);  // software renderer: flushes into the surface
    glBindTexture(GL_TEXTURE_2D, g_ov_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, g_ov_surf->pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 640, 480, GL_BGRA, GL_UNSIGNED_BYTE, g_ov_surf->pixels);
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
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, flipped.data(), w * 4);
    if (!s || !SDL_SaveBMP(s, path)) log_msg(LogCat::Host, "screenshot %s failed: %s", path, SDL_GetError());
    else log_msg(LogCat::Host, "screenshot saved to %s", path);
    if (s) SDL_DestroySurface(s);
}

void gl_present_swap() { SDL_GL_SwapWindow(g_win); }

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

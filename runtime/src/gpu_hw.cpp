// OpenGL PS1 renderer. See gpu_hw.h.
//
// VRAM lives in two RGBA8 textures of (1024*S)x(512*S): `draw` (render target) and
// `read` (sampled for textures and palettes). Texture row 0 is VRAM row 0, so VRAM
// coordinates map straight to GL coordinates; only the final presentation flips.
// 15-bit colours are stored as (r5,g5,b5)/31 plus alpha = bit 15, which the shaders turn
// back into exact 16-bit values for palette lookups.
#include "gpu_hw.h"
#include "render_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gl_api.h"
#include "psx.h"

namespace {

int S = 0;  // scale; 0 = not initialised
GLuint g_draw_tex, g_draw_fbo, g_read_tex, g_read_fbo, g_stage_tex;
GLuint g_draw_rb = 0;                     // multisampled draw target (instead of g_draw_tex)
int g_samples_wanted = 1, g_samples = 1;  // MSAA samples asked for / in use
GLuint g_prog, g_copy_prog, g_vao, g_vbo;
GLint u_scale, u_texwin, u_pass, u_set_mask, u_filter, u_vram;
GLint uc_src, uc_scale, uc_off;
bool g_filter = false;

struct GLVtx {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t info0, info1;
    float w;  // 1, or view depth for perspective-correct texturing (HwVtx::w)
    uint32_t box;  // HwPrim::box
};

// Primitives are batched while the state below stays the same.
struct BatchKey {
    bool semi = false;
    int semi_mode = 0;
    bool set_mask = false;
    int tw[4] = {};
    int sc[4] = {};  // scissor x1,y1,x2,y2 (native, inclusive)
    bool operator==(const BatchKey& o) const {
        return semi == o.semi && (!semi || semi_mode == o.semi_mode) && set_mask == o.set_mask &&
               std::memcmp(tw, o.tw, sizeof tw) == 0 && std::memcmp(sc, o.sc, sizeof sc) == 0;
    }
};
std::vector<GLVtx> g_batch;
BatchKey g_key;

// Region drawn since `read` was last synchronised (native, exclusive max).
bool g_dirty = false;
int g_dx0, g_dy0, g_dx1, g_dy1;

const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec2 a_info;
layout(location = 4) in float a_w;
layout(location = 5) in uint a_box;
flat out uint v_box;
out vec4 v_color;  // like v_uv: perspective-correct when a_w varies (PGXP colour correction)
out vec2 v_uv;  // perspective-correct when a_w varies (precise 3D); affine when it is 1
flat out uvec2 v_info;
void main() {
    gl_Position = vec4(a_pos.x / 512.0 - 1.0, a_pos.y / 256.0 - 1.0, 0.0, 1.0) * a_w;
    v_color = a_color;
    v_uv = a_uv;
    v_info = a_info;
    v_box = a_box;
}
)";

const char* kFS = R"(#version 330 core
uniform sampler2D u_vram;
uniform int u_scale;
uniform ivec4 u_texwin;   // mask x, mask y, offset x, offset y (8-pixel units)
uniform int u_pass;       // 0: draw all, 1: only opaque texels, 2: only semi-transparent texels
uniform int u_set_mask;
uniform int u_filter;
in vec4 v_color;
in vec2 v_uv;
flat in uvec2 v_info;
flat in uint v_box;
out vec4 o_color;

uint fetch16(ivec2 p) {
    p = ivec2(p.x & 1023, p.y & 511);
    vec4 c = texelFetch(u_vram, p * u_scale, 0);
    uvec4 q = uvec4(round(c * vec4(31.0, 31.0, 31.0, 1.0)));
    return q.r | (q.g << 5) | (q.b << 10) | (q.a << 15);
}

vec4 unpack(uint w) {
    return vec4(float(w & 31u) / 31.0, float((w >> 5) & 31u) / 31.0, float((w >> 10) & 31u) / 31.0, float(w >> 15));
}

ivec2 window(ivec2 t) {
    t = t & 255;
    ivec2 m = u_texwin.xy * 8;
    return (t & ~m) | ((u_texwin.zw & u_texwin.xy) * 8);
}

// Texel colour (rgb + alpha = semi-transparency bit) and whether it is transparent (0000h).
// `sub` is the position inside the texel, used for 15-bit textures at high resolution.
vec4 texel(ivec2 t, vec2 sub, out bool transparent) {
    // only the primitive's own texels (v_box: its vertices' range): at a polygon's edges the
    // interpolated coordinate can land one texel outside it, on another texture or a palette
    // (bright dots along the seams)
    t = clamp(t, ivec2(int(v_box & 255u), int((v_box >> 8) & 255u)), ivec2(int((v_box >> 16) & 255u), int(v_box >> 24)));
    t = window(t);
    ivec2 page = ivec2(int(v_info.x & 1023u), int((v_info.x >> 10) & 511u));
    int depth = int((v_info.x >> 20) & 3u);
    ivec2 clut = ivec2(int(v_info.y & 1023u), int((v_info.y >> 10) & 511u));
    uint w;
    if (depth == 0) {
        uint i = (fetch16(page + ivec2(t.x >> 2, t.y)) >> uint((t.x & 3) * 4)) & 15u;
        w = fetch16(clut + ivec2(int(i), 0));
    } else if (depth == 1) {
        uint i = (fetch16(page + ivec2(t.x >> 1, t.y)) >> uint((t.x & 1) * 8)) & 255u;
        w = fetch16(clut + ivec2(int(i), 0));
    } else {
        // direct colour: sample the high-resolution VRAM inside the texel, so replaced
        // (HD) textures and hi-res render targets keep their detail
        ivec2 p = page + t;
        ivec2 hp = ivec2(p.x & 1023, p.y & 511) * u_scale + ivec2(clamp(sub, 0.0, 0.999) * float(u_scale));
        vec4 c = texelFetch(u_vram, hp, 0);
        transparent = c == vec4(0.0);
        return c;
    }
    transparent = w == 0u;
    return unpack(w);
}

// Game text and icons (HwPrim::smooth): their texels as pixel art at high resolution. Each
// pixel takes the colour that has the most weight around it (a cubic B-spline over 4x4 texels,
// transparent counting as a colour of its own): edges become smooth curves at any slope, and
// colours are never mixed, so the letters stay sharp. Only the glyph's own texels (v_box) count:
// the ones around it in the font are someone else's, and a glyph drawn in parts must not show
// where they meet.
vec4 glyph_key(ivec2 t) {
    ivec2 lo = ivec2(int(v_box & 255u), int((v_box >> 8) & 255u));
    ivec2 hi = ivec2(int((v_box >> 16) & 255u), int(v_box >> 24));
    bool transparent;
    vec4 c = texel(clamp(t, lo, hi), vec2(0.0), transparent);
    return transparent ? vec4(-1.0) : c;
}

vec4 bspline(float f) {
    float g = 1.0 - f;
    return vec4(g * g * g, 4.0 - 6.0 * f * f + 3.0 * f * f * f, 4.0 - 6.0 * g * g + 3.0 * g * g * g, f * f * f) / 6.0;
}

vec4 text_texel(vec2 uv, out bool transparent) {
    vec2 st = uv - 0.5;
    vec2 fl = floor(st + 0.001);
    vec2 f = st - fl;
    ivec2 b = ivec2(fl) - 1;
    vec4 wx = bspline(f.x), wy = bspline(f.y);
    vec4 col[16];
    float w[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            col[j * 4 + i] = glyph_key(b + ivec2(i, j));
            w[j * 4 + i] = wx[i] * wy[j];
        }
    // the winner is one of the four texels around the pixel
    vec4 best = col[5];
    float best_w = -1.0;
    for (int k = 0; k < 4; k++) {
        vec4 cand = col[k == 0 ? 5 : k == 1 ? 6 : k == 2 ? 9 : 10];
        float s = 0.0;
        for (int n = 0; n < 16; n++) s += col[n] == cand ? w[n] : 0.0;
        if (s > best_w) { best_w = s; best = cand; }
    }
    transparent = best.a < 0.0;
    return best;
}

void main() {
    bool textured = ((v_info.x >> 22) & 1u) != 0u;
    bool raw = ((v_info.x >> 23) & 1u) != 0u;
    vec3 col = v_color.rgb;
    bool semi_px = true;
    float mbit = 0.0;
    if (textured) {
        // PS1 samples at the pixel's top-left corner, GL at its centre
        vec2 uv = v_uv - 0.5 * dFdx(v_uv) - 0.5 * dFdy(v_uv);
        vec4 tc;
        bool transparent;
        if (((v_info.x >> 24) & 1u) != 0u) {  // game text
            tc = text_texel(uv, transparent);
        } else if (u_filter != 0) {
            vec2 st = uv - 0.5;
            ivec2 b = ivec2(floor(st));
            vec2 f = st - vec2(b);
            vec4 acc = vec4(0.0);
            float wsum = 0.0;
            bool t00, t10, t01, t11;
            vec4 c00 = texel(b, vec2(0.0), t00), c10 = texel(b + ivec2(1, 0), vec2(0.0), t10);
            vec4 c01 = texel(b + ivec2(0, 1), vec2(0.0), t01), c11 = texel(b + ivec2(1, 1), vec2(0.0), t11);
            float w00 = (1.0 - f.x) * (1.0 - f.y), w10 = f.x * (1.0 - f.y), w01 = (1.0 - f.x) * f.y, w11 = f.x * f.y;
            if (!t00) { acc += c00 * w00; wsum += w00; }
            if (!t10) { acc += c10 * w10; wsum += w10; }
            if (!t01) { acc += c01 * w01; wsum += w01; }
            if (!t11) { acc += c11 * w11; wsum += w11; }
            if (wsum < 0.5) discard;
            tc = acc / wsum;
            tc.a = step(0.5, tc.a);
            transparent = false;
        } else {
            vec2 fl = floor(uv + 0.001);
            tc = texel(ivec2(fl), uv - fl, transparent);
        }
        if (transparent) discard;
        semi_px = tc.a > 0.5;
        col = raw ? tc.rgb : min(tc.rgb * v_color.rgb * (255.0 / 128.0), vec3(1.0));
        mbit = semi_px ? 1.0 : 0.0;
    }
    if (u_pass == 1 && semi_px) discard;
    if (u_pass == 2 && !semi_px) discard;
    o_color = vec4(col, max(mbit, float(u_set_mask)));
}
)";

// Copies the staging texture (native resolution) into the scaled VRAM: nearest filtering.
const char* kCopyVS = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos.x / 512.0 - 1.0, a_pos.y / 256.0 - 1.0, 0.0, 1.0); }
)";
// Copies texels into the draw target by drawing (a multisampled target cannot be blitted
// into): destination pixel p takes source texel (p - u_off) / u_scale.
const char* kCopyFS = R"(#version 330 core
uniform sampler2D u_src;
uniform int u_scale;
uniform ivec2 u_off;
out vec4 o_color;
void main() { o_color = texelFetch(u_src, (ivec2(gl_FragCoord.xy) - u_off) / u_scale, 0); }
)";

GLuint make_vram_texture(int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

GLuint make_fbo(GLuint tex) {
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return f;
}

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

void set_blend(int mode) {
    glEnable(GL_BLEND);
    switch (mode) {
        case 0:  // B/2 + F/2
            glBlendColor(0, 0, 0, 0.5f);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
            break;
        case 1:  // B + F
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
            break;
        case 2:  // B - F
            glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
            break;
        default:  // B + F/4
            glBlendColor(0, 0, 0, 0.25f);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
            break;
    }
}

void bind_draw_target() {
    glBindFramebuffer(GL_FRAMEBUFFER, g_draw_fbo);
    glViewport(0, 0, 1024 * S, 512 * S);
}

void flush() {
    if (g_batch.empty()) return;
    bind_draw_target();
    glEnable(GL_SCISSOR_TEST);
    glScissor(g_key.sc[0] * S, g_key.sc[1] * S, (g_key.sc[2] - g_key.sc[0] + 1) * S,
              (g_key.sc[3] - g_key.sc[1] + 1) * S);
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_read_tex);
    glUniform1i(u_vram, 0);
    glUniform1i(u_scale, S);
    glUniform4i(u_texwin, g_key.tw[0], g_key.tw[1], g_key.tw[2], g_key.tw[3]);
    glUniform1i(u_set_mask, g_key.set_mask ? 1 : 0);
    glUniform1i(u_filter, g_filter ? 1 : 0);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_batch.size() * sizeof(GLVtx)), g_batch.data(), GL_STREAM_DRAW);
    if (!g_key.semi) {
        glDisable(GL_BLEND);
        glUniform1i(u_pass, 0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
    } else {
        glDisable(GL_BLEND);
        glUniform1i(u_pass, 1);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
        set_blend(g_key.semi_mode);
        glUniform1i(u_pass, 2);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
        glDisable(GL_BLEND);
    }
    glDisable(GL_SCISSOR_TEST);
    g_batch.clear();
}

// Makes `read` match `draw` for the region drawn since the last sync.
void sync_read() {
    flush();
    if (!g_dirty) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_draw_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_read_fbo);
    glBlitFramebuffer(g_dx0 * S, g_dy0 * S, g_dx1 * S, g_dy1 * S, g_dx0 * S, g_dy0 * S, g_dx1 * S, g_dy1 * S,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
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
    static bool warned_mask = false;
    if (s.check_mask && !warned_mask) {
        warned_mask = true;
        log_msg(LogCat::Gpu, "hardware renderer: mask-bit test is not emulated");
    }
    prepare_textures(p);
    BatchKey k;
    k.semi = p.semi;
    k.semi_mode = p.semi_mode;
    k.set_mask = s.set_mask;
    k.tw[0] = s.tw_mask_x; k.tw[1] = s.tw_mask_y; k.tw[2] = s.tw_off_x; k.tw[3] = s.tw_off_y;
    k.sc[0] = s.area_x1; k.sc[1] = s.area_y1; k.sc[2] = s.area_x2; k.sc[3] = s.area_y2;
    if (!(k == g_key)) {
        flush();
        g_key = k;
    }
}

uint32_t info0(const HwPrim& p) {
    return (uint32_t)(p.page_x & 1023) | (uint32_t)(p.page_y & 511) << 10 | (uint32_t)p.depth << 20 |
           (uint32_t)p.textured << 22 | (uint32_t)p.raw << 23 | (uint32_t)p.smooth << 24;
}
uint32_t info1(const HwPrim& p) { return (uint32_t)(p.clut_x & 1023) | (uint32_t)(p.clut_y & 511) << 10; }

void push(float x, float y, int r, int g, int b, float u, float v, const HwPrim& p, float w = 1.0f) {
    GLVtx t;
    t.x = x; t.y = y; t.w = w;
    t.r = (uint8_t)r; t.g = (uint8_t)g; t.b = (uint8_t)b; t.a = 255;
    t.u = u; t.v = v;
    t.info0 = info0(p);
    t.info1 = info1(p);
    t.box = p.box;
    g_batch.push_back(t);
}

void draw_area_dirty(int x0, int y0, int x1, int y1, const HwState& s) {
    mark_dirty(std::max(x0, s.area_x1), std::max(y0, s.area_y1), std::min(x1, s.area_x2 + 1),
               std::min(y1, s.area_y2 + 1));
}

}  // namespace

// ================================================================ lifecycle

bool gl_hw_init(int scale) {
    if (!g_prog) {
        g_prog = gl_program(kVS, kFS);
        g_copy_prog = gl_program(kCopyVS, kCopyFS);
        if (!g_prog || !g_copy_prog) return false;
        u_scale = glGetUniformLocation(g_prog, "u_scale");
        u_texwin = glGetUniformLocation(g_prog, "u_texwin");
        u_pass = glGetUniformLocation(g_prog, "u_pass");
        u_set_mask = glGetUniformLocation(g_prog, "u_set_mask");
        u_filter = glGetUniformLocation(g_prog, "u_filter");
        u_vram = glGetUniformLocation(g_prog, "u_vram");
        uc_src = glGetUniformLocation(g_copy_prog, "u_src");
        uc_scale = glGetUniformLocation(g_copy_prog, "u_scale");
        uc_off = glGetUniformLocation(g_copy_prog, "u_off");
        glGenVertexArrays(1, &g_vao);
        glGenBuffers(1, &g_vbo);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLVtx), (void*)offsetof(GLVtx, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVtx), (void*)offsetof(GLVtx, r));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLVtx), (void*)offsetof(GLVtx, u));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 2, GL_UNSIGNED_INT, sizeof(GLVtx), (void*)offsetof(GLVtx, info0));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GLVtx), (void*)offsetof(GLVtx, w));
        glEnableVertexAttribArray(5);
        glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(GLVtx), (void*)offsetof(GLVtx, box));
        g_stage_tex = make_vram_texture(1024, 512);
    }
    gl_hw_shutdown();
    S = std::clamp(scale, 1, 8);
    g_read_tex = make_vram_texture(1024 * S, 512 * S);
    g_read_fbo = make_fbo(g_read_tex);
    // Multisampling: polygons are drawn into a multisampled target that is resolved into
    // `read` (which textures and the display come from). Sample counts that would take more
    // than a quarter of the video memory (at least 512 MB), or more than the GPU offers, are lowered.
    g_samples = 1;
    if (g_samples_wanted > 1) {
        GLint max_samples = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        int n = std::min(g_samples_wanted, (int)max_samples);
        // video memory, where the driver says: GL_NVX_gpu_memory_info (total) or GL_ATI_meminfo
        // (free texture memory), both in KB
        uint64_t budget = 512ull << 20;
        GLint kb[4] = {};
        while (glGetError() != GL_NO_ERROR) {}
        glGetIntegerv(0x9048, kb);  // GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
        if (glGetError() != GL_NO_ERROR || kb[0] <= 0) {
            kb[0] = 0;
            glGetIntegerv(0x87FC, kb);  // TEXTURE_FREE_MEMORY_ATI
            if (glGetError() != GL_NO_ERROR) kb[0] = 0;
        }
        if (kb[0] > 0) budget = std::max<uint64_t>(budget, (uint64_t)kb[0] * 1024 / 4);
        log_msg(LogCat::Gpu, "OpenGL: %d MB video memory reported, MSAA image up to %llu MB", kb[0] / 1024,
                (unsigned long long)(budget >> 20));
        while (n > 1 && (uint64_t)1024 * S * 512 * S * 4 * (uint64_t)n > budget) n /= 2;
        while (n > 1) {
            while (glGetError() != GL_NO_ERROR) {}
            glGenRenderbuffers(1, &g_draw_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, g_draw_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, n, GL_RGBA8, 1024 * S, 512 * S);
            glGenFramebuffers(1, &g_draw_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_draw_fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_draw_rb);
            if (glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                g_samples = n;
                break;
            }
            glDeleteFramebuffers(1, &g_draw_fbo);
            glDeleteRenderbuffers(1, &g_draw_rb);
            g_draw_fbo = g_draw_rb = 0;
            n /= 2;
        }
    }
    if (g_samples == 1) {
        g_draw_tex = make_vram_texture(1024 * S, 512 * S);
        g_draw_fbo = make_fbo(g_draw_tex);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_msg(LogCat::Gpu, "hardware renderer: %dx VRAM framebuffer not supported", S);
        gl_hw_shutdown();
        return false;
    }
    g_dirty = false;
    g_batch.clear();
    g_key = BatchKey{};
    log_msg(LogCat::Gpu, "hardware renderer: %dx (%dx%d VRAM), %dx MSAA", S, 1024 * S, 512 * S, g_samples);
    return true;
}

struct Capture {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;  // texels
};
static Capture g_caps[16];
static Capture g_stash;
static int g_stash_x, g_stash_y, g_stash_w, g_stash_h;

static void free_capture(Capture& c) {
    if (c.fbo) glDeleteFramebuffers(1, &c.fbo);
    if (c.tex) glDeleteTextures(1, &c.tex);
    c = Capture{};
}

// Copies a native VRAM rectangle of the draw target into `c` (resized as needed).
static void copy_out(Capture& c, int x, int y, int w, int h) {
    flush();
    if (c.w != w * S || c.h != h * S) {
        free_capture(c);
        c.tex = make_vram_texture(w * S, h * S);
        c.fbo = make_fbo(c.tex);
        c.w = w * S;
        c.h = h * S;
    }
    if (g_samples > 1) sync_read();  // resolved copy
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_samples > 1 ? g_read_fbo : g_draw_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c.fbo);
    glBlitFramebuffer(x * S, y * S, (x + w) * S, (y + h) * S, 0, 0, w * S, h * S, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static bool clip_rect(int& x, int& y, int& w, int& h) {
    x = std::clamp(x, 0, 1023);
    y = std::clamp(y, 0, 511);
    w = std::min(w, 1024 - x);
    h = std::min(h, 512 - y);
    return w > 0 && h > 0;
}

void gl_hw_shutdown() {
    if (!S) return;
    for (auto& c : g_caps) free_capture(c);
    free_capture(g_stash);
    glDeleteFramebuffers(1, &g_draw_fbo);
    glDeleteFramebuffers(1, &g_read_fbo);
    if (g_draw_tex) glDeleteTextures(1, &g_draw_tex);
    if (g_draw_rb) glDeleteRenderbuffers(1, &g_draw_rb);
    glDeleteTextures(1, &g_read_tex);
    g_draw_tex = g_draw_rb = 0;
    S = 0;
}

bool gl_hw_active() { return S != 0; }
int gl_hw_scale() { return S; }
int gl_hw_samples() { return S ? g_samples : 0; }
void gl_hw_set_samples(int n) { g_samples_wanted = std::clamp(n, 1, 8); }
int gl_hw_samples_wanted() { return g_samples_wanted; }
void gl_hw_set_texture_filter(bool on) {
    if (on != g_filter) flush();
    g_filter = on;
}

// ================================================================ primitives

void gl_hw_triangle(const HwVtx* a, const HwVtx* b, const HwVtx* c, const HwPrim& p, const HwState& s) {
    begin_prim(p, s);
    for (const HwVtx* v : {a, b, c}) {
        const HwVtx* col = p.gouraud ? v : a;
        push((float)v->x, (float)v->y, col->r, col->g, col->b, (float)v->u, (float)v->v, p, v->w);
    }
    draw_area_dirty((int)std::floor(std::min({a->x, b->x, c->x})), (int)std::floor(std::min({a->y, b->y, c->y})),
                    (int)std::ceil(std::max({a->x, b->x, c->x})) + 1, (int)std::ceil(std::max({a->y, b->y, c->y})) + 1, s);
}

void gl_hw_rect(int x, int y, int w, int h, int u0, int v0, bool flip_x, bool flip_y, uint32_t color, const HwPrim& p,
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

void gl_hw_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gouraud, const HwPrim& p,
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

void gl_hw_fill(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    flush();
    uint32_t c = rgba_of(color & 0x7FFF);
    bind_draw_target();
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * S, y * S, std::min(w, 1024 - x) * S, std::min(h, 512 - y) * S);
    glClearColor((c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    mark_dirty(x, y, x + w, y + h);
}

// Draws texels of `src` into the draw target over the native rectangle x, y, w, h: target
// pixel p takes texel (p - off) / scale (see kCopyFS). Used where a blit cannot write
// (a multisampled target).
static void draw_copy(GLuint src, int offx, int offy, int scale, int x, int y, int w, int h) {
    glUseProgram(g_copy_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(uc_src, 0);
    glUniform1i(uc_scale, scale);
    glUniform2i(uc_off, offx, offy);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    GLVtx q[6] = {};
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    float c[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 6; i++) { q[i].x = c[i][0]; q[i].y = c[i][1]; }
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glDisable(GL_BLEND);
    bind_draw_target();
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Uploads a native rectangle of the CPU VRAM copy into both GPU copies.
static void upload_rect(int x, int y, int w, int h, const uint16_t* vram) {
    std::vector<uint32_t> px((size_t)w * h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px[(size_t)j * w + i] = rgba_of(vram[(y + j) * 1024 + x + i]);
    glBindTexture(GL_TEXTURE_2D, g_stage_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    glUseProgram(g_copy_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_stage_tex);
    glUniform1i(uc_src, 0);
    glUniform1i(uc_scale, S);
    glUniform2i(uc_off, 0, 0);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    GLVtx q[6] = {};
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    float c[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 6; i++) { q[i].x = c[i][0]; q[i].y = c[i][1]; }
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glDisable(GL_BLEND);
    for (GLuint fbo : {g_draw_fbo, g_read_fbo}) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 1024 * S, 512 * S);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
}

void gl_hw_upload(int x, int y, int w, int h, const uint16_t* vram) {
    flush();
    // the region may wrap around the edges of VRAM
    for (int yy = y; yy < y + h;) {
        int ry = yy & 511, rh = std::min(y + h - yy, 512 - ry);
        for (int xx = x; xx < x + w;) {
            int rx = xx & 1023, rw = std::min(x + w - xx, 1024 - rx);
            // the uploaded area in `read` is now current; if it overlaps the dirty
            // region, sync the rest of that region first so nothing newer is lost
            if (overlaps_dirty(rx, ry, rx + rw, ry + rh)) sync_read();
            upload_rect(rx, ry, rw, rh, vram);
            xx += rw;
        }
        yy += rh;
    }
}

void gl_hw_upload_all(const uint16_t* vram) {
    flush();
    upload_rect(0, 0, 1024, 512, vram);
    g_dirty = false;
}

void gl_hw_draw_picture(int x, int y, int w, int h, const uint32_t* rgba) {
    if (!S || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > 1024 || y + h > 512) return;
    if (overlaps_dirty(x, y, x + w, y + h)) sync_read();
    else flush();
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * S, h * S, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    draw_copy(tex, x * S, y * S, 1, x, y, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &tex);
    mark_dirty(x, y, x + w, y + h);
}

void gl_hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (overlaps_dirty(sx, sy, sx + w, sy + h)) sync_read();
    else flush();
    w = std::min({w, 1024 - sx, 1024 - dx});
    h = std::min({h, 512 - sy, 512 - dy});
    if (w <= 0 || h <= 0) return;
    if (g_samples > 1) {
        draw_copy(g_read_tex, (dx - sx) * S, (dy - sy) * S, 1, dx, dy, w, h);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_read_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_draw_fbo);
        glBlitFramebuffer(sx * S, sy * S, (sx + w) * S, (sy + h) * S, dx * S, dy * S, (dx + w) * S, (dy + h) * S,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    mark_dirty(dx, dy, dx + w, dy + h);
}

void gl_hw_readback(int x, int y, int w, int h, uint16_t* vram) {
    flush();
    w = std::min(w, 1024 - x);
    h = std::min(h, 512 - y);
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> px((size_t)w * S * h * S);
    if (g_samples > 1) sync_read();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_samples > 1 ? g_read_fbo : g_draw_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x * S, y * S, w * S, h * S, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint32_t c = px[(size_t)j * S * w * S + (size_t)i * S];
            auto q = [](uint32_t v) { return (uint16_t)((v * 31 + 127) / 255); };
            uint16_t p = (uint16_t)(q(c & 0xFF) | q((c >> 8) & 0xFF) << 5 | q((c >> 16) & 0xFF) << 10 |
                                    ((c >> 24) > 127 ? 0x8000 : 0));
            vram[(y + j) * 1024 + x + i] = p;
        }
    }
}

void gl_hw_capture(int slot, int x, int y, int w, int h) {
    if (!S || slot < 0 || slot >= 16 || !clip_rect(x, y, w, h)) return;
    copy_out(g_caps[slot], x, y, w, h);
}

unsigned gl_hw_capture_texture(int slot, int* tex_w, int* tex_h) {
    if (!S || slot < 0 || slot >= 16 || !g_caps[slot].tex) return 0;
    *tex_w = g_caps[slot].w;
    *tex_h = g_caps[slot].h;
    return g_caps[slot].tex;
}

void gl_hw_stash(int x, int y, int w, int h) {
    g_stash_w = 0;
    if (!S || !clip_rect(x, y, w, h)) return;
    copy_out(g_stash, x, y, w, h);
    g_stash_x = x;
    g_stash_y = y;
    g_stash_w = w;
    g_stash_h = h;
}

void gl_hw_unstash() {
    if (!S || !g_stash_w) return;
    flush();
    if (g_samples > 1) {
        draw_copy(g_stash.tex, g_stash_x * S, g_stash_y * S, 1, g_stash_x, g_stash_y, g_stash_w, g_stash_h);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_stash.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_draw_fbo);
        glBlitFramebuffer(0, 0, g_stash_w * S, g_stash_h * S, g_stash_x * S, g_stash_y * S,
                          (g_stash_x + g_stash_w) * S, (g_stash_y + g_stash_h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mark_dirty(g_stash_x, g_stash_y, g_stash_x + g_stash_w, g_stash_y + g_stash_h);  // read copy re-syncs
    g_stash_w = 0;
}

unsigned gl_hw_display_texture(int x, int y, int w, int h, int* px, int* py, int* pw, int* ph) {
    if (g_samples > 1) sync_read();  // show the resolved image
    else flush();
    *px = x * S;
    *py = y * S;
    *pw = w * S;
    *ph = h * S;
    return g_samples > 1 ? g_read_tex : g_draw_tex;
}

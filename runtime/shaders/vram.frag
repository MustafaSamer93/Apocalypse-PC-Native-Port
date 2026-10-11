#version 450
// Vulkan port of gpu_gl.cpp kFS: palette lookups, texture windows, semi-transparency passes.
layout(set = 0, binding = 0) uniform sampler2D u_vram;
layout(set = 1, binding = 0) uniform sampler2D u_hd;  // replacement textures (bit 27: v_uv is in its pixels)
layout(push_constant) uniform PC {
    int scale;
    int pass_;     // 0: draw all, 1: only opaque texels, 2: only semi-transparent texels
    int set_mask;
    int filter_;
    ivec4 texwin;  // mask x, mask y, offset x, offset y (8-pixel units)
} pc;
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uvec2 v_info;
layout(location = 3) flat in uint v_box;
layout(location = 4) in float v_z;
layout(location = 0) out vec4 o_color;

uint fetch16(ivec2 p) {
    p = ivec2(p.x & 1023, p.y & 511);
    vec4 c = texelFetch(u_vram, p * pc.scale, 0);
    uvec4 q = uvec4(round(c * vec4(31.0, 31.0, 31.0, 1.0)));
    return q.r | (q.g << 5) | (q.b << 10) | (q.a << 15);
}

vec4 unpack(uint w) {
    return vec4(float(w & 31u) / 31.0, float((w >> 5) & 31u) / 31.0, float((w >> 10) & 31u) / 31.0, float(w >> 15));
}

ivec2 window(ivec2 t) {
    t = t & 255;
    ivec2 m = pc.texwin.xy * 8;
    return (t & ~m) | ((pc.texwin.zw & pc.texwin.xy) * 8);
}

// Texel colour (rgb + alpha = semi-transparency bit) and whether it is transparent (0000h).
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
        ivec2 p = page + t;
        ivec2 hp = ivec2(p.x & 1023, p.y & 511) * pc.scale + ivec2(clamp(sub, 0.0, 0.999) * float(pc.scale));
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

// Replacement textures (HwPrim::hd): HD texel p of mip level l (n = 4 >> l per game texel), in the
// texture's own coordinates. p stays inside the polygon's own texels (v_box) and its game texel
// goes through the texture window, so a filter never reaches another texture, the atlas's empty
// parts or the wrong side of a repeating floor.
vec4 hd_fetch(ivec2 p, int l) {
    int sh = 2 - l, n = 1 << sh;
    ivec2 lo = ivec2(int(v_box & 255u), int((v_box >> 8) & 255u)), hi = ivec2(int((v_box >> 16) & 255u), int(v_box >> 24));
    p = clamp(p, lo * n, hi * n + n - 1);
    int d = int((v_info.x >> 20) & 3u);
    ivec2 base = ivec2((int(v_info.x & 1023u) - 512) * (d == 0 ? 4 : 2), int((v_info.x >> 10) & 511u) + (d == 1 ? 512 : 0));
    return texelFetch(u_hd, (base + window(p >> sh)) * n + (p & (n - 1)), l);
}

// Bilinear at level l, weighted by alpha (see-through texels don't darken the edges): colour
// times coverage in rgb, coverage in a.
vec4 hd_level(vec2 uv, int l) {
    vec2 st = uv * float(4 >> l) - 0.5;
    ivec2 b = ivec2(floor(st));
    vec2 f = st - vec2(b);
    vec4 acc = vec4(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec4 c = hd_fetch(b + ivec2(i, j), l);
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y) * c.a;
            acc += vec4(c.rgb * w, w);
        }
    return acc;
}

// Trilinear over the atlas's levels 0-2 (texreplace.cpp makes them): level 2 is one pixel per
// game texel, so far away it looks like the game's own texture instead of shimmering.
vec4 hd_texel(vec2 uv, vec2 dx, vec2 dy, out bool transparent) {
    float lod = clamp(log2(max(length(dx), length(dy)) * 4.0), 0.0, 2.0);
    int l = int(lod);
    float m = lod - float(l);
    vec4 acc = hd_level(uv, l);
    if (m > 0.0) acc = mix(acc, hd_level(uv, l + 1), m);
    transparent = acc.a < 0.5;
    return vec4(acc.rgb / max(acc.a, 1e-4), 1.0);
}

void main() {
    bool textured = ((v_info.x >> 22) & 1u) != 0u;
    bool raw = ((v_info.x >> 23) & 1u) != 0u;
    vec3 col = v_color.rgb;
    bool semi_px = true;
    float mbit = 0.0;
    if (textured) {
        // PS1 samples at the pixel's top-left corner, the GPU at its centre
        vec2 duv_x = dFdx(v_uv), duv_y = dFdy(v_uv);
        vec2 uv = v_uv - 0.5 * duv_x - 0.5 * duv_y;
        vec4 tc;
        bool transparent;
        if (((v_info.x >> 27) & 1u) != 0u) {  // a replacement texture: its HD copy of VRAM (texreplace.cpp)
            tc = hd_texel(v_uv, duv_x, duv_y, transparent);
            // the game's own texel decides what is solid and semi-transparent: palette black uploaded
            // with the mask bit (8000h) is solid on the PS1, while exports (and so replacements) have it
            // see-through. There the game's colour shows; the replacement keeps its own soft edges.
            bool game_transparent;
            vec4 g = texel(ivec2(floor(v_uv)), vec2(0.0), game_transparent);
            if (((v_info.x >> 28) & 1u) != 0u) {
                // a letter in the game font (texreplace.cpp): its shape, coloured like the game's
                // letter on that row (the nearest row with any), in the palette it is drawn with.
                // Semi-transparent or not goes by the row too: the shape is not the game letter's,
                // and by the texel under it the edges drawn big showed the game letter's steps
                int lo = int(v_box & 255u), hi = int((v_box >> 16) & 255u), y = int(floor(v_uv.y));
                vec4 sum = vec4(0.0);
                float n = 0.0;
                for (int d = 0; d < 8 && n == 0.0; d++)
                    for (int s = -1; s <= 1; s += 2) {
                        int row = y + d * s;
                        if (n > 0.0 || (d == 0 && s > 0)) continue;
                        for (int x = lo; x <= hi && x < lo + 64; x++) {
                            bool tr;
                            vec4 c = texel(ivec2(x, row), vec2(0.0), tr);
                            if (!tr) { sum += c; n += 1.0; }
                        }
                    }
                if (n > 0.0) { tc.rgb = sum.rgb / n; g.a = step(0.5, sum.a / n); }
            } else if (transparent && !game_transparent) { tc.rgb = g.rgb; transparent = false; }
            tc.a = g.a;
        } else if (((v_info.x >> 24) & 1u) != 0u) {  // game text
            tc = text_texel(uv, transparent);
        } else if (pc.filter_ != 0) {
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
    if (pc.pass_ == 1 && semi_px) discard;
    if (pc.pass_ == 2 && !semi_px) discard;
    o_color = vec4(col, max(mbit, float(pc.set_mask)));
    gl_FragDepth = clamp(v_z * (1.0 / 65536.0), 0.0, 1.0);  // Z-buffer (HwPrim::ztest batches)
}

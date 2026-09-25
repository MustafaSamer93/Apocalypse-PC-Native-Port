#version 450
// Vulkan port of gpu_hw.cpp kFS: palette lookups, texture windows, semi-transparency passes.
layout(set = 0, binding = 0) uniform sampler2D u_vram;
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

void main() {
    bool textured = ((v_info.x >> 22) & 1u) != 0u;
    bool raw = ((v_info.x >> 23) & 1u) != 0u;
    vec3 col = v_color.rgb;
    bool semi_px = true;
    float mbit = 0.0;
    if (textured) {
        // PS1 samples at the pixel's top-left corner, the GPU at its centre
        vec2 uv = v_uv - 0.5 * dFdx(v_uv) - 0.5 * dFdy(v_uv);
        vec4 tc;
        bool transparent;
        if (pc.filter_ != 0) {
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
}

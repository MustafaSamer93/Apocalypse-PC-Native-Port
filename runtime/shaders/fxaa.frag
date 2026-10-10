#version 450
// Vulkan port of present_gl.cpp kFxaaFS (the source image is top-down here: no flip).
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC { vec2 px; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 p) { return texture(u_tex, p).rgb; }
void main() {
    vec2 uv = v_uv;
    vec3 m = at(uv);
    float lNW = luma(at(uv + vec2(-1.0, -1.0) * pc.px)), lNE = luma(at(uv + vec2(1.0, -1.0) * pc.px));
    float lSW = luma(at(uv + vec2(-1.0, 1.0) * pc.px)), lSE = luma(at(uv + vec2(1.0, 1.0) * pc.px));
    float lM = luma(m);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) { o_color = vec4(m, 1.0); return; }
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * pc.px;
    vec3 a = 0.5 * (at(uv + dir * (1.0 / 3.0 - 0.5)) + at(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (at(uv - dir * 0.5) + at(uv + dir * 0.5));
    float lB = luma(b);
    o_color = vec4((lB < lMin || lB > lMax) ? a : b, 1.0);
}

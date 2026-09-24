#version 450
// Vulkan port of present.cpp kCrtFS: curvature, gaussian scanlines, aperture-grille mask.
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC { vec4 rect; vec2 tex_size; vec2 out_size; float lines; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 lin(vec3 c) { return c * c; }
void main() {
    vec2 uv = v_uv * 2.0 - 1.0;
    uv *= 1.0 + vec2(uv.y * uv.y, uv.x * uv.x) * vec2(0.035, 0.045);
    vec2 pos = uv * 0.5 + 0.5;
    if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    float lines = pc.lines > 300.0 ? pc.lines * 0.5 : pc.lines;
    float line_pos = pos.y * lines;
    float center = floor(line_pos) + 0.5;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = -1; i <= 1; i++) {
        float ly = center + float(i);
        vec2 t = pc.rect.xy + vec2(pos.x, ly / lines) * pc.rect.zw;
        vec3 c = lin(texture(u_tex, t / pc.tex_size).rgb);
        float lum = dot(c, vec3(0.3, 0.6, 0.1));
        float width = mix(0.28, 0.46, lum);
        float d = (line_pos - ly) / width;
        float w = exp(-0.5 * d * d);
        acc += c * w;
        wsum += w;
    }
    vec3 col = acc / max(wsum, 0.0001);
    float beam = clamp(wsum, 0.0, 1.0);
    col *= mix(0.55, 1.0, beam);
    int m = int(mod(gl_FragCoord.x, 3.0));
    vec3 mask = m == 0 ? vec3(1.0, 0.72, 0.72) : m == 1 ? vec3(0.72, 1.0, 0.72) : vec3(0.72, 0.72, 1.0);
    col *= mask * 1.35;
    float vig = 1.0 - 0.18 * dot(uv * vec2(0.8, 0.9), uv * vec2(0.8, 0.9));
    col *= vig;
    o_color = vec4(sqrt(clamp(col, 0.0, 1.0)), 1.0);
}

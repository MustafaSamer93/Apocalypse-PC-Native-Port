#version 450
// Vulkan port of present.cpp kPlainFS: area-average downsampling, sharp bilinear upscaling.
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC { vec4 rect; vec2 tex_size; vec2 out_size; float lines; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 texel = pc.rect.xy + v_uv * pc.rect.zw;
    vec2 fp = pc.rect.zw / pc.out_size;
    if (fp.x > 1.01 || fp.y > 1.01) {
        ivec2 n = ivec2(clamp(ceil(fp), vec2(1.0), vec2(4.0)));
        vec3 acc = vec3(0.0);
        for (int j = 0; j < n.y; j++)
            for (int i = 0; i < n.x; i++) {
                vec2 t = texel + ((vec2(i, j) + 0.5) / vec2(n) - 0.5) * fp;
                acc += texture(u_tex, t / pc.tex_size).rgb;
            }
        o_color = vec4(acc / float(n.x * n.y), 1.0);
        return;
    }
    vec2 scale = max(floor(pc.out_size / pc.rect.zw), vec2(1.0));
    vec2 region = 0.5 - 0.5 / scale;
    vec2 f = fract(texel) - 0.5;
    vec2 off = (f - clamp(f, -region, region)) * scale + 0.5;
    vec2 p = floor(texel) + off;
    o_color = vec4(texture(u_tex, p / pc.tex_size).rgb, 1.0);
}

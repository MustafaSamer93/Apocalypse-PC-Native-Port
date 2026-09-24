#version 450
// PS1 primitives into scaled VRAM (Vulkan port of gpu_hw.cpp kVS). VRAM row 0 is image row 0.
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec2 a_info;
layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec2 v_uv;
layout(location = 2) flat out uvec2 v_info;
void main() {
    gl_Position = vec4(a_pos.x / 512.0 - 1.0, a_pos.y / 256.0 - 1.0, 0.0, 1.0);
    v_color = a_color;
    v_uv = a_uv;
    v_info = a_info;
}

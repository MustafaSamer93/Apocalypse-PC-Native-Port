#version 450
// PS1 primitives into scaled VRAM (Vulkan port of gpu_gl.cpp kVS). VRAM row 0 is image row 0.
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec2 a_info;
layout(location = 4) in float a_w;
layout(location = 5) in uint a_box;  // HwPrim::box
layout(location = 6) in float a_z;   // Z-buffer depth (HwVtx::z)
layout(push_constant) uniform PC {
    int scale, pass_, set_mask, filter_;
    ivec4 texwin;
    vec4 origin;  // the VRAM rectangle the target covers (x, y, w, h): all of VRAM, or a frame (hw_replay)
} pc;
layout(location = 0) out vec4 v_color;  // like v_uv: perspective-correct when a_w varies
layout(location = 1) out vec2 v_uv;  // perspective-correct when a_w varies (precise 3D); affine when it is 1
layout(location = 2) flat out uvec2 v_info;
layout(location = 3) flat out uint v_box;
layout(location = 4) out float v_z;
void main() {
    gl_Position = vec4((a_pos - pc.origin.xy) / pc.origin.zw * 2.0 - 1.0, 0.0, 1.0) * a_w;
    v_color = a_color;
    v_uv = a_uv;
    v_info = a_info;
    v_box = a_box;
    v_z = a_z;
}

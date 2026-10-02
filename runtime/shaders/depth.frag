#version 450
// Soft glows: a triangle's view depth into the depth image (gpu_vk.cpp vk_hw_depth_draw).
layout(location = 4) in float v_z;
layout(location = 0) out vec4 o_z;
void main() { o_z = vec4(v_z, 0.0, 0.0, 1.0); }

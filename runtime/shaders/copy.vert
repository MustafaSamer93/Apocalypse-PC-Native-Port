#version 450
layout(location = 0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos.x / 512.0 - 1.0, a_pos.y / 256.0 - 1.0, 0.0, 1.0); }

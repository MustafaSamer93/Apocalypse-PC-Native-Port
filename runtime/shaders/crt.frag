#version 450
#extension GL_GOOGLE_include_directive : require
// The CRT filter (crt_body.glsl; the OpenGL side is present_gl.cpp).
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC { vec4 rect; vec2 tex_size; vec2 out_size; float lines; float time; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
#define SRC u_tex
#define RECT pc.rect
#define TEX_SIZE pc.tex_size
#define OUT_SIZE pc.out_size
#define LINES pc.lines
#define TIME pc.time
#include "crt_body.glsl"
void main() { o_color = crt(v_uv, gl_FragCoord.x); }

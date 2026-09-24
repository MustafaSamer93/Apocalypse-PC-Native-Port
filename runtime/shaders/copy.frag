#version 450
// Texels into a target by drawing: target pixel p takes source texel (p - off) / scale.
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform PC { int scale; int pad; ivec2 off; } pc;
layout(location = 0) out vec4 o_color;
void main() { o_color = texelFetch(u_src, (ivec2(gl_FragCoord.xy) - pc.off) / pc.scale, 0); }

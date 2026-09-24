#version 450
// Full-viewport quad from the vertex index; v_uv is 0..1 with y down (image space).
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = p;
}

#version 450
// fg_cutsel_rt.frag's triangle over the whole picture
void main() { vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0); }

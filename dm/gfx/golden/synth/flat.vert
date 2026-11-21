#version 450
// Synthetic golden frame (golden_synth.c): a vertex-coloured triangle, drawn into an offscreen target in frame 1 and sampled in the recorded frame 2
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
layout(location = 0) out vec3 vcol;
void main() { vcol = col; gl_Position = vec4(pos, 1.0); }

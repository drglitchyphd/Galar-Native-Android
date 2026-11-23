#version 450
// Synthetic golden frame, GOLDEN_SYNTH_B10 (golden_synth.c): a colour with a negative channel and one past R11G11B10F's range, drawn blended by MAX into an
//   R11G11B10F target holding (0.5, 2, 0): stored (0.5, 65024, 0.25) whether the value is rounded before the max or after it
layout(location = 0) in vec3 vcol;
layout(location = 0) out vec4 o;
void main() { o = vec4(-1.0, 70000.0, 0.25, 0.5); }

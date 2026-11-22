#version 450
// Synthetic golden frame, GOLDEN_SYNTH_B10 (golden_synth.c): a colour R11G11B10F holds exactly (so no rounding rule can tell the formats apart) with alpha 0.25,
//   drawn into an R11G11B10F target, which has no alpha to keep
layout(location = 0) in vec3 vcol;
layout(location = 0) out vec4 o;
void main() { o = vec4(0.75, 0.375, 0.5, 0.25); }

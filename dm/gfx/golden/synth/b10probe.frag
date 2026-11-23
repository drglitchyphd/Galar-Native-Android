#version 450
// Synthetic golden frame, GOLDEN_SYNTH_B10 (golden_synth.c): what a later pass reads from an R11G11B10F texture, texel for texel, written as it is into an RGBA32F
//   target the CPU reads back (a CPU read of the texture itself packs, which would hide a negative or out-of-range value the stand-in kept)
layout(location = 0) in vec3 vcol;
layout(set = 0, binding = 0) uniform sampler2D fp_t0;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(fp_t0, ivec2(gl_FragCoord.xy), 0); }

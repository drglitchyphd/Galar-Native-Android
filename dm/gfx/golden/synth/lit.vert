#version 450
// Synthetic golden frame: textured, lit geometry. Resource names follow the translator's (vp_c1: the vertex stage's uniform block in slot 1); golden_synth.c writes the .vkr
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec2 uv;
layout(set = 0, binding = 0, std140) uniform VB { mat4 mvp; mat4 model; vec4 tint; } vp_c1;
layout(location = 0) out vec3 vn;
layout(location = 1) out vec2 vuv;
layout(location = 2) out vec4 vtint;
void main() { gl_Position = vp_c1.mvp * vec4(pos, 1.0); vn = mat3(vp_c1.model) * nrm; vuv = uv; vtint = vp_c1.tint; }

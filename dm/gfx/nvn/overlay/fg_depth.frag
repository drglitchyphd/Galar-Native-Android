#version 450
// Frame generation, depth guidance: the game's scene depth (a copy made before its HUD pass, nvn_vk.c) as nearness, 1 at the near plane down to 0 at the far one, whichever
// way round the game keeps its depth (p.a.x = 1: the far plane is depth 1; 0: reversed, far is 0). Sampled nearest (a depth format need not filter)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D zs;
layout(push_constant) uniform P { vec4 a; } p;
void main() {
    float d = textureLod(zs, uv, 0.0).r;
    o = vec4(p.a.x > 0.5 ? 1.0 - d : d);
}

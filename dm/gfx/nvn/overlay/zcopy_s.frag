#version 450
// Stencil of a texture copy, one bit a draw (nvn_vk.c zcopy_draw; Vulkan 1.0 has no stencil export): the draw writes its bit (write mask) where the source's is set
layout(set = 0, binding = 1) uniform usampler2D s;
layout(push_constant) uniform P { vec4 m; ivec4 lim; } p;   // lim.z: the bit
void main() {
    ivec2 t = clamp(ivec2(floor(gl_FragCoord.xy * p.m.xy + p.m.zw)), ivec2(0), p.lim.xy);
    if (((texelFetch(s, t, 0).r >> uint(p.lim.z)) & 1u) == 0u) discard;
}

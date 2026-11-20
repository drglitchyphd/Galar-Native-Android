#version 450
// Depth of a texture copy (nvn_vk.c zcopy_draw): the source texel under this pixel, nearest, as a blit of depth takes it.
//   m: source texel = pixel * m.xy + m.zw (mirroring: a negative scale); lim: the source level's size - 1
layout(set = 0, binding = 0) uniform sampler2D z;
layout(push_constant) uniform P { vec4 m; ivec4 lim; } p;
void main() {
    ivec2 t = clamp(ivec2(floor(gl_FragCoord.xy * p.m.xy + p.m.zw)), ivec2(0), p.lim.xy);
    gl_FragDepth = texelFetch(z, t, 0).r;
}

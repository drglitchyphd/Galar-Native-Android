#version 450
// After a camera cut (fg_cut.comp's flag), for a picture that is a render target (our own frame generation, nvn_fgen.h): drawn over the picture, the newer real frame,
//   through views in the frame's own format on both sides (sRGB decoded and encoded again: exact). Without a cut every fragment is discarded and the picture stays
layout(set = 0, binding = 0) readonly buffer R { uint flag, changed, cells, sum; } r;
layout(set = 0, binding = 1) uniform sampler2D real;   // the newer real frame, the picture's size
layout(location = 0) out vec4 o;
void main() {
    if (r.flag == 0u) discard;
    o = texelFetch(real, ivec2(gl_FragCoord.xy), 0);
}

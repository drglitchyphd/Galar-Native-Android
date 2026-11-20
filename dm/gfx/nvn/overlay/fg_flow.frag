#version 450
// Frame generation, the motion between two frames at one pyramid level, seen from the picture halfway between them: for each texel the vector v for which the
// previous frame at -v/2 and the current frame at +v/2 look the same (a 3x3 window of taps 1.5 texels apart, sum of absolute brightness differences). The search
// starts from the coarser level's vector (doubled) and tries every whole step within the radius; a parabola through the best step's neighbours gives the fraction.
// A vector found from the middle needs no hole filling: every pixel of the in-between picture has one. Out: xy = v in texels of this level, z = the mean
// difference left at v, w = the mean difference of the window's taps from its centre (how much there is to match)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D ya;   // the previous frame's brightness
layout(set = 0, binding = 1) uniform sampler2D yb;   // the current frame's
layout(set = 0, binding = 2) uniform sampler2D fc;   // the coarser level's vectors
layout(set = 0, binding = 3) uniform sampler2D zn;   // the current frame's nearness (fg_depth.frag), when the camera guides the search
// a: 1 / width, 1 / height, search radius, 1 when there is a coarser level. b: x 1 = camera guidance on; y 1 = the game's depth is 0..1 (near = 0), 0 = GL's -1..1;
//   z 1 = nearness is 1 - depth (the far plane at depth 1). m: the game's view-projection of the frame before times the inverse of the current frame's
layout(push_constant) uniform P { vec4 a; vec4 b; mat4 m; } p;
float cost(vec2 v) {
    vec2 h = 0.5 * v * p.a.xy; float s = 0.0;
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
        vec2 q = uv + vec2(float(i), float(j)) * 1.5 * p.a.xy;
        s += abs(textureLod(ya, q - h, 0.0).r - textureLod(yb, q + h, 0.0).r); }
    return s;
}
// The camera's own motion at this texel (in texels of this level, current minus previous): the current frame's point at its depth taken back through the
//   matrices. Which way the picture's rows run in clip space is not known here, so ys tries both; a wrong guess only loses to the image search below
vec2 camv(float ys) {
    float n = textureLod(zn, uv, 0.0).r, d = p.b.z > 0.5 ? 1.0 - n : n, z = p.b.y > 0.5 ? d : 2.0 * d - 1.0;
    vec4 q = p.m * vec4(2.0 * uv.x - 1.0, ys * (2.0 * uv.y - 1.0), z, 1.0);
    if (q.w <= 1e-6) return vec2(0.0);
    vec2 c = q.xy / q.w, u = vec2(0.5 * c.x + 0.5, 0.5 * ys * c.y + 0.5);
    return (uv - u) / p.a.xy;
}
void main() {
    vec2 v0 = p.a.w > 0.5 ? 2.0 * textureLod(fc, uv, 0.0).xy : vec2(0.0);
    if (p.b.x > 0.5) {   // the search starts from whichever matches best: the coarser level's vector, or the camera's motion (fast turns, beyond the search's reach)
        float c0 = cost(v0); vec2 s0 = camv(1.0), s1 = camv(-1.0); float k0 = cost(s0), k1 = cost(s1);
        float c1 = 0.5 * c0 - 0.01;   // (only a clearly better match: where the pictures cannot tell, as in flat areas, the image search keeps its own)
        if (k0 < c1 && k0 <= k1) v0 = s0; else if (k1 < c1) v0 = s1; }
    int r = int(p.a.z + 0.5);
    float best = 1e9, bc = 0.0; vec2 bd = vec2(0.0);
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        vec2 d = vec2(float(i), float(j)); float c = cost(v0 + d), k = c + 0.01 * length(d);   // (among equals the step nearest the coarser level's vector)
        if (k < best) { best = k; bc = c; bd = d; } }
    vec2 v = v0 + bd;
    float xm = cost(v - vec2(1.0, 0.0)), xp = cost(v + vec2(1.0, 0.0)), ym = cost(v - vec2(0.0, 1.0)), yp = cost(v + vec2(0.0, 1.0));
    float dx = xm - 2.0 * bc + xp, dy = ym - 2.0 * bc + yp;
    vec2 f = vec2(dx > 1e-4 ? clamp(0.5 * (xm - xp) / dx, -0.5, 0.5) : 0.0, dy > 1e-4 ? clamp(0.5 * (ym - yp) / dy, -0.5, 0.5) : 0.0);
    v += f;
    float c0 = cost(vec2(0.0));
    if (c0 < bc * 1.1 + 0.004) { v = vec2(0.0); bc = min(bc, c0); }   // what does not move stays exactly where it is (the HUD, a still camera)
    vec2 h = 0.5 * v * p.a.xy; float ca = textureLod(ya, uv - h, 0.0).r, cb = textureLod(yb, uv + h, 0.0).r, ct = 0.0;   // the window's own contrast, both frames
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
        vec2 q = uv + vec2(float(i), float(j)) * 1.5 * p.a.xy;
        ct += abs(textureLod(ya, q - h, 0.0).r - ca) + abs(textureLod(yb, q + h, 0.0).r - cb); }
    o = vec4(v, bc / 9.0, ct / 16.0);
}

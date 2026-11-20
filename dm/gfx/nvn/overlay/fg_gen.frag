#version 450
// Frame generation, the in-between picture: the previous frame taken half the motion back and the current frame half the motion forward, averaged. Where the two
// disagree (something uncovered, a match that failed) the plain average of the two frames stands in, which ghosts for one sixtieth of a second instead of tearing;
// a pixel that is the same in both frames is left alone (HUD, text, a still camera); when the frames are different pictures (fg_stat.frag) the previous one is shown.
// With the game's depth (p.b.x = 1): where they disagree, the vectors around are tried (at an object's edge its own vector is often the neighbour's), and when none
// matches the pixel is seen in one frame only, uncovered or about to be covered: the background's vector (the farthest of those around) gives the two places it
// comes from, and the farther of the two is what lies between (the background, not a ghost of the object in front). Around a character the camera turns past
// that is the halo the plain average leaves
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D a;    // the previous frame
layout(set = 0, binding = 1) uniform sampler2D b;    // the current frame
layout(set = 0, binding = 2) uniform sampler2D fl;   // the finest level's vectors
layout(set = 0, binding = 3) uniform sampler2D st;   // fg_stat.frag's texel
layout(set = 0, binding = 4) uniform sampler2D hud;  // HUD-free (p.a.w = 1): the current frame as shown, its HUD on; a, b are the frames before the HUD was drawn
layout(set = 0, binding = 5) uniform sampler2D za;   // depth guidance (p.b.x = 1): the previous frame's nearness (fg_depth.frag: 1 near .. 0 far)
layout(set = 0, binding = 6) uniform sampler2D zb;   // the current frame's
layout(push_constant) uniform P { vec4 a; vec4 b; } p;   // a: 1 / width, 1 / height of the vectors' level; z: 1 = show the vectors (DM_FRAMEGEN=debug); w: 1 = HUD-free. b.x: 1 = depth
float max3(vec3 v) { return max(v.x, max(v.y, v.z)); }
const vec2 D[8] = vec2[](vec2(1.0, 0.0), vec2(0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071), vec2(-1.0, 0.0), vec2(-0.7071, -0.7071), vec2(0.0, -1.0), vec2(0.7071, -0.7071));
void main() {
    vec4 f = textureLod(fl, uv, 0.0);
    vec2 h = 0.5 * f.xy * p.a.xy, ua = uv - h, ub = uv + h;
    vec3 a0 = textureLod(a, uv, 0.0).rgb, b0 = textureLod(b, uv, 0.0).rgb, aw = textureLod(a, ua, 0.0).rgb, bw = textureLod(b, ub, 0.0).rgb;
    bool ina = all(greaterThanEqual(ua, vec2(0.0))) && all(lessThanEqual(ua, vec2(1.0))), inb = all(greaterThanEqual(ub, vec2(0.0))) && all(lessThanEqual(ub, vec2(1.0)));
    vec3 mw = 0.5 * (aw + bw), fb = 0.5 * (a0 + b0); float mis = smoothstep(0.10, 0.30, max3(abs(aw - bw)));
    if (p.b.x > 0.5 && ina && inb && mis > 0.0) {
        float bd = max3(abs(aw - bw)), gn = 1e9; vec2 gv = f.xy; vec3 bm = mw;
        for (int r = 0; r < 3; r++) for (int i = 0; i < 8; i++) {   // rings 3, 8 and 16 texels of the vectors' level out (12 to 64 pixels: halos up to half of 128 pixels of motion)
            vec2 q = uv + D[i] * (r == 0 ? 3.0 : r == 1 ? 8.0 : 16.0) * p.a.xy, c = textureLod(fl, q, 0.0).xy, hc = 0.5 * c * p.a.xy;
            vec3 ac = textureLod(a, uv - hc, 0.0).rgb, bc = textureLod(b, uv + hc, 0.0).rgb; float dc = max3(abs(ac - bc));
            if (dc < bd) { bd = dc; bm = 0.5 * (ac + bc); }
            float n = textureLod(za, q - hc, 0.0).r + textureLod(zb, q + hc, 0.0).r;   // how near the surface that vector belongs to is
            if (n < gn) { gn = n; gv = c; } }
        if (bd < 0.06) { mw = bm; mis = 0.0; }   // a vector from around matches: the pixel moves with its neighbours
        else { vec2 hg = 0.5 * gv * p.a.xy, pa = uv - hg, pb = uv + hg;
            vec3 ag = textureLod(a, pa, 0.0).rgb, bg = textureLod(b, pb, 0.0).rgb; float na = textureLod(za, pa, 0.0).r, nb = textureLod(zb, pb, 0.0).r;
            fb = na < 0.9 * nb ? ag : nb < 0.9 * na ? bg : 0.5 * (ag + bg); }   // seen in one frame only: the farther of the two
    }
    vec3 m = ina && inb ? mix(mw, fb, mis) : inb ? bw : ina ? aw : 0.5 * (a0 + b0);   // (off the edge on one side: the other frame alone)
    m = mix(m, b0, 1.0 - smoothstep(0.004, 0.016, max3(abs(a0 - b0))));
    m = mix(m, a0, smoothstep(0.70, 0.95, textureLod(st, vec2(0.5), 0.0).r));
    // HUD-free: the motion came from frames without the HUD; the HUD goes back on top where the shown frame differs from its own HUD-less copy (soft at its edges)
    if (p.a.w > 0.5) { vec3 hv = textureLod(hud, uv, 0.0).rgb; m = mix(m, hv, smoothstep(0.004, 0.02, max3(abs(hv - b0)))); }
    if (p.a.z > 0.5) m = mix(m, vec3(0.5) + vec3(f.xy * 0.1, -f.x * 0.1), 0.6);
    o = vec4(m, 1.0);
}

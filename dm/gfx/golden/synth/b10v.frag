#version 450
// Synthetic golden frame, GOLDEN_SYNTH_B10 (golden_synth.c b10_vbits, the same function in C): each pixel of a 64 x 64 R11G11B10F target gets three float bit
//   patterns of its own. Row 0: values every rounding rule stores alike (negative, -0, -inf, NaN, +inf, past the largest finite value, exact ones); the other rows:
//   any sign and exponent around the packed range, which only the stand-in's rule (round to nearest even) is checked against
layout(location = 0) in vec3 vcol;
layout(location = 0) out vec4 o;
const uint SP[24] = uint[](0xbf800000u, 0x4788b800u, 0x7fc00000u, 0x7f800000u, 0xff800000u, 0x80000000u, 0x477e0000u, 0x477c0000u, 0x477e4c00u, 0x477fff00u,
                           0x501502f9u, 0x3f000000u, 0x3f800000u, 0x38800000u, 0x40400000u, 0x3f400000u, 0x3fc00000u, 0x42c80000u, 0x44800000u, 0x00000000u,
                           0xc2c80000u, 0x7fffffffu, 0xffc00000u, 0x46000000u);
uint vbits(uint i, uint c) {
    if (i < 64u) return SP[(i + c * 7u) % 24u];
    uint h = (i * 0x9E3779B1u) ^ (c * 0x85EBCA77u); h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    uint e = 100u + (h >> 8) % 47u;
    return ((h & 7u) == 0u ? 0x80000000u : 0u) | (e << 23) | ((h * 0x27d4eb2du) >> 9);
}
void main() {
    uint i = uint(gl_FragCoord.y) * 64u + uint(gl_FragCoord.x);
    o = vec4(uintBitsToFloat(vbits(i, 0u)), uintBitsToFloat(vbits(i, 1u)), uintBitsToFloat(vbits(i, 2u)), 0.25);
}

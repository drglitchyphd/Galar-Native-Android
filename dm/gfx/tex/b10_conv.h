// R11G11B10F (NVN 0x3F; Vulkan B10G11R11_UFLOAT_PACK32: R in bits 0-10, G in 11-21, B in 22-31) and RGBA16F, texel by texel, for an image whose R11G11B10F is
//   stood in for by RGBA16F (DM_VK_B10_AS_RGBA16, nvn_vk.c, the Mali float-target test #282). Each packed channel is an unsigned float with the half's exponent
//   (5 bits, bias 15) and 6 (R, G) or 5 (B) mantissa bits, so every value it holds (denormals, infinity and NaN included) is a half exactly: the expansion is exact,
//   and packing an expanded texel gives back the same bits. Packing any other half (what the GPU drew) follows Vulkan's rules for the unsigned 11 / 10-bit floats:
//   negative values and -inf become 0, NaN stays a NaN, +inf stays +inf, finite values round to the nearest (ties to even), and those past the largest finite value
//   (65024 for R / G, 64512 for B) become it. dm/gfx/tex/tests/t_b10.c checks every code of both channel kinds and every half.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

enum { B10_MR = 6, B10_MB = 5 };   // mantissa bits: R and G, B
// one packed channel (mbits mantissa bits) as a half
static inline uint16_t b10_chan_to_half(uint32_t c, int mbits) { uint32_t e = c >> mbits & 31, m = c & ((1u << mbits) - 1); return (uint16_t)(e << 10 | m << (10 - mbits)); }
// a half as one packed channel (the rules above)
static inline uint32_t b10_half_to_chan(uint16_t h, int mbits) {
    uint32_t e = (uint32_t)h >> 10 & 31, m = h & 1023u, sh = (uint32_t)(10 - mbits);
    if (e == 31) return m ? (31u << mbits | (m >> sh ? m >> sh : 1u)) : (h & 0x8000 ? 0u : 31u << mbits);   // NaN (still a NaN), +inf; -inf: 0
    if (h & 0x8000) return 0;                                                                             // negative, -0
    uint32_t v = e << 10 | m, r = v >> sh, rem = v & ((1u << sh) - 1), half = 1u << (sh - 1);           // (a carry out of the mantissa raises the exponent)
    if (rem > half || (rem == half && (r & 1))) r++;
    return r >= 31u << mbits ? (31u << mbits) - 1 : r;                                                    // past the largest finite value: that value
}
// a float as one packed channel, rounded once (clear colours): the same rules
static inline uint32_t b10_float_to_chan(float f, int mbits) {
    uint32_t u; memcpy(&u, &f, 4); uint32_t e = u >> 23 & 255, m = u & 0x7fffffu;
    if (e == 255) return m ? (31u << mbits | 1u) : (u >> 31 ? 0u : 31u << mbits);
    if ((u >> 31) || !e) return 0;   // negative, zero, float denormals (far below the packed format's smallest value, 2^-20)
    int E = (int)e - 112;            // the packed exponent (bias 15)
    uint64_t mant = (uint64_t)(m | 0x800000u); int sh = 23 - mbits + (E >= 1 ? 0 : 1 - E);
    if (sh > 40) return 0;
    uint64_t r = mant >> sh, rem = mant & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rem > half || (rem == half && (r & 1))) r++;
    uint64_t c = (E >= 1 ? (uint64_t)(E - 1) << mbits : 0) + r;   // (r holds the leading one for a normal value: E - 1 above it makes the exponent E)
    return c >= 31u << mbits ? (31u << mbits) - 1 : (uint32_t)c;
}
static inline float b10_half_to_float(uint16_t h) {
    uint32_t e = (uint32_t)h >> 10 & 31, m = h & 1023u; float v = e == 31 ? (m ? NAN : INFINITY) : e ? ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15) : ldexpf((float)m / 1024.0f, -14);
    return h & 0x8000 ? -v : v;
}
static inline float b10_chan_to_float(uint32_t c, int mbits) { return b10_half_to_float(b10_chan_to_half(c, mbits)); }
// a float as the packed format stores it (a clear colour on an R11G11B10F target)
static inline float b10_quantize(float f, int mbits) { return b10_chan_to_float(b10_float_to_chan(f, mbits), mbits); }
// one texel: packed → RGBA16F (alpha 1, as R11G11B10F reads) and back
static inline void b10_texel_expand(uint32_t v, uint16_t o[4]) { o[0] = b10_chan_to_half(v & 0x7ff, B10_MR); o[1] = b10_chan_to_half(v >> 11 & 0x7ff, B10_MR); o[2] = b10_chan_to_half(v >> 22, B10_MB); o[3] = 0x3c00; }
static inline uint32_t b10_texel_pack(const uint16_t h[4]) { return b10_half_to_chan(h[0], B10_MR) | b10_half_to_chan(h[1], B10_MR) << 11 | b10_half_to_chan(h[2], B10_MB) << 22; }
// rows of texels: rows x w packed (spitch apart) → RGBA16F (dpitch apart), and back
static inline void b10_rows_expand(const uint8_t *src, size_t spitch, uint32_t w, size_t rows, uint8_t *dst, size_t dpitch) {
    for (size_t r = 0; r < rows; r++) for (uint32_t i = 0; i < w; i++) { uint32_t v; memcpy(&v, src + r * spitch + 4 * (size_t)i, 4); uint16_t o[4]; b10_texel_expand(v, o); memcpy(dst + r * dpitch + 8 * (size_t)i, o, 8); }
}
static inline void b10_rows_pack(const uint8_t *src, size_t spitch, uint32_t w, size_t rows, uint8_t *dst, size_t dpitch) {
    for (size_t r = 0; r < rows; r++) for (uint32_t i = 0; i < w; i++) { uint16_t h[4]; memcpy(h, src + r * spitch + 8 * (size_t)i, 8); uint32_t v = b10_texel_pack(h); memcpy(dst + r * dpitch + 4 * (size_t)i, &v, 4); }
}

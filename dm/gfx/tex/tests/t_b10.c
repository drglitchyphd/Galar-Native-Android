// R11G11B10F <-> RGBA16F (b10_conv.h, the DM_VK_B10_AS_RGBA16 stand-in), exhaustively: every code of an 11-bit (R, G) and a 10-bit (B) channel, infinities, NaN
//   and denormals included, expands to the half of exactly its value and packs back to exactly its bits; every one of the 65,536 halves packs to the channel code a
//   reference picks independently (the nearest finite value, ties to even, negative and -inf to 0, NaN to a NaN, +inf kept, past the largest value to it); floats
//   round the same way (all channel values, the midpoints between them, 2 million random floats); whole texels round-trip (2 million random ones).
//   Build and run: cc -O2 -Wall -o t_b10 t_b10.c -lm && ./t_b10   (dm/gfx/tex/tests/Makefile: make t_b10)
#include "../b10_conv.h"
#include <stdio.h>
#include <stdlib.h>

static long checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 40) { printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

// the reference: a channel's value from its fields (not through the code under test), and the nearest finite code to a value
static double chan_value(uint32_t c, int mb) {
    uint32_t e = c >> mb & 31, m = c & ((1u << mb) - 1);
    if (e == 31) return m ? NAN : INFINITY;
    return e ? ldexp(1.0 + m / (double)(1u << mb), (int)e - 15) : ldexp(m / (double)(1u << mb), -14);
}
static double half_value(uint16_t h) {
    uint32_t e = h >> 10 & 31, m = h & 1023; double v = e == 31 ? (m ? NAN : INFINITY) : e ? ldexp(1.0 + m / 1024.0, (int)e - 15) : ldexp(m / 1024.0, -14);
    return h & 0x8000 ? -v : v;
}
static int is_nan_code(uint32_t c, int mb) { return (c >> mb & 31) == 31 && (c & ((1u << mb) - 1)); }
static uint32_t ref_code(double v, int signbit, int mb) {
    uint32_t inf = 31u << mb, nfin = inf;   // codes 0 .. inf - 1 are the finite values, increasing
    if (isnan(v)) return inf | 1;            // (a NaN: compared as "is a NaN")
    if (signbit || v <= 0) return 0;
    if (isinf(v)) return inf;
    uint32_t lo = 0, hi = nfin - 1;
    if (v >= chan_value(hi, mb)) return hi;
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (chan_value(mid, mb) <= v) lo = mid; else hi = mid; }
    double dl = v - chan_value(lo, mb), dh = chan_value(hi, mb) - v;
    return dl < dh ? lo : dh < dl ? hi : (lo & 1 ? hi : lo);
}
static int same_code(uint32_t got, uint32_t want, int mb) { return is_nan_code(want, mb) ? is_nan_code(got, mb) : got == want; }

static void channel(int mb) {
    const char *nm = mb == B10_MR ? "11-bit (R, G)" : "10-bit (B)"; uint32_t n = 1u << (5 + mb); long f0 = fails;
    for (uint32_t c = 0; c < n; c++) {   // every code: expanded exactly, packed back to the same bits, the float route agreeing
        uint16_t h = b10_chan_to_half(c, mb); double want = chan_value(c, mb), got = half_value(h);
        CHECK(isnan(want) ? isnan(got) : got == want, "%s code %#x: half %#06x is %.9g, the code is %.9g", nm, c, h, got, want);
        CHECK(!(h & 0x8000), "%s code %#x: half %#06x is negative", nm, c, h);
        CHECK(b10_half_to_chan(h, mb) == c, "%s code %#x: half %#06x packs to %#x", nm, c, h, b10_half_to_chan(h, mb));
        if (!is_nan_code(c, mb)) { float fv = (float)want; CHECK(b10_float_to_chan(fv, mb) == c, "%s code %#x: float %.9g packs to %#x", nm, c, (double)fv, b10_float_to_chan(fv, mb));
                                   CHECK(b10_quantize(fv, mb) == fv, "%s code %#x: %.9g quantizes to %.9g", nm, c, (double)fv, (double)b10_quantize(fv, mb)); }
        else CHECK(is_nan_code(b10_float_to_chan(NAN, mb), mb), "%s: NaN float packs to %#x", nm, b10_float_to_chan(NAN, mb));
    }
    for (uint32_t h = 0; h < 65536; h++) {   // every half: the reference's code
        double v = half_value((uint16_t)h); uint32_t got = b10_half_to_chan((uint16_t)h, mb), want = ref_code(v, h >> 15, mb);
        CHECK(same_code(got, want, mb), "%s: half %#06x (%.9g) packs to %#x, the reference to %#x", nm, h, v, got, want);
        uint32_t viaf = b10_float_to_chan(b10_half_to_float((uint16_t)h), mb);
        CHECK(same_code(viaf, want, mb), "%s: half %#06x as a float (%.9g) packs to %#x, the reference to %#x", nm, h, v, viaf, want);
    }
    for (uint32_t c = 0; c + 1 < (31u << mb); c++) {   // the midpoint between two neighbours: to the even one
        double mid = (chan_value(c, mb) + chan_value(c + 1, mb)) / 2; float fm = (float)mid; if ((double)fm != mid) continue;
        uint32_t want = c & 1 ? c + 1 : c; CHECK(b10_float_to_chan(fm, mb) == want, "%s: midpoint %.9g between %#x and %#x packs to %#x", nm, mid, c, c + 1, b10_float_to_chan(fm, mb));
    }
    for (int i = 0; i < 2000000; i++) {   // random floats, every exponent
        uint32_t u = rnd32(); float f; memcpy(&f, &u, 4); uint32_t got = b10_float_to_chan(f, mb), want = ref_code((double)f, u >> 31, mb);
        CHECK(same_code(got, want, mb), "%s: float %#010x (%.9g) packs to %#x, the reference to %#x", nm, u, (double)f, got, want);
    }
    // the edges by name
    CHECK(b10_half_to_chan(0x7c00, mb) == 31u << mb, "%s: +inf", nm);
    CHECK(b10_half_to_chan(0xfc00, mb) == 0, "%s: -inf to 0", nm);
    CHECK(b10_half_to_chan(0x8000, mb) == 0, "%s: -0 to 0", nm);
    CHECK(is_nan_code(b10_half_to_chan(0x7c01, mb), mb) && is_nan_code(b10_half_to_chan(0xfe00, mb), mb), "%s: NaN (low payload, negative) stays a NaN", nm);
    CHECK(b10_half_to_chan(0x7bff, mb) == (31u << mb) - 1, "%s: 65504 to the largest finite value", nm);
    CHECK(b10_float_to_chan(1e30f, mb) == (31u << mb) - 1 && b10_float_to_chan(-1e30f, mb) == 0, "%s: huge floats", nm);
    printf("%s channel: %u codes, 65536 halves, 2,000,000 floats: %s\n", nm, n, fails == f0 ? "exact" : "FAILED");
}

int main(void) {
    channel(B10_MR); channel(B10_MB);
    long f0 = fails; uint8_t pk[64], ex[128], back[64];
    for (int i = 0; i < 2000000 / 16; i++) {   // whole texels, rows of 16: expand, pack, the same bits; alpha 1.0
        for (int k = 0; k < 16; k++) { uint32_t v = rnd32(); memcpy(pk + 4 * k, &v, 4); }
        b10_rows_expand(pk, 64, 16, 1, ex, 128); b10_rows_pack(ex, 128, 16, 1, back, 64);
        CHECK(!memcmp(pk, back, 64), "texel row %d: the round trip changed the bits", i);
        for (int k = 0; k < 16; k++) { uint16_t a; memcpy(&a, ex + 8 * k + 6, 2); CHECK(a == 0x3c00, "texel row %d: alpha %#06x", i, a); }
    }
    { uint8_t two[2][8], o2[2][16];   // pitches: rows apart by more than their texels
      uint32_t a = 0x3ff | 0x7ffu << 11 | 0x3e0u << 22, b = 0xffffffffu; memcpy(two[0], &a, 4); memcpy(two[1], &b, 4);
      b10_rows_expand(&two[0][0], 8, 1, 2, &o2[0][0], 16); uint8_t r[2][8]; memset(r, 0xee, sizeof r); b10_rows_pack(&o2[0][0], 16, 1, 2, &r[0][0], 8);
      CHECK(!memcmp(r[0], two[0], 4) && !memcmp(r[1], two[1], 4) && r[0][4] == 0xee, "rows with pitches"); }
    printf("texels: 2,000,000 random round trips: %s\n", fails == f0 ? "exact" : "FAILED");
    printf("%s t_b10: %ld of %ld checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
    return fails ? 1 : 0;
}

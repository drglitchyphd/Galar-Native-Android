// PC test for Scarlet / Violet's frame-rate patch (dm/host/paldea_fps.h, applied by loader.c paldea_fps_patch) against the real 4.0.0 mains:
//   - at 30 (and Auto / nothing) nothing is written: the image stays byte for byte the game's;
//   - at 60 every site's original word is checked first, in both mains, and only then written; the step word holds the game's 33333334;
//   - a main that differs at any one site is left whole (nothing written).
// Build and run (any C compiler; the mains are the decompressed images, offset 0 = the module's base; the NSO gives the code area's end):
//   cc -O1 -Wall -Wextra -o /tmp/t_paldea_fps dm/tests/test_paldea_fps.c && /tmp/t_paldea_fps <scarlet main.bin> <scarlet main (NSO)> <violet main.bin> <violet main (NSO)>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../host/paldea_fps.h"

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long z = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)z); if (!b || fread(b, 1, (size_t)z, f) != (size_t)z) { fprintf(stderr, "%s: read failed\n", p); exit(2); }
    fclose(f); *n = (size_t)z; return b;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// what loader.c does, without the module: the setting, the build, the check, then the write
static int apply(uint8_t *img, size_t n, uint64_t text_end, const char *build, const char *fps) {
    if (!pfps_build(build) || !pfps_wanted(fps)) return 0;
    if (pfps_check(img, n, text_end) >= 0) return 0;
    pfps_write_code(img); return PFPS_NCODE;
}
static void one(const char *name, const char *build, const char *bin, const char *nso) {
    size_t n, nn; uint8_t *img = slurp(bin, &n), *hdr = slurp(nso, &nn);
    uint32_t tsz; memcpy(&tsz, hdr + 0x18, 4); uint64_t text_end = tsz;   // (NSO header: the text segment's size, the segment at memory offset 0)
    CHECK(!memcmp(hdr, "NSO0", 4), "%s: %s is not an NSO", name, nso);
    uint8_t *copy = malloc(n); memcpy(copy, img, n);
    CHECK(pfps_build(build), "%s: build %s not known", name, build);
    CHECK(pfps_check(img, n, text_end) == -1, "%s: a site does not hold the game's own word (site %d)", name, pfps_check(img, n, text_end));
    const char *no[] = { NULL, "", "30", "0", "29" };
    for (unsigned i = 0; i < sizeof no / sizeof *no; i++) {
        int k = apply(img, n, text_end, build, no[i]);
        CHECK(k == 0 && !memcmp(img, copy, n), "%s: DM_FPS_TARGET=%s wrote something", name, no[i] ? no[i] : "(unset)");
    }
    for (int i = 0; i < PFPS_NCODE; i++) {   // a main that differs at one site: nothing at all is written
        uint8_t *w = img + PFPS_CODE[i].off; w[0] ^= 1;
        CHECK(pfps_check(img, n, text_end) == i && apply(img, n, text_end, build, "60") == 0, "%s: site %#x changed, yet the check passed", name, PFPS_CODE[i].off);
        w[0] ^= 1; CHECK(!memcmp(img, copy, n), "%s: site %#x changed: the patch wrote anyway", name, PFPS_CODE[i].off);
    }
    { uint8_t *w = img + PFPS_STEP; w[0] ^= 1;
      CHECK(pfps_check(img, n, text_end) == PFPS_NCODE && apply(img, n, text_end, build, "60") == 0, "%s: step word changed, yet the check passed", name);
      w[0] ^= 1; }
    CHECK(apply(img, n, text_end, "0123456789abcdef0123456789abcdef", "60") == 0 && !memcmp(img, copy, n), "%s: another build was patched", name);
    int k = apply(img, n, text_end, build, "60");
    CHECK(k == PFPS_NCODE, "%s: 60 not applied (%d)", name, k);
    size_t diff = 0; for (size_t i = 0; i < n; i++) diff += img[i] != copy[i];
    for (int i = 0; i < PFPS_NCODE; i++) { uint32_t w; memcpy(&w, img + PFPS_CODE[i].off, 4); CHECK(w == PFPS_CODE[i].ours, "%s: %s not written", name, PFPS_CODE[i].what); }
    uint64_t s; memcpy(&s, img + PFPS_STEP, 8);
    CHECK(s == PFPS_STEP_30, "%s: the step word is %llu at load, not the game's 33333334", name, (unsigned long long)s);
    CHECK(pfps_step_ns(1) == PFPS_STEP_60 && pfps_step_ns(2) == PFPS_STEP_30 && pfps_step_ns(0) == PFPS_STEP_30 && pfps_step_ns(3) == PFPS_STEP_30, "%s: step by hold", name);
    printf("%s: 30 / Auto: nothing written; 60: %d sites checked then written (%zu bytes changed), step word %llu ns at load\n", name, k, diff, (unsigned long long)s);
    free(img); free(hdr); free(copy);
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s <scarlet main.bin> <scarlet NSO> <violet main.bin> <violet NSO>\n", argv[0]); return 2; }
    one("Scarlet 4.0.0", PFPS_BUILDS[0], argv[1], argv[2]);
    one("Violet 4.0.0", PFPS_BUILDS[1], argv[3], argv[4]);
    printf(fails ? "test_paldea_fps: %d FAILED\n" : "test_paldea_fps: all passed\n", fails);
    return fails != 0;
}

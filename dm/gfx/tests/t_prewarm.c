// The prewarm's rules without a GPU (nvn/prewarm_policy.h, perf-1124 S3 / S4): the stamp that lets a later start go without the hold, the hold's setting, the cap's
//   share, and the pipeline list without its repeats (count, order, the shipped list's entries left out, a second pass changing nothing). The GPU side (the hold
//   itself, the compaction of a real list file, the cap's builds) is simdev t_vkdev's PREWARM.
//   make GAME=<id> t_prewarm && ./t_prewarm
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "prewarm_policy.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static pw_stamp base(void) {
    pw_stamp s; memset(&s, 0, sizeof s);
    s.seed = 1000012; s.rec = 400012; s.rec_sst = 0; s.cs_seed = 332; s.cs_rec = 92; s.cache = 105800000; s.pak = 230000000; s.pak_mtime = 1763900000;
    s.vendor = 0x5143; s.device = 0x44050001; s.drv_ver = 0x80000000 | 762; s.flags = 0x1234abcd;
    for (int i = 0; i < 16; i++) s.uuid[i] = (uint8_t)(i * 17);
    snprintf(s.app, sizeof s.app, "98"); snprintf(s.chain, sizeof s.chain, "bc2f6b2e5f0d");
    return s;
}
static void stamp_tests(void) {
    pw_stamp s = base(); char t[512]; pw_stamp_fmt(&s, t, sizeof t);
    CHECK(pw_stamp_match(t, &s), "a stamp does not match itself: %s", t);
    CHECK(strchr(t, '\n') == t + strlen(t) - 1, "the stamp is one line");
    // every field that says the cache may not hold the lists makes a later start hold again
    #define DIFF(field, v) do { pw_stamp d = base(); d.field = v; CHECK(!pw_stamp_match(t, &d), "a changed " #field " still matches"); } while (0)
    DIFF(seed, 1000013); DIFF(rec, 400012 + 616); DIFF(rec_sst, 12); DIFF(cs_seed, 340); DIFF(cs_rec, 100); DIFF(cache, 105800001); DIFF(pak, 1); DIFF(pak_mtime, 1);
    DIFF(vendor, 0x13b5); DIFF(device, 1); DIFF(drv_ver, 0x80000000 | 800); DIFF(flags, 0x1234abce);
    { pw_stamp d = base(); d.uuid[15] ^= 1; CHECK(!pw_stamp_match(t, &d), "another pipelineCacheUUID still matches"); }
    { pw_stamp d = base(); snprintf(d.app, sizeof d.app, "99"); CHECK(!pw_stamp_match(t, &d), "another app version still matches"); }
    { pw_stamp d = base(); snprintf(d.chain, sizeof d.chain, "91294b095153"); CHECK(!pw_stamp_match(t, &d), "another shader chain still matches"); }
    // a cache the driver refused (0 bytes taken) never matches, even against a stamp that says 0
    { pw_stamp z = base(); z.cache = 0; char tz[512]; pw_stamp_fmt(&z, tz, sizeof tz); CHECK(!pw_stamp_match(tz, &z), "an empty cache matches"); }
    CHECK(!pw_stamp_match(NULL, &s) && !pw_stamp_match("", &s), "no stamp file matches");
    { char cut[512]; snprintf(cut, sizeof cut, "%.*s", (int)strlen(t) - 5, t); CHECK(!pw_stamp_match(cut, &s), "a cut stamp matches"); }
}
static void hold_tests(void) {
    CHECK(pw_hold_parse(NULL, 600) == 600 && pw_hold_parse("", PW_HOLD_NONE) == PW_HOLD_NONE, "unset: the default");
    CHECK(pw_hold_parse("all", 0) == PW_HOLD_ALL && pw_hold_parse("none", 600) == PW_HOLD_NONE && pw_hold_parse("0", 600) == PW_HOLD_NONE, "all / none / 0");
    CHECK(pw_hold_parse("250", 0) == 250 && pw_hold_parse("-3", 600) == 600 && pw_hold_parse("12x", 600) == 600 && pw_hold_parse("99999999", 0) == 1000000, "a count, and bad values");
    CHECK(pw_keep_of_cap(0) == 0 && pw_keep_of_cap(800) == 720 && pw_keep_of_cap(1) == 0 && pw_keep_of_cap(10) == 9, "the cap's share: 90 %%");
}
typedef struct { uint8_t key[5][8]; uint8_t rest[64]; } ent;   // (the shape of a pso_rec: what the compaction compares is all of its bytes)
static ent mk(int i) { ent e; memset(&e, 0, sizeof e); e.key[0][0] = 0x70; e.key[0][3] = (uint8_t)i; e.key[1][3] = (uint8_t)(i * 7); e.rest[0] = (uint8_t)(i >> 8); e.rest[1] = (uint8_t)i; return e; }
static void dedupe_tests(void) {
    enum { N = 300 }; static ent a[N * 2]; int n = 0;
    // the list as a capped phone grew it: 300 distinct, then 200 of them again (the ones destroyed and built again), interleaved with 0..99 repeated twice more
    for (int i = 0; i < N; i++) a[n++] = mk(i);
    for (int i = 100; i < N; i++) a[n++] = mk(i);
    for (int i = 0; i < 100 && n < N * 2; i++) a[n++] = mk(i);
    int m = pw_dedupe((uint8_t *)a, n, sizeof(ent), NULL);
    CHECK(m == N, "%d entries kept of %d, want %d", m, n, N);
    int order = 1; for (int i = 0; i < m; i++) { ent e = mk(i); order &= !memcmp(&a[i], &e, sizeof e); }
    CHECK(order, "the first of each is not kept in its place");
    CHECK(pw_compact_worth(n, m) && !pw_compact_worth(1000, 950) && pw_compact_worth(1000, 899) && !pw_compact_worth(0, 0), "rewritten only past a tenth of repeats");
    int again = pw_dedupe((uint8_t *)a, m, sizeof(ent), NULL);
    CHECK(again == m, "a second pass changed the list (%d -> %d)", m, again);
    // the shipped list's entries are left out of the record (they come with the app at every start)
    nvn_hmap seed; nvn_hm_init(&seed, sizeof(ent)); for (int i = 0; i < 50; i++) { ent e = mk(i * 2); nvn_hm_put(&seed, &e, (void *)1); }
    int k = pw_dedupe((uint8_t *)a, m, sizeof(ent), &seed);
    CHECK(k == N - 50, "%d left beside the shipped list, want %d", k, N - 50);
    int none = 1; for (int i = 0; i < k; i++) none &= nvn_hm_get(&seed, &a[i]) == NULL;
    CHECK(none, "an entry of the shipped list was kept");
    CHECK(pw_dedupe((uint8_t *)a, 0, sizeof(ent), NULL) == 0, "an empty list");
    free(seed.keys); free(seed.vals);
}
int main(void) {
    stamp_tests(); hold_tests(); dedupe_tests();
    printf("%s t_prewarm: %d of %d checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
    return fails != 0;
}

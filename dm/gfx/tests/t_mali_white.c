// The Mali white-screen ladder's rules (nvn/mali_white.h): the verdict from the detector's samples (white runs, the slow rule for a partly white picture, fine only
//   after a long stretch of play, empty samples of a black target, the cap), who runs the ladder (Arm's own driver with Sword / Shield, the test switches,
//   DM_MALI_LADDER=0), the route with env.txt's own settings winning, other white-screen tests keeping a verdict out of the file, env.txt's leftovers from the older
//   menu taken out once, and the ladder's state from start to start through its file: every transition, fine kept only after two starts and re-checked after, a
//   route that closed the game (the start marker), both routes white at once, the PanVK offer made once, started over for another driver ID, driver version or app
//   version, a file that is missing, damaged or edited. Expected values are hand-written.
//   cc -O1 -Wall -I../nvn -o /tmp/t_mali_white t_mali_white.c && /tmp/t_mali_white
//   MW_FIXTURE_DIR=<dir>: also writes state files for the app's side (dm/android/tools/tests/MaliLadderTest.java reads them)
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "mali_white.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s ", ok_ ? "\xe2\x9c\x93" : "\xe2\x9c\x97"); printf(__VA_ARGS__); printf("\n"); } while (0)
enum { T = 576 };   // the detector's grid: 32 x 18 texels
static const int ARM = 9;   // VK_DRIVER_ID_ARM_PROPRIETARY

// samples (saturated texels of T, -1 ends; -2: an empty one, all near black) every `every` ms into a fresh verdict; the sample it changed at first in *at (0: never)
static int run_t(const mw_rules *r, const int *s, int every, int *at, mw_judge *out) {
    mw_judge j; memset(&j, 0, sizeof j); *at = 0; int prev = 0;
    for (int i = 0; s[i] != -1; i++) { int v = s[i] == -2 ? mw_judge_add(&j, r, 0, T, T, (int64_t)i * every) : mw_judge_add(&j, r, (uint32_t)s[i], 0, T, (int64_t)i * every);
        if (v != prev && !*at) *at = i + 1;
        prev = v; }
    if (out) *out = j;
    return j.verdict;
}
static int run(const mw_rules *r, const int *s, int *at) { return run_t(r, s, 3000, at, NULL); }
static int *rep(int *buf, int n, int v) { for (int i = 0; i < n; i++) buf[i] = v; buf[n] = -1; return buf; }   // (n samples of v)
static const char *const RULE_ENV[] = { "DM_MALI_WHITE_N", "DM_MALI_WHITE_PCT", "DM_MALI_FINE_N", "DM_MALI_FINE_PCT", "DM_MALI_FINE_SPAN_MS", "DM_MALI_FINE_STARTS", "DM_MALI_SLOW_N",
    "DM_MALI_SLOW_PCT", "DM_MALI_DARK_PCT", "DM_MALI_WHITE_CAP", "DM_MALI_WHITE_EVERY_MS", "DM_MALI_RECHECK_MS", "DM_MALI_RECHECK_STARTS", "DM_MALI_WHITE_MIN_DRAWS",
    "DM_MALI_WHITE_LIT_DRAWS", "DM_MALI_WHITE_COVER_PCT", "DM_MALI_WHITE_MIN_W", "DM_MALI_WHITE_LEVEL", "DM_MALI_WHITE_LEVEL_LOW" };
static void unset_rules(void) { for (unsigned i = 0; i < sizeof RULE_ENV / sizeof *RULE_ENV; i++) unsetenv(RULE_ENV[i]); }

static void t_judge(void) {
    unset_rules(); mw_rules r = mw_rules_default(); mw_rules_env(&r); int at; static int b[2000]; mw_judge j;
    CHECK(r.white_n == 3 && r.white_pct == 25 && r.fine_n == 20 && r.fine_pct == 2 && r.fine_span_ms == 180000 && r.fine_starts == 2 && r.slow_n == 40 && r.slow_pct == 8
          && r.dark_pct == 95 && r.cap_n == 300 && r.every_ms == 3000 && r.recheck_ms == 60000 && r.recheck_starts == 3 && r.min_draws == 200 && r.lit_draws == 100
          && r.cover_pct == 60 && r.min_w == 1280 && r.level == 1024 && r.level_low == 64,
          "default rules: white 3 in a row at 25 %%; fine 20 samples under 2 %% over 180 s, kept after 2 starts; slow rule 40 at 8 %%; 95 %% dark is empty; cap 300; every 3 s, "
          "60 s after fine, 3 re-check starts; 200 draws, 100 lit, 60 %% covered, 1280 wide; saturated at 1024, bucket 64");
    CHECK(mw_float_bits(1024.0f) == 0x44800000u && mw_float_bits(64.0f) == 0x42800000u && MW_DARK_BITS == mw_float_bits(1.0f / 1024), "the thresholds' bits (1024, 64, 1/1024)");
    // white quickly
    { int s[] = { 576, 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_WHITE && at == 3, "three white samples in a row: white at the third"); }
    { int s[] = { 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "two white samples: undecided"); }
    { int s[] = { 144, 144, 144, -1 }; CHECK(run(&r, s, &at) == MW_WHITE, "exactly 25 %% (144 of 576) counts as white"); }
    { int s[] = { 143, 143, 143, 143, 143, 143, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "just under 25 %% (143 of 576): between, not white"); }
    { int s[] = { 576, 576, 0, 576, 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_WHITE && at == 6, "a fine sample breaks the white run: white only three later (sample 6)"); }
    { int s[] = { 576, 576, 50, 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "a sample in between (8.7 %%) breaks the white run too"); }
    // fine slowly: 20 fine samples are not enough, 3 minutes of play are needed, and white after a few fine samples is still white
    CHECK(run(&r, rep(b, 5, 0), &at) == MW_UNDECIDED, "five fine samples (15 s): undecided (the old rule called this fine)");
    { int s[16]; for (int i = 0; i < 10; i++) s[i] = 0; s[10] = s[11] = s[12] = 576; s[13] = -1;
      CHECK(run(&r, s, &at) == MW_WHITE && at == 13, "fine for 10 samples, then white: WHITE (at sample 13)"); }
    { int s[16]; for (int i = 0; i < 5; i++) s[i] = 0; s[5] = s[6] = s[7] = 576; s[8] = -1;
      CHECK(run(&r, s, &at) == MW_WHITE && at == 8, "fine for 5 samples, then white: WHITE"); }
    CHECK(run(&r, rep(b, 20, 0), &at) == MW_UNDECIDED, "20 fine samples 3 s apart (57 s): not fine yet (180 s of play needed)");
    CHECK(run(&r, rep(b, 61, 0), &at) == MW_FINE && at == 61, "61 fine samples 3 s apart (180 s of play): fine at the 61st");
    CHECK(run(&r, rep(b, 60, 0), &at) == MW_UNDECIDED, "60 fine samples (177 s): not yet");
    CHECK(run(&r, rep(b, 61, 11), &at) == MW_FINE, "11 of 576 (1.9 %%) counts as fine");
    CHECK(run(&r, rep(b, 100, 12), &at) == MW_UNDECIDED, "12 of 576 (2.08 %%) is between, never fine");
    { for (int i = 0; i < 61; i++) b[i] = 0; b[61] = b[62] = b[63] = 576; b[64] = -1;
      CHECK(run_t(&r, b, 3000, &at, &j) == MW_WHITE && at == 61 && j.rule == MW_RULE_RUN, "fine at this start (61), then three white samples (the once-a-minute re-check): turns WHITE"); }
    { for (int i = 0; i < 40; i++) b[i] = 0; b[40] = 58; for (int i = 41; i < 101; i++) b[i] = 0; b[101] = -1;
      CHECK(run(&r, b, &at) == MW_UNDECIDED, "40 fine, one partly white sample (10 %%), 60 fine: the stretch starts over after it (undecided)"); }
    { for (int i = 0; i < 40; i++) b[i] = 0; b[40] = 576; for (int i = 41; i < 102; i++) b[i] = 0; b[102] = -1;
      CHECK(run(&r, b, &at) == MW_FINE && at == 102, "40 fine, one white, 61 fine: fine at the 102nd (the white one started the stretch over)"); }
    { mw_judge k; memset(&k, 0, sizeof k); int64_t t = 0; for (int i = 0; i < 10; i++, t += 3000) mw_judge_add(&k, &r, 0, 0, T, t);
      t += 600000; for (int i = 0; i < 10; i++, t += 3000) mw_judge_add(&k, &r, 0, 0, T, t);
      CHECK(k.verdict == MW_UNDECIDED && k.fine_n == 20 && k.fine_ms == 9 * 3000 + 13000 + 9 * 3000, "a 10-minute pause (a menu) counts as 13 s of play: 20 fine samples over 67 s, not fine (%lld ms)", (long long)k.fine_ms); }
    // empty samples: a black, cleared or never drawn target is no evidence
    { for (int i = 0; i < 100; i++) b[i] = -2; b[100] = -1; CHECK(run_t(&r, b, 3000, &at, &j) == MW_UNDECIDED && j.kinds[MW_K_EMPTY] == 100 && j.samples == 100, "100 all-black samples: undecided, never fine (100 empty)"); }
    { int s[] = { 576, 576, -2, -2, 576, -1 }; CHECK(run(&r, s, &at) == MW_WHITE && at == 5, "empty samples do not break a white run (white at sample 5)"); }
    { for (int i = 0; i < 30; i++) { b[2 * i] = 0; b[2 * i + 1] = -2; } for (int i = 60; i < 92; i++) b[i] = 0; b[92] = -1;
      CHECK(run(&r, b, &at) == MW_FINE, "empty samples between fine ones: the stretch goes on (fine once the fine samples span 180 s of play)"); }
    CHECK(mw_sample_kind(&r, 0, 548, T) == MW_K_EMPTY && mw_sample_kind(&r, 0, 547, T) == MW_K_FINE, "95 %% near black (548 of 576) is empty, 547 (94.97 %%) is fine");
    CHECK(mw_sample_kind(&r, 20, 556, T) == MW_K_BETWEEN && mw_sample_kind(&r, 200, 376, T) == MW_K_WHITE, "near black with 3.5 %% saturated: between, not empty; 35 %% saturated: white");
    // the slow rule: a partly white picture
    CHECK(run_t(&r, rep(b, 100, 58), 3000, &at, &j) == MW_WHITE && at == 40 && j.rule == MW_RULE_SLOW, "a steady 10 %% (58 of 576): WHITE at sample 40 by the slow rule");
    CHECK(run(&r, rep(b, 100, 47), &at) == MW_WHITE && at == 40, "8.2 %% (47 of 576): white at 40");
    CHECK(run(&r, rep(b, 200, 46), &at) == MW_UNDECIDED, "7.98 %% (46 of 576): never white by the slow rule");
    { for (int i = 0; i < 100; i++) b[i] = i % 2 ? 0 : 116; b[100] = -1; CHECK(run(&r, b, &at) == MW_WHITE && at == 40, "alternating 20 %% and fine (a 10 %% mean): white at 40"); }
    { for (int i = 0; i < 39; i++) b[i] = 58; for (int i = 39; i < 139; i++) b[i] = -2; b[139] = 58; b[140] = -1;
      CHECK(run(&r, b, &at) == MW_WHITE && at == 140, "the slow rule's window holds samples with content only: 39 at 10 %%, 100 empty, one more: white"); }
    { for (int i = 0; i < 100; i++) b[i] = 0; for (int i = 100; i < 160; i++) b[i] = 58; b[160] = -1;
      CHECK(run(&r, b, &at) == MW_WHITE, "100 fine (fine at 61), then a steady 10 %%: the window of the last 40 turns it WHITE"); }
    // the cap: no verdict after 300 samples
    { mw_judge k; memset(&k, 0, sizeof k); for (int i = 0; i < 299; i++) mw_judge_add(&k, &r, 29, 0, T, i * 3000LL);
      CHECK(!mw_judge_capped(&k, &r), "299 samples at 5 %% (between): not capped yet");
      mw_judge_add(&k, &r, 29, 0, T, 299 * 3000LL); CHECK(mw_judge_capped(&k, &r) && !k.verdict, "300 samples without a verdict: capped (the start stops checking)"); }
    { mw_judge k; memset(&k, 0, sizeof k); mw_judge_add(&k, &r, 0, 0, 0, 0); mw_judge_add(&k, &r, 0, 0, 0, 0); CHECK(k.samples == 0 && !k.verdict, "a sample of no texels is not a sample"); }
    { mw_judge k; memset(&k, 0, sizeof k); for (int i = 0; i < 3; i++) mw_judge_add(&k, &r, 300, 0, 400, i); CHECK(k.verdict == MW_WHITE && k.samples == 3, "any grid size: 300 of 400 three times is white"); }
    { mw_judge k; memset(&k, 0, sizeof k); for (int i = 0; i < 3; i++) mw_judge_add(&k, &r, 576, 0, T, i); for (int i = 0; i < 100; i++) mw_judge_add(&k, &r, 0, 0, T, 10 + i * 3000LL);
      CHECK(k.verdict == MW_WHITE && k.samples == 3, "WHITE stays: later samples are not even counted"); }
    // the numbers from the environment (tests and tuning); bad values keep the defaults
    setenv("DM_MALI_WHITE_N", "1", 1); setenv("DM_MALI_FINE_N", "2", 1); setenv("DM_MALI_WHITE_PCT", "50", 1); setenv("DM_MALI_FINE_PCT", "10", 1); setenv("DM_MALI_FINE_SPAN_MS", "0", 1);
    setenv("DM_MALI_FINE_STARTS", "1", 1); setenv("DM_MALI_SLOW_N", "5", 1); setenv("DM_MALI_SLOW_PCT", "30", 1); setenv("DM_MALI_DARK_PCT", "50", 1); setenv("DM_MALI_WHITE_CAP", "7", 1);
    setenv("DM_MALI_WHITE_EVERY_MS", "0", 1); setenv("DM_MALI_RECHECK_MS", "0", 1); setenv("DM_MALI_RECHECK_STARTS", "1", 1); setenv("DM_MALI_WHITE_MIN_DRAWS", "8", 1);
    setenv("DM_MALI_WHITE_COVER_PCT", "10", 1); setenv("DM_MALI_WHITE_MIN_W", "64", 1); setenv("DM_MALI_WHITE_LEVEL", "60000", 1); setenv("DM_MALI_WHITE_LEVEL_LOW", "16", 1);
    mw_rules e = mw_rules_default(); mw_rules_env(&e);
    CHECK(e.white_n == 1 && e.fine_n == 2 && e.white_pct == 50 && e.fine_pct == 10 && e.fine_span_ms == 0 && e.fine_starts == 1 && e.slow_n == 5 && e.slow_pct == 30 && e.dark_pct == 50
          && e.cap_n == 7 && e.every_ms == 0 && e.recheck_ms == 0 && e.recheck_starts == 1 && e.min_draws == 8 && e.lit_draws == 4 && e.cover_pct == 10 && e.min_w == 64
          && e.level == 60000 && e.level_low == 16, "rules from the environment (the lit draws: half the frame's when not set)");
    setenv("DM_MALI_WHITE_LIT_DRAWS", "3", 1); mw_rules_env(&e); CHECK(e.lit_draws == 3, "DM_MALI_WHITE_LIT_DRAWS on its own");
    { int s[] = { 300, -1 }; CHECK(run(&e, s, &at) == MW_WHITE && at == 1, "DM_MALI_WHITE_N=1, 50 %%: one sample of 300 / 576 is white"); }
    { int s[] = { 57, 57, -1 }; CHECK(run(&e, s, &at) == MW_FINE && at == 2, "DM_MALI_FINE_N=2, 10 %%, no span: two samples of 57 / 576 are fine"); }
    setenv("DM_MALI_WHITE_N", "x", 1); setenv("DM_MALI_WHITE_PCT", "0", 1); setenv("DM_MALI_FINE_N", "", 1); setenv("DM_MALI_WHITE_MIN_W", "-5", 1); setenv("DM_MALI_SLOW_N", "999", 1);
    setenv("DM_MALI_WHITE_LEVEL", "1", 1); setenv("DM_MALI_SLOW_PCT", "0", 1);
    mw_rules bd = mw_rules_default(); mw_rules_env(&bd);
    CHECK(bd.white_n == 3 && bd.white_pct == 25 && bd.fine_n == 20 && bd.min_w == 1280 && bd.slow_n == 40 && bd.level == 1024 && bd.slow_pct == 8, "bad or empty values keep the defaults (x, 0 %%, empty, -5, 999, 1)");
    unset_rules();
}

static void t_enabled(void) {
    CHECK(mw_enabled(1, 1, NULL, NULL) == 1, "Arm's own driver with Sword / Shield: on");
    CHECK(mw_enabled(1, 0, NULL, NULL) == 0, "Arm's own driver with another game (Legends: Arceus, Scarlet / Violet): off");
    CHECK(mw_enabled(0, 1, NULL, NULL) == 0, "another driver (Adreno, PanVK, Xclipse, PowerVR, lavapipe) with Sword / Shield: off");
    CHECK(mw_enabled(1, 1, "0", NULL) == 0 && mw_enabled(1, 1, "0", "1") == 0 && mw_enabled(0, 1, "0", "1") == 0, "DM_MALI_LADDER=0: off everywhere, the simulation too");
    CHECK(mw_enabled(0, 0, "1", NULL) == 2 && mw_enabled(0, 1, "1", NULL) == 2, "DM_MALI_LADDER=1: on for any GPU and game (a test)");
    CHECK(mw_enabled(0, 1, NULL, "1") == 2 && mw_enabled(0, 0, NULL, "1") == 2, "DM_MALI_WHITE_SIM=1: on (the ladder on a phone that draws right)");
    CHECK(mw_enabled(0, 1, "", "") == 0 && mw_enabled(1, 1, "", "0") == 1 && mw_enabled(0, 1, NULL, "0") == 0, "empty or 0 values: as unset");
    CHECK(mw_default_route(1, 1) == MW_ROUTE_A && mw_default_route(1, 0) == MW_ROUTE_NONE && mw_default_route(0, 1) == MW_ROUTE_NONE,
          "the route without the ladder: A on Arm's own driver with Sword / Shield only (it stays with DM_MALI_LADDER=0)");
}

static void t_route(void) {
    int a, b;
    CHECK(mw_route_apply(MW_ROUTE_A, NULL, NULL, &a, &b) == MW_ROUTE_A && a && !b, "the ladder's route A: the stand-in on, compression as usual");
    CHECK(mw_route_apply(MW_ROUTE_B, NULL, NULL, &a, &b) == MW_ROUTE_B && !a && b, "the ladder's route B: no compression, no stand-in");
    CHECK(mw_route_apply(MW_ROUTE_NONE, NULL, NULL, &a, &b) == MW_ROUTE_NONE && !a && !b, "no route (another driver): nothing changes");
    CHECK(mw_route_apply(MW_ROUTE_A, "0", NULL, &a, &b) == MW_ROUTE_NONE && !a && !b, "DM_VK_B10_AS_RGBA16=0 wins over the ladder's A");
    CHECK(mw_route_apply(MW_ROUTE_A, NULL, "1", &a, &b) == MW_ROUTE_BOTH && a && b, "DM_VK_B10_NOCOMPRESS=1 beside the ladder's A: both");
    CHECK(mw_route_apply(MW_ROUTE_B, "1", "0", &a, &b) == MW_ROUTE_A && a && !b, "both set in env.txt: env.txt's route (A) over the ladder's B");
    CHECK(mw_route_apply(MW_ROUTE_NONE, "1", NULL, &a, &b) == MW_ROUTE_A && a && !b, "DM_VK_B10_AS_RGBA16=1 on another driver: on (the menu's test)");
    CHECK(mw_route_apply(MW_ROUTE_A, "", "", &a, &b) == MW_ROUTE_A, "empty values: the route's");
}

static void t_other_tests(void) {
    const char *k[] = { "DM_VK_FLOAT_CONTROLS", "DM_VK_NAN_MINMAX", "DM_VK_CLAMP_ZERO", "DM_VK_EXACT_MATH", "DM_VK_INIT_LOCALS", "DM_VK_PROBE", "DM_VK_FMZ", "DM_SAFE_START_LEVEL" };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) unsetenv(k[i]);
    char o[200];
    CHECK(mw_other_tests(o, sizeof o) == 0 && !o[0], "no other white-screen test: none");
    setenv("DM_VK_PROBE", "f16target", 1); CHECK(mw_other_tests(o, sizeof o) == 1 && !strcmp(o, "DM_VK_PROBE=f16target"), "DM_VK_PROBE=f16target: one (%s)", o);
    setenv("DM_VK_PROBE", "", 1); CHECK(mw_other_tests(o, sizeof o) == 0, "DM_VK_PROBE empty: no probe, none");
    setenv("DM_VK_FMZ", "1", 1); setenv("DM_VK_NAN_MINMAX", "", 1);
    CHECK(mw_other_tests(o, sizeof o) == 2 && strstr(o, "DM_VK_NAN_MINMAX=") && strstr(o, "DM_VK_FMZ=1"), "DM_VK_FMZ=1 and an empty DM_VK_NAN_MINMAX (read as 0 by the engine): two (%s)", o);
    unsetenv("DM_VK_FMZ"); unsetenv("DM_VK_NAN_MINMAX");
    setenv("DM_VK_FLOAT_CONTROLS", "0", 1); CHECK(mw_other_tests(o, sizeof o) == 1, "DM_VK_FLOAT_CONTROLS=0 set in env.txt: a test");
    setenv("DM_SAFE_START_LEVEL", "5", 1); CHECK(mw_other_tests(o, sizeof o) == 0, "DM_VK_FLOAT_CONTROLS=0 at safe-start level 5: the app's own fallback, not a test");
    setenv("DM_VK_FLOAT_CONTROLS", "1", 1); CHECK(mw_other_tests(o, sizeof o) == 1, "DM_VK_FLOAT_CONTROLS=1 at safe-start level 5: a test");
    setenv("DM_SAFE_START_LEVEL", "4", 1); setenv("DM_VK_FLOAT_CONTROLS", "0", 1); CHECK(mw_other_tests(o, sizeof o) == 1, "DM_VK_FLOAT_CONTROLS=0 at safe-start level 4: a test");
    setenv("DM_VK_CLAMP_ZERO", "1", 1); setenv("DM_VK_EXACT_MATH", "0", 1); setenv("DM_VK_INIT_LOCALS", "0", 1);
    char small[16]; CHECK(mw_other_tests(small, sizeof small) == 4 && strlen(small) < sizeof small, "four of them into a small buffer: counted, the list cut short safely");
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) unsetenv(k[i]);
}

static void t_env_migrate(void) {
    char o[512]; int rm;
    CHECK(mw_env_migrate("DM_VK_B10_AS_RGBA16=1\nDM_LANG=fr\n", o, sizeof o, &rm) == 1 && rm == MW_ROUTE_A && !strcmp(o, "DM_LANG=fr\nDM_MALI_ENV=2\n"),
          "the older menu's DM_VK_B10_AS_RGBA16=1 taken out, the other lines kept, the marker added");
    CHECK(mw_env_migrate("  DM_VK_B10_NOCOMPRESS=1 \r\nDM_GRAPHICS=low\r\n", o, sizeof o, &rm) == 1 && rm == MW_ROUTE_B && !strcmp(o, "DM_GRAPHICS=low\r\nDM_MALI_ENV=2\n"),
          "with spaces and Windows line ends: taken out, the other line kept as it was");
    CHECK(mw_env_migrate("DM_VK_B10_AS_RGBA16=1\nDM_VK_B10_NOCOMPRESS=1", o, sizeof o, &rm) == 1 && rm == (MW_ROUTE_A | MW_ROUTE_B) && !strcmp(o, "DM_MALI_ENV=2\n"), "both, the last line without a line end");
    CHECK(mw_env_migrate("DM_VK_B10_AS_RGBA16=0\nDM_VK_B10_NOCOMPRESS=11\n# DM_VK_B10_AS_RGBA16=1\n", o, sizeof o, &rm) == 1 && rm == 0 && strstr(o, "DM_VK_B10_AS_RGBA16=0\n")
          && strstr(o, "DM_VK_B10_NOCOMPRESS=11\n") && strstr(o, "# DM_VK_B10_AS_RGBA16=1\n"), "an Off (0), another value and a comment are kept (the older menu wrote only 1)");
    CHECK(mw_env_migrate("DM_MALI_ENV=2\nDM_VK_B10_AS_RGBA16=1\n", o, sizeof o, &rm) == 0 && rm == 0, "the marker there: nothing changes (the new menu's On stays)");
    CHECK(mw_env_migrate("DM_VK_B10_AS_RGBA16=1\n DM_MALI_ENV=2\r\n", o, sizeof o, &rm) == 0 && rm == 0, "the marker after the line, with spaces: nothing changes");
    CHECK(mw_env_migrate("", o, sizeof o, &rm) == 1 && !strcmp(o, "DM_MALI_ENV=2\n") && mw_env_migrate(NULL, o, sizeof o, &rm) == 1, "an empty or missing env.txt: only the marker");
    { char o2[512]; mw_env_migrate("DM_VK_B10_NOCOMPRESS=1\nDM_X=1\n", o, sizeof o, &rm); CHECK(mw_env_migrate(o, o2, sizeof o2, &rm) == 0, "once: a migrated file is left alone"); }
    char tiny[12]; CHECK(mw_env_migrate("DM_LANG=fr\n", tiny, sizeof tiny, &rm) == -1 && mw_env_migrate("", o, 0, &rm) == -1, "a buffer too small: -1, nothing promised");
}

// ---- the ladder, start by start, through the file ----
static char D[256], F[300];
static mw_rules R;
static mw_plan start_c(int driver, uint32_t ver, long app, int crashed, int marker, mw_state *s, int *mk) {   // a start as mw_init makes it: read, plan, the marker found, this start's marker, written
    int have = mw_state_load(F, s); mw_plan p = mw_ladder_start(s, &R, have, driver, ver, app); int reset = p.reset, mr, k = mw_marker_check(s, &R, crashed, &mr);
    if (mk) *mk = k;
    p = mw_plan_of(s, &R); p.reset = reset;
    if (p.exhausted && s->panvk == MW_PANVK_NONE) s->panvk = MW_PANVK_OFFER;
    mw_marker_set(s, marker && p.check == MW_CHECK_FULL && (p.route == MW_ROUTE_A || p.route == MW_ROUTE_B) ? p.route : 0);
    mw_state_save(F, s, &R);
    return p;
}
static mw_plan start(int driver, uint32_t ver, long app, mw_state *s) { return start_c(driver, ver, app, 0, 0, s, NULL); }
static int verdict(int route, int v) { mw_state s; mw_state_load(F, &s); int ev = mw_ladder_verdict(&s, &R, route, v); mw_state_save(F, &s, &R); return ev; }
static void marker_clear(void) { mw_state s; mw_state_load(F, &s); mw_marker_clear(&s); mw_state_save(F, &s, &R); }
static void java_shows_offer(void) {   // MaliLadder.java: panvk=offer becomes panvk=shown once the dialog was shown (the other lines kept)
    char b[4096]; FILE *f = fopen(F, "r"); size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f);
    char *p = strstr(b, "panvk=offer"); if (p) memcpy(p, "panvk=shown", 11);
    f = fopen(F, "w"); fputs(b, f); fclose(f);
}
static const char *file(void) { static char b[4096]; FILE *f = fopen(F, "r"); if (!f) return ""; size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f); return b; }

static void t_ladder(void) {
    mw_state s; mw_plan p; R = mw_rules_default();
    unlink(F);
    p = start(ARM, 0xc401000, 96, &s);
    CHECK(p.reset && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL && !p.offer, "first start (no file): route A, checked, the file made");
    CHECK(strstr(file(), "driver=9\n") && strstr(file(), "driver_version=0xc401000\n") && strstr(file(), "app=96\n") && strstr(file(), "A=-\n") && strstr(file(), "next=A (checked)\n"),
          "the file names the driver, its version, the app and the route");
    p = start(ARM, 0xc401000, 96, &s); CHECK(!p.reset && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL, "a start without a verdict (undecided): the same again, nothing started over");
    CHECK(verdict(MW_ROUTE_A, MW_UNDECIDED) == MW_EV_NONE && !strstr(file(), "A=white"), "no verdict: nothing recorded");
    CHECK(verdict(MW_ROUTE_A, MW_WHITE) == MW_EV_TRY_B && strstr(file(), "A=white\n") && strstr(file(), "next=B (checked)\n"), "route A white: the second fix (B) at the next start");
    p = start(ARM, 0xc401000, 96, &s); CHECK(p.route == MW_ROUTE_B && p.check == MW_CHECK_FULL && !p.offer && !p.exhausted, "next start: route B, checked");
    CHECK(verdict(MW_ROUTE_B, MW_WHITE) == MW_EV_OFFER_PANVK && strstr(file(), "B=white\n") && strstr(file(), "panvk=offer\n") && strstr(file(), "next=A (both failed: unchecked)\n"),
          "route B white too: back to A, PanVK offered at the next start");
    p = start(ARM, 0xc401000, 96, &s); CHECK(p.route == MW_ROUTE_A && p.check == MW_CHECK_NONE && p.exhausted && p.offer, "next start: route A unchecked, the offer pending (the app shows it)");
    java_shows_offer();
    p = start(ARM, 0xc401000, 96, &s); CHECK(p.route == MW_ROUTE_A && !p.check && p.exhausted && !p.offer && s.panvk == MW_PANVK_SHOWN, "after the app showed it: no second offer, nothing checked");
    CHECK(verdict(MW_ROUTE_A, MW_WHITE) == MW_EV_EXHAUSTED, "a white verdict once the offer was shown: exhausted, no second offer");
    p = start(ARM, 0xc401000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL && !p.offer && s.panvk == MW_PANVK_NONE && strstr(file(), "app=97\n") && strstr(file(), "A=-\nB=-\n"),
                                             "an app update (versionCode 97): started over at route A, checked");
    // fine is slow: kept only after two starts that came out fine, then re-checked in three starts, then not checked
    CHECK(verdict(MW_ROUTE_A, MW_FINE) == MW_EV_FINE_ONCE && strstr(file(), "A=-\n") && strstr(file(), "A_fine_starts=1\n") && strstr(file(), "next=A (checked)\n"),
          "route A fine at one start: not kept yet (fine at 1 of 2 starts), checked again");
    p = start(ARM, 0xc401000, 97, &s); CHECK(p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL, "the next start: route A, checked in full again");
    CHECK(verdict(MW_ROUTE_A, MW_FINE) == MW_EV_FINE && strstr(file(), "A=fine\n") && strstr(file(), "next=A (fine: re-checked once a minute)\n"), "fine at the second start: kept");
    for (int i = 0; i < 3; i++) { p = start(ARM, 0xc401000, 97, &s); CHECK(p.route == MW_ROUTE_A && p.check == MW_CHECK_RE, "re-check start %d: route A, one sample a minute", i + 1);
        mw_state_load(F, &s); s.rechecks[MW_ROUTE_A]++; mw_state_save(F, &s, &R);   // (mw_judged counts a re-check start at its first sample)
        CHECK(verdict(MW_ROUTE_A, MW_FINE) == MW_EV_NONE, "a re-check start that comes out fine again: nothing changes"); }
    p = start(ARM, 0xc401000, 97, &s); CHECK(p.route == MW_ROUTE_A && p.check == MW_CHECK_NONE && strstr(file(), "next=A (fine: unchecked)\n"), "after three re-check starts: route A, not checked (costs nothing)");
    // a white re-check moves the ladder on
    mw_state_load(F, &s); s.rechecks[MW_ROUTE_A] = 1; mw_state_save(F, &s, &R);
    p = start(ARM, 0xc401000, 97, &s); CHECK(p.check == MW_CHECK_RE, "a route kept as fine with re-checks left: re-checked");
    CHECK(verdict(MW_ROUTE_A, MW_WHITE) == MW_EV_TRY_B && strstr(file(), "A=white\n"), "the re-check found it white: A white, route B next (as any white verdict)");
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL && strstr(file(), "driver_version=0xd801000\n"), "a driver update (r49p1 → r54p1): started over, checked");
    CHECK(verdict(MW_ROUTE_A, MW_WHITE) == MW_EV_TRY_B, "A white again");
    CHECK(verdict(MW_ROUTE_B, MW_FINE) == MW_EV_FINE_ONCE && verdict(MW_ROUTE_B, MW_FINE) == MW_EV_FINE && strstr(file(), "B=fine\n") && strstr(file(), "next=B (fine: re-checked once a minute)\n"),
          "route B fine at two starts: kept");
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.route == MW_ROUTE_B && p.check == MW_CHECK_RE, "next start: route B, re-checked");
    p = start(20, 0xd801000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL, "another driver ID (20, Mesa PanVK, forced on by a test switch): started over");
    // both routes at once (A by the ladder, B set in env.txt) and white: both count as white, the PanVK offer is reached
    CHECK(verdict(MW_ROUTE_BOTH, MW_WHITE) == MW_EV_OFFER_PANVK && strstr(file(), "A=white\nB=white\n") && strstr(file(), "panvk=offer\n"), "A+B white: both white, PanVK offered");
    p = start(20, 0xd801000, 97, &s);
    CHECK(verdict(MW_ROUTE_BOTH, MW_FINE) == MW_EV_NONE, "A+B fine: kept for neither (which one did it?)");
    // the other orders (env.txt chose the route that ran)
    mw_state t; mw_state_reset(&t, ARM, 1, 1);
    CHECK(mw_ladder_verdict(&t, &R, MW_ROUTE_B, MW_WHITE) == MW_EV_USE_A && mw_plan_of(&t, &R).route == MW_ROUTE_A && mw_plan_of(&t, &R).check == MW_CHECK_FULL, "B white with A untried (B forced in env.txt): A next, checked");
    CHECK(mw_ladder_verdict(&t, &R, MW_ROUTE_A, MW_WHITE) == MW_EV_OFFER_PANVK && t.panvk == MW_PANVK_OFFER && mw_plan_of(&t, &R).exhausted, "then A white: both white, the offer");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_B] = MW_FINE;
    CHECK(mw_ladder_verdict(&t, &R, MW_ROUTE_A, MW_WHITE) == MW_EV_USE_B && mw_plan_of(&t, &R).route == MW_ROUTE_B && mw_plan_of(&t, &R).check == MW_CHECK_RE, "A white with B known fine: B, re-checked");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_A] = MW_FINE; t.rechecks[MW_ROUTE_A] = 3;
    CHECK(mw_ladder_verdict(&t, &R, MW_ROUTE_B, MW_WHITE) == MW_EV_USE_A && mw_plan_of(&t, &R).route == MW_ROUTE_A && mw_plan_of(&t, &R).check == MW_CHECK_NONE, "B white with A known fine: A, unchecked");
    mw_state_reset(&t, ARM, 1, 1);
    CHECK(mw_ladder_verdict(&t, &R, MW_ROUTE_NONE, MW_WHITE) == MW_EV_NONE && mw_ladder_verdict(&t, &R, MW_ROUTE_NONE, MW_FINE) == MW_EV_NONE && !t.res[MW_ROUTE_A] && !t.res[MW_ROUTE_B],
          "a verdict with no route (env.txt turned the fix off): recorded for neither");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_B] = MW_WHITE;
    CHECK(mw_plan_of(&t, &R).route == MW_ROUTE_A && mw_plan_of(&t, &R).check == MW_CHECK_FULL, "B white, A untried: A, checked");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_A] = MW_WHITE; t.res[MW_ROUTE_B] = MW_WHITE; t.panvk = MW_PANVK_SHOWN;
    CHECK(mw_ladder_verdict(&t, &R, MW_ROUTE_B, MW_WHITE) == MW_EV_EXHAUSTED && t.panvk == MW_PANVK_SHOWN, "an offer already shown is not made again");
    // a damaged, edited or foreign file
    { FILE *f = fopen(F, "w"); fputs("garbage\n\x01\x02 =\nA=white\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL, "a file without the driver and app lines: started over");
    { FILE *f = fopen(F, "w"); fputs("# edited\nB=white\npanvk=shown\napp=97\nunknown=3\ndriver_version=0xd801000\nA=white\ndriver=9\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(!p.reset && p.exhausted && !p.offer && !p.check && p.route == MW_ROUTE_A, "lines in any order, unknown ones left out: both white, the offer shown");
    { FILE *f = fopen(F, "w"); fputs("driver=9\ndriver_version=0xd801000\napp=97\nA=white\nB=white\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.exhausted && p.offer && strstr(file(), "panvk=offer\n"), "both white without the panvk line (an older file): the offer made at the start");
    { FILE *f = fopen(F, "w"); fputs("driver=9\r\ndriver_version=0xd801000\r\napp=97\r\nA=white\r\nB=-\r\ngpu=Mali-G720 MC7\r\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(!p.reset && p.route == MW_ROUTE_B && !strcmp(s.gpu, "Mali-G720 MC7"), "a file edited on Windows (CR LF): read as it is (A white, B next; gpu '%s')", s.gpu);
    { FILE *f = fopen(F, "w"); fputs("driver=9\ndriver_version=0xd801000\napp=97\nA_crashes=-4\nB_fine_starts=99999\nearly_ends=x\nstarted=Q\nA=sideways\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(!p.reset && s.crashes[MW_ROUTE_A] == 0 && s.fine_starts[MW_ROUTE_B] == 1000 && s.early == 0 && s.started == 0 && !s.res[MW_ROUTE_A],
                                             "bad numbers and values in the file: clamped or left out");
    unlink(F);
    CHECK(!mw_state_save("", &s, &R) && !mw_state_save(NULL, &s, &R) && !mw_state_load(NULL, &s), "no path: nothing read or written (the ladder runs in memory)");
    { char p2[400]; snprintf(p2, sizeof p2, "%s/no/such/folder/mali_ladder.txt", D); CHECK(!mw_state_save(p2, &s, &R), "a folder that is not there: the save fails, nothing crashes"); }
    // the format round trip
    mw_state a, b2; mw_state_reset(&a, ARM, 0xb001000, 123); a.res[MW_ROUTE_A] = MW_CRASHED; a.res[MW_ROUTE_B] = MW_FINE; a.panvk = MW_PANVK_SHOWN; a.fine_starts[MW_ROUTE_B] = 2;
    a.rechecks[MW_ROUTE_B] = 1; a.crashes[MW_ROUTE_A] = 2; a.started = MW_ROUTE_B; a.early = 1; snprintf(a.gpu, sizeof a.gpu, "Mali-G615 MC6 = (test) name");
    char buf[2048]; mw_state_format(&a, &R, buf, sizeof buf); CHECK(mw_state_parse(buf, &b2) && !memcmp(&a, &b2, sizeof a), "format → parse gives the same state (every field, a name with '=' in it)");
    CHECK(strstr(buf, "next=B (fine: re-checked once a minute)\n") != NULL && strstr(buf, "gpu=Mali-G615 MC6 = (test) name\n"), "next= says what the next start does; gpu= names the GPU");
    mw_state_reset(&a, -1, 0x1000, 0); mw_state_format(&a, &R, buf, sizeof buf); CHECK(mw_state_parse(buf, &b2) && b2.driver == -1 && b2.app == 0, "no driver ID (an old driver without driver properties): -1 round trips");
    // random text: never a crash, every value in its range
    { unsigned seed = 1; static char txt[1024]; int bad = 0;
      for (int it = 0; it < 200000; it++) { int n = (int)((seed = seed * 1103515245 + 12345) >> 16) % 1023;
          for (int i = 0; i < n; i++) { seed = seed * 1103515245 + 12345; int c = (seed >> 16) & 0xff; const char *al = "driver=_version0x123app=A=B=panvk=offershownwhitefinecrashed_starts\n#-";
              txt[i] = (c & 1) ? al[c % 70] : (char)(c | 1); }
          txt[n] = 0; mw_state q; int have = mw_state_parse(txt, &q); mw_plan qp = mw_ladder_start(&q, &R, have, 9, 1, 1); int r2; mw_marker_check(&q, &R, it & 1, &r2); qp = mw_plan_of(&q, &R);
          for (int k = 1; k < 3; k++) bad += q.res[k] < 0 || q.res[k] > 3 || q.crashes[k] < 0 || q.fine_starts[k] < 0 || q.rechecks[k] < 0;
          bad += qp.route < 0 || qp.route > 2 || q.panvk < 0 || q.panvk > 2; char out[2048]; bad += mw_state_format(&q, &R, out, sizeof out) <= 0; }
      CHECK(!bad, "200,000 random files: parsed, planned and written without a value out of range (%d bad)", bad); }
}

// a route that closes the game: the start marker
static void t_marker(void) {
    mw_state s; mw_plan p; int mk; R = mw_rules_default(); unlink(F);
    p = start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); CHECK(p.route == MW_ROUTE_A && s.started == MW_ROUTE_A && strstr(file(), "started=A\n"), "a start that checks route A leaves its marker in the file");
    p = start_c(ARM, 0xc401000, 96, 1, 1, &s, &mk);
    CHECK(mk == MW_MK_CRASHED && s.res[MW_ROUTE_A] == MW_CRASHED && s.crashes[MW_ROUTE_A] == 1 && p.route == MW_ROUTE_B && p.check == MW_CHECK_FULL && s.started == MW_ROUTE_B,
          "the next start finds it after a crash: route A closed the game (A crashed, once), route B next with its own marker");
    marker_clear();   // (B played 20 s)
    CHECK(verdict(MW_ROUTE_B, MW_WHITE) == MW_EV_RETRY && strstr(file(), "next=A (once more: it closed the game once; checked)\n"), "B white: route A once more (it closed the game once, the other route is white)");
    p = start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); CHECK(mk == MW_MK_NONE && p.route == MW_ROUTE_A && p.retry && p.check == MW_CHECK_FULL && s.started == MW_ROUTE_A, "route A tried once more, checked, marked");
    p = start_c(ARM, 0xc401000, 96, 1, 1, &s, &mk);
    CHECK(mk == MW_MK_CRASHED && s.crashes[MW_ROUTE_A] == 2 && p.exhausted && p.route == MW_ROUTE_B && !p.check && s.panvk == MW_PANVK_OFFER,
          "it closed the game again: both failed, route B (white but it runs) unchecked, PanVK offered");
    // no crash flag: a start the player closed early, or the system ended (a hang): once is tolerated, twice in a row is a crash
    unlink(F); start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk);
    p = start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); CHECK(mk == MW_MK_EARLY && p.route == MW_ROUTE_A && s.early == 1 && s.started == MW_ROUTE_A, "ended early once without a crash: route A again (counted)");
    p = start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); CHECK(mk == MW_MK_CRASHED && s.res[MW_ROUTE_A] == MW_CRASHED && p.route == MW_ROUTE_B, "ended early twice in a row: route A closed the game, B next");
    unlink(F); start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); marker_clear();
    p = start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); CHECK(mk == MW_MK_NONE && s.early == 0 && s.res[MW_ROUTE_A] == MW_UNDECIDED, "an early end, then a start that played 20 s: the count starts over");
    p = start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); CHECK(mk == MW_MK_EARLY && s.early == 1, "...so one more early end is the first again");
    // both routes close the game: not the routes' fault, both checked again
    unlink(F); start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk);
    p = start_c(ARM, 0xc401000, 96, 1, 1, &s, &mk); CHECK(p.route == MW_ROUTE_B && s.res[MW_ROUTE_A] == MW_CRASHED, "A closed the game: B");
    p = start_c(ARM, 0xc401000, 96, 1, 1, &s, &mk);
    CHECK(mk == MW_MK_BOTH && !s.res[MW_ROUTE_A] && !s.res[MW_ROUTE_B] && !s.crashes[MW_ROUTE_A] && !s.crashes[MW_ROUTE_B] && p.route == MW_ROUTE_A && p.check == MW_CHECK_FULL && s.panvk == MW_PANVK_NONE,
          "B closed it too: the crashes are not the routes' (both undecided again, route A, no PanVK offer)");
    // a marker is left only by a start that checks one of the ladder's routes in full; a start that checks none clears an old one
    unlink(F); start_c(ARM, 0xc401000, 96, 0, 0, &s, &mk); CHECK(s.started == 0 && strstr(file(), "started=-\n"), "a start that does not mark (env.txt chose the route): no marker");
    start_c(ARM, 0xc401000, 96, 0, 1, &s, &mk); p = start_c(ARM, 0xc401000, 96, 1, 0, &s, &mk);
    CHECK(mk == MW_MK_CRASHED && s.started == 0, "a crash after a marked start is counted even when this start marks nothing");
    mw_state_reset(&s, ARM, 1, 1); s.started = MW_ROUTE_A; s.early = 1; mw_marker_set(&s, MW_ROUTE_B); CHECK(s.started == MW_ROUTE_B && s.early == 0, "a marker for another route: the early count starts over");
    mw_state_reset(&s, ARM, 1, 1); s.res[MW_ROUTE_A] = MW_FINE; s.started = MW_ROUTE_A; int r2; CHECK(mw_marker_check(&s, &R, 1, &r2) == MW_MK_CRASHED && s.res[MW_ROUTE_A] == MW_CRASHED && r2 == MW_ROUTE_A,
          "a marker found on a route kept as fine (a re-check start does not mark, an edited file can): counted");
    mw_state_reset(&s, ARM, 1, 1); s.res[MW_ROUTE_A] = MW_CRASHED; s.res[MW_ROUTE_B] = MW_CRASHED;
    CHECK(mw_plan_of(&s, &R).exhausted && mw_plan_of(&s, &R).route == MW_ROUTE_NONE, "both crashed (an edited file): no route at all");
    unlink(F);
}

// MW_FIXTURE_DIR: state files as the engine writes them, for the app's side (MaliLadderTest.java)
static void fixtures(const char *dir) {
    struct { const char *name, *gpu; long app; int a, b, panvk; } fx[] = {
        { "offer.txt", "Mali-G720 MC7", 96, MW_WHITE, MW_WHITE, MW_PANVK_OFFER }, { "offer-g310.txt", "Mali-G310", 96, MW_WHITE, MW_CRASHED, MW_PANVK_OFFER },
        { "offer-g1.txt", "Mali-G1-Ultra MC12", 96, MW_WHITE, MW_WHITE, MW_PANVK_OFFER }, { "offer-nogpu.txt", "", 96, MW_WHITE, MW_WHITE, MW_PANVK_OFFER },
        { "offer-g57.txt", "Mali-G57 MC2", 96, MW_WHITE, MW_WHITE, MW_PANVK_OFFER }, { "shown.txt", "Mali-G720 MC7", 96, MW_WHITE, MW_WHITE, MW_PANVK_SHOWN },
        { "none.txt", "Mali-G720 MC7", 96, MW_WHITE, MW_UNDECIDED, MW_PANVK_NONE }, { "oldapp.txt", "Mali-G720 MC7", 95, MW_WHITE, MW_WHITE, MW_PANVK_OFFER } };
    char p[600]; mw_rules r = mw_rules_default();
    for (unsigned i = 0; i < sizeof fx / sizeof *fx; i++) {
        mw_state s; mw_state_reset(&s, ARM, 0xc401000, fx[i].app); snprintf(s.gpu, sizeof s.gpu, "%s", fx[i].gpu); s.res[MW_ROUTE_A] = fx[i].a; s.res[MW_ROUTE_B] = fx[i].b;
        if (fx[i].b == MW_CRASHED) s.crashes[MW_ROUTE_B] = 2;
        s.panvk = fx[i].panvk;
        snprintf(p, sizeof p, "%s/%s", dir, fx[i].name); CHECK(mw_state_save(p, &s, &r), "fixture %s written", fx[i].name);
    }
}

int main(void) {
    snprintf(D, sizeof D, "/tmp/t_mali_white.%d", (int)getpid()); mkdir(D, 0700); snprintf(F, sizeof F, "%s/mali_ladder.txt", D);
    t_judge(); t_enabled(); t_route(); t_other_tests(); t_env_migrate(); t_ladder(); t_marker();
    const char *fd = getenv("MW_FIXTURE_DIR"); if (fd && *fd) fixtures(fd);
    unlink(F); rmdir(D);
    printf("%s t_mali_white: %d of %d checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
    return fails != 0;
}

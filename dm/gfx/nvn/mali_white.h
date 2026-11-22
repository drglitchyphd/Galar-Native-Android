// The Mali white-screen ladder's rules, free of Vulkan so a unit test runs them (dm/gfx/tests/t_mali_white.c); the detector and the hooks are nvn_vk.c's
//   (mali_white_vk.inc). Arm's own Mali driver draws Sword / Shield's lit scene white; two fixes exist: route A, R11G11B10F drawn as an RGBA16F stand-in
//   (DM_VK_B10_AS_RGBA16), and route B, R11G11B10F kept without framebuffer compression (DM_VK_B10_NOCOMPRESS). Nobody can test them on Mali, so the game
//   checks its own lit scene while playing and moves on by itself, one start at a time. It must not be fooled, so white is quick to call and fine is slow:
//  - each sample (the detector's grid of 576 texels of the lit target): saturated texels (a channel at 1024 or more, infinite or NaN; the stand-in does not clamp at
//    R11G11B10F's 65024, and Arm's driver left a finite 21250 in #110, while the lit scene's values are below 1 in every capture), the texels at 64 or more (for the log)
//    and the near-black ones. Its kind (mw_sample_kind): white (25 % or more saturated), between (2 % to 25 %), empty (next to nothing saturated and 95 % near black: a
//    cleared, black or never drawn target, no evidence either way) or fine;
//  - the verdict (mw_judge_add): WHITE after 3 white samples in a row, or when the last 40 samples with content are 8 % saturated on average (a partly white picture);
//    FINE at this start only after 20 fine samples with no white or partial one between, spread over 3 minutes of play (gaps capped: time in menus does not count).
//    A fine verdict can still turn white: after it the check goes on at one sample a minute for the rest of the start. Empty samples change nothing. A start that
//    reaches 300 samples without a verdict stops checking (a summary line) and the next start checks again;
//  - the ladder (mw_ladder_start at each start, mw_ladder_verdict on a verdict), kept per phone in one small text file (mw_state_format): route A first; A white
//    → route B from the next start; B white too → back to route A, and the app offers the PanVK driver at its next start (once). A route is kept as fine only after
//    2 starts that came out fine, and re-checked (one sample a minute) in the 3 starts after; a white re-check moves the ladder on as any white verdict does. Started
//    over when the Vulkan driver (its ID or version) or the app (its versionCode) changes. Both fixes at once (route A by the ladder, B set in env.txt) and white:
//    both count as white;
//  - a route that closes the game (mw_marker_*): a start that checks a route leaves a marker in the file until 20 s of play or its first sample read back; a start that
//    finds it treats that route as failed when the start crashed after the graphics device came up (outside the game's own code: android_main.c boot_guard), or when
//    two starts in a row on it ended that early. If the other route had failed that way too, the crashes are not the routes' and both are checked again. A route that
//    closed the game once, with the other one white, is tried once more before the ladder gives up on both.
// Switches (env.txt): DM_MALI_LADDER=0 no ladder and no check (route A stays the default on Arm's own driver with Sword / Shield), =1 both on any GPU (a test);
//   DM_MALI_WHITE_SIM=1 every sample counts as white (the ladder on a phone that draws right; it turns the ladder on too, unless DM_MALI_LADDER=0);
//   DM_VK_B10_AS_RGBA16 / DM_VK_B10_NOCOMPRESS set in env.txt always win over the ladder's route (mw_route_apply); an older version's menu wrote "1" for its only
//   choice, so a "1" found before the marker line DM_MALI_ENV=2 is taken out once (mw_env_migrate, android_main.c). Other white-screen tests set in env.txt
//   (mw_other_tests) keep the start's verdict out of the file. The rules' numbers (mw_rules_env, for tests): DM_MALI_WHITE_N (3), DM_MALI_WHITE_PCT (25),
//   DM_MALI_FINE_N (20), DM_MALI_FINE_PCT (2), DM_MALI_FINE_SPAN_MS (180000), DM_MALI_FINE_STARTS (2), DM_MALI_SLOW_N (40), DM_MALI_SLOW_PCT (8), DM_MALI_DARK_PCT (95),
//   DM_MALI_WHITE_CAP (300), DM_MALI_WHITE_EVERY_MS (3000: between samples), DM_MALI_RECHECK_MS (60000), DM_MALI_RECHECK_STARTS (3), DM_MALI_WHITE_MIN_DRAWS (200: a
//   frame drawing fewer is a load or a menu, not sampled), DM_MALI_WHITE_LIT_DRAWS (half of that: the lit target's own draws), DM_MALI_WHITE_COVER_PCT (60: the part of
//   the drawn region its draws cover), DM_MALI_WHITE_MIN_W (1280: the narrowest lit target sampled), DM_MALI_WHITE_LEVEL (1024) and DM_MALI_WHITE_LEVEL_LOW (64)
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MW_UNDECIDED = 0, MW_WHITE = 1, MW_FINE = 2, MW_CRASHED = 3 };              // a verdict, and a route's result in the state (MW_UNDECIDED: not known yet; MW_CRASHED: state only)
enum { MW_K_BETWEEN = 0, MW_K_WHITE = 1, MW_K_FINE = 2, MW_K_EMPTY = 3 };           // one sample's kind (mw_sample_kind)
enum { MW_ROUTE_NONE = 0, MW_ROUTE_A = 1, MW_ROUTE_B = 2, MW_ROUTE_BOTH = 3 };      // (bits: A the stand-in, B no compression)
enum { MW_PANVK_NONE = 0, MW_PANVK_OFFER = 1, MW_PANVK_SHOWN = 2 };                 // the PanVK offer: none, to show at the app's next start, shown (MaliLadder.java)
enum { MW_CHECK_NONE = 0, MW_CHECK_FULL = 1, MW_CHECK_RE = 2 };                     // how a start checks: not at all, every few seconds, one sample a minute
enum { MW_EV_NONE = 0, MW_EV_FINE_ONCE, MW_EV_FINE, MW_EV_TRY_B, MW_EV_USE_B, MW_EV_OFFER_PANVK, MW_EV_EXHAUSTED, MW_EV_USE_A, MW_EV_RETRY };   // what a verdict did
enum { MW_RULE_RUN = 1, MW_RULE_SLOW = 2, MW_RULE_FINE = 3 };                        // what decided a verdict
enum { MW_MK_NONE = 0, MW_MK_EARLY, MW_MK_CRASHED, MW_MK_BOTH };                    // what a start found of the marker an earlier start left (mw_marker_check)
#define MW_DARK_BITS 0x3A800000u   // 1/1024 as a float's bits: a texel with every channel below it is near black
#define MW_ENV_MARK "DM_MALI_ENV=2"   // env.txt's line once mw_env_migrate looked at it

// ---- the verdict ----
typedef struct {
    int white_n, white_pct;                          // WHITE: white_n samples in a row with white_pct % or more of their texels saturated
    int fine_n, fine_pct, fine_span_ms, fine_starts; // fine at a start: fine_n samples under fine_pct % (no white or partial one between) over fine_span_ms of play; kept after fine_starts such starts
    int slow_n, slow_pct;                            // WHITE too: the last slow_n samples with content at slow_pct % saturated on average
    int dark_pct;                                    // a sample with dark_pct % of its texels near black (and under fine_pct % saturated) is empty: not judged
    int cap_n;                                       // a start stops checking after cap_n samples without a verdict
    int every_ms, recheck_ms, recheck_starts;        // between samples; one every recheck_ms after fine, and in the recheck_starts starts after a route is kept as fine
    int min_draws, lit_draws, cover_pct, min_w;      // a frame is sampled with min_draws+ draws, lit_draws+ of them into the lit target, their cells cover_pct % of the part drawn
    int level, level_low;                            // saturated: a channel at level or more, infinite or NaN; the log's lower bucket: level_low or more
} mw_rules;
static inline mw_rules mw_rules_default(void) {
    mw_rules r = { .white_n = 3, .white_pct = 25, .fine_n = 20, .fine_pct = 2, .fine_span_ms = 180000, .fine_starts = 2, .slow_n = 40, .slow_pct = 8, .dark_pct = 95,
                   .cap_n = 300, .every_ms = 3000, .recheck_ms = 60000, .recheck_starts = 3, .min_draws = 200, .lit_draws = 100, .cover_pct = 60, .min_w = 1280,
                   .level = 1024, .level_low = 64 };
    return r;
}
static inline int mw_env_int(const char *name, int def, int lo, int hi) {   // an integer from the environment, def when unset, empty or out of range
    const char *e = getenv(name); if (!e || !*e) return def;
    char *end; long v = strtol(e, &end, 10); return *end || v < lo || v > hi ? def : (int)v;
}
static inline void mw_rules_env(mw_rules *r) {
    r->white_n = mw_env_int("DM_MALI_WHITE_N", r->white_n, 1, 1000); r->white_pct = mw_env_int("DM_MALI_WHITE_PCT", r->white_pct, 1, 100);
    r->fine_n = mw_env_int("DM_MALI_FINE_N", r->fine_n, 1, 10000); r->fine_pct = mw_env_int("DM_MALI_FINE_PCT", r->fine_pct, 0, 100);
    r->fine_span_ms = mw_env_int("DM_MALI_FINE_SPAN_MS", r->fine_span_ms, 0, 86400000); r->fine_starts = mw_env_int("DM_MALI_FINE_STARTS", r->fine_starts, 1, 100);
    r->slow_n = mw_env_int("DM_MALI_SLOW_N", r->slow_n, 1, 256); r->slow_pct = mw_env_int("DM_MALI_SLOW_PCT", r->slow_pct, 1, 100);
    r->dark_pct = mw_env_int("DM_MALI_DARK_PCT", r->dark_pct, 1, 100); r->cap_n = mw_env_int("DM_MALI_WHITE_CAP", r->cap_n, 1, 1000000);
    r->every_ms = mw_env_int("DM_MALI_WHITE_EVERY_MS", r->every_ms, 0, 3600000); r->recheck_ms = mw_env_int("DM_MALI_RECHECK_MS", r->recheck_ms, 0, 3600000);
    r->recheck_starts = mw_env_int("DM_MALI_RECHECK_STARTS", r->recheck_starts, 0, 100);
    r->min_draws = mw_env_int("DM_MALI_WHITE_MIN_DRAWS", r->min_draws, 0, 1 << 24); r->lit_draws = mw_env_int("DM_MALI_WHITE_LIT_DRAWS", r->min_draws / 2, 0, 1 << 24);
    r->cover_pct = mw_env_int("DM_MALI_WHITE_COVER_PCT", r->cover_pct, 0, 100); r->min_w = mw_env_int("DM_MALI_WHITE_MIN_W", r->min_w, 1, 16384);
    r->level = mw_env_int("DM_MALI_WHITE_LEVEL", r->level, 2, 65504); r->level_low = mw_env_int("DM_MALI_WHITE_LEVEL_LOW", r->level_low, 1, 65504);
}
static inline uint32_t mw_float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }   // (the detector's thresholds: compared as bits, positive floats order as integers)
typedef struct {
    int verdict, rule;                               // MW_UNDECIDED / MW_WHITE / MW_FINE (fine at this start: it can still turn white; white stays), what decided it (MW_RULE_*)
    int samples, kinds[4];                           // samples judged, by kind (MW_K_*)
    int white_run;                                   // white samples in a row (empty ones skipped)
    int fine_n; int64_t fine_ms, last_ms;            // the fine stretch: its samples, the play they span (each gap at most 4 sample intervals + 1 s), the last one's time
    int win_n, win_i; uint32_t win_sum; uint16_t win[256];   // the slow rule: the last slow_n samples with content, each one's saturated share in 1/10000
} mw_judge;
// one sample (sat saturated and dark near-black texels of total): MW_K_WHITE, MW_K_BETWEEN (fine_pct % to white_pct %), else MW_K_EMPTY (dark_pct % near black) or MW_K_FINE
static inline int mw_sample_kind(const mw_rules *r, uint32_t sat, uint32_t dark, uint32_t total) {
    uint64_t s = (uint64_t)sat * 100, t = total;
    if (s >= t * (uint64_t)r->white_pct) return MW_K_WHITE;
    if (s >= t * (uint64_t)r->fine_pct) return MW_K_BETWEEN;
    return (uint64_t)dark * 100 >= t * (uint64_t)r->dark_pct ? MW_K_EMPTY : MW_K_FINE;
}
static inline const char *mw_kind_name(int k) { return k == MW_K_WHITE ? "white" : k == MW_K_FINE ? "fine" : k == MW_K_EMPTY ? "empty" : "between"; }
// one sample taken at t_ms into the verdict so far (returned); a sample of no texels changes nothing, an empty one only its count; WHITE stays, FINE can turn WHITE
static inline int mw_judge_add(mw_judge *j, const mw_rules *r, uint32_t sat, uint32_t dark, uint32_t total, int64_t t_ms) {
    if (!total || j->verdict == MW_WHITE) return j->verdict;
    int k = mw_sample_kind(r, sat, dark, total);
    j->samples++; j->kinds[k]++;
    if (k == MW_K_EMPTY) return j->verdict;   // (a black, cleared or never drawn target: no evidence either way)
    int n = r->slow_n < 1 ? 1 : r->slow_n > 256 ? 256 : r->slow_n; uint16_t bp = (uint16_t)((uint64_t)sat * 10000 / total);
    if (j->win_n >= n) j->win_sum -= j->win[j->win_i]; else j->win_n++;
    j->win[j->win_i] = bp; j->win_sum += bp; j->win_i = (j->win_i + 1) % n;
    if (k == MW_K_WHITE) { j->white_run++; j->fine_n = 0; j->fine_ms = 0; }
    else if (k == MW_K_FINE) {
        if (j->fine_n) { int64_t g = t_ms - j->last_ms, cap = 4LL * r->every_ms + 1000; j->fine_ms += g < 0 ? 0 : g > cap ? cap : g; }
        j->fine_n++; j->last_ms = t_ms; j->white_run = 0; }
    else { j->white_run = 0; j->fine_n = 0; j->fine_ms = 0; }
    if (j->white_run >= r->white_n) { j->verdict = MW_WHITE; j->rule = MW_RULE_RUN; }
    else if (j->win_n >= n && j->win_sum >= (uint32_t)r->slow_pct * 100u * (uint32_t)n) { j->verdict = MW_WHITE; j->rule = MW_RULE_SLOW; }
    else if (!j->verdict && j->fine_n >= r->fine_n && j->fine_ms >= r->fine_span_ms) { j->verdict = MW_FINE; j->rule = MW_RULE_FINE; }
    return j->verdict;
}
static inline int mw_judge_capped(const mw_judge *j, const mw_rules *r) { return !j->verdict && j->samples >= r->cap_n; }   // (a start stops checking there)

// ---- who runs it, and the route ----
// 0 off; 1 on (Arm's own driver with Sword / Shield); 2 on through a test switch on any GPU (DM_MALI_LADDER=1, DM_MALI_WHITE_SIM=1). DM_MALI_LADDER=0 wins over both
static inline int mw_enabled(int arm_own, int galar, const char *ladder_env, const char *sim_env) {
    int l = ladder_env && *ladder_env ? atoi(ladder_env) : -1, s = sim_env && *sim_env ? atoi(sim_env) : 0;
    if (l == 0) return 0;
    if (l > 0 || s > 0) return 2;
    return arm_own && galar ? 1 : 0;
}
// the route without the ladder: route A on Arm's own driver with Sword / Shield (the fix as it ships), nothing anywhere else
static inline int mw_default_route(int arm_own, int galar) { return arm_own && galar ? MW_ROUTE_A : MW_ROUTE_NONE; }
// the route this start runs: the one asked for (the ladder's, or mw_default_route), each of DM_VK_B10_AS_RGBA16 / DM_VK_B10_NOCOMPRESS set in env.txt winning
//   (1 on, 0 off; unset or empty: the route's); *a_on / *b_on: the two switches as they come out; returns the routes on (MW_ROUTE_* bits)
static inline int mw_route_apply(int want, const char *env_a, const char *env_b, int *a_on, int *b_on) {
    *a_on = env_a && *env_a ? atoi(env_a) > 0 : (want & MW_ROUTE_A) != 0;
    *b_on = env_b && *env_b ? atoi(env_b) > 0 : (want & MW_ROUTE_B) != 0;
    return (*a_on ? MW_ROUTE_A : 0) | (*b_on ? MW_ROUTE_B : 0);
}
static inline const char *mw_route_name(int r) { return r == MW_ROUTE_A ? "A" : r == MW_ROUTE_B ? "B" : r == MW_ROUTE_BOTH ? "A+B" : "none"; }
// Other white-screen tests set in env.txt (the menu's Mali tests): they change the shaders' arithmetic or the lit target's format beside the route, so a verdict
//   then says nothing about the route alone. Their "NAME=value" list into out (empty: none); returns how many. The shader rules are read as on / off when present at
//   all (an empty value is 0 to the engine), the probe and FMZ only when not empty. DM_VK_FLOAT_CONTROLS=0 from the safe-start ladder (level 5, DM_SAFE_START_LEVEL)
//   is the app's own fallback, not a test
static inline int mw_other_tests(char *out, size_t n) {
    static const char *const any[] = { "DM_VK_FLOAT_CONTROLS", "DM_VK_NAN_MINMAX", "DM_VK_CLAMP_ZERO", "DM_VK_EXACT_MATH", "DM_VK_INIT_LOCALS" }, *const full[] = { "DM_VK_PROBE", "DM_VK_FMZ" };
    int k = 0; size_t o = 0; if (n) out[0] = 0;
    for (int i = 0; i < 7; i++) {
        const char *name = i < 5 ? any[i] : full[i - 5], *v = getenv(name);
        if (!v || (i >= 5 && !*v)) continue;
        if (i == 0 && !strcmp(v, "0")) { const char *sl = getenv("DM_SAFE_START_LEVEL"); if (sl && atoi(sl) >= 5) continue; }
        k++; if (o < n) { int w = snprintf(out + o, n - o, "%s%s=%s", o ? ", " : "", name, v); o = w < 0 ? o : o + (size_t)w > n ? n : o + (size_t)w; }
    }
    return k;
}
// env.txt's text: a line DM_VK_B10_AS_RGBA16=1 or DM_VK_B10_NOCOMPRESS=1 is what an older version's Mali tests menu wrote for "On" (its only other choice was no line);
//   the menu is Auto / On / Off now, and such a leftover would hold the ladder's route for good. Once per env.txt (the marker line MW_ENV_MARK, added here and by the
//   menu when it sets either), those two are taken out: Auto. Returns 1 when out holds the new text (*removed: MW_ROUTE_A / MW_ROUTE_B bits of what went), 0 when the
//   file stays as it is (the marker is there), -1 when out is too small
static inline int mw_env_migrate(const char *in, char *out, size_t n, int *removed) {
    size_t o = 0; *removed = 0; if (!n) return -1;
    for (const char *l = in ? in : ""; *l;) {
        const char *e = strchr(l, '\n'), *end = e ? e : l + strlen(l), *a = l, *b = end;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t')) b--;
        size_t len = (size_t)(b - a);
        if (len == strlen(MW_ENV_MARK) && !memcmp(a, MW_ENV_MARK, len)) { *removed = 0; return 0; }
        int drop = len == 21 && !memcmp(a, "DM_VK_B10_AS_RGBA16=1", 21) ? MW_ROUTE_A : len == 22 && !memcmp(a, "DM_VK_B10_NOCOMPRESS=1", 22) ? MW_ROUTE_B : 0;
        if (drop) *removed |= drop;
        else { size_t k = (size_t)(end - l); if (o + k + 2 > n) return -1; memcpy(out + o, l, k); o += k; out[o++] = '\n'; }
        l = e ? e + 1 : end;
    }
    size_t m = strlen(MW_ENV_MARK); if (o + m + 2 > n) return -1;
    memcpy(out + o, MW_ENV_MARK, m); o += m; out[o++] = '\n'; out[o] = 0;
    return 1;
}

// ---- the ladder's state (one file per phone) ----
typedef struct {
    int driver; uint32_t driver_version; long app; char gpu[128];   // whose state: the Vulkan driver's ID and version, the app's versionCode; the GPU's name (for the app's PanVK offer)
    int res[3], fine_starts[3], rechecks[3], crashes[3];             // per route ([MW_ROUTE_A], [MW_ROUTE_B]): its result; starts that came out fine; re-check starts since it was
                                                                     //   kept as fine; starts it closed the game at
    int started, early;                                              // the start marker: the route a start checks, until 20 s of play or its first sample; starts on it in a
                                                                     //   row that ended before that without a crash
    int panvk;
} mw_state;
static inline void mw_state_reset(mw_state *s, int driver, uint32_t driver_version, long app) { memset(s, 0, sizeof *s); s->driver = driver; s->driver_version = driver_version; s->app = app; }
static inline int mw_failed(int r) { return r == MW_WHITE || r == MW_CRASHED; }
// the route a start with this state runs, and how it checks: a route kept as fine is re-checked for recheck_starts starts, then not at all; A failed → B, checked;
//   both failed → a route that closed the game once (the other white) once more, else the exhausted state: the white route unchecked (A first), none if both crashed
typedef struct { int route, check, exhausted, retry, offer, reset; } mw_plan;
static inline mw_plan mw_plan_of(const mw_state *s, const mw_rules *r) {
    mw_plan p = { MW_ROUTE_A, MW_CHECK_FULL, 0, 0, 0, 0 }; int a = s->res[MW_ROUTE_A], b = s->res[MW_ROUTE_B];
    if (a == MW_FINE) p.check = s->rechecks[MW_ROUTE_A] < r->recheck_starts ? MW_CHECK_RE : MW_CHECK_NONE;
    else if (b == MW_FINE) { p.route = MW_ROUTE_B; p.check = s->rechecks[MW_ROUTE_B] < r->recheck_starts ? MW_CHECK_RE : MW_CHECK_NONE; }
    else if (mw_failed(a) && mw_failed(b)) {
        if (a == MW_CRASHED && b == MW_WHITE && s->crashes[MW_ROUTE_A] < 2) p.retry = 1;
        else if (b == MW_CRASHED && a == MW_WHITE && s->crashes[MW_ROUTE_B] < 2) { p.route = MW_ROUTE_B; p.retry = 1; }
        else { p.exhausted = 1; p.check = MW_CHECK_NONE; p.route = a == MW_WHITE ? MW_ROUTE_A : b == MW_WHITE ? MW_ROUTE_B : MW_ROUTE_NONE; }
    }
    else if (mw_failed(a)) p.route = MW_ROUTE_B;
    p.offer = s->panvk == MW_PANVK_OFFER;
    return p;
}
// at a start: the state read from the file (have = 0: none) is this driver's and this app's, else started over (plan.reset); the ladder exhausted and no offer made
//   yet: the offer is made now (shown by the app at its next start)
static inline mw_plan mw_ladder_start(mw_state *s, const mw_rules *r, int have, int driver, uint32_t driver_version, long app) {
    int reset = !have || s->driver != driver || s->driver_version != driver_version || s->app != app;
    if (reset) mw_state_reset(s, driver, driver_version, app);
    mw_plan p = mw_plan_of(s, r);
    if (p.exhausted && s->panvk == MW_PANVK_NONE) { s->panvk = MW_PANVK_OFFER; p.offer = 1; }
    p.reset = reset;
    return p;
}
// a verdict for the route that ran: MW_WHITE / MW_CRASHED for MW_ROUTE_A or MW_ROUTE_B (both at once and white: both white), MW_FINE (fine at this start) for A or B.
//   Its result kept; returns what follows (MW_EV_*): FINE_ONCE (fine at fewer than fine_starts starts: checked again), FINE (kept), TRY_B / USE_B / USE_A / RETRY (the
//   next start's route), OFFER_PANVK (the ladder exhausted: the app offers PanVK), EXHAUSTED (so, the offer made before), NONE (nothing changed)
static inline int mw_ladder_verdict(mw_state *s, const mw_rules *r, int route, int v) {
    if (route == MW_ROUTE_BOTH && v == MW_WHITE) { s->res[MW_ROUTE_A] = s->res[MW_ROUTE_B] = MW_WHITE; s->fine_starts[MW_ROUTE_A] = s->fine_starts[MW_ROUTE_B] = 0; }
    else if (route != MW_ROUTE_A && route != MW_ROUTE_B) return MW_EV_NONE;
    else if (v == MW_FINE) {
        if (s->res[route] == MW_FINE) return MW_EV_NONE;   // (a re-check start that came out fine again)
        if (++s->fine_starts[route] < r->fine_starts) return MW_EV_FINE_ONCE;
        s->res[route] = MW_FINE; s->rechecks[route] = 0; return MW_EV_FINE;
    }
    else if (v == MW_WHITE || v == MW_CRASHED) { s->res[route] = v; s->fine_starts[route] = 0; if (v == MW_CRASHED && s->crashes[route] < 1000) s->crashes[route]++; }
    else return MW_EV_NONE;
    mw_plan p = mw_plan_of(s, r);
    if (p.exhausted) { if (s->panvk != MW_PANVK_NONE) return MW_EV_EXHAUSTED; s->panvk = MW_PANVK_OFFER; return MW_EV_OFFER_PANVK; }
    if (p.retry) return MW_EV_RETRY;
    if (p.route == MW_ROUTE_B) return p.check == MW_CHECK_FULL ? MW_EV_TRY_B : MW_EV_USE_B;
    return MW_EV_USE_A;
}
// The start marker. At a start (after mw_ladder_start): the marker an earlier start left. No crash and the first such start in a row: counted (early), the route runs
//   again (MW_MK_EARLY). A crash after the device came up (crashed), or the second such start in a row: the route closed the game (MW_MK_CRASHED, the ladder moves);
//   when the other route had closed it before, the crashes are not the routes': both are checked again (MW_MK_BOTH). *route: the marker's route (0: none)
static inline int mw_marker_check(mw_state *s, const mw_rules *r, int crashed, int *route) {
    int rt = s->started; *route = rt == MW_ROUTE_A || rt == MW_ROUTE_B ? rt : 0;
    if (!*route) { s->started = s->early = 0; return MW_MK_NONE; }
    if (!crashed && s->early < 1) { s->early++; return MW_MK_EARLY; }   // (the marker stays: mw_marker_set keeps the count for the same route)
    s->started = s->early = 0;
    int other = rt == MW_ROUTE_A ? MW_ROUTE_B : MW_ROUTE_A;
    if (s->res[other] == MW_CRASHED) {
        s->res[MW_ROUTE_A] = s->res[MW_ROUTE_B] = MW_UNDECIDED; s->crashes[MW_ROUTE_A] = s->crashes[MW_ROUTE_B] = 0; s->fine_starts[MW_ROUTE_A] = s->fine_starts[MW_ROUTE_B] = 0;
        if (s->panvk == MW_PANVK_OFFER) s->panvk = MW_PANVK_NONE;
        return MW_MK_BOTH; }
    mw_ladder_verdict(s, r, rt, MW_CRASHED);
    return MW_MK_CRASHED;
}
// this start's marker: the route it checks (0: none, it checks none of the ladder's routes); the early-end count goes on only for the same route
static inline void mw_marker_set(mw_state *s, int route) { if (route != s->started) s->early = 0; s->started = route; }
static inline void mw_marker_clear(mw_state *s) { s->started = s->early = 0; }   // (20 s of play, or the first sample read back)

static inline const char *mw_res_name(int r) { return r == MW_WHITE ? "white" : r == MW_FINE ? "fine" : r == MW_CRASHED ? "crashed" : "-"; }
static inline int mw_res_parse(const char *v) { return !strncmp(v, "white", 5) ? MW_WHITE : !strncmp(v, "fine", 4) ? MW_FINE : !strncmp(v, "crashed", 7) ? MW_CRASHED : MW_UNDECIDED; }
// the file: key=value lines, # comments; unknown lines are left out, missing ones take their defaults. next= is for people (mw_plan_of decides)
static inline int mw_state_format(const mw_state *s, const mw_rules *r, char *out, size_t n) {
    mw_plan p = mw_plan_of(s, r);
    const char *how = p.exhausted ? " (both failed: unchecked)" : p.retry ? " (once more: it closed the game once; checked)" : p.check == MW_CHECK_RE ? " (fine: re-checked once a minute)"
                    : p.check == MW_CHECK_NONE ? " (fine: unchecked)" : " (checked)";
    return snprintf(out, n, "# Galar Native: the white-screen check on this phone's Mali driver (nvn_vk.c, mali_white.h). Started over when the driver or the app changes\n"
                    "driver=%d\ndriver_version=%#x\napp=%ld\ngpu=%s\nA=%s\nB=%s\nA_fine_starts=%d\nB_fine_starts=%d\nA_rechecks=%d\nB_rechecks=%d\nA_crashes=%d\nB_crashes=%d\n"
                    "started=%s\nearly_ends=%d\nnext=%s%s\npanvk=%s\n", s->driver, s->driver_version, s->app, s->gpu, mw_res_name(s->res[MW_ROUTE_A]), mw_res_name(s->res[MW_ROUTE_B]),
                    s->fine_starts[MW_ROUTE_A], s->fine_starts[MW_ROUTE_B], s->rechecks[MW_ROUTE_A], s->rechecks[MW_ROUTE_B], s->crashes[MW_ROUTE_A], s->crashes[MW_ROUTE_B],
                    s->started == MW_ROUTE_A ? "A" : s->started == MW_ROUTE_B ? "B" : "-", s->early, mw_route_name(p.route), how,
                    s->panvk == MW_PANVK_OFFER ? "offer" : s->panvk == MW_PANVK_SHOWN ? "shown" : "-");
}
static inline int mw_count_parse(const char *v) { long x = strtol(v, NULL, 10); return x < 0 ? 0 : x > 1000 ? 1000 : (int)x; }
static inline int mw_state_parse(const char *text, mw_state *s) {   // 1 when the text has the three key fields (driver, driver_version, app)
    mw_state_reset(s, -1, 0, -1); int have = 0;
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (*l == '#') continue;
        const char *eq = strchr(l, '='), *nl = strchr(l, '\n'); if (!eq || (nl && eq > nl) || eq == l || eq - l > 31) continue;
        char k[32], v[256]; size_t kl = (size_t)(eq - l), vl = nl ? (size_t)(nl - eq - 1) : strlen(eq + 1);
        while (vl && eq[vl] == '\r') vl--;   // (a file edited on Windows)
        if (vl > sizeof v - 1) vl = sizeof v - 1;
        memcpy(k, l, kl); k[kl] = 0; memcpy(v, eq + 1, vl); v[vl] = 0;
        if (!strcmp(k, "driver")) { s->driver = (int)strtol(v, NULL, 0); have |= 1; }
        else if (!strcmp(k, "driver_version")) { s->driver_version = (uint32_t)strtoul(v, NULL, 0); have |= 2; }
        else if (!strcmp(k, "app")) { s->app = strtol(v, NULL, 0); have |= 4; }
        else if (!strcmp(k, "gpu")) { size_t gl = vl < sizeof s->gpu - 1 ? vl : sizeof s->gpu - 1; memcpy(s->gpu, v, gl); s->gpu[gl] = 0; }   // (a longer name cut)
        else if (!strcmp(k, "A") || !strcmp(k, "B")) s->res[k[0] == 'A' ? MW_ROUTE_A : MW_ROUTE_B] = mw_res_parse(v);
        else if ((k[0] == 'A' || k[0] == 'B') && k[1] == '_') {
            int rt = k[0] == 'A' ? MW_ROUTE_A : MW_ROUTE_B;
            if (!strcmp(k + 2, "fine_starts")) s->fine_starts[rt] = mw_count_parse(v);
            else if (!strcmp(k + 2, "rechecks")) s->rechecks[rt] = mw_count_parse(v);
            else if (!strcmp(k + 2, "crashes")) s->crashes[rt] = mw_count_parse(v); }
        else if (!strcmp(k, "started")) s->started = v[0] == 'A' ? MW_ROUTE_A : v[0] == 'B' ? MW_ROUTE_B : 0;
        else if (!strcmp(k, "early_ends")) s->early = mw_count_parse(v);
        else if (!strcmp(k, "panvk")) s->panvk = !strncmp(v, "offer", 5) ? MW_PANVK_OFFER : !strncmp(v, "shown", 5) ? MW_PANVK_SHOWN : MW_PANVK_NONE;
    }
    return have == 7;
}
static inline int mw_state_load(const char *path, mw_state *s) {
    char b[4096]; FILE *f = path && *path ? fopen(path, "r") : NULL; if (!f) { mw_state_reset(s, -1, 0, -1); return 0; }
    size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f);
    return mw_state_parse(b, s);
}
static inline int mw_state_save(const char *path, const mw_state *s, const mw_rules *r) {   // written beside and renamed over (a crash mid-write leaves the old file)
    if (!path || !*path) return 0;
    char t[1200], b[2048]; snprintf(t, sizeof t, "%s.tmp", path); int n = mw_state_format(s, r, b, sizeof b); if (n < 0 || n >= (int)sizeof b) return 0;
    FILE *f = fopen(t, "w"); if (!f) return 0;
    int ok = fwrite(b, 1, (size_t)n, f) == (size_t)n; ok &= fclose(f) == 0;
    return ok && rename(t, path) == 0;
}

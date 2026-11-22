// The Mali white-screen ladder's rules, free of Vulkan so a unit test runs them (dm/gfx/tests/t_mali_white.c); the detector and the hooks are nvn_vk.c's
//   (mali_white_vk.inc). Arm's own Mali driver draws Sword / Shield's lit scene white; two fixes exist: route A, R11G11B10F drawn as an RGBA16F stand-in
//   (DM_VK_B10_AS_RGBA16), and route B, R11G11B10F kept without framebuffer compression (DM_VK_B10_NOCOMPRESS). Nobody can test them on Mali, so the game
//   checks its own lit scene while playing and moves on by itself, one start at a time. It must not be fooled, so white is quick to call and fine is slow:
//  - each sample (mw_obs: the detector's grid of 576 texels of the lit target, and 576 of the picture shown, the texture handed to the present): the lit target's
//    saturated texels (a channel at 1024 or more, or +infinity; the stand-in does not clamp at R11G11B10F's 65024, and Arm's driver left a finite 21250 in #110, while
//    the lit scene's values are below 1 in every capture), its NaN texels (counted apart: in the stand-in a NaN stays a NaN and shows black, as on PanVK, not white),
//    the texels at 64 or more (for the log), the near-black ones, and the shown picture's white texels (every colour channel at 0.99 or more). Its kind
//    (mw_obs_kind): white (25 % or more saturated), nan (25 % or more NaN: a black or broken picture there), shown (the lit target not white, but 90 % of the
//    picture shown white: a white that comes from a later pass), between (2 % to 25 % saturated, or 8 % or more NaN), empty (next to nothing saturated and 95 %
//    near black or NaN: a cleared, black or never drawn target), bright (the lit target fine, half the picture shown white or more: snow, a bright menu, a fade; no
//    evidence either way) or fine;
//  - the verdict (mw_judge_obs): WHITE after 3 white samples in a row (5 when most of them are shown ones: a fade to white lasts a few seconds), or when the last
//    40 samples with content are 8 % saturated on average (a partly white picture); BROKEN after 3 such samples in a row that are mostly nan ones, or 10 empty
//    samples in a row (a lit target near black for 30 s of play: a black picture); FINE at this start only after 20 fine samples with nothing but empty or bright
//    ones between, spread over 3 minutes of play (gaps capped: time in menus does not count). A fine verdict can still turn white or broken: after it the check goes
//    on at one sample a minute for the rest of the start. A start that reaches 300 samples without a verdict stops checking (a summary line) and is counted for its
//    route, unless most of its samples were bright; so is a start that took no sample at all in 10 minutes of frames that drew into a lit target (every frame
//    under the floors below: a summary line); the next start checks again;
//  - the ladder (mw_ladder_start at each start, mw_ladder_verdict on a verdict), kept per phone in one small text file (mw_state_format): route A first; A white or
//    broken → route B from the next start; B failed too → back to route A, and the app offers the PanVK driver at its next start (once). A route is kept as fine only
//    after 2 starts that came out fine, and re-checked (one sample a minute) in the 3 starts after; a failed re-check moves the ladder on as any failure does. A route
//    is broken too after 3 starts that reached the cap with no verdict (or took no sample), or one whose samples were mostly empty (mw_ladder_undecided). Started
//    over when the Vulkan driver (its ID or version) or the fixes' own code (MW_ROUTE_CODE: raised when route A, route B or the check change) changes, not at
//    every app update: a route found fine or white stays so (the file's app= follows the app's versionCode, for MaliLadder.java's offer). Both fixes at once
//    (route A by the ladder, B set in env.txt) and white: both count as white;
//  - a route that closes the game (mw_marker_*): a start that checks a route leaves a marker in the file until 20 s of play on the route or its first sample read
//    back, with the stage it got to: detector (making the check's own compute pass: an end there is the check's, never the route's), loading (the device up, the
//    route not drawn yet: the pipeline builds, the load, the logos and the title screen) and playing (the first frame that drew into the lit target, the check
//    running). The 20 s run from that frame, not from the start's first picture (the route's code first runs there, in the field; without one in 10 minutes the
//    marker goes). A start that finds it treats that route as failed when the start crashed after the graphics device came up at the loading or playing stage,
//    or when two starts in a row on it ended at the playing stage without a crash (closed early by the player, or ended by the system: android_main.c boot_guard's
//    crash flag lasts 30 s of play only, so a crash later on counts so); an end while loading without a crash (the player leaving during the pipeline builds,
//    Android ending it in the background), and a crash in the game's own code at any stage (boot_guard: DM_LAST_CRASH_GAME), count for nothing. If the other
//    route had failed that way too, the crashes are not the
//    routes' and both are checked again. A route that closed the game once, with the other one white or broken, is tried once more before the ladder gives up on
//    both. Given up on both, the start runs route A unless A closed the game with the crash flag (then B): a route left only on early ends is never traded for one
//    known to be white; the same when the check cannot run on this driver (mw_route_unchecked)
// Switches (env.txt): DM_MALI_LADDER=0 no ladder and no check (route A stays the default on Arm's own driver with Sword / Shield), =1 both on any GPU (a test);
//   DM_MALI_WHITE_SIM=1 every sample counts as white (the ladder on a phone that draws right; it turns the ladder on too, unless DM_MALI_LADDER=0);
//   DM_MALI_WHITE_INIT_ABORT=1 (a test: golden_check.py mali) ends the start with abort() while the check's compute pass is being made, as a driver that crashes there;
//   DM_VK_B10_AS_RGBA16 / DM_VK_B10_NOCOMPRESS set in env.txt always win over the ladder's route (mw_route_apply); an older version's menu wrote "1" for its only
//   choice, so a "1" found before the marker line DM_MALI_ENV=2 is taken out once (mw_env_migrate, android_main.c). Other white-screen tests set in env.txt
//   (mw_other_tests, the menu's White screen tests) keep a fine verdict, a start without a verdict and the start marker out of the file; a white or broken verdict
//   still moves the ladder (those tests only ever tried to take the white away: a white picture with them is still a fix that did not take). The rules' numbers
//   (mw_rules_env, for tests): DM_MALI_WHITE_N (3), DM_MALI_WHITE_PCT (25), DM_MALI_FINE_N (20), DM_MALI_FINE_PCT (2), DM_MALI_FINE_SPAN_MS (180000),
//   DM_MALI_FINE_STARTS (2), DM_MALI_SLOW_N (40), DM_MALI_SLOW_PCT (8), DM_MALI_DARK_PCT (95), DM_MALI_DARK_N (10), DM_MALI_NAN_PCT (8), DM_MALI_SHOWN_WHITE_PCT (90),
//   DM_MALI_SHOWN_N (5), DM_MALI_SHOWN_FINE_PCT (50), DM_MALI_WHITE_CAP (300), DM_MALI_UNDECIDED_STARTS (3), DM_MALI_WHITE_EVERY_MS (3000: between samples), DM_MALI_RECHECK_MS (60000),
//   DM_MALI_RECHECK_STARTS (3), DM_MALI_WHITE_MIN_DRAWS (120: a frame drawing fewer is a load or a menu, not sampled), DM_MALI_WHITE_LIT_DRAWS (half of that: the
//   lit target's own draws; a frame with as many is also the route's first frame for the start marker), DM_MALI_WHITE_COVER_PCT (60: the part of the drawn region
//   its draws cover), DM_MALI_WHITE_MIN_W (1280: the narrowest lit target sampled), DM_MALI_WHITE_LEVEL (1024), DM_MALI_WHITE_LEVEL_LOW (64),
//   DM_MALI_NOSAMPLE_MS (600000: frames with a lit target and no sample before the start counts as one without a verdict), DM_MALI_MARK_N (600: presents on the
//   route that clear the start marker; or DM_MALI_MARK_MIN_N (60) of them over DM_MALI_MARK_MS, 20000 ms) and DM_MALI_MARK_CAP_MS (600000: the marker goes
//   when no frame drew into the lit target that long).
//   The draw floors are low on purpose: a Mali-G615 MC2 on Arm's r44p1 (the D7400 player's log, 2026-11-22) drew about 175-260 draws a frame in play, all passes
//   together, so floors of 200 / 100 could leave the very phones the check is for never sampled; a menu's 3D model or a title screen stays under them, and a scene
//   that gets through is judged by rules that do not call it fine on a few minutes of it
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MW_UNDECIDED = 0, MW_WHITE = 1, MW_FINE = 2, MW_CRASHED = 3, MW_BROKEN = 4, MW_ENDED = 5 };   // a verdict, and a route's result in the state (MW_UNDECIDED: not known
                                                                                                    //   yet; MW_CRASHED: state only; MW_ENDED: mw_ladder_verdict's argument for a
                                                                                                    //   route that closed the game by early ends, kept as MW_CRASHED)
enum { MW_K_BETWEEN = 0, MW_K_WHITE = 1, MW_K_FINE = 2, MW_K_EMPTY = 3, MW_K_NAN = 4, MW_K_SHOWN = 5, MW_K_BRIGHT = 6, MW_NKIND = 7 };   // one sample's kind (mw_obs_kind)
enum { MW_ROUTE_NONE = 0, MW_ROUTE_A = 1, MW_ROUTE_B = 2, MW_ROUTE_BOTH = 3 };      // (bits: A the stand-in, B no compression)
enum { MW_PANVK_NONE = 0, MW_PANVK_OFFER = 1, MW_PANVK_SHOWN = 2 };                 // the PanVK offer: none, to show at the app's next start, shown (MaliLadder.java)
enum { MW_CHECK_NONE = 0, MW_CHECK_FULL = 1, MW_CHECK_RE = 2 };                     // how a start checks: not at all, every few seconds, one sample a minute
enum { MW_EV_NONE = 0, MW_EV_FINE_ONCE, MW_EV_FINE, MW_EV_TRY_B, MW_EV_USE_B, MW_EV_OFFER_PANVK, MW_EV_EXHAUSTED, MW_EV_USE_A, MW_EV_RETRY };   // what a verdict did
enum { MW_RULE_RUN = 1, MW_RULE_SLOW = 2, MW_RULE_FINE = 3, MW_RULE_NAN = 4, MW_RULE_DARK = 5, MW_RULE_SHOWN = 6 };   // what decided a verdict
enum { MW_MK_NONE = 0, MW_MK_EARLY, MW_MK_CRASHED, MW_MK_BOTH, MW_MK_DETECTOR, MW_MK_LOADING, MW_MK_GAME };   // what a start found of the marker an earlier start left (mw_marker_check)
enum { MW_CRASH_NONE = 0, MW_CRASH_DEVICE = 1, MW_CRASH_GAME = 2 };   // how the last start ended (android_main.c boot_guard): no crash flag; a crash after the graphics
                                                                     //   device came up; one in the game's own code (not a route's)
enum { MW_STAGE_NONE = 0, MW_STAGE_DETECTOR = 1, MW_STAGE_LOADING = 2, MW_STAGE_PLAYING = 3 };   // how far the start that left the marker got
#define MW_DARK_BITS 0x3A800000u    // 1/1024 as a float's bits: a texel with every channel below it is near black
#define MW_SHOWN_BITS 0x3F7D70A4u   // 0.99 as a float's bits: a texel of the picture shown with every colour channel at it or more is white
#define MW_ENV_MARK "DM_MALI_ENV=2"   // env.txt's line once mw_env_migrate looked at it
#define MW_ROUTE_CODE 1   // the fixes' own code (route A: b10_conv.h, spv_b10_outputs, b10_view_fmt; route B; the check and its rules): raise it when any of them changes
                          //   what a start on a route draws or how it is judged, and every phone's ladder starts over (an app update alone keeps it)

// ---- the verdict ----
typedef struct {
    int white_n, white_pct;                          // WHITE: white_n samples in a row with white_pct % or more of their texels saturated
    int fine_n, fine_pct, fine_span_ms, fine_starts; // fine at a start: fine_n samples under fine_pct % (no other kind but empty between) over fine_span_ms of play; kept after fine_starts such starts
    int slow_n, slow_pct;                            // WHITE too: the last slow_n samples with content at slow_pct % saturated on average
    int dark_pct, dark_n;                            // a sample with dark_pct % of its texels near black or NaN (and under fine_pct % saturated) is empty: no evidence; dark_n in a row: BROKEN
    int nan_pct;                                     // a sample with nan_pct % or more of its texels NaN is not fine (white_pct % or more: a nan sample, as a white one)
    int shown_white_pct, shown_n, shown_fine_pct;    // the picture shown: shown_white_pct % white is a white sample (a run of them mostly such: shown_n long); shown_fine_pct % white: bright
    int cap_n, undecided_starts;                     // a start stops checking after cap_n samples without a verdict; undecided_starts such starts on a route: BROKEN
    int every_ms, recheck_ms, recheck_starts;        // between samples; one every recheck_ms after fine, and in the recheck_starts starts after a route is kept as fine
    int min_draws, lit_draws, cover_pct, min_w;      // a frame is sampled with min_draws+ draws, lit_draws+ of them into the lit target, their cells cover_pct % of the part drawn
    int level, level_low;                            // saturated: a channel at level or more, or +inf; the log's lower bucket: level_low or more
    int nosample_ms;                                 // a start whose frames drew into a lit target this long without one sample: counted as one without a verdict
    int mark_n, mark_min_n, mark_ms, mark_cap_ms;    // the start marker goes after mark_n presents on the route, or mark_min_n of them over mark_ms; or mark_cap_ms
                                                     //   after the start's first picture with no frame on the route
} mw_rules;
static inline mw_rules mw_rules_default(void) {
    mw_rules r = { .white_n = 3, .white_pct = 25, .fine_n = 20, .fine_pct = 2, .fine_span_ms = 180000, .fine_starts = 2, .slow_n = 40, .slow_pct = 8, .dark_pct = 95,
                   .dark_n = 10, .nan_pct = 8, .shown_white_pct = 90, .shown_n = 5, .shown_fine_pct = 50, .cap_n = 300, .undecided_starts = 3, .every_ms = 3000, .recheck_ms = 60000,
                   .recheck_starts = 3, .min_draws = 120, .lit_draws = 60, .cover_pct = 60, .min_w = 1280, .level = 1024, .level_low = 64, .nosample_ms = 600000,
                   .mark_n = 600, .mark_min_n = 60, .mark_ms = 20000, .mark_cap_ms = 600000 };
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
    r->dark_pct = mw_env_int("DM_MALI_DARK_PCT", r->dark_pct, 1, 100); r->dark_n = mw_env_int("DM_MALI_DARK_N", r->dark_n, 1, 100000);
    r->nan_pct = mw_env_int("DM_MALI_NAN_PCT", r->nan_pct, 1, 100);
    r->shown_white_pct = mw_env_int("DM_MALI_SHOWN_WHITE_PCT", r->shown_white_pct, 1, 101); r->shown_fine_pct = mw_env_int("DM_MALI_SHOWN_FINE_PCT", r->shown_fine_pct, 1, 101);
    r->shown_n = mw_env_int("DM_MALI_SHOWN_N", r->shown_n, 1, 1000);
    r->cap_n = mw_env_int("DM_MALI_WHITE_CAP", r->cap_n, 1, 1000000); r->undecided_starts = mw_env_int("DM_MALI_UNDECIDED_STARTS", r->undecided_starts, 1, 1000);
    r->every_ms = mw_env_int("DM_MALI_WHITE_EVERY_MS", r->every_ms, 0, 3600000); r->recheck_ms = mw_env_int("DM_MALI_RECHECK_MS", r->recheck_ms, 0, 3600000);
    r->recheck_starts = mw_env_int("DM_MALI_RECHECK_STARTS", r->recheck_starts, 0, 100);
    r->min_draws = mw_env_int("DM_MALI_WHITE_MIN_DRAWS", r->min_draws, 0, 1 << 24); r->lit_draws = mw_env_int("DM_MALI_WHITE_LIT_DRAWS", r->min_draws / 2, 0, 1 << 24);
    r->cover_pct = mw_env_int("DM_MALI_WHITE_COVER_PCT", r->cover_pct, 0, 100); r->min_w = mw_env_int("DM_MALI_WHITE_MIN_W", r->min_w, 1, 16384);
    r->level = mw_env_int("DM_MALI_WHITE_LEVEL", r->level, 2, 65504); r->level_low = mw_env_int("DM_MALI_WHITE_LEVEL_LOW", r->level_low, 1, 65504);
    r->nosample_ms = mw_env_int("DM_MALI_NOSAMPLE_MS", r->nosample_ms, 0, 86400000);
    r->mark_n = mw_env_int("DM_MALI_MARK_N", r->mark_n, 1, 1000000); r->mark_min_n = mw_env_int("DM_MALI_MARK_MIN_N", r->mark_min_n, 1, 1000000);
    r->mark_ms = mw_env_int("DM_MALI_MARK_MS", r->mark_ms, 0, 86400000); r->mark_cap_ms = mw_env_int("DM_MALI_MARK_CAP_MS", r->mark_cap_ms, 0, 86400000);
}
static inline uint32_t mw_float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }   // (the detector's thresholds: compared as bits, positive floats order as integers)
// one sample: of the lit target's total texels, sat saturated (a channel at the level or more, or +inf), nan NaN, dark near black; of the picture shown's shown_total
//   texels (0: not read), shown white
typedef struct { uint32_t sat, nan, dark, total, shown, shown_total; } mw_obs;
typedef struct {
    int verdict, rule;                               // MW_UNDECIDED / MW_WHITE / MW_BROKEN / MW_FINE (fine at this start: it can still fail; a failure stays), what decided it (MW_RULE_*)
    int samples, kinds[MW_NKIND];                    // samples judged, by kind (MW_K_*)
    int white_run, run_nan, run_shown;               // white, nan and shown samples in a row (empty ones skipped), how many of them nan / shown ones
    int empty_run;                                   // empty samples in a row
    int fine_n; int64_t fine_ms, last_ms;            // the fine stretch: its samples, the play they span (each gap at most 4 sample intervals + 1 s), the last one's time
    int win_n, win_i; uint32_t win_sum; uint16_t win[256];   // the slow rule: the last slow_n samples with content, each one's saturated share in 1/10000
} mw_judge;
static inline int mw_bad(int v) { return v == MW_WHITE || v == MW_BROKEN; }   // (a verdict that stays)
// one sample's kind: MW_K_WHITE, MW_K_NAN, MW_K_SHOWN, MW_K_BETWEEN, MW_K_EMPTY, MW_K_BRIGHT or MW_K_FINE (see the top)
static inline int mw_obs_kind(const mw_rules *r, const mw_obs *o) {
    uint64_t t = o->total, st = o->shown_total;
    if ((uint64_t)o->sat * 100 >= t * (uint64_t)r->white_pct) return MW_K_WHITE;
    if ((uint64_t)o->nan * 100 >= t * (uint64_t)r->white_pct) return MW_K_NAN;
    if (st && (uint64_t)o->shown * 100 >= st * (uint64_t)r->shown_white_pct) return MW_K_SHOWN;
    if ((uint64_t)o->sat * 100 >= t * (uint64_t)r->fine_pct || (uint64_t)o->nan * 100 >= t * (uint64_t)r->nan_pct) return MW_K_BETWEEN;
    if ((uint64_t)(o->dark + o->nan) * 100 >= t * (uint64_t)r->dark_pct) return MW_K_EMPTY;
    if (st && (uint64_t)o->shown * 100 >= st * (uint64_t)r->shown_fine_pct) return MW_K_BRIGHT;
    return MW_K_FINE;
}
static inline int mw_sample_kind(const mw_rules *r, uint32_t sat, uint32_t dark, uint32_t total) { mw_obs o = { sat, 0, dark, total, 0, 0 }; return mw_obs_kind(r, &o); }
static inline const char *mw_kind_name(int k) {
    return k == MW_K_WHITE ? "white" : k == MW_K_FINE ? "fine" : k == MW_K_EMPTY ? "empty" : k == MW_K_NAN ? "nan" : k == MW_K_SHOWN ? "shown white" : k == MW_K_BRIGHT ? "bright" : "between";
}
// one sample taken at t_ms into the verdict so far (returned); a sample of no texels changes nothing; WHITE and BROKEN stay, FINE can turn into either
static inline int mw_judge_obs(mw_judge *j, const mw_rules *r, const mw_obs *o, int64_t t_ms) {
    if (!o->total || mw_bad(j->verdict)) return j->verdict;
    int k = mw_obs_kind(r, o);
    j->samples++; j->kinds[k]++;
    if (k == MW_K_EMPTY) {   // (a black, cleared or never drawn target: it breaks no run; a minute of nothing else is a black picture)
        if (++j->empty_run >= r->dark_n) { j->verdict = MW_BROKEN; j->rule = MW_RULE_DARK; }
        return j->verdict; }
    j->empty_run = 0;
    int n = r->slow_n < 1 ? 1 : r->slow_n > 256 ? 256 : r->slow_n; uint16_t bp = (uint16_t)((uint64_t)o->sat * 10000 / o->total);
    if (j->win_n >= n) j->win_sum -= j->win[j->win_i]; else j->win_n++;
    j->win[j->win_i] = bp; j->win_sum += bp; j->win_i = (j->win_i + 1) % n;
    if (k == MW_K_WHITE || k == MW_K_NAN || k == MW_K_SHOWN) { j->white_run++; j->run_nan += k == MW_K_NAN; j->run_shown += k == MW_K_SHOWN; j->fine_n = 0; j->fine_ms = 0; }
    else if (k == MW_K_FINE) {
        if (j->fine_n) { int64_t g = t_ms - j->last_ms, cap = 4LL * r->every_ms + 1000; j->fine_ms += g < 0 ? 0 : g > cap ? cap : g; }
        j->fine_n++; j->last_ms = t_ms; j->white_run = j->run_nan = j->run_shown = 0; }
    else if (k == MW_K_BRIGHT) j->white_run = j->run_nan = j->run_shown = 0;   // (no evidence: the fine stretch goes on, a white run starts over)
    else { j->white_run = j->run_nan = j->run_shown = 0; j->fine_n = 0; j->fine_ms = 0; }
    int v = MW_UNDECIDED, rule = 0;
    if (j->white_run >= r->white_n) {
        if (j->run_nan * 2 > j->white_run) { v = MW_BROKEN; rule = MW_RULE_NAN; }
        else if (j->run_shown * 2 <= j->white_run) { v = MW_WHITE; rule = MW_RULE_RUN; }
        else if (j->white_run >= r->shown_n) { v = MW_WHITE; rule = MW_RULE_SHOWN; } }   // (mostly the picture shown alone: a longer run, past a fade to white)
    if (!v && j->win_n >= n && j->win_sum >= (uint32_t)r->slow_pct * 100u * (uint32_t)n) { v = MW_WHITE; rule = MW_RULE_SLOW; }
    if (v) { j->verdict = v; j->rule = rule; }
    else if (!j->verdict && j->fine_n >= r->fine_n && j->fine_ms >= r->fine_span_ms) { j->verdict = MW_FINE; j->rule = MW_RULE_FINE; }
    return j->verdict;
}
static inline int mw_judge_add(mw_judge *j, const mw_rules *r, uint32_t sat, uint32_t dark, uint32_t total, int64_t t_ms) {   // (the lit target alone, no NaN)
    mw_obs o = { sat, 0, dark, total, 0, 0 }; return mw_judge_obs(j, r, &o, t_ms);
}
static inline int mw_judge_capped(const mw_judge *j, const mw_rules *r) { return !j->verdict && j->samples >= r->cap_n; }   // (a start stops checking there)
static inline int mw_judge_mostly_empty(const mw_judge *j) { return j->kinds[MW_K_EMPTY] * 2 > j->samples; }   // (at the cap: a black picture, the route broken at once)
static inline int mw_judge_mostly_bright(const mw_judge *j) { return j->kinds[MW_K_BRIGHT] * 2 > j->samples; }   // (at the cap: a bright scene, nothing to count)

// ---- who runs it, and the route ----
// 0 off; 1 on (Arm's own driver with Sword / Shield); 2 on through a test switch on any GPU (DM_MALI_LADDER=1, DM_MALI_WHITE_SIM=1). DM_MALI_LADDER=0 wins over both.
//   arm_own && galar is the default: nvn_vk.c mw_init passes the quirk table's rule as arm_own and 1 as galar (vk_quirks.h Q_MALI_WHITE_CHECK, Q_B10_STANDIN below:
//   Arm's own driver, a game whose profile says mali_lit_standin; dm/gfx/tests/t_quirks.c checks the table against these two with arm_own = Arm's own driver and
//   galar = Sword / Shield's profile)
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
// Other white-screen tests set in env.txt (the menu's White screen tests): they change the shaders' arithmetic or the lit target's format beside the route, so a
//   fine verdict then says nothing about the route alone (a white one still says the fix did not take). Their "NAME=value" list into out (empty: none); returns how
//   many. The shader rules are read as on / off when present at all (an empty value is 0 to the engine), the probe and FMZ only when not empty.
//   DM_VK_FLOAT_CONTROLS=0 from the safe-start ladder (level 5, DM_SAFE_START_LEVEL) is the app's own fallback, not a test
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
    int driver; uint32_t driver_version; long app; char gpu[128];   // whose state: the Vulkan driver's ID and version (the app's versionCode: the last start's, for the
                                                                     //   app's PanVK offer; the GPU's name, for that offer too)
    int code;                                                        // MW_ROUTE_CODE of the fixes that found it (another: started over; a file without it: 1)
    int res[3], fine_starts[3], rechecks[3];                         // per route ([MW_ROUTE_A], [MW_ROUTE_B]): its result; starts that came out fine; re-check starts since it was kept as fine
    int crashes[3], ends[3], undecided[3];                           //   times it closed the game with the crash flag, times by two early ends in a row; starts on it that reached the cap
                                                                     //   with no verdict
    int started, stage, early;                                       // the start marker: the route a start checks, until 20 s of play or its first sample; how far that start got
                                                                     //   (MW_STAGE_*); starts on it in a row that ended early while playing, without a crash
    int panvk;
} mw_state;
static inline void mw_state_reset(mw_state *s, int driver, uint32_t driver_version, long app) {
    memset(s, 0, sizeof *s); s->driver = driver; s->driver_version = driver_version; s->app = app; s->code = MW_ROUTE_CODE; }
static inline int mw_failed(int r) { return r == MW_WHITE || r == MW_CRASHED || r == MW_BROKEN; }
static inline int mw_crash_flagged(const mw_state *s, int rt) { return s->res[rt] == MW_CRASHED && s->crashes[rt] > 0; }   // (it closed the game with the crash flag, not only on early ends)
// the route a start with this state runs, and how it checks: a route kept as fine is re-checked for recheck_starts starts, then not at all; A failed → B, checked;
//   both failed → a route that closed the game once (the other one white or broken) once more, else the exhausted state, unchecked: route A unless it closed the game
//   with the crash flag, else B unless it did too, else none
typedef struct { int route, check, exhausted, retry, offer, reset; } mw_plan;
static inline mw_plan mw_plan_of(const mw_state *s, const mw_rules *r) {
    mw_plan p = { MW_ROUTE_A, MW_CHECK_FULL, 0, 0, 0, 0 }; int a = s->res[MW_ROUTE_A], b = s->res[MW_ROUTE_B];
    if (a == MW_FINE) p.check = s->rechecks[MW_ROUTE_A] < r->recheck_starts ? MW_CHECK_RE : MW_CHECK_NONE;
    else if (b == MW_FINE) { p.route = MW_ROUTE_B; p.check = s->rechecks[MW_ROUTE_B] < r->recheck_starts ? MW_CHECK_RE : MW_CHECK_NONE; }
    else if (mw_failed(a) && mw_failed(b)) {
        if (a == MW_CRASHED && b != MW_CRASHED && s->crashes[MW_ROUTE_A] + s->ends[MW_ROUTE_A] < 2) p.retry = 1;
        else if (b == MW_CRASHED && a != MW_CRASHED && s->crashes[MW_ROUTE_B] + s->ends[MW_ROUTE_B] < 2) { p.route = MW_ROUTE_B; p.retry = 1; }
        else { p.exhausted = 1; p.check = MW_CHECK_NONE; p.route = !mw_crash_flagged(s, MW_ROUTE_A) ? MW_ROUTE_A : !mw_crash_flagged(s, MW_ROUTE_B) ? MW_ROUTE_B : MW_ROUTE_NONE; }
    }
    else if (mw_failed(a)) p.route = MW_ROUTE_B;
    p.offer = s->panvk == MW_PANVK_OFFER;
    return p;
}
// the route a start runs when the check cannot run on this driver (its compute pass failed, or closed the game making it at an earlier start): the plan's, except a
//   route B planned only because A closed the game on early ends (nothing the check judged, no crash flag): route A then, the default, not a route never checked
static inline int mw_route_unchecked(const mw_state *s, const mw_plan *p) {
    if (p->route == MW_ROUTE_B && !p->exhausted && s->res[MW_ROUTE_A] == MW_CRASHED && !s->crashes[MW_ROUTE_A] && s->res[MW_ROUTE_B] != MW_FINE) return MW_ROUTE_A;
    return p->route;
}
// at a start: the state read from the file (have = 0: none) is this driver's and these fixes' (MW_ROUTE_CODE), else started over (plan.reset); the app's
//   versionCode noted (an app update keeps the state); the ladder exhausted and no offer made yet: the offer is made now (shown by the app at its next start)
static inline mw_plan mw_ladder_start(mw_state *s, const mw_rules *r, int have, int driver, uint32_t driver_version, long app) {
    int reset = !have || s->driver != driver || s->driver_version != driver_version || s->code != MW_ROUTE_CODE;
    if (reset) mw_state_reset(s, driver, driver_version, app);
    s->app = app;
    mw_plan p = mw_plan_of(s, r);
    if (p.exhausted && s->panvk == MW_PANVK_NONE) { s->panvk = MW_PANVK_OFFER; p.offer = 1; }
    p.reset = reset;
    return p;
}
// a verdict for the route that ran: MW_WHITE / MW_BROKEN for MW_ROUTE_A or MW_ROUTE_B (both at once: both), MW_CRASHED / MW_ENDED (it closed the game: the crash
//   flag / two early ends; kept as MW_CRASHED) for A or B, MW_FINE (fine at this start) for A or B. Its result kept; returns what follows (MW_EV_*): FINE_ONCE (fine at
//   fewer than fine_starts starts: checked again), FINE (kept), TRY_B / USE_B / USE_A / RETRY (the next start's route), OFFER_PANVK (the ladder exhausted: the app
//   offers PanVK), EXHAUSTED (so, the offer made before), NONE (nothing changed)
static inline int mw_ladder_verdict(mw_state *s, const mw_rules *r, int route, int v) {
    if (route == MW_ROUTE_BOTH && mw_bad(v)) { s->res[MW_ROUTE_A] = s->res[MW_ROUTE_B] = v; s->fine_starts[MW_ROUTE_A] = s->fine_starts[MW_ROUTE_B] = 0; }
    else if (route != MW_ROUTE_A && route != MW_ROUTE_B) return MW_EV_NONE;
    else if (v == MW_FINE) {
        if (s->res[route] == MW_FINE) return MW_EV_NONE;   // (a re-check start that came out fine again)
        s->undecided[route] = 0;
        if (++s->fine_starts[route] < r->fine_starts) return MW_EV_FINE_ONCE;
        s->res[route] = MW_FINE; s->rechecks[route] = 0; return MW_EV_FINE;
    }
    else if (mw_bad(v) || v == MW_CRASHED || v == MW_ENDED) {
        s->res[route] = v == MW_ENDED ? MW_CRASHED : v; s->fine_starts[route] = 0;
        if (v == MW_CRASHED && s->crashes[route] < 1000) s->crashes[route]++;
        if (v == MW_ENDED && s->ends[route] < 1000) s->ends[route]++; }
    else return MW_EV_NONE;
    mw_plan p = mw_plan_of(s, r);
    if (p.exhausted) { if (s->panvk != MW_PANVK_NONE) return MW_EV_EXHAUSTED; s->panvk = MW_PANVK_OFFER; return MW_EV_OFFER_PANVK; }
    if (p.retry) return MW_EV_RETRY;
    if (p.route == MW_ROUTE_B) return p.check == MW_CHECK_FULL ? MW_EV_TRY_B : MW_EV_USE_B;
    return MW_EV_USE_A;
}
// a start on a route that reached the cap with no verdict: counted; at undecided_starts such starts, or at once when its samples were mostly empty (mostly_empty: a
//   black picture), the route is BROKEN (both at once: both broken when mostly empty, else neither counted). Returns what follows, as mw_ladder_verdict; MW_EV_NONE
//   when only counted
static inline int mw_ladder_undecided(mw_state *s, const mw_rules *r, int route, int mostly_empty) {
    if (route == MW_ROUTE_BOTH) return mostly_empty ? mw_ladder_verdict(s, r, route, MW_BROKEN) : MW_EV_NONE;
    if (route != MW_ROUTE_A && route != MW_ROUTE_B) return MW_EV_NONE;
    if (s->undecided[route] < 1000) s->undecided[route]++;
    return mostly_empty || s->undecided[route] >= r->undecided_starts ? mw_ladder_verdict(s, r, route, MW_BROKEN) : MW_EV_NONE;
}
// The start marker. At a start (after mw_ladder_start): the marker an earlier start left. Left at the detector stage: the end was the check's own (MW_MK_DETECTOR,
//   nobody blamed). A crash in the game's own code (crashed MW_CRASH_GAME): not the route's, nothing counted, the early count as it was (MW_MK_GAME). No crash:
//   left while loading, nothing counted (MW_MK_LOADING); left while playing, the first such start in a row is counted (early) and the route runs again
//   (MW_MK_EARLY). A crash after the device came up (crashed MW_CRASH_DEVICE) while loading or playing, or the second early end in a row: the route closed the game
//   (MW_MK_CRASHED, the ladder moves); when the other route had closed it before, the crashes are not the routes': both are checked again (MW_MK_BOTH). The route
//   and the early count stay for this start's marker (mw_marker_set keeps the count for the same route). *route: the marker's route (0: none)
static inline int mw_marker_check(mw_state *s, const mw_rules *r, int crashed, int *route) {
    int rt = s->started, st = s->stage; *route = rt == MW_ROUTE_A || rt == MW_ROUTE_B ? rt : 0;
    if (!*route) { s->started = s->early = s->stage = 0; return MW_MK_NONE; }
    if (st == MW_STAGE_DETECTOR) { s->stage = 0; return MW_MK_DETECTOR; }
    if (crashed == MW_CRASH_GAME) { s->stage = 0; return MW_MK_GAME; }
    if (!crashed && st == MW_STAGE_LOADING) { s->stage = 0; return MW_MK_LOADING; }
    if (!crashed && s->early < 1) { s->early++; s->stage = 0; return MW_MK_EARLY; }
    s->started = s->early = s->stage = 0;
    int other = rt == MW_ROUTE_A ? MW_ROUTE_B : MW_ROUTE_A;
    if (s->res[other] == MW_CRASHED) {
        s->res[MW_ROUTE_A] = s->res[MW_ROUTE_B] = MW_UNDECIDED; s->fine_starts[MW_ROUTE_A] = s->fine_starts[MW_ROUTE_B] = 0;
        s->crashes[MW_ROUTE_A] = s->crashes[MW_ROUTE_B] = s->ends[MW_ROUTE_A] = s->ends[MW_ROUTE_B] = 0;
        if (s->panvk == MW_PANVK_OFFER) s->panvk = MW_PANVK_NONE;
        return MW_MK_BOTH; }
    mw_ladder_verdict(s, r, rt, crashed ? MW_CRASHED : MW_ENDED);
    return MW_MK_CRASHED;
}
// this start's marker: the route it checks (0: none, it checks none of the ladder's routes) and how far it got (MW_STAGE_*); the early-end count goes on only for
//   the same route
static inline void mw_marker_set(mw_state *s, int route, int stage) { if (route != s->started) s->early = 0; s->started = route; s->stage = route ? stage : 0; }
static inline void mw_marker_clear(mw_state *s) { s->started = s->early = s->stage = 0; }   // (20 s of play, or the first sample read back)

static inline const char *mw_res_name(int r) { return r == MW_WHITE ? "white" : r == MW_FINE ? "fine" : r == MW_CRASHED ? "crashed" : r == MW_BROKEN ? "broken" : "-"; }
static inline int mw_res_parse(const char *v) {
    return !strncmp(v, "white", 5) ? MW_WHITE : !strncmp(v, "fine", 4) ? MW_FINE : !strncmp(v, "crashed", 7) ? MW_CRASHED : !strncmp(v, "broken", 6) ? MW_BROKEN : MW_UNDECIDED;
}
static inline const char *mw_stage_name(int st) { return st == MW_STAGE_DETECTOR ? "detector" : st == MW_STAGE_LOADING ? "loading" : st == MW_STAGE_PLAYING ? "playing" : "-"; }
// the file: key=value lines, # comments; unknown lines are left out, missing ones take their defaults. next= is for people (mw_plan_of decides)
static inline int mw_state_format(const mw_state *s, const mw_rules *r, char *out, size_t n) {
    mw_plan p = mw_plan_of(s, r);
    const char *how = p.exhausted ? " (both failed: unchecked)" : p.retry ? " (once more: it closed the game once; checked)" : p.check == MW_CHECK_RE ? " (fine: re-checked once a minute)"
                    : p.check == MW_CHECK_NONE ? " (fine: unchecked)" : " (checked)";
    return snprintf(out, n, "# Galar Native: the white-screen check on this phone's Mali driver (nvn_vk.c, mali_white.h). Started over when the driver or the fixes' code changes\n"
                    "driver=%d\ndriver_version=%#x\napp=%ld\ncode=%d\ngpu=%s\nA=%s\nB=%s\nA_fine_starts=%d\nB_fine_starts=%d\nA_rechecks=%d\nB_rechecks=%d\nA_crashes=%d\nB_crashes=%d\n"
                    "A_ends=%d\nB_ends=%d\nA_undecided=%d\nB_undecided=%d\nstarted=%s\nstage=%s\nearly_ends=%d\nnext=%s%s\npanvk=%s\n", s->driver, s->driver_version, s->app, s->code, s->gpu,
                    mw_res_name(s->res[MW_ROUTE_A]), mw_res_name(s->res[MW_ROUTE_B]), s->fine_starts[MW_ROUTE_A], s->fine_starts[MW_ROUTE_B], s->rechecks[MW_ROUTE_A],
                    s->rechecks[MW_ROUTE_B], s->crashes[MW_ROUTE_A], s->crashes[MW_ROUTE_B], s->ends[MW_ROUTE_A], s->ends[MW_ROUTE_B], s->undecided[MW_ROUTE_A],
                    s->undecided[MW_ROUTE_B], s->started == MW_ROUTE_A ? "A" : s->started == MW_ROUTE_B ? "B" : "-", mw_stage_name(s->stage), s->early, mw_route_name(p.route), how,
                    s->panvk == MW_PANVK_OFFER ? "offer" : s->panvk == MW_PANVK_SHOWN ? "shown" : "-");
}
static inline int mw_count_parse(const char *v) { long x = strtol(v, NULL, 10); return x < 0 ? 0 : x > 1000 ? 1000 : (int)x; }
static inline int mw_state_parse(const char *text, mw_state *s) {   // 1 when the text has the three key fields (driver, driver_version, app)
    mw_state_reset(s, -1, 0, -1); s->code = 1; int have = 0, stage = -1;   // (a file without code=: written by the fixes of code 1, before the line was)
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
        else if (!strcmp(k, "code")) s->code = (int)strtol(v, NULL, 10);
        else if (!strcmp(k, "gpu")) { size_t gl = vl < sizeof s->gpu - 1 ? vl : sizeof s->gpu - 1; memcpy(s->gpu, v, gl); s->gpu[gl] = 0; }   // (a longer name cut)
        else if (!strcmp(k, "A") || !strcmp(k, "B")) s->res[k[0] == 'A' ? MW_ROUTE_A : MW_ROUTE_B] = mw_res_parse(v);
        else if ((k[0] == 'A' || k[0] == 'B') && k[1] == '_') {
            int rt = k[0] == 'A' ? MW_ROUTE_A : MW_ROUTE_B;
            if (!strcmp(k + 2, "fine_starts")) s->fine_starts[rt] = mw_count_parse(v);
            else if (!strcmp(k + 2, "rechecks")) s->rechecks[rt] = mw_count_parse(v);
            else if (!strcmp(k + 2, "crashes")) s->crashes[rt] = mw_count_parse(v);
            else if (!strcmp(k + 2, "ends")) s->ends[rt] = mw_count_parse(v);
            else if (!strcmp(k + 2, "undecided")) s->undecided[rt] = mw_count_parse(v); }
        else if (!strcmp(k, "started")) s->started = v[0] == 'A' ? MW_ROUTE_A : v[0] == 'B' ? MW_ROUTE_B : 0;
        else if (!strcmp(k, "stage")) stage = !strcmp(v, "detector") ? MW_STAGE_DETECTOR : !strcmp(v, "loading") ? MW_STAGE_LOADING : !strcmp(v, "playing") ? MW_STAGE_PLAYING : 0;
        else if (!strcmp(k, "early_ends")) s->early = mw_count_parse(v);
        else if (!strcmp(k, "panvk")) s->panvk = !strncmp(v, "offer", 5) ? MW_PANVK_OFFER : !strncmp(v, "shown", 5) ? MW_PANVK_SHOWN : MW_PANVK_NONE;
    }
    // (a marker without its stage: a file from before the stages, whose ends all counted as played)
    s->stage = !s->started ? 0 : stage >= 0 ? stage : MW_STAGE_PLAYING;
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

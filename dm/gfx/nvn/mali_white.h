// The Mali white-screen ladder's rules, free of Vulkan so a unit test runs them (dm/gfx/tests/t_mali_white.c); the detector and the hooks are nvn_vk.c's
//   (mali_white_vk.inc). Arm's own Mali driver draws Sword / Shield's lit scene white; two fixes exist: route A, R11G11B10F drawn as an RGBA16F stand-in
//   (DM_VK_B10_AS_RGBA16), and route B, R11G11B10F kept without framebuffer compression (DM_VK_B10_NOCOMPRESS). Nobody can test them on Mali, so the game
//   checks its own lit scene while playing and moves on by itself, one start at a time:
//  - the verdict (mw_judge_add): each sample is the share of a grid of the lit target's texels at 60000 or more, infinite or NaN; WHITE after white_n samples in
//    a row at white_pct % or more, FINE after fine_n samples in a row under fine_pct %, anything between breaks both runs (UNDECIDED: the check goes on);
//  - the ladder (mw_ladder_start at each start, mw_ladder_verdict on a verdict), kept per phone in one small text file (mw_state_format): route A first; A white
//    → route B from the next start; B white too → back to route A, and the app offers the PanVK driver at its next start (once); a route found fine is kept
//    and the check stops for good. Started over when the Vulkan driver (its ID or version) or the app (its versionCode) changes.
// Switches (env.txt): DM_MALI_LADDER=0 no ladder and no check (route A stays the default on Arm's own driver with Sword / Shield), =1 both on any GPU (a test);
//   DM_MALI_WHITE_SIM=1 every sample counts as white (the ladder on a phone that draws right; it turns the ladder on too, unless DM_MALI_LADDER=0);
//   DM_VK_B10_AS_RGBA16 / DM_VK_B10_NOCOMPRESS set in env.txt always win over the ladder's route (mw_route_apply). The rules' numbers (mw_rules_env, for tests):
//   DM_MALI_WHITE_N (3), DM_MALI_FINE_N (5), DM_MALI_WHITE_PCT (25), DM_MALI_FINE_PCT (2), DM_MALI_WHITE_EVERY_MS (3000: between samples),
//   DM_MALI_WHITE_MIN_DRAWS (200: a frame drawing fewer is a load or a menu, not sampled), DM_MALI_WHITE_MIN_W (1280: the narrowest lit target sampled)
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MW_UNDECIDED = 0, MW_WHITE = 1, MW_FINE = 2 };                              // a verdict, and a route's result in the state (MW_UNDECIDED: not tried yet)
enum { MW_ROUTE_NONE = 0, MW_ROUTE_A = 1, MW_ROUTE_B = 2, MW_ROUTE_BOTH = 3 };     // (bits: A the stand-in, B no compression)
enum { MW_PANVK_NONE = 0, MW_PANVK_OFFER = 1, MW_PANVK_SHOWN = 2 };                // the PanVK offer: none, to show at the app's next start, shown (MaliLadder.java)
enum { MW_EV_NONE = 0, MW_EV_FINE, MW_EV_TRY_B, MW_EV_USE_B, MW_EV_OFFER_PANVK, MW_EV_USE_A };   // what a verdict did (mw_ladder_verdict)

// ---- the verdict ----
typedef struct { int white_n, fine_n, white_pct, fine_pct, every_ms, min_draws, min_w; } mw_rules;
static inline mw_rules mw_rules_default(void) { mw_rules r = { 3, 5, 25, 2, 3000, 200, 1280 }; return r; }
static inline int mw_env_int(const char *name, int def, int lo, int hi) {   // an integer from the environment, def when unset, empty or out of range
    const char *e = getenv(name); if (!e || !*e) return def;
    char *end; long v = strtol(e, &end, 10); return *end || v < lo || v > hi ? def : (int)v;
}
static inline void mw_rules_env(mw_rules *r) {
    r->white_n = mw_env_int("DM_MALI_WHITE_N", r->white_n, 1, 1000); r->fine_n = mw_env_int("DM_MALI_FINE_N", r->fine_n, 1, 1000);
    r->white_pct = mw_env_int("DM_MALI_WHITE_PCT", r->white_pct, 1, 100); r->fine_pct = mw_env_int("DM_MALI_FINE_PCT", r->fine_pct, 0, 100);
    r->every_ms = mw_env_int("DM_MALI_WHITE_EVERY_MS", r->every_ms, 0, 3600000); r->min_draws = mw_env_int("DM_MALI_WHITE_MIN_DRAWS", r->min_draws, 0, 1 << 24);
    r->min_w = mw_env_int("DM_MALI_WHITE_MIN_W", r->min_w, 1, 16384);
}
typedef struct { int white_run, fine_run, samples, verdict; } mw_judge;
static inline int mw_sample_kind(const mw_rules *r, uint32_t sat, uint32_t total) {   // one sample: MW_WHITE (at white_pct % or more), MW_FINE (under fine_pct %), else MW_UNDECIDED
    uint64_t s = (uint64_t)sat * 100, t = total;
    return s >= t * (uint64_t)r->white_pct ? MW_WHITE : s < t * (uint64_t)r->fine_pct ? MW_FINE : MW_UNDECIDED;
}
// one sample (sat of total texels saturated) into the verdict so far; a sample of no texels changes nothing; once decided the verdict stays
static inline int mw_judge_add(mw_judge *j, const mw_rules *r, uint32_t sat, uint32_t total) {
    if (j->verdict || !total) return j->verdict;
    j->samples++;
    int k = mw_sample_kind(r, sat, total);
    if (k == MW_WHITE) { j->white_run++; j->fine_run = 0; }
    else if (k == MW_FINE) { j->fine_run++; j->white_run = 0; }
    else j->white_run = j->fine_run = 0;
    if (j->white_run >= r->white_n) j->verdict = MW_WHITE;
    else if (j->fine_run >= r->fine_n) j->verdict = MW_FINE;
    return j->verdict;
}

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

// ---- the ladder's state (one file per phone) ----
typedef struct { int driver; uint32_t driver_version; long app; int res[3]; int panvk; } mw_state;   // res[MW_ROUTE_A], res[MW_ROUTE_B]: MW_UNDECIDED / WHITE / FINE
static inline void mw_state_reset(mw_state *s, int driver, uint32_t driver_version, long app) { memset(s, 0, sizeof *s); s->driver = driver; s->driver_version = driver_version; s->app = app; }
// the route a start with this state runs, and whether it checks: a route found fine is kept unchecked; A white → B, checked; both white → A unchecked (exhausted)
typedef struct { int route, sample, exhausted, offer, reset; } mw_plan;
static inline mw_plan mw_plan_of(const mw_state *s) {
    mw_plan p = { MW_ROUTE_A, 1, 0, 0, 0 }; int a = s->res[MW_ROUTE_A], b = s->res[MW_ROUTE_B];
    if (a == MW_FINE) { p.route = MW_ROUTE_A; p.sample = 0; }
    else if (b == MW_FINE) { p.route = MW_ROUTE_B; p.sample = 0; }
    else if (a == MW_WHITE && b == MW_WHITE) { p.route = MW_ROUTE_A; p.sample = 0; p.exhausted = 1; }
    else if (a == MW_WHITE) { p.route = MW_ROUTE_B; p.sample = 1; }
    p.offer = s->panvk == MW_PANVK_OFFER;
    return p;
}
// at a start: the state read from the file (have = 0: none) is this driver's and this app's, else started over (plan.reset); both routes white and no offer made
//   yet: the offer is made now (shown by the app at its next start)
static inline mw_plan mw_ladder_start(mw_state *s, int have, int driver, uint32_t driver_version, long app) {
    int reset = !have || s->driver != driver || s->driver_version != driver_version || s->app != app;
    if (reset) mw_state_reset(s, driver, driver_version, app);
    if (s->res[MW_ROUTE_A] == MW_WHITE && s->res[MW_ROUTE_B] == MW_WHITE && s->panvk == MW_PANVK_NONE) s->panvk = MW_PANVK_OFFER;
    mw_plan p = mw_plan_of(s); p.reset = reset;
    return p;
}
// a verdict for the route that ran (MW_ROUTE_A or MW_ROUTE_B; another one, or no verdict, changes nothing): its result kept; returns what follows (MW_EV_*)
static inline int mw_ladder_verdict(mw_state *s, int route, int verdict) {
    if ((route != MW_ROUTE_A && route != MW_ROUTE_B) || (verdict != MW_WHITE && verdict != MW_FINE)) return MW_EV_NONE;
    s->res[route] = verdict;
    if (verdict == MW_FINE) return MW_EV_FINE;
    int other = route == MW_ROUTE_A ? MW_ROUTE_B : MW_ROUTE_A, o = s->res[other];
    if (o == MW_WHITE) { if (s->panvk == MW_PANVK_NONE) s->panvk = MW_PANVK_OFFER; return MW_EV_OFFER_PANVK; }   // both white: back to A, PanVK offered
    if (o == MW_FINE) return other == MW_ROUTE_B ? MW_EV_USE_B : MW_EV_USE_A;                                   // the other one is known to work
    return route == MW_ROUTE_A ? MW_EV_TRY_B : MW_EV_USE_A;                                                     // A white: B next; B white (A untried): A next
}
static inline const char *mw_res_name(int r) { return r == MW_WHITE ? "white" : r == MW_FINE ? "fine" : "-"; }
static inline int mw_res_parse(const char *v) { return !strncmp(v, "white", 5) ? MW_WHITE : !strncmp(v, "fine", 4) ? MW_FINE : MW_UNDECIDED; }
// the file: key=value lines, # comments; unknown lines are left out, missing ones take their defaults. next= is for people (mw_plan_of decides)
static inline int mw_state_format(const mw_state *s, char *out, size_t n) {
    mw_plan p = mw_plan_of(s);
    return snprintf(out, n, "# Galar Native: the white-screen check on this phone's Mali driver (nvn_vk.c, mali_white.h). Started over when the driver or the app changes\n"
                    "driver=%d\ndriver_version=%#x\napp=%ld\nA=%s\nB=%s\nnext=%s%s\npanvk=%s\n", s->driver, s->driver_version, s->app,
                    mw_res_name(s->res[MW_ROUTE_A]), mw_res_name(s->res[MW_ROUTE_B]), mw_route_name(p.route), p.sample ? "" : p.exhausted ? " (both white: unchecked)" : " (fine: unchecked)",
                    s->panvk == MW_PANVK_OFFER ? "offer" : s->panvk == MW_PANVK_SHOWN ? "shown" : "-");
}
static inline int mw_state_parse(const char *text, mw_state *s) {   // 1 when the text has the three key fields (driver, driver_version, app)
    mw_state_reset(s, -1, 0, -1); int have = 0;
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char k[32], v[64]; if (*l == '#' || sscanf(l, "%31[^=\n]=%63[^\n]", k, v) != 2) continue;
        if (!strcmp(k, "driver")) { s->driver = (int)strtol(v, NULL, 0); have |= 1; }
        else if (!strcmp(k, "driver_version")) { s->driver_version = (uint32_t)strtoul(v, NULL, 0); have |= 2; }
        else if (!strcmp(k, "app")) { s->app = strtol(v, NULL, 0); have |= 4; }
        else if (!strcmp(k, "A")) s->res[MW_ROUTE_A] = mw_res_parse(v);
        else if (!strcmp(k, "B")) s->res[MW_ROUTE_B] = mw_res_parse(v);
        else if (!strcmp(k, "panvk")) s->panvk = !strncmp(v, "offer", 5) ? MW_PANVK_OFFER : !strncmp(v, "shown", 5) ? MW_PANVK_SHOWN : MW_PANVK_NONE;
    }
    return have == 7;
}
static inline int mw_state_load(const char *path, mw_state *s) {
    char b[1024]; FILE *f = path && *path ? fopen(path, "r") : NULL; if (!f) { mw_state_reset(s, -1, 0, -1); return 0; }
    size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f);
    return mw_state_parse(b, s);
}
static inline int mw_state_save(const char *path, const mw_state *s) {   // written beside and renamed over (a crash mid-write leaves the old file)
    if (!path || !*path) return 0;
    char t[1200], b[1024]; snprintf(t, sizeof t, "%s.tmp", path); int n = mw_state_format(s, b, sizeof b);
    FILE *f = fopen(t, "w"); if (!f) return 0;
    int ok = fwrite(b, 1, (size_t)n, f) == (size_t)n; ok &= fclose(f) == 0;
    return ok && rename(t, path) == 0;
}

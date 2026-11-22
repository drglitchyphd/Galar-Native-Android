// The Mali white-screen ladder's rules (nvn/mali_white.h): the verdict from the detector's samples (runs, boundaries, a verdict that stays, the env's numbers),
//   who runs the ladder (Arm's own driver with Sword / Shield, the test switches, DM_MALI_LADDER=0), the route with env.txt's own settings winning, and the
//   ladder's state from start to start through its file: every transition, the PanVK offer made once, started over for another driver ID, driver version or
//   app version, a file that is missing, damaged or edited. Expected values are hand-written.
//   cc -O1 -Wall -I../nvn -o /tmp/t_mali_white t_mali_white.c && /tmp/t_mali_white
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "mali_white.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s ", ok_ ? "\xe2\x9c\x93" : "\xe2\x9c\x97"); printf(__VA_ARGS__); printf("\n"); } while (0)
enum { T = 576 };   // the detector's grid: 32 x 18 texels
static const int ARM = 9;   // VK_DRIVER_ID_ARM_PROPRIETARY

// samples (saturated texels of T, -1 ends) into a fresh verdict; the sample number it was decided at (0: undecided) in *at
static int run(const mw_rules *r, const int *s, int *at) {
    mw_judge j = { 0 }; *at = 0;
    for (int i = 0; s[i] >= 0; i++) { int v = mw_judge_add(&j, r, (uint32_t)s[i], T); if (v && !*at) *at = i + 1; }
    return j.verdict;
}
static void unset_rules(void) { const char *k[] = { "DM_MALI_WHITE_N", "DM_MALI_FINE_N", "DM_MALI_WHITE_PCT", "DM_MALI_FINE_PCT", "DM_MALI_WHITE_EVERY_MS", "DM_MALI_WHITE_MIN_DRAWS", "DM_MALI_WHITE_MIN_W" };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) unsetenv(k[i]); }

static void t_judge(void) {
    unset_rules(); mw_rules r = mw_rules_default(); mw_rules_env(&r); int at;
    CHECK(r.white_n == 3 && r.fine_n == 5 && r.white_pct == 25 && r.fine_pct == 2 && r.every_ms == 3000 && r.min_draws == 200 && r.min_w == 1280, "default rules: 3 white / 5 fine in a row, 25 %% / 2 %%, every 3 s, 200 draws, 1280 wide");
    { int s[] = { 576, 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_WHITE && at == 3, "three white samples in a row: white at the third"); }
    { int s[] = { 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "two white samples: undecided"); }
    { int s[] = { 144, 144, 144, -1 }; CHECK(run(&r, s, &at) == MW_WHITE, "exactly 25 %% (144 of 576) counts as white"); }
    { int s[] = { 143, 143, 143, 143, 143, 143, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "just under 25 %% (143 of 576): neither, never decided"); }
    { int s[] = { 0, 0, 0, 0, 0, -1 }; CHECK(run(&r, s, &at) == MW_FINE && at == 5, "five fine samples in a row: fine at the fifth"); }
    { int s[] = { 11, 11, 11, 11, 11, -1 }; CHECK(run(&r, s, &at) == MW_FINE, "11 of 576 (1.9 %%) counts as fine"); }
    { int s[] = { 12, 12, 12, 12, 12, 12, 12, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "12 of 576 (2.08 %%) is not fine: undecided"); }
    { int s[] = { 576, 576, 0, 576, 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_WHITE && at == 6, "a fine sample breaks the white run: white only three later (sample 6)"); }
    { int s[] = { 0, 0, 0, 0, 576, 0, 0, 0, 0, 0, -1 }; CHECK(run(&r, s, &at) == MW_FINE && at == 10, "a white sample breaks the fine run: fine at sample 10"); }
    { int s[] = { 576, 576, 50, 576, 576, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "a sample in between (8.7 %%) breaks the white run too"); }
    { int s[] = { 0, 0, 0, 0, 50, 0, 0, 0, 0, -1 }; CHECK(run(&r, s, &at) == MW_UNDECIDED, "a sample in between breaks the fine run too"); }
    { int s[] = { 576, 576, 576, 0, 0, 0, 0, 0, 0, -1 }; CHECK(run(&r, s, &at) == MW_WHITE && at == 3, "a verdict stays: later fine samples do not change it"); }
    { mw_judge j = { 0 }; mw_judge_add(&j, &r, 0, 0); mw_judge_add(&j, &r, 0, 0); CHECK(j.samples == 0 && !j.verdict, "a sample of no texels is not a sample"); }
    { mw_judge j = { 0 }; for (int i = 0; i < 3; i++) mw_judge_add(&j, &r, 300, 400); CHECK(j.verdict == MW_WHITE && j.samples == 3, "any grid size: 300 of 400 three times is white"); }
    // the numbers from the environment (tests and tuning); bad values keep the defaults
    setenv("DM_MALI_WHITE_N", "1", 1); setenv("DM_MALI_FINE_N", "2", 1); setenv("DM_MALI_WHITE_PCT", "50", 1); setenv("DM_MALI_FINE_PCT", "10", 1);
    setenv("DM_MALI_WHITE_EVERY_MS", "0", 1); setenv("DM_MALI_WHITE_MIN_DRAWS", "7", 1); setenv("DM_MALI_WHITE_MIN_W", "64", 1);
    mw_rules e = mw_rules_default(); mw_rules_env(&e);
    CHECK(e.white_n == 1 && e.fine_n == 2 && e.white_pct == 50 && e.fine_pct == 10 && e.every_ms == 0 && e.min_draws == 7 && e.min_w == 64, "rules from the environment");
    { int s[] = { 300, -1 }; CHECK(run(&e, s, &at) == MW_WHITE && at == 1, "DM_MALI_WHITE_N=1, 50 %%: one sample of 300 / 576 is white"); }
    { int s[] = { 287, 287, -1 }; CHECK(run(&e, s, &at) == MW_UNDECIDED, "under 50 %% and over 10 %%: undecided"); }
    { int s[] = { 57, 57, -1 }; CHECK(run(&e, s, &at) == MW_FINE && at == 2, "DM_MALI_FINE_N=2, 10 %%: two samples of 57 / 576 are fine"); }
    setenv("DM_MALI_WHITE_N", "x", 1); setenv("DM_MALI_WHITE_PCT", "0", 1); setenv("DM_MALI_FINE_N", "", 1); setenv("DM_MALI_WHITE_MIN_W", "-5", 1);
    mw_rules b = mw_rules_default(); mw_rules_env(&b);
    CHECK(b.white_n == 3 && b.white_pct == 25 && b.fine_n == 5 && b.min_w == 1280, "bad or empty values keep the defaults (x, 0 %%, empty, -5)");
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

// ---- the ladder, start by start, through the file ----
static char D[256], F[300];
static mw_plan start(int driver, uint32_t ver, long app, mw_state *s) {   // a start: the file read, the plan made, the file written when it changed (as mw_init)
    int have = mw_state_load(F, s); mw_state before = *s; mw_plan p = mw_ladder_start(s, have, driver, ver, app);
    if (p.reset || memcmp(&before, s, sizeof before)) mw_state_save(F, s);
    return p;
}
static int verdict(int route, int v) { mw_state s; mw_state_load(F, &s); int ev = mw_ladder_verdict(&s, route, v); mw_state_save(F, &s); return ev; }
static void java_shows_offer(void) {   // MaliLadder.java: panvk=offer becomes panvk=shown once the dialog was shown (the other lines kept)
    char b[1024]; FILE *f = fopen(F, "r"); size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f);
    char *p = strstr(b, "panvk=offer"); if (p) memcpy(p, "panvk=shown", 11);
    f = fopen(F, "w"); fputs(b, f); fclose(f);
}
static const char *file(void) { static char b[1024]; FILE *f = fopen(F, "r"); if (!f) return ""; size_t k = fread(b, 1, sizeof b - 1, f); b[k] = 0; fclose(f); return b; }

static void t_ladder(void) {
    mw_state s; mw_plan p;
    unlink(F);
    p = start(ARM, 0xc401000, 96, &s);
    CHECK(p.reset && p.route == MW_ROUTE_A && p.sample && !p.offer, "first start (no file): route A, checked, the file made");
    CHECK(strstr(file(), "driver=9\n") && strstr(file(), "driver_version=0xc401000\n") && strstr(file(), "app=96\n") && strstr(file(), "A=-\n") && strstr(file(), "next=A\n"), "the file names the driver, its version, the app and the route");
    p = start(ARM, 0xc401000, 96, &s); CHECK(!p.reset && p.route == MW_ROUTE_A && p.sample, "a start without a verdict (undecided): the same again, nothing started over");
    CHECK(verdict(MW_ROUTE_A, MW_UNDECIDED) == MW_EV_NONE && !strstr(file(), "A=white"), "no verdict: nothing recorded");
    CHECK(verdict(MW_ROUTE_A, MW_WHITE) == MW_EV_TRY_B && strstr(file(), "A=white\n") && strstr(file(), "next=B\n"), "route A white: the second fix (B) at the next start");
    p = start(ARM, 0xc401000, 96, &s); CHECK(p.route == MW_ROUTE_B && p.sample && !p.offer && !p.exhausted, "next start: route B, checked");
    CHECK(verdict(MW_ROUTE_B, MW_WHITE) == MW_EV_OFFER_PANVK && strstr(file(), "B=white\n") && strstr(file(), "panvk=offer\n") && strstr(file(), "next=A (both white: unchecked)\n"),
          "route B white too: back to A, PanVK offered at the next start");
    p = start(ARM, 0xc401000, 96, &s); CHECK(p.route == MW_ROUTE_A && !p.sample && p.exhausted && p.offer, "next start: route A unchecked, the offer pending (the app shows it)");
    java_shows_offer();
    p = start(ARM, 0xc401000, 96, &s); CHECK(p.route == MW_ROUTE_A && !p.sample && p.exhausted && !p.offer && s.panvk == MW_PANVK_SHOWN, "after the app showed it: no second offer, nothing checked");
    p = start(ARM, 0xc401000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.sample && !p.offer && s.panvk == MW_PANVK_NONE && strstr(file(), "app=97\n") && strstr(file(), "A=-\nB=-\n"),
                                             "an app update (versionCode 97): started over at route A, checked");
    // the fine branch
    CHECK(verdict(MW_ROUTE_A, MW_FINE) == MW_EV_FINE && strstr(file(), "A=fine\n") && strstr(file(), "next=A (fine: unchecked)\n"), "route A fine: kept");
    p = start(ARM, 0xc401000, 97, &s); CHECK(p.route == MW_ROUTE_A && !p.sample && !p.reset, "next start: route A, no check any more (costs nothing)");
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.sample && strstr(file(), "driver_version=0xd801000\n"), "a driver update (r49p1 → r54p1): started over, checked");
    CHECK(verdict(MW_ROUTE_A, MW_WHITE) == MW_EV_TRY_B, "A white again");
    CHECK(verdict(MW_ROUTE_B, MW_FINE) == MW_EV_FINE && strstr(file(), "B=fine\n") && strstr(file(), "next=B (fine: unchecked)\n"), "route B fine: kept");
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.route == MW_ROUTE_B && !p.sample, "next start: route B, unchecked");
    p = start(20, 0xd801000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.sample, "another driver ID (20, Mesa PanVK, forced on by a test switch): started over");
    // the other orders (env.txt chose the route that ran)
    mw_state t; mw_state_reset(&t, ARM, 1, 1);
    CHECK(mw_ladder_verdict(&t, MW_ROUTE_B, MW_WHITE) == MW_EV_USE_A && mw_plan_of(&t).route == MW_ROUTE_A && mw_plan_of(&t).sample, "B white with A untried (B forced in env.txt): A next, checked");
    CHECK(mw_ladder_verdict(&t, MW_ROUTE_A, MW_WHITE) == MW_EV_OFFER_PANVK && t.panvk == MW_PANVK_OFFER && mw_plan_of(&t).exhausted, "then A white: both white, the offer");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_B] = MW_FINE;
    CHECK(mw_ladder_verdict(&t, MW_ROUTE_A, MW_WHITE) == MW_EV_USE_B && mw_plan_of(&t).route == MW_ROUTE_B && !mw_plan_of(&t).sample, "A white with B known fine: B, unchecked");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_A] = MW_FINE;
    CHECK(mw_ladder_verdict(&t, MW_ROUTE_B, MW_WHITE) == MW_EV_USE_A && mw_plan_of(&t).route == MW_ROUTE_A && !mw_plan_of(&t).sample, "B white with A known fine: A, unchecked");
    mw_state_reset(&t, ARM, 1, 1);
    CHECK(mw_ladder_verdict(&t, MW_ROUTE_NONE, MW_WHITE) == MW_EV_NONE && mw_ladder_verdict(&t, MW_ROUTE_BOTH, MW_WHITE) == MW_EV_NONE && !t.res[MW_ROUTE_A] && !t.res[MW_ROUTE_B],
          "a verdict with no route or both (env.txt set them so): recorded for neither");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_B] = MW_WHITE;
    CHECK(mw_plan_of(&t).route == MW_ROUTE_A && mw_plan_of(&t).sample, "B white, A untried: A, checked");
    mw_state_reset(&t, ARM, 1, 1); t.res[MW_ROUTE_A] = MW_WHITE; t.res[MW_ROUTE_B] = MW_WHITE; t.panvk = MW_PANVK_SHOWN;
    CHECK(mw_ladder_verdict(&t, MW_ROUTE_B, MW_WHITE) == MW_EV_OFFER_PANVK && t.panvk == MW_PANVK_SHOWN, "an offer already shown is not made again");
    // a damaged, edited or foreign file
    { FILE *f = fopen(F, "w"); fputs("garbage\n\x01\x02 =\nA=white\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.reset && p.route == MW_ROUTE_A && p.sample, "a file without the driver and app lines: started over");
    { FILE *f = fopen(F, "w"); fputs("# edited\nB=white\npanvk=shown\napp=97\nunknown=3\ndriver_version=0xd801000\nA=white\ndriver=9\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(!p.reset && p.exhausted && !p.offer && !p.sample && p.route == MW_ROUTE_A, "lines in any order, unknown ones left out: both white, the offer shown");
    { FILE *f = fopen(F, "w"); fputs("driver=9\ndriver_version=0xd801000\napp=97\nA=white\nB=white\n", f); fclose(f); }
    p = start(ARM, 0xd801000, 97, &s); CHECK(p.exhausted && p.offer && strstr(file(), "panvk=offer\n"), "both white without the panvk line (an older file): the offer made at the start");
    unlink(F);
    CHECK(!mw_state_save("", &s) && !mw_state_save(NULL, &s) && !mw_state_load(NULL, &s), "no path: nothing read or written (the ladder runs in memory)");
    { char p2[400]; snprintf(p2, sizeof p2, "%s/no/such/folder/mali_ladder.txt", D); CHECK(!mw_state_save(p2, &s), "a folder that is not there: the save fails, nothing crashes"); }
    // the format round trip
    mw_state a, b2; mw_state_reset(&a, ARM, 0xb001000, 123); a.res[MW_ROUTE_A] = MW_WHITE; a.res[MW_ROUTE_B] = MW_FINE; a.panvk = MW_PANVK_SHOWN; char buf[1024];
    mw_state_format(&a, buf, sizeof buf); CHECK(mw_state_parse(buf, &b2) && !memcmp(&a, &b2, sizeof a), "format → parse gives the same state");
    CHECK(strstr(buf, "next=B (fine: unchecked)\n") != NULL, "next= says what the next start does");
    mw_state_reset(&a, -1, 0x1000, 0); mw_state_format(&a, buf, sizeof buf); CHECK(mw_state_parse(buf, &b2) && b2.driver == -1 && b2.app == 0, "no driver ID (an old driver without driver properties): -1 round trips");
}

int main(void) {
    snprintf(D, sizeof D, "/tmp/t_mali_white.%d", (int)getpid()); mkdir(D, 0700); snprintf(F, sizeof F, "%s/mali_ladder.txt", D);
    t_judge(); t_enabled(); t_route(); t_ladder();
    unlink(F); rmdir(D);
    printf("%s t_mali_white: %d of %d checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
    return fails != 0;
}

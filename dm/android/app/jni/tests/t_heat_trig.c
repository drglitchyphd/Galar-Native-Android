// Heat trigger unit test (heat_trig.h): the readings players' logs show, one check every 5 s, against when the guard counts the phone as hot, with the rule
//   before 2026-11-24 (one reading) and now (three in a row of the thermal status, or of the headroom now with the status raised). Expected values are hand-written.
// Linux / WSL / macOS: cc -o /tmp/t_heat_trig t_heat_trig.c && /tmp/t_heat_trig
#include "../heat_trig.h"
#include <stdio.h>
#include <math.h>

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
typedef struct { int st; float hr, cs; } rd;
// the first check (0-based) at which the rule counts the phone as hot; -1: never
static int first_hot(const rd *r, int n, int saver, int sustain) {
    heat_trig h = { 0 }; int calm;
    for (int i = 0; i < n; i++) if (heat_check(&h, r[i].st, r[i].hr, r[i].cs, saver, sustain, &calm)) return i;
    return -1;
}

int main(void) {
    // Arceus #98 (RedMagic, cooled): status none in every log line, headroom 0.51-0.74, and Graphics Auto said "the phone is hot" between two of them: one reading
    //   of the forecast at 0.9 or more (the log shows only every 30 s)
    const rd a98[] = { {0, .70f, -1}, {0, .71f, -1}, {0, .73f, -1}, {0, .92f, -1}, {0, .74f, -1}, {0, .73f, -1}, {0, .68f, -1}, {0, .91f, -1}, {0, .67f, -1} };
    CHECK(first_hot(a98, 9, 0, 0) == 3, "Arceus #98, before: hot at the one forecast of 0.92 (the misfire)");
    CHECK(first_hot(a98, 9, 0, 1) == -1, "Arceus #98, now: never hot (status none)");
    // Arceus #90 (Infinix, PanVK: no kgsl limit): status 0; forecasts that cross 0.9 now and then, even two in a row
    const rd a90[] = { {0, .80f, -1}, {0, .93f, -1}, {0, .95f, -1}, {0, .85f, -1}, {0, .97f, -1}, {0, .88f, -1} };
    CHECK(first_hot(a90, 6, 0, 0) == 1, "Arceus #90, before: hot at 0.93");
    CHECK(first_hot(a90, 6, 0, 1) == -1, "Arceus #90, now: never hot (status 0)");
    // Arceus #92 (Galaxy A55, really hot): severe, then critical in every reading, headroom 0.96-1.13
    const rd a92[] = { {3, .96f, -1}, {3, 1.07f, -1}, {4, 1.13f, -1}, {4, 1.07f, -1}, {4, 1.06f, -1} };
    CHECK(first_hot(a92, 5, 0, 0) == 0, "Galaxy A55 (#92), before: hot at once");
    CHECK(first_hot(a92, 5, 0, 1) == 2, "Galaxy A55 (#92), now: hot at the third check (10 s later)");
    // status light with the headroom now past where throttling starts, three times: hot by headroom; with one reading under, the count starts over
    const rd lt[] = { {1, .96f, -1}, {1, .97f, -1}, {1, .94f, -1}, {1, .96f, -1}, {1, .98f, -1}, {1, 1.0f, -1} };
    CHECK(first_hot(lt, 6, 0, 1) == 5, "status light, headroom now 0.96 0.97 0.94 0.96 0.98 1.0: hot at the sixth (three in a row after the 0.94)");
    { heat_trig h = { 0 }; int calm, w = 0; for (int i = 0; i < 6; i++) w = heat_check(&h, lt[i].st, lt[i].hr, lt[i].cs, 0, 1, &calm);
      CHECK(w == HEAT_HEADROOM, "... and the reason is the headroom"); }
    // Galaxy S25+ held at 160 MHz by kgsl with the status light (2026-11-20): hot by the GPU limit after three checks
    const rd s25[] = { {1, .7f, .13f}, {1, .7f, .13f}, {1, .7f, .13f} };
    CHECK(first_hot(s25, 3, 0, 1) == 2, "S25+ GPU held at 13 % of its clock, status light: hot at the third check");
    CHECK(first_hot(s25, 3, 0, 0) == 0, "... at the first before");
    // battery saver: status light counts, three in a row
    const rd sv[] = { {1, .5f, -1}, {1, .5f, -1}, {1, .5f, -1} };
    CHECK(first_hot(sv, 3, 1, 1) == 2, "battery saver, status light: hot at the third check");
    // NaN headroom (asked twice in a second) is no reading: not hot, not calm by headroom alone either way
    { heat_trig h = { 0 }; int calm = -1; int w = heat_check(&h, 0, NAN, -1, 0, 1, &calm);
      CHECK(!w && calm == 1, "NaN headroom with status none: not hot, calm"); }
    // calm as before: status light, headroom under 0.75, the GPU's clock not held
    { heat_trig h = { 0 }; int calm = -1;
      heat_check(&h, 1, .7f, 1.0f, 0, 1, &calm); CHECK(calm == 1, "status light, headroom 0.70, GPU at its clock: calm");
      heat_check(&h, 1, .8f, 1.0f, 0, 1, &calm); CHECK(calm == 0, "headroom 0.80: not calm");
      heat_check(&h, 0, .5f, .8f, 0, 1, &calm); CHECK(calm == 0, "GPU held at 80 %: not calm"); }
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}

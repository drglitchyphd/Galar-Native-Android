// The touch-menu watcher's all-memory scans (touch_sched.h, fable-loads.md L2) on a fake clock: the watcher's loop (touch_menu.c watch_main: a look every 0.5 s)
//   driven with a pane count and a load flag, each all-memory scan reading 2 GB and taking 2 s (#112's phone: 1.7-2.5 s on the title screen), against the rule
//   before 2026-11-24 (a scan whenever none were known, every 12 s while under 2000). Expected values are hand-written.
// Linux / WSL / macOS: cc -O1 -Wall -o /tmp/t_touch_sched t_touch_sched.c && /tmp/t_touch_sched
#include "../touch_sched.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)
enum { MAXS = 4096 };
typedef struct { double at[MAXS]; int n; double gb; } scans;
// the watcher from t = 0 to `until` seconds; panes(t): what a scan at t finds; loading(t): a load on. old: the rule before
static void run(scans *o, double until, int (*panes)(double), int (*loading)(double), int old) {
    tsched s = { -1e9, 0 }; int found = 0, was_load = 0; double scan_t = -100; o->n = 0; o->gb = 0;
    for (double t = 0.5; t < until; t += 0.5) {
        if (!old && was_load && !loading(t)) tsched_load_over(&s);
        was_load = loading(t);
        int want = !found || t - scan_t > 12;   // (no menu open in these runs)
        if (!want) continue;
        int all = found < TS_ENOUGH;
        if (!all) { scan_t = t; continue; }   // (the kept mappings only: not counted, a few MB)
        if (!old && !tsched_ok(&s, t, loading(t), 0, 10)) continue;
        if (o->n < MAXS) o->at[o->n] = t;
        o->n++; o->gb += 2;
        found = panes(t); t += 2; scan_t = t; tsched_done(&s, t, found);
    }
}
static int none(double t) { (void)t; return 0; }
static int few(double t) { (void)t; return 900; }   // (the title screen: some panes, fewer than 2000)
static int field_at_40(double t) { return t >= 40 ? 5200 : 300; }
static int no_load(double t) { (void)t; return 0; }
static int load_20_30(double t) { return t >= 20 && t < 30; }
static int max_in_window(const scans *o, double from, double w) {
    int best = 0; for (int i = 0; i < o->n; i++) { if (o->at[i] < from) continue; int k = 0; for (int j = i; j < o->n && o->at[j] < o->at[i] + w; j++) k++; if (k > best) best = k; }
    return best;
}

int main(void) {
    scans a, b;
    run(&a, 600, none, no_load, 1); run(&b, 600, none, no_load, 0);
    int after_first = 0; for (int i = 1; i < b.n; i++) if (b.at[i] <= b.at[0] + 2 + 60) after_first++;
    CHECK(after_first <= 2, "no panes: %d all-memory scans in the 60 s after the first (rule before: %d)", after_first, max_in_window(&a, 0, 60));
    CHECK(max_in_window(&b, 60, 60) <= 2, "no panes: at most 2 in any later minute (%d)", max_in_window(&b, 60, 60));
    printf("     no panes for 10 minutes: %.0f GB read (rule before: %.0f GB)\n", b.gb, a.gb);
    CHECK(b.gb * 10 < a.gb, "no panes: a tenth of the reads or less");
    run(&a, 600, few, no_load, 1); run(&b, 600, few, no_load, 0);
    CHECK(max_in_window(&b, 60, 60) <= 2 && a.n > b.n * 2, "900 panes (title screen): %d scans in 10 minutes (rule before: %d)", b.n, a.n);
    run(&b, 120, field_at_40, no_load, 0);
    int late = 0; for (int i = 0; i < b.n; i++) if (b.at[i] >= 40) late++;
    CHECK(late >= 1 && b.at[b.n - 1] < 40 + 42, "the field's panes found within 42 s of them appearing (last all-memory scan at %.1f s)", b.at[b.n - 1]);
    run(&b, 60, none, load_20_30, 0);
    int in_load = 0; for (int i = 0; i < b.n; i++) if (b.at[i] >= 20 && b.at[i] < 30) in_load++;
    CHECK(!in_load, "no all-memory scan while a load is on (%d)", in_load);
    int next = -1; for (int i = 0; i < b.n; i++) if (b.at[i] >= 30) { next = i; break; }
    CHECK(next >= 0 && b.at[next] <= 30.5, "the first all-memory scan right after the load (at %.1f s, the load ended at 30 s)", next >= 0 ? b.at[next] : -1);
    tsched s = { -1e9, 0 };
    CHECK(tsched_ok(&s, 0, 1, 1, 10), "DM_TOUCH_SCAN_LOAD=1: allowed in a load");
    tsched_done(&s, 0, 100); tsched_done(&s, 10, 100); tsched_done(&s, 30, 100);
    CHECK(tsched_gap(&s, 10) == 40 && !tsched_ok(&s, 69, 0, 0, 10) && tsched_ok(&s, 70, 0, 0, 10), "back-off 10 / 20 / 40 s");
    tsched_done(&s, 70, 3000);
    CHECK(tsched_gap(&s, 10) == 10, "back to 10 s once a scan finds enough panes");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

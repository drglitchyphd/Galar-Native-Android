// When the touch-menu watcher (touch_menu.c watch_main) may read all of the process's memory for the game's menu panes (fable-loads.md L2, 2026-11-24).
//   It scanned whenever fewer than 2000 panes were known, with no limit at all while none were: a load destroys the layouts, so the scan landed in the load
//   (#112: one scan of 6878 ms inside a 10.1 s load, dm-touch 5.29 s of CPU; a 1.7-2.5 s scan every 12 s on the title screen). Now: none while a load is on
//   (hle_nvn.c dm_loading), and after a scan that found fewer than TS_ENOUGH panes the next waits base, then 2 x base, then 4 x base (10 / 20 / 40 s by
//   default), back to base once one finds enough. A scan a tap asks for (touch_menu.c full_scan, its own 10 s limit) counts here too. Plain C, no state of its own
#pragma once

enum { TS_ENOUGH = 2000 };
typedef struct { double last; int poor; } tsched;   // the last all-memory scan (seconds; set to -1e9 before the first), scans in a row that found too few panes

static inline double tsched_gap(const tsched *s, double base) { double g = base; for (int i = 1; i < s->poor && i < 3; i++) g *= 2; return g; }
// 1: the watcher may start an all-memory scan now (in_load: allowed in a load anyway, DM_TOUCH_SCAN_LOAD=1)
static inline int tsched_ok(const tsched *s, double now, int loading, int in_load, double base) {
    if (loading && !in_load) return 0;
    return now - s->last >= tsched_gap(s, base);
}
static inline void tsched_done(tsched *s, double now, int panes) { s->last = now; s->poor = panes < TS_ENOUGH ? s->poor + 1 : 0; }
// a load just ended: the game builds the next screen's layouts now, so the next scan may start at once and the back-off starts over
static inline void tsched_load_over(tsched *s) { s->last = -1e9; s->poor = 0; }

// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}
// The game's frame grid on whole refreshes of the display (perf-1124 G3'; hle_nvn.c present_hold, DM_PACE_GRID=display: not the default, see there). period: the game's (2 / 60 s at 30 fps); refresh: the display's
//   measured refresh (nvn_vk.c dm_display_refresh_ns, 0: unknown). On a 59.93 Hz panel the game handed over every 33.333 ms and the display showed every 33.37:
//   the two slid a refresh apart about every 15 s, and near the edge frames flipped between two vsyncs. Only for a display whose refresh, or a whole multiple of
//   it, is within 10 % of 1/60 s (60 and 120 Hz and their near values, as hle_sdk16.c's vsync timer); a 90 Hz panel keeps the game's grid
static inline uint64_t nvn_pace_grid(uint64_t period, uint64_t refresh) {
    const uint64_t base = 16666667ull;
    if (!period || !refresh) return period;
    uint64_t m = (base + refresh / 2) / refresh, p = (m ? m : 1) * refresh;
    if (p <= base * 9 / 10 || p >= base * 11 / 10) return period;
    uint64_t k = (period + p / 2) / p;
    return k ? k * p : period;
}

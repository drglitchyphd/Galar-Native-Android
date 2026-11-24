// A loading screen told by the picture (fable-loads.md L1 / I1, 2026-11-24). android_main.c used to call it a load while the game read 8 MB/s or more of its files,
//   until reads stayed under 1 MB/s for 2 s; open-world streaming met that too: the "12.03 s load" of Arceus #98 was ordinary play (430-530 draws a frame, 600
//   frames per 10 s, 19 MB read), while the real loading screens in the logs read 140-420 MB with 20-40 draws a frame. Here a load is reads of 8 MB/s or more
//   AND fewer than LD_DRAWS draws in each of the last LD_PRESENTS presents; it ends at the first present with LD_DRAWS draws or more. Two additions to that rule:
//   reads under 1 MB/s for LD_QUIET_NS with the picture still sparse end it as well (a menu with few draws is not a load once the reads stop), and a picture
//   that stops presenting for LD_FROZEN_NS while the reads go on is a load too (the game loading on its main thread: no present calls the detector then, so
//   load_det_poll, from android_main.c's 500 ms poll, looks). Plain C, no locks: the caller serialises the calls (hle_nvn.c holds a mutex)
#pragma once
#include <stdint.h>

enum { LD_DRAWS = 60, LD_PRESENTS = 15, LD_RING = 16 };
#define LD_MBS_ON 8.0             // MB/s of file reads that can begin a load
#define LD_MBS_QUIET 1.0          // under this the reads count as quiet
#define LD_QUIET_NS 3000000000ull // quiet this long with a sparse picture: over
#define LD_FROZEN_NS 1000000000ull // no present this long with reads going on: a load
#define LD_STEP_NS 100000000ull   // the read history keeps one sample per 100 ms (16 of them: 1.6 s)
#define LD_WIN_NS 500000000ull    // the read rate is taken over the last 0.5 s or a little more

typedef struct {
    uint64_t rt[LD_RING], rb[LD_RING]; unsigned rn;   // read history: (time, bytes read so far), one sample per LD_STEP_NS at most
    int low;               // presents in a row with fewer than LD_DRAWS draws
    int on;                // a load is on
    uint64_t since;        // when it began
    uint64_t quiet;        // since when the reads are quiet during it (0: not quiet)
    uint64_t last_present; // the last present's time (0: none yet)
} load_det;

static inline void ld_sample(load_det *d, uint64_t t, uint64_t b) {
    if (d->rn && t < d->rt[(d->rn - 1) % LD_RING] + LD_STEP_NS) return;
    d->rt[d->rn % LD_RING] = t; d->rb[d->rn % LD_RING] = b; d->rn++;
}
// MB/s read over the last LD_WIN_NS or a little more (the newest sample at least that old, else the oldest kept); 0 with no history
static inline double load_det_rate(const load_det *d, uint64_t t, uint64_t b) {
    unsigned n = d->rn < LD_RING ? d->rn : LD_RING; int pick = -1;
    for (unsigned k = 0; k < n; k++) { unsigned i = (d->rn - 1 - k) % LD_RING; if (d->rt[i] + LD_WIN_NS <= t) { pick = (int)i; break; } }
    if (pick < 0 && n) pick = (int)((d->rn - n) % LD_RING);
    if (pick < 0 || t <= d->rt[pick] + LD_STEP_NS / 2 || b < d->rb[pick]) return 0;
    return (double)(b - d->rb[pick]) / 1048576.0 / ((double)(t - d->rt[pick]) / 1e9);
}
static inline int ld_quiet(load_det *d, uint64_t t, double mbs) {   // 1: over by the quiet rule
    if (mbs >= LD_MBS_QUIET) { d->quiet = 0; return 0; }
    if (!d->quiet) d->quiet = t;
    return t - d->quiet >= LD_QUIET_NS;
}
// one present with this many draws, at time t, with b bytes read so far: +1 a load began, -1 it ended, 0 no change
static inline int load_det_present(load_det *d, uint64_t t, uint64_t b, uint64_t draws) {
    double mbs = load_det_rate(d, t, b); ld_sample(d, t, b); d->last_present = t;
    d->low = draws < LD_DRAWS ? d->low + 1 : 0;
    if (!d->on) {
        if (d->low >= LD_PRESENTS && mbs >= LD_MBS_ON) { d->on = 1; d->since = t; d->quiet = 0; return 1; }
        return 0;
    }
    if (draws >= LD_DRAWS || ld_quiet(d, t, mbs)) { d->on = 0; d->quiet = 0; return -1; }
    return 0;
}
// a look without a present (android_main.c's poll, every 500 ms): the picture frozen while the game reads is a load; the quiet rule ends one
static inline int load_det_poll(load_det *d, uint64_t t, uint64_t b) {
    double mbs = load_det_rate(d, t, b); ld_sample(d, t, b);
    int frozen = d->last_present && t >= d->last_present + LD_FROZEN_NS;
    if (!d->on) {
        if (frozen && mbs >= LD_MBS_ON) { d->on = 1; d->since = t; d->quiet = 0; return 1; }
        return 0;
    }
    if (frozen && ld_quiet(d, t, mbs)) { d->on = 0; d->quiet = 0; return -1; }
    return 0;
}

// The frame pacing of a load (L1, DM_LOAD_PACE): game = the game's own interval, as on the console (today's behaviour); display = one refresh of the display
//   per frame; free = no hold. The hold the next acquire waits for, given the pacer's slot for this frame, the game's period and one refresh
enum { LP_GAME = 0, LP_DISPLAY = 1, LP_FREE = 2 };
static inline uint64_t load_pace_period(int mode, int loading, uint64_t period, uint64_t refresh) {
    if (!loading || mode == LP_GAME || !period) return period;
    if (mode == LP_FREE) return 0;
    return refresh && refresh < period ? refresh : period;   // (never longer than the game's own)
}

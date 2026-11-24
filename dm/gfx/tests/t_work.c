// A frame's work for the ADPF hint, the fps guard and the frame step (nvn/nvn_work.h, perf-1124 G2') against a fake clock: the presenting thread's frames as the
//   Galaxy S25+ ran Shield's field (perf2-run1 / run2: 33.3 ms between presents, 20-25 ms of it asleep in the acquire, 1-6 ms in sync waits, 5-8 ms of work),
//   a phone whose replay thread is the limit (#324: game 12 ms, replay 33 ms), a load, and the switch DM_HINT_WORK=wall.
// Plain C, any OS: make t_work && ./t_work
#include "../nvn/nvn_work.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MS 1000000ull

// the presenting thread over n frames: each frame works `work` ms in pieces around an acquire sleep and a sync wait; the clock and the thread's running totals
//   advance as hle_nvn.c sees them (t_acq_ns / t_sync_ns), and the present computes the work from the time since the last present returned
typedef struct { uint64_t clock, sync, acq, prev_out; nvn_work_snap snap; nvn_replay_work rw; } thr;
static uint64_t frame(thr *t, uint64_t work, uint64_t acq, uint64_t sync, uint64_t replay, int wall_mode, uint64_t *wall_out) {
    t->clock += acq; t->acq += acq;          // the acquire's pacing sleep
    t->clock += work / 2;                    // the game's code
    t->clock += sync; t->sync += sync;       // a sync wait (W1)
    t->clock += work - work / 2;
    uint64_t wall = t->prev_out ? t->clock - t->prev_out : 0;
    uint64_t own = nvn_work_own(&t->snap, wall, t->sync, t->acq);
    // the replay thread's frame: three submissions, the second waiting 2 ms for a free submission slot, then the present
    nvn_replay_item(&t->rw, replay / 3, 0); nvn_replay_item(&t->rw, replay / 3 + 2 * MS, 2 * MS); nvn_replay_item(&t->rw, replay - 2 * (replay / 3), 0);
    uint64_t r = nvn_replay_present(&t->rw);
    t->clock += MS / 2; t->prev_out = t->clock;   // (the present itself)
    if (wall_out) *wall_out = wall;
    return t->prev_out == t->clock && wall ? nvn_work_frame(wall_mode, wall, own, r) : 0;
}
int main(void) {
    // S25+ field: 6 ms of work, 23 ms of acquire sleep, 3.8 ms of sync wait (the present itself 0.5 ms, outside the span): the hint and the guard get the 6 ms, not the 33.3
    { thr t = { 0 }; uint64_t w = 0, wall = 0;
      for (int i = 0; i < 10; i++) w = frame(&t, 6 * MS, 23 * MS, 3800000, 0, 0, &wall);
      CHECK(wall > 32 * MS && wall < 34 * MS, "S25+ field: %.1f ms between presents (the paced frame)", wall / 1e6);
      CHECK(w == 6 * MS, "S25+ field: the frame's work is %.1f ms (the game's 6), not the %.1f between presents", w / 1e6, wall / 1e6);
      uint64_t ww = frame(&t, 6 * MS, 23 * MS, 3800000, 0, 1, &wall);
      CHECK(ww == wall, "DM_HINT_WORK=wall: the whole %.1f ms between presents, as before", ww / 1e6); }
    // the replay thread the limit: game 12 ms, replay 33 ms (its slot wait left out): the frame's work is the replay's
    { thr t = { 0 }; uint64_t w = 0;
      for (int i = 0; i < 10; i++) w = frame(&t, 12 * MS, 10 * MS, 11 * MS, 33 * MS, 0, NULL);
      CHECK(w == 33 * MS, "replay thread the limit (#324): the frame's work is the replay's %.1f ms (want 33.0: its 2 ms slot wait left out)", w / 1e6); }
    // a load: 2.6 % running, the rest acquire sleep and sync waits (perf2-run2's 3.1 s load, 92 frames): under a millisecond of work
    { thr t = { 0 }; uint64_t w = 0;
      for (int i = 0; i < 92; i++) w = frame(&t, MS / 2, 9 * MS, 24 * MS, 0, 0, NULL);
      CHECK(w <= 1100000, "a load's frame: %.2f ms of work, not 33", w / 1e6); }
    // the first present: no time before it, nothing reported; waits before the first present are not taken out of the second
    { nvn_work_snap s = { 0 }; uint64_t o = nvn_work_own(&s, 0, 500 * MS, 900 * MS);
      CHECK(o == 0 && s.primed, "first present: nothing to measure, the counters noted");
      o = nvn_work_own(&s, 33 * MS, 504 * MS, 920 * MS);
      CHECK(o == 9 * MS, "second present: 33 - 4 - 20 = %.1f ms", o / 1e6);
      o = nvn_work_own(&s, 10 * MS, 520 * MS, 920 * MS);
      CHECK(o == 0, "waits longer than the time between presents (counted on a frame boundary): 0, never negative"); }
    printf("%s: %d of %d checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
    return fails != 0;
}

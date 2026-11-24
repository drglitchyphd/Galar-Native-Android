// A frame's work as the ADPF hint, the fps guard and the frame step are told it (perf-1124 G2'). Pure functions, no clock of their own: tests/t_work.c runs them
//   against a fake one.
// The presenting thread's time from one present to the next (the wall) holds the pacing sleep in the image acquire and the game's sync waits: on a Galaxy S25+ in
//   Shield's field 61-77 % of the main thread asleep in the acquire, 3-4 % in nvnSyncWait, 4-8 % running game code (perf2-run1 / run2), so "frame work" read
//   33 ms for 5-8 ms of work. Android got 33 ms against a 25 ms target every frame and kept the clocks up; the fps guard saw the CPU over 85 % of the budget
//   whenever the GPU was under 23 ms (no render scale step: "the CPU is the limit") and never under half of it (lock-30-lite never came back).
// Now: the wall minus the acquire's measured sleep and the sync waits, both on the presenting thread (nvn_work_own); with the replay on its own thread (Faster
//   rendering) the frame's work is the longer of that and the replay thread's own work for its last frame (its items' time without the waits for a free
//   submission slot, nvn_replay_*), so a phone whose replay is the limit is still seen as CPU-bound. DM_HINT_WORK=wall: the wall, as before
#pragma once
#include <stdint.h>

typedef struct { uint64_t sync, acq; int primed; } nvn_work_snap;   // the presenting thread's wait counters at its last present
// wall: the time since the last present returned; sync / acq: the thread's running totals of sync waits and acquire sleeps (never reset). Returns the work
static inline uint64_t nvn_work_own(nvn_work_snap *s, uint64_t wall, uint64_t sync, uint64_t acq) {
    uint64_t waits = s->primed ? (sync - s->sync) + (acq - s->acq) : 0;
    s->sync = sync; s->acq = acq; s->primed = 1;
    return wall > waits ? wall - waits : 0;
}
// The figure the hint and the guard get: the wall (DM_HINT_WORK=wall), or the longer of the game's own work and the replay thread's
static inline uint64_t nvn_work_frame(int wall_mode, uint64_t wall, uint64_t own, uint64_t replay) {
    if (wall_mode) return wall;
    return own > replay ? own : replay;
}
// The replay thread's work: each item's time without its waits for a free submission slot (the GPU); a present closes the frame
typedef struct { uint64_t acc; } nvn_replay_work;
static inline void nvn_replay_item(nvn_replay_work *r, uint64_t took, uint64_t slot_wait) { r->acc += took > slot_wait ? took - slot_wait : 0; }
static inline uint64_t nvn_replay_present(nvn_replay_work *r) { uint64_t w = r->acc; r->acc = 0; return w; }

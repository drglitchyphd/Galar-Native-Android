// Whether a load's busy threads are pinned to the fast cores or only raised (android_main.c load_boost, fable-loads.md L3, 2026-11-24). 5dc27d5 (v0.95-pre) pinned
//   every busy thread to the cores whose top clock is 70 % of the fastest's or more; on a 1+3+4 chip whose little cores fall under that, a load's 9-11 runnable
//   threads (main, render, 4-6 job threads, dm-replay, dm-texupload ...) then had 4 cores where the scheduler had 8. Now they are pinned only when they fit: busy
//   threads boosted in all plus the threads pinned already (the hint session's, pin_update) at most the fast cores; otherwise their priority is raised (nice -4)
//   and the scheduler keeps every core. On a chip without slow cores a mask changes nothing, so none is set. DM_LOAD_BOOST=pin: always pin (the rule before),
//   nice: never pin, 0: no boost at all. Plain C
#pragma once

enum { BOOST_AUTO = 0, BOOST_PIN = 1, BOOST_NICE = 2 };
// the fast cores of a core table (each core's top clock in kHz, 0 = no such core): 70 % of the fastest or more
static inline int boost_fast_cores(const long *khz, int n, int *all) {
    long top = 0; int k = 0, a = 0;
    for (int c = 0; c < n; c++) if (khz[c] > top) top = khz[c];
    for (int c = 0; c < n; c++) if (khz[c]) { a++; if (khz[c] * 10 >= top * 7) k++; }
    if (all) *all = a;
    return k;
}
// 1: pin the busy threads to the fast cores; 0: raise their priority only
static inline int boost_pin(int mode, int fast, int all, int busy, int pinned) {
    if (mode == BOOST_PIN) return fast > 0 && fast < all;
    if (mode == BOOST_NICE || fast <= 0 || fast >= all) return 0;
    return busy + pinned <= fast;
}

// Present-time pacing (nvn/vk_pace.h) against a simulated compositor: counts the uneven frames (shown more than half a refresh off the frame interval, as
//   nvn_vk.c pace_time's "frame timing" line counts them) with the times asked as before (vkp_want_legacy) and now (vkp_want), at an unchanged mean.
// The model, per frame: the game's own timer ticks every interval (it follows the display's refresh, hle_sdk16.c, at a phase of its own); the game's work then
//   takes work..work+jit ms and the frame is handed over (pace_time's now); the GPU draws it from then (or from the end of the frame before) in gpu +- gjit ms;
//   the compositor shows it at the first vsync that is no earlier than the time asked (give or take 0.5 ms: where SurfaceFlinger compares it) and that comes at
//   least `latch` after the picture was finished, never two frames at one vsync. The display's record (shown, asked) reaches the pacer 2 refreshes after the
//   frame was shown, with a fixed delay of its own added (the compositor's: `disp`), and margin 0 (Android's swapchain reports none).
// Hitches (2026-11-24, the S25+ logs of perf-all): a frame's work can run long (hitch: one frame of hitch ms every `every` frames), after which the game starts its
//   next frame at once and keeps its interval from there (its timer re-phased); a load replays the frames of the S25+ field load at 322 s (load_trace: the time
//   from one frame to the next and its GPU time, as [longframe] logged them). gwait: the game waits for the GPU's frame before (the hot run, GPU 50 ms a frame).
//   Those scenarios count the grid's moves in 10 s with the hold (vkp_want as now) and without it (DM_PACE_HOLD=0, PR #361 as first pushed)
// One grid (perf-1124 G3'): a 59.93 Hz panel against the game's own 60.000 Hz hand-over grid, before and with the game on the display's grid and vkp_want_grid.
// Plain C, any OS: cc -O2 -o t_vk_pace t_vk_pace.c && ./t_vk_pace
#include "../nvn/vk_pace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (double)(rng >> 11) / 9007199254740992.0; }
#define MS 1000000.0

typedef struct { const char *name; double r, gpu, gjit, work, jit, latch, disp; int nots; int every; double hitch; int load, gwait; double giv; } scen;   // nots: the driver gives no GPU time; giv: the game's own hand-over interval (ms; 0: the display's, iv)
typedef struct { long frames, uneven; double mean_iv, lat, secs, mean_all; unsigned rephase; int extra, wmax; } res;   // mean_all: pauses counted in; wmax: the most uneven frames in one 300-frame window
// the S25+ field load at 322 s (perf-all-cool.txt, frames 9368-9414): time to the next frame and GPU time, ms
static const double load_trace[][2] = { { 51.6, 3.3 }, { 30.5, 24.3 }, { 19.8, 42.0 }, { 57.8, 71.6 }, { 72.3, 26.1 }, { 113.3, 23.4 }, { 143.6, 23.6 }, { 65.4, 32.3 },
    { 75.9, 35.2 }, { 96.8, 48.8 }, { 75.2, 44.6 }, { 52.9, 49.1 }, { 52.3, 32.5 }, { 35.9, 32.2 }, { 35.4, 27.7 }, { 30.8, 20.3 }, { 23.7, 18.8 }, { 32.2, 18.8 } };
enum { LOAD_N = sizeof load_trace / sizeof *load_trace };

// one run of n frames; mode 0: as before (lead one interval), 1: now, 2: now without the hold (DM_PACE_HOLD=0)
static res run(const scen *s, int mode, int n, double phase) {
    uint64_t r = (uint64_t)(s->r * MS), iv = ((uint64_t)(33333333.0 + r / 2) / r) * r;
    uint64_t giv = s->giv > 0 ? (uint64_t)(s->giv * MS) : iv; int wn = 0;   // (giv: the game's hand-over grid; wn: uneven frames in this 300-frame window)
    vk_pacer P; memset(&P, 0, sizeof P); P.hold_off = mode == 2;
    uint64_t vs0 = 1000000000ull + (uint64_t)(urand() * r), t0 = 2000000000ull + (uint64_t)(phase * iv);
    enum { Q = 64 }; struct { uint64_t asked, shown, at; int used; } q[Q]; memset(q, 0, sizeof q);
    uint64_t gpu_free = 0, last_shown = 0, prev_actual = 0; res o = { 0 }; double sum = 0, sall = 0; long nsum = 0, nall = 0;
    uint64_t start = t0, prev_now = 0; int li = -1;   // the game's frame start (its timer: an interval after the last, or at once after a frame that ran over); a load's frame
    for (int i = 0; i < n; i++) {
        if (i) { start += giv; if (prev_now > start) start = prev_now; }
        if (s->load && i >= 120 && i % s->every == 0) li = 0;
        uint64_t now = start + (uint64_t)((s->work + urand() * s->jit) * MS);
        if (s->every && !s->load && i >= 120 && i % s->every == 0) now = start + (uint64_t)(s->hitch * MS);
        if (li >= 0) { now = prev_now + (uint64_t)(load_trace[li][0] * MS); start = now; }
        if (s->gwait && gpu_free + 2000000 > now) now = gpu_free + 2000000;   // (the game waits for the frame before to be drawn, then submits)
        prev_now = now;
        for (int k = 0; k < Q; k++) if (q[k].used && q[k].at <= now) {   // the display's record so far
            if (mode == 3) P.vs = q[k].shown; else if (mode) vkp_seen(&P, q[k].asked, q[k].shown, 0, now, r);   // (3: pace_time sets the phase from every frame shown)
            q[k].used = 0; }
        uint64_t g = (uint64_t)((s->gpu + (urand() * 2 - 1) * s->gjit) * MS);
        if (li >= 0) { g = (uint64_t)(load_trace[li][1] * MS); if (++li == LOAD_N) li = -1; }
        if (mode && mode != 3 && i >= 2 && !s->nots) vkp_gpu(&P, g);   // (pace_time reads the GPU time of frames already done; the model's is steady enough that this one's stands in)
        uint64_t want = mode == 3 ? vkp_want_grid(&P, now, r, iv, iv) : mode ? vkp_want(&P, now, r, iv) : vkp_want_legacy(&P, now, r, iv, iv);
        uint64_t start = now > gpu_free ? now : gpu_free, ready = start + g; gpu_free = ready;
        // the asked time as the compositor compares it with a vsync: give or take 0.5 ms
        uint64_t w = want ? (uint64_t)((double)want + (urand() - 0.5) * MS) : 0;
        uint64_t earliest = ready + (uint64_t)(s->latch * MS); if (w > earliest) earliest = w;
        uint64_t k = earliest > vs0 ? (earliest - vs0 + r - 1) / r : 0, v = vs0 + k * r;
        if (last_shown && v <= last_shown) v = last_shown + r;
        last_shown = v;
        uint64_t actual = v + (uint64_t)(s->disp * MS);
        for (int j = 0; j < Q; j++) if (!q[j].used) { q[j].used = 1; q[j].asked = want; q[j].shown = actual; q[j].at = actual + 2 * r; break; }
        if (i >= 120) {   // (the first 4 s: the grid settles)
            o.lat += (v - now) / MS;
            if (prev_actual && actual - prev_actual <= 4 * iv) {   // (longer: a pause, left out as pace_time's log does)
                uint64_t d = actual - prev_actual; o.frames++; sum += d; nsum++; if (d + r / 2 < iv || d > iv + r / 2) { o.uneven++; wn++; } }
            if (prev_actual) { sall += actual - prev_actual; nall++; }
            if ((i - 120) % 300 == 299) { if (wn > o.wmax) o.wmax = wn; wn = 0; } }
        prev_actual = actual;
    }
    o.secs = (prev_now - t0) / 1e9; o.mean_all = nall ? sall / nall / MS : 0; o.mean_iv = nsum ? sum / nsum / MS : 0; o.lat = o.frames ? o.lat / o.frames : 0; o.rephase = P.rephase; o.extra = P.extra;
    return o;
}

int main(void) {
    // S25+ (Adreno 830): 60 and 120 Hz, GPU 18-28 ms (the Arceus figures); POCO F6 (Adreno 735): GPU 25-30 ms; a light frame; a GPU without timestamps is the
    //   same model with vkp_gpu never fed (Mali / PowerVR: Q_GPU_TIME_OFF), the display's record alone then moves the lead
    const scen S[] = {
        { "S25+ 60 Hz, GPU 18 ms",      16.667, 18.4, 3, 7, 5, 16.667, 16.667, 0 },
        { "S25+ 60 Hz, GPU 26 ms",      16.667, 25.6, 3, 7, 5, 16.667, 16.667, 0 },
        { "S25+ 120 Hz, GPU 28 ms",      8.333, 28.2, 3, 7, 5, 16.667, 8.333, 0 },
        { "S25+ 120 Hz, GPU 18 ms",      8.333, 18.4, 3, 7, 5, 16.667, 8.333, 0 },
        { "POCO F6 60 Hz, GPU 28 ms",   16.667, 28.0, 4, 10, 8, 16.667, 16.667, 0 },
        { "POCO F6 120 Hz, GPU 30 ms",   8.333, 30.0, 3, 10, 8, 16.667, 8.333, 0 },
        { "light frame 60 Hz, GPU 8 ms", 16.667, 8.0, 2, 5, 4, 16.667, 16.667, 0 },
        { "latch 2 refreshes, 60 Hz",   16.667, 20.0, 3, 7, 5, 33.333, 16.667, 0 },
        { "no GPU time (Mali) 60 Hz, 25 ms", 16.667, 25.0, 4, 10, 8, 16.667, 16.667, 1 },
    };
    int ns = sizeof S / sizeof *S, fail = 0; const int N = 3000, PH = 12;
    printf("%-30s | %-24s | %-24s | %s\n", "scenario (30 fps)", "before: uneven / mean", "now: uneven / mean", "delay before -> now, grid moves, lead +refreshes");
    for (int i = 0; i < ns; i++) {
        res a = { 0 }, b = { 0 }; double ma = 0, mb = 0, la = 0, lb = 0; unsigned rp = 0; int ex = 0;
        for (int ph = 0; ph < PH; ph++) {   // the game's timer at 12 phases against the display's vsync
            res x = run(&S[i], 0, N, ph / (double)PH), y = run(&S[i], 1, N, ph / (double)PH);
            a.frames += x.frames; a.uneven += x.uneven; ma += x.mean_iv; la += x.lat;
            b.frames += y.frames; b.uneven += y.uneven; mb += y.mean_iv; lb += y.lat; rp += y.rephase; if (y.extra > ex) ex = y.extra; }
        double ua = 100.0 * a.uneven / a.frames, ub = 100.0 * b.uneven / b.frames;
        printf("%-30s | %6.2f %% / %6.2f ms      | %6.2f %% / %6.2f ms      | %5.1f -> %5.1f ms, %u, +%d\n", S[i].name, ua, ma / PH, ub, mb / PH, la / PH, lb / PH, rp / PH, ex);
        // now: at most 2 % uneven, the mean still the interval, never worse than before, at most an interval and two refreshes more delay
        int ok = ub <= 2.0 && ub <= ua + 0.01 && mb / PH > 33.0 && mb / PH < 33.8 && lb / PH - la / PH <= 33.4 + 2 * S[i].r;
        if (!ok) { printf("FAIL: %s\n", S[i].name); fail++; }
    }
    // Hitches and an overloaded GPU (the S25+ logs of 2026-11-24): grid moves in 10 s without the hold (DM_PACE_HOLD=0) and with it; uneven and mean as above.
    //   (uneven: frames handed over unevenly are counted too, a load's own; mean: of all intervals, pauses in). With the hold: at most half the moves (at 120 Hz the grid
    //   still moves once after the grace period when the game came back early), uneven at most 1 % more, the mean at most 0.1 ms longer
    const scen H[] = {
        { "S25+ 60 Hz, a 140 ms frame / 10 s", 16.667, 21.0, 3, 7, 5, 16.667, 16.667, 0, 300, 140, 0, 0 },
        { "S25+ 60 Hz, a 140 ms frame / 2 s",  16.667, 21.0, 3, 7, 5, 16.667, 16.667, 0,  60, 140, 0, 0 },
        { "S25+ 120 Hz, a 140 ms frame / 10 s", 8.333, 21.0, 3, 7, 5, 16.667, 8.333, 0, 300, 140, 0, 0 },
        { "S25+ 60 Hz, field load / 10 s",     16.667, 21.0, 3, 7, 5, 16.667, 16.667, 0, 300, 0, 1, 0 },
        { "S25+ 120 Hz, field load / 10 s",     8.333, 21.0, 3, 7, 5, 16.667, 8.333, 0, 300, 0, 1, 0 },
        { "S25+ 60 Hz hot, GPU 50 ms",         16.667, 50.0, 3, 7, 5, 16.667, 16.667, 0, 0, 0, 0, 1 },
        { "S25+ 120 Hz hot, GPU 50 ms",         8.333, 50.0, 3, 7, 5, 16.667, 8.333, 0, 0, 0, 0, 1 },
        { "POCO F6 60 Hz hot, GPU 38 ms",      16.667, 38.0, 4, 10, 8, 16.667, 16.667, 0, 0, 0, 0, 1 },
    };
    int nh = sizeof H / sizeof *H;
    printf("\n%-34s | %-24s | %-24s | %s\n", "hitches / GPU over (30 fps)", "no hold: uneven / mean", "hold: uneven / mean", "grid moves in 10 s: no hold -> hold");
    for (int i = 0; i < nh; i++) {
        res a = { 0 }, b = { 0 }; double ma = 0, mb = 0, sa = 0, sb = 0; unsigned rpa = 0, rpb = 0;
        for (int ph = 0; ph < PH; ph++) {
            res x = run(&H[i], 2, N, ph / (double)PH), y = run(&H[i], 1, N, ph / (double)PH);
            a.frames += x.frames; a.uneven += x.uneven; ma += x.mean_all; rpa += x.rephase; sa += x.secs;
            b.frames += y.frames; b.uneven += y.uneven; mb += y.mean_all; rpb += y.rephase; sb += y.secs; }
        double ua = 100.0 * a.uneven / a.frames, ub = 100.0 * b.uneven / b.frames, ra = 10 * rpa / sa, rb = 10 * rpb / sb;
        printf("%-34s | %6.2f %% / %6.2f ms      | %6.2f %% / %6.2f ms      | %6.1f -> %5.1f\n", H[i].name, ua, ma / PH, ub, mb / PH, ra, rb);
        int ok = rb * 2 <= ra + 0.1 && ub <= ua + 1.0 && mb / PH <= ma / PH + 0.1;
        if (!ok) { printf("FAIL: %s\n", H[i].name); fail++; }
    }
    ns += nh;
    // One grid (perf-1124 G3'): the game handing over on its own 60.000 Hz grid (33.333 ms) to a panel of 59.93 Hz (two refreshes: 33.37 ms). Ways:
    //   before (vkp_want_legacy, the game's grid); now (vkp_want_grid: the asked times snapped to the display's vsyncs, the game's grid, the default); the game on
    //   the display's grid alone (hle_nvn.c nvn_pace_grid, DM_PACE_GRID=display without the snap); both (DM_PACE_GRID=display). 3,000 frames (100 s). Columns:
    //   uneven frames, the most in one 300-frame window (the S25+ log's bursts: 0, 4, 7, 80, 47 ...), mean interval.
    //   What it showed (2026-11-24): the game on the display's grid locks the hand-over's phase against the vsync for good, where the drift used to sweep it:
    //   a good phase stays good, a bad one stays bad (GPU 22 ms, latch 8: 4.8 % uneven before, 8.9 % with a window of 148 of 300), so it is not the default.
    //   The snapped times on the game's grid are never worse and help wherever the frame fits its lead; where it does not (GPU + latch over the interval) no
    //   asked time within the lead mailbox allows helps (fable-final G4 / M1). Now must be: no more uneven than before (+ 0.25 points), no longer burst (+ 2)
    const scen G[] = {
        { "59.93 Hz, GPU 18, latch 8 ms",  16.686, 18.4, 3, 7, 5, 8.0, 16.686, 0, 0, 0, 0, 0, 33.3333 },
        { "59.93 Hz, GPU 22, latch 8 ms",  16.686, 22.0, 3, 7, 5, 8.0, 16.686, 0, 0, 0, 0, 0, 33.3333 },
        { "59.93 Hz, GPU 14, latch 12 ms", 16.686, 14.0, 3, 7, 5, 12.0, 16.686, 0, 0, 0, 0, 0, 33.3333 },
        { "119.86 Hz, GPU 18, latch 8 ms",  8.343, 18.4, 3, 7, 5, 8.0, 8.343, 0, 0, 0, 0, 0, 33.3333 },
        { "59.93 Hz panel, GPU 18 ms",   16.686, 18.4, 3, 7, 5, 16.686, 16.686, 0, 0, 0, 0, 0, 33.3333 },
        { "59.93 Hz panel, GPU 26 ms",   16.686, 25.6, 3, 7, 5, 16.686, 16.686, 0, 0, 0, 0, 0, 33.3333 },
        { "59.93 Hz, work jitter 12 ms", 16.686, 18.4, 3, 7, 12, 16.686, 16.686, 0, 0, 0, 0, 0, 33.3333 },
        { "119.86 Hz panel, GPU 18 ms",   8.343, 18.4, 3, 7, 5, 16.686, 8.343, 0, 0, 0, 0, 0, 33.3333 },
        { "60.00 Hz panel (no drift)",   16.667, 18.4, 3, 7, 5, 16.667, 16.667, 0, 0, 0, 0, 0, 33.3333 },
    };
    int ng = sizeof G / sizeof *G;
    printf("\n%-30s | %-23s | %-23s | %-23s | %s\n", "one grid (30 fps)", "before: uneven / max", "now (snapped): uneven / max", "display grid alone", "display grid + snapped");
    for (int i = 0; i < ng; i++) {
        scen dg = G[i]; dg.giv = 0;   // (the game on the display's grid)
        res a = { 0 }, b = { 0 }, c = { 0 }, d = { 0 }; int wa = 0, wb = 0, wc = 0, wd = 0;
        for (int ph = 0; ph < PH; ph++) {   // (each way the same random draws: the seed set again before each run)
            uint64_t seed = 0x9e3779b97f4a7c15ull ^ (uint64_t)(i * 131 + ph + 1);
            rng = seed; res x = run(&G[i], 0, N, ph / (double)PH); rng = seed; res y = run(&G[i], 3, N, ph / (double)PH);
            rng = seed; res z = run(&dg, 0, N, ph / (double)PH); rng = seed; res u = run(&dg, 3, N, ph / (double)PH);
            a.frames += x.frames; a.uneven += x.uneven; if (x.wmax > wa) wa = x.wmax;
            b.frames += y.frames; b.uneven += y.uneven; if (y.wmax > wb) wb = y.wmax;
            c.frames += z.frames; c.uneven += z.uneven; if (z.wmax > wc) wc = z.wmax;
            d.frames += u.frames; d.uneven += u.uneven; if (u.wmax > wd) wd = u.wmax; }
        double ua = 100.0 * a.uneven / a.frames, ub = 100.0 * b.uneven / b.frames;
        printf("%-30s | %6.2f %% / %3d          | %6.2f %% / %3d          | %6.2f %% / %3d          | %6.2f %% / %3d\n", G[i].name, ua, wa, ub, wb,
               100.0 * c.uneven / c.frames, wc, 100.0 * d.uneven / d.frames, wd);
        if (!(ub <= ua + 0.25 && wb <= wa + 2)) { printf("FAIL: %s\n", G[i].name); fail++; }
    }
    ns += ng;
    // vkp_want_grid is what runs with DM_PACE_LEAD off (the default): the steady scenarios above (the game's grid the display's) with it against vkp_want_legacy
    printf("\n%-30s | %-24s | %s\n", "steady, game on the grid", "legacy: uneven / mean", "snapped: uneven / mean");
    for (int i = 0; i < (int)(sizeof S / sizeof *S); i++) {
        res a = { 0 }, c = { 0 }; double ma = 0, mc = 0;
        for (int ph = 0; ph < PH; ph++) { uint64_t seed = 0x9e3779b97f4a7c15ull ^ (uint64_t)(i * 977 + ph + 1); rng = seed; res x = run(&S[i], 0, N, ph / (double)PH); rng = seed; res z = run(&S[i], 3, N, ph / (double)PH);
            a.frames += x.frames; a.uneven += x.uneven; ma += x.mean_iv; c.frames += z.frames; c.uneven += z.uneven; mc += z.mean_iv; }
        double ua = 100.0 * a.uneven / a.frames, uc = 100.0 * c.uneven / c.frames;
        printf("%-30s | %6.2f %% / %6.2f ms      | %6.2f %% / %6.2f ms\n", S[i].name, ua, ma / PH, uc, mc / PH);
        if (!(uc <= ua + 1.0 && mc / PH > 33.0 && mc / PH < 33.8)) { printf("FAIL: snapped, %s\n", S[i].name); fail++; }
    }
    // unit checks of the parts
    { vk_pacer p; memset(&p, 0, sizeof p); uint64_t r = 16666667;
      for (int i = 0; i < 16; i++) vkp_gpu(&p, (uint64_t)(10 + i) * 1000000);
      if (vkp_gpu_p95(&p) != 24000000) { printf("FAIL: p95 of 10..25 ms is the second longest (24), got %.1f\n", vkp_gpu_p95(&p) / MS); fail++; }
      p.vs = 1000 * r; uint64_t t = vkp_snap(&p, 1000 * r + 3 * r + 1, r, 1);
      if ((t + r / 2 - p.vs) % r || t < 1000 * r + 3 * r + 1 || t - (1000 * r + 3 * r + 1) > r) { printf("FAIL: snap up\n"); fail++; }
      // the display's record: 10 % of a window a refresh late adds a refresh; all late (the compositor's own delay) adds none
      vk_pacer q; memset(&q, 0, sizeof q); int up = 0; uint64_t now = 5000000000ull;
      for (int i = 0; i < 80; i++, now += 33333333) up += vkp_seen(&q, now, now + (i % 10 == 0 ? r : 0) + 1000, 0, now, r) > 0;
      if (up != 1 || q.extra != 1) { printf("FAIL: 10 %% late frames add one refresh (got %d, extra %d)\n", up, q.extra); fail++; }
      vk_pacer z; memset(&z, 0, sizeof z); up = 0; uint64_t now2 = now;
      for (int i = 0; i < 80; i++, now2 += 33333333) up += vkp_seen(&z, now2, now2 + r + 1000, 0, now2, r) != 0;
      if (up || z.extra) { printf("FAIL: a fixed compositor delay is not lateness\n"); fail++; }
      // 30 s calm with no margin reported gives the refresh back
      for (int i = 0; i < 1200; i++, now += 33333333) up += vkp_seen(&q, now, now + 1000, 0, now, r);
      if (q.extra != 0) { printf("FAIL: 30 s calm gives the refresh back (extra %d)\n", q.extra); fail++; } }
    printf("== %d scenarios, %d failed ==\n", ns, fail);
    return fail != 0;
}

// Present-time pacing (nvn/vk_pace.h) against a simulated compositor: counts the uneven frames (shown more than half a refresh off the frame interval, as
//   nvn_vk.c pace_time's "frame timing" line counts them) with the times asked as before (vkp_want_legacy) and now (vkp_want), at an unchanged mean.
// The model, per frame: the game's own timer ticks every interval (it follows the display's refresh, hle_sdk16.c, at a phase of its own); the game's work then
//   takes work..work+jit ms and the frame is handed over (pace_time's now); the GPU draws it from then (or from the end of the frame before) in gpu +- gjit ms;
//   the compositor shows it at the first vsync that is no earlier than the time asked (give or take 0.5 ms: where SurfaceFlinger compares it) and that comes at
//   least `latch` after the picture was finished, never two frames at one vsync. The display's record (shown, asked) reaches the pacer 2 refreshes after the
//   frame was shown, with a fixed delay of its own added (the compositor's: `disp`), and margin 0 (Android's swapchain reports none).
// Plain C, any OS: cc -O2 -o t_vk_pace t_vk_pace.c && ./t_vk_pace
#include "../nvn/vk_pace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (double)(rng >> 11) / 9007199254740992.0; }
#define MS 1000000.0

typedef struct { const char *name; double r, gpu, gjit, work, jit, latch, disp; int nots; } scen;   // nots: the driver gives no GPU time
typedef struct { long frames, uneven; double mean_iv, lat; unsigned rephase; int extra; } res;

// one run of n frames; mode 0: as before (lead one interval), 1: now
static res run(const scen *s, int mode, int n, double phase) {
    uint64_t r = (uint64_t)(s->r * MS), iv = ((uint64_t)(33333333.0 + r / 2) / r) * r;
    vk_pacer P; memset(&P, 0, sizeof P);
    uint64_t vs0 = 1000000000ull + (uint64_t)(urand() * r), t0 = 2000000000ull + (uint64_t)(phase * iv);
    enum { Q = 64 }; struct { uint64_t asked, shown, at; int used; } q[Q]; memset(q, 0, sizeof q);
    uint64_t gpu_free = 0, last_shown = 0, prev_actual = 0; res o = { 0 }; double sum = 0; long nsum = 0;
    for (int i = 0; i < n; i++) {
        uint64_t now = t0 + (uint64_t)i * iv + (uint64_t)((s->work + urand() * s->jit) * MS);
        for (int k = 0; k < Q; k++) if (q[k].used && q[k].at <= now) {   // the display's record so far
            if (mode) vkp_seen(&P, q[k].asked, q[k].shown, 0, now, r);
            q[k].used = 0; }
        uint64_t g = (uint64_t)((s->gpu + (urand() * 2 - 1) * s->gjit) * MS);
        if (mode && i >= 2 && !s->nots) vkp_gpu(&P, g);   // (pace_time reads the GPU time of frames already done; the model's is steady enough that this one's stands in)
        uint64_t want = mode ? vkp_want(&P, now, r, iv) : vkp_want_legacy(&P, now, r, iv, iv);
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
            if (prev_actual) { uint64_t d = actual - prev_actual; o.frames++; sum += d; nsum++; if (d + r / 2 < iv || d > iv + r / 2) o.uneven++; } }
        prev_actual = actual;
    }
    o.mean_iv = nsum ? sum / nsum / MS : 0; o.lat = o.frames ? o.lat / o.frames : 0; o.rephase = P.rephase; o.extra = P.extra;
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

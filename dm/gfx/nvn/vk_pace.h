// Present times for VK_GOOGLE_display_timing (nvn_vk.c pace_time): when a frame asks to be shown. Pure functions, no Vulkan: dm/gfx/tests/t_vk_pace.c runs them
//   against a simulated compositor (a frame's GPU time, the game's own timer, the display's vsync and how early the compositor must have the finished picture).
// The frames ask for a grid one frame interval apart (30 fps: two refreshes at 60 Hz, four at 120). Before (vkp_want_legacy) a frame kept its place on the grid
//   whenever its time was at least half a refresh away. That ignored two things: the GPU has not drawn the frame yet when it is handed over (18-28 ms on a Galaxy
//   S25+, more on a POCO F6), and the compositor needs the finished picture about a refresh before the vsync it is shown at. A frame handed over 9-25 ms before its
//   time kept that time, missed it, showed a refresh late, and the next one was on time again: a 50 / 16.7 ms pair at an unchanged 33.3 ms mean (S25+ 4-20 % of
//   frames, Legends: Arceus 13 % at 18.4 ms of GPU, 29 % at 28.2 ms; POCO F6 19-33 %). Asked times also fell anywhere between two vsyncs, so one just after a vsync
//   went one way or the other by a fraction of a millisecond.
// Now (vkp_want): a frame keeps its place only when the GPU's time (p95 of the last 16 frames) plus the compositor's lead (one refresh, plus up to two more the
//   display's own record asks for) still fits before it; else the grid moves later, once, and stays there. Every asked time sits half a refresh before a vsync the
//   display reported, so it can not fall on either side of one. The display's record (vkp_seen): frames shown more than half a refresh after the rest of a 2 s
//   window in 5-95 % of it (some, not all: all is the compositor's own fixed delay) add a refresh to the lead; 5 s without one, with a reported margin over 1.5
//   refreshes (30 s when the driver reports no margin: Android's often does not), take one back. Costs up to a refresh of input delay; DM_PACE_LEAD=0: as before
// Hitches and an overloaded GPU (vkp_want's hold, DM_PACE_HOLD=0 to turn it off): on the S25+ the grid moved 16 times in 10 s after a field load (frames of 50-143 ms,
//   then a few handed over 24-35 ms apart while the game caught up; mean interval 35.9 ms) and 186 times in 10 s when heat took the GPU to 50 ms a frame. A frame
//   handed over more than 1.5 intervals after the one before (a load, a pipeline built, a hitch) starts a 1 s grace period: in it a late frame asks the earliest
//   time it can make (GPU p95 + the compositor's lead, not the full lead a move gives: a move asked half a refresh to two refreshes more, one refresh more on the
//   screen for that frame) and the grid goes on from there; two late frames in a row that were not hitches still move it; frames that come early while the game
//   catches up keep their place (up to an interval more ahead than otherwise, where a move pulled the grid back and showed one a refresh early). With the GPU's
//   p95 over the frame interval the 30 can not be held anyway: a frame keeps its slot when it fits, else asks one fixed lead (an interval and two refreshes, the
//   most vkp_lead gives) from now, and that is no move (the grid is not chased), until the p95 is a quarter refresh under the interval. t_vk_pace (the S25+
//   cases): grid moves in 10 s 9.8-11.6 -> 0.1-0.5 over a field load, 1.6-7.4 -> 0.2-1.0 with a 140 ms frame every 2-10 s, 96-192 -> 0 with the GPU at 38-50 ms;
//   uneven frames the same or fewer, the steady cases unchanged (no hitch there: the hold never acts)
#pragma once
#include <stdint.h>

enum { VKP_GPU_N = 16, VKP_WIN = 256, VKP_EXTRA_MAX = 2 };
typedef struct {
    uint64_t next;                    // the time the last paced frame asked for (the grid); 0: none yet
    int extra;                        // refreshes added to the compositor's lead from the display's record (0..VKP_EXTRA_MAX)
    uint64_t vs;                      // a vsync the display reported (actualPresentTime): the grid's phase; 0: none yet
    uint64_t gpu[VKP_GPU_N]; int ngpu, gpos;   // the GPU's time of the last frames
    uint64_t win_t; int win_n; uint64_t off[VKP_WIN];   // this 2 s window: each frame's shown time minus its asked time
    uint64_t calm_t, calm_margin;     // since the last late frame: when, and the smallest margin the driver reported (0: none reported)
    unsigned rephase, last_late, last_n;   // counts for the log: grid moves; the last window's late frames of its frames
    uint64_t last_now, grace_t;       // when the frame before was handed over; a hitch's grace period runs until grace_t
    int late, over, hold_off;         // the frame before was late in a grace period; the GPU's p95 is over the interval (fixed lead); DM_PACE_HOLD=0: no hold
    unsigned kept, fixed;             // counts for the log: late frames that kept the grid in a grace period; frames asked at the fixed lead
} vk_pacer;
enum { VKP_GRACE_MS = 1000 };

static inline void vkp_gpu(vk_pacer *p, uint64_t ns) { p->gpu[p->gpos] = ns; p->gpos = (p->gpos + 1) % VKP_GPU_N; if (p->ngpu < VKP_GPU_N) p->ngpu++; }
// p95 of the samples kept (16: the second longest, so one odd frame does not move the grid); 0 with none (a driver without timestamps: Mali, PowerVR)
static inline uint64_t vkp_gpu_p95(const vk_pacer *p) {
    uint64_t a = 0, b = 0;
    for (int i = 0; i < p->ngpu; i++) { uint64_t v = p->gpu[i]; if (v > a) { b = a; a = v; } else if (v > b) b = v; }
    return p->ngpu >= 8 ? b : a;
}
// t moved to half a refresh before a vsync of the display's (up: the first such time at or after t; else the nearest)
static inline uint64_t vkp_snap(const vk_pacer *p, uint64_t t, uint64_t r, int up) {
    if (!p->vs || !r || p->vs < r) return t;
    uint64_t base = p->vs - r / 2;
    if (t >= base) { uint64_t k = up ? (t - base + r - 1) / r : (t - base + r / 2) / r; return base + k * r; }
    uint64_t k = up ? (base - t) / r : (base - t + r / 2) / r; return base - k * r;
}
// The lead a frame needs (handed over at now, shown no earlier than now + need) and the lead a moved grid gets (half a refresh more, at least one interval, at most
//   an interval and two refreshes)
static inline uint64_t vkp_need(const vk_pacer *p, uint64_t r) { return vkp_gpu_p95(p) + (uint64_t)(1 + p->extra) * r; }
static inline uint64_t vkp_lead(const vk_pacer *p, uint64_t r, uint64_t iv) {
    uint64_t l = vkp_need(p, r) + r / 2; if (l < iv) l = iv; if (l > iv + 2 * r) l = iv + 2 * r; return l;
}
// The time a frame handed over at now asks for, iv apart from the one before. Returns it and keeps it as the grid
static inline uint64_t vkp_want(vk_pacer *p, uint64_t now, uint64_t r, uint64_t iv) {
    uint64_t lead = vkp_lead(p, r, iv), need = vkp_need(p, r); if (need > lead) need = lead;
    uint64_t want = p->next ? p->next + iv : 0;
    if (!p->hold_off) {
        uint64_t gap = p->last_now && now > p->last_now ? now - p->last_now : 0, g = vkp_gpu_p95(p); p->last_now = now;
        if (g > iv) p->over = 1; else if (g + r / 4 < iv) p->over = 0;
        if (p->over) {   // the GPU can not draw the interval: one fixed lead, a slot kept when it fits, no grid to chase (the grace period covers the way back)
            uint64_t fl = iv + 2 * r; if (need > fl) need = fl;
            p->late = 0; p->grace_t = now + VKP_GRACE_MS * 1000000ull;
            if (want && want >= now + need && want <= now + fl + 2 * r) return p->next = vkp_snap(p, want, r, 0);
            p->fixed++; return p->next = vkp_snap(p, now + fl, r, 1); }
        int hitch = gap > iv + iv / 2;
        if (hitch) p->grace_t = now + VKP_GRACE_MS * 1000000ull;
        if (want && now < p->grace_t) {
            if (want < now + need && (hitch || !p->late)) {   // late after a hitch: the earliest time it can make (not the full lead), the grid on from it
                p->kept++; p->late = 1;
                return p->next = vkp_snap(p, now + need, r, 1); }
            if (want >= now + need && want <= now + lead + iv + 2 * r) { p->late = 0; return p->next = vkp_snap(p, want, r, 0); }
        }
        p->late = 0;
    }
    if (!want || want < now + need || want > now + lead + 2 * r) {   // late (or the game ran ahead of the display): the grid moves, once
        if (p->next) p->rephase++;
        want = vkp_snap(p, now + lead, r, 1);
    } else want = vkp_snap(p, want, r, 0);   // (on the grid already; the nearest only corrects a drift of the refresh's measure)
    p->next = want; return want;
}
// As before 2026-11-24 (DM_PACE_LEAD=0): late only when the time is under half a refresh away; lead: one interval, or DM_VK_PACE_LEAD's
static inline uint64_t vkp_want_legacy(vk_pacer *p, uint64_t now, uint64_t r, uint64_t iv, uint64_t lead) {
    if (lead < r) lead = r;
    uint64_t want = p->next ? p->next + iv : now + lead;
    if (want < now + r / 2) want = now + lead;
    if (want > now + (lead > iv ? lead : iv)) want = now + (lead > iv ? lead : iv);
    p->next = want; return want;
}
// The default since perf-1124 G3' (DM_PACE_GRID=game: vkp_want_legacy). The game handed frames over on a grid of its own, 2 / 60 s = 33.333 ms (hle_nvn.c), while
//   the display shows them 2 refreshes apart, 33.37 ms on a 59.93 Hz panel: each hand-over came 0.04 ms earlier against the display, so vkp_want_legacy's
//   clamp (no more than one lead ahead of now) put the asked time at now + lead, which carried the hand-over's jitter and slid through a whole refresh about
//   every 15 s; near the edge frames flipped between two vsyncs (S25+: uneven frames per 300 in bursts, 0, 4, 7, 80, 47, 0 ..., "mean interval 33.26-33.32 ms
//   (target 33.37)"). Here the grid is kept while its time is no later than the legacy clamp allows (+ half a refresh), and every asked time sits half a
//   refresh before a reported vsync (vkp_snap, the nearest), so jitter under half a refresh changes nothing. The vsync a frame can be shown at stays within
//   legacy's bounds (now + lead .. now + lead + a refresh; never more than an interval and a refresh ahead): with mailbox a longer lead lets a newer frame
//   replace one still waiting (fable-final M1), which t_vk_pace's compositor does not model. t_vk_pace "one grid": a 59.93 Hz panel against the game's 60.000
//   Hz grid, uneven frames before / now per scenario (and hle_nvn.c's grid on the display's refresh too, nvn_pace_grid)
static inline uint64_t vkp_want_grid(vk_pacer *p, uint64_t now, uint64_t r, uint64_t iv, uint64_t lead) {
    if (lead < r) lead = r;
    uint64_t hi = lead > iv ? lead : iv, want = p->next ? p->next + iv : 0;
    if (!want || want < now + r / 2 || want > now + hi + r / 2) want = vkp_snap(p, now + lead, r, 0);   // (late, or ahead of the display: a full lead again)
    else want = vkp_snap(p, want, r, 0);
    p->next = want; return want;
}
// The display's record of one paced frame: asked (desiredPresentTime), shown (actualPresentTime), margin (presentMargin, 0: not reported). now: the clock when it
//   is read. Returns +1 when the lead grew a refresh, -1 when it gave one back, 0 else (the caller logs the change)
static inline int vkp_seen(vk_pacer *p, uint64_t asked, uint64_t shown, uint64_t margin, uint64_t now, uint64_t r) {
    if (!asked || !shown || !r) return 0;
    p->vs = shown;
    if (!p->win_t) p->win_t = now;
    if (!p->calm_t) p->calm_t = now;
    if (p->win_n < VKP_WIN) p->off[p->win_n++] = shown > asked ? shown - asked : 0;
    if (margin && (!p->calm_margin || margin < p->calm_margin)) p->calm_margin = margin;
    if (now - p->win_t < 2000000000ull || p->win_n < 20) return 0;
    uint64_t lo = UINT64_MAX; for (int i = 0; i < p->win_n; i++) if (p->off[i] < lo) lo = p->off[i];
    int late = 0; for (int i = 0; i < p->win_n; i++) late += p->off[i] > lo + r / 2;
    int n = p->win_n, ret = 0; p->last_late = (unsigned)late; p->last_n = (unsigned)n; p->win_n = 0; p->win_t = now;
    if (late * 100 >= n * 5 && late * 100 <= n * 95) {   // some frames a refresh late, not all: the compositor needs them earlier than the lead gives
        if (p->extra < VKP_EXTRA_MAX) { p->extra++; p->next = 0; ret = 1; }
        p->calm_t = now; p->calm_margin = 0;
    } else if (late) { p->calm_t = now; p->calm_margin = 0; }
    else if (p->extra > 0 && now - p->calm_t >= (p->calm_margin > r + r / 2 ? 5000000000ull : 30000000000ull)) {
        p->extra--; p->calm_t = now; p->calm_margin = 0; ret = -1; }
    return ret;
}

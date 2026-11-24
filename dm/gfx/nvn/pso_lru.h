// Which pipelines the cap (vk_quirks.h Q_PIPE_KEEP, DM_VK_PIPE_KEEP) destroys, in nvn_vk.c pso_evict: kept apart so t_pso_lru.c can run it on a PC.
// Before 2026-11-24 a pipeline the prewarm built had never been used, so the first check after the prewarm (300 presents in) destroyed every one over the cap at once:
//   "pipelines kept: 720 of 2467 left (1747 destroyed)" on a Mali (#215), and the game then built them again one by one while it played. Now (DM_VK_PIPE_LRU, on by default):
//   - a pipeline counts as used when it is built (pso_lru_stamp), so a prewarmed one is as recent as the start of the game, not older than everything;
//   - one check destroys at most `step` (DM_VK_PIPE_EVICT_STEP, half the cap by default), least recently used first, so the pipelines over the cap go over several
//     checks (10 s apart) and those the game reaches meanwhile are used, so kept.
// A pipeline used in the last PSO_LRU_RECENT presents (30 s at 30 fps) is never destroyed, as before.
#ifndef PSO_LRU_H
#define PSO_LRU_H
#include <stdint.h>
#include <stdlib.h>

#define PSO_LRU_RECENT 900

typedef struct { uint64_t last; uint32_t id; } pso_lru_ent;   // last: the present after its last use (0: never used), id: the caller's own index

static int pso_lru_cmp(const void *a, const void *b) {
    const pso_lru_ent *x = a, *y = b;
    if (x->last != y->last) return x->last < y->last ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;   // (the same age: the caller's order, so a run gives the same choice every time)
}
// the stamp a pipeline gets when it is built: the present after now (as a draw's use gives it), or 0 with the LRU off (the old rule: a pipeline never drawn with is the oldest)
static inline uint64_t pso_lru_stamp(uint64_t frame, int lru) { return lru ? frame + 1 : 0; }
// the default number destroyed per check: half the cap (at least 1)
static inline int pso_lru_step_default(int cap) { return cap / 2 > 0 ? cap / 2 : 1; }
// e[0..n): every live pipeline. Moves the ones that may go to the front, oldest first, and returns how many of them to destroy now: down to 90 % of the cap, at most
//   step (step <= 0: no limit, the old rule). now1: the current present + 1 (the stamp a use now would give)
static uint32_t pso_lru_pick(pso_lru_ent *e, uint32_t n, uint64_t now1, int cap, int step) {
    if (cap <= 0 || n <= (uint32_t)cap) return 0;
    uint32_t nc = 0;
    for (uint32_t i = 0; i < n; i++) { if (e[i].last && e[i].last + PSO_LRU_RECENT > now1) continue; pso_lru_ent t = e[nc]; e[nc++] = e[i]; e[i] = t; }
    if (!nc) return 0;
    qsort(e, nc, sizeof *e, pso_lru_cmp);
    uint32_t drop = n - (uint32_t)cap * 9 / 10; if (drop > nc) drop = nc;
    if (step > 0 && drop > (uint32_t)step) drop = (uint32_t)step;
    return drop;
}
#endif

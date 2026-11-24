// t_pso_lru: the pipeline cap's choice (dm/gfx/nvn/pso_lru.h, nvn_vk.c pso_evict) on a PC. The #215 case: a Mali with the cap of 800 prewarms 2,467 pipelines; the
//   game then draws with 500 of them (and 20 s in reaches 300 more). The old rule destroyed 1,747 at the first check, 300 presents in; the LRU rule keeps every pipeline
//   for 30 s after it was built, then destroys at most half the cap per check, least recently used first, and never one the game used in the last 30 s.
// Build: cc -O1 -Wall -I../nvn -o t_pso_lru t_pso_lru.c
#include <stdio.h>
#include <string.h>
#include "pso_lru.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { NP = 2467, CAP = 800 };
typedef struct { uint64_t last; int live, destroyed_used; } pipe;
static pipe P[NP];
static uint32_t live_n(void) { uint32_t n = 0; for (int i = 0; i < NP; i++) n += P[i].live; return n; }

// one check, as pso_evict makes it: every live pipeline, the pick, the first `drop` destroyed. Returns how many it destroyed
static uint32_t check(uint64_t frame, int step, int used_from, int used_to) {
    static pso_lru_ent e[NP]; uint32_t n = 0;
    for (int i = 0; i < NP; i++) if (P[i].live) e[n++] = (pso_lru_ent){ P[i].last, (uint32_t)i };
    uint32_t d = pso_lru_pick(e, n, frame + 1, CAP, step);
    for (uint32_t k = 0; k < d; k++) { pipe *p = &P[e[k].id]; p->live = 0; if ((int)e[k].id >= used_from && (int)e[k].id < used_to) p->destroyed_used++; }
    return d;
}
// the game from frame 1 to `to`: draws with pipelines [a, b) every frame, then from present 600 also with [500, 800); a pipeline destroyed and needed again is built again
static void run(int lru, int step, uint32_t *first_drop, uint64_t *first_drop_at, uint32_t *max_drop, uint32_t *rebuilt, uint32_t *final_live) {
    memset(P, 0, sizeof P);
    for (int i = 0; i < NP; i++) { P[i].live = 1; P[i].last = pso_lru_stamp(0, lru); }   // the prewarm, before the first present
    *first_drop = 0; *first_drop_at = 0; *max_drop = 0; *rebuilt = 0;
    for (uint64_t f = 1; f <= 6000; f++) {
        int to = f >= 600 ? 800 : 500;   // (the next area 20 s in: within the 30 s a prewarmed pipeline counts as new)
        for (int i = 0; i < to; i++) { if (!P[i].live) { P[i].live = 1; (*rebuilt)++; } P[i].last = f + 1; }
        if (f % 300 == 0) { uint32_t d = check(f, step, 0, to); if (d && !*first_drop) { *first_drop = d; *first_drop_at = f; } if (d > *max_drop) *max_drop = d; }
    }
    *final_live = live_n();
}

int main(void) {
    // the pieces
    CHECK(pso_lru_step_default(800) == 400 && pso_lru_step_default(1) == 1, "default step: half the cap");
    CHECK(pso_lru_stamp(41, 1) == 42 && pso_lru_stamp(41, 0) == 0, "stamp: the present after the build with the LRU, never used without");
    { pso_lru_ent e[4] = { { 0, 0 }, { 5, 1 }, { 2000, 2 }, { 3, 3 } };   // cap 1, now 2001: 2000 is recent, the others go oldest first
      uint32_t d = pso_lru_pick(e, 4, 2001, 1, 0);
      CHECK(d == 3 && e[0].id == 0 && e[1].id == 3 && e[2].id == 1, "pick: oldest first, the recent one kept (%u: %u %u %u)", d, e[0].id, e[1].id, e[2].id); }
    { pso_lru_ent e[3] = { { 0, 0 }, { 0, 1 }, { 0, 2 } }; CHECK(pso_lru_pick(e, 3, 10, 3, 0) == 0 && pso_lru_pick(e, 3, 10, 0, 0) == 0, "pick: at or under the cap, or no cap: nothing"); }
    { pso_lru_ent e[10]; for (int i = 0; i < 10; i++) e[i] = (pso_lru_ent){ 1, (uint32_t)(9 - i) };
      uint32_t d = pso_lru_pick(e, 10, 5000, 4, 2); CHECK(d == 2 && e[0].id == 0 && e[1].id == 1, "pick: at most step, ties in the caller's order (%u: %u %u)", d, e[0].id, e[1].id); }

    // #215 with the old rule: 1,747 at once at the first check
    uint32_t fd, md, rb, fl; uint64_t at;
    run(0, 0, &fd, &at, &md, &rb, &fl);
    printf("old rule: first check at present %llu destroyed %u (largest %u), %u built again in play, %u live at the end\n", (unsigned long long)at, fd, md, rb, fl);
    CHECK(at == 300 && fd == NP - CAP * 9 / 10, "old rule reproduces #215: %u destroyed at present %llu", fd, (unsigned long long)at);
    // the LRU rule
    uint32_t rb_old = rb;
    run(1, pso_lru_step_default(CAP), &fd, &at, &md, &rb, &fl);
    printf("LRU rule: first check that destroyed any at present %llu: %u (largest %u), %u built again in play, %u live at the end\n", (unsigned long long)at, fd, md, rb, fl);
    CHECK(at >= PSO_LRU_RECENT, "LRU: nothing destroyed in the first 30 s (first at present %llu)", (unsigned long long)at);
    CHECK(md <= (uint32_t)pso_lru_step_default(CAP), "LRU: at most half the cap per check (%u)", md);
    CHECK(fl <= CAP, "LRU: under the cap in the end (%u live)", fl);
    int used_lost = 0; for (int i = 0; i < 500; i++) used_lost += P[i].destroyed_used; CHECK(!used_lost, "LRU: no pipeline the game drew with every frame destroyed (%d)", used_lost);
    CHECK(rb < rb_old, "LRU: fewer pipelines built again in play (%u, old rule %u)", rb, rb_old);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

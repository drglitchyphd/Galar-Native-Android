// Faster rendering's queue (DM_NVN_ASYNC: the replay on a thread of its own, hle_nvn.c), in a header of its own so a PC test (tests/t_rq_order.c) runs the same code.
// Turning NVN commands into Vulkan took 14 to 21 ms of the game thread's frame (Violet, Galaxy S25+), one after the other with the game's own work; side by side a frame
// costs the longer of the two. It is what the console's GPU does anyway: it runs behind the CPU, and a game may only change what the GPU reads once a sync says it is done.
// So the game goes on after a submit, and waits only where it asks for a result: a sync wait, Finish, reading texels back, or finalizing an object that commands in the
// queue still name (rq_drain). At most one present is ahead of the game, so frames stay paced and the delay stays under one frame.
// What the replay reads that the game sets outside a command buffer goes through the queue too, in order with the submissions (perf-1124 fix 3; DM_NVN_ASYNC_ORDER=0:
//   as before, at once):
//   - texture / image / sampler registrations (rq_reg_tex, rq_reg_samp). On the console a registration is the game writing a descriptor into pool memory, read by the GPU
//     when it runs the draw; here it wrote the replay's ID table at once, so a slot reused for another texture changed what draws still in the queue sampled (a fit for
//     #158: models and crowd wrong for single frames in a cutscene that streams characters in and out, on a phone whose replay runs 60 ms behind). Now the registration is
//     copied when the game makes it and applied by the replay thread between the submissions it came between; one that changes nothing (the slot already holds the
//     same bytes, as the game's per-frame re-registrations mostly do) is left out.
//   - a sparse pool's MapVirtual (rq_before_remap): an unmap makes the pages inaccessible and splits the pool the replay knows, while commands in the queue may still name
//     that range (a fit for Arceus #3, crashes at loading screens with Faster rendering): what is queued is replayed first.
// Not queued (the console reads these from memory when the GPU runs, and a game must wait on a sync before it rewrites them, as it must here): bindless handles in
//   uniform memory, indirect counts, index data, uniform and storage data.
// The includer defines LOG(...) and die(const char *), and the two functions declared below; mtl_reg_tex / mtl_reg_samp are the replay's ID tables (nvn_obj.h).
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "nvn_obj.h"
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#define DM_THREAD_NAME(n) prctl(PR_SET_NAME, (n), 0, 0, 0)
#else
#define DM_THREAD_NAME(n) ((void)0)
#endif

enum { RQ_SUBMIT, RQ_FENCE, RQ_PRESENT, RQ_REG };
typedef struct { int id; uint8_t samp, ok, has_v; tex_t t; view_t v; samp_par s; } rq_reg;   // one registration as the game made it (samp: a sampler's, else a texture's)
typedef struct { int kind, n, idx; uint64_t *h; void *q, *s, *o; rq_reg *reg; double period; int crop[4]; } rq_item;   // RQ_REG: reg[n]
static int rq_on(void);                   // the includer's: whether the replay has a thread of its own (decided once)
static void rq_run(const rq_item *it);    // the includer's: a submission, fence or present, on the replay thread
enum { RQ_N = 512 };
static rq_item g_rq[RQ_N]; static uint64_t g_rq_head, g_rq_tail; static int g_rq_presents;
static pthread_mutex_t g_rq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_rq_cv = PTHREAD_COND_INITIALIZER;
static const char *volatile g_rq_at = "idle";
static _Atomic int g_rq_tid;   // the replay thread's id (android_main.c: its loads profiled)
// Registrations waiting for the next item (g_rq_mu): they go into the queue as one RQ_REG item just ahead of it, so they land between the same two submissions as the
//   game made them. The game's view of the ID tables (g_rq_tsh / g_rq_ssh: what is in the queue or applied, per ID) finds those that change nothing
static rq_reg *g_rq_pend; static int g_rq_npend, g_rq_cpend;
static rq_reg *g_rq_tsh, *g_rq_ssh; static uint8_t *g_rq_tshok, *g_rq_sshok; static int g_rq_ntsh, g_rq_nssh;
static uint64_t g_rq_reg_n, g_rq_reg_same;   // registrations queued / left out as unchanged (the log, the test)
static int g_rq_order = -1;   // DM_NVN_ASYNC_ORDER (above): 1 registrations queued and a remap waits for the queue, 0 at once as before
static int rq_order_on(void) { if (g_rq_order < 0) { const char *e = getenv("DM_NVN_ASYNC_ORDER"); g_rq_order = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
    if (rq_on()) LOG("[nvn] the replay thread's ID tables: %s (DM_NVN_ASYNC_ORDER %s)", g_rq_order ? "registrations applied in order with the submissions, remaps after the queue" : "registrations applied at once (the old way)", e ? e : "Auto"); }
    return g_rq_order; }
// A wait on the replay thread that takes this long has gone wrong: said in the log with who is waiting and what the replay thread is doing
static void rq_wait(const char *who) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    if (pthread_cond_timedwait(&g_rq_cv, &g_rq_mu, &ts) == ETIMEDOUT) {
        static int n; if (n++ < 20) LOG("[RED] %s has waited 5 s on the replay thread: it is at %s, queue %llu..%llu, %d presents, caller lr %p", who, g_rq_at, (unsigned long long)g_rq_tail, (unsigned long long)g_rq_head, g_rq_presents, __builtin_return_address(0));
    }
}
static void rq_apply_regs(const rq_reg *r, int n) {
    for (int i = 0; i < n; i++) if (r[i].samp) mtl_reg_samp(r[i].id, &r[i].s); else mtl_reg_tex(r[i].id, r[i].ok ? &r[i].t : NULL, r[i].has_v ? &r[i].v : NULL);
}
static void *rq_main(void *a) { DM_THREAD_NAME("dm-replay");
#ifdef __linux__
    atomic_store(&g_rq_tid, (int)syscall(SYS_gettid));   // (gettid needs _GNU_SOURCE on glibc: the PC test)
#endif
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_rq_mu);
        while (g_rq_tail == g_rq_head) pthread_cond_wait(&g_rq_cv, &g_rq_mu);
        rq_item it = g_rq[g_rq_tail % RQ_N];
        pthread_mutex_unlock(&g_rq_mu);
        if (it.kind == RQ_REG) { g_rq_at = "registrations"; rq_apply_regs(it.reg, it.n); free(it.reg); }
        else rq_run(&it);
        g_rq_at = "idle";
        pthread_mutex_lock(&g_rq_mu);
        g_rq_tail++; if (it.kind == RQ_PRESENT) g_rq_presents--;
        pthread_cond_broadcast(&g_rq_cv); pthread_mutex_unlock(&g_rq_mu);
    }
    return NULL;
}
static void rq_put_locked(rq_item it) {   // caller holds g_rq_mu: the pending registrations, then the item (kind < 0: the registrations alone)
    static int started;
    if (!started) { started = 1; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20); pthread_t t; if (pthread_create(&t, &at, rq_main, NULL)) die("replay thread"); pthread_detach(t); }
    // (the slots needed counted again after each wait: another thread may have registered something while this one waited)
    while (g_rq_head - g_rq_tail + (uint64_t)(g_rq_npend > 0) + (uint64_t)(it.kind >= 0) > RQ_N) rq_wait("a full queue");
    if (g_rq_npend) { g_rq[g_rq_head++ % RQ_N] = (rq_item){ .kind = RQ_REG, .n = g_rq_npend, .reg = g_rq_pend }; g_rq_pend = NULL; g_rq_npend = g_rq_cpend = 0; }
    if (it.kind >= 0) { g_rq[g_rq_head++ % RQ_N] = it; if (it.kind == RQ_PRESENT) g_rq_presents++; }
    pthread_cond_broadcast(&g_rq_cv);
}
static void rq_push(rq_item it) {
    pthread_mutex_lock(&g_rq_mu);
    rq_put_locked(it);
    if (it.kind == RQ_PRESENT) while (g_rq_presents > 1) rq_wait("the next present");
    pthread_mutex_unlock(&g_rq_mu);
}
// Wait until what is queued has been replayed (registrations made since the last submission included)
static void rq_drain_now(void) {
    if (!rq_on()) return;
    pthread_mutex_lock(&g_rq_mu);
    if (g_rq_npend) rq_put_locked((rq_item){ .kind = -1 });
    uint64_t want = g_rq_head;
    while (g_rq_tail < want) rq_wait("a drain");
    pthread_mutex_unlock(&g_rq_mu);
}
// The game's view of one ID table: grown to hold id; caller holds g_rq_mu
static void rq_sh_grow(rq_reg **sh, uint8_t **ok, int *n, int id) {
    if (id < *n) return;
    int m = id + 1024; rq_reg *s2 = realloc(*sh, (size_t)m * sizeof **sh); uint8_t *o2 = realloc(*ok, (size_t)m);
    if (!s2 || !o2) die("replay queue allocation failed");
    memset(o2 + *n, 0, (size_t)(m - *n)); *sh = s2; *ok = o2; *n = m;
}
static void rq_reg_put(const rq_reg *r) {   // caller holds g_rq_mu
    if (g_rq_npend == g_rq_cpend) { int m = g_rq_cpend ? 2 * g_rq_cpend : 64; rq_reg *p = realloc(g_rq_pend, (size_t)m * sizeof *p); if (!p) die("replay queue allocation failed"); g_rq_pend = p; g_rq_cpend = m; }
    g_rq_pend[g_rq_npend++] = *r; g_rq_reg_n++;
}
static void rq_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    if (!rq_on() || !rq_order_on()) { mtl_reg_tex(id, t, v); return; }
    rq_reg r; memset(&r, 0, sizeof r); r.id = id; r.ok = t != NULL; r.has_v = v != NULL;
    if (t) memcpy(&r.t, t, sizeof r.t);
    if (v) memcpy(&r.v, v, sizeof r.v);
    pthread_mutex_lock(&g_rq_mu);
    rq_sh_grow(&g_rq_tsh, &g_rq_tshok, &g_rq_ntsh, id);
    const rq_reg *o = &g_rq_tsh[id];
    if (g_rq_tshok[id] && o->ok == r.ok && o->has_v == r.has_v && !memcmp(&o->t, &r.t, sizeof r.t) && !memcmp(&o->v, &r.v, sizeof r.v)) g_rq_reg_same++;
    else { g_rq_tsh[id] = r; g_rq_tshok[id] = 1; rq_reg_put(&r); }
    pthread_mutex_unlock(&g_rq_mu);
}
static void rq_reg_samp(int id, const samp_par *s) {
    if (id < 0) return;
    if (!rq_on() || !rq_order_on()) { mtl_reg_samp(id, s); return; }
    rq_reg r; memset(&r, 0, sizeof r); r.id = id; r.samp = 1; r.ok = 1; memcpy(&r.s, s, sizeof r.s);
    pthread_mutex_lock(&g_rq_mu);
    rq_sh_grow(&g_rq_ssh, &g_rq_sshok, &g_rq_nssh, id);
    if (g_rq_sshok[id] && !memcmp(&g_rq_ssh[id].s, &r.s, sizeof r.s)) g_rq_reg_same++;
    else { g_rq_ssh[id] = r; g_rq_sshok[id] = 1; rq_reg_put(&r); }
    pthread_mutex_unlock(&g_rq_mu);
}
// Before a sparse pool's pages move (MapVirtual): commands in the queue may still name them
static void rq_before_remap(void) { if (rq_on() && rq_order_on()) rq_drain_now(); }

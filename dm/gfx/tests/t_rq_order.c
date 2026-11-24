// Faster rendering's queue (nvn/nvn_rq.h, DM_NVN_ASYNC: the replay on a thread of its own) keeps what a queued draw reads as the game had it when it recorded the draw
//   (perf-1124 fix 3). The queue code is the engine's own header; the replay here is a stand-in: a "draw" names a texture ID and a sampler ID, and at replay it reads
//   them from the ID tables mtl_reg_tex / mtl_reg_samp fill (as nvn_vk.c's push_stage does), or reads a word of memory (as a draw reads a sparse pool's page).
//   1. a texture and a sampler registered, a draw recorded while the replay thread is held back, the same IDs registered to another texture / sampler, a second draw:
//      the first draw must see the first registration, the second the second; registering the same bytes again is left out of the queue;
//   2. a remap (MapVirtual: rq_before_remap) while a draw that reads the old page is still queued: the draw must read the old page;
//   3. the replay running alongside: 2,000 times a registration of the same ID then a draw, every draw sees its own;
//   4. DM_NVN_ASYNC_ORDER=0 (the old way, at once): 1 and 2 again, and the test must see the hole (the draw reads the later registration / page), so it can find one.
//   Build and run: make t_rq_order && ./t_rq_order (plain C, any OS with pthreads).
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>
#define LOG(...) do { fprintf(stderr, "[t] " __VA_ARGS__); fputc('\n', stderr); } while (0)
static void die(const char *m) { fprintf(stderr, "die: %s\n", m); _exit(2); }
#include "../nvn/nvn_rq.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the replay's ID tables (nvn_vk.c's, reduced to what a draw reads: the texture's serial, the sampler's LOD bias) ----
static uint64_t g_tser[64]; static float g_sbias[64]; static int g_tapplied, g_sapplied;
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)v; if (id >= 0 && id < 64) { g_tser[id] = t ? t->serial : 0; g_tapplied++; } }
void mtl_reg_samp(int id, const samp_par *p) { if (id >= 0 && id < 64) { g_sbias[id] = p->lod_bias; g_sapplied++; } }
static int rq_on(void) { return 1; }

// ---- the replay: a held gate, and what each draw saw ----
static pthread_mutex_t g_gmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_gcv = PTHREAD_COND_INITIALIZER; static int g_gate_closed;
enum { NDRAW = 4096 };
static uint64_t g_seen_tex[NDRAW]; static float g_seen_bias[NDRAW]; static uint32_t g_seen_word[NDRAW]; static _Atomic int g_ndraw;
static void gate(int closed) { pthread_mutex_lock(&g_gmu); g_gate_closed = closed; pthread_cond_broadcast(&g_gcv); pthread_mutex_unlock(&g_gmu); }
// a submission: h[0] = texture ID << 32 | sampler ID, or with q set, h[0] = the address of a word to read
static void rq_run(const rq_item *it) {
    pthread_mutex_lock(&g_gmu); while (g_gate_closed) pthread_cond_wait(&g_gcv, &g_gmu); pthread_mutex_unlock(&g_gmu);
    if (it->kind != RQ_SUBMIT) return;
    int d = atomic_fetch_add(&g_ndraw, 1);
    if (d < NDRAW) {
        if (it->q) g_seen_word[d] = *(volatile uint32_t *)(uintptr_t)it->h[0];
        else { int tid = (int)(it->h[0] >> 32), sid = (int)(it->h[0] & 0xffffffff); g_seen_tex[d] = g_tser[tid]; g_seen_bias[d] = g_sbias[sid]; }
    }
    free(it->h);
}
// the game's side, as hle_nvn.c calls the queue
static void reg_tex(int id, uint64_t serial) { tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = serial; rq_reg_tex(id, &t, NULL); }
static void reg_samp(int id, float bias) { samp_par p; memset(&p, 0, sizeof p); p.lod_bias = bias; rq_reg_samp(id, &p); }
static void submit(uint64_t h0, int mem) { uint64_t *c = malloc(sizeof *c); c[0] = h0; rq_push((rq_item){ .kind = RQ_SUBMIT, .n = 1, .h = c, .q = mem ? (void *)1 : NULL }); }
static void draw(int tid, int sid) { submit((uint64_t)tid << 32 | (uint32_t)sid, 0); }
static void *open_later(void *a) { (void)a; usleep(100000); gate(0); return NULL; }

// 1 (and 4 with the switch off): a registration changed between a draw's record and its replay
static void reregister(int order, uint64_t base) {
    g_rq_order = order; int d0 = atomic_load(&g_ndraw);
    reg_tex(7, base + 1); reg_samp(3, 1.0f);
    gate(1); draw(7, 3);
    reg_tex(7, base + 2); reg_samp(3, 2.0f);
    draw(7, 3);
    uint64_t same0 = g_rq_reg_same; reg_tex(7, base + 2); reg_samp(3, 2.0f); uint64_t same = g_rq_reg_same - same0;
    gate(0); rq_drain_now();
    if (order) {
        CHECK(g_seen_tex[d0] == base + 1 && g_seen_bias[d0] == 1.0f, "in order: the first draw sampled the texture and sampler registered when it was recorded (serial %llu, bias %.0f)", (unsigned long long)g_seen_tex[d0], g_seen_bias[d0]);
        CHECK(g_seen_tex[d0 + 1] == base + 2 && g_seen_bias[d0 + 1] == 2.0f, "in order: the second draw sampled the new registration (serial %llu, bias %.0f)", (unsigned long long)g_seen_tex[d0 + 1], g_seen_bias[d0 + 1]);
        CHECK(same == 2, "in order: registering the same bytes again left out of the queue (%llu of 2)", (unsigned long long)same);
        CHECK(g_tser[7] == base + 2 && g_sbias[3] == 2.0f, "in order: after a drain the tables hold the last registration");
    } else
        CHECK(g_seen_tex[d0] == base + 2 && g_seen_bias[d0] == 2.0f, "DM_NVN_ASYNC_ORDER=0 (at once, the old way): the first draw sampled the later registration (serial %llu, bias %.0f): the hole this test finds", (unsigned long long)g_seen_tex[d0], g_seen_bias[d0]);
}
// 2 (and 4): a remap while a draw that reads the page is queued (hle_nvn.c MapVirtual: rq_before_remap, then the pages change)
static void remap(int order) {
    g_rq_order = order; int d0 = atomic_load(&g_ndraw);
    uint32_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) die("mmap");
    page[0] = 0xA1;
    gate(1); submit((uint64_t)(uintptr_t)page, 1);
    pthread_t t; pthread_create(&t, NULL, open_later, NULL);   // (the replay is let go 100 ms later: a remap that does not wait runs first)
    rq_before_remap();
    int waited = atomic_load(&g_ndraw) > d0;
    page[0] = 0xB2;   // the new pages (an unmap: inaccessible; a write here stands for both without faulting the old way)
    pthread_join(t, NULL); rq_drain_now();
    if (order) {
        CHECK(waited && g_seen_word[d0] == 0xA1, "in order: the remap waited for the queued draw (%d), which read the old page (%#x)", waited, g_seen_word[d0]);
        mprotect(page, 4096, PROT_NONE);   // (and an unmap after it faults nothing queued)
    } else
        CHECK(!waited && g_seen_word[d0] == 0xB2, "DM_NVN_ASYNC_ORDER=0: the remap went first and the queued draw read the new page (%#x): the hole this test finds", g_seen_word[d0]);
    munmap(page, 4096);
}
// 3: the replay running alongside the game, each draw after its own registration of the same ID
static void alongside(void) {
    g_rq_order = 1; int d0 = atomic_load(&g_ndraw), n = 2000, bad = 0;
    for (int i = 0; i < n; i++) { reg_tex(9, 5000 + (uint64_t)i); reg_samp(9, (float)i); draw(9, 9); }
    rq_drain_now();
    for (int i = 0; i < n; i++) bad += g_seen_tex[d0 + i] != 5000 + (uint64_t)i || g_seen_bias[d0 + i] != (float)i;
    CHECK(!bad, "in order, the replay alongside: %d draws each sampled the registration made just before it (%d did not)", n, bad);
}
int main(void) {
    reregister(1, 100);
    remap(1);
    alongside();
    reregister(0, 200);
    remap(0);
    printf("%s: %d of %d checks failed (registrations queued %llu, left out as unchanged %llu; applied: textures %d, samplers %d)\n", fails ? "FAIL" : "PASS", fails, checks,
           (unsigned long long)g_rq_reg_n, (unsigned long long)g_rq_reg_same, g_tapplied, g_sapplied);
    return fails != 0;
}

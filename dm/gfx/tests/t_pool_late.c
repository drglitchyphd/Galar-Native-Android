// A memory pool backed late (nvn_vk.c pool_back with late = 1: fix 5 of the 2026-11-24 memory analysis) while other threads write it: the pool's content is copied into
// "driver" memory (a memfd here, as t_pool_pages.c) and that memory moved over the pool's pages, inside nvn_dirty_freeze / nvn_dirty_thaw. Three writer threads keep
// writing their own counters into every page, a fourth keeps protecting the "texture" half (as uploads do), the move is made 300 times; then
//   - no write is lost: every counter in the pool holds the last value its thread wrote (a write between the copy and the move would be gone),
//   - the write tracking still sees every write: each page whose state is "protected" is read-only in /proc/self/maps, each other page writable,
//   - the waits end: the run finishes, and the writers made progress during it.
// The same moves without the freeze run first, to show the counters catch the lost writes (reported, not a failure: a race need not hit).
// Linux (WSL): make t_pool_late && ./t_pool_late   (the fault hook is called from this test's own SIGSEGV handler; crash.c is the phones')
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "../nvn/nvn_dirty.c"

// ---- the host functions nvn_dirty.c uses ----
uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }   // (android/compat/compat.c's)
static int (*g_hook)(void *, int, void *); static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_prep = fn; }

enum { PG = 4096, NPG = 64, POOL = NPG * PG, NW = 3, ROUNDS = 300 };
static uint8_t *g_pool;
static atomic_int g_stop; static _Atomic uint64_t g_wrote[NW], g_gone;
static uint64_t g_seq[NW][NPG];   // each writer's last value per page (its own, read after it stopped)
// (g_gone: a writer found its own counter below what it last wrote there: a write lost in a move, seen when it happens, not only at the end)

static void on_segv(int s, siginfo_t *si, void *uc) {
    (void)s;
    if (g_hook && g_hook(si->si_addr, -1, uc)) return;   // (-1: unknown, taken as a write: as on the phones for pages the tracking protected)
    fprintf(stderr, "t_pool_late: unexpected fault at %p\n", si->si_addr); _exit(99);
}
static void *writer(void *u) {
    int t = (int)(intptr_t)u; unsigned r = 12345u * (unsigned)(t + 1);
    while (!atomic_load(&g_stop)) {
        r = r * 1103515245u + 12345u; int k = (int)((r >> 8) % NPG);
        volatile uint64_t *c = (volatile uint64_t *)(g_pool + (size_t)k * PG + (size_t)t * 64);
        if (*c != g_seq[t][k]) atomic_fetch_add(&g_gone, 1);
        uint64_t v = ++g_seq[t][k];
        *c = v;
        atomic_fetch_add(&g_wrote[t], 1);
    }
    return NULL;
}
static void *protector(void *u) {   // the texture code: the upper half protected again and again (uploads), the writers unprotect it by writing
    (void)u;
    while (!atomic_load(&g_stop)) { nvn_dirty_protect(g_pool + POOL / 2, POOL / 2); usleep(50); }
    return NULL;
}
// One late backing: the pool's content into new "driver" pages, which arrive read-only, moved over the pool; frozen around it when freeze is set
static void back_once(int freeze) {
    int fd = memfd_create("t_pool_late", 0); if (fd < 0 || ftruncate(fd, POOL)) { perror("memfd"); exit(2); }
    uint8_t *p = mmap(NULL, POOL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    if (freeze) nvn_dirty_freeze(g_pool, POOL, 1);
    memcpy(p, g_pool, POOL);
    usleep(200);   // (a slow copy: a large pool on a phone; the writers get their chance)
    if (freeze) mprotect(p, POOL, PROT_READ);
    if (mremap(p, POOL, POOL, MREMAP_MAYMOVE | MREMAP_FIXED, g_pool) != g_pool) { perror("mremap"); exit(2); }
    if (freeze) nvn_dirty_thaw();
    else { mprotect(g_pool, POOL, PROT_READ | PROT_WRITE); nvn_dirty_release(g_pool, POOL); }   // (no freeze: as an unguarded move would leave it)
}
static int run(int freeze, int *lost, int *badprot) {
    memset(g_seq, 0, sizeof g_seq); memset(g_pool, 0, POOL); atomic_store(&g_stop, 0); atomic_store(&g_gone, 0);
    for (int t = 0; t < NW; t++) atomic_store(&g_wrote[t], 0);
    pthread_t w[NW], pr;
    for (int t = 0; t < NW; t++) pthread_create(&w[t], NULL, writer, (void *)(intptr_t)t);
    pthread_create(&pr, NULL, protector, NULL);
    for (int i = 0; i < ROUNDS; i++) back_once(freeze);
    usleep(2000); atomic_store(&g_stop, 1);
    for (int t = 0; t < NW; t++) pthread_join(w[t], NULL);
    pthread_join(pr, NULL);
    *lost = (int)atomic_load(&g_gone);   // (lost while running, and below: at the end)
    for (int t = 0; t < NW; t++) for (int k = 0; k < NPG; k++) if (*(volatile uint64_t *)(g_pool + (size_t)k * PG + (size_t)t * 64) != g_seq[t][k]) (*lost)++;
    // each page's protection against its tracking state
    *badprot = 0; range_t *r = find_span((uintptr_t)g_pool, POOL);
    FILE *m = fopen("/proc/self/maps", "r"); char ln[512]; char want[NPG], got[NPG]; memset(got, '?', sizeof got);
    for (int k = 0; k < NPG; k++) want[k] = r && atomic_load(&r->state[((uintptr_t)g_pool + (size_t)k * PG - r->base) >> g_shift]) == PG_PROT ? 'r' : 'w';
    while (m && fgets(ln, sizeof ln, m)) { unsigned long s0, e0; char pr_[8] = "";
        if (sscanf(ln, "%lx-%lx %7s", &s0, &e0, pr_) != 3) continue;
        for (int k = 0; k < NPG; k++) { uintptr_t a = (uintptr_t)g_pool + (size_t)k * PG; if (a >= s0 && a < e0) got[k] = pr_[1] == 'w' ? 'w' : 'r'; } }
    if (m) fclose(m);
    for (int k = 0; k < NPG; k++) if (want[k] != got[k]) (*badprot)++;
    uint64_t sum = 0; for (int t = 0; t < NW; t++) sum += atomic_load(&g_wrote[t]);
    return (int)(sum > (uint64_t)INT32_MAX ? INT32_MAX : sum);
}
int main(void) {
    struct sigaction sa = { 0 }; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    g_pool = mmap(NULL, POOL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    nvn_dirty_add_range(g_pool, POOL);
    int lost, bad, fails = 0;
    int n = run(0, &lost, &bad);
    printf("without the freeze: %d writes, %d lost writes (%d counters), %d pages with the wrong protection (shows what the freeze is for; not a failure)\n", n, lost, NW * NPG, bad);
    n = run(1, &lost, &bad);
    printf("with the freeze:    %d writes, %d lost writes (%d counters), %d pages with the wrong protection\n", n, lost, NW * NPG, bad);
    if (lost) { fails++; printf("FAIL: writes lost across a late backing\n"); }
    if (bad) { fails++; printf("FAIL: a page's protection does not match its tracking state (a protected page writable: its writes go unseen)\n"); }
    if (n < ROUNDS) { fails++; printf("FAIL: the writers barely ran (%d writes)\n", n); }
    printf("t_pool_late: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}

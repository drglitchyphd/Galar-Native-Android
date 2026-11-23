// Giving a memory pool's pages back to the game (nvn_vk.c, mtl_pool_del) without a moment where another thread finds them unmapped or zeroed.
// The game's allocator hands out memory next to and inside pools on other threads while a pool is finalized: Pokémon Shield's file loader crashed writing a heap
// block header into a page that was unmapped at that moment (#54, Xclipse 940, where every give-back took the slow path). The old way mapped fresh zero pages over
// the range and copied the content in afterwards (milliseconds for a large pool); now a private copy is made first and moved over the range in one mremap, which
// holds the address space's lock throughout. Test: dm/gfx/tests/t_pool_pages.c
#ifndef POOL_PAGES_H
#define POOL_PAGES_H
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#ifdef __linux__
#include <sys/uio.h>
#include <unistd.h>
#endif

// The one moment left: between the driver's pages leaving the range and the copy arriving (two mremaps back to back; no single call does both). The range is marked
// for it (pp_begin / pp_end), and a fault inside a marked range, or one that ended less than 100 ms ago, waits 50 µs and runs the faulting instruction again
// (pp_fault, called from the page fault hooks: dm_fault_hook_register). A fault anywhere else is left to the crash handler as before
typedef struct { _Atomic uintptr_t lo, hi; _Atomic int64_t end_ns; } pp_range;   // end_ns 0: still going
static pp_range g_pp[16]; static atomic_uint g_pp_next;
static int64_t pp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int pp_begin(uintptr_t lo, size_t size) {
    int s = (int)(atomic_fetch_add(&g_pp_next, 1) & 15);
    atomic_store(&g_pp[s].end_ns, 0); atomic_store(&g_pp[s].hi, lo + size); atomic_store(&g_pp[s].lo, lo);
    return s;
}
static void pp_end(int s) { atomic_store(&g_pp[s].end_ns, pp_now()); }
static int pp_fault(void *addr) {   // async-signal-safe: atomics, clock_gettime and nanosleep only
    uintptr_t a = (uintptr_t)addr; int64_t now = 0;
    for (int s = 0; s < 16; s++) {
        uintptr_t lo = atomic_load(&g_pp[s].lo), hi = atomic_load(&g_pp[s].hi);
        if (!lo || a < lo || a >= hi) continue;
        int64_t end = atomic_load(&g_pp[s].end_ns); if (end && !now) now = pp_now();
        if (end && now - end > 100000000) continue;
        struct timespec w = { 0, 50000 }; nanosleep(&w, NULL);
        return 1;
    }
    return 0;
}

// a private copy of the pages at [base, base+size), for pages_back (MAP_FAILED when there is no memory for it).
// Only the parts /proc/self/maps shows mapped readable are copied: Turnip (Adreno 660, #281) had taken back pages moved under a pool by the time the pool was freed,
//   and the plain memcpy here crashed reading them (SIGSEGV in memcpy, mtl_pool_del, after minutes of play). What is not mapped readable stays zero in the copy:
//   the pool is being freed, the game only gets its memory back. pages_lost counts it (said in the log by the caller). (process_vm_readv, tried first, cannot read
//   the driver's own mappings at all, VM_PFNMAP: on Adreno's own driver it gave readable pools back as zeros, S25+ 2026-11-19)
static _Atomic uint64_t pages_lost;
static void *pages_copy(uintptr_t base, size_t size) {
    void *c = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (c == MAP_FAILED) return c;
#ifdef __linux__
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) { memcpy(c, (void *)base, size); return c; }   // (no map to look at: the plain copy, as before)
    char ln[512]; size_t covered = 0; uintptr_t end = base + size;
    while (fgets(ln, sizeof ln, m)) { unsigned long s0, e0; char pr[8] = "";
        if (sscanf(ln, "%lx-%lx %7s", &s0, &e0, pr) != 3 || e0 <= base || s0 >= end || pr[0] != 'r') continue;
        uintptr_t a = s0 > base ? s0 : base, b = e0 < end ? e0 : end;
        memcpy((uint8_t *)c + (a - base), (void *)a, b - a); covered += b - a; }
    fclose(m);
    if (covered < size) atomic_fetch_add(&pages_lost, size - covered);
#else
    memcpy(c, (void *)base, size);
#endif
    return c;
}
// [base, base+size) becomes the copy (ordinary memory, readable and writable), whatever was mapped there; the copy's own address is gone afterwards.
// Returns 1 when it went in one step, 0 when the fallback (fresh pages, then the content) was needed
static int pages_back(uintptr_t base, size_t size, void *copy) {
#ifdef __linux__
    if (mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)base) == (void *)base) return 1;
#endif
    mmap((void *)base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    memcpy((void *)base, copy, size); munmap(copy, size);
    return 0;
}
#endif

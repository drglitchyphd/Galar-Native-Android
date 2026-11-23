// What the NVN replay layer (nvn_vk.c, nvn_dirty.c) takes from the game host (dm/host, hle_nvn.c, the Android front end), for PC tools that run the replay layer
// without a game (dm/pc/Makefile: the golden-frame player and recorder test, dm/gfx/golden). Everything is weak: a tool brings its own where it needs another.
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include "nvn_obj.h"

#define W __attribute__((weak))
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
W uint64_t guest_ns(void) { return mono(); }
W uint64_t now_ns(void) { return mono(); }
// Serials the replay makes itself (combined programs) start far above any a recording brings from the phone
W uint64_t nvn_new_serial(void) { static _Atomic uint64_t n = 1ull << 48; return ++n; }
W int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) { (void)p; (void)stage; (void)key; return 0; }
W void *front_metal_layer(void) { return NULL; }   // no window: presents go nowhere, screenshots on request
W void touch_ui_demo(int c, float dx, float dy) { (void)c; (void)dx; (void)dy; }
_Atomic uint64_t g_wait_sync_ns W, g_wait_sync_n W;

// Page faults on write-protected guest pages (nvn_dirty.c's texture tracking): the registered hooks first, else the default action
static int (*g_fault_hooks[8])(void *addr, int is_write, void *uc); static int g_nfault;
static void on_fault(int sig, siginfo_t *si, void *ucv) {
    int wr = 1;
#if defined(__x86_64__)
    ucontext_t *uc = ucv; wr = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#endif
    for (int i = 0; i < g_nfault; i++) if (g_fault_hooks[i](si->si_addr, wr, ucv)) return;
    signal(sig, SIG_DFL); raise(sig);
}
W void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault < 8) g_fault_hooks[g_nfault++] = fn;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}
// Host writes into guest memory: the tracking unprotects (and marks written) the pages first
static void (*g_prep)(void *addr, size_t len);
W void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_prep = fn; }
W void dm_guest_write_prepare(void *addr, size_t len) { if (g_prep) g_prep(addr, len); }
// (hle_nvn.c is not part of the PC replay: DM_GPU_TS=614, the Switch GPU's tick rate for counter reports, off here)
W int dm_gpu_ts614(void) { return 0; }

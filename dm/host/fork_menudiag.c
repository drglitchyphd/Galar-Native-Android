// FORK v62: menu touch, step 1: a log-only look at how Sword's menus are built (DM_FORK_MENU_DIAG=on, Tweaks, default off). Nothing is changed.
//   Upstream's menu touch (android/app/jni/touch_menu.c, for Legends: Arceus) needs the game's ui2d pane classes (their vtables in main) and the pane fields; nobody
//   has published them for Sword. So, 3 s into a full-screen menu (gfx/nvn/fork_ui.h screen kind 1, up to 4 times a run): every "RootPane" name in the heap
//   (each ui2d layout's root pane) gives the name field's offset (the object start is the nearest word before it pointing into main); then every heap object
//   with a vtable in main and a pane-like name at that offset is counted by class. Logged ([FORK] menu-diag): the name offset, the classes (main + offset,
//   count, sample names), the cursor-like panes, and the first words of a root pane and a cursor pane (to check the position / size / flag offsets)
#include "hle.h"
#include <stdatomic.h>
uintptr_t dm_fork_heap_base(void);   // hle_svc.c (FORK)
size_t dm_fork_heap_size(void);      // hle_svc.c (FORK)
extern _Atomic int dm_fork_screen __attribute__((weak)); extern volatile int dm_fork_census_want __attribute__((weak));   // gfx/nvn/fork_ui.h

static uintptr_t g_mb, g_me;   // main's image
static int in_main(uint64_t v) { return v >= g_mb && v < g_me && !(v & 7); }
static int pane_name(const char *s) {   // RootPane, or <Letter>[letter]_<name>: N_ P_ T_ W_ L_ B_ ... (ui2d naming), up to 24 printable characters
    if (!strncmp(s, "RootPane", 9)) return 1;
    int i = 0; if (s[0] < 'A' || s[0] > 'Z') return 0;
    for (; i < 24 && s[i]; i++) if (s[i] < 0x21 || s[i] > 0x7e) return 0;
    if (i < 3 || i >= 24 || s[i]) return 0;
    return s[1] == '_' || (s[2] == '_' && s[1] >= 'a' && s[1] <= 'z');
}
static void dump(const char *what, const uint8_t *o, int n) {
    for (int i = 0; i < n; i += 32) { char ln[200]; int k = snprintf(ln, sizeof ln, "[FORK] menu-diag %s +%#04x:", what, i);
        for (int j = 0; j < 32 && i + j < n; j += 4) { uint32_t w; memcpy(&w, o + i + j, 4); k += snprintf(ln + k, sizeof ln - (size_t)k, " %08x", w); }
        LOG("%s", ln); }
}
static void *run(void *arg) {
    (void)arg; uintptr_t b = dm_fork_heap_base(); uint64_t lim = dm_fork_heap_size(), t0 = now_ns();
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_mb = (uintptr_t)g_mod[i].base; g_me = g_mb + g_mod[i].size; }
    if (!b || !g_mb) return NULL;
    // 1. the name offset from the root panes
    static int offc[64]; memset(offc, 0, sizeof offc); int roots = 0; uint64_t root_obj = 0;
    for (uint64_t off = 0x1000; off + 16 <= lim; off += 8) {
        const uint8_t *p = (const uint8_t *)(b + off); if (p[0] != 'R' || memcmp(p, "RootPane", 9)) continue; roots++;
        for (int k = 8; k < 64; k++) { if ((uint64_t)k * 8 > off) break; uint64_t v = *(const uint64_t *)(p - k * 8); if (in_main(v)) { offc[k]++; if (!root_obj) root_obj = off - (uint64_t)k * 8; break; } } }
    int best = 0; for (int k = 8; k < 64; k++) if (offc[k] > offc[best]) best = k;
    int noff = best * 8;
    LOG("[FORK] menu-diag: %d RootPane names in memory; the object start (a word pointing into main) most often %#x before the name (%d of them) (%.1f s)", roots, noff, offc[best], (now_ns() - t0) / 1e9);
    if (!noff) return NULL;
    // 2. the pane classes: objects with a vtable in main and a pane name at +noff
    enum { NV = 256 }; static struct { uint64_t vt; int n; char s[3][25]; uint64_t obj; } vt[NV]; int nv = 0, total = 0, cur = 0; uint64_t cur_obj = 0;
    memset(vt, 0, sizeof vt);
    for (uint64_t off = 0x1000; off + (uint64_t)noff + 32 <= lim; off += 8) {
        uint64_t v = *(const uint64_t *)(b + off); if (!in_main(v)) continue;
        const char *nm = (const char *)(b + off + (uint64_t)noff); if (!pane_name(nm)) continue; total++;
        int i = 0; while (i < nv && vt[i].vt != v) i++;
        if (i == nv) { if (nv == NV) continue; nv++; vt[i].vt = v; vt[i].obj = off; }
        if (vt[i].n < 3) snprintf(vt[i].s[vt[i].n], 25, "%s", nm); vt[i].n++;
        if (strstr(nm, "ursor") || strstr(nm, "elect")) { cur++; if (!cur_obj) cur_obj = off; if (cur <= 12) LOG("[FORK] menu-diag: cursor-like pane %s at heap + %#llx, class main + %#llx", nm, (unsigned long long)off, (unsigned long long)(v - g_mb)); } }
    for (int i = 1; i < nv; i++) for (int j = i; j > 0 && vt[j].n > vt[j - 1].n; j--) { __typeof__(vt[0]) t = vt[j]; vt[j] = vt[j - 1]; vt[j - 1] = t; }
    LOG("[FORK] menu-diag: %d pane-like objects in %d classes, %d cursor-like (%.1f s)", total, nv, cur, (now_ns() - t0) / 1e9);
    for (int i = 0; i < nv && i < 14; i++) LOG("[FORK] menu-diag: class main + %#llx: %d (%s, %s, %s)", (unsigned long long)(vt[i].vt - g_mb), vt[i].n, vt[i].s[0], vt[i].s[1], vt[i].s[2]);
    if (root_obj) dump("root pane", (const uint8_t *)(b + root_obj), noff + 16);
    if (cur_obj) dump("cursor pane", (const uint8_t *)(b + cur_obj), noff + 32);
    return NULL;
}
void dm_fork_menudiag_tick(void) {
    static int on = -1, runs, held; static uint64_t n;
    if (on < 0) { const char *e = getenv("DM_FORK_MENU_DIAG"); on = e && !strcmp(e, "on"); }
    if (!on || runs >= 4 || ++n % 30) return;   // (once a second)
    if (&dm_fork_census_want) dm_fork_census_want = 1;   // (the screen kind is needed)
    int k = &dm_fork_screen ? atomic_load(&dm_fork_screen) : 0;
    held = k == 1 ? held + 1 : 0;
    if (held == 3) { runs++; LOG("[FORK] menu-diag: a menu has shown 3 s: looking (%d of 4)", runs); pthread_t t; if (!pthread_create(&t, NULL, run, NULL)) pthread_detach(t); }
}

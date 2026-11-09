// FORK: Picture "Fill, corrected" (DM_ASPECT=fix). The 16:9 picture is stretched over the whole screen; this file sets the game's camera to the screen's own ratio so the 3D
// looks right after the stretch, and tells the graphics layer how much to shrink the UI so it keeps its shape (gfx/nvn/fork_ui.h).
// Camera: the sites of the "4.3 Ultrawide v1.3.2" mod for Pokémon Sword 1.3.2 (Fl4sh, updated by TLin-Y), written from here with the ratio of the screen in use and
// written again when the screen changes (fold / unfold), on the game's thread at each present. Original instructions are kept and put back when the option is off.
//   0x6076c4  b 0x607664               jump into a cave
//   0x607664  movz w9, #lo / movk w9, #hi, lsl 16 / b 0x6076c8   the ratio as a float, both halves (exact)
//   0x6076c4  originally ldr w9, [x1, #0xd4]: the camera's own aspect field, read here
//   0x607de8  movk x9, #lo, lsl 32 / movk x9, #hi, lsl 48   a second aspect: the game builds 16/9 (0x3fe38e39) in x9's high half here. The mod meant 4/3 but its
//             second word has a typo (0x67aa for 0x3faa: a huge ratio); the fork writes the screen's ratio. DM_FORK_ASPECT_X9=0 leaves it alone
//   0xd93414  the mod writes movk x12 here, but the game has ldp x12, x13, [x0, #0xd0] (log, Nov 9): not an aspect constant, always left alone
// At the first look the game's code is also searched for other places that build 16/9 (movk ..., #0x3fe3) and 16/9 words in its data, logged for finding more sites
// A site whose original instruction is not what the mod changed, or that the code rewriter turned into a branch, is left alone (logged). With the mod itself switched on,
// nothing here runs (its jump is already there).
#include "hle.h"
#include "../gfx/nvn/nvn_ovl.h"
#include <math.h>
#include <stdatomic.h>
#include <sys/mman.h>

static _Atomic uint32_t g_win;   // the window in landscape, width << 16 | height (gfx/nvn/nvn_vk.c swap_ensure_now)
void dm_fork_window(uint32_t w, uint32_t h) { atomic_store(&g_win, (w & 0xffff) << 16 | (h & 0xffff)); }
static double win_ratio(void) { uint32_t p = atomic_load(&g_win), w = p >> 16, h = p & 0xffff; return w && h ? (double)w / h : 0; }
// the ratio the picture is corrected for, 0 when it is not (another Picture setting, or a screen within 2 % of 16:9)
static double fix_ratio(void) {
    if (dm_aspect() != DM_ASPECT_FIX) return 0;
    double r = win_ratio(); return r > 0 && fabs(r / (16.0 / 9.0) - 1) > 0.02 ? r : 0;
}
// gfx/nvn/fork_ui.h and android/app/jni/touch_menu.c: the UI's scale in x and y (1, 1 = none)
void dm_fork_ui_scale(float *kx, float *ky) {
    *kx = *ky = 1; double r = fix_ratio(); if (!r) return;
    if (r < 16.0 / 9.0) *ky = (float)(r / (16.0 / 9.0)); else *kx = (float)(16.0 / 9.0 / r);
}

enum { S_JUMP, S_CAVE0, S_CAVE1, S_CAVE2, S_X12, S_X9A, S_X9B, NS };
static const uint64_t k_off[NS] = { 0x6076c4, 0x607664, 0x607668, 0x60766c, 0xd93414, 0x607de8, 0x607dec };
static const char *const k_sword = "a3b75bcd3311385aeed67fbeeb79cbb7";
static uint32_t *g_at[NS], g_orig[NS];
static int g_state = -2;   // -2 not looked up yet, -1 not usable here, 0 ready
static int g_ok_cave, g_ok_x12, g_ok_x9;
static double g_applied;   // the ratio written (0 = the originals are in place)
static int g_x9_on;

static int is_branch_into_patch(const module_t *m, uint64_t off, uint32_t w) {   // (loader.c overlaps_rewrite: the rewriter's branches into its patch area)
    if ((w & 0xfc000000u) != 0x14000000u) return 0;
    int64_t t = (int64_t)off + ((int64_t)(int32_t)(w << 6) >> 4), p0 = m->patch_off, p1 = m->patch_off < 0 ? m->patch_off + (int64_t)m->patch_len : (int64_t)m->size;
    return t >= p0 && t < p1;
}
static int find(void) {
    if (g_state != -2) return g_state;
    module_t *m = NULL; int seen = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && g_mod[i].build_id[0]) { seen = 1; if (!strcasecmp(g_mod[i].build_id, k_sword)) m = &g_mod[i]; }
    if (!seen) return -2;   // main not loaded yet: asked again later
    if (!m) { g_state = -1; return -1; }   // (Shield, another version: Fill, corrected stays a plain fill)
    for (int i = 0; i < NS; i++) { g_at[i] = (uint32_t *)(m->base + k_off[i]); g_orig[i] = *g_at[i]; }
    LOG("[FORK] aspect sites: jump %08x, cave %08x %08x %08x, x12 %08x, x9 %08x %08x", g_orig[S_JUMP], g_orig[S_CAVE0], g_orig[S_CAVE1], g_orig[S_CAVE2], g_orig[S_X12], g_orig[S_X9A], g_orig[S_X9B]);
    if (g_orig[S_JUMP] == 0x17ffffe8u) { LOG("[FORK] [RED] the 4:3 mod is switched on: Fill, corrected leaves the camera to it (switch the mod off to use it)"); g_state = -1; return -1; }
    int bad = 0; for (int i = 0; i < NS; i++) if (is_branch_into_patch(m, k_off[i], g_orig[i])) { LOG("[FORK] [RED] aspect site main+%#llx was rewritten: left alone", (unsigned long long)k_off[i]); bad |= 1 << i; }
    g_ok_cave = !(bad & (1 << S_JUMP | 1 << S_CAVE0 | 1 << S_CAVE1 | 1 << S_CAVE2));
    g_ok_x12 = !(bad & 1 << S_X12) && (g_orig[S_X12] & 0xffe0001fu) == 0xf2a0000cu;   // movk x12, #imm, lsl 16
    g_ok_x9 = !(bad & (1 << S_X9A | 1 << S_X9B)) && (g_orig[S_X9A] & 0xffe0001fu) == 0xf2c00009u && (g_orig[S_X9B] & 0xffe0001fu) == 0xf2e00009u;   // movk x9 lsl 32 / 48
    if (!g_ok_x12) LOG("[FORK] aspect site x12 is not an aspect constant (%08x): left alone, as it should be", g_orig[S_X12]);
    if (!g_ok_x9) LOG("[FORK] aspect site x9 is not the expected instruction (%08x %08x): left alone", g_orig[S_X9A], g_orig[S_X9B]);
    // where else the game builds or keeps 16/9: for finding the sites still missing (a camera that stays squashed)
    { int n = 0; uint32_t *t = (uint32_t *)m->base; uint64_t words = m->text_end / 4;
      for (uint64_t i = 0; i < words; i++) { uint32_t w = t[i];
          if ((w & 0x7fffffe0u) == 0x72e7fc60u || (w & 0x7fffffe0u) == 0x72a7fc60u) {   // movk w/x, #0x3fe3, lsl 16 or lsl 48 (16/9's high half)
              if (n < 40) LOG("[FORK] 16:9 built at main+%#llx: %08x (before it %08x %08x)", (unsigned long long)(i * 4), w, i > 1 ? t[i - 2] : 0, i ? t[i - 1] : 0); n++; } }
      int k = 0; uint64_t lo = m->ro_off & ~3ull, hi = m->data_off ? m->data_off : m->size;
      for (uint64_t o = lo; o + 4 <= hi && o + 4 <= m->size; o += 4) { uint32_t w; memcpy(&w, m->base + o, 4);
          if (w == 0x3fe38e39u) { if (k < 20) LOG("[FORK] 16:9 word at main+%#llx", (unsigned long long)o); k++; } }
      LOG("[FORK] 16:9 in main: built in %d places, kept as a word in %d", n, k); }
    g_state = g_ok_cave || g_ok_x9 ? 0 : -1;
    return g_state;
}
// one instruction into the game's code while it runs: its page writable and executable for the moment of the store (other threads may be running in it)
static int put(uint32_t *p, uint32_t w) {
    if (*p == w) return 1;
    uintptr_t pg = (uintptr_t)p & ~(uintptr_t)0x3fff;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC)) { LOG("[FORK] [RED] aspect: code page not writable (%s)", strerror(errno)); return 0; }
    __atomic_store_n(p, w, __ATOMIC_RELEASE);
    mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)(p + 1));
    return 1;
}
static void write_ratio(double r) {
    float f = (float)r; uint32_t b; memcpy(&b, &f, sizeof b);
    uint32_t lo = b & 0xffff, hi = b >> 16, hr = (b + 0x8000) >> 16;
    const char *x9 = getenv("DM_FORK_ASPECT_X9"); int want_x9 = x9 && *x9 ? atoi(x9) != 0 : 1;
    int ok = 1;
    if (g_ok_cave) {   // the cave before the jump into it
        ok &= put(g_at[S_CAVE0], 0x52800009u | lo << 5); ok &= put(g_at[S_CAVE1], 0x72a00009u | hi << 5); ok &= put(g_at[S_CAVE2], 0x14000017u);
        ok &= put(g_at[S_JUMP], 0x17ffffe8u); }
    if (g_ok_x12) ok &= put(g_at[S_X12], (g_orig[S_X12] & ~(0xffffu << 5)) | hr << 5);
    if (g_ok_x9) { if (want_x9) { ok &= put(g_at[S_X9A], (g_orig[S_X9A] & ~(0xffffu << 5)) | lo << 5); ok &= put(g_at[S_X9B], (g_orig[S_X9B] & ~(0xffffu << 5)) | hi << 5); }
                   else { ok &= put(g_at[S_X9A], g_orig[S_X9A]); ok &= put(g_at[S_X9B], g_orig[S_X9B]); } }
    g_x9_on = g_ok_x9 && want_x9;
    LOG("[FORK] aspect: camera %.4f (%08x)%s%s, UI scale for a %s screen%s", r, b, g_ok_cave ? "" : ", cave left alone", g_ok_x12 ? "" : ", x12 left alone",
        r < 16.0 / 9.0 ? "narrower" : "wider", g_x9_on ? "; x9 written" : "");
    if (!ok) LOG("[FORK] [RED] aspect: some sites could not be written");
}
static void restore(void) {
    if (g_ok_cave) { put(g_at[S_JUMP], g_orig[S_JUMP]); put(g_at[S_CAVE0], g_orig[S_CAVE0]); put(g_at[S_CAVE1], g_orig[S_CAVE1]); put(g_at[S_CAVE2], g_orig[S_CAVE2]); }   // (the jump out first)
    if (g_ok_x12) put(g_at[S_X12], g_orig[S_X12]);
    if (g_ok_x9) { put(g_at[S_X9A], g_orig[S_X9A]); put(g_at[S_X9B], g_orig[S_X9B]); }
    LOG("[FORK] aspect: camera back to the game's own");
}
// gfx/nvn/hle_nvn.c, every present on the game's thread
void dm_fork_aspect_tick(void) {
    double r = fix_ratio();
    if (!r && !g_applied) return;
    if (find() != 0) return;
    if (r && fabs(r - g_applied) > 1e-4) { write_ratio(r); g_applied = r; }
    else if (!r && g_applied) { restore(); g_applied = 0; }
}

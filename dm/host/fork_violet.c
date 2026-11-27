// FORK (Paldea fork): Picture "Fill, corrected" (DM_ASPECT=fix) for Pokémon Violet 4.0.0 (main 709bfd66...). The 16:9 picture is stretched over the whole screen, in
//   landscape and in portrait; the game itself is changed so the result keeps its shape: the 3D projection's vertical scale (or horizontal, on a screen wider than
//   16:9) times k = screen ratio / (16/9), and the UI's layout rectangles squeezed to a centred 16:9 box. The renderer's UI shrink (gfx/nvn/fork_ui.h, Sword's way)
//   stays off here (dm_fork_ui_scale gives 1, 1). From the Violet IPS mods worked out with Steven (Nov 23, his main's slices; scratch notes, not in the repo):
//   - 0x01526D64  stp x9, x11, [sp, #0x50] -> NOP: the callback at 0x01760EC8 is never registered (Fl4sh9174's 21:9 mod does the same), so its body is free
//   - 0x0192BD34  ldp s2, s3, [x8, #0xc] -> b 0x01760F98: the UI rect lookup (0x0192BCB4: {u32 id; float x, y, w, h}) returns its rect squeezed (k < 1: h k,
//                 y + h (1 - k) / 2; k > 1: w / k, x + (w - w / k) / 2)
//   - 0x00CF12C8  fdiv s4, s1, s16 -> b 0x01760FE4: the projection's x scale (cot / aspect) as before, then the y scale (v1 lane 0) times k (k < 1), or x / k
//   - 0x01761010  the screen's ratio as a float (16/9 = no change), read by both; written again on the game's thread at each present when the screen changes
//                 (fold / unfold, turned), like fork_aspect.c's camera word
// The code is written at load, before it is sealed (loader.c builtin_patches), and only when the player chose Fill, corrected (to or from it: from the next start).
// Every original word is checked first; a site the code rewriter turned into a branch, or another game version, leaves the game as shipped. Logs "[FORK] violet".
#include "hle.h"
#include "../gfx/nvn/nvn_ovl.h"
#include <math.h>
#include <stdatomic.h>
#include <sys/mman.h>

static const char *const k_violet = "709bfd66115298640155fcc4979dba15";
enum { O_NOP = 0x01526D64, O_UI = 0x0192BD34, O_3D = 0x00CF12C8, O_CAVE = 0x01760F98, O_3DCAVE = 0x01760FE4, O_LIT = 0x01761010 };
static const uint32_t k_cave[31] = {   // assembled at 0x01760F98 (llvm, checked by disassembly; v3 of the IPS mod)
    0x2d418d02, 0x1c0003be, 0x1e2c501f, 0x1e3f0bde, 0x1e2e101f, 0x1e3f23c0, 0x540000ec, 0x1e3e3bff, 0x1e230bff, 0x1e2c101d, 0x1f1d07e1, 0x1e3e0863, 0x14000006,
    0x1e3e185d, 0x1e3d385f, 0x1e2c101c, 0x1f1c03e0, 0x1e2043a2, 0x14072b56,   // UI: ... b 0x0192bd38
    0x1e301824, 0x1c00015e, 0x1e2c501f, 0x1e3f0bde, 0x1e2e101f, 0x1e3f23c0, 0x5400006c, 0x4f9e9021, 0x17d640b2, 0x1e3e1884, 0x17d640b0,   // 3D: ... b 0x00cf12cc
    0x3fe38e39 };   // the ratio, 16/9 as written
static _Atomic int g_on;   // the code is in (this run)
static uint32_t *g_lit; static float g_lit_now = 16.0f / 9.0f;

int dm_fork_violet_active(void) { return atomic_load(&g_on); }
static uint32_t b_to(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)to - (int64_t)from) >> 2 & 0x3ffffff); }

// loader.c builtin_patches, Scarlet / Violet 4.0.0's main, before the code is sealed. ov: loader.c overlaps_rewrite. 1 when written
int dm_fork_violet_load(module_t *m, int (*ov)(const module_t *, uint64_t, size_t, int64_t, int64_t), int64_t p0, int64_t p1) {
    const char *a = getenv("DM_ASPECT"); if (!a || strcmp(a, "fix")) return 0;
    if (strncasecmp(m->build_id, k_violet, 32)) { LOG("[FORK] violet: Fill, corrected is for Violet 4.0.0 only (main %s): the picture is only stretched", m->build_id); return 0; }
    struct { uint64_t off; uint32_t want; } chk[] = { { O_NOP, 0xa9052fe9u }, { O_UI, 0x2d418d02u }, { O_3D, 0x1e301824u } };
    for (unsigned i = 0; i < sizeof chk / sizeof *chk; i++) { uint32_t w; memcpy(&w, m->base + chk[i].off, 4);
        if (w != chk[i].want || ov(m, chk[i].off, 4, p0, p1)) { LOG("[FORK] [RED] violet: main+%#llx is %08x, not %08x (a mod there, or rewritten): Fill, corrected left out", (unsigned long long)chk[i].off, w, chk[i].want); return 0; } }
    if (ov(m, O_CAVE, sizeof k_cave, p0, p1)) { LOG("[FORK] [RED] violet: the callback's body at main+%#x was rewritten: Fill, corrected left out", O_CAVE); return 0; }
    uint32_t nop = 0xd503201fu, bu = b_to(O_UI, O_CAVE), b3 = b_to(O_3D, O_3DCAVE);
    memcpy(m->base + O_CAVE, k_cave, sizeof k_cave);
    memcpy(m->base + O_NOP, &nop, 4); memcpy(m->base + O_UI, &bu, 4); memcpy(m->base + O_3D, &b3, 4);
    __builtin___clear_cache((char *)(m->base + O_CAVE), (char *)(m->base + O_CAVE + sizeof k_cave));
    for (uint64_t o = 0; o < 3; o++) { uint64_t s = o == 0 ? O_NOP : o == 1 ? O_UI : O_3D; __builtin___clear_cache((char *)(m->base + s), (char *)(m->base + s + 4)); }
    g_lit = (uint32_t *)(m->base + O_LIT); atomic_store(&g_on, 1);
    LOG("[FORK] violet: Fill, corrected in (3D view and UI follow the screen's shape; written again when it changes)");
    return 1;
}
static double win_ratio(void) { extern double dm_fork_win_ratio(void); return dm_fork_win_ratio(); }
// each present (hle_nvn.c): the screen's ratio into the code's word when it changed (the code page writable for the moment of the store)
void dm_fork_violet_tick(void) {
    if (!atomic_load(&g_on) || dm_aspect() != DM_ASPECT_FIX) return;
    double r = win_ratio(); if (!(r > 0.3 && r < 4.0)) return;
    float f = (float)r; if (fabsf(f - g_lit_now) < 0.0005f) return;
    uintptr_t pg = (uintptr_t)g_lit & ~(uintptr_t)0x3fff;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC)) { static int said; if (!said++) LOG("[FORK] [RED] violet: code page not writable (%s)", strerror(errno)); return; }
    uint32_t w; memcpy(&w, &f, 4); __atomic_store_n(g_lit, w, __ATOMIC_RELEASE);
    mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)g_lit, (char *)g_lit + 4);
    LOG("[FORK] violet: screen ratio %.4f (k %.4f): %s", r, r / (16.0 / 9.0), r < 16.0 / 9.0 ? "UI kept 16:9 with bars above and below, more seen up and down" : "UI kept 16:9 with bars at the sides, more seen sideways");
    g_lit_now = f;
}

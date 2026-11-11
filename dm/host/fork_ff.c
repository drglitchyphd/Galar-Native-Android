// FORK: fast-forward. Pokémon Sword 1.3.2 advances its world by a fixed time step per frame, taken from a table of nanoseconds by present interval
// (main+0x203eeb8 holds the 30 fps entry, 33333334: loader.c FPS_SITES, the same word the 60 fps patch rewrites). Writing a larger value makes each
// frame advance more game time: the game runs faster at the same 30 frames a second (animations skip ahead rather than play smoother).
// dm_fork_ff_req (touch_ui.c: the fast-forward button, tapped = on / off, held = while held; only where the setting allows) asks for it; dm_fork_ff_speed is the
// speed in % (DM_FORK_FF_SPEED 110-400, default 150). Applied on the game's thread at each present (gfx/nvn/hle_nvn.c); the original word is put back when it
// ends. When upstream's frame step rewrites that word every frame itself (dm_step_word: the live 30 / 60 frame rate or the 60 fps mode), its step is scaled
// instead (dm_fork_ff_pct). Left alone when the word is not the expected one.
#include "hle.h"
#include <stdatomic.h>

volatile int dm_fork_ff_req, dm_fork_ff_speed;
volatile int dm_fork_ff_pct = 100;   // the speed in % while the frame step owns the word (hle_nvn.c vstep_tick multiplies its step by it)
extern int dm_step_hz __attribute__((weak)); extern uint8_t *dm_step_word __attribute__((weak));   // loader.c (the 60 fps patch)

static const char *const k_sword = "a3b75bcd3311385aeed67fbeeb79cbb7";
static uint32_t *g_word; static int g_state = -2;   // -2 not looked up, -1 not usable, 0 ready
static const uint32_t k_orig = 33333334u;

static int find(void) {
    if (g_state != -2) return g_state;
    int seen = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && g_mod[i].build_id[0]) {
        seen = 1;
        if (strcasecmp(g_mod[i].build_id, k_sword)) continue;
        uint32_t *w = (uint32_t *)(g_mod[i].base + 0x203eeb8);
        if (0x203eeb8 < g_mod[i].text_end) { LOG("[FORK] [RED] fast-forward: the step word lies in code: off"); g_state = -1; return -1; }
        if (*w != k_orig) { LOG("[FORK] [RED] fast-forward: unexpected step word %u: off", *w); g_state = -1; return -1; }
        g_word = w; g_state = 0; return 0;
    }
    if (seen) g_state = -1;
    return g_state;
}

// gfx/nvn/hle_nvn.c, every present
void dm_fork_ff_tick(void) {
    static int on, warned;
    int want = dm_fork_ff_req != 0;
    if (!want && !on) return;
    if (&dm_step_word && dm_step_word) {   // upstream's frame step owns the word (the live 30 / 60 frame rate, on by default since upstream's Nov 9 change; or the 60 fps mode):
        //   it scales its own step by dm_fork_ff_pct (gfx/nvn/hle_nvn.c vstep_tick)
        int sp = dm_fork_ff_speed; if (sp <= 0) { const char *e = getenv("DM_FORK_FF_SPEED"); sp = e ? atoi(e) : 150; }
        if (sp < 110) sp = 110; if (sp > 400) sp = 400;
        int pct = want ? sp : 100;
        if (dm_fork_ff_pct != pct) { static int lines; if (lines++ < 200) LOG("[FORK] fast-forward %s (%d %%, through the frame step)", want ? "on" : "off", pct); dm_fork_ff_pct = pct; }
        (void)warned; on = want; return; }
    if (find() != 0) return;
    int sp = dm_fork_ff_speed; if (sp <= 0) { const char *e = getenv("DM_FORK_FF_SPEED"); sp = e ? atoi(e) : 150; }
    if (sp < 110) sp = 110; if (sp > 400) sp = 400;
    uint32_t v = want ? (uint32_t)((uint64_t)k_orig * (uint64_t)sp / 100u) : k_orig;
    if (__atomic_load_n(g_word, __ATOMIC_RELAXED) != v) {
        dm_guest_write_prepare(g_word, sizeof *g_word); __atomic_store_n(g_word, v, __ATOMIC_RELAXED);
        static int lines; if (lines++ < 200) LOG("[FORK] fast-forward %s (%d %%)", want ? "on" : "off", want ? sp : 100);
    }
    on = want;
}

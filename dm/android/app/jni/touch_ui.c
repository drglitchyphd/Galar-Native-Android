// The on-screen controls: what they look like (front_overlay, drawn by the Vulkan replay over the game, nvn_ovl.h), what touching them does (touch_ui_event), and the
// haptic click of each press (Android's Vibrator through JNI, on a thread of its own so input never waits on it). Proportions and look after LAS Recomp's Android port
// (runtime/ui/touch_controls.mm), in units of the screen height, landscape: glossy bubbles (shadow, dark rim, body, glow, a gloss spot, a bright rim), all the same
// size as on the Switch; a floating stick (a compass ring that rests bottom-left and comes to where the thumb lands in the lower-left part); A B X Y in the Switch's diamond bottom-right;
// L / R round in the top corners; − and + at the top centre; a right stick below-left of the diamond, as on a right Joy-Con (the camera, pushed like the real stick).
// Dragging anywhere else on the right turns the camera too, following the finger:
// the stick is the finger's speed, back to rest as soon as the finger stops. A quick tap is held at least 80 ms (the game reads its input once a frame, every 33 ms);
// touch areas are 1.3 times the drawing; a thumb sliding across the face buttons presses the one under it. Hidden while a controller is used, back on the next touch.
// The layout and drawing parts also build into the Mac Vulkan host (mac_vk_build.sh), whose DM_OVL_SHOT composes them over a game frame to check the look.
#include <stdatomic.h>   // FORK: dm_fork_screen
#include "dm.h"
#include "game_profile.h"   // DM_GAME_TOUCH_ZLZR (the profile key touch_zlzr)
#if defined(DM_GAME_TOUCH_MENU) && DM_GAME_TOUCH_MENU
#define GESTURES_ON 1   // the throw button and the touch gestures (touch_ui_event): the games with touch menus (Legends: Arceus)
#else
#define GESTURES_ON 0
#endif
#include "dm_input.h"
#include "nvn_ovl.h"
#ifdef __ANDROID__
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

enum { C_STICK, C_CAM, C_RSTICK, C_A, C_B, C_X, C_Y, C_L, C_R, C_ZL, C_ZR, C_PLUS, C_MINUS, C_UP, C_DOWN, C_LEFT, C_RIGHT, C_THROW, C_MENU, C_L3, C_R3, C_FF, C_TURBO, C_ALL, NCTL };   /* FORK v47: C_ALL, the context controls' "all buttons" override */   /* FORK: C_FF (fast-forward), C_TURBO (turbo A) */   // (C_MENU: the ☰ button, the in-game settings; C_L3 / C_R3: pressing the sticks)
typedef struct { float cx, cy, r; int glyph, shown; uint64_t btn; } ctl;
static ctl g_c[NCTL];
static float g_w, g_h;
static struct { int32_t id; int target; float x0, y0, x, y, lx, ly; double lt, t0; int thr, lp; } g_ptr[10];   // (thr: this finger's swipe became a throw)   // (t0: when the finger landed)   // fingers down: id, what they hold, where, last move (camera)
static uint64_t g_btn; static int32_t g_lx, g_ly, g_rx, g_ry;
static double g_held_until[NCTL];                   // the 80 ms minimum press
static float g_cam_vx, g_cam_vy; static double g_cam_t;   // the camera finger's speed (pixels / s) and when it last moved
static float g_stick_x, g_stick_y, g_knob_x, g_knob_y; static int g_stick_on;
static float g_rknob_x, g_rknob_y; static int g_rstick_on;
static double g_inner_t;   // the last touch of the D-pad or the right stick   // the right stick: its base stays put, the knob follows the thumb
static int g_visible = 1;
static int g_edit;   // the layout editor is on (touch_ui_event, front_overlay)
// FORK: the control scheme (DM_FORK_TOUCH=float; off = the stock controls). Floating sticks: a finger down anywhere on the left half that is not on a button is the
//   movement stick, its base where the finger landed; on the right half the camera stick the same way (tilt from where the finger landed, not the finger's speed).
//   A quick tap on either half presses A (a menu item when one of the game's menus shows); two fingers tapping together stay B. DM_FORK_DEADZONE: 0-40 % of the stick
static int g_float = -1; static float g_dead = 0.12f;
static float g_tsize = 1.0f;   // FORK: the overall size (DM_FORK_TOUCH_SIZE), see tint()
static int g_fr_on; static float g_fr_x, g_fr_y, g_frk_x, g_frk_y;   // the floating camera stick: on, its base, its knob
// FORK: "In battle: hide unused buttons" (DM_FORK_BATTLE_HIDE=on): while the graphics layer sees a battle (gfx/nvn/fork_ui.h dm_fork_screen == 2), L / R, ZL / ZR,
//   + / - and the on-screen right stick are neither drawn nor touchable; A, B, X (the quick Poké Ball), Y, the D-pad, the movement stick and the menu button stay
static int g_bhide;
extern _Atomic int dm_fork_screen __attribute__((weak)); extern volatile int dm_fork_census_want __attribute__((weak));
static int g_ctx_over;   // FORK v47 (defined below, with the context controls)
static int battle_hidden(int c) {
    if (!g_bhide || !&dm_fork_screen || atomic_load(&dm_fork_screen) != 2) return 0;
    if (g_ctx_over == 2) return 0;   // FORK v47: the ⊞ override
    return c == C_L || c == C_R || c == C_ZL || c == C_ZR || c == C_PLUS || c == C_MINUS || c == C_RSTICK;   // (X stays: the quick Poké Ball in wild battles)
}
// FORK: fast-forward (DM_FORK_FF: "" off, "battle" in battles only, "all" anywhere; the speed: host/fork_ff.c), turbo A (DM_FORK_TURBO=on, DM_FORK_TURBO_HZ presses a
//   second), battle icons on X / B / Y (DM_FORK_BATTLE_ICONS, default on), a battle layout of its own (DM_FORK_BATTLE_LAYOUT=on) and battle opacity
//   (DM_FORK_BATTLE_OPACITY 5-100 %, 0 = the same as elsewhere)
static float g_bfade = -1;   // FORK: DM_FORK_BATTLE_FADE seconds in battles (-1: as elsewhere)
static int g_ffscope, g_ff_toggle, g_ff_hold, g_turbo_on, g_turbo_held, g_bicons = 1, g_blayout; static float g_turbo_hz = 10, g_bopacity;
extern volatile int dm_fork_ff_req __attribute__((weak)), dm_fork_ff_speed __attribute__((weak));
static int in_battle(void) { return &dm_fork_screen && atomic_load(&dm_fork_screen) == 2; }
// FORK v47: context controls, each its own switch (default off): dialogue (DM_FORK_CTX_DIALOGUE), full-screen menus (DM_FORK_CTX_MENU), the overworld
//   (DM_FORK_CTX_WORLD). Each hides what the screen does not use; dialogue and menus keep a layout of their own (like the battle's). The ⊞ button (C_ALL) brings every
//   button and the normal layout back until the screen changes (the screen guess: gfx/nvn/fork_ui.h, 0 world, 1 menu, 2 battle, 3 dialogue)
static int g_ctx_dlg, g_ctx_menu, g_ctx_world, g_ctx_over = -1;   // (g_ctx_over: the screen kind the override was asked on; -1 none)
static int screen_kind(void) { return &dm_fork_screen ? atomic_load(&dm_fork_screen) : 0; }
static int g_bmode;   // Battle mode (battle_mode below)
static int ctx_on(int k) { return k == 3 ? g_ctx_dlg : k == 1 ? g_ctx_menu : k == 0 ? g_ctx_world : k == 2 ? g_bmode : 0; }
static int ctx_active(int k) { if (g_ctx_over >= 0 && g_ctx_over != screen_kind()) g_ctx_over = -1; return ctx_on(k) && g_ctx_over != k; }
static int ctx_hidden(int c) {
    int k = screen_kind(); if (!ctx_active(k) || c == C_ALL || c == C_MENU) return 0;
    if (k == 3) return !(c == C_A || c == C_B || (c >= C_UP && c <= C_RIGHT) || c == C_TURBO || c == C_FF);   // dialogue: A, B, the D-pad (choices), turbo, fast-forward
    if (k == 1) return c == C_STICK || c == C_RSTICK || c == C_ZL || c == C_ZR || c == C_L3 || c == C_R3 || c == C_THROW || c == C_TURBO;   // menus: no sticks
    if (k == 0) return c == C_ZL || c == C_ZR || c == C_L3 || c == C_R3 || (c >= C_UP && c <= C_RIGHT);   // the overworld: no ZL / ZR, L3 / R3, D-pad
    return 0;
}
static int g_ctx_any;   // FORK v47: a context switch is on (census_sync)
static void census_sync(void) { if (&dm_fork_census_want) dm_fork_census_want = g_bhide || g_bicons || g_blayout || g_bopacity > 0 || g_ffscope == 1 || g_bfade >= 0 || g_ctx_any; }
static void bhide_set(int on) { g_bhide = on; census_sync(); }
// FORK v46: one switch, "Battle controls: Battle mode" (DM_FORK_BATTLE=on): unused buttons hidden, battle icons, the battle's own layout. Set before v46 (no
//   DM_FORK_BATTLE yet), the old DM_FORK_BATTLE_HIDE / _LAYOUT still turn it on, so a player's battle setup carries over
static void battle_mode(int on) { g_bmode = on; bhide_set(on); g_bicons = on; g_blayout = on; g_w = g_h = 0; census_sync(); }
static int fork_hidden(int c) {
    if (c == C_FF) return !g_ffscope || (g_ffscope == 1 && !in_battle());
    if (c == C_TURBO) return !g_turbo_on;
    if (c == C_ALL) { int k = screen_kind(); return !ctx_on(k); }   // FORK v47: only where a context mode applies (lit while its override is on)
    return battle_hidden(c) || ctx_hidden(c);
}
static void ff_publish(void) { if (&dm_fork_ff_req) dm_fork_ff_req = (g_ff_toggle || g_ff_hold) && (g_ffscope == 2 || (g_ffscope == 1 && in_battle())); }
static double g_touch_t; static double g_two_t, g_tap_mute;   // two-finger tap: when the second finger landed; single taps ignored until g_tap_mute
int touch_menu_active(void);   // touch_menu.c: one of the game's menus shows
#if defined(DM_GAME_TOUCH_MENU) && DM_GAME_TOUCH_MENU
enum { GESTURES = 1 };   // tap = A in the world, swipe up = throw: only where menu taps work too (else a tap in a menu would press A on whatever is selected)
#else
enum { GESTURES = 0 };
#endif   // the last touch anywhere: every control fades 3 s after it (they cover the game's own menus and text)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------- the player's layout (the in-game editor): position, size and visibility of each control, kept in <files>/touch_layout.txt ----------
// One line per changed control: "<name> <x / width> <y / height> <radius / height> <shown>"; controls without a line keep the default layout
static const char *const CTL_NAME[NCTL] = { "stick", "camera", "rstick", "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "up", "down", "left", "right", "throw", "menu", "l3", "r3", "ff", "turbo", "all" };   // FORK: + ff, turbo
static struct { int set; float fx, fy, fr; int shown, shape; } g_ov[NCTL];   // FORK: shape (0 circle, 1 rounded square, 2 square, 3 pill), a 6th field in the file
static char g_lay_path[1100]; static int g_lay_loaded;
// FORK: one layout per screen and orientation: touch_layout_<profile>.txt (outer_landscape, inner_landscape, inner_portrait, outer_portrait), picked from the window's
//   shape in layout(). A profile without its own file starts from the old single touch_layout.txt when there is one, else from the default layout
static char g_lay_dir[1000]; static const char *g_lay_prof = "";
void touch_ui_set_dir(const char *dir) { snprintf(g_lay_dir, sizeof g_lay_dir, "%s", dir); g_lay_path[0] = 0; g_lay_prof = ""; g_lay_loaded = 0; }
static const char *profile_of(float w, float h) {
    float lo = w < h ? w : h, hi = w < h ? h : w; int outer = lo > 0 && hi / lo > 1.6f;   // the outer screen is about 21:9, the inner nearly square
    return h > w ? (outer ? "outer_portrait" : "inner_portrait") : (outer ? "outer_landscape" : "inner_landscape");
}
const char *touch_ui_profile(void) { return g_lay_prof; }   // (android_main.c: the editor bar names it)
static void layout_profile(float w, float h) {
    const char *pf = profile_of(w, h);
    static char pb[40]; if (g_blayout && in_battle() && g_ctx_over != 2) { snprintf(pb, sizeof pb, "%s_battle", pf); pf = pb; }
    else if (screen_kind() == 3 && ctx_active(3)) { snprintf(pb, sizeof pb, "%s_dialogue", pf); pf = pb; }   // FORK v47: dialogue and menus: layouts of their own
    else if (screen_kind() == 1 && ctx_active(1)) { snprintf(pb, sizeof pb, "%s_menu", pf); pf = pb; }   // FORK: the battle layout (DM_FORK_BATTLE_LAYOUT)
    if (!g_lay_dir[0] || !strcmp(pf, g_lay_prof)) return;
    static char keep[40]; snprintf(keep, sizeof keep, "%s", pf); pf = keep;
    g_lay_prof = pf; snprintf(g_lay_path, sizeof g_lay_path, "%s/touch_layout_%s.txt", g_lay_dir, pf); g_lay_loaded = 0;
    fprintf(stderr, "[FORK] [touch] layout profile %s (%.0fx%.0f)\n", pf, w, h);
}
static void layout_load(void) {
    g_lay_loaded = 1; memset(g_ov, 0, sizeof g_ov); if (!g_lay_path[0]) return;
    FILE *f = fopen(g_lay_path, "r");
    if (!f && strstr(g_lay_prof, "_battle")) { char base[1100]; snprintf(base, sizeof base, "%s/touch_layout_%.*s.txt", g_lay_dir, (int)(strstr(g_lay_prof, "_battle") - g_lay_prof), g_lay_prof); f = fopen(base, "r"); }   // FORK: the battle layout starts from this screen's own
    if (!f) { char old[1100]; snprintf(old, sizeof old, "%s/touch_layout.txt", g_lay_dir); f = fopen(old, "r"); }   // FORK: the old single layout as a start
    char ln[160], nm[32]; float fx, fy, fr; int sh;
    int shp;
    while (f && fgets(ln, sizeof ln, f)) { shp = 0; if (sscanf(ln, "%31s %f %f %f %d %d", nm, &fx, &fy, &fr, &sh, &shp) >= 5)
        for (int c = 0; c < NCTL; c++) if (!strcmp(nm, CTL_NAME[c])) { g_ov[c].set = 1; g_ov[c].fx = fx; g_ov[c].fy = fy; g_ov[c].fr = fr; g_ov[c].shown = sh; g_ov[c].shape = shp & 3; } }   // FORK: shape
    if (f) fclose(f);
}
static void layout_save(void) {
    if (!g_lay_path[0]) return; FILE *f = fopen(g_lay_path, "w"); if (!f) return;
    for (int c = 0; c < NCTL; c++) if (g_ov[c].set) fprintf(f, "%s %.4f %.4f %.4f %d %d\n", CTL_NAME[c], g_ov[c].fx, g_ov[c].fy, g_ov[c].fr, g_ov[c].shown, g_ov[c].shape);   // FORK: + shape
    fclose(f);
}
static void layout_apply(float w, float h) {
    if (!g_lay_loaded) layout_load();
    for (int c = 0; c < NCTL; c++) if (g_ov[c].set && c != C_CAM) { g_c[c].cx = g_ov[c].fx * w; g_c[c].cy = g_ov[c].fy * h; g_c[c].r = g_ov[c].fr * h; g_c[c].shown = g_ov[c].shown; }
}
// L3 / R3 on screen: an option (DM_TOUCH_L3R3=on, Controls → Touch controls), off by default: Sword and Shield hardly use them. -1: not read yet
static int g_l3r3 = -1;
static int l3r3_on(void) { if (g_l3r3 < 0) { const char *e = getenv("DM_TOUCH_L3R3"); g_l3r3 = e && !strcmp(e, "on"); } return g_l3r3; }
static void layout(float w, float h) {
    static int init; if (!init++) for (int i = 0; i < 10; i++) g_ptr[i].id = -1;   // no finger down
    if (w == g_w && h == g_h) return;
    layout_profile(w, h);   // FORK: this screen's own layout file
    g_w = w; g_h = h;
    // the outer controls keep about 0.2 of the height (~290 px on a 1440 px phone) clear of the screen's long edges, where the system's navigation buttons and edge
    // swipes are: with the A button nearer, a thumb on it opened recents
    float s = h, br = 0.074f * s, gap = 0.135f * s, fx = w - 0.40f * s, fy = h - 0.30f * s, sr = 0.064f * s, mr = 0.040f * s;
    g_c[C_A] = (ctl){ fx + gap, fy, br, OVL_G_A, 1, DM_BTN_A };   // the Switch's diamond, all four the same
    g_c[C_B] = (ctl){ fx, fy + gap, br, OVL_G_B, 1, DM_BTN_B };
    g_c[C_X] = (ctl){ fx, fy - gap, br, OVL_G_X, 1, DM_BTN_X };
    g_c[C_Y] = (ctl){ fx - gap, fy, br, OVL_G_Y, 1, DM_BTN_Y };
    g_c[C_L] = (ctl){ 0.25f * s, 0.30f * s, sr, OVL_G_L, 1, DM_BTN_L };
    g_c[C_R] = (ctl){ w - 0.25f * s, 0.30f * s, sr, OVL_G_R, 1, DM_BTN_R };
    // ZL / ZR above L / R where the game uses them (profile key touch_zlzr: Legends: Arceus aims with ZL and throws with ZR); hidden otherwise (Shield)
#if defined(DM_GAME_TOUCH_ZLZR) && DM_GAME_TOUCH_ZLZR
    g_c[C_ZL] = (ctl){ 0.25f * s, 0.11f * s, sr, OVL_G_ZL, 1, DM_BTN_ZL };
    g_c[C_ZR] = (ctl){ w - 0.25f * s, 0.11f * s, sr, OVL_G_ZR, 1, DM_BTN_ZR };
#else
    g_c[C_ZL] = (ctl){ 0.26f * s, 0.10f * s, sr * 0.8f, OVL_G_ZL, 0, DM_BTN_ZL };
    g_c[C_ZR] = (ctl){ w - 0.26f * s, 0.10f * s, sr * 0.8f, OVL_G_ZR, 0, DM_BTN_ZR };
#endif
    g_c[C_MINUS] = (ctl){ w * 0.5f - 0.13f * s, 0.06f * s, mr, OVL_G_MINUS, 1, DM_BTN_MINUS };
    g_c[C_PLUS] = (ctl){ w * 0.5f + 0.13f * s, 0.06f * s, mr, OVL_G_PLUS, 1, DM_BTN_PLUS };
    g_c[C_STICK] = (ctl){ 0.36f * s, h - 0.27f * s, 0.14f * s, OVL_G_NONE, 1, 0 };
    {   // the D-pad below-right of the left stick, as on a left Joy-Con (bag pockets, menus): four small buttons with a dot towards their direction
        float dx = 0.80f * s, dy = h - 0.19f * s, dg = 0.085f * s, dr = 0.050f * s;
        g_c[C_UP] = (ctl){ dx, dy - dg, dr, OVL_G_NONE, 1, DM_BTN_UP }; g_c[C_DOWN] = (ctl){ dx, dy + dg, dr, OVL_G_NONE, 1, DM_BTN_DOWN };
        g_c[C_LEFT] = (ctl){ dx - dg, dy, dr, OVL_G_NONE, 1, DM_BTN_LEFT }; g_c[C_RIGHT] = (ctl){ dx + dg, dy, dr, OVL_G_NONE, 1, DM_BTN_RIGHT };
    }
    // THROW (Legends: Arceus): a Poké Ball where the right thumb rests, above-left of A B X Y: held = ZR (aim, the finger's drag turns the aim), released = throw
    g_c[C_THROW] = (ctl){ w - 0.66f * s, h - 0.56f * s, 0.10f * s, OVL_G_NONE, GESTURES_ON, DM_BTN_ZR };
    g_c[C_RSTICK] = (ctl){ w - 0.80f * s, h - 0.19f * s, 0.11f * s, OVL_G_NONE, 1, 0 };
    g_c[C_MENU] = (ctl){ w * 0.5f - 0.24f * s, 0.06f * s, mr, OVL_G_NONE, 1, 0 };   // ☰ just left of −, at the top centre: the settings (front_open_menu); no Switch button
    // L3 / R3 (pressing a stick in, #108): small buttons just above each stick, outside its touch area (1.3 x its radius) and clear of the D-pad and A B X Y on
    //   16:9 to 20:9 screens; hide or move them in the layout editor like any other
    g_c[C_L3] = (ctl){ g_c[C_STICK].cx, g_c[C_STICK].cy - 0.22f * s, 0.05f * s, OVL_G_L3, 1, DM_BTN_LS };
    g_c[C_R3] = (ctl){ g_c[C_RSTICK].cx, g_c[C_RSTICK].cy - 0.21f * s, 0.05f * s, OVL_G_R3, 1, DM_BTN_RS };
    g_c[C_FF] = (ctl){ w * 0.5f + 0.24f * s, 0.06f * s, mr, OVL_G_FF, 1, 0 };
    g_c[C_ALL] = (ctl){ w * 0.5f + 0.36f * s, 0.06f * s, mr * 0.85f, OVL_G_ALL, 1, 0 };   // FORK v47: ⊞ all buttons, right of fast-forward   // FORK: fast-forward, just right of + (fork_hidden: only when Fast-forward is on)
    g_c[C_TURBO] = (ctl){ w - 0.12f * s, h - 0.50f * s, 0.05f * s, OVL_G_TURBO, 1, 0 };   // FORK: turbo A, above-right of A (only when Turbo A is on)
    // touch-first games (Legends: Arceus): only the movement stick and − / + on screen; everything else is touch on the game itself (touch_ui_event's gestures, touch_menu.c)
    if (GESTURES_ON) for (int c = C_RSTICK; c < NCTL; c++) if (c != C_PLUS && c != C_MINUS && c != C_MENU) g_c[c].shown = 0;
    if (h > w * 1.05f) {   // FORK: portrait (the inner screen upright): the 16:9 picture fills the top (nvn_ovl.h dm_picture_box), the controls go in the space below it
        float ph = w * 9.0f / 16.0f, a = h - ph, q = 0.85f * w, cb = 0.072f * q, cg = 0.125f * q, ax0 = 0.78f * w, ay0 = ph + 0.40f * a, lr = 0.06f * q, sm = 0.040f * q;
        g_c[C_A] = (ctl){ ax0 + cg, ay0, cb, OVL_G_A, 1, DM_BTN_A }; g_c[C_B] = (ctl){ ax0, ay0 + cg, cb, OVL_G_B, 1, DM_BTN_B };
        g_c[C_X] = (ctl){ ax0, ay0 - cg, cb, OVL_G_X, 1, DM_BTN_X }; g_c[C_Y] = (ctl){ ax0 - cg, ay0, cb, OVL_G_Y, 1, DM_BTN_Y };
        g_c[C_L] = (ctl){ 0.11f * w, ph + 0.10f * a, lr, OVL_G_L, 1, DM_BTN_L }; g_c[C_R] = (ctl){ 0.89f * w, ph + 0.10f * a, lr, OVL_G_R, 1, DM_BTN_R };
        g_c[C_ZL].cx = 0.11f * w; g_c[C_ZL].cy = ph + 0.10f * a - 2.4f * lr; g_c[C_ZR].cx = 0.89f * w; g_c[C_ZR].cy = g_c[C_ZL].cy;
        g_c[C_MENU] = (ctl){ 0.36f * w, ph + 0.08f * a, sm, OVL_G_NONE, 1, 0 };
        g_c[C_MINUS] = (ctl){ 0.44f * w, ph + 0.08f * a, sm, OVL_G_MINUS, 1, DM_BTN_MINUS }; g_c[C_PLUS] = (ctl){ 0.56f * w, ph + 0.08f * a, sm, OVL_G_PLUS, 1, DM_BTN_PLUS };
        g_c[C_STICK] = (ctl){ 0.22f * w, ph + 0.40f * a, 0.13f * q, OVL_G_NONE, 1, 0 };
        float dx = 0.22f * w, dy = h - 0.20f * a, dg = 0.07f * q, dr = 0.042f * q;   // (under the stick; positions checked for overlaps at 2256x2504 and 1080x2520)
        g_c[C_UP] = (ctl){ dx, dy - dg, dr, OVL_G_NONE, 1, DM_BTN_UP }; g_c[C_DOWN] = (ctl){ dx, dy + dg, dr, OVL_G_NONE, 1, DM_BTN_DOWN };
        g_c[C_LEFT] = (ctl){ dx - dg, dy, dr, OVL_G_NONE, 1, DM_BTN_LEFT }; g_c[C_RIGHT] = (ctl){ dx + dg, dy, dr, OVL_G_NONE, 1, DM_BTN_RIGHT };
        g_c[C_RSTICK] = (ctl){ 0.50f * w, h - 0.20f * a, 0.09f * q, OVL_G_NONE, 1, 0 };
        g_c[C_THROW].shown = 0;
        g_c[C_FF] = (ctl){ 0.68f * w, ph + 0.08f * a, sm, OVL_G_FF, 1, 0 }; g_c[C_ALL] = (ctl){ 0.78f * w, ph + 0.08f * a, sm * 0.85f, OVL_G_ALL, 1, 0 }; g_c[C_TURBO] = (ctl){ 0.62f * w, ph + 0.22f * a, 0.045f * q, OVL_G_TURBO, 1, 0 };   // FORK
    }
    if (g_tsize != 1.0f) for (int c = 0; c < NCTL; c++) g_c[c].r *= g_tsize;   // FORK: overall size (before the player's own sizes)
    if (!g_rstick_on) { g_rknob_x = g_c[C_RSTICK].cx; g_rknob_y = g_c[C_RSTICK].cy; }
    layout_apply(w, h);
    if (!l3r3_on()) g_c[C_L3].shown = g_c[C_R3].shown = 0;   // (an option, off by default: the layout editor's own choice only counts while it is on)
    if (!g_stick_on) { g_stick_x = g_knob_x = g_c[C_STICK].cx; g_stick_y = g_knob_y = g_c[C_STICK].cy; }
}
// FORK v47 (from Arceus Native 652ab9d): the touch camera's own sensitivity (DM_TOUCH_CAM_SENS 25-300 %, apart from the right stick's DM_RSTICK_SENS), how a drag on
//   the camera side turns it (DM_TOUCH_CAMERA: drag = the finger's speed, stock; stick = where the finger lands is the centre, held away it keeps turning, faster the
//   further; the floating sticks scheme already works that way), and whether the right stick is drawn (DM_TOUCH_RSTICK=off hides it and R3)
static int g_cam_pct = -1, g_cam_stick = -1, g_rs_off = -1;
static int cam_pct(void) { if (g_cam_pct < 0) { const char *e = getenv("DM_TOUCH_CAM_SENS"); int v = e ? atoi(e) : 100; g_cam_pct = v < 25 || v > 300 ? 100 : v; } return g_cam_pct; }
static int cam_stick(void) { if (g_cam_stick < 0) { const char *e = getenv("DM_TOUCH_CAMERA"); g_cam_stick = e && !strcmp(e, "stick"); } return g_cam_stick; }
static int rs_off(void) { if (g_rs_off < 0) { const char *e = getenv("DM_TOUCH_RSTICK"); g_rs_off = e && !strcmp(e, "off"); } return g_rs_off; }
static int ctl_off(int c) { return ((c == C_L3 || c == C_R3) && !l3r3_on()) || ((c == C_RSTICK || c == C_R3) && rs_off()); }   // left out entirely, the layout editor too (FORK v47: + rs_off)
static int hit(int c, float x, float y, float slack) {
    if (fork_hidden(c)) return 0;   // FORK
    const ctl *k = &g_c[c]; float dx = x - k->cx, dy = y - k->cy, r = k->r * slack;
    if (g_ov[c].set && g_ov[c].shape == 3) dx /= 1.7f;   // FORK: a pill is wider
    return k->shown && dx * dx + dy * dy <= r * r;
}
static int target_at(float x, float y) {
    if (g_float <= 0 && hit(C_RSTICK, x, y, 1.3f)) return C_RSTICK;   // FORK: floating sticks: the fixed right stick is not there
    for (int c = C_A; c < NCTL; c++) if (hit(c, x, y, 1.3f)) return c;   // thumbs miss: touch areas 1.3 times the drawing
    if (g_float > 0) return x < g_w * 0.5f ? C_STICK : C_CAM;   // FORK: floating sticks, each on its own half
    if (x < g_w * 0.42f && y > g_h * 0.30f) return C_STICK;
    if (x > g_w * 0.45f) return C_CAM;
    return -1;
}
static int32_t ax(float v) { v = clampf(v, -1, 1); return (int32_t)(v * DM_STICK_MAX); }

#ifdef __ANDROID__
void touch_menu_tap(float x, float y, float w, float h);   // touch_menu.c
void front_open_menu(int what);   // android_main.c: the in-game settings (GameMenu.java), 0 = all, 1 = the controller buttons
// ---------- haptics ----------
static JavaVM *g_vm; static jobject g_act; static pthread_cond_t g_hcv = PTHREAD_COND_INITIALIZER; static int g_hpend;
// rumble (hle_hid.c dm_rumble): the game's motor strength, handed to GameActivity.rumble on this thread when it changes, and again every 100 ms while it lasts
// (each hand-over is a short pulse, so a rumble the game stops without a zero value ends by itself)
static float g_ramp; static int g_rpend;
static void *haptic_main(void *p) {
    (void)p; JNIEnv *e; if ((*g_vm)->AttachCurrentThread(g_vm, &e, NULL)) return NULL;
    jclass ctx = (*e)->FindClass(e, "android/content/Context"); jmethodID gss = (*e)->GetMethodID(e, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = (*e)->NewStringUTF(e, "vibrator"); jobject vib = (*e)->CallObjectMethod(e, g_act, gss, name);
    jclass vc = (*e)->FindClass(e, "android/os/Vibrator"), ec = (*e)->FindClass(e, "android/os/VibrationEffect");
    jmethodID vibrate = vc ? (*e)->GetMethodID(e, vc, "vibrate", "(Landroid/os/VibrationEffect;)V") : NULL;
    jmethodID pre = ec ? (*e)->GetStaticMethodID(e, ec, "createPredefined", "(I)Landroid/os/VibrationEffect;") : NULL;
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    jobject click = pre ? (*e)->CallStaticObjectMethod(e, ec, pre, 0 /* EFFECT_CLICK */) : NULL;
    if (click) click = (*e)->NewGlobalRef(e, click);
    if (!vib || !vibrate || !click) { __android_log_print(ANDROID_LOG_WARN, "nxnative", "haptics unavailable"); (*g_vm)->DetachCurrentThread(g_vm); return NULL; }
    vib = (*e)->NewGlobalRef(e, vib);
    jmethodID rumble = (*e)->GetMethodID(e, (*e)->GetObjectClass(e, g_act), "rumble", "(F)V"); if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); rumble = NULL; }
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_hpend && !g_rpend) {
            if (g_ramp <= 0) { pthread_cond_wait(&g_hcv, &g_mu); continue; }
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            if (pthread_cond_timedwait(&g_hcv, &g_mu, &ts) == ETIMEDOUT) g_rpend = 1;
        }
        int tap = g_hpend, rum = g_rpend; float amp = g_ramp; g_hpend = g_rpend = 0; pthread_mutex_unlock(&g_mu);
        if (tap) { (*e)->CallVoidMethod(e, vib, vibrate, click); if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e); }
        if (rum && rumble) { (*e)->CallVoidMethod(e, g_act, rumble, (jfloat)amp); if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e); }
    }
    return NULL;
}
void touch_ui_init(ANativeActivity *a) {
    g_vm = a->vm; g_act = a->clazz;
    static int once; if (once++) return;   // a second GameActivity in the same process (the first handed over to the setup screen): one haptic thread
    pthread_t t; if (!pthread_create(&t, NULL, haptic_main, NULL)) pthread_detach(t);
}
// the click on a press: DM_TOUCH_HAPTIC on / off, or by default as DM_RUMBLE (Off silences it too: the player asked for no vibration, #114). -1: not read yet
static int g_haptic = -1, g_rumble_off = -1;
static int haptic_on(void) {
    if (g_haptic < 0) { const char *h = getenv("DM_TOUCH_HAPTIC"); g_haptic = h && !strcmp(h, "off") ? 0 : h && !strcmp(h, "on") ? 1 : 2; }
    if (g_rumble_off < 0) { const char *r = getenv("DM_RUMBLE"); g_rumble_off = r && !strcmp(r, "off"); }
    return g_haptic == 1 || (g_haptic == 2 && !g_rumble_off);
}
void touch_ui_haptic_setting(const char *key, const char *value) {   // (touch_ui_setting, under g_mu)
    if (!strcmp(key, "DM_TOUCH_HAPTIC")) g_haptic = value && !strcmp(value, "off") ? 0 : value && !strcmp(value, "on") ? 1 : 2;
    else if (!strcmp(key, "DM_RUMBLE")) g_rumble_off = value && !strcmp(value, "off");
}
static void click_locked(void) { if (!haptic_on()) return; g_hpend = 1; pthread_cond_signal(&g_hcv); }   // callers hold g_mu
void dm_rumble(float left, float right) {   // hle_hid.c: the game's two motors; one strength for the one motor Android drives
    float a = left > right ? left : right;
    pthread_mutex_lock(&g_mu);
    if ((a > 0) != (g_ramp > 0) || fabsf(a - g_ramp) > 0.04f) { g_ramp = a; g_rpend = 1; pthread_cond_signal(&g_hcv); }
    pthread_mutex_unlock(&g_mu);
}
#endif
#ifdef __ANDROID__

// ---------- touch ----------
static void recompute_locked(void) {
    uint64_t b = 0; float lx = 0, ly = 0, rx = 0, ry = 0; int stick = 0, rstick = 0; double t = now_s(); int ffh = 0, turbo = 0;   // FORK: ffh, turbo
    for (int i = 0; i < 10; i++) {
        if (g_ptr[i].id < 0) continue;
        int c = g_ptr[i].target;
        if (c == C_STICK) { float r = g_c[C_STICK].r, dx = g_ptr[i].x - g_stick_x, dy = g_ptr[i].y - g_stick_y, d = sqrtf(dx * dx + dy * dy);
            if (d > r) { dx *= r / d; dy *= r / d; }
            float nx = dx / r, ny = -dy / r; if (nx * nx + ny * ny < g_dead * g_dead) nx = ny = 0;   // dead zone (FORK: g_dead, DM_FORK_DEADZONE)
            lx = nx; ly = ny; stick = 1; g_knob_x = g_stick_x + dx; g_knob_y = g_stick_y + dy; }
        else if (c == C_CAM && g_float <= 0 && cam_stick() && !g_ptr[i].thr) {   // FORK v47: the camera as a stick under the finger (stock scheme): full at 12 % of the screen height
            float r = g_h * 0.12f, dx = (g_ptr[i].x - g_ptr[i].x0) / r, dy = (g_ptr[i].y - g_ptr[i].y0) / r, m = sqrtf(dx * dx + dy * dy);
            if (m > 0.08f) { float k = fminf(1.0f, (m - 0.08f) / 0.92f) / m; rx = dx * k; ry = -dy * k; } rstick = 3; }
        else if (c == C_CAM && g_float > 0 && !g_ptr[i].thr) {   // FORK: the floating camera stick, tilt from where the finger landed
            float r = g_c[C_STICK].r, dx = g_ptr[i].x - g_fr_x, dy = g_ptr[i].y - g_fr_y, d = sqrtf(dx * dx + dy * dy);
            if (d > r) { dx *= r / d; dy *= r / d; }
            float nx = dx / r, ny = -dy / r; if (nx * nx + ny * ny < g_dead * g_dead) nx = ny = 0;
            rx = nx; ry = ny; rstick = 2; g_frk_x = g_fr_x + dx; g_frk_y = g_fr_y + dy; }
        else if (c == C_RSTICK) { const ctl *k = &g_c[C_RSTICK]; float r = k->r, dx = g_ptr[i].x - k->cx, dy = g_ptr[i].y - k->cy, d = sqrtf(dx * dx + dy * dy);
            if (d > r) { dx *= r / d; dy *= r / d; }
            float nx = dx / r, ny = -dy / r; if (nx * nx + ny * ny < 0.12f * 0.12f) nx = ny = 0;
            rx = nx; ry = ny; rstick = 1; g_rknob_x = k->cx + dx; g_rknob_y = k->cy + dy; }
        else if (c == C_FF) { if (t - g_ptr[i].t0 >= 0.35) ffh = 1; }   // FORK: held = fast-forward while held (a quick tap toggles: touch_ui_event)
        else if (c == C_TURBO) { turbo = 1; if (fmod((t - g_ptr[i].t0) * g_turbo_hz, 1.0) < 0.5) b |= DM_BTN_A; }   // FORK: turbo A: A pressed half of each period
        else if (c >= C_A) b |= g_c[c].btn;
    }
    g_ff_hold = ffh; g_turbo_held = turbo; ff_publish();   // FORK
    for (int c = C_A; c < NCTL; c++) if (t < g_held_until[c]) b |= g_c[c].btn;
    for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0 && g_ptr[i].thr) b |= DM_BTN_ZR;
    for (int i = 0; i < 10; i++) if (GESTURES_ON && !cam_stick() && g_ptr[i].id >= 0 && g_ptr[i].target == C_CAM && !g_ptr[i].thr && !touch_menu_active() && t - g_ptr[i].t0 > 0.45 &&   // FORK v47: not in stick mode (a held finger steers)
                                     fabsf(g_ptr[i].x - g_ptr[i].x0) < g_h * 0.03f && fabsf(g_ptr[i].y - g_ptr[i].y0) < g_h * 0.03f) { g_ptr[i].lp = 1; b |= DM_BTN_ZL; }   // press and hold: ZL (lock on)
    for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0 && g_ptr[i].lp) b |= DM_BTN_ZL;   // a throw swipe holds ZR (aim) until the finger lifts (throw)
    if (rstick || (b & (DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT))) g_inner_t = t;   // held: stays clear
    int camstick = rstick == 3; if (camstick) rstick = 0;   // FORK v47: the camera stick under the finger: not the on-screen right stick either
    g_fr_on = rstick == 2; if (rstick == 2) rstick = 0;   // FORK: the floating camera stick is not the on-screen right stick (drawn on its own)
    g_rstick_on = rstick; if (!rstick) { g_rknob_x = g_c[C_RSTICK].cx; g_rknob_y = g_c[C_RSTICK].cy; }
    if (!stick) { g_stick_on = 0; g_stick_x = g_knob_x = g_c[C_STICK].cx; g_stick_y = g_knob_y = g_c[C_STICK].cy; }
    // the camera: the finger's speed, full turn at about a screen height per 0.6 s, at rest within 60 ms of the finger stopping
    float vmax = g_h / 0.6f * 100.0f / (float)cam_pct(); if (t - g_cam_t > 0.06) g_cam_vx = g_cam_vy = 0;   // FORK v47: the touch camera's own sensitivity
    int own = !rstick;   // FORK v47: the touch camera (drag, stick under the finger, floating): its own sensitivity; the on-screen right stick keeps DM_RSTICK_SENS
    if (g_fr_on || camstick) { rstick = 1; if (cam_pct() != 100) { float k = cam_pct() / 100.0f; rx = clampf(rx * k, -1, 1); ry = clampf(ry * k, -1, 1); } }   // FORK: the floating camera stick's tilt instead of the finger's speed
    g_btn = b; g_lx = ax(lx); g_ly = ax(ly); g_rx = rstick ? ax(rx) : ax(g_cam_vx / vmax); g_ry = rstick ? ax(ry) : ax(-g_cam_vy / vmax);
    { extern _Atomic int dm_touch_cam_own __attribute__((weak)); if (&dm_touch_cam_own) atomic_store(&dm_touch_cam_own, own); }   // FORK v47 (hle_hid.c)
}
// ---------- the layout editor: hold − for 2 s to enter; drag a control to move it, pinch on it to resize it, double-tap it to hide / show it;
// hold + for 2 s to go back to the default layout; hold − for 2 s again to save and leave. The game gets no input meanwhile ----------
static double g_edit_hold_t;   // (g_edit_hold_t: when the hold that toggles the editor started)
static struct { int32_t id; int c; float ox, oy, x0, y0; double t0; int mv; } g_ep[10];   // FORK v47 (from Arceus Native 1c2214e): mv, the finger moved: dragging, not a hold
static struct { int c, active; float d0, r0; } g_pinch;
static int g_last_tap_c = -1; static double g_last_tap_t;
static int edit_hit(float x, float y) {   // the smallest control (shown or not) under the finger
    int best = -1; float br = 1e9f;
    for (int c = 0; c < NCTL; c++) { if (c == C_CAM || ctl_off(c) || (!g_c[c].shown && !g_ov[c].set && c != C_STICK)) continue;
        if (GESTURES_ON && !g_c[c].shown && !g_ov[c].set && c != C_PLUS && c != C_MINUS && c != C_MENU) continue;
        float dx = x - g_c[c].cx, dy = y - g_c[c].cy, r = g_c[c].r * 1.2f; if (dx * dx + dy * dy <= r * r && g_c[c].r < br) { br = g_c[c].r; best = c; } }
    return best;
}
static int g_edit_sel = -1;   // FORK: the control last touched in the editor (the bar's size slider: ForkTouch.java)
static void edit_store(int c) { g_ov[c].set = 1; g_ov[c].fx = g_c[c].cx / g_w; g_ov[c].fy = g_c[c].cy / g_h; g_ov[c].fr = g_c[c].r / g_h; g_ov[c].shown = g_c[c].shown; }
static void edit_event(const AInputEvent *e) {
    int32_t act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    size_t idx = (size_t)((AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (act == AMOTION_EVENT_ACTION_CANCEL) { for (int i = 0; i < 10; i++) g_ep[i].id = -1; g_pinch.active = 0; return; }
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        size_t k = act == AMOTION_EVENT_ACTION_DOWN ? 0 : idx; float x = AMotionEvent_getX(e, k), y = AMotionEvent_getY(e, k);
        if (act == AMOTION_EVENT_ACTION_DOWN) { for (int i = 0; i < 10; i++) g_ep[i].id = -1; g_pinch.active = 0; }
        int c = edit_hit(x, y);
        if (c >= 0) g_edit_sel = c;   // FORK
        for (int i = 0; i < 10; i++) if (g_ep[i].id >= 0 && c >= 0 && g_ep[i].c == c) {   // a second finger on the same control: pinch
            float dx = x - g_ep[i].x0, dy = y - g_ep[i].y0; g_pinch.c = c; g_pinch.active = 1; g_pinch.d0 = sqrtf(dx * dx + dy * dy) + 1; g_pinch.r0 = g_c[c].r; break; }
        for (int i = 0; i < 10; i++) if (g_ep[i].id < 0) { g_ep[i].id = AMotionEvent_getPointerId(e, k); g_ep[i].c = c; g_ep[i].x0 = x; g_ep[i].y0 = y; g_ep[i].t0 = now_s(); g_ep[i].mv = 0;
            g_ep[i].ox = c >= 0 ? g_c[c].cx - x : 0; g_ep[i].oy = c >= 0 ? g_c[c].cy - y : 0; break; }
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        float px[10], py[10]; int n = 0;
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) { int32_t id = AMotionEvent_getPointerId(e, k); float x = AMotionEvent_getX(e, k), y = AMotionEvent_getY(e, k);
            for (int i = 0; i < 10; i++) if (g_ep[i].id == id && g_ep[i].c >= 0) {
                if (fabsf(x - g_ep[i].x0) > g_h * 0.03f || fabsf(y - g_ep[i].y0) > g_h * 0.03f) g_ep[i].mv = 1;   // FORK v47
                if (g_pinch.active && g_pinch.c == g_ep[i].c) { if (n < 10) { px[n] = x; py[n] = y; n++; } }
                else { int c = g_ep[i].c; g_c[c].cx = clampf(x + g_ep[i].ox, g_c[c].r, g_w - g_c[c].r); g_c[c].cy = clampf(y + g_ep[i].oy, g_c[c].r, g_h - g_c[c].r); edit_store(c); } } }
        if (g_pinch.active && n >= 2) { float dx = px[0] - px[1], dy = py[0] - py[1]; int c = g_pinch.c;
            g_c[c].r = clampf(g_pinch.r0 * (sqrtf(dx * dx + dy * dy) + 1) / g_pinch.d0, g_h * 0.025f, g_h * 0.25f); edit_store(c); }
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t id = AMotionEvent_getPointerId(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx); double t = now_s();
        for (int i = 0; i < 10; i++) if (g_ep[i].id == id) { int c = g_ep[i].c;
            float mx = AMotionEvent_getX(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx) - g_ep[i].x0, my = AMotionEvent_getY(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx) - g_ep[i].y0;
            if (c >= 0 && t - g_ep[i].t0 < 0.3 && mx * mx + my * my < g_h * g_h * 0.0004f) {   // a tap: a double tap toggles the control
                if (g_last_tap_c == c && t - g_last_tap_t < 0.4 && c != C_PLUS && c != C_MINUS && c != C_MENU) { g_c[c].shown = !g_c[c].shown; edit_store(c); g_last_tap_c = -1; click_locked(); }
                else { g_last_tap_c = c; g_last_tap_t = t; } }
            if (g_pinch.active && g_pinch.c == c) g_pinch.active = 0;
            g_ep[i].id = -1; }
    }
}
// FORK: the editor's commands, also from its Save / Reset / Cancel bar (ForkTouch.java through android_main.c)
static void edit_reset_locked(void) {   // the default layout for this profile, kept: an empty file (a missing one would start from the old single layout again)
    memset(g_ov, 0, sizeof g_ov); FILE *f = g_lay_path[0] ? fopen(g_lay_path, "w") : NULL; if (f) fclose(f); g_lay_loaded = 1; g_w = g_h = 0;
    fprintf(stderr, "[FORK] [touch] layout %s reset to the default\n", g_lay_prof);
}
static void edit_leave_locked(void) { g_edit = 0; for (int i = 0; i < 10; i++) g_ep[i].id = -1; }
// cmd 0: save and leave, 1: reset to the default (the editor stays), 2: leave without saving (the saved layout back)
void touch_ui_edit_cmd(int cmd) {
    pthread_mutex_lock(&g_mu);
    if (g_edit) {
        if (cmd == 0) { layout_save(); edit_leave_locked(); fprintf(stderr, "[FORK] [touch] layout %s saved: %s\n", g_lay_prof, g_lay_path); }
        else if (cmd == 1) edit_reset_locked();
        else { g_lay_loaded = 0; g_w = g_h = 0; edit_leave_locked(); fprintf(stderr, "[FORK] [touch] layout %s: changes dropped\n", g_lay_prof); }
        click_locked();
    }
    pthread_mutex_unlock(&g_mu);
}
int touch_ui_editing(void) { return g_edit; }
// FORK: the editor bar's size slider: the selected control's name ("" none) and size as a fraction of the screen height, and setting it
const char *touch_ui_edit_sel(float *frac) { pthread_mutex_lock(&g_mu); int c = g_edit ? g_edit_sel : -1; if (frac) *frac = c >= 0 && g_h > 0 ? g_c[c].r / g_h : 0;
    pthread_mutex_unlock(&g_mu); return c >= 0 ? CTL_NAME[c] : ""; }
int touch_ui_edit_shape(void) {   // FORK: the editor bar's Shape button: the selected control's next shape
    pthread_mutex_lock(&g_mu); int c = g_edit_sel, sh = -1;
    if (g_edit && c >= 0 && c != C_STICK && c != C_RSTICK && c != C_MENU && c != C_THROW) { edit_store(c); g_ov[c].shape = (g_ov[c].shape + 1) & 3; sh = g_ov[c].shape; click_locked(); }
    pthread_mutex_unlock(&g_mu); return sh; }
void touch_ui_edit_size(float frac) { pthread_mutex_lock(&g_mu); int c = g_edit_sel;
    if (g_edit && c >= 0 && g_h > 0) { g_c[c].r = clampf(frac, 0.025f, 0.25f) * g_h; edit_store(c); } pthread_mutex_unlock(&g_mu); }
// the 2-second holds, checked once a frame (a finger held still sends no events): − toggles the editor (saving on the way out), + in the editor resets the layout
static void edit_holds_locked(void) {
    double t = now_s(); int minus = 0, plus = 0;
    if (g_edit) { for (int i = 0; i < 10; i++) if (g_ep[i].id >= 0 && !g_ep[i].mv && !g_pinch.active) { if (g_ep[i].c == C_MINUS) minus = 1; if (g_ep[i].c == C_PLUS) plus = 1; } }   // FORK v47: !mv
    else for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0 && g_ptr[i].target == C_MINUS) minus = 1;
    if (g_edit && plus && !minus) { g_edit_hold_t = 0; return; }   // FORK v47 (Arceus Native 1c2214e): + alone does nothing (it is being placed); + and − held together still reset
    if (!minus && !plus) { g_edit_hold_t = 0; return; }
    if (!g_edit_hold_t) { g_edit_hold_t = t; return; }
    if (t - g_edit_hold_t < 2.0) return;
    g_edit_hold_t = 1e18;   // fired: nothing more until the finger lifts
    if (plus && minus && g_edit) { edit_reset_locked(); click_locked(); return; }   // FORK: edit_reset_locked; v47: + and − together
    if (g_edit) { layout_save(); g_edit = 0; for (int i = 0; i < 10; i++) g_ep[i].id = -1; fprintf(stderr, "[touch] layout saved: %s\n", g_lay_path); }
    else { g_edit = 1; for (int i = 0; i < 10; i++) { if (g_ptr[i].id >= 0) { g_ep[0].id = g_ptr[i].id; g_ep[0].c = C_MINUS; g_ep[0].t0 = t; } g_ptr[i].id = -1; }
           g_btn = 0; g_lx = g_ly = g_rx = g_ry = 0; dm_input_set(0, 0, 0, 0, 0); fprintf(stderr, "[touch] layout editor on\n"); front_open_menu(2); }   // FORK: 2 = the editor's bar
    click_locked();
}
// Returns the touch buttons and sticks (for the host's touch source) after this event
// FORK v47: gesture shortcuts (DM_FORK_GESTURES=on, default off): a quick three-finger tap opens the ☰ menu; two fingers swiped down together press X (the
//   game's menu). Fingers that land on buttons still press them; the gestures are meant for the empty parts of the screen
static int g_gest = -1; static double g_g3_t, g_g2_t; static int g_g3, g_g2; static float g_g2_y[2]; static int32_t g_g2_id[2];
static int gestures_on(void) { if (g_gest < 0) { const char *e = getenv("DM_FORK_GESTURES"); g_gest = e && !strcmp(e, "on"); } return g_gest; }
static void gestures_event(const AInputEvent *e, int32_t act) {
    if (!gestures_on()) return;
    size_t n = AMotionEvent_getPointerCount(e); double t = now_s();
    if (act == AMOTION_EVENT_ACTION_DOWN) { g_g3 = 0; g_g2 = 0; }
    if (act == AMOTION_EVENT_ACTION_POINTER_DOWN && n == 3) { g_g3 = 1; g_g3_t = t; g_g2 = 0; }
    if (act == AMOTION_EVENT_ACTION_POINTER_DOWN && n == 2) { g_g2 = 1; g_g2_t = t; for (int k = 0; k < 2; k++) { g_g2_id[k] = AMotionEvent_getPointerId(e, (size_t)k); g_g2_y[k] = AMotionEvent_getY(e, (size_t)k); } }
    if (act == AMOTION_EVENT_ACTION_POINTER_DOWN && n > 3) g_g3 = 0;
    if (act == AMOTION_EVENT_ACTION_MOVE && g_g2 && n == 2 && t - g_g2_t < 0.6) {
        int down = 0; for (size_t k = 0; k < n; k++) for (int j = 0; j < 2; j++) if (AMotionEvent_getPointerId(e, k) == g_g2_id[j] && AMotionEvent_getY(e, k) - g_g2_y[j] > g_h * 0.10f) down++;
        if (down == 2) { g_g2 = 0; dm_input_press(DM_BTN_X, 80000000ull); click_locked(); g_tap_mute = t + 0.4; fprintf(stderr, "[FORK] [touch] gesture: two-finger swipe down = X\n"); } }
    if (act == AMOTION_EVENT_ACTION_UP) {
        if (g_g3 && t - g_g3_t < 0.45) { g_tap_mute = t + 0.3; click_locked(); fprintf(stderr, "[FORK] [touch] gesture: three-finger tap = the menu\n"); front_open_menu(0); }
        g_g3 = 0; g_g2 = 0; }
}
void touch_ui_event(const AInputEvent *e, float w, float h, uint64_t *btn, int32_t s[4]) {
    pthread_mutex_lock(&g_mu);
    layout(w, h); g_visible = 1; g_touch_t = now_s();
    if (g_edit) { edit_event(e); *btn = 0; s[0] = s[1] = s[2] = s[3] = 0; pthread_mutex_unlock(&g_mu); return; }
    int32_t act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    size_t idx = (size_t)((AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    gestures_event(e, act);   // FORK v47
    uint64_t before = g_btn;
    if (act == AMOTION_EVENT_ACTION_CANCEL) for (int i = 0; i < 10; i++) g_ptr[i].id = -1;
    else if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        size_t k = act == AMOTION_EVENT_ACTION_DOWN ? 0 : idx; float x = AMotionEvent_getX(e, k), y = AMotionEvent_getY(e, k);
        if (act == AMOTION_EVENT_ACTION_DOWN) for (int i = 0; i < 10; i++) g_ptr[i].id = -1;
        int t = target_at(x, y);
        if (t >= 0) for (int i = 0; i < 10; i++) if (g_ptr[i].id < 0) {
            if (t == C_STICK || t == C_CAM) for (int j = 0; j < 10; j++) if (g_ptr[j].id >= 0 && (g_ptr[j].target == C_STICK || g_ptr[j].target == C_CAM) && now_s() - g_ptr[j].t0 < 0.25) g_two_t = now_s();
            g_ptr[i].thr = g_ptr[i].lp = 0; g_ptr[i].id = AMotionEvent_getPointerId(e, k); g_ptr[i].target = t; g_ptr[i].x0 = g_ptr[i].x = g_ptr[i].lx = x; g_ptr[i].y0 = g_ptr[i].y = g_ptr[i].ly = y; g_ptr[i].lt = g_ptr[i].t0 = now_s();
            if (t >= C_A) g_held_until[t] = now_s() + 0.08;
            if (t == C_RSTICK || (t >= C_UP && t <= C_RIGHT)) g_inner_t = now_s();
            if (t == C_STICK) { float r = g_c[C_STICK].r; g_stick_on = 1;   // the base comes to the thumb, kept on screen
                (void)r; g_stick_x = g_c[C_STICK].cx; g_stick_y = g_c[C_STICK].cy;
                if (g_float > 0) { g_stick_x = x; g_stick_y = y; } }   // FORK: floating: the base where the thumb landed
            if (t == C_CAM && g_float > 0) { g_fr_x = g_frk_x = x; g_fr_y = g_frk_y = y; }   // FORK   // fixed, as on a Joy-Con: the thumb's offset from the base is the tilt
            break; }
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t id = AMotionEvent_getPointerId(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx);
        // gestures outside the controls (stick or camera area): a quick tap (under 0.25 s, barely moved) is a tap on a menu item while a menu shows (touch_menu.c),
        // else, on the camera side, A (talk, pick up, investigate); two fingers tapping together are B (back)
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id && (g_ptr[i].target == C_STICK || g_ptr[i].target == C_CAM) && !g_ptr[i].thr && now_s() - g_ptr[i].t0 < 0.3 &&
                                         fabsf(g_ptr[i].x - g_ptr[i].x0) < g_h * 0.02f && fabsf(g_ptr[i].y - g_ptr[i].y0) < g_h * 0.02f) {
            double t = now_s();
            if (t - g_two_t < 0.4) { g_two_t = 0; g_tap_mute = t + 0.3; dm_input_press(DM_BTN_B, 80000000ull); click_locked(); }
            else if (t >= g_tap_mute && !g_ptr[i].lp) { static int dg = -1; if (dg < 0) dg = getenv("DM_TOUCH_DIAG") != NULL; if (dg && !touch_menu_active()) touch_menu_tap(g_ptr[i].x0, g_ptr[i].y0, g_w, g_h);   // (diagnosis: what is under the tap)
                if (touch_menu_active()) touch_menu_tap(g_ptr[i].x0, g_ptr[i].y0, g_w, g_h); else if ((GESTURES && g_ptr[i].target == C_CAM) || g_float > 0) { dm_input_press(DM_BTN_A, 80000000ull); click_locked(); } }   // FORK: || floating (a tap on either half)
        }
        // quick swipes on the camera side (under 0.35 s, a tenth of the screen height or more): left / right = L / R (the item or Pokémon), down = X (items / Pokémon)
        for (int i = 0; i < 10; i++) if (GESTURES && g_ptr[i].id == id && g_ptr[i].target == C_CAM && !g_ptr[i].thr && !g_ptr[i].lp && now_s() - g_ptr[i].t0 < 0.35 && !touch_menu_active()) {
            float sx = g_ptr[i].x - g_ptr[i].x0, sy = g_ptr[i].y - g_ptr[i].y0; uint64_t sb = 0;
            if (fabsf(sx) > g_h * 0.10f && fabsf(sy) < fabsf(sx) * 0.6f) sb = sx > 0 ? DM_BTN_R : DM_BTN_L;
            else if (sy > g_h * 0.10f && fabsf(sx) < sy * 0.6f) sb = DM_BTN_X;
            if (sb) { dm_input_press(sb, 80000000ull); click_locked(); g_cam_vx = g_cam_vy = 0; }
        }
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id && g_ptr[i].target == C_MENU && hit(C_MENU, g_ptr[i].x, g_ptr[i].y, 1.5f)) { click_locked(); front_open_menu(0); }
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id && g_ptr[i].target == C_ALL && hit(C_ALL, g_ptr[i].x, g_ptr[i].y, 1.5f)) {   // FORK v47: ⊞ all buttons, until the screen changes
            int k = screen_kind(); g_ctx_over = g_ctx_over == k ? -1 : k; g_w = g_h = 0; click_locked();
            fprintf(stderr, "[FORK] [touch] all buttons %s (screen %d)\n", g_ctx_over >= 0 ? "on" : "off", k); }
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id && g_ptr[i].target == C_FF && now_s() - g_ptr[i].t0 < 0.35) { g_ff_toggle = !g_ff_toggle; click_locked();   // FORK: a tap toggles
            fprintf(stderr, "[FORK] [touch] fast-forward %s\n", g_ff_toggle ? "on" : "off"); }
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id) { if (g_ptr[i].target == C_CAM) g_cam_vx = g_cam_vy = 0; g_ptr[i].id = -1; }
    }
    // FORK v47 (from Arceus Native 1c2214e): a finger the system never lifted. Every move (and every new finger) lists all fingers down, so one held here that the
    //   event does not list went without its UP (a system gesture or panel took it): it lets go of its button or stick (before: a ghost press until the next touch)
    if (act == AMOTION_EVENT_ACTION_MOVE || act == AMOTION_EVENT_ACTION_POINTER_DOWN)
        for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0) { int seen = 0;
            for (size_t k = 0; k < AMotionEvent_getPointerCount(e) && !seen; k++) seen = AMotionEvent_getPointerId(e, k) == g_ptr[i].id;
            if (!seen) { static int nl; if (nl++ < 20) fprintf(stderr, "[FORK] [touch] a finger on control %d was never lifted by the system: let go\n", g_ptr[i].target);
                if (g_ptr[i].target == C_CAM) g_cam_vx = g_cam_vy = 0; g_ptr[i].id = -1; } }
    if (act == AMOTION_EVENT_ACTION_MOVE || act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN)
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int32_t id = AMotionEvent_getPointerId(e, k);
            for (int i = 0; i < 10; i++) if (g_ptr[i].id == id) {
                g_ptr[i].x = AMotionEvent_getX(e, k); g_ptr[i].y = AMotionEvent_getY(e, k);
                if (g_ptr[i].target == C_CAM && !g_ptr[i].thr && act == AMOTION_EVENT_ACTION_MOVE && GESTURES && now_s() - g_ptr[i].t0 < 0.3 && !touch_menu_active()) {   // a quick swipe up: a throw
                    float up = g_ptr[i].y0 - g_ptr[i].y; if (up > g_h * 0.10f && fabsf(g_ptr[i].x - g_ptr[i].x0) < up * 0.5f) { g_ptr[i].thr = 1; g_cam_vx = g_cam_vy = 0; click_locked(); }
                }
                if ((g_ptr[i].target == C_CAM || g_ptr[i].target == C_THROW) && !g_ptr[i].thr && act == AMOTION_EVENT_ACTION_MOVE) {   // the finger's speed, smoothed (the throw button's finger aims)
                    double t = now_s(), dt = t - g_ptr[i].lt;
                    if (dt > 0.001) { float vx = (g_ptr[i].x - g_ptr[i].lx) / (float)dt, vy = (g_ptr[i].y - g_ptr[i].ly) / (float)dt;
                        g_cam_vx = g_cam_vx * 0.5f + vx * 0.5f; g_cam_vy = g_cam_vy * 0.5f + vy * 0.5f; g_cam_t = t;
                        g_ptr[i].lx = g_ptr[i].x; g_ptr[i].ly = g_ptr[i].y; g_ptr[i].lt = t; }
                }
                if (g_ptr[i].target >= C_A && g_ptr[i].target <= C_Y)   // sliding across the face buttons
                    for (int c = C_A; c <= C_Y; c++) if (c != g_ptr[i].target && hit(c, g_ptr[i].x, g_ptr[i].y, 1.0f)) { g_ptr[i].target = c; g_held_until[c] = now_s() + 0.08; break; }
            }
        }
    recompute_locked();
    if (g_btn & ~before) click_locked();   // a new press: one click
    *btn = g_btn; s[0] = g_lx; s[1] = g_ly; s[2] = g_rx; s[3] = g_ry;
    pthread_mutex_unlock(&g_mu);
}
void touch_ui_hide(void) { pthread_mutex_lock(&g_mu); g_visible = 0; pthread_mutex_unlock(&g_mu); }
// FORK v47 (from Arceus Native 1c2214e): the app lost focus or went to the background (android_main.c): every finger lets go. Android does not always send the lift
//   for fingers down when a panel opens over the game (the notification shade, Game Booster, the ☰ menu), and their buttons stayed pressed
void touch_ui_release(void) {
    pthread_mutex_lock(&g_mu); int n = 0;
    for (int i = 0; i < 10; i++) { if (g_ptr[i].id >= 0) n++; g_ptr[i].id = -1; g_ep[i].id = -1; }
    g_pinch.active = 0; g_cam_vx = g_cam_vy = 0; g_stick_on = g_rstick_on = 0; for (int c = 0; c < NCTL; c++) g_held_until[c] = 0;
    recompute_locked(); dm_input_set(g_btn, g_lx, g_ly, g_rx, g_ry);
    pthread_mutex_unlock(&g_mu);
    if (n) fprintf(stderr, "[FORK] [touch] focus lost with %d finger%s down: let go\n", n, n == 1 ? "" : "s");
}
// GameActivity.java: a controller connected hides the controls at once (no input needed first); the last one disconnected shows them again
void touch_ui_pad(int connected) { pthread_mutex_lock(&g_mu); g_visible = !connected; if (!connected) g_touch_t = now_s(); pthread_mutex_unlock(&g_mu); }
#else
// Mac test hook (DM_OVL_SHOT): show control c as held, the stick pushed by (dx, dy) in units of its radius
void touch_ui_demo(int c, float dx, float dy) {
    pthread_mutex_lock(&g_mu); layout(1, 1); g_ptr[0].id = 1; g_ptr[0].target = c; g_btn = c >= C_A && c < NCTL ? g_c[c].btn : 0; g_w = g_h = 0; pthread_mutex_unlock(&g_mu);
    (void)dx; (void)dy;
}
#endif

// ---------- drawing ----------
// The front end's input push for what changes with time alone (a tap's hold running out, the camera coming to rest): called once a frame by the drawing
#ifdef __ANDROID__
static void frame_input_locked(void) {
    edit_holds_locked(); if (g_edit) return;   // (the editor: the game gets nothing)
    uint64_t b0 = g_btn; int32_t r0 = g_rx, r1 = g_ry;
    recompute_locked();
    if (g_btn != b0 || g_rx != r0 || g_ry != r1) dm_input_set(g_btn, g_lx, g_ly, g_rx, g_ry);
}
#else
static void frame_input_locked(void) {}
#endif
static dm_ovl_item *add(dm_ovl_item *it, int *n, int max, float cx, float cy, float r, int shape, int R, int G, int B, float a) {
    if (*n >= max) return NULL;
    dm_ovl_item *o = &it[(*n)++];
    *o = (dm_ovl_item){ cx - r, cy - r, 2 * r, 2 * r, clampf(R, 0, 255) / 255.0f, clampf(G, 0, 255) / 255.0f, clampf(B, 0, 255) / 255.0f, clampf(a, 0, 1), shape, 0.1f, 0.4f, OVL_G_NONE, 0.5f, 0 };
    return o;
}
// a glossy bubble: shadow, dark rim, body, glow below, a gloss spot, a bright rim; the label on top
static int g_bshape;   // FORK: the shape bubble() draws (set around each call: g_ov[].shape)
static void rr(dm_ovl_item *it, int *n, int max, float x, float y, float w, float h, float rad, int R, int G, int B, float a) {   // FORK: a rounded rectangle
    dm_ovl_item *o = add(it, n, max, x, y, h / 2, OVL_RRECT, R, G, B, a); if (o) { o->x = x - w / 2; o->y = y - h / 2; o->w = w; o->h = h; o->radius = rad; } }
static void bubble(dm_ovl_item *it, int *n, int max, const ctl *c, int on, int teal, float a) {
    float r = on ? c->r * 0.95f : c->r, x = c->cx, y = c->cy, th = g_h / 1080.0f;
    int br = teal ? 30 : 38, bg = teal ? 170 : 128, bb = teal ? 150 : 200;
    if (on) { br += 70; bg += 60; bb += 40; }
    if (g_bshape) {   // FORK: rounded square, square, pill (as the game's battle buttons): the same layers as the round button
        float w = g_bshape == 3 ? 3.0f * r : 1.8f * r, h = g_bshape == 3 ? 1.6f * r : 1.8f * r, rad = g_bshape == 1 ? 0.28f : g_bshape == 2 ? 0.10f : 0.5f;
        rr(it, n, max, x, y + r * 0.06f, w * 1.03f, h * 1.03f, rad, 5, 30, 60, 0.35f * a);
        rr(it, n, max, x, y, w, h, rad, 185, 232, 255, 0.95f * a);
        rr(it, n, max, x, y, w - 2.8f * th * 2, h - 2.8f * th * 2, rad, br / 2, bg / 2, bb / 2, 0.92f * a);
        rr(it, n, max, x, y, w * 0.90f, h * 0.86f, rad, br, bg, bb, (on ? 0.95f : 0.72f) * a);
        rr(it, n, max, x, y - h * 0.22f, w * 0.80f, h * 0.30f, 0.5f, 255, 255, 255, 0.12f * a);
        dm_ovl_item *o = add(it, n, max, x, y, r, OVL_DISC, 0, 0, 0, 0); if (o) { o->glyph = c->glyph; o->gscale = c->glyph == OVL_G_ZL || c->glyph == OVL_G_ZR ? 0.62f : 0.55f; o->galpha = a; }
        return;
    }
    add(it, n, max, x, y + r * 0.06f, r * 1.02f, OVL_DISC, 5, 30, 60, 0.35f * a);
    add(it, n, max, x, y, r, OVL_DISC, br / 2, bg / 2, bb / 2, 0.80f * a);
    add(it, n, max, x, y, r * 0.90f, OVL_DISC, br, bg, bb, (on ? 0.95f : 0.72f) * a);
    add(it, n, max, x, y + r * 0.22f, r * 0.62f, OVL_DISC, br + 30, bg + 40, bb + 30, 0.35f * a);
    add(it, n, max, x - r * 0.30f, y - r * 0.40f, r * 0.24f, OVL_DISC, 255, 255, 255, 0.28f * a);
    dm_ovl_item *o = add(it, n, max, x, y, r * 0.90f, OVL_RING, 185, 232, 255, 0.95f * a); if (o) o->ring = clampf(2.8f * th / (r * 0.9f), 0.02f, 0.2f);
    o = add(it, n, max, x, y, r, OVL_DISC, 0, 0, 0, 0); if (o) { o->glyph = c->glyph; o->gscale = c->glyph == OVL_G_ZL || c->glyph == OVL_G_ZR ? 0.62f : 0.55f; o->galpha = a; }
}
// the throw button: a Poké Ball (shadow, dark rim, red, a dark band, the white centre button), pressed = slightly smaller and brighter
static void pokeball(dm_ovl_item *it, int *n, int max, const ctl *c, int on, float a) {
    float r = on ? c->r * 0.93f : c->r, x = c->cx, y = c->cy, th = g_h / 1080.0f; int lift = on ? 40 : 0;
    add(it, n, max, x, y + r * 0.07f, r * 1.03f, OVL_DISC, 5, 20, 40, 0.35f * a);
    add(it, n, max, x, y, r, OVL_DISC, 30, 30, 36, 0.92f * a);
    add(it, n, max, x, y, r * 0.90f, OVL_DISC, 225 + lift / 3, 40 + lift, 45 + lift, 0.95f * a);
    add(it, n, max, x, y + r * 0.45f, r * 0.62f, OVL_DISC, 245, 245, 245, 0.55f * a);
    dm_ovl_item *o = add(it, n, max, x, y, r * 0.90f, OVL_RRECT, 30, 30, 36, 0.95f * a); if (o) { o->y = y - r * 0.09f; o->h = r * 0.18f; o->radius = 0.5f; }
    add(it, n, max, x, y, r * 0.30f, OVL_DISC, 30, 30, 36, 0.95f * a);
    add(it, n, max, x, y, r * 0.21f, OVL_DISC, 250, 250, 250, 0.98f * a);
    add(it, n, max, x - r * 0.32f, y - r * 0.45f, r * 0.18f, OVL_DISC, 255, 255, 255, 0.35f * a);
    o = add(it, n, max, x, y, r, OVL_RING, 210, 225, 240, 0.9f * a); if (o) o->ring = clampf(2.6f * th / r, 0.01f, 0.2f);
}
// the settings button: a bubble with three white bars (☰)
static void menu_icon(dm_ovl_item *it, int *n, int max, const ctl *c, float a) {
    bubble(it, n, max, c, 0, 0, a);
    float bw = c->r * 0.95f, bh = c->r * 0.13f;
    for (int k = -1; k <= 1; k++) { dm_ovl_item *o = add(it, n, max, c->cx, c->cy, c->r, OVL_RRECT, 255, 255, 255, 0.95f * a);
        if (o) { o->x = c->cx - bw / 2; o->y = c->cy + k * c->r * 0.30f - bh / 2; o->w = bw; o->h = bh; o->radius = 0.5f; } }
}
// the stick: a ring with an inner ring, and a glossy knob
static void compass(dm_ovl_item *it, int *n, int max, float a, float g_stick_x, float g_stick_y, float g_knob_x, float g_knob_y, float r) {   // (base, knob, radius)
    float th = g_h / 1080.0f, kr = r * 0.42f;
    add(it, n, max, g_stick_x, g_stick_y, r, OVL_DISC, 30, 100, 165, 0.45f * a);
    dm_ovl_item *o = add(it, n, max, g_stick_x, g_stick_y, r, OVL_RING, 190, 235, 255, 0.80f * a); if (o) o->ring = clampf(2.6f * th / r, 0.01f, 0.2f);
    o = add(it, n, max, g_stick_x, g_stick_y, r * 0.80f, OVL_RING, 190, 235, 255, 0.35f * a); if (o) o->ring = clampf(1.6f * th / (r * 0.8f), 0.01f, 0.2f);
    add(it, n, max, g_knob_x, g_knob_y + kr * 0.12f, kr, OVL_DISC, 0, 0, 0, 0.25f * a);
    add(it, n, max, g_knob_x, g_knob_y, kr, OVL_DISC, 70, 165, 225, 0.85f * a);
    add(it, n, max, g_knob_x, g_knob_y - kr * 0.32f, kr * 0.6f, OVL_DISC, 255, 255, 255, 0.14f * a);
    o = add(it, n, max, g_knob_x, g_knob_y, kr, OVL_RING, 220, 245, 255, 0.95f * a); if (o) o->ring = clampf(2.4f * th / kr, 0.02f, 0.3f);
}
// the player's settings (env.txt, set on the app's settings screen): DM_TOUCH_OPACITY 20-100 (%), DM_TOUCH_FADE seconds untouched before the controls go faint (0 = never)
static float g_opacity = -1, g_fade = 3.0f;
// FORK: the controls' colour (DM_FORK_TOUCH_COLOR: "" the stock blue, grey, white, dark, red, green, purple) and overall size (DM_FORK_TOUCH_SIZE 50-150 %, the default
//   layout's sizes; a control resized in the editor keeps its own). The colour keeps each part's shading: its brightness is carried over to the chosen tint
static int g_tint;   // (g_tsize: with g_float, above)
static int tint_of(const char *v) { return !v ? 0 : !strcmp(v, "grey") ? 1 : !strcmp(v, "white") ? 2 : !strcmp(v, "dark") ? 3 : !strcmp(v, "red") ? 4 : !strcmp(v, "green") ? 5 : !strcmp(v, "purple") ? 6 : 0; }
static void tint(dm_ovl_item *o) {
    if (!g_tint || o->a <= 0) return;
    float l = 0.30f * o->r + 0.59f * o->g + 0.11f * o->b;   // (colours are 0-1 here: add())
    static const float k[7][3] = { {0,0,0}, {1,1,1}, {1,1,1}, {1,1,1}, {1.0f,0.35f,0.35f}, {0.40f,1.0f,0.50f}, {0.75f,0.45f,1.0f} };
    float m = g_tint == 2 ? 1.5f : g_tint == 3 ? 0.45f : 1.0f;
    o->r = clampf(l * m * k[g_tint][0], 0, 1); o->g = clampf(l * m * k[g_tint][1], 0, 1); o->b = clampf(l * m * k[g_tint][2], 0, 1);
}
// the in-game menu changed one (android_main.c nativeSetting): value NULL = back to the default
void touch_ui_setting(const char *key, const char *value) {
    pthread_mutex_lock(&g_mu);
    if (!strcmp(key, "DM_TOUCH_OPACITY")) g_opacity = value ? clampf((float)atof(value) / 100.0f, 0.05f, 1.0f) : 1.0f;   // FORK: down to 5 % (stock 20)
    else if (!strcmp(key, "DM_FORK_TOUCH_COLOR")) g_tint = tint_of(value);   // FORK
    else if (!strcmp(key, "DM_FORK_TOUCH_SIZE")) { g_tsize = value ? clampf((float)atof(value) / 100.0f, 0.5f, 1.5f) : 1.0f; g_w = g_h = 0; }   // FORK (the layout made again)
    else if (!strcmp(key, "DM_TOUCH_FADE")) g_fade = value ? fmaxf(0.0f, (float)atof(value)) : 3.0f;
    else if (!strcmp(key, "DM_FORK_TOUCH")) g_float = value && !strcmp(value, "float");   // FORK
    else if (!strcmp(key, "DM_FORK_BATTLE")) battle_mode(value && !strcmp(value, "on"));   // FORK v46
    else if (!strcmp(key, "DM_FORK_GESTURES")) g_gest = value && !strcmp(value, "on");   // FORK v47
    else if (!strncmp(key, "DM_FORK_CTX_", 12)) { int on = value && !strcmp(value, "on");   // FORK v47: the context switches
        if (!strcmp(key, "DM_FORK_CTX_DIALOGUE")) g_ctx_dlg = on; else if (!strcmp(key, "DM_FORK_CTX_MENU")) g_ctx_menu = on; else if (!strcmp(key, "DM_FORK_CTX_WORLD")) g_ctx_world = on;
        g_ctx_any = g_ctx_dlg || g_ctx_menu || g_ctx_world; g_ctx_over = -1; g_w = g_h = 0; census_sync(); }
    else if (!strcmp(key, "DM_FORK_FF")) { g_ffscope = !value ? 0 : !strcmp(value, "battle") ? 1 : !strcmp(value, "all") ? 2 : 0; if (!g_ffscope) g_ff_toggle = 0; census_sync(); }   // FORK
    else if (!strcmp(key, "DM_FORK_FF_SPEED")) { if (&dm_fork_ff_speed) dm_fork_ff_speed = value ? atoi(value) : 0; }   // FORK (host/fork_ff.c)
    else if (!strcmp(key, "DM_FORK_TURBO")) g_turbo_on = value && !strcmp(value, "on");   // FORK
    else if (!strcmp(key, "DM_FORK_TURBO_HZ")) g_turbo_hz = value ? clampf((float)atof(value), 3, 20) : 10;   // FORK
    else if (!strcmp(key, "DM_FORK_BATTLE_FADE")) { g_bfade = value && *value && atof(value) >= 0 ? clampf((float)atof(value), 0, 10) : -1; census_sync(); }   // FORK
    else if (!strcmp(key, "DM_FORK_BATTLE_OPACITY")) { g_bopacity = value && atof(value) > 0 ? clampf((float)atof(value) / 100.0f, 0.05f, 1.0f) : 0; census_sync(); }   // FORK
    else if (!strcmp(key, "DM_FORK_DEADZONE")) g_dead = value ? clampf((float)atof(value) / 100.0f, 0.0f, 0.4f) : 0.12f;   // FORK
    else if (!strcmp(key, "DM_TOUCH_L3R3")) { g_l3r3 = value && !strcmp(value, "on"); g_w = g_h = 0; }   // (laid out again on the next frame)
    else if (!strcmp(key, "DM_TOUCH_CAM_SENS")) { int v = value ? atoi(value) : 100; g_cam_pct = v < 25 || v > 300 ? 100 : v; }   // FORK v47
    else if (!strcmp(key, "DM_TOUCH_CAMERA")) g_cam_stick = value && !strcmp(value, "stick");   // FORK v47
    else if (!strcmp(key, "DM_TOUCH_RSTICK")) { g_rs_off = value && !strcmp(value, "off"); g_w = g_h = 0; }   // FORK v47
#ifdef __ANDROID__
    else { void touch_ui_haptic_setting(const char *, const char *); touch_ui_haptic_setting(key, value); }   // DM_TOUCH_HAPTIC, DM_RUMBLE
#endif
    g_touch_t = now_s();
    pthread_mutex_unlock(&g_mu);
}
#ifdef __ANDROID__
// the in-game menu's "Move the touch controls": the layout editor (as holding − for 2 s)
void touch_ui_edit(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_edit) { g_edit = 1; for (int i = 0; i < 10; i++) { g_ptr[i].id = -1; g_ep[i].id = -1; } g_btn = 0; g_lx = g_ly = g_rx = g_ry = 0; dm_input_set(0, 0, 0, 0, 0); g_visible = 1; g_touch_t = now_s(); }
    pthread_mutex_unlock(&g_mu);
}
#endif
static void prefs(void) {
    if (g_opacity >= 0) return;
    const char *o = getenv("DM_TOUCH_OPACITY"), *f = getenv("DM_TOUCH_FADE");
    g_opacity = o ? clampf((float)atof(o) / 100.0f, 0.05f, 1.0f) : 1.0f;   // FORK: down to 5 %
    { const char *tc = getenv("DM_FORK_TOUCH_COLOR"), *ts = getenv("DM_FORK_TOUCH_SIZE"); g_tint = tint_of(tc); if (ts && *ts) { g_tsize = clampf((float)atof(ts) / 100.0f, 0.5f, 1.5f); g_w = g_h = 0; } }   // FORK if (f) g_fade = fmaxf(0.0f, (float)atof(f));
    { const char *ft = getenv("DM_FORK_TOUCH"), *dz = getenv("DM_FORK_DEADZONE"); g_float = ft && !strcmp(ft, "float");   // FORK: the control scheme, the dead zone
      { const char *b = getenv("DM_FORK_BATTLE"), *bh = getenv("DM_FORK_BATTLE_HIDE"), *bl = getenv("DM_FORK_BATTLE_LAYOUT");   // FORK v46: battle mode (the old keys: before v46)
        battle_mode(b && *b ? !strcmp(b, "on") : (bh && !strcmp(bh, "on")) || (bl && !strcmp(bl, "on"))); }
      { static const char *const keys[] = { "DM_FORK_FF", "DM_FORK_TURBO", "DM_FORK_TURBO_HZ", "DM_FORK_BATTLE_OPACITY", "DM_FORK_BATTLE_FADE", "DM_FORK_CTX_DIALOGUE", "DM_FORK_CTX_MENU", "DM_FORK_CTX_WORLD" };   // FORK: the rest, as the menu sets them
        pthread_mutex_unlock(&g_mu); for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) { const char *v = getenv(keys[i]); if (v && *v) touch_ui_setting(keys[i], v); } pthread_mutex_lock(&g_mu); census_sync(); }
      if (dz && *dz) g_dead = clampf((float)atof(dz) / 100.0f, 0.0f, 0.4f); if (g_float) fprintf(stderr, "[FORK] [touch] floating sticks, tap for A (dead zone %.0f %%)\n", g_dead * 100); }
}
int front_overlay(dm_ovl_item *it, int max, int width, int height) {
    pthread_mutex_lock(&g_mu); prefs();
    { static int was = -1; int b = g_blayout && in_battle(); if (b != was) { was = b; if (!g_edit) g_w = g_h = 0; } ff_publish(); }   // FORK: battle layout in / out; FF scope
    layout((float)width, (float)height);
    frame_input_locked();
    int n = 0;
    if (g_visible && g_edit) {   // the editor: every control at full strength with a yellow ring, the hidden ones as ghosts
        for (int c = 0; c < NCTL; c++) { if (c == C_CAM || ctl_off(c)) continue; const ctl *k = &g_c[c];
            if (GESTURES_ON && !k->shown && !g_ov[c].set && c != C_PLUS && c != C_MINUS && c != C_STICK && c != C_MENU) continue;
            float a = k->shown ? 1.0f : 0.3f;
            if (c == C_STICK || c == C_RSTICK) compass(it, &n, max, a, k->cx, k->cy, k->cx, k->cy, k->r);
            else if (c == C_THROW) pokeball(it, &n, max, k, 0, a); else if (c == C_MENU) menu_icon(it, &n, max, k, a); else { g_bshape = g_ov[c].set ? g_ov[c].shape : 0; bubble(it, &n, max, k, 0, 0, a); g_bshape = 0; }   // FORK: shape
            dm_ovl_item *o = add(it, &n, max, k->cx, k->cy, k->r * 1.12f, OVL_RING, 255, 210, 60, 0.95f * a); if (o) o->ring = clampf(4.0f * (g_h / 1080.0f) / (k->r * 1.12f), 0.02f, 0.3f); }
    } else if (g_visible) {
        double t = now_s(); float fade = g_bfade >= 0 && in_battle() ? g_bfade : g_fade;   // FORK: battle fade
        int idle = ((fade > 0 && t - g_touch_t > fade) || touch_menu_active()) && !g_btn && !g_stick_on && !g_rstick_on;   // (in a menu its items are tapped directly)   // nothing touched for 3 s: everything faint
        float ia = idle ? 0.22f : 1.0f;
        int faint = 0; (void)g_inner_t;   // FORK: the D-pad and the right stick fade with the rest (upstream: almost gone 2 s after their last touch)
        for (int c = C_A; c < NCTL; c++) { if (!g_c[c].shown || fork_hidden(c)) continue;   // FORK: battle_hidden
            int on = (g_btn & g_c[c].btn) != 0 || (c == C_FF && (g_ff_toggle || g_ff_hold)) || (c == C_TURBO && g_turbo_held) || (c == C_ALL && g_ctx_over >= 0);   // FORK: lit while on
            float al = c >= C_UP && c <= C_RIGHT && faint && !on ? 0.25f : 1.0f; if (idle) al = fminf(al, ia);
            if (c == C_THROW) { pokeball(it, &n, max, &g_c[c], on, al); continue; }
            if (c == C_MENU) { menu_icon(it, &n, max, &g_c[c], idle ? fmaxf(al, 0.45f) : al); continue; }   // (never quite gone: it is how the settings are found)
            { ctl k = g_c[c];   // FORK: battle icons (Poké Ball on X: the quick ball; back on B; info on Y)
              if (g_bicons && in_battle()) k.glyph = c == C_X ? OVL_G_BALL : c == C_B ? OVL_G_BACK : c == C_Y ? OVL_G_INFO : k.glyph;
              g_bshape = g_ov[c].set ? g_ov[c].shape : 0; bubble(it, &n, max, &k, on, 0, al); g_bshape = 0; }   // FORK: shape
            if (c >= C_UP && c <= C_RIGHT && al == 1.0f) {   // FORK: <= C_RIGHT (the controls after the D-pad have no direction dot)
                float r = g_c[c].r, ox = c == C_LEFT ? -0.42f : c == C_RIGHT ? 0.42f : 0, oy = c == C_UP ? -0.42f : c == C_DOWN ? 0.42f : 0;
                add(it, &n, max, g_c[c].cx + ox * r, g_c[c].cy + oy * r, r * 0.22f, OVL_DISC, 255, 255, 255, 0.9f); } }
        if (g_float <= 0 || g_stick_on)   // FORK: floating sticks: shown only under the thumb
            compass(it, &n, max, g_stick_on ? 1.0f : idle ? ia : 0.75f, g_stick_x, g_stick_y, g_knob_x, g_knob_y, g_c[C_STICK].r);
        if (g_fr_on) compass(it, &n, max, 1.0f, g_fr_x, g_fr_y, g_frk_x, g_frk_y, g_c[C_STICK].r);   // FORK: the floating camera stick under the finger
        if (g_c[C_RSTICK].shown && g_float <= 0 && !fork_hidden(C_RSTICK)) compass(it, &n, max, g_rstick_on ? 1.0f : idle ? ia : faint ? 0.25f : 0.75f, g_c[C_RSTICK].cx,   // faint while unused: it and the D-pad sit over the dialogue box
             g_c[C_RSTICK].cy, g_rknob_x, g_rknob_y, g_c[C_RSTICK].r);
        float op = g_bopacity > 0 && in_battle() ? g_bopacity : g_opacity;   // FORK: battle opacity
        for (int i = 0; i < n; i++) { it[i].a *= op; it[i].galpha *= op; tint(&it[i]); }   // FORK: tint
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

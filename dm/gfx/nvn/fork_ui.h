// FORK: the UI half of Picture "Fill, corrected" (host/fork_aspect.c has the camera half). Included by nvn_vk.c; runs on the replay's thread.
// Pokémon Sword 1.3.2 ends a frame (frame captures, Adreno 840): the 3D scene is composited into a 1920x1080 image by one draw with vertex shader 7fa0c6373aca1086,
// the UI (HUD, text boxes, menus) is drawn into that same image after it, and one draw with vertex shader 389df45bf4d9a0f6 copies the image to the one presented.
// Battles with a split screen composite each half with vertex shader e0feaaf96d273c2b instead, then draw one effect (57319aada7d3772b) before the UI (capture, Nov 9).
// So, in each frame, the draws into the composite's image are the 3D scene until the first draw with a UI shader (af0ef3ac1bd35ad3, 56af7f00aa776d97); from then on
// they are UI. A composite the list does not know yet stays a full-size scene draw (the 3D is already right through the camera), not a shrunk one.
// The UI draws' viewport and scissor are shrunk around the image's centre by the scale
// fork_aspect.c gives (narrower screen: in y; wider: in x). After the picture is stretched over the screen the UI has its own shape again; the 3D behind it still
// fills the image. A frame with UI but no 3D composite (the bag, the X menu) gets the parts the shrunk UI no longer covers cleared to black first.
#pragma once
void dm_fork_ui_scale(float *kx, float *ky);

static const void *g_fk_img;   // the composite's image (its vtex), learnt from the composite draw
static int g_fk_comp, g_fk_clear;   // this frame: the composite was drawn / the bands were cleared
static uint64_t g_fk_ui;   // UI draws shrunk (logged now and then)

// Fades and flashes are UI draws too, one quad over the whole picture with a tiny one-colour texture (4x4, capture Nov 9); shrunk, they left the bands unfaded (the
// flashing bars). So a shrunk draw whose textures are all 4x4 or smaller gets its full viewport and scissor back once its textures are known (fork_ui_fill, after
// nvn_vk.c's push_stage; fork_ui_tex notes their sizes from tex_view_for)
static int g_fk_sq, g_fk_texmax, g_fk_ntex, g_fk_screenish; static VkViewport g_fk_v0; static VkRect2D g_fk_sc0; static uint64_t g_fk_fills;   // FORK v38: g_fk_screenish (it samples a picture-sized image: a full-screen effect)
static int g_fk_banner, g_fk_tw, g_fk_th; static float g_fk_ky, g_fk_kx;   // FORK v44: this draw samples the 1200x180 "spotted" banner; the image's size; the y scale
static _Atomic int g_origin_ll;   // (nvn_vk.c, below: the game draws with a lower-left origin, so the screen's bottom is the image's row 0)
static void fork_ui_tex(uint32_t w, uint32_t h) { if (!g_fk_sq) return; g_fk_ntex++; if (w == 1200 && h == 180) g_fk_banner = 1; if (w == 128 && h == 4) g_fk_banner = 2;
    if (w >= 1280 && h >= 700 && w * 10 >= h * 16 && w * 10 <= h * 20) g_fk_screenish = 1;   // (a 16:9-ish image of the picture's size: the trainer-approach overlay, blurs, wipes)
    if ((int)w > g_fk_texmax) g_fk_texmax = (int)w; if ((int)h > g_fk_texmax) g_fk_texmax = (int)h; }
static char g_fk_sqkey[17];   // the shrunk draw's vertex shader (the log below)
extern _Atomic int dm_fork_screen;   // (below)
volatile int dm_fork_capture_trainer;   // FORK v41: android_main.c DM_FORK_CAPTURE=trainer (the menu's "Capture the trainer-spotted moment"): captures the first frame
//   in the overworld whose shrunk UI has a shader other than the overworld's usual ones (56af7f00, af0ef3ac, af80749a), at once (no 90-frame wait)
static void fork_ui_fill(VkCommandBuffer cb) {
    if (!g_fk_sq) return; g_fk_sq = 0;
    { static char seen[96][24]; static int ns;   // FORK diagnostics: each kind of shrunk UI draw once (shader, largest texture side), to tell overlays from icons
      char k[24]; snprintf(k, sizeof k, "%.8s %d", g_fk_sqkey, g_fk_ntex ? g_fk_texmax : 0); int i;
      for (i = 0; i < ns && strcmp(seen[i], k); i++) {}
      if (i == ns && ns < 96) { memcpy(seen[ns++], k, sizeof k); VLOG("[FORK] ui draw kind: vertex %.8s, largest texture side %d", g_fk_sqkey, g_fk_ntex ? g_fk_texmax : 0); } }
    // FORK v43: the trainer battle intro (capture Nov 12): fa0b9117 draws the black bars that slide in at the top and bottom, 63b6f752 the diagonal divider between
    //   the two sides. Shrunk, the bars stopped at the UI band's edges (the "spotted" fade) and the divider no longer met the split 3D behind it: always full size.
    //   (v40's 438515ea guess dropped: not part of it)
    if (dm_fork_capture_trainer && g_fk_comp && atomic_load(&dm_fork_screen) == 0 && strncmp(g_fk_sqkey, "56af7f00", 8) && strncmp(g_fk_sqkey, "af0ef3ac", 8)
        && strncmp(g_fk_sqkey, "af80749a", 8) && atomic_load(&g_cap) == 0) {
        dm_fork_capture_trainer = 0; atomic_store(&g_cap, 1);
        VLOG("[FORK] capture: overworld UI shader %.16s (largest texture side %d, %d textures): this frame", g_fk_sqkey, g_fk_ntex ? g_fk_texmax : 0, g_fk_ntex); }
    // FORK v44: the trainer "spotted you" banner (capture Nov 13): b6ff050a with its 1200x180 texture, a dark strip in the screen's bottom-right corner. Shrunk around the
    //   centre it floated above the bottom with nothing under it; it keeps its shape (the y scale) but sits on the screen's real bottom edge
    // v47: the visible part is the shading before the banner (capture Nov 13): af0ef3ac stretching a 128x4 gradient over the bottom-right, then af0ef3ac again with
    //   no new texture (the same one still bound); shrunk, it stopped in a hard line at the band's lower edge. Both anchored like the banner
    static int after_shade;   // the last draw was that shading: an af0ef3ac draw with no texture of its own right after it belongs to it
    int shade = g_fk_banner == 2 && !strncmp(g_fk_sqkey, "af0ef3ac", 8), shade2 = after_shade && !g_fk_ntex && !strncmp(g_fk_sqkey, "af0ef3ac", 8);
    after_shade = shade;
    if (((g_fk_banner == 1 && !strncmp(g_fk_sqkey, "b6ff050a", 8)) || shade || shade2) && g_fk_th > 0) {
        VkViewport v = g_fk_v0; VkRect2D sc = g_fk_sc0; float k = g_fk_ky, H = (float)g_fk_th; int ll = atomic_load(&g_origin_ll);
        float a = ll ? 0.0f : H;   // the screen's bottom edge, in the image
        v.y = a + (v.y - a) * k; v.height *= k;
        { float cx = g_fk_tw * 0.5f, kx = g_fk_kx; v.x = cx + (v.x - cx) * kx; v.width *= kx;   // (x as every UI draw: around the centre)
          float x0 = cx + ((float)sc.offset.x - cx) * kx, x1 = cx + ((float)sc.offset.x + (float)sc.extent.width - cx) * kx; if (x0 < 0) x0 = 0; if (x1 > g_fk_tw) x1 = (float)g_fk_tw;
          sc.offset.x = (int32_t)floorf(x0); sc.extent.width = (uint32_t)(ceilf(x1) > floorf(x0) ? ceilf(x1) - floorf(x0) : 0); }
        float y0 = a + ((float)sc.offset.y - a) * k, y1 = a + ((float)sc.offset.y + (float)sc.extent.height - a) * k; if (y0 < 0) y0 = 0; if (y1 > H) y1 = H;
        sc.offset.y = (int32_t)floorf(y0); sc.extent.height = (uint32_t)(ceilf(y1) > floorf(y0) ? ceilf(y1) - floorf(y0) : 0);
        vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
        static int said[3]; int wh = shade ? 1 : shade2 ? 2 : 0; if (!said[wh]++) VLOG("[FORK] spotted %s: kept on the screen's bottom edge (scale %.3f, origin %s)", wh == 1 ? "shading" : wh == 2 ? "shading (2nd draw)" : "banner", k, ll ? "lower left" : "upper left");
        return; }
    int full = !strncmp(g_fk_sqkey, "fa0b9117", 8) || !strncmp(g_fk_sqkey, "63b6f752", 8);
    if (!full && (!g_fk_ntex || g_fk_texmax > 4)) return;   // FORK v42: v38's "screenish" rule removed (the party list's Pokémon icons come from one large icon sheet: they were kept full size, off their rows; it did not fix the trainer fade either)   // FORK v38: || screenish (full-screen effects from a picture-sized image: 766fba38 / a63059a8 at 1920)
    vkCmdSetViewport(cb, 0, 1, &g_fk_v0); vkCmdSetScissor(cb, 0, 1, &g_fk_sc0);
    if (++g_fk_fills == 1 || g_fk_fills % 1000 == 0) VLOG("[FORK] fill draws kept full size: %llu", (unsigned long long)g_fk_fills);
}
// The census (for telling battles, menus and the world apart later): which UI shaders each frame draws into the picture and whether it has 3D under them.
// A frame's set is logged when it differs from the last logged one and has held for 15 frames ([FORK] ui census), at most 400 lines a run
static char g_fk_ck[12][17]; static int g_fk_cn;
static void fk_census_note(const char *k) { for (int i = 0; i < g_fk_cn; i++) if (!strncmp(g_fk_ck[i], k, 16)) return; if (g_fk_cn < 12) { memcpy(g_fk_ck[g_fk_cn], k, 16); g_fk_ck[g_fk_cn][16] = 0; g_fk_cn++; } }
// FORK: what kind of screen this is, from the census (for context-aware controls later; for now logged, to check): 0 world, 1 full-screen menu, 2 battle.
//   Battle: 3D with b6ff050a or ecacdb40 (the census of a battle; 56af7f00 alone is not one: dialogue boxes draw with it too, capture Nov 9); menu: no 3D, UI only.
//   Held 10 frames before it changes
_Atomic int dm_fork_screen;
volatile int dm_fork_census_want;   // touch_ui.c: the battle controls need the screen kind (else only Fill, corrected runs the census)
static void fk_screen_frame(int comp) {
    int bat = 0; for (int i = 0; i < g_fk_cn; i++) if (!strncmp(g_fk_ck[i], "b6ff050a", 8) || !strncmp(g_fk_ck[i], "ecacdb40", 8)) bat = 1;
    // FORK v47: 3 = dialogue: 3D with the text-box shader 56af7f00 and no battle HUD (an NPC talking in the field). A guess: logged, and touch_ui.c has an
    //   "all buttons" override for when it is wrong
    int dlg = 0; for (int i = 0; i < g_fk_cn; i++) if (!strncmp(g_fk_ck[i], "56af7f00", 8)) dlg = 1;
    int k = !comp && g_fk_cn ? 1 : comp && bat ? 2 : comp && dlg ? 3 : 0; static int cand = -1, held;
    if (k != cand) { cand = k; held = 0; return; }
    // (out of a battle only after 3 s without its HUD, to the world or dialogue: its text phases hide the HUD for a moment. To a full-screen menu (the Bag in a
    //   battle) at the usual 10 frames: v47 waited 3 s there too, and the menu's layout came late, Steven Nov 13)
    int need = atomic_load(&dm_fork_screen) == 2 && (k == 0 || k == 3) ? 90 : 10;
    if (++held == need && k != atomic_load(&dm_fork_screen)) { atomic_store(&dm_fork_screen, k); static int lines; if (lines++ < 400) VLOG("[FORK] screen: %s", k == 3 ? "dialogue" : k == 2 ? "battle" : k == 1 ? "menu" : "world"); }
}
static void fk_census_frame(int comp) {
    fk_screen_frame(comp);
    static char last[256], cand[256]; static int held, lines;
    char cur[256]; int n = snprintf(cur, sizeof cur, "3D %s; UI:", comp ? "yes" : "no");
    for (int i = 0; i < g_fk_cn; i++) for (int j = i + 1; j < g_fk_cn; j++) if (strcmp(g_fk_ck[j], g_fk_ck[i]) < 0) { char t[17]; memcpy(t, g_fk_ck[i], 17); memcpy(g_fk_ck[i], g_fk_ck[j], 17); memcpy(g_fk_ck[j], t, 17); }
    for (int i = 0; i < g_fk_cn && n < (int)sizeof cur - 20; i++) n += snprintf(cur + n, sizeof cur - n, " %.8s", g_fk_ck[i]);
    if (!g_fk_cn) snprintf(cur + n, sizeof cur - n, " none");
    g_fk_cn = 0;
    if (strcmp(cur, cand)) { snprintf(cand, sizeof cand, "%s", cur); held = 0; return; }
    if (++held == 15 && strcmp(cur, last) && lines < 400) { lines++; snprintf(last, sizeof last, "%s", cur); VLOG("[FORK] ui census: %s", cur); }
}
static int g_fk_uimode;   // this frame: a UI shader was drawn into the image, so the rest of its draws there are UI
static int fk_is(const char *k, const char *const *list) { for (; *list; list++) if (!strncmp(k, *list, 16)) return 1; return 0; }
static const char *const k_fk_scene[] = { "7fa0c6373aca1086", "e0feaaf96d273c2b", "57319aada7d3772b", NULL };   // composites (the image is learnt from them)
static const char *const k_fk_ui[] = { "af0ef3ac1bd35ad3", "56af7f00aa776d97", NULL };   // UI: the HUD, text boxes, menus
static int g_fk_inimg, g_fk_prev_comp, g_fk_capped, g_fk_odd;   // (g_fk_odd: this frame drew an unknown shader full size before any UI)
volatile int dm_fork_capture_arm;   // android_main.c nativeSetting DM_FORK_CAPTURE (the menu's "Capture the next odd frame"): one more automatic capture   // this frame: draws into the image; last frame had the composite; the automatic capture was taken
// present_window_locked: a new frame begins. DM_FORK_CAPTURE=1 (env.txt): the frame after the first one that went from 3D to UI only (a transition, a menu opening)
//   is captured (frame-capture.zip in Download, as the menu's Capture a frame), to see what the game draws there
static void fork_ui_frame(void) {
    static int want = -1; if (want < 0) { const char *e = getenv("DM_FORK_CAPTURE"); want = e && atoi(e) > 0; }
    if (dm_fork_capture_arm) { dm_fork_capture_arm = 0; want = 1; g_fk_capped = 0; VLOG("[FORK] capture: armed for the next odd frame"); }
    int sw = g_fk_prev_comp && !g_fk_comp && g_fk_inimg;   // from 3D to UI only (a transition, a menu opening)
    if (want && !g_fk_capped && (sw || g_fk_odd) && atomic_load(&g_cap) == 0) {   // (and an unknown shader drawn full size: perhaps a menu's opening animation)
        g_fk_capped = 1; atomic_store(&g_cap, 1); VLOG("[FORK] capture: the frame after %s", sw ? "a switch from 3D to UI only" : "an unknown shader drawn full size"); }
    if (dm_aspect() == DM_ASPECT_FIX || dm_fork_census_want) fk_census_frame(g_fk_comp);
    g_fk_prev_comp = g_fk_comp; g_fk_comp = 0; g_fk_clear = 0; g_fk_inimg = 0; g_fk_uimode = 0; g_fk_odd = 0;
}
static void fork_ui_draw(const char *vkey, const void *img, VkCommandBuffer cb, VkViewport *v, VkRect2D *sc, uint32_t tw, uint32_t th) {
    if (!vkey || !img) return;
    if (fk_is(vkey, k_fk_scene)) {   // a 3D composite: its image is the one the UI goes into. (Never shrunk, even after UI this frame: the Pokémon screen draws its
        //   menu first, copies the picture, draws the 3D Pokémon into the copy and composites that back; v18-v29 shrank that composite too, the menu twice)
        if (g_fk_img != img) { g_fk_img = img; VLOG("[FORK] UI image found (%ux%u, composite %.16s)", tw, th, vkey); }
        g_fk_comp = 1; return; }
    if (img != g_fk_img || !strncmp(vkey, "389df45bf4d9a0f6", 16)) return;
    g_fk_inimg++; fk_census_note(vkey);
    if (!g_fk_uimode) {
        if (!fk_is(vkey, k_fk_ui)) {   // before any UI this frame: a scene draw the list does not know, kept full size
            static int said; if (said < 16) { said++; VLOG("[FORK] scene draw kept full size (vertex %.16s)", vkey); }
            if (g_fk_comp) g_fk_odd = 1;   // (after a composite: something drawn over the 3D before the UI)
            g_fk_comp = 1; return; }
        g_fk_uimode = 1; }
    float kx, ky; dm_fork_ui_scale(&kx, &ky);
    if (kx == 1 && ky == 1) return;
    g_fk_sq = 1; g_fk_ntex = 0; g_fk_texmax = 0; g_fk_screenish = 0; g_fk_banner = 0; g_fk_tw = (int)tw; g_fk_th = (int)th; g_fk_ky = ky; g_fk_kx = kx;   // FORK v44: + banner (v46: the rest of this line had gone into the comment: no shader name, a stale viewport)
    g_fk_v0 = *v; g_fk_sc0 = *sc; snprintf(g_fk_sqkey, sizeof g_fk_sqkey, "%.16s", vkey);   // (fork_ui_fill may give it back)
    float cx = tw * 0.5f, cy = th * 0.5f;
    if (!g_fk_comp && !g_fk_clear) {   // no 3D under the UI this frame: black where the shrunk UI does not reach (else last frame's picture would stay there)
        g_fk_clear = 1;
        uint32_t bw = (uint32_t)(tw * (1 - kx) * 0.5f), bh = (uint32_t)(th * (1 - ky) * 0.5f);
        VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 0, 0, 0, 1 } } } };
        VkClearRect cr[2]; int n = 0;
        if (bh) { cr[n++] = (VkClearRect){ { { 0, 0 }, { tw, bh } }, 0, 1 }; cr[n++] = (VkClearRect){ { { 0, (int32_t)(th - bh) }, { tw, bh } }, 0, 1 }; }
        else if (bw) { cr[n++] = (VkClearRect){ { { 0, 0 }, { bw, th } }, 0, 1 }; cr[n++] = (VkClearRect){ { { (int32_t)(tw - bw), 0 }, { bw, th } }, 0, 1 }; }
        if (n) vkCmdClearAttachments(cb, 1, &ca, (uint32_t)n, cr);
    }
    v->x = cx + (v->x - cx) * kx; v->width *= kx; v->y = cy + (v->y - cy) * ky; v->height *= ky;
    float x0 = cx + (sc->offset.x - cx) * kx, y0 = cy + (sc->offset.y - cy) * ky;
    float x1 = cx + (sc->offset.x + (float)sc->extent.width - cx) * kx, y1 = cy + (sc->offset.y + (float)sc->extent.height - cy) * ky;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > tw) x1 = (float)tw; if (y1 > th) y1 = (float)th;
    int32_t ix0 = (int32_t)floorf(x0), iy0 = (int32_t)floorf(y0), ix1 = (int32_t)ceilf(x1), iy1 = (int32_t)ceilf(y1);
    sc->offset = (VkOffset2D){ ix0, iy0 }; sc->extent = (VkExtent2D){ (uint32_t)(ix1 > ix0 ? ix1 - ix0 : 0), (uint32_t)(iy1 > iy0 ? iy1 - iy0 : 0) };
    if (++g_fk_ui == 1 || g_fk_ui % 100000 == 0) VLOG("[FORK] UI draws shrunk: %llu (scale %.3f x %.3f)", (unsigned long long)g_fk_ui, kx, ky);
}

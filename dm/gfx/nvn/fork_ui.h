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
static int g_fk_sq, g_fk_texmax, g_fk_ntex; static VkViewport g_fk_v0; static VkRect2D g_fk_sc0; static uint64_t g_fk_fills;
static void fork_ui_tex(uint32_t w, uint32_t h) { if (!g_fk_sq) return; g_fk_ntex++; if ((int)w > g_fk_texmax) g_fk_texmax = (int)w; if ((int)h > g_fk_texmax) g_fk_texmax = (int)h; }
static void fork_ui_fill(VkCommandBuffer cb) {
    if (!g_fk_sq) return; g_fk_sq = 0;
    if (!g_fk_ntex || g_fk_texmax > 4) return;
    vkCmdSetViewport(cb, 0, 1, &g_fk_v0); vkCmdSetScissor(cb, 0, 1, &g_fk_sc0);
    if (++g_fk_fills == 1 || g_fk_fills % 1000 == 0) VLOG("[FORK] fill draws kept full size: %llu", (unsigned long long)g_fk_fills);
}
// The census (for telling battles, menus and the world apart later): which UI shaders each frame draws into the picture and whether it has 3D under them.
// A frame's set is logged when it differs from the last logged one and has held for 15 frames ([FORK] ui census), at most 400 lines a run
static char g_fk_ck[12][17]; static int g_fk_cn;
static void fk_census_note(const char *k) { for (int i = 0; i < g_fk_cn; i++) if (!strncmp(g_fk_ck[i], k, 16)) return; if (g_fk_cn < 12) { memcpy(g_fk_ck[g_fk_cn], k, 16); g_fk_ck[g_fk_cn][16] = 0; g_fk_cn++; } }
static void fk_census_frame(int comp) {
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
    if (dm_aspect() == DM_ASPECT_FIX) fk_census_frame(g_fk_comp);
    g_fk_prev_comp = g_fk_comp; g_fk_comp = 0; g_fk_clear = 0; g_fk_inimg = 0; g_fk_uimode = 0; g_fk_odd = 0;
}
static void fork_ui_draw(const char *vkey, const void *img, VkCommandBuffer cb, VkViewport *v, VkRect2D *sc, uint32_t tw, uint32_t th) {
    if (!vkey || !img) return;
    if (!g_fk_uimode && fk_is(vkey, k_fk_scene)) {   // a 3D composite: its image is the one the UI goes into
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
    g_fk_sq = 1; g_fk_ntex = 0; g_fk_texmax = 0; g_fk_v0 = *v; g_fk_sc0 = *sc;   // (fork_ui_fill may give it back)
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

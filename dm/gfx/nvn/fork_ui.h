// FORK: the UI half of Picture "Fill, corrected" (host/fork_aspect.c has the camera half). Included by nvn_vk.c; runs on the replay's thread.
// Pokémon Sword 1.3.2 ends a frame (frame captures, Adreno 840): the 3D scene is composited into a 1920x1080 image by one draw with vertex shader 7fa0c6373aca1086,
// the UI (HUD, text boxes, menus) is drawn into that same image after it, and one draw with vertex shader 389df45bf4d9a0f6 copies the image to the one presented.
// So: every draw into the composite's image that is not the composite itself is UI, and its viewport and scissor are shrunk around the image's centre by the scale
// fork_aspect.c gives (narrower screen: in y; wider: in x). After the picture is stretched over the screen the UI has its own shape again; the 3D behind it still
// fills the image. A frame with UI but no 3D composite (the bag, the X menu) gets the parts the shrunk UI no longer covers cleared to black first.
#pragma once
void dm_fork_ui_scale(float *kx, float *ky);

static const void *g_fk_img;   // the composite's image (its vtex), learnt from the composite draw
static int g_fk_comp, g_fk_clear;   // this frame: the composite was drawn / the bands were cleared
static uint64_t g_fk_ui;   // UI draws shrunk (logged now and then)

static int g_fk_inimg, g_fk_prev_comp, g_fk_capped;   // this frame: draws into the image; last frame had the composite; the automatic capture was taken
// present_window_locked: a new frame begins. DM_FORK_CAPTURE=1 (env.txt): the frame after the first one that went from 3D to UI only (a transition, a menu opening)
//   is captured (frame-capture.zip in Download, as the menu's Capture a frame), to see what the game draws there
static void fork_ui_frame(void) {
    static int want = -1; if (want < 0) { const char *e = getenv("DM_FORK_CAPTURE"); want = e && atoi(e) > 0; }
    if (want && !g_fk_capped && g_fk_prev_comp && !g_fk_comp && g_fk_inimg && atomic_load(&g_cap) == 0) {
        g_fk_capped = 1; atomic_store(&g_cap, 1); VLOG("[FORK] capture: the frame after a switch from 3D to UI only"); }
    g_fk_prev_comp = g_fk_comp; g_fk_comp = 0; g_fk_clear = 0; g_fk_inimg = 0;
}
static void fork_ui_draw(const char *vkey, const void *img, VkCommandBuffer cb, VkViewport *v, VkRect2D *sc, uint32_t tw, uint32_t th) {
    if (!vkey || !img) return;
    if (!strncmp(vkey, "7fa0c6373aca1086", 16)) {   // the 3D composite: its image is the one the UI goes into
        if (g_fk_img != img) { g_fk_img = img; VLOG("[FORK] UI image found (%ux%u)", tw, th); }
        g_fk_comp = 1; return; }
    if (img != g_fk_img || !strncmp(vkey, "389df45bf4d9a0f6", 16)) return;
    g_fk_inimg++;
    float kx, ky; dm_fork_ui_scale(&kx, &ky);
    if (kx == 1 && ky == 1) return;
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

// SPDX-License-Identifier: GPL-3.0-or-later
// Frame generation's quality score on the PC (Mesa's lavapipe is enough): a game recorded at a real 60 fps, every second frame dropped, each dropped frame made again
// from its two neighbours (the 30 fps the game would give) and compared with the truth. One number per game for tuning, and with --synthetic a regression test beside
// t_fgen_vk / t_lsfg_vk / t_fgcut_vk.
//   methods: ours (nvn_fgen.h), Lossless Scaling's (nvn_lsfg.h, 2x: the picture at 0.5) when a folder of the player's converted shaders is given, and the plain blend of
//   the two neighbours (what any method must beat, or it is not worth its cost). The cut check (nvn_fgcut.h) runs as the engine runs it (nvn_vk.c present_window_locked):
//   after a cut the newer real frame takes the picture's place, fgc_select for Lossless Scaling, fgc_select_rt for ours (its picture is a render target; without a
//   cut flag fg_gen.frag still shows the frame before again where fg_stat.frag finds two different pictures). Like the engine, Lossless Scaling makes its first picture once it has three
//   frames of history: the first dropped frame of a sequence is left out of every method's score so all three are scored on the same frames.
//   scores: PSNR over RGB (dB; 99 for a picture equal to the truth) and SSIM over luma (BT.601) in 8x8 windows, stride 4; per frame, then per method the mean, the
//   5th percentile (the bad moments a player notices) and the worst.
//
// Build and run (WSL, lavapipe):
//   make t_fg_score GAME=<id> VULKAN_INC=<a folder holding vulkan/vulkan.h>      (or: gcc -O2 -I<that folder> -o t_fg_score t_fg_score.c -l:libvulkan.so.1 -lm)
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./t_fg_score [--synthetic] [--lsfg <folder of .spv>]      (no folder of frames: --synthetic)
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./t_fg_score <folder of recorded frames> [--lsfg <folder of .spv>] [--csv out.csv] [--out <folder for pictures>]
//   (bash galar-re/run-fg-tests.sh <worktree> t_fg_score runs the synthetic test with the other frame generation tests: ours only, unless DM_LSFG_DIR names the shaders)
// Options: --lsfg DIR (Lossless Scaling's converted shaders, else $DM_LSFG_DIR as the engine's; never copied into the repository), --flow F (its motion search scale, default 0.5 as the engine's DM_LSFG_FLOW),
//   --margin N (pixels left out at each edge, default 0; --synthetic 16), --size WxH (--synthetic, default 640x360), --srgb (the frames are the game's sRGB format: both
//   generators then sample and blend in linear light, as on the phone; a recording's info.txt says so itself), --first N / --count N (a part of a recording),
//   --csv FILE (one line per dropped frame), --out DIR (each method's picture and the truth as PPM).
//
// Recording on the phone (nvn_vk.c, DM_FG_DUMP; off by default, nothing allocated or done while off):
//   1. a line in the game's env.txt (the app's files folder, beside log.txt: /sdcard/Android/data/<package>/files/[<program id>/]env.txt):
//        DM_FG_DUMP=<start present>:<count>[:<scale %>]     e.g. DM_FG_DUMP=3600:120:50 (two seconds at 60 fps, from the 3600th present on, at half the size)
//      start 0: wait for a file fgdump.req in that folder instead (adb shell touch .../fgdump.req when the scene is the one wanted; looked for every 30 presents).
//      Each recorded frame is the game's frame as frame generation gets it (its crop), read back into memory as it is presented (a copy, or a blit when scaled);
//      nothing is written until the last one, so the recording does not slow the frames it records (the game stalls once after it, while the GPU is waited for).
//      Frames are recorded only while consecutive: a missed present, or a new size or format, ends the recording there. Leave frame generation off (it records the
//      real frames anyway, but costs the game the frame rate the recording needs).
//   2. then <DM_CAPTURE_DIR, else the files folder>/fgdump/0000.ppm ... and info.txt (size, the frame's Vulkan format, scale); the log says "fg dump: N frames written".
//   3. adb pull /sdcard/Android/data/<package>/files/[<program id>/]fgdump <PC folder>, then run this program on that folder (in WSL: /mnt/c/...).
//   For Legends: Arceus (a real 60) the same switch is needed on its own branch's nvn_vk.c; Sword / Shield at 30 give a 30 fps recording: its "dropped" frames are
//   33 ms apart, a harder test than the 30 -> 60 the player sees.
//
// --synthetic: five sequences made here at 640x360, 60 fps (11 frames each, 4 dropped frames scored): a camera pan (5,-3 px a frame), a fast pan (18,6), a rotation with a
//   zoom about the centre, a textured block crossing a still background, and a camera cut (a panning scene, then another scene from frame 7). It fails when
//   - on a moving case, ours or Lossless Scaling's mean PSNR or mean SSIM is below the plain blend's;
//   - on a moving case, the cut check finds a cut (a pan taken for a cut would show the newer frame twice);
//   - on the cut, the check does not find it, or ours or Lossless Scaling's picture is not exactly the newer frame.
//   A sixth sequence, a cut between two scenes of the same look, is printed and not checked (the cut check misses it: under 60 % of its cells change).
//   Exit status 0 passed, 1 failed (recordings: always 0 unless something could not run).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <vulkan/vulkan.h>
#include "../nvn/nvn_fgen.h"
#include "../nvn/nvn_lsfg.h"
#include "../nvn/nvn_fgcut.h"
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool pool; static VkPhysicalDeviceMemoryProperties mp;
static fgen G; static lsfg L; static fgc C; static int have_ls;
static int W, H, MARGIN = -1; static VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM; static float FLOW = 0.5f;
static VkBuffer bin, bours, bls; static uint8_t *min_, *mours, *mls;   // the frame going in, the two pictures coming out
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------- buffers, command buffers ----------
static VkBuffer mkbuf(VkDeviceSize n, uint8_t **map) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT }; VkBuffer b;
    if (vkCreateBuffer(dev, &bci, NULL, &b)) { printf("FAIL: no buffer\n"); exit(1); }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, b, &rq); uint32_t t = 0; const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (t < mp.memoryTypeCount && !((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & hv) == hv)) t++;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; VkDeviceMemory m; void *v;
    if (t == mp.memoryTypeCount || vkAllocateMemory(dev, &mai, NULL, &m) || vkBindBufferMemory(dev, b, m, 0) || vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &v)) { printf("FAIL: no buffer memory\n"); exit(1); }
    *map = v; return b;
}
static VkCommandBuffer cb_begin(void) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &ai, &cb); VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void cb_end(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb); VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VkResult r = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE); if (!r) r = vkQueueWaitIdle(q);
    if (r) { printf("FAIL: the queue returned %d\n", r); exit(1); }
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

// ---------- frames: a recording's PPM files, or made here ----------
typedef void (*maker)(int i, uint8_t *rgba);
static maker MK; static char **files; static int nfiles;
static int ppm_read(const char *path, uint8_t *rgba, int *w, int *h) {   // P6, maxval 255; rgba NULL: the size alone
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    char mg[3] = ""; int ww = 0, hh = 0, mx = 0, ok = fscanf(f, "%2s", mg) == 1 && !strcmp(mg, "P6");
    for (int k = 0; ok && k < 3; k++) { int c; while ((c = fgetc(f)) == '#' || c == ' ' || c == '\n' || c == '\r' || c == '\t') if (c == '#') while ((c = fgetc(f)) != '\n' && c != EOF) {}
        ungetc(c, f); ok = fscanf(f, "%d", k == 0 ? &ww : k == 1 ? &hh : &mx) == 1; }
    if (!ok || mx != 255 || ww <= 0 || hh <= 0) { fclose(f); return 0; }
    fgetc(f); if (w) *w = ww; if (h) *h = hh;
    if (rgba) { if (ww != W || hh != H) { fclose(f); return 0; }
        uint8_t *row = malloc((size_t)ww * 3);
        for (int y = 0; y < hh && ok; y++) { ok = fread(row, 1, (size_t)ww * 3, f) == (size_t)ww * 3;
            for (int x = 0; x < ww; x++) { uint8_t *o = rgba + ((size_t)y * ww + x) * 4; o[0] = row[x * 3]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3 + 2]; o[3] = 255; } }
        free(row); }
    fclose(f); return ok;
}
static void ppm_write(const char *dir, const char *name, const uint8_t *m) { if (!dir) return; char p[1024]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name); FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6 %d %d 255\n", W, H); for (int i = 0; i < W * H; i++) fwrite(m + (size_t)i * 4, 1, 3, f); fclose(f); }
static int frame_at(int i, uint8_t *rgba) { if (MK) { MK(i, rgba); return 1; } return ppm_read(files[i], rgba, NULL, NULL); }
static float hash(int x, int y, int s) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u; h = (h ^ (h >> 13)) * 1274126177u; return ((h ^ (h >> 16)) & 0xffff) / 65535.0f; }
static float vnoise(float x, float y, int s) { int xi = (int)floorf(x), yi = (int)floorf(y); float fx = x - xi, fy = y - yi; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    return (hash(xi, yi, s) * (1 - fx) + hash(xi + 1, yi, s) * fx) * (1 - fy) + (hash(xi, yi + 1, s) * (1 - fx) + hash(xi + 1, yi + 1, s) * fx) * fy; }
static void scene(float x, float y, int seed, float *rgb) {   // detail at several sizes and hard edges, defined at any real position so a moved frame is exact (t_lsfg_vk's)
    for (int c = 0; c < 3; c++) { float v = 0.5f * vnoise(x / 97, y / 97, seed + c) + 0.3f * vnoise(x / 23, y / 23, seed + 7 + c) + 0.2f * vnoise(x / 6, y / 6, seed + 13 + c);
        if (vnoise(x / 41, y / 41, seed + 31) > 0.62f) v = 1 - v;
        rgb[c] = v; }
}
static void put(uint8_t *o, const float *c) { for (int k = 0; k < 3; k++) o[k] = (uint8_t)(fminf(fmaxf(c[k], 0), 1) * 255 + 0.5f); o[3] = 255; }
static void s_pan(int i, uint8_t *m) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3]; scene(x + 5.0f * i, y - 3.0f * i, 1, c); put(m + ((size_t)y * W + x) * 4, c); } }
static void s_fast(int i, uint8_t *m) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3]; scene(x + 18.0f * i, y + 6.0f * i, 2, c); put(m + ((size_t)y * W + x) * 4, c); } }
static void s_rot(int i, uint8_t *m) {   // the camera rolls 0.25 degrees and closes in 0.8 % a frame, about the centre
    float a = 0.0044f * i, s = 1.0f + 0.008f * i, ca = cosf(a), sa = sinf(a), cx = W * 0.5f, cy = H * 0.5f;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float dx = x - cx, dy = y - cy, c[3]; scene(cx + (dx * ca - dy * sa) / s, cy + (dx * sa + dy * ca) / s, 5, c); put(m + ((size_t)y * W + x) * 4, c); }
}
static void s_obj(int i, uint8_t *m) {   // a still background, a textured block crossing it at 8,2 px a frame
    float ox = 100 + 8.0f * i, oy = 130 + 2.0f * i;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3];
        if (x >= ox && x < ox + 120 && y >= oy && y < oy + 90) { scene((x - ox) * 1.7f, (y - oy) * 1.7f, 90, c); c[0] = 0.3f + 0.7f * c[0]; }
        else scene((float)x, (float)y, 4, c);
        put(m + ((size_t)y * W + x) * 4, c); }
}
#define CUT_AT 7
static void s_cut(int i, uint8_t *m) {   // a slow pan, then from frame CUT_AT another scene, darker and bluer (a battle's cut to another camera: the dropped frame CUT_AT is
    //   the new one's, the pair 6-8 goes across the cut)
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3];
        if (i < CUT_AT) scene(x + 4.0f * i, (float)y, 1, c); else { scene(x - 3.0f * i, y + 2.0f * i, 50, c); c[0] *= 0.45f; c[1] = 0.1f + 0.5f * c[1]; c[2] = 0.35f + 0.6f * c[2]; }
        put(m + ((size_t)y * W + x) * 4, c); }
}
static void s_cut_alike(int i, uint8_t *m) {   // a cut between two scenes of the same look (the same textures' statistics, as two angles on one field): reported, not checked
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float c[3]; if (i < CUT_AT) scene(x + 4.0f * i, (float)y, 1, c); else scene(x - 3.0f * i, y + 2.0f * i, 50, c); put(m + ((size_t)y * W + x) * 4, c); }
}

// ---------- scores ----------
static double psnr(const uint8_t *a, const uint8_t *b) {
    double s = 0; long n = 0; int m = MARGIN;
    for (int y = m; y < H - m; y++) for (int x = m; x < W - m; x++) for (int k = 0; k < 3; k++) { double d = (double)a[((size_t)y * W + x) * 4 + k] - b[((size_t)y * W + x) * 4 + k]; s += d * d; n++; }
    if (!n) return 0;
    s /= (double)n; return s <= 0 ? 99.0 : fmin(99.0, 10 * log10(255.0 * 255.0 / s));
}
static float *la, *lb;
static void luma(const uint8_t *m, float *l) { for (int i = 0; i < W * H; i++) l[i] = 0.299f * m[i * 4] + 0.587f * m[i * 4 + 1] + 0.114f * m[i * 4 + 2]; }
static double ssim(const uint8_t *a, const uint8_t *b) {   // 8x8 windows every 4 pixels, the usual constants for 8-bit values
    luma(a, la); luma(b, lb);
    const double C1 = (0.01 * 255) * (0.01 * 255), C2 = (0.03 * 255) * (0.03 * 255); double s = 0; long n = 0; int m = MARGIN;
    for (int y0 = m; y0 + 8 <= H - m; y0 += 4) for (int x0 = m; x0 + 8 <= W - m; x0 += 4) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        for (int y = y0; y < y0 + 8; y++) for (int x = x0; x < x0 + 8; x++) { double u = la[y * W + x], v = lb[y * W + x]; sa += u; sb += v; saa += u * u; sbb += v * v; sab += u * v; }
        double ma = sa / 64, mb = sb / 64, va = saa / 64 - ma * ma, vb = sbb / 64 - mb * mb, cab = sab / 64 - ma * mb;
        s += ((2 * ma * mb + C1) * (2 * cab + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2)); n++; }
    return n ? s / (double)n : 0;
}
static int same_rgb(const uint8_t *a, const uint8_t *b) { for (int i = 0; i < W * H; i++) if (memcmp(a + (size_t)i * 4, b + (size_t)i * 4, 3)) return 0; return 1; }
enum { M_OURS, M_LS, M_BLEND, NM };
static const char *MNAME[NM] = { "ours", "lsfg", "blend" };
typedef struct { double *p, *s; int n, cap; } series;
static void add(series *z, double p, double s) { if (z->n == z->cap) { z->cap = z->cap ? z->cap * 2 : 64; z->p = realloc(z->p, z->cap * sizeof *z->p); z->s = realloc(z->s, z->cap * sizeof *z->s); } z->p[z->n] = p; z->s[z->n] = s; z->n++; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void stats(const double *v, int n, double *mean, double *p5, double *worst) {
    double *t = malloc((size_t)(n ? n : 1) * sizeof *t), s = 0; memcpy(t, v, (size_t)n * sizeof *t); qsort(t, (size_t)n, sizeof *t, cmpd);
    for (int i = 0; i < n; i++) s += t[i];
    *mean = n ? s / n : 0; *p5 = n ? t[(int)(0.05 * (n - 1))] : 0; *worst = n ? t[0] : 0; free(t);
}
static void summary(const char *what, series *z) {
    printf("%s:\n", what);
    for (int k = 0; k < NM; k++) { if (k == M_LS && !have_ls) continue; double pm, p5, pw, sm, s5, sw; stats(z[k].p, z[k].n, &pm, &p5, &pw); stats(z[k].s, z[k].n, &sm, &s5, &sw);
        printf("  %-5s PSNR mean %6.2f  5%% %6.2f  worst %6.2f dB    SSIM mean %.4f  5%% %.4f  worst %.4f   (%d frames)\n", MNAME[k], pm, p5, pw, sm, s5, sw, z[k].n); }
}

// ---------- one sequence through both generators, as nvn_vk.c drives them ----------
typedef struct { int cut_frame, ls_exact, ours_exact, ncuts; } seq_res;   // cut_frame: the dropped frame of the pair the check called a cut (the last one)
static int KNOWN_CUT = -1;   // (synthetic: the dropped frame whose pair goes across the cut; ours_exact / ls_exact are taken there whatever the check said)
static FILE *csv; static const char *outdir;
static void run_seq(const char *name, int first, int n, series *z, seq_res *res) {
    static uint8_t *prev, *drop, *cur, *blend; size_t fs = (size_t)W * H * 4;
    if (!prev) { prev = malloc(fs); drop = malloc(fs); cur = malloc(fs); blend = malloc(fs); }
    memset(res, 0, sizeof *res); res->cut_frame = -1;
    if (have_ls) L.fresh = 1;   // (ls_begin clears its history: a new sequence starts from nothing, as after a pause in the engine)
    const VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)W, (uint32_t)H, 1 } };
    int p = 0;
    for (int r = 0; first + 2 * r < first + n; r++) {   // real frame r is recorded frame first + 2r; the frame dropped before it, first + 2r - 1
        int fi = first + 2 * r, di = fi - 1;
        if (!frame_at(fi, cur)) { printf("FAIL: cannot read frame %d (%s)\n", fi, files ? files[fi] : "?"); exit(1); }
        memcpy(min_, cur, fs);
        int have_prev = r > 0, ls_gen = have_ls && L.n >= 2;   // (Lossless Scaling: three frames of history first, as the engine asks)
        VkCommandBuffer cb = cb_begin();
        fg_begin(&G, cb);
        vkCmdCopyBufferToImage(cb, bin, G.c[p].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        fg_ingest(&G, cb, p);
        if (have_prev) { fgc_stat(&C, cb, 0, G.c[p ^ 1].view, G.c[p].view); fg_generate(&G, cb, p);
            fgc_select_rt(&C, cb, 0, G.c[p].view, G.m.view, G.fmt, G.w, G.h);   // (a cut: the newer frame instead, as nvn_vk.c does)
            vkCmdCopyImageToBuffer(cb, G.m.img, VK_IMAGE_LAYOUT_GENERAL, bours, 1, &bc); }
        if (have_ls) {
            ls_begin(&L, cb); vkCmdCopyBufferToImage(cb, bin, L.c[p].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc); ls_ingest(&L, cb, p);
            if (have_prev) fgc_stat(&C, cb, 1, L.c[p ^ 1].view, L.c[p].view);
            if (have_prev && ls_gen) { ls_generate(&L, cb, 1, 0); fgc_select(&C, cb, 1, 0, L.c[p].view, L.m[0].view, L.w, L.h);
                vkCmdCopyImageToBuffer(cb, L.m[0].img, VK_IMAGE_LAYOUT_GENERAL, bls, 1, &bc); } }
        cb_end(cb);
        fgc_result cr = { 0 }, cr1; float mean = 0; int cut = have_prev && fgc_read(&C, 0, &cr, &mean) && cr.flag; if (have_ls && have_prev) fgc_read(&C, 1, &cr1, NULL);
        if (have_prev && r >= 2 && (!have_ls || ls_gen)) {   // scored: every dropped frame but the first of the sequence (all methods the same frames)
            if (!frame_at(di, drop)) { printf("FAIL: cannot read frame %d\n", di); exit(1); }
            for (size_t i = 0; i < fs; i++) blend[i] = (uint8_t)((prev[i] + cur[i] + 1) / 2);
            const uint8_t *pic[NM] = { mours, have_ls ? mls : NULL, blend }; double ps[NM] = { 0 }, ss[NM] = { 0 };
            for (int k = 0; k < NM; k++) if (pic[k]) { ps[k] = psnr(pic[k], drop); ss[k] = ssim(pic[k], drop); add(&z[k], ps[k], ss[k]); }
            printf("  %-9s frame %4d (between %d and %d)%s  ours %6.2f dB %.4f", name, di, di - 1, fi, cut ? " CUT" : "    ", ps[M_OURS], ss[M_OURS]);
            if (have_ls) printf(" | lsfg %6.2f dB %.4f", ps[M_LS], ss[M_LS]);
            printf(" | blend %6.2f dB %.4f   (cells changed %u of %u, mean change %.4f)\n", ps[M_BLEND], ss[M_BLEND], cr.changed, cr.cells, mean);
            if (csv) { fprintf(csv, "%s,%d,%d,%.3f,%.5f,", name, di, cut, ps[M_OURS], ss[M_OURS]);
                       if (have_ls) fprintf(csv, "%.3f,%.5f", ps[M_LS], ss[M_LS]); else fputc(',', csv);
                       fprintf(csv, ",%.3f,%.5f\n", ps[M_BLEND], ss[M_BLEND]); }
            if (outdir) { char nm[96]; snprintf(nm, sizeof nm, "%s_%04d_ours", name, di); ppm_write(outdir, nm, mours); if (have_ls) { snprintf(nm, sizeof nm, "%s_%04d_lsfg", name, di); ppm_write(outdir, nm, mls); }
                snprintf(nm, sizeof nm, "%s_%04d_truth", name, di); ppm_write(outdir, nm, drop); }
            if (cut) res->cut_frame = di;
            if (di == KNOWN_CUT) { res->ls_exact = have_ls && same_rgb(mls, cur); res->ours_exact = same_rgb(mours, prev) ? 1 : same_rgb(mours, cur) ? 2 : 0; }
        }
        if (cut) res->ncuts++;   // (every pair, scored or not)
        memcpy(prev, cur, fs); p ^= 1;
    }
}

// ---------- Vulkan ----------
static void vk_init(const char *lsdir) {
    if (lsdir && !ls_load(&L, lsdir)) { printf("note: Lossless Scaling's shaders not loaded (%s): ours and the blend only\n", L.why ? L.why : "?"); lsdir = NULL; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = lsdir ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("SKIP: no Vulkan instance on this machine\n"); exit(0); }
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds); if (!n) { printf("SKIP: no Vulkan device on this machine\n"); exit(0); }
    pd = pds[0]; VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    if (lsdir && (pp.apiVersion < VK_API_VERSION_1_3 || ls_device_lacks(pd, 0))) { printf("note: %s cannot run Lossless Scaling's chain (%s)\n", pp.deviceName, pp.apiVersion < VK_API_VERSION_1_3 ? "no Vulkan 1.3" : ls_device_lacks(pd, 0)); lsdir = NULL; }
    uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp); uint32_t qf = 0;
    while (qf < nq && (qp[qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) qf++;
    float prio = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    // every feature the device has, on (as nvn_vk.c creates its device): Lossless Scaling's chain needs 1.1-1.3 features, the cut swap storage writes without a format
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = lsdir ? &f13 : NULL };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = lsdir ? (void *)&f11 : NULL }; vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("SKIP: no Vulkan device could be opened\n"); exit(0); }
    vkGetDeviceQueue(dev, qf, 0, &q); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf }; vkCreateCommandPool(dev, &pci, NULL, &pool);
    if (!fg_setup(&G, dev, pd, FMT, (uint32_t)W, (uint32_t)H)) { printf("FAIL: our frame generation cannot be set up on %s: %s\n", pp.deviceName, G.why ? G.why : "?"); exit(1); }
    if (!fgc_init(&C, dev, pd)) { printf("FAIL: the cut check cannot be set up: %s\n", C.why ? C.why : "?"); exit(1); }
    if (!C.can_select) printf("note: no storage writes without a format on %s: cuts are counted, Lossless Scaling's picture is not replaced (as on such a phone)\n", pp.deviceName);
    have_ls = lsdir && ls_setup(&L, dev, pd, FMT, (uint32_t)W, (uint32_t)H, FLOW);
    if (lsdir && !have_ls) printf("note: Lossless Scaling's chain cannot be set up: %s\n", L.why ? L.why : "?");
    size_t fs = (size_t)W * H * 4; bin = mkbuf(fs, &min_); bours = mkbuf(fs, &mours); bls = mkbuf(fs, &mls);
    la = malloc((size_t)W * H * sizeof *la); lb = malloc((size_t)W * H * sizeof *lb);
    printf("on %s: %dx%d frames, format %s; ours: motion from %ux%u down; Lossless Scaling: %s", pp.deviceName, W, H, FMT == VK_FORMAT_R8G8B8A8_SRGB ? "sRGB" : "UNORM", G.lw[0], G.lh[0],
           have_ls ? "" : "not run\n");
    if (have_ls) printf("2x, motion at %.0f%% (%ux%u)\n", FLOW * 100, L.fw, L.fh);
}
static int cmps(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int main(int argc, char **argv) {
    const char *dir = NULL, *lsdir = NULL, *csvp = NULL; int synth = 0, first = 0, count = 0, srgb = 0; W = 640; H = 360;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--synthetic")) synth = 1;
        else if (!strcmp(a, "--srgb")) srgb = 1;
        else if (!strcmp(a, "--lsfg") && v) { lsdir = v; i++; }
        else if (!strcmp(a, "--flow") && v) { FLOW = (float)atof(v); i++; }
        else if (!strcmp(a, "--margin") && v) { MARGIN = atoi(v); i++; }
        else if (!strcmp(a, "--size") && v) { if (sscanf(v, "%dx%d", &W, &H) != 2 || W < 64 || H < 64) { printf("bad --size\n"); return 2; } i++; }
        else if (!strcmp(a, "--first") && v) { first = atoi(v); i++; }
        else if (!strcmp(a, "--count") && v) { count = atoi(v); i++; }
        else if (!strcmp(a, "--csv") && v) { csvp = v; i++; }
        else if (!strcmp(a, "--out") && v) { outdir = v; i++; }
        else if (a[0] != '-' && !dir) dir = a;
        else { printf("usage: t_fg_score (--synthetic | <folder of recorded frames>) [--lsfg <folder of .spv>] [--flow F] [--margin N] [--size WxH] [--srgb] [--first N] [--count N] [--csv FILE] [--out DIR]\n"); return 2; }
    }
    if (!dir) synth = 1;   // (no recording given: the regression test)
    if (!lsdir && getenv("DM_LSFG_DIR") && *getenv("DM_LSFG_DIR")) lsdir = getenv("DM_LSFG_DIR");
    if (!synth) {   // the recording: its PPM files in name order, its info.txt (the recorder's: the game's frame format)
        DIR *d = opendir(dir); struct dirent *e; if (!d) { printf("FAIL: cannot open %s\n", dir); return 1; }
        while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".ppm")) { files = realloc(files, (size_t)(nfiles + 1) * sizeof *files);
            files[nfiles] = malloc(strlen(dir) + l + 2); sprintf(files[nfiles++], "%s/%s", dir, e->d_name); } }
        closedir(d); qsort(files, (size_t)nfiles, sizeof *files, cmps);
        if (nfiles < 5) { printf("FAIL: %s holds %d .ppm frames: at least 5 are needed\n", dir, nfiles); return 1; }
        if (!ppm_read(files[0], NULL, &W, &H)) { printf("FAIL: %s is not a binary PPM (P6, 8 bits)\n", files[0]); return 1; }
        char ip[1100]; snprintf(ip, sizeof ip, "%s/info.txt", dir); FILE *f = fopen(ip, "r");
        if (f) { char line[256]; while (fgets(line, sizeof line, f)) { int fm; if (sscanf(line, "format %d", &fm) == 1 && (fm == VK_FORMAT_R8G8B8A8_SRGB || fm == VK_FORMAT_B8G8R8A8_SRGB)) srgb = 1; } fclose(f); }
        if (first < 0 || first >= nfiles) first = 0;
        if (!count || first + count > nfiles) count = nfiles - first; }
    if (MARGIN < 0) MARGIN = synth ? 16 : 0;
    if (srgb) FMT = VK_FORMAT_R8G8B8A8_SRGB;
    vk_init(lsdir);
    if (csvp) { csv = fopen(csvp, "w"); if (csv) fprintf(csv, "case,frame,cut,ours_psnr,ours_ssim,lsfg_psnr,lsfg_ssim,blend_psnr,blend_ssim\n"); }
    series all[NM]; memset(all, 0, sizeof all);
    if (!synth) {
        printf("%d frames from %s (frames %d..%d; every second one dropped and made again)\n", count, dir, first, first + count - 1);
        seq_res r; run_seq("recording", first, count, all, &r);
        summary("all dropped frames", all);
        printf("cuts found: %d\n", r.ncuts);
        if (csv) fclose(csv);
        return 0; }
    static const struct { const char *name; maker f; int moving; } CASES[] = { { "pan", s_pan, 1 }, { "fast-pan", s_fast, 1 }, { "rot-zoom", s_rot, 1 }, { "object", s_obj, 1 }, { "cut", s_cut, 0 }, { "cut-alike", s_cut_alike, 2 } };
    const int N = 11;
    for (unsigned c = 0; c < sizeof CASES / sizeof *CASES; c++) {
        series z[NM]; memset(z, 0, sizeof z); seq_res r; MK = CASES[c].f;
        KNOWN_CUT = CASES[c].moving == 1 ? -1 : CUT_AT; run_seq(CASES[c].name, 0, N, z, &r);
        double m[NM][2];
        for (int k = 0; k < NM; k++) { double p5, w; stats(z[k].p, z[k].n, &m[k][0], &p5, &w); stats(z[k].s, z[k].n, &m[k][1], &p5, &w);
            if (CASES[c].moving != 2) for (int i = 0; i < z[k].n; i++) add(&all[k], z[k].p[i], z[k].s[i]); }
        printf("%-9s mean: ours %6.2f dB %.4f", CASES[c].name, m[M_OURS][0], m[M_OURS][1]); if (have_ls) printf(" | lsfg %6.2f dB %.4f", m[M_LS][0], m[M_LS][1]);
        printf(" | blend %6.2f dB %.4f\n", m[M_BLEND][0], m[M_BLEND][1]);
        if (CASES[c].moving == 1) {
            CHECK(m[M_OURS][0] >= m[M_BLEND][0] && m[M_OURS][1] >= m[M_BLEND][1], "%s: ours (%.2f dB, %.4f) below the plain blend (%.2f dB, %.4f)", CASES[c].name, m[M_OURS][0], m[M_OURS][1], m[M_BLEND][0], m[M_BLEND][1]);
            if (have_ls) CHECK(m[M_LS][0] >= m[M_BLEND][0] && m[M_LS][1] >= m[M_BLEND][1], "%s: Lossless Scaling (%.2f dB, %.4f) below the plain blend (%.2f dB, %.4f)", CASES[c].name, m[M_LS][0], m[M_LS][1], m[M_BLEND][0], m[M_BLEND][1]);
            CHECK(r.ncuts == 0, "%s: the cut check found %d cut(s) in a moving scene", CASES[c].name, r.ncuts);
        } else if (CASES[c].moving == 2) {
            printf("cut-alike (not checked): the cut check %s; ours shows %s; Lossless Scaling's picture %s\n", r.ncuts ? "found it" : "did NOT find it",
                   r.ours_exact == 1 ? "the frame before exactly" : r.ours_exact == 2 ? "the newer frame exactly" : "a mix (neither real frame)",
                   !have_ls ? "not run" : r.ls_exact ? "is the newer frame exactly" : "is a mix (not replaced)");
        } else {
            CHECK(r.cut_frame == CUT_AT && r.ncuts == 1, "cut: the check found the cut at %d (%d cuts), expected only the pair around frame %d", r.cut_frame, r.ncuts, CUT_AT);
            {
                printf("cut: ours shows %s; Lossless Scaling's picture %s\n", r.ours_exact == 1 ? "the frame before exactly" : r.ours_exact == 2 ? "the newer frame exactly (fgc_select_rt)" : "neither real frame exactly",
                       !have_ls ? "not run" : r.ls_exact ? "is the newer frame exactly (fgc_select)" : "is NOT the newer frame");
                CHECK(r.ours_exact == 2, "cut: ours is not exactly the newer frame");
                if (have_ls && C.can_select) CHECK(r.ls_exact, "cut: Lossless Scaling's picture is not exactly the newer frame"); } }
    }
    summary("all synthetic cases", all);
    if (csv) fclose(csv);
    printf(fails ? "t_fg_score: %d FAILED\n" : "t_fg_score: all passed\n", fails);
    return fails != 0;
}

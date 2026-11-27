// Frame recorder (renderer phase 1: it records, it changes nothing). A few whole frames as the replay builds them: every render pass with its size, its attachments
// (which image, format, mip level and layer, cleared or loaded, stored or not) and its draw count, every read of an image between or inside passes (sampled, copied,
// written in part, loaded by a later pass, shown) and the compute dispatches, with each pass's GPU time where the GPU gives timestamps. From that, per frame:
// - each pass's stores: read later in the frame, overwritten unread (a store a tiler could leave in tile memory), or kept into the next frame (read there or not);
// - passes back to back at the same size (candidates to merge into one pass with subpasses);
// - the last pass that uses depth (the 3D scene), and what is drawn after it into the image that is shown (post effects and the HUD: where frame generation
//   would split the scene from the HUD).
// Asked for with framegraph.req in the game's folder (beside the pipeline cache, as capture.req; it holds how many frames to record, 1-4, default 3) or
//   DM_FG_RECORD=<present number>:<frames>. Written as framegraph.txt there and in Download/. Nothing is recorded or kept while not asked for.
// All calls come from the thread that builds the GPU's work (the replay); g_fgr.on is read without the lock, everything else is under it
#ifndef FG_REC_H
#define FG_REC_H
#include <stdio.h>

enum { FGR_FRAMES = 4, FGR_MAXP = 1024, FGR_MAXR = 16384, FGR_MAXI = 4096 };
typedef struct { int img; uint32_t fmt; uint16_t lv, ly; uint8_t slot, clear, store; } fgr_att;   // slot 0-7 colour, 8 depth, 9 stencil; store 0 store, 1 don't care, 2 none
typedef struct { const void *x; uint32_t w, h, fmt; uint16_t lv, ly; uint8_t slot, clear, store; } fgr_attin;   // (what pass_begin hands over: the image itself)
typedef struct { uint32_t w, h, layers, samples, draws, natt, frame; uint32_t after_disp; fgr_att a[10]; uint64_t t0, t1; } fgr_pass;   // t0 / t1: timestamps (0 none)
typedef struct { uint32_t pass, in_pass, frame; int img; uint8_t kind; } fgr_read;   // pass: passes begun before it; in_pass: during that pass; kind fgr_kind
enum { FGR_READ = 0, FGR_SHOWN = 1 };
typedef struct { const void *x; uint32_t w, h, fmt, mips, layers; } fgr_img;
static struct {
    _Atomic int on;                 // recording now
    int want, done, armed_at;       // frames asked for, frames finished, the present it starts at (DM_FG_RECORD)
    int cont, overflow;             // cont: recording all the time for the held frame's store decisions (nvn_vk.c hold_dec); overflow: a list was full since the last reset
    uint64_t frame0;                // the first recorded frame's number
    fgr_pass p[FGR_MAXP]; int np, open;
    fgr_read r[FGR_MAXR]; int nr;
    fgr_img im[FGR_MAXI]; int ni;
    uint32_t disp;                  // compute dispatches since the last pass began
    uint32_t pf[FGR_FRAMES + 1];    // the first pass of each recorded frame (pf[n]: the end)
    VkQueryPool qp; double tsper; int qn;
    int pending_write;              // the frames are done: written once the GPU has finished them
} g_fgr;
static pthread_mutex_t g_fgrmu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fgr_series[4]; static int g_fgr_nser; static int g_fgr_named;   // a series armed by an event (a battle's transition): the presents each recording starts at; named: framegraph-<frame>.txt

static int fgr_img_id(const void *x, uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips, uint32_t layers) {
    for (int i = 0; i < g_fgr.ni; i++) if (g_fgr.im[i].x == x) return i;
    if (g_fgr.ni >= FGR_MAXI) { g_fgr.overflow = 1; return -1; }
    g_fgr.im[g_fgr.ni] = (fgr_img){ x, w, h, fmt, mips, layers }; return g_fgr.ni++;
}
// a pass begins (before its vkCmdBeginRendering, nothing open: the timestamp and, at the first pass, the reset go into cb)
// returns the pass's index in the recording (-1: not recorded)
static int fgr_pass_begin(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples, const fgr_attin *a, int na) {
    if (!atomic_load(&g_fgr.on)) return -1;
    int idx = -1;
    pthread_mutex_lock(&g_fgrmu);
    if (g_fgr.np >= FGR_MAXP) g_fgr.overflow = 1;
    else {
        fgr_pass *p = &g_fgr.p[g_fgr.np];
        memset(p, 0, sizeof *p); p->w = w; p->h = h; p->layers = layers; p->samples = samples; p->frame = (uint32_t)g_fgr.done; p->after_disp = g_fgr.disp; g_fgr.disp = 0;
        p->natt = (uint32_t)(na > 10 ? 10 : na);
        for (uint32_t t = 0; t < p->natt; t++) p->a[t] = (fgr_att){ fgr_img_id(a[t].x, a[t].w, a[t].h, a[t].fmt, 0, 0), a[t].fmt, a[t].lv, a[t].ly, a[t].slot, a[t].clear, a[t].store };
        if (g_fgr.qp) {
            if (!g_fgr.np) vkCmdResetQueryPool(cb, g_fgr.qp, 0, (uint32_t)g_fgr.qn);
            if ((uint32_t)g_fgr.np * 2 + 1 < (uint32_t)g_fgr.qn) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_fgr.qp, (uint32_t)g_fgr.np * 2);
        }
        g_fgr.open = 1; idx = g_fgr.np++;
    }
    pthread_mutex_unlock(&g_fgrmu);
    return idx;
}
static void fgr_pass_end(VkCommandBuffer cb) {   // (after vkCmdEndRendering)
    if (!atomic_load(&g_fgr.on) || !g_fgr.open) return;
    pthread_mutex_lock(&g_fgrmu);
    int k = g_fgr.np - 1; g_fgr.open = 0;
    if (g_fgr.qp && k >= 0 && (uint32_t)k * 2 + 1 < (uint32_t)g_fgr.qn) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_fgr.qp, (uint32_t)k * 2 + 1);
    pthread_mutex_unlock(&g_fgrmu);
}
static void fgr_draw(void) { if (atomic_load(&g_fgr.on) && g_fgr.open && g_fgr.np > 0) g_fgr.p[g_fgr.np - 1].draws++; }
static void fgr_dispatch(void) { if (atomic_load(&g_fgr.on)) g_fgr.disp++; }
static void fgr_read_img(const void *x, uint32_t w, uint32_t h, uint32_t fmt, uint8_t kind) {
    if (!atomic_load(&g_fgr.on) || !x) return;
    pthread_mutex_lock(&g_fgrmu);
    if (g_fgr.nr < FGR_MAXR) { int id = fgr_img_id(x, w, h, fmt, 0, 0);
        g_fgr.r[g_fgr.nr++] = (fgr_read){ (uint32_t)g_fgr.np, (uint32_t)g_fgr.open, (uint32_t)g_fgr.done, id, kind }; }
    else g_fgr.overflow = 1;   // (a read not kept: the decisions until the next reset store everything)
    pthread_mutex_unlock(&g_fgrmu);
}

static const char *fgr_store_name(int s) { return s == 1 ? "don't care" : s == 2 ? "none" : "store"; }
// What becomes of what pass k left in attachment a: the first later event on the same image (conservatively the whole image, any level / layer)
//   0 read in its frame, 1 overwritten unread in its frame (cleared by a later pass), 2 read in a later recorded frame, 3 overwritten unread later, 4 nothing seen
static int g_fgr_rstart[FGR_MAXP + 3];   // the first read made after pass j-1 began (reads are kept in that order): fgr_write fills it
static int fgr_fate(int k, const fgr_att *a) {
    const fgr_pass *p = &g_fgr.p[k];
    int img = a->img; uint32_t fr = p->frame;
    // events after pass k, in order: reads (by pass count) and later passes attaching the image
    for (int j = k + 1; j <= g_fgr.np; j++) {
        // reads between pass j-1's start and pass j's start, other than inside pass k itself (its own feedback)
        for (int i = g_fgr_rstart[j]; i < g_fgr_rstart[j + 1]; i++) { const fgr_read *r = &g_fgr.r[i];
            if (r->img != img) continue;
            if (j == k + 1 && r->in_pass) continue;   // (read while pass k was open: it saw what was before, not this pass's output)
            return r->frame == fr ? 0 : 2; }
        if (j == g_fgr.np) break;
        const fgr_pass *q = &g_fgr.p[j];
        for (uint32_t t = 0; t < q->natt; t++) if (q->a[t].img == img) {
            int same = q->frame == fr;
            if (q->a[t].clear) return same ? 1 : 3;
            return same ? 0 : 2;   // loaded by that pass: needed
        }
    }
    return 4;
}
static void fgr_write(void) {
    const char *pc = getenv("DM_VK_PCACHE"); char dir[1024] = "."; if (pc) { snprintf(dir, sizeof dir, "%s", pc); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; }
    char path[1200]; if (g_fgr_named) snprintf(path, sizeof path, "%s/framegraph-%llu.txt", dir, (unsigned long long)g_fgr.frame0); else snprintf(path, sizeof path, "%s/framegraph.txt", dir);
    FILE *f = fopen(path, "w"); if (!f) { VLOG("[RED] frame recorder: cannot write %s", path); return; }
    if (g_fgr.qp) {   // the passes' GPU times (the GPU has finished: written after vkDeviceWaitIdle)
        for (int k = 0; k < g_fgr.np && (uint32_t)k * 2 + 1 < (uint32_t)g_fgr.qn; k++) { uint64_t t[2] = { 0, 0 };
            if (vkGetQueryPoolResults(g_dev, g_fgr.qp, (uint32_t)k * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) { g_fgr.p[k].t0 = t[0]; g_fgr.p[k].t1 = t[1]; } }
    }
    for (int j = 0, i = 0; j <= g_fgr.np + 2; j++) { while (i < g_fgr.nr && (int)g_fgr.r[i].pass < j) i++; g_fgr_rstart[j] = i; }
    fprintf(f, "Frame graph: %d frame(s) from frame %llu, %d passes, %d reads, %d images; GPU %s; times are GPU time between a pass's start and end (approximate on tilers)\n\n",
            g_fgr.done, (unsigned long long)g_fgr.frame0, g_fgr.np, g_fgr.nr, g_fgr.ni, g_props.deviceName);
    for (int fi = 0; fi < g_fgr.done; fi++) {
        uint32_t a0 = g_fgr.pf[fi], a1 = g_fgr.pf[fi + 1];
        int stores = 0, need = 0, unread = 0, later = 0, unknown = 0, last3d = -1, shown = -1; double gpu = 0;
        for (int i = 0; i < g_fgr.nr; i++) if (g_fgr.r[i].frame == (uint32_t)fi && g_fgr.r[i].kind == FGR_SHOWN) shown = g_fgr.r[i].img;
        fprintf(f, "== frame %llu: passes %u..%u\n", (unsigned long long)g_fgr.frame0 + (unsigned)fi, a0, a1 ? a1 - 1 : 0);
        for (uint32_t k = a0; k < a1; k++) {
            fgr_pass *p = &g_fgr.p[k]; double ms = p->t1 > p->t0 ? (p->t1 - p->t0) * g_fgr.tsper / 1e6 : -1; if (ms > 0) gpu += ms;
            int hasd = 0; for (uint32_t t = 0; t < p->natt; t++) if (p->a[t].slot >= 8) hasd = 1;
            if (hasd) last3d = (int)k;
            fprintf(f, "pass %u: %ux%u%s%s, %u draws", k, p->w, p->h, p->layers > 1 ? " layered" : "", p->samples > 1 ? " MSAA" : "", p->draws);
            if (p->after_disp) fprintf(f, " (after %u compute dispatches)", p->after_disp);
            if (ms >= 0) fprintf(f, ", GPU %.3f ms", ms);
            fputc('\n', f);
            for (uint32_t t = 0; t < p->natt; t++) { const fgr_att *a = &p->a[t]; const fgr_img *im = a->img >= 0 ? &g_fgr.im[a->img] : NULL;
                int fate = fgr_fate((int)k, a); stores += a->store == 0;
                if (a->store == 0) { if (fate == 0 || fate == 2) need++; else if (fate == 1) unread++; else if (fate == 3) later++; else unknown++; }
                static const char *fn[] = { "read in this frame", "overwritten unread in this frame", "read in a later frame", "overwritten unread in a later frame", "not seen again" };
                char sn[8]; if (a->slot < 8) snprintf(sn, sizeof sn, "c%u", a->slot); else snprintf(sn, sizeof sn, "%s", a->slot == 8 ? "depth" : "stencil");
                fprintf(f, "  %s img%d %ux%u vk%u L%u/%u: %s, %s -> %s%s\n", sn, a->img,
                        im ? im->w : 0, im ? im->h : 0, a->fmt, a->lv, a->ly, a->clear ? "cleared" : "loaded", fgr_store_name(a->store), fn[fate], a->img == shown ? " (SHOWN)" : ""); }
            for (int i = 0; i < g_fgr.nr; i++) { const fgr_read *r = &g_fgr.r[i]; if (r->pass == k + 1 && r->in_pass && r->kind == FGR_READ) fprintf(f, "  reads img%d\n", r->img); }
            if (k + 1 < a1) { const fgr_pass *q = &g_fgr.p[k + 1];
                if (q->w == p->w && q->h == p->h && q->layers == p->layers && q->samples == p->samples && !q->after_disp) fprintf(f, "  -> next pass same size: merge candidate\n"); }
        }
        fprintf(f, "frame %llu summary: %u passes, GPU %.2f ms in passes; stores %d: needed %d, overwritten unread %d (tile-memory candidates), unread then overwritten in a later frame %d, not seen again %d\n",
                (unsigned long long)g_fgr.frame0 + (unsigned)fi, a1 - a0, gpu, stores, need, unread, later, unknown);
        (void)last3d;
        if (shown >= 0) {   // the scene / HUD split (where frame generation would take the picture without the HUD)
            // the final image: what the pass drawing the shown image reads, when that pass is a copy of one rendered image (Sword / Shield: pass 156 copies img2); else the shown one
            int fin = shown, wrote_shown = -1;
            for (uint32_t k = a0; k < a1; k++) for (uint32_t t = 0; t < g_fgr.p[k].natt; t++) if (g_fgr.p[k].a[t].img == shown) wrote_shown = (int)k;
            if (wrote_shown >= 0) { int cand = -1, nc = 0;
                for (int i = g_fgr_rstart[wrote_shown + 1]; i < g_fgr_rstart[wrote_shown + 2]; i++) { const fgr_read *r = &g_fgr.r[i]; if (!r->in_pass || r->img < 0 || r->img == shown) continue;
                    int rendered = 0; for (uint32_t q = a0; q < (uint32_t)wrote_shown && !rendered; q++) for (uint32_t t = 0; t < g_fgr.p[q].natt; t++) if (g_fgr.p[q].a[t].img == r->img) rendered = 1;
                    if (rendered && r->img != cand) { cand = r->img; nc++; } }
                if (nc == 1) fin = cand; }
            // the passes drawing into it: a scene / post pass reads something rendered earlier in this frame; a HUD pass reads only textures not rendered this frame
            int last_scene = -1; char hud[512] = ""; size_t hl = 0;
            for (uint32_t k = a0; k < a1; k++) { int writes = 0; for (uint32_t t = 0; t < g_fgr.p[k].natt; t++) if (g_fgr.p[k].a[t].img == fin && g_fgr.p[k].a[t].slot < 8) writes = 1;
                if (!writes || !g_fgr.p[k].draws) continue;
                int reads_rendered = 0;
                for (int i = g_fgr_rstart[k + 1]; i < g_fgr_rstart[k + 2] && !reads_rendered; i++) { const fgr_read *r = &g_fgr.r[i]; if (!r->in_pass || r->img < 0) continue;
                    for (uint32_t q = a0; q < k && !reads_rendered; q++) for (uint32_t t = 0; t < g_fgr.p[q].natt; t++) if (g_fgr.p[q].a[t].img == r->img) { reads_rendered = 1; break; } }
                if (reads_rendered) { last_scene = (int)k; hl = 0; hud[0] = 0; }   // (a later scene pass: what came before it was not the HUD on top)
                else if (hl < sizeof hud - 32) hl += (size_t)snprintf(hud + hl, sizeof hud - hl, " pass %u (%u draws)", k, g_fgr.p[k].draws); }
            fprintf(f, "  scene / HUD split: the final image img%d (shown: img%d); the scene is complete after pass %d; HUD drawn on it:%s\n", fin, shown, last_scene, hl ? hud : " none found");
        }
        fputc('\n', f);
    }
    fclose(f);
    const char *id = getenv("DM_PROGRAM_ID"); char dl[256]; if (g_fgr_named) snprintf(dl, sizeof dl, "/storage/emulated/0/Download/%s-framegraph-%llu.txt", id ? id : "game", (unsigned long long)g_fgr.frame0); else snprintf(dl, sizeof dl, "/storage/emulated/0/Download/%s-framegraph.txt", id ? id : "game");
    { FILE *a = fopen(path, "r"), *b = a ? fopen(dl, "w") : NULL; char buf[8192]; size_t n; while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b); if (a) fclose(a); if (b) fclose(b); }
    VLOG("frame recorder: %d frame(s), %d passes written to %s (and Download/)", g_fgr.done, g_fgr.np, path);
}
// at every present, after the frame's work was built: arms, counts frames, ends, and writes once the GPU is done
static void fgr_present(uint64_t n, const void *shown, uint32_t w, uint32_t h, uint32_t fmt) {
    if (g_fgr.cont) return;   // (recording for the held frames' decisions: no recordings asked for meanwhile)
    if (atomic_load(&g_fgr.on)) {
        fgr_read_img(shown, w, h, fmt, FGR_SHOWN);
        pthread_mutex_lock(&g_fgrmu);
        g_fgr.done++; g_fgr.pf[g_fgr.done] = (uint32_t)g_fgr.np;
        if (g_fgr.done >= g_fgr.want) { atomic_store(&g_fgr.on, 0); g_fgr.pending_write = 2; }
        pthread_mutex_unlock(&g_fgrmu);
        return;
    }
    if (g_fgr.pending_write) {   // two presents later, then with the GPU idle (as the frame capture)
        if (--g_fgr.pending_write) return;
        pthread_mutex_lock(&g_qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_qmu);
        fgr_write(); return;
    }
    int want = 0;
    { static long at = -2; static int frames; if (at == -2) { const char *e = getenv("DM_FG_RECORD"); at = e ? atol(e) : -1; const char *c = e ? strchr(e, ':') : NULL; frames = c ? atoi(c + 1) : 3; }
      if (at > 0 && (long)n == at) want = frames; }
    { pthread_mutex_lock(&g_fgrmu); int hit = -1; for (int i = 0; i < g_fgr_nser; i++) if (g_fgr_series[i] <= n) hit = i;   // (an armed series: its next recording, 2 frames)
      if (hit >= 0) { g_fgr_series[hit] = g_fgr_series[--g_fgr_nser]; want = 2; g_fgr_named = 1; }
      pthread_mutex_unlock(&g_fgrmu); }
    static uint64_t req_at; static int req_want;   // a request with a wait (the menu's button: time to close the menu first)
    if (!want && req_at && n >= req_at) { want = req_want; req_at = 0; g_fgr_named = 0; }
    if (!want && !req_at && n % 30 == 15) {
        const char *pc = getenv("DM_VK_PCACHE"); char req[1100]; if (!pc) return; snprintf(req, sizeof req, "%s", pc); char *sl = strrchr(req, '/'); if (!sl) return;
        snprintf(sl + 1, sizeof req - (size_t)(sl + 1 - req), "framegraph.req");
        FILE *f = fopen(req, "r");
        if (f) { int fr = 3, wait = 0; int k = fscanf(f, "%d %d", &fr, &wait); fclose(f); unlink(req); if (k < 1) fr = 3;   // "<frames> [<presents to wait>]"
            if (wait > 0) { req_at = n + (uint64_t)wait; req_want = fr; VLOG("frame recorder: asked for, in %d frames", wait); return; }
            want = fr; g_fgr_named = 0; }
    }
    if (want <= 0) return;
    if (want > FGR_FRAMES) want = FGR_FRAMES;
    pthread_mutex_lock(&g_fgrmu);
    g_fgr.want = want; g_fgr.done = 0; g_fgr.np = g_fgr.nr = g_fgr.ni = 0; g_fgr.open = 0; g_fgr.disp = 0; g_fgr.pf[0] = 0; g_fgr.frame0 = n + 1;
    if (!g_fgr.qp && g_tsq_ok()) {
        g_fgr.qn = FGR_MAXP * 2; VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = (uint32_t)g_fgr.qn };
        if (vkCreateQueryPool(g_dev, &qi, NULL, &g_fgr.qp)) g_fgr.qp = VK_NULL_HANDLE;
        g_fgr.tsper = g_props.limits.timestampPeriod; }
    atomic_store(&g_fgr.on, 1);
    pthread_mutex_unlock(&g_fgrmu);
    VLOG("frame recorder: recording the next %d frame(s)%s", want, g_fgr.qp ? ", with each pass's GPU time" : " (no GPU timestamps on this GPU)");
}
// ---- continuous recording for the held frame (nvn_vk.c, DM_VK_FRAME_HOLD=2): each pass's stores decided from what the rest of the held work does ----
static void fgr_cont_start(void) { pthread_mutex_lock(&g_fgrmu); g_fgr.cont = 1; g_fgr.want = 1 << 30; atomic_store(&g_fgr.on, 1); pthread_mutex_unlock(&g_fgrmu); }
static void fgr_cont_reset(void) {   // the held work was submitted and nothing is held or being recorded: a fresh window
    pthread_mutex_lock(&g_fgrmu); g_fgr.np = g_fgr.nr = g_fgr.ni = 0; g_fgr.open = 0; g_fgr.disp = 0; g_fgr.overflow = 0; pthread_mutex_unlock(&g_fgrmu);
}
static void fgr_rstart_build(void) { for (int j = 0, i = 0; j <= g_fgr.np + 2; j++) { while (i < g_fgr.nr && (int)g_fgr.r[i].pass < j) i++; g_fgr_rstart[j] = i; } }   // (caller holds g_fgrmu)
// What pass k's attachment a holds when the work recorded so far ends, exactly for its own subresource and aspect: 0 read (by a read of the image, or a later pass
//   loading it or clearing it only in part), 1 overwritten unread (a later pass clears that same level, layer and aspect over the whole of it), 4 nothing yet (unknown).
//   Any read of the image counts, whatever level or layer it reads (on the safe side). Caller holds g_fgrmu, rstart built
static int fgr_fate_exact(int k, const fgr_att *a) {
    if (a->img < 0) return 0;
    const fgr_img *im = &g_fgr.im[a->img]; uint32_t lw = im->w >> a->lv ? im->w >> a->lv : 1, lh = im->h >> a->lv ? im->h >> a->lv : 1;
    int cls = a->slot < 8 ? 0 : a->slot;   // colour, depth or stencil
    for (int j = k + 1; j <= g_fgr.np; j++) {
        for (int i = g_fgr_rstart[j]; i < g_fgr_rstart[j + 1]; i++) { const fgr_read *r = &g_fgr.r[i];
            if (r->img != a->img) continue;
            if (j == k + 1 && r->in_pass) continue;   // (read while pass k was open: what was there before it)
            return 0; }
        if (j == g_fgr.np) break;
        const fgr_pass *q = &g_fgr.p[j];
        for (uint32_t t = 0; t < q->natt; t++) { const fgr_att *b = &q->a[t];
            if (b->img != a->img || (b->slot < 8 ? 0 : b->slot) != cls) continue;
            if (b->lv != a->lv || b->ly != a->ly) continue;   // another level or layer of it: not ours
            return b->clear && q->w >= lw && q->h >= lh && q->layers == 1 ? 1 : 0;
        }
    }
    return 4;
}
// three short recordings of what follows an event, at about 1, 3 and 5 s (30 / 90 / 150 presents later): a battle's transition (hle_resscale.c, DM_FG_ON_BATTLE)
static void fgr_arm_series(uint64_t now) {
    pthread_mutex_lock(&g_fgrmu);
    if (!g_fgr_nser && !atomic_load(&g_fgr.on) && !g_fgr.cont) { g_fgr_series[0] = now + 30; g_fgr_series[1] = now + 90; g_fgr_series[2] = now + 150; g_fgr_nser = 3;
        VLOG("frame recorder: an event at present %llu: recordings at +30, +90 and +150 presents", (unsigned long long)now); }
    pthread_mutex_unlock(&g_fgrmu);
}
#endif

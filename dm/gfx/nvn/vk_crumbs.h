// GPU breadcrumbs (included by nvn_vk.c after the submission types): what each submission held, kept in memory and printed when the GPU fails, so a device
// lost or a GPU hang names the work it died in instead of only "vkWaitSemaphores failed: -4".
//   Always on, a few stores per pass and per pipeline change: for each of the NSUB submission slots its frame, fence, the passes it began (size, first colour
//   format, depth format) and the pipelines it bound (vertex / fragment shader keys, with the pass each came in), the last CR_PASS / CR_PIPE of each; and a
//   history of the last CR_HIST submissions that completed (passes, pipelines, CPU→GPU time), about the last few seconds of play.
//   DM_VK_BREADCRUMBS=1 (☰ → Bug reports → GPU crash tracing): the GPU itself also writes, after each pass of a game submission, the pass's number into a small
//   host-visible buffer (a barrier then vkCmdFillBuffer, outside the pass), so after a device lost the first pass it did not finish is known. It costs a
//   barrier per pass (passes no longer overlap on the GPU), so it is off unless asked for.
// Why not LunarG's Crash Diagnostic Layer: a driver package (Turnip, PanVK) is opened directly (vk_open), past Android's Vulkan loader, so no layer can sit
//   in front of it; and on the phone's own driver Android loads layers from an app's own folder only when the app is debuggable, which releases are not.
// Printed (lines "[gpu-crumbs]") on a device lost or a failed submit (before the fatal error), once when a submission is 10 s late (a GPU hang), and from the
//   crash handler (crash.c, nvn_vk_crumbs_dump). DebugBundle.java copies these lines into shaders_at_crash.txt.
#ifndef VK_CRUMBS_H
#define VK_CRUMBS_H
enum { CR_PASS = 64, CR_PIPE = 48, CR_HIST = 256 };
typedef struct { uint32_t idx, w, h, cf, ds; } cr_pass;
typedef struct { uint32_t pass; char vs[24], fs[24]; } cr_bind;
typedef struct {
    uint64_t frame, fence, t_begin, t_submit;
    uint32_t npass, nbind, open;           // passes begun, pipelines bound (changes), a pass open now (1 + its index)
    cr_pass pass[CR_PASS]; cr_bind pipe[CR_PIPE];   // rings: entry n % CR_*
} cr_sub;
typedef struct { uint64_t frame, fence; uint32_t npass, nbind; uint32_t us; } cr_hist;
static cr_sub g_cr[NSUB];
static cr_hist g_crh[CR_HIST]; static _Atomic uint64_t g_crh_n;
static int g_cr_gpu = -1;                          // DM_VK_BREADCRUMBS: -1 not read yet
static VkBuffer g_cr_buf; static VkDeviceMemory g_cr_mem; static volatile uint32_t *g_cr_map;   // NSUB words: the last pass each slot's GPU work finished (1-based)

static int cr_slot(const sub_t *s) { return (int)(s - g_subs); }
static void cr_gpu_init(void) {
    const char *e = getenv("DM_VK_BREADCRUMBS"); g_cr_gpu = e && atoi(e) > 0;
    if (!g_cr_gpu) return;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements rq; int t;
    if (vkCreateBuffer(g_dev, &bi, NULL, &g_cr_buf)) { g_cr_gpu = 0; return; }
    vkGetBufferMemoryRequirements(g_dev, g_cr_buf, &rq);
    t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
    void *m = NULL;
    if (t < 0 || vkAllocateMemory(g_dev, &ai, NULL, &g_cr_mem) || vkBindBufferMemory(g_dev, g_cr_buf, g_cr_mem, 0) || vkMapMemory(g_dev, g_cr_mem, 0, VK_WHOLE_SIZE, 0, &m)) {
        VLOG("[RED] GPU breadcrumbs: no host-visible buffer, the GPU's pass marks are off"); g_cr_gpu = 0; return; }
    g_cr_map = m; memset(m, 0, 256);
    VLOG("GPU breadcrumbs on (DM_VK_BREADCRUMBS): the GPU marks each finished pass; a barrier per pass, slower");
}
static void cr_sub_begin(sub_t *s) {
    if (g_cr_gpu < 0) cr_gpu_init();
    cr_sub *c = &g_cr[cr_slot(s)];
    c->frame = g_frame_no; c->fence = 0; c->t_begin = mono_ns(); c->t_submit = 0; c->npass = c->nbind = c->open = 0;
    if (g_cr_map) g_cr_map[cr_slot(s)] = 0;   // (the slot's last submission has completed: mtl_sub_begin waited for it)
}
static cr_sub *cr_cur(VkCommandBuffer cb) { return g_sub && cb == g_sub->cb && on_sub_thread() ? &g_cr[cr_slot(g_sub)] : NULL; }
static void cr_pass_begin(VkCommandBuffer cb, const VkRenderingInfo *ri, const uint32_t *cf, uint32_t dsfmt) {
    cr_sub *c = cr_cur(cb); if (!c) return;
    cr_pass *p = &c->pass[c->npass % CR_PASS];
    p->idx = ++c->npass; p->w = ri->renderArea.extent.width; p->h = ri->renderArea.extent.height;
    p->cf = cf && ri->colorAttachmentCount ? cf[0] : 0; p->ds = dsfmt;
    c->open = p->idx;
}
static void cr_pass_end(VkCommandBuffer cb) {
    cr_sub *c = cr_cur(cb); if (!c) return;
    uint32_t idx = c->open; c->open = 0;
    if (!g_cr_map || !idx) return;
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, &mb, 0, NULL, 0, NULL);   // (the mark only once the pass is done)
    vkCmdFillBuffer(cb, g_cr_buf, (VkDeviceSize)cr_slot(g_sub) * 4, 4, idx);
}
static void cr_pipe(const char *vs, const char *fs) {
    if (!g_sub || !on_sub_thread()) return;
    cr_sub *c = &g_cr[cr_slot(g_sub)]; cr_bind *p = &c->pipe[c->nbind++ % CR_PIPE];
    p->pass = c->open ? c->open : c->npass;
    snprintf(p->vs, sizeof p->vs, "%s", vs ? vs : "-"); snprintf(p->fs, sizeof p->fs, "%s", fs ? fs : "-");
}
static int cr_ours(const sub_t *s) { return s >= g_subs && s < g_subs + NSUB; }   // (the present thread's own submissions are not slots)
static void cr_submitted(sub_t *s) { if (!cr_ours(s)) return; cr_sub *c = &g_cr[cr_slot(s)]; c->fence = s->fence; c->t_submit = mono_ns(); }
static void cr_completed(sub_t *s) {
    if (!cr_ours(s)) return;
    cr_sub *c = &g_cr[cr_slot(s)]; uint64_t n = atomic_fetch_add(&g_crh_n, 1);
    cr_hist *h = &g_crh[n % CR_HIST]; h->frame = c->frame; h->fence = c->fence; h->npass = c->npass; h->nbind = c->nbind;
    h->us = c->t_submit ? (uint32_t)((mono_ns() - c->t_submit) / 1000) : 0;
}
// One submission: its passes and pipelines around where the GPU stopped (mark: the last pass it finished, 0 none or unknown)
static void cr_print_sub(int slot, const char *what, uint64_t now) {
    cr_sub *c = &g_cr[slot]; uint32_t mark = g_cr_map ? g_cr_map[slot] : 0;
    fprintf(stderr, "[dm] [gpu-crumbs] %s: slot %d fence %llu frame %llu, %u passes, %u pipeline binds, submitted %.0f ms ago", what, slot, (unsigned long long)c->fence,
            (unsigned long long)c->frame, c->npass, c->nbind, c->t_submit ? (now - c->t_submit) / 1e6 : -1.0);
    if (g_cr_map) fprintf(stderr, "; the GPU finished pass %u of %u%s\n", mark, c->npass, mark < c->npass ? " (the next one is where it stopped)" : ""); else fprintf(stderr, " (DM_VK_BREADCRUMBS=1 marks the pass the GPU stopped in)\n");
    uint32_t from = c->npass > CR_PASS ? c->npass - CR_PASS + 1 : 1;
    if (g_cr_map && mark + 1 >= from) from = mark > 4 ? (mark - 4 > from ? mark - 4 : from) : from;   // (from a few passes before the mark)
    else if (c->npass > 12) from = c->npass - 11 > from ? c->npass - 11 : from;
    for (uint32_t i = from; i <= c->npass && i < from + 16; i++) { cr_pass *p = &c->pass[(i - 1) % CR_PASS]; if (p->idx != i) continue;
        fprintf(stderr, "[dm] [gpu-crumbs]   pass %u%s: %ux%u colour fmt %u depth fmt %u\n", i, g_cr_map && i == mark + 1 ? " <- not finished" : "", p->w, p->h, p->cf, p->ds);
        for (uint32_t k = c->nbind > CR_PIPE ? c->nbind - CR_PIPE : 0; k < c->nbind; k++) { cr_bind *q = &c->pipe[k % CR_PIPE]; if (q->pass == i) fprintf(stderr, "[dm] [gpu-crumbs]     pipeline vertex %s fragment %s\n", q->vs, q->fs); } }
}
// Everything known: the submissions still on the GPU (oldest first: the first is the one that failed or hangs) and the last completed ones
void nvn_vk_crumbs_dump(const char *why) {
    uint64_t now = mono_ns(); fprintf(stderr, "[dm] [gpu-crumbs] ---- %s ----\n", why ? why : "dump");
    int n = g_nin; if (n < 0) n = 0; if (n > NSUB * 2) n = NSUB * 2;   // (read without the lock: a crash or a dying device, the completion thread may hold it)
    for (int i = 0; i < n; i++) { sub_t *s = g_inflight[i]; if (cr_ours(s)) cr_print_sub(cr_slot(s), i == 0 ? "oldest unfinished submission" : "also on the GPU", now); }
    if (g_sub) cr_print_sub(cr_slot(g_sub), "being recorded", now);
    uint64_t h = atomic_load(&g_crh_n); fprintf(stderr, "[dm] [gpu-crumbs] last completed submissions (newest last; frame fence passes binds submit->done us):");
    for (uint64_t k = h > 24 ? h - 24 : 0; k < h; k++) { cr_hist *e = &g_crh[k % CR_HIST]; fprintf(stderr, "%s%llu/%llu/%u/%u/%u", (k - (h > 24 ? h - 24 : 0)) % 6 ? " " : "\n[dm] [gpu-crumbs]   ",
        (unsigned long long)e->frame, (unsigned long long)e->fence, e->npass, e->nbind, e->us); }
    fprintf(stderr, "\n"); fflush(stderr);
}
#endif

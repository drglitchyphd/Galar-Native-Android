// ================= The Mali white-screen ladder and its detector (nvn_vk.c; the rules: mali_white.h, tested by dm/gfx/tests/t_mali_white.c) =================
// Arm's own Mali driver draws Sword / Shield's lit scene (an R11G11B10F target, 1280x720 or 1920x1080, hundreds of draws a frame) at the format's maximum: a white
//   picture (#47 #110 #111 #172 #214 #282); PanVK and Adreno draw it right. Two fixes: route A, R11G11B10F drawn as an RGBA16F stand-in (b10_on: the only change that
//   ever took the white away there, #282, Mali-G720), the default on Arm's own driver with Sword / Shield; route B, R11G11B10F kept without framebuffer compression
//   (g_b10nc). There are no Mali testers, so the game checks its own picture on each player's phone:
//  - the lit target: in each frame, the R11G11B10F colour target (or its stand-in) at least DM_MALI_WHITE_MIN_W (1280) wide that the most draws went into, and the
//    part of it they drew (the union of their viewports and scissors: Sword / Shield draw the scene into part of a full-size target at a lower render scale);
//  - while playing (a frame of DM_MALI_WHITE_MIN_DRAWS (200) draws or more that drew into it), every DM_MALI_WHITE_EVERY_MS (3 s) at a present: a compute pass of its
//    own (overlay/mali_white.comp: a 32 x 18 grid of texel fetches, sampled image + storage buffer, Vulkan 1.0) submitted after the frame's work, ordered by a full
//    barrier, into a host-visible buffer read once its fence has signalled at a later present (never waited for): texels with a channel at 60000 or more, infinite
//    or NaN, of 576;
//  - the verdict (mw_judge_add) moves the ladder (mw_ladder_verdict), kept per phone in mali_ladder.txt in the app's files folder (DM_MALI_LADDER_FILE, set by
//    android_main.c; else beside the pipeline cache): route A white → route B from the next start, with a note over the game; B white too → route A again and the app
//    offers PanVK at its next start (MaliLadder.java); a route found fine is kept and the check stops for good (until the driver or the app changes). Every sample,
//    verdict and step is a "[mali-white]" line in the log; the state is in the frame capture's notes.
// Nothing of this runs on any other driver, nor for Legends: Arceus / Scarlet / Violet, unless DM_MALI_LADDER=1 or DM_MALI_WHITE_SIM=1 asks (mali_white.h)
#include "mali_white.h"
#include "overlay/mali_white_spv.h"
void front_note(const char *key) __attribute__((weak));   // android_main.c: a one-line note over the game by its key (MaliLadder.java shows it, translated)
#define MWLOG(...) VLOG("[mali-white] " __VA_ARGS__)
enum { MW_SLOTS = 3, MW_GX = 32, MW_GY = 18, MW_TEX = MW_GX * MW_GY, MW_WORDS = 2 * MW_TEX, MW_CAND = 4 };
typedef struct { vtex *x; uint64_t draws; int32_t r[4]; } mw_cand;   // a lit-target candidate of the frame: its draws, the part they drew (x0, y0, x1, y1)
static struct {
    int on, sampling, sim, record;   // the ladder runs; the detector samples this start; DM_MALI_WHITE_SIM; verdicts are kept (this start runs route A or B alone)
    int route, want;                 // MW_ROUTE_*: what this start runs, what the ladder (or the default) asked for
    mw_rules rules; mw_judge judge; mw_state st; char path[1100];
    uint64_t last_ns, frame_d0, pd0; int32_t pr[4];   // the last sample's time; draws at the frame's start, at the open pass's start; the open pass's drawn part
    int nsamples, nsmall; uint64_t quiet_ns; int qn[3];   // samples read; frames whose drawn part was too small; past the first 30 samples, one line a minute (qn: its counts)
    mw_cand cand[MW_CAND]; int ncand;
    int gpu;                         // the compute pass: 0 not made yet, 1 ready, -1 failed
    VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline cs; VkDescriptorPool dp; VkCommandPool cp; VkBuffer buf; vmalloc_t ba; const uint32_t *map;
    struct { VkCommandBuffer cb; VkFence f; VkDescriptorSet ds; vtex *x; int busy; uint64_t seq, present; int32_t r[4]; uint32_t w, h; } slot[MW_SLOTS]; uint64_t seq;
} g_mw;
static const char *const MW_NOTE_TRY_B = "Mali: the white-screen fix did not take; trying the second fix at the next start";   // (MaliLadder.java's texts, by key)
static const char *const MW_NOTE_PANVK = "Mali: neither white-screen fix took; the next start offers the PanVK driver";

static void mw_path(char *out, size_t n) {   // the state file: DM_MALI_LADDER_FILE (the app's files folder), else beside the pipeline cache, else none (in memory only)
    const char *f = getenv("DM_MALI_LADDER_FILE"), *pc = getenv("DM_VK_PCACHE"); const char *sl = pc ? strrchr(pc, '/') : NULL;
    if (f && *f) snprintf(out, n, "%s", f);
    else if (sl) snprintf(out, n, "%.*s/mali_ladder.txt", (int)(sl - pc), pc);
    else out[0] = 0;
}
static int mw_gpu_init(void) {   // the compute pass, its descriptor sets, command buffers, fences and read-back buffer: made once (the start's first check)
    if (g_mw.gpu) return g_mw.gpu > 0;
    g_mw.gpu = -1;
    // a driver that crashed making it (#89: a Mali-G57 closed on a tiny test shader) leaves the marker: later starts on that driver go without the check
    const char *le = getenv("DM_MALI_LADDER");
    if (mark_file(drv_mark("mali_white_crashed"), "", 0) && !(le && atoi(le) > 0)) { MWLOG("[RED] the check's compute shader closed the game on this driver at an earlier start: no check (DM_MALI_LADDER=1 tries again)"); return 0; }
    mark_file(drv_mark("mali_white_crashed"), "making the white-screen check's compute shader; if this file is still here the driver crashed doing so, and later starts go without the check\n", 1);
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof mali_white_comp_spv, .pCode = mali_white_comp_spv };
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };   // (plain sets: no push descriptors needed)
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 };
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MW_SLOTS }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MW_SLOTS } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = MW_SLOTS, .poolSizeCount = 2, .pPoolSizes = ps };
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf };
    if (vkCreateShaderModule(g_dev, &mi, NULL, &g_mw.sm) || vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_mw.dsl) || vkCreateDescriptorPool(g_dev, &dpi, NULL, &g_mw.dp)
        || vkCreateCommandPool(g_dev, &cpi, NULL, &g_mw.cp)) goto fail;
    { VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_mw.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
      if (vkCreatePipelineLayout(g_dev, &pi, NULL, &g_mw.pl)) goto fail;
      VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = g_mw.sm, .pName = "main" }, .layout = g_mw.pl };
      if (vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_mw.cs)) goto fail; }
    { VkDescriptorSetLayout l[MW_SLOTS]; VkDescriptorSet ds[MW_SLOTS]; VkCommandBuffer cbs[MW_SLOTS];
      for (int i = 0; i < MW_SLOTS; i++) l[i] = g_mw.dsl;
      VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_mw.dp, .descriptorSetCount = MW_SLOTS, .pSetLayouts = l };
      VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_mw.cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = MW_SLOTS };
      if (vkAllocateDescriptorSets(g_dev, &ai, ds) || vkAllocateCommandBuffers(g_dev, &cai, cbs)) goto fail;
      VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
      for (int i = 0; i < MW_SLOTS; i++) { g_mw.slot[i].ds = ds[i]; g_mw.slot[i].cb = cbs[i]; if (vkCreateFence(g_dev, &fi, NULL, &g_mw.slot[i].f)) goto fail; } }
    { VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)MW_SLOTS * MW_WORDS * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
      if (vkCreateBuffer(g_dev, &bi, NULL, &g_mw.buf)) goto fail;
      VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, g_mw.buf, &rq);
      g_mw.ba = vk_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
      if (!g_mw.ba.b->map || vkBindBufferMemory(g_dev, g_mw.buf, g_mw.ba.b->mem, g_mw.ba.off)) goto fail;
      g_mw.map = (const uint32_t *)(const void *)((const uint8_t *)g_mw.ba.b->map + g_mw.ba.off); }
    mark_file(drv_mark("mali_white_crashed"), "", -1);
    g_mw.gpu = 1;
    return 1;
fail:
    mark_file(drv_mark("mali_white_crashed"), "", -1);
    MWLOG("[RED] the check's compute pass could not be made: no check this start");
    return 0;
}
// mtl_init, before any image is made: whether the ladder runs, the route of this start (*a_on: the RGBA16F stand-in, *b_on: no compression; *why_a / *why_b: who asked)
static void mw_init(int *a_on, int *b_on, const char **why_a, const char **why_b) {
    int arm = g_arm_rel != 0; const char *le = getenv("DM_MALI_LADDER"), *se = getenv("DM_MALI_WHITE_SIM"), *ea = getenv("DM_VK_B10_AS_RGBA16"), *eb = getenv("DM_VK_B10_NOCOMPRESS");
    int en = mw_enabled(arm, DM_PORT_GALAR, le, se);
    g_mw.rules = mw_rules_default(); mw_rules_env(&g_mw.rules);
    g_mw.want = mw_default_route(arm, DM_PORT_GALAR);
    if (en) {
        g_mw.on = 1; g_mw.sim = se && *se && atoi(se) > 0;
        mw_path(g_mw.path, sizeof g_mw.path);
        const char *av = getenv("DM_APP_VERSION"); long app = av && *av ? atol(av) : -1;
        int have = mw_state_load(g_mw.path, &g_mw.st); mw_state was = g_mw.st;
        mw_plan p = mw_ladder_start(&g_mw.st, have, g_drv_id, g_props.driverVersion, app);
        if (p.reset) MWLOG("ladder started over: %s", !have ? (g_mw.path[0] ? "no state file yet" : "no state file (DM_MALI_LADDER_FILE / DM_VK_PCACHE unset): this start's results stay in memory")
                                                           : "another driver or app version than the state file's");
        if ((p.reset || memcmp(&was, &g_mw.st, sizeof was)) && g_mw.path[0] && !mw_state_save(g_mw.path, &g_mw.st)) MWLOG("[RED] the state file %s could not be written (errno %d)", g_mw.path, errno);
        g_mw.want = p.route; g_mw.sampling = p.sample;
        char rel[32] = "not Arm's own driver"; if (arm) snprintf(rel, sizeof rel, "Arm's r%dp%d", g_arm_rel, g_arm_p);
        MWLOG("on (%s): driver %d version %#x (%s), app %ld; results so far A %s, B %s, PanVK offer %s; this start: route %s, %s", en == 1 ? "Arm's own Mali driver with Sword / Shield"
              : le && *le && atoi(le) > 0 ? "DM_MALI_LADDER=1, a test" : "DM_MALI_WHITE_SIM=1: every sample counts as white", g_drv_id, g_props.driverVersion, rel, app,
              mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]), g_mw.st.panvk == MW_PANVK_OFFER ? "pending (the app shows it at its start)" : g_mw.st.panvk == MW_PANVK_SHOWN ? "shown" : "none",
              mw_route_name(p.route), p.sample ? "checked while playing" : p.exhausted ? "not checked (both fixes were white on this driver)" : "not checked (found fine on this driver)");
        if (p.sample) MWLOG("rules: white after %d samples in a row with %d %% or more of %d texels at 60000+, infinite or NaN; fine after %d in a row under %d %%; one sample every %d ms "
                            "of a frame with %d+ draws into an R11G11B10F target %d+ wide (state file %s)", g_mw.rules.white_n, g_mw.rules.white_pct, MW_TEX, g_mw.rules.fine_n, g_mw.rules.fine_pct,
                            g_mw.rules.every_ms, g_mw.rules.min_draws, g_mw.rules.min_w, g_mw.path[0] ? g_mw.path : "none");
    }
    g_mw.route = mw_route_apply(g_mw.want, ea, eb, a_on, b_on);
    g_mw.record = g_mw.route == MW_ROUTE_A || g_mw.route == MW_ROUTE_B;
    if (en && g_mw.route != g_mw.want)
        MWLOG("env.txt sets%s%s: this start runs %s, not route %s%s", ea && *ea ? " DM_VK_B10_AS_RGBA16" : "", eb && *eb ? " DM_VK_B10_NOCOMPRESS" : "", mw_route_name(g_mw.route),
              mw_route_name(g_mw.want), g_mw.record ? " (its verdict is kept for that route)" : " (its verdict is logged, not kept)");
    const char *dflt = en ? "the white-screen ladder's route" : !g_mw.want ? "" : le && *le && !atoi(le) ? "the white-screen fix on Arm's own Mali driver with Sword / Shield; DM_MALI_LADDER=0: no check"
                                                                                                            : "the white-screen fix on Arm's own Mali driver with Sword / Shield";
    *why_a = ea && *ea ? "DM_VK_B10_AS_RGBA16, a Mali test" : dflt; *why_b = eb && *eb ? "DM_VK_B10_NOCOMPRESS, a Mali test" : dflt;
    if (g_mw.sampling && !mw_gpu_init()) g_mw.sampling = 0;
}
// pass_begin: a colour target in R11G11B10F or its stand-in (lv: the attachment's level); the pass's largest one is its lit-target candidate
static void mw_pass_target(vtex *x, uint32_t lv) {
    if (!g_mw.sampling || P_tf || lv || !x || x->samples != 1 || x->is3d || !(x->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || x->w < (uint32_t)g_mw.rules.min_w) return;
    if (g_mw_px && (uint64_t)g_mw_px->w * g_mw_px->h >= (uint64_t)x->w * x->h) return;
    g_mw_px = x; g_mw.pd0 = g_stat_draws; g_mw.pr[0] = g_mw.pr[1] = INT32_MAX; g_mw.pr[2] = g_mw.pr[3] = INT32_MIN;
}
// draw_, in a pass with a candidate: what the draw covers (its viewport within its scissor, which is within the pass), into the pass's drawn part
static void mw_draw_rect(float vx, float vy, float vw, float vh, const VkRect2D *sc) {
    float x0 = vw < 0 ? vx + vw : vx, y0 = vh < 0 ? vy + vh : vy, x1 = x0 + fabsf(vw), y1 = y0 + fabsf(vh);
    float sx0 = (float)sc->offset.x, sy0 = (float)sc->offset.y, sx1 = sx0 + (float)sc->extent.width, sy1 = sy0 + (float)sc->extent.height;
    int32_t a = (int32_t)fmaxf(x0, sx0), b = (int32_t)fmaxf(y0, sy0), c = (int32_t)fminf(x1, sx1), d = (int32_t)fminf(y1, sy1);   // (a NaN viewport: the scissor's)
    if (c <= a || d <= b) return;
    if (a < g_mw.pr[0]) g_mw.pr[0] = a; if (b < g_mw.pr[1]) g_mw.pr[1] = b; if (c > g_mw.pr[2]) g_mw.pr[2] = c; if (d > g_mw.pr[3]) g_mw.pr[3] = d;
}
// pass_end of a pass with a candidate: its draws and drawn part go to the frame's candidate for that image (held by a reference until the present)
static void mw_pass_end(void) {
    vtex *x = g_mw_px; g_mw_px = NULL;
    uint64_t d = g_stat_draws - g_mw.pd0; if (!d || g_mw.pr[2] <= g_mw.pr[0] || g_mw.pr[3] <= g_mw.pr[1]) return;
    mw_cand *c = NULL; for (int i = 0; i < g_mw.ncand; i++) if (g_mw.cand[i].x == x) c = &g_mw.cand[i];
    if (!c) {
        if (g_mw.ncand == MW_CAND) { int k = 0; for (int i = 1; i < MW_CAND; i++) if (g_mw.cand[i].draws < g_mw.cand[k].draws) k = i;   // (full: the one with the fewest draws goes)
            if (g_mw.cand[k].draws >= d) return; vtex_unref(g_mw.cand[k].x); c = &g_mw.cand[k]; }
        else c = &g_mw.cand[g_mw.ncand++];
        vtex_ref(x); c->x = x; c->draws = 0; c->r[0] = c->r[1] = INT32_MAX; c->r[2] = c->r[3] = INT32_MIN; }
    c->draws += d;
    if (g_mw.pr[0] < c->r[0]) c->r[0] = g_mw.pr[0]; if (g_mw.pr[1] < c->r[1]) c->r[1] = g_mw.pr[1]; if (g_mw.pr[2] > c->r[2]) c->r[2] = g_mw.pr[2]; if (g_mw.pr[3] > c->r[3]) c->r[3] = g_mw.pr[3];
}
// one sample of image x's part r (x0, y0, width, height): recorded and submitted after everything submitted so far (a full barrier first), read at a later present
//   (mw_poll); 0 when it could not be (no free slot, no view, the queue refused it)
static int mw_sample(vtex *x, const int32_t r[4], uint64_t present) {
    int k = -1; for (int i = 0; i < MW_SLOTS && k < 0; i++) if (!g_mw.slot[i].busy) k = i;
    if (k < 0 || !mw_gpu_init()) return 0;
    uint8_t sw[4] = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    VkImageView v = view_get(x, VK_IMAGE_VIEW_TYPE_2D, x->fmt, 0, 1, 0, 1, sw, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!v) { static int said; if (!said++) MWLOG("[RED] no view of the lit target (format %d) to check: not sampled", x->fmt); return 0; }
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL };   // (every image stays in the GENERAL layout)
    VkDescriptorBufferInfo bi = { g_mw.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wd[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_mw.slot[k].ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii },
                                   { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_mw.slot[k].ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi } };
    vkUpdateDescriptorSets(g_dev, 2, wd, 0, NULL);   // (this slot's set: its last use has finished, its fence was read)
    VkCommandBuffer cb = g_mw.slot[k].cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(cb, &bgi)) return 0;
    barrier_all(cb);   // everything before in submission order (the frame drawing the target) done and visible
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mw.cs);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mw.pl, 0, 1, &g_mw.slot[k].ds, 0, NULL);
    uint32_t pc[6] = { (uint32_t)r[0], (uint32_t)r[1], (uint32_t)r[2], (uint32_t)r[3], (uint32_t)(k * MW_WORDS), 0x476A6000u /* 60000.0f */ };
    vkCmdPushConstants(cb, g_mw.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, MW_GY, 1);
    barrier_host(cb);
    if (vkEndCommandBuffer(cb)) return 0;
    vkResetFences(g_dev, 1, &g_mw.slot[k].f);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    pthread_mutex_lock(&g_qmu); VkResult sr = vkQueueSubmit(g_q, 1, &si, g_mw.slot[k].f); pthread_mutex_unlock(&g_qmu);
    if (sr) { MWLOG("[RED] the check's submission failed (%d): the check stops", (int)sr); g_mw.sampling = 0; return 0; }
    vtex_ref(x); g_mw.slot[k].x = x; g_mw.slot[k].busy = 1; g_mw.slot[k].seq = ++g_mw.seq; g_mw.slot[k].present = present;
    memcpy(g_mw.slot[k].r, r, sizeof g_mw.slot[k].r); g_mw.slot[k].w = x->w; g_mw.slot[k].h = x->h;
    return 1;
}
// a finished slot's texels: how many are saturated (any verdict bit), infinite, NaN, and the largest finite channel
typedef struct { uint32_t sat, inf, nan; float mx; } mw_count;
static mw_count mw_read_slot(int k) {
    mw_count c = { 0, 0, 0, 0 }; const uint32_t *w = g_mw.map + (size_t)k * MW_WORDS; uint32_t mx = 0;
    for (int t = 0; t < MW_TEX; t++) { uint32_t f = w[2 * t]; c.sat += f != 0; c.inf += (f & 2) != 0; c.nan += (f & 4) != 0; if (w[2 * t + 1] > mx) mx = w[2 * t + 1]; }
    memcpy(&c.mx, &mx, 4);
    return c;
}
// a verdict: the ladder moves (when this start runs one of its routes alone), the state file is written, a note goes over the game; the check stops
static void mw_act(int v) {
    g_mw.sampling = 0;
    MWLOG("verdict after %d samples: route %s is %s", g_mw.judge.samples, mw_route_name(g_mw.route), v == MW_WHITE ? "WHITE" : "FINE");
    if (!g_mw.record) { MWLOG("this start runs %s (set in env.txt), not one of the ladder's routes alone: the verdict is not kept; no more checks this start", mw_route_name(g_mw.route)); return; }
    int ev = mw_ladder_verdict(&g_mw.st, g_mw.route, v), saved = g_mw.path[0] && mw_state_save(g_mw.path, &g_mw.st);
    const char *what = ev == MW_EV_FINE ? "kept on this driver, no more checks"
                     : ev == MW_EV_TRY_B ? "the second fix (route B: R11G11B10F without framebuffer compression) from the next start"
                     : ev == MW_EV_USE_B ? "route B (found fine before) from the next start" : ev == MW_EV_USE_A ? "route A from the next start"
                     : ev == MW_EV_OFFER_PANVK ? "neither fix took on this driver: route A from the next start, not checked again; the app offers the PanVK driver at its next start" : "unchanged";
    MWLOG("ladder: %s (A %s, B %s; state file %s%s)", what, mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]), g_mw.path[0] ? g_mw.path : "none: in memory only",
          g_mw.path[0] && !saved ? ", NOT written" : "");
    const char *key = ev == MW_EV_TRY_B || ev == MW_EV_USE_B ? "mali-try-b" : ev == MW_EV_OFFER_PANVK ? "mali-panvk" : NULL;
    if (key) { if (front_note) front_note(key); MWLOG("note over the game (%s): %s", front_note ? "shown" : "no screen here", !strcmp(key, "mali-try-b") ? MW_NOTE_TRY_B : MW_NOTE_PANVK); }
}
static void mw_judged(int k, mw_count c, uint64_t now_present) {
    g_mw.nsamples++;
    const int32_t *r = g_mw.slot[k].r;
    if (g_mw.judge.verdict) { MWLOG("sample %d (taken before the verdict): %u of %d texels saturated", g_mw.nsamples, c.sat, MW_TEX); return; }
    uint32_t s = g_mw.sim ? MW_TEX : c.sat; int kind = mw_sample_kind(&g_mw.rules, s, MW_TEX), v = mw_judge_add(&g_mw.judge, &g_mw.rules, s, MW_TEX);
    if (g_mw.nsamples <= 30 || v) MWLOG("sample %d, route %s, present %llu (read at %llu): %u of %d texels saturated (%.1f %%: %u infinite, %u NaN; largest finite %.6g) in the %dx%d part drawn of the %ux%u target%s: %s; runs white %d / %d, fine %d / %d",
        g_mw.nsamples, mw_route_name(g_mw.route), (unsigned long long)g_mw.slot[k].present, (unsigned long long)now_present, c.sat, MW_TEX, 100.0 * c.sat / MW_TEX, c.inf, c.nan, (double)c.mx,
        r[2], r[3], g_mw.slot[k].w, g_mw.slot[k].h, g_mw.sim ? " (counted as all white: DM_MALI_WHITE_SIM)" : "", kind == MW_WHITE ? "white" : kind == MW_FINE ? "fine" : "between",
        g_mw.judge.white_run, g_mw.rules.white_n, g_mw.judge.fine_run, g_mw.rules.fine_n);
    else {   // (past the first 30: one line a minute, so an undecided scene does not fill the log)
        g_mw.qn[kind]++; uint64_t t = mono_ns();
        if (!g_mw.quiet_ns) g_mw.quiet_ns = t;
        if (t - g_mw.quiet_ns >= 60000000000ull) { MWLOG("still undecided after %d samples: in the last minute %d white, %d fine, %d between; runs white %d / %d, fine %d / %d", g_mw.nsamples,
                                                       g_mw.qn[MW_WHITE], g_mw.qn[MW_FINE], g_mw.qn[MW_UNDECIDED], g_mw.judge.white_run, g_mw.rules.white_n, g_mw.judge.fine_run, g_mw.rules.fine_n);
                                                 g_mw.quiet_ns = t; memset(g_mw.qn, 0, sizeof g_mw.qn); } }
    if (v) mw_act(v);
}
static void mw_poll(uint64_t n) {   // the finished samples, oldest first (they finish in submission order); a slot still running ends the look
    for (;;) {
        int k = -1; for (int i = 0; i < MW_SLOTS; i++) if (g_mw.slot[i].busy && (k < 0 || g_mw.slot[i].seq < g_mw.slot[k].seq)) k = i;
        if (k < 0) return;
        VkResult fr = vkGetFenceStatus(g_dev, g_mw.slot[k].f);
        if (fr == VK_NOT_READY) return;
        vtex *x = g_mw.slot[k].x; g_mw.slot[k].busy = 0; g_mw.slot[k].x = NULL;
        if (fr != VK_SUCCESS) { MWLOG("[RED] a check's fence came back %d: the check stops", (int)fr); g_mw.sampling = 0; }
        else mw_judged(k, mw_read_slot(k), n);
        vtex_unref(x);
    }
}
// mtl_present: the finished samples read; then this frame's lit target sampled when it is time; the frame's candidates let go
static void mw_present(uint64_t n) {
    if (!g_mw.on) return;
    uint64_t fd = g_stat_draws - g_mw.frame_d0; g_mw.frame_d0 = g_stat_draws;
    mw_poll(n);
    mw_cand *b = NULL;   // the lit target: the candidate the most draws went into (the larger on a tie)
    for (int i = 0; i < g_mw.ncand; i++) { mw_cand *c = &g_mw.cand[i];
        if (!b || c->draws > b->draws || (c->draws == b->draws && (uint64_t)c->x->w * c->x->h > (uint64_t)b->x->w * b->x->h)) b = c; }
    if (g_mw.sampling && b && fd >= (uint64_t)g_mw.rules.min_draws) {
        uint64_t now = mono_ns();
        if (!g_mw.last_ns || now - g_mw.last_ns >= (uint64_t)g_mw.rules.every_ms * 1000000ull) {
            int32_t x0 = b->r[0] < 0 ? 0 : b->r[0], y0 = b->r[1] < 0 ? 0 : b->r[1], x1 = b->r[2] > (int32_t)b->x->w ? (int32_t)b->x->w : b->r[2], y1 = b->r[3] > (int32_t)b->x->h ? (int32_t)b->x->h : b->r[3];
            int32_t r[4] = { x0, y0, x1 - x0, y1 - y0 };
            // the drawn part at least a quarter of the narrowest lit target's width and a seventh of it high (the game's background scale, 0.001, draws one pixel)
            if (r[2] * 4 >= g_mw.rules.min_w && r[3] * 7 >= g_mw.rules.min_w) { if (mw_sample(b->x, r, n)) g_mw.last_ns = now; }
            else if (g_mw.nsmall++ < 3) MWLOG("the part drawn of the lit target is %dx%d (a low render scale): too small to judge, not sampled", r[2], r[3]);
        }
    }
    for (int i = 0; i < g_mw.ncand; i++) vtex_unref(g_mw.cand[i].x);
    g_mw.ncand = 0;
}
static void mw_capture_note(void) {   // cap_start: the ladder's state in the frame capture's notes (a bug report)
    if (!g_mw.on) return;
    cap_printf("Mali white-screen check: route %s (the ladder asked for %s), %s; %d samples so far, runs white %d / %d, fine %d / %d%s; results on this driver: A %s, B %s, PanVK offer %s.\n",
               mw_route_name(g_mw.route), mw_route_name(g_mw.want), g_mw.sampling ? "checking" : "not checking", g_mw.nsamples, g_mw.judge.white_run, g_mw.rules.white_n, g_mw.judge.fine_run,
               g_mw.rules.fine_n, g_mw.judge.verdict ? (g_mw.judge.verdict == MW_WHITE ? ", verdict WHITE" : ", verdict FINE") : "", mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]),
               g_mw.st.panvk == MW_PANVK_OFFER ? "pending" : g_mw.st.panvk == MW_PANVK_SHOWN ? "shown" : "none");
}

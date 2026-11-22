// ================= The Mali white-screen ladder and its detector (nvn_vk.c; the rules: mali_white.h, tested by dm/gfx/tests/t_mali_white.c) =================
// Arm's own Mali driver draws Sword / Shield's lit scene (an R11G11B10F target, 1280x720 or 1920x1080, hundreds of draws a frame) at the format's maximum: a white
//   picture (#47 #110 #111 #172 #214 #282); PanVK and Adreno draw it right. Two fixes: route A, R11G11B10F drawn as an RGBA16F stand-in (b10_on: the only change that
//   ever took the white away there, #282, Mali-G720), the default on Arm's own driver with Sword / Shield; route B, R11G11B10F kept without framebuffer compression
//   (g_b10nc). There are no Mali testers, so the game checks its own picture on each player's phone, and must not be fooled into keeping a white route:
//  - the lit target: in each frame, the R11G11B10F colour target (or its stand-in) at least DM_MALI_WHITE_MIN_W (1280) wide that the most draws went into, the part of
//    it they drew (the union of their viewports and scissors: Sword / Shield draw the scene into part of a full-size target at a lower render scale) and the cells of a
//    32 x 18 grid over it their rectangles touched;
//  - while playing, every DM_MALI_WHITE_EVERY_MS (3 s) at a present: a frame of DM_MALI_WHITE_MIN_DRAWS (200) draws or more, half that many of them into the lit target
//    itself, whose cells cover DM_MALI_WHITE_COVER_PCT (60 %) of the part drawn: a compute pass of its own (overlay/mali_white.comp: a 32 x 18 grid of texel fetches,
//    sampled image + storage buffer, Vulkan 1.0) submitted after the frame's work, ordered by a full barrier, into a host-visible buffer read once its fence has
//    signalled at a later present (never waited for): texels with a channel at 1024 or more, infinite or NaN (saturated), at 64 or more, near black, of 576;
//  - the verdict (mw_judge_add: white quickly, fine slowly; a fine one is re-checked once a minute for the rest of the start) moves the ladder (mw_ladder_verdict),
//    kept per phone in mali_ladder.txt in the app's files folder (DM_MALI_LADDER_FILE, set by android_main.c; else beside the pipeline cache): route A white → route B
//    from the next start, with a note over the game; B white too → route A again and the app offers PanVK at its next start (MaliLadder.java); a route found fine at 2
//    starts is kept, re-checked in the 3 starts after, then not checked (until the driver or the app changes). A start that checks a route leaves a marker in the file
//    until 20 s of play or its first sample read back: a start that finds it after a crash counts the route as closing the game (mw_marker_check). Every sample,
//    verdict and step is a "[mali-white]" line in the log; the state is in the frame capture's notes.
// Nothing of this runs on any other driver, nor for Legends: Arceus / Scarlet / Violet, unless DM_MALI_LADDER=1 or DM_MALI_WHITE_SIM=1 asks (mali_white.h)
#include "mali_white.h"
#include "overlay/mali_white_spv.h"
void front_note(const char *key) __attribute__((weak));   // android_main.c: a one-line note over the game by its key (MaliLadder.java shows it, translated)
#define MWLOG(...) VLOG("[mali-white] " __VA_ARGS__)
enum { MW_SLOTS = 3, MW_GX = 32, MW_GY = 18, MW_TEX = MW_GX * MW_GY, MW_WORDS = 2 * MW_TEX, MW_CAND = 4 };
typedef struct { vtex *x; uint64_t draws; int32_t r[4]; uint32_t cov[MW_GY]; } mw_cand;   // a lit-target candidate of the frame: its draws, the part they drew (x0, y0, x1, y1), the grid cells they touched
static struct {
    int on, check, sim, record;      // the ladder runs; how this start checks now (MW_CHECK_*); DM_MALI_WHITE_SIM; verdicts are kept (a route of the ladder's ran, no other white-screen test)
    int route, want, plan_check;     // MW_ROUTE_*: what this start runs, what the ladder (or the default) asked for; how the ladder's plan said to check
    int marker, rechecked;           // the start marker is up (mw_marker_set); a re-check start was counted
    char others[200];                // other white-screen tests set in env.txt (mw_other_tests)
    mw_rules rules; mw_judge judge; mw_state st; char path[1100];
    uint64_t last_ns, frame_d0, pd0, mark_ns; int32_t pr[4]; uint32_t pcov[MW_GY];   // the last sample's time; draws at the frame's / the open pass's start; the marker's first present; the open pass's part and cells
    int nsamples, nskip[4]; uint64_t quiet_ns; int qn[4];   // samples read; frames not sampled by why (the first 3 of each logged); past the first 30 samples one line a minute (qn: its kinds)
    mw_cand cand[MW_CAND]; int ncand;
    int gpu;                         // the compute pass: 0 not made yet, 1 ready, -1 failed
    VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline cs; VkDescriptorPool dp; VkCommandPool cp; VkBuffer buf; vmalloc_t ba; const uint32_t *map;
    struct { VkCommandBuffer cb; VkFence f; VkDescriptorSet ds; vtex *x; int busy, cover; uint64_t seq, present, draws, fdraws; int32_t r[4]; uint32_t w, h; int64_t t_ms; } slot[MW_SLOTS]; uint64_t seq;
} g_mw;
// the notes over the game (MaliLadder.java's texts, by key)
static const char *const MW_NOTE_TRY_B = "Mali: the white-screen fix did not take; trying the second fix at the next start";
static const char *const MW_NOTE_RETRY = "Mali: the white-screen fix did not take; the next start tries the other fix again";
static const char *const MW_NOTE_PANVK = "Mali: neither white-screen fix took; the next start offers the PanVK driver";
static const char *const MW_NOTE_ENV = "Mali: a test switch in the settings overrides the white-screen fix";

static void mw_path(char *out, size_t n) {   // the state file: DM_MALI_LADDER_FILE (the app's files folder), else beside the pipeline cache, else none (in memory only)
    const char *f = getenv("DM_MALI_LADDER_FILE"), *pc = getenv("DM_VK_PCACHE"); const char *sl = pc ? strrchr(pc, '/') : NULL;
    if (f && *f) snprintf(out, n, "%s", f);
    else if (sl) snprintf(out, n, "%.*s/mali_ladder.txt", (int)(sl - pc), pc);
    else out[0] = 0;
}
static int mw_save(void) {   // the state into its file (1: written)
    if (!g_mw.path[0]) return 0;
    if (mw_state_save(g_mw.path, &g_mw.st, &g_mw.rules)) return 1;
    MWLOG("[RED] the state file %s could not be written (errno %d)", g_mw.path, errno);
    return 0;
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
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
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
static const char *mw_check_name(int c) { return c == MW_CHECK_FULL ? "checked while playing" : c == MW_CHECK_RE ? "re-checked once a minute" : "not checked"; }
// mtl_init, before any image is made: whether the ladder runs, the route of this start (*a_on: the RGBA16F stand-in, *b_on: no compression; *why_a / *why_b: who asked)
static void mw_init(int *a_on, int *b_on, const char **why_a, const char **why_b) {
    int arm = g_arm_rel != 0; const char *le = getenv("DM_MALI_LADDER"), *se = getenv("DM_MALI_WHITE_SIM"), *ea = getenv("DM_VK_B10_AS_RGBA16"), *eb = getenv("DM_VK_B10_NOCOMPRESS");
    int en = mw_enabled(arm, DM_PORT_GALAR, le, se), reset = 0;
    g_mw.rules = mw_rules_default(); mw_rules_env(&g_mw.rules);
    g_mw.want = mw_default_route(arm, DM_PORT_GALAR);
    static mw_state was;   // (static: the state is a few hundred bytes; mtl_init runs once)
    if (en) {
        g_mw.on = 1; g_mw.sim = se && *se && atoi(se) > 0;
        mw_path(g_mw.path, sizeof g_mw.path);
        const char *av = getenv("DM_APP_VERSION"); long app = av && *av ? atol(av) : -1;
        int have = mw_state_load(g_mw.path, &g_mw.st); was = g_mw.st;
        mw_plan p = mw_ladder_start(&g_mw.st, &g_mw.rules, have, g_drv_id, g_props.driverVersion, app); reset = p.reset;
        if (p.reset) MWLOG("ladder started over: %s", !have ? (g_mw.path[0] ? "no state file yet" : "no state file (DM_MALI_LADDER_FILE / DM_VK_PCACHE unset): this start's results stay in memory")
                                                           : "another driver or app version than the state file's");
        // the marker an earlier start left on the route it checked: closed before 20 s of play or its first sample (mali_white.h mw_marker_check)
        const char *lc = getenv("DM_LAST_CRASH_DEVICE"), *lg = getenv("DM_LAST_CRASH_GAME"); int crashed = lc && atoi(lc) > 0 && !(lg && atoi(lg) > 0), mr = 0;
        int mk = mw_marker_check(&g_mw.st, &g_mw.rules, crashed, &mr);
        if (mk == MW_MK_EARLY) MWLOG("the last start on route %s ended before 20 s of play or a sample, without a crash (closed, or ended by the system): route %s again; a second such start in a row counts as the route closing the game",
                                     mw_route_name(mr), mw_route_name(mr));
        else if (mk == MW_MK_CRASHED) MWLOG("route %s closed the game at an earlier start (%s): counted as failed (%d time%s on this driver); results now A %s, B %s", mw_route_name(mr),
                                            crashed ? "it crashed after the graphics device came up, before 20 s of play or a sample" : "two starts in a row on it ended before 20 s of play or a sample",
                                            g_mw.st.crashes[mr], g_mw.st.crashes[mr] == 1 ? "" : "s", mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]));
        else if (mk == MW_MK_BOTH) MWLOG("route %s closed the game at an earlier start, and route %s had before it: the crashes are not the routes' (both are checked again)", mw_route_name(mr),
                                         mw_route_name(mr == MW_ROUTE_A ? MW_ROUTE_B : MW_ROUTE_A));
        p = mw_plan_of(&g_mw.st, &g_mw.rules);
        if (p.exhausted && g_mw.st.panvk == MW_PANVK_NONE) { g_mw.st.panvk = MW_PANVK_OFFER; p.offer = 1; }   // (exhausted by a crash just counted)
        snprintf(g_mw.st.gpu, sizeof g_mw.st.gpu, "%s", g_props.deviceName);   // (MaliLadder.java's PanVK offer names it and picks the driver for it)
        g_mw.want = p.route; g_mw.plan_check = p.check;
        char rel[32] = "not Arm's own driver"; if (arm) snprintf(rel, sizeof rel, "Arm's r%dp%d", g_arm_rel, g_arm_p);
        char how[160]; int rt = p.route == MW_ROUTE_A || p.route == MW_ROUTE_B ? p.route : 0;
        if (p.exhausted) snprintf(how, sizeof how, "not checked (both fixes failed on this driver)");
        else if (p.check == MW_CHECK_RE) snprintf(how, sizeof how, "re-checked once a minute (kept as fine on this driver; re-check start %d of %d)", g_mw.st.rechecks[rt] + 1, g_mw.rules.recheck_starts);
        else if (p.check == MW_CHECK_NONE) snprintf(how, sizeof how, "not checked (kept as fine on this driver, re-checked in %d starts)", g_mw.rules.recheck_starts);
        else if (p.retry) snprintf(how, sizeof how, "checked while playing (once more: it closed the game once, and the other route was white)");
        else snprintf(how, sizeof how, "checked while playing (fine at %d of the %d starts it needs so far)", rt ? g_mw.st.fine_starts[rt] : 0, g_mw.rules.fine_starts);
        MWLOG("on (%s): %s, driver %d version %#x (%s), app %ld; results so far A %s, B %s (crashes A %d, B %d), PanVK offer %s; this start: route %s, %s",
              en == 1 ? "Arm's own Mali driver with Sword / Shield" : le && *le && atoi(le) > 0 ? "DM_MALI_LADDER=1, a test" : "DM_MALI_WHITE_SIM=1: every sample counts as white",
              g_props.deviceName, g_drv_id, g_props.driverVersion, rel, app, mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]), g_mw.st.crashes[MW_ROUTE_A],
              g_mw.st.crashes[MW_ROUTE_B], g_mw.st.panvk == MW_PANVK_OFFER ? "pending (the app shows it at its start)" : g_mw.st.panvk == MW_PANVK_SHOWN ? "shown" : "none",
              mw_route_name(p.route), how);
    }
    g_mw.route = mw_route_apply(g_mw.want, ea, eb, a_on, b_on);
    int others = en ? mw_other_tests(g_mw.others, sizeof g_mw.others) : 0;
    g_mw.record = (g_mw.route == MW_ROUTE_A || g_mw.route == MW_ROUTE_B || g_mw.route == MW_ROUTE_BOTH) && !others;
    if (en && g_mw.route != g_mw.want)
        MWLOG("env.txt sets%s%s: this start runs %s, not route %s%s", ea && *ea ? " DM_VK_B10_AS_RGBA16" : "", eb && *eb ? " DM_VK_B10_NOCOMPRESS" : "", mw_route_name(g_mw.route),
              mw_route_name(g_mw.want), !g_mw.record ? " (its verdict is logged, not kept)" : g_mw.route == MW_ROUTE_BOTH ? " (white counts for both routes, fine for neither)" : " (its verdict is kept for that route)");
    if (en && others) MWLOG("env.txt also sets other white-screen tests (%s): they change the shaders or the lit target beside the route, so this start's verdict is logged, not kept", g_mw.others);
    if (en) {
        g_mw.check = g_mw.plan_check;
        // the start marker: this start checks one of the ladder's routes, as the ladder asked; it goes after 20 s of play or the first sample read back
        int mk = g_mw.record && g_mw.route == g_mw.want && g_mw.plan_check == MW_CHECK_FULL && (g_mw.route == MW_ROUTE_A || g_mw.route == MW_ROUTE_B) ? g_mw.route : 0;
        mw_marker_set(&g_mw.st, mk); g_mw.marker = mk != 0;
        if ((reset || memcmp(&was, &g_mw.st, sizeof was)) && g_mw.path[0]) mw_save();
        if (mk) MWLOG("start marker: route %s in the state file until 20 s of play or its first sample read back (a start that finds it after a crash counts the route as closing the game)", mw_route_name(mk));
        if (g_mw.check) MWLOG("rules: saturated = a channel at %d or more, infinite or NaN (the log also counts %d or more); white after %d samples in a row with %d %% or more saturated, or when "
                              "the last %d samples with content are %d %% saturated on average; fine at this start after %d fine samples (under %d %%) over %d s of play with none white or "
                              "partial between, kept after %d such starts and re-checked in %d starts after; a sample %d %% near black is empty (not judged); one sample every %d ms (%d ms "
                              "after fine), at most %d without a verdict; sampled frames: %d+ draws, %d+ of them into an R11G11B10F target %d+ wide, covering %d %% of the part drawn "
                              "(state file %s)", g_mw.rules.level, g_mw.rules.level_low, g_mw.rules.white_n, g_mw.rules.white_pct, g_mw.rules.slow_n, g_mw.rules.slow_pct, g_mw.rules.fine_n,
                              g_mw.rules.fine_pct, g_mw.rules.fine_span_ms / 1000, g_mw.rules.fine_starts, g_mw.rules.recheck_starts, g_mw.rules.dark_pct, g_mw.rules.every_ms,
                              g_mw.rules.recheck_ms, g_mw.rules.cap_n, g_mw.rules.min_draws, g_mw.rules.lit_draws, g_mw.rules.min_w, g_mw.rules.cover_pct, g_mw.path[0] ? g_mw.path : "none");
    }
    const char *dflt = en ? "the white-screen ladder's route" : !g_mw.want ? "" : le && *le && !atoi(le) ? "the white-screen fix on Arm's own Mali driver with Sword / Shield; DM_MALI_LADDER=0: no check"
                                                                                                            : "the white-screen fix on Arm's own Mali driver with Sword / Shield";
    *why_a = ea && *ea ? "DM_VK_B10_AS_RGBA16, a Mali test" : dflt; *why_b = eb && *eb ? "DM_VK_B10_NOCOMPRESS, a Mali test" : dflt;
    if (g_mw.check && !mw_gpu_init()) g_mw.check = MW_CHECK_NONE;
}
// pass_begin: a colour target in R11G11B10F or its stand-in (lv: the attachment's level); the pass's largest one is its lit-target candidate
static void mw_pass_target(vtex *x, uint32_t lv) {
    if (!g_mw.check || P_tf || lv || !x || x->samples != 1 || x->is3d || !(x->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || x->w < (uint32_t)g_mw.rules.min_w) return;
    if (g_mw_px && (uint64_t)g_mw_px->w * g_mw_px->h >= (uint64_t)x->w * x->h) return;
    g_mw_px = x; g_mw.pd0 = g_stat_draws; g_mw.pr[0] = g_mw.pr[1] = INT32_MAX; g_mw.pr[2] = g_mw.pr[3] = INT32_MIN; memset(g_mw.pcov, 0, sizeof g_mw.pcov);
}
// draw_, in a pass with a candidate: what the draw covers (its viewport within its scissor, which is within the pass), into the pass's drawn part and its grid cells
static void mw_draw_rect(float vx, float vy, float vw, float vh, const VkRect2D *sc) {
    float x0 = vw < 0 ? vx + vw : vx, y0 = vh < 0 ? vy + vh : vy, x1 = x0 + fabsf(vw), y1 = y0 + fabsf(vh);
    float sx0 = (float)sc->offset.x, sy0 = (float)sc->offset.y, sx1 = sx0 + (float)sc->extent.width, sy1 = sy0 + (float)sc->extent.height;
    int32_t a = (int32_t)fmaxf(x0, sx0), b = (int32_t)fmaxf(y0, sy0), c = (int32_t)fminf(x1, sx1), d = (int32_t)fminf(y1, sy1);   // (a NaN viewport: the scissor's)
    if (c <= a || d <= b) return;
    if (a < g_mw.pr[0]) g_mw.pr[0] = a; if (b < g_mw.pr[1]) g_mw.pr[1] = b; if (c > g_mw.pr[2]) g_mw.pr[2] = c; if (d > g_mw.pr[3]) g_mw.pr[3] = d;
    uint32_t W = g_mw_px->w, H = g_mw_px->h; if (a < 0) a = 0; if (b < 0) b = 0; if ((uint32_t)c > W) c = (int32_t)W; if ((uint32_t)d > H) d = (int32_t)H; if (c <= a || d <= b) return;
    uint32_t cx0 = (uint32_t)a * MW_GX / W, cx1 = (uint32_t)(c - 1) * MW_GX / W, cy0 = (uint32_t)b * MW_GY / H, cy1 = (uint32_t)(d - 1) * MW_GY / H;
    uint32_t m = (cx1 >= 31 ? ~0u : (2u << cx1) - 1) & ~((1u << cx0) - 1);
    for (uint32_t y = cy0; y <= cy1 && y < MW_GY; y++) g_mw.pcov[y] |= m;
}
// pass_end of a pass with a candidate: its draws, drawn part and cells go to the frame's candidate for that image (held by a reference until the present)
static void mw_pass_end(void) {
    vtex *x = g_mw_px; g_mw_px = NULL;
    uint64_t d = g_stat_draws - g_mw.pd0; if (!d || g_mw.pr[2] <= g_mw.pr[0] || g_mw.pr[3] <= g_mw.pr[1]) return;
    mw_cand *c = NULL; for (int i = 0; i < g_mw.ncand; i++) if (g_mw.cand[i].x == x) c = &g_mw.cand[i];
    if (!c) {
        if (g_mw.ncand == MW_CAND) { int k = 0; for (int i = 1; i < MW_CAND; i++) if (g_mw.cand[i].draws < g_mw.cand[k].draws) k = i;   // (full: the one with the fewest draws goes)
            if (g_mw.cand[k].draws >= d) return; vtex_unref(g_mw.cand[k].x); c = &g_mw.cand[k]; }
        else c = &g_mw.cand[g_mw.ncand++];
        vtex_ref(x); c->x = x; c->draws = 0; c->r[0] = c->r[1] = INT32_MAX; c->r[2] = c->r[3] = INT32_MIN; memset(c->cov, 0, sizeof c->cov); }
    c->draws += d;
    if (g_mw.pr[0] < c->r[0]) c->r[0] = g_mw.pr[0]; if (g_mw.pr[1] < c->r[1]) c->r[1] = g_mw.pr[1]; if (g_mw.pr[2] > c->r[2]) c->r[2] = g_mw.pr[2]; if (g_mw.pr[3] > c->r[3]) c->r[3] = g_mw.pr[3];
    for (int y = 0; y < MW_GY; y++) c->cov[y] |= g_mw.pcov[y];
}
// how much of the part r (x0, y0, width, height) of candidate c's image its draws' rectangles touched: the share of the grid cells r spans, in %
static int mw_cover(const mw_cand *c, const int32_t r[4]) {
    uint32_t W = c->x->w, H = c->x->h; if (r[2] <= 0 || r[3] <= 0) return 0;
    uint32_t cx0 = (uint32_t)r[0] * MW_GX / W, cx1 = (uint32_t)(r[0] + r[2] - 1) * MW_GX / W, cy0 = (uint32_t)r[1] * MW_GY / H, cy1 = (uint32_t)(r[1] + r[3] - 1) * MW_GY / H;
    if (cx1 >= MW_GX) cx1 = MW_GX - 1; if (cy1 >= MW_GY) cy1 = MW_GY - 1;
    uint32_t m = (cx1 >= 31 ? ~0u : (2u << cx1) - 1) & ~((1u << cx0) - 1); int on = 0, all = (int)((cx1 - cx0 + 1) * (cy1 - cy0 + 1));
    for (uint32_t y = cy0; y <= cy1; y++) on += __builtin_popcount(c->cov[y] & m);
    return all ? on * 100 / all : 0;
}
// one sample of image x's part r (x0, y0, width, height): recorded and submitted after everything submitted so far (a full barrier first), read at a later present
//   (mw_poll); 0 when it could not be (no free slot, no view, the queue refused it). draws / fd / cover: the lit target's draws, the frame's, the part covered (the log)
static int mw_sample(vtex *x, const int32_t r[4], uint64_t present, uint64_t draws, uint64_t fd, int cover) {
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
    uint32_t pc[8] = { (uint32_t)r[0], (uint32_t)r[1], (uint32_t)r[2], (uint32_t)r[3], (uint32_t)(k * MW_WORDS), mw_float_bits((float)g_mw.rules.level),
                       mw_float_bits((float)g_mw.rules.level_low), MW_DARK_BITS };
    vkCmdPushConstants(cb, g_mw.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, MW_GY, 1);
    barrier_host(cb);
    if (vkEndCommandBuffer(cb)) return 0;
    vkResetFences(g_dev, 1, &g_mw.slot[k].f);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    pthread_mutex_lock(&g_qmu); VkResult sr = vkQueueSubmit(g_q, 1, &si, g_mw.slot[k].f); pthread_mutex_unlock(&g_qmu);
    if (sr) { MWLOG("[RED] the check's submission failed (%d): the check stops", (int)sr); g_mw.check = MW_CHECK_NONE; return 0; }
    vtex_ref(x); g_mw.slot[k].x = x; g_mw.slot[k].busy = 1; g_mw.slot[k].seq = ++g_mw.seq; g_mw.slot[k].present = present;
    memcpy(g_mw.slot[k].r, r, sizeof g_mw.slot[k].r); g_mw.slot[k].w = x->w; g_mw.slot[k].h = x->h;
    g_mw.slot[k].draws = draws; g_mw.slot[k].fdraws = fd; g_mw.slot[k].cover = cover; g_mw.slot[k].t_ms = (int64_t)(mono_ns() / 1000000ull);
    return 1;
}
// a finished slot's texels: how many are saturated (a channel at the level or more, infinite or NaN), at the lower bucket or more (or saturated), near black, infinite,
//   NaN, and the largest finite channel
typedef struct { uint32_t sat, low, dark, inf, nan; float mx; } mw_count;
static mw_count mw_read_slot(int k) {
    mw_count c = { 0, 0, 0, 0, 0, 0 }; const uint32_t *w = g_mw.map + (size_t)k * MW_WORDS; uint32_t mx = 0;
    for (int t = 0; t < MW_TEX; t++) { uint32_t f = w[2 * t]; c.sat += (f & 7) != 0; c.low += (f & 15) != 0; c.dark += (f & 16) != 0; c.inf += (f & 2) != 0; c.nan += (f & 4) != 0;
        if (w[2 * t + 1] > mx) mx = w[2 * t + 1]; }
    memcpy(&c.mx, &mx, 4);
    return c;
}
static double mw_win_mean(void) { return g_mw.judge.win_n ? g_mw.judge.win_sum / 100.0 / g_mw.judge.win_n : 0.0; }   // (the slow rule's window mean, %)
// a verdict (the judge's verdict changed): FINE at this start → the ladder counts it (kept after fine_starts starts) and the check goes on once a minute; WHITE → the
//   ladder moves (when this start runs one of its routes, no other test), the state file is written, a note goes over the game, the check stops
static void mw_act(int v) {
    const mw_judge *j = &g_mw.judge;
    if (v == MW_FINE) {
        g_mw.check = MW_CHECK_RE;   // (one sample a minute for the rest of this start: the verdict can still turn white)
        MWLOG("verdict after %d samples: route %s is FINE at this start (%d fine samples over %.0f s of play, none white or partial between); checked once a minute for the rest of this start",
              j->samples, mw_route_name(g_mw.route), j->fine_n, j->fine_ms / 1000.0);
        if (!g_mw.record || g_mw.route == MW_ROUTE_BOTH) { MWLOG("this start runs %s%s: fine is not kept", mw_route_name(g_mw.route), !g_mw.record ? " with other tests or not one of the ladder's routes" : " (both fixes at once)"); return; }
        int ev = mw_ladder_verdict(&g_mw.st, &g_mw.rules, g_mw.route, MW_FINE), saved = mw_save();
        if (ev == MW_EV_FINE_ONCE) MWLOG("ladder: route %s fine at %d of the %d starts it needs: the next start checks it again (state file %s%s)", mw_route_name(g_mw.route), g_mw.st.fine_starts[g_mw.route],
                                         g_mw.rules.fine_starts, g_mw.path[0] ? g_mw.path : "none: in memory only", g_mw.path[0] && !saved ? ", NOT written" : "");
        else if (ev == MW_EV_FINE) MWLOG("ladder: route %s kept as fine on this driver; re-checked once a minute in the next %d starts, then not checked (state file %s%s)", mw_route_name(g_mw.route),
                                         g_mw.rules.recheck_starts, g_mw.path[0] ? g_mw.path : "none: in memory only", g_mw.path[0] && !saved ? ", NOT written" : "");
        else MWLOG("ladder: route %s was kept as fine already (a re-check start): unchanged", mw_route_name(g_mw.route));
        return;
    }
    g_mw.check = MW_CHECK_NONE;
    MWLOG("verdict after %d samples: route %s is WHITE (%s)", j->samples, mw_route_name(g_mw.route), j->rule == MW_RULE_SLOW ? "the last samples with content saturated on average past the slow rule's share"
          : "white samples in a row");
    int ev = MW_EV_NONE;
    if (g_mw.record) {
        ev = mw_ladder_verdict(&g_mw.st, &g_mw.rules, g_mw.route, MW_WHITE); int saved = mw_save();
        const char *what = ev == MW_EV_TRY_B ? "the second fix (route B: R11G11B10F without framebuffer compression) from the next start, checked"
                         : ev == MW_EV_USE_B ? "route B (found fine before) from the next start" : ev == MW_EV_USE_A ? "route A from the next start"
                         : ev == MW_EV_RETRY ? "the route that closed the game once, once more from the next start (the other one is white)"
                         : ev == MW_EV_OFFER_PANVK ? "neither fix took on this driver: route A from the next start, not checked again; the app offers the PanVK driver at its next start"
                         : ev == MW_EV_EXHAUSTED ? "neither fix took on this driver (the PanVK offer was made before)" : "unchanged";
        MWLOG("ladder: %s (A %s, B %s; state file %s%s)", what, mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]), g_mw.path[0] ? g_mw.path : "none: in memory only",
              g_mw.path[0] && !saved ? ", NOT written" : "");
    } else MWLOG("this start runs %s%s: the verdict is not kept; no more checks this start", mw_route_name(g_mw.route), g_mw.others[0] ? " with other white-screen tests set in env.txt" : " (set in env.txt), not one of the ladder's routes");
    // the note: what the next start does, never a fix the settings hold back (a route or test set in env.txt wins over the ladder)
    const char *key = ev == MW_EV_OFFER_PANVK ? "mali-panvk" : !g_mw.record || g_mw.route != g_mw.want ? "mali-env" : ev == MW_EV_TRY_B || ev == MW_EV_USE_B ? "mali-try-b"
                    : ev == MW_EV_RETRY ? "mali-retry" : NULL;
    if (key) { if (front_note) front_note(key); MWLOG("note over the game (%s): %s", front_note ? "shown" : "no screen here", !strcmp(key, "mali-try-b") ? MW_NOTE_TRY_B
                                                     : !strcmp(key, "mali-retry") ? MW_NOTE_RETRY : !strcmp(key, "mali-panvk") ? MW_NOTE_PANVK : MW_NOTE_ENV); }
}
static void mw_judged(int k, mw_count c, uint64_t now_present) {
    g_mw.nsamples++;
    if (g_mw.marker) { g_mw.marker = 0; mw_marker_clear(&g_mw.st); mw_save(); MWLOG("route %s: the first sample came back (start marker cleared)", mw_route_name(g_mw.route)); }
    const int32_t *r = g_mw.slot[k].r;
    if (g_mw.judge.verdict == MW_WHITE || !g_mw.check) { MWLOG("sample %d (taken before the check stopped): %u of %d texels saturated, %u near black", g_mw.nsamples, c.sat, MW_TEX, c.dark); return; }
    uint32_t s = g_mw.sim ? MW_TEX : c.sat, dk = g_mw.sim ? 0 : c.dark;
    int kind = mw_sample_kind(&g_mw.rules, s, dk, MW_TEX), prev = g_mw.judge.verdict, v = mw_judge_add(&g_mw.judge, &g_mw.rules, s, dk, MW_TEX, g_mw.slot[k].t_ms);
    if (g_mw.nsamples <= 30 || v != prev)
        MWLOG("sample %d, route %s%s, present %llu (read at %llu): %u of %d texels saturated (%.1f %%: %u infinite, %u NaN), %u at %d or more, %u near black; largest finite %.6g; the %dx%d "
              "part drawn of the %ux%u target, %llu lit draws of %llu in the frame, %d %% of the part covered%s: %s; runs white %d / %d, fine %d / %d over %.0f / %d s; last %d with content "
              "%.1f %% saturated on average", g_mw.nsamples, mw_route_name(g_mw.route), g_mw.check == MW_CHECK_RE ? " (re-check)" : "", (unsigned long long)g_mw.slot[k].present,
              (unsigned long long)now_present, c.sat, MW_TEX, 100.0 * c.sat / MW_TEX, c.inf, c.nan, c.low, g_mw.rules.level_low, c.dark, (double)c.mx, r[2], r[3], g_mw.slot[k].w, g_mw.slot[k].h,
              (unsigned long long)g_mw.slot[k].draws, (unsigned long long)g_mw.slot[k].fdraws, g_mw.slot[k].cover, g_mw.sim ? " (counted as all white: DM_MALI_WHITE_SIM)" : "",
              mw_kind_name(kind), g_mw.judge.white_run, g_mw.rules.white_n, g_mw.judge.fine_n, g_mw.rules.fine_n, g_mw.judge.fine_ms / 1000.0, g_mw.rules.fine_span_ms / 1000,
              g_mw.judge.win_n, mw_win_mean());
    else {   // (past the first 30: one line a minute, so a long check does not fill the log)
        g_mw.qn[kind]++; uint64_t t = mono_ns();
        if (!g_mw.quiet_ns) g_mw.quiet_ns = t;
        if (t - g_mw.quiet_ns >= 60000000000ull) { MWLOG("%s after %d samples: in the last minute %d white, %d fine, %d between, %d empty; runs white %d / %d, fine %d / %d over %.0f / %d s; last %d with content %.1f %% saturated on average",
                                                       g_mw.judge.verdict == MW_FINE ? "still fine" : "still undecided", g_mw.nsamples, g_mw.qn[MW_K_WHITE], g_mw.qn[MW_K_FINE], g_mw.qn[MW_K_BETWEEN],
                                                       g_mw.qn[MW_K_EMPTY], g_mw.judge.white_run, g_mw.rules.white_n, g_mw.judge.fine_n, g_mw.rules.fine_n, g_mw.judge.fine_ms / 1000.0,
                                                       g_mw.rules.fine_span_ms / 1000, g_mw.judge.win_n, mw_win_mean());
                                                 g_mw.quiet_ns = t; memset(g_mw.qn, 0, sizeof g_mw.qn); } }
    // a start that re-checks a route kept as fine: counted at its first sample (the ladder's own route, nothing else set)
    if (g_mw.plan_check == MW_CHECK_RE && !g_mw.rechecked && g_mw.record && g_mw.route == g_mw.want && (g_mw.route == MW_ROUTE_A || g_mw.route == MW_ROUTE_B)) {
        g_mw.rechecked = 1; g_mw.st.rechecks[g_mw.route]++; mw_save();
        MWLOG("re-check start %d of %d on route %s", g_mw.st.rechecks[g_mw.route], g_mw.rules.recheck_starts, mw_route_name(g_mw.route)); }
    if (v != prev) mw_act(v);
    else if (g_mw.check == MW_CHECK_FULL && mw_judge_capped(&g_mw.judge, &g_mw.rules)) {
        g_mw.check = MW_CHECK_NONE;
        MWLOG("no verdict after %d samples (%d white, %d fine, %d between, %d empty: near black; the last %d with content %.1f %% saturated on average): the check stops for this start, the next "
              "start checks again", g_mw.judge.samples, g_mw.judge.kinds[MW_K_WHITE], g_mw.judge.kinds[MW_K_FINE], g_mw.judge.kinds[MW_K_BETWEEN], g_mw.judge.kinds[MW_K_EMPTY],
              g_mw.judge.win_n, mw_win_mean());
        if (g_mw.judge.kinds[MW_K_EMPTY] * 2 > g_mw.judge.samples) MWLOG("[RED] the lit target was near black in %d of %d samples: it may not be drawn at all on this route", g_mw.judge.kinds[MW_K_EMPTY], g_mw.judge.samples);
    }
}
static void mw_poll(uint64_t n) {   // the finished samples, oldest first (they finish in submission order); a slot still running ends the look
    for (;;) {
        int k = -1; for (int i = 0; i < MW_SLOTS; i++) if (g_mw.slot[i].busy && (k < 0 || g_mw.slot[i].seq < g_mw.slot[k].seq)) k = i;
        if (k < 0) return;
        VkResult fr = vkGetFenceStatus(g_dev, g_mw.slot[k].f);
        if (fr == VK_NOT_READY) return;
        vtex *x = g_mw.slot[k].x; g_mw.slot[k].busy = 0; g_mw.slot[k].x = NULL;
        if (fr != VK_SUCCESS) { MWLOG("[RED] a check's fence came back %d: the check stops", (int)fr); g_mw.check = MW_CHECK_NONE; }
        else mw_judged(k, mw_read_slot(k), n);
        vtex_unref(x);
    }
}
// the start marker goes after 20 s of play on the route: 600 presents, or 20 s of wall time and 60 presents (as the heap route's marker)
static void mw_marker_tick(uint64_t n) {
    if (!g_mw.marker) return;
    uint64_t t = mono_ns(); if (!g_mw.mark_ns) g_mw.mark_ns = t;
    if (n >= 600 || (n >= 60 && t - g_mw.mark_ns >= 20000000000ull)) { g_mw.marker = 0; mw_marker_clear(&g_mw.st); mw_save();
        MWLOG("route %s: 20 s of play (%llu presents): start marker cleared", mw_route_name(g_mw.route), (unsigned long long)n); }
}
// mtl_present: the finished samples read; then this frame's lit target sampled when it is time; the frame's candidates let go
static void mw_present(uint64_t n) {
    if (!g_mw.on) return;
    uint64_t fd = g_stat_draws - g_mw.frame_d0; g_mw.frame_d0 = g_stat_draws;
    mw_poll(n);
    mw_marker_tick(n);
    mw_cand *b = NULL;   // the lit target: the candidate the most draws went into (the larger on a tie)
    for (int i = 0; i < g_mw.ncand; i++) { mw_cand *c = &g_mw.cand[i];
        if (!b || c->draws > b->draws || (c->draws == b->draws && (uint64_t)c->x->w * c->x->h > (uint64_t)b->x->w * b->x->h)) b = c; }
    if (g_mw.check && b) {
        uint64_t now = mono_ns(), iv = (uint64_t)(g_mw.check == MW_CHECK_RE ? g_mw.rules.recheck_ms : g_mw.rules.every_ms) * 1000000ull;
        if (!g_mw.last_ns || now - g_mw.last_ns >= iv) {
            int32_t x0 = b->r[0] < 0 ? 0 : b->r[0], y0 = b->r[1] < 0 ? 0 : b->r[1], x1 = b->r[2] > (int32_t)b->x->w ? (int32_t)b->x->w : b->r[2], y1 = b->r[3] > (int32_t)b->x->h ? (int32_t)b->x->h : b->r[3];
            int32_t r[4] = { x0, y0, x1 - x0, y1 - y0 };
            int cover = mw_cover(b, r);
            // a frame of play: enough draws in the frame and into the lit target itself (a menu with a 3D model, a load, a fade draw far fewer), the drawn part at least
            //   a quarter of the narrowest lit target's width and a seventh of it high (the game's background scale, 0.001, draws one pixel), and its cells mostly drawn
            int why = fd < (uint64_t)g_mw.rules.min_draws ? 0 : b->draws < (uint64_t)g_mw.rules.lit_draws ? 1 : r[2] * 4 < g_mw.rules.min_w || r[3] * 7 < g_mw.rules.min_w ? 2
                    : cover < g_mw.rules.cover_pct ? 3 : -1;
            if (why < 0) { if (mw_sample(b->x, r, n, b->draws, fd, cover)) g_mw.last_ns = now; }
            else if (g_mw.nskip[why]++ < 3) {
                if (why == 0) MWLOG("not sampled: %llu draws in the frame (%d needed: a load, a menu or a fade)", (unsigned long long)fd, g_mw.rules.min_draws);
                else if (why == 1) MWLOG("not sampled: %llu of the frame's %llu draws into the lit target (%d needed)", (unsigned long long)b->draws, (unsigned long long)fd, g_mw.rules.lit_draws);
                else if (why == 2) MWLOG("not sampled: the part drawn of the lit target is %dx%d (a low render scale): too small to judge", r[2], r[3]);
                else MWLOG("not sampled: the lit target's %llu draws cover %d %% of the %dx%d part they span (%d %% needed)", (unsigned long long)b->draws, cover, r[2], r[3], g_mw.rules.cover_pct);
            }
        }
    }
    for (int i = 0; i < g_mw.ncand; i++) vtex_unref(g_mw.cand[i].x);
    g_mw.ncand = 0;
}
static void mw_capture_note(void) {   // cap_start: the ladder's state in the frame capture's notes (a bug report)
    if (!g_mw.on) return;
    cap_printf("Mali white-screen check: route %s (the ladder asked for %s), %s; %d samples so far (white %d, fine %d, between %d, empty %d), runs white %d / %d, fine %d / %d%s; results on this "
               "driver: A %s, B %s (crashes A %d, B %d); PanVK offer %s.\n", mw_route_name(g_mw.route), mw_route_name(g_mw.want), mw_check_name(g_mw.check), g_mw.nsamples,
               g_mw.judge.kinds[MW_K_WHITE], g_mw.judge.kinds[MW_K_FINE], g_mw.judge.kinds[MW_K_BETWEEN], g_mw.judge.kinds[MW_K_EMPTY], g_mw.judge.white_run, g_mw.rules.white_n,
               g_mw.judge.fine_n, g_mw.rules.fine_n, g_mw.judge.verdict ? (g_mw.judge.verdict == MW_WHITE ? ", verdict WHITE" : ", verdict FINE at this start") : "",
               mw_res_name(g_mw.st.res[MW_ROUTE_A]), mw_res_name(g_mw.st.res[MW_ROUTE_B]), g_mw.st.crashes[MW_ROUTE_A], g_mw.st.crashes[MW_ROUTE_B],
               g_mw.st.panvk == MW_PANVK_OFFER ? "pending" : g_mw.st.panvk == MW_PANVK_SHOWN ? "shown" : "none");
}

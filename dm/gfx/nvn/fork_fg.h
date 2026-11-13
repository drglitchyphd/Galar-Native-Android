// FORK: frame generation (Lossless Scaling's LSFG 3.1 through libforklsfg.so, dm/fork/lsfg). Included by nvn_vk.c just above present_window_locked.
// Settings (env.txt, the in-game menu's Performance page; all off by default):
//   DM_FORK_FG=2x          one generated frame between every two real ones: the game keeps its own 30 fps (its timing and animations untouched), the screen shows 60
//   DM_FORK_FG=3x          two generated frames between every two real ones: 90 on screen (v46; on a 120 Hz screen they cannot be evenly spaced: 1 and 2 refreshes in turn)
//   DM_FORK_FG_FLOW=25-100 the flow scale in % (lower: lighter on the GPU, rougher motion), default 75
//   DM_FORK_FG_PERF=off    the full model instead of the lighter "performance" one (default: performance)
//   DM_FORK_LSFG_DLL       the player's own Lossless.dll (set by the app: Fork.exportRoot, after the setup screen copied it into private storage)
// How: each real frame is copied (a blit, queued on the GPU, also turning it to 16-bit float) into one of two AHardwareBuffer images the library shares (it
//   runs its own Vulkan device). A worker thread (fg_worker) waits for the copy, has the library make the in-between frame into a third image (no shared
//   semaphores: lsfg-vk's Android path, done when it returns), and presents it half a frame interval before the real one (VK_GOOGLE_display_timing,
//   nvn_vk.c pace_time), both from the shared images. The game's thread goes on with its next frame meanwhile. The real frame comes out about half a
//   frame later than without it, plus the generation time. The touch controls are drawn on both; the picture box and the Fill, corrected UI are the same.
// The library loads and the shaders are made from the DLL on a thread of its own at the first present; until it is ready (and if anything fails) frames are
//   presented as without it. Log lines: [FORK] fg ...
#pragma once
#include <android/hardware_buffer.h>

int fork_fg_live(void);
int g_fk_ahb;   // (below)
// the multiplier: 2 or 3 (frames on screen per game frame), 0 when off
int fork_fg_mult(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("DM_FORK_FG"); m = e && !strcmp(e, "2x") ? 2 : e && !strcmp(e, "3x") ? 3 : 0; }
    return m;
}   // the device was made with VK_ANDROID_external_memory_android_hardware_buffer (nvn_vk.c device creation, when fork_fg_want)
int fork_fg_want(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_FORK_FG"), *d = getenv("DM_FORK_LSFG_DLL"); struct stat st;
        v = fork_fg_mult() && d && *d && !stat(d, &st) && st.st_size > 0;
        if (e && *e && !v) VLOG("[FORK] fg: DM_FORK_FG=%s but no Lossless.dll (%s): off", e, d && *d ? d : "not chosen on the setup screen"); }
    return v;
}
static struct {
    _Atomic int state;   // 0 not started, 1 loading, 2 ready, -1 off
    int (*init)(const char *, uint64_t, int, float, int); int (*mkctx)(struct AHardwareBuffer *, struct AHardwareBuffer *, struct AHardwareBuffer *const *, int, uint32_t, uint32_t, int);
    int (*run)(int); void (*del)(int); const char *(*err)(void);
    int ctx; uint32_t w, h; AHardwareBuffer *ahb[4]; VkImage img[4]; VkDeviceMemory mem[4]; int used[4];   // 0, 1: the inputs; 2, 3: the generated frames (3x: both)
    uint64_t n, ngen; double gen_ms; VkCommandPool pool; VkCommandBuffer cb; VkFence fence;
} FG = { .ctx = -1 };
static const VkFormat FG_FMT = VK_FORMAT_R16G16B16A16_SFLOAT;   // (lsfg-vk's own choice for SDR frames)

static void *fg_load(void *p) {
    (void)p; const char *dll = getenv("DM_FORK_LSFG_DLL");
    void *lib = dlopen("libforklsfg.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { VLOG("[FORK] fg: libforklsfg.so not loaded (%s): off", dlerror()); atomic_store(&FG.state, -1); return NULL; }
    FG.init = (int (*)(const char *, uint64_t, int, float, int))dlsym(lib, "fork_lsfg_init");
    FG.mkctx = (int (*)(struct AHardwareBuffer *, struct AHardwareBuffer *, struct AHardwareBuffer *const *, int, uint32_t, uint32_t, int))dlsym(lib, "fork_lsfg_context");
    FG.run = (int (*)(int))dlsym(lib, "fork_lsfg_present"); FG.del = (void (*)(int))dlsym(lib, "fork_lsfg_delete"); FG.err = (const char *(*)(void))dlsym(lib, "fork_lsfg_error");
    if (!FG.init || !FG.mkctx || !FG.run || !FG.del || !FG.err) { VLOG("[FORK] fg: libforklsfg.so is missing functions: off"); atomic_store(&FG.state, -1); return NULL; }
    const char *fl = getenv("DM_FORK_FG_FLOW"), *pf = getenv("DM_FORK_FG_PERF");
    float flow = fl && atoi(fl) >= 25 && atoi(fl) <= 100 ? atoi(fl) / 100.0f : 0.75f; int perf = !(pf && !strcmp(pf, "off"));
    uint64_t dev = (uint64_t)g_props.vendorID << 32 | g_props.deviceID, t0 = mono_ns();
    if (FG.init(dll, dev, perf, flow, fork_fg_mult() - 1)) { VLOG("[FORK] [RED] fg: %s: off", FG.err()); atomic_store(&FG.state, -1); return NULL; }
    VLOG("[FORK] fg: ready in %.1f s (LSFG 3.1%s, flow scale %.2f, %dx)", (mono_ns() - t0) / 1e9, perf ? " performance" : "", flow, fork_fg_mult());
    atomic_store(&FG.state, 2); return NULL;
}
static int fg_mem_type(uint32_t bits) {
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits & (1u << i)) && (g_memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return (int)i;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if (bits & (1u << i)) return (int)i;
    return -1;
}
static void fg_free_images(void) {
    if (FG.ctx >= 0) { FG.del(FG.ctx); FG.ctx = -1; }
    for (int i = 0; i < 4; i++) { if (FG.img[i]) vkDestroyImage(g_dev, FG.img[i], NULL); if (FG.mem[i]) vkFreeMemory(g_dev, FG.mem[i], NULL); if (FG.ahb[i]) AHardwareBuffer_release(FG.ahb[i]);
        FG.img[i] = VK_NULL_HANDLE; FG.mem[i] = VK_NULL_HANDLE; FG.ahb[i] = NULL; FG.used[i] = 0; }
    FG.w = FG.h = 0; FG.n = 0;
}
// three w x h images shared with the library, and its context over them
static int fg_images(uint32_t w, uint32_t h) {
    if (FG.w == w && FG.h == h && FG.ctx >= 0) return 1;
    pthread_mutex_lock(&g_qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_qmu);
    fg_free_images();
    static PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of;
    if (!props_of) props_of = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(g_dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!props_of) { VLOG("[FORK] [RED] fg: vkGetAndroidHardwareBufferPropertiesANDROID missing: off"); return 0; }
    int nimg = 1 + fork_fg_mult();   // the two inputs and the generated frames
    for (int i = 0; i < nimg; i++) {
        AHardwareBuffer_Desc d = { .width = w, .height = h, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT,
                                   .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT };
        if (AHardwareBuffer_allocate(&d, &FG.ahb[i])) { VLOG("[FORK] [RED] fg: AHardwareBuffer_allocate %ux%u failed: off", w, h); fg_free_images(); return 0; }
        VkAndroidHardwareBufferFormatPropertiesANDROID fp = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
        VkAndroidHardwareBufferPropertiesANDROID bp = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp };
        if (props_of(g_dev, FG.ahb[i], &bp)) { VLOG("[FORK] [RED] fg: hardware buffer properties failed: off"); fg_free_images(); return 0; }
        VkExternalMemoryImageCreateInfo ei = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ei, .imageType = VK_IMAGE_TYPE_2D, .format = FG_FMT, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if (vkCreateImage(g_dev, &ci, NULL, &FG.img[i])) { VLOG("[FORK] [RED] fg: image creation failed: off"); fg_free_images(); return 0; }
        VkMemoryDedicatedAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = FG.img[i] };
        VkImportAndroidHardwareBufferInfoANDROID ii = { .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .pNext = &da, .buffer = FG.ahb[i] };
        int mt = fg_mem_type(bp.memoryTypeBits);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ii, .allocationSize = bp.allocationSize, .memoryTypeIndex = (uint32_t)(mt < 0 ? 0 : mt) };
        if (mt < 0 || vkAllocateMemory(g_dev, &ai, NULL, &FG.mem[i]) || vkBindImageMemory(g_dev, FG.img[i], FG.mem[i], 0)) { VLOG("[FORK] [RED] fg: importing the hardware buffer failed: off"); fg_free_images(); return 0; }
    }
    AHardwareBuffer *out[2] = { FG.ahb[2], FG.ahb[3] };
    FG.ctx = FG.mkctx(FG.ahb[0], FG.ahb[1], out, nimg - 2, w, h, (int)FG_FMT);
    if (FG.ctx < 0) { VLOG("[FORK] [RED] fg: %s: off", FG.err()); fg_free_images(); return 0; }
    FG.w = w; FG.h = h; FG.n = 0; for (int i = 2; i < nimg; i++) FG.used[i] = 1;   // (the generated frames are the library's from the start: taken over from it, contents kept)
    VLOG("[FORK] fg: images %ux%u shared with the library", w, h);
    return 1;
}
// image i to or from the library: our queue takes it over (acquire) or hands it back (release, in GENERAL as the library expects)
static void fg_own(VkCommandBuffer cb, int i, int acquire, VkImageLayout ours, VkAccessFlags acc) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = FG.img[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (acquire) { b.oldLayout = FG.used[i] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = ours; b.dstAccessMask = acc;
                   b.srcQueueFamilyIndex = FG.used[i] ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = FG.used[i] ? g_qf : VK_QUEUE_FAMILY_IGNORED; }
    else { b.oldLayout = ours; b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.srcAccessMask = acc; b.srcQueueFamilyIndex = g_qf; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; FG.used[i] = 1; }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
// one frame to the screen: src (w x h from (sx, sy), in layout sl) blitted into the picture box, the touch controls over it, shown at time t (0: as soon as it can)
static int fg_show(VkImage src, VkImageLayout sl, int fgi, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int flip, uint64_t t, uint32_t id) {
    uint32_t s = W.slot++ % 3;
    vk_wait(NULL, W.fence[s], UINT64_MAX);
    uint32_t ii; VkResult r = vkAcquireNextImageKHR(g_dev, W.sc, UINT64_MAX, W.acq[s], VK_NULL_HANDLE, &ii);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { swap_destroy(); return 0; }
    if (r && r != VK_SUBOPTIMAL_KHR) return 0;
    vkResetFences(g_dev, 1, &W.fence[s]);
    VkCommandBuffer cb = W.cb[s]; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);
    if (fgi >= 0) fg_own(cb, fgi, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = W.img[ii], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, W.img[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    int32_t box[4]; dm_picture_box((int32_t)W.lext.width, (int32_t)W.lext.height, sw, sh, box);
    VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{sx, flip ? sy + sh : sy, 0}, {sx + sw, flip ? sy : sy + sh, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{box[0], box[1], 0}, {box[0] + box[2], box[1] + box[3], 1}} };
    vkCmdBlitImage(cb, src, fgi >= 0 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : sl, W.img[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    if (fgi >= 0) fg_own(cb, fgi, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    if (!ovl_draw_turned(cb, W.img[ii], W.view[ii], W.ext, W.lext, W.rot, W.fmt, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) {
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = 0; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    }
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &W.acq[s], .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &W.done[s] };
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &W.done[s], .swapchainCount = 1, .pSwapchains = &W.sc, .pImageIndices = &ii };
    VkPresentTimeGOOGLE pt = { id, t }; VkPresentTimesInfoGOOGLE pti = { .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &pt };
    if (g_refresh_ns) pi.pNext = &pti;
    pthread_mutex_lock(&g_qmu);
    r = vkQueueSubmit(g_q, 1, &si, W.fence[s]);
    if (!r) r = vkQueuePresentKHR(g_q, &pi);
    pthread_mutex_unlock(&g_qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { swap_destroy(); return 0; }
    return 1;
}
// The worker: everything after the copy runs here, so the game's thread goes straight on with its next frame (v23 did it all on that thread and waited
// for the GPU at every frame: 18 frames a second on screen). One frame at a time: the next copy waits until the worker is done with this one (the library reads
// both inputs). It presents under g_wmu like present_window, both frames from the shared images (the real one from its input copy: the same blit as the other)
static pthread_mutex_t fg_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t fg_cv = PTHREAD_COND_INITIALIZER;
// frames put on screen by frame generation (generated + real), for the performance overlay (android_main.c nativeFgFrames); and where the time goes, logged
static _Atomic uint64_t g_fg_shown; static double g_fg_t[5]; static uint64_t g_fg_tn;   // per frame, ms: the game waiting for the worker, copy wait, generation, 1st, 2nd present
int fork_fg_live(void) { return atomic_load(&FG.state) == 2 && FG.ngen > 0 ? fork_fg_mult() : 0; }   // (nvn_vk.c pace_time: frames shown every interval / this)
uint64_t dm_fork_fg_frames(void) { if (framegen_mode() && !FGE.off) return atomic_load(&g_fork_up_shown);   // (upstream's frame generation: its pictures, nvn_vk.c)
    return atomic_load(&FG.state) == 2 ? atomic_load(&g_fg_shown) : (uint64_t)-1; }
static void fg_time(int i, uint64_t t0) { g_fg_t[i] += (mono_ns() - t0) / 1e6; }
static struct { int has, busy; uint64_t n; int in, flip; int32_t w, h; uint64_t t, half; uint32_t id; } FJ;
static void *fg_worker(void *p) {
    (void)p;
    for (;;) {
        pthread_mutex_lock(&fg_mu); while (!FJ.has) pthread_cond_wait(&fg_cv, &fg_mu); __typeof__(FJ) j = FJ; FJ.has = 0; pthread_mutex_unlock(&fg_mu);
        uint64_t t0 = mono_ns();
        vk_wait(NULL, FG.fence, UINT64_MAX);   // the copy (and the game's frame before it on the queue) is done
        fg_time(1, t0); uint64_t t1 = mono_ns();
        int gen = 0;
        // the library on every frame from the first: it counts its own calls and takes input (count % 2) as the newest frame. v23-v27 skipped the first frame,
        //   so from then on it read the two frames the wrong way round (motion estimated backwards: ghosting). The first frame's output (nothing to go between) is not shown
        if (j.n >= 1 && atomic_load(&FG.state) == 2) {
            if (FG.run(FG.ctx)) { VLOG("[FORK] [RED] fg: %s: off", FG.err()); atomic_store(&FG.state, -1); }
            else if (j.n >= 2) { gen = 1; fg_time(2, t1); double ms = (mono_ns() - t0) / 1e6; FG.gen_ms = FG.ngen ? FG.gen_ms * 0.98 + ms * 0.02 : ms;
                   if (++FG.ngen == 1 || FG.ngen % 1800 == 0) VLOG("[FORK] fg: %llu frames generated, %.1f ms each (on its own thread)", (unsigned long long)FG.ngen, FG.gen_ms); }
        }
        static int nopace = -1; if (nopace < 0) { const char *e = getenv("DM_FORK_FG_PACE"); nopace = e && !atoi(e); }   // DM_FORK_FG_PACE=0: no display times (a test)
        // the times, now that both frames are ready (v26 used the ones worked out ~20 ms earlier on the game's thread: often past by now, and of two frames both
        //   past due Android shows only the newer, the real one): the generated frame no earlier than the next refresh, the real one half an interval after it
        int ng = fork_fg_mult() - 1;   // generated frames this game frame (1 at 2x, 2 at 3x)
        if (j.t && g_refresh_ns && !nopace) {   // (j.half: the step between two shown frames, interval / multiplier)
            uint64_t now = mono_ns(), span = j.half * (uint64_t)ng, gt = j.t > span ? j.t - span : 0;
            if (gt < now + g_refresh_ns) gt = now + g_refresh_ns;
            if (j.t < gt + span) j.t = gt + span;
            j.half = (j.t - gt) / (uint64_t)ng; }
        pthread_mutex_lock(&g_wmu);
        if (W.sc) {
            uint64_t t2 = mono_ns();
            for (int k = 0; gen && k < ng && W.sc; k++) {   // the generated frames in order (lsfg-vk's outputs: the first is the earliest), each a step before the next
                uint64_t back = j.half * (uint64_t)(ng - k), at = nopace ? 0 : j.t > back ? j.t - back : 0;
                if (fg_show(FG.img[2 + k], VK_IMAGE_LAYOUT_GENERAL, 2 + k, 0, 0, j.w, j.h, j.flip, at, (j.id | 0x80000000u) + (uint32_t)k * 0x10000000u)) atomic_fetch_add(&g_fg_shown, 1); }   // (ids of their own: the display's timing report lists them too)
            fg_time(3, t2); uint64_t t3 = mono_ns();
            if (W.sc && fg_show(FG.img[j.in], VK_IMAGE_LAYOUT_GENERAL, j.in, 0, 0, j.w, j.h, j.flip, nopace ? 0 : j.t, j.id)) atomic_fetch_add(&g_fg_shown, 1);
            fg_time(4, t3);
        }
        pthread_mutex_unlock(&g_wmu);
        if (++g_fg_tn % 300 == 0) {
            VLOG("[FORK] fg timing, ms per frame (last 300): game waited for the worker %.1f; worker: copy wait %.1f, generation %.1f, generated frame shown %.1f, real frame shown %.1f",
                 g_fg_t[0] / 300, g_fg_t[1] / 300, g_fg_t[2] / 300, g_fg_t[3] / 300, g_fg_t[4] / 300);
            memset(g_fg_t, 0, sizeof g_fg_t); }
        pthread_mutex_lock(&fg_mu); FJ.busy = 0; pthread_cond_broadcast(&fg_cv); pthread_mutex_unlock(&fg_mu);
    }
    return NULL;
}
// present_window_locked (g_wmu held), once the swapchain is ready: 1 when this frame went to the worker, 0 to present it as usual
static int fork_fg_present(vtex *x, const int crop[4]) {
    if (!g_fk_ahb || !fork_fg_want()) return 0;
    int st = atomic_load(&FG.state);
    if (st == 0) { atomic_store(&FG.state, 1); pthread_t t; if (pthread_create(&t, NULL, fg_load, NULL)) atomic_store(&FG.state, -1); else pthread_detach(t); return 0; }
    if (st != 2) return 0;
    static int worker; if (!worker) { pthread_t t; if (pthread_create(&t, NULL, fg_worker, NULL)) { atomic_store(&FG.state, -1); return 0; } pthread_detach(t); worker = 1; }
    // the worker done with the last frame (g_wmu let go meanwhile: it presents under it)
    uint64_t tw = mono_ns(); pthread_mutex_lock(&fg_mu);
    if (FJ.busy) { pthread_mutex_unlock(&g_wmu); while (FJ.busy) pthread_cond_wait(&fg_cv, &fg_mu); pthread_mutex_unlock(&fg_mu); pthread_mutex_lock(&g_wmu); }
    else pthread_mutex_unlock(&fg_mu);
    fg_time(0, tw);
    if (!W.sc) return 1;   // (the window went while waiting: this frame is dropped)
    int32_t cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop && crop[2] ? crop[2] : (int32_t)x->w, ch = crop && crop[3] ? crop[3] : (int32_t)x->h;
    if (cw <= 0 || ch <= 0 || !fg_images((uint32_t)cw, (uint32_t)ch)) { if (FG.ctx < 0 && FG.w == 0) atomic_store(&FG.state, -1); return 0; }
    if (!FG.pool) {
        VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf };
        VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandBufferCount = 1, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY };
        VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateCommandPool(g_dev, &pc, NULL, &FG.pool)) { atomic_store(&FG.state, -1); return 0; }
        ca.commandPool = FG.pool; vkAllocateCommandBuffers(g_dev, &ca, &FG.cb); vkCreateFence(g_dev, &fc, NULL, &FG.fence);
    }
    // the real frame into the library's input (frame number % 2), as 16-bit float: queued, not waited for
    int in = (int)(FG.n % 2);
    VkCommandBuffer cb = FG.cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);
    fg_own(cb, in, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {cw, ch, 1}} };
    vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, FG.img[in], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    fg_own(cb, in, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkResetFences(g_dev, 1, &FG.fence);
    pthread_mutex_lock(&g_qmu); VkResult r = vkQueueSubmit(g_q, 1, &si, FG.fence); pthread_mutex_unlock(&g_qmu);
    if (r) return 0;
    // when the two frames should show: the real one at this frame's paced time, the generated one half an interval before it
    VkPresentTimeGOOGLE pt = { 0, 0 }; uint64_t half = 0;
    if (pace_time(&pt) && pt.desiredPresentTime && g_refresh_ns) {
        const char *e = getenv("DM_FPS_TARGET"); int fps = e && atoi(e) >= 30 ? atoi(e) : 30;
        uint64_t r2 = g_refresh_ns, k = (1000000000ull / (uint64_t)fps + r2 / 2) / r2; half = (k ? k : 1) * r2 / (uint64_t)fork_fg_mult(); }   // (v46: interval / multiplier)
    pthread_mutex_lock(&fg_mu);
    FJ.n = ++FG.n; FJ.in = in; FJ.w = cw; FJ.h = ch; FJ.flip = atomic_load(&g_origin_ll); FJ.t = pt.desiredPresentTime; FJ.half = half; FJ.id = pt.presentID;
    FJ.has = 1; FJ.busy = 1; pthread_cond_signal(&fg_cv); pthread_mutex_unlock(&fg_mu);
    return 1;
}

// The frame recorder's analysis (nvn/fg_rec.h) on a made-up game of two frames, each: a 3D pass (colour A + depth D, both cleared), a composite into the
// shown image B that samples A, then a HUD pass loaded onto B; plus a shadow map S cleared and drawn but never read. Expected: A's store read in the frame,
// D's overwritten unread in the next frame, S's too, B shown; passes 1-2 a merge candidate (same size, back to back); the last pass with depth is the 3D pass and the
// HUD pass is listed after it.
// cc -O1 -pthread -I../nvn -I../../third_party/vulkan-headers/include -o /tmp/t_fg_rec t_fg_rec.c && /tmp/t_fg_rec
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#define VLOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
static VkDevice g_dev; static VkPhysicalDeviceProperties g_props; static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static int g_tsq_ok(void) { return 0; }
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool p, uint32_t a, uint32_t n) { (void)c; (void)p; (void)a; (void)n; }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q) { (void)c; (void)s; (void)p; (void)q; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t a, uint32_t n, size_t sz, void *o, VkDeviceSize st, VkQueryResultFlags f) { (void)d; (void)p; (void)a; (void)n; (void)sz; (void)o; (void)st; (void)f; return VK_NOT_READY; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo *i, const VkAllocationCallbacks *a, VkQueryPool *p) { (void)d; (void)i; (void)a; (void)p; return VK_ERROR_FEATURE_NOT_PRESENT; }
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice d) { (void)d; return VK_SUCCESS; }
#include "fg_rec.h"

static int A, B, D, S;   // the images (their addresses are what the recorder keys on)
static void pass(int *img, int depth, int clear, int draws) {
    fgr_attin a[2]; int n = 0;
    a[n++] = (fgr_attin){ img, 1280, 720, 37, 0, 0, 0, (uint8_t)clear, 0 };
    if (depth) a[n++] = (fgr_attin){ &D, 1280, 720, 126, 0, 0, 8, 1, 0 };
    fgr_pass_begin(NULL, 1280, 720, 1, 1, a, n);
    for (int i = 0; i < draws; i++) fgr_draw();
}
static void frame(uint64_t n) {
    { fgr_attin s = { &S, 2048, 2048, 126, 0, 0, 8, 1, 0 }; fgr_pass_begin(NULL, 2048, 2048, 1, 1, &s, 1); fgr_draw(); fgr_pass_end(NULL); }   // shadow map, never read
    pass(&A, 1, 1, 300); fgr_pass_end(NULL);                         // 3D
    pass(&B, 0, 1, 1); fgr_read_img(&A, 1280, 720, 37, FGR_READ); fgr_pass_end(NULL);   // composite samples A
    pass(&B, 0, 0, 40); fgr_pass_end(NULL);                          // HUD on B
    fgr_present(n, &B, 1280, 720, 37);
}
int main(void) {
    char dir[] = "/tmp/fgrXXXXXX"; if (!mkdtemp(dir)) return 2;
    char pc[256]; snprintf(pc, sizeof pc, "%s/pipeline_cache.bin", dir); setenv("DM_VK_PCACHE", pc, 1); setenv("DM_FG_RECORD", "1:2", 1);
    for (uint64_t n = 1; n <= 6; n++) frame(n);
    char path[300]; snprintf(path, sizeof path, "%s/framegraph.txt", dir); FILE *f = fopen(path, "r"); if (!f) { puts("✗ framegraph.txt not written"); return 1; }
    static char t[1 << 16]; size_t len = fread(t, 1, sizeof t - 1, f); t[len] = 0; fclose(f); fputs(t, stdout);
    int fails = 0, checks = 0;
    #define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
    CHECK(strstr(t, "2 frame(s)") != NULL, "two frames recorded");
    CHECK(strstr(t, "c0 img1 1280x720 vk37 L0/0: cleared, store -> read in this frame") != NULL, "the 3D colour's store is read in its frame (the composite samples it)");
    CHECK(strstr(t, "depth img2 1280x720 vk126 L0/0: cleared, store -> overwritten unread in a later frame") != NULL, "the 3D depth's store is overwritten unread in the next frame");
    CHECK(strstr(t, "depth img0 2048x2048 vk126 L0/0: cleared, store -> overwritten unread in a later frame") != NULL, "the shadow map's store is never read");
    CHECK(strstr(t, "(SHOWN)") != NULL, "the shown image is marked");
    CHECK(strstr(t, "merge candidate") != NULL, "back-to-back passes of one size are merge candidates");
    CHECK(strstr(t, "last pass with depth (the 3D scene): pass 1; after it, into the shown image img3: pass 2 (1 draws) pass 3 (40 draws)") != NULL, "the scene / HUD split: the composite and HUD passes after the 3D pass");
    {   // the held frame's verdict (fgr_fate_exact, DM_VK_FRAME_HOLD=2): what decides that a store can be left out
        fgr_cont_start(); fgr_cont_reset();
        static int X, Y;   // X a 1024x1024 depth image with two layers, Y a colour image
        fgr_attin d0 = { &X, 1024, 1024, 126, 0, 0, 8, 1, 0 }, d0b = { &X, 1024, 1024, 126, 0, 1, 8, 1, 0 }, c = { &Y, 1024, 1024, 37, 0, 0, 0, 1, 0 };
        fgr_pass_begin(NULL, 1024, 1024, 1, 1, &d0, 1); fgr_pass_end(NULL);       // 0: X layer 0 written
        fgr_pass_begin(NULL, 1024, 1024, 1, 1, &d0b, 1); fgr_pass_end(NULL);      // 1: X layer 1 cleared (another layer)
        fgr_pass_begin(NULL, 512, 512, 1, 1, &d0, 1); fgr_pass_end(NULL);         // 2: X layer 0 cleared over a quarter only
        fgr_pass_begin(NULL, 1024, 1024, 1, 1, &d0, 1); fgr_pass_end(NULL);       // 3: X layer 0 cleared over all of it
        fgr_pass_begin(NULL, 1024, 1024, 1, 1, &c, 1); fgr_pass_end(NULL);        // 4: Y written
        fgr_read_img(&Y, 1024, 1024, 37, FGR_READ);                                  //    Y read
        fgr_pass_begin(NULL, 1024, 1024, 1, 1, &c, 1); fgr_pass_end(NULL);        // 5: Y cleared
        fgr_pass_begin(NULL, 1024, 1024, 1, 1, &d0b, 1); fgr_pass_end(NULL);      // 6: X layer 1 written, nothing after
        pthread_mutex_lock(&g_fgrmu); fgr_rstart_build();
        int f2 = fgr_fate_exact(2, &g_fgr.p[2].a[0]), f0 = fgr_fate_exact(0, &g_fgr.p[0].a[0]), f3 = fgr_fate_exact(3, &g_fgr.p[3].a[0]),
            f4 = fgr_fate_exact(4, &g_fgr.p[4].a[0]), f6 = fgr_fate_exact(6, &g_fgr.p[6].a[0]), f1 = fgr_fate_exact(1, &g_fgr.p[1].a[0]);
        pthread_mutex_unlock(&g_fgrmu);
        CHECK(f0 == 0, "verdict: a later clear over only part of the image keeps the store (needed)");
        CHECK(f2 == 1, "verdict: a later clear of the same level, layer and aspect over all of it: overwritten unread");
        CHECK(f3 == 4, "verdict: another layer's clear (pass 6, layer 1) is not ours: unknown (stays stored)");
        CHECK(f1 == 1, "verdict: layer 1's store, its layer cleared whole later (pass 6), passes on other layers between: overwritten unread");
        CHECK(f4 == 0, "verdict: read before it is cleared: needed");
        CHECK(f6 == 4, "verdict: the last pass, nothing after it: unknown");
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

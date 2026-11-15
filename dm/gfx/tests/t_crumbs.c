// GPU breadcrumbs (nvn/vk_crumbs.h) with a fake Vulkan: submissions with passes and pipeline binds are recorded, the dump (into a temporary stderr) names the
// oldest unfinished submission, the pass the GPU's mark says it stopped in and the shaders bound in that pass; the rings wrap without stale entries showing;
// a submission that is not one of the slots (the present thread's) is ignored; the completed history counts.
// cc -O2 -I../nvn -I../../third_party/vulkan-headers/include -o /tmp/t_crumbs t_crumbs.c -lpthread && /tmp/t_crumbs
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#define VLOG(...) do { fprintf(stderr, "[dm] [nvn-vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
// what vk_crumbs.h takes from nvn_vk.c
typedef struct { VkCommandBuffer cb; uint64_t fence, frame; } sub_t;
#define NSUB 6
static sub_t g_subs[NSUB]; static sub_t *g_sub; static sub_t *g_inflight[NSUB * 2]; static int g_nin; static uint64_t g_frame_no;
static VkDevice g_dev = (VkDevice)1;
static uint64_t T_now = 1000000000ull; static uint64_t mono_ns(void) { return T_now; }
static int on_sub_thread(void) { return 1; }
static uint32_t T_mem[64]; static int T_fills, T_barriers;
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) { (void)bits; (void)want; (void)avoid; return 0; }
VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *i, const VkAllocationCallbacks *a, VkBuffer *b) { (void)d; (void)i; (void)a; *b = (VkBuffer)7; return VK_SUCCESS; }
void vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) { (void)d; (void)b; r->size = 256; r->memoryTypeBits = 1; r->alignment = 4; }
VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m) { (void)d; (void)i; (void)a; *m = (VkDeviceMemory)8; return VK_SUCCESS; }
VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; return VK_SUCCESS; }
VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **p) { (void)d; (void)m; (void)o; (void)s; (void)f; *p = T_mem; return VK_SUCCESS; }
void vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags f, uint32_t n1, const VkMemoryBarrier *m, uint32_t n2,
                                 const VkBufferMemoryBarrier *bm, uint32_t n3, const VkImageMemoryBarrier *im) { (void)cb; (void)a; (void)b; (void)f; (void)n1; (void)m; (void)n2; (void)bm; (void)n3; (void)im; T_barriers++; }
static int T_gpu_runs = 1;   // the fake GPU carries out fills (0: it "hangs" and never writes)
void vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize n, uint32_t v) { (void)cb; (void)b; (void)n; T_fills++; if (T_gpu_runs) T_mem[off / 4] = v; }
#include "vk_crumbs.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char *dump(const char *why) {   // the dump's text
    fflush(stderr); FILE *t = tmpfile(); int saved = dup(2); dup2(fileno(t), 2);
    nvn_vk_crumbs_dump(why); fflush(stderr); dup2(saved, 2); close(saved);
    long n = ftell(t); rewind(t); char *s = calloc(1, (size_t)n + 1); if (fread(s, 1, (size_t)n, t) != (size_t)n) s[0] = 0; fclose(t); return s;
}
static void pass(sub_t *s, uint32_t w, uint32_t fmt, const char *vs, const char *fs) {
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, w / 2 } }, .colorAttachmentCount = 1 };
    cr_pass_begin(s->cb, &ri, &fmt, 126); cr_pipe(vs, fs); cr_pass_end(s->cb);
}
int main(void) {
    for (int i = 0; i < NSUB; i++) g_subs[i].cb = (VkCommandBuffer)(uintptr_t)(100 + i);
    // without DM_VK_BREADCRUMBS: recorded, no GPU work added
    unsetenv("DM_VK_BREADCRUMBS");
    sub_t *s = &g_subs[0]; g_frame_no = 5; cr_sub_begin(s); g_sub = s;
    pass(s, 1920, 97, "aaaa000000000001", "bbbb000000000001"); pass(s, 960, 97, "aaaa000000000002", "bbbb000000000002");
    CHECK(T_fills == 0 && T_barriers == 0, "off: no barriers or fills recorded (%d, %d)", T_barriers, T_fills);
    s->fence = 1; cr_submitted(s); g_sub = NULL; g_inflight[g_nin++] = s; T_now += 50000000ull;
    char *d = dump("device lost");
    CHECK(strstr(d, "oldest unfinished submission: slot 0 fence 1 frame 5, 2 passes, 2 pipeline binds, submitted 50 ms ago") != NULL, "off: the submission summarised");
    CHECK(strstr(d, "DM_VK_BREADCRUMBS=1 marks") != NULL, "off: the dump says how to get the pass");
    CHECK(strstr(d, "pass 2: 960x480 colour fmt 97 depth fmt 126") && strstr(d, "pipeline vertex aaaa000000000002 fragment bbbb000000000002"), "off: passes and shaders listed");
    free(d);
    cr_completed(s); g_nin = 0;
    CHECK(atomic_load(&g_crh_n) == 1 && g_crh[0].npass == 2 && g_crh[0].nbind == 2 && g_crh[0].us == 50000, "history: one completed submission, 2 passes, 50 ms submit to done (%u us)", g_crh[0].us);

    // with it: each pass ends with a barrier and a fill; the GPU stops after pass 3 of 5
    g_cr_gpu = -1; setenv("DM_VK_BREADCRUMBS", "1", 1); T_fills = T_barriers = 0;
    s = &g_subs[1]; g_frame_no = 6; cr_sub_begin(s); g_sub = s;
    char v[24], f[24];
    for (int i = 1; i <= 5; i++) { if (i == 4) T_gpu_runs = 0; snprintf(v, sizeof v, "vs%014d", i); snprintf(f, sizeof f, "fs%014d", i); pass(s, 1024, 44, v, f); }
    CHECK(T_fills == 5 && T_barriers == 5, "on: a barrier and a fill after each of 5 passes (%d, %d)", T_barriers, T_fills);
    CHECK(T_mem[1] == 3, "on: the GPU's mark for slot 1 is pass 3 (%u)", T_mem[1]);
    s->fence = 2; cr_submitted(s); g_sub = NULL; g_inflight[g_nin++] = s;
    sub_t other = { (VkCommandBuffer)999, 77, 0 }; g_inflight[g_nin++] = &other;   // (the present thread's own submission: not a slot)
    d = dump("device lost");
    CHECK(strstr(d, "the GPU finished pass 3 of 5 (the next one is where it stopped)") != NULL, "on: the stopping point stated");
    CHECK(strstr(d, "pass 4 <- not finished: 1024x512") != NULL, "on: pass 4 marked as not finished");
    { char *p4 = strstr(d, "pass 4 <- not finished"), *p5 = p4 ? strstr(p4, "pass 5") : NULL, *sh = p4 ? strstr(p4, "fragment fs00000000000004") : NULL;
      CHECK(sh && (!p5 || sh < p5), "on: the shaders bound in pass 4 listed under it"); }
    CHECK(strstr(d, "fence 77") == NULL, "a submission that is not a slot is not printed");
    free(d); g_nin = 0; T_gpu_runs = 1;

    // ring wrap: 100 passes, 100 binds in one submission; only real entries print, the last passes are there
    s = &g_subs[2]; cr_sub_begin(s); g_sub = s;
    for (int i = 1; i <= 100; i++) { snprintf(v, sizeof v, "w%015d", i); pass(s, 64, 1, v, "x"); }
    T_mem[2] = 0;   // (the GPU did nothing yet)
    s->fence = 3; cr_submitted(s); g_sub = NULL; g_inflight[g_nin++] = s;
    d = dump("hang");
    CHECK(strstr(d, "100 passes, 100 pipeline binds") != NULL, "wrap: counts kept past the rings");
    CHECK(strstr(d, "pass 89:") && strstr(d, "pass 100:") && !strstr(d, "pass 88:") && !strstr(d, "not finished"), "wrap: an unfinished pass older than the ring: the newest 12 passes listed, none marked");
    CHECK(strstr(d, "pipeline vertex w000000000000100 fragment x") && !strstr(d, "w000000000000001 "), "wrap: the newest binds listed, no stale ones");
    free(d);
    // a slot reused: its old passes do not leak into the new submission
    cr_sub_begin(s); g_sub = s; pass(s, 32, 2, "new0000000000001", "f");
    s->fence = 4; cr_submitted(s); g_sub = NULL; g_inflight[0] = s; g_nin = 1;
    d = dump("reuse"); if (getenv("T_SHOW")) fputs(d, stdout);
    CHECK(strstr(d, "1 passes, 1 pipeline binds") && !strstr(d, "vertex w0"), "reuse: only the new submission's work");
    free(d);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}

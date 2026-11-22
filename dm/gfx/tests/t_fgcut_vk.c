// nvn_fgcut.h on a real Vulkan driver (lavapipe is enough): the cut / still check and the picture swap after a cut.
//   Pairs: a pan of a textured scene (12 and 40 px a frame: not a cut), the same frame twice (still, not a cut), two unrelated scenes (a cut), a fade to black over
//   one frame (a cut: nothing to blend from), a small HUD change only (not a cut, not still). After each pair, fgc_select over a made-up picture and fgc_select_rt
//   over another (a render target, as our own frame generation's): replaced by the newer frame after a cut, untouched otherwise.
//   cc -O1 -I<vulkan headers> -I../nvn -o /tmp/t_fgcut_vk t_fgcut_vk.c -l:libvulkan.so.1 -lm && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json /tmp/t_fgcut_vk
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include "../nvn/nvn_fgcut.h"
#define W 480
#define H 270
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags want) { for (uint32_t t = 0; t < mp.memoryTypeCount; t++) if ((bits >> t & 1) && (mp.memoryTypes[t].propertyFlags & want) == want) return t; return 0; }
typedef struct { VkImage img; VkImageView view; } img;
static img mkimg(void) {
    img i; VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
    vkCreateImage(dev, &ci, NULL, &i.img); VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, i.img, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }; VkDeviceMemory m;
    vkAllocateMemory(dev, &ai, NULL, &m); vkBindImageMemory(dev, i.img, m, 0);
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i.img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCreateImageView(dev, &vci, NULL, &i.view); return i;
}
// scenes: a landscape-ish texture (smooth bands + detail), shifted by dx; kind 1 another scene entirely; kind 2 black; kind 3 the first with a HUD box changed
static uint8_t px(int kind, int x, int y, int c, int dx) {
    if (kind == 2) return c == 3 ? 255 : 0;
    if (kind == 1) return c == 3 ? 255 : (uint8_t)(128 + 100 * sin((x * 0.11 + y * 0.07) + c) * cos(y * 0.05));
    int xs = x + dx; double v = 90 + 60 * sin(xs * 0.021 + c * 0.7) + 40 * sin(y * 0.033) + 20 * sin((xs + y) * 0.17);
    if (kind == 3 && x >= 20 && x < 90 && y >= 15 && y < 35) v = c == 0 ? 250 : 30;
    return c == 3 ? 255 : (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}
int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };   // (1.0: the check must run on the baseline)
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("SKIP: no Vulkan instance\n"); return 0; }
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); if (!n) { printf("SKIP: no Vulkan device\n"); return 0; }
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f); VkPhysicalDeviceFeatures en = { .shaderStorageImageWriteWithoutFormat = f.shaderStorageImageWriteWithoutFormat };
    uint32_t nq = 8; VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp); uint32_t qf = 0; while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) qf++;
    float prio = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &en };
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("SKIP: no device\n"); return 0; }
    // (as in the engine: the frames are only compared where frame generation itself runs, which samples them filtered (nvn_fgen.h fg_setup, nvn_lsfg.h ls_fmt_lacks):
    //  a driver without linear filtering of RGBA8 (PowerVR Rogue G6430) has neither; and fgc_init says no where a group of 256 threads is not allowed)
    { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) { printf("SKIP: no linear filtering of RGBA8: no frame generation here\n"); return 0; } }
    VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf }; VkCommandPool pool; vkCreateCommandPool(dev, &pci, NULL, &pool);
    static fgc C;
    { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
      if (pp.limits.maxComputeWorkGroupInvocations < 256 || pp.limits.maxComputeWorkGroupSize[0] < 256) { CHECK(!fgc_init(&C, dev, pd), "fgc_init took a driver with %u threads a group", pp.limits.maxComputeWorkGroupInvocations);
          if (fails) { printf("t_fgcut_vk: %d FAILED\n", fails); return 1; }
          printf("SKIP: %u threads a work group at most: no cut check here (fgc_init refused it)\n", pp.limits.maxComputeWorkGroupInvocations); return 0; } }
    CHECK(fgc_init(&C, dev, pd), "fgc_init: %s", C.why ? C.why : "?"); if (!C.ok) return 1;
    if (!C.can_select) printf("note: no storage writes without a format here: the swap is not tested\n");
    VkBuffer buf; uint8_t *map; { VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        vkCreateBuffer(dev, &bci, NULL, &buf); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf, &rq);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory m; vkAllocateMemory(dev, &ai, NULL, &m); vkBindBufferMemory(dev, buf, m, 0); void *p; vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p); map = p; }
    img a = mkimg(), b = mkimg(), pic = mkimg(), pic2 = mkimg();
    struct { const char *what; int ka, kb, dx; int cut, still; } T[] = {
        { "a pan of 12 px", 0, 0, 12, 0, 0 }, { "a pan of 40 px", 0, 0, 40, 0, 0 }, { "the same frame twice", 0, 0, 0, 0, 1 },
        { "two unrelated scenes", 0, 1, 0, 1, 0 }, { "a cut to black", 0, 2, 0, 1, 0 }, { "a HUD box changed only", 0, 3, 0, 0, 0 } };
    const float still_max = 0.0015f;
    for (unsigned t = 0; t < sizeof T / sizeof *T; t++) {
        int slot = (int)(t % FGC_SLOTS);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 4; c++) { size_t i = ((size_t)y * W + x) * 4 + c;
            map[i] = px(T[t].ka, x, y, c, 0); map[W * H * 4 + i] = px(T[t].kb, x, y, c, T[t].dx); map[2 * W * H * 4 + i] = map[3 * W * H * 4 + i] = c == 3 ? 255 : 77; }
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &ai, &cb); VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
        img *ims[4] = { &a, &b, &pic, &pic2 };
        for (int k = 0; k < 4; k++) {
            VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = ims[k]->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            VkBufferImageCopy bc = { (VkDeviceSize)(k < 3 ? k : 2) * W * H * 4, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
            vkCmdCopyBufferToImage(cb, buf, ims[k]->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc); }
        fgc_stat(&C, cb, slot, a.view, b.view);
        fgc_select(&C, cb, slot, 0, b.view, pic.view, W, H);
        fgc_select_rt(&C, cb, slot, b.view, pic2.view, VK_FORMAT_R8G8B8A8_UNORM, W, H);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferImageCopy bc = { (VkDeviceSize)2 * W * H * 4, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        vkCmdCopyImageToBuffer(cb, pic.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bc);
        VkBufferImageCopy bc3 = bc; bc3.bufferOffset = (VkDeviceSize)3 * W * H * 4; vkCmdCopyImageToBuffer(cb, pic2.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bc3);
        vkEndCommandBuffer(cb); VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHECK(!vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) && !vkQueueWaitIdle(q), "submit");
        fgc_result r; float mean = -1; int got = fgc_read(&C, slot, &r, &mean);
        CHECK(got, "%s: no result", T[t].what);
        int replaced = 0, kept = 0;
        for (size_t i = 0; i < (size_t)W * H * 4; i++) { if (i % 4 == 3) continue; uint8_t o = map[2 * W * H * 4 + i]; if (o == map[W * H * 4 + i]) replaced++; if (o == 77) kept++; }
        int was_replaced = replaced == W * H * 3, replaced2 = 0, kept2 = 0;
        for (size_t i = 0; i < (size_t)W * H * 4; i++) { if (i % 4 == 3) continue; uint8_t o = map[3 * W * H * 4 + i]; if (o == map[W * H * 4 + i]) replaced2++; if (o == 77) kept2++; }
        printf("%-24s changed cells %3u of %u, mean change %.4f: %s%s; picture %s\n", T[t].what, r.changed, r.cells, mean, r.flag ? "CUT" : "no cut", mean < still_max ? ", still" : "",
               was_replaced ? "replaced by the newer frame" : kept == W * H * 3 ? "untouched" : "partly changed");
        CHECK((int)r.flag == T[t].cut, "%s: cut %u, expected %d", T[t].what, r.flag, T[t].cut);
        CHECK((mean < still_max) == T[t].still, "%s: still %d, expected %d", T[t].what, mean < still_max, T[t].still);
        if (C.can_select) CHECK(T[t].cut ? was_replaced : kept == W * H * 3, "%s: picture %s", T[t].what, was_replaced ? "replaced" : "not replaced as expected");
        CHECK(T[t].cut ? replaced2 == W * H * 3 : kept2 == W * H * 3, "%s: render-target picture %s (replaced %d, kept %d)", T[t].what, T[t].cut ? "not replaced" : "changed", replaced2, kept2);
        vkFreeCommandBuffers(dev, pool, 1, &cb);
    }
    printf(fails ? "t_fgcut_vk: %d FAILED\n" : "t_fgcut_vk: all passed\n", fails);
    return fails != 0;
}

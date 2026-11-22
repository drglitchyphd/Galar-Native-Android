// SPDX-License-Identifier: GPL-3.0-or-later
// nvn_lsfg.h's HUD pass (ls_hud, overlay/ls_hud.comp, ours) on a real Vulkan driver (lavapipe is enough), without the Lossless Scaling shaders: a made-up in-between
// picture, the newest frame's scene and the frame as shown (that scene with HUD boxes drawn on it). Expected out: the HUD boxes exactly as shown, everywhere else the
// in-between picture untouched; sRGB frames handled on their raw values (the pass sees UNORM views). Third case: the chain at half the frame's size (the work size,
// ls_setup_work): the picture filtered up to the frame's size (a smooth one, so the expected value is its bilinear filtering), the HUD rule on the full-size scene (sfull).
//   cc -O1 -I<vulkan headers> -o /tmp/t_lshud_vk t_lshud_vk.c -l:libvulkan.so.1 -lm && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json /tmp/t_lshud_vk
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include "../nvn/nvn_lsfg.h"
#define W 320
#define H 200
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int inbox(int x, int y) { return (x >= 20 && x < 120 && y >= 10 && y < 40) || (x >= 200 && x < 300 && y >= 150 && y < 190); }
int main(void) {
    // (1.3, as the engine's instance where Lossless Scaling runs: the sRGB frame's image takes a UNORM storage view, and the formats' STORAGE_WRITE_WITHOUT_FORMAT is asked)
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("SKIP: no Vulkan instance\n"); return 0; }
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd); if (!n) { printf("SKIP: no Vulkan device\n"); return 0; }
    // the pass runs only where Lossless Scaling set up (nvn_lsfg.h ls_setup_work): its device needs, then each frame format's (below)
    { const char *lack = ls_device_lacks(pd, 0); if (lack) { printf("SKIP: Lossless Scaling (and so its HUD pass) cannot run here: no %s\n", lack); return 0; } }
    VkPhysicalDeviceFeatures en = { .shaderStorageImageWriteWithoutFormat = VK_TRUE };
    uint32_t nq = 8; VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp); uint32_t qf = 0; while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) qf++;
    float prio = 1; VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &en }; VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("SKIP: no device\n"); return 0; }
    VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf }; VkCommandPool pool; vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBuffer buf[4]; uint8_t *map[4];   // in: picture, scene, shown; out
    for (int i = 0; i < 4; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT }; vkCreateBuffer(dev, &bci, NULL, &buf[i]);
        VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf[i], &rq); uint32_t t = 0; const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (t < mp.memoryTypeCount && !((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & hv) == hv)) t++;
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; VkDeviceMemory m; vkAllocateMemory(dev, &mai, NULL, &m);
        vkBindBufferMemory(dev, buf[i], m, 0); void *p; vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p); map[i] = p; }
    static const VkFormat fmts[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
    for (int fi = 0; fi < 3; fi++) {
        int half = fi == 2; uint32_t mw = half ? W / 2 : W, mh = half ? H / 2 : H;
        static lsfg G; memset(&G, 0, sizeof G); G.dev = dev; G.pd = pd;
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        vkCreateSampler(dev, &sci, NULL, &G.samp[LS_S_EDGE]);
        G.hbase = ls_hud_base(&G); CHECK(G.hbase, "the HUD pass's pipeline was not made"); if (!G.hbase) break;
        VkFormat fmt = fmts[fi & 1], u = ls_unorm(fmt); int ok = 1;
        { const char *lack = ls_fmt_lacks(pd, u); if (!lack) lack = ls_fmt_lacks(pd, VK_FORMAT_R8_UNORM); if (!lack) lack = ls_fmt_lacks(pd, VK_FORMAT_R16G16B16A16_SFLOAT);
          if (lack) { printf("%s: Lossless Scaling falls back to ours here (no %s), no HUD pass\n", fi == 1 ? "sRGB frame" : "UNORM frame", lack); ls_images_free(&G); continue; } }
        for (int k = 0; k < LS_MAXGEN; k++) ok = ok && ls_img_make(&G, &G.m[k], u, mw, mh, fmt);
        ok = ok && ls_img_make(&G, &G.c[0], u, mw, mh, fmt) && ls_img_make(&G, &G.c[1], u, mw, mh, fmt) && ls_img_make(&G, &G.hud, u, W, H, fmt) && ls_img_make(&G, &G.o, u, W, H, fmt);
        if (half) ok = ok && ls_img_make(&G, &G.sfull, u, W, H, fmt);
        CHECK(ok, "images"); if (!ok) break;
        G.hpipe = ls_hud_pipe(&G, u); CHECK(G.hpipe, "no HUD pipeline for the frame's format"); if (!G.hpipe) break;   // (RGBA8 frames: the module that names rgba8)
        G.w = mw; G.h = mh; G.ow = W; G.oh = H; G.fresh = 1; ls_hud_write(&G); CHECK(ls_hud_ok(&G), "ls_hud_ok");
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 4; c++) { int i = (y * W + x) * 4 + c;
            uint8_t sc = (uint8_t)((x * 3 + y * 5 + c * 40) & 255);
            if (!half) map[0][i] = c == 3 ? 255 : (uint8_t)((x * 7 + y * 2 + c * 11) & 255);   // the in-between picture: something else entirely
            else if (x < (int)mw && y < (int)mh) map[0][(y * (int)mw + x) * 4 + c] = c == 3 ? 255 : (uint8_t)(x + y * 2 + c * 10);   // (half: smooth, packed mw a row)
            map[1][i] = c == 3 ? 255 : sc;                                                      // the newest frame's scene
            map[2][i] = c == 3 ? 255 : inbox(x, y) ? (uint8_t)(c == 0 ? 250 : sc ^ 0x80) : sc; }   // shown: the scene with the HUD boxes on it
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &ai, &cb); VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
        ls_begin(&G, cb);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        const VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        G.p = 1; int gen = 1;   // (the second set row, a picture other than the first: the indexing as nvn_vk.c uses it)
        const VkBufferImageCopy bh = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { mw, mh, 1 } };
        vkCmdCopyBufferToImage(cb, buf[0], G.m[gen].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bh); vkCmdCopyBufferToImage(cb, buf[1], half ? G.sfull.img : G.c[1].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        vkCmdCopyBufferToImage(cb, buf[2], G.hud.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        ls_hud(&G, cb, gen);
        vkCmdCopyImageToBuffer(cb, G.o.img, VK_IMAGE_LAYOUT_GENERAL, buf[3], 1, &bc);
        vkEndCommandBuffer(cb); VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHECK(!vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) && !vkQueueWaitIdle(q), "submit");
        int bad_hud = 0, bad_pic = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) { int i = (y * W + x) * 4 + c, want;
            if (inbox(x, y)) want = map[2][i];
            else if (!half) want = map[0][i];
            else {   // bilinear filtering of the half-size picture at this pixel's centre, clamped at the edges
                float u = (x + 0.5f) * mw / W - 0.5f, v = (y + 0.5f) * mh / H - 0.5f; int x0 = (int)floorf(u), y0 = (int)floorf(v); float fx = u - x0, fy = v - y0;
#define PX(xx, yy) (float)map[0][((((yy) < 0 ? 0 : (yy) >= (int)mh ? (int)mh - 1 : (yy)) * (int)mw) + ((xx) < 0 ? 0 : (xx) >= (int)mw ? (int)mw - 1 : (xx))) * 4 + c]
                want = (int)lroundf((PX(x0, y0) * (1 - fx) + PX(x0 + 1, y0) * fx) * (1 - fy) + (PX(x0, y0 + 1) * (1 - fx) + PX(x0 + 1, y0 + 1) * fx) * fy);
#undef PX
            }
            if (abs(map[3][i] - want) > (half && !inbox(x, y) ? 1 : 0)) { if (inbox(x, y)) bad_hud++; else bad_pic++; } }
        const char *nm = half ? "half-size work, UNORM frame" : fi ? "sRGB frame" : "UNORM frame";
        printf("%s: HUD pixels wrong %d, picture pixels changed %d\n", nm, bad_hud, bad_pic);
        CHECK(bad_hud == 0 && bad_pic == 0, "%s: HUD %d, picture %d", nm, bad_hud, bad_pic);
        vkFreeCommandBuffers(dev, pool, 1, &cb); ls_images_free(&G); }
    printf(fails ? "t_lshud_vk: %d FAILED\n" : "t_lshud_vk: all passed\n", fails);
    return fails != 0;
}

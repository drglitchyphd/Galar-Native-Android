// SPDX-License-Identifier: GPL-3.0-or-later
// nvn_lsfg.h's HUD pass (ls_hud, overlay/ls_hud.comp, ours) on a real Vulkan driver (lavapipe is enough), without the Lossless Scaling shaders: a made-up in-between
// picture, the newest frame's scene and the frame as shown (that scene with HUD boxes drawn on it). Expected out: the HUD boxes exactly as shown, everywhere else the
// in-between picture untouched; sRGB frames handled on their raw values (the pass sees UNORM views).
//   cc -O1 -I<vulkan headers> -o /tmp/t_lshud_vk t_lshud_vk.c -l:libvulkan.so.1 -lm && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json /tmp/t_lshud_vk
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "../nvn/nvn_lsfg.h"
#define W 320
#define H 200
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int inbox(int x, int y) { return (x >= 20 && x < 120 && y >= 10 && y < 40) || (x >= 200 && x < 300 && y >= 150 && y < 190); }
int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };   // (1.1: the sRGB frame's image takes a UNORM storage view, as on the phone)
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("SKIP: no Vulkan instance\n"); return 0; }
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd); if (!n) { printf("SKIP: no Vulkan device\n"); return 0; }
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);   // (the RGBA8 frames here take the module that names its format: no feature needed)
    VkPhysicalDeviceFeatures en = { .shaderStorageImageWriteWithoutFormat = f.shaderStorageImageWriteWithoutFormat };
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
    for (int fi = 0; fi < 2; fi++) {
        static lsfg G; memset(&G, 0, sizeof G); G.dev = dev; G.pd = pd;
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        vkCreateSampler(dev, &sci, NULL, &G.samp[LS_S_EDGE]);
        G.hbase = ls_hud_base(&G); CHECK(G.hbase, "the HUD pass's pipeline was not made"); if (!G.hbase) break;
        VkFormat fmt = fmts[fi], u = ls_unorm(fmt); int ok = 1;
        for (int k = 0; k < LS_MAXGEN; k++) ok = ok && ls_img_make(&G, &G.m[k], u, W, H, fmt);
        ok = ok && ls_img_make(&G, &G.c[0], u, W, H, fmt) && ls_img_make(&G, &G.c[1], u, W, H, fmt) && ls_img_make(&G, &G.hud, u, W, H, fmt) && ls_img_make(&G, &G.o, u, W, H, fmt);
        G.hpipe = ls_hud_pipe(&G, u); CHECK(G.hpipe, "no HUD pipeline for an RGBA8 frame"); if (!G.hpipe) break;
        CHECK(ok, "images"); if (!ok) break;
        G.w = W; G.h = H; G.fresh = 1; ls_hud_write(&G); CHECK(ls_hud_ok(&G), "ls_hud_ok");
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 4; c++) { int i = (y * W + x) * 4 + c;
            uint8_t sc = (uint8_t)((x * 3 + y * 5 + c * 40) & 255);
            map[0][i] = c == 3 ? 255 : (uint8_t)((x * 7 + y * 2 + c * 11) & 255);              // the in-between picture: something else entirely
            map[1][i] = c == 3 ? 255 : sc;                                                      // the newest frame's scene
            map[2][i] = c == 3 ? 255 : inbox(x, y) ? (uint8_t)(c == 0 ? 250 : sc ^ 0x80) : sc; }   // shown: the scene with the HUD boxes on it
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &ai, &cb); VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
        ls_begin(&G, cb);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        const VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        G.p = 1; int gen = 1;   // (the second set row, a picture other than the first: the indexing as nvn_vk.c uses it)
        vkCmdCopyBufferToImage(cb, buf[0], G.m[gen].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc); vkCmdCopyBufferToImage(cb, buf[1], G.c[1].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        vkCmdCopyBufferToImage(cb, buf[2], G.hud.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        ls_hud(&G, cb, gen);
        vkCmdCopyImageToBuffer(cb, G.o.img, VK_IMAGE_LAYOUT_GENERAL, buf[3], 1, &bc);
        vkEndCommandBuffer(cb); VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHECK(!vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) && !vkQueueWaitIdle(q), "submit");
        int bad_hud = 0, bad_pic = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) { int i = (y * W + x) * 4 + c, want = inbox(x, y) ? map[2][i] : map[0][i];
            if (abs(map[3][i] - want) > 0) { if (inbox(x, y)) bad_hud++; else bad_pic++; } }
        printf("%s: HUD pixels wrong %d, picture pixels changed %d\n", fi ? "sRGB frame" : "UNORM frame", bad_hud, bad_pic);
        CHECK(bad_hud == 0 && bad_pic == 0, "%s: HUD %d, picture %d", fi ? "sRGB" : "UNORM", bad_hud, bad_pic);
        vkFreeCommandBuffers(dev, pool, 1, &cb); ls_images_free(&G); }
    printf(fails ? "t_lshud_vk: %d FAILED\n" : "t_lshud_vk: all passed\n", fails);
    return fails != 0;
}

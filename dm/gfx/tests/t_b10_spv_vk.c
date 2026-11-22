// The stand-in's store rule (nvn_spv.c bq_emit, through spv_b10_image_writes) run by a real Vulkan driver over millions of values, checked against b10_conv.h:
//   t_b10_spv_vk <store.spv>   (store.spv: t_b10_spv.sh's compute shader, imageStore into an r11f_g11f_b10f image of each value it reads from a buffer)
// The module is patched as nvn_vk.c shader_load patches it with DM_VK_B10_AS_RGBA16 on (spv_b10_image_writes, then the declaration made Rgba16f) and writes into an
//   RGBA16F image; every half read back must be b10_chan_to_half (b10_float_to_chan (value)) (a NaN any NaN) and every alpha 1. The same module unpatched (only
//   re-declared) must differ: the check sees what it checks. On the PC: lavapipe (VK_ICD_FILENAMES), headers from the NDK (t_b10_spv.sh). Exit 1 on a failure
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include "nvn_spv.h"
#include "../tex/b10_conv.h"
#define VK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s: %d\n", #x, r_); exit(1); } } while (0)
enum { W = 1024, H = 1024 };
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue queue; static uint32_t qf;
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type\n"); exit(1);
}
static VkBuffer buffer(VkDeviceSize n, VkBufferUsageFlags u, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = u }; VkBuffer b; VK(vkCreateBuffer(dev, &bi, NULL, &b));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, b, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &ai, NULL, m)); VK(vkBindBufferMemory(dev, b, *m, 0)); VK(vkMapMemory(dev, *m, 0, n, 0, map));
    return b;
}
// the values: specials, every packed code of both channel kinds, every midpoint between neighbours (the ties) and one float ulp either side, the range's edges,
//   then random bits with exponents around the packed range (and some anywhere)
static uint32_t rng = 0x12345678u; static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t bitsf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void values(float *v, size_t n) {
    size_t k = 0;
    static const uint32_t sp[] = { 0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7fbfffffu, 0x00000001u, 0x807fffffu, 0x7f7fffffu, 0xff7fffffu,
        0x477E0000u, 0x477E0001u, 0x477DFFFFu, 0x477C0000u, 0x477C0001u, 0x477F0000u, 0x477FE000u, 0x477FF000u, 0x47800000u, 0x3f800000u, 0xbf800000u, 0x38800000u, 0x35800000u, 0x35000000u, 0x35000001u };
    for (size_t i = 0; i < sizeof sp / sizeof *sp; i++) v[k++] = fbits(sp[i]);
    for (int mb = 5; mb <= 6; mb++) for (uint32_t c = 0; c < (31u << mb); c++) {   // every finite code, the midpoint to the next, and a float ulp either side of it
        float a = b10_chan_to_float(c, mb), b = b10_chan_to_float(c + 1, mb), mid = (a + b) / 2;   // (exact: a, b are halves)
        v[k++] = a; if (c + 1 < (31u << mb)) { v[k++] = mid; v[k++] = fbits(bitsf(mid) + 1); v[k++] = fbits(bitsf(mid) - 1); v[k++] = -a; } }
    while (k < n) { uint32_t r = rnd(), u = rnd() % 8 ? (r & 0x807fffffu) | ((96u + rnd() % 52u) << 23) : r; v[k++] = fbits(u); }
}
static int run(const uint32_t *code, size_t nw, const float *in, uint16_t *out) {
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = code }; VkShaderModule sm; VK(vkCreateShaderModule(dev, &mi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb }; VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &pi, NULL, &pl));
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = pl };
    VkPipeline p; VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage img; VK(vkCreateImage(dev, &ii, NULL, &img)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(dev, img, &rq);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mem_type(rq.memoryTypeBits, 0) }; VkDeviceMemory im; VK(vkAllocateMemory(dev, &ai, NULL, &im)); VK(vkBindImageMemory(dev, img, im, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView iv; VK(vkCreateImageView(dev, &vi, NULL, &iv));
    VkDeviceMemory bm, om; void *bmap, *omap;
    VkBuffer ib = buffer((VkDeviceSize)W * H * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &bm, &bmap), ob = buffer((VkDeviceSize)W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &om, &omap);
    memcpy(bmap, in, (size_t)W * H * 16);
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps }; VkDescriptorPool dp; VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }; VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorImageInfo dii = { VK_NULL_HANDLE, iv, VK_IMAGE_LAYOUT_GENERAL }; VkDescriptorBufferInfo dbi = { ib, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wd[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii },
                                   { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi } };
    vkUpdateDescriptorSets(dev, 2, wd, 0, NULL);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf }; VkCommandPool cp; VK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }; VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; VK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier b0 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    uint32_t w = W; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &w); vkCmdDispatch(cb, W / 8, H / 8, 1);
    VkImageMemoryBarrier b1 = b0; b1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b1.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, ob, 1, &rg);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb }; VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(queue));
    memcpy(out, omap, (size_t)W * H * 8);
    vkDestroyCommandPool(dev, cp, NULL); vkDestroyDescriptorPool(dev, dp, NULL); vkDestroyBuffer(dev, ib, NULL); vkDestroyBuffer(dev, ob, NULL); vkFreeMemory(dev, bm, NULL); vkFreeMemory(dev, om, NULL);
    vkDestroyImageView(dev, iv, NULL); vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, im, NULL); vkDestroyPipeline(dev, p, NULL); vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL); vkDestroyShaderModule(dev, sm, NULL);
    return 0;
}
static int is_nan_h(uint16_t h) { return (h >> 10 & 31) == 31 && (h & 1023); }
// texels that differ from the packed format's rule (and alpha not 1); prints the first few
static long check(const float *in, const uint16_t *out, int verbose) {
    long bad = 0;
    for (size_t t = 0; t < (size_t)W * H; t++) {
        int ok = out[4 * t + 3] == 0x3c00;
        for (int c = 0; c < 3; c++) { int mb = c == 2 ? B10_MB : B10_MR; uint16_t want = b10_chan_to_half(b10_float_to_chan(in[4 * t + c], mb), mb), got = out[4 * t + c];
            if (is_nan_h(want) ? !is_nan_h(got) : got != want) ok = 0; }
        if (!ok && ++bad <= 6 && verbose) { printf("  texel %zu:", t); for (int c = 0; c < 4; c++) printf(" %#010x -> %#06x (want %#06x)", bitsf(in[4 * t + c]), out[4 * t + c], c < 3 ? b10_chan_to_half(b10_float_to_chan(in[4 * t + c], c == 2 ? B10_MB : B10_MR), c == 2 ? B10_MB : B10_MR) : 0x3c00); printf("\n"); }
    }
    return bad;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: t_b10_spv_vk store.spv\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("FAIL no %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *code = malloc((size_t)n); if (fread(code, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    size_t pw; int left = 0; uint32_t *patched = spv_b10_image_writes(code, (size_t)n / 4, &pw, &left);
    if (!patched || left) { printf("FAIL the store was not patched (left %d)\n", left); return 1; }
    if (spv_image_format_swap(patched, pw, SPV_IMGFMT_R11G11B10F, SPV_IMGFMT_RGBA16F) != 1 || spv_image_format_swap(code, (size_t)n / 4, SPV_IMGFMT_R11G11B10F, SPV_IMGFMT_RGBA16F) != 1) { printf("FAIL the declaration was not swapped\n"); return 1; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app }; VkInstance inst; VK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t one = 1; vkEnumeratePhysicalDevices(inst, &one, &pd); if (!one) { printf("FAIL no Vulkan device\n"); return 1; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qf = 0; qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT); qf++) {}
    float pri = 1; VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &pri };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi }; VK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, qf, 0, &queue);
    float *in = malloc((size_t)W * H * 16); uint16_t *out = malloc((size_t)W * H * 8); size_t nv = (size_t)W * H; float *v = malloc(nv * 4); values(v, nv);
    // R and B each meet every value (both channel rules see the whole set), G the same set shifted; the alpha written is 0.25 or a NaN (read back: 1)
    for (size_t t = 0; t < nv; t++) { in[4 * t] = v[t]; in[4 * t + 1] = v[(t + 333331) % nv]; in[4 * t + 2] = v[t]; in[4 * t + 3] = (t & 1) ? 0.25f : fbits(0xffc00000u); }
    run(patched, pw, in, out); long bad = check(in, out, 1);
    run(code, (size_t)n / 4, in, out); long raw = check(in, out, 0);
    printf("%s t_b10_spv_vk on %s: %ld of %d texels (3 values and alpha each) differ from R11G11B10F's rule with the store patched; unpatched %ld differ (must be many)\n",
           !bad && raw > W * H / 4 ? "PASS" : "FAIL", pp.deviceName, bad, W * H, raw);
    return !bad && raw > W * H / 4 ? 0 : 1;
}

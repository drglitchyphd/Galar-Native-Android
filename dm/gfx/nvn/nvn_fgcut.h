// Camera cuts and still frames for frame generation (both kinds: ours, nvn_fgen.h, and Lossless Scaling's, nvn_lsfg.h).
//   fgc_stat:   the two frames an in-between picture goes between, compared on a coarse grid on the GPU (overlay/fg_cut.comp): a cut flag and how much moved
//   fgc_select: after a cut, the picture becomes the newer real frame (overlay/fg_cutsel.comp; nothing happens without one). The picture blended across a cut is the
//               most visible fault of frame generation, and Pokémon battles cut the camera all the time
//   fgc_read:   the slot's numbers back on the CPU once its fence has signalled: a pair that barely moved (a menu, a loading screen) lets the caller leave the
//               next pictures out
// Per slot (the caller's frames in flight, FGC_SLOTS): its own result words and descriptor sets, written while the slot's earlier work is known to be done.
// Needs storage image writes without a format (Lossless Scaling's chain needs it anyway); fgc_init says no otherwise and nothing else changes
#pragma once
#include <stdint.h>
#include <string.h>
#include "overlay/fgcut_spv.h"
#define FGC_SLOTS 3
#define FGC_PICS 3
typedef struct { uint32_t flag, changed, cells, sum; } fgc_result;
typedef struct {
    VkDevice dev; int ok, can_select; const char *why;
    VkSampler samp; VkDescriptorSetLayout dsl[2]; VkPipelineLayout pl[2]; VkPipeline pipe[2]; VkDescriptorPool dp;
    VkBuffer buf; VkDeviceMemory mem; uint8_t *map; uint32_t stride;
    VkDescriptorSet ds_stat[FGC_SLOTS], ds_sel[FGC_SLOTS][FGC_PICS];
    float cell_diff, cut_share, cut_mean; int on;   // DM_FG_CUT=0: no cut handling (the still count still runs)
} fgc;
static int fgc_fail(fgc *c, const char *why) { c->why = why; c->ok = 0; return 0; }
static int fgc_pipe(fgc *c, int i, const uint32_t *code, size_t sz, const VkDescriptorSetLayoutBinding *b, uint32_t nb, uint32_t push) {
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b };
    if (vkCreateDescriptorSetLayout(c->dev, &dli, NULL, &c->dsl[i])) return 0;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl[i], .pushConstantRangeCount = push ? 1 : 0, .pPushConstantRanges = &pr };
    if (vkCreatePipelineLayout(c->dev, &pli, NULL, &c->pl[i])) return 0;
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code }; VkShaderModule mod;
    if (vkCreateShaderModule(c->dev, &mi, NULL, &mod)) return 0;
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = c->pl[i],
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" } };
    VkResult r = vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &ci, NULL, &c->pipe[i]); vkDestroyShaderModule(c->dev, mod, NULL);
    return r == VK_SUCCESS;
}
// Once: 1 ready (c->can_select: cuts can be replaced, else only counted), 0 with c->why
static int fgc_init(fgc *c, VkDevice dev, VkPhysicalDevice pd) {
    memset(c, 0, sizeof *c); c->dev = dev;
    { const char *e = getenv("DM_FG_CUT"); c->on = !(e && !strcmp(e, "0")); }
    // (tuned on the PC score, fg_score: a pan at walking speed changes about a third of the cells by less than 0.08; a battle's camera cut over 70 % by 0.15 and more)
    { const char *e = getenv("DM_FG_CUT_TUNE"); float a, b, d; c->cell_diff = 0.08f; c->cut_share = 0.6f; c->cut_mean = 0.07f;
      if (e && sscanf(e, "%f,%f,%f", &a, &b, &d) == 3 && a > 0 && b > 0 && b <= 1 && d >= 0) { c->cell_diff = a; c->cut_share = b; c->cut_mean = d; } }
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (vkCreateSampler(dev, &sci, NULL, &c->samp)) return fgc_fail(c, "no sampler");
    VkDescriptorSetLayoutBinding bs[3] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                           { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    if (!fgc_pipe(c, 0, fg_cut_comp_spv, sizeof fg_cut_comp_spv, bs, 3, 16)) return fgc_fail(c, "the driver could not build the cut check");
    c->can_select = f.shaderStorageImageWriteWithoutFormat;
    VkDescriptorSetLayoutBinding bl[3] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                           { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    if (c->can_select && !fgc_pipe(c, 1, fg_cutsel_comp_spv, sizeof fg_cutsel_comp_spv, bl, 3, 0)) c->can_select = 0;
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FGC_SLOTS * (2 + FGC_PICS) }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FGC_SLOTS * (1 + FGC_PICS) },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FGC_SLOTS * FGC_PICS } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FGC_SLOTS * (1 + FGC_PICS), .poolSizeCount = 3, .pPoolSizes = ps };
    if (vkCreateDescriptorPool(dev, &dpi, NULL, &c->dp)) return fgc_fail(c, "no descriptor pool");
    for (int s = 0; s < FGC_SLOTS; s++) {
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp, .descriptorSetCount = 1, .pSetLayouts = &c->dsl[0] };
        if (vkAllocateDescriptorSets(dev, &ai, &c->ds_stat[s])) return fgc_fail(c, "no descriptor sets");
        if (c->can_select) for (int k = 0; k < FGC_PICS; k++) { ai.pSetLayouts = &c->dsl[1]; if (vkAllocateDescriptorSets(dev, &ai, &c->ds_sel[s][k])) c->can_select = 0; } }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); VkDeviceSize al = pp.limits.minStorageBufferOffsetAlignment; c->stride = al > 64 ? (uint32_t)al : 64;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)FGC_SLOTS * c->stride, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    if (vkCreateBuffer(dev, &bci, NULL, &c->buf)) return fgc_fail(c, "no buffer");
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, c->buf, &rq); VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp); uint32_t t = 0;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (t < mp.memoryTypeCount && !((rq.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & hv) == hv)) t++;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = t }; void *m = NULL;
    if (t == mp.memoryTypeCount || vkAllocateMemory(dev, &mai, NULL, &c->mem) || vkBindBufferMemory(dev, c->buf, c->mem, 0) || vkMapMemory(dev, c->mem, 0, VK_WHOLE_SIZE, 0, &m))
        return fgc_fail(c, "no memory for its results");
    c->map = m; memset(c->map, 0, (size_t)FGC_SLOTS * c->stride);
    c->ok = 1; return 1;
}
// Records the comparison of a (older) and b (newer) into slot s; both views sampled in the GENERAL layout, written before (the caller's barrier)
static void fgc_stat(fgc *c, VkCommandBuffer cb, int s, VkImageView a, VkImageView b) {
    if (!c->ok) return;
    VkDescriptorImageInfo ii[2] = { { c->samp, a, VK_IMAGE_LAYOUT_GENERAL }, { c->samp, b, VK_IMAGE_LAYOUT_GENERAL } };
    VkDescriptorBufferInfo bi = { c->buf, (VkDeviceSize)s * c->stride, sizeof(fgc_result) };
    VkWriteDescriptorSet w[3];
    for (uint32_t j = 0; j < 3; j++) w[j] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds_stat[s], .dstBinding = j, .descriptorCount = 1,
        .descriptorType = j < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pImageInfo = &ii[j < 2 ? j : 0], .pBufferInfo = &bi };
    vkUpdateDescriptorSets(c->dev, 3, w, 0, NULL);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    float pc[4] = { c->cell_diff, c->cut_share, c->cut_mean, c->on ? 1.0f : 0.0f };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipe[0]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl[0], 0, 1, &c->ds_stat[s], 0, NULL);
    vkCmdPushConstants(cb, c->pl[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// After slot s's fgc_stat and the picture's own pass: picture k (dst, w x h, a storage view, GENERAL) becomes real (sampled view of the same size) if a cut was found
static void fgc_select(fgc *c, VkCommandBuffer cb, int s, int k, VkImageView real, VkImageView dst, uint32_t w, uint32_t h) {
    if (!c->ok || !c->can_select || !c->on || k < 0 || k >= FGC_PICS) return;
    VkDescriptorImageInfo ii[2] = { { c->samp, real, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL } };
    VkDescriptorBufferInfo bi = { c->buf, (VkDeviceSize)s * c->stride, sizeof(fgc_result) };
    VkWriteDescriptorSet w3[3];
    for (uint32_t j = 0; j < 3; j++) w3[j] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds_sel[s][k], .dstBinding = j, .descriptorCount = 1,
        .descriptorType = j == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : j == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[j ? j - 1 : 0], .pBufferInfo = &bi };
    vkUpdateDescriptorSets(c->dev, 3, w3, 0, NULL);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipe[1]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl[1], 0, 1, &c->ds_sel[s][k], 0, NULL);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// Slot s's numbers (after its fence): 1 when they are there; *mean: the average brightness change of a cell (0..1)
static int fgc_read(fgc *c, int s, fgc_result *r, float *mean) {
    if (!c->ok) return 0;
    memcpy(r, c->map + (size_t)s * c->stride, sizeof *r);
    if (!r->cells) return 0;
    if (mean) *mean = (float)r->sum / 1000.0f / (float)r->cells;
    memset(c->map + (size_t)s * c->stride, 0, sizeof *r);   // (read once)
    return 1;
}

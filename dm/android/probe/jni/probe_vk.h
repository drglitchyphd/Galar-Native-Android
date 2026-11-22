// The self-tests' Vulkan functions, loaded at run time from a driver of our choice (pv_load): the system's libvulkan, or the custom driver the game will use
//   (DM_VK_DRIVER: Mesa Turnip / PanVK packages, opened through libadrenotools as nvn_vk.c vk_open does), so the checks test the driver the game runs on.
// Each vkX below is a macro for the pointer pv_vkX: selftest.c is written against plain Vulkan names, and probe.c (which links libvulkan) is not affected.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define PV_PRE(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties)
#define PV_INST(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkGetPhysicalDeviceExternalBufferProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkBindImageMemory) \
    X(vkCmdBeginRenderPass) X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdClearAttachments) X(vkCmdCopyBuffer) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdDispatch) X(vkCmdDraw) X(vkCmdEndRenderPass) X(vkCmdFillBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdPushConstants) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias) X(vkCmdSetScissor) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilWriteMask) X(vkCmdSetViewport) X(vkCmdClearDepthStencilImage) X(vkCmdClearColorImage) \
    X(vkCreateBuffer) X(vkCreateCommandPool) X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) \
    X(vkCreateFence) X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateImage) X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateRenderPass) \
    X(vkCreateSampler) X(vkCreateShaderModule) X(vkDestroyBuffer) X(vkDestroyCommandPool) X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDevice) X(vkDestroyFence) X(vkDestroyFramebuffer) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) \
    X(vkDestroyRenderPass) X(vkDestroySampler) X(vkDestroyShaderModule) X(vkEndCommandBuffer) X(vkFlushMappedMemoryRanges) X(vkFreeCommandBuffers) \
    X(vkFreeMemory) X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) X(vkGetImageMemoryRequirements) X(vkInvalidateMappedMemoryRanges) X(vkMapMemory) \
    X(vkQueueSubmit) X(vkUnmapMemory) X(vkUpdateDescriptorSets) X(vkWaitForFences) X(vkDeviceWaitIdle)

#define PV_DECL(f) extern PFN_##f pv_##f;
PV_PRE(PV_DECL) PV_INST(PV_DECL)
#undef PV_DECL
// (each name → its pointer)
#define vkCreateInstance pv_vkCreateInstance
#define vkEnumerateInstanceVersion pv_vkEnumerateInstanceVersion
#define vkEnumerateInstanceExtensionProperties pv_vkEnumerateInstanceExtensionProperties
#include "probe_vk_names.h"

// Opens the driver (driver: a DM_VK_DRIVER value, or NULL / "" for the system's) and loads the functions that need no instance; *how: what was opened.
//   pv_load_instance: the rest, once the instance exists. 0 = ok
int pv_load(const char *driver, const char **how);
void pv_load_instance(VkInstance inst);
PFN_vkVoidFunction pv_instance_proc(VkInstance inst, const char *name);   // (one the lists above do not name: an extension's, under its own name)

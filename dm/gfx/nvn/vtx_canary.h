// Vertex canary (included by nvn_vk.c; run once by mtl_init after the device is made, before the game draws anything).
//
// With extended dynamic state the engine gives each draw's vertex sizes (strides) to the driver at draw time (vkCmdBindVertexBuffers2) instead of building them into
// the pipeline. Qualcomm's older drivers read those wrong, and Legends: Arceus came out as exploded, stretched polygons across the view (Arceus-Native-Android #34
// #46, Galar #209); v0.39 turns that off by the driver's version, but a driver that reports a newer version and still gets it wrong would slip through.
// This draws two squares with one pipeline, each from vertex data with its own stride (16 and 28 bytes) given per draw, as the game's draws do. Every byte around each
// vertex's position holds 1e30, so a stride read wrong puts corners far off the image instead of near where they belong. A 32x32 image is read back:
//   both squares filled, the gap between them and the border empty = right; anything else = wrong, and the draw state goes into each pipeline from then on (g_eds 0,
//   the path drivers without extended dynamic state take), noted in the log. A few milliseconds once per start. DM_VK_CANARY=0 skips it; DM_VK_NO_EDS set by hand
// decides on its own. A start that dies inside it leaves a marker and later starts on that driver skip it.
#include "vtx_canary_spv.h"

// one draw's worth of vertex data: six positions (two triangles) of the square x0..x1 (y -0.5..0.5) at byte 4 of each stride-sized vertex, the rest 1e30
static void vcan_fill(uint8_t *d, uint32_t stride, float x0, float x1) {
    const float q[6][2] = { { x0, -0.5f }, { x1, -0.5f }, { x1, 0.5f }, { x0, -0.5f }, { x1, 0.5f }, { x0, 0.5f } };
    for (uint32_t i = 0; i < 6 * stride / 4; i++) { float big = 1e30f; memcpy(d + 4 * i, &big, 4); }
    for (int v = 0; v < 6; v++) memcpy(d + v * stride + 4, q[v], 8);
}

static void vtx_canary(void) {
    if (!g_eds) return;
    { const char *e = getenv("DM_VK_CANARY"); if (e && !atoi(e)) return; }
    if (getenv("DM_VK_NO_EDS")) return;   // (chosen by hand: left as it is)
    if (mark_file(drv_mark("vtx_canary_crashed"), "", 0)) { VLOG("[RED] vertex canary: the driver crashed in it at an earlier start: skipped (delete vtx_canary_crashed to run it again)"); return; }
    mark_file(drv_mark("vtx_canary_crashed"), "running the vertex canary; if this file is still here the driver crashed in it, and later starts skip it\n", 1);
    enum { W = 32, H = 32, S0 = 16, S1 = 28 };
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage img = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory imem = VK_NULL_HANDLE, bmem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE; uint8_t *map = NULL; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline p = VK_NULL_HANDLE; VkCommandPool cp = VK_NULL_HANDLE;
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE; int bad = -1, inside = 0, outside = 0;
    // the image (drawn into, then copied out) and one host buffer: vertex data at 0 and 1024, the read-back at 4096
    { VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
      if (vkCreateImage(g_dev, &ic, NULL, &img)) { img = VK_NULL_HANDLE; goto out; }
      VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, img, &rq); int t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)(t < 0 ? 0 : t) };
      if (t < 0 || vkAllocateMemory(g_dev, &ai, NULL, &imem) || vkBindImageMemory(g_dev, img, imem, 0)) goto out;
      VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      if (vkCreateImageView(g_dev, &vi, NULL, &view)) { view = VK_NULL_HANDLE; goto out; } }
    { VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096 + W * H * 4, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
      if (vkCreateBuffer(g_dev, &bci, NULL, &buf)) { buf = VK_NULL_HANDLE; goto out; }
      VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, buf, &rq);
      int t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0); if (t < 0) goto out;
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
      void *m = NULL; if (vkAllocateMemory(g_dev, &ai, NULL, &bmem) || vkBindBufferMemory(g_dev, buf, bmem, 0) || vkMapMemory(g_dev, bmem, 0, VK_WHOLE_SIZE, 0, &m)) goto out;
      map = m; memset(map, 0, 4096 + W * H * 4);
      vcan_fill(map, S0, -0.75f, -0.25f); vcan_fill(map + 1024, S1, 0.25f, 0.75f); }
    // the pipeline: one binding (stride given per draw), position at offset 4, the draw state the engine's own pipelines take as dynamic
    { VkShaderModuleCreateInfo mv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vcan_vs, .pCode = vcan_vs }, mf = mv;
      mf.codeSize = sizeof vcan_fs; mf.pCode = vcan_fs;
      if (vkCreateShaderModule(g_dev, &mv, NULL, &vm) || vkCreateShaderModule(g_dev, &mf, NULL, &fm)) goto out;
      VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
      if (vkCreatePipelineLayout(g_dev, &pi, NULL, &pl)) goto out;
      VkPipelineShaderStageCreateInfo st[2] = { { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
                                                { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
      VkVertexInputBindingDescription vb = { 0, S0, VK_VERTEX_INPUT_RATE_VERTEX };
      VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 4 };
      VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                                   .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &va };
      VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
      VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
      VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
      VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
      VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
      VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
      VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                               VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE };
      VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 6, .pDynamicStates = dyn };
      VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
      VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
          .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pl };
      if (!g_dynren) { gi.pNext = NULL; gi.renderPass = rp_for_pipeline(1, &fmt, 0, 1, 1); gi.subpass = 0; }
      if (vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gi, NULL, &p)) { p = VK_NULL_HANDLE; goto out; } }
    { VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_qf };
      if (vkCreateCommandPool(g_dev, &ci, NULL, &cp)) { cp = VK_NULL_HANDLE; goto out; } }
    { VkCommandBuffer cmd; VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
      if (vkAllocateCommandBuffers(g_dev, &cai, &cmd)) goto out;
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
      vkBeginCommandBuffer(cmd, &bi);
      VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };   // (cleared to 0)
      VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
      uint32_t cf = (uint32_t)fmt; rend_begin(cmd, &ri, &cf, 0, 1, 0);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
      VkViewport v = { 0, 0, W, H, 0, 1 }; VkRect2D sc = {{0, 0}, {W, H}}; vkCmdSetViewport(cmd, 0, 1, &v); vkCmdSetScissor(cmd, 0, 1, &sc);
      vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE); vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE); vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
      VkDeviceSize o0 = 0, o1 = 1024, s0 = S0, s1 = getenv("DM_VK_CANARY_FAKE") ? S0 : S1;   // (DM_VK_CANARY_FAKE: the second stride given wrong, as a broken driver reads it: proves the check)
      vkCmdBindVertexBuffers2(cmd, 0, 1, &buf, &o0, NULL, &s0); vkCmdDraw(cmd, 6, 1, 0, 0);   // the left square, 16-byte vertices
      vkCmdBindVertexBuffers2(cmd, 0, 1, &buf, &o1, NULL, &s1); vkCmdDraw(cmd, 6, 1, 0, 0);   // the right one, 28-byte vertices, same pipeline
      rend_end(cmd);
      ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      VkBufferImageCopy bc = { .bufferOffset = 4096, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
      vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
      VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
      vkEndCommandBuffer(cmd);
      VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f;
      if (!vkCreateFence(g_dev, &fi, NULL, &f)) {
          VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
          pthread_mutex_lock(&g_qmu); VkResult e = vkQueueSubmit(g_q, 1, &si, f); pthread_mutex_unlock(&g_qmu);
          if (!e && vkWaitForFences(g_dev, 1, &f, VK_TRUE, 2000000000ull) == VK_SUCCESS) {
              // squares at pixels x 4..11 and 20..27, y 8..23; checked well inside (x 6..9, 22..25; y 11..20) and well outside (the border 3 wide, the gap x 14..17)
              const uint8_t *px = map + 4096; bad = 0;
              for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                  int red = px[(y * W + x) * 4] > 128, in = y >= 11 && y <= 20 && ((x >= 6 && x <= 9) || (x >= 22 && x <= 25));
                  int out = x < 3 || x >= W - 3 || y < 3 || y >= H - 3 || (x >= 14 && x <= 17);
                  if (in) { inside++; bad += !red; } else if (out) { outside++; bad += red; } }
          }
          vkDestroyFence(g_dev, f, NULL);
      } }
out:
    if (cp) vkDestroyCommandPool(g_dev, cp, NULL); if (p) vkDestroyPipeline(g_dev, p, NULL); if (pl) vkDestroyPipelineLayout(g_dev, pl, NULL);
    if (vm) vkDestroyShaderModule(g_dev, vm, NULL); if (fm) vkDestroyShaderModule(g_dev, fm, NULL);
    if (view) vkDestroyImageView(g_dev, view, NULL); if (img) vkDestroyImage(g_dev, img, NULL); if (imem) vkFreeMemory(g_dev, imem, NULL);
    if (buf) vkDestroyBuffer(g_dev, buf, NULL); if (bmem) vkFreeMemory(g_dev, bmem, NULL);
    mark_file(drv_mark("vtx_canary_crashed"), "", -1);
    if (bad < 0) { VLOG("vertex canary: could not be set up (draw state stays per draw)"); return; }
    if (!bad) { VLOG("vertex canary: two squares drawn with vertex sizes given per draw came out right (%d / %d pixels checked)", inside + outside, inside + outside); return; }
    g_eds = 0;
    VLOG("[RED] vertex canary: %d of %d checked pixels wrong with vertex sizes given per draw (this driver misreads them: stretched or exploding polygons): "
         "draw state built into each pipeline from now on (DM_VK_NO_EDS=0 forces the per-draw way, DM_VK_CANARY=0 skips this check)", bad, inside + outside);
}

// GPU self-tests (the probe app, and the driver check of Galar / Arceus / Paldea Native): what the engine (dm/gfx/nvn/nvn_vk.c) asks of the phone's own Vulkan
// driver, recreated with synthetic data.
// No game files, no keys: safe to run on any phone, including shared ones (Samsung Remote Test Lab, Firebase Test Lab, AWS Device Farm).
// Each test mirrors one engine path and prints one line: [PASS] / [WARN] / [FAIL] / [SKIP] / [INFO] name: what it means for the game.
//   float-controls      nvn_vk.c mtl_init: shaders patched by spv_float_controls (SignedZeroInfNanPreserve); NaN / infinity checks with and without it
//   shader-maths        spv_nan_minmax / spv_nan_select: NaN in min / max / saturate, pow as exp2(log2), integers carried in float variables, four ways
//   end-of-submit-barrier  barrier_host: shader writes the CPU reads after the fence, with and without the barrier, coherent and cached memory
//   reinterpreted-format   MUTABLE_FORMAT textures written as one format and sampled as another (compressed render targets: AFBC, DCC)
//   3d-render-target    att_view: a 3D image's slices attached as a layered 2D array (the colour-grading tables; black 3D scenes when only slice 0 drew), recorded
//                       as the engine records its passes on this driver (dynamic rendering, or render passes: Q_NO_DYNREN), and the clear inside that pass as
//                       clear_in_pass records it there (its layer 0, a part of it; Q_CLEAR_BY_PASS: on Xclipse a pass of its own on that slice)
//   dmabuf-heap         the heap route: /dev/dma_heap memory mapped at a fixed address and imported into the driver (the Mali / Xclipse memory pools)
//   bc-textures         the BC layer (gfx/tex/bc_gpu.c, linked as is): BC1-BC7 converted on the GPU only where it matches the CPU bit for bit
//   depth-copy          nvn_vk.c zcopy_draw: a scaled copy into a depth / stencil image drawn (gl_FragDepth, the stencil one bit a draw), read back
//   int-view            nvn_vk.c tex_view_for / fmt_as_class: 32-bit integer textures read through a view of the shader's class (the same bits)
//   int-view-own        tex_view_for where it keeps the texture's own format (an image whose formats are fixed, a 16-bit integer read as float or as the other
//                       signedness): what this driver returns for that old binding, undefined in Vulkan (information only)
// A crash inside the driver (SIGSEGV / SIGBUS / SIGABRT) is caught: that test reports it and the remaining GPU tests are skipped.
// The same tests run inside the games (dprobe_jni.c, the app's driver check before Play): there in a process of their own, on the driver the game will use
//   (probe_vk.h), each one noted in a journal before it starts, so one that takes the process down is known; a test names the setting that avoids what it found
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "probe_vk.h"
#include "bc_gpu.h"
#include "nvn_spv.h"
#include "vk_quirks.h"   // (the engine's own per-GPU rules: what it asks of this driver by default)
#include "selftest_spv.h"
#include "overlay/zcopy_spv.h"

void probe_vout(const char *fmt, va_list ap);   // probe.c (the probe app) / dprobe_jni.c (the games)
static void O(const char *fmt, ...) { va_list ap; va_start(ap, fmt); probe_vout(fmt, ap); va_end(ap); }

// The journal (the games' driver check): "begin <test> [crash fix]" before a test (and again before a later step of it whose crash asks for another setting:
//   crash_fix; the app applies the last one a lost process named), "result <test> <TAG> <text>" after, "fix <test> KEY=VALUE" for a setting it asks for, "done" at
//   the end; written straight to the file (no buffering) so whatever the process managed to say survives it. -1: none (the probe app)
static int g_jfd = -1;
static void J(const char *fmt, ...) {
    if (g_jfd < 0) return;
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n < 0) return; if (n >= (int)sizeof b) n = sizeof b - 1;
    for (int i = 0; i < n - 1; i++) if (b[i] == '\n') b[i] = ' ';   // (one line each)
    if (write(g_jfd, b, (size_t)n) < 0) { } fsync(g_jfd);
}
static void FIX(const char *name, const char *kv) { O("    -> %s\n", kv); J("fix %s %s\n", name, kv); }

enum { PASS, WARN, FAIL, SKIP, INFO };
static int g_count[5];
static void R(int st, const char *name, const char *fmt, ...) {
    static const char *tag[] = { "PASS", "WARN", "FAIL", "SKIP", "INFO" };
    char b[900]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    g_count[st]++; O("[%s] %s: %s\n", tag[st], name, b);
    J("result %s %s %s\n", name, tag[st], b);
}

// ---- crash guard ----
static sigjmp_buf g_jb; static volatile sig_atomic_t g_sig;
static const char *volatile g_step;   // what a test was doing (named in the report when the driver crashes there)
static const char *volatile g_fix;    // the setting that keeps the game off the path the running test is on (its T[].crash; crash_fix for a later step)
static void on_crash(int s) { g_sig = s; siglongjmp(g_jb, 1); }
// From here on a crash in test <name> asks for kv (NULL: nothing), not for its T[].crash: named in the journal ("begin" again: the app applies the last setting a lost
//   process named) and applied by run_tests when the guard catches the crash
static void crash_fix(const char *name, const char *kv) { g_fix = kv; J("begin %s%s%s\n", name, kv ? " " : "", kv ? kv : ""); }
static void guard(int on) {
    static struct sigaction old[3]; static const int sigs[3] = { SIGSEGV, SIGBUS, SIGABRT };
    for (int i = 0; i < 3; i++) {
        if (on) { struct sigaction sa = { 0 }; sa.sa_handler = on_crash; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER; sigaction(sigs[i], &sa, &old[i]); }
        else sigaction(sigs[i], &old[i], NULL);
    }
}

// ---- device (as the engine makes it: first GPU, a graphics + compute queue, every core feature on) ----
static VkInstance s_inst; static VkPhysicalDevice s_pd; static VkDevice s_dev; static VkQueue s_q; static uint32_t s_qf, s_api;
static VkCommandPool s_cp; static VkPhysicalDeviceMemoryProperties s_mp; static VkPhysicalDeviceProperties s_props; static VkPhysicalDeviceFeatures s_feat;
static int s_ext_fd, s_ext_dmabuf, s_ext_fctl, s_ext_drvp, s_ext_m1, s_dynren;   // (s_ext_m1: VK_KHR_maintenance1 on Vulkan 1.0; s_dynren: dynamic rendering on)
static PFN_vkGetMemoryFdPropertiesKHR p_fdprops;
static PFN_vkCmdBeginRendering p_begin_rend; static PFN_vkCmdEndRendering p_end_rend;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < s_mp.memoryTypeCount; i++) { VkMemoryPropertyFlags f = s_mp.memoryTypes[i].propertyFlags; if ((bits >> i & 1) && (f & want) == want && !(f & avoid)) return (int)i; }
    return -1;
}

// Vulkan 1.0 drivers take SPIR-V 1.0 only: every test shader goes through the engine's spv_to_10 there (nvn_vk.c vk_shader_module_10)
static PFN_vkCreateShaderModule s_csm_real;
static VKAPI_ATTR VkResult VKAPI_CALL csm_10(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    size_t nw; uint32_t *c = spv_to_10(ci->pCode, ci->codeSize / 4, &nw);
    if (!c) return s_csm_real(d, ci, a, m);
    VkShaderModuleCreateInfo c2 = *ci; c2.codeSize = nw * 4; c2.pCode = c; VkResult r = s_csm_real(d, &c2, a, m); free(c); return r;
}
static const char *s_driver;   // DM_VK_DRIVER of the game (NULL: the system's driver)
static int st_device(void) {
    const char *how = NULL;
    if (pv_load(s_driver, &how)) { R(FAIL, "device", "no Vulkan driver could be opened (%s)", s_driver && *s_driver ? s_driver : "the system's libvulkan"); return -1; }
    O("  driver: %s%s%s\n", how, s_driver && *s_driver ? " " : "", s_driver && *s_driver ? s_driver : "");
    uint32_t iv = VK_API_VERSION_1_0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iv);   // (a Vulkan 1.0 loader has no vkEnumerateInstanceVersion)
    if (getenv("DM_PROBE_API10")) iv = VK_API_VERSION_1_0;   // (testing: the Vulkan 1.0 route on any driver: no 1.1 functions, shaders through spv_to_10)
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative self-test", .apiVersion = iv < VK_API_VERSION_1_3 ? iv : VK_API_VERSION_1_3 };
    // the instance's extensions as the engine enables them (nvn_vk.c mtl_init): the 1.1 queries of properties and features, and external memory's capabilities, which
    //   a Vulkan 1.0 device's own extensions rest on (float controls, the driver's identity, dynamic rendering; external memory)
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL); VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
    const char *iext[2]; uint32_t niext = 0; int inst_p2 = 0, inst_xmc = 0;
    if (has_ext(ie, nie, "VK_KHR_get_physical_device_properties2")) { iext[niext++] = "VK_KHR_get_physical_device_properties2"; inst_p2 = 1; }
    if (has_ext(ie, nie, "VK_KHR_external_memory_capabilities")) { iext[niext++] = "VK_KHR_external_memory_capabilities"; inst_xmc = 1; }
    free(ie);
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext };
    VkResult vr = vkCreateInstance(&ici, NULL, &s_inst);
    if (vr) { R(FAIL, "device", "vkCreateInstance %d", vr); return -1; }
    pv_load_instance(s_inst);
    if (!vkEnumeratePhysicalDevices || !vkCreateDevice) { R(FAIL, "device", "the driver did not give its functions"); return -1; }
    uint32_t n = 1; vkEnumeratePhysicalDevices(s_inst, &n, &s_pd);
    if (!n) { R(FAIL, "device", "no Vulkan GPU"); return -1; }
    vkGetPhysicalDeviceProperties(s_pd, &s_props); vkGetPhysicalDeviceMemoryProperties(s_pd, &s_mp); vkGetPhysicalDeviceFeatures(s_pd, &s_feat);
    s_api = s_props.apiVersion < ai.apiVersion ? s_props.apiVersion : ai.apiVersion;
    int dev10 = s_api < VK_API_VERSION_1_1;
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)pv_instance_proc(s_inst, "vkGetPhysicalDeviceFeatures2");
    if (dev10) {   // (Vulkan 1.0: the 1.1 functions are not there to call; the property and feature queries under the extension's names where the instance has it)
        vkGetPhysicalDeviceProperties2 = inst_p2 ? (PFN_vkGetPhysicalDeviceProperties2)pv_instance_proc(s_inst, "vkGetPhysicalDeviceProperties2KHR") : NULL;
        gf2 = inst_p2 ? (PFN_vkGetPhysicalDeviceFeatures2)pv_instance_proc(s_inst, "vkGetPhysicalDeviceFeatures2KHR") : NULL;
        pv_vkGetPhysicalDeviceExternalBufferProperties = NULL;
        if (!s_csm_real) { s_csm_real = pv_vkCreateShaderModule; pv_vkCreateShaderModule = csm_10; } }
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, NULL); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, qp);
    for (s_qf = 0; s_qf < nq && (qp[s_qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT); s_qf++) { }
    if (s_qf == nq) { R(FAIL, "device", "no queue family with graphics and compute"); return -1; }
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, ex);
    const char *on[16]; uint32_t non = 0;
    // external memory: on Vulkan 1.0 VK_KHR_external_memory beside it (core from 1.1), which needs the instance's capabilities extension (nvn_vk.c xmem_ok)
    if (has_ext(ex, ne, "VK_KHR_external_memory_fd") && (!dev10 || (has_ext(ex, ne, "VK_KHR_external_memory") && inst_xmc))) {
        on[non++] = "VK_KHR_external_memory_fd"; if (dev10) on[non++] = "VK_KHR_external_memory"; s_ext_fd = 1;
        if (has_ext(ex, ne, "VK_EXT_external_memory_dma_buf")) { on[non++] = "VK_EXT_external_memory_dma_buf"; s_ext_dmabuf = 1; } }
    if (s_api < VK_API_VERSION_1_2 && (!dev10 || inst_p2)) {   // Vulkan 1.1: float controls and the driver's identity are extensions (nvn_vk.c, older Mali-G52 / G72 / G76);
                                                             //   on 1.0 they extend the property query the instance's extension gives, so only with it
        if (has_ext(ex, ne, "VK_KHR_shader_float_controls")) { on[non++] = "VK_KHR_shader_float_controls"; s_ext_fctl = 1; }
        if (has_ext(ex, ne, "VK_KHR_driver_properties")) { on[non++] = "VK_KHR_driver_properties"; s_ext_drvp = 1; }
    }
    // Vulkan 1.0: what 1.1 has built in, as the engine enables it there (nvn_vk.c x10): maintenance1, which a 3D image drawn through 2D-array views needs (the 3D
    //   tests are skipped without it), and storage_buffer_storage_class, which the test shaders' storage buffers declare
    if (dev10 && has_ext(ex, ne, "VK_KHR_maintenance1")) { on[non++] = "VK_KHR_maintenance1"; s_ext_m1 = 1; }
    if (dev10 && has_ext(ex, ne, "VK_KHR_storage_buffer_storage_class")) on[non++] = "VK_KHR_storage_buffer_storage_class";
    // dynamic rendering, as the engine enables it (its passes on every GPU but Adreno: Q_NO_DYNREN; the 3D tests record them so where it does): core in 1.3; before,
    //   VK_KHR_dynamic_rendering with what it rests on before 1.2 (depth_stencil_resolve and create_renderpass2; on 1.0 multiview and maintenance2 too)
    VkPhysicalDeviceDynamicRenderingFeatures fdr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    static const char *const dr_deps[4] = { "VK_KHR_depth_stencil_resolve", "VK_KHR_create_renderpass2", "VK_KHR_multiview", "VK_KHR_maintenance2" };
    int dr_ext = s_api < VK_API_VERSION_1_3 && has_ext(ex, ne, "VK_KHR_dynamic_rendering");
    for (int k = 0; k < 4 && dr_ext; k++) if (s_api < VK_API_VERSION_1_2 && (k < 2 || dev10) && !has_ext(ex, ne, dr_deps[k])) dr_ext = 0;
    if ((s_api >= VK_API_VERSION_1_3 || dr_ext) && gf2) { VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fdr }; gf2(s_pd, &f2); }
    if (fdr.dynamicRendering && dr_ext) { on[non++] = "VK_KHR_dynamic_rendering"; if (s_api < VK_API_VERSION_1_2) for (int k = 0; k < 4; k++) if (k < 2 || dev10) on[non++] = dr_deps[k]; }
    free(ex);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = s_qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = fdr.dynamicRendering ? &fdr : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = non, .ppEnabledExtensionNames = on, .pEnabledFeatures = &s_feat };
    fdr.pNext = NULL;
    if ((vr = vkCreateDevice(s_pd, &dci, NULL, &s_dev))) { R(FAIL, "device", "vkCreateDevice %d with every core feature on (as the engine asks)", vr); return -1; }
    vkGetDeviceQueue(s_dev, s_qf, 0, &s_q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s_qf };
    vkCreateCommandPool(s_dev, &pci, NULL, &s_cp);
    if (s_ext_fd) p_fdprops = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(s_dev, "vkGetMemoryFdPropertiesKHR");
    if (fdr.dynamicRendering) { int core = s_api >= VK_API_VERSION_1_3;
        p_begin_rend = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(s_dev, core ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
        p_end_rend = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(s_dev, core ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");
        s_dynren = p_begin_rend && p_end_rend; }
    return 0;
}

// ---- helpers ----
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; int coherent, type; } hbuf;
static int hbuf_make(hbuf *h, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    memset(h, 0, sizeof *h); h->type = -1;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    if (vkCreateBuffer(s_dev, &bi, NULL, &h->b)) { h->b = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(s_dev, h->b, &rq);
    int t = mem_type(rq.memoryTypeBits, want, avoid);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || vkAllocateMemory(s_dev, &ai, NULL, &h->m)) { vkDestroyBuffer(s_dev, h->b, NULL); memset(h, 0, sizeof *h); h->type = -1; return -1; }
    vkBindBufferMemory(s_dev, h->b, h->m, 0);
    if (vkMapMemory(s_dev, h->m, 0, VK_WHOLE_SIZE, 0, &h->p)) h->p = NULL;
    h->coherent = !!(s_mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); h->type = t;
    return t;
}
static void hbuf_free(hbuf *h) { if (h->m) { if (h->p) vkUnmapMemory(s_dev, h->m); vkFreeMemory(s_dev, h->m, NULL); } if (h->b) vkDestroyBuffer(s_dev, h->b, NULL); memset(h, 0, sizeof *h); }
static void hbuf_sync(hbuf *h, int to_gpu) {
    if (h->coherent || !h->m) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = h->m, .size = VK_WHOLE_SIZE };
    if (to_gpu) vkFlushMappedMemoryRanges(s_dev, 1, &r); else vkInvalidateMappedMemoryRanges(s_dev, 1, &r);
}
static VkCommandBuffer cb_begin(void) {
    VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s_cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(s_dev, &ca, &cb)) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi); return cb;
}
static VkResult cb_submit(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f; VkResult r = vkCreateFence(s_dev, &fi, NULL, &f);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (!r) { r = vkQueueSubmit(s_q, 1, &si, f); if (!r) r = vkWaitForFences(s_dev, 1, &f, VK_TRUE, 5000000000ull); vkDestroyFence(s_dev, f, NULL); }
    vkFreeCommandBuffers(s_dev, s_cp, 1, &cb); return r;
}
// the engine's end-of-submission barrier (nvn_vk.c barrier_host), as is
static void barrier_host(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void barrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 1, &mb, 0, NULL, 0, NULL);
}
typedef struct { VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkDescriptorPool dp; VkDescriptorSet ds; } comp;
static void comp_free(comp *c) {
    if (c->pipe) vkDestroyPipeline(s_dev, c->pipe, NULL); if (c->pl) vkDestroyPipelineLayout(s_dev, c->pl, NULL); if (c->dp) vkDestroyDescriptorPool(s_dev, c->dp, NULL);
    if (c->dsl) vkDestroyDescriptorSetLayout(s_dev, c->dsl, NULL); if (c->sm) vkDestroyShaderModule(s_dev, c->sm, NULL); memset(c, 0, sizeof *c);
}
static VkResult comp_make(comp *c, const uint32_t *code, size_t words, const VkDescriptorType *types, uint32_t nb, uint32_t push_bytes) {
    memset(c, 0, sizeof *c); VkResult r;
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code };
    if ((r = vkCreateShaderModule(s_dev, &mci, NULL, &c->sm))) return r;
    VkDescriptorSetLayoutBinding b[4]; VkDescriptorPoolSize ps[4];
    for (uint32_t i = 0; i < nb; i++) { b[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorType = types[i], .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }; ps[i] = (VkDescriptorPoolSize){ types[i], 1 }; }
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b };
    if ((r = vkCreateDescriptorSetLayout(s_dev, &dli, NULL, &c->dsl))) return r;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl, .pushConstantRangeCount = push_bytes ? 1 : 0, .pPushConstantRanges = &pr };
    if ((r = vkCreatePipelineLayout(s_dev, &pli, NULL, &c->pl))) return r;
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = c->pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = c->sm, .pName = "main" } };
    if ((r = vkCreateComputePipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &c->pipe))) { c->pipe = VK_NULL_HANDLE; return r; }
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = nb, .pPoolSizes = ps };
    if ((r = vkCreateDescriptorPool(s_dev, &dpi, NULL, &c->dp))) return r;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp, .descriptorSetCount = 1, .pSetLayouts = &c->dsl };
    return vkAllocateDescriptorSets(s_dev, &dai, &c->ds);
}
static void ds_buffer(comp *c, uint32_t binding, VkBuffer b) {
    VkDescriptorBufferInfo bi = { b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
}
static VkDeviceMemory image_mem(VkImage img) {
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(s_dev, img, &rq);
    int t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); if (t < 0) t = mem_type(rq.memoryTypeBits, 0, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = (uint32_t)t };
    VkDeviceMemory m = VK_NULL_HANDLE; if (t < 0 || vkAllocateMemory(s_dev, &ai, NULL, &m)) return VK_NULL_HANDLE;
    vkBindImageMemory(s_dev, img, m, 0); return m;
}

// ---- float controls ----
static const char *FC_NAME[8] = { "isnan(NaN)", "isinf(inf)", "sign of -0", "inf*0 is NaN", "1/0 is inf", "NaN != NaN", "inf-inf is NaN", "inf+1 is inf" };
static const uint32_t FC_WANT[8] = { 1, 1, 0x80000000u, 1, 1, 1, 1, 0x7f800000u };
// 0 ran (out filled), else the VkResult that stopped it
static VkResult run_floatctl(const uint32_t *code, size_t words, uint32_t out[8]) {
    hbuf h; if (hbuf_make(&h, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0) < 0 &&
                  hbuf_make(&h, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t *p = h.p; static const uint32_t in[8] = { 0x7f800000u, 0x7fc00000u, 0x80000000u, 0, 0x3f800000u, 0, 0, 0 };
    memcpy(p, in, sizeof in); memset(p + 8, 0xee, 32); hbuf_sync(&h, 1);
    comp c; VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; VkResult r = comp_make(&c, code, words, &t, 1, 0);
    if (!r) {
        ds_buffer(&c, 0, h.b); VkCommandBuffer cb = cb_begin();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
        if (!(r = cb_submit(cb))) { hbuf_sync(&h, 0); memcpy(out, p + 8, 32); }
    }
    comp_free(&c); hbuf_free(&h); return r;
}
static int fc_wrong(const uint32_t out[8], char *list, size_t n) {
    int bad = 0; list[0] = 0;
    for (int i = 0; i < 8; i++) if (out[i] != FC_WANT[i]) { size_t l = strlen(list); snprintf(list + l, n - l, "%s%s (got %#x)", bad++ ? ", " : "", FC_NAME[i], out[i]); }
    return bad;
}
// The GPU as nvn_vk.c mtl_init describes it to the engine's own per-GPU rules (vk_quirks.h gpu_id), from the same queries: the driver ID only where mtl_init reads
//   the driver properties (Vulkan 1.2, or 1.1 with the float-controls and driver-properties extensions), Arm's release from driverVersion on Arm's own driver.
//   dp / fc: the driver and float-controls properties (left zero where they cannot be read: *drvp / *fcp 0). The game's profile (gpu_id.port / eng) and dynamic
//   rendering are not known here: no rule the probe asks reads them (t_quirks.c checks that the ones it asks, Q_FLOAT_CONTROLS, Q_CLEAR_BY_PASS and Q_NO_DYNREN,
//   read the vendor and the driver ID only)
static void st_gpu(gpu_id *gid, VkPhysicalDeviceDriverProperties *dp, VkPhysicalDeviceFloatControlsProperties *fc, int *drvp, int *fcp) {
    *drvp = s_api >= VK_API_VERSION_1_2 || s_ext_drvp; *fcp = s_api >= VK_API_VERSION_1_2 || s_ext_fctl;
    memset(dp, 0, sizeof *dp); dp->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    memset(fc, 0, sizeof *fc); fc->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES; fc->pNext = *drvp ? dp : NULL;
    VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = *fcp ? (void *)fc : *drvp ? (void *)dp : NULL };
    if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(s_pd, &pp); else *drvp = *fcp = 0;   // (Vulkan 1.0: neither can be read)
    memset(gid, 0, sizeof *gid); gid->vendor = s_props.vendorID; gid->driver_version = s_props.driverVersion; gid->api = s_props.apiVersion;
    snprintf(gid->name, sizeof gid->name, "%s", s_props.deviceName); gid->custom = s_driver && *s_driver;
    gid->driver_id = *fcp && *drvp ? (int)dp->driverID : -1;
    if (gpu_is_arm_driver(gid) && (gid->driver_id >= 0 || *fcp || !gid->custom)) { gid->arm_rel = (int)(s_props.driverVersion >> 22); gid->arm_p = (int)(s_props.driverVersion >> 12 & 0x3ff);
        gid->arm_beta = gid->driver_id >= 0 && strstr(dp->driverInfo, "bet") != NULL; }
    { FILE *mf = fopen("/proc/meminfo", "r"); char ln[128]; long kb = 0; while (mf && fgets(ln, sizeof ln, mf)) if (sscanf(ln, "MemTotal: %ld kB", &kb) == 1) break; if (mf) fclose(mf); gid->ram_kb = kb; }
}
static void t_floatctl(void) {
    const char *N = "float-controls";
    // whether the engine asks for them here: its own rule (vk_quirks.h Q_FLOAT_CONTROLS: Arm's and Samsung's own drivers, by driver ID, else by vendor), not a copy of it,
    //   on the GPU described as nvn_vk.c mtl_init describes it (st_gpu)
    VkPhysicalDeviceDriverProperties dp; VkPhysicalDeviceFloatControlsProperties fc; int drvp, fcp; gpu_id gid;
    st_gpu(&gid, &dp, &fc, &drvp, &fcp);
    int arm = quirk_default(Q_FLOAT_CONTROLS, &gid);
    int support = fcp && fc.shaderSignedZeroInfNanPreserveFloat32;
    O("  driver %s (id %d)%s; SignedZeroInfNanPreserve float32: %s; the engine asks for it by default: %s\n", drvp ? dp.driverName : "(no driver properties)", (int)dp.driverID,
      arm ? ", ARM's own" : "", support ? "supported" : "not supported", arm && support ? "yes" : "no");
    size_t words = sizeof spv_floatctl / 4; uint32_t plain[8], patched[8]; char wrong[512], wrong2[512];
    VkResult r = run_floatctl(spv_floatctl, words, plain);
    if (r) { R(FAIL, N, "the plain test shader did not run (%d)", r); return; }
    int bad = fc_wrong(plain, wrong, sizeof wrong);
    O("  without float controls: %s\n", bad ? wrong : "all 8 checks right");
    if (!support) {
        if (!bad) R(PASS, N, "keeps infinities and NaN without being asked (the driver cannot be asked)");
        else R(WARN, N, "wrong without float controls (%s) and the driver cannot be asked to keep them: shaders that guard with isnan may draw white or black", wrong);
        return;
    }
    size_t nw = 0; uint32_t *code = spv_float_controls(spv_floatctl, words, &nw);
    if (!code) { R(FAIL, N, "spv_float_controls did not patch the test shader (probe bug)"); return; }
    r = run_floatctl(code, nw, patched); free(code);
    if (r) { R(FAIL, N, "the driver refused a shader asking for float controls (%d): the engine's own start-up test turns them off, and %s", r,
               bad ? "shaders can then draw wrong" : "this driver is right without them anyway"); return; }
    int bad2 = fc_wrong(patched, wrong2, sizeof wrong2);
    if (bad2) R(FAIL, N, "wrong even with float controls: %s", wrong2);
    else if (!bad) R(PASS, N, "right with and without float controls");
    else if (arm) R(PASS, N, "needs float controls (without: %s); the engine asks for them on this driver", wrong);
    else R(WARN, N, "needs float controls (without: %s), but the engine only asks on ARM's and Samsung's drivers: this driver's profile should turn them on (DM_VK_FLOAT_CONTROLS=1)", wrong);
    if (bad && !bad2 && !arm) FIX(N, "DM_VK_FLOAT_CONTROLS=1");   // (the WARN above: right only with them, and the engine would not ask)
}

// ---- shader maths: NaN in min / max / saturate, pow as exp2(log2), integers carried in floats ----
enum { SM_N = 16 };
static const char *SM_NAME[SM_N] = { "min(NaN, 1)", "min(1, NaN)", "max(NaN, 0.5)", "max(0.5, NaN)", "saturate(NaN)", "clamp(NaN, lo, hi)", "max(vec2(NaN, ..), 0).x", "pow(0, 2.2)",
    "log2(0)", "exp2(-inf)", "pow(0, 0) is NaN", "saturate(pow(0, 0))", "255 carried in a float", "carried 255 / 255", "fma(0.5, 1, 0.5)", "mix(0.5, 1, saturate(NaN))" };
static const uint32_t SM_WANT[SM_N] = { 0x3f800000u, 0x3f800000u, 0x3f000000u, 0x3f000000u, 0, 0, 0, 0, 0xff800000u, 0, 1, 0, 255, 0x3f800000u, 0x3f800000u, 0x3f000000u };
static VkResult run_shmath(const uint32_t *code, size_t words, uint32_t out[SM_N]) {
    hbuf h; if (hbuf_make(&h, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0) < 0 &&
                  hbuf_make(&h, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t *p = h.p; static const uint32_t in[16] = { 0x7f800000u, 0x7fc00000u, 0x80000000u, 0, 0x3f800000u, 0x3f000000u, 0x400ccccdu, 0,   255, 128, 0, 0, 0, 0, 0, 0 };
    memcpy(p, in, sizeof in); memset(p + 16, 0xee, 4 * SM_N); hbuf_sync(&h, 1);
    comp c; VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; VkResult r = comp_make(&c, code, words, &t, 1, 0);
    if (!r) {
        ds_buffer(&c, 0, h.b); VkCommandBuffer cb = cb_begin();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
        if (!(r = cb_submit(cb))) { hbuf_sync(&h, 0); memcpy(out, p + 16, 4 * SM_N); }
    }
    comp_free(&c); hbuf_free(&h); return r;
}
static int sm_wrong(const uint32_t out[SM_N], char *list, size_t n) {
    int bad = 0; list[0] = 0;
    for (int i = 0; i < SM_N; i++) if (out[i] != SM_WANT[i]) { size_t l = strlen(list); snprintf(list + l, n - l, "%s%s (got %#x)", bad++ ? ", " : "", SM_NAME[i], out[i]); }
    return bad;
}
// The test shader four ways: as compiled; with float controls; with float controls + spv_nan_minmax (NMin / NMax / NClamp: what v0.57-v0.59 gave Arm's driver);
// and with spv_nan_select on top (the rule written out with isnan: what the engine gives Arm's driver now). The last one is the verdict
static void t_shmath(void) {
    const char *N = "shader-maths";
    static const char *WAY[4] = { "as compiled", "float controls", "float controls + NMin / NMax / NClamp", "float controls + NMin / NMax / NClamp + isnan written out" };
    int fcp = (s_api >= VK_API_VERSION_1_2 || s_ext_fctl), bad[4] = { -1, -1, -1, -1 }; char wrong[4][768]; VkResult rr[4] = { 0 };
    if (fcp) { VkPhysicalDeviceFloatControlsProperties fc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES };
        VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &fc }; if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(s_pd, &pp); fcp = vkGetPhysicalDeviceProperties2 && fc.shaderSignedZeroInfNanPreserveFloat32; }
    for (int way = 0; way < 4; way++) {
        if (way && !fcp) break;
        size_t words = sizeof spv_shmath / 4, nw; uint32_t *code = malloc(4 * words), *c2; if (!code) return; memcpy(code, spv_shmath, 4 * words);
        if (way >= 1 && (c2 = spv_float_controls(code, words, &nw))) { free(code); code = c2; words = nw; }
        if (way >= 2 && (c2 = spv_nan_minmax(code, words, &nw))) { free(code); code = c2; words = nw; }
        if (way >= 3) { if ((c2 = spv_nan_select(code, words, &nw))) { free(code); code = c2; words = nw; } else { free(code); R(FAIL, N, "spv_nan_select did not patch the test shader (probe bug)"); return; } }
        uint32_t out[SM_N]; g_step = WAY[way]; rr[way] = run_shmath(code, words, out); g_step = NULL; free(code);
        if (rr[way]) { O("  %s: the shader did not run (%d)\n", WAY[way], rr[way]); continue; }
        bad[way] = sm_wrong(out, wrong[way], sizeof wrong[way]);
        O("  %s: %s\n", WAY[way], bad[way] ? wrong[way] : "all 16 answers as on the Switch's GPU");
    }
    int last = fcp ? 3 : 0;
    if (bad[last] < 0) { R(FAIL, N, "the test shader did not run %s (%d)", WAY[last], rr[last]); return; }
    if (!fcp) { if (!bad[0]) R(PASS, N, "right as compiled (the driver cannot be asked for float controls)"); else R(WARN, N, "wrong and the driver cannot be asked for float controls: %s", wrong[0]); return; }
    if (bad[3]) R(FAIL, N, "wrong even with the rule written out: %s: lit scenes can still come out white or black here", wrong[3]);
    else if (bad[2] > 0) R(PASS, N, "right only with the NaN rule written out; this driver does not honour NMin / NMax / NClamp (%s): the white screen of v0.57-v0.59", wrong[2]);
    else if (bad[2] == 0 && bad[1] > 0) R(PASS, N, "right with NMin / NMax / NClamp and with the rule written out (plain min / max / clamp: %s)", wrong[1]);
    else R(PASS, N, "right with float controls alone");
}

// ---- end-of-submission barrier ----
static void t_barrier(void) {
    const char *N = "end-of-submit-barrier"; const uint32_t n = 1u << 20; const int rounds = 12;
    comp c; VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkResult r = comp_make(&c, spv_barrier, sizeof spv_barrier / 4, &t, 1, 8);
    if (r) { R(FAIL, N, "the test shader did not build (%d)", r); comp_free(&c); return; }
    static const struct { const char *name; VkMemoryPropertyFlags want, avoid; } K[] = {
        { "coherent", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 },
        { "cached, not coherent", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT },
        { "cached, coherent", VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 },
    };
    int seen[VK_MAX_MEMORY_TYPES] = { 0 }, stale_with = 0, stale_without = 0, kinds = 0; char notes[512] = "";
    for (size_t k = 0; k < sizeof K / sizeof *K; k++) {
        hbuf h; if (hbuf_make(&h, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, K[k].want, K[k].avoid) < 0 || !h.p) { hbuf_free(&h); continue; }
        if (seen[h.type]) { hbuf_free(&h); continue; } seen[h.type] = 1; kinds++;
        ds_buffer(&c, 0, h.b);
        int stale[2] = { 0, 0 };
        for (int with = 0; with < 2; with++) for (int round = 0; round < rounds; round++) {
            uint32_t seed = 0x51ed0000u + (uint32_t)(with * 100 + round), *p = h.p;
            memset(p, 0xde, (size_t)n * 4); hbuf_sync(&h, 1);
            VkCommandBuffer cb = cb_begin(); uint32_t pc[2] = { seed, n };
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
            vkCmdPushConstants(cb, c.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc); vkCmdDispatch(cb, n / 64, 1, 1);
            if (with) barrier_host(cb);
            if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d) on %s memory", r, K[k].name); hbuf_free(&h); comp_free(&c); return; }
            hbuf_sync(&h, 0);
            uint32_t badw = 0; for (uint32_t i = 0; i < n; i++) if (p[i] != (seed ^ (i * 2654435761u))) badw++;
            if (badw) stale[with]++;
        }
        size_t l = strlen(notes);
        snprintf(notes + l, sizeof notes - l, "%s%s (type %d): stale reads in %d/%d rounds without the barrier, %d/%d with it", l ? "; " : "", K[k].name, h.type, stale[0], rounds, stale[1], rounds);
        stale_without += stale[0]; stale_with += stale[1]; hbuf_free(&h);
    }
    comp_free(&c);
    if (!kinds) { R(SKIP, N, "no host-visible memory type for a storage buffer"); return; }
    O("  %s\n", notes);
    if (stale_with) R(FAIL, N, "the CPU read stale GPU writes even after the engine's barrier: the game can read old data back (white / frozen pictures)");
    else if (stale_without) R(PASS, N, "this driver needs the end-of-submission barrier (stale without it); the engine has it since v0.53");
    else R(PASS, N, "GPU writes reach the CPU with and without the barrier");
}

// ---- reinterpreted format: written as RGBA8, sampled as R32_UINT ----
static VkRenderPass make_pass(VkFormat f, VkImageLayout final, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkAttachmentDescription a = { .format = f, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, ds, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, da, 0 };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp = VK_NULL_HANDLE; vkCreateRenderPass(s_dev, &ci, NULL, &rp); return rp;
}
static void t_reinterp(void) {
    const char *N = "reinterpreted-format"; const uint32_t W = 16;
    VkFormatProperties a, b; vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_R8G8B8A8_UNORM, &a); vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_R32_UINT, &b);
    if (!(a.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !(b.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { R(SKIP, N, "RGBA8 targets or R32_UINT sampling missing"); return; }
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory im = VK_NULL_HANDLE; VkImageView v8 = VK_NULL_HANDLE, v32 = VK_NULL_HANDLE; VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE; VkSampler smp = VK_NULL_HANDLE; comp c = { 0 }; hbuf h = { 0 }; VkResult r;
    if (vkCreateImage(s_dev, &ii, NULL, &img) || !(im = image_mem(img))) { R(FAIL, N, "a MUTABLE_FORMAT RGBA8 target could not be made"); goto out; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCreateImageView(s_dev, &vi, NULL, &v8); vi.format = VK_FORMAT_R32_UINT;
    if ((r = vkCreateImageView(s_dev, &vi, NULL, &v32))) { R(FAIL, N, "an R32_UINT view of an RGBA8 texture was refused (%d)", r); goto out; }
    rp = make_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &v8, .width = W, .height = W, .layers = 1 };
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (!rp || vkCreateFramebuffer(s_dev, &fi, NULL, &fb) || vkCreateSampler(s_dev, &si, NULL, &smp)) { R(FAIL, N, "render pass / framebuffer / sampler setup failed"); goto out; }
    if (hbuf_make(&h, W * W * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "no host-visible storage buffer"); goto out; }
    VkDescriptorType ts[2] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    if ((r = comp_make(&c, spv_reinterp, sizeof spv_reinterp / 4, ts, 2, 0))) { R(FAIL, N, "the test shader did not build (%d)", r); goto out; }
    VkDescriptorImageInfo dii = { smp, v32, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c.ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL); ds_buffer(&c, 1, h.b);
    memset(h.p, 0xee, W * W * 4); hbuf_sync(&h, 1);
    VkCommandBuffer cb = cb_begin();
    VkClearValue cv = { .color = { .float32 = { 1, 0, 1, 0 } } };   // R FF G 00 B FF A 00 → 0x00FF00FF (the render pass's clear: a driver's fast-clear path)
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, W } }, .clearValueCount = 1, .pClearValues = &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 0, 1, 0, 1 } } } };   // left half 0xFF00FF00 (a drawn clear)
    VkClearRect cr = { { { 0, 0 }, { W / 2, W } }, 0, 1 }; vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
    if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d)", r); goto out; }
    hbuf_sync(&h, 0);
    { const uint32_t *p = h.p; int bad_l = 0, bad_r = 0; uint32_t first = 0; int have = 0;
      for (uint32_t i = 0; i < W * W; i++) { uint32_t want = (i % W) < W / 2 ? 0xFF00FF00u : 0x00FF00FFu; if (p[i] != want) { if (!have) { first = p[i]; have = 1; } if ((i % W) < W / 2) bad_l++; else bad_r++; } }
      if (!bad_l && !bad_r) R(PASS, N, "a render target sampled as another format reads the bytes that were drawn (fast clear and drawn clear)");
      else R(FAIL, N, "sampled as R32_UINT, %d texels of the drawn clear and %d of the fast clear are wrong (first %#x): textures the game re-reads as another type come out wrong (compressed render target not resolved)", bad_l, bad_r, first); }
out:
    comp_free(&c); hbuf_free(&h);
    if (smp) vkDestroySampler(s_dev, smp, NULL); if (fb) vkDestroyFramebuffer(s_dev, fb, NULL); if (rp) vkDestroyRenderPass(s_dev, rp, NULL);
    if (v32) vkDestroyImageView(s_dev, v32, NULL); if (v8) vkDestroyImageView(s_dev, v8, NULL); if (img) vkDestroyImage(s_dev, img, NULL); if (im) vkFreeMemory(s_dev, im, NULL);
}

// ---- 3D render target: the game's 32x32x32 colour-grading tables are drawn into a 3D image through 2D-array views of its slices ----
// The image as the engine makes a 3D render target (tex_make: MUTABLE_FORMAT | 2D_ARRAY_COMPATIBLE, transfer source and destination, sampled, colour attachment).
// layered (the engine's att_view): one view of all 32 slices, its passes 32 layers deep, each step a submission of its own so a driver crash, or a lost device,
//   names the step: 0 the pass clearing every slice (its load op); 1 that pass begun again, loading (the engine reopens it at the game's next draw); 2 the clear the
//   engine records inside that pass, exactly as clear_in_pass records it on this driver: vkCmdClearAttachments of the pass's layer 0 over a part of it (the
//   scissor), or where the engine clears by pass (clear_by_pass: DM_VK_CLEAR_BY_PASS, else Q_CLEAR_BY_PASS: Samsung's Xclipse, whose driver crashed recording a
//   layered vkCmdClearAttachments, Xclipse 940 driver 24.0.534 and Xclipse 540 24.0.560) the layered pass ended, the slice cleared by a pass of its own over that
//   part (its load op, clear_slice_by_pass) and the layered pass begun again; 3 all 32 slices read back. Recorded as the engine records its passes on this driver:
//   dynamic rendering (vkCmdBeginRendering, no framebuffer) where the engine uses it (st_dynren), else render passes and framebuffers.
// slices: a 1-slice view, framebuffer and pass per slice (a candidate workaround where the layered way fails), slices 16-31 cleared again inside their passes.
// What a crash asks for (the journal's "begin" lines, crash_fix): the layered steps DM_VK_NO_LAYERED_3D=1 (T below: slice 0 only, the engine before att_view drew
//   every slice), except the clear inside the pass (recording it, or the submission that runs it, after step 1 ran the same pass without it): DM_VK_CLEAR_BY_PASS=1,
//   the engine's own way around it, which keeps every slice (unless env.txt sets that switch: it wins; then slice 0 only); and nothing for the read-back (the
//   probe's own copy). A lost device at a step's submission asks for what a crash there would. Without a crash: the clear's part wrong and the rest right asks for
//   DM_VK_CLEAR_BY_PASS=1 too (where the engine does not clear by pass already); slices wrong elsewhere ask for nothing (slice 0 only would not draw them either)
static int st_clear_by_pass(int *from_env) {
    const char *e = getenv("DM_VK_CLEAR_BY_PASS"); *from_env = e != NULL;   // (the game's env.txt: dprobe_jni.c sets it here as the game does)
    if (e) return atoi(e) != 0;
    VkPhysicalDeviceDriverProperties dp; VkPhysicalDeviceFloatControlsProperties fc; int drvp, fcp; gpu_id gid;
    st_gpu(&gid, &dp, &fc, &drvp, &fcp);
    return quirk_default(Q_CLEAR_BY_PASS, &gid);
}
// whether the engine records its passes with dynamic rendering on this driver (nvn_vk.c mtl_init g_dynren): the driver has it (st_device), and neither DM_VK_BASELINE
//   nor DM_VK_NO_DYNREN (env.txt) nor its default (vk_quirks.h Q_NO_DYNREN: Adreno) asks for render passes
static int st_dynren(void) {
    if (!s_dynren) return 0;
    const char *b = getenv("DM_VK_BASELINE"), *e = getenv("DM_VK_NO_DYNREN");
    if (b && atoi(b) > 0) return 0;
    if (e) return atoi(e) == 0;
    VkPhysicalDeviceDriverProperties dp; VkPhysicalDeviceFloatControlsProperties fc; int drvp, fcp; gpu_id gid;
    st_gpu(&gid, &dp, &fc, &drvp, &fcp);
    return !quirk_default(Q_NO_DYNREN, &gid);
}
enum { D3 = 32 };
static const VkRect2D CLEAR_RC = { { 5, 9 }, { 19, 14 } };   // the part the clear inside the layered pass covers (a scissored clear: clear_in_pass's rectangle)
static int in_clear_rc(uint32_t x, uint32_t y) { return x >= 5 && x < 5 + 19 && y >= 9 && y < 9 + 14; }
// the slices read back: *bad_part, texels of the layered clear's part (slice 0) wrong; returns the slices wrong anywhere else
static int check_3d(const uint32_t *p, int layered, int *bad_part, char *why, size_t n) {
    int bad = 0, first = -1; uint32_t got = 0, gotp = 0; *bad_part = 0;
    for (uint32_t z = 0; z < D3; z++) { int sb = 0;
        for (uint32_t y = 0; y < D3; y++) for (uint32_t x = 0; x < D3; x++) {
            int part = layered && z == 0 && in_clear_rc(x, y);
            uint32_t want = (layered ? part : z >= D3 / 2) ? 0xFFFF0000u : 0xFF0000FFu, v = p[(z * D3 + y) * D3 + x];   // (cleared again: blue; else red)
            if (v == want) continue;
            if (part) { if (!*bad_part) gotp = v; (*bad_part)++; } else { if (!sb && first < 0) got = v; sb = 1; } }
        if (sb) { bad++; if (first < 0) first = (int)z; } }
    if (bad) snprintf(why, n, "%d of 32 slices wrong (first slice %d holds %#x)", bad, first, got);
    else if (*bad_part) snprintf(why, n, "%d of the %u texels the clear inside the pass covers wrong (holding %#x)", *bad_part, CLEAR_RC.extent.width * CLEAR_RC.extent.height, gotp);
    return bad;
}
// a pass over the 3D image: a render pass (rp with framebuffer fb) or dynamic rendering (view v, layers deep), over area; cv: cleared to it (load op CLEAR), else LOAD
static void pass3d(VkCommandBuffer cb, int dyn, VkRenderPass rp, VkFramebuffer fb, VkImageView v, uint32_t layers, VkRect2D area, const VkClearValue *cv) {
    if (dyn) {
        VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .loadOp = cv ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        if (cv) ca.clearValue = *cv;
        VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = layers, .colorAttachmentCount = 1, .pColorAttachments = &ca };
        p_begin_rend(cb, &ri);
    } else {
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = area, .clearValueCount = cv ? 1 : 0, .pClearValues = cv };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    }
}
static void pass3d_end(VkCommandBuffer cb, int dyn) { if (dyn) p_end_rend(cb); else vkCmdEndRenderPass(cb); }
static void full_barrier(VkCommandBuffer cb) { barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT); }
static void t_3d_mode(int layered) {
    const char *N = layered ? "3d-layered" : "3d-slices"; const uint32_t D = D3;
    if (s_api < VK_API_VERSION_1_1 && !s_ext_m1) { R(SKIP, N, "Vulkan 1.0 without VK_KHR_maintenance1: a 3D image cannot be drawn through 2D-array views on this driver"); return; }
    int cbp_env = 0, cbp = layered && st_clear_by_pass(&cbp_env), dyn = layered && st_dynren();   // (the clear by passes of their own; dynamic rendering, as the engine here)
    const char *clear_fix = cbp_env ? "DM_VK_NO_LAYERED_3D=1" : "DM_VK_CLEAR_BY_PASS=1";   // (what a crash in the clear inside the pass asks for: env.txt's switch wins)
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    g_step = "image format query";
    VkImageFormatProperties ifp; VkResult r = vkGetPhysicalDeviceImageFormatProperties(s_pd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL, usage,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, &ifp);
    if (r) { R(FAIL, N, "3D colour targets drawn through 2D-array views are not supported (%d): the game's 3D colour-grading tables cannot be filled", r); return; }
    O("  %s: %s%s\n", N, layered ? (dyn ? "dynamic rendering (as the engine records its passes here)" : "render passes (as the engine records its passes here)") : "render passes",
      layered ? (cbp ? "; the clear inside the pass as a pass of its own (as the engine here)" : "; the clear inside the pass with vkCmdClearAttachments (as the engine here)") : "");
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, .imageType = VK_IMAGE_TYPE_3D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { D, D, D }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory im = VK_NULL_HANDLE; VkImageView v[32] = { 0 }; VkFramebuffer fb[32] = { 0 }; VkRenderPass rp[3] = { 0 }; hbuf h = { 0 };
    char why[160] = "";
    g_step = "3D image creation";
    if (vkCreateImage(s_dev, &ii, NULL, &img) || !(im = image_mem(img))) { R(FAIL, N, "a 32x32x32 3D target could not be made"); goto out; }
    // the views (and, with render passes, their framebuffers): slices, v[i] slice i; layered, v[0] all 32 slices, and with clears by pass v[1] slice 0 (its pass)
    for (uint32_t i = 0; i < (layered ? (cbp ? 2u : 1u) : D); i++) {
        int all = layered && i == 0;
        g_step = all ? "the 32-slice 2D-array view" : "a 1-slice 2D-array view";
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layered ? 0 : i, all ? D : 1 } };
        if ((r = vkCreateImageView(s_dev, &vi, NULL, &v[i]))) { R(FAIL, N, "%s was refused (%d)", g_step, r); goto out; }
    }
    // render passes: 0 clear everything (load op CLEAR), 1 keep (LOAD), 2 a clear by its load op, the image already in use; all GENERAL as the engine's passes
    for (int k = 0; !dyn && k < 3; k++) {
        g_step = "render pass creation";
        VkAttachmentDescription a = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = k == 1 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = k ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
        VkSubpassDependency dep[2] = {
            { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
            { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
        if ((r = vkCreateRenderPass(s_dev, &rci, NULL, &rp[k]))) { R(FAIL, N, "render pass refused (%d)", r); goto out; }
    }
    for (uint32_t i = 0; !dyn && i < D; i++) {
        if (!v[i]) continue;
        int all = layered && i == 0;
        g_step = all ? "the 32-layer framebuffer" : "a 1-layer framebuffer";
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp[0], .attachmentCount = 1, .pAttachments = &v[i], .width = D, .height = D, .layers = all ? D : 1 };
        if ((r = vkCreateFramebuffer(s_dev, &fi, NULL, &fb[i]))) { R(FAIL, N, "%s was refused (%d)", g_step, r); goto out; }
    }
    if (hbuf_make(&h, D * D * D * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "no host-visible readback buffer"); goto out; }
    memset(h.p, 0, D * D * D * 4); hbuf_sync(&h, 1);
    const VkClearValue red = { .color = { .float32 = { 1, 0, 0, 1 } } }, blue = { .color = { .float32 = { 0, 0, 1, 1 } } };   // 0xFF0000FF, 0xFFFF0000
    const VkRect2D all = { { 0, 0 }, { D, D } };
    const int steps = layered ? 4 : 2;   // (slices: 0 every slice cleared by its pass, 1 slices 16-31 cleared again in their passes and the read-back)
    for (int k = 0; k < steps; k++) {
        int readback = k == steps - 1;
        if (layered && k == 3) crash_fix(N, NULL);   // (the read-back is the probe's own: nothing in the game to turn off)
        g_step = !layered ? (!k ? "recording the render-pass clear of every slice" : "recording the per-slice vkCmdClearAttachments")
               : k == 0 ? "recording the layered pass's clear of every slice (its load op)" : k == 1 ? "recording the layered pass begun again (loading)"
               : k == 2 ? (cbp ? "recording the clear inside the layered pass as a pass of its own (clear by pass, the engine's way here)" : "recording the layered pass's vkCmdClearAttachments")
               : "recording the read-back";
        VkCommandBuffer cb = cb_begin();
        if (k == 0 && dyn) {   // (render passes start from UNDEFINED; dynamic rendering takes the image in the layout it is in: GENERAL, as the engine keeps its images)
            VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        } else full_barrier(cb);
        if (!layered) {
            for (uint32_t i = k ? D / 2 : 0; i < D; i++) {
                pass3d(cb, 0, rp[k], fb[i], v[i], 1, all, k ? NULL : &red);
                if (k) { VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, blue }; VkClearRect cr = { all, 0, 1 }; vkCmdClearAttachments(cb, 1, &ca, 1, &cr); }
                pass3d_end(cb, 0);
            }
        } else if (k == 0) { pass3d(cb, dyn, rp[0], fb[0], v[0], D, all, &red); pass3d_end(cb, dyn); }
        else if (k == 1) { pass3d(cb, dyn, rp[1], fb[0], v[0], D, all, NULL); pass3d_end(cb, dyn); }
        else if (k == 2 && cbp) {   // clear_in_pass → clear_slice_by_pass: the layered pass (open when the clear comes) ended, a pass of its own on the pass's first slice over
                                    //   the clear's part (load op CLEAR), and the layered pass begun again at the game's next draw, loading what is there
            pass3d(cb, dyn, rp[1], fb[0], v[0], D, all, NULL); pass3d_end(cb, dyn); full_barrier(cb);
            pass3d(cb, dyn, rp[2], fb[1], v[1], 1, CLEAR_RC, &blue); pass3d_end(cb, dyn); full_barrier(cb);
            pass3d(cb, dyn, rp[1], fb[0], v[0], D, all, NULL); pass3d_end(cb, dyn);
        } else if (k == 2) {   // clear_in_pass: vkCmdClearAttachments of the pass's layer 0, over the clear's part
            pass3d(cb, dyn, rp[1], fb[0], v[0], D, all, NULL);
            crash_fix(N, clear_fix);   // (from here, this step's submission included: the clear; the pass begun alone ran in step 1)
            VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, blue }; VkClearRect cr = { CLEAR_RC, 0, 1 }; vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
            pass3d_end(cb, dyn);
        }
        if (readback) { if (layered) full_barrier(cb);
                        VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { D, D, D } };
                        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, h.b, 1, &bc);
                        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT); }
        g_step = !layered ? (!k ? "running the render-pass clear of every slice" : "running the per-slice clear and the read-back")
               : k == 0 ? "running the layered pass's clear of every slice" : k == 1 ? "running the layered pass begun again" : k == 2 ? (cbp ? "running the clear by a pass of its own" : "running the layered pass's vkCmdClearAttachments")
               : "running the read-back";
        if ((r = cb_submit(cb))) { R(FAIL, N, "%s failed (%d)", g_step, r);
            if (r == VK_ERROR_DEVICE_LOST && g_fix) FIX(N, g_fix);   // (the GPU crashed there: as a crash of that step)
            goto out; }
    }
    g_step = "checking the slices"; hbuf_sync(&h, 0);
    int bad_part; int bad = check_3d(h.p, layered, &bad_part, why, sizeof why);
    if (!bad && !bad_part) {
        if (!layered) R(PASS, N, "all 32 slices drew one pass per slice (1-slice views): a way to fill 3D colour-grading tables where the layered way fails");
        else R(PASS, N, "all 32 slices of a 3D target drew as a layered 2D array (the engine's way, with %s), and the engine's clear inside that pass (%s) drew right",
               dyn ? "dynamic rendering" : "render passes", cbp ? (cbp_env ? "a pass of its own: DM_VK_CLEAR_BY_PASS in env.txt" : "a pass of its own: Q_CLEAR_BY_PASS, Samsung's Xclipse") : "vkCmdClearAttachments");
    }
    else if (layered && !bad) {   // (the layered pass right, the clear inside it wrong)
        R(FAIL, N, "%s%s: 3D scenes that clear their colour-grading tables can come out wrong this way", why, cbp ? " (the clear as a pass of its own)" : " (vkCmdClearAttachments inside the layered pass)");
        if (!cbp && !cbp_env) FIX(N, "DM_VK_CLEAR_BY_PASS=1");   // (the engine's way around that call; env.txt's switch, when set, wins anyway)
    }
    else R(FAIL, N, "%s: 3D scenes that use colour-grading tables can come out black or wrong this way", why);
out:
    g_step = NULL; hbuf_free(&h);
    for (uint32_t i = 0; i < 32; i++) { if (fb[i]) vkDestroyFramebuffer(s_dev, fb[i], NULL); if (v[i]) vkDestroyImageView(s_dev, v[i], NULL); }
    for (int k = 0; k < 3; k++) if (rp[k]) vkDestroyRenderPass(s_dev, rp[k], NULL);
    if (img) vkDestroyImage(s_dev, img, NULL); if (im) vkFreeMemory(s_dev, im, NULL);
}
static void t_3d_slices(void) { t_3d_mode(0); }
static void t_3d_layered(void) { t_3d_mode(1); }

// ---- the engine's stock-driver fallback (nvn_vk.c rend_begin / pso_make with sst): a classic render pass in GENERAL layout, draw state built into the pipelines ----
typedef struct { VkCullModeFlags cull; VkFrontFace front; VkPrimitiveTopology topo; VkBool32 dt, dw, st; VkCompareOp scmp; VkStencilOp spass; } fbs;
static VkPipeline fb_pipe(VkRenderPass rp, VkPipelineLayout pl, VkShaderModule vm, VkShaderModule fm, const fbs *s) {
    VkPipelineShaderStageCreateInfo st[2] = { { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
                                              { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };   // the stride in the pipeline (no VERTEX_INPUT_BINDING_STRIDE)
    VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32_SFLOAT, 8 } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                                .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = s->topo };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = s->cull,
                                                  .frontFace = s->front, .depthBiasEnable = VK_TRUE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState so = { VK_STENCIL_OP_KEEP, s->spass, VK_STENCIL_OP_KEEP, s->scmp, 0, 0, 0 };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = s->dt, .depthWriteEnable = s->dw,
                                                 .depthCompareOp = s->dt ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS, .stencilTestEnable = s->st, .front = so, .back = so };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo bs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[7] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,   // the engine's 7 (Vulkan 1.0)
                              VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 7, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &bs, .pDynamicState = &dy, .layout = pl, .renderPass = rp };
    VkPipeline p = VK_NULL_HANDLE; return vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gi, NULL, &p) ? VK_NULL_HANDLE : p;
}
static void quad(float *v, float x0, float y0, float x1, float y1, float z) {   // two triangles of the same winding, 4 floats (x, y, z, pad) per vertex
    const float p[6][2] = { { x0, y0 }, { x0, y1 }, { x1, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
    for (int i = 0; i < 6; i++) { v[i * 4] = p[i][0]; v[i * 4 + 1] = p[i][1]; v[i * 4 + 2] = z; v[i * 4 + 3] = 0; }
}
static void t_fallback(void) {
    const char *N = "fallback-draw"; const uint32_t W = 32;
    VkFormat dsf = VK_FORMAT_UNDEFINED; static const VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2 && !dsf; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(s_pd, cand[i], &fp); if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) dsf = cand[i]; }
    if (!dsf) { R(SKIP, N, "no depth / stencil target format"); return; }
    VkImage ci = VK_NULL_HANDLE, di = VK_NULL_HANDLE; VkDeviceMemory cm = VK_NULL_HANDLE, dm = VK_NULL_HANDLE; VkImageView cv = VK_NULL_HANDLE, dv = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE; VkFramebuffer fb = VK_NULL_HANDLE; VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pp[6] = { 0 }; hbuf vbuf = { 0 }, rb = { 0 }; VkResult r;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    if (vkCreateImage(s_dev, &ii, NULL, &ci) || !(cm = image_mem(ci))) { R(FAIL, N, "colour target"); goto out; }
    ii.format = dsf; ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(s_dev, &ii, NULL, &di) || !(dm = image_mem(di))) { R(FAIL, N, "depth / stencil target"); goto out; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ci, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCreateImageView(s_dev, &vi, NULL, &cv);
    vi.image = di; vi.format = dsf; vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; vkCreateImageView(s_dev, &vi, NULL, &dv);
    {   // the engine's render pass (rp_get): GENERAL layouts, cleared, stored, ordered like barrier_all on both sides
        VkAttachmentDescription a[2] = {
            { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL },
            { 0, dsf, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL } };
        VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_GENERAL }, dr = { 1, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cr, .pDepthStencilAttachment = &dr };
        VkSubpassDependency dep[2] = {
            { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
            { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
        if ((r = vkCreateRenderPass(s_dev, &rci, NULL, &rp))) { R(FAIL, N, "the render pass was refused (%d)", r); goto out; }
        VkImageView at[2] = { cv, dv };
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = at, .width = W, .height = W, .layers = 1 };
        if ((r = vkCreateFramebuffer(s_dev, &fi, NULL, &fb))) { R(FAIL, N, "the framebuffer was refused (%d)", r); goto out; }
    }
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spv_fallback_vert, .pCode = spv_fallback_vert };
    vkCreateShaderModule(s_dev, &mci, NULL, &vm); mci.codeSize = sizeof spv_fallback_frag; mci.pCode = spv_fallback_frag; vkCreateShaderModule(s_dev, &mci, NULL, &fm);
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (!vm || !fm || vkCreatePipelineLayout(s_dev, &pli, NULL, &pl)) { R(FAIL, N, "shaders / pipeline layout"); goto out; }
    // A strip over everything (red, writes depth 0.5 and stencil 1); top quarters: the same triangles culled BACK and culled FRONT (green: exactly one draws);
    // bottom left: stencil EQUAL 1 in front (blue, draws); bottom right: behind with depth LESS (white, must not draw), then depth test off but stencil EQUAL 0 (yellow, must not draw)
    const fbs S[6] = {
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, 1, 1, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE },
        { VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 1, VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP },
        { VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 1, VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP } };
    for (int i = 0; i < 6; i++) if (!(pp[i] = fb_pipe(rp, pl, vm, fm, &S[i]))) { R(FAIL, N, "pipeline %d with its draw state built in was refused", i); goto out; }
    float vd[34 * 4]; const float strip[4][2] = { { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) { vd[i * 4] = strip[i][0]; vd[i * 4 + 1] = strip[i][1]; vd[i * 4 + 2] = 0.5f; vd[i * 4 + 3] = 0; }
    quad(vd + 4 * 4, -1, -1, 0, 0, 0.3f); quad(vd + 10 * 4, 0, -1, 1, 0, 0.3f); quad(vd + 16 * 4, -1, 0, 0, 1, 0.3f); quad(vd + 22 * 4, 0, 0, 1, 1, 0.9f); quad(vd + 28 * 4, 0, 0, 1, 1, 0.1f);
    if (hbuf_make(&vbuf, sizeof vd, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0 ||
        hbuf_make(&rb, W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "buffers"); goto out; }
    memcpy(vbuf.p, vd, sizeof vd); hbuf_sync(&vbuf, 1);
    VkCommandBuffer cb = cb_begin();
    VkClearValue clr[2]; memset(clr, 0, sizeof clr); clr[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, W } }, .clearValueCount = 2, .pClearValues = clr };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v = { 0, 0, (float)W, (float)W, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, W } }; float bc[4] = { 0, 0, 0, 0 };
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc); vkCmdSetDepthBias(cb, 0, 0, 0); vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    VkDeviceSize zero = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.b, &zero);   // (Vulkan 1.0's bind: no strides)
    static const float col[6][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 0, 1 } };
    static const uint32_t first[6] = { 0, 4, 10, 16, 22, 28 }, count[6] = { 4, 6, 6, 6, 6, 6 }, ref[6] = { 1, 0, 0, 1, 0, 0 };
    for (int i = 0; i < 6; i++) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp[i]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ref[i]);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, col[i]); vkCmdDraw(cb, count[i], 1, first[i], 0);
    }
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, W, 1 } };
    vkCmdCopyImageToBuffer(cb, ci, VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &bic);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d)", r); goto out; }
    hbuf_sync(&rb, 0);
    {   // each quarter's inside (2 texels from its edges) in one colour, or 1 for mixed
        const uint32_t *p = rb.p; uint32_t q[4];
        for (int k = 0; k < 4; k++) { uint32_t x0 = (k & 1) ? 18 : 2, y0 = (k & 2) ? 18 : 2, c = p[y0 * W + x0];
            for (uint32_t y = y0; y < y0 + 12; y++) for (uint32_t x = x0; x < x0 + 12; x++) if (p[y * W + x] != c) c = 1;
            q[k] = c; }
        const uint32_t RED = 0xFF0000FFu, GREEN = 0xFF00FF00u, BLUE = 0xFFFF0000u, WHITE = 0xFFFFFFFFu, YELLOW = 0xFF00FFFFu;
        char why[256] = ""; size_t l;
        if (q[0] == 0 || q[1] == 0 || q[2] == 0 || q[3] == 0) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe triangle strip did not cover the target", l ? "; " : ""); }
        if (q[0] == GREEN && q[1] == GREEN) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe cull mode in the pipeline was ignored", l ? "; " : ""); }
        else if (!((q[0] == GREEN && q[1] == RED) || (q[0] == RED && q[1] == GREEN))) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sculling drew neither or mixed (top %#x / %#x)", l ? "; " : "", q[0], q[1]); }
        if (q[2] != BLUE) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sstencil EQUAL / depth LESS in front drew %#x, not blue", l ? "; " : "", q[2]); }
        if (q[3] == WHITE) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe depth test in the pipeline was ignored", l ? "; " : ""); }
        else if (q[3] == YELLOW) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sthe stencil test in the pipeline was ignored", l ? "; " : ""); }
        else if (q[3] != RED) { l = strlen(why); snprintf(why + l, sizeof why - l, "%sbottom right %#x, not red", l ? "; " : "", q[3]); }
        if (!why[0]) R(PASS, N, "render pass in GENERAL layout and pipelines with cull, topology, depth, stencil and vertex stride built in drew right: the engine's fallback path works on this driver");
        else R(FAIL, N, "%s: the engine's stock-driver fallback would draw wrong here", why);
    }
out:
    for (int i = 0; i < 6; i++) if (pp[i]) vkDestroyPipeline(s_dev, pp[i], NULL);
    if (pl) vkDestroyPipelineLayout(s_dev, pl, NULL); if (vm) vkDestroyShaderModule(s_dev, vm, NULL); if (fm) vkDestroyShaderModule(s_dev, fm, NULL);
    hbuf_free(&vbuf); hbuf_free(&rb);
    if (fb) vkDestroyFramebuffer(s_dev, fb, NULL); if (rp) vkDestroyRenderPass(s_dev, rp, NULL);
    if (cv) vkDestroyImageView(s_dev, cv, NULL); if (dv) vkDestroyImageView(s_dev, dv, NULL);
    if (ci) vkDestroyImage(s_dev, ci, NULL); if (di) vkDestroyImage(s_dev, di, NULL); if (cm) vkFreeMemory(s_dev, cm, NULL); if (dm) vkFreeMemory(s_dev, dm, NULL);
}

// ---- dma-buf heap route ----
struct st_heap_alloc { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define ST_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct st_heap_alloc)
// CPU writes the first half at view, the GPU copies it out and fills the second half; both directions checked, twice. NULL = pass
static const char *roundtrip(VkBuffer a, VkDeviceSize size, volatile uint32_t *view) {
    VkDeviceSize half = size / 2; hbuf rb; const char *res = NULL;
    if (hbuf_make(&rb, half, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return "no readback buffer";
    for (int round = 0; round < 2 && !res; round++) {
        uint32_t salt = 0x9e3779b9u * (uint32_t)(round + 1), fill = 0xCAFE0000u + (uint32_t)round;
        for (VkDeviceSize i = 0; i < half / 4; i++) view[i] = (uint32_t)i ^ salt;
        VkCommandBuffer cb = cb_begin(); VkBufferCopy cp = { 0, 0, half };
        vkCmdCopyBuffer(cb, a, rb.b, 1, &cp); vkCmdFillBuffer(cb, a, half, half, fill); barrier_host(cb);
        if (cb_submit(cb)) { res = "submission failed"; break; }
        hbuf_sync(&rb, 0); const uint32_t *rp = rb.p;
        for (VkDeviceSize i = 0; !res && i < half / 4; i++) if (rp[i] != ((uint32_t)i ^ salt)) res = "the GPU did not see the CPU's writes";
        for (VkDeviceSize i = half / 4; !res && i < size / 4; i++) if (view[i] != fill) res = "the CPU did not see the GPU's writes";
    }
    hbuf_free(&rb); return res;
}
static void t_dmabuf(void) {
    const char *N = "dmabuf-heap";
    if (!s_ext_fd || !s_ext_dmabuf || !p_fdprops || !vkGetPhysicalDeviceExternalBufferProperties) { R(SKIP, N, "the driver imports no dma-bufs (VK_EXT_external_memory_dma_buf missing): the engine moves the driver's own mappings instead"); return; }
    VkPhysicalDeviceExternalBufferInfo xi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkExternalBufferProperties xp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(s_pd, &xi, &xp);
    unsigned feat = (unsigned)xp.externalMemoryProperties.externalMemoryFeatures;
    O("  dma-buf buffer features %#x (import %s, export %s)\n", feat, feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? "yes" : "no", feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "yes" : "no");
    if (!(feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) { R(WARN, N, "the driver lists dma-buf buffers as not importable: the engine keeps its memory pools on another route (more memory, slower)"); return; }
    static const char *heaps[] = { "/dev/dma_heap/system-uncached", "/dev/dma_heap/system" }; int hfd = -1, err = 0; const char *hname = NULL;
    for (int i = 0; i < 2 && hfd < 0; i++) { if ((hfd = open(heaps[i], O_RDONLY | O_CLOEXEC)) >= 0) hname = heaps[i]; else err = errno; }
    if (hfd < 0) { R(SKIP, N, "no dma-buf heap opens for an app (errno %d): the engine falls back the same way", err); return; }
    char notes[512] = ""; int fails = 0;
    static const VkDeviceSize sizes[] = { 4ull << 20, 64ull << 20 };
    for (int s = 0; s < 2; s++) {
        VkDeviceSize size = sizes[s]; const char *res = NULL;
        struct st_heap_alloc d = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
        void *at = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); int fd = -1;
        VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE;
        if (at == MAP_FAILED) { at = NULL; res = "no address space"; }
        else if (ioctl(hfd, ST_HEAP_IOCTL_ALLOC, &d) < 0) res = "the heap refused the allocation";
        else if ((fd = (int)d.fd), mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != at) res = "mapping the dma-buf at the chosen address failed";
        else {
            VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
            VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
            VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
            VkResult r;
            if ((r = p_fdprops(s_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp))) res = "vkGetMemoryFdPropertiesKHR failed";
            else if ((r = vkCreateBuffer(s_dev, &bi, NULL, &b))) { b = VK_NULL_HANDLE; res = "the external buffer was refused"; }
            else {
                VkMemoryRequirements rq; vkGetBufferMemoryRequirements(s_dev, b, &rq);
                int t = mem_type(rq.memoryTypeBits & fp.memoryTypeBits, 0, 0); int dupfd = dup(fd);
                VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dupfd };
                VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size, .memoryTypeIndex = (uint32_t)t };
                if (t < 0) { res = "no memory type takes this dma-buf"; close(dupfd); }
                else if ((r = vkAllocateMemory(s_dev, &ai, NULL, &m))) { m = VK_NULL_HANDLE; res = "the driver refused the dma-buf import"; close(dupfd); }
                else if ((r = vkBindBufferMemory(s_dev, b, m, 0))) res = "binding the imported memory failed";
                else res = roundtrip(b, size, (volatile uint32_t *)at);
            }
        }
        if (b) vkDestroyBuffer(s_dev, b, NULL); if (m) vkFreeMemory(s_dev, m, NULL); if (at) munmap(at, size); if (fd >= 0) close(fd);
        size_t l = strlen(notes); snprintf(notes + l, sizeof notes - l, "%s%llu MB: %s", l ? "; " : "", (unsigned long long)(size >> 20), res ? res : "pass");
        if (res) fails++;
    }
    close(hfd);
    if (!fails) R(PASS, N, "%s mapped at a fixed address and imported: GPU and CPU see each other's writes (%s)", hname, notes);
    else R(FAIL, N, "%s: %s; the engine's heap route falls back to mirrored pools here (twice the memory, stalls)", hname, notes);
}

// ---- BC textures: the engine's BC layer, linked as is ----
static void bc_log(const char *fmt, ...) { O("    bc_gpu: "); va_list ap; va_start(ap, fmt); probe_vout(fmt, ap); va_end(ap); O("\n"); }
static void t_bc(void) {
    const char *N = "bc-textures";
    if (s_feat.textureCompressionBC) { R(PASS, N, "BC1-BC7 sampled natively (textureCompressionBC): the game's textures upload as they are"); return; }
    static const struct { int k; const char *n; } K[] = { {1,"BC1"}, {2,"BC2"}, {3,"BC3"}, {4,"BC4"}, {-4,"BC4s"}, {5,"BC5"}, {-5,"BC5s"}, {6,"BC6H"}, {-6,"BC6Hs"}, {7,"BC7"} };
    int etc = s_feat.textureCompressionETC2, mode = etc ? 1 : 0;   // nvn_vk.c: re-encoded (ETC2 / EAC / ASTC) where the GPU has ETC2, else decoded to plain texels
    O("  no BC on this GPU: textures are %s (ETC2 %d, ASTC LDR %d)\n", etc ? "re-encoded to ETC2 / EAC / ASTC (the same size as BC)" : "decoded to plain texels (4-8 times the memory)",
      etc, s_feat.textureCompressionASTC_LDR);
    bc_gpu_info bi = { s_dev, s_q, s_qf, vkGetDeviceProcAddr, &s_mp, bc_log };
    int on = bc_gpu_init(&bi);
    char gpu[160] = "", cpu[160] = ""; int ncpu = 0;
    for (size_t i = 0; i < sizeof K / sizeof *K; i++) { char *l = on && bc_gpu_can(K[i].k, mode) ? gpu : (ncpu++, cpu); size_t n = strlen(l); snprintf(l + n, 160 - n, "%s%s", n ? " " : "", K[i].n); }
    if (!on) R(WARN, N, "no BC kind converts on this GPU bit for bit with the CPU: every BC texture converts on the CPU (longer loads, more stutter)");
    else if (ncpu) R(WARN, N, "on the GPU: %s; on the CPU (slower): %s", gpu, cpu);
    else R(PASS, N, "every BC kind converts on the GPU, bit for bit with the CPU");
}

static void meminfo(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char ln[256]; long total = 0, avail = 0;
    if (f) { while (fgets(ln, sizeof ln, f)) { sscanf(ln, "MemTotal: %ld kB", &total); sscanf(ln, "MemAvailable: %ld kB", &avail); } fclose(f); }
    // the size on the box: the kernel reports less (firmware and GPU carve-outs), so round up to the next size phones are sold with
    static const long sold[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32 }; long gb = 0;
    for (size_t i = 0; i < sizeof sold / sizeof *sold && !gb; i++) if (total <= sold[i] * 1024 * 1024) gb = sold[i];
    if (!gb) gb = (total + 1024 * 1024 - 1) / (1024 * 1024);
    R(INFO, "memory", "%ld GB phone (%ld MB seen by Android), %ld MB available now: %s", gb, total / 1024, avail / 1024,
      gb >= 8 ? "enough for the games" : gb >= 6 ? "tight for the games (Low graphics and Half textures help)" : "below the games' needs: Android may close the game");
}

// The engine's hard requirements (nvn_vk.c mtl_init): a driver older than Vulkan 1.3 needs these extensions, or the game stops at start (VDIE)
static void t_requirements(void) {
    const char *N = "engine-requirements";
    if (s_api >= VK_API_VERSION_1_3) { R(PASS, N, "Vulkan 1.3 driver: dynamic rendering, timeline semaphores and extended dynamic state are core"); return; }
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, NULL); VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, ex);
    int timeline = s_api >= VK_API_VERSION_1_2 || has_ext(ex, ne, "VK_KHR_timeline_semaphore");
    int dynren = has_ext(ex, ne, "VK_KHR_dynamic_rendering"), eds = has_ext(ex, ne, "VK_EXT_extended_dynamic_state");
    free(ex);
    unsigned ma = VK_API_VERSION_MAJOR(s_api), mi = VK_API_VERSION_MINOR(s_api);
    if (!timeline) { R(WARN, N, "Vulkan %u.%u driver without timeline semaphores (and%s%s): the engine runs its Vulkan 1.0 path here (fences, render passes, pipelines with their draw state; slower)", ma, mi,
                       dynren ? "" : " without dynamic rendering", eds ? "" : ", extended dynamic state"); return; }
    if (dynren && eds) { R(PASS, N, "Vulkan %u.%u driver with every extension the engine needs", ma, mi); return; }
    // Since the stock-driver fallbacks (render passes; draw state built into pipelines) the engine runs without these two; v0.53 and older stopped at start
    R(WARN, N, "Vulkan %u.%u driver without%s%s%s: the engine uses its fallback%s (%s%s%s); releases older than those fallbacks (Galar Native v0.53) stop at start here", ma, mi,
      dynren ? "" : " KHR_dynamic_rendering", !dynren && !eds ? " and" : "", eds ? "" : " EXT_extended_dynamic_state", !dynren && !eds ? "s" : "",
      dynren ? "" : "render passes", !dynren && !eds ? ", " : "", eds ? "" : "draw state built into each pipeline: no pipeline prewarm, more pipelines");
}

// ---- helpers for the image tests ----
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout from) {   // to GENERAL, ordered after everything before
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { asp, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void all_barrier(VkCommandBuffer cb) { barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT); }
static int fmt_has(VkFormat f, VkFormatFeatureFlags want) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(s_pd, f, &fp); return (fp.optimalTilingFeatures & want) == want; }

// ---- depth / stencil copy drawn (nvn_vk.c zcopy_draw, its own shaders: overlay/zcopy_*): a 16x16 source scaled to 8x8 ----
static void t_depth_copy(void) {
    const char *N = "depth-copy"; enum { S = 16, D = 8 };
    VkFormat f = VK_FORMAT_UNDEFINED; static const VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2 && !f; i++) if (fmt_has(cand[i], VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) f = cand[i];
    if (!f) { R(SKIP, N, "no depth / stencil format that can be both sampled and drawn into: the engine blits such copies"); return; }
    int d24 = f == VK_FORMAT_D24_UNORM_S8_UINT;
    VkImage si = VK_NULL_HANDLE, di = VK_NULL_HANDLE; VkDeviceMemory sm = VK_NULL_HANDLE, dm = VK_NULL_HANDLE; VkImageView sdv = VK_NULL_HANDLE, ssv = VK_NULL_HANDLE, dv = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE; VkFramebuffer fb = VK_NULL_HANDLE; VkSampler smp = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE; VkShaderModule vm = VK_NULL_HANDLE, fdm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE; VkPipeline pd = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    hbuf up = { 0 }, rb = { 0 }; VkResult r;
    g_step = "making the images";
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { S, S, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    if (vkCreateImage(s_dev, &ii, NULL, &si) || !(sm = image_mem(si))) { R(FAIL, N, "the source depth / stencil texture could not be made"); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    ii.extent = (VkExtent3D){ D, D, 1 }; ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(s_dev, &ii, NULL, &di) || !(dm = image_mem(di))) { R(FAIL, N, "the destination depth / stencil target could not be made"); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = si, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    if ((r = vkCreateImageView(s_dev, &vi, NULL, &sdv))) { R(FAIL, N, "a depth view for sampling was refused (%d)", r); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if ((r = vkCreateImageView(s_dev, &vi, NULL, &ssv))) { R(FAIL, N, "a stencil view for sampling was refused (%d)", r); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    vi.image = di; vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; vkCreateImageView(s_dev, &vi, NULL, &dv);
    {   // the engine's pass: depth / stencil attachment only, GENERAL (cleared here: in the engine it loads, the target's contents being the game's)
        VkAttachmentDescription a = { 0, f, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL };
        VkAttachmentReference dr = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &dr };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp };
        if ((r = vkCreateRenderPass(s_dev, &rci, NULL, &rp))) { R(FAIL, N, "the depth-only render pass was refused (%d)", r); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &dv, .width = D, .height = D, .layers = 1 };
        if ((r = vkCreateFramebuffer(s_dev, &fi, NULL, &fb))) { R(FAIL, N, "the framebuffer was refused (%d)", r); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    }
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    if (vkCreateSampler(s_dev, &sci, NULL, &smp) || vkCreateDescriptorSetLayout(s_dev, &dli, NULL, &dsl) || vkCreateDescriptorPool(s_dev, &dpi, NULL, &dp)) { R(FAIL, N, "sampler / descriptor setup"); goto out; }
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (vkCreatePipelineLayout(s_dev, &pli, NULL, &pl)) { R(FAIL, N, "pipeline layout"); goto out; }
    VkDescriptorSet set; VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    if (vkAllocateDescriptorSets(s_dev, &dai, &set)) { R(FAIL, N, "descriptor set"); goto out; }
    VkDescriptorImageInfo dii[2] = { { smp, sdv, VK_IMAGE_LAYOUT_GENERAL }, { smp, ssv, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet wr[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii[0] },
                                   { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii[1] } };
    vkUpdateDescriptorSets(s_dev, 2, wr, 0, NULL);
    g_step = "building the copy's pipelines";
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof zcopy_vert_spv, .pCode = zcopy_vert_spv };
    vkCreateShaderModule(s_dev, &mci, NULL, &vm); mci.codeSize = sizeof zcopy_d_frag_spv; mci.pCode = zcopy_d_frag_spv; vkCreateShaderModule(s_dev, &mci, NULL, &fdm);
    mci.codeSize = sizeof zcopy_s_frag_spv; mci.pCode = zcopy_s_frag_spv; vkCreateShaderModule(s_dev, &mci, NULL, &fsm);
    if (!vm || !fdm || !fsm) { R(FAIL, N, "the copy's shaders were refused"); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    for (int k = 0; k < 2; k++) {   // as nvn_vk.c zc_pipe
        VkPipelineShaderStageCreateInfo sg[2] = { { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
                                                  { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k ? fsm : fdm, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkStencilOpState so = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, 0, 0 };
        VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = !k, .depthWriteEnable = !k,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS, .stencilTestEnable = VK_TRUE, .front = so, .back = so, .maxDepthBounds = 1 };
        VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 4, .pDynamicStates = dyn };
        VkGraphicsPipelineCreateInfo gi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = sg, .pVertexInputState = &vis, .pInputAssemblyState = &ia,
            .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = pl, .renderPass = rp };
        if ((r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gi, NULL, k ? &ps : &pd))) { R(FAIL, N, "the copy's %s pipeline was refused (%d)", k ? "stencil" : "depth", r); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    }
    // the source: depth (x + y * 16 + 1) / 300, stencil (x * 13 + y * 7 + 5) & 255
    if (hbuf_make(&up, S * S * 4 + S * S, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0 ||
        hbuf_make(&rb, D * D * 4 + D * D, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "buffers"); goto out; }
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        double v = (x + y * S + 1) / 300.0; uint32_t w;
        if (d24) w = (uint32_t)(v * 16777215.0 + 0.5); else { float fv = (float)v; memcpy(&w, &fv, 4); }
        ((uint32_t *)up.p)[y * S + x] = w; ((uint8_t *)up.p)[S * S * 4 + y * S + x] = (uint8_t)((x * 13 + y * 7 + 5) & 255); }
    hbuf_sync(&up, 1); memset(rb.p, 0xcd, D * D * 5); hbuf_sync(&rb, 1);
    g_step = "drawing the copy";
    VkCommandBuffer cb = cb_begin();
    img_barrier(cb, si, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    img_barrier(cb, di, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkBufferImageCopy bc[2] = { { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { S, S, 1 } },
                                { .bufferOffset = S * S * 4, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { S, S, 1 } } };
    vkCmdCopyBufferToImage(cb, up.b, si, VK_IMAGE_LAYOUT_GENERAL, 2, bc);
    all_barrier(cb);
    VkClearValue cv; memset(&cv, 0, sizeof cv); cv.depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { D, D } }, .clearValueCount = 1, .pClearValues = &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v = { 0, 0, D, D, 0, 1 }; VkRect2D sc = { { 0, 0 }, { D, D } };
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    struct { float m[4]; int32_t lim[4]; } pc = { { (float)S / D, (float)S / D, 0, 0 }, { S - 1, S - 1, 0, 0 } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pd);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc); vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    for (int bit = 0; bit < 8; bit++) { pc.lim[2] = bit; vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc); vkCmdDraw(cb, 3, 1, 0, 0); }
    vkCmdEndRenderPass(cb);
    all_barrier(cb);
    VkBufferImageCopy rc[2] = { { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { D, D, 1 } },
                                { .bufferOffset = D * D * 4, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { D, D, 1 } } };
    vkCmdCopyImageToBuffer(cb, di, VK_IMAGE_LAYOUT_GENERAL, rb.b, 2, rc);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if ((r = cb_submit(cb))) { R(FAIL, N, "submission failed (%d)", r); FIX(N, "DM_VK_ZCOPY=0"); goto out; }
    hbuf_sync(&rb, 0);
    {   // each destination texel holds the source's at (2x + 1, 2y + 1): nearest, as the shader's floor(pixel centre * 2)
        int bad_d = 0, bad_s = 0; uint32_t got_d = 0, want_d = 0; unsigned got_s = 0, want_s = 0;
        for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {
            int sx = 2 * x + 1, sy = 2 * y + 1; uint32_t w = ((uint32_t *)up.p)[sy * S + sx], g = ((uint32_t *)rb.p)[y * D + x];
            int ok = d24 ? (int)((g & 0xffffff) - w) >= -2 && (int)((g & 0xffffff) - w) <= 2 : g == w;
            if (!ok && !bad_d++) { got_d = d24 ? g & 0xffffff : g; want_d = w; }
            unsigned ws = (unsigned)((sx * 13 + sy * 7 + 5) & 255), gs = ((uint8_t *)rb.p)[D * D * 4 + y * D + x];
            if (gs != ws && !bad_s++) { got_s = gs; want_s = ws; } }
        if (!bad_d && !bad_s) R(PASS, N, "a scaled copy into %s drawn with the depth from the shader and the stencil bit by bit reads back right: the engine's way for depth copies a blit cannot take works here", d24 ? "D24S8" : "D32S8");
        else { R(FAIL, N, "the drawn copy into %s is wrong: depth %d of %d texels (first %#x, wanted %#x), stencil %d (first %u, wanted %u): such copies go back to the blit", d24 ? "D24S8" : "D32S8",
                 bad_d, D * D, got_d, want_d, bad_s, got_s, want_s); FIX(N, "DM_VK_ZCOPY=0"); }
    }
out:
    if (pd) vkDestroyPipeline(s_dev, pd, NULL); if (ps) vkDestroyPipeline(s_dev, ps, NULL);
    if (vm) vkDestroyShaderModule(s_dev, vm, NULL); if (fdm) vkDestroyShaderModule(s_dev, fdm, NULL); if (fsm) vkDestroyShaderModule(s_dev, fsm, NULL);
    if (pl) vkDestroyPipelineLayout(s_dev, pl, NULL); if (dp) vkDestroyDescriptorPool(s_dev, dp, NULL); if (dsl) vkDestroyDescriptorSetLayout(s_dev, dsl, NULL); if (smp) vkDestroySampler(s_dev, smp, NULL);
    if (fb) vkDestroyFramebuffer(s_dev, fb, NULL); if (rp) vkDestroyRenderPass(s_dev, rp, NULL);
    if (sdv) vkDestroyImageView(s_dev, sdv, NULL); if (ssv) vkDestroyImageView(s_dev, ssv, NULL); if (dv) vkDestroyImageView(s_dev, dv, NULL);
    if (si) vkDestroyImage(s_dev, si, NULL); if (di) vkDestroyImage(s_dev, di, NULL); if (sm) vkFreeMemory(s_dev, sm, NULL); if (dm) vkFreeMemory(s_dev, dm, NULL);
    hbuf_free(&up); hbuf_free(&rb);
}

// ---- integer textures read as another numeric class (nvn_vk.c tex_view_for / fmt_as_class), two tests over one shader (shaders/intview.comp: binding 0 read as
//   float, its .r bits; binding 1 read as int, four channels) ----
enum { IV_W = 4 };
static const uint32_t IV_F32[IV_W] = { 0x3f800000u, 0xc2c80000u, 0x12345678u, 0x40490fdbu };   // 1, -100, a small normal, pi: plain floats, so the texture unit has nothing to flush
static const uint32_t IV_I32[IV_W][4] = { { 1, 0x7fffffffu, 0x80000000u, 0xffffffffu }, { 0, 2, 0xfffffffeu, 0x12345678u }, { 0x80000001u, 0x7ffffffeu, 3, 0xabcdef01u }, { 0xffff8000u, 0, 0x8000u, 0xc0000000u } };
static const uint16_t IV_I16[IV_W][4] = { { 1, 0x7fff, 0x8000, 0xffff }, { 0, 2, 0xfffe, 0x1234 }, { 0x8001, 0x7ffe, 3, 0xabcd }, { 0xffff, 0, 0x4000, 0xc000 } };
// an integer texture as tex_new makes it for the engine, 4 x 1: kind 0 mutable without a format list (fixed_fmt 0: no render-target use, so a view of another class
//   needs no render-target support), 1 not mutable (Mali's plain images, fixed_fmt 1), 2 mutable with a list of its own format (a render-target format where format
//   lists exist, fixed_fmt 2: Vulkan 1.2 here, the probe's device enables no VK_KHR_image_format_list); 1 and 2 with the render-target use the format has, as tex_new
static VkResult iv_image(VkFormat f, int kind, VkImage *im, VkDeviceMemory *mem) {
    VkImageFormatListCreateInfo fl = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 1, .pViewFormats = &f };
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = kind == 2 ? &fl : NULL, .flags = kind == 1 ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D,
        .format = f, .extent = { IV_W, 1, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (kind && fmt_has(f, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) };
    VkResult r = vkCreateImage(s_dev, &ii, NULL, im); if (r) { *im = VK_NULL_HANDLE; return r; }
    return (*mem = image_mem(*im)) ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
// The shader over images im[0] (viewed as vf[0]) and im[1] (as vf[1]), filled from d0 / d1 (n0 / n1 bytes): o[0..3] the first's bits, o[4..19] the second's channels
static VkResult iv_run(const VkImage im[2], const VkFormat vf[2], const void *d0, size_t n0, const void *d1, size_t n1, uint32_t o[20], const char **what) {
    VkImageView vw[2] = { 0 }; VkSampler smp = VK_NULL_HANDLE; comp c = { 0 }; hbuf up = { 0 }, out = { 0 }; VkResult r = VK_SUCCESS;
    for (int k = 0; k < 2 && !r; k++) { VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im[k], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vf[k],
                                                                      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if ((r = vkCreateImageView(s_dev, &vi, NULL, &vw[k]))) *what = "a view was refused"; }
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (!r && (r = vkCreateSampler(s_dev, &si, NULL, &smp))) *what = "sampler";
    VkDescriptorType ts[3] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    if (!r && (r = comp_make(&c, spv_intview, sizeof spv_intview / 4, ts, 3, 0))) *what = "the test shader did not build";
    if (!r && (hbuf_make(&up, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0 ||
               hbuf_make(&out, 20 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0)) { r = VK_ERROR_OUT_OF_DEVICE_MEMORY; *what = "buffers"; }
    if (!r) {
        memcpy(up.p, d0, n0); memcpy((uint8_t *)up.p + 128, d1, n1); hbuf_sync(&up, 1); memset(out.p, 0xcd, 80); hbuf_sync(&out, 1);
        VkDescriptorImageInfo dii[2] = { { smp, vw[0], VK_IMAGE_LAYOUT_GENERAL }, { smp, vw[1], VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet w[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c.ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii[0] },
                                      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c.ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii[1] } };
        vkUpdateDescriptorSets(s_dev, 2, w, 0, NULL); ds_buffer(&c, 2, out.b);
        g_step = "reading the views";
        VkCommandBuffer cb = cb_begin();
        for (int k = 0; k < 2; k++) img_barrier(cb, im[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        VkBufferImageCopy bc0 = { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { IV_W, 1, 1 } }, bc1 = bc0; bc1.bufferOffset = 128;
        vkCmdCopyBufferToImage(cb, up.b, im[0], VK_IMAGE_LAYOUT_GENERAL, 1, &bc0); vkCmdCopyBufferToImage(cb, up.b, im[1], VK_IMAGE_LAYOUT_GENERAL, 1, &bc1);
        all_barrier(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1); barrier_host(cb);
        if ((r = cb_submit(cb))) *what = "submission failed";
        else { hbuf_sync(&out, 0); memcpy(o, out.p, 80); }
    }
    comp_free(&c); hbuf_free(&up); hbuf_free(&out); if (smp) vkDestroySampler(s_dev, smp, NULL);
    for (int k = 0; k < 2; k++) if (vw[k]) vkDestroyImageView(s_dev, vw[k], NULL);
    return r;
}
static int iv_bad(const uint32_t *o, const uint32_t *want, int n, char *why, size_t nw, const char *what) {
    int bad = 0; for (int i = 0; i < n; i++) if (o[i] != want[i] && !bad++ && !why[0]) snprintf(why, nw, "%s: %#x read %#x", what, want[i], o[i]);
    return bad;
}
// int-view: the views the engine makes, on a mutable image without a format list (where it makes them): R32_UINT texels through an R32_SFLOAT view (read as floats),
//   R32G32B32A32_UINT through R32G32B32A32_SINT (the other signedness): their own bits, 32-bit channels being the same in every class
static void t_int_view(void) {
    const char *N = "int-view";
    static const VkFormat own[2] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32B32A32_UINT }, as[2] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32B32A32_SINT };
    for (int k = 0; k < 2; k++) if (!fmt_has(own[k], VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !fmt_has(as[k], VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { R(SKIP, N, "the formats cannot be sampled here"); return; }
    VkImage im[2] = { 0 }; VkDeviceMemory mem[2] = { 0 }; VkResult r; const char *what = ""; uint32_t o[20];
    for (int k = 0; k < 2; k++) if ((r = iv_image(own[k], 0, &im[k], &mem[k]))) { R(FAIL, N, "a MUTABLE_FORMAT integer texture could not be made (%d)", r); FIX(N, "DM_VK_INT_TEX=keep"); goto out; }
    if ((r = iv_run(im, as, IV_F32, sizeof IV_F32, IV_I32, sizeof IV_I32, o, &what))) { R(FAIL, N, "%s (%d)", what, r); FIX(N, "DM_VK_INT_TEX=keep"); goto out; }
    {   char why[160] = ""; int bad32 = iv_bad(o, IV_F32, IV_W, why, sizeof why, "R32_UINT as float"), badi = iv_bad(o + 4, &IV_I32[0][0], IV_W * 4, why, sizeof why, "R32G32B32A32_UINT as SINT");
        if (!bad32 && !badi) R(PASS, N, "32-bit integer textures read through float / other-signedness views give their own bits: the engine's fix for shaders that read them so is right here");
        else { R(FAIL, N, "%d of %d float and %d of %d other-signedness reads wrong (%s): such textures are read in their own format as before", bad32, IV_W, badi, IV_W * 4, why); FIX(N, "DM_VK_INT_TEX=keep"); }
    }
out:
    for (int k = 0; k < 2; k++) { if (im[k]) vkDestroyImage(s_dev, im[k], NULL); if (mem[k]) vkFreeMemory(s_dev, mem[k], NULL); }
}
// int-view-own: where the engine keeps the texture's own format, as 6e0e1dd did for all of them (tex_view_for): R32_UINT read as float on an image whose formats are
//   fixed (not mutable: Mali; a format list of its own format: render-target formats where lists exist), and on every kind of image R16G16B16A16_UINT read as int
//   (a 16-bit integer of the other signedness) and read as float (an 8 / 16-bit integer under a float sampler: the translator declares every sampled texture float,
//   and a shader that does integer work on the value takes its bits; Shield's fp_t_tcb_8). A format and a shader of different classes is undefined in Vulkan: what
//   this driver returns is reported against the Switch's answer (the texel's own bits; the 16-bit channels zero-extended, as their format says). Information only,
//   and no setting after a crash: nothing turns that old binding off (DM_VK_INT_TEX_EMPTY=1 reads the 16-bit one under a float sampler as 0 instead, to compare)
static void catf(char *b, size_t n, const char *fmt, ...) {   // appends to the string in b (n bytes), cut at its end
    size_t l = strlen(b); if (l + 1 >= n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(b + l, n - l, fmt, ap); va_end(ap);
}
static void t_int_view_own(void) {
    const char *N = "int-view-own";
    if (!fmt_has(VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || !fmt_has(VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { R(SKIP, N, "the formats cannot be sampled here"); return; }
    static const VkFormat fm[3] = { VK_FORMAT_R32_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT }, own16[2] = { VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT };
    uint32_t z16[IV_W * 4], r16[IV_W];   // the Switch's answers: the 16-bit channels zero-extended; each texel's .r under a float sampler, its bits (the same value)
    for (int i = 0; i < IV_W; i++) { r16[i] = IV_I16[i][0]; for (int ch = 0; ch < 4; ch++) z16[i * 4 + ch] = IV_I16[i][ch]; }
    char line[760] = ""; int nk = s_api >= VK_API_VERSION_1_2 ? 3 : 2;
    for (int kind = 0; kind < nk; kind++) {   // (kind 0, mutable without a list: the 32-bit reads get a view of their class there, int-view; the 16-bit ones keep theirs)
        VkImage im[3] = { 0 }; VkDeviceMemory mem[3] = { 0 }; VkResult r = VK_SUCCESS; const char *what = ""; uint32_t o[20], o2[20];
        const char *kn = kind == 0 ? "mutable" : kind == 1 ? "not mutable" : "format-listed";
        for (int k = kind ? 0 : 1; k < 3 && !r; k++) if ((r = iv_image(fm[k], kind, &im[k], &mem[k]))) what = "an image could not be made";
        if (!r && kind) r = iv_run(im, fm, IV_F32, sizeof IV_F32, IV_I16, sizeof IV_I16, o, &what);   // R32_UINT as float (binding 0)
        if (!r) { VkImage i16[2] = { im[2], im[1] }; r = iv_run(i16, own16, IV_I16, sizeof IV_I16, IV_I16, sizeof IV_I16, o2, &what); }   // R16G16B16A16_UINT as float, as int
        char why[160] = "";
        int bad32 = r || !kind ? 0 : iv_bad(o, IV_F32, IV_W, why, sizeof why, "R32_UINT as float"), bad16 = r ? 0 : iv_bad(o2 + 4, z16, IV_W * 4, why, sizeof why, "R16G16B16A16_UINT as int"),
            badf = r ? 0 : iv_bad(o2, r16, IV_W, why, sizeof why, "R16G16B16A16_UINT as float");
        catf(line, sizeof line, "%s%s: ", line[0] ? "; " : "", kn);
        if (r) catf(line, sizeof line, "%s (%d)", what, (int)r);
        else {
            if (kind) catf(line, sizeof line, "R32_UINT as float %d of %d its own bits, ", IV_W - bad32, IV_W);
            catf(line, sizeof line, "R16G16B16A16_UINT as int %d of %d zero-extended, as float %d of %d its own bits%s%s", IV_W * 4 - bad16, IV_W * 4, IV_W - badf, IV_W,
                 why[0] ? ", first: " : "", why);
        }
        for (int k = 0; k < 3; k++) { if (im[k]) vkDestroyImage(s_dev, im[k], NULL); if (mem[k]) vkFreeMemory(s_dev, mem[k], NULL); }
    }
    R(INFO, N, "the old binding the engine keeps there (undefined in Vulkan), against the Switch's answer: %s%s", line, nk < 3 ? "; format-listed: not tried (Vulkan 1.1)" : "");
}

// ---- the test list. crash: the setting that keeps the game off the path a test crashed the driver on (the games' driver check applies it) ----
typedef void (*test_fn)(void);
static const struct { test_fn f; const char *n, *crash; } T[] = {
    { t_requirements, "engine-requirements", NULL }, { t_floatctl, "float-controls", NULL }, { t_shmath, "shader-maths", NULL }, { t_barrier, "end-of-submit-barrier", NULL },
    { t_reinterp, "reinterpreted-format", NULL }, { t_fallback, "fallback-draw", NULL }, { t_dmabuf, "dmabuf-heap", "DM_VK_DMAHEAP=0" }, { t_bc, "bc-textures", "DM_VK_BC_GPU=0" },
    { t_depth_copy, "depth-copy", "DM_VK_ZCOPY=0" }, { t_int_view, "int-view", "DM_VK_INT_TEX=keep" }, { t_int_view_own, "int-view-own", NULL },
    { t_3d_slices, "3d-slices", NULL }, { t_3d_layered, "3d-layered", "DM_VK_NO_LAYERED_3D=1" } };   // (3D last: it crashed Xclipse 540's driver; 3d-layered's clear
                                                                                                    //   inside its pass asks for DM_VK_CLEAR_BY_PASS=1 instead, its
                                                                                                    //   read-back for nothing: crash_fix)
static int skipped(const char *skip, const char *name) {   // skip: names separated by commas
    size_t n = strlen(name);
    for (const char *p = skip; p && (p = strstr(p, name)); p += n) if ((p == skip || p[-1] == ',') && (p[n] == ',' || !p[n])) return 1;
    return 0;
}
static int s_dev_ok = -1;
static void run_tests(const char *skip) {
    size_t nt = sizeof T / sizeof *T; volatile size_t i;
    guard(1);
    for (i = 0; i < nt; i++) {
        if (skipped(skip, T[i].n)) { R(SKIP, T[i].n, "left out: it took the driver down in an earlier attempt"); continue; }
        if (sigsetjmp(g_jb, 1)) { const char *st = g_step, *fx = g_fix; R(FAIL, T[i].n, "the driver crashed during this test (signal %d%s%s): the game would close at the same point", (int)g_sig, st ? ", while " : "", st ? st : "");
            if (fx) FIX(T[i].n, fx);
            break; }
        g_step = NULL; g_fix = T[i].crash;
        J("begin %s%s%s\n", T[i].n, T[i].crash ? " " : "", T[i].crash ? T[i].crash : "");
        T[i].f();
    }
    guard(0);
    for (size_t j = i + 1; j < nt && i < nt; j++) R(SKIP, T[j].n, "not run: the driver crashed in an earlier test");
    O("Self-tests: %d pass, %d warn, %d fail, %d skip\n", g_count[PASS], g_count[WARN], g_count[FAIL], g_count[SKIP]);
}
void probe_selftests(void) {   // the probe app: the system's driver, every test
    O("\n== GPU self-tests (synthetic data: no game files)\n");
    memset(g_count, 0, sizeof g_count);
    meminfo();
    if (s_dev_ok < 0) s_dev_ok = !st_device();
    if (!s_dev_ok) return;
    O("  GPU %s, Vulkan %u.%u.%u, driver version %#x, vendor %#x device %#x\n", s_props.deviceName, VK_API_VERSION_MAJOR(s_props.apiVersion), VK_API_VERSION_MINOR(s_props.apiVersion),
      VK_API_VERSION_PATCH(s_props.apiVersion), s_props.driverVersion, s_props.vendorID, s_props.deviceID);
    run_tests(NULL);
    // the device stays alive for the rest of the probe: the BC layer keeps its objects for the process, as in the engine
}
// The games' driver check (dprobe_jni.c), in a process of its own: journal_fd = the file the app reads, driver = the game's DM_VK_DRIVER (NULL / "": the system's),
//   skip = tests that took the driver down in an earlier attempt. 0: ran to the end ("done" in the journal)
int dprobe_run(int journal_fd, const char *driver, const char *skip) {
    g_jfd = journal_fd; s_driver = driver;
    memset(g_count, 0, sizeof g_count);
    J("probe 1\n");
    if (st_device()) { J("nodevice\n"); return -1; }
    J("gpu %#x %#x %#x %u.%u.%u %s\n", s_props.vendorID, s_props.deviceID, s_props.driverVersion, VK_API_VERSION_MAJOR(s_props.apiVersion), VK_API_VERSION_MINOR(s_props.apiVersion),
      VK_API_VERSION_PATCH(s_props.apiVersion), s_props.deviceName);
    run_tests(skip);
    J("done %d %d %d %d\n", g_count[PASS], g_count[WARN], g_count[FAIL], g_count[SKIP]);
    return 0;
}

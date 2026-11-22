// t_vkdev: the engine's own Vulkan device set-up (nvn_vk.c mtl_init, compiled here as it is, in this translation unit so its decisions can be read) on whatever
//   device the loader gives it: on a PC, Mesa's lavapipe, alone or behind the Khronos profiles layer pretending to be a phone's GPU (simdev/run_simdev.sh). It checks
//   what the engine decided against what that device reports (read again independently, through the same instance), and against what is known for the GPU named
//   by SIMDEV_PROFILE (Arm's release from driverVersion, no dual-source blending on r44p1, the Vulkan 1.0 routes, ...). Then a little real work through the engine:
//   samplers with every optional feature asked for, a memory pool mapped for the GPU, a fill submitted and waited for.
//   Build: simdev/build_vkdev.sh. Run: SIMDEV_PROFILE=<name> ./t_vkdev (exit 1 on a failed check; "FAIL" lines name it).
// Nothing here changes the engine: the few symbols it takes from the rest of the host (page tracking, the game's clock, the overlay) are stubbed below, unused by the set-up.
#include "../../nvn/nvn_vk.c"
#include <execinfo.h>
#include <signal.h>

static int fails, checks;
static const char *volatile g_step = "start";   // (the step a crash happened in)
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define DEC(k, fmt, ...) printf("DECISION %-22s " fmt "\n", k, __VA_ARGS__)

// ---- the rest of the host, stubbed (none of it is reached by device set-up, samplers, pools or a fill) ----
void nvn_dirty_add_range(const void *mem, size_t size) { (void)mem; (void)size; }
void nvn_dirty_remove_range(const void *mem) { (void)mem; }
int nvn_dirty_tracked(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
void nvn_dirty_protect(const void *addr, size_t len) { (void)addr; (void)len; }
uint64_t nvn_dirty_now(void) { return 1; }
const void *nvn_dirty_span(const void *addr, size_t len) { (void)addr; (void)len; return NULL; }
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) { (void)span; (void)addr; (void)len; (void)stamp; return 1; }
void nvn_dirty_runs(const void *addr, size_t len, uint64_t stamp, void (*cb)(uintptr_t a, size_t n, void *u), void *u) { (void)stamp; cb((uintptr_t)addr, len, u); }
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { (void)fn; }
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }
void *front_metal_layer(void) { return NULL; }
_Atomic uint64_t g_wait_sync_ns, g_wait_sync_n;
uint64_t guest_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t nvn_new_serial(void) { static _Atomic uint64_t s = 1; return atomic_fetch_add(&s, 1); }
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) { (void)p; (void)stage; memset(key, 0, 8); return 0; }
void touch_ui_demo(int c, float dx, float dy) { (void)c; (void)dx; (void)dy; }
int dm_gpu_ts614(void) __attribute__((weak)); int dm_gpu_ts614(void) { return 0; }   // (newer engines: hle_nvn.c)

// ---- what the engine asks of the loader, watched: vkGetInstanceProcAddr wrapped, so the instance and device it makes are seen as they are made ----
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkCreateInstance real_ci; static PFN_vkCreateDevice real_cd;
static PFN_vkGetPhysicalDeviceFeatures2 real_f2; static PFN_vkGetPhysicalDeviceProperties2 real_p2; static PFN_vkGetPhysicalDeviceExternalBufferProperties real_xb;
static uint32_t w_inst_api; static int w_inst_p2ext;    // the instance as made: its apiVersion, VK_KHR_get_physical_device_properties2 enabled
static int w_f2_calls, w_f2_bad;                         // Features2 / Properties2 / external buffer queries, and those made where the instance has no right to them
static char w_dext[64][VK_MAX_EXTENSION_NAME_SIZE]; static uint32_t w_ndext; static int w_dev_made;
static VkResult w_dev_result = VK_RESULT_MAX_ENUM;
static int w_struct_bad;
// The GPU's identity. The profiles layer simulates limits, features, formats and extensions but on purpose not who the device is (vendorID, deviceID, driverVersion,
//   deviceName, deviceType and the driver properties are "not modifiable" there), and the engine's per-GPU choices key on exactly that. run_simdev.sh passes the
//   profile's identity (SIMDEV_VENDOR, SIMDEV_DEVICE_ID, SIMDEV_DRIVER_VERSION, SIMDEV_DEVICE_NAME, SIMDEV_DEVICE_TYPE, SIMDEV_DRIVER_ID / NAME / INFO) and the
//   answers the engine gets are given it here, above every layer
static PFN_vkGetPhysicalDeviceProperties real_p1;
static void ident_apply(VkPhysicalDeviceProperties *p) {
    const char *e;
    if ((e = getenv("SIMDEV_VENDOR")) && *e) p->vendorID = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("SIMDEV_DEVICE_ID")) && *e) p->deviceID = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("SIMDEV_DRIVER_VERSION")) && *e) p->driverVersion = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("SIMDEV_DEVICE_TYPE")) && *e) p->deviceType = (VkPhysicalDeviceType)atoi(e);
    if ((e = getenv("SIMDEV_DEVICE_NAME")) && *e) snprintf(p->deviceName, sizeof p->deviceName, "%s", e);
}
static void ident_apply2(VkPhysicalDeviceProperties2 *p) {
    ident_apply(&p->properties);
    for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext) {
        VkDriverId *id = NULL; char *name = NULL, *info = NULL;
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) { VkPhysicalDeviceDriverProperties *d = (void *)s; id = &d->driverID; name = d->driverName; info = d->driverInfo; }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) { VkPhysicalDeviceVulkan12Properties *d = (void *)s; id = &d->driverID; name = d->driverName; info = d->driverInfo; }
        const char *e;
        if (id && (e = getenv("SIMDEV_DRIVER_ID")) && *e) *id = (VkDriverId)atoi(e);
        if (name && (e = getenv("SIMDEV_DRIVER_NAME"))) snprintf(name, VK_MAX_DRIVER_NAME_SIZE, "%s", e);
        if (info && (e = getenv("SIMDEV_DRIVER_INFO"))) snprintf(info, VK_MAX_DRIVER_INFO_SIZE, "%s", e);
    }
}
static VKAPI_ATTR void VKAPI_CALL w_p1(VkPhysicalDevice p, VkPhysicalDeviceProperties *o) { real_p1(p, o); ident_apply(o); }
static void f2_note(const char *what) { w_f2_calls++; if (w_inst_api < VK_API_VERSION_1_1 && !w_inst_p2ext) { w_f2_bad++; printf("FAIL %s:%d: %s called on a Vulkan 1.0 instance without VK_KHR_get_physical_device_properties2\n", __FILE__, __LINE__, what); fails++; } }
static VKAPI_ATTR void VKAPI_CALL w_f2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *f) { f2_note("vkGetPhysicalDeviceFeatures2"); real_f2(p, f); }
static VKAPI_ATTR void VKAPI_CALL w_p2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *f) { f2_note("vkGetPhysicalDeviceProperties2"); real_p2(p, f); ident_apply2(f); }
static VKAPI_ATTR void VKAPI_CALL w_xb(VkPhysicalDevice p, const VkPhysicalDeviceExternalBufferInfo *i, VkExternalBufferProperties *o) {
    if (w_inst_api < VK_API_VERSION_1_1 && !w_inst_p2ext) { printf("FAIL %s:%d: vkGetPhysicalDeviceExternalBufferProperties on a Vulkan 1.0 instance\n", __FILE__, __LINE__); fails++; }
    real_xb(p, i, o); }
static VKAPI_ATTR VkResult VKAPI_CALL w_ci(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *o) {
    w_inst_api = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) if (!strcmp(ci->ppEnabledExtensionNames[i], "VK_KHR_get_physical_device_properties2")) w_inst_p2ext = 1;
    return real_ci(ci, a, o);
}
// Every structure in the device's create chain is allowed by the device's version or by an extension the engine enabled (a 1.2 / 1.3 block on an older device,
//   or an extension's structure without the extension, is invalid usage: drivers may refuse the device or ignore it)
static int w_has(const char *e) { for (uint32_t i = 0; i < w_ndext; i++) if (!strcmp(w_dext[i], e)) return 1; return 0; }
// lavapipe itself, opened directly (its ICD library, no loader, no layers): what it really has, to tell the simulation's gaps from the engine's choices
static struct { int tried; VkPhysicalDevice pd; PFN_vkGetPhysicalDeviceFeatures2 f2; VkExtensionProperties *ext; uint32_t next; } LVP;
static size_t feat_size(VkStructureType t) {
    switch ((int)t) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: return sizeof(VkPhysicalDeviceFeatures2);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: return sizeof(VkPhysicalDeviceVulkan11Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: return sizeof(VkPhysicalDeviceVulkan12Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: return sizeof(VkPhysicalDeviceVulkan13Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: return sizeof(VkPhysicalDeviceDynamicRenderingFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: return sizeof(VkPhysicalDeviceShaderDrawParametersFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: return sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: return sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: return sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT: return sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: return sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT: return sizeof(VkPhysicalDeviceImageCompressionControlFeaturesEXT);
    }
    return 0;
}
static const char *feat_name(VkStructureType t) { static char b[32]; snprintf(b, sizeof b, "structure %d", (int)t); return b; }
static void lvp_open(void) {
    if (LVP.tried++) return;
    const char *icd = getenv("VK_ICD_FILENAMES"); char lib[512] = "libvulkan_lvp.so";
    if (icd) { FILE *f = fopen(icd, "r"); char j[4096] = ""; if (f) { size_t n = fread(j, 1, sizeof j - 1, f); j[n] = 0; fclose(f); }
               char *p = strstr(j, "\"library_path\""); if (p && (p = strchr(p + 14, '"'))) { char *e = strchr(p + 1, '"'); if (e && e - p - 1 < (long)sizeof lib) { memcpy(lib, p + 1, (size_t)(e - p - 1)); lib[e - p - 1] = 0; } } }
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL); if (!h) { printf("NOTE lavapipe not opened directly (%s): no simulation-gap filtering\n", lib); return; }
    PFN_vkGetInstanceProcAddr gi = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr"); if (!gi) return;
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai }; VkInstance in;
    if (((PFN_vkCreateInstance)gi(NULL, "vkCreateInstance"))(&ici, NULL, &in)) return;
    uint32_t n = 1; ((PFN_vkEnumeratePhysicalDevices)gi(in, "vkEnumeratePhysicalDevices"))(in, &n, &LVP.pd); if (!n) { LVP.pd = VK_NULL_HANDLE; return; }
    LVP.f2 = (PFN_vkGetPhysicalDeviceFeatures2)gi(in, "vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties ee = (PFN_vkEnumerateDeviceExtensionProperties)gi(in, "vkEnumerateDeviceExtensionProperties");
    ee(LVP.pd, NULL, &LVP.next, NULL); LVP.ext = calloc(LVP.next + 1, sizeof *LVP.ext); ee(LVP.pd, NULL, &LVP.next, LVP.ext);
}
static int lvp_has_ext(const char *e) { lvp_open(); if (!LVP.ext) return 1; for (uint32_t i = 0; i < LVP.next; i++) if (!strcmp(LVP.ext[i].extensionName, e)) return 1; return 0; }
// member names of the feature structures the engine enables (vulkan_core.h order), for the SIMGAP lines
static const char *const FN10[] = { "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat", "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing", "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance", "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries", };
static const char *const FN11[] = { "storageBuffer16BitAccess", "uniformAndStorageBuffer16BitAccess", "storagePushConstant16", "storageInputOutput16", "multiview", "multiviewGeometryShader", "multiviewTessellationShader", "variablePointersStorageBuffer", "variablePointers", "protectedMemory", "samplerYcbcrConversion", "shaderDrawParameters", };
static const char *const FN12[] = { "samplerMirrorClampToEdge", "drawIndirectCount", "storageBuffer8BitAccess", "uniformAndStorageBuffer8BitAccess", "storagePushConstant8", "shaderBufferInt64Atomics", "shaderSharedInt64Atomics", "shaderFloat16", "shaderInt8", "descriptorIndexing", "shaderInputAttachmentArrayDynamicIndexing", "shaderUniformTexelBufferArrayDynamicIndexing", "shaderStorageTexelBufferArrayDynamicIndexing", "shaderUniformBufferArrayNonUniformIndexing", "shaderSampledImageArrayNonUniformIndexing", "shaderStorageBufferArrayNonUniformIndexing", "shaderStorageImageArrayNonUniformIndexing", "shaderInputAttachmentArrayNonUniformIndexing", "shaderUniformTexelBufferArrayNonUniformIndexing", "shaderStorageTexelBufferArrayNonUniformIndexing", "descriptorBindingUniformBufferUpdateAfterBind", "descriptorBindingSampledImageUpdateAfterBind", "descriptorBindingStorageImageUpdateAfterBind", "descriptorBindingStorageBufferUpdateAfterBind", "descriptorBindingUniformTexelBufferUpdateAfterBind", "descriptorBindingStorageTexelBufferUpdateAfterBind", "descriptorBindingUpdateUnusedWhilePending", "descriptorBindingPartiallyBound", "descriptorBindingVariableDescriptorCount", "runtimeDescriptorArray", "samplerFilterMinmax", "scalarBlockLayout", "imagelessFramebuffer", "uniformBufferStandardLayout", "shaderSubgroupExtendedTypes", "separateDepthStencilLayouts", "hostQueryReset", "timelineSemaphore", "bufferDeviceAddress", "bufferDeviceAddressCaptureReplay", "bufferDeviceAddressMultiDevice", "vulkanMemoryModel", "vulkanMemoryModelDeviceScope", "vulkanMemoryModelAvailabilityVisibilityChains", "shaderOutputViewportIndex", "shaderOutputLayer", "subgroupBroadcastDynamicId", };
static const char *const FN13[] = { "robustImageAccess", "inlineUniformBlock", "descriptorBindingInlineUniformBlockUpdateAfterBind", "pipelineCreationCacheControl", "privateData", "shaderDemoteToHelperInvocation", "shaderTerminateInvocation", "subgroupSizeControl", "computeFullSubgroups", "synchronization2", "textureCompressionASTC_HDR", "shaderZeroInitializeWorkgroupMemory", "dynamicRendering", "shaderIntegerDotProduct", "maintenance4", };
static void lvp_mask(VkStructureType t, const char *what, VkBool32 *b, size_t n) {
    lvp_open(); if (!LVP.f2) return;
    static unsigned char q[1024]; memset(q, 0, sizeof q); VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkBool32 *have;
    if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) { LVP.f2(LVP.pd, &f2); have = (VkBool32 *)&f2.features; }
    else { ((VkBaseOutStructure *)q)->sType = t; f2.pNext = q; LVP.f2(LVP.pd, &f2); have = (VkBool32 *)((VkBaseOutStructure *)q + 1); }
    const char *const *nm = NULL; size_t nn = 0;
#define NM(a) (nm = a, nn = sizeof a / sizeof *a)
    if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) NM(FN10); else if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) NM(FN11);
    else if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) NM(FN12); else if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) NM(FN13);
#undef NM
    for (size_t i = 0; i < n; i++) if (b[i] && !have[i]) { b[i] = VK_FALSE;
        if (i < nn) printf("SIMGAP feature %s: the profile has it, lavapipe has not (not enabled)\n", nm[i]);
        else printf("SIMGAP feature %s member %zu: the profile has it, lavapipe has not (not enabled)\n", what, i); }
}
static VKAPI_ATTR VkResult VKAPI_CALL w_cd(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *o) {
    w_dev_made = 1; w_ndext = ci->enabledExtensionCount > 64 ? 64 : ci->enabledExtensionCount;
    for (uint32_t i = 0; i < w_ndext; i++) snprintf(w_dext[i], sizeof w_dext[i], "%s", ci->ppEnabledExtensionNames[i]);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    uint32_t api = pp.apiVersion < w_inst_api ? pp.apiVersion : w_inst_api;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        const char *need = NULL; uint32_t need_api = 0; const char *name = "";
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: name = "Features2"; need_api = VK_API_VERSION_1_1; need = "VK_KHR_get_physical_device_properties2"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: name = "Vulkan11Features"; need_api = VK_API_VERSION_1_2; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: name = "Vulkan12Features"; need_api = VK_API_VERSION_1_2; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: name = "Vulkan13Features"; need_api = VK_API_VERSION_1_3; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: name = "TimelineSemaphoreFeatures"; need_api = VK_API_VERSION_1_2; need = "VK_KHR_timeline_semaphore"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: name = "DynamicRenderingFeatures"; need_api = VK_API_VERSION_1_3; need = "VK_KHR_dynamic_rendering"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: name = "ShaderDrawParametersFeatures"; need_api = VK_API_VERSION_1_1; need = "VK_KHR_shader_draw_parameters"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: name = "ExtendedDynamicStateFeatures"; need_api = VK_API_VERSION_1_3; need = "VK_EXT_extended_dynamic_state"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: name = "ExtendedDynamicState2Features"; need_api = VK_API_VERSION_1_3; need = "VK_EXT_extended_dynamic_state2"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: name = "CustomBorderColorFeatures"; need_api = ~0u; need = "VK_EXT_custom_border_color"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT: name = "DepthClipControlFeatures"; need_api = ~0u; need = "VK_EXT_depth_clip_control"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: name = "VertexAttributeDivisorFeatures"; need_api = ~0u; need = "VK_EXT_vertex_attribute_divisor"; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT: name = "ImageCompressionControlFeatures"; need_api = ~0u; need = "VK_EXT_image_compression_control"; break;
        default: printf("NOTE device create chain: structure type %d not checked here\n", (int)s->sType); continue;
        }
        int ok = (need_api != ~0u && api >= need_api) || (need && (w_has(need) || (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && w_inst_p2ext)));
        if (!ok) { w_struct_bad++; printf("FAIL %s:%d: vkCreateDevice chain holds %s on a Vulkan %u.%u device without %s\n", __FILE__, __LINE__, name,
                                           VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), need ? need : "(core in a later version only)"); fails++; }
    }
    // The simulation's gap: the profile may promise features / extensions lavapipe has not got, and the engine (rightly) enables every feature the device reports.
    //   Those are taken out of the request before it goes down (the validation layer still sees a request within the simulated device), each one named as SIMGAP:
    //   a limit of the simulation, not an engine finding
    VkDeviceCreateInfo c = *ci; const char *keep[64]; uint32_t nk = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && nk < 64; i++) { if (lvp_has_ext(ci->ppEnabledExtensionNames[i])) keep[nk++] = ci->ppEnabledExtensionNames[i]; else printf("SIMGAP extension %s: the profile lists it, lavapipe has not got it (not enabled)\n", ci->ppEnabledExtensionNames[i]); }
    c.enabledExtensionCount = nk; c.ppEnabledExtensionNames = keep;
    VkPhysicalDeviceFeatures ef; if (ci->pEnabledFeatures) { ef = *ci->pEnabledFeatures; lvp_mask(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "VkPhysicalDeviceFeatures", (VkBool32 *)&ef, sizeof ef / 4); c.pEnabledFeatures = &ef; }
    // the chain copied, each structure's VkBool32 members ANDed with lavapipe's
    static unsigned char copy[16][1024]; VkBaseOutStructure *prev = NULL; int ncopy = 0; c.pNext = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s && ncopy < 16; s = s->pNext) {
        size_t sz = feat_size(s->sType); VkBaseOutStructure *d = (VkBaseOutStructure *)copy[ncopy++];
        memcpy(d, s, sz ? sz : sizeof(VkBaseInStructure)); d->pNext = NULL;
        if (sz) { if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) lvp_mask(s->sType, "VkPhysicalDeviceFeatures", (VkBool32 *)&((VkPhysicalDeviceFeatures2 *)d)->features, sizeof(VkPhysicalDeviceFeatures) / 4);
                  else lvp_mask(s->sType, feat_name(s->sType), (VkBool32 *)(d + 1), (sz - sizeof(VkBaseOutStructure)) / 4); }
        if (prev) prev->pNext = d; else c.pNext = d; prev = d;
    }
    return w_dev_result = real_cd(pd, &c, a, o);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_gipa(VkInstance inst, const char *n) {
    PFN_vkVoidFunction f = real_gipa(inst, n); if (!f) return NULL;
    if (!strcmp(n, "vkCreateInstance")) { real_ci = (PFN_vkCreateInstance)f; return (PFN_vkVoidFunction)w_ci; }
    if (!strcmp(n, "vkCreateDevice")) { real_cd = (PFN_vkCreateDevice)f; return (PFN_vkVoidFunction)w_cd; }
    if (!strcmp(n, "vkGetPhysicalDeviceProperties")) { real_p1 = (PFN_vkGetPhysicalDeviceProperties)f; return (PFN_vkVoidFunction)w_p1; }
    if (!strcmp(n, "vkGetPhysicalDeviceFeatures2") || !strcmp(n, "vkGetPhysicalDeviceFeatures2KHR")) { real_f2 = (PFN_vkGetPhysicalDeviceFeatures2)f; return (PFN_vkVoidFunction)w_f2; }
    if (!strcmp(n, "vkGetPhysicalDeviceProperties2") || !strcmp(n, "vkGetPhysicalDeviceProperties2KHR")) { real_p2 = (PFN_vkGetPhysicalDeviceProperties2)f; return (PFN_vkVoidFunction)w_p2; }
    if (!strcmp(n, "vkGetPhysicalDeviceExternalBufferProperties") || !strcmp(n, "vkGetPhysicalDeviceExternalBufferPropertiesKHR")) { real_xb = (PFN_vkGetPhysicalDeviceExternalBufferProperties)f; return (PFN_vkVoidFunction)w_xb; }
    return f;
}

// ---- the device as it reports itself (after the engine made its instance: the same loader, layers and profile) ----
static struct { VkPhysicalDeviceProperties p; VkPhysicalDeviceFeatures f; uint32_t next; VkExtensionProperties *ext;
                int has_f2, has_p2; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12; VkPhysicalDeviceVulkan13Features f13;
                VkPhysicalDeviceTimelineSemaphoreFeatures fts; VkPhysicalDeviceDynamicRenderingFeatures fdr; VkPhysicalDeviceShaderDrawParametersFeatures fsdp;
                VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc; VkPhysicalDeviceDepthClipControlFeaturesEXT fcc;
                VkPhysicalDeviceDriverProperties drv; int drv_ok; VkPhysicalDeviceFloatControlsProperties fc; int fc_ok; } DV;
static int dev_ext(const char *e) { for (uint32_t i = 0; i < DV.next; i++) if (!strcmp(DV.ext[i].extensionName, e)) return 1; return 0; }
static void device_read(void) {
    vkGetPhysicalDeviceProperties(g_pd, &DV.p); vkGetPhysicalDeviceFeatures(g_pd, &DV.f);
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &DV.next, NULL); DV.ext = calloc(DV.next + 1, sizeof *DV.ext); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &DV.next, DV.ext);
    uint32_t api = DV.p.apiVersion < g_instance_api ? DV.p.apiVersion : g_instance_api;
    PFN_vkGetPhysicalDeviceFeatures2 f2 = real_f2; PFN_vkGetPhysicalDeviceProperties2 p2 = real_p2;
    DV.has_f2 = f2 && (api >= VK_API_VERSION_1_1 || w_inst_p2ext); DV.has_p2 = p2 && DV.has_f2;
    if (DV.has_f2) {   // each structure on its own, only where the device's version or extension allows it
        VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
#define F2(s, st, cond) do { if (cond) { memset(&s, 0, sizeof s); s.sType = st; q.pNext = &s; real_f2(g_pd, &q); q.pNext = NULL; s.pNext = NULL; } } while (0)
        F2(DV.f11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, api >= VK_API_VERSION_1_2);
        F2(DV.f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, api >= VK_API_VERSION_1_2);
        F2(DV.f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, api >= VK_API_VERSION_1_3);
        F2(DV.fts, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, api >= VK_API_VERSION_1_2 || dev_ext("VK_KHR_timeline_semaphore"));
        F2(DV.fdr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, api >= VK_API_VERSION_1_3 || dev_ext("VK_KHR_dynamic_rendering"));
        F2(DV.fsdp, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, api >= VK_API_VERSION_1_1);
        F2(DV.fbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, dev_ext("VK_EXT_custom_border_color"));
        F2(DV.fcc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, dev_ext("VK_EXT_depth_clip_control"));
#undef F2
        VkPhysicalDeviceProperties2 pq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        if (api >= VK_API_VERSION_1_2 || dev_ext("VK_KHR_driver_properties")) { DV.drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES; pq.pNext = &DV.drv; real_p2(g_pd, &pq); ident_apply2(&pq); DV.drv_ok = 1; DV.drv.pNext = NULL; }
        if (api >= VK_API_VERSION_1_2 || dev_ext("VK_KHR_shader_float_controls")) { DV.fc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES; pq.pNext = &DV.fc; real_p2(g_pd, &pq); ident_apply2(&pq); DV.fc_ok = 1; DV.fc.pNext = NULL; }
    }
}

// ---- what is known for each simulated GPU (profiles/<name>.json; the facts players' logs and Arm's notes give) ----
typedef struct { const char *name; uint32_t vendor; int api_minor; int arm_rel, arm_p, arm_beta; int dualsrc; int clampzero; int dynren; int tlsem; int bc_native; } expect_t;
static const expect_t EXPECT[] = {
    //  name                   vendor  api  rel p beta dual clamp0 dynren tlsem bc
    { "mali-g720-r44p1",      0x13B5, 3,  44, 1, 0,   0,   1,     1,     1,    0 },   // r44p1: no dualSrcBlend, no robustness2; erratum 3779215 (r38p1-r49p0)
    { "mali-g720-r49p1",      0x13B5, 3,  49, 1, 1,   1,   1,     1,     1,    0 },   // the report's driver is a beta r49p1 ("v1.r49p1-03bet0"): the erratum kept
    { "mali-g72-r26p0",       0x13B5, 1,  26, 0, 0,   0,   0,     0,     1,    0 },   // Bifrost, Vulkan 1.1: render passes, timeline semaphores by extension
    { "mali-g72-r14p0",       0x13B5, 0,  14, 0, 0,   0,   0,     0,     0,    0 },   // Vulkan 1.0: a fence per submission, SPIR-V 1.0, Arm's release from the vendor ID
    { "powervr-bxm-8-256",    0x1010, 1,  0,  0, 0,   0,   0,     0,     1,    0 },
    { "powervr-dxt-48-1536",  0x1010, 3,  0,  0, 0,   1,   0,     1,     1,    0 },   // (a 1.4 driver: the engine's instance asks for 1.3)
    { "xclipse-920",          0x144D, 3,  0,  0, 0,   1,   0,     1,     1,    0 },   // (no BC in these reports: Samsung's driver lists none)
    { "xclipse-940",          0x144D, 3,  0,  0, 0,   1,   0,     1,     1,    0 },
    // added 2026-11-21 (generated from each profile: vendor, Vulkan version up to 1.3, Arm release from driverVersion, dualSrcBlend, the erratum range, dynamic rendering
    //   where the device has it except on Adreno (render passes by default), timeline semaphores, textureCompressionBC)
    { "adreno-830",          0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-750",          0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-740",          0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-735",          0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-710-vk13",     0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-710-vk11",     0x5143, 1,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-660",          0x5143, 1,  0 , 0, 0,   1,   0,     0,     1,    0 },
    { "adreno-650",          0x5143, 1,  0 , 0, 0,   1,   0,     0,     0,    0 },
    { "adreno-619",          0x5143, 1,  0 , 0, 0,   1,   0,     0,     0,    0 },
    { "adreno-613-vk11",     0x5143, 1,  0 , 0, 0,   1,   0,     0,     0,    1 },
    { "adreno-613-vk13",     0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "adreno-610",          0x5143, 1,  0 , 0, 0,   1,   0,     0,     0,    1 },
    { "adreno-506",          0x5143, 1,  0 , 0, 0,   1,   0,     0,     0,    0 },
    { "adreno-512-vk10",     0x5143, 0,  0 , 0, 0,   0,   0,     0,     0,    0 },
    { "turnip-adreno-650",   0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "turnip-adreno-740",   0x5143, 3,  0 , 0, 0,   1,   0,     0,     1,    1 },
    { "mali-g615-mc6-r44p1", 0x13B5, 3,  44, 1, 0,   0,   1,     1,     1,    0 },
    { "mali-g615-mc2-r44p1", 0x13B5, 3,  44, 1, 0,   0,   1,     1,     1,    0 },
    { "mali-g57-r32p1",      0x13B5, 1,  32, 1, 0,   0,   0,     0,     1,    0 },
    { "mali-g57-r38p1",      0x13B5, 3,  38, 1, 0,   0,   1,     1,     1,    0 },
    { "mali-g57-r54p1",      0x13B5, 3,  54, 1, 0,   1,   0,     1,     1,    0 },
    { "mali-g52-r32p1",      0x13B5, 1,  32, 1, 0,   0,   0,     0,     1,    0 },
    { "mali-g52-r49p1",      0x13B5, 3,  49, 1, 1,   1,   1,     1,     1,    0 },
    { "mali-g610-r32p1",     0x13B5, 1,  32, 1, 0,   0,   0,     0,     1,    0 },
    { "mali-g610-r38p1",     0x13B5, 1,  38, 1, 0,   0,   1,     0,     1,    0 },
    { "mali-g68-r32p1",      0x13B5, 1,  32, 1, 1,   0,   0,     0,     1,    0 },
    { "mali-g710-r32p1",     0x13B5, 1,  32, 1, 0,   0,   0,     0,     1,    0 },
    { "mali-g310-r44p1",     0x13B5, 3,  44, 1, 0,   0,   1,     1,     1,    0 },
    { "mali-g925-r49p1",     0x13B5, 3,  49, 1, 1,   1,   1,     1,     1,    0 },
    { "powervr-ge8320",      0x1010, 1,  0 , 0, 0,   0,   0,     0,     0,    0 },
    { "powervr-ge8322-vk13", 0x1010, 3,  0 , 0, 0,   1,   0,     1,     1,    0 },
    { "xclipse-530",         0x144D, 3,  0 , 0, 0,   1,   0,     1,     1,    0 },
    { "xclipse-540",         0x144D, 3,  0 , 0, 0,   1,   0,     1,     1,    0 },
    { "xclipse-950",         0x144D, 3,  0 , 0, 0,   1,   0,     1,     1,    0 },
};

static void decisions_and_checks(const char *profile) {
    uint32_t api = DV.p.apiVersion < g_instance_api ? DV.p.apiVersion : g_instance_api;
    int arm_drv = DV.drv_ok ? DV.drv.driverID == VK_DRIVER_ID_ARM_PROPRIETARY : DV.p.vendorID == 0x13B5;
    int sec_drv = DV.drv_ok ? DV.drv.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY : DV.p.vendorID == 0x144D;
    printf("DEVICE %s, vendor %#x, Vulkan %u.%u.%u, driverVersion %#x, driver %s (id %d) %s\n", DV.p.deviceName, DV.p.vendorID, VK_API_VERSION_MAJOR(DV.p.apiVersion),
           VK_API_VERSION_MINOR(DV.p.apiVersion), VK_API_VERSION_PATCH(DV.p.apiVersion), DV.p.driverVersion, DV.drv_ok ? DV.drv.driverName : "-", DV.drv_ok ? (int)DV.drv.driverID : -1, DV.drv_ok ? DV.drv.driverInfo : "");
    printf("DEVICE extensions enabled by the engine (%u):", w_ndext); for (uint32_t i = 0; i < w_ndext; i++) printf(" %s", w_dext[i]); printf("\n");
    DEC("api", "%u.%u", VK_API_VERSION_MAJOR(g_api), VK_API_VERSION_MINOR(g_api));
    DEC("arm_release", "r%dp%d", g_arm_rel, g_arm_p); DEC("arm_beta", "%d", g_arm_beta);
    DEC("dual_source_blend", "%d", g_dualsrc); DEC("dynamic_rendering", "%d", g_dynren); DEC("extended_dyn_state", "%d", g_eds); DEC("eds2", "%d", g_eds2);
    DEC("timeline_semaphores", "%d", g_tlsem); DEC("push_descriptors", "%d", !g_nopush); DEC("max_push", "%u", g_maxpush); DEC("format_lists", "%d", g_fmtlist);
    DEC("store_op_none", "%d", g_store_none); DEC("host_import", "%d", g_host_import); DEC("fd_route", "%d", g_fdmap); DEC("heap_route", "%d", g_heapmap);
    DEC("depth_clip_control", "%d", g_clipctl); DEC("custom_border", "%d", g_border); DEC("vertex_divisor", "%d", g_divisor);
    DEC("float_controls", "%d", g_fctl); DEC("clamp_zero", "%d", g_clampzero); DEC("nan_minmax", "%d", g_nanmm); DEC("exact_math", "%u", g_exact); DEC("init_locals", "%d", g_initloc);
    DEC("draw_params", "%d", g_drawp); DEC("sb_as_uniform", "%d", g_nosbsc); DEC("minmax_filter", "%d", g_minmax); DEC("aniso", "%d", g_f_aniso);
    DEC("depth_clamp", "%d", g_f_clamp); DEC("logic_op", "%d", g_f_logic); DEC("independent_blend", "%d", g_f_indblend);
    DEC("spirv10_modules", "%d", g_csm_real != NULL);
    DEC("bc_textures", "%s", nvn_fmt_vk_transcode(0x46) ? "transcoded" : nvn_fmt_vk_decode(0x46) ? "decoded" : "sampled");
    DEC("bc_gpu_layer", "%d", g_bcgpu);
    int brel = barrier_relaxed(), pdef = pass_defer_on(), apso = apso_on(), cbp = clear_by_pass(), pkeep = pso_cap();
    DEC("barrier_relaxed", "%d", brel); DEC("pass_defer", "%d", pdef); DEC("async_pso", "%d", apso); DEC("clear_by_pass", "%d", cbp); DEC("pipelines_kept", "%d", pkeep);

    // what the engine enabled exists
    CHECK(w_dev_made && w_dev_result == VK_SUCCESS, "the device was made (vkCreateDevice %d)", (int)w_dev_result);
    for (uint32_t i = 0; i < w_ndext; i++) CHECK(dev_ext(w_dext[i]), "extension %s enabled but the device does not list it", w_dext[i]);
    for (uint32_t i = 0; i < w_ndext; i++) for (uint32_t j = i + 1; j < w_ndext; j++) CHECK(strcmp(w_dext[i], w_dext[j]), "extension %s enabled twice", w_dext[i]);
    // the version and what follows from it
    CHECK(g_api == api, "g_api %#x, the device's and instance's lower version %#x", g_api, api);
    CHECK((g_csm_real != NULL) == (api < VK_API_VERSION_1_1), "SPIR-V 1.0 modules (%d) exactly on a Vulkan 1.0 device", g_csm_real != NULL);
    if (api < VK_API_VERSION_1_2) CHECK(!g_tlsem || dev_ext("VK_KHR_timeline_semaphore"), "timeline semaphores used on Vulkan 1.%u without the extension", VK_API_VERSION_MINOR(api));
    if (g_tlsem) CHECK(api >= VK_API_VERSION_1_2 ? DV.f12.timelineSemaphore : DV.fts.timelineSemaphore, "timeline semaphores used, the feature is off");
    if (api < VK_API_VERSION_1_2) CHECK(!g_fmtlist || w_has("VK_KHR_image_format_list"), "view format lists on Vulkan 1.%u without VK_KHR_image_format_list (#158)", VK_API_VERSION_MINOR(api));
    if (api < VK_API_VERSION_1_3) CHECK(!g_store_none || w_has("VK_KHR_load_store_op_none") || w_has("VK_EXT_load_store_op_none"), "store op NONE without its extension");
    if (g_dynren) CHECK(api >= VK_API_VERSION_1_3 ? DV.f13.dynamicRendering : (w_has("VK_KHR_dynamic_rendering") && DV.fdr.dynamicRendering), "dynamic rendering used where the device has none");
    if (g_eds && api < VK_API_VERSION_1_3) CHECK(w_has("VK_EXT_extended_dynamic_state"), "extended dynamic state used without the extension");
    CHECK(!g_eds2 || w_has("VK_EXT_extended_dynamic_state2"), "eds2 without the extension");
    CHECK(g_nopush == !w_has("VK_KHR_push_descriptor"), "push descriptors %d, extension enabled %d", !g_nopush, w_has("VK_KHR_push_descriptor"));
    CHECK(!g_clipctl || (w_has("VK_EXT_depth_clip_control") && DV.fcc.depthClipControl), "depth clip control used without extension / feature");
    CHECK(!g_border || (w_has("VK_EXT_custom_border_color") && DV.fbc.customBorderColors && DV.fbc.customBorderColorWithoutFormat), "custom border colours without extension / formatless feature");
    CHECK(!g_divisor || w_has("VK_EXT_vertex_attribute_divisor"), "vertex divisor without the extension");
    CHECK(!g_host_import || w_has("VK_EXT_external_memory_host"), "host import without the extension");
    CHECK(!g_minmax || (api >= VK_API_VERSION_1_2 ? DV.f12.samplerFilterMinmax : w_has("VK_EXT_sampler_filter_minmax")), "min / max filtering without the feature / extension");
    if (api >= VK_API_VERSION_1_1) CHECK(g_drawp == (api >= VK_API_VERSION_1_2 ? DV.f11.shaderDrawParameters : DV.fsdp.shaderDrawParameters) != 0, "draw parameters %d", g_drawp);
    else CHECK(g_drawp == w_has("VK_KHR_shader_draw_parameters"), "Vulkan 1.0: draw parameters %d, extension %d", g_drawp, w_has("VK_KHR_shader_draw_parameters"));
    if (api < VK_API_VERSION_1_1) CHECK(g_nosbsc == !w_has("VK_KHR_storage_buffer_storage_class"), "Vulkan 1.0: storage buffers as uniform %d", g_nosbsc);
    // optional Vulkan 1.0 features: each one the engine uses is one the device has (the fallback taken otherwise)
    CHECK(g_dualsrc == (DV.f.dualSrcBlend != 0), "dual-source blending %d, the device's dualSrcBlend %d", g_dualsrc, DV.f.dualSrcBlend);
    CHECK(g_f_aniso == (DV.f.samplerAnisotropy != 0) && g_f_clamp == (DV.f.depthClamp != 0) && g_f_logic == (DV.f.logicOp != 0) && g_f_indblend == (DV.f.independentBlend != 0)
          && g_f_nonsolid == (DV.f.fillModeNonSolid != 0) && g_f_biasclamp == (DV.f.depthBiasClamp != 0), "optional 1.0 features follow the device");
    // BC textures: sampled only where the device has them; PowerVR decoded, not transcoded
    if (!DV.f.textureCompressionBC) CHECK(nvn_fmt_vk_transcode(0x46) || nvn_fmt_vk_decode(0x46), "BC1 sampled on a device without BC");
    if (DV.p.vendorID == 0x1010) CHECK(!nvn_fmt_vk_transcode(0x46), "PowerVR: BC transcoded (#32: should be decoded)");
    // the quirk table's defaults (vendor / driver keyed), as vk_quirks.h states them
    { int v = g_arm_rel * 100 + g_arm_p;
      CHECK(g_arm_rel == (arm_drv ? (int)(DV.p.driverVersion >> 22) : 0) && g_arm_p == (arm_drv ? (int)(DV.p.driverVersion >> 12 & 0x3ff) : 0), "Arm release r%dp%d from driverVersion %#x", g_arm_rel, g_arm_p, DV.p.driverVersion);
      CHECK(g_clampzero == (g_arm_rel && ((v >= 3801 && v < 4901) || (g_arm_beta && v >= 4901 && v < 4902) || (v >= 5000 && v < 5101))), "clamp-zero erratum %d for r%dp%d%s", g_clampzero, g_arm_rel, g_arm_p, g_arm_beta ? " beta" : "");
      CHECK(g_nanmm == (g_arm_rel ? 2 : 0) && (g_exact == (g_arm_rel ? EXACT_ALL : 0u)) && g_initloc == (g_arm_rel != 0), "Arm shader rules (nan %d exact %u init %d)", g_nanmm, g_exact, g_initloc);
      if (DV.fc_ok) CHECK(g_fctl == (DV.fc.shaderSignedZeroInfNanPreserveFloat32 && (arm_drv || sec_drv)), "float controls %d (Arm %d, Samsung %d, supported %d)", g_fctl, arm_drv, sec_drv, DV.fc.shaderSignedZeroInfNanPreserveFloat32);
      else CHECK(!g_fctl, "float controls asked for where the driver cannot say it has them"); }
    CHECK(g_dynren == 0 || DV.p.vendorID != 0x5143, "Adreno: render passes by default");
    CHECK(pdef == (DV.p.vendorID == 0x5143) && apso == (DV.p.vendorID == 0x5143) && cbp == (DV.p.vendorID == 0x144D), "per-vendor pass / pipeline / clear defaults");
    if (DV.p.vendorID == 0x13B5) CHECK(pkeep == 800, "Mali: at most 800 pipelines kept (%d)", pkeep);
    if (DV.p.vendorID == 0x144D) CHECK(!g_host_import, "Xclipse: pools not imported as host memory (#39)");
    // the profile's own facts
    if (profile && *profile) {
        const expect_t *e = NULL; for (unsigned i = 0; i < sizeof EXPECT / sizeof *EXPECT; i++) if (!strcmp(EXPECT[i].name, profile)) e = &EXPECT[i];
        // the auto-discovered profiles (discover.py) have no row: profile_env.py's SIMDEV_EXPECT, computed from the profile by the rows' own rules
        static expect_t ax; const char *xs = getenv("SIMDEV_EXPECT"); unsigned v[10];
        if (xs && *xs && sscanf(xs, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) == 10) {
            ax = (expect_t){ profile, v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], (int)v[5], (int)v[6], (int)v[7], (int)v[8], (int)v[9] };
            if (e) CHECK(e->vendor == ax.vendor && e->api_minor == ax.api_minor && e->arm_rel == ax.arm_rel && e->arm_p == ax.arm_p && e->arm_beta == ax.arm_beta && e->dualsrc == ax.dualsrc
                         && e->clampzero == ax.clampzero && e->dynren == ax.dynren && e->tlsem == ax.tlsem && e->bc_native == ax.bc_native,
                         "%s: the hand-written row and profile_env.py's SIMDEV_EXPECT (%s) differ", profile, xs);
            else e = &ax; }
        CHECK(e != NULL, "no expectations for profile %s", profile);
        if (e) {
            CHECK(DV.p.vendorID == e->vendor, "%s: vendor %#x, simulated %#x (is the profile layer on?)", profile, e->vendor, DV.p.vendorID);
            CHECK((int)VK_API_VERSION_MINOR(g_api) == e->api_minor, "%s: Vulkan 1.%d expected, the engine runs 1.%u", profile, e->api_minor, VK_API_VERSION_MINOR(g_api));
            CHECK(g_arm_rel == e->arm_rel && g_arm_p == e->arm_p && g_arm_beta == e->arm_beta, "%s: Arm r%dp%d%s expected, decided r%dp%d%s", profile, e->arm_rel, e->arm_p, e->arm_beta ? " beta" : "", g_arm_rel, g_arm_p, g_arm_beta ? " beta" : "");
            CHECK(g_dualsrc == e->dualsrc, "%s: dual-source blending %d expected, %d", profile, e->dualsrc, g_dualsrc);
            CHECK(g_clampzero == e->clampzero, "%s: clamp-zero erratum %d expected, %d", profile, e->clampzero, g_clampzero);
            CHECK(g_dynren == e->dynren, "%s: dynamic rendering %d expected, %d", profile, e->dynren, g_dynren);
            CHECK(g_tlsem == e->tlsem, "%s: timeline semaphores %d expected, %d", profile, e->tlsem, g_tlsem);
            CHECK((!nvn_fmt_vk_transcode(0x46) && !nvn_fmt_vk_decode(0x46)) == e->bc_native, "%s: BC sampled natively %d expected", profile, e->bc_native);
            if (!strcmp(profile, "mali-g720-r44p1")) CHECK(!dev_ext("VK_EXT_robustness2"), "r44p1 lists VK_EXT_robustness2: the profile is wrong");
        }
    }
}

// ---- the unified driver layer (vk_quirks.h), where the engine has it: the GPU description it decides from, and every quirk's default for this GPU ----
#ifdef VK_QUIRKS_H
static void quirk_checks(void) {
    const gpu_id *g = &g_gpuid;
    printf("QUIRKS gpu_id: vendor %#x, driver version %#x, driver id %d, Arm r%dp%d%s, api %#x, dynren %d, custom %d, port %d, name %s\n", g->vendor, g->driver_version,
           g->driver_id, g->arm_rel, g->arm_p, g->arm_beta ? " beta" : "", g->api, g->dynren, g->custom, g->port, g->name);
    for (int q = 0; q < Q_N; q++) printf("QUIRK %-50s default %d, decided %d%s\n", Q_INFO[q].what, quirk_default(q, g), g_qval[q], g_qset[q] ? "" : " (not decided yet)");
    CHECK(g->vendor == DV.p.vendorID && g->driver_version == DV.p.driverVersion && !strcmp(g->name, DV.p.deviceName), "gpu_id: the device's identity");
    CHECK(g->driver_id == (DV.drv_ok ? (int)DV.drv.driverID : -1), "gpu_id: driver id %d, the device's %d", g->driver_id, DV.drv_ok ? (int)DV.drv.driverID : -1);
    CHECK(g->arm_rel == g_arm_rel && g->arm_p == g_arm_p && g->arm_beta == g_arm_beta && g->dynren == g_dynren && !g->custom, "gpu_id: complete after set-up");
    int arm = g->vendor == VEN_ARM, sec = g->vendor == VEN_SAMSUNG, img = g->vendor == VEN_IMG, qc = g->vendor == VEN_QCOM;
    CHECK(quirk_default(Q_DMAHEAP, g) == (arm || sec) && quirk_default(Q_HOST_IMPORT_OFF, g) == sec && quirk_default(Q_CLEAR_BY_PASS, g) == sec, "Mali / Xclipse memory and clear rules");
    CHECK(quirk_default(Q_PCACHE_GAPS_OFF, g) == img && quirk_default(Q_ARM_FORMATS, g) == arm && quirk_default(Q_BARRIER_RELAXED, g) == qc, "PowerVR / Arm / Adreno rules");
    CHECK(quirk_default(Q_TEX_HALF_UNSAFE, g) == (g->arm_rel && g->arm_rel < 49), "half textures unsafe before r49 (#82): %d for r%d", quirk_default(Q_TEX_HALF_UNSAFE, g), g->arm_rel);
    CHECK(quirk_default(Q_VIEW_QUARANTINE, g) == (g->arm_rel && g->dynren ? 2048 : 0), "view quarantine on Arm's driver with dynamic rendering");
    CHECK(!quirk_default(Q_G615_SAFE, g) || strstr(g->name, "G615"), "the G615 start-up set only on a G615");
}
#else
static void quirk_checks(void) { printf("QUIRKS: this engine has no vk_quirks.h (the per-GPU choices are checked one by one above)\n"); }
#endif

// ---- a little real work through the engine ----
static void work(void) {
    // samplers: anisotropy, min / max reduction, a custom border colour, depth compare; each through samp_vk's fallbacks (the validation layer judges the result)
    samp_par sp[] = {
        { .minf = 5, .magf = 1, .ws = 0, .wt = 0, .wr = 0, .lod_max = 16, .aniso = 16 },
        { .minf = 5, .magf = 1, .reduction = 1, .lod_max = 16, .aniso = 1 },
        { .minf = 5, .magf = 1, .reduction = 2, .lod_max = 16, .aniso = 1 },
        { .minf = 1, .magf = 1, .ws = 5, .wt = 5, .wr = 5, .lod_max = 1, .aniso = 1, .border = { 0.25f, 0.5f, 0.75f, 1 } },   // (NVN wrap 5: clamp to border)
        { .minf = 1, .magf = 1, .ws = 3, .wt = 2, .wr = 3, .lod_max = 1, .aniso = 1 },   // (NVN wrap 2 / 3: the mirror-clamp modes)
        { .minf = 1, .magf = 1, .cmp_mode = 1, .cmp_func = 3, .lod_max = 1, .aniso = 1 },
    };
    g_step = "samplers"; for (unsigned i = 0; i < sizeof sp / sizeof *sp; i++) { VkSampler s = samp_vk(&sp[i]); CHECK(s != VK_NULL_HANDLE, "sampler %u not made", i); }
    // a memory pool the GPU writes and the CPU reads back (the route the engine chose: host import, fd export, dma-buf heap or moved mappings)
    size_t n = 8u << 20; uint8_t *mem = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mem != MAP_FAILED, "mmap"); if (mem == MAP_FAILED) return;
    memset(mem, 0x11, n);
    g_step = "mtl_pool_add"; mtl_pool_add(mem, n);
    VkBuffer b; VkDeviceSize o; CHECK(pool_buf((uint64_t)(uintptr_t)mem + 4096, 4096, &b, &o, NULL), "the pool has no GPU buffer");
    int shifted = 0;
    // The last route, the driver's own mapping moved onto the pool (mremap), is how a phone's driver without import or export is used: its GPU reaches the memory by
    //   the GPU's own addresses. lavapipe's "GPU" is the CPU reading through the mapping the engine moved away, and it crashes in its worker thread (it does so on
    //   plain lavapipe too, DM_VK_NO_HOST_IMPORT=1 DM_VK_NO_FDMAP=1). There the submissions go without the fill: the fences / timeline are still exercised
    int moved = !g_host_import && !g_fdmap && !g_heapmap;
    if (moved) { printf("SIMGAP pool: the moved-mapping route (mremap) cannot run on lavapipe (its GPU is the CPU reading the moved mapping): empty submissions instead\n");
        for (int round = 0; round < 3; round++) { g_step = "submission (empty)"; mtl_sub_begin(); uint64_t f = mtl_sub_commit(); mtl_wait(f, UINT64_MAX); mtl_finish(); } }
    for (int round = 0; round < 3 && !moved; round++) {   // (three: the fence / timeline ring reused). Pages 1, 3, 5 filled; 2, 4, 6 must stay as they were
        uint8_t *pg = mem + 4096 * (2 * round + 1); uint32_t v = 0xC0DE0000u + (uint32_t)round;
        g_step = "submission"; mtl_sub_begin(); mtl_sub_fill((uint64_t)(uintptr_t)pg, 4096, v);
        uint64_t f = mtl_sub_commit(); mtl_wait(f, UINT64_MAX);
        mtl_finish();
        const uint32_t *w = (const uint32_t *)pg, *nx = (const uint32_t *)(pg + 4096); int bad = 0, next = 0;
        for (int k = 0; k < 1024; k++) { bad += w[k] != v; next += nx[k] == v; }
        // lavapipe's exported opaque-fd memory starts with a page of its own (llvmpipe's fd allocations keep a header there): mapped at offset 0, as the engine does,
        //   everything lands one page off. An opaque fd's layout is the driver's business (Vulkan says nothing about mmap of one), so this is lavapipe's answer, noted
        if (bad == 1024 && next == 1024 && g_fdmap && g_fdtype == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) { shifted++; continue; }
        CHECK(!bad, "round %d: the GPU's fill not seen in the pool by the CPU (%d of 1024 words; first %#x)", round, bad, w[0]);
        CHECK(!next && nx[1] == 0x11111111u, "round %d: the fill went past its range", round);
    }
    if (shifted) printf("SIMGAP pool: the opaque-fd route (lavapipe's fd mapped at offset 0) put the GPU's writes one page after the pool's (%d of 3 rounds): lavapipe's fd layout, see README\n", shifted);
    printf("WORK pool route: %s\n", g_host_import ? "host import" : g_heapmap ? "dma-buf heap" : g_fdmap ? "fd export" : "driver memory moved onto the pool");
}

// A crash prints where it was (addresses for addr2line -f -e t_vkdev; the run's log keeps them) and the step the test was in
static void on_crash(int sig) {
    void *bt[48]; int n = backtrace(bt, 48);
    char m[160]; int k = snprintf(m, sizeof m, "CRASH signal %d during %s; backtrace (addr2line -f -e t_vkdev <address - load base>):\n", sig, g_step); write(1, m, (size_t)k);
    backtrace_symbols_fd(bt, n, 1);
    signal(sig, SIG_DFL); raise(sig);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGSEGV, on_crash); signal(SIGBUS, on_crash); signal(SIGABRT, on_crash);
    unsetenv("DM_VK_PCACHE");   // (no marker files from this test)
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL); if (!lib) { printf("FAIL no libvulkan.so.1\n"); return 1; }
    real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkGetInstanceProcAddr = w_gipa; VK_GLOBAL(VK_LOAD_G)   // (what mtl_init does when it opens the loader itself, through the watching wrapper)
    g_step = "mtl_init"; mtl_init();
    device_read();
    const char *profile = getenv("SIMDEV_PROFILE");
    decisions_and_checks(profile);
    quirk_checks();
    g_step = "work"; if (!getenv("SIMDEV_NO_WORK")) work();
    printf("%s t_vkdev %s: %d of %d checks failed\n", fails ? "FAIL" : "PASS", profile && *profile ? profile : "(no profile)", fails, checks);
    fflush(stdout);
    _exit(fails ? 1 : 0);   // (the engine's threads keep running; the device is not torn down)
}

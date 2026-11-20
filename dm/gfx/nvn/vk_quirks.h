// The unified driver layer's one table: what this engine does differently for each GPU and driver, decided in one place from one description of the GPU (gpu_id),
// instead of a vendor test at every site. The translation itself (NVN → Vulkan, Maxwell → SPIR-V, BC → ETC2 / ASTC) is the same for every GPU; these are the
// workarounds on top of it, each one found on real phones (the issue numbers are in the comments at each site in nvn_vk.c).
//   quirk_default(q, g): the default for this GPU, a pure function (dm/gfx/tests/t_quirks.c pins it for the GPUs players reported). env.txt still wins at each
//   site, as before (the DM_VK_* name in Q_INFO). quirk_note(): what was decided and why, printed together at start ("[quirk]" lines; DebugBundle copies them)
//   so a log says at once which workarounds a phone runs with.
// DM_VK_BASELINE=1: the Vulkan 1.1 floor on any GPU (render passes instead of dynamic rendering, draw state in each pipeline, a fence per submission instead of a
//   timeline semaphore): what a phone with an old driver runs, reproducible on a phone with a new one.
#ifndef VK_QUIRKS_H
#define VK_QUIRKS_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
enum { VEN_QCOM = 0x5143, VEN_ARM = 0x13B5, VEN_SAMSUNG = 0x144D, VEN_IMG = 0x1010 };
// The port: the same table in Galar Native and Arceus Native; where the two ports were tuned apart on players' phones, the rule says which port it is for
enum { PORT_GALAR = 0, PORT_ARCEUS = 1 };
typedef struct {
    int port;               // PORT_* (nvn_vk.c: from DM_GAME_PROGRAM_ID, the build's game)
    uint32_t api;           // the device's Vulkan version (VkPhysicalDeviceProperties.apiVersion)
    uint32_t vendor, driver_version;
    int driver_id;          // VkDriverId; -1 when the driver gives no driver properties (Vulkan 1.1 without the extension)
    int arm_rel, arm_p, arm_beta;   // Arm's own Mali driver: r<rel>p<p> (0: another driver, PanVK included)
    int custom;             // a driver package (DM_VK_DRIVER: Turnip, PanVK...), opened past Android's loader
    int dynren;             // dynamic rendering in use (decided by Q_NO_DYNREN and the driver; read by Q_VIEW_QUARANTINE)
    long ram_kb;            // the phone's memory (/proc/meminfo), 0 unknown
    char name[256];         // VkPhysicalDeviceProperties.deviceName
} gpu_id;
enum {
    Q_NO_DYNREN,         // render passes instead of dynamic rendering: Adreno's vkCmdBeginRendering allocates every time (a third of the replay's CPU on the S25+)
    Q_HOST_IMPORT_OFF,   // the game's pools not imported as host memory: Xclipse refuses submissions with them (#39)
    Q_DMAHEAP,           // pools backed by dma-buf heap memory the driver imports: Mali and Xclipse (#41 #45)
    Q_DMABUF_TRY,        // the dma-buf route tried even when the driver lists no exportable buffer: Mali
    Q_G615_SAFE,         // Mali-G615 on Arm's driver before r49: uniform snapshots, BC decoded not transcoded (#63 #65 #82 #105 #110 #112)
    Q_FLOAT_CONTROLS,    // shaders keep infinities and NaN: Arm's and Samsung's own drivers (#47 #53)
    Q_CLAMP_ZERO,        // Arm erratum 3779215: r38p1-r49p0, beta r49p1, r50p0-r51p0
    Q_NAN_MINMAX,        // min / max / saturate drop NaN as Maxwell does: Arm's own driver (2: with the isnan rule written out)
    Q_EXACT_MATH,        // log2 / exp2 / rsqrt / sqrt / f2i edge cases as Maxwell: Arm's own driver
    Q_INIT_LOCALS,       // translator registers start at zero: Arm's own driver
    Q_VIEW_QUARANTINE,   // destroyed views and images kept back (2048): Arm's driver with dynamic rendering reuses a handle's stale state
    Q_TEX_HALF_UNSAFE,   // the profile's half textures not used: Arm's driver before r49 closed the game with them (#82)
    Q_ARM_FORMATS,       // view format lists so Mali keeps AFBC
    Q_BARRIER_RELAXED,   // fewer full barriers before passes: Adreno
    Q_GATHER_BIAS,       // gathers nudged 1/512 texel: Adreno rounds texel centres the other way
    Q_PASS_DEFER,        // target changes that need no new pass keep it open: Adreno
    Q_ASYNC_PSO,         // pipelines built on worker threads: Adreno
    Q_PIPE_KEEP,         // at most 800 pipelines kept (least recently used destroyed): Mali, or a phone under 7.5 GB
    Q_CLEAR_BY_PASS,     // clears in a layered pass over a 3D target as passes of their own: Xclipse
    Q_PCACHE_GAPS_OFF,   // a pipeline with a colour gap made without the pipeline cache: PowerVR's driver crashes on it
    Q_NO_EDS,            // draw state built into each pipeline: Qualcomm's Vulkan 1.1 drivers misread per-draw vertex strides (Arceus #34; Galar #209 reported the same)
    Q_BC_GPU_OFF,        // BC textures converted on the CPU, not the GPU: PowerVR's compute results differ, objects purple (Arceus #32)
    Q_N
};
static const struct { const char *env, *what; } Q_INFO[Q_N] = {
    [Q_NO_DYNREN] = { "DM_VK_NO_DYNREN", "render passes, not dynamic rendering" },
    [Q_HOST_IMPORT_OFF] = { "DM_VK_HOST_IMPORT", "pools not imported as host memory" },
    [Q_DMAHEAP] = { "DM_VK_DMAHEAP", "pools in dma-buf heap memory" },
    [Q_DMABUF_TRY] = { "DM_VK_DMAHEAP", "dma-buf route tried without a listed export" },
    [Q_G615_SAFE] = { "DM_VK_UBO_SNAPSHOT", "Mali-G615 start-up set (uniform snapshots, BC decoded)" },
    [Q_FLOAT_CONTROLS] = { "DM_VK_FLOAT_CONTROLS", "shaders keep infinities and NaN" },
    [Q_CLAMP_ZERO] = { "DM_VK_CLAMP_ZERO", "clamp-to-zero erratum workaround" },
    [Q_NAN_MINMAX] = { "DM_VK_NAN_MINMAX", "min / max drop NaN as on the Switch" },
    [Q_EXACT_MATH] = { "DM_VK_EXACT_MATH", "Maxwell's special-function edge cases" },
    [Q_INIT_LOCALS] = { "DM_VK_INIT_LOCALS", "shader registers start at zero" },
    [Q_VIEW_QUARANTINE] = { "DM_VK_VIEW_QUARANTINE", "destroyed views kept back" },
    [Q_TEX_HALF_UNSAFE] = { "DM_VK_TEX_QUALITY", "the profile's half textures not used" },
    [Q_ARM_FORMATS] = { "DM_VK_ARM_FORMATS", "view format lists for AFBC" },
    [Q_BARRIER_RELAXED] = { "DM_VK_BARRIER", "fewer full barriers before passes" },
    [Q_GATHER_BIAS] = { "DM_VK_GATHER_BIAS", "texture gathers nudged" },
    [Q_PASS_DEFER] = { "DM_VK_PASS_DEFER", "passes kept open across target changes" },
    [Q_ASYNC_PSO] = { "DM_VK_ASYNC_PSO", "pipelines built on worker threads" },
    [Q_PIPE_KEEP] = { "DM_VK_PIPE_KEEP", "pipelines kept at most (count)" },
    [Q_CLEAR_BY_PASS] = { "DM_VK_CLEAR_BY_PASS", "3D-target clears as passes" },
    [Q_PCACHE_GAPS_OFF] = { "", "colour-gap pipelines without the pipeline cache" },
    [Q_NO_EDS] = { "DM_VK_NO_EDS", "draw state built into each pipeline" },
    [Q_BC_GPU_OFF] = { "DM_VK_BC_GPU", "BC textures converted on the CPU" },
};
// Arm's own driver: by driver ID where the driver gives one, else by vendor (PanVK reports Arm's vendor ID with its own driver ID)
static int gpu_is_arm_driver(const gpu_id *g) { return g->driver_id >= 0 ? g->driver_id == 9 /* VK_DRIVER_ID_ARM_PROPRIETARY */ : g->vendor == VEN_ARM; }
static int gpu_is_samsung_driver(const gpu_id *g) { return g->driver_id >= 0 ? g->driver_id == 21 /* VK_DRIVER_ID_SAMSUNG_PROPRIETARY */ : g->vendor == VEN_SAMSUNG; }
static int quirk_default(int q, const gpu_id *g) {
    int v = g->arm_rel * 100 + g->arm_p;
    int arc = g->port == PORT_ARCEUS;
    switch (q) {
    // the two ports apart: Galar's Adreno render passes (#205: 27.5 → 17.3 ms on the S25+) and worker-built pipelines are not in Arceus, where async pipelines left
    //   objects missing for frames on an Adreno 710 (Arceus #34); Arceus's Qualcomm 1.1 stride fix and PowerVR CPU BC conversion are not in Galar yet
    case Q_NO_DYNREN: case Q_ASYNC_PSO: return g->vendor == VEN_QCOM && !arc;
    case Q_NO_EDS: return arc && g->vendor == VEN_QCOM && (g->api >> 22 & 0x7f) == 1 && (g->api >> 12 & 0x3ff) < 2;   // (VK_API_VERSION_MAJOR / MINOR)
    case Q_BC_GPU_OFF: return arc && g->vendor == VEN_IMG;
    case Q_BARRIER_RELAXED: case Q_GATHER_BIAS: case Q_PASS_DEFER: return g->vendor == VEN_QCOM;
    case Q_HOST_IMPORT_OFF: case Q_CLEAR_BY_PASS: return g->vendor == VEN_SAMSUNG;
    case Q_DMAHEAP: return g->vendor == VEN_ARM || g->vendor == VEN_SAMSUNG;
    case Q_DMABUF_TRY: case Q_ARM_FORMATS: return g->vendor == VEN_ARM;
    case Q_G615_SAFE: return g->vendor == VEN_ARM && (g->driver_version >> 22) < 49 && strstr(g->name, "G615") != NULL && !g->custom;
    case Q_FLOAT_CONTROLS: return gpu_is_arm_driver(g) || gpu_is_samsung_driver(g);
    case Q_CLAMP_ZERO: return g->arm_rel && ((v >= 3801 && v < 4901) || (g->arm_beta && v >= 4901 && v < 4902) || (v >= 5000 && v < 5101));
    case Q_NAN_MINMAX: return g->arm_rel ? 2 : 0;
    case Q_EXACT_MATH: case Q_INIT_LOCALS: return g->arm_rel != 0;
    case Q_VIEW_QUARANTINE: return g->arm_rel && g->dynren ? 2048 : 0;
    case Q_TEX_HALF_UNSAFE: return g->arm_rel && g->arm_rel < 49;
    case Q_PIPE_KEEP: return g->vendor == VEN_ARM || (g->ram_kb > 0 && g->ram_kb < 7500000) ? 800 : 0;
    case Q_PCACHE_GAPS_OFF: return g->vendor == VEN_IMG;
    }
    return 0;
}
// What was decided, for the start's summary: value and whether env.txt set it. A site calls this once, when it decides (some decide lazily, at first use)
static int g_qval[Q_N], g_qenv[Q_N], g_qset[Q_N];
static int quirk_note(int q, int value, int from_env) {
    if (q >= 0 && q < Q_N && !g_qset[q]) { g_qset[q] = 1; g_qval[q] = value; g_qenv[q] = from_env;
        if (value || from_env) fprintf(stderr, "[dm] [quirk] %s: %d (%s%s)\n", Q_INFO[q].what, value, from_env ? "env.txt " : "default for this GPU", from_env ? Q_INFO[q].env : ""); }
    return value;
}
static void gpu_id_log(const gpu_id *g) {
    fprintf(stderr, "[dm] [quirk] %s: GPU %s, Vulkan %u.%u, vendor %#x, driver id %d, driver version %#x, %s, memory %ld MB\n", g->port == PORT_ARCEUS ? "Arceus Native" : "Galar Native",
            g->name, g->api >> 22 & 0x7f, g->api >> 12 & 0x3ff, g->vendor, g->driver_id, g->driver_version, g->custom ? "a driver package" : "the phone's own driver", g->ram_kb / 1024);
    if (g->arm_rel) fprintf(stderr, "[dm] [quirk] Arm driver r%dp%d%s\n", g->arm_rel, g->arm_p, g->arm_beta ? " (beta)" : "");
}
#endif

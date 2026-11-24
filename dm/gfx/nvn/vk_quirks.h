// The unified driver layer's one table: what this engine does differently for each GPU and driver, decided in one place from one description of the GPU (gpu_id),
// instead of a vendor test at every site. The translation itself (NVN → Vulkan, Maxwell → SPIR-V, BC → ETC2 / ASTC) is the same for every GPU; these are the
// workarounds on top of it, each one found on real phones (the issue numbers are in the comments at each site in nvn_vk.c).
//   quirk_default(q, g): the default for this GPU, a pure function. dm/gfx/tests/t_quirks.c checks it, for each of the three apps, against the per-site rules it
//   replaced as beta-exit 6e0e1dd had them (the reviewed behaviour; docs/ENGINE_UNIFY.md), Q_ASYNC_PSO as beta-exit has it since f09e58f (background pipelines
//   on every GPU; the head merged here: 3d21471), Q_B10_STANDIN / Q_MALI_WHITE_CHECK as mali-all e6f6d0c decided them (mali_white.h mw_default_route / mw_enabled:
//   Sword / Shield's white-screen fix on Arm's own driver), and pins it for the GPUs players reported. env.txt still wins at each
//   site, as before (the DM_VK_* name in Q_INFO). quirk_note(): what was decided and why, printed together at start ("[quirk]" lines; DebugBundle copies them) so a
//   log says at once which workarounds a phone runs with.
// One table for every game (Galar Native: Sword / Shield, Arceus Native, Paldea Native: Scarlet / Violet; docs/ENGINE_UNIFY.md): no rule tests which game it is.
//   gpu_id.port (the profile's key port) only names the app in the log. Where the games were tuned apart on players' phones, the rule reads that game's own choice
//   from gpu_id.eng: the profile's engine_quirks (game.tsv → game_profile.h DM_GAME_ENG_* → one bit each, ENG_* below), so the choice lives in the game's profile,
//   not in this file, and a game takes another's rule by naming the quirk in its game.tsv. engine_quirks that decide nothing per GPU stay at their sites as
//   #if DM_GAME_ENG_*: frame_hold_off (nvn_vk.c hold_on), gpu_time_auto_only (GPU timestamps only with Graphics Auto: a setting, not a GPU), nvn_async_never's
//   refusal of the player's own Faster rendering setting (hle_nvn.c rq_on; its Auto is Q_NVN_ASYNC here); framegen_game_rate has had no reader since fg-plan
//   (every game's frame generation floor is its own rate - 2).
// DM_VK_BASELINE=1: the Vulkan 1.1 floor on any GPU (render passes instead of dynamic rendering, draw state in each pipeline, a fence per submission instead of a
//   timeline semaphore): what a phone with an old driver runs, reproducible on a phone with a new one.
// Deliberate raw checks (GPU tests that stay where they are, on purpose):
//   - building gpu_id itself (nvn_vk.c mtl_init): Arm's release from driverVersion and driverInfo's "bet", Arm by vendor ID when the driver gives no driver
//     properties, the driver's name in the log;
//   - what is decided before any driver is open: android_main.c device_profile (Graphics level, filtering, texture size by chip model and memory: a performance
//     tier, not a driver workaround), panvk_defaults and the driver check's copy of it (probe/jni/dprobe_jni.c: PanVK's own chip table, for that driver package),
//     the safe-start ladder (safe_start.h: this phone's own failed starts, any GPU), the heat guard's kgsl clock limit (a sysfs file Adreno phones have), and
//     mali_env_migrate (env.txt's float-target "1"s an older Mali tests menu left, taken out on phones whose gpu.txt says Mali: a file only that menu wrote);
//   - the driver check's self-tests (probe/jni/selftest.c) run on every GPU; what the engine asks of a driver by default comes from this table there too
//     (Q_FLOAT_CONTROLS: whether float-controls asks for them; Q_CLEAR_BY_PASS: how 3d-layered records its clear inside the layered pass, as clear_in_pass does;
//     Q_NO_DYNREN: whether 3d-layered records its passes with dynamic rendering or render passes, as the engine does there);
//   - the Java app: which test sections the menu shows (DriverPackages.gpu: Mali tests, Adreno tests), which driver packages fit the GPU (Turnip on Adreno only,
//     the white-screen ladder's PanVK offer: MaliLadder.java), and texts that describe these rules;
//   - the white-screen ladder's own state (mali_white_vk.inc: mali_ladder.txt is kept per driver ID and driver version, so another driver starts it over): whether
//     the ladder and its route run by default is this table's (Q_MALI_WHITE_CHECK, Q_B10_STANDIN);
//   - test switches that make a GPU act as another (DM_VK_AS_MALI) or pick a device by name (DM_VK_DEVICE), log and capture texts;
//   - the frame hold's default (nvn_vk.c hold_on) is Q_FRAME_HOLD's, applied only where the replay has its own thread (Q_NVN_ASYNC, or the player's setting);
//   - what follows from how an image was made rather than from a GPU test: tex_view_for's view of another numeric class for an integer texture is made only
//     where the image allows it (tex_new: not mutable on Mali by Q_ARM_FORMATS, or a format list where the driver has them), else its own format as before.
// Values the driver check (DriverCheck.java, libdmprobe) found for this driver come in as environment settings: driver-check.env, read after env.txt and the
//   safe-start ladder and before the phone profile (android_main.c host_start), so the player's own settings and the ladder's levels win over it (a ladder level's
//   DM_VK_FLOAT_CONTROLS=0 over the check's 1). android_main.c names the keys it took from that file in DM_DRIVER_CHECK_KEYS, and a site takes such a value as this
//   driver's default, not as the player's choice: vkq_env_player() is NULL for it, so a crash marker that only the player's own setting skips (float_controls_crashed,
//   bc_gpu_crashed, heap_import_crashed) still applies; its [quirk] line says "driver check" (vkq_env_src() 2). The keys the check writes: DM_VK_FLOAT_CONTROLS,
//   DM_VK_DMAHEAP, DM_VK_BC_GPU, DM_VK_CLEAR_BY_PASS (table rules) and DM_VK_ZCOPY, DM_VK_INT_TEX, DM_VK_NO_LAYERED_3D (switches of their own code paths); the debug
//   report holds the file.
// gpu_id is filled in two steps (nvn_vk.c mtl_init): the name, version, vendor, memory and the game's profile before the device is made, the driver ID and Arm's
//   release after (quirk_late: the rules that read those). nvn_vk_quirk_default (hle_nvn.c) answers -1 for a late rule until both are in.
#ifndef VK_QUIRKS_H
#define VK_QUIRKS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define VKQ_UNUSED __attribute__((unused))   // (included where only part of it is used: hle_nvn.c, the probe)
enum { VEN_QCOM = 0x5143, VEN_ARM = 0x13B5, VEN_SAMSUNG = 0x144D, VEN_IMG = 0x1010 };
// The port: the released app this build is, from the game profile's key port (game_profile.h DM_PORT_*: Sword and Scarlet take Shield's and Violet's through
//   tables_from). Only the log names it: no rule tests it
enum { PORT_GALAR = 0, PORT_ARCEUS = 1, PORT_PALDEA = 2 };
// The game's own per-GPU choices: the profile's engine_quirks that decide a GPU default (game.tsv → game_profile.h DM_GAME_ENG_*; nvn_vk.c mtl_init maps each one by
//   name to its bit, so the order of game.py's list cannot move a bit). Today Legends: Arceus sets the first four, Sword / Shield the fifth, Scarlet / Violet none:
//   qc11_no_eds → Q_NO_EDS, async_pso_off → Q_ASYNC_PSO, driver_selftests → Q_DRIVER_SELFTESTS, nvn_async_never → Q_NVN_ASYNC,
//   mali_lit_standin → Q_B10_STANDIN and Q_MALI_WHITE_CHECK
enum { ENG_QC11_NO_EDS = 1u << 0, ENG_ASYNC_PSO_OFF = 1u << 1, ENG_DRIVER_SELFTESTS = 1u << 2, ENG_NVN_ASYNC_NEVER = 1u << 3, ENG_MALI_LIT_STANDIN = 1u << 4 };
typedef struct {
    int port;               // PORT_* (nvn_vk.c: from the game profile, DM_PORT_*)
    unsigned eng;           // ENG_* (nvn_vk.c: from the game profile, DM_GAME_ENG_*)
    uint32_t api;           // the device's Vulkan version (VkPhysicalDeviceProperties.apiVersion)
    uint32_t vendor, driver_version;
    int driver_id;          // VkDriverId where mtl_init reads the driver properties (Vulkan 1.2, or 1.1 with the float-controls and driver-properties extensions);
                            //   -1 elsewhere (Arm's own driver is then told by its vendor ID)
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
    Q_BARRIER_RELAXED,   // fewer full barriers before passes: Adreno, but not Qualcomm's own drivers before Vulkan 1.3 (#209, Arceus #34 #46 #59)
    Q_GATHER_BIAS,       // gathers nudged 1/512 texel: Adreno rounds texel centres the other way
    Q_PASS_DEFER,        // target changes that need no new pass keep it open: Adreno
    Q_ASYNC_PSO,         // pipelines built on worker threads: every GPU (beta-exit f09e58f, 2026-11-22; Adreno only before), unless the game's profile says
                         //   async_pso_off (Arceus #34)
    Q_PIPE_KEEP,         // at most 800 pipelines kept (least recently used destroyed): Mali, Xclipse, or a phone under 9 GB (an "8 GB" Galaxy A55 / A56 reports ~7.6 M kB,
                         //   which the old 7.5 GB line missed: 428-1,450 MB swapped, #97 #179 #324, Arceus #78 #92)
    Q_CLEAR_BY_PASS,     // clears in a layered pass over a 3D target as passes of their own: Xclipse
    Q_PCACHE_GAPS_OFF,   // a pipeline with a colour gap made without the pipeline cache: PowerVR's driver crashes on it
    Q_NO_EDS,            // draw state built into each pipeline: Qualcomm's Vulkan 1.1 drivers misread per-draw vertex strides, for a game whose profile says qc11_no_eds
                         //   (Arceus #34; Galar #209 reported the same)
    Q_BC_GPU_OFF,        // BC textures converted on the CPU, not the GPU: PowerVR's compute results differ, objects purple (Arceus #32; every game)
    Q_QUERY_LOD,         // textureQueryLod's level as clamp (LOD, 0, levels - 1): Arm's own driver gives floor (LOD) (dEQP texturequerylod, Mali-G615 MC6). New with the
                         //   driver probes (galar-driver-probes / arceus-vk10-validation), every game: the one rule here that 6e0e1dd did not have
    // the per-GPU tests 6e0e1dd still made at their sites (nvn_vk.c, hle_nvn.c, tf_selftest.h), moved here with the same rules
    Q_IMPORT_MARK_OFF,   // a submission that failed with imported pools does not turn host import off at the next start: Adreno (its fallback, moving the driver's
                         //   mappings, is the route that crashed it on area changes)
    Q_BC_DECODE,         // BC textures decoded to RGBA, not transcoded to ETC2 / ASTC: PowerVR (Tensor G5, Arceus #32: static meshes flat lavender transcoded)
    Q_GPU_TIME_OFF,      // no GPU timestamps by default: Mali and PowerVR, whose timestamps around tiler work are least trustworthy
    Q_TF_CHECK,          // the first transform feedback capture checked once (a marker read back 60 frames later): Arm's own driver (#6)
    Q_DRIVER_SELFTESTS,  // the start-up driver self-tests (vtx_canary.h vertex strides, tf_selftest.h transform feedback): a game whose profile says driver_selftests
    Q_TF_SELFTEST,       // within those: the transform feedback self-test runs on Arm's own driver
    Q_NVN_ASYNC,         // the replay on its own thread (Faster rendering's Auto, hle_nvn.c): Adreno by name (Turnip too), Arm's own driver, PowerVR and Xclipse, unless
                         //   the game's profile says nvn_async_never (Arceus #3: crashed at loading screens with it)
    // Mali's white lit scene (#47 #110 #111 #172), a game whose profile says mali_lit_standin (Sword / Shield) on Arm's own driver (mali-all e6f6d0c)
    Q_B10_STANDIN,       // route A: the lit R11G11B10F target drawn and sampled as an RGBA16F stand-in, converted exactly where it meets the game's memory (b10_on)
    Q_MALI_WHITE_CHECK,  // the white-screen check while playing and its ladder (mali_white_vk.inc: route A, then B, then the PanVK offer, start by start)
    Q_FRAME_HOLD,        // the whole frame held until its present, stores decided from it (nvn_vk.c hold_on, mode 2), where the replay has its own thread: Adreno by name
                         //   and Arm's own driver, where it has been run (was: wherever Q_NVN_ASYNC was on)
    Q_RENDER_AREA_FULL,  // a held frame's passes keep the whole target as their render area, not the part their draws reach (seg_area): no GPU listed yet; the row
                         //   for a driver that misdraws with a smaller area
    Q_N
};
// (env: the switches that decide the rule at its site, named in its [quirk] line when one of them is set)
static const struct { const char *env, *what; } Q_INFO[Q_N] VKQ_UNUSED = {
    [Q_NO_DYNREN] = { "DM_VK_NO_DYNREN", "render passes, not dynamic rendering" },
    [Q_HOST_IMPORT_OFF] = { "DM_VK_HOST_IMPORT, DM_VK_NO_HOST_IMPORT", "pools not imported as host memory" },
    [Q_DMAHEAP] = { "DM_VK_DMAHEAP", "pools in dma-buf heap memory" },
    [Q_DMABUF_TRY] = { "DM_VK_NO_DMABUF", "dma-buf route tried without a listed export" },
    [Q_G615_SAFE] = { "DM_VK_UBO_SNAPSHOT, DM_VK_BC_TRANSCODE", "Mali-G615 start-up set (uniform snapshots, BC decoded)" },
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
    [Q_QUERY_LOD] = { "DM_VK_QUERY_LOD", "texture LOD queries give the level clamped" },
    [Q_IMPORT_MARK_OFF] = { "DM_VK_HOST_IMPORT, DM_VK_NO_HOST_IMPORT", "a failed submission does not turn host import off" },
    [Q_BC_DECODE] = { "DM_VK_BC_TRANSCODE", "BC textures decoded, not transcoded" },
    [Q_GPU_TIME_OFF] = { "DM_VK_GPU_TIME", "no GPU timestamps" },
    [Q_TF_CHECK] = { "DM_VK_TF_CHECK", "transform feedback captures checked" },
    [Q_DRIVER_SELFTESTS] = { "", "driver self-tests at start" },
    [Q_TF_SELFTEST] = { "DM_VK_TF_SELFTEST", "transform feedback self-test" },
    [Q_NVN_ASYNC] = { "DM_NVN_ASYNC", "the replay on its own thread" },
    [Q_B10_STANDIN] = { "DM_VK_B10_AS_RGBA16, DM_MALI_LADDER, DM_MALI_WHITE_SIM", "the lit R11G11B10F target drawn as RGBA16F" },
    [Q_MALI_WHITE_CHECK] = { "DM_MALI_LADDER, DM_MALI_WHITE_SIM", "the white-screen check and its ladder" },
    [Q_FRAME_HOLD] = { "DM_VK_FRAME_HOLD", "frames held whole, stores decided at the present (mode)" },
    [Q_RENDER_AREA_FULL] = { "DM_VK_RENDER_AREA", "passes keep the whole target as their render area" },
};
// Arm's own driver: by driver ID where the driver gives one, else by vendor (PanVK reports Arm's vendor ID with its own driver ID)
VKQ_UNUSED static int gpu_is_arm_driver(const gpu_id *g) { return g->driver_id >= 0 ? g->driver_id == 9 /* VK_DRIVER_ID_ARM_PROPRIETARY */ : g->vendor == VEN_ARM; }
VKQ_UNUSED static int gpu_is_samsung_driver(const gpu_id *g) { return g->driver_id >= 0 ? g->driver_id == 21 /* VK_DRIVER_ID_SAMSUNG_PROPRIETARY */ : g->vendor == VEN_SAMSUNG; }
// Qualcomm's own driver before Vulkan 1.3 (Adreno 6xx / 7xx phones on driver 1.1.128 and the like): by the API's minor version and a name that starts "Adreno", so
//   Turnip, which names itself "Turnip Adreno", is not one (was nvn_vk.c old_qcom_driver)
VKQ_UNUSED static int gpu_is_old_qcom(const gpu_id *g) { return g->vendor == VEN_QCOM && (g->api >> 12 & 0x3ff) < 3 && !strncmp(g->name, "Adreno", 6); }   // (VK_API_VERSION_MINOR)
VKQ_UNUSED static int quirk_default(int q, const gpu_id *g) {
    int v = g->arm_rel * 100 + g->arm_p;
    switch (q) {
    // Adreno, every game: render passes (#205: 27.5 → 17.3 ms on the S25+; Legends: Arceus since its v0.45), gathers nudged, passes kept open, a failed submission's
    //   marker not read; barriers relaxed but on Qualcomm's own drivers before Vulkan 1.3, where skinned models exploded with them (Galar #209, Arceus #34 #46 #59).
    //   Mali, Xclipse and PowerVR keep full barriers for every game (Legends: Arceus flickered with them relaxed: #20 #31 #32)
    case Q_NO_DYNREN: case Q_GATHER_BIAS: case Q_PASS_DEFER: case Q_IMPORT_MARK_OFF: return g->vendor == VEN_QCOM;
    case Q_BARRIER_RELAXED: return g->vendor == VEN_QCOM && !gpu_is_old_qcom(g);
    // the replay's own thread: by the GPU's name, as Faster rendering's Auto always tested it (Turnip names itself "Turnip Adreno"); and Arm's own Mali driver since
    //   v0.97: the POCO X6 Pro (G615 MC6, r44p1) spent 22-42 ms a frame recording on the game's thread with every pass storing its attachments (the held frame's store
    //   decisions came with it then: Q_FRAME_HOLD now). PowerVR and Xclipse since perf-1124 fix 3, once the queue carried registrations and remaps in order
    //   (nvn_rq.h): #324 (Xclipse 550, Shield) spent ~45 ms a frame as ~12 game + ~33 replay on one thread, #206 #300 #270 #248 likewise; the frame becomes the
    //   longer of the two. Their frames are not held (Q_FRAME_HOLD: not run there). Not PanVK (untested there; after the #158 stadium is rerun clean)
    case Q_NVN_ASYNC: return (strstr(g->name, "Adreno") != NULL || gpu_is_arm_driver(g) || g->vendor == VEN_IMG || g->vendor == VEN_SAMSUNG) && !(g->eng & ENG_NVN_ASYNC_NEVER);
    // the held frame (mode 2), decided per GPU instead of following the replay thread to every GPU it reaches: where both have been run together (the S25+, the
    //   owner's choice 2026-11-19; the POCO X6 Pro since v0.97). nvn_vk.c applies it only where the replay has its own thread, as before
    case Q_FRAME_HOLD: return strstr(g->name, "Adreno") != NULL || gpu_is_arm_driver(g) ? 2 : 0;
    // the games apart, each by its own profile (engine_quirks → gpu_id.eng): worker-built pipelines left objects missing for frames on an Adreno 710 (Arceus #34:
    //   async_pso_off); Qualcomm's Vulkan 1.1 drivers misread per-draw vertex strides (Arceus #34: qc11_no_eds; Sword / Shield answered #209 with full barriers
    //   instead); the driver self-tests at start (driver_selftests); the replay never on its own thread (nvn_async_never, above).
    //   Pipelines the lists lacked are built in the background on every GPU, not only on Adreno (beta-exit f09e58f, the owner's call 2026-11-22: a hitch waiting for a
    //   pipeline was the stutter left on Mali / Xclipse / PowerVR): no vendor test, only the game's own async_pso_off
    case Q_ASYNC_PSO: return !(g->eng & ENG_ASYNC_PSO_OFF);
    //   Arm's own Mali driver before r44 (every game) ignores the per-draw stride and fetches with the pipeline's (0): every vertex reads vertex 0 and nothing
    //   is drawn (seen on a Mali-G68 r38p1 by another Vulkan layer; our testers' r44p1 is fine, DM_VK_NO_EDS=1 changed nothing there). Not PanVK (custom, Mesa's version)
    case Q_NO_EDS: return (g->vendor == VEN_ARM && !g->custom && (g->driver_version >> 22) > 0 && (g->driver_version >> 22) < 44) || ((g->eng & ENG_QC11_NO_EDS) && g->vendor == VEN_QCOM && (g->api >> 22 & 0x7f) == 1 && (g->api >> 12 & 0x3ff) < 2);   // (VK_API_VERSION_MAJOR / MINOR)
    case Q_DRIVER_SELFTESTS: return (g->eng & ENG_DRIVER_SELFTESTS) != 0;
    // Mali's white lit scene on Arm's own driver (#47 #110 #111 #172), for a game whose profile says mali_lit_standin (Sword / Shield): route A (the lit target as
    //   an RGBA16F stand-in) and the white-screen check that moves on to route B and the PanVK offer when it stays white. Arm's own driver only (by its release:
    //   not PanVK, which reports Arm's vendor ID and draws the scene right); other games draw their own G-buffer and bloom in R11G11B10F (Legends: Arceus) and
    //   were never white this way.
    //   OFF by default for v0.96 (2026-11-22): on a Mali-G615 MC6 (r44p1) route A and route B both stayed white; the cause is small varyings arriving at
    //   world-position size, which neither route changes. The menu's "White screen tests (Mali)" still turn them on; MALI_LIT_DEFAULT 1 restores the old rule
#ifndef MALI_LIT_DEFAULT
#define MALI_LIT_DEFAULT 0
#endif
    case Q_B10_STANDIN: case Q_MALI_WHITE_CHECK: return MALI_LIT_DEFAULT && (g->eng & ENG_MALI_LIT_STANDIN) && g->arm_rel != 0;
    // PowerVR, every game: BC textures converted on the CPU, not by the BC layer, and decoded, not transcoded (Tensor G5, Arceus #32: objects purple, meshes lavender)
    case Q_BC_GPU_OFF: case Q_BC_DECODE: case Q_PCACHE_GAPS_OFF: return g->vendor == VEN_IMG;
    case Q_GPU_TIME_OFF: return g->vendor == VEN_ARM || g->vendor == VEN_IMG;
    case Q_HOST_IMPORT_OFF: case Q_CLEAR_BY_PASS: return g->vendor == VEN_SAMSUNG;
    case Q_DMAHEAP: return g->vendor == VEN_ARM || g->vendor == VEN_SAMSUNG;
    case Q_DMABUF_TRY: case Q_ARM_FORMATS: return g->vendor == VEN_ARM;
    case Q_G615_SAFE: return g->vendor == VEN_ARM && (g->driver_version >> 22) < 49 && strstr(g->name, "G615") != NULL && !g->custom;
    case Q_FLOAT_CONTROLS: return gpu_is_arm_driver(g) || gpu_is_samsung_driver(g);
    case Q_CLAMP_ZERO: return g->arm_rel && ((v >= 3801 && v < 4901) || (g->arm_beta && v >= 4901 && v < 4902) || (v >= 5000 && v < 5101));
    case Q_NAN_MINMAX: return g->arm_rel ? 2 : 0;
    case Q_EXACT_MATH: case Q_INIT_LOCALS: case Q_QUERY_LOD: case Q_TF_CHECK: case Q_TF_SELFTEST: return g->arm_rel != 0;
    case Q_VIEW_QUARANTINE: return g->arm_rel && g->dynren ? 2048 : 0;
    case Q_TEX_HALF_UNSAFE: return g->arm_rel && g->arm_rel < 49;
    case Q_PIPE_KEEP: return g->vendor == VEN_ARM || g->vendor == VEN_SAMSUNG || (g->ram_kb > 0 && g->ram_kb < 9000000) ? 800 : 0;   // (2026-11-24: Xclipse and 8 GB phones too, now that the cap evicts gradually, pso_lru.h)
    }
    return 0;
}
// The rules that read what mtl_init learns only after the device is made (gpu_id.driver_id, arm_rel / arm_p / arm_beta): before that, their answer would be another
//   GPU's (t_quirks.c checks that every other rule reads neither)
VKQ_UNUSED static int quirk_late(int q) {
    switch (q) {
    case Q_FLOAT_CONTROLS: case Q_CLAMP_ZERO: case Q_NAN_MINMAX: case Q_EXACT_MATH: case Q_INIT_LOCALS: case Q_QUERY_LOD: case Q_TF_CHECK: case Q_TF_SELFTEST:
    case Q_VIEW_QUARANTINE: case Q_TEX_HALF_UNSAFE: case Q_B10_STANDIN: case Q_MALI_WHITE_CHECK: case Q_NVN_ASYNC: case Q_FRAME_HOLD: return 1;
    }
    return 0;
}
// Where a setting came from (the header above): 0 unset, 1 the player (env.txt, or anything else that set it before the game started), 2 the driver check
//   (android_main.c: the keys it took from driver-check.env, in DM_DRIVER_CHECK_KEYS, separated by commas)
VKQ_UNUSED static int vkq_env_src(const char *name) {
    if (!name || !*name || !getenv(name)) return 0;
    size_t n = strlen(name);
    for (const char *p = getenv("DM_DRIVER_CHECK_KEYS"); p && *p; ) {
        const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return 2;
        p = e ? e + 1 : "";
    }
    return 1;
}
// The player's own value of a setting: NULL when it is unset or came from the driver check (a default for this driver: the crash markers still apply to it)
VKQ_UNUSED static const char *vkq_env_player(const char *name) { return vkq_env_src(name) == 1 ? getenv(name) : NULL; }
// What was decided, for the start's summary: value and where it came from (from_env: 0 the default, 1 env.txt, 2 the driver check, vkq_env_src). A site calls this
//   once, when it decides (some decide lazily, at first use, on the replay's or the game's thread: the first call for a rule wins, atomically)
static int g_qval[Q_N] VKQ_UNUSED, g_qenv[Q_N] VKQ_UNUSED, g_qset[Q_N] VKQ_UNUSED;
VKQ_UNUSED static int quirk_note(int q, int value, int from_env) {
    if (q >= 0 && q < Q_N && !__atomic_exchange_n(&g_qset[q], 1, __ATOMIC_ACQ_REL)) { g_qval[q] = value; g_qenv[q] = from_env;
        // (the rules that read the game's profile, gpu_id.eng: their default is this GPU's in this game)
        int prof = q == Q_NO_EDS || q == Q_ASYNC_PSO || q == Q_DRIVER_SELFTESTS || q == Q_NVN_ASYNC || q == Q_B10_STANDIN || q == Q_MALI_WHITE_CHECK;
        if (value || from_env) fprintf(stderr, "[dm] [quirk] %s: %d (%s%s)\n", Q_INFO[q].what, value,
                                       from_env == 2 ? "driver check " : from_env ? "env.txt " : prof ? "default for this GPU in this game's profile" : "default for this GPU", from_env ? Q_INFO[q].env : ""); }
    return value;
}
VKQ_UNUSED static const char *gpu_port_app(int port) { return port == PORT_ARCEUS ? "Arceus Native" : port == PORT_PALDEA ? "Paldea Native" : port == PORT_GALAR ? "Galar Native" : "?"; }
VKQ_UNUSED static void gpu_id_log(const gpu_id *g) {
    fprintf(stderr, "[dm] [quirk] %s: GPU %s, Vulkan %u.%u, vendor %#x, driver id %d, driver version %#x, %s, memory %ld MB; the game's own GPU choices (engine_quirks) %#x\n",
            gpu_port_app(g->port), g->name, g->api >> 22 & 0x7f, g->api >> 12 & 0x3ff, g->vendor, g->driver_id, g->driver_version, g->custom ? "a driver package" : "the phone's own driver",
            g->ram_kb / 1024, g->eng);
    if (g->arm_rel) fprintf(stderr, "[dm] [quirk] Arm driver r%dp%d%s\n", g->arm_rel, g->arm_p, g->arm_beta ? " (beta)" : "");
}
#endif

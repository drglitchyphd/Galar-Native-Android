# Simulated-device tests

The engine's Vulkan code run on a PC against GPUs we do not own: Mesa's software driver (lavapipe) behind the Khronos profiles layer
(`VK_LAYER_KHRONOS_profiles`), which makes lavapipe report another GPU's limits, features, formats and extensions, with the Khronos
validation layer (`VK_LAYER_KHRONOS_validation`) judging every call against that simulated GPU.

## Profiles

Each `profiles/<name>.json` is a public vulkan.gpuinfo.org report's own profile export ("Full JSON profile"), unchanged except for the
profile's name (`VP_SIMDEV_<NAME>`), label and description, and the GPU's identity (deviceName, driverVersion, deviceID, deviceType), which the export leaves out and `fetch_profiles.py` reads from the same
report's page. Each file's `description` names its report and the date it was fetched (2026-11-21). Driver versions were picked to
match what players' logs show.

| profile | GPU, driver | Vulkan | report |
|---|---|---|---|
| mali-g720-r44p1 | Mali-G720-Immortalis MC12, Arm r44p1 (0xb001000) | 1.3.247 | [53376](https://vulkan.gpuinfo.org/displayreport.php?id=53376) |
| mali-g720-r49p1 | Mali-G720 MC7, Arm r49p1, a beta build (`v1.r49p1-03bet0`, 0xc401000) | 1.3.278 | [52703](https://vulkan.gpuinfo.org/displayreport.php?id=52703) |
| mali-g72-r26p0 | Mali-G72 MP3, Arm r26p0 (0x6800000; the Mali-G52 players' driver) | 1.1.131 | [52809](https://vulkan.gpuinfo.org/displayreport.php?id=52809) |
| mali-g72-r14p0 | Mali-G72 MP3, Arm r14p0: Vulkan 1.0 only | 1.0.74 | [50471](https://vulkan.gpuinfo.org/displayreport.php?id=50471) |
| powervr-bxm-8-256 | PowerVR BXM-8-256, driver 6133110 (0x5d9576) | 1.1.170 | [48620](https://vulkan.gpuinfo.org/displayreport.php?id=48620) |
| powervr-dxt-48-1536 | PowerVR D-Series DXT-48-1536 MC1 (Tensor G5), 0x6a5136 | 1.4.344 | [51806](https://vulkan.gpuinfo.org/displayreport.php?id=51806) |
| xclipse-920 | Samsung Xclipse 920, 24.0.534 (0x6000216) | 1.3.279 | [49029](https://vulkan.gpuinfo.org/displayreport.php?id=49029) |
| xclipse-940 | Samsung Xclipse 940, 25.3.3 (0x6403003) | 1.3.304 | [53571](https://vulkan.gpuinfo.org/displayreport.php?id=53571) |

Facts the profiles carry, checked: r44p1 has no `dualSrcBlend` and no `VK_EXT_robustness2`; Arm's driverVersion is
VK_MAKE_VERSION(release, patch, 0); Samsung reports its own vendor ID 0x144D (not AMD's); none of these reports has BC textures
(Xclipse included).

## Running

    bash setup_layers.sh            # once: builds both layers (vulkan-sdk-1.4.363.0) into ~/vkprofiles, outside the repository
    bash run_simdev.sh all          # builds t_vkdev, t_fgen_vk, t_lshud_vk and runs each on lavapipe and on every profile
    bash run_simdev.sh <binary>     # any Vulkan test binary, the same way
    PROFILES="mali-g720-r44p1 lavapipe" bash run_simdev.sh all

On Windows: `wsl bash /mnt/c/.../simdev/run_simdev.sh all`. Logs: `/tmp/simdev-out/<test>.<profile>.log` and `.vvl` (validation).
The summary counts per run: the test's result, its FAIL lines, validation errors / warnings (by VUID) and the profiles layer's errors.
`fetch_profiles.py` refreshes the profiles (network); the runs themselves need none.

## t_vkdev

The engine's own device set-up, `nvn_vk.c` `mtl_init`, compiled as it is (`#include`d into the test, so its decisions can be read; the few
host symbols it links against are stubbed). It checks what the engine decided against what the simulated device reports (every enabled
extension listed, every feature structure allowed by the version or an enabled extension, timeline semaphores / dynamic rendering /
format lists / store-op-none only where the device has them, the optional Vulkan 1.0 features, no Features2 / Properties2 call on a 1.0
instance without its extension, SPIR-V 1.0 modules exactly on 1.0) and against each GPU's known facts (Arm's release from driverVersion,
the clamp-to-zero erratum range, dual-source blending, the vendor defaults of the quirk table). Then it samples the engine's sampler
fallbacks and maps a memory pool for the GPU, fills it from the GPU and reads it back on the CPU.

The profiles layer does not simulate who the device is (vendorID, deviceID, driverVersion, deviceName, the driver ID and info are
"not modifiable" in it), and the engine's per-GPU choices key on exactly that: `t_vkdev` gives the engine the profile's identity itself
(`profile_env.py` exports it as `SIMDEV_*`). Other test binaries see lavapipe's identity with the profile's capabilities.

Mesa's implicit device-select layer is turned off in the runs: it queries `vkGetPhysicalDeviceProperties2` above the validation layer,
which on a Vulkan 1.0 profile showed up as the test's own API-version violation.

`SIMGAP` lines are the simulation's own gaps, not findings: features or extensions the profile promises that lavapipe has not got
(taken out of the device request before it reaches lavapipe), and lavapipe's opaque-fd memory layout (below).

## What this can catch

- invalid usage against another GPU's limits, features, formats and extensions (the validation layer judges against the profile);
- wrong fallbacks: a feature or extension used where the device lacks it, a Vulkan 1.1+ structure or call on a 1.0 / 1.1 device;
- the per-GPU decisions (quirks) taken for the wrong GPU or driver version;
- crashes in set-up code paths that only run on another vendor's device (PowerVR, Xclipse, old Mali).

## What it cannot catch

- driver bugs and shader-compiler bugs: lavapipe executes everything (Mali's white screen, the r44p1 half-texture crash, PowerVR's
  purple BC textures, Xclipse's refused submissions would all pass here);
- swapchain behaviour: image counts (Arm gives more images than minImageCount, 14 on a G57), present modes, timing; there is no window here
  and the profiles layer does not change the swapchain;
- anything lavapipe cannot do that the profile promises: ETC2 / ASTC textures (so the BC-to-ETC2/ASTC transcode path is checked only up
  to the device request, never sampled), `VK_GOOGLE_display_timing`, `VK_EXT_image_compression_control`, protected memory;
- the memory-pool routes as on a phone: there is no `/dev/dma_heap` here (Mali's and Xclipse's dma-buf heap route is never taken);
  lavapipe's exported opaque fd starts with a page of its own, so the engine's opaque-fd route (the fd mapped at offset 0) puts the GPU's
  writes one page off on lavapipe (Vulkan does not define an opaque fd's layout: noted, not counted); and the last route, the driver's
  mapping moved onto the pool with mremap, crashes lavapipe, whose "GPU" is the CPU reading through the moved mapping (on plain lavapipe
  too), so profiles without import or export (Mali-G72 r14p0) get empty submissions instead of the fill;
- performance, memory pressure, tile-based behaviour, timing.

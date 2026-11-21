# Simulated-device tests

The engine's Vulkan code run on a PC against GPUs we do not own: Mesa's software driver (lavapipe) behind the Khronos profiles layer
(`VK_LAYER_KHRONOS_profiles`), which makes lavapipe report another GPU's limits, features, formats and extensions, with the Khronos
validation layer (`VK_LAYER_KHRONOS_validation`) judging every call against that simulated GPU.

## Profiles

Each `profiles/<name>.json` is a public vulkan.gpuinfo.org report's own profile export ("Full JSON profile"), unchanged except for the
profile's name (`VP_SIMDEV_<NAME>`), label and description, and the GPU's identity (deviceName, driverVersion, deviceID, deviceType), which the export leaves out and `fetch_profiles.py` reads from the same
report's page. Each file's `description` names its report and the date it was fetched (2026-11-21). Driver versions were picked to
match what players' logs show: the GPUs named in the Galar / Arceus / Paldea issues (Mali-G615, G52, G57, Adreno 613, 710, 650, 610,
Xclipse 530 / 540 most), each at the stock driver most gpuinfo reports for it show; Adreno 830 is the Galaxy S25 series' own driver.
Two Turnip (Mesa) profiles stand for the custom drivers players install. `t_vkdev`'s expectations for the 34 added on 2026-11-21 were
generated from each profile's own data (vendor, version, Arm release, dualSrcBlend, BC, timeline semaphores, dynamic rendering except on Adreno).

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
| adreno-830 | Adreno 830 (Snapdragon 8 Elite, Galaxy S25 series), Qualcomm 512.800.64 (0x80320040) | 1.3.284 | [52731](https://vulkan.gpuinfo.org/displayreport.php?id=52731) |
| adreno-750 | Adreno 750 (Galaxy S24), Qualcomm 512.762.39 (0x802fa027) | 1.3.128 | [48887](https://vulkan.gpuinfo.org/displayreport.php?id=48887) |
| adreno-740 | Adreno 740 (Galaxy S23), Qualcomm 512.676.1 (0x802a4001) | 1.3.128 | [53490](https://vulkan.gpuinfo.org/displayreport.php?id=53490) |
| adreno-735 | Adreno 735 (Snapdragon 7+ Gen 3), Qualcomm 512.762.46 (0x802fa02e) | 1.3.128 | [52949](https://vulkan.gpuinfo.org/displayreport.php?id=52949) |
| adreno-710-vk13 | Adreno 710 (Galaxy A27), Qualcomm 512.800.70 (0x80320046) | 1.3.284 | [53437](https://vulkan.gpuinfo.org/displayreport.php?id=53437) |
| adreno-710-vk11 | Adreno 710 on an older Qualcomm driver, 512.615.98 (0x80267062) | 1.1.128 | [52519](https://vulkan.gpuinfo.org/displayreport.php?id=52519) |
| adreno-660 | Adreno 660 (Snapdragon 888), Qualcomm 512.530.0 (0x80212000) | 1.1.128 | [53307](https://vulkan.gpuinfo.org/displayreport.php?id=53307) |
| adreno-650 | Adreno 650 (Snapdragon 865), Qualcomm 512.502.0 (0x801f6000) | 1.1.128 | [53114](https://vulkan.gpuinfo.org/displayreport.php?id=53114) |
| adreno-619 | Adreno 619 (Snapdragon 695 / 4 Gen 1 class), Qualcomm 512.530.0 (0x80212000) | 1.1.128 | [52765](https://vulkan.gpuinfo.org/displayreport.php?id=52765) |
| adreno-613-vk11 | Adreno 613 (Snapdragon 4 Gen 2), Qualcomm 512.615.98 (0x80267062) | 1.1.128 | [44905](https://vulkan.gpuinfo.org/displayreport.php?id=44905) |
| adreno-613-vk13 | Adreno 613 on a newer Qualcomm driver, 512.800.61 (0x8032003d) | 1.3.284 | [53115](https://vulkan.gpuinfo.org/displayreport.php?id=53115) |
| adreno-610 | Adreno 610 (Snapdragon 665 / 680), Qualcomm 512.615.98 (0x80267062) | 1.1.128 | [52932](https://vulkan.gpuinfo.org/displayreport.php?id=52932) |
| adreno-506 | Adreno 506 (Snapdragon 625 class), Qualcomm 512.415.0 (0x8019f000) | 1.1.87 | [44748](https://vulkan.gpuinfo.org/displayreport.php?id=44748) |
| adreno-512-vk10 | Adreno 512 (Snapdragon 636), Qualcomm 55.238.2634 (0xdceea4a) | 1.0.61 | [15481](https://vulkan.gpuinfo.org/displayreport.php?id=15481) |
| turnip-adreno-650 | Adreno 650 on Mesa Turnip 26.0.3 (0x6800003) | 1.3.335 | [49424](https://vulkan.gpuinfo.org/displayreport.php?id=49424) |
| turnip-adreno-740 | Adreno 740 on Mesa Turnip 25.2.99 (0x6402063) | 1.4.325 | [43535](https://vulkan.gpuinfo.org/displayreport.php?id=43535) |
| mali-g615-mc6-r44p1 | Mali-G615 MC6 (Dimensity 7300 / 8300 class), Arm r44p1 (0xb001000) | 1.3.247 | [53568](https://vulkan.gpuinfo.org/displayreport.php?id=53568) |
| mali-g615-mc2-r44p1 | Mali-G615 MC2, Arm r44p1 (0xb001000) | 1.3.247 | [53448](https://vulkan.gpuinfo.org/displayreport.php?id=53448) |
| mali-g57-r32p1 | Mali-G57 MC2, Arm r32p1 (0x8001000) | 1.1.177 | [53579](https://vulkan.gpuinfo.org/displayreport.php?id=53579) |
| mali-g57-r38p1 | Mali-G57 MC2, Arm r38p1 (0x9801000) | 1.3.219 | [53570](https://vulkan.gpuinfo.org/displayreport.php?id=53570) |
| mali-g57-r54p1 | Mali-G57 MC2, Arm r54p1 (0xd801000) | 1.3.303 | [53480](https://vulkan.gpuinfo.org/displayreport.php?id=53480) |
| mali-g52-r32p1 | Mali-G52 MC2, Arm r32p1 (0x8001000) | 1.1.177 | [53315](https://vulkan.gpuinfo.org/displayreport.php?id=53315) |
| mali-g52-r49p1 | Mali-G52 MC2, Arm r49p1 (0xc401000) | 1.3.278 | [53514](https://vulkan.gpuinfo.org/displayreport.php?id=53514) |
| mali-g610-r32p1 | Mali-G610 MC6, Arm r32p1 (0x8001000) | 1.1.177 | [53569](https://vulkan.gpuinfo.org/displayreport.php?id=53569) |
| mali-g610-r38p1 | Mali-G610 MC6, Arm r38p1 (0x9801000) | 1.1.219 | [53524](https://vulkan.gpuinfo.org/displayreport.php?id=53524) |
| mali-g68-r32p1 | Mali-G68 MC4, Arm r32p1 (0x8001000) | 1.1.177 | [53425](https://vulkan.gpuinfo.org/displayreport.php?id=53425) |
| mali-g710-r32p1 | Mali-G710 MC10, Arm r32p1 (0x8001000) | 1.1.177 | [52734](https://vulkan.gpuinfo.org/displayreport.php?id=52734) |
| mali-g310-r44p1 | Mali-G310 (ODROID-M2 class), Arm r44p1 (0xb001000) | 1.3.247 | [42905](https://vulkan.gpuinfo.org/displayreport.php?id=42905) |
| mali-g925-r49p1 | Mali-G925-Immortalis MC12 (Dimensity 9400), Arm r49p1 (0xc401000) | 1.3.278 | [53340](https://vulkan.gpuinfo.org/displayreport.php?id=53340) |
| powervr-ge8320 | PowerVR Rogue GE8320 (Helio G35 / P35), driver 5776728 (0x582558) | 1.1.131 | [53553](https://vulkan.gpuinfo.org/displayreport.php?id=53553) |
| powervr-ge8322-vk13 | PowerVR Rogue GE8322, driver 6643903 (0x6560bf) | 1.3.277 | [51013](https://vulkan.gpuinfo.org/displayreport.php?id=51013) |
| xclipse-530 | Samsung Xclipse 530 (Galaxy A55), driver 25.3.21 (0x6403015) | 1.3.304 | [53544](https://vulkan.gpuinfo.org/displayreport.php?id=53544) |
| xclipse-540 | Samsung Xclipse 540 (Galaxy A56), driver 24.0.560 (0x6000230) | 1.3.279 | [53092](https://vulkan.gpuinfo.org/displayreport.php?id=53092) |
| xclipse-950 | Samsung Xclipse 950 (Exynos 2500), driver 25.1.866 (0x6401362) | 1.3.304 | [53128](https://vulkan.gpuinfo.org/displayreport.php?id=53128) |

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

## Many more GPUs: discover.py

Every Android GPU / driver combination gpuinfo has a report for, fetched into a cache outside the repository (`~/simdev-cache`, or
`$SIMDEV_CACHE`; ~150 KB a profile), and run beside the hand-picked ones:

    python3 discover.py index                      # every Android report's GPU, driver and Vulkan version (~90 pages; later runs list only the new ones)
    python3 discover.py pick --by release --custom # one report per GPU + driver release (Qualcomm 512.800, Arm r44p1) + Vulkan version, custom drivers kept
    python3 discover.py fetch                      # the picked profiles not cached yet (two requests each, one a second)
    PROFILE_DIRS=~/simdev-cache/profiles PROFILES="$(python3 discover.py list)" JOBS=8 bash run_simdev.sh all

`pick` options: `--by driver` (every driver build; the most), `--by release`, `--by gpu` (newest report of each GPU only);
`--vendor qualcomm,arm,imgtec,samsung`, `--min-reports N` (combinations seen at least N times), `--min-android 10`, `--limit N`.
`list --vendor arm` narrows the run. On 2026-11-21 the index held 8,895 Android reports: 213 GPUs, 1,336 GPU + release combinations,
1,931 GPU + driver builds (custom drivers, virtual and CPU devices and emulators left out); 1,544 with `--by release --custom`.

t_vkdev has no hand-written row for these: `profile_env.py` computes the expectations from the profile by the same rules
(`SIMDEV_EXPECT`), and for the hand-picked profiles t_vkdev checks that both agree. Each run gets a folder with a link to its one
profile, so the layer does not read the whole cache. `JOBS=8` does about one run a second on a 22-thread PC (16 profiles x 3 tests in 48 s): ~75 min for 1,500 profiles.
Over 60 profiles, the summary lists only the runs that are not clean, then counts per test and result and the VUIDs hit.

## OpenGL ES data and the coverage report

`gles_gpuinfo.py` does the same for opengles.gpuinfo.org (8,328 reports on 2026-11-21): `index`, `pick --by renderer|all`, `fetch`,
`summary`. The site has no export, so each report page is parsed into a small JSON (`~/simdev-cache/gles/reports/<id>.json`: the
GL_* strings, limits, GL and EGL extensions, compressed formats). There is no GLES profiles layer, so nothing is run on these: they are
data for planning an OpenGL ES backend.

`coverage.py` turns both caches into one table (`~/simdev-cache/coverage.md`): Vulkan versions and vendors; for each Vulkan feature,
extension, compressed format and key limit, the share of devices that have it (weighted by gpuinfo's report count per combination);
for GLES, versions, the ANGLE share, extensions on 3.1+ devices, compressed formats and limits. `--min-android 10`, `--vendor arm`,
`--weight combos`. gpuinfo's submitters lean to enthusiasts and newer phones: read the shares as a map, not a census, and weigh them
against the GPUs players report.

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

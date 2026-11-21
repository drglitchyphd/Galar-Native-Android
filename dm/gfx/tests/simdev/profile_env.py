#!/usr/bin/env python3
# Prints the shell exports a simulated run needs for one profile: the profiles layer's name for it, and the GPU's identity (SIMDEV_*), which the profiles laye
# does not simulate (vendorID, deviceID, driverVersion, deviceName, deviceType, driver properties are "not modifiable" in it) and t_vkdev gives the engine itself.
#   eval "$(python3 -I profile_env.py profiles/<name>.json)"
import json, re, shlex, sys

DRIVER_IDS = {   # VkDriverId (vulkan_core.h)
    "VK_DRIVER_ID_AMD_PROPRIETARY": 1, "VK_DRIVER_ID_AMD_OPEN_SOURCE": 2, "VK_DRIVER_ID_MESA_RADV": 3, "VK_DRIVER_ID_NVIDIA_PROPRIETARY": 4,
    "VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS": 5, "VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA": 6, "VK_DRIVER_ID_IMAGINATION_PROPRIETARY": 7,
    "VK_DRIVER_ID_QUALCOMM_PROPRIETARY": 8, "VK_DRIVER_ID_ARM_PROPRIETARY": 9, "VK_DRIVER_ID_GOOGLE_SWIFTSHADER": 10, "VK_DRIVER_ID_GGP_PROPRIETARY": 11,
    "VK_DRIVER_ID_BROADCOM_PROPRIETARY": 12, "VK_DRIVER_ID_MESA_LLVMPIPE": 13, "VK_DRIVER_ID_MOLTENVK": 14, "VK_DRIVER_ID_COREAVI_PROPRIETARY": 15,
    "VK_DRIVER_ID_JUICE_PROPRIETARY": 16, "VK_DRIVER_ID_VERISILICON_PROPRIETARY": 17, "VK_DRIVER_ID_MESA_TURNIP": 18, "VK_DRIVER_ID_MESA_V3DV": 19,
    "VK_DRIVER_ID_MESA_PANVK": 20, "VK_DRIVER_ID_SAMSUNG_PROPRIETARY": 21, "VK_DRIVER_ID_MESA_VENUS": 22, "VK_DRIVER_ID_MESA_DOZEN": 23,
    "VK_DRIVER_ID_MESA_NVK": 24, "VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA": 25, "VK_DRIVER_ID_MESA_HONEYKRISP": 26,
}
DEVICE_TYPES = {"VK_PHYSICAL_DEVICE_TYPE_OTHER": 0, "VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU": 1, "VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU": 2,
                "VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU": 3, "VK_PHYSICAL_DEVICE_TYPE_CPU": 4}

j = json.load(open(sys.argv[1]))
name = next(iter(j["profiles"]))
props = j["capabilities"]["device"].get("properties", {})
p = props.get("VkPhysicalDeviceProperties", {})
drv = {}
for k in ("VkPhysicalDeviceDriverProperties", "VkPhysicalDeviceDriverPropertiesKHR", "VkPhysicalDeviceVulkan12Properties"):
    if "driverID" in props.get(k, {}): drv = props[k]; break
# Old reports have no driver properties (gpuinfo did not record them yet): lavapipe's own ID would show through. The vendor's proprietary driver is the
#   likely one (a Mesa driver names itself in deviceName: Turnip, PanVK, ...); the engine without driver properties decides from the vendor the same way
if not drv.get("driverID"):
    mesa = re.search(r"turnip|panvk|panfrost|freedreno|radv|nvk|venus|llvmpipe|v3d", p.get("deviceName", ""), re.I)
    guess = {0x13B5: "VK_DRIVER_ID_ARM_PROPRIETARY", 0x5143: "VK_DRIVER_ID_QUALCOMM_PROPRIETARY", 0x1010: "VK_DRIVER_ID_IMAGINATION_PROPRIETARY", 0x144D: "VK_DRIVER_ID_SAMSUNG_PROPRIETARY"}
    if not mesa and p["vendorID"] in guess: drv = {"driverID": guess[p["vendorID"]], "driverName": "(guessed from the vendor: the report has no driver properties)", "driverInfo": ""}
env = {
    "VK_KHRONOS_PROFILES_PROFILE_NAME": name,
    "SIMDEV_VENDOR": hex(p["vendorID"]), "SIMDEV_DEVICE_ID": hex(p.get("deviceID", 0)), "SIMDEV_DRIVER_VERSION": hex(p["driverVersion"]),
    "SIMDEV_DEVICE_NAME": p["deviceName"], "SIMDEV_DEVICE_TYPE": str(DEVICE_TYPES.get(p.get("deviceType", ""), 1)),
    "SIMDEV_DRIVER_ID": str(DRIVER_IDS.get(drv.get("driverID", ""), "")), "SIMDEV_DRIVER_NAME": drv.get("driverName", ""), "SIMDEV_DRIVER_INFO": drv.get("driverInfo", ""),
}
for k, v in env.items(): print(f"export {k}={shlex.quote(str(v))}")

# t_vkdev's expectations for a profile it has no hand-written row for (the auto-discovered ones, discover.py): computed here from the profile's own data, the same
#   rules the hand-written rows follow. SIMDEV_EXPECT = vendor,api_minor,arm_rel,arm_p,arm_beta,dualsrc,clampzero,dynren,tlsem,bc (t_vkdev.c expect_t)
caps = j["capabilities"]["device"]; feats = caps.get("features", {}); exts = set(caps.get("extensions", {}).keys())
def feat(s, n): return bool(feats.get(s, {}).get(n, False))
api = p.get("apiVersion", 0)
mn = int(str(api).split(".")[1]) if isinstance(api, str) else (api >> 12) & 0x3ff
mn = min(mn, 3)   # (the engine's instance asks for 1.3 at most)
vendor, dv = p["vendorID"], p["driverVersion"]
rel = pp = beta = 0
# Arm's rules: as the engine decides them, from the driver ID (Huawei's Maleoon driver reports Arm's), or from the vendor when there is none
if (drv.get("driverID") == "VK_DRIVER_ID_ARM_PROPRIETARY") if drv.get("driverID") else vendor == 0x13B5:
    rel, pp = dv >> 22, dv >> 12 & 0x3ff; beta = int("bet" in drv.get("driverInfo", ""))
x = rel * 100 + pp
clamp = int(bool(rel) and ((3801 <= x < 4901) or (beta and 4901 <= x < 4902) or (5000 <= x < 5101)))
# (a feature may sit in the version block or in its own structure, depending on how old the report is: either counts, as the profiles layer fills both)
def anyfeat(*a): return any(feat(s, n) for s, n in a)
DRF = (("VkPhysicalDeviceVulkan13Features", "dynamicRendering"), ("VkPhysicalDeviceDynamicRenderingFeatures", "dynamicRendering"), ("VkPhysicalDeviceDynamicRenderingFeaturesKHR", "dynamicRendering"))
TSF = (("VkPhysicalDeviceVulkan12Features", "timelineSemaphore"), ("VkPhysicalDeviceTimelineSemaphoreFeatures", "timelineSemaphore"), ("VkPhysicalDeviceTimelineSemaphoreFeaturesKHR", "timelineSemaphore"))
if mn >= 3: dr = True   # (required by Vulkan 1.3; some reports leave the structure out)
else:   # (the engine before 1.2: the extension with all its prerequisites, multiview and maintenance2 too on a 1.0 device)
    deps = ["VK_KHR_depth_stencil_resolve", "VK_KHR_create_renderpass2"] + (["VK_KHR_multiview", "VK_KHR_maintenance2"] if mn == 0 else [])
    dr = "VK_KHR_dynamic_rendering" in exts and anyfeat(*DRF) and (mn >= 2 or all(d in exts for d in deps))
ts = True if mn >= 2 else ("VK_KHR_timeline_semaphore" in exts and anyfeat(*TSF))
expect = [vendor, mn, rel, pp, beta, int(feat("VkPhysicalDeviceFeatures", "dualSrcBlend")), clamp, 0 if vendor == 0x5143 else int(bool(dr)), int(bool(ts)),
          int(feat("VkPhysicalDeviceFeatures", "textureCompressionBC"))]
print("export SIMDEV_EXPECT=" + shlex.quote(",".join(str(v) for v in expect)))

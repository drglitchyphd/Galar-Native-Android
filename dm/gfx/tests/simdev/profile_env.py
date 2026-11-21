#!/usr/bin/env python3
# Prints the shell exports a simulated run needs for one profile: the profiles layer's name for it, and the GPU's identity (SIMDEV_*), which the profiles laye
# does not simulate (vendorID, deviceID, driverVersion, deviceName, deviceType, driver properties are "not modifiable" in it) and t_vkdev gives the engine itself.
#   eval "$(python3 -I profile_env.py profiles/<name>.json)"
import json, shlex, sys

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
env = {
    "VK_KHRONOS_PROFILES_PROFILE_NAME": name,
    "SIMDEV_VENDOR": hex(p["vendorID"]), "SIMDEV_DEVICE_ID": hex(p.get("deviceID", 0)), "SIMDEV_DRIVER_VERSION": hex(p["driverVersion"]),
    "SIMDEV_DEVICE_NAME": p["deviceName"], "SIMDEV_DEVICE_TYPE": str(DEVICE_TYPES.get(p.get("deviceType", ""), 1)),
    "SIMDEV_DRIVER_ID": str(DRIVER_IDS.get(drv.get("driverID", ""), "")), "SIMDEV_DRIVER_NAME": drv.get("driverName", ""), "SIMDEV_DRIVER_INFO": drv.get("driverInfo", ""),
}
for k, v in env.items(): print(f"export {k}={shlex.quote(str(v))}")

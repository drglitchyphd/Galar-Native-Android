#!/usr/bin/env python3
# Fetches the simulated devices' Vulkan profiles from vulkan.gpuinfo.org (public reports, "Full JSON profile" export) and writes profiles/<name>.json.
# gpuinfo's export leaves out the device's identity (deviceName, driverVersion, deviceID, deviceType), which the engine's quirk choices key on (Arm's release
# from driverVersion, the G615 rule from deviceName): those are read from the same report's page and put back. The profile is renamed VP_SIMDEV_<NAME>, its label
# and description name the report and the date it was fetched; nothing else is changed. Kept in the repository so a run needs no network; run again to refresh.
#   python3 fetch_profiles.py [name ...]        (default: every entry of DEVICES)
import json, re, sys, os, urllib.request, datetime

# name: (gpuinfo report id, why this one)
DEVICES = {
    "mali-g720-r44p1":     (53376, "Mali-G720-Immortalis MC12, Arm r44p1 (driverVersion 0xb001000), Vulkan 1.3.247, Android 16: the r44p1 driver players run on G615 / G720"),
    "mali-g720-r49p1":     (52703, "Mali-G720 MC7, Arm r49p1 (0xc401000), Vulkan 1.3.278, Android 16: as in players' logs (#128)"),
    "mali-g72-r26p0":      (52809, "Mali-G72 MP3, Arm r26p0 (0x6800000), Vulkan 1.1.131, Android 13: Bifrost; the same driver as the Mali-G52 r26p0 players"),
    "mali-g72-r14p0":      (50471, "Mali-G72 MP3, Arm r14p0, Vulkan 1.0.74, Android 13: a Vulkan 1.0-only driver"),
    "powervr-bxm-8-256":   (48620, "PowerVR BXM-8-256, driver 6133110 (0x5d9576), Vulkan 1.1.170, Android 13: as in players' logs"),
    "powervr-dxt-48-1536": (51806, "PowerVR D-Series DXT-48-1536 MC1 (Tensor G5), driver 6967606 (0x6a5136), Vulkan 1.4.344, Android 17: as in players' logs"),
    "xclipse-920":         (49029, "Samsung Xclipse 920, driver 24.0.534 (0x6000216), Vulkan 1.3.279, Android 16: as in players' logs"),
    "xclipse-940":         (53571, "Samsung Xclipse 940, driver 25.3.3 (0x6403003), Vulkan 1.3.304, Android 17: as in players' logs"),
    # added 2026-11-21: the GPUs in players' issues (Galar / Arceus / Paldea), stock drivers at the versions most reports show
    "adreno-830":          (52731, "Adreno 830 (Snapdragon 8 Elite, Galaxy S25 series), Qualcomm 512.800.64, Vulkan 1.3.284, Android 17: the test phone's GPU on Samsung's driver"),
    "adreno-750":          (48887, "Adreno 750 (Galaxy S24), Qualcomm 512.762.39, Vulkan 1.3.128, Android 16"),
    "adreno-740":          (53490, "Adreno 740 (Galaxy S23), Qualcomm 512.676.1, Vulkan 1.3.128, Android 17"),
    "adreno-735":          (52949, "Adreno 735 (Snapdragon 7+ Gen 3), Qualcomm 512.762.46, Vulkan 1.3.128, Android 16: players' logs"),
    "adreno-710-vk13":     (53437, "Adreno 710 (Galaxy A27), Qualcomm 512.800.70, Vulkan 1.3.284, Android 16: players' logs (#330)"),
    "adreno-710-vk11":     (52519, "Adreno 710 on an older Qualcomm driver, 512.615.98, Vulkan 1.1.128, Android 15: the Vulkan 1.1 Qualcomm rules"),
    "adreno-660":          (53307, "Adreno 660 (Snapdragon 888), Qualcomm 512.530.0, Vulkan 1.1.128, Android 16: players' logs (#281, #240 on Turnip)"),
    "adreno-650":          (53114, "Adreno 650 (Snapdragon 865), Qualcomm 512.502.0, Vulkan 1.1.128, Android 13: players' logs (#331 on Turnip), Galar #209"),
    "adreno-619":          (52765, "Adreno 619 (Snapdragon 695 / 4 Gen 1 class), Qualcomm 512.530.0, Vulkan 1.1.128, Android 14"),
    "adreno-613-vk11":     (44905, "Adreno 613 (Snapdragon 4 Gen 2), Qualcomm 512.615.98, Vulkan 1.1.128, Android 16: the most-named Adreno in players' issues (#211, #189)"),
    "adreno-613-vk13":     (53115, "Adreno 613 on a newer Qualcomm driver, 512.800.61, Vulkan 1.3.284, Android 16"),
    "adreno-610":          (52932, "Adreno 610 (Snapdragon 665 / 680), Qualcomm 512.615.98, Vulkan 1.1.128, Android 14: players' logs (#319)"),
    "adreno-506":          (44748, "Adreno 506 (Snapdragon 625 class), Qualcomm 512.415.0, Vulkan 1.1.87, Android 14: the oldest Vulkan 1.1 Adreno"),
    "adreno-512-vk10":     (15481, "Adreno 512 (Snapdragon 636), Qualcomm 55.238.2634, Vulkan 1.0.61, Android 8.1: a Vulkan 1.0 Adreno driver"),
    "turnip-adreno-650":   (49424, "Adreno 650 on Mesa Turnip 26.0.3, Vulkan 1.3.335: the custom driver players install (#331)"),
    "turnip-adreno-740":   (43535, "Adreno 740 on Mesa Turnip 25.2.99, Vulkan 1.4.325: the custom driver players install"),
    "mali-g615-mc6-r44p1": (53568, "Mali-G615 MC6 (Dimensity 7300 / 8300 class), Arm r44p1, Vulkan 1.3.247, Android 17: the most-named GPU in players' issues"),
    "mali-g615-mc2-r44p1": (53448, "Mali-G615 MC2, Arm r44p1, Vulkan 1.3.247, Android 17: the G615 start-up rule's GPU (#110)"),
    "mali-g57-r32p1":      (53579, "Mali-G57 MC2, Arm r32p1, Vulkan 1.1.177, Android 13: players' logs"),
    "mali-g57-r38p1":      (53570, "Mali-G57 MC2, Arm r38p1, Vulkan 1.3.219, Android 15: players' logs; the first release with the clamp-zero erratum"),
    "mali-g57-r54p1":      (53480, "Mali-G57 MC2, Arm r54p1, Vulkan 1.3.303, Android 16: players' logs"),
    "mali-g52-r32p1":      (53315, "Mali-G52 MC2, Arm r32p1, Vulkan 1.1.177, Android 13: players' logs"),
    "mali-g52-r49p1":      (53514, "Mali-G52 MC2, Arm r49p1, Vulkan 1.3.278, Android 17: players' logs"),
    "mali-g610-r32p1":     (53569, "Mali-G610 MC6, Arm r32p1, Vulkan 1.1.177, Android 15: players' logs (#298)"),
    "mali-g610-r38p1":     (53524, "Mali-G610 MC6, Arm r38p1, Vulkan 1.1.219, Android 14: a Valhall Vulkan 1.1 driver in the erratum range"),
    "mali-g68-r32p1":      (53425, "Mali-G68 MC4, Arm r32p1, Vulkan 1.1.177, Android 15: players' logs"),
    "mali-g710-r32p1":     (52734, "Mali-G710 MC10, Arm r32p1, Vulkan 1.1.177, Android 16: players' logs"),
    "mali-g310-r44p1":     (42905, "Mali-G310 (ODROID-M2 class), Arm r44p1, Vulkan 1.3.247, Android 15: the smallest Valhall"),
    "mali-g925-r49p1":     (53340, "Mali-G925-Immortalis MC12 (Dimensity 9400), Arm r49p1, Vulkan 1.3.278, Android 17: players' logs"),
    "powervr-ge8320":      (53553, "PowerVR Rogue GE8320 (Helio G35 / P35), driver 5776728, Vulkan 1.1.131, Android 14: the commonest budget PowerVR"),
    "powervr-ge8322-vk13": (51013, "PowerVR Rogue GE8322, driver 6643903, Vulkan 1.3.277, Android 15: a Rogue on a Vulkan 1.3 driver"),
    "xclipse-530":         (53544, "Samsung Xclipse 530 (Galaxy A55), driver 25.3.21, Vulkan 1.3.304, Android 17: players' logs"),
    "xclipse-540":         (53092, "Samsung Xclipse 540 (Galaxy A56), driver 24.0.560, Vulkan 1.3.279, Android 16: players' logs (#206)"),
    "xclipse-950":         (53128, "Samsung Xclipse 950 (Exynos 2500), driver 25.1.866, Vulkan 1.3.304, Android 17"),
}
BASE = "https://vulkan.gpuinfo.org/"
UA = {"User-Agent": "simdev-profile-fetch"}

def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return r.read().decode("utf-8", "replace")

def identity(report_id):
    page = get(f"{BASE}displayreport.php?id={report_id}")
    out = {}
    for key in ("deviceName", "driverVersion", "deviceID", "deviceType", "vendorID"):
        m = re.search(r"<td class='subkey'>%s</td><td>([^<]*)</td>" % key, page)
        if not m: raise SystemExit(f"report {report_id}: no {key} on its page")
        out[key] = m.group(1).strip()
    return out

def driver_version(text, vendor):
    # gpuinfo prints Arm's / Samsung's / Mesa's versions decoded as VK_VERSION (major.minor.patch: 10 / 10 / 12 bits) and Imagination's as the raw numbe
    if re.fullmatch(r"\d+", text): return int(text)
    a, b, c = (int(x) for x in text.split("."))
    return a << 22 | b << 12 | c

def fetch(name):
    rid, why = DEVICES[name]
    fetch_one(name, rid, why)

# one report -> <outdir>/<name>.json (discover.py uses it for its cache, outside the repository)
def fetch_one(name, rid, why, outdir=None):
    prof = json.loads(get(f"{BASE}api/v3/getprofile.php?id={rid}"))
    ident = identity(rid)
    (old, meta), = prof["profiles"].items()
    caps = prof["capabilities"]["device"]
    props = caps.setdefault("properties", {}).setdefault("VkPhysicalDeviceProperties", {})
    vendor = int(ident["vendorID"], 16)
    if props.get("vendorID") not in (None, vendor): raise SystemExit(f"{name}: vendor {props.get('vendorID')} in the profile, {vendor:#x} on the page")
    props["deviceName"] = ident["deviceName"]
    props["driverVersion"] = driver_version(ident["driverVersion"], vendor)
    props["deviceID"] = int(ident["deviceID"], 16)
    props["deviceType"] = "VK_PHYSICAL_DEVICE_TYPE_" + ident["deviceType"]
    today = datetime.date.today().isoformat()
    pname = "VP_SIMDEV_" + re.sub(r"[^A-Z0-9]", "_", name.upper())
    meta["label"] = f"simdev {name}: {ident['deviceName']}"
    meta["description"] = f"{why}. Source: {BASE}displayreport.php?id={rid} (profile export {BASE}api/v3/getprofile.php?id={rid}), fetched {today}; " \
                          f"identity (deviceName, driverVersion {props['driverVersion']:#x}, deviceID, deviceType) from the report page"
    prof["profiles"] = {pname: meta}
    outdir = outdir or os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles")
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, name + ".json")
    with open(path, "w", newline="\n") as f: json.dump(prof, f, indent=1); f.write("\n")
    print(f"{name}: report {rid}, {ident['deviceName']}, api {meta.get('api-version')}, driverVersion {props['driverVersion']:#x}, vendor {vendor:#x} -> {path}")

if __name__ == "__main__":
    for n in sys.argv[1:] or DEVICES: fetch(n)

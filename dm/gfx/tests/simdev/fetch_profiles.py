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
    os.makedirs(os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles"), exist_ok=True)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles", name + ".json")
    with open(path, "w", newline="\n") as f: json.dump(prof, f, indent=1); f.write("\n")
    print(f"{name}: report {rid}, {ident['deviceName']}, api {meta.get('api-version')}, driverVersion {props['driverVersion']:#x}, vendor {vendor:#x} -> {path}")

for n in sys.argv[1:] or DEVICES: fetch(n)

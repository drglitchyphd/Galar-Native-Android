#!/usr/bin/env python3
# What real Android devices support, from the caches discover.py (Vulkan profiles) and gles_gpuinfo.py (OpenGL ES reports) fill: for every Vulkan feature,
# extension, compressed format and key limit, and every GLES extension, compressed format and limit, the share of devices that have it. The input for deciding
# what a Vulkan 1.0 baseline and an OpenGL ES backend can count on.
#   python3 coverage.py [--min-android N] [--vendor qualcomm,arm,...] [--weight reports|combos] [--out coverage.md]
# Weights: reports (default): each Vulkan combination counts as many times as gpuinfo has reports of it (discover.py's pick wrote the count), each GLES report
#   once; combos: every combination / report once. gpuinfo's submitters lean to enthusiasts and newer phones: read the shares as a map, not a census.
import json, os, re, sys, glob, argparse, collections

CACHE = os.environ.get("SIMDEV_CACHE") or os.path.expanduser("~/simdev-cache")
ap = argparse.ArgumentParser(); ap.add_argument("--min-android", type=float, default=0); ap.add_argument("--vendor"); ap.add_argument("--weight", choices=("reports", "combos"), default="reports")
ap.add_argument("--out", default=os.path.join(CACHE, "coverage.md")); a = ap.parse_args()
VEND = {"qualcomm": 0x5143, "arm": 0x13B5, "imgtec": 0x1010, "samsung": 0x144D, "nvidia": 0x10DE, "huawei": 0x19E5}
want = {VEND[v.strip().lower()] for v in a.vendor.split(",")} if a.vendor else None
L = []
def out(s=""): L.append(s)
def pct(n, d): return f"{100.0 * n / d:5.1f}%" if d else "  -  "

# ---------------- Vulkan ----------------
vk = []
for f in glob.glob(os.path.join(CACHE, "profiles", "*.json")):
    j = json.load(open(f)); meta = next(iter(j["profiles"].values())); desc = meta.get("description", "")
    m = re.search(r"(\d+) report\(s\)", desc); n = int(m.group(1)) if m and a.weight == "reports" else 1
    m = re.search(r"Android ([\d.]+)", desc); andv = float(m.group(1).split(".")[0]) if m and m.group(1).split(".")[0] else 0
    if a.min_android and andv < a.min_android: continue
    c = j["capabilities"]["device"]; p = c["properties"]["VkPhysicalDeviceProperties"]
    if want and p["vendorID"] not in want: continue
    api = meta.get("api-version", "1.0.0"); mn = int(api.split(".")[1])
    feats = {}
    for s, fs in c.get("features", {}).items():
        for k, v in fs.items():
            if isinstance(v, bool): feats[k] = feats.get(k, False) or v
    fmts = c.get("formats", {})
    vk.append({"w": n, "api": mn, "vendor": p["vendorID"], "feats": feats, "exts": set(c.get("extensions", {}).keys()), "limits": p.get("limits", {}), "fmts": fmts,
               "name": os.path.basename(f)[:-5]})
W = sum(d["w"] for d in vk)
out(f"# Device coverage ({'weighted by gpuinfo reports' if a.weight == 'reports' else 'one per combination'}" + (f", Android {a.min_android:g}+" if a.min_android else "") + (f", {a.vendor}" if a.vendor else "") + ")")
out()
out(f"## Vulkan: {len(vk)} GPU / driver combinations, {W} reports")
out()
if vk:
    out("| Vulkan version | share |"); out("|---|---|")
    for v in range(0, 5):
        s = sum(d["w"] for d in vk if d["api"] == v)
        if s: out(f"| 1.{v} | {pct(s, W)} |")
    vn = collections.Counter(); [vn.__setitem__(d["vendor"], vn[d["vendor"]] + d["w"]) for d in vk]
    names = {v: k for k, v in VEND.items()}
    out(); out("Vendors: " + ", ".join(f"{names.get(v, hex(v))} {pct(n, W).strip()}" for v, n in vn.most_common())); out()
    # features: share having each (only ones some device lacks)
    allf = sorted({k for d in vk for k in d["feats"]})
    rows = [(sum(d["w"] for d in vk if d["feats"].get(k)), k) for k in allf]
    out("### Features (any version's structure), lowest first"); out(); out("| feature | devices with it |"); out("|---|---|")
    for s, k in sorted(rows):
        if s < W: out(f"| {k} | {pct(s, W)} |")
    out(); out("### Extensions, by share (those on at least 2% of devices)"); out(); out("| extension | devices with it |"); out("|---|---|")
    ex = collections.Counter()
    for d in vk:
        for e in d["exts"]: ex[e] += d["w"]
    for e, s in sorted(ex.items(), key=lambda t: -t[1]):
        if s >= 0.02 * W: out(f"| {e} | {pct(s, W)} |")
    out(); out("### Compressed textures (sampled, optimal tiling)"); out(); out("| format | devices |"); out("|---|---|")
    for fm in ("VK_FORMAT_BC1_RGBA_UNORM_BLOCK", "VK_FORMAT_BC3_UNORM_BLOCK", "VK_FORMAT_BC5_UNORM_BLOCK", "VK_FORMAT_BC6H_UFLOAT_BLOCK", "VK_FORMAT_BC7_UNORM_BLOCK",
               "VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK", "VK_FORMAT_EAC_R11G11_UNORM_BLOCK", "VK_FORMAT_ASTC_4x4_UNORM_BLOCK", "VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK"):
        def has(d):
            f = d["fmts"].get(fm, {}); fl = f.get("VkFormatProperties", f).get("optimalTilingFeatures", [])
            return "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT" in fl
        out(f"| {fm[10:]} | {pct(sum(d['w'] for d in vk if has(d)), W)} |")
    out(); out("### Limits (the lowest value, and what 5% / 50% of devices have at most)"); out(); out("| limit | min | 5% | median |"); out("|---|---|---|---|")
    for lim in ("maxImageDimension2D", "maxPushConstantsSize", "maxBoundDescriptorSets", "maxPerStageDescriptorSamplers", "maxPerStageDescriptorUniformBuffers",
                "maxPerStageDescriptorStorageBuffers", "maxPerStageDescriptorSampledImages", "maxPerStageDescriptorStorageImages", "maxPerStageResources",
                "maxDescriptorSetUniformBuffersDynamic", "maxDescriptorSetStorageBuffersDynamic", "maxUniformBufferRange", "maxStorageBufferRange",
                "maxVertexInputAttributes", "maxVertexInputBindings", "maxVertexOutputComponents", "maxFragmentInputComponents", "maxColorAttachments",
                "maxComputeSharedMemorySize", "maxComputeWorkGroupInvocations", "maxSamplerAnisotropy", "minUniformBufferOffsetAlignment", "minStorageBufferOffsetAlignment",
                "maxClipDistances", "maxCullDistances", "maxCombinedClipAndCullDistances", "maxViewports", "maxTessellationGenerationLevel", "maxGeometryOutputVertices"):
        vals = sorted((d["limits"][lim], d["w"]) for d in vk if isinstance(d["limits"].get(lim), (int, float)))
        if not vals: continue
        rev = lim.startswith("min")   # (alignments: higher is worse)
        if rev: vals = vals[::-1]
        tot = sum(w for _, w in vals)
        def q(f):
            acc = 0
            for v, w in vals:
                acc += w
                if acc >= f * tot: return v
            return vals[-1][0]
        out(f"| {lim} | {vals[0][0]} | {q(0.05)} | {q(0.5)} |")
    out()

# ---------------- OpenGL ES ----------------
gl = []
for f in glob.glob(os.path.join(CACHE, "gles", "reports", "*.json")):
    r = json.load(open(f)); i = r["info"]
    try: andv = float((i.get("Android version") or r.get("listing", {}).get("android") or "0").split(".")[0] or 0)
    except ValueError: andv = 0
    if a.min_android and andv < a.min_android: continue
    ven = i.get("GL_VENDOR", "").lower(); ren = i.get("GL_RENDERER", "")
    if want:
        vk_of = {0x5143: "qualcomm", 0x13B5: "arm", 0x1010: "imagination", 0x144D: "samsung", 0x10DE: "nvidia", 0x19E5: "huawei"}
        if not any(vk_of.get(w, "~") in ven for w in want): continue
    m = re.search(r"OpenGL ES (\d)\.(\d)", i.get("GL_VERSION", "")); ver = f"{m.group(1)}.{m.group(2)}" if m else "?"
    gl.append({"ver": ver, "angle": "ANGLE" in ren, "ext": set(r["extensions"]), "fmt": set(r["compressed_formats"]), "caps": r["caps"], "vendor": ven})
G = len(gl)
out(f"## OpenGL ES: {G} reports")
out()
if gl:
    out("| GLES version | share |"); out("|---|---|")
    for v, n in sorted(collections.Counter(d["ver"] for d in gl).items()): out(f"| {v} | {pct(n, G)} |")
    out(); out(f"ANGLE (GLES on the device's Vulkan driver): {pct(sum(d['angle'] for d in gl), G).strip()}. "
               f"Vendors: " + ", ".join(f"{v or '?'} {pct(n, G).strip()}" for v, n in collections.Counter(d['vendor'] for d in gl).most_common(8))); out()
    g3 = [d for d in gl if d["ver"] in ("3.1", "3.2")]
    out(f"### Extensions on GLES 3.1+ devices ({len(g3)} reports; those on at least 2%)"); out(); out("| extension | devices with it |"); out("|---|---|")
    ex = collections.Counter(e for d in g3 for e in d["ext"])
    for e, s in sorted(ex.items(), key=lambda t: -t[1]):
        if s >= 0.02 * len(g3): out(f"| {e} | {pct(s, len(g3))} |")
    out(); out("### Compressed formats (all reports)"); out(); out("| format | devices |"); out("|---|---|")
    fx = collections.Counter(e for d in gl for e in d["fmt"])
    for e, s in sorted(fx.items(), key=lambda t: -t[1]):
        if s >= 0.02 * G: out(f"| {e} | {pct(s, G)} |")
    out(); out("### Limits on GLES 3.1+ devices (lowest, 5%, median)"); out(); out("| limit | min | 5% | median |"); out("|---|---|---|---|")
    keys = sorted({k for d in g3 for k, v in d["caps"].items() if isinstance(v, (int, float))})
    for k in keys:
        vals = sorted(d["caps"][k] for d in g3 if isinstance(d["caps"].get(k), (int, float)))
        if len(vals) < 0.5 * len(g3): continue
        out(f"| {k} | {vals[0]:g} | {vals[int(0.05 * (len(vals) - 1))]:g} | {vals[len(vals) // 2]:g} |")
    out()

open(a.out, "w").write("\n".join(L) + "\n")
print(f"{len(vk)} Vulkan combinations ({W} reports), {G} OpenGL ES reports -> {a.out}")

#!/usr/bin/env python3
# Many more simulated GPUs: every Android GPU / driver combination vulkan.gpuinfo.org has a report for, one report each, fetched into a cache outside the
# repository (a profile is ~150 KB; thousands do not belong in git). The 42 hand-picked profiles in profiles/ stay as they are; these run beside them.
#   python3 discover.py index                 every Android report's GPU, driver, Vulkan version (gpuinfo's report listing, 100 a page; again: only the new ones)
#   python3 discover.py pick [options]        one report per combination -> <cache>/picked.tsv (prints how many, per vendor)
#        --by driver      one per GPU + driver version + Vulkan version (default; the most)
#        --by release     one per GPU + driver release (Qualcomm 512.800, Arm r44p1, ...) + Vulkan version
#        --by gpu         the newest report of each GPU only
#        --min-reports N  combinations seen in at least N reports (how common it is)   --vendor qualcomm,arm,samsung,imgtec,...   --min-android 10   --limit N
#        --custom         keep custom drivers (Turnip, PanVK, ...); left out by default except with --by driver
#   python3 discover.py fetch [--limit N]     the picked profiles not yet in <cache>/profiles (2 requests each, one every DELAY seconds)
#   python3 discover.py list                  the cached profile names (for PROFILES=...)
# Then: PROFILE_DIRS=<cache>/profiles PROFILES="$(python3 discover.py list)" JOBS=8 bash run_simdev.sh all
# Cache: $SIMDEV_CACHE or ~/simdev-cache. Requests: User-Agent simdev-profile-fetch, one every $DELAY seconds (default 1), so gpuinfo is not hammered.
import json, os, re, sys, time, urllib.request, urllib.parse, collections, argparse

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import fetch_profiles   # (fetch_one: the profile export plus the identity from the report page, as for the hand-picked ones)

CACHE = os.environ.get("SIMDEV_CACHE") or os.path.expanduser("~/simdev-cache")
DELAY = float(os.environ.get("DELAY", "1"))
BASE = "https://vulkan.gpuinfo.org/"
UA = {"User-Agent": "simdev-profile-fetch", "X-Requested-With": "XMLHttpRequest"}
COLS = ["id", "device", "gpuname", "driver", "api", "vendor", "devicetype", "osname", "osversion", "osarchitecture", "compare"]
_last = [0.0]

def get_json(url):
    wait = _last[0] + DELAY - time.time()
    if wait > 0: time.sleep(wait)
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                _last[0] = time.time(); return json.loads(r.read().decode("utf-8", "replace"))
        except Exception as e:
            if attempt == 3: raise
            time.sleep(5 * (attempt + 1))

def page(start, n=100):
    p = {"draw": "1", "start": str(start), "length": str(n), "order[0][column]": "id", "order[0][dir]": "desc", "filter[platform]": "android"}
    for i, c in enumerate(COLS): p[f"columns[{i}][data]"] = c
    return get_json(BASE + "api/internal/reports.php?" + urllib.parse.urlencode(p))

def strip(s): return re.sub(r"<[^>]+>", "", str(s)).strip()

def load(name, default):
    path = os.path.join(CACHE, name)
    return json.load(open(path)) if os.path.exists(path) else default

def save(name, obj):
    os.makedirs(CACHE, exist_ok=True); path = os.path.join(CACHE, name)
    json.dump(obj, open(path + ".tmp", "w"), indent=0); os.replace(path + ".tmp", path)

def cmd_index(a):
    idx = load("index.json", {}); start = 0; total = None; new = 0
    while True:
        d = page(start); rows = d.get("data", []); total = d.get("recordsFiltered", total)
        if not rows: break
        fresh = 0
        for x in rows:
            k = str(x["id"])
            if k not in idx: fresh += 1
            idx[k] = {"gpu": strip(x["gpuname"]), "driver": strip(x["driver"]), "api": strip(x["api"]), "device": strip(x["device"]), "vendor": strip(x["vendor"]),
                      "type": strip(x["devicetype"]), "android": strip(x["osversion"])}
        new += fresh; start += len(rows)
        print(f"\r  {start} of {total} reports listed, {new} new", end="", flush=True)
        if idx and fresh == 0 and not a.full: break   # (newest first: a page with nothing new means the rest is already here)
        if start % 1000 == 0: save("index.json", idx)
    save("index.json", idx); print(f"\nindex: {len(idx)} Android reports in {CACHE}/index.json")

CUSTOM = re.compile(r"turnip|panvk|panfrost|mesa|freedreno|v3dv|nvk|venus|virgl|virtio|radv|swiftshader|llvmpipe|lavapipe", re.I)
FAKE = re.compile(r"virtio|waydroid|emulator|sdk_gphone|\bavd\b|genymotion|bluestacks|ldplayer|nox|memu|virtual", re.I)

def release(vendor, driver):
    if vendor.lower().startswith("qualcomm") and re.fullmatch(r"512\.\d+\.\d+", driver): return ".".join(driver.split(".")[:2])
    if vendor.upper() == "ARM" and re.fullmatch(r"\d+\.\d+\.\d+", driver): a, b, _ = driver.split("."); return f"r{a}p{b}"
    return driver

def slug(s): return re.sub(r"-+", "-", re.sub(r"[^a-z0-9.]+", "-", s.lower())).strip("-.")

def cmd_pick(a):
    idx = load("index.json", None)
    if not idx: sys.exit("no index: run 'discover.py index' first")
    vendors = [v.strip().lower() for v in a.vendor.split(",")] if a.vendor else None
    groups = collections.defaultdict(list)
    for rid, r in idx.items():
        if r["type"].lower().replace("_gpu", "") not in ("integrated", "discrete"): continue   # (CPU / virtual: software renderers, not phones)
        if FAKE.search(r["device"]) or FAKE.search(r["gpu"]): continue
        if not a.desktop and r["vendor"].upper() in ("AMD", "INTEL", "APPLE", "NONE", ""): continue   # (x86 Android, Chromebooks, Waydroid on a PC: not phones)
        custom = bool(CUSTOM.search(r["gpu"]))
        if custom and not (a.custom or a.by == "driver"): continue
        if vendors and not any(r["vendor"].lower().startswith(v) for v in vendors): continue
        try:
            if a.min_android and float(r["android"].split(".")[0] or 0) < a.min_android: continue
        except ValueError: pass
        apimm = ".".join(r["api"].split(".")[:2])
        key = (r["gpu"],) if a.by == "gpu" else (r["gpu"], release(r["vendor"], r["driver"]), apimm) if a.by == "release" else (r["gpu"], r["driver"], apimm)
        groups[key].append(int(rid))
    picked = []
    for key, ids in groups.items():
        if len(ids) < a.min_reports: continue
        rid = max(ids); r = idx[str(rid)]   # (the newest report of the combination)
        name = "auto-" + slug(r["gpu"].replace("(TM)", "")) + "--" + slug(r["driver"]) + "--" + str(rid)
        why = f"{r['gpu']}, driver {r['driver']}, Vulkan {r['api']}, Android {r['android']} ({r['device']}); {len(ids)} report(s) of this combination (discover.py --by {a.by})"
        picked.append((len(ids), name, rid, r["vendor"], why))
    picked.sort(key=lambda t: (-t[0], t[1]))
    if a.limit: picked = picked[:a.limit]
    os.makedirs(CACHE, exist_ok=True)
    with open(os.path.join(CACHE, "picked.tsv"), "w") as f:
        for n, name, rid, vendor, why in picked: f.write(f"{name}\t{rid}\t{vendor}\t{why}\n")
    per = collections.Counter(t[3] for t in picked)
    print(f"picked {len(picked)} combinations ({', '.join(f'{v} {c}' for v, c in per.most_common())}) -> {CACHE}/picked.tsv")

def cmd_fetch(a):
    path = os.path.join(CACHE, "picked.tsv")
    if not os.path.exists(path): sys.exit("nothing picked: run 'discover.py pick' first")
    out = os.path.join(CACHE, "profiles"); bad = load("failed.json", {})
    todo = [l.rstrip("\n").split("\t") for l in open(path) if l.strip()]
    todo = [t for t in todo if not os.path.exists(os.path.join(out, t[0] + ".json")) and t[0] not in bad]
    if a.limit: todo = todo[:a.limit]
    print(f"{len(todo)} profiles to fetch (about {len(todo) * 2 * DELAY / 60:.0f} min at one request every {DELAY:g} s)")
    for i, (name, rid, vendor, why) in enumerate(todo, 1):
        wait = _last[0] + DELAY - time.time()
        if wait > 0: time.sleep(wait)
        try: fetch_profiles.fetch_one(name, int(rid), why, out)
        except (Exception, SystemExit) as e:   # (old reports may have no profile export, or a page without some identity field)
            bad[name] = str(e)[:200]; save("failed.json", bad); print(f"  {name}: skipped ({str(e)[:100]})")
        _last[0] = time.time() + DELAY   # (fetch_one made two requests)
        if i % 25 == 0: print(f"  {i} of {len(todo)}")
    print(f"cached: {len(os.listdir(out)) if os.path.isdir(out) else 0} profiles in {out}; {len(bad)} reports without a usable profile ({CACHE}/failed.json)")

def cmd_list(a):
    out = os.path.join(CACHE, "profiles")
    names = sorted(f[:-5] for f in os.listdir(out) if f.endswith(".json")) if os.path.isdir(out) else []
    if a.vendor:
        pre = {"qualcomm": "auto-adreno", "turnip": "auto-turnip", "arm": "auto-mali", "imgtec": "auto-powervr", "samsung": "auto-samsung-xclipse"}
        keep = tuple(pre.get(v.strip().lower(), "auto-" + v.strip().lower()) for v in a.vendor.split(","))
        names = [n for n in names if n.startswith(keep)]
    print(" ".join(names))

ap = argparse.ArgumentParser(description="Find, fetch and list many more simulated GPUs (gpuinfo's Android reports)")
sp = ap.add_subparsers(dest="cmd", required=True)
s = sp.add_parser("index"); s.add_argument("--full", action="store_true", help="list every page again, not only up to the first known one"); s.set_defaults(f=cmd_index)
s = sp.add_parser("pick"); s.add_argument("--by", choices=("driver", "release", "gpu"), default="driver"); s.add_argument("--min-reports", type=int, default=1)
s.add_argument("--vendor"); s.add_argument("--min-android", type=float, default=0); s.add_argument("--limit", type=int, default=0); s.add_argument("--custom", action="store_true"); s.add_argument("--desktop", action="store_true", help="keep AMD / Intel / Apple GPUs"); s.set_defaults(f=cmd_pick)
s = sp.add_parser("fetch"); s.add_argument("--limit", type=int, default=0); s.set_defaults(f=cmd_fetch)
s = sp.add_parser("list"); s.add_argument("--vendor"); s.set_defaults(f=cmd_list)
a = ap.parse_args(); a.f(a)

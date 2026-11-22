#!/usr/bin/env python3
# OpenGL ES capabilities of real Android devices, from opengles.gpuinfo.org: the data an OpenGL ES backend is planned from (which extensions, limits and
# compressed formats it can count on, per GPU and driver). Reports are parsed from their pages (the site has no export) into one small JSON each, in a cache
# outside the repository ($SIMDEV_CACHE or ~/simdev-cache, folder gles/). Nothing is simulated with these: there is no GLES profiles layer; this is data.
#   python3 gles_gpuinfo.py index                every report's device, GLES / GLSL version, renderer, Android version (100 a page; again: only the new ones)
#   python3 gles_gpuinfo.py pick [--by renderer|version|all] [--min-android N] [--limit N]
#        --by renderer   one report per renderer string + GLES version + Android major (default)
#        --by version    one per renderer + the driver's GL_VERSION string (needs the report pages: picks from what fetch has, then the rest per renderer)
#        --by all        every report (8,300+ on 2026-11-21: ~2.5 h at one a second)
#   python3 gles_gpuinfo.py fetch [--limit N]    the picked reports not cached yet -> gles/reports/<id>.json
#   python3 gles_gpuinfo.py summary              one line per cached report (renderer, driver, GLES version, extension count, ANGLE or not)
# Requests: User-Agent simdev-profile-fetch, one every $DELAY seconds (default 1).
import json, os, re, sys, time, html, urllib.request, urllib.parse, collections, argparse

CACHE = os.path.join(os.environ.get("SIMDEV_CACHE") or os.path.expanduser("~/simdev-cache"), "gles")
DELAY = float(os.environ.get("DELAY", "1"))
BASE = "https://opengles.gpuinfo.org/"
UA = {"User-Agent": "simdev-profile-fetch"}
COLS = ["id", "name", "glesversion", "slversion", "renderer", "os", "date", "compare"]
_last = [0.0]

def get(url, xhr=False):
    wait = _last[0] + DELAY - time.time()
    if wait > 0: time.sleep(wait)
    h = dict(UA);
    if xhr: h["X-Requested-With"] = "XMLHttpRequest"
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=60) as r:
                _last[0] = time.time(); return r.read().decode("utf-8", "replace")
        except Exception:
            if attempt == 3: raise
            time.sleep(5 * (attempt + 1))

def strip(s): return html.unescape(re.sub(r"<[^>]+>", "", str(s))).strip()

def load(name, default):
    path = os.path.join(CACHE, name)
    return json.load(open(path)) if os.path.exists(path) else default

def save(name, obj):
    os.makedirs(CACHE, exist_ok=True); path = os.path.join(CACHE, name)
    json.dump(obj, open(path + ".tmp", "w"), indent=0); os.replace(path + ".tmp", path)

def cmd_index(a):
    idx = load("index.json", {}); start = 0; new = 0; total = None
    while True:
        p = {"draw": "1", "start": str(start), "length": "100", "order[0][column]": "id", "order[0][dir]": "desc"}
        for i, c in enumerate(COLS): p[f"columns[{i}][data]"] = c
        d = json.loads(get(BASE + "backend/reports.php?" + urllib.parse.urlencode(p), xhr=True)); rows = d.get("data", []); total = d.get("recordsFiltered", total)
        if not rows: break
        fresh = 0
        for x in rows:
            k = str(strip(x["id"]))
            if k not in idx: fresh += 1
            idx[k] = {"device": strip(x["name"]), "gles": strip(x["glesversion"]), "glsl": strip(x["slversion"]), "renderer": strip(x["renderer"]),
                      "android": strip(x["os"]), "date": strip(x["date"])}
        new += fresh; start += len(rows)
        print(f"\r  {start} of {total} reports listed, {new} new", end="", flush=True)
        if fresh == 0 and not a.full: break
        if start % 1000 == 0: save("index.json", idx)
    save("index.json", idx); print(f"\nindex: {len(idx)} OpenGL ES reports in {CACHE}/index.json")

def android_major(s):
    m = re.match(r"\d+", s or ""); return int(m.group()) if m else 0

def cmd_pick(a):
    idx = load("index.json", None)
    if not idx: sys.exit("no index: run 'gles_gpuinfo.py index' first")
    have = os.path.join(CACHE, "reports"); groups = collections.defaultdict(list)
    for rid, r in idx.items():
        if a.min_android and android_major(r["android"]) < a.min_android: continue
        if a.by == "all": key = (rid,)
        elif a.by == "version":
            f = os.path.join(have, rid + ".json"); ver = json.load(open(f)).get("info", {}).get("GL_VERSION", "") if os.path.exists(f) else "?"
            key = (r["renderer"], ver)
        else: key = (r["renderer"], r["gles"], android_major(r["android"]))
        groups[key].append(int(rid))
    picked = sorted(((len(ids), max(ids)) for ids in groups.values()), key=lambda t: (-t[0], -t[1]))
    if a.limit: picked = picked[:a.limit]
    os.makedirs(CACHE, exist_ok=True)
    with open(os.path.join(CACHE, "picked.tsv"), "w") as f:
        for n, rid in picked: f.write(f"{rid}\t{n}\n")
    print(f"picked {len(picked)} reports of {len(idx)} (--by {a.by}) -> {CACHE}/picked.tsv")

def parse(page):
    t = re.sub(r"\s+", " ", page)
    info = {}
    for k, v in re.findall(r"<td class='subkey'>([^<]+)</td>\s*<td[^>]*>(.*?)</td>", t):
        k = strip(k); v = strip(v)
        if k and k not in info: info[k] = v
    def links(param): return sorted(set(html.unescape(x) for x in re.findall(r"listreports\.php\?" + param + r"=([^'\"&]+)['\"]", t)))
    caps = {}
    for k, v in info.items():
        if k.startswith("GL_MAX") or k.startswith("GL_MIN") or k.startswith("GL_ALIASED") or k.startswith("GL_SUBPIXEL") or k.startswith("GL_NUM"):
            n = v.replace(",", "")
            try: caps[k] = float(n) if "." in n else int(n)
            except ValueError: caps[k] = v
    return {"info": {k: v for k, v in info.items() if k not in caps}, "caps": caps, "extensions": links("extension"), "egl_extensions": links("eglextension"),
            "compressed_formats": links("compressedtextureformat"), "device_features": links("devicefeature")}

def cmd_fetch(a):
    path = os.path.join(CACHE, "picked.tsv")
    if not os.path.exists(path): sys.exit("nothing picked: run 'gles_gpuinfo.py pick' first")
    out = os.path.join(CACHE, "reports"); os.makedirs(out, exist_ok=True); idx = load("index.json", {})
    todo = [l.split("\t")[0] for l in open(path) if l.strip()]
    todo = [r for r in todo if not os.path.exists(os.path.join(out, r + ".json"))]
    if a.limit: todo = todo[:a.limit]
    print(f"{len(todo)} reports to fetch (about {len(todo) * DELAY / 60:.0f} min at one request every {DELAY:g} s)")
    for i, rid in enumerate(todo, 1):
        try:
            rep = parse(get(f"{BASE}displayreport.php?id={rid}"))
            rep["id"] = int(rid); rep["listing"] = idx.get(rid, {})
            if not rep["extensions"] and not rep["caps"]: raise ValueError("no extensions or limits on the page")
            json.dump(rep, open(os.path.join(out, rid + ".json"), "w"), indent=0)
        except Exception as e: print(f"  report {rid}: skipped ({str(e)[:100]})")
        if i % 50 == 0: print(f"  {i} of {len(todo)}", flush=True)
    print(f"cached: {len(os.listdir(out))} reports in {out}")

def cmd_summary(a):
    out = os.path.join(CACHE, "reports")
    for f in sorted(os.listdir(out), key=lambda s: int(s.split(".")[0])):
        r = json.load(open(os.path.join(out, f))); i = r["info"]; ren = i.get("GL_RENDERER", "")
        print(f"{r['id']:>6}  {ren[:48]:48}  {i.get('GL_VERSION', '')[:44]:44}  ext {len(r['extensions']):3}  egl {len(r['egl_extensions']):2}  "
              f"fmt {len(r['compressed_formats']):2}  {'ANGLE' if 'ANGLE' in ren else ''}")

ap = argparse.ArgumentParser(description="OpenGL ES capabilities of Android devices from opengles.gpuinfo.org")
sp = ap.add_subparsers(dest="cmd", required=True)
s = sp.add_parser("index"); s.add_argument("--full", action="store_true"); s.set_defaults(f=cmd_index)
s = sp.add_parser("pick"); s.add_argument("--by", choices=("renderer", "version", "all"), default="renderer"); s.add_argument("--min-android", type=int, default=0)
s.add_argument("--limit", type=int, default=0); s.set_defaults(f=cmd_pick)
s = sp.add_parser("fetch"); s.add_argument("--limit", type=int, default=0); s.set_defaults(f=cmd_fetch)
s = sp.add_parser("summary"); s.set_defaults(f=cmd_summary)
a = ap.parse_args(); a.f(a)

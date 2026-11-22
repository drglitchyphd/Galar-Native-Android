#!/usr/bin/env python3
"""Golden-frame checks: replays every recording (.nxgold) in a folder with golden_play and compares each picture with its expected one.

  golden_check.py run <folder> [--out DIR] [--threshold N] [--max-diff PCT] [--players DIR] [--only NAME...]
        PASS / FAIL / NEW (no expected picture yet) / ERROR per recording; exit 0 when all pass, 1 when any fails or is new, 2 on errors.
        Writes <out>/<name>.png (this run's picture) and, when they differ, <out>/<name>.diff.png (differing pixels red, the rest dimmed).
  golden_check.py accept <folder> [--out DIR] [NAME...]
        This run's pictures become the expected ones: <folder>/<name>.expected.png (all of them, or the named ones).
  golden_check.py synth [--out DIR] [--title ID] [--players DIR]
        The round trip without a phone: golden_synth draws two frames through the PC replay layer with the recorder on, golden_play replays the recording in a
        fresh process, the pictures must be identical; and with the recording's images from earlier frames (TEXI) or its start state (PROL) left out they must not be.
  golden_check.py b10 [--out DIR] [--engine ID] [--title ID] [--players DIR] [--threshold N] [--max-diff PCT]
        The Mali float-target tests on this PC (DM_VK_B10_AS_RGBA16: R11G11B10F drawn as RGBA16F; DM_VK_B10_NOCOMPRESS: R11G11B10F targets without framebuffer
        compression): golden_synth with its R11G11B10F part (GOLDEN_SYNTH_B10=1: a lit pass into R11G11B10F with blending that reads the target's alpha, a copy
        into the game's memory and back, a texture clear, a blit from RGBA8, CPU-filled textures, CPU reads; draws writing negative, NaN, infinite and out-of-range
        values, MAX and REVERSE_SUB blending, a compute shader's image stores, 2-layer and 3D copies, a block-linear upload) once with each setting, both, neither,
        and the stand-in with its CPU copy route (DM_VK_B10_CPU_COPY=1). Each run: its own round trip identical, its CPU reads exact ("b10 check" lines); each
        picture against the default run's within the tolerance (not the CPU route's: one of its corners is a copy that route cannot see); the test's recording
        replayed without the test and the default's with it (the player converts the R11G11B10F images) within the tolerance. Then the pipeline lists: recorded
        with the stand-in and without (DM_VK_PSO_RECORD) they must be the same file, and a start that prewarms one (with the stand-in, without it, and without
        extended dynamic state: DM_VK_NO_EDS=1) must find every pipeline its draws ask for built (nothing more recorded). Exit 0 when everything passes.

A recording's name is its file name without .nxgold. Tolerance: a pixel differs when any channel differs by more than --threshold (default 2 of 255); a recording
fails when more than --max-diff percent of its pixels differ (default 0.1). <folder>/<name>.tolerance ("<threshold> <max-diff>") overrides both for one recording.
The player is <players>/pc-<engine>/golden_play (engine: the title ID the recording's build was made for; dm/pc/build.sh GAME=<engine>). The Vulkan device is
Mesa's lavapipe unless VK_ICD_FILENAMES is set (a Vulkan Profiles layer pretending to be another GPU: VK_INSTANCE_LAYERS etc. pass through; DM_VK_DEVICE picks one).
Recordings and expected pictures are made from the game: they stay outside the repository.
"""
import os, shutil, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PLAYERS = os.path.normpath(os.path.join(HERE, '..', '..', 'pc', 'out'))
LVP = '/usr/share/vulkan/icd.d/lvp_icd.json'

# ---------- PNG (8-bit RGB / RGBA, as the engine and the phone write them) ----------
def png_read(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n': raise ValueError(f'{path}: not a PNG')
    o, idat, w = 8, [], None
    while o < len(d):
        n, t = struct.unpack('>I4s', d[o:o + 8]); body = d[o + 8:o + 8 + n]; o += 12 + n
        if t == b'IHDR': w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT': idat.append(body)
        elif t == b'IEND': break
    if bd != 8 or ct not in (2, 6) or il: raise ValueError(f'{path}: only 8-bit RGB / RGBA, not interlaced')
    bpp = 4 if ct == 6 else 3; stride = w * bpp; raw = zlib.decompress(b''.join(idat)); out = bytearray(h * w * 4); prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]; line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0; p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if bpp == 4: out[y * w * 4:(y + 1) * w * 4] = line
        else:
            row = out[y * w * 4:(y + 1) * w * 4]; row[0::4] = line[0::3]; row[1::4] = line[1::3]; row[2::4] = line[2::3]; row[3::4] = b'\xff' * w
            out[y * w * 4:(y + 1) * w * 4] = row
        prev = line
    return w, h, bytes(out)

def png_write(path, w, h, rgba):
    raw = b''.join(b'\x00' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def compare(a, b, threshold):
    """(differing pixels, total, max channel difference, diff picture or None); RGB only (the engine writes alpha opaque)"""
    (w, h, x), (w2, h2, y) = a, b
    if (w, h) != (w2, h2): return None
    try:
        import numpy as np
        xa = np.frombuffer(x, np.uint8).reshape(h, w, 4)[..., :3].astype(np.int16); ya = np.frombuffer(y, np.uint8).reshape(h, w, 4)[..., :3].astype(np.int16)
        d = np.abs(xa - ya).max(axis=2); bad = d > threshold; n = int(bad.sum()); mx = int(d.max()) if d.size else 0
        img = None
        if n:
            g = (ya.mean(axis=2) / 3).astype(np.uint8); img = np.stack([g, g, g, np.full_like(g, 255)], axis=2)
            img[bad] = np.stack([np.full(n, 255, np.uint8), np.zeros(n, np.uint8), np.zeros(n, np.uint8), np.full(n, 255, np.uint8)], axis=1)
            img = img.tobytes()
        return n, w * h, mx, img
    except ImportError:
        n = mx = 0; img = bytearray(w * h * 4)
        for i in range(0, w * h * 4, 4):
            d = max(abs(x[i] - y[i]), abs(x[i + 1] - y[i + 1]), abs(x[i + 2] - y[i + 2])); mx = max(mx, d)
            if d > threshold: n += 1; img[i:i + 4] = b'\xff\x00\x00\xff'
            else: g = (y[i] + y[i + 1] + y[i + 2]) // 9; img[i:i + 4] = bytes((g, g, g, 255))
        return n, w * h, mx, bytes(img) if n else None

# ---------- Recordings ----------
def meta(path):
    """The recording's META chunk as a dict (its first chunk)"""
    with open(path, 'rb') as f:
        hdr = f.read(16)
        if hdr[:8] != b'NXGOLDN1': raise ValueError(f'{path}: not a golden recording')
        f.seek(struct.unpack('<II', hdr[8:16])[1])
        t, fl, raw, stored = struct.unpack('<IIQQ', f.read(24)); body = f.read(stored)
        if t != struct.unpack('<I', b'META')[0]: raise ValueError(f'{path}: no META chunk first')
        if fl & 1: body = zlib.decompress(body)
    return dict(l.split('=', 1) for l in body.decode('utf-8', 'replace').splitlines() if '=' in l)

def player_for(engine, players):
    p = os.path.join(players, f'pc-{engine}', 'golden_play')
    if not os.path.exists(p): raise FileNotFoundError(f'no player for engine {engine}: {p} (bash dm/pc/build.sh GAME={engine})')
    return p

def vk_env():
    e = dict(os.environ)
    if 'VK_ICD_FILENAMES' not in e and 'VK_DRIVER_FILES' not in e and os.path.exists(LVP): e['VK_ICD_FILENAMES'] = LVP
    return e

def play(rec, out_png, players, extra_env=None):
    m = meta(rec); env = vk_env(); env.update(extra_env or {})
    r = subprocess.run([player_for(m.get('engine', '?'), players), rec, out_png, '--quiet'], env=env, capture_output=True, text=True, timeout=1800)
    return r.returncode, (r.stderr or '')[-4000:]

def tolerance(folder, name, threshold, max_diff):
    p = os.path.join(folder, name + '.tolerance')
    if os.path.exists(p):
        v = open(p).read().split()
        if len(v) >= 2: return int(v[0]), float(v[1])
    return threshold, max_diff

def recordings(folder, only):
    names = sorted(f[:-7] for f in os.listdir(folder) if f.endswith('.nxgold'))
    return [n for n in names if not only or n in only]

def cmd_run(a):
    os.makedirs(a.out, exist_ok=True); bad = err = 0; names = recordings(a.folder, a.only)
    if not names: print(f'no recordings (.nxgold) in {a.folder}'); return 2
    for n in names:
        rec, got, exp = os.path.join(a.folder, n + '.nxgold'), os.path.join(a.out, n + '.png'), os.path.join(a.folder, n + '.expected.png')
        if os.path.exists(got): os.remove(got)
        try: rc, log = play(rec, got, a.players)
        except Exception as e: print(f'ERROR {n}: {e}'); err += 1; continue
        if rc or not os.path.exists(got): print(f'ERROR {n}: the player failed (exit {rc})\n{log}'); err += 1; continue
        if not os.path.exists(exp): print(f'NEW   {n}: no expected picture ({got}; accept it with: golden_check.py accept {a.folder} {n})'); bad += 1; continue
        th, md = tolerance(a.folder, n, a.threshold, a.max_diff)
        c = compare(png_read(got), png_read(exp), th)
        if c is None: print(f'FAIL  {n}: size differs from the expected picture'); bad += 1; continue
        nd, tot, mx, img = c; pct = 100.0 * nd / tot
        diff = os.path.join(a.out, n + '.diff.png')
        if img: png_write(diff, png_read(exp)[0], png_read(exp)[1], img)
        elif os.path.exists(diff): os.remove(diff)
        ok = pct <= md
        print(f'{"PASS " if ok else "FAIL "} {n}: {nd} of {tot} pixels differ ({pct:.3f}%, limit {md}%), largest channel difference {mx} (threshold {th})' + (f'; diff {diff}' if img else ''))
        bad += not ok
    print(f'{len(names)} recordings: {len(names) - bad - err} passed, {bad} failed or new, {err} errors')
    return 2 if err else 1 if bad else 0

def cmd_accept(a):
    names = a.names or recordings(a.folder, None); n_ok = 0
    for n in names:
        got = os.path.join(a.out, n + '.png')
        if not os.path.exists(got): print(f'{n}: no picture from a run in {a.out}'); continue
        shutil.copyfile(got, os.path.join(a.folder, n + '.expected.png')); print(f'{n}: accepted'); n_ok += 1
    return 0 if n_ok == len(names) else 1

def cmd_synth(a):
    """The round trip on this PC (no phone): record → replay in a fresh process → identical picture; parts of the recording left out → a different one"""
    os.makedirs(a.out, exist_ok=True)
    for f in os.listdir(a.out):
        if f.endswith(('.nxgold', '.png')): os.remove(os.path.join(a.out, f))
    engine = a.engine
    synth = os.path.join(a.players, f'pc-{engine}', 'golden_synth')
    if not os.path.exists(synth): print(f'ERROR no {synth} (bash dm/pc/build.sh GAME={engine})'); return 2
    title = a.title or engine
    r = subprocess.run([synth, a.out, title], env=vk_env(), capture_output=True, text=True, timeout=600)
    rec = os.path.join(a.out, f'{title}-synth-p2.nxgold'); live = os.path.join(a.out, f'{title}-synth-p2.phone.png')
    if r.returncode or not os.path.exists(rec) or not os.path.exists(live): print(f'ERROR golden_synth failed (exit {r.returncode})\n{r.stderr[-4000:]}'); return 2
    print(f'recorded {rec} ({os.path.getsize(rec)} bytes); live picture {live}')
    fails = 0
    for label, skip, want_same in (('full recording', None, True), ('without TEXI (earlier frames\' images)', 'texi', False), ('without PROL (start state)', 'prol', False), ('without MEM (memory)', 'mem', False)):
        out = os.path.join(a.out, f'replay{"-no-" + skip if skip else ""}.png')
        rc, log = play(rec, out, a.players, {'GOLDEN_SKIP': skip} if skip else None)
        if rc or not os.path.exists(out):
            if want_same: print(f'FAIL  {label}: the player failed (exit {rc})\n{log}'); fails += 1
            else: print(f'PASS  {label}: no picture (exit {rc}): the recording needs it')
            continue
        c = compare(png_read(out), png_read(live), 0); nd, tot, mx, img = c if c else (1, 1, 255, None)
        if img: png_write(out[:-4] + '.diff.png', png_read(live)[0], png_read(live)[1], img)
        same = nd == 0; ok = same == want_same; fails += not ok
        print(f'{"PASS " if ok else "FAIL "} {label}: {out}: {nd} of {tot} pixels differ from the live picture (largest difference {mx})' + ('' if want_same else ' — must differ'))
    print('synthetic round trip: ' + ('PASS' if not fails else f'FAIL ({fails})'))
    return 1 if fails else 0

def cmd_b10(a):
    """The Mali float-target tests (DM_VK_B10_AS_RGBA16, DM_VK_B10_NOCOMPRESS) on golden_synth's R11G11B10F part: see the module's help"""
    engine = a.engine; synth = os.path.join(a.players, f'pc-{engine}', 'golden_synth'); title = a.title or engine
    if not os.path.exists(synth): print(f'ERROR no {synth} (bash dm/pc/build.sh GAME={engine})'); return 2
    if os.path.isdir(a.out): shutil.rmtree(a.out)
    A = {'DM_VK_B10_AS_RGBA16': '1'}
    cfgs = [('default', {}), ('rgba16', A), ('nocompress', {'DM_VK_B10_NOCOMPRESS': '1'}), ('both', {**A, 'DM_VK_B10_NOCOMPRESS': '1'}), ('rgba16-cpu', {**A, 'DM_VK_B10_CPU_COPY': '1'})]
    fails = 0; live = {}; recs = {}
    def synth_run(out, sw):
        os.makedirs(out, exist_ok=True)
        env = vk_env(); env.update(sw); env['GOLDEN_SYNTH_B10'] = '1'
        r = subprocess.run([synth, out, title], env=env, capture_output=True, text=True, timeout=900)
        open(os.path.join(out, 'golden_synth.log'), 'w').write(r.stderr)
        return r
    for name, sw in cfgs:
        out = os.path.join(a.out, name); r = synth_run(out, sw)
        rec = os.path.join(out, f'{title}-synth-p2.nxgold'); lp = rec[:-7] + '.phone.png'
        checks = [l.split('] ', 1)[-1] for l in r.stderr.splitlines() if 'b10 check' in l]
        done = [l for l in r.stderr.splitlines() if 'b10 checks:' in l and ' run, ' in l]
        notes = sorted({l.split('] ', 1)[-1].split(' (first:')[0] for l in r.stderr.splitlines() if any(k in l for k in ('b10 swap:', 'b10 nocompress:', 'b10 target:', 'formats: R11G11B10F', 'formats: the float-target'))})
        ok = r.returncode == 0 and os.path.exists(rec) and os.path.exists(lp) and len(checks) >= 4 and done and not [c for c in checks if 'FAIL' in c]
        print(f'{"PASS " if ok else "FAIL "} {name} ({" ".join(f"{k}={v}" for k, v in sw.items()) or "no test switch"}): golden_synth exit {r.returncode}')
        for c in checks: print(f'        {c}')
        for n in notes: print(f'        log: {n}')
        if not ok: print(r.stderr[-3000:]); fails += 1; continue
        live[name] = lp; recs[name] = rec
        got = os.path.join(out, 'replay.png'); rc, log = play(rec, got, a.players, sw)
        c = compare(png_read(got), png_read(lp), 0) if not rc and os.path.exists(got) else None
        same = c is not None and c[0] == 0; fails += not same
        print(f'{"PASS " if same else "FAIL "} {name}: its recording replayed with the same setting: ' + (f'{c[0]} of {c[1]} pixels differ from its live picture' if c else f'the player failed (exit {rc})\n{log}'))
    def within(label, got, exp):
        c = compare(png_read(got), png_read(exp), a.threshold)
        if c is None: print(f'FAIL  {label}: size differs'); return 0
        nd, tot, mx, img = c; pct = 100.0 * nd / tot; ok = pct <= a.max_diff
        if img: png_write(got[:-4] + '.diff.png', png_read(exp)[0], png_read(exp)[1], img)
        print(f'{"PASS " if ok else "FAIL "} {label}: {nd} of {tot} pixels differ ({pct:.3f}%, limit {a.max_diff}%), largest channel difference {mx} (threshold {a.threshold})')
        return ok
    if 'default' in live:
        for name in ('rgba16', 'nocompress', 'both'):
            if name in live: fails += not within(f'{name}: its picture against the default one', live[name], live['default'])
        for src, sw, label in (('rgba16', {}, 'the rgba16 recording replayed without the test'), ('default', A, 'the default recording replayed with DM_VK_B10_AS_RGBA16=1')):
            if src not in recs: continue
            got = os.path.join(a.out, src, 'cross.png'); rc, log = play(recs[src], got, a.players, sw)
            if rc or not os.path.exists(got): print(f'FAIL  {label}: the player failed (exit {rc})\n{log}'); fails += 1; continue
            fails += not within(label + ', against the default picture', got, live['default'])
    # The pipeline lists (DM_VK_PSO_RECORD): the stand-in's targets are written as R11G11B10F (b10_key_disk) and read back as the stand-in (b10_key_here), so a list is the
    #   same file with the test or without, and a start that prewarms a list builds the very keys its draws ask for: nothing more is recorded. With and without extended
    #   dynamic state (DM_VK_NO_EDS=1: this phone's own list with the draw state, list + '.sst', prewarm_static)
    def size(p): return os.path.getsize(p) if os.path.exists(p) else -1
    for eds, xs in (('', {}), (' without extended dynamic state', {'DM_VK_NO_EDS': '1'})):
        lists = {}; tag = 'noeds' if xs else 'eds'
        for name, sw in (('default', {}), ('rgba16', A)):
            lst = os.path.join(a.out, f'plist-{tag}-{name}.bin'); r = synth_run(os.path.join(a.out, f'plist-{tag}-{name}'), {**sw, **xs, 'DM_VK_PSO_RECORD': lst})
            ok = r.returncode == 0 and size(lst) > 12 and (not xs or size(lst + '.sst') > 12); fails += not ok; lists[name] = lst
            print(f'{"PASS " if ok else "FAIL "} pipeline list{eds} recorded {"with the stand-in" if sw else "without the test"}: {size(lst)} bytes' + (f', with the draw state {size(lst + ".sst")} bytes' if xs else '') + ('' if ok else f'\n{r.stderr[-2000:]}'))
        if len(lists) == 2:
            same = open(lists['default'], 'rb').read() == open(lists['rgba16'], 'rb').read() and (not xs or open(lists['default'] + '.sst', 'rb').read() == open(lists['rgba16'] + '.sst', 'rb').read())
            fails += not same; print(f'{"PASS " if same else "FAIL "} pipeline list{eds}: the same file with the stand-in and without (its targets written as R11G11B10F)')
        # the prewarm finds shaders by key in the shader pack, as on a phone (before the game reads its shader map): golden_synth's shaders packed by spv_pak.py
        pak = os.path.join(a.out, 'synth.pak')
        if not os.path.exists(pak):
            import glob
            bdir = glob.glob(os.path.join(a.out, f'plist-{tag}-default', 'synth-shaders', 'b-*'))
            r = subprocess.run([sys.executable, os.path.join(HERE, '..', '..', 'android', 'tools', 'spv_pak.py'), bdir[0] if bdir else '-', pak], capture_output=True, text=True)
            if r.returncode or not os.path.exists(pak): print(f'FAIL  the shader pack for the prewarm: {r.stdout[-500:]}{r.stderr[-1500:]}'); fails += 1; continue
        for src, name, sw in (('rgba16', 'rgba16', A), ('rgba16', 'default', {}), ('default', 'rgba16', A)):
            if src not in lists: continue
            lst = os.path.join(a.out, f'plist-{tag}-{src}-then-{name}.bin'); shutil.copyfile(lists[src], lst)
            if xs: shutil.copyfile(lists[src] + '.sst', lst + '.sst')
            before = (size(lst), size(lst + '.sst')); r = synth_run(os.path.join(a.out, f'plist-{tag}-{src}-then-{name}'), {**sw, **xs, 'DM_VK_PSO_RECORD': lst, 'GOLDEN_SYNTH_PAK': pak})
            pw = [l.split('] ', 1)[-1] for l in r.stderr.splitlines() if 'prewarm' in l and ('pipelines' in l or 'listed' in l)]
            built = [l for l in pw if ' 0 built' not in l and 'built' in l and ' 0 with a shader missing' in l and ' 0 failed' in l]
            ok = r.returncode == 0 and (size(lst), size(lst + '.sst')) == before and built; fails += not ok
            print(f'{"PASS " if ok else "FAIL "} the list recorded {"with the stand-in" if src == "rgba16" else "without the test"}, prewarmed {"with the stand-in" if sw else "without the test"}{eds}: '
                  + (f'every pipeline the draws asked for was built (list {before[0]} bytes, unchanged)' if ok else f'list {before} -> {(size(lst), size(lst + ".sst"))} bytes, golden_synth exit {r.returncode}'))
            for l in pw[:2]: print(f'        log: {l}')
    print('float-target tests: ' + ('PASS' if not fails else f'FAIL ({fails})'))
    return 1 if fails else 0

def main():
    import argparse
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    s = p.add_subparsers(dest='cmd', required=True)
    r = s.add_parser('run'); r.add_argument('folder'); r.add_argument('--out'); r.add_argument('--threshold', type=int, default=2); r.add_argument('--max-diff', type=float, default=0.1)
    r.add_argument('--players', default=PLAYERS); r.add_argument('--only', nargs='*')
    c = s.add_parser('accept'); c.add_argument('folder'); c.add_argument('names', nargs='*'); c.add_argument('--out')
    y = s.add_parser('synth'); y.add_argument('--out', default=os.path.join(PLAYERS, 'golden-synth')); y.add_argument('--engine', default='01008DB008C2C000'); y.add_argument('--title')
    y.add_argument('--players', default=PLAYERS)
    b = s.add_parser('b10'); b.add_argument('--out', default=os.path.join(PLAYERS, 'golden-b10')); b.add_argument('--engine', default='01008DB008C2C000'); b.add_argument('--title'); b.add_argument('--players', default=PLAYERS)
    b.add_argument('--threshold', type=int, default=2); b.add_argument('--max-diff', type=float, default=0.1)
    a = p.parse_args()
    if a.cmd in ('run', 'accept') and not a.out: a.out = os.path.join(a.folder, 'out')
    sys.exit({'run': cmd_run, 'accept': cmd_accept, 'synth': cmd_synth, 'b10': cmd_b10}[a.cmd](a))

if __name__ == '__main__':
    main()

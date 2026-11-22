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
  golden_check.py mali [--out DIR] [--engine ID] [--title ID] [--players DIR]
        The Mali white-screen check and its ladder (nvn_vk.c mali_white_vk.inc) on this PC: golden_synth with lit frames (GOLDEN_SYNTH_LIT: a 1280x720 R11G11B10F
        target, 240 draws a frame: white at 65504, fine, black, fine then white, a white corner, drawn through two corner viewports only, NaN, NaN spots, or fine with
        the window then drawn white by a later pass, all of it or 60 %), run as successive starts of the game sharing the ladder's state file. Lavapipe is not Arm's
        driver, so with no switch nothing may happen; DM_MALI_LADDER=1 forces the ladder on: white moves it A -> B -> back to A with the PanVK offer, start by start,
        an app update keeps it and another code of the fixes (the file's code=) starts it over; fine (made quick here: 5 samples, no span) is kept only after two starts, then re-checked in three starts, then not checked;
        a route kept as fine that is re-checked on a white scene turns white; with the default rules a few fine samples then white end WHITE; a black target is
        never fine: 10 empty samples in a row are BROKEN, and a start capped with mostly empty ones makes the route broken; a white tenth is WHITE by the slow rule; a
        start capped with no verdict is counted, and the third makes the route broken; a lit target of NaN is BROKEN, NaN spots stay fine; a white window over a fine
        lit target is WHITE after 5 samples, a 60 % white one is bright (never fine, a start capped on it not counted); frames with too few draws into the lit
        target, or whose draws cover little of the part they span, are not sampled, and a start whose lit frames are never sampled is counted as one without a verdict
        (DM_MALI_NOSAMPLE_MS); a start that crashes on a route (GOLDEN_SYNTH_LIT_ABORT) leaves the marker, also after a stretch of frames without a lit target
        (GOLDEN_SYNTH_PLAIN_FRAMES: the marker's 20 s run from the route's first lit frame), and the next start counts the route as closing the game after a
        crash flag, or after two early ends in a row, never after crashes in the game's own code; starts that end before their first picture (GOLDEN_SYNTH_ABORT_LOADING) count only with the crash flag; a start
        that dies making the check's own compute pass (DM_MALI_WHITE_INIT_ABORT) blames no route, and the check is off on that driver after it; other white-screen
        tests (DM_VK_PROBE=f16target, DM_VK_FMZ=1) keep a fine verdict out of the file but not a white one, and a route set in env.txt keeps the verdict out, its
        note naming the switch; both fixes at once and white count as both white; DM_MALI_WHITE_SIM=1 makes a fine scene count as white; DM_MALI_LADDER=0 turns
        that off. Every sample is the engine's compute pass on the real lit target and the picture shown, read back after its fence. Exit 0 when everything passes.

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
    # every pipeline built where its draw asks for it: since background builds are on for every GPU (DM_VK_ASYNC_PSO, nvn_vk.c apso_on), a draw whose pipeline is
    #   still being built is skipped for a frame or two, which a game hides and a golden frame cannot (golden_check.py b10's one-off draws came out empty on lavapipe,
    #   every run). DM_VK_ASYNC_PSO set by the caller wins
    e.setdefault('DM_VK_ASYNC_PSO', '0')
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

def cmd_mali(a):
    """The Mali white-screen check and its ladder over successive starts (golden_synth's lit frames): see the module's help"""
    import re
    engine = a.engine; synth = os.path.join(a.players, f'pc-{engine}', 'golden_synth'); title = a.title or engine
    if not os.path.exists(synth): print(f'ERROR no {synth} (bash dm/pc/build.sh GAME={engine})'); return 2
    if os.path.isdir(a.out): shutil.rmtree(a.out)
    os.makedirs(a.out); fails = 0; nstart = [0]
    def start(case, lit, sw, frames=16, app='96'):
        """one start of the game: golden_synth in a folder of its own, the case's state file; returns ([mali-white] lines, the state file's fields, exit code, the
        route format lines, the log's path)"""
        nstart[0] += 1; d = os.path.join(a.out, case); os.makedirs(d, exist_ok=True); out = os.path.join(d, f'start{nstart[0]}'); sp = os.path.join(d, 'mali_ladder.txt')
        env = vk_env()
        for k in [k for k in env if k.startswith(('DM_MALI_', 'DM_VK_B10_', 'GOLDEN_SYNTH_', 'DM_LAST_CRASH', 'DM_VK_PROBE', 'DM_VK_PCACHE'))]: del env[k]   # (only what the case sets)
        env.update({'GOLDEN_SYNTH_LIT': lit, 'GOLDEN_SYNTH_FRAMES': str(frames), 'DM_MALI_WHITE_EVERY_MS': '0', 'DM_MALI_RECHECK_MS': '0', 'DM_APP_VERSION': app, 'DM_MALI_LADDER_FILE': sp})
        env.update(sw)
        r = subprocess.run([synth, out, title], env=env, capture_output=True, text=True, timeout=900)
        open(out + '.log', 'w').write(r.stderr)
        lines = [l.split('[mali-white] ', 1)[1] for l in r.stderr.splitlines() if '[mali-white] ' in l]
        st = {}
        if os.path.exists(sp): st = dict(l.split('=', 1) for l in open(sp).read().splitlines() if '=' in l and not l.startswith('#'))
        fmt = [l.split('formats: ', 1)[1].split(' (')[0] for l in r.stderr.splitlines() if 'formats: R11G11B10F' in l]
        return lines, st, r.returncode, fmt, out + '.log'
    def check(ok, what):
        nonlocal fails; fails += not ok; print(f'{"PASS " if ok else "FAIL "} {what}'); return ok
    def plan(lines):   # the start's route and how it checks, from the "on (...)" line
        m = [re.search(r'this start: route (\S+), (checked while playing|re-checked once a minute|not checked)', l) for l in lines]; m = [x for x in m if x]
        return (m[0].group(1), m[0].group(2)) if m else (None, None)
    def samples(lines): return [l for l in lines if re.match(r'sample \d+, ', l)]
    def verdicts(lines): return [(int(m.group(1)), m.group(2), m.group(3)) for m in (re.search(r'verdict after (\d+) samples: route (\S+) is (WHITE|FINE|BROKEN)', l) for l in lines) if m]
    def verdict(lines): v = verdicts(lines); return v[-1] if v else None
    def has(lines, text): return any(text in l for l in lines)
    def kinds(lines): return [m.group(1) for m in (re.search(r': (white|fine|between|empty|nan|shown white|bright); runs', l) for l in samples(lines)) if m]
    A16 = 'R11G11B10F drawn and sampled as RGBA16F'; NC = 'R11G11B10F render targets made without framebuffer compression'
    L1 = {'DM_MALI_LADDER': '1'}; FAST = dict(L1, DM_MALI_FINE_N='5', DM_MALI_FINE_SPAN_MS='0')   # (fine after 5 samples, no span: the fine branch in a few frames)
    print(f'golden_synth {synth}, title {title}; lavapipe unless VK_ICD_FILENAMES is set; logs in {a.out}/<case>/start<n>.log')
    # 1. lavapipe with no switch: not Arm's own driver, so nothing at all
    L, st, rc, fmt, lg = start('off', 'white', {})
    check(rc == 0 and not L and not st and not fmt, f'off on lavapipe with no switch, a white lit scene: exit {rc}, {len(L)} [mali-white] lines, state file {"written" if st else "none"}, route formats {fmt or "none"}')
    # 2. DM_MALI_LADDER=1 (a test switch: the ladder on lavapipe), white: A -> B -> A + PanVK offer, then an app update starts over
    L, st, rc, fmt, lg = start('white', 'white', L1); v = verdict(L); s = samples(L)
    check(rc == 0 and plan(L) == ('A', 'checked while playing') and A16 in fmt and NC not in fmt, f'white, start 1: route A (the RGBA16F stand-in), checked: {plan(L)}, formats {fmt}')
    check(v == (3, 'A', 'WHITE') and len(s) == 3 and all(': white; runs' in x and '576 of 576 texels saturated' in x and '576 at 64 or more' in x and '240 lit draws of 240 in the frame, 100 % of the part covered' in x for x in s),
          f'white, start 1: 3 samples, all 576 of 576 texels saturated (and at 64+), 240 lit draws, the part fully covered; verdict {v}')
    check(has(L, 'start marker: route A') and has(L, 'route A: the first sample came back (start marker cleared)') and st.get('started') == '-',
          f'white, start 1: the start marker set at the start, cleared at the first sample (started={st.get("started")})')
    check(has(L, 'the second fix (route B') and has(L, 'note over the game (no screen here, mali-try-b): Mali: the white-screen fix did not take; trying the second fix') and st.get('A') == 'white'
          and st.get('B') == '-' and st.get('next') == 'B (checked)' and st.get('gpu', '') != '',
          f'white, start 1: the ladder moves to B, its note; state A={st.get("A")} B={st.get("B")} next={st.get("next")} gpu={st.get("gpu")}')
    L, st, rc, fmt, lg = start('white', 'white', L1); v = verdict(L)
    check(rc == 0 and plan(L) == ('B', 'checked while playing') and NC in fmt and A16 not in fmt, f'white, start 2: route B (no framebuffer compression), checked: {plan(L)}, formats {fmt}')
    check(v == (3, 'B', 'WHITE') and has(L, 'neither fix took') and has(L, 'the next start offers the PanVK driver') and st.get('B') == 'white' and st.get('panvk') == 'offer'
          and st.get('next', '').startswith('A (both failed'), f'white, start 2: verdict {v}; back to A with the PanVK offer: state B={st.get("B")} panvk={st.get("panvk")} next={st.get("next")}')
    L, st, rc, fmt, lg = start('white', 'white', L1)
    check(rc == 0 and plan(L) == ('A', 'not checked') and not samples(L) and A16 in fmt and st.get('panvk') == 'offer' and has(L, 'PanVK offer pending') and st.get('started') == '-',
          f'white, start 3: route A, not checked, {len(samples(L))} samples, no marker, the offer pending for the app: {plan(L)}, panvk={st.get("panvk")}')
    L, st, rc, fmt, lg = start('white', 'white', L1, app='97')
    check(rc == 0 and not has(L, 'ladder started over') and plan(L) == ('A', 'not checked') and not samples(L) and st.get('app') == '97' and st.get('panvk') == 'offer' and st.get('A') == 'white'
          and st.get('B') == 'white' and st.get('code') == '1', f'white, start 4 after an app update (97): the ladder kept (both white: route A unchecked, the offer still pending), '
          f'app={st.get("app")} code={st.get("code")}: {plan(L)}')
    sp = os.path.join(a.out, 'white', 'mali_ladder.txt'); txt = open(sp).read(); open(sp, 'w').write(txt.replace('code=1\n', 'code=0\n'))   # (fixes of another code wrote it)
    L, st, rc, fmt, lg = start('white', 'white', L1, app='97'); v = verdict(L)
    check(rc == 0 and has(L, 'ladder started over: another driver, or another code of the fixes') and plan(L) == ('A', 'checked while playing') and v == (3, 'A', 'WHITE') and st.get('code') == '1'
          and st.get('panvk') == '-' and st.get('next') == 'B (checked)', f'white, start 5, the file from fixes of another code: started over at route A, checked, white again: {plan(L)}, {v}, panvk={st.get("panvk")}')
    # 3. a white scene drawn into half of the target (a lower render scale): judged on the part drawn, still white
    L, st, rc, fmt, lg = start('half', 'white', dict(L1, GOLDEN_SYNTH_LIT_SCALE='0.5')); v = verdict(L); s = samples(L)
    check(rc == 0 and v == (3, 'A', 'WHITE') and s and all('the 640x360 part drawn of the 1280x720 target' in x and '576 of 576' in x for x in s), f'half scale: the 640x360 part drawn sampled, all saturated, verdict {v}')
    # 4. fine (5 samples, no span: DM_MALI_FINE_N=5 DM_MALI_FINE_SPAN_MS=0): fine at one start is not kept; kept at the second; re-checked in 3 starts; then unchecked
    L, st, rc, fmt, lg = start('fine', 'fine', FAST, frames=24); v = verdicts(L); s = samples(L)
    check(rc == 0 and plan(L) == ('A', 'checked while playing') and v == [(5, 'A', 'FINE')] and all(': fine; runs' in x and '0 of 576 texels saturated' in x for x in s),
          f'fine, start 1: verdict {v} after samples of 0 / 576')
    check(has(L, 'route A fine at 1 of the 2 starts it needs') and len(s) > 6 and any('(re-check)' in x for x in s) and st.get('A') == '-' and st.get('A_fine_starts') == '1' and st.get('next') == 'A (checked)',
          f'fine, start 1: not kept yet, the check goes on once a minute ({len(s)} samples, re-checks after the verdict); state A={st.get("A")} A_fine_starts={st.get("A_fine_starts")} next={st.get("next")}')
    L, st, rc, fmt, lg = start('fine', 'fine', FAST, frames=24); v = verdicts(L)
    check(rc == 0 and plan(L) == ('A', 'checked while playing') and v == [(5, 'A', 'FINE')] and has(L, 'route A kept as fine on this driver') and st.get('A') == 'fine'
          and st.get('next') == 'A (fine: re-checked once a minute)', f'fine, start 2: fine again, kept: state A={st.get("A")} next={st.get("next")}')
    for i in range(3):
        L, st, rc, fmt, lg = start('fine', 'fine', FAST, frames=12); s = samples(L)
        check(rc == 0 and plan(L) == ('A', 're-checked once a minute') and s and all('(re-check)' in x for x in s) and has(L, f're-check start {i + 1} of 3 on route A') and st.get('A_rechecks') == str(i + 1)
              and st.get('started') == '-' and not has(L, 'start marker: route'), f'fine, start {3 + i}: re-check start {i + 1} of 3, {len(s)} samples, no start marker (A_rechecks={st.get("A_rechecks")})')
    L, st, rc, fmt, lg = start('fine', 'fine', FAST, frames=12)
    check(rc == 0 and plan(L) == ('A', 'not checked') and not samples(L) and A16 in fmt and st.get('next') == 'A (fine: unchecked)', f'fine, start 6: route A kept, not checked, {len(samples(L))} samples')
    # 5. the default rules: fine for 14 frames (5 samples or more), then white: never fine (20 samples over 3 minutes needed), WHITE
    #   (lavapipe signals a sample's fence a few presents later: about one sample every two frames here, three slots in flight)
    L, st, rc, fmt, lg = start('finewhite', 'finewhite', dict(L1, GOLDEN_SYNTH_LIT_FINE_FRAMES='14'), frames=40); v = verdicts(L); k = kinds(L)
    check(rc == 0 and v == [(len(k), 'A', 'WHITE')] and k.count('fine') >= 5 and k[:5] == ['fine'] * 5 and k[-3:] == ['white'] * 3 and st.get('A') == 'white' and st.get('A_fine_starts') == '0',
          f'fine for 14 frames, then white (default rules): {k.count("fine")} fine samples, no fine verdict, then {v}; state A={st.get("A")}')
    # 6. a route kept as fine and re-checked turns white: the ladder moves on
    for i in range(2): start('recheck', 'fine', FAST, frames=16)
    L, st, rc, fmt, lg = start('recheck', 'white', FAST, frames=16); v = verdicts(L); s = samples(L)
    check(rc == 0 and plan(L) == ('A', 're-checked once a minute') and v == [(3, 'A', 'WHITE')] and all('(re-check)' in x for x in s) and st.get('A') == 'white' and st.get('next') == 'B (checked)'
          and has(L, 'trying the second fix'), f'a route kept as fine, re-checked on a white scene: {v}, state A={st.get("A")} next={st.get("next")}')
    # 7. a black (cleared, never drawn) lit target: empty samples, never fine; the cap stops the check with a summary, and a start that was mostly black there makes
    #   the route broken at once (a black picture is no fix either); with no cap in the way, 10 empty samples in a row (30 s of play) are a BROKEN verdict
    L, st, rc, fmt, lg = start('black', 'black', dict(FAST, DM_MALI_WHITE_CAP='8'), frames=32); k = kinds(L)
    check(rc == 0 and not verdicts(L) and k[:8] == ['empty'] * 8 and all('576 near black' in x for x in samples(L)) and has(L, 'no verdict after 8 samples') and has(L, 'the lit target was near black in 8 of 8')
          and has(L, 'route A is BROKEN: its lit target was near black at most samples') and st.get('A') == 'broken' and st.get('A_fine_starts') == '0' and st.get('next') == 'B (checked)'
          and has(L, 'trying the second fix'), f'a black lit target: {len(k)} samples, all empty ({k[:3]}...), no verdict even with fine after 5; capped at 8 with a summary, A broken at once; '
          f'state A={st.get("A")} next={st.get("next")}')
    L, st, rc, fmt, lg = start('blackrun', 'black', FAST, frames=40); v = verdicts(L); k = kinds(L)
    check(rc == 0 and v == [(10, 'A', 'BROKEN')] and k[:10] == ['empty'] * 10 and has(L, 'near black at every sample in a row') and st.get('A') == 'broken' and st.get('next') == 'B (checked)',
          f'a black lit target, no cap: BROKEN at the 10th empty sample in a row: {v}, state A={st.get("A")} next={st.get("next")}')
    # 8. a partly white picture (a tenth of the part: 60 of 576 texels): between, never 3 white in a row; the slow rule calls it WHITE at the 40th sample
    L, st, rc, fmt, lg = start('partial', 'partial', L1, frames=110); v = verdicts(L); k = kinds(L)
    #   (past the first 30 samples one line a minute: the 30 logged and the 40th, the verdict's, are all between)
    check(rc == 0 and v == [(40, 'A', 'WHITE')] and len(k) >= 30 and k == ['between'] * len(k) and all('60 of 576 texels saturated' in x for x in samples(L)) and has(L, 'saturated on average past the slow rule')
          and st.get('A') == 'white', f'a white corner over a tenth (60 of 576 each sample): {k.count("between")} logged samples all between, verdict {v} by the slow rule; state A={st.get("A")}')
    # 9. the 'playing' gate: the frame has 240 draws, but only 40 into the lit target: not sampled
    L, st, rc, fmt, lg = start('fewlit', 'white', dict(L1, GOLDEN_SYNTH_LIT_DRAWS='40', GOLDEN_SYNTH_OTHER_DRAWS='200'))
    check(rc == 0 and not samples(L) and has(L, "not sampled: 40 of the frame's 240 draws into the lit target (60 needed)") and st.get('A') == '-',
          f'40 lit draws of 240 in the frame: {len(samples(L))} samples (the lit target\'s own draws gate it)')
    # 10. ... and the lit draws' rectangles must cover most of the part they span: two small corner viewports do not
    L, st, rc, fmt, lg = start('sparse', 'sparse', L1)
    check(rc == 0 and not samples(L) and any(re.search(r"not sampled: the lit target's 240 draws cover \d+ % of the 1280x720 part they span \(60 % needed\)", l) for l in L) and st.get('A') == '-',
          f'240 white draws through two corner viewports: {len(samples(L))} samples (their cells cover a few % of the part they span)')
    # 11. a route that closes the game: the start marker
    L, st, rc, fmt, lg = start('crash', 'white', dict(L1, GOLDEN_SYNTH_LIT_ABORT='1'))
    check(rc != 0 and st.get('started') == 'A' and st.get('A') == '-' and not samples(L), f'a start that crashes on route A before its first sample is read (abort, exit {rc}): the marker stays (started={st.get("started")})')
    L, st, rc, fmt, lg = start('crash', 'white', dict(L1, DM_LAST_CRASH_DEVICE='1')); v = verdict(L)
    check(rc == 0 and has(L, 'route A closed the game at an earlier start (it crashed after the graphics device came up') and plan(L) == ('B', 'checked while playing') and NC in fmt
          and st.get('A') == 'crashed' and st.get('A_crashes') == '1', f'the next start (crashed after the device came up): route A closed the game, route B checked: {plan(L)}; state A={st.get("A")} A_crashes={st.get("A_crashes")}')
    check(v == (3, 'B', 'WHITE') and has(L, 'once more from the next start') and st.get('next') == 'A (once more: it closed the game once; checked)' and has(L, 'the next start tries the other fix again'),
          f'... B white too: route A once more (it closed the game once): {v}, next={st.get("next")}')
    start('early', 'white', dict(L1, GOLDEN_SYNTH_LIT_ABORT='1'))
    L, st, rc, fmt, lg = start('early', 'white', dict(L1, GOLDEN_SYNTH_LIT_ABORT='1'))
    check(rc != 0 and has(L, 'the last start on route A ended before 20 s of play or a sample, without a crash') and plan(L) == ('A', 'checked while playing') and st.get('early_ends') == '1' and st.get('started') == 'A',
          f'a start that ended early without the crash flag (closed, or ended by the system): route A again, counted (early_ends={st.get("early_ends")})')
    L, st, rc, fmt, lg = start('early', 'white', L1)
    check(rc == 0 and has(L, 'route A closed the game at an earlier start (two starts in a row on it ended before 20 s of play') and plan(L) == ('B', 'checked while playing') and st.get('A') == 'crashed'
          and st.get('A_ends') == '1' and st.get('A_crashes') == '0', f'the second such start in a row: route A closed the game on early ends, route B: {plan(L)}, A={st.get("A")} A_ends={st.get("A_ends")} A_crashes={st.get("A_crashes")}')
    GC = dict(L1, DM_LAST_CRASH_DEVICE='1', DM_LAST_CRASH_GAME='1')
    start('gamecrash', 'white', dict(L1, GOLDEN_SYNTH_LIT_ABORT='1'))
    L, st, rc, fmt, lg = start('gamecrash', 'white', dict(GC, GOLDEN_SYNTH_LIT_ABORT='1'))
    check(rc != 0 and has(L, "the last start on route A crashed in the game's own code (playing stage): not the route's, nothing counted (early ends in a row: 0)") and plan(L) == ('A', 'checked while playing')
          and st.get('early_ends') == '0' and st.get('A') == '-' and st.get('started') == 'A', f'a crash in the game\'s own code (DM_LAST_CRASH_GAME): not the route\'s, nothing counted; route A again: {plan(L)}, '
          f'early_ends={st.get("early_ends")}')
    L, st, rc, fmt, lg = start('gamecrash', 'white', GC); v = verdict(L)
    check(rc == 0 and has(L, "crashed in the game's own code (playing stage): not the route's, nothing counted (early ends in a row: 0)") and plan(L) == ('A', 'checked while playing')
          and st.get('A_crashes') == '0' and st.get('A_ends') == '0' and v == (3, 'A', 'WHITE'), f'two in a row: still route A, nothing counted (A_crashes={st.get("A_crashes")} A_ends={st.get("A_ends")}), '
          f'then judged: {v}')
    # 11b. a crash on the route after a stretch of frames without a lit target (the logos, the title screen and the loads: DM_MALI_MARK_N=10 stands in for 20 s of
    #   presents): the marker's 20 s run from the route's first lit frame, so it is still there and the next start counts the route; on lit frames that are never
    #   sampled (sparse: their draws cover little) it goes after DM_MALI_MARK_N presents on the route
    LATE = dict(L1, GOLDEN_SYNTH_PLAIN_FRAMES='24', DM_MALI_MARK_N='10')
    L, st, rc, fmt, lg = start('latecrash', 'white', dict(LATE, GOLDEN_SYNTH_LIT_ABORT='1'))
    check(rc != 0 and st.get('started') == 'A' and st.get('stage') == 'playing' and has(L, 'route A: its first frame with a lit target') and not has(L, 'start marker cleared') and not samples(L),
          f'24 frames without a lit target (past DM_MALI_MARK_N=10), then a crash at the first lit frame: the marker stays (started={st.get("started")} stage={st.get("stage")})')
    L, st, rc, fmt, lg = start('latecrash', 'white', dict(LATE, DM_LAST_CRASH_DEVICE='1'))
    check(rc == 0 and has(L, 'route A closed the game at an earlier start (it crashed after the graphics device came up') and plan(L) == ('B', 'checked while playing') and st.get('A_crashes') == '1',
          f'the next start: route A closed the game, route B: {plan(L)} (A_crashes={st.get("A_crashes")})')
    L, st, rc, fmt, lg = start('latewindow', 'sparse', LATE, frames=16)
    check(rc == 0 and not samples(L) and has(L, 'route A: 20 s of play on it (10 presents from its first frame with a lit target') and st.get('started') == '-',
          f'lit frames never sampled after 24 without: the marker goes 10 presents into the lit ones (started={st.get("started")})')
    # 11c. lit frames never sampled (40 lit draws of 240, under the floor) for DM_MALI_NOSAMPLE_MS: counted as a start without a verdict; three such starts: route A broken
    NOS = dict(L1, GOLDEN_SYNTH_LIT_DRAWS='40', GOLDEN_SYNTH_OTHER_DRAWS='200', DM_MALI_NOSAMPLE_MS='1')
    for i in range(3):
        L, st, rc, fmt, lg = start('nosample', 'white', NOS)
        check(rc == 0 and not samples(L) and any(re.search(r"no sample in \d+ s of frames with a lit target: not sampled \d+ times for the frame's draws \(120 needed\), \d+ for the lit target's \(60\)", l) and
              'the most draws seen in a frame 240, into a lit target 40' in l for l in L) and (st.get('A_undecided') == str(i + 1) if i < 2 else st.get('A') == 'broken' and st.get('next') == 'B (checked)'),
              f'no sample start {i + 1}: a summary line, counted as one without a verdict (A_undecided={st.get("A_undecided")} A={st.get("A")} next={st.get("next")})')
    # 12. other white-screen tests set in env.txt (the menu's White screen tests, left set): a white verdict still moves the ladder (the fix did not take with them),
    #   a fine one is not kept, and no start marker is left (DM_VK_PROBE=f16target under route B: B white too, so both white and the PanVK offer)
    start('others', 'white', L1)
    L, st, rc, fmt, lg = start('others', 'white', dict(L1, DM_VK_PROBE='f16target')); v = verdict(L)
    check(rc == 0 and plan(L) == ('B', 'checked while playing') and NC in fmt and has(L, 'other white-screen tests (DM_VK_PROBE=f16target)') and v == (3, 'B', 'WHITE')
          and has(L, 'kept although other white-screen tests are set in env.txt: DM_VK_PROBE=f16target') and st.get('B') == 'white' and st.get('A') == 'white' and st.get('panvk') == 'offer'
          and not has(L, 'start marker: route') and has(L, 'neither white-screen fix took'),
          f'DM_VK_PROBE=f16target under route B: verdict {v} kept (B={st.get("B")}, panvk={st.get("panvk")}), no start marker, the ladder\'s own note')
    L, st, rc, fmt, lg = start('othersfine', 'fine', dict(FAST, DM_VK_FMZ='1'), frames=24); v = verdicts(L)
    check(rc == 0 and v == [(5, 'A', 'FINE')] and has(L, 'with other white-screen tests set in env.txt: fine is not kept') and st.get('A') == '-' and st.get('A_fine_starts') == '0'
          and not has(L, 'start marker: route'), f'DM_VK_FMZ=1, a fine scene: {v} at this start, not kept (A={st.get("A")}, A_fine_starts={st.get("A_fine_starts")})')
    # 13. a route set in env.txt wins: its verdict is not kept for a route it is not, and the note names the menu's switch that holds the fix back
    L, st, rc, fmt, lg = start('envwins', 'white', dict(L1, DM_VK_B10_AS_RGBA16='0')); v = verdict(L)
    check(rc == 0 and has(L, 'env.txt sets DM_VK_B10_AS_RGBA16: this start runs none, not route A') and not fmt and v == (3, 'none', 'WHITE') and has(L, 'the verdict is not kept')
          and st.get('A') == '-' and st.get('next') == 'A (checked)' and has(L, 'mali-env:DM_VK_B10_AS_RGBA16): Mali: a test switch in the settings overrides the white-screen fix')
          and not has(L, 'trying the second fix'),
          f'DM_VK_B10_AS_RGBA16=0 over the ladder: runs none (formats {fmt or "none"}), verdict {v}, not kept: A={st.get("A")}, the settings note naming the switch (not the try-B one)')
    # 14. both fixes at once (B set in env.txt beside the ladder's A) and white: both count as white, the PanVK offer is reached
    L, st, rc, fmt, lg = start('both', 'white', dict(L1, DM_VK_B10_NOCOMPRESS='1')); v = verdict(L)
    check(rc == 0 and A16 in fmt and NC in fmt and v == (3, 'A+B', 'WHITE') and st.get('A') == 'white' and st.get('B') == 'white' and st.get('panvk') == 'offer' and has(L, 'neither white-screen fix took'),
          f'A+B white: {v}, state A={st.get("A")} B={st.get("B")} panvk={st.get("panvk")}')
    # 15. DM_MALI_WHITE_SIM=1 on its own: the ladder on lavapipe, a fine scene counted as white
    L, st, rc, fmt, lg = start('sim', 'fine', {'DM_MALI_WHITE_SIM': '1'}); v = verdict(L); s = samples(L)
    check(rc == 0 and has(L, 'DM_MALI_WHITE_SIM=1') and v == (3, 'A', 'WHITE') and s and all('0 of 576 texels saturated' in x and 'counted as all white' in x for x in s)
          and st.get('A') == 'white' and st.get('next') == 'B (checked)', f'DM_MALI_WHITE_SIM=1, a fine scene: measured 0 / 576, counted white, verdict {v}, next={st.get("next")}')
    # 16. DM_MALI_LADDER=0 wins over the simulation
    L, st, rc, fmt, lg = start('ladder0', 'white', {'DM_MALI_LADDER': '0', 'DM_MALI_WHITE_SIM': '1'})
    check(rc == 0 and not L and not st and not fmt, f'DM_MALI_LADDER=0 with DM_MALI_WHITE_SIM=1: {len(L)} [mali-white] lines, state file {"written" if st else "none"}, formats {fmt or "none"}')
    # 17. the driver crashes making the check's own compute shader (#89; DM_MALI_WHITE_INIT_ABORT stands in for it): the start dies in mw_gpu_init with route A's marker
    #   at the detector stage, and the next start, crash flag and all, blames the check, not route A; from then on the check is off on this driver (its own marker
    #   beside the pipeline cache: DM_VK_PCACHE in the case's folder) and an end without a crash counts for nothing. DM_MALI_WHITE_SIM=1 turns the ladder on here
    #   (DM_MALI_LADDER=1 would make mw_gpu_init try again)
    dd = os.path.join(a.out, 'detector'); SIMD = {'DM_VK_PCACHE': os.path.join(dd, 'pcache.bin'), 'DM_MALI_WHITE_SIM': '1'}
    L, st, rc, fmt, lg = start('detector', 'white', dict(SIMD, DM_MALI_WHITE_INIT_ABORT='1'))
    check(rc != 0 and st.get('started') == 'A' and st.get('stage') == 'detector' and os.path.exists(os.path.join(dd, 'mali_white_crashed')) and not samples(L),
          f'a start whose driver dies making the check\'s compute pass (abort, exit {rc}): marker started={st.get("started")} stage={st.get("stage")}, the check\'s own marker left')
    L, st, rc, fmt, lg = start('detector', 'white', dict(SIMD, DM_LAST_CRASH_DEVICE='1'))
    check(rc == 0 and has(L, "ended while making the check's own compute pass (it crashed there): the check's doing, not the route's") and st.get('A') == '-' and st.get('A_crashes') == '0'
          and plan(L) == ('A', 'checked while playing') and has(L, "the check's compute shader closed the game on this driver at an earlier start: no check") and not samples(L) and A16 in fmt
          and st.get('started') == 'A' and st.get('stage') == 'loading',
          f'the next start with the crash flag: route A not blamed (A={st.get("A")}, A_crashes={st.get("A_crashes")}), route A, no check on this driver, its marker at loading (stage={st.get("stage")})')
    for i in range(2): L, st, rc, fmt, lg = start('detector', 'white', SIMD)
    check(rc == 0 and has(L, 'ended at the loading stage') and st.get('A') == '-' and st.get('early_ends') == '0' and not samples(L),
          f'two more starts without the check, ended without a crash: nothing counted (A={st.get("A")}, early_ends={st.get("early_ends")})')
    # 18. starts that end before their first picture without a crash (the player leaving during the pipeline builds, Android ending it in the background): not
    #   counted, however many; one with the crash flag is
    for i in range(3): L, st, rc, fmt, lg = start('loading', 'white', dict(L1, GOLDEN_SYNTH_ABORT_LOADING='1'))
    check(rc != 0 and st.get('started') == 'A' and st.get('stage') == 'loading' and has(L, 'ended at the loading stage') and st.get('A') == '-' and st.get('early_ends') == '0',
          f'three starts ended before their first picture: not counted (A={st.get("A")}, early_ends={st.get("early_ends")}, stage={st.get("stage")})')
    L, st, rc, fmt, lg = start('loading', 'white', dict(L1, DM_LAST_CRASH_DEVICE='1'))
    check(rc == 0 and has(L, 'route A closed the game at an earlier start (it crashed after the graphics device came up') and st.get('A') == 'crashed' and st.get('A_crashes') == '1'
          and plan(L) == ('B', 'checked while playing'), f'... then one with the crash flag: route A closed the game (A={st.get("A")}, A_crashes={st.get("A_crashes")}), route B')
    # 19. NaN in the lit target is counted apart from white: a lit target of NaN (what the stand-in keeps of a NaN: black on screen) is BROKEN, not white; NaN over
    #   28 texels (dark spots) does not keep a route from being fine
    L, st, rc, fmt, lg = start('nan', 'nan', L1); v = verdict(L); s = samples(L)
    check(rc == 0 and v == (3, 'A', 'BROKEN') and s and all('0 of 576 texels saturated' in x and '576 NaN' in x for x in s) and kinds(L)[:3] == ['nan'] * 3
          and has(L, 'NaN in a quarter of the lit target') and st.get('A') == 'broken' and st.get('next') == 'B (checked)',
          f'a lit target of NaN: {v}, samples 0 saturated / 576 NaN, state A={st.get("A")} next={st.get("next")}')
    L, st, rc, fmt, lg = start('nanspots', 'nanspots', dict(FAST, GOLDEN_SYNTH_LIT_CORNER='0.224'), frames=24); v = verdicts(L); s = samples(L)
    check(rc == 0 and v == [(5, 'A', 'FINE')] and s and all('0 of 576 texels saturated' in x and '28 NaN' in x for x in s) and st.get('A_fine_starts') == '1',
          f'NaN spots over 28 of 576 texels (fine after 5 here): {v}, A_fine_starts={st.get("A_fine_starts")}')
    # 20. the picture shown as a second signal: the lit target fine but a later pass drawing the window white is WHITE (5 in a row: longer than a fade to white);
    #   60 % of it white is a bright sample, no evidence either way: never fine (FINE after 5 here otherwise), never white, and a start capped on bright samples is
    #   not counted against the route; a fine scene's picture shown has no white in it
    L, st, rc, fmt, lg = start('postwhite', 'postwhite', L1); v = verdict(L); s = samples(L)
    check(rc == 0 and v == (5, 'A', 'WHITE') and s and all('0 of 576 texels saturated' in x and 'the picture shown 576 of 576 white' in x for x in s) and kinds(L)[:5] == ['shown white'] * 5
          and has(L, 'the picture shown white in samples in a row (longer than a fade to white), the lit target not') and st.get('A') == 'white',
          f'the lit target fine, the window drawn white after it: {v} (5 shown-white samples in a row), state A={st.get("A")}')
    L, st, rc, fmt, lg = start('postbright', 'postbright', dict(FAST, DM_MALI_WHITE_CAP='8'), frames=24); v = verdicts(L); s = samples(L)
    check(rc == 0 and not v and len(s) >= 8 and all('the picture shown 350 of 576 white' in x for x in s) and set(kinds(L)) == {'bright'} and has(L, 'no verdict after 8 samples')
          and has(L, 'a bright scene: no evidence either way): this start is not counted') and st.get('A') == '-' and st.get('A_fine_starts') == '0' and st.get('A_undecided') == '0',
          f'the lit target fine, 60 % of the window drawn white: {len(s)} samples, kinds {sorted(set(kinds(L)))}, no verdict (fine after 5 otherwise); capped at 8, not counted '
          f'(A_undecided={st.get("A_undecided")})')
    L, st, rc, fmt, lg = start('shownfine', 'fine', FAST, frames=24); s = samples(L)
    check(rc == 0 and s and all('the picture shown 0 of 576 white' in x for x in s) and verdicts(L) == [(5, 'A', 'FINE')], f'a fine scene: the picture shown read, none of it white ({len(s)} samples), fine')
    # 21. starts with no verdict are remembered: a white corner over 28 texels (4.9 %: between, under the slow rule's 8 %) reaches the cap (8 here) with no verdict;
    #   the third such start makes route A broken, and the next start runs B
    logs = []
    for i in range(3): L, st, rc, fmt, lg = start('undecided', 'partial', dict(L1, GOLDEN_SYNTH_LIT_CORNER='0.224', DM_MALI_WHITE_CAP='8'), frames=24); logs.append(L)
    check(has(logs[0], 'no verdict after 8 samples') and has(logs[0], 'route A: starts with no verdict 1 of the 3 that make it broken') and has(logs[1], 'starts with no verdict 2 of the 3')
          and set(kinds(logs[0])) == {'between'} and all('28 of 576 texels saturated' in x for x in samples(logs[0])),
          f'a start capped with no verdict (28 of 576 saturated, between): counted, 1 then 2 of 3')
    check(rc == 0 and has(L, 'route A is BROKEN: no verdict at the starts it needed') and st.get('A') == 'broken' and st.get('A_undecided') == '3' and st.get('next') == 'B (checked)'
          and has(L, 'trying the second fix'), f'the third: route A broken (A={st.get("A")}, A_undecided={st.get("A_undecided")}), next={st.get("next")}')
    print('Mali white-screen check: ' + ('PASS' if not fails else f'FAIL ({fails})') + f' ({nstart[0]} starts)')
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
    m = s.add_parser('mali'); m.add_argument('--out', default=os.path.join(PLAYERS, 'golden-mali')); m.add_argument('--engine', default='01008DB008C2C000'); m.add_argument('--title'); m.add_argument('--players', default=PLAYERS)
    a = p.parse_args()
    if a.cmd in ('run', 'accept') and not a.out: a.out = os.path.join(a.folder, 'out')
    sys.exit({'run': cmd_run, 'accept': cmd_accept, 'synth': cmd_synth, 'b10': cmd_b10, 'mali': cmd_mali}[a.cmd](a))

if __name__ == '__main__':
    main()

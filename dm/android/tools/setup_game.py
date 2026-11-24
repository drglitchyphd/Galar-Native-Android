#!/usr/bin/env python3
# The game's setup from the player's own extracted game files, on the phone (the app's setup screen runs it with the Python it ships) or on Linux (the reference run):
#   1. copy: the ExeFS and RomFS folders the player extracted on their own hardware with the update applied, identified by main's build ID (the profiles' build
#      rows); ExeFS → <work>/extract/ExeFS, RomFS → <work>/extract/RomFS, each DLC's RomFS folder → aoc/<n> (dlc_index). No keys, nothing decrypted or unpacked
#   2. rewrite: patch_nso.py, Android target, for every module the profile loads → patched/
#   3. shaders: shader_split.py (the shaders inside RomFS) → dmsh_build.py in SPIR-V-only mode (the translator and spirv-cross built for the platform)
#      → vk_meta.py (the replay's reflection) → spv_pak.py → shaders.pak
#   4. the rest: shader_map.tsv from the profile, fonts (the phone's own font as the standard font, the button-icon font shipped with the app)
# The game ends up in <out>: RomFS/, patched/, shaders.pak, shader_map.tsv, font/, aoc/ (what android_main.c reads). Progress goes to stdout as lines
#   "STEP <n>/<total> <what>" and "DONE" / "FAILED <why>"; details go to <work>/setup.log.
#        setup_game.py --captures <dir> --out <game dir> --work <scratch dir> --bin <dir>: translate the shaders the game built in memory and a run saved
#          (nvn_vk.c mtl_shader_seen: <key>.ctl / <key>.code), into the same shader set, and pack it again; the game picks them up at its next start
# Usage: setup_game.py --exefs <ExeFS folder> --romfs <RomFS folder> [--dlc-dir <DLC RomFS folder> ...] [--move] --out <game dir> --work <scratch dir>
#                      --bin <dir with dmsh_xlate, spirv-cross> [--ext-font <nintendo_ext.ttf>] [--std-font <ttf>]
#        (--exefs and --romfs may both be the dump's parent folder that holds exefs/ and romfs/, any case)
# Environment: DM_GAME (the profile: the app's game, SetupActivity passes it; default 01008DB008C2C000, Pokémon Shield), J (jobs)
import argparse, glob, hashlib, os, re, shutil, signal, struct, subprocess, sys, threading, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.normpath(os.path.join(HERE, '../..'))
GAME = os.environ.setdefault('DM_GAME', '01008DB008C2C000')
sys.path.insert(0, os.path.join(DM, 'games')); import game as _game
PROFILE = _game.load()
NAME = PROFILE.get('name')

_TOOLS = None
MOVED_WORK = []   # the setup moved the player's folders (--move): a failure moves them back (restore_moved)
def chain_tool(name):
    """dmsh_chain.py / dmsh_build.py of this game's shader chain generation (dm/gfx/chains/chain_tree.py, the profile's xlate_chain): dm/gfx/tools itself when it
    is that generation (an APK carries its game's generation there, named in gfx/tools/CHAIN; a checkout's live tools for a game on them), else a tool tree with
    the generation's frozen files, made once a run in the temp folder (a checkout setting up Legends: Arceus or Scarlet / Violet: the translator before FMZ)"""
    global _TOOLS
    if _TOOLS is None:
        live = os.path.join(DM, 'gfx/tools'); mark = os.path.join(live, 'CHAIN'); chains = os.path.join(DM, 'gfx/chains')
        have = open(mark).read().strip() if os.path.isfile(mark) else ''
        _TOOLS = live
        if os.path.isdir(chains):   # (an APK has no gfx/chains: its gfx/tools is the generation already)
            sys.path.insert(0, chains); import chain_tree
            gen = chain_tree.gen_of(PROFILE)
            if gen != have:
                import tempfile; t = os.path.join(tempfile.gettempdir(), f'dm-chain-{gen or "live"}-{os.getpid()}')
                _TOOLS = os.path.join(chain_tree.materialize(gen, t), 'gfx/tools')
                print(f'shader tools: chain generation {gen or "live"} ({_TOOLS})', flush=True)
    return os.path.join(_TOOLS, name)

def write_runtime_shader_map(found_map, profile_map, output):
    """Use shader locations found in this dump, retaining profile slots for runtime-captured shaders."""
    with open(found_map, encoding='utf-8') as f: rows = f.readlines()
    keys = set()
    for line in rows:
        c = line.rstrip('\n').split('\t')
        if len(c) > 4 and not line.startswith('#'): keys.add(c[3])
    with open(profile_map, encoding='utf-8') as f:
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) > 4 and c[3] not in keys and c[4].startswith('/nvn_capture/'):
                rows.append(line)
    with open(output, 'w', encoding='utf-8') as f: f.writelines(rows)

def watch_translation(found_map, work, built, stop):
    """'PROGRESS done/total' lines for the setup screen (SetupActivity.runSetup) while dmsh_build.py runs: it prints nothing until the end, and a phone takes hours.
    Counted from what it writes (left unchanged, since any edit to it changes the shader chain version): every shader's SPIR-V in its dmsh_* scratch dir,
    the vertex shaders' GL depth variants in gl_depth/ there, then every shader's reflection json in the cache"""
    n = vs = 0
    with open(found_map, encoding='utf-8') as f:
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) > 4 and not line.startswith('#') and c[0] != 'package': n += 1; vs += c[2] == 'vertex'
    total, shown = 2 * n + vs, -1
    def count(d, end):
        try: return sum(e.name.endswith(end) for e in os.scandir(d))
        except OSError: return 0
    while not stop.wait(5):
        done = sum(count(d, '.spv') + count(os.path.join(d, 'gl_depth'), '.spv') for d in glob.glob(os.path.join(work, 'dmsh_*'))) + count(built, '_b.json')
        done = min(done, total - 1)   # the map also lists shaders that wait for capture, so the count can fall short of it: 100% is the step's end
        if done != shown: shown = done; print(f'PROGRESS {done}/{total}', flush=True)

def watch_files(d, end, total, stop):
    """'PROGRESS done/total' lines while a step writes one file per shader into d (vk_meta.py: a .vkr for each .spv); it prints nothing until it ends"""
    shown = -1
    while not stop.wait(5):
        try: done = sum(e.name.endswith(end) for e in os.scandir(d))
        except OSError: done = 0
        done = min(done, total - 1)
        if done != shown: shown = done; print(f'PROGRESS {done}/{total}', flush=True)

def distinct_shaders(found_map, pkg, fact_tables, never, out_map):
    """The shaders that are another shader's program under a different key are left out of the map the shader build reads (out_map), so each distinct program is
    translated once; → {left-out key: the key translated for it}. About half of Sword / Shield's 38,501 shaders are such repeats (census 2026-11-22: 18,689
    distinct, 0 exceptions). Two shaders are the same program when everything the translation reads of them is the same: the stage, the code area (with the
    c1 tables in it), the control section (the transform feedback layout is read from it) and their rows in the answer tables. The control sections of such
    repeats differ only in the 8 bytes at +0x7d0, which nothing reads, so those are left out of the comparison. Shaders inside the game's main program, shaders
    that wait for capture and shaders with a hand-written replacement (never) are always kept. The shader build itself is unchanged (any edit to it changes the
    shader chain version): it only gets a shorter map"""
    facts = {}
    for t in fact_tables:
        if not os.path.isfile(t): continue
        for ln in open(t, encoding='utf-8'):
            if ln.strip() and not ln.startswith('#'): k, _, v = ln.rstrip('\n').partition('\t'); facts.setdefault(k, []).append(v)
    first, same, kept, lines = {}, {}, set(), []; pack = (None, b'', [])
    for ln in open(found_map, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) <= 4 or c[0] == 'package' or c[0].startswith('#'): lines.append(ln); continue
        key = c[3]
        if key in same: continue
        fp = None
        if key not in kept and key not in never and c[0].endswith('.bin') and c[1].isdigit() and os.path.isfile(os.path.join(pkg, c[0])):
            if pack[0] != c[0]:
                d = open(os.path.join(pkg, c[0]), 'rb').read(); pack = (c[0], d, [m.start() for m in re.finditer(b'BTCDBTCD', d)])
            _, d, at = pack; k = int(c[1])
            if k < len(at):
                rec = d[at[k]:at[k + 1] if k + 1 < len(at) else len(d)]; ci = rec.find(bytes.fromhex('34127698'))
                if ci >= 0x38 and len(rec) >= ci + 0x7d8:
                    ctl = bytearray(rec[ci:ci + 0x880]); ctl[0x7d0:0x7d8] = bytes(8)
                    fp = hashlib.sha256(b'\0'.join([c[0][:2].encode(), str(ci).encode(), rec[0x38:ci], bytes(ctl), '\n'.join(facts.get(key, [])).encode()])).digest()
        if fp is not None and fp in first: same[key] = first[fp]; continue
        if fp is not None: first[fp] = key
        kept.add(key); lines.append(ln)
    with open(out_map, 'w', encoding='utf-8') as f: f.writelines(lines)
    return same

def share_translated(vk, same):
    """Each left-out shader gets the files of the one translated for it (distinct_shaders): its SPIR-V, the vertex shaders' GL depth variant and the reflection,
    which hold nothing of the key. A link where the storage has them (no bytes written), else a copy; → how many shaders got theirs"""
    n = 0
    for key, rep in same.items():
        got = False
        for end in ('.spv', '.gl.spv', '.vkr'):
            src = os.path.join(vk, rep + end); dst = os.path.join(vk, key + end)
            try: os.remove(dst)
            except FileNotFoundError: pass
            if not os.path.isfile(src): continue
            try: os.link(src, dst)
            except OSError:   # (some storage makes the link and still reports an error: copying onto it then fails as the same file, #320)
                if os.path.lexists(dst): os.remove(dst)
                shutil.copyfile(src, dst)
            got = got or end == '.spv'
        n += got
    return n

def translated_mark(work):
    return os.path.join(work, 'translated.json')

def found_digest(work):
    """What the shader scan found, as one hash: a translation made for the same list (and chain) can be kept by the next setup of the same dump"""
    try: return hashlib.sha256(open(os.path.join(work, 'shader_map.found.tsv'), 'rb').read()).hexdigest()
    except OSError: return ''

def translation_kept(work, chain):
    """A fresh setup of the dump the last one translated (the same shaders, the same chain): its translated shaders are kept and only what is missing is
    translated, instead of hours of translating everything again (#100: a setup stopped after the translation was all done)"""
    import json
    try: m = json.load(open(translated_mark(work), encoding='utf-8'))
    except (OSError, ValueError): return False
    return m.get('chain') == chain and m.get('found') == found_digest(work) and bool(m.get('found'))

# ---- step 1: the player's own extracted game files ----
# The player extracts the game on their own hardware into two folders with the update applied (a merged base + update): ExeFS (main, main.npdm, rtld, sdk,
# subsdk*) and RomFS, plus one RomFS folder per DLC. Nothing is decrypted or unpacked here: no keys, no NSP / XCI (nx_extract, prod.keys and title.keys are gone
# from the app)
def child_dir(d, name):
    """d's sub-folder called name in any case (exefs, ExeFS, EXEFS), or None"""
    try:
        for e in sorted(os.scandir(d), key=lambda e: e.name):
            if e.is_dir() and e.name.lower() == name.lower(): return e.path
    except OSError: pass
    return None

def resolve_exefs(path):
    """The ExeFS folder: path itself when it holds main, else its exefs sub-folder (one pick of a dump's parent folder with exefs/ and romfs/, as nxdumptool and the
    emulators' "dump ExeFS / RomFS" make it)"""
    if not path or not os.path.isdir(path): raise RuntimeError(f'the ExeFS folder {path or ""} was not found: choose your files again')
    if os.path.isfile(os.path.join(path, 'main')): return path
    sub = child_dir(path, 'exefs')
    if sub and os.path.isfile(os.path.join(sub, 'main')): return sub
    raise RuntimeError(f'{path} is not an ExeFS folder: it has no file named main. Choose the ExeFS folder extracted from your game (main, main.npdm, rtld, sdk, ...)')

def resolve_romfs(path):
    """The RomFS folder: path itself, or its romfs sub-folder (the dump's parent folder)"""
    if not path or not os.path.isdir(path): raise RuntimeError(f'the RomFS folder {path or ""} was not found: choose your files again')
    sub = child_dir(path, 'romfs')
    if sub: path = sub
    if os.path.isfile(os.path.join(path, 'main')): raise RuntimeError(f'{path} is an ExeFS folder, not a RomFS folder: choose the RomFS folder too')
    try: empty = not any(True for _ in os.scandir(path))
    except OSError as e: raise RuntimeError(f'the RomFS folder {path} cannot be read ({e})')
    if empty: raise RuntimeError(f'the RomFS folder {path} is empty')
    return path

TID_RE = re.compile(r'(?<![0-9A-Fa-f])(01[0-9A-Fa-f]{14})(?![0-9A-Fa-f])')
def app_of(tid):   # the game a title ID belongs to: an update is the game's ID + 0x800, a DLC the game's ID + 0x1000 + its number
    return (tid & ~0xFFF) - 0x1000 if (tid >> 12) & 1 else tid & ~0xFFF

def dlc_marked(name):   # a folder named after a DLC: a title ID in its name, or a plain number (the app's own aoc/<n>)
    return bool(TID_RE.search(name)) or (name.isdigit() and 1 <= int(name) <= 2000)

def has_files(d):
    return any(fs for _, _, fs in os.walk(d))

def dlc_folders(paths):
    """The DLC folders the player chose, each one DLC's RomFS. One pick can also be a folder of DLC folders: a dump's DLC/ (or aoc/) folder, or the dump's parent
    folder holding it (exefs/, romfs/, DLC/<title ID>/romfs, as one of the PC dumps here is laid out)"""
    out = []
    for p in paths:
        p = os.path.normpath(p)
        if not os.path.isdir(p): raise RuntimeError(f'the DLC folder {p} was not found: choose it again')
        holder = child_dir(p, 'dlc') or child_dir(p, 'aoc')
        if holder: out += [e.path for e in sorted(os.scandir(holder), key=lambda e: e.name) if e.is_dir() and has_files(e.path)]; continue   # (an empty one: no DLC there)
        subs = [e.path for e in sorted(os.scandir(p), key=lambda e: e.name) if e.is_dir()]
        if subs and not child_dir(p, 'romfs') and not dlc_marked(os.path.basename(p)) and all(dlc_marked(os.path.basename(s)) for s in subs): out += [s for s in subs if has_files(s)]   # (the DLC/ folder itself)
        else: out.append(p)
    return out

def dlc_index(path, base_tid, name):
    """aoc/<n>, the index the game asks the add-on content service for (hle_misc.c lists aoc/<n>, hle_fs.c mounts it). nx_extract took it from the DLC's title ID: the
    base game's + 0x1000 + n (Shield's 01008DB008C2D001 → 1, ...D002 → 2; the index is the ID's low 11 bits, 0 taken as 1). A folder holds no title ID, so it comes
    from the names: the folder's own and up to three parents', the deepest first (dump tools name their output after the title ID: DLC/0100A3D008C5D001/romfs,
    "Pokemon Shield [01008DB008C2D002][v0]/romfs"); a name with the base game's or its update's ID says nothing and the search goes on up. A folder named with a
    plain number (1, 2: the app's own aoc/<n>, or the player's naming; its parent's name when the folder itself is romfs) is that index. The order the folders were
    chosen in is never used: a player with only the second DLC must get index 2, which a count would make 1. → (index, title ID or '')"""
    parts = os.path.normpath(os.path.abspath(path)).split(os.sep)
    own = parts[-2] if parts[-1].lower() == 'romfs' and len(parts) > 1 else parts[-1]
    for comp in reversed(parts[-4:]):
        for m in TID_RE.finditer(comp):
            t = int(m.group(1), 16)
            if app_of(t) != base_tid:
                other = _game.by_title(app_of(t))
                raise RuntimeError(f'the DLC folder {path} is named after a title of {other.get("name") if other else "another game"} ({t:016X}), not of {name}')
            if 0x1000 <= t - base_tid <= 0x17ff: return (t & 0x7ff) or 1, f'{t:016X}'
    if own.isdigit() and 1 <= int(own) <= 2000: return int(own), ''
    raise RuntimeError(f'which DLC {path} is cannot be told from its name: rename its folder to the DLC\'s title ID ({base_tid + 0x1001:016X} for the first DLC of {name}, '
                       f'{base_tid + 0x1002:016X} for the second) or to its number (1, 2), then choose it again')

def npdm_title(exefs):
    """The program's title ID from main.npdm (META; its ACI0 at the offset in 0x70, the program ID at ACI0 + 0x10); 0 when there is none"""
    try:
        with open(os.path.join(exefs, 'main.npdm'), 'rb') as f: b = f.read(0x10000)
        if b[:4] != b'META': return 0
        o = struct.unpack_from('<I', b, 0x70)[0]
        return struct.unpack_from('<Q', b, o + 0x10)[0] if b[o:o + 4] == b'ACI0' else 0
    except (OSError, struct.error): return 0

def identify(exefs, games):
    """Which game and version the ExeFS is, from main's build ID (extracted folders have no control.nacp): the profiles' build rows (dm/games/*/game.tsv) are the
    versions the apps run, their other_build rows versions they do not. games: the app's games (its own first); empty: the profile's game (DM_GAME) only.
    → (the game's profile id, build ID); otherwise a RuntimeError says what the files are"""
    with open(os.path.join(exefs, 'main'), 'rb') as f: h = f.read(0x50)
    if len(h) < 0x50 or h[:4] != b'NSO0':
        raise RuntimeError(f'{os.path.join(exefs, "main")} is not a Switch program (NSO0): choose the ExeFS folder as it was extracted, not a decompressed or patched copy')
    bid = h[0x40:0x50].hex()
    want = [g.upper() for g in games] or [GAME.upper()]
    named = lambda ids: ' and '.join(_game.load(g).get('name') for g in ids)
    need = lambda g: ' or '.join(sorted({b['display'] for b in g.builds().values()}))
    for gid in want:
        if bid in {b.get('main') for b in _game.load(gid).builds().values()}: return gid, bid
    profiles = _game.all_games()
    for g in profiles:   # a version another app runs (Legends: Arceus files chosen in Galar Native)
        for b in g.builds().values():
            if b.get('main') == bid: raise RuntimeError(f'your files are {g.get("name")} {b["display"]}: this app runs {named(want)}')
    for g in profiles:   # a version of a game the profiles know but no app runs (most often the base game without its update)
        for disp, *mods in g.values('other_build'):
            if dict(m.split('=', 1) for m in mods if '=' in m).get('main') != bid: continue
            if g.id.upper() in want:
                raise RuntimeError(f'your files are {g.get("name")} {disp}, and this app needs update {need(g)}: extract ExeFS and RomFS again with the {need(g)} update applied')
            raise RuntimeError(f'your files are {g.get("name")} {disp}: this app runs {named(want)}')
    tid = npdm_title(exefs)
    own = next((gid for gid in want if int(gid, 16) == tid), None) if tid else None
    if own:
        g = _game.load(own)
        raise RuntimeError(f'your files are {g.get("name")} of a version this app does not know (main build ID {bid}): this app needs update {need(g)}. '
                           f'Extract ExeFS and RomFS again with the {need(g)} update applied')
    if tid:
        other = _game.by_title(tid)
        raise RuntimeError(f'your files are {other.get("name") if other else "another game"} ({tid:016X}): this app runs {named(want)}')
    raise RuntimeError(f'your ExeFS is of an unknown game or version (main build ID {bid}): this app runs ' +
                       ', '.join(f'{_game.load(g).get("name")} {need(_game.load(g))}' for g in want))

def tree_size(d):
    n = 0
    for r, _, fs in os.walk(d):
        for f in fs:
            try: n += os.path.getsize(os.path.join(r, f))
            except OSError: pass
    return n

def copy_tree(src, dst, prog):
    """src's files into dst (made). prog = [bytes done, bytes in all, percent shown]: a 'PROGRESS done/total' line at each new percent"""
    for r, ds, fs in os.walk(src):
        ds.sort(); t = os.path.join(dst, os.path.relpath(r, src)); os.makedirs(t, exist_ok=True)
        for f in sorted(fs):
            s = os.path.join(r, f); shutil.copyfile(s, os.path.join(t, f)); prog[0] += os.path.getsize(s)
            pct = prog[0] * 100 // max(prog[1], 1)
            if pct != prog[2]: prog[2] = pct; print(f'PROGRESS {min(prog[0], prog[1])}/{max(prog[1], 1)}', flush=True)

def same_storage(a, b):
    try: return os.stat(a).st_dev == os.stat(b).st_dev
    except OSError: return False

def moved_mark(work): return os.path.join(work, 'moved.json')

def restore_moved(work, log=None):
    """The player's folders a setup moved into the game's folder (--move) go back where they were when that setup did not finish: on its failure, or at the start
    of the next full setup when Android stopped it and it was not resumed. A rename back, nothing copied"""
    import json
    try: moved = json.load(open(moved_mark(work), encoding='utf-8'))
    except (OSError, ValueError): return
    for src, dst in reversed(moved):
        if os.path.isdir(dst) and not os.path.exists(src):
            try:
                os.makedirs(os.path.dirname(src), exist_ok=True); os.replace(dst, src)
                if log: log.write(f'moved back: {dst} → {src}\n')
            except OSError as e:
                if log: log.write(f'could not move {dst} back to {src}: {e}\n')
    os.remove(moved_mark(work))

def note_moved(work, src, dst):
    import json
    try: moved = json.load(open(moved_mark(work), encoding='utf-8'))
    except (OSError, ValueError): moved = []
    moved.append([src, dst]); json.dump(moved, open(moved_mark(work), 'w', encoding='utf-8'))

def main():
    global PROFILE, NAME   # (the dump can name another game of the app: --games)
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', help='the ExeFS folder extracted from the game of the player, with the update applied (main, main.npdm, rtld, sdk, subsdk*), or the folder holding exefs/')
    ap.add_argument('--romfs', help='the RomFS folder extracted with the update applied, or the folder holding romfs/')
    ap.add_argument('--dlc-dir', action='append', default=[], help='a DLC RomFS folder (or a folder of them), named after the DLC title ID or its number (dlc_index)')
    ap.add_argument('--move', action='store_true', help='with too little free storage for a copy: move the RomFS and DLC folders into the game folder instead (the player agreed: the setup screen asks)')
    ap.add_argument('--captures')
    ap.add_argument('--out', required=True); ap.add_argument('--work', required=True); ap.add_argument('--bin', required=True)
    ap.add_argument('--ext-font'); ap.add_argument('--std-font'); ap.add_argument('--games'); ap.add_argument('--root')
    ap.add_argument('--live', help='with --captures, while the game runs: the translated shaders also go into this folder for the engine to load at once (nvn_vk.c live_check)')
    ap.add_argument('--mod-shaders', help="the RomFS folders of the mods that are on, ':'-separated: their new shaders translated into the pack")
    ap.add_argument('--shaders-only', action='store_true', help='translate the shaders again from the game already set up in --out (its RomFS and patched/main.patched.bin): '
                    'no game files chosen, for an app version whose shader translation changed')
    ap.add_argument('--scratch', help='the folder for the shader steps\' many small files (the scan\'s packages, the translation, its cache, the reflection, the raw pack): the app\'s private storage, where a file costs a fraction of what it does in shared storage (#262: 10,000 small files written in 0.5 s there, 26 s in /sdcard); default --work')
    ap.add_argument('--installed', help='the folder of an installed app\'s APKs that already hold this game (assets/game: RomFS and the rewritten code; the Legends: Arceus app built on a PC): set up from them instead of a dump')
    ap.add_argument('--flat-normals', action='store_true', help='only make the profile\'s flat normal maps (flat_normals: Legends: Arceus "No reflections") for the game set up in --out')
    ap.add_argument('--resume', action='store_true', help='go on from the shader translation when an earlier run of this setup got that far (resume.json in --work)')
    a = ap.parse_args()
    if a.mod_shaders is not None: return mod_shaders(a)
    if a.flat_normals: return flat_normals(a)
    if a.games and not a.root: ap.error('--games needs --root')
    if a.captures:
        if a.live: live_yield()
        return captures(a)
    if not a.shaders_only and not (a.installed or (a.exefs and a.romfs)): ap.error('--exefs and --romfs are needed (or --installed, --captures, or --shaders-only)')
    os.makedirs(a.work, exist_ok=True); os.makedirs(a.out, exist_ok=True)
    S = a.scratch or a.work; os.makedirs(S, exist_ok=True)   # the shader steps' small files (--scratch); setup.log, the unpacked game and the marks stay in --work
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1)
    py = sys.executable; env = dict(os.environ, PYTHONUNBUFFERED='1')
    env['PATH'] = a.bin + os.pathsep + env.get('PATH', '')
    tool = lambda n: os.path.join(a.bin, n)
    steps = ['copy the game files', 'rewrite the code', 'find the shaders', 'translate the shaders', 'pack the shaders', 'finish']
    def step(i, extra=''):
        print(f'STEP {i}/{len(steps)} {steps[i - 1]}{extra}', flush=True); log.write(f'== {time.strftime("%H:%M:%S")} {steps[i - 1]}{extra}\n')
    def run(cmd, retry_killed=0, **kw):
        # retry_killed: the shader translation, which Android's low-memory killer can stop as a whole (#51: both setups ended in it with no failure line): it is run
        #   again that many times, with half the translators each time; what it translated is kept in its cache, so a retry goes on where it stopped
        e = kw.pop('env', env)
        for attempt in range(retry_killed + 1):
            log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
            r = subprocess.run(list(map(str, cmd)), cwd=a.bin, stdout=log, stderr=subprocess.STDOUT, env=e, **kw)
            if r.returncode >= 0 or attempt == retry_killed: break
            slots = max(1, int(e.get('DM_GATE_SLOTS', e.get('J', '8'))) // 2); e = dict(e, J=str(slots), DM_GATE_SLOTS=str(slots))
            log.write(f'{os.path.basename(str(cmd[1]))} was stopped ({r.returncode}): again with {slots} at a time\n')
        if r.returncode:
            name = os.path.basename(str(cmd[1] if str(cmd[0]) == py else cmd[0]))
            if r.returncode < 0:
                try: reason = signal.Signals(-r.returncode).name
                except ValueError: reason = f'signal {-r.returncode}'
                raise RuntimeError(f'{name} terminated by {reason} ({r.returncode}); see {log.name}')
            raise RuntimeError(f'{name} failed ({r.returncode}); see {log.name}')
    def write_translated(work, chain):   # the translation is done: a fresh setup of the same dump keeps it (translation_kept)
        import json
        json.dump({'chain': chain, 'found': found_digest(work)}, open(translated_mark(work), 'w', encoding='utf-8'))
    def reflect(vk, built):
        # the replay's reflection (a .vkr for each translated shader), read straight from the shader build's folder (built: its <key>_b.json). Copying those files
        #   (38,500 for Sword) into vk first took hours on a phone's shared storage, with nothing on the screen (#100: the setup sat after the translation was done)
        nspv = sum(f.endswith('.spv') and not f.endswith('.gl.spv') for f in os.listdir(vk))
        log.write(f'reflection for {nspv} shaders\n')
        stop5 = threading.Event(); threading.Thread(target=watch_files, args=(vk, '.vkr', max(nspv, 1), stop5), daemon=True).start()
        try: run([py, os.path.join(DM, 'android/tools/vk_meta.py'), built, vk, built])
        finally: stop5.set()
    ex = os.path.join(a.work, 'extract')

    # --resume (the setup screen, after Android stopped this setup, #51 / #52: exit 137 during the shader translation): resume.json, written once the shader scan is done,
    #   says what steps 1-3 found; the unpacked game, the rewritten code and the scanned shaders are still in place, and the translation goes on from what it had done
    state = os.path.join(a.work, 'resume.json'); resumed = False; from_app = False
    if a.resume and os.path.isfile(state) and (a.shaders_only or os.path.isfile(os.path.join(ex, 'ExeFS', 'main'))) and os.path.isdir(os.path.join(S, 'shader_pkg')):
        import json
        st = json.load(open(state, encoding='utf-8'))
        a.out = st['out']; games = st['games']; files = st['files']
        if st.get('game'):
            PROFILE = _game.load(st['game']); NAME = PROFILE.get('name'); env['DM_GAME'] = os.environ['DM_GAME'] = st['game']
            print(f'GAME {st["game"].upper()}', flush=True)
        exefs, romfs, pkg, pat = os.path.join(ex, 'ExeFS'), st.get('romfs') or os.path.join(ex, 'RomFS'), os.path.join(S, 'shader_pkg'), os.path.join(a.out, 'patched')
        if a.shaders_only: romfs = os.path.join(a.out, 'RomFS')   # (a shaders-only update reads the game's own RomFS: nothing was copied)
        resumed = True; log.write(f'== {time.strftime("%H:%M:%S")} resumed after a stop: steps 1-3 kept, the translation goes on\n')
    # --shaders-only: an app version whose shader translation changed needs only steps 3-6. The game set up before already holds what they read: RomFS/ (moved there
    #   at the end of that setup) and the rewritten main (patched/main.patched.bin: the decompressed image with only its code rewritten, so the shaders embedded in its
    #   data are the game's own). Nothing chosen, nothing copied: the player is not asked for anything (before, every such update meant choosing the files and
    #   unpacking 12 GB again). The old pack and its ready file stay until the new one is complete
    main_src = None
    if not resumed and a.shaders_only:
        files, games = [], []
        romfs, exefs = os.path.join(a.out, 'RomFS'), os.path.join(ex, 'ExeFS')
        main_src, pkg = os.path.join(a.out, 'patched', 'main.patched.bin'), os.path.join(S, 'shader_pkg')
        pat = os.path.join(a.out, 'patched')   # (the Trinity games' shader scan runs the rewritten modules: step 3)
        if not (os.path.isdir(romfs) and os.path.isfile(main_src)):
            raise RuntimeError('this game was set up without the files a shaders-only update reads (RomFS, patched/main.patched.bin): choose your files and set it up again')
        log.write(f'== {time.strftime("%H:%M:%S")} shaders only: from {romfs} and {main_src}\n')
    elif not resumed and a.installed:
        # --installed: the game from an app that already holds it (the Legends: Arceus app built on a PC: its APKs carry assets/game/RomFS and assets/game/patched, the code
        #   rewritten by this same patch_nso.py). Steps 1 and 2 take them from there; the shaders are scanned and translated as for a dump (the old app's pack is another chain)
        import zipfile
        step(1, ' (from the installed app)')
        apks = sorted(os.path.join(a.installed, f) for f in os.listdir(a.installed) if f.endswith('.apk'))
        if not apks: raise RuntimeError(f'no APKs in {a.installed}')
        if os.path.isdir(ex): shutil.rmtree(ex)
        romfs = os.path.join(ex, 'RomFS'); exefs = os.path.join(ex, 'ExeFS'); files = apks; games = [g for g in (a.games or '').split(',') if g]; aoc = None
        zs = [zipfile.ZipFile(f) for f in apks]
        # its RomFS is in two trees: assets/game/RomFS (files kept as they are) and assets/gamez/RomFS (files that compress, in the app's block format: "NXZ1", u32 block
        #   size, u64 size, u32 block count, u32 block offsets [count + 1] from the end of the table, then the blocks: zlib, or raw when stored at their full size;
        #   that app's compat/apkfs.c, dm/android/tools/nxz_pack.py there). Most of the game's shader files (.bnsh, .ptcl) are in the second
        RP, ZP, PP = 'assets/game/RomFS/', 'assets/gamez/RomFS/', 'assets/game/patched/'
        mem = [(z, i, False) for z in zs for i in z.infolist() if i.filename.startswith(RP) and not i.is_dir()] + \
              [(z, i, True) for z in zs for i in z.infolist() if i.filename.startswith(ZP) and not i.is_dir()]
        def nxz_size(z, i):
            with z.open(i) as f: h = f.read(20)
            return struct.unpack_from('<Q', h, 8)[0] if h[:4] == b'NXZ1' else i.file_size
        need = sum(nxz_size(z, i) if c else i.file_size for z, i, c in mem) + (1 << 30); free = shutil.disk_usage(a.work).free
        log.write(f'installed app: {len(apks)} APKs, {len(mem)} RomFS files ({need >> 20} MB with room for the shaders), {free >> 20} MB free\n')
        if not mem: raise RuntimeError('the installed app holds no game files (assets/game/RomFS)')
        if free < need: raise RuntimeError(f'not enough free storage: the setup needs about {need / (1 << 30):.1f} GB free and the phone has {free / (1 << 30):.1f} GB')
        cuts = next((z.read(PP + 'main.cuts.tsv').decode() for z in zs if PP + 'main.cuts.tsv' in z.namelist()), '')
        build_id = next((l.split('\t')[1].strip() for l in cuts.splitlines() if l.startswith('# BUILD_ID')), '')
        builds = PROFILE.builds()
        if build_id not in {b.get('main') for b in builds.values()}:
            raise RuntimeError(f'the installed app holds {NAME} of another version (main {build_id or "unknown"}): this app needs update ' + ', '.join(sorted(b['display'] for b in builds.values())))
        try: os.remove(os.path.join(a.out, 'ready'))
        except FileNotFoundError: pass
        done = 0
        import zlib
        for z, i, comp in mem:
            dst = os.path.join(romfs, i.filename[len(ZP if comp else RP):]); os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(i) as src, open(dst, 'wb') as out:
                if not comp: shutil.copyfileobj(src, out, 1 << 22)
                else:
                    h = src.read(20)
                    if h[:4] != b'NXZ1': raise RuntimeError(f'{i.filename} is not in the block format')
                    bs, size, n = struct.unpack_from('<IQI', h, 4); offs = struct.unpack(f'<{n + 1}I', src.read(4 * (n + 1)))
                    for k in range(n):
                        blk = src.read(offs[k + 1] - offs[k]); rawn = min(bs, size - k * bs)
                        out.write(blk if len(blk) == rawn else zlib.decompress(blk))
                    if out.tell() != size: raise RuntimeError(f'{i.filename}: {out.tell()} bytes unpacked, not {size}')
            done += 1
            if done % 200 == 0: print(f'PROGRESS {done}/{len(mem)}', flush=True)
        step(2, ' (from the installed app)')
        pat = os.path.join(a.out, 'patched'); shutil.rmtree(pat, ignore_errors=True); os.makedirs(pat)
        for z in zs:
            for i in z.infolist():
                if i.filename.startswith(PP) and not i.is_dir():
                    with z.open(i) as src, open(os.path.join(pat, i.filename[len(PP):]), 'wb') as out: shutil.copyfileobj(src, out, 1 << 22)
        if not all(os.path.isfile(os.path.join(pat, m + '.patched.bin')) for m in PROFILE.words('modules')): raise RuntimeError('the installed app lacks some of the rewritten code (assets/game/patched)')
        log.write(f'from the installed app: main {build_id}, rewritten code {sorted(os.listdir(pat))[:3]}...\n')
        step(3)
        pkg = os.path.join(S, 'shader_pkg'); shutil.rmtree(pkg, ignore_errors=True)
        if PROFILE.get('shader_oodle', 'off') == 'on':
            if not os.path.isfile(tool('dmoodle')): raise RuntimeError(f'{NAME} needs the dmoodle tool, which this app build does not have')
            env.update(DM_OODLE_HOST=tool('dmoodle'), DM_OODLE_MODULES=pat, DM_OODLE_FILTER='BNSH')
        run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), romfs, '-', pkg,   # ('-': no main; this game has no shaders in it)
             os.path.join(a.work, 'shader_map.found.tsv'), os.path.join(a.work, 'shader_facts.found.tsv'), os.path.join(a.work, 'shader_pairs.found.tsv')],
            env=dict(env, DM_NVN_CAPTURE=os.path.join(a.out, 'nvn_capture')))
        from_app = True   # (steps 1-3 done: on to the translation)
    elif not resumed:
        step(1)
        restore_moved(a.work, log)   # (a moved setup Android stopped and nobody resumed: the player's folders go back first, so they can be chosen again)
        if os.path.isdir(ex): shutil.rmtree(ex)
        exefs_src, romfs_src = resolve_exefs(a.exefs), resolve_romfs(a.romfs)
        log.write(f'ExeFS: {exefs_src}\nRomFS: {romfs_src}\n' + ''.join(f'DLC: {d}\n' for d in a.dlc_dir))
        # which game: main's build ID against the profiles (identify), checked now rather than as a failure ten minutes later in the shader steps. --games (the app's
        # games, its own first) and --root (the app's folder): the game is set up in its own place, the first game in --root, another in --root/<title ID>, replacing
        # what was there (one of each game). Without --games, the profile's game only
        games = [g for g in (a.games or '').split(',') if g]
        gid, build_id = identify(exefs_src, games)
        log.write(f'main build ID {build_id}: {_game.load(gid).get("name")}\n')
        if games:
            hit = next(i for i, g in enumerate(games) if g.upper() == gid)
            PROFILE = _game.load(games[hit]); NAME = PROFILE.get('name'); env['DM_GAME'] = os.environ['DM_GAME'] = games[hit]
            out = a.root if hit == 0 else os.path.join(a.root, games[hit].upper())
            if os.path.realpath(out) != os.path.realpath(a.out):
                os.makedirs(out, exist_ok=True); a.out = out
            log.write(f'the files are {NAME}: set up in {a.out}\n'); print(f'GAME {games[hit].upper()}', flush=True)
        dlcs = {}
        for d in dlc_folders(a.dlc_dir):
            n, tid = dlc_index(d, PROFILE.title_id, NAME); d = resolve_romfs(d)
            if n in dlcs: raise RuntimeError(f'two DLC folders are DLC {n}: {dlcs[n]} and {d}. Choose each DLC once')
            dlcs[n] = d; log.write(f'DLC {n}{" (" + tid + ")" if tid else ""}: {d}\n')
        files = [exefs_src, romfs_src] + [dlcs[n] for n in sorted(dlcs)]
        rom_out, aoc = os.path.join(a.out, 'RomFS'), os.path.join(a.out, 'aoc')
        in_place = os.path.realpath(romfs_src) == os.path.realpath(rom_out)   # (the game's own RomFS from an earlier setup chosen again: nothing to copy)
        # room for the copy and the shaders, said now, not as a failure halfway through: the folders' sizes. An earlier setup's RomFS and DLC of this game, about to
        # be replaced, count as room. Too little: a move instead of a copy (the player's folders become the game's, one rename each) when they are on the same
        # storage and the player agreed (--move; the setup screen asks after the OFFER line), else what to free
        sizes = {'ExeFS': tree_size(exefs_src), 'RomFS': 0 if in_place else tree_size(romfs_src), 'DLC': sum(tree_size(d) for d in dlcs.values())}
        need = sum(sizes.values()) + (1 << 30)
        try: free = shutil.disk_usage(a.work).free
        except OSError as e: free = None; log.write(f'free-space check failed: {e}\n')
        old = (0 if in_place else tree_size(rom_out)) + (tree_size(aoc) if dlcs else 0)
        log.write(f'sizes: ' + ', '.join(f'{k} {v >> 20} MB' for k, v in sizes.items()) + f'; free {free if free is None else free >> 20} MB, the old game files {old >> 20} MB\n')
        move = False
        if free is not None and free + old < need:
            movable = all(same_storage(s, a.out) for s in [romfs_src, *dlcs.values()])
            if movable and a.move and free + old >= sizes['ExeFS'] + (1 << 30): move = True
            else:
                if movable and not a.move: print(f'OFFER move {need} {free + old}', flush=True)   # the setup screen offers to move the folders instead
                raise RuntimeError(f'not enough free storage to copy your game files: the setup needs about {need / (1 << 30):.1f} GB free and the phone has '
                                   f'{(free + old) / (1 << 30):.1f} GB. Free some space, then set up again')
        # from here the game's folder changes: it is not ready until this setup finishes (a setup stopped halfway left the old ready file, and Play then started a game
        # with its rewritten code deleted)
        try: os.remove(os.path.join(a.out, 'ready'))
        except FileNotFoundError: pass
        if free is not None and free < need and old:
            log.write(f'too little room beside the old game files: they go first ({old >> 20} MB)\n')
            if not in_place: shutil.rmtree(rom_out, ignore_errors=True)
            if dlcs: shutil.rmtree(aoc, ignore_errors=True)
        exefs = os.path.join(ex, 'ExeFS'); prog = [0, sizes['ExeFS'] + (0 if move else sizes['RomFS'] + sizes['DLC']), -1]
        copy_tree(exefs_src, exefs, prog)
        if dlcs: shutil.rmtree(aoc, ignore_errors=True); os.makedirs(aoc)   # the DLC chosen replace the game's; none chosen: the game keeps the DLC it had
        if move:
            # one rename per folder: RomFS straight into the game's folder (setup_work's leftovers are deleted by the app, the game folder's RomFS is not); noted in
            # moved.json, so a failure moves them back (restore_moved)
            print('MOVING', flush=True); log.write('moving the RomFS and DLC folders into the game folder (--move)\n')
            if not in_place: shutil.rmtree(rom_out, ignore_errors=True)
            for s, d in ([] if in_place else [(romfs_src, rom_out)]) + [(dlcs[n], os.path.join(aoc, str(n))) for n in sorted(dlcs)]:
                try: os.replace(s, d)
                except OSError as e: raise RuntimeError(f'{s} could not be moved ({e.strerror}): free some space so it can be copied instead')
                note_moved(a.work, s, d); MOVED_WORK.append(a.work)
            romfs = rom_out
        else:
            romfs = rom_out if in_place else os.path.join(ex, 'RomFS')
            if not in_place: copy_tree(romfs_src, romfs, prog)
            for n in sorted(dlcs): copy_tree(dlcs[n], os.path.join(aoc, str(n)), prog)
        log.write(f'copied: {prog[0] >> 20} MB\n')

        step(2)
        pat = os.path.join(a.out, 'patched'); shutil.rmtree(pat, ignore_errors=True); os.makedirs(pat)
        for m in PROFILE.words('modules'):
            if os.path.isfile(os.path.join(exefs, m)):
                run([py, os.path.join(DM, 'tools/patch_nso.py'), os.path.join(exefs, m), pat, m], env=dict(env, DM_PATCH_TARGET='android'))

    if not resumed:
        step(3)
        pkg = os.path.join(S, 'shader_pkg'); shutil.rmtree(pkg, ignore_errors=True)
        # the shaders an earlier run of the game captured (nvn_capture/ in the game dir, android_main.c DM_NVN_CAPTURE) go into the new pack too: the game saves each
        # one only once, so a pack rebuilt without them would never get them back
        # Trinity games (Violet: every file of arc/data.trpfs is Oodle-compressed): the scan decodes them with the game's own decoder, dmoodle running the rewritten
        # modules of step 2, and only the files that hold shaders come back over the pipe (hle_oodle.c DM_OODLE_SERVE=BNSH)
        if PROFILE.get('shader_oodle', 'off') == 'on':
            if not os.path.isfile(tool('dmoodle')): raise RuntimeError(f'{NAME} needs the dmoodle tool, which this app build does not have')
            env.update(DM_OODLE_HOST=tool('dmoodle'), DM_OODLE_MODULES=pat, DM_OODLE_FILTER='BNSH')
        run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), romfs, main_src or os.path.join(exefs, 'main'), pkg,
             os.path.join(a.work, 'shader_map.found.tsv'), os.path.join(a.work, 'shader_facts.found.tsv'), os.path.join(a.work, 'shader_pairs.found.tsv')],
            env=dict(env, DM_NVN_CAPTURE=os.path.join(a.out, 'nvn_capture')))

        import json
        json.dump({'out': a.out, 'game': env.get('DM_GAME'), 'games': games, 'files': files, 'romfs': romfs}, open(state, 'w', encoding='utf-8'))
    step(4)
    chain = subprocess.run([py, chain_tool('dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    # The other game of the pair already set up with the same translation chain (Scarlet and Violet share every one of their 35,937 shaders; Sword and Shield most of
    # theirs): its pack is linked (or copied) in, and only the shaders it lacks are translated and merged into it, instead of the hours of translating them all
    found_keys = {bytes.fromhex(c[3][:16]) for c in (l.rstrip('\n').split('\t') for l in open(os.path.join(a.work, 'shader_map.found.tsv'), encoding='utf-8'))
                  if len(c) > 3 and len(c[3]) >= 16 and not c[0].startswith('#') and c[0] != 'package'}
    donor = None
    for i, g in enumerate(games):
        d = a.root if i == 0 else os.path.join(a.root, g.upper())
        dp = os.path.join(d, 'shaders.pak')
        if os.path.realpath(d) == os.path.realpath(a.out) or not os.path.isfile(dp) or not os.path.isfile(os.path.join(d, 'ready')): continue
        try: have, pchain = pak_keys(dp)
        except (OSError, RuntimeError, struct.error): continue
        if pchain == chain and len(found_keys & have) > len(found_keys) // 2: donor = (dp, have); break
    if donor:
        dp, have = donor; missing = sorted(k.hex() for k in found_keys - have)
        log.write(f'shaders: {len(found_keys) - len(missing)} of {len(found_keys)} taken from {dp}, {len(missing)} to translate\n')
        print(f'STEP 4/6 translate the shaders (reusing {len(found_keys) - len(missing)} from your other game)', flush=True)
        target = os.path.join(a.out, 'shaders.pak'); tmp = target + '.part'
        for f in (tmp, target):
            if os.path.exists(f): os.remove(f)
        try: os.link(dp, tmp)   # (no extra space: a later merge into either game writes a new file and leaves the other's alone)
        except OSError:   # (a link made but reported failed would make the copy below 'the same file': #320)
            if os.path.lexists(tmp): os.remove(tmp)
            shutil.copyfile(dp, tmp)
        if missing:
            vk = os.path.join(S, 'vk', 'b-' + chain); cache = os.path.join(S, 'shader_cache')
            kept = not resumed and translation_kept(a.work, chain)
            if kept: log.write('the shaders the last setup of this dump translated are kept: only what is missing is translated\n')
            if not resumed and not kept: shutil.rmtree(os.path.dirname(vk), ignore_errors=True); shutil.rmtree(cache, ignore_errors=True)
            os.makedirs(vk, exist_ok=True)
            gate = os.path.isfile(tool('dmsh_gate')) and os.path.isfile('/proc/meminfo')
            xenv = dict(XLATE=tool('dmsh_gate'), DM_GATE_EXEC=tool('dmsh_xlate'), DM_GATE_SLOTS=env.get('J', '8'), DM_GATE_DIR=S) if gate else dict(XLATE=tool('dmsh_xlate'))
            run([py, chain_tool('dmsh_build.py'), pkg, cache, *missing], retry_killed=3,
                env=dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, **xenv, DMSH_CAS=os.path.join(S, 'cas'), WORK=S,
                         DMSH_SOURCE_MAP=os.path.join(a.work, 'shader_map.found.tsv'), DMSH_FACTS=os.path.join(a.work, 'shader_facts.found.tsv'),
                         DMSH_PAIRS=os.path.join(a.work, 'shader_pairs.found.tsv'), MAIN=os.path.join(exefs, 'main')))
            built = os.path.join(cache, 'b-' + chain)
            write_translated(a.work, chain)
            step(5)
            reflect(vk, built)
            small = os.path.join(S, 'shaders.new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
            pak_merge(tmp, small); os.remove(small)
        else: step(5)
        os.replace(tmp, target)
    else:
        vk = os.path.join(S, 'vk', 'b-' + chain); cache = os.path.join(S, 'shader_cache')
        kept = not resumed and translation_kept(a.work, chain)
        if kept: log.write('the shaders the last setup of this dump translated are kept: only what is missing is translated\n')
        if not resumed and not kept:
            shutil.rmtree(os.path.dirname(vk), ignore_errors=True); shutil.rmtree(cache, ignore_errors=True)
        if not resumed:
            for d in glob.glob(os.path.join(S, 'dmsh_*')): shutil.rmtree(d, ignore_errors=True)   # an interrupted run's scratch dir would count toward the progress
        os.makedirs(vk, exist_ok=True)
        stop = threading.Event(); found = os.path.join(a.work, 'shader_map.found.tsv'); same = {}
        # each distinct program translated once (distinct_shaders), with DM_SETUP_DEDUP=1 only: OFF by default. It gives the same pack, but a full PC setup of Shield
        #   (22 translators, 2026-11-22) took 75 min with it against 7 min without, twice in either order; why the shader build is that much slower on the shorter
        #   map is not known yet. Every shader is translated for itself, as before, until that is found
        if os.environ.get('DM_SETUP_DEDUP', '0') == '1':
            uniq = os.path.join(a.work, 'shader_map.distinct.tsv')
            try:
                over = PROFILE.table('msl_override')
                same = distinct_shaders(found, pkg, [str(PROFILE.table('shader_facts.tsv')), os.path.join(a.work, 'shader_facts.found.tsv')],
                                        {f.name[:-len('_b.metal')] for f in over.glob('*_b.metal')} if over.is_dir() else set(), uniq)
            except (OSError, ValueError) as e: log.write(f'shaders: the repeats could not be found ({e}): every shader is translated\n'); same = {}
            if same: log.write(f'shaders: {len(same)} are repeats of another one, {len(found_keys) - len(same)} to translate\n'); log.flush(); found = uniq
        threading.Thread(target=watch_translation, args=(found, S, os.path.join(cache, 'b-' + chain), stop), daemon=True).start()
        # the translators go through dmsh_gate when the app has it: each beyond the first starts only while the phone has the memory for it (dmsh_gate.c)
        gate = os.path.isfile(tool('dmsh_gate')) and os.path.isfile('/proc/meminfo')
        xenv = dict(XLATE=tool('dmsh_gate'), DM_GATE_EXEC=tool('dmsh_xlate'), DM_GATE_SLOTS=env.get('J', '8'), DM_GATE_DIR=S) if gate else dict(XLATE=tool('dmsh_xlate'))
        log.write(f'translators: {env.get("J", "8")} at most, ' + ('started as memory allows (dmsh_gate)\n' if gate else 'all at once (no dmsh_gate)\n'))
        try: run([py, chain_tool('dmsh_build.py'), pkg, cache], retry_killed=3,
            env=dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, **xenv, DMSH_CAS=os.path.join(S, 'cas'), WORK=S,
                     DMSH_SOURCE_MAP=found,
                     DMSH_FACTS=os.path.join(a.work, 'shader_facts.found.tsv'),
                     DMSH_PAIRS=os.path.join(a.work, 'shader_pairs.found.tsv'),
                     MAIN=os.path.join(exefs, 'main')))
        finally: stop.set()
        built = os.path.join(cache, 'b-' + chain)
        # No shader came out (every one failed in the shader build, e.g. spirv-cross could not be started): the setup stops here. It used to go on, write a pack
        #   of 0 shaders over the player's working one and mark the game ready (seen on the Android emulator, 2026-11-21)
        if not any(f.endswith('.spv') and not f.endswith('.gl.spv') for f in os.listdir(vk)):
            raise RuntimeError(f'the shader translation produced no shaders (its last lines are in {log.name}); the shader pack was left as it was')
        write_translated(a.work, chain)

        step(5)
        reflect(vk, built)
        if same: log.write(f'shaders: {share_translated(vk, same)} repeats take their translated shader\'s files\n'); log.flush()
        # the pack compressed (zlib per shader, the engine inflates each as it loads it: nvn_vk.c pak_blob): about 3.4 times smaller on the phone (Violet 2.9 GB → 0.85 GB, Sword 2.3 → 0.7)
        raw = os.path.join(S, 'shaders.raw.pak')
        run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, raw])
        part = os.path.join(a.out, 'shaders.pak.part')   # written beside it, then swapped in: a setup stopped while writing leaves the old pack whole
        run([py, os.path.join(DM, 'android/tools/spv_pak.py'), '--compress', raw, part]); os.replace(part, os.path.join(a.out, 'shaders.pak'))
        os.remove(raw)

    step(6)
    write_runtime_shader_map(os.path.join(a.work, 'shader_map.found.tsv'),
                             str(PROFILE.table('shader_map.tsv')),
                             os.path.join(a.out, 'shader_map.tsv'))
    font = os.path.join(a.out, 'font'); os.makedirs(font, exist_ok=True)
    std = a.std_font or next((f for f in ('/system/fonts/Roboto-Regular.ttf', '/system/fonts/NotoSans-Regular.ttf') if os.path.isfile(f)), None)
    if std: shutil.copyfile(std, os.path.join(font, 'standard.ttf'))
    if a.ext_font: shutil.copyfile(a.ext_font, os.path.join(font, 'nintendo_ext.ttf'))
    # (no icon.jpg any more: extracted folders have no control data. One an older setup wrote stays; without it the app's own icon stands in, SetupActivity)
    rom = os.path.join(a.out, 'RomFS')
    if os.path.realpath(rom) != os.path.realpath(romfs):
        shutil.rmtree(rom, ignore_errors=True); os.replace(romfs, rom)   # same storage: a rename, nothing copied
    for d in ('shader_pkg', 'cas', 'vk', 'shader_cache', 'extract', 'aoc'): shutil.rmtree(os.path.join(a.work, d), ignore_errors=True)   # only setup.log stays
    if S != a.work: shutil.rmtree(S, ignore_errors=True)
    try: os.remove(translated_mark(a.work))   # (its translation went with shader_cache)
    except FileNotFoundError: pass
    # the new pack holds every capture there is, so the launcher's list of captures already added is stale: the next start checks them against the pack again
    try: os.remove(os.path.join(a.out, 'captures_done'))
    except FileNotFoundError: pass
    if PROFILE.get('flat_normals', ''):   # (Legends: Arceus: No reflections, from the packs just set up; a failure leaves the game playable as the game draws it)
        try: flat_normals(a, log)
        except Exception as e: log.write(f'flat normals: {e}\n')
    open(os.path.join(a.out, 'ready'), 'w').write(chain + '\n')
    try: os.remove(state)
    except FileNotFoundError: pass
    try: os.remove(moved_mark(a.work))   # (the folders moved in are the game's now)
    except FileNotFoundError: pass
    print('DONE', flush=True)

def flat_normals(a, log=None):
    """The profile's flat normal maps (flat_normals: the RomFS folder of the packs, flat_normals_names: the textures) into <out>/flat_normals, from the game's own packs in
    <out>/RomFS, decoded with the game's own Oodle decoder (dmoodle and the rewritten modules of this setup). The engine lays the folder over the RomFS while the
    setting is on (android_main.c flat_normals). <out>/flat_normals/done holds the profile's names, so a changed list makes them again (SetupActivity)"""
    folder, names = PROFILE.get('flat_normals', ''), PROFILE.get('flat_normals_names', '')
    if not folder: return
    own = log is None
    if own: os.makedirs(a.work, exist_ok=True); log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1)
    dst = os.path.join(a.out, 'flat_normals'); dmo = os.path.join(a.bin, 'dmoodle')
    if not os.path.isfile(dmo): raise RuntimeError(f'{NAME} needs the dmoodle tool for its flat normal maps, which this app build does not have')
    if own: print('STEP 1/1 flat normal maps', flush=True)
    cmd = [sys.executable, os.path.join(DM, 'android/tools/flat_normals.py'), '--romfs', os.path.join(a.out, 'RomFS'), '--out', dst, '--folder', folder, '--names', names,
           '--oodle', dmo, '--modules', os.path.join(a.out, 'patched')]
    log.write('$ ' + ' '.join(cmd) + '\n')
    r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, PYTHONUNBUFFERED='1'))
    if r.returncode: raise RuntimeError(f'flat_normals.py failed ({r.returncode}); see {log.name}')
    open(os.path.join(dst, 'done'), 'w').write(names + '\n')
    if own: print('DONE', flush=True)

def pak_keys(path):   # the keys an NXSP shader pack holds, and its chain version
    with open(path, 'rb') as f:
        h = f.read(28)
        if h[:4] != b'NXSP': raise RuntimeError(f'{path} is not a shader pack')
        ver, n = struct.unpack_from('<II', h, 4)
        if ver not in (1, 2, 3, 4): raise RuntimeError(f'{path} has unsupported shader pack version {ver}')
        stride = 44 if ver >= 3 else 32; idx = f.read(stride * n)
    if len(idx) != stride * n: raise RuntimeError(f'{path} has a truncated shader pack index')
    return {idx[stride * i:stride * i + 8] for i in range(n)}, h[12:28].rstrip(b'\0').decode()

def pak_merge(path, add):   # merge a new uncompressed shader pack into the existing pack, preserving any GL depth variants
    with open(add, 'rb') as f: b = f.read()
    add_ver, nb = struct.unpack_from('<II', b, 4)
    if b[:4] != b'NXSP' or add_ver not in (1, 3): raise RuntimeError(f'{add} is not an uncompressed shader pack')
    add_stride = 44 if add_ver == 3 else 32
    src = open(path, 'rb'); h = src.read(28); ver, na = struct.unpack_from('<II', h, 4)
    if h[:4] != b'NXSP' or ver not in (1, 2, 3, 4): raise RuntimeError(f'{path} has an unsupported shader pack')
    old_stride = 44 if ver >= 3 else 32; compressed = ver in (2, 4)
    old_index = src.read(old_stride * na)
    has_gl = ver >= 3 or add_ver >= 3; new_stride = 44 if has_gl else 32; new_ver = (4 if compressed else 3) if has_gl else (2 if compressed else 1)
    index_delta = (new_stride - old_stride) * na + new_stride * nb
    end = os.fstat(src.fileno()).st_size + index_delta; rows = []; blobs = []
    for i in range(na):
        e = old_index[old_stride * i:old_stride * (i + 1)]
        so, sl, ro, rl = struct.unpack_from('<QIQI', e, 8)
        row = [e[:8], so + index_delta, sl, ro + index_delta, rl]
        if has_gl:
            go, gl = struct.unpack_from('<QI', e, 32) if old_stride == 44 else (0, 0)
            row += [go + index_delta if gl else 0, gl]
        rows.append(tuple(row))
    for i in range(nb):
        e = b[28 + add_stride * i:28 + add_stride * (i + 1)]
        so, sl, ro, rl = struct.unpack_from('<QIQI', e, 8); row = [e[:8]]
        pairs = [(so, sl), (ro, rl)]
        if add_stride == 44: pairs.append(struct.unpack_from('<QI', e, 32))
        elif has_gl: pairs.append((0, 0))
        for o, l in pairs:
            if not l: row += [0, 0]; continue
            blob = b[o:o + l]
            if len(blob) != l: raise RuntimeError(f'{add} has a truncated shader blob')
            if compressed: blob = struct.pack('<I', l) + zlib.compress(blob, 9)
            row += [end, len(blob)]; blobs.append(blob); end += len(blob)
        rows.append(tuple(row))
    rows.sort(key=lambda r: r[0]); tmp = path + '.new'
    with open(tmp, 'wb') as out:
        out.write(h[:4] + struct.pack('<II', new_ver, na + nb) + h[12:28])
        for row in rows:
            out.write(row[0] + (struct.pack('<QIQIQI', *row[1:]) if has_gl else struct.pack('<QIQI', *row[1:])))
        src.seek(28 + old_stride * na)
        while True:
            chunk = src.read(1 << 24)
            if not chunk: break
            out.write(chunk)
        for blob in blobs: out.write(blob)
    src.close(); os.replace(tmp, path)

def trinity_index(a, roots, log):
    # Trinity games (Scarlet / Violet) read every file through arc/data.trpfd, the sorted list of its paths' FNV-1a 64 hashes: a mod's loose file is only read once its path
    #   is out of that list. <game>/mods/.trinity_index/romfs/arc/data.trpfd is the game's own index with every path the enabled mods replace moved up by one (the list stays
    #   sorted, every other file is still found); android_main.c lays it over the RomFS first. No mods: it goes, so no path is left pointing at a loose file that is gone
    src = os.path.join(a.out, 'RomFS', 'arc', 'data.trpfd'); idx = os.path.join(a.out, 'mods', '.trinity_index')
    if not os.path.isfile(src): return
    sys.path.insert(0, os.path.join(DM, 'gfx/tools')); import shader_split as ss, bisect
    def fnv(s):
        h = 0xcbf29ce484222645
        for c in s.encode(): h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
        return h
    rels = set()
    for r in roots:
        for d, _, fs in os.walk(r):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), r).replace(os.sep, '/')
                if rel != 'arc/data.trpfd': rels.add(rel)
    shutil.rmtree(idx, ignore_errors=True)
    if not rels: log.write('trinity index: no mod files, none\n'); return
    fd = bytearray(open(src, 'rb').read()); fr = ss.fb_root(fd); st, n = ss.fb_vec(fd, fr, 0)
    hashes = list(struct.unpack_from(f'<{n}Q', fd, st)); moved = 0
    for rel in sorted(rels):
        h = fnv(rel); i = bisect.bisect_left(hashes, h)
        if i < n and hashes[i] == h and (i + 1 >= n or hashes[i + 1] != h + 1):
            struct.pack_into('<Q', fd, st + 8 * i, h + 1); moved += 1
    os.makedirs(os.path.join(idx, 'romfs', 'arc')); open(os.path.join(idx, 'romfs', 'arc', 'data.trpfd'), 'wb').write(fd)
    log.write(f'trinity index: {moved} of {len(rels)} mod file(s) taken out of the pack (the rest are new files)\n')

def mod_shaders(a):
    # Mods with shaders of their own (a changed .bfsha, a new effect): the game would hand the engine shaders the pack lacks, which then draw nothing until a capture
    # is translated on the next start, and captures miss the vertex / pixel pairing and compute facts a full scan has. So the mods' RomFS is scanned as setup scans the
    # game's (shader_split.py), and the shaders the pack does not have are translated with their pairs and facts and merged into it, before the game starts
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1); py = sys.executable
    env = dict(os.environ, PYTHONUNBUFFERED='1', PATH=a.bin + os.pathsep + os.environ.get('PATH', ''))
    tool = lambda n: os.path.join(a.bin, n)
    def run(cmd, **kw):
        log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
        r = subprocess.run(list(map(str, cmd)), cwd=a.bin, stdout=log, stderr=subprocess.STDOUT, env=kw.pop('env', env), **kw)
        if r.returncode: raise RuntimeError(f'{os.path.basename(str(cmd[1]))} failed ({r.returncode}): see {log.name}')
    roots = [r for r in a.mod_shaders.split(':') if r and os.path.isdir(r)]
    log.write(f'== {time.strftime("%H:%M:%S")} mod shaders: {", ".join(roots) or "no mod folders"}\n')
    trinity_index(a, roots, log)
    if not roots: print('DONE', flush=True); return
    chain = subprocess.run([py, chain_tool('dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    pak = os.path.join(a.out, 'shaders.pak'); have, pchain = pak_keys(pak)
    if pchain != chain: raise RuntimeError(f'the shader pack is version {pchain}, this app makes {chain}: set the game up again')
    mw = os.path.join(a.work, 'mods'); shutil.rmtree(mw, ignore_errors=True)
    pkg, cache = os.path.join(mw, 'pkg'), os.path.join(mw, 'shader_cache'); vk, built = os.path.join(mw, 'vk', 'b-' + chain), os.path.join(cache, 'b-' + chain)
    smap, facts, pairs = (os.path.join(mw, f) for f in ('map.tsv', 'facts.tsv', 'pairs.tsv'))
    os.makedirs(vk)
    print('STEP 1/3 find the mods\' shaders', flush=True)
    if PROFILE.get('shader_oodle', 'off') == 'on' and os.path.isfile(tool('dmoodle')):   # (Trinity games: a mod's own packs are Oodle-compressed too)
        env.update(DM_OODLE_HOST=tool('dmoodle'), DM_OODLE_MODULES=os.path.join(a.out, 'patched'), DM_OODLE_FILTER='BNSH')
    run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), ','.join(roots), ','.join('-' for _ in roots), pkg, smap, facts, pairs])
    new = []
    for ln in open(smap, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 3 and not ln.startswith(('#', 'package\t')) and bytes.fromhex(c[3][:16]) not in have: new.append(c)
    log.write(f'mod shaders: {len(new)} the pack does not have\n')
    if not new: shutil.rmtree(mw, ignore_errors=True); print('DONE', flush=True); return
    keys = [c[3] for c in new]
    print(f'STEP 2/3 translate {len(keys)} shaders', flush=True)
    gate = os.path.isfile(tool('dmsh_gate')) and os.path.isfile('/proc/meminfo')
    xenv = dict(XLATE=tool('dmsh_gate'), DM_GATE_EXEC=tool('dmsh_xlate'), DM_GATE_SLOTS=env.get('J', '8'), DM_GATE_DIR=mw) if gate else dict(XLATE=tool('dmsh_xlate'))
    benv = dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, **xenv, DMSH_CAS=os.path.join(mw, 'cas'), WORK=mw, DMSH_SOURCE_MAP=smap, DMSH_FACTS=facts, DMSH_PAIRS=pairs)
    try: run([py, chain_tool('dmsh_build.py'), pkg, cache, *keys], env=benv)
    except RuntimeError as e: log.write(f'mod shaders: {e}; the ones translated are kept\n')
    ok = [k for k in keys if os.path.exists(os.path.join(vk, k + '.spv')) and os.path.exists(os.path.join(built, k + '_b.json'))]
    for k in keys:
        if k not in ok:
            log.write(f'mod shader {k}: not translated\n')
            if os.path.exists(os.path.join(vk, k + '.spv')): os.unlink(os.path.join(vk, k + '.spv'))
    for k in ok: shutil.copyfile(os.path.join(built, k + '_b.json'), os.path.join(vk, k + '_b.json'))
    if not ok: shutil.rmtree(mw, ignore_errors=True); print('DONE', flush=True); return
    print('STEP 3/3 pack the shaders', flush=True)
    run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
    small = os.path.join(mw, 'new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
    pak_merge(pak, small)
    gmap = os.path.join(a.out, 'shader_map.tsv'); got = set(ok)   # the engine reads the map at start: the keys and their stage, with where they came from
    with open(gmap, 'a', encoding='utf-8') as f: f.writelines('\t'.join(c[:4] + ['/mods' + c[4] if len(c) > 4 else '/mods']) + '\n' for c in new if c[3] in got)
    log.write(f'mod shaders: {len(ok)} of {len(keys)} translated and added to the pack\n')
    shutil.rmtree(mw, ignore_errors=True)
    print('DONE', flush=True)

def live_yield():
    # beside the running game (inherited by every tool started from here): the lowest priority, the phone's small cores only (the game keeps the fast ones), and
    # first in line for Android's low-memory killer (oom_score_adj 1000), so a phone short of memory ends this translation, not the game; the shader then waits for the next start
    try: os.nice(10)
    except OSError: pass
    try:
        with open('/proc/self/oom_score_adj', 'w') as f: f.write('1000')
    except OSError: pass
    top = {}
    for c in range(os.cpu_count() or 0):
        try: top[c] = int(open(f'/sys/devices/system/cpu/cpu{c}/cpufreq/cpuinfo_max_freq').read())
        except (OSError, ValueError): pass
    small = {c for c, f in top.items() if f == min(top.values())} if len(set(top.values())) > 1 else set()
    if small:
        try: os.sched_setaffinity(0, small)
        except (OSError, AttributeError): pass

def live_publish(live, vk, rows):
    # the game is running: its shaders go where the engine looks while it runs (nvn_vk.c live_check), files first, then their map rows, then the generation the engine
    # watches; the next start finds them in the pack (merged above) and clears this folder
    os.makedirs(live, exist_ok=True)
    for r in rows:
        for ext in ('.spv', '.vkr', '.gl.spv'):
            f = os.path.join(vk, r[3] + ext)
            if os.path.exists(f): shutil.copyfile(f, os.path.join(live, r[3] + ext + '.part')); os.replace(os.path.join(live, r[3] + ext + '.part'), os.path.join(live, r[3] + ext))
    with open(os.path.join(live, 'map.tsv'), 'a', encoding='utf-8') as f: f.writelines('\t'.join(map(str, r)) + '\n' for r in rows)
    gen = os.path.join(live, 'gen'); open(gen + '.part', 'w').write(f'{time.time_ns()}\n'); os.replace(gen + '.part', gen)

def captures(a):
    # The profile's map places each in-memory shader at a fixed index of <stage>_other.bin; the package is written with the captured ones there and an empty record
    # in each other slot (never read: only the captured keys are translated). They are translated on their own and merged into the shader pack, so nothing
    # from the first setup has to be kept.
    # Captured shaders the map does not know (the profile's slots only list those its recording reached) go after the slots in <stage>_other.bin, the stage from the
    # shader program header (shader_split.py nvn_captured); they are translated in a run of their own that may fail without costing the others, and the ones translated
    # are added to the game's shader_map.tsv. Compute ones wait for the next full setup (their thread group comes from shader_split's facts)
    sys.path.insert(0, os.path.join(DM, 'gfx/tools')); import shader_split
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1); py = sys.executable
    env = dict(os.environ, PYTHONUNBUFFERED='1', PATH=a.bin + os.pathsep + os.environ.get('PATH', ''))
    def run(cmd, **kw):
        log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
        r = subprocess.run(list(map(str, cmd)), cwd=a.bin, stdout=log, stderr=subprocess.STDOUT, env=kw.pop('env', env), **kw)
        if r.returncode: raise RuntimeError(f'{os.path.basename(str(cmd[1]))} failed ({r.returncode}): see {log.name}')
    chain = subprocess.run([py, chain_tool('dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    pak = os.path.join(a.out, 'shaders.pak'); have, pchain = pak_keys(pak)
    if pchain != chain: raise RuntimeError(f'the shader pack is version {pchain}, this app makes {chain}: set the game up again')
    slots = {}
    for ln in open(PROFILE.table('shader_map.tsv'), encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[4].startswith('/nvn_capture/'): slots[c[3]] = (c[0], int(c[1]))
    gmap = os.path.join(a.out, 'shader_map.tsv'); mapped = set()
    if os.path.exists(gmap):
        for ln in open(gmap, encoding='utf-8'):
            c = ln.rstrip('\n').split('\t')
            if len(c) > 3 and not ln.startswith('#'): mapped.add(c[3])
    print(f'STEP 1/3 read the captured shaders', flush=True)
    new, extra = [], []
    for f in sorted(os.listdir(a.captures)) if os.path.isdir(a.captures) else []:
        k = f[:-5]
        if not f.endswith('.code') or bytes.fromhex(k) in have or (k not in slots and k in mapped): continue
        if not os.path.exists(os.path.join(a.captures, k + '.ctl')): continue
        ctl, code = open(os.path.join(a.captures, k + '.ctl'), 'rb').read(), open(os.path.join(a.captures, f), 'rb').read()
        if shader_split.skey(ctl, code) != k: log.write(f'capture {k}: the saved files do not hash to their key, skipped\n'); continue
        if k in slots: new.append((k, ctl, code)); continue
        st = 'cs' if code[:4] == shader_split.CS_MAGIC else shader_split.SPH_STAGE.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 0xf) if len(code) >= 0x34 else None
        why = 'no stage in the shader program header' if st is None else 'compute: waits for the next full setup' if st == 'cs' else shader_split.check(f'nvn_capture/{k}', st, ctl, code)
        if why: log.write(f'capture {k} (not in the shader map): {why}, skipped\n'); continue
        extra.append((k, st, ctl, code))
    if not new and not extra: print('DONE', flush=True); return
    cw = os.path.join(a.work, 'capture'); shutil.rmtree(cw, ignore_errors=True)
    pkg, cache = os.path.join(cw, 'pkg'), os.path.join(cw, 'shader_cache'); vk, built = os.path.join(cw, 'vk', 'b-' + chain), os.path.join(cache, 'b-' + chain)
    os.makedirs(pkg); os.makedirs(vk)
    got = {k: (ctl, code) for k, ctl, code in new}
    recs, rows = {}, []   # package name → records; the source map rows dmsh_build finds them by
    for name in sorted({p for p, _ in slots.values()}):
        recs[name] = []
        for i, k in sorted((i, k) for k, (p, i) in slots.items() if p == name):
            assert i == len(recs[name]), f'{name}: index {i} out of order'
            ctl, code = got.get(k, (b'', b''))
            rows.append((name, i, shader_split.STAGE_NAME[name[:2]], k, f'/nvn_capture/{k}'))
            rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); recs[name].append(bytes(rec) + code + ctl)
    added = []
    for k, st, ctl, code in extra:
        name = f'{st}_other.bin'; r = recs.setdefault(name, [])
        added.append((name, len(r), shader_split.STAGE_NAME[st], k, f'/nvn_capture/{k}')); rows.append(added[-1])
        rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); r.append(bytes(rec) + code + ctl)
    for name, r in recs.items(): open(os.path.join(pkg, name), 'wb').write(b''.join(r))
    smap = os.path.join(cw, 'capture_map.tsv')
    with open(smap, 'w', encoding='utf-8') as f: f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n'); f.writelines('\t'.join(map(str, r)) + '\n' for r in rows)
    print(f'STEP 2/3 translate {len(new) + len(extra)} shaders', flush=True)
    benv = dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, XLATE=os.path.join(a.bin, 'dmsh_xlate'), DMSH_CAS=os.path.join(cw, 'cas'), WORK=cw, DMSH_SOURCE_MAP=smap)
    if got: run([py, chain_tool('dmsh_build.py'), pkg, cache, *got], env=benv)
    if added:
        try: run([py, chain_tool('dmsh_build.py'), pkg, cache, *(r[3] for r in added)], env=benv)
        except RuntimeError as e: log.write(f'the captured shaders not in the shader map: {e}; the ones translated are kept\n')
        ok = [r for r in added if os.path.exists(os.path.join(vk, r[3] + '.spv')) and os.path.exists(os.path.join(built, r[3] + '_b.json'))]
        for r in added:
            if r not in ok:
                log.write(f'capture {r[3]} (not in the shader map): not translated\n')
                if os.path.exists(os.path.join(vk, r[3] + '.spv')): os.unlink(os.path.join(vk, r[3] + '.spv'))
        added = ok
    for k in [*got, *(r[3] for r in added)]: shutil.copyfile(os.path.join(built, k + '_b.json'), os.path.join(vk, k + '_b.json'))
    if not got and not added: shutil.rmtree(cw, ignore_errors=True); print('DONE', flush=True); return
    print(f'STEP 3/3 pack the shaders', flush=True)
    run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
    small = os.path.join(cw, 'new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
    pak_merge(pak, small)
    if a.live: live_publish(a.live, vk, [r for r in rows if r[3] in got] + added)
    if added:   # the engine reads the map at start: the next one finds these keys and their stage
        with open(gmap, 'a', encoding='utf-8') as f: f.writelines('\t'.join(map(str, r)) + '\n' for r in added)
        log.write(f'{len(added)} captured shaders not in the shader map translated and added to it\n')
    shutil.rmtree(cw, ignore_errors=True)
    print('DONE', flush=True)

if __name__ == '__main__':
    try: main()
    except Exception as e:
        for w in MOVED_WORK[:1]:
            try: restore_moved(w); print('MOVED BACK', flush=True)
            except OSError: pass
        print(f'FAILED {e}', flush=True); sys.exit(1)

#!/usr/bin/env python3
# Shader translation chain generations: one source tree, every game on the translation chain its own releases use.
#
# A game's chain version V (dm/gfx/tools/dmsh_chain.py) hashes the bytes of the translation tools themselves (dmsh_chain.py, dmsh_build.py, the translator's
# Program.cs, the tool lock with the translator's dll fingerprints) plus the game's tables and options; a player's installed shaders.pak loads only when its V
# matches the engine's DMSH_CHAIN. So any change to those tools gives every game a new V, and its players must set the game up again. Galar Native moved to
# a newer translator (FMZ multiplies, the AST optimizer on: chain 2026-11-13); Arceus Native and Paldea Native still ship the translator before it.
#
# A generation is a frozen copy of exactly the tool files that differ from the live ones (dm/gfx/chains/<generation>/, laid out as under dm/), chosen per game
# profile by the key xlate_chain (no key: the live tools, dm/gfx/tools). The frozen files are data, never edited: editing one would change that game's V.
# A tree for a generation is the live tools with the frozen files laid over them (materialize); APKs carry it in place of dm/gfx/tools (build_apk.py), the
# phone translator is built from it (dm/android/xlate_aot/build_xlate_aot.sh), and the PC tools use it (setup_game.py, vk_shaders.sh).
#
# Usage: chain_tree.py <title ID> <out dir>        materialize the game's tool tree into <out dir> (gfx/tools, ios/tools, third_party/ryujinx, android/xlate_aot)
#        chain_tree.py <title ID> --gen            print the game's generation ('' for the live tools)
#        chain_tree.py <title ID> --version        print the game's chain version V, computed by its own generation's dmsh_chain.py
#        chain_tree.py <title ID> --header         rewrite the game's dmsh_chain.h from its own generation (instead of dmsh_chain.py --header)
#        chain_tree.py --verify                    every profile: V from its generation against its dmsh_chain.h; exit 1 on a mismatch
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.normpath(os.path.join(HERE, '../..'))
sys.path.insert(0, os.path.join(DM, 'games')); import game as _game
# the parts of dm/ a tool tree holds (the translation scripts and the translator's sources; the shader tools import ios_metal.py from ios/tools)
PARTS = ['gfx/tools', 'ios/tools', 'third_party/ryujinx', 'android/xlate_aot']
GENS = {
    'pre-fmz': 'the translator before chain 2026-11-13 (Galar Native v0.8x): no FMZ in Ryujinx InstEmitFloatArithmetic.cs, the else-if fold in AstOptimizer.cs, '
               'XLATE_DEBUG 1 by default (dmsh_chain.py / dmsh_build.py), Program.cs without the DebugMode retry; Arceus Native and Paldea Native ship it',
}


def gen_of(g):
    """the profile's generation: its own xlate_chain key, else the one of the profile it takes its tables from; '' = the live tools"""
    v = g.get('xlate_chain', '')
    if not v and g.get('tables_from', ''): v = _game.load(g.get('tables_from')).get('xlate_chain', '')
    if v and v not in GENS: sys.exit(f'[RED] {g.id}: xlate_chain {v!r} is not a generation in dm/gfx/chains ({", ".join(GENS)})')
    return v


def overlay(gen):
    """{path under dm/: frozen file} of a generation ({} for the live tools)"""
    if not gen: return {}
    root = os.path.join(HERE, gen); out = {}
    for d, _, fs in os.walk(root):
        for f in fs: p = os.path.join(d, f); out[os.path.relpath(p, root).replace(os.sep, '/')] = p
    return out


def materialize(gen, out):
    """the live tool parts copied into out (laid out as under dm/), the generation's frozen files over them; out/gfx/tools/CHAIN names the generation"""
    for part in PARTS:
        src, dst = os.path.join(DM, part), os.path.join(out, part)
        if os.path.isdir(dst): shutil.rmtree(dst)
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns('out', 'obj', '__pycache__', '*.pyc'), symlinks=True)
    for rel, src in overlay(gen).items():
        dst = os.path.join(out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copyfile(src, dst)
    open(os.path.join(out, 'gfx/tools/CHAIN'), 'w', newline='\n').write(gen + '\n')
    # games/game.py: the shader scripts import game from <tree>/games; this stand-in loads the real dm/games/game.py, so the profiles are the checkout's own
    os.makedirs(os.path.join(out, 'games'), exist_ok=True)
    real = os.path.join(DM, 'games', 'game.py')
    open(os.path.join(out, 'games/game.py'), 'w', newline='\n').write(
        '# stand-in made by dm/gfx/chains/chain_tree.py: the real game.py of the checkout\n'
        'import importlib.util, sys\n'
        f'_s = importlib.util.spec_from_file_location("game", {real!r}); _m = importlib.util.module_from_spec(_s)\n'
        'sys.modules["game"] = _m; _s.loader.exec_module(_m)\n')
    return out


def tools_env(g, env=None):
    """the environment the generation's scripts run in: the game, and dm/games on the path (a tool tree has no games folder of its own)"""
    e = dict(env if env is not None else os.environ); e['DM_GAME'] = g.id
    e['PYTHONPATH'] = os.path.join(DM, 'games') + (os.pathsep + e['PYTHONPATH'] if e.get('PYTHONPATH') else '')
    return e


def run_chain(g, *args):
    """dmsh_chain.py of the game's own generation (the live one, or a materialized tree), with args; its stdout"""
    gen = gen_of(g)
    with tempfile.TemporaryDirectory(prefix='chain_') as t:
        tools = os.path.join(DM, 'gfx/tools') if not gen else os.path.join(materialize(gen, t), 'gfx/tools')
        r = subprocess.run([sys.executable, os.path.join(tools, 'dmsh_chain.py'), *args], capture_output=True, text=True, env=tools_env(g))
        if r.returncode: sys.exit(f'[RED] dmsh_chain.py {" ".join(args)} for {g.id} failed: {r.stderr.strip()}')
        return r.stdout.strip()


def header_chain(g):
    h = g.table('dmsh_chain.h')
    if not os.path.isfile(h): return None
    for ln in open(h, encoding='utf-8'):
        if ln.startswith('#define DMSH_CHAIN '): return ln.split('"')[1]
    return None


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['--verify']:
        bad = 0
        for g in _game.all_games():
            want = header_chain(g)
            if want is None: print(f'{g.id} {g.get("name")}: no dmsh_chain.h (no shader tables yet)'); continue
            got = run_chain(g); gen = gen_of(g) or 'live'
            print(f'{g.id} {g.get("name")}: chain {got} ({gen}), dmsh_chain.h {want}{"" if got == want else "  [RED] MISMATCH"}'); bad += got != want
        sys.exit(1 if bad else 0)
    if len(a) == 2 and a[1] == '--gen': print(gen_of(_game.load(a[0])))
    elif len(a) == 2 and a[1] == '--version': print(run_chain(_game.load(a[0])))
    elif len(a) == 2 and a[1] == '--header': print(run_chain(_game.load(a[0]), '--header'))
    elif len(a) == 2 and not a[1].startswith('--'): print(materialize(gen_of(_game.load(a[0])), a[1]))
    else: sys.exit(__doc__ or 'usage: see the file header')

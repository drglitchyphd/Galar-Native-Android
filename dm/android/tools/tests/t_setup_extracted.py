#!/usr/bin/env python3
# setup_game.py's step 1 from extracted folders (no keys, no NSP / XCI): which game and version an ExeFS is (main's build ID against the profiles, main.npdm's title ID
# when the build is unknown), the folder layouts a pick may be, the DLC index a folder gets, and the copy / move of the files. Synthetic files only; expected values
# are hand-written from the profiles (dm/games/*/game.tsv). Run: python3 -I dm/android/tools/tests/t_setup_extracted.py
import io, os, struct, sys, tempfile, contextlib, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
os.environ.setdefault('DM_GAME', '01008DB008C2C000')
import setup_game as sg

checks = fails = 0
def check(ok, what):
    global checks, fails
    checks += 1; fails += not ok; print(('✓ ' if ok else '✗ ') + what)
def err(f, *a):
    try: f(*a); return ''
    except RuntimeError as e: return str(e)

SHIELD, SWORD, ARCEUS, VIOLET, SCARLET = '01008DB008C2C000', '0100ABF008968000', '01001F5010DFA000', '01008F6008C5E000', '0100A3D008C5C000'
GALAR, PALDEA = [SHIELD, SWORD], [VIOLET, SCARLET]
BID = {SHIELD: 'a16802625e7826bf83b6f9708e475b91', SWORD: 'a3b75bcd3311385aeed67fbeeb79cbb7', ARCEUS: 'aee8f150dda1b5a838806e1a5ea6827a',
       VIOLET: '709bfd66115298640155fcc4979dba15', SCARLET: '421c5411b487eb4d049dd065fec95477'}

def exefs(root, bid, tid=None, name='ExeFS', magic=b'NSO0'):
    d = os.path.join(root, name); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'main'), 'wb').write(magic + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x100))
    for m in ('rtld', 'sdk', 'subsdk0'): open(os.path.join(d, m), 'wb').write(b'NSO0' + bytes(0x60))
    if tid is not None:   # META header, ACI0 at 0x80, the program ID at ACI0 + 0x10
        npdm = bytearray(0x100); npdm[:4] = b'META'; struct.pack_into('<I', npdm, 0x70, 0x80); npdm[0x80:0x84] = b'ACI0'; struct.pack_into('<Q', npdm, 0x90, int(tid, 16))
        open(os.path.join(d, 'main.npdm'), 'wb').write(npdm)
    return d

def romfs(root, name='RomFS', files=(('bin/a.bin', 3000), ('data/b.dat', 5000))):
    d = os.path.join(root, name)
    for f, n in files: os.makedirs(os.path.dirname(os.path.join(d, f)), exist_ok=True); open(os.path.join(d, f), 'wb').write(os.urandom(n))
    return d

with tempfile.TemporaryDirectory() as t:
    # ---- which game: the five games, each in its own app, Sword / Shield and Scarlet / Violet each to their own place
    for gid, app in ((SHIELD, GALAR), (SWORD, GALAR), (ARCEUS, [ARCEUS]), (VIOLET, PALDEA), (SCARLET, PALDEA)):
        d = exefs(os.path.join(t, gid), BID[gid], gid)
        check(sg.identify(d, app) == (gid, BID[gid]), f'{gid} ({BID[gid]}) is itself in its app {",".join(app)}')
    check(sg.identify(exefs(os.path.join(t, 'sw-no-npdm'), BID[SWORD]), GALAR)[0] == SWORD, 'Sword without main.npdm: the build ID alone says Sword')
    e = err(sg.identify, exefs(os.path.join(t, 'sw-pc2'), BID[SWORD]), [])
    check('Pokémon Sword 1.3.2' in e and 'this app runs Pokémon Shield' in e, f'no --games, Shield profile, Sword files: {e}')
    e = err(sg.identify, os.path.join(t, SHIELD, 'ExeFS'), PALDEA)
    check(e == 'your files are Pokémon Shield 1.3.2: this app runs Pokémon Violet and Pokémon Scarlet', f'Shield files in Paldea Native: {e}')
    e = err(sg.identify, os.path.join(t, SCARLET, 'ExeFS'), [ARCEUS])
    check(e == 'your files are Pokémon Scarlet 4.0.0: this app runs Pokémon Legends: Arceus', f'Scarlet files in Arceus Native: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'sh100'), 'b56ca071216570e8943206f10c3f3f2f', SHIELD), GALAR)
    check(e == 'your files are Pokémon Shield 1.0.0, and this app needs update 1.3.2: extract ExeFS and RomFS again with the 1.3.2 update applied', f'Shield 1.0.0 (no update): {e}')
    e = err(sg.identify, os.path.join(t, 'sh100', 'ExeFS'), PALDEA)
    check(e == 'your files are Pokémon Shield 1.0.0: this app runs Pokémon Violet and Pokémon Scarlet', f'Shield 1.0.0 in Paldea Native: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'swold'), '00' * 16, SWORD), GALAR)
    check(e.startswith('your files are Pokémon Sword of a version this app does not know (main build ID 000000') and 'needs update 1.3.2' in e, f'Sword of an unknown version: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'viold'), '11' * 16, VIOLET), PALDEA)
    check('Pokémon Violet of a version this app does not know' in e and 'needs update 4.0.0' in e, f'Violet of an unknown version: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'other'), '22' * 16, '0100000000010000'), GALAR)
    check(e == 'your files are another game (0100000000010000): this app runs Pokémon Shield and Pokémon Sword', f'another game: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'arcold'), '33' * 16, ARCEUS), GALAR)
    check(e == 'your files are Pokémon Legends: Arceus (01001F5010DFA000): this app runs Pokémon Shield and Pokémon Sword', f'another version of another app\'s game: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'unk'), '44' * 16), PALDEA)
    check(e.startswith('your ExeFS is of an unknown game or version') and 'Pokémon Violet 4.0.0, Pokémon Scarlet 4.0.0' in e, f'no npdm, unknown build: {e}')
    e = err(sg.identify, exefs(os.path.join(t, 'elf'), BID[SHIELD], magic=b'\x7fELF'), GALAR)
    check('is not a Switch program (NSO0)' in e, f'a decompressed main: {e}')

    # ---- layouts: one pick of the parent (any case), or each folder
    p = os.path.join(t, 'dump'); exefs(p, BID[SHIELD], SHIELD, name='exefs'); romfs(p, name='romfs')
    check(sg.resolve_exefs(p) == os.path.join(p, 'exefs') and sg.resolve_romfs(p) == os.path.join(p, 'romfs'), 'parent with exefs/ and romfs/: both found')
    q = os.path.join(t, 'DUMP2'); exefs(q, BID[SHIELD], SHIELD, name='EXEFS'); romfs(q, name='RomFS')
    check(sg.resolve_exefs(q) == os.path.join(q, 'EXEFS') and sg.resolve_romfs(q) == os.path.join(q, 'RomFS'), 'parent with EXEFS/ and RomFS/: both found')
    check(sg.resolve_exefs(os.path.join(q, 'EXEFS')) == os.path.join(q, 'EXEFS') and sg.resolve_romfs(os.path.join(q, 'RomFS')) == os.path.join(q, 'RomFS'), 'each folder itself')
    check('is not an ExeFS folder' in err(sg.resolve_exefs, os.path.join(q, 'RomFS')), 'a RomFS chosen as ExeFS: said')
    check('is an ExeFS folder, not a RomFS folder' in err(sg.resolve_romfs, os.path.join(q, 'EXEFS')), 'an ExeFS chosen as RomFS: said')
    os.makedirs(os.path.join(t, 'empty')); check('is empty' in err(sg.resolve_romfs, os.path.join(t, 'empty')), 'an empty RomFS: said')

    # ---- DLC: index from the names (nx_extract's rule: title ID = base + 0x1000 + n), never from the order chosen
    base = int(SHIELD, 16)
    def idx(path, b=base, n='Pokémon Shield'): return sg.dlc_index(path, b, n)
    check(idx('/sd/DLC/01008DB008C2D002/romfs') == (2, '01008DB008C2D002'), 'DLC/<title ID>/romfs → its number (2)')
    check(idx('/sd/Pokemon Shield [01008DB008C2D001][v0]/romfs') == (1, '01008DB008C2D001'), '"Name [title ID][v0]"/romfs → 1')
    check(idx('/sd/Pokemon Shield [01008DB008C2C000]/dlc/01008db008c2d002') == (2, '01008DB008C2D002'), 'lower case ID under a parent named after the base game → 2')
    check(idx('/sd/aoc/2') == (2, '') and idx('/sd/x/1/romfs') == (1, ''), 'a folder named 2 → 2; 1/romfs → 1')
    check(idx('/sd/DLC/0100A3D008C5D001/romfs', int(SCARLET, 16), 'Pokémon Scarlet') == (1, '0100A3D008C5D001'), 'Scarlet DLC 1')
    e = err(idx, '/sd/DLC/0100ABF008969001/romfs'); check('Pokémon Sword' in e and 'not of Pokémon Shield' in e, f'Sword DLC chosen for Shield: {e}')
    e = err(idx, '/sd/Pokemon Shield [01008DB008C2C000]/IsleOfArmor'); check('cannot be told from its name' in e and '01008DB008C2D001' in e, f'no ID, no number: {e}')
    dd = os.path.join(t, 'dlcs'); romfs(os.path.join(dd, 'DLC', '01008DB008C2D001'), name='romfs'); romfs(os.path.join(dd, 'DLC', '01008DB008C2D002'), name='romfs')
    got = sg.dlc_folders([dd]); check([os.path.basename(g) for g in got] == ['01008DB008C2D001', '01008DB008C2D002'], 'the dump parent with DLC/<ID>/romfs: each DLC')
    check(sg.dlc_folders([os.path.join(dd, 'DLC')]) == got, 'the DLC/ folder itself: each DLC')
    one = os.path.join(dd, 'DLC', '01008DB008C2D002')
    check(sg.dlc_folders([one]) == [one] and sg.resolve_romfs(one) == os.path.join(one, 'romfs'), 'one DLC folder: itself, its romfs/ inside')
    plain = romfs(t, name='IoA'); check(sg.dlc_folders([plain]) == [plain], 'a plain RomFS folder: itself')

    # ---- the copy / move helpers
    src = romfs(t, name='copysrc', files=(('a/1.bin', 1234), ('b/c/2.bin', 4321)))
    out = io.StringIO()
    with contextlib.redirect_stdout(out): prog = [0, sg.tree_size(src), -1]; sg.copy_tree(src, os.path.join(t, 'copydst'), prog)
    same = all(open(os.path.join(src, f), 'rb').read() == open(os.path.join(t, 'copydst', f), 'rb').read() for f in ('a/1.bin', 'b/c/2.bin'))
    check(same and prog[0] == 5555 and out.getvalue().strip().splitlines()[-1] == 'PROGRESS 5555/5555', 'copy_tree: same bytes, progress to 5555/5555')
    w = os.path.join(t, 'work'); os.makedirs(w); mv_src = romfs(t, name='mvsrc'); mv_dst = os.path.join(t, 'game', 'RomFS'); os.makedirs(os.path.dirname(mv_dst))
    os.replace(mv_src, mv_dst); sg.note_moved(w, mv_src, mv_dst); sg.restore_moved(w)
    check(os.path.isdir(mv_src) and not os.path.exists(mv_dst) and not os.path.exists(sg.moved_mark(w)), 'restore_moved: the moved folder goes back, the note goes')
    check(sg.same_storage(mv_src, w), 'same_storage: two folders of one temp folder')

print(f'{checks} checks, {fails} failed')
sys.exit(1 if fails else 0)

# STATUS

**Build:** v50 (not tested yet): v49 + spotted fades full size + frame generation paused on still screens (setting, default keep; 60 Hz there) + power readout (log / overlay). Upstream: nothing new.
- v49: v48 + spotted banner frame not a battle (g_fk_spot), spotted fades by draw order (bottom-right, then top-left on the top edge), encounter probe only with Encounter info on, **Lowest resolution when hot** (DM_FORK_SCALE_FLOOR stock / 85 / 70; hle_resscale.c factor_now). Upstream: nothing new.
- Heat (Steven's cool run on v48, Performance model): heat none, generation 4.4-6.2 ms, full resolution. The Quality model (15-24 ms) made frames miss and the guard drop to 0.70: keep Performance.
- v48:
- **Encounter info** (Controls → Encounter info, DM_FORK_ENC, off by default): in wild battles a panel (species, ★ shiny, nature, ability, IVs, EVs) from host/fork_enc.c. C_ENC control: drag = move (saved as "enc" in this screen's layout), tap = fold to a Poké Ball (gold ★ badge on a shiny). A battle is wild when the wild slot's EC is new (trainer battles keep the last wild one: no panel). Drawn after the controls at full strength.
- Label atlas 96 cells: ASCII 33-95 at their codes (DejaVu Sans Mono Bold), ★ at 18 (`fork_glyphs.py`); names from PKHeX (`fork_enc_names.h`, `fork_enc_names.py`). Overlay cap 512 items.
v47 (tested by Steven, notes below):
Upstream policy (Steven, Nov 13): merge from upstream `main` only, when needed. Watch: upstream branch `ultrawide-camera` patches the same addresses as Fill, corrected (fork_aspect.c); keep ours.

## New in v47
- **Save slots** (ForkSaves.java, menu → Saves): named, unlimited; the active one is always `<game>/save` (engine, export / import unchanged), the others `<game>/saves/<id>/save`, swapped by rename at the next start (DM_FORK_SLOT / _NEXT). New slot: empty or a copy. 3 backups per slot at each start, restorable (DM_FORK_RESTORE).
- **Smoothness** = 30 fps / 60 on screen (Lossless 2x). 60 fps mode and 3x scrapped; Fork.exportRoot sets DM_FPS_TARGET=30 and turns 3x into 2x.
- Restart confirm says OK. Trainer "spotted" shading (af0ef3ac + 128x4 gradient, and the next af0ef3ac draw) anchored to the bottom edge (log `[FORK] spotted shading`).
- From Arceus Native: ghost presses (lost fingers released, focus loss releases all, editor reset needs + and −), controller dead zone on the tilt, Turnip pool-reuse crash fix (7d49d92, verbatim), movement stick sensitivity, touch camera sensitivity / stick mode / hide right stick, freeze report (`[FORK] [RED] freeze`), captures at 960x540.
- **Context controls** (Controls → Context controls, each off by default): Dialogue (A, B, D-pad, turbo, FF; own layout `_dialogue`), Full-screen menus (no sticks / ZL ZR / turbo; own layout `_menu`), Overworld (hide ZL ZR, L3 R3, D-pad). ⊞ button (C_ALL, atlas cell 17) = every button until the screen changes. Dialogue = 3D + 56af7f00 without battle shaders (a guess: `[FORK] screen: dialogue`).
- **Gesture shortcuts** (DM_FORK_GESTURES, off): three-finger tap = ☰ menu; two-finger swipe down = X.
- **Encounter probe** (host/fork_enc.c, log only): after ~1 min logs heap base, trainer name / TID / SID, party slot 1; each new wild Pokémon in battle (species, shiny, IVs, nature, ability, EVs, checksum). Offsets from docs/ENCOUNTER_INFO_RESEARCH.md (SysBot / CaptureSight / PyNXReader, our build).

## Works (Steven)
- Setup, launch, battles, fold / unfold, portrait, Fill, corrected camera, DLC, layout editor + shapes, floating sticks, fork frame generation 2x (Turnip, pacing Smooth), fast-forward on upstream's real-time step.

## Open / check in v47
- Trainer shading fix; dialogue detection accuracy; the `[FORK] enc` lines (trainer name must match the save, checksum ok).
- Opponent trainer parties: no known offset (research doc).

## Steven's v47 notes (fixed in v48)
- Battle → Bag: the menu layout came ~3 s late (the battle-exit hold applied to menus too). Fixed in fork_ui.h: 10 frames to a menu, 90 only to world / dialogue.

- Trainer spotted (v47): the bottom-right fade is right now; the top-left one (the 2nd af0ef3ac shading draw) sat far down: now anchored to the TOP edge (fork_ui.h).
- Encounter probe works (v47 log): trainer "Eris" TID 61230 / SID 50863, party slot 1 species 816 (Sobble), checksum ok. Heap offsets hold on this host: the overlay can be built.

- The faded Poké Ball in the layout editor was upstream's Legends: Arceus throw button (off in Sword): left out of the editor now (touch_ui.c ctl_off).
- Encounter overlay design (Steven): a floating panel he can drag anywhere; a tap collapses it to a Poké Ball icon (tap again to open).

- Area view (Steven): out of battle the panel shows the area's spawns (rates) with party-style icons: black silhouette = not seen, faded = seen, full colour = caught. Needs: encounter tables (PKHeX data, GPL), current area + weather in memory (research), Pokédex block in RAM (research), icons copied from the player's own romfs at setup (never in the APK / repo), image drawing in the overlay. Order: battle panel first, research meanwhile.

## Steven's v48 notes (Nov 13)
- Battle buttons as soon as a trainer spots him: the spotted banner is b6ff050a (the battle shader): that frame no longer counts as a battle (fork_ui.h g_fk_spot). Not built.
- Top-left spotted fade sat 38 % down: both fades sample the 128x4 gradient; now by draw order (1st bottom-right, 2nd top-left on the top edge). Not built.
- Stutter, ~3 °C warmer, blurry: logs show heat "moderate" (v47: none), GPU copy waits 3-5 → 8-11 ms, generation 4.7 → 7 ms (Performance), Adaptive render scale down to 0.56. The Quality model run: generation 15-24 ms. Cause of the heat not found in the v48 diff; the encounter probe now runs only with Encounter info on. Asked Steven for a cool-phone A/B; offered a render-scale floor (e.g. 85 %).

- v49: the spotted fades swapped edges (the first draw is the top-left one). Now both kept full size, no anchoring (fork_ui.h; not built). Banner still anchored to the bottom.

## In v50
- Spotted fades full size (above).
- Frame generation paused on still screens (DM_FORK_FG_STILL=pause, default keep): menus / dialogue present without generation, screen asked for 60 Hz (window_rate), first 2 generated frames after resuming not shown. Log `[FORK] fg: paused / resumed`.
- Power readout (ForkPower.java): `[FORK] power: W, mA, V, battery °C, level, charging, fg paused` every minute; watts in the performance overlay.

## Steven's v50 notes (Nov 14)
- Corner fades: perfect now.
- Side fade at the "!" (screen recording): dark from left / right, cut at the UI band. Guess: 7c73d8ac without texture in the overworld (also battle HUD / menus): kept full size only at screen kind 0 with 3D (fork_ui.h sidefade). In the tree, not built. If wrong: the trainer capture names the shader.

- Encounter panel never showed: SysBot's wild offset 0x8FEA3648 is garbage on this host (v48 log: checksum BAD), while MyStatus / party hold. v51 (in the tree, compiles, not built): battle = the game's flag OR the fork's screen guess; the wild Pokémon is searched for in the heap (fork_enc.c find_wild, own thread: PK8-like header, species + TID / SID decoded by an LCG jump, must be the player's TID / SID, then full checksum; party / box block skipped), the find remembered for the next battle; a remembered slot still holding the last battle's Pokémon = a trainer battle (no search). Logs `[FORK] enc search`.

## Next
1. Steven tests v47 (long run).
2. If the `[FORK] enc` lines are right: the encounter info overlay (shown in wild battles; design with Steven).

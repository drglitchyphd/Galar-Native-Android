# STATUS

**Build:** v47 (not tested yet). Branch `fork/v20`, upstream main at 4e0814b (checked before the build: nothing new).
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

## Steven's v47 notes (fixes in the tree, not built)
- Battle → Bag: the menu layout came ~3 s late (the battle-exit hold applied to menus too). Fixed in fork_ui.h: 10 frames to a menu, 90 only to world / dialogue.

## Next
1. Steven tests v47 (long run).
2. If the `[FORK] enc` lines are right: the encounter info overlay (shown in wild battles; design with Steven).

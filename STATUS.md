# STATUS

**Build:** v19 on branch `fork/v15` (Sword only; the icon opens the game). Data in `/storage/emulated/0/GalarFork/`.

## Works (tested on the Fold, v13–v18)
- Own setup in the shared folder; launch, battles, 30 fps; fold / unfold switch in place.
- Upstream merged (21 commits), with no new setup needed.
- Picture "Fill, corrected": the camera is right on both screens. The battle cameras were fixed in v18: the x9 site is the game's second 16:9 constant, and the 4:3 mod has a typo there. Only one place in main builds 16:9, so the camera sites are complete.
- Layout profiles switch with the screen (`touch_layout_<profile>.txt`).

## New in v19 (not tested yet)
- In-game menu → System → **Setup screen**, over the running game, for DLC, saves and the driver (one-game builds had no way there).
- Fades no longer leave flashing bars: a shrunk UI draw that samples only ≤4x4 textures is drawn full size again (`fork_ui_fill`).

## Not tested yet
- Add DLC (`ForkDlc.java`) and the touch editor's Save / Reset / Cancel bar.

## Known / watch
- Unknown shaders drawn before the first UI shader stay full size (logged: `ecacdb40…`, `b6ff050a…`). Check whether they are UI pieces that look stretched.
- The game is killed in the background after a few minutes (about 2.4 GB). Idea: a foreground-service toggle.

## Next
1. Steven tests v19: fades, DLC via Setup screen, the editor bar.
2. Portrait on the inner screen and pause on fold (M2/M3). Then the keep-alive toggle and save backups (M6).

# STATUS

**Build:** v20 on branch `fork/v20` (v19 is merged into `main`). Sword only; the icon opens the game. Data in `/storage/emulated/0/GalarFork/`.

## Works (tested on the Fold, v13–v19)
- Own setup in the shared folder, launch, battles, 30 fps, fold / unfold in place, upstream merged.
- Picture "Fill, corrected": the camera is right on both screens, the battle cameras too (v18). Main menu fades are fixed (v19).
- DLC through in-game menu → System → Setup screen (v19). Layout profiles per screen.

## New in v20 (not tested yet)
- Controls → **Control scheme: Floating sticks** (`DM_FORK_TOUCH=float`; default Stock):
  - left half = movement stick, right half = camera stick, each based where the thumb lands;
  - a quick tap that is not on a button = A (a menu item while a game menu shows); two-finger tap = B.
- Controls → **Stick dead zone** (`DM_FORK_DEADZONE`, 0–40 %, default 12).
- Touch editor bar: drag it by its label (⠿); its position is kept per screen.

## Known / watch
- A brief flash at the top / bottom when the in-game (X) menu opens. Needs `DM_FORK_CAPTURE=1` and the zip.
- Unknown shaders drawn before the first UI shader are kept full size (`ecacdb40…`, `b6ff050a…`). Check for stretched HUD pieces.

## Next (agreed with Steven, Nov 9)
1. Portrait on the inner screen.
2. Keep alive in background (foreground-service toggle).
3. Frame generation 30→60 (research first: lsfg-vk / the player's own Lossless.dll, vs one built into the fork). Steven is collecting documentation.
- The game's own 60 fps mode changes some animation speeds: not pursued.

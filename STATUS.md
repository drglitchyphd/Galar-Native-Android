# STATUS

**Build:** v18 (`fork/v15` branch). v17 tested (log Nov 9):
- Fill, corrected works on both screens, the layout profiles switch with the screen, and DLC is not added yet.
- v18 fixes:
  - The x9 site is the game's second 16:9 constant, and the 4:3 mod has a typo there (0x67aa for 0x3faa). The fork now writes the real ratio.
  - The x12 site is not a constant, so it is left alone.
  - Split-screen battles composite with e0feaaf9/57319aad. Draws now count as scene until the first UI shader each frame.
  - The log lists other places the game builds 16:9 (`[FORK] 16:9 ...`). **Not yet tested on the Fold.**
v15 test: "Fill, corrected" looks good in normal play. Transitions flash a squished image, or show the old picture in the bands; some battle camera cuts are squished.
v16 adds:
- DLC card on the setup screen ("Add DLC (.nsp)": `ForkDlc.java`, nx_extract `--aoc` into `aoc/`, no new setup).
- Touch layout per screen and orientation (`touch_layout_<profile>.txt`, from `profile_of` in `touch_ui.c`).
- Save / Reset / Cancel bar in the touch editor (`ForkTouch.java`).
- `DM_FORK_CAPTURE=1`: captures the frame after a switch from 3D to UI only, to diagnose the transition glitches.

## What works (tested on v13/v14)
- Own setup in `/storage/emulated/0/GalarFork/` (game, saves, settings, mods, caches, logs). Keys stay private.
- Launch, loading, battles. 30 fps with about 1 % long frames.
- **Fold / unfold switches in place** (inner 2504x2256 ↔ outer 2520x1080), no restart.

## New in v15
- **Upstream merged** (21 commits; the shader chain is unchanged, so no new setup):
  - faster setup step 5 (#100), and a finished translation is kept;
  - leftovers cleanup (#47), now also old `gate.*` lock files;
  - texture uploads about 25x faster;
  - frame pacing and a 30 fps guard;
  - 3D render scale and Graphics "Lowest";
  - "Faster rendering (experimental)";
  - crash-record fixes.
- The mods and setup-log hints show `GalarFork/...`.
- **Picture → "Fill, corrected"** (`DM_ASPECT=fix`), off unless chosen:
  - `host/fork_aspect.c`: writes the 4:3 mod's camera sites with the screen's own ratio, and rewrites them on fold / unfold. Skipped when the 4:3 mod is on.
  - `gfx/nvn/fork_ui.h`: draws into the 3D composite's image after it (UI) get a shrunk viewport and scissor. Menus without 3D get black bands.
  - `touch_menu.c`: menu taps corrected by the same scale.
  - `DM_FORK_ASPECT_X9=0/1` in env.txt forces the mod's unexplained x9 sites (default: on for the narrower inner screen only).

## Test v15
1. Picture "16:9": the game as before, faster loading.
2. Picture "Fill, corrected" on the inner screen: the 3D is not squashed, and the HUD, text boxes and bag are in shape, with bands above / below. Fold to the outer screen and back.
3. Send the `[FORK] aspect` and `[FORK] UI` log lines, plus a screenshot of each screen.

## Next
- Fix whatever v15 shows. Then portrait on the inner screen and pause on fold (M2/M3).
- Idea: keep the game alive in the background (foreground service toggle). Save states are not possible.

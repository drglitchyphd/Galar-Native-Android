# STATUS

**Build:** v38 (not tested yet):
- Touch editor: ☰, ⏩ and ⚡ show and move again (the overlay held 128 items; the editor needs more: now 320 in `nvn_vk.c`).
- Editor bar **Shape** button: the selected control cycles circle / rounded square / square / pill (6th field in `touch_layout_*.txt`; old files load as circles). Not for sticks, ☰, Poké Ball.
- Controls → Battle → **Battle: fade after** (`DM_FORK_BATTLE_FADE`, default "same as elsewhere").
- Back icon redrawn (`fork_glyphs.py`). Fill, corrected: a UI draw that samples a picture-sized (≥1280×700, ~16:9) image keeps full size (trainer-approach overlay, `fork_ui.h`).
- Open: split-screen battle divider sits too far left (Fill, corrected). Needs a 16:9 vs Fill screenshot of the same moment.
v37: Battle icons / layout / opacity; fast-forward (`host/fork_ff.c`, main+0x203eeb8); turbo A; icons in atlas cells 10–14.
v36: Back (button or gesture) = an 80 ms B press (a gesture's down/up came too close together); the touch-editor bar starts at the bottom centre (it covered ☰). v35: X stays in battle. v34: Controls → Opacity from 5 %, **Colour** (blue / grey / white / dark / red / green / purple), **Size** 50–150 %; editor bar size slider for the selected control. Steven: Frame pacing Smooth plays great (A/B tests still running). v33: Performance → **Frame pacing** Stock / Smooth / Smoothest (`DM_FORK_PACE_MARGIN` 1–3 refreshes ahead, default 1 = stock); "uneven" judged against half the interval with frame generation on.
v32 log: no shader or texture stalls, no heat; game frames p50 33.4 / p95 37 ms (jitter, ~half miss their 8-refresh slot). "Frame work" is not comparable while Faster rendering (`DM_NVN_ASYNC`) is on. Next: an A/B with Faster rendering off and on.
v32: Controls → **In battle: Hide unused** (`DM_FORK_BATTLE_HIDE`, default off): X, L/R, ZL/ZR, +/−, right stick hidden in battles. Screen detection runs whenever that's on (not only Fill, corrected); it leaves battle after 3 s without the HUD. v31 log: battle / menu / world detection matched Steven's play. v31: battle guess = 3D + b6ff050a / ecacdb40 (56af7f00 also draws dialogue boxes). Frame generation now 44–56 frames/s on screen (v29 log). v30: composites are never shrunk (the Pokémon screen composites after its menu: v18–v29 shrank its menu twice); `[FORK] screen: world / menu / battle` guess logged (battle = 3D + shader 56af7f00). v29: menu banner + **Restart now** for settings that apply next start (`ForkRestartActivity`, `:restart` process); `[FORK] ui census` log (UI shaders per screen, for battle detection). v28: frame generation input order fixed (the library counts its own calls; the fork skipped the first frame, so every generated frame used reversed motion: cutscene ghosting); generated frames get their own present id (the display timing report counts them). v27: the worker sets the display times when it presents (v26: 30–45 shown/s, the times were stale by then, so Android showed only the newer frame). v26: FIFO while frame generation is on (v25 log: the worker takes ~20 ms of 33 and the game never waits, but mailbox dropped the generated frame, so ~30/s on screen); no "unofficial build" notices in fork builds. Grey bars at game load predate frame generation, and the game's image is black there: the cause is outside the game (an Android layer?), to check.

## Works (tested on the Fold, v13–v19)
- Own setup in the shared folder, launch, battles, 30 fps, fold / unfold in place, upstream merged.
- Picture "Fill, corrected": the camera is right on both screens, the battle cameras too (v18). Main menu fades are fixed (v19).
- DLC through in-game menu → System → Setup screen (v19). Layout profiles per screen.

## New in v23 (not tested yet): frame generation
- Setup screen → **Frame generation: Choose Lossless.dll**: the player's own DLL is copied to `filesDir/lsfg` and never shared.
- In-game menu → Performance → **Frame generation 2x** (`DM_FORK_FG`), **Flow scale** (`DM_FORK_FG_FLOW`, default 75), **Model** (`DM_FORK_FG_PERF`, default Performance). They apply from the next start.
- `dm/fork/lsfg` → `libforklsfg.so`:
  - lsfg-vk-android 3e89e54 framegen and extract (MIT), the exyorha/dxbc translator (zlib), built by `build_fork.sh` (`FORK_LSFG=1`);
  - `PancakeTAS/dxbc` is gone from GitHub, so its translator was swapped in, with a `.code` adapter.
- `gfx/nvn/fork_fg.h`:
  - each frame: blit the real frame into the shared AHB image (frame % 2), `presentContext` + `waitIdle`;
  - present the generated frame half an interval before the real one;
  - the AHB device extension is enabled only when frame generation is on.
- Risks: the library uses the system Vulkan loader. With a custom Turnip / Adreno driver package, use "the phone's own driver" if frame generation fails. Generation blocks the present (logged as `[FORK] fg: … ms each`).

## In v22
- System → Background → **Keep running in the background** (`DM_FORK_KEEPALIVE=on`, default off): the `ForkKeepAlive` foreground service (specialUse) with a quiet notification while the game is open.
- System → Bug reports → **Capture the next odd frame**: arms `fork_ui.h`'s capture live. It fires on a switch from 3D to UI only, or on an unknown shader drawn full size over the 3D (the X-menu flash suspect).

## New in v21 (portrait works, Steven Nov 9)
- Display → **Portrait on the inner screen** (`DM_FORK_PORTRAIT=on`, default off):
  - on the inner screen the game turns with the phone (`Fork.orientation`); the outer screen stays landscape;
  - portrait puts the picture at the top (`dm_picture_box`) with its own default layout below it;
  - "Fill, corrected" applies only to landscape.
- Floating sticks: the fixed sticks are hidden and only the one under the thumb is drawn. The D-pad and the right stick fade with the rest.

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
1. ~~Portrait on the inner screen~~ (v21: works, Steven Nov 9).
2. ~~Keep alive in background~~ (v22).
3. ~~Frame generation~~ (v23, first try: test).
- The game's own 60 fps mode changes some animation speeds: not pursued.
- Later idea: switch the control behaviour in full-screen menus (no 3D composite drawn: `fork_ui.h`) and in battles (detection needs a shader-logging test first). Sword has no `touch_menu` profile.
- Battle controls: Steven wants unused buttons hidden in battle plus context-aware controls. Needs battle detection: v29's ui census shows the shader sets; then map hide-lists per screen kind.
## Roadmap agreed Nov 11 (Steven's baseline: Turnip driver, frame generation 2x, Frame pacing Smooth)
1. Battle layout: its own layout profile and style (opacity / colour / size), icons for the core actions, a sensible default around Sword's battle UI.
2. Fast-forward via the game's frame step (loader.c FPS_SITES dt): hold = while held, tap = toggle; speed in the options (1.25 / 1.5 / 2x); in battle and in the overworld.
3. Turbo A (hold to repeat). Encounter info overlay (shiny / IVs / nature) from game memory: research the 1.3.2 locations.
4. Save slots (separate save folders per character), switched on the setup screen. Trading / universal box later (PKHeX.Core is GPL: keep it separate).
- Skipped: autosave changes; screenshot button.
- Research: live HUD from game memory (party HP / status, Poké Balls, Pokédex seen / caught, area dex).
- Queued: the trainer 'spotted you' corner overlay shows only in the middle band (Fill, corrected). Likely a full-screen UI overlay with a texture larger than the 4x4 fade rule (`fork_ui_fill`). Steven may send screenshots (Fill vs 16:9).

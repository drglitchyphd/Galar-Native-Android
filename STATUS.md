# STATUS

**Build:** v25 (overlay shows on-screen fps; `[FORK] fg timing` log; `DM_FORK_FG_PACE=0` test switch). v24 test: still uneven (39 → 30 on screen per s, game 16 fps at the intro), and grey bars over the picture's top / bottom, cause unknown (the game's own image is black there). v23 test: frame generation ran (7.6 ms per generated frame) but only ~18 frames/s reached the screen, because it waited for the GPU on the game's thread. Branch `fork/v20` (v19 is merged into `main`). Sword only; the icon opens the game. Data in `/storage/emulated/0/GalarFork/`.

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

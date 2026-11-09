# NOTES.md: M0 recon

Paths are relative to `dm/` unless they start with `scripts/` or `docs/`. Line numbers are from upstream `be5b6d3`.

## Game IDs and profiles
- Sword is `0100ABF008968000`; its profile is `games/0100ABF008968000/game.tsv`.
- Shield is `01008DB008C2C000`. It is the **base game**: the app defaults, package name and shader tables all come from Shield.
- Sword's profile sets `tables_from 01008DB008C2C000`, so it shares Shield's shader tables and translation chain.
- The game list is `games/game.py`. `android/tools/build_apk.py:156` writes it to the `assets/games` file in the APK, base game first.
- Sword 1.3.2 build ID: `host/loader.c:532` `FPS_SITES`.

## 1. Package, app name, icon
- The manifest is generated in `android/tools/build_apk.py` `main()` (L99–123). No manifest is checked in.
- Package: L75, `com.nxnative.` + `file_stem(game).lower()`, which gives `com.nxnative.pokemonshield`. `--package` overrides it, but `scripts/build_apk_pc.sh:101` does not pass it.
- App name: L74, from the profile's `app_name` ("Galar Native"). `--name` overrides it.
- Icon: L73 and L96–98. `png()` (L20) draws a ball from the profile's `app_icon` colours into `@mipmap/ic_launcher`.
- Version: `--version` (L69, default 12).
- APK filename: `scripts/build_apk_pc.sh:100`.
- Game selection:
  - `<extFiles>/current_game` is read by `GameSettings.game()` (L39) and written by `choose()` (L45).
  - On the native side, `android/app/jni/android_main.c:54` `choose_game()` reads it.
  - Each game's shortcut passes the intent extra `game`: `SetupActivity.pinShortcut()` (L850), read in `SetupActivity.onCreate` (L124–129).

## 2. Extracted game files: location matters for M1
- Files live in `getExternalFilesDir(null)` = `/storage/emulated/0/Android/data/<pkg>/files` (`GameSettings.root()` L37).
- `GameSettings.dir()` (L49) puts the base game (Shield) in the root and other games in `root/<titleID>/`.
  - So Sword is `Android/data/com.nxnative.pokemonshield/files/0100ABF008968000/`.
- The native side builds the same path: `android_main.c` `host_start()` L614–615 plus `choose_game()`.
- Contents: `RomFS/`, `patched/`, `shaders.pak`, `shader_map.tsv`, `font/`, `aoc/`, `icon.jpg` (`android/tools/setup_game.py` header).
- `g_game` (`android_main.c:632`) is the path the engine reads game data from.
- Internal `filesDir` holds only `keys/`, `python/`, `dm/`, `bin/` and `drivers/`.
- **Implication:** the files sit in another app's `Android/data`. On Android 11+, "All files access" cannot read there, so the plan's Shizuku path (or the ~15 GB own-setup fallback) is the realistic route.

## 3. Setup version/format
- `android/tools/setup_game.py:328` writes `<gameDir>/ready`, whose first line is the shader-chain string. L212 deletes it when setup is redone.
- `SetupActivity`:
  - `setUp()` (L100) needs both `ready` and `shaders.pak`.
  - `chainCurrent()` (L104–109) compares that first line with the APK asset `shader_chain` (from `build_apk.py:44`).
  - `ready()` (L103) is `setUp() && chainCurrent()`.
  - On a mismatch the app asks for setup to be redone (L364–365).
- Other markers: `captures_done` and `mod_shaders_done` (`pendingCaptures` L693, `modShaderState` L705–716, `pendingWork` L703). `GameActivity.onCreate:31` hands off to setup when work is pending.
- **For M1:** the fork's `shader_chain` must equal the main app's, or the shared files read as stale. That makes the chain string the version check.

## 4. Storage (all per game, under `<gameDir>`, set by `SETP` in `android_main.c` from L634)
- **Settings:** `env.txt`, `KEY=VALUE` lines, with no SharedPreferences.
  - Java: `GameSettings.envFile/get/set/setIn` (L52–60).
  - Native: `android_main.c:624` `env_file()`.
  - Pad mapping is in it too (`DM_PAD_MAPPING`).
- **Touch layout:** `touch_layout.txt` (`android/app/jni/touch_ui.c:60` `touch_ui_set_dir`).
- **Saves:** `DM_SAVE` = `save/` (L657). Imports are staged in `save_import/`, and the old save is kept as `save.before-import-*`. Export is `SetupActivity` L603.
- **Mods:** `DM_MODS_DIR` = `mods/` (L648).
- **Shader cache:**
  - `vk_pipelines.bin` and `vk_pipeline_cache.bin` (L652), saved on `APP_CMD_PAUSE` by `nvn_vk_cache_save`.
  - Also `texcache/`, `live/` and `nvn_capture/`.
  - `vk_pipelines_seed.bin` sits in `g_game`.
- **Logs:**
  - `log.txt` and `log.prev.txt` (L618–619).
  - Mirrored to `Download/<stem>-log.txt` (`#define DL` L482, `log_mirror` thread). The stem gives `PokemonSword-log.txt`.

## 5. Touch controls (native overlay, not an Android View)
- **Drawing:**
  - `android/app/jni/touch_ui.c` `front_overlay()` (L427) builds the `dm_ovl_item`s (struct in `gfx/nvn/nvn_ovl.h`).
  - `gfx/nvn/nvn_vk.c:3396` draws them on the swapchain image with `ovl_draw_turned()`.
- **Controls:**
  - The control enum is at `touch_ui.c:31`.
  - `target_at()` (L120) maps hit areas. Left of 42% of the width is the floating left stick; right of 45% is the camera.
  - `ax()` (L127) scales stick values.
- **Touch input path:** `android_main.c` ~L240 → `touch_ui_event()` (`touch_ui.c:267`) → `dm_input_set()` / `dm_input_press()` (`host/hle_hid.c:33/41`). Back key = B (`android_main.c:248`).
- **Two-finger tap = B:** `touch_ui.c` L280 and L290–294 (`g_two_t`, `g_tap_mute`).
- **Tap = A:** only when `GESTURES_ON` (L13/15, the Legends: Arceus build) or over a game menu (`touch_menu.c` `touch_menu_tap`).
- **Pad auto-hide:**
  - `GameActivity` `InputDeviceListener` (L143–145) and `check()` → `nativePadConnected` (L192) → `android_main.c:197` → `touch_ui_pad()` (`touch_ui.c:333`).
  - Gamepad input also calls `touch_ui_hide()` (`android_main.c` L263/274).
- **Layout:**
  - Defaults come from `layout(w,h)` (`touch_ui.c:77`) as fractions of the screen height.
  - The file has one line per changed control: `name fx fy fr shown` (`layout_load/save/apply` L61/68/73, `CTL_NAME` L57).
  - There is **only one layout**, not one per orientation.
- **Editor (exists):**
  - Opened by holding − for 2 s, or from the menu `GameMenu.java:271` → `nativeEditTouch` → `touch_ui_edit` (L416).
  - `edit_event` (L220): drag, pinch, double-tap hide.
  - `edit_holds_locked` (L252): − saves, + resets.
- **Opacity and fade:** `DM_TOUCH_OPACITY` and `DM_TOUCH_FADE`. Sliders are at `GameMenu.java:269–270`. They apply live through `nativeSetting` (`android_main.c:207`) → `touch_ui_setting` (L407).
- `android/CONTROLS.md` is an unbuilt design brief.

## 6. Display / surface / Picture
- **Window:** NativeActivity's `ANativeWindow`, with no SurfaceView.
  - `APP_CMD_INIT_WINDOW` sets `g_window` (`android_main.c:773`).
  - `APP_CMD_TERM_WINDOW` calls `nvn_vk_window_term` (L774).
  - Window frame rate: `window_rate()` (L353).
- **Swapchain:**
  - Created in `nvn_vk.c` `swap_ensure_now()` (~L3282): FIFO, pre-rotated.
  - `swap_ensure()` (~L3265) rebuilds it when the window size changes.
  - `present_window_locked()` (L3583) handles `OUT_OF_DATE` / `SURFACE_LOST` (L3592, L3625).
- **Scaling:** `present_window_locked` → `dm_picture_box()` (L3606) → `scale_draw` (`DM_SCALER`) or a `vkCmdBlitImage` stretch into that box.
- **Game render size:** `host/hle_oe.c:110` `h_oe_DefRes` (1080p docked, 720p handheld).
- **Picture setting:**
  - Key `DM_ASPECT`, values `""` (16:9 fit), `fill` and `20:9`.
  - Menu: `GameMenu.java:236–237`.
  - Applied live: `nativeSetting` (`android_main.c:208`) sets `dm_aspect_mode`.
  - Box math: `gfx/nvn/nvn_ovl.h:23–35` (`dm_aspect()`, `dm_picture_box()`).
  - `touch_menu.c:170` maps menu taps through the same box.
- **M3 hook:** add the new aspect modes and per-screen boxes in `dm_picture_box()`. The touch overlay and `touch_menu.c` must use the same box.

## 7. .pchtxt patches (`host/loader.c`, the non-Apple branch)
- **Parser:** `pchtxt_apply` (L429).
  - It applies a file only if `@nsobid-` matches `m->build_id`.
  - It understands `@flag offset_shift`, `@enabled/@disabled/@stop`, and hex or string values.
  - Offset: `off = addr + shift - 0x100` (L459).
  - It rejects writes out of range or onto rewritten code (`overlaps_rewrite` L419).
- **Scanning:**
  - `pchtxt_scan` (L470) reads `mods/` in alphabetical order, plus `<mod>/exefs/` and `<mod>/`.
  - Names starting with `-` or `.` are off. `romfs` is skipped.
  - `pchtxt_dir` (L484) is driven by `DM_MODS_DIR` and called from `map_image` (L577, L590).
- **Mod manager:** `GameMenu.java` (~L300). Turning a mod off adds a `-` to its folder name. Order comes from `mods/order.txt`. Zip import is at L526–563.
- **Crash guard:** `android_main.c` `mods_off` (L304, used L327) turns all mods off after a crash.
- **RomFS mods:** `android_main.c` `mods_romfs` (L364–388) → `DM_ROMFS_OVERLAY` → `host/hle_fs.c` `rom_overlay_path`.
- **Builtin mods:** `host/builtin_mods.h`, generated by `tools/builtin_mods.py`. There are none for Sword/Shield, only Scarlet/Violet.

## 8. Present interval and fps
- **NVN window, in `gfx/nvn/hle_nvn.c`:**
  - `nvnWindowSetPresentInterval` (L1204) stores the game's request in `win_t.interval`. `Get` is L1203, the builder is L1190.
- **Present, `nvnQueuePresentTexture` (L1608):**
  - `period = present_period()` (L1521) = interval × 1e9 / `g_hz`.
  - `present_cfg` (L1508) reads `DM_PRESENT_HZ` (0 means uncapped) and `DM_PRESENT_INTERVAL`.
  - Pacing goes through `gfx/nvn/nvn_pace.h` `nvn_pace()` (~L1711).
- **Fps setting:**
  - `DM_FPS_TARGET` (Auto / 30 / 60) at `GameMenu.java:252–254`, shown only when `GameSettings.has60` (L83).
  - Auto is always 30 (`AUTO_60=false`, `GameSettings.java:82`).
- **The 60fps patch is already built in:**
  - `loader.c` `fps_patch` (L536) with `FPS_SITES` (L532). For Sword: `itv=0x18a2c88`, `dt=0x203eeb8`.
  - It sets the interval to 1 and the frame step to 1e9/fps.
  - `hle_nvn.c` `vstep_tick` (L1571) then rewrites the step word each frame, so game time follows real time.
  - **The app's 60fps mode is therefore not 2× speed.** The crowd flicker reported upstream comes from this path.
- `host/hle_fpslock.c` is not used for Sword.
- **Fast-forward and turbo:** not found anywhere.
  - M6 candidate: while held, shorten `present_period()` (or skip the pacing wait) and leave the game's fixed step alone.
  - Not verified: whether `vstep_tick` would then cancel out the speed-up when the 60fps patch is on.

## 9. Lifecycle / fold
- **Manifest (`build_apk.py:113–119`):**
  - Both activities set `configChanges=orientation|screenSize|screenLayout|…|smallestScreenSize|density`, so fold, unfold and rotation do **not** recreate the activity.
  - `GameActivity` has `screenOrientation=sensorLandscape` and `launchMode=singleTask`. `resizeableActivity` is not set.
- **`GameActivity` overrides:** only onCreate, onResume (re-checks pads), onNewIntent, onActivityResult and onDestroy.
- **Native `on_cmd()` (`android_main.c:771–781`):**
  - `INIT_WINDOW` starts the host once (`g_started`).
  - `TERM_WINDOW` frees the swapchain.
  - Focus and pause call `dm_focus_set` (`host/hle_oe.c:62`).
  - Audio is suspended only when `DM_BACKGROUND=off` (`bg_play` L203). By default the game runs in the background.
  - `WINDOW_RESIZED`, `CONFIG_CHANGED` and `CONTENT_RECT_CHANGED` are not handled. The swapchain resizes lazily in `swap_ensure`.
- `host/hle_lifecycle.c`: SIGTERM leads to a clean shutdown, nothing more.
- **For M2:** fold/unfold may already work in place. Portrait (inner screen) is blocked by `sensorLandscape`. Test on the Fold before writing code.

## 10. Inner-screen fullscreen research (frame captures, Nov 8, Adreno 840)
- **4:3 mod** (`[4.3 Ultrawide v1.3.2]/exefs/1.3.2.pchtxt`, Fl4sh / TLin-Y): `@nsobid-A3B75BCD…` is **Sword 1.3.2**, `offset_shift 0x100`.
  - `0x6076c4` `b 0x607664`: a cave that sets `w9 = 0x3FAAAAAB` (4/3, both halves, so any ratio fits), then `b 0x6076c8`.
  - `0xd93414` `movk x12,#0x3faa,lsl16`: replaces the high half only; the low half comes from an earlier instruction.
  - `0x607de8/dec` `movk x9,#0xd2ab,lsl32` / `movk x9,#0x67aa,lsl48`: not an aspect float; purpose unknown without the game's code.
- **How a frame ends:**
  1. Post-processing chain → `149.png`, then `150.png` (the composited scene in `vk 43` / `nvn 0x38`, 1920x1080).
  2. **UI** is drawn into that same 1080p image with vertex `af0ef3ac1bd35ad3` / fragment `5d53d90745ea1f7d`. The bag and X menu are 46 draws, all this pair.
  3. **Final copy**: vertex `389df45bf4d9a0f6` / fragment `f7c8611fd795251a` reads that image (texture 264) plus a 32x32 noise texture into the presented image.
- The overworld frames have no UI draws (no HUD on screen).
- **Battle capture (confirmed):** 3D composite → `110.png` (1 draw), then **one HUD pass of 58 draws into the same 1080p image** → `111.png`, then the final copy → `112.png`.
  - The HUD uses at least two shader pairs: `af0ef3ac…/5d53d907…` and `56af7f00aa776d97/431215e1a621c55c`. The capture lists only each pass's first 3 draws.
  - **Rule to use (by target, not by shader):** every draw into the image the final copy (`389df45b…`) reads, after the 3D composite's pass, is UI.
- **Inner screen** (log): swapchain **2504x2256** (aspect 1.11). UI squeeze `k = 1.11 / 1.778 ≈ 0.62`.
- **Plan for fixed UI scaling, in the graphics layer, no game-code patch:** for UI-shader draws into the composite target, scale the viewport and scissor
  vertically by `k = (W/H) / (16/9)`, centred. Present with the `fill` box. `touch_menu.c` tap mapping needs the same inverse transform.

# One engine for Galar, Arceus and Paldea Native

Branch `engine-unified` (local only) merges the three ports into one source tree, so renderer work (the GLES backend next) is written once instead of
cherry-picked across three repositories. It starts from Galar Native's `main` (389214a) and merges Arceus Native's `main` (def6392) and Paldea Native's
`main` (8e8fda7). Nothing here is pushed or released; each app's own `main` is untouched.

| Game | Profile | `port` | Shader chain (expected = its own main) | Chain generation |
| --- | --- | --- | --- | --- |
| Pokémon Shield | `01008DB008C2C000` | galar | bc2f6b2e5f0d = bc2f6b2e5f0d | live (`dm/gfx/tools`) |
| Pokémon Sword | `0100ABF008968000` (tables_from Shield) | galar | bc2f6b2e5f0d = bc2f6b2e5f0d | live |
| Pokémon Legends: Arceus | `01001F5010DFA000` | arceus | ba40200b20b8 = ba40200b20b8 | `pre-fmz` |
| Pokémon Violet | `01008F6008C5E000` | paldea | b72f9b70ebb2 = b72f9b70ebb2 | `pre-fmz` |
| Pokémon Scarlet | `0100A3D008C5C000` (tables_from Violet) | paldea | b72f9b70ebb2 = b72f9b70ebb2 | `pre-fmz` |

(Note the title IDs: `0100ABF008968000` is Sword and `01008F6008C5E000` is Violet.) `python3 dm/gfx/chains/chain_tree.py --verify` checks the table.

## What is shared

Everything: `dm/host`, `dm/gfx` (NVN layer, Vulkan replay, SPIR-V passes, frame generation, Lossless Scaling, FSR1), `dm/android/app/jni`
(android_main.c, touch_ui.c, touch_menu.c), the Java app, `setup_game.py`, `build_apk.py`, the language tables and the shader tools. A game differs only
through its profile (`dm/games/<id>/game.tsv` → `game_profile.h`), through build-ID checks that were already there (loader.c's patches only touch the
`main` build they were written for), or through a runtime title check in the Java app.

What each game gains from the others is listed under "Behaviour changes" below.

## Game-conditional switches added in this merge

All new switches come from the profile; none is a copy of code per game.

### Profile keys (`dm/games/game.py`, documented in `dm/games/README.md`)

| Key | Generated | Values | Why |
| --- | --- | --- | --- |
| `port` | `DM_PORT_GALAR / _ARCEUS / _PALDEA` | Shield galar, Arceus arceus, Violet paldea (Sword / Scarlet through `tables_from`) | the parts of android_main.c that are per app (below) |
| `docs_dir` | `DM_GAME_DOCS_DIR` | Galar Native / NXNative / Paldea Native | the shared save folder `Documents/<docs_dir>/<title id>/save` each app's releases already use (Galar hard-coded "Galar Native", Arceus "NXNative", Paldea took its app name). Players with their save in Documents keep it |
| `engine_quirks` | `DM_GAME_ENG_<NAME>` 0 / 1 | Arceus: all seven below; others none | where the two engines' defaults disagreed, each game keeps what its releases did |
| `xlate_chain` | (shader tools only) | Arceus, Violet: `pre-fmz` | reproduces each game's shader chain (below) |

### `engine_quirks` (each one keeps Arceus Native's released behaviour; Sword / Shield / Scarlet / Violet take Galar's)

| Quirk | Where | Arceus (on) | Galar (off) |
| --- | --- | --- | --- |
| `qc11_no_eds` | nvn_vk.c mtl_init | Qualcomm Vulkan 1.1 drivers: draw state built into each pipeline (Arceus #34 exploded polygons) | per-draw state; #209 handled with full barriers (old_qcom_driver), which both keep |
| `gpu_time_auto_only` | nvn_vk.c | GPU timestamps only with Graphics Auto | on by default except Mali / PowerVR |
| `frame_hold_off` | nvn_vk.c hold_on | DM_VK_FRAME_HOLD off by default (TF captures / report timestamps never run held) | mode 2 by default where the replay has its own thread |
| `async_pso_off` | nvn_vk.c apso_on | background pipeline builds off (Adreno 710 #34: objects missing) | on for Adreno, 3 workers, 8 ms wait |
| `framegen_game_rate` | nvn_vk.c frame generation | runs at the game's 30 or 60, floor = rate − 2 | needs a locked 60 |
| `nvn_async_never` | hle_nvn.c rq_on | the replay never on its own thread (crashed at loading screens, Arceus #3) | Auto: on for Adreno. **Without this quirk the merge would have turned it back on for Arceus** (hle_nvn.c merged cleanly to Galar's Auto) |
| `driver_selftests` | nvn_vk.c mtl_init | vtx_canary + tf_selftest at start | not run (unchanged start-up for Galar) |

### `#if DM_PORT_ARCEUS` in android_main.c

- The fps / heat guard and Graphics Auto (`dm_res_scale_ready` … `graphics_auto`): Arceus keeps its own ladder (handheld / TV mode, loader.c `arc_scale`,
  `lock30_draw`); the others keep Galar's (hle_resscale.c render scale, RES_START, battery saver). The two are whole blocks side by side, not interleaved.
- `DM_RES_CAP` set live: `dm_arc_scale_set` (Arceus) or `dm_res_cap_set` + Mode: Performance cap (others).
- The "frame generation locked off" boot rule (DM_UNLOCK_EXPERIMENTS) applies to every port but Arceus, whose releases offer frame generation;
  `GameSettings.experimentsUnlocked` agrees.
- Already there before: `DM_GAME_PROGRAM_ID == Arceus` blocks (Auto 30 fps, Faster rendering dropped, present interval), `arceus_pass_default` in nvn_vk.c.

### Other per-game choices

- `touch_ui.c target_at`: the touch-menu games (`GESTURES_ON`, profile `touch_menu`, Arceus) send the top left and every other free spot to the camera / menu
  taps and give the small back button a 2.5x touch area; Sword / Shield keep the top left dead and 1.3x for all, as before. Galar's floating sticks work in both.
- `barrier_relaxed` (nvn_vk.c): rewritten so both games get exactly what they had: relaxed on Adreno (not old stock Qualcomm), full elsewhere (Arceus #20 #31 #32);
  the Galar version taken as is would have relaxed barriers on Mali / Xclipse for Arceus.
- Java menu (GameMenu / GameSettings): Arceus-only entries stay Arceus-only (resolution `DM_ARC_RES`, No reflections, Lower specs, draw distance, style move
  effects, movement stick, touch camera, right stick hide, gyro aiming, barrier choice); Galar-only entries stay off Arceus (`DM_GALAR_RES`, Battery saver,
  Faster rendering). Texts that differed (Widescreen, frame generation notes, Lossless Scaling Auto, render scale note, Auto fps) are chosen per game.
- `GameSettings.docsDir` mirrors `docs_dir` for the save-folder entries (the language tables' key became `Documents/%s/%s/save, …`).

## Shader chains: generations instead of options

A chain version hashes the bytes of `dmsh_chain.py`, `dmsh_build.py`, `dmsh_fetch.py`, `dmsh_facts.py`, `dmsh_xlate/Program.cs`, its csproj, `ios_metal.py`,
`dmsh_chain.lock` (which holds the translator dlls' fingerprints), the game's tables and the options. Galar moved to the FMZ translator (Ryujinx
`InstEmitFloatArithmetic.cs`, `AstOptimizer.cs`, Program.cs retry, `XLATE_DEBUG` default 0); Arceus and Paldea did not. Making FMZ / debug a per-game *option* of
one translator is not possible without changing those files, and any change to them changes **every** game's chain (all players would have to set up again).

So the profile key `xlate_chain` picks a frozen **generation**: `dm/gfx/chains/pre-fmz/` holds exactly the files that differ (laid out as under `dm/`):
`gfx/tools/{dmsh_chain.py, dmsh_build.py, dmsh_chain.lock, dmsh_xlate/Program.cs, dmsh_xlate_bin/dmsh_xlate.dll, dmsh_xlate_bin/Ryujinx.Graphics.Shader.dll}`
and `third_party/ryujinx/src/Ryujinx.Graphics.Shader/{Instructions/InstEmitFloatArithmetic.cs, StructuredIr/AstOptimizer.cs}`, byte for byte from Arceus's
main (identical in Paldea's). They are data: never edit them. `dm/gfx/chains/chain_tree.py` lays them over the live tools:

- `chain_tree.py <id> <dir>` makes a tool tree; `--gen`, `--version`, `--header`, `--verify`.
- `build_apk.py` puts the game's generation into the APK's `dm.zip` in place of the live files and writes `gfx/tools/CHAIN`; it also refuses an APK whose tools
  compute another chain than `dmsh_chain.h`. (Simulated for all four profiles: the packed tools compute bc2f6b2e5f0d / ba40200b20b8 / b72f9b70ebb2 / b72f9b70ebb2.)
- `xlate_aot/build_xlate_aot.sh` takes `GAME=` and builds the phone translator from the generation's tree (`scripts/build_apk_pc.sh` passes it). Built both in
  WSL: the pre-fmz one has no "optimizer failed" retry string, the live one has.
- `setup_game.py` (checkout runs) and `vk_shaders.sh` use the game's generation; on the phone the APK already carries it.
- Violet's `dmsh_chain.h` is Paldea's again (b72f9b70ebb2); Galar's tree had moved it to 0e0f2f122ecb, which no released app uses.

When Arceus or Paldea move to the live translator: drop `xlate_chain` from the profile, `dmsh_chain.py --header`, and their players set up again (a release
note); once no profile names `pre-fmz`, delete `dm/gfx/chains/pre-fmz`.

## Behaviour changes (not gated: read before a release)

Arceus gains from Galar (Galar's engine work since 2026-11-10 that Arceus had not taken): pool-page copy by /proc/self/maps, PowerVR BC decoded, the Vulkan 1.0
baseline pieces it lacked, renderer phases 1-3 code (frame hold stays off by quirk), HUD-free and depth-guided frame generation and HUD-free Lossless Scaling,
threads named, load report / load profiler / loads on the fast cores (`DM_LOAD_BOOST`, `DM_LOAD_PROF` on by default), memory breakdown lines, descriptor set
reuse, Galar's newer frame recorder, the upload worker's batching (Galar's version replaced Arceus's own `pre_batch`), swapchain fallbacks, `DM_VK_PROBE` and
the other Mali test switches in the menu, crash-record vtables, Dynamic frame rate code.

Galar / Paldea gain from Arceus (engine-wide, defaults leave them inert unless noted): transform feedback as points / batching / `tf_end` before passes and clears
(only matters for games with TF captures), the TF check marker on Arm's driver, the Vulkan 1.0 y-flip route for drivers without `VK_KHR_maintenance1`
(was a red log line "picture may come out upside down"; now flipped in the vertex shader), camera-guided frame generation (no camera reader for Galar: off),
the freeze report (no frame for 10 s in the foreground dumps every thread's wait, logging only), the layout editor's reset is now "+ and − held together"
(Arceus #17: dragging + for 2 s reset the layout; Galar's menu text says so), focus loss also releases the editor / aim state, left-stick sensitivity and gyro
in the engine (menu shows them for Arceus only), `setup_game.py --installed` and `--flat-normals`, shader_split reads an already-unpacked main, crash.c's fault
kind on the Android Emulator, `DM_SCAN_F32` / `DM_POKE_F32` / `DM_VK_TRACE_FS` tests, the present-interval caller in the log.

Paldea gains all of Galar since 2026-11-12 (FSR1, frame recorder, Lossless Scaling HUD pass, Graphics → Mode, battery saver, heat guard, …) and keeps its own
`DM_SHADOWS=off`, `shadows_low`, the app name / repository in the log and `build_apk.py --official`.

## Verification (2026-11-21, WSL, NDK r27c)

- `make -f Makefile.host GAME=<id> lib` for all five profiles: builds; no unresolved engine symbols (`llvm-nm -D --undefined-only`).
- Chains: see the table; `chain_tree.py --verify` passes.
- Unit tests on Linux: see the report of this branch (host tests built with gcc per profile, gfx tests that are not Metal-only, `t_*.sh`, `dm/tests`).
- Not done: no APK packaged end to end (needs the CPython-for-Android / build-tools set-up of `build_apk_pc.sh`), nothing run on a phone or emulator.

## Not merged yet / open

- READMEs: the root README is Galar's. Arceus's and Paldea's READMEs are not merged (per-game READMEs to do); `docs/HANDOFF.md` took Arceus's edits as git merged them.
  Paldea's deletion of `site/` and `docs/DEVICES.md` was not taken (Galar's repo keeps them). Arceus's removal of `docs/handoff/arceus-lowspec_*` was taken.
- Releasing Arceus / Paldea from this tree is a product decision: the source of a release must be in that app's repository (GPL), so either this branch
  becomes each repo's `main`, or each repo gets a copy; package names stay `com.nxnative.pokemonshield` / `.pokemonlegendsarceus` / `.pokemonviolet`.
- Adoption candidates (one quirk / menu line each, after a phone test): `qc11_no_eds` and `driver_selftests` for Galar (#209 is the same bug), Arceus's touch
  camera / gyro / movement stick menu for Galar, Galar's battery saver for Arceus, the y-flip route on real Vulkan 1.0 drivers.
- `lang_src.py` is still stale; the `.tsv` tables were merged by union (Galar's text wins on the same key).
- Linux reference setups of a pre-fmz game need a host translator built from the generation's tree (`--bin`); the frozen dlls come with a macOS apphost wrapper.
- Phone tests for all three apps before any release from this tree.

## Cutting a release from this tree

`GAME=<id> [KS=<release keystore> OFFICIAL=1 VERSION=<n>] bash scripts/build_apk_pc.sh` (WSL), then the usual release steps of that app:

- Galar Native: `GAME=01008DB008C2C000` (runs Sword too) → GalarNative.apk.
- Arceus Native: `GAME=01001F5010DFA000` → ArceusNative.apk; its translator and shader tools are `pre-fmz`.
- Paldea Native: `GAME=01008F6008C5E000` (runs Scarlet too) → PaldeaNative.apk; `pre-fmz`.

Before: `python3 dm/gfx/chains/chain_tree.py --verify`; touch the `.c` files that include changed headers (Makefile.host misses header dependencies) or build
from a clean `out/`.

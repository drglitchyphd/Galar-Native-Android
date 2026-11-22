# One engine for Galar, Arceus and Paldea Native

Branch `engine-unified` (local only) merges the three ports into one source tree, so renderer work (the GLES backend next) is written once instead of
cherry-picked across three repositories. It starts from Galar Native's `main` (389214a) and merges Arceus Native's `main` (def6392) and Paldea Native's
`main` (8e8fda7). Nothing here is pushed or released; each app's own `main` is untouched.

Branch `beta-exit-quirks` (local only) is `beta-exit` 6e0e1dd with the driver probes and the debug layers merged in: `galar-driver-probes` ad0f638 (it holds
`debug-bundle` and `galar-vk10-validation`), then `arceus-debug-layer` b16bdd5 and `arceus-vk10-validation` 85e72fd; then `beta-exit` again at 3d21471
(2026-11-22: prewarm before play, zero-stutter, the budget tier, fg-plan's HUD pass, ...: "beta-exit since 6e0e1dd" below). Their per-GPU rules became one table
for all three apps ("Per-GPU rules" below); every default is beta-exit's on every GPU, for every game (6e0e1dd's, and background pipelines as beta-exit has them
since f09e58f), except the new rules listed under "Behaviour changes".

Branch `beta-exit-qm` (local only) is the combined head: `beta-exit-quirks` ef46af1 with `beta-exit` bcf1d76 (setup: each distinct shader translated once)
and `mali-all` e6f6d0c (the Mali white-screen fix for Sword / Shield on Arm's own driver) merged in. The fix's two per-GPU defaults moved into the same table
(`Q_B10_STANDIN`, `Q_MALI_WHITE_CHECK`, read through Shield's `engine_quirks` word `mali_lit_standin`): "The combined head" below.

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
| `port` | `DM_PORT_GALAR / _ARCEUS / _PALDEA` | Shield galar, Arceus arceus, Violet paldea (Sword / Scarlet through `tables_from`) | the parts of android_main.c that are per app (below); `gpu_id.port` names the app in the `[quirk]` log line (no table rule reads it) |
| `docs_dir` | `DM_GAME_DOCS_DIR` | Galar Native / NXNative / Paldea Native | the shared save folder `Documents/<docs_dir>/<title id>/save` each app's releases already use (Galar hard-coded "Galar Native", Arceus "NXNative", Paldea took its app name). Players with their save in Documents keep it |
| `engine_quirks` | `DM_GAME_ENG_<NAME>` 0 / 1 | Arceus: the first seven below; Shield (Sword through `tables_from`): `mali_lit_standin`; Violet / Scarlet none | where the two engines' defaults disagreed, each game keeps what its releases did, and a game's own GPU fix; the five that decide a GPU default reach vk_quirks.h as `gpu_id.eng` bits ("Per-GPU rules") |
| `xlate_chain` | (shader tools only) | Arceus, Violet: `pre-fmz` | reproduces each game's shader chain (below) |

### `engine_quirks` (each of the first seven keeps Arceus Native's released behaviour, Sword / Shield / Scarlet / Violet take Galar's; `mali_lit_standin` is Sword / Shield's own)

| Quirk | Where | Arceus (on) | Galar (off) |
| --- | --- | --- | --- |
| `qc11_no_eds` | vk_quirks.h `Q_NO_EDS` (through `gpu_id.eng`), nvn_vk.c mtl_init | Qualcomm Vulkan 1.1 drivers: draw state built into each pipeline (Arceus #34 exploded polygons) | per-draw state; #209 handled with full barriers (`Q_BARRIER_RELAXED`, old Qualcomm drivers), which both keep |
| `gpu_time_auto_only` | nvn_vk.c (`#if`: a setting, not a GPU rule) | GPU timestamps only with Graphics Auto | on by default except Mali / PowerVR (`Q_GPU_TIME_OFF`, both) |
| `frame_hold_off` | nvn_vk.c hold_on (`#if`) | DM_VK_FRAME_HOLD off by default (TF captures / report timestamps never run held) | mode 2 by default where the replay has its own thread |
| `async_pso_off` | vk_quirks.h `Q_ASYNC_PSO` (through `gpu_id.eng`), nvn_vk.c apso_on | background pipeline builds off (Adreno 710 #34: objects missing) | on for every GPU since beta-exit f09e58f (Adreno only before), 3 workers, 8 ms wait |
| `framegen_game_rate` | no reader since fg-plan f9ae686 | runs at the game's 30 or 60, floor = rate − 2 | the same since f9ae686 (frame generation is locked off by default for Sword / Shield; Scarlet / Violet also need `DM_FG_PALDEA=1`) |
| `nvn_async_never` | vk_quirks.h `Q_NVN_ASYNC` (through `gpu_id.eng`) for Auto; hle_nvn.c rq_on (`#if`) refuses the player's setting too | the replay never on its own thread (crashed at loading screens, Arceus #3) | Auto: on for Adreno. **Without this quirk the merge would have turned it back on for Arceus** (hle_nvn.c merged cleanly to Galar's Auto) |
| `driver_selftests` | vk_quirks.h `Q_DRIVER_SELFTESTS` (through `gpu_id.eng`), nvn_vk.c mtl_init | vtx_canary + tf_selftest at start | not run (unchanged start-up for Galar) |
| `mali_lit_standin` (Sword / Shield **on**; Arceus, Scarlet / Violet off) | vk_quirks.h `Q_B10_STANDIN`, `Q_MALI_WHITE_CHECK` (through `gpu_id.eng`), mali_white_vk.inc mw_init | off: Legends: Arceus draws its G-buffer and bloom in R11G11B10F (21-31 images a frame) and was never white this way | on Arm's own Mali driver: the lit R11G11B10F target drawn as an RGBA16F stand-in (route A) and the white-screen check with its ladder (route B, then the PanVK offer); mali-all e6f6d0c keyed it on `DM_PORT_GALAR` |

### `#if DM_PORT_ARCEUS` in android_main.c

- The fps / heat guard and Graphics Auto (`dm_res_scale_ready` … `graphics_auto`): Arceus keeps its own ladder (handheld / TV mode, loader.c `arc_scale`,
  `lock30_draw`); the others keep Galar's (hle_resscale.c render scale, RES_START, battery saver). The two are whole blocks side by side, not interleaved.
- `DM_RES_CAP` set live: `dm_arc_scale_set` (Arceus) or `dm_res_cap_set` + Mode: Performance cap (others).
- The "frame generation locked off" boot rule (DM_UNLOCK_EXPERIMENTS) applies to every port but Arceus, whose releases offer frame generation;
  `GameSettings.experimentsUnlocked` agrees.
- Already there before: `DM_GAME_PROGRAM_ID == Arceus` blocks (Auto 30 fps, Faster rendering dropped, present interval), `arceus_pass_default` in nvn_vk.c
  (since beta-exit-quirks only `DM_VK_UB_DEFER` reads it: uniform updates inside a pass deferred for Arceus; no GPU rule does).

### Other per-game choices

- `touch_ui.c target_at`: the touch-menu games (`GESTURES_ON`, profile `touch_menu`, Arceus) send the top left and every other free spot to the camera / menu
  taps and give the small back button a 2.5x touch area; Sword / Shield keep the top left dead and 1.3x for all, as before. Galar's floating sticks work in both.
- `barrier_relaxed` (nvn_vk.c): rewritten so both games get exactly what they had: relaxed on Adreno (not old stock Qualcomm), full elsewhere (Arceus #20 #31 #32);
  the Galar version taken as is would have relaxed barriers on Mali / Xclipse for Arceus. Since beta-exit-quirks the rule is vk_quirks.h's `Q_BARRIER_RELAXED`
  (`old_qcom_driver` became `gpu_is_old_qcom`), the same for every game.
- Java menu (GameMenu / GameSettings): Arceus-only entries stay Arceus-only (resolution `DM_ARC_RES`, No reflections, Lower specs, draw distance, style move
  effects, movement stick, touch camera, right stick hide, gyro aiming, barrier choice); Galar-only entries stay off Arceus (`DM_GALAR_RES`, Battery saver,
  Faster rendering). Texts that differed (Widescreen, frame generation notes, Lossless Scaling Auto, render scale note, Auto fps) are chosen per game.
- `GameSettings.docsDir` mirrors `docs_dir` for the save-folder entries (the language tables' key became `Documents/%s/%s/save, …`).

## Per-GPU rules: one table for every game (`dm/gfx/nvn/vk_quirks.h`, since beta-exit-quirks)

Every per-GPU default comes from one place. `nvn_vk.c mtl_init` describes the GPU once (`gpu_id`: vendor, driver ID or none, Vulkan version, driver version,
Arm's own driver release and beta, device name, a driver package or the phone's own driver, the phone's memory, dynamic rendering in use) together with the
game it runs, from its profile:

- `gpu_id.port`: the profile's `port` (`DM_PORT_*`; Sword and Scarlet through `tables_from`). It only names the app in the `[quirk]` log line (Galar Native,
  Arceus Native, Paldea Native): no rule reads it.
- `gpu_id.eng`: the profile's `engine_quirks` that decide a GPU default, one bit each, mapped by name: `qc11_no_eds` → `Q_NO_EDS`, `async_pso_off` →
  `Q_ASYNC_PSO`, `driver_selftests` → `Q_DRIVER_SELFTESTS`, `nvn_async_never` → `Q_NVN_ASYNC`, `mali_lit_standin` → `Q_B10_STANDIN` and
  `Q_MALI_WHITE_CHECK`. A game's own choice lives in its `game.tsv`: Sword / Shield would take Arceus's Qualcomm Vulkan 1.1 rule by adding `qc11_no_eds` to
  Shield's `engine_quirks`, nothing else; Scarlet / Violet would take the Mali white-screen fix by adding `mali_lit_standin` to Violet's.

`quirk_default(q, gpu_id)` is each site's default; the site still lets env.txt win with the same `DM_VK_*` switch as before, and `quirk_note` prints a
`[quirk]` line for each workaround in effect at start (the debug report copies them to `quirks.txt`), saying where its value came from: "default for this
GPU" (or "… in this game's profile" for the six rules that read `gpu_id.eng`), "env.txt" or "driver check". The first decision of a rule is kept, atomically
(the game's thread and the replay's can decide one lazily). `hle_nvn.c` asks through `nvn_vk_quirk_default`, which answers -1 before the device exists (as
6e0e1dd's `nvn_vk_gpu_name` test: `rq_on` takes that as Auto off) and, for the rules that read the driver ID or Arm's release (`quirk_late`), until `mtl_init`
has read them just after the device is made; t_quirks.c checks that no other rule reads those.

Settings start-up order (android_main.c `host_start`, each file's keys set only when not set: `env_file.h`): env.txt (the player's), then the safe-start
ladder's level (safe_start.h), then `driver-check.env` (the driver check's settings for this driver, DriverCheck.java), then the phone profile. So the player's
settings and the ladder's levels win over the driver check (ladder level 5's `DM_VK_FLOAT_CONTROLS=0` over the check's 1, #89). android_main.c names the keys
it took from `driver-check.env` in `DM_DRIVER_CHECK_KEYS`, and nvn_vk.c takes those values as this driver's defaults, not as the player's (vk_quirks.h
`vkq_env_src` / `vkq_env_player`): the crash markers that only the player's own setting skips still apply to them (`float_controls_crashed`,
`bc_gpu_crashed`, `heap_import_crashed`), and their `[quirk]` line says "driver check". The check writes `DM_VK_FLOAT_CONTROLS`, `DM_VK_DMAHEAP`, `DM_VK_BC_GPU`,
`DM_VK_CLEAR_BY_PASS` (table rules) and `DM_VK_ZCOPY`, `DM_VK_INT_TEX`, `DM_VK_NO_LAYERED_3D` (switches of their own code paths). Where a test records what the
engine records, it asks the table too, on the GPU as `mtl_init` describes it (selftest.c `st_gpu`): `float-controls` (`Q_FLOAT_CONTROLS`) and `3d-layered`
(`Q_CLEAR_BY_PASS`: how a clear inside the layered pass is recorded).

The other `engine_quirks` decide nothing per GPU and stay `#if DM_GAME_ENG_*` at their sites: `frame_hold_off`, `gpu_time_auto_only`
(timestamps only with Graphics Auto: a setting), `nvn_async_never`'s refusal of the player's own setting; `framegen_game_rate` has no reader (above).
GPU tests left outside the table on purpose are listed at the top of vk_quirks.h: building `gpu_id` itself, what is decided before a driver is open
(android_main.c chip profiles, `panvk_defaults` and the driver check's copy of it, the safe-start ladder, the heat guard's kgsl clock, `mali_env_migrate`:
env.txt's float-target "1"s an older Mali tests menu left, taken out on phones whose `gpu.txt` says Mali), the driver check's own self-tests (they run on every
GPU), the Java menu's GPU sections, texts and the white-screen ladder's PanVK offer (`MaliLadder.java`), the ladder's own state file (kept per driver ID and
version), test switches (`DM_VK_AS_MALI`, `DM_VK_DEVICE`), and the frame hold, which follows the replay thread's decision.

What each app runs by default (env.txt still wins). "Arm's own driver": by driver ID where the driver gives one, else Arm's vendor ID and not a driver package
(PanVK is not). Bold: the games differ, each by its own profile.

| Rule (`vk_quirks.h`) | Switch | Sword / Shield | Legends: Arceus | Scarlet / Violet |
| --- | --- | --- | --- | --- |
| `Q_NO_DYNREN` render passes, not dynamic rendering | `DM_VK_NO_DYNREN` | Adreno | Adreno | Adreno |
| `Q_BARRIER_RELAXED` fewer full barriers before passes | `DM_VK_BARRIER` | Adreno, but not Qualcomm's own driver before Vulkan 1.3 (Turnip is not one) | same | same |
| `Q_GATHER_BIAS`, `Q_PASS_DEFER`, `Q_IMPORT_MARK_OFF` | `DM_VK_GATHER_BIAS`, `DM_VK_PASS_DEFER`, (`DM_VK_HOST_IMPORT`) | Adreno | same | same |
| `Q_ASYNC_PSO` pipelines built in the background | `DM_VK_ASYNC_PSO` | every GPU (beta-exit f09e58f, 2026-11-22; Adreno only before) | **never** (`async_pso_off`, #34) | every GPU |
| `Q_NO_EDS` draw state in each pipeline | `DM_VK_NO_EDS` | never | **Qualcomm Vulkan 1.0 / 1.1** (`qc11_no_eds`, #34) | never |
| `Q_NVN_ASYNC` Faster rendering's Auto (the replay's own thread) | `DM_NVN_ASYNC` | a GPU name holding "Adreno" (Turnip too) | **never**, the player's setting refused too (`nvn_async_never`, #3) | as Sword / Shield |
| `Q_DRIVER_SELFTESTS` vertex canary and TF self-test at start; `Q_TF_SELFTEST` within them | —, `DM_VK_TF_SELFTEST` | not run | **run**; the TF self-test on Arm's own driver (`driver_selftests`) | not run |
| `Q_HOST_IMPORT_OFF`, `Q_CLEAR_BY_PASS` | `DM_VK_HOST_IMPORT`, `DM_VK_CLEAR_BY_PASS` | Xclipse | same | same |
| `Q_DMAHEAP` | `DM_VK_DMAHEAP` | Arm's or Samsung's vendor ID | same | same |
| `Q_DMABUF_TRY`, `Q_ARM_FORMATS` | —, `DM_VK_ARM_FORMATS` | Arm's vendor ID (PanVK too) | same | same |
| `Q_PIPE_KEEP` at most 800 pipelines | `DM_VK_PIPE_KEEP` | Arm's vendor ID, or a phone under 7.5 GB | same | same |
| `Q_GPU_TIME_OFF` no GPU timestamps | `DM_VK_GPU_TIME` | Mali, PowerVR | same; on the others **only with Graphics Auto** (`gpu_time_auto_only`, at the site) | as Sword / Shield |
| `Q_FLOAT_CONTROLS` | `DM_VK_FLOAT_CONTROLS` | Arm's and Samsung's own drivers (without driver properties: their vendor IDs) | same | same |
| `Q_G615_SAFE` uniform snapshots, BC decoded | `DM_VK_UBO_SNAPSHOT`, `DM_VK_BC_TRANSCODE` | a Mali-G615 on Arm's driver before r49, not a package | same | same |
| `Q_CLAMP_ZERO` | `DM_VK_CLAMP_ZERO` | Arm's own r38p1-r49p0, beta r49p1, r50p0-r51p0 | same | same |
| `Q_NAN_MINMAX` (2), `Q_EXACT_MATH`, `Q_INIT_LOCALS`, `Q_TF_CHECK`, `Q_QUERY_LOD` (new) | `DM_VK_NAN_MINMAX`, `_EXACT_MATH`, `_INIT_LOCALS`, `_TF_CHECK`, `_QUERY_LOD` | Arm's own driver | same | same |
| `Q_VIEW_QUARANTINE` (2048 views kept back) | `DM_VK_VIEW_QUARANTINE` | Arm's own driver with dynamic rendering | same | same |
| `Q_TEX_HALF_UNSAFE` (the profile's half textures dropped) | `DM_VK_TEX_QUALITY` | Arm's own driver before r49 | same | same |
| `Q_BC_GPU_OFF`, `Q_BC_DECODE` BC on the CPU, decoded | `DM_VK_BC_GPU`, `DM_VK_BC_TRANSCODE` | PowerVR | same | same |
| `Q_PCACHE_GAPS_OFF` | — | PowerVR (render passes) | same | same |
| `Q_B10_STANDIN` the lit R11G11B10F target as an RGBA16F stand-in (route A); `Q_MALI_WHITE_CHECK` the white-screen check and its ladder | `DM_VK_B10_AS_RGBA16`; `DM_MALI_LADDER`, `DM_MALI_WHITE_SIM` | **Arm's own driver** (by its release: not PanVK) (`mali_lit_standin`, #47 #110 #111 #172) | **never** | **never** |

Checks: `dm/gfx/tests/t_quirks.c` compares the table with beta-exit's per-site expressions (copied, with their line numbers): 6e0e1dd's for every rule (each of
those 33 lines is the same line at beta-exit 3d21471), and 3d21471's for `Q_ASYNC_PSO` (f09e58f: `!DM_GAME_ENG_ASYNC_PSO_OFF`, no vendor test). It does so for each
port's profile over 2.57 M GPU descriptions each (vendors, driver IDs or none, Vulkan 1.0-1.4, Arm releases r20p0-r55p2 with and without beta, driver packages,
names, memory sizes, dynamic rendering), then every port with every combination of the `eng` bits (no rule reads the port), reads the five `game.tsv` files, and
pins players' phones for each game: 454.4 M comparisons (32 rules), equal everywhere; `Q_QUERY_LOD` is the one rule without a beta-exit counterpart, and
`Q_B10_STANDIN` / `Q_MALI_WHITE_CHECK` take mali-all e6f6d0c's `mw_default_route` / `mw_enabled` as their reference (the game read from the profile's
`mali_lit_standin`, and for the real profiles also from e6f6d0c's own `DM_PORT_GALAR`). 6e0e1dd's
background-pipeline rule stays in the test too: the table differs from it exactly on the GPUs other than Adreno for Sword / Shield and Scarlet / Violet (2.32 M of
the 2.57 M descriptions each) and nowhere for Legends: Arceus. It also checks, over the same grid,
that only the `quirk_late` rules change when the driver ID and Arm's release are not known yet and that `Q_FLOAT_CONTROLS` and `Q_CLEAR_BY_PASS` read the vendor
and driver ID alone (the driver check asks them from those), and where a setting came from (`DM_DRIVER_CHECK_KEYS`): 739.4 M checks in all.
`simdev/t_vkdev.c`, built for each game, checks the decisions `mtl_init` makes against the same rules on the 42 simulated GPUs, every rule decided at its site
so far against the table, the GPU timestamps site, `nvn_vk_quirk_default` before, while and after the device is made, (`SIMDEV_DC_FC`) a driver-check value
under a crash marker, how `tex_view_for` binds integer textures under a float sampler (with and without `DM_VK_INT_TEX_EMPTY` / `DM_VK_INT_TEX=keep`), and the
white-screen fix's route and check against e6f6d0c's own expectation (Arm's own driver with Sword / Shield);
`t_safe_start` checks the settings order with `driver-check.env`.

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

### 6e0e1dd's changes that the tables above make explicit (not new in beta-exit-quirks)

Found while copying 6e0e1dd's rules into t_quirks.c; 6e0e1dd already had them, the lines above did not name them:

- Sword / Shield and Scarlet / Violet: the BC layer is off on PowerVR (BC converted on the CPU), as for Arceus (#32). Galar 389214a / v0.94 and Paldea 8e8fda7
  had it on there.
- Legends: Arceus: `spv_exact_math` and `spv_init_locals` on Arm's own driver (Galar's #47 work); def6392 / v0.44 had neither.
- Scarlet / Violet (within "Paldea gains all of Galar"): exact math and zeroed registers on Arm's own driver, PowerVR BC decoded, PowerVR pipelines with a colour
  gap made without the cache, full barriers on Qualcomm's own drivers before Vulkan 1.3, GPU timestamps on by default (not on Mali / PowerVR), Faster
  rendering's Auto on Adreno (8e8fda7 had it locked off) and the frame hold (mode 2) with it.
- `framegen_game_rate` is read by nothing since fg-plan f9ae686: every game's frame generation floor is its own rate − 2.

### beta-exit since 6e0e1dd (3d21471, merged into beta-exit-quirks on 2026-11-22; not new in beta-exit-quirks)

- Background pipelines on every GPU (f09e58f, zero-stutter; the owner's call): `DM_VK_ASYNC_PSO` is on by default on Mali, Xclipse, PowerVR and Turnip as on
  Adreno, for Sword / Shield and Scarlet / Violet; Legends: Arceus keeps `async_pso_off` (off on every GPU). It is the one per-GPU default beta-exit changed
  since 6e0e1dd, and here it is the table's: `Q_ASYNC_PSO` = `!(eng & ENG_ASYNC_PSO_OFF)`, no vendor test (apso_on asks `quirk_default` and notes it as before).
  t_quirks.c takes that rule from 3d21471's line; t_vkdev.c expects `apso == !DM_GAME_ENG_ASYNC_PSO_OFF`, as beta-exit's does.
- Nothing else beta-exit added since 6e0e1dd is a per-GPU default, so nothing else went into the table: the prewarm before play (`DM_VK_PREWARM`, threads by
  cores and memory; the shipped list's pipelines left out where this GPU lacks a format, `pso_formats_ok`: a format feature query), the compute prewarm
  (`DM_VK_PREWARM_COMPUTE`), `prewarm_pace_play` and the long-frame log's waits (a0f6d9f), one-off uploads not waited for (`DM_VK_ONCE_WAIT=1`: waited), HUD-free
  Lossless Scaling's module per output format (`ls_hud_pipe`: by the format's storage-without-format feature), the game's memory-pool dump (hle_sdk16.c LogImpl,
  f0035ae; AArch64 Linux / Android only), the budget tier (android_main.c: 35 % under the cap of the Graphics level the phone profile picked; that profile's chip
  table is one of vk_quirks.h's deliberate raw checks), main threads pinned without a hint session, the shader pack storing equal blobs once (3d21471,
  `spv_pak.py`: same format, same chains). The 33 rule-site lines t_quirks.c copies from 6e0e1dd are the same lines at 3d21471.
- Conflicts: `nvn_vk.c apso_on` (the table's decision kept, beta-exit's rule moved into `Q_ASYNC_PSO`), `simdev/t_vkdev.c` (beta-exit's background-pipeline
  expectation, this branch's barrier / EDS checks), `overlay/build_spv.sh` (`ls_hud_spv.h` is `build_comp_spv.sh`'s since beta-exit-hud; the `zcopy_spv.h` section
  kept). `game.py`'s note for `async_pso_off` now says what it turns off.
- The gates need no temporary patches any more: 95d0777's `dm_gpu_ts614` stub (`dm/pc/pc_host.c`) and 3812695's `golden_rec.c` link (`build_vkdev.sh`) are merged.

### New in beta-exit-quirks (the merged branches; every game unless noted)

- `Q_QUERY_LOD` (`spv_query_lod`, the one new table rule): on Arm's own driver textureQueryLod's level component becomes clamp (LOD, 0, levels − 1); Arm's driver
  gives floor (LOD) (dEQP-VK texturequerylod, Mali-G615 MC6). A load-time SPIR-V pass: no new setup, the shader chains are unchanged. `DM_VK_QUERY_LOD=0 / 1`.
- Vulkan validation fixes (the S25+ layer run), on every GPU: `spv_strip_clipcull` drops the unused ClipDistance[8] / CullDistance[1] of every vertex shader at
  load (over the limit of 8); a scaled / mirrored / format-changing copy into a depth / stencil format that is no blit destination (D24S8 on Adreno) is drawn
  (`zcopy_draw`; `DM_VK_ZCOPY=0`: the blit); a 32-bit integer texture read as another numeric class (`tex_view_for` / `fmt_as_class`; `DM_VK_INT_TEX=keep`: its
  own format always), as the table below.
  - `zcopy_draw` begins its pass through `rend_begin`, as the game's passes: in a held frame (`DM_VK_FRAME_HOLD=2`, the default with Faster rendering's Auto on
    Adreno, Sword / Shield and Scarlet / Violet) the command buffer outside a pass is a secondary, where the branch's `vkCmdBeginRenderPass` was invalid with
    Adreno's render passes (VUID-vkCmdBeginRenderPass-bufferlevel); the copy is now a segment of its own, its pass begun in the primary at the flush. And it
    tells `draw_`'s pipeline-bind cache (`g_gfx_bind_gen`), so the game's next draw binds its own pipeline again instead of drawing with the copy's. simdev's
    t_vkdev draws a copy in both kinds of submission (`run_simdev.sh all` runs it again with `DM_VK_FRAME_HOLD=2`); with the branch's code the held run
    reported VUID-vkCmdBeginRenderPass-bufferlevel / -EndRenderPass-bufferlevel on the Adreno 830 and 710 profiles (render passes), with this one none.
  - An integer texture read as another class gets a view of that class or, as 6e0e1dd did in every case, its own format. "Read as" is the shader's declared
    class, and for a sampled texture that is always float: the translator (Ryujinx's SPIR-V backend) declares every sampler float, since `QuerySamplerType`
    carries no component type, and takes the result's bits with a bitcast where the Maxwell code does integer work on them. So an 8 / 16-bit integer texture
    under a float sampler says nothing about how the shader uses it:

    | Texture (its view format) | Read as | Its image's formats (nvn_vk.c `tex_new`) | Outcome |
    | --- | --- | --- | --- |
    | 32-bit channels: R32 / R32G32 / R32G32B32A32 UINT or SINT | float, or the other signedness | mutable without a format list: 3D images, `DM_VK_NO_FMT_LIST`, and outside Mali a format that cannot be a render target or a driver without format lists (Vulkan 1.1 without `VK_KHR_image_format_list`) | **the same bits** in a view of the shader's class (new) |
    | the same | the same | fixed: not mutable (every plain colour image on Mali, `Q_ARM_FORMATS`: Arm's driver and PanVK), or listing only its own format and sRGB twin (render-target formats wherever format lists exist: Adreno, Xclipse, most Vulkan 1.2 drivers) | its own format, as 6e0e1dd (a view of another class would be invalid there; undefined in Vulkan, the layer still names it) |
    | 8 / 16 / 10-bit integers | float (every sampled one) | any | its own format, as 6e0e1dd: most drivers return the texel's own bits, what a shader that uses them as an integer gets on the Switch (Shield's `fp_t_tcb_8` is the one seen). `DM_VK_INT_TEX_EMPTY=1`: the empty texture of its class instead (0, the Switch's answer only where the shader does float work on them: the flushed denormals), to compare |
    | 8 / 16-bit integers | the other signedness | any | its own format, as 6e0e1dd: a view of the other signedness extends the sign the other way (R16_UINT 0xffff read as -1, the Switch gives 65535) |
    | float, UNORM, SNORM, sRGB (or BC decoded here) | uint / sint | any | its own format, as 6e0e1dd (not seen in Sword / Shield yet) |

    galar-vk10-validation viewed 8 / 16-bit integers as the other signedness, and on fixed-format images read 32-bit ones as 0 although its notes said "the
    same bits": both are 6e0e1dd's binding again. It also gave every 8 / 16 / 10-bit integer under a float sampler the empty texture, on every GPU and in every
    game; since the sampler's class is float for all of them, that hit the shaders that take the bits as an integer too, so the fix round after the merge
    (below) made that 6e0e1dd's binding again and the empty texture an opt-in (`DM_VK_INT_TEX_EMPTY=1`). The driver check's `int-view` test reads 32-bit textures
    through float and other-signedness views of mutable images (where the engine makes them; a failure writes `DM_VK_INT_TEX=keep`), and `int-view-own` reports
    what the driver returns for the old binding: a 32-bit integer read as float on a non-mutable and a format-listed image, and a 16-bit integer read as int and
    as float on every kind of image (information only, no setting).
- The driver check before Play (`DriverCheck.java`, `DriverCheckService` in its own `:drvcheck` process, `libdmprobe.so`: built by the `lib` target and
  `win_build_lib.py`, optional in the APK): the probe app's GPU self-tests on the game's own driver, once per driver / system build / app version (and
  `DriverCheck.VERSION`, 4), crash- and hang-safe (a journal, up to 4 attempts, 90 s; Play always follows; one check at a time; a check the app itself closed
  during is resumed from its journal at the next Play, and given up after 2 such starts: `driver-check.run`). What it finds goes to `driver-check.env`, read after
  env.txt and the safe-start ladder and before the phone profile (`DM_DRIVER_CHECK=0`: not read; "Per-GPU rules" above): `DM_VK_DMAHEAP=0`, `DM_VK_BC_GPU=0`,
  `DM_VK_ZCOPY=0` or `DM_VK_INT_TEX=keep` after a test that crashed or failed, `DM_VK_NO_LAYERED_3D=1` after a crash (or a lost device) in `3d-layered`'s
  layered pass, `DM_VK_CLEAR_BY_PASS=1` after a crash or a lost device in its clear inside that pass, or that clear drawn wrong with the rest right,
  `DM_VK_FLOAT_CONTROLS=1` where a driver is right only with float controls. `3d-layered` records what the engine records on the driver: its passes with
  dynamic rendering where the engine uses it (`Q_NO_DYNREN`, `DM_VK_NO_DYNREN`, `DM_VK_BASELINE`), the image with the engine's usage, and the engine's own
  clear (`clear_in_pass`: the pass's layer 0, a part of it) as it records it there (`Q_CLEAR_BY_PASS`, or env.txt's `DM_VK_CLEAR_BY_PASS`): on Xclipse a pass of
  its own on that slice, not the `vkCmdClearAttachments`
  that crashed the Xclipse 940 (driver 24.0.534) and 540 (24.0.560), which as first written made the check write `DM_VK_NO_LAYERED_3D=1` there and draw every
  3D target into slice 0 alone (the fix round below). A test can name another setting for a later step (selftest.c `crash_fix`: a second "begin" line in the
  journal; the app applies the last one a lost process named). Those are this driver's defaults: the player's env.txt and the ladder win over them, the
  engine's crash markers still apply to them, and
  their `[quirk]` line says "driver check" (the debug report holds `driver-check.env`). A test that only stops answering for 20 s (hung, or slow: the BC layer's
  pipelines on an old Mali) is left out of the next attempt without its setting, and named in `driver-check.txt` (the game's own markers, `bc_gpu_crashed`
  and `heap_import_crashed`, still cover a hang the player ends). A driver change drops every game's results; menu: "Check the graphics driver again".
  Scarlet / Violet get it too (shared app code; no branch targeted Paldea). Its report no longer calls itself Galar Native's in the other apps.
- The debug report (`DebugBundle.java`; ☰ → Bug reports → Make a debug report, and offered once after a run that crashed, hung or was ended for memory on
  screen): one ZIP in Download and a pre-filled
  GitHub issue (Galar-Native-Android's issue form; Arceus's and Paldea's repositories get a plain body). With it: Android tombstones after our crash record
  (`crash.c` hands the signal on to debuggerd; `DM_CRASH_CHAIN=0`: the old `_exit`), `DM_TEST_CRASH=<s>`, GPU breadcrumbs (`vk_crumbs.h`, printed on device
  lost, a failed submit, a 10 s hang or a crash; `DM_VK_BREADCRUMBS=1`: the GPU marks each pass, menu "GPU crash tracing"; in a held frame the mark goes into
  the gap after the pass, so with it on those passes are not run as one), a `[mem-by]` status line every 30 s beside 6e0e1dd's smaps breakdown, the `[quirk]`
  lines, `tools/read_debug_report.py`. The breadcrumbs' bookkeeping on every pipeline bind copies the two shader keys (no formatting).
- Old-driver mode (`DM_VK_BASELINE=1`, menu): the Vulkan 1.1 floor on any GPU (render passes, draw state in each pipeline, a fence per submission).
- The four menu entries above are in the shared system menu of all three apps.

Not taken from the branches: arceus-debug-layer b16bdd5 was built on Arceus v0.44 (3c5c6d1), older than arceus-renderer: its table kept dynamic rendering on
Adreno for Arceus, never relaxed barriers for Sword / Shield / Scarlet / Violet, relaxed them on old Qualcomm drivers for Arceus and keyed the EDS and async
pipeline rules on the port; galar-driver-probes' barrier site relaxed Arceus on Mali / Xclipse / PowerVR (`arceus_pass_default`); both tables had the PowerVR BC
layer off for Arceus only; arceus-vk10-validation added a second query-LOD pass outside the table. 6e0e1dd's rules hold instead (t_quirks.c).

## Verification (2026-11-21, WSL, NDK r27c)

- `make -f Makefile.host GAME=<id> lib` for all five profiles: builds; no unresolved engine symbols (`llvm-nm -D --undefined-only`).
- Chains: see the table; `chain_tree.py --verify` passes.
- Unit tests on Linux: see the report of this branch (host tests built with gcc per profile, gfx tests that are not Metal-only, `t_*.sh`, `dm/tests`).
- Not done: no APK packaged end to end (needs the CPython-for-Android / build-tools set-up of `build_apk_pc.sh`), nothing run on a phone or emulator.

## Verification of beta-exit-quirks (2026-11-22, WSL, NDK r27c)

- `make -f Makefile.host GAME=<id> lib` from a clean `out/` for all five profiles: `libnxnative.so` and `libdmprobe.so` build and link (`--no-undefined`);
  nvn_vk.c, hle_nvn.c and the probe compile without warnings for each port.
- `chain_tree.py --verify`: bc2f6b2e5f0d (Shield, Sword), ba40200b20b8 (Arceus), b72f9b70ebb2 (Violet, Scarlet), unchanged.
- Linux tests pass: t_quirks (328.6 M comparisons), t_crumbs, t_fgen_vk, t_lshud_vk, t_pool_pages, t_pool_index, t_xf_learn, t_fg_rec, t_fb_cache, t_hmap,
  t_cover, t_pace, t_fstep, t_fmz.sh, t_clamp_zero.sh, t_nan_minmax.sh, t_safe_start, and dm/tests' test_android_shader_map, test_build_apk, test_spv_pak.
  test_shader_split_capture.py fails as on 6e0e1dd (it expects a [RED] line for an empty capture folder that shader_split.py does not print).
- Golden frames: 6e0e1dd's PC tools do not link (`dm_gpu_ts614`; beta-exit 95d0777 adds the stub). With that stub applied for the run only,
  `golden_check.py synth` passes for Shield's, Arceus's and Violet's engines.
- simdev (lavapipe and the 42 profiles; t_vkdev built for Shield, Arceus and Violet; on 6e0e1dd t_vkdev needs beta-exit 3812695's golden_rec.c to link, applied
  for the run only): no t_vkdev check fails in 129 runs. t_lshud_vk's VUID-vkCmdDispatch-OpTypeImage-07027 on 24 profiles and, for Arceus only, its transform
  feedback self-test's VUID-RuntimeSpirv-NonWritable-06341 / VUID-vkCmdDraw-colorAttachmentCount-06179 on the 17 Mali profiles are the same on 6e0e1dd. (There
  Arceus's t_vkdev also failed on the 16 Adreno / Turnip profiles: the test expected background pipelines; it now reads async_pso_off.)
- Not done: no APK packaged, nothing run on a phone or emulator.

After the review (2026-11-22; the depth copy in held frames, draw_'s bind cache, the integer-texture views, the driver check's order and values, the table's
late rules and notes):

- `make -f Makefile.host GAME=<id> lib` from a clean `out/` for all five profiles; nvn_vk.c, hle_nvn.c, android_main.c and libdmprobe.so compile without
  warnings for Shield, Arceus and Violet. The Java app compiles (javac 21, android-34). `chain_tree.py --verify` unchanged.
- t_quirks: 567.3 M checks pass (parts 5 and 6 new); its mutation runs (rules drifting from 6e0e1dd, a rule left out of `quirk_late` or added without reading
  a late field, `Q_FLOAT_CONTROLS` reading the API version, `vkq_env_src` matching a key's prefix, a profile change) each fail. t_safe_start 23 checks (the
  settings order with `driver-check.env`), t_crumbs, and the other Linux tests as above; test_shader_split_capture.py fails as on 6e0e1dd.
- simdev, `run_simdev.sh all` for Shield, Arceus and Violet (516 runs): t_vkdev and t_vkdev_held (`DM_VK_FRAME_HOLD=2`) pass on all 43 devices for each game;
  the depth copy drawn right in both kinds of submission on the 41 devices with Vulkan 1.1 or newer (D24S8 or D32S8; render passes on 24 of them: Adreno and
  the drivers without dynamic rendering), with no validation error; the validation errors left are the ones listed above, unchanged. With the branch's
  `zcopy_draw` restored for a control run, t_vkdev_held reported VUID-vkCmdBeginRenderPass-bufferlevel and -EndRenderPass-bufferlevel on the Adreno 830 and 710
  profiles. `SIMDEV_DC_FC=1` (the driver check's `DM_VK_FLOAT_CONTROLS=1` with a `float_controls_crashed` marker) leaves float controls off, noted "driver
  check", and `=2` (the same from env.txt) asks for them, on 7 devices for each game.
- The probe's self-tests built for the PC and run on lavapipe: `int-view` passes; `int-view-own` reports lavapipe's own-format reads as the Switch's bits on
  the non-mutable and the format-listed image (and the 16-bit integers zero-extended); its three VUID-vkCmdDispatch-format-07753 are that test's deliberate
  mismatches.
- Golden synthetic round trip (with 95d0777's stub for the run only): PASS for Shield's, Arceus's and Violet's engines.

## Verification after merging beta-exit 3d21471 (2026-11-22, WSL, NDK r27c; it also re-checks the review fixes above)

- `make -f Makefile.host GAME=<id> lib` for all five profiles (the files that include `vk_quirks.h` touched, from a clean `out/`): `libnxnative.so` and
  `libdmprobe.so` link (`--no-undefined`), no undefined `dm_` / `nvn_` / `mtl_` symbol, `nvn_vk_prewarm_progress` and `nativePrewarm` exported. nvn_vk.c,
  hle_nvn.c, android_main.c, hle_resscale.c and libdmprobe.so compile without warnings for Shield, Arceus and Violet (hle_sdk16.c's `-Wframe-address` is
  beta-exit's own file). The Java app compiles (javac 17, android-35). `chain_tree.py --verify`: bc2f6b2e5f0d / ba40200b20b8 / b72f9b70ebb2, unchanged.
- t_quirks: 575.1 M checks pass. Mutation runs each fail: background pipelines back to Adreno only, ignoring `async_pso_off`, keyed on the port, off on
  PowerVR; the earlier barrier and replay-thread ones; Shield's `game.tsv` with `async_pso_off`. A line-for-line check finds each of the 33 rule-site lines
  t_quirks.c copies from 6e0e1dd unchanged at 3d21471, and the background-pipeline line at 3d21471:3964.
- Linux tests pass: t_fgen_vk, t_lshud_vk, t_fgcut_vk, t_fg_score, t_fgen, t_pool_pages, t_pool_index, t_xf_learn, t_fg_rec, t_fb_cache, t_hmap, t_cover,
  t_pace, t_fstep, t_quirks, t_crumbs, t_fmz.sh, t_clamp_zero.sh, t_nan_minmax.sh, test_tbdr_budget.py (with `DM_GAME` set, Shield and Sword: its tool needs a
  profile), t_safe_start (23 checks), test_android_shader_map, test_build_apk, test_spv_pak. test_shader_split_capture.py fails as on beta-exit and 6e0e1dd.
  t_resscale (beta-exit's, unchanged here) needs the Android compat and is not built on Linux.
- Golden synthetic round trip, no temporary patch: PASS for Shield's, Arceus's and Violet's engines on the default route and with `DM_VK_API=10`,
  `DM_VK_MIN_FEATURES=1` and `DM_VK_NO_DYNREN=1`, and as Sword and as Scarlet (14 runs).
- simdev, `run_simdev.sh all` for Shield, Sword, Arceus, Violet and Scarlet (860 runs, 43 devices): t_vkdev, t_vkdev_held and t_fgen_vk pass everywhere, t_lshud_vk
  passes on 25 and skips on 18 (HUD-free off where the frame's format cannot be written without a format, beta-exit-hud); the depth copy drawn in both kinds of
  submission on the 41 devices with Vulkan 1.1 or newer; background pipelines started (3 workers; `[quirk]` "default for this GPU in this game's profile") on all
  43 devices for Sword / Shield / Scarlet / Violet, on none for Legends: Arceus. No validation error for Sword / Shield / Scarlet / Violet (t_lshud_vk's
  VUID-vkCmdDispatch-OpTypeImage-07027 is gone with beta-exit-hud's per-format modules); for Legends: Arceus only its transform feedback self-test's
  VUID-RuntimeSpirv-NonWritable-06341 / VUID-vkCmdDraw-colorAttachmentCount-06179 on the 17 Mali profiles, the same on 3d21471 itself (a control run).
  `SIMDEV_DC_FC=1` / `=2` on 7 devices for Shield, Arceus and Violet: the driver check's float controls under a crash marker stay off ("driver check"), the same
  value from env.txt asks for them.
- The probe's self-tests on the PC (lavapipe): 10 pass, 1 warn (float controls), 1 skip (dma-buf heap); `depth-copy` and `int-view` pass. Validation: `int-view-own`'s
  deliberate VUID-vkCmdDispatch-format-07753, and VUID-vkCmdDraw-None-09600 from `fallback-draw` (found by leaving one test out at a time: its render pass
  starts its attachments in GENERAL while the images are still UNDEFINED; the test is beta-exit's own, unchanged since 6e0e1dd: left alone here).

## Fix round after the merge (2026-11-22, WSL, NDK r27c)

The review of 7cfb4fe found two real problems, fixed here. A third finding, the depth copy for Legends: Arceus, needs no code change; it is on the open list
below for the phone pass.

- The driver check's `3d-layered` recorded `vkCmdClearAttachments` inside a layered pass over a 3D image on every driver. That is the call that crashes
  Samsung's Xclipse 940 (driver 24.0.534) and 540 (24.0.560), and the engine never makes it there (`Q_CLEAR_BY_PASS`). A crash there wrote
  `DM_VK_NO_LAYERED_3D=1`, and the games then drew every 3D render target into slice 0 alone (Violet's colour-grading tables: black 3D scenes). The test now
  records that clear as the engine does on the driver. On another driver, a crash in that call asks for `DM_VK_CLEAR_BY_PASS=1`, which keeps every slice.
  `DriverCheck.VERSION` is 3, so every phone checks again.
- `tex_view_for` gave every 8 / 16 / 10-bit integer texture under a float sampler the empty texture (0), on every GPU and in every game. The translator
  declares every sampled texture float, so this also hit shaders that take the bits as an integer. It is 6e0e1dd's binding (the texture's own format) again;
  `DM_VK_INT_TEX_EMPTY=1` gives the empty texture, to compare. The reviews of Legends: Arceus and of Scarlet / Violet named the same line.

Verification (the same gates as above, on the fixed tree):

- `make -f Makefile.host GAME=<id> lib` from a clean `out/` (the `vk_quirks.h` includers touched) for all five profiles: `libnxnative.so` and `libdmprobe.so`
  link, nothing undefined; nvn_vk.c, hle_nvn.c, android_main.c, hle_resscale.c and libdmprobe.so compile without warnings for Shield, Arceus and Violet
  (hle_sdk16.c's `-Wframe-address` is beta-exit's). gcc `-Wall -Wextra` on selftest.c: the same 17 old style warnings as before the change, none new. The Java
  app compiles. `chain_tree.py --verify`: bc2f6b2e5f0d / ba40200b20b8 / b72f9b70ebb2, unchanged.
- t_quirks: 582.8 M checks pass (part 5 asks `Q_CLEAR_BY_PASS` too: 7.7 M more). The mutation runs each fail, including two new ones: `Q_CLEAR_BY_PASS` reading
  dynamic rendering, or the game's profile, which the probe cannot know.
- Linux tests, t_safe_start (23) and dm/tests as before (test_shader_split_capture.py fails as on beta-exit). Golden synthetic round trip: 14 of 14.
- The probe's self-tests on the PC (lavapipe, validation layer on): 10 pass, 1 warn, 1 skip, as before. `3d-layered` passes both ways: with
  `vkCmdClearAttachments`, and with `DM_VK_CLEAR_BY_PASS=1` as a pass of its own per slice. It also passes on the Vulkan 1.0 route (`DM_PROBE_API10=1`). The
  validation messages are the old ones: `int-view-own`'s deliberate 07753 and `fallback-draw`'s 09600, and on the 1.0 route the same set as the selftest.c of
  7cfb4fe gives there. `int-view-own`: lavapipe returns a 16-bit integer's own bits under a float
  sampler on every kind of image. The test was also run through a test "driver package" over lavapipe that reports Samsung's vendor ID and crashes on a layered
  `vkCmdClearAttachments`, as the Xclipse drivers did. There `3d-layered` passes and writes nothing. With another vendor ID the crash writes
  `DM_VK_CLEAR_BY_PASS=1`, whether the probe catches it or the process is lost (the journal read as DriverCheck.java reads it). With env.txt's
  `DM_VK_CLEAR_BY_PASS=0` it writes `DM_VK_NO_LAYERED_3D=1`, and so does a crash at the 32-layer framebuffer.
- simdev, `run_simdev.sh all` for the five games, before and after t_vkdev gained its integer-texture check (860 runs each time): t_vkdev, t_vkdev_held and
  t_fgen_vk pass everywhere, and t_lshud_vk passes on 25 and skips on 18. The depth copy is drawn on the 41 devices with Vulkan 1.1 or newer, and background
  pipelines are as before. The only validation errors are Legends: Arceus's TF self-test ones on the 17 Mali profiles, as on beta-exit; the `SIMDEV_DC_FC` runs
  are as before.
- The new check binds integer textures through `tex_view_for`, made and registered as the game's are, on the 41 devices with Vulkan 1.1 or newer (the two
  1.0 ones skip it, as the depth copy), as recorded and held. R16G16B16A16_UINT under a float sampler keeps its own format everywhere. A 3D R32_UINT texture
  gets the float view (the same bits). A 2D one keeps its own format where its formats are fixed (40 devices) and gets the float view on the Adreno 506 profile
  (Vulkan 1.1 without `VK_KHR_image_format_list`). On 7 devices for Shield, Arceus and Violet, `DM_VK_INT_TEX_EMPTY=1` gives the 16-bit one the empty
  texture, and `DM_VK_INT_TEX=keep` keeps all three in their own format. Built against a copy of nvn_vk.c with 7cfb4fe's default put back, or with no float
  views, the check fails (the unchanged copy passes).

## The combined head: beta-exit-qm (2026-11-22)

`beta-exit-quirks` ef46af1, then `beta-exit` bcf1d76 (setup_game.py only: each distinct shader translated once), then `mali-all` e6f6d0c: one head for both.
Conflicts (each side kept whole; every line either side added since 3d21471 is in the result): nvn_vk.c mtl_init (gpu_id's driver ID and `Q_FLOAT_CONTROLS`
from the table; the ladder's `g_drv_id` beside it), android_main.c (`env_file.h`'s start-up order with `mali_env_migrate` before env.txt is read and
`DM_MALI_LADDER_FILE` after it: env.txt → safe-start → driver-check.env → phone profile), SetupActivity.java (the crash report offer, then the PanVK offer),
simdev t_vkdev.c (both sides' work checks).

Then the fix's per-GPU defaults moved into the table, the one mechanism: e6f6d0c decided them in `mw_init` from `g_arm_rel != 0` and `DM_PORT_GALAR`. Now
Shield's `game.tsv` says `engine_quirks mali_lit_standin` (Sword inherits it through `tables_from`; Legends: Arceus and Violet / Scarlet do not), game.py
generates `DM_GAME_ENG_MALI_LIT_STANDIN`, mtl_init maps it to `ENG_MALI_LIT_STANDIN`, and `Q_B10_STANDIN` (route A's default) and `Q_MALI_WHITE_CHECK` (the check
and the ladder) are both `(eng & ENG_MALI_LIT_STANDIN) && arm_rel != 0`, in `quirk_late` (they read Arm's release). `mw_init` asks the table and passes the rule
to `mw_enabled` / `mw_default_route` (their truth table and unit tests unchanged); env.txt's `DM_VK_B10_AS_RGBA16` / `DM_VK_B10_NOCOMPRESS`, `DM_MALI_LADDER`,
`DM_MALI_WHITE_SIM` and the other `DM_MALI_*` switches act exactly as on e6f6d0c. New `[quirk]` lines: "the white-screen check and its ladder" and "the lit
R11G11B10F target drawn as RGBA16F" where they are on (on Arm's own driver with Sword / Shield by default; from a test switch elsewhere). The shader chains are
unchanged (`engine_quirks` is no chain key).

Verification (WSL, NDK r27c, on a copy of the tree outside the worktree):

- Builds: `make -f Makefile.host GAME=<id> lib` for the five profiles, from a clean `out/`: all built, no warning in the changed files; `game_profile.h` has
  `DM_GAME_ENG_MALI_LIT_STANDIN 1` for Shield and Sword, 0 for Arceus, Violet, Scarlet. (`-j20` from a clean `out/` sometimes fails with `game_profile.h` not
  found: the `touch_ui.o` rule lacks the header, the same rule as on 3d21471 and ef46af1; making the header first avoids it.) `dm/pc` for the three engines.
- `t_quirks`: 739.4 M checks, all pass (route A and the check by default on 1.53 M of the 2.57 M descriptions for Sword / Shield, none for Arceus or Scarlet /
  Violet). 21 mutation runs fail as they must, the unchanged control passes: the 8 earlier ones, the two white-screen rules keyed on the port (both, each
  alone), on Arm's vendor ID (PanVK on), on `gpu_is_arm_driver`, without the eng bit (both, the check alone), the check out of `quirk_late`, and Shield's
  `game.tsv` with `async_pso_off` / without `mali_lit_standin`, Arceus's with it, Sword with an `engine_quirks` of its own.
- `t_mali_white` (206 checks; ASan + UBSan with fixtures, 214), `MaliLadderTest` (34 with the fixtures; 18 without), `t_b10` (6.4 M), `t_b10_spv`, `t_fmz`,
  `t_nan_minmax`, `t_clamp_zero`, `t_safe_start`, `t_crumbs` and the other Linux gfx tests pass; `test_shader_split_capture.py` fails as on beta-exit.
- golden_check.py `synth`, `b10` and `mali` (51 starts) for Shield, Sword, Violet, Scarlet and the Arceus engine: PASS; the synth round trips with
  `DM_VK_API=10`, `DM_VK_MIN_FEATURES=1`, `DM_VK_NO_DYNREN=1`: PASS.
- simdev `run_simdev.sh all` for the five games (lavapipe + 42 profiles; t_vkdev as recorded and held, t_fgen_vk, t_lshud_vk): only the known failures
  (adreno-506 / adreno-512-vk10 "b10: the R11G11B10F target's pass did not begin"; Arceus's TF self-test VUIDs on the Mali profiles). Route A and the check
  are on in exactly the 17 Mali profiles (Arm's own driver) for Shield and Sword and nowhere for Arceus, Violet, Scarlet. Mali env runs (Shield):
  `DM_MALI_LADDER=1`, `DM_VK_B10_AS_RGBA16=1` / `=0`, `DM_VK_B10_NOCOMPRESS=1`, `DM_VK_PROBE=f16target` on lavapipe + 17 Mali profiles, all PASS with
  e6f6d0c's decisions; Arm's identity on lavapipe: Shield and Sword route A + check, Arceus and Violet none; PanVK's identity (driver ID 20, Arm's vendor ID)
  on lavapipe: no route, no check.
- The probe's self-tests on lavapipe and through the fake-Xclipse shim: the same results and `driver-check.env` lines as ef46af1's run.
- `chain_tree.py --verify`: bc2f6b2e5f0d / ba40200b20b8 / b72f9b70ebb2 as before. javac + d8 of the app: built. lang_check: no line missing or unused that is
  not so on one of the two heads (164 missing, as on ef46af1).

### Fix round on beta-exit-qm (review findings, 2026-11-22)

The reviewers' findings on ab568f9, each checked against the code and, where it could be, a run on the PC first; all eleven held except the last one's ask (real
captures), which has nothing on this PC to run on (open, below).

- Driver check, app side: two checks could run at once (a second Play during the check, or the setup screen made again): both started the service, the
  second `dprobe_run` shared the probe's globals in the same process, and the first one's `System.exit` ended it mid-test, so a healthy phone could get that
  test's crash setting. Now `DriverCheck.claim` / `release` run one check and only the latest Play's start after it, and `DriverCheckService` ignores a second
  start. A check that never wrote `driver-check.key` (the app itself closed during it, or the service could not start) began again from nothing at every Play;
  now `driver-check.run` counts such starts, the next check resumes from the journal (the tests lost keep their settings, the one running when the app closed is
  left out without a setting) and gives up after 2. `DriverCheckTest.java` (`t_driver_check_java.sh`, desktop JVM) tests both.
- Driver check, `3d-layered`: `DM_VK_CLEAR_BY_PASS=1` covered all of the clear's submission (the layered pass begun again, the read-back copy, the GPU's work),
  so a crash anywhere there asked for a setting that does not keep the game off the call that crashed. Each step is now a submission of its own: the pass
  clearing every slice, the pass begun again (loading), the clear, the read-back; `DM_VK_CLEAR_BY_PASS=1` belongs to the clear alone (recording it and its
  submission), the read-back asks for nothing, a lost device asks what a crash there would. The clear is the engine's own (`{rect, 0, 1}`, a part of the pass's
  layer 0; by pass: one pass on that slice over that part), not 16 layers over the whole image. Where the engine uses dynamic rendering (every GPU but Adreno),
  the test records `vkCmdBeginRendering` with 32 layers, no framebuffer; the image has the engine's usage (transfer source and destination, sampled). A clear
  drawn wrong with the rest right now writes `DM_VK_CLEAR_BY_PASS=1` as a crash there does. On Vulkan 1.0 the probe's device is made as the engine makes it:
  `VK_KHR_maintenance1` (the 3D tests are skipped without it) and `VK_KHR_storage_buffer_storage_class`, `VK_KHR_external_memory` beside `_fd` with the
  instance's capabilities extension, the property extensions only with `VK_KHR_get_physical_device_properties2` (enabled on the instance, as the engine does).
  t_quirks part 5 asks `Q_NO_DYNREN` too (the vendor alone). `DriverCheck.VERSION` 4.
- Mali ladder: the start marker's 20 s ran from the start's first present, so they passed on the logos and the title screen and a route that crashed where the
  field first draws was never blamed; they now run from the first frame with `DM_MALI_WHITE_LIT_DRAWS` draws into a lit target (stage playing from there; the
  marker goes after 10 minutes without one). A start whose lit frames were never sampled (under the draw floors) was never counted; after `DM_MALI_NOSAMPLE_MS`
  (10 minutes) of them it logs a summary (skips by reason, the most draws seen) and counts as a start without a verdict. An app update started the ladder over;
  now only the driver or the fixes' own code (`MW_ROUTE_CODE`, the file's `code=`) does, and `app=` follows the app. A crash in the game's own code counted
  as an early end (two in a row closed the route); now it counts for nothing (`MW_MK_GAME`).
- Tests: t_mali_white (217 checks), golden_check.py mali (new cases: two game-code crashes in a row, a crash after 24 frames without a lit target
  (`GOLDEN_SYNTH_PLAIN_FRAMES`), the marker's window on lit frames never sampled, three starts without a sample, an app update keeping the ladder and the code
  starting it over), the probe on lavapipe with the validation layer (dynamic rendering, render passes, clears by pass, Vulkan 1.0: no `01387`, maintenance1 or
  `08742` messages any more) and through a fault shim (a crash, a lost process or a lost device at each step; a clear drawn wrong; Samsung and Qualcomm IDs).

## Not merged yet / open

- READMEs: the root README is Galar's. Arceus's and Paldea's READMEs are not merged (per-game READMEs to do); `docs/HANDOFF.md` took Arceus's edits as git merged them.
  Paldea's deletion of `site/` and `docs/DEVICES.md` was not taken (Galar's repo keeps them). Arceus's removal of `docs/handoff/arceus-lowspec_*` was taken.
- Releasing Arceus / Paldea from this tree is a product decision: the source of a release must be in that app's repository (GPL), so either this branch
  becomes each repo's `main`, or each repo gets a copy; package names stay `com.nxnative.pokemonshield` / `.pokemonlegendsarceus` / `.pokemonviolet`.
- Adoption candidates (one quirk / menu line each, after a phone test): `qc11_no_eds` and `driver_selftests` for Galar (#209 is the same bug: one word in
  Shield's `engine_quirks` each, the table does the rest), Arceus's touch camera / gyro / movement stick menu for Galar, Galar's battery saver for Arceus, the
  y-flip route on real Vulkan 1.0 drivers.
- beta-exit-quirks: the driver check, the debug report (its crash / hang handling, the tombstone hand-over), `Q_QUERY_LOD`, the depth copy (`zcopy_draw`) and
  the integer-texture views have not run on a phone. In the phone pass, a scene of each game that copies depth into a D24S8 texture by a scaled copy (a depth
  downsample: `zcopy_draw` on Adreno; Legends: Arceus with `qc11_no_eds` on Qualcomm's 1.1 driver too); `DM_VK_ZCOPY=0` brings back the blit.
- 8 / 16 / 10-bit integer textures under a float sampler keep their own format (6e0e1dd's binding) until a run of each game says more: count the `read as
  float by a shader` log lines (tex_view_for; the first 8 of a run), Legends: Arceus's and Scarlet / Violet's shaders above all (only Shield's `fp_t_tcb_8` was
  seen), and read the translated SPIR-V of those shaders: where the sampled value only reaches float arithmetic, the Switch reads 0 (`DM_VK_INT_TEX_EMPTY=1`
  shows that); where it reaches a bitcast to an integer, the texel's own bits are right. The driver check's `int-view-own` line says, per phone, whether its
  driver returns those bits for this binding (lavapipe does).
- The driver check's `3d-layered` on Xclipse now tests the engine's clear by pass, not the `vkCmdClearAttachments` in a layered pass that crashed Samsung's
  24.0.534 / 24.0.560 drivers, so whether a newer Samsung driver still crashes there is not probed any more. env.txt's `DM_VK_CLEAR_BY_PASS=0` makes the check
  record that call (a crash then writes `DM_VK_NO_LAYERED_3D=1`, since env.txt's 0 wins over the check's settings). The Xclipse crash was seen with a clear of
  16 layers over the whole image; whether the engine's own clear (layer 0, a part) crashes there is not known.
- What NVN's clear does to a layered 3D target is not checked: a clear through the pass's load op clears every slice of the pass, a clear inside the pass
  (`clear_in_pass`, either route) its first slice only. If the game clears a whole colour-grading table inside a pass, the two routes differ from the Switch.
- The Mali verdict's thresholds (empty, shown white, bright) are checked on synthetic frames only. No Sword / Shield capture of night, caves, snow, white menus
  over the field or fades is on this PC (only frame 31 / 32 recordings of Sword); with such captures, `mw_obs_kind` over the lit target and the picture shown
  of each is the test to run. "In a row" counts sampled frames only (empty and unsampled ones do not break a run).
- The Mali check's own compute pass is guarded while it is made (`mali_white_crashed`), not while its samples run: a driver that crashed running the first
  sample would be blamed on the route (the marker is up then), a later one not at all.
- The no-sample count (10 minutes of frames with a lit target, none sampled) also runs in menus that draw into a lit target; three such starts on a route make
  it broken.
- The probe's self-tests do not run behind the Vulkan profiles layer on this PC (`vkCreateDevice` -8 with every core feature on, on every profile, as on ab568f9):
  the Xclipse / Mali / Adreno routes are checked through the fault shim's vendor IDs on lavapipe instead.

- `arceus_pass_default` (uniform updates deferred inside a pass, `DM_VK_UB_DEFER`, Arceus only) still picks the game by `DM_GAME_PROGRAM_ID`, as 6e0e1dd: no GPU
  rule, a candidate for an `engine_quirks` word (`ub_defer`).
- beta-exit moves on: beta-exit-qm holds it up to bcf1d76. At the next merge, a per-GPU default beta-exit adds or changes goes into `vk_quirks.h` and t_quirks.c
  takes its reference from that head (the copied site lines checked against it as above).
- The Mali white-screen fix (route A, the check, its ladder and the PanVK offer) has not run on a phone with Arm's own driver; on PC and simulated devices only.
- Background pipelines on Mali / Xclipse / PowerVR / Turnip (beta-exit f09e58f) ran only on simulated devices here; nothing on a phone.
- The probe's `fallback-draw` self-test begins its render pass with the attachments in GENERAL without moving them there from UNDEFINED first
  (VUID-vkCmdDraw-None-09600 on lavapipe; beta-exit's own test, as in 6e0e1dd). It passes on lavapipe; on a phone that is undefined in the driver check, where a
  failure would only be a report line (the test writes no setting).
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

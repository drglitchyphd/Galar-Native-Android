# One engine for Galar, Arceus and Paldea Native

Branch `engine-unified` (local only) merges the three ports into one source tree, so renderer work (the GLES backend next) is written once instead of
cherry-picked across three repositories. It starts from Galar Native's `main` (389214a) and merges Arceus Native's `main` (def6392) and Paldea Native's
`main` (8e8fda7). Nothing here is pushed or released; each app's own `main` is untouched.

Branch `beta-exit-quirks` (local only) is `beta-exit` 6e0e1dd with the driver probes and the debug layers merged in: `galar-driver-probes` ad0f638 (it holds
`debug-bundle` and `galar-vk10-validation`), then `arceus-debug-layer` b16bdd5 and `arceus-vk10-validation` 85e72fd. Their per-GPU rules became one table for
all three apps ("Per-GPU rules" below); every default is 6e0e1dd's on every GPU, for every game, except the new rules listed under "Behaviour changes".

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
| `engine_quirks` | `DM_GAME_ENG_<NAME>` 0 / 1 | Arceus: all seven below; others none | where the two engines' defaults disagreed, each game keeps what its releases did; the four that decide a GPU default reach vk_quirks.h as `gpu_id.eng` bits ("Per-GPU rules") |
| `xlate_chain` | (shader tools only) | Arceus, Violet: `pre-fmz` | reproduces each game's shader chain (below) |

### `engine_quirks` (each one keeps Arceus Native's released behaviour; Sword / Shield / Scarlet / Violet take Galar's)

| Quirk | Where | Arceus (on) | Galar (off) |
| --- | --- | --- | --- |
| `qc11_no_eds` | vk_quirks.h `Q_NO_EDS` (through `gpu_id.eng`), nvn_vk.c mtl_init | Qualcomm Vulkan 1.1 drivers: draw state built into each pipeline (Arceus #34 exploded polygons) | per-draw state; #209 handled with full barriers (`Q_BARRIER_RELAXED`, old Qualcomm drivers), which both keep |
| `gpu_time_auto_only` | nvn_vk.c (`#if`: a setting, not a GPU rule) | GPU timestamps only with Graphics Auto | on by default except Mali / PowerVR (`Q_GPU_TIME_OFF`, both) |
| `frame_hold_off` | nvn_vk.c hold_on (`#if`) | DM_VK_FRAME_HOLD off by default (TF captures / report timestamps never run held) | mode 2 by default where the replay has its own thread |
| `async_pso_off` | vk_quirks.h `Q_ASYNC_PSO` (through `gpu_id.eng`), nvn_vk.c apso_on | background pipeline builds off (Adreno 710 #34: objects missing) | on for Adreno, 3 workers, 8 ms wait |
| `framegen_game_rate` | no reader since fg-plan f9ae686 | runs at the game's 30 or 60, floor = rate − 2 | the same since f9ae686 (frame generation is locked off by default for Sword / Shield; Scarlet / Violet also need `DM_FG_PALDEA=1`) |
| `nvn_async_never` | vk_quirks.h `Q_NVN_ASYNC` (through `gpu_id.eng`) for Auto; hle_nvn.c rq_on (`#if`) refuses the player's setting too | the replay never on its own thread (crashed at loading screens, Arceus #3) | Auto: on for Adreno. **Without this quirk the merge would have turned it back on for Arceus** (hle_nvn.c merged cleanly to Galar's Auto) |
| `driver_selftests` | vk_quirks.h `Q_DRIVER_SELFTESTS` (through `gpu_id.eng`), nvn_vk.c mtl_init | vtx_canary + tf_selftest at start | not run (unchanged start-up for Galar) |

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
  `Q_ASYNC_PSO`, `driver_selftests` → `Q_DRIVER_SELFTESTS`, `nvn_async_never` → `Q_NVN_ASYNC`. A game's own choice lives in its `game.tsv`: Sword / Shield
  would take Arceus's Qualcomm Vulkan 1.1 rule by adding `qc11_no_eds` to Shield's `engine_quirks`, nothing else.

`quirk_default(q, gpu_id)` is each site's default; the site still lets env.txt win with the same `DM_VK_*` switch as before, and `quirk_note` prints a
`[quirk]` line for each workaround in effect at start (the debug report copies them to `quirks.txt`), saying where its value came from: "default for this
GPU" (or "… in this game's profile" for the four rules that read `gpu_id.eng`), "env.txt" or "driver check". The first decision of a rule is kept, atomically
(the game's thread and the replay's can decide one lazily). `hle_nvn.c` asks through `nvn_vk_quirk_default`, which answers -1 before the device exists (as
6e0e1dd's `nvn_vk_gpu_name` test: `rq_on` takes that as Auto off) and, for the rules that read the driver ID or Arm's release (`quirk_late`), until `mtl_init`
has read them just after the device is made; t_quirks.c checks that no other rule reads those.

Settings start-up order (android_main.c `host_start`, each file's keys set only when not set: `env_file.h`): env.txt (the player's), then the safe-start
ladder's level (safe_start.h), then `driver-check.env` (the driver check's settings for this driver, DriverCheck.java), then the phone profile. So the player's
settings and the ladder's levels win over the driver check (ladder level 5's `DM_VK_FLOAT_CONTROLS=0` over the check's 1, #89). android_main.c names the keys
it took from `driver-check.env` in `DM_DRIVER_CHECK_KEYS`, and nvn_vk.c takes those values as this driver's defaults, not as the player's (vk_quirks.h
`vkq_env_src` / `vkq_env_player`): the crash markers that only the player's own setting skips still apply to them (`float_controls_crashed`,
`bc_gpu_crashed`, `heap_import_crashed`), and their `[quirk]` line says "driver check". The check writes `DM_VK_FLOAT_CONTROLS`, `DM_VK_DMAHEAP`, `DM_VK_BC_GPU`
(table rules) and `DM_VK_ZCOPY`, `DM_VK_INT_TEX`, `DM_VK_NO_LAYERED_3D` (switches of their own code paths).

The other `engine_quirks` decide nothing per GPU and stay `#if DM_GAME_ENG_*` at their sites: `frame_hold_off`, `gpu_time_auto_only`
(timestamps only with Graphics Auto: a setting), `nvn_async_never`'s refusal of the player's own setting; `framegen_game_rate` has no reader (above).
GPU tests left outside the table on purpose are listed at the top of vk_quirks.h: building `gpu_id` itself, what is decided before a driver is open
(android_main.c chip profiles, `panvk_defaults` and the driver check's copy of it, the safe-start ladder, the heat guard's kgsl clock), the driver check's own
self-tests (they run on every GPU), the Java menu's GPU sections and texts, test switches (`DM_VK_AS_MALI`, `DM_VK_DEVICE`), and the frame hold, which follows
the replay thread's decision.

What each app runs by default (env.txt still wins). "Arm's own driver": by driver ID where the driver gives one, else Arm's vendor ID and not a driver package
(PanVK is not). Bold: the games differ, each by its own profile.

| Rule (`vk_quirks.h`) | Switch | Sword / Shield | Legends: Arceus | Scarlet / Violet |
| --- | --- | --- | --- | --- |
| `Q_NO_DYNREN` render passes, not dynamic rendering | `DM_VK_NO_DYNREN` | Adreno | Adreno | Adreno |
| `Q_BARRIER_RELAXED` fewer full barriers before passes | `DM_VK_BARRIER` | Adreno, but not Qualcomm's own driver before Vulkan 1.3 (Turnip is not one) | same | same |
| `Q_GATHER_BIAS`, `Q_PASS_DEFER`, `Q_IMPORT_MARK_OFF` | `DM_VK_GATHER_BIAS`, `DM_VK_PASS_DEFER`, (`DM_VK_HOST_IMPORT`) | Adreno | same | same |
| `Q_ASYNC_PSO` pipelines built in the background | `DM_VK_ASYNC_PSO` | Adreno | **never** (`async_pso_off`, #34) | Adreno |
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

Checks: `dm/gfx/tests/t_quirks.c` compares the table with 6e0e1dd's per-site expressions (copied, with their line numbers) for each port's profile over
2.57 M GPU descriptions each (vendors, driver IDs or none, Vulkan 1.0-1.4, Arm releases r20p0-r55p2 with and without beta, driver packages, names, memory sizes,
dynamic rendering), then every port with every combination of the `eng` bits (no rule reads the port), reads the five `game.tsv` files, and pins players'
phones for each game: 328.6 M comparisons, equal everywhere; `Q_QUERY_LOD` is the one rule without a 6e0e1dd counterpart. It also checks, over the same grid,
that only the `quirk_late` rules change when the driver ID and Arm's release are not known yet and that `Q_FLOAT_CONTROLS` reads the vendor and driver ID alone
(the driver check asks it from those), and where a setting came from (`DM_DRIVER_CHECK_KEYS`): 567 M checks in all. `simdev/t_vkdev.c`, built for each game,
checks the decisions `mtl_init` makes against the same rules on the 42 simulated GPUs, every rule decided at its site so far against the table, the GPU
timestamps site, `nvn_vk_quirk_default` before, while and after the device is made, and (`SIMDEV_DC_FC`) a driver-check value under a crash marker;
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

### New in beta-exit-quirks (the merged branches; every game unless noted)

- `Q_QUERY_LOD` (`spv_query_lod`, the one new table rule): on Arm's own driver textureQueryLod's level component becomes clamp (LOD, 0, levels − 1); Arm's driver
  gives floor (LOD) (dEQP-VK texturequerylod, Mali-G615 MC6). A load-time SPIR-V pass: no new setup, the shader chains are unchanged. `DM_VK_QUERY_LOD=0 / 1`.
- Vulkan validation fixes (the S25+ layer run), on every GPU: `spv_strip_clipcull` drops the unused ClipDistance[8] / CullDistance[1] of every vertex shader at
  load (over the limit of 8); a scaled / mirrored / format-changing copy into a depth / stencil format that is no blit destination (D24S8 on Adreno) is drawn
  (`zcopy_draw`; `DM_VK_ZCOPY=0`: the blit); an integer texture read as another numeric class (`tex_view_for` / `fmt_as_class`; `DM_VK_INT_TEX=keep`: its own
  format always), as the table below.
  - `zcopy_draw` begins its pass through `rend_begin`, as the game's passes: in a held frame (`DM_VK_FRAME_HOLD=2`, the default with Faster rendering's Auto on
    Adreno, Sword / Shield and Scarlet / Violet) the command buffer outside a pass is a secondary, where the branch's `vkCmdBeginRenderPass` was invalid with
    Adreno's render passes (VUID-vkCmdBeginRenderPass-bufferlevel); the copy is now a segment of its own, its pass begun in the primary at the flush. And it
    tells `draw_`'s pipeline-bind cache (`g_gfx_bind_gen`), so the game's next draw binds its own pipeline again instead of drawing with the copy's. simdev's
    t_vkdev draws a copy in both kinds of submission (`run_simdev.sh all` runs it again with `DM_VK_FRAME_HOLD=2`); with the branch's code the held run
    reported VUID-vkCmdBeginRenderPass-bufferlevel / -EndRenderPass-bufferlevel on the Adreno 830 and 710 profiles (render passes), with this one none.
  - An integer texture read as another class gets one of three outcomes (6e0e1dd bound its own format in every case):

    | Texture (its view format) | Read as | Its image's formats (nvn_vk.c `tex_new`) | Outcome |
    | --- | --- | --- | --- |
    | 32-bit channels: R32 / R32G32 / R32G32B32A32 UINT or SINT | float, or the other signedness | mutable without a format list: 3D images, `DM_VK_NO_FMT_LIST`, and outside Mali a format that cannot be a render target or a driver without format lists (Vulkan 1.1 without `VK_KHR_image_format_list`) | **the same bits** in a view of the shader's class (new) |
    | the same | the same | fixed: not mutable (every plain colour image on Mali, `Q_ARM_FORMATS`: Arm's driver and PanVK), or listing only its own format and sRGB twin (render-target formats wherever format lists exist: Adreno, Xclipse, most Vulkan 1.2 drivers) | its own format, as 6e0e1dd (a view of another class would be invalid there; undefined in Vulkan, the layer still names it) |
    | 8 / 16 / 10-bit integers | float | any | **the empty texture** of its class: 0, the Switch's flushed denormals (new; Shield's `fp_t_tcb_8`) |
    | 8 / 16-bit integers | the other signedness | any | its own format, as 6e0e1dd: a view of the other signedness extends the sign the other way (R16_UINT 0xffff read as -1, the Switch gives 65535) |
    | float, UNORM, SNORM, sRGB (or BC decoded here) | uint / sint | any | its own format, as 6e0e1dd (not seen in Sword / Shield yet) |

    galar-vk10-validation viewed 8 / 16-bit integers as the other signedness, and on fixed-format images read 32-bit ones as 0 although its notes said "the
    same bits": both are 6e0e1dd's binding again. The driver check's `int-view` test now reads 32-bit textures through float and other-signedness views of
    mutable images (where the engine makes them; a failure writes `DM_VK_INT_TEX=keep`), and `int-view-own` reports what the driver returns for the old binding
    on a non-mutable and a format-listed image and for a 16-bit integer read as int (information only, no setting).
- The driver check before Play (`DriverCheck.java`, `DriverCheckService` in its own `:drvcheck` process, `libdmprobe.so`: built by the `lib` target and
  `win_build_lib.py`, optional in the APK): the probe app's GPU self-tests on the game's own driver, once per driver / system build / app version (and
  `DriverCheck.VERSION`, 2), crash- and hang-safe (a journal, up to 4 attempts, 90 s; Play always follows). What it finds goes to `driver-check.env`, read after
  env.txt and the safe-start ladder and before the phone profile (`DM_DRIVER_CHECK=0`: not read; "Per-GPU rules" above): `DM_VK_DMAHEAP=0`, `DM_VK_BC_GPU=0`,
  `DM_VK_ZCOPY=0`, `DM_VK_INT_TEX=keep` or `DM_VK_NO_LAYERED_3D=1` after a test that crashed or failed, `DM_VK_FLOAT_CONTROLS=1` where a driver is right only
  with float controls. Those are this driver's defaults: the player's env.txt and the ladder win over them, the engine's crash markers still apply to them, and
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

## Not merged yet / open

- READMEs: the root README is Galar's. Arceus's and Paldea's READMEs are not merged (per-game READMEs to do); `docs/HANDOFF.md` took Arceus's edits as git merged them.
  Paldea's deletion of `site/` and `docs/DEVICES.md` was not taken (Galar's repo keeps them). Arceus's removal of `docs/handoff/arceus-lowspec_*` was taken.
- Releasing Arceus / Paldea from this tree is a product decision: the source of a release must be in that app's repository (GPL), so either this branch
  becomes each repo's `main`, or each repo gets a copy; package names stay `com.nxnative.pokemonshield` / `.pokemonlegendsarceus` / `.pokemonviolet`.
- Adoption candidates (one quirk / menu line each, after a phone test): `qc11_no_eds` and `driver_selftests` for Galar (#209 is the same bug: one word in
  Shield's `engine_quirks` each, the table does the rest), Arceus's touch camera / gyro / movement stick menu for Galar, Galar's battery saver for Arceus, the
  y-flip route on real Vulkan 1.0 drivers.
- beta-exit-quirks: the driver check, the debug report (its crash / hang handling, the tombstone hand-over), `Q_QUERY_LOD`, the depth copy (`zcopy_draw`) and
  the integer-texture views have not run on a phone. Before a release: count the `read as float by a shader` / `read as uint` / `read as sint` log lines
  (tex_view_for) in a run of each game, Legends: Arceus's shaders above all (only Shield's `fp_t_tcb_8` was seen): an 8 / 16-bit integer read as float now
  reads 0, which is the Switch's answer only where the shader uses it as a float (a shader that takes the bits as an integer read them through the old binding
  on most drivers).
- `arceus_pass_default` (uniform updates deferred inside a pass, `DM_VK_UB_DEFER`, Arceus only) still picks the game by `DM_GAME_PROGRAM_ID`, as 6e0e1dd: no GPU
  rule, a candidate for an `engine_quirks` word (`ub_defer`).
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

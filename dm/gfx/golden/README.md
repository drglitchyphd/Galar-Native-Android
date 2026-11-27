# Golden frames

A frame recorded on a phone (title screen, overworld, battle, menu) becomes an automatic PC check: the PC replays the recorded frame through the engine's own
Vulkan replay layer (`dm/gfx/nvn/nvn_vk.c`, built for x86-64 Linux in `dm/pc`) on Mesa's lavapipe, writes a PNG and compares it with the expected picture.
The recordings are NVN-level, not Vulkan-level, so a second (GLES) backend can later replay the same files.

| File | What it is |
|---|---|
| `dm/gfx/nvn/golden_fmt.h` | the recording format (`.nxgold`) |
| `dm/gfx/nvn/golden_rec.h`, `golden_rec.c` | the recorder (backend-neutral); off unless `DM_GOLDEN_REC` is set |
| `dm/gfx/nvn/golden_vk.inc` | the Vulkan backend's side of it (`golden_be_*`), `#include`d at the end of `nvn_vk.c` |
| hooks | `hle_nvn.c`: `GOLDEN_SUB_BEGIN` / `GOLDEN_SUB_END` around a submission, `GOLDEN_PRESENT` before both `mtl_present` calls; `nvn_vk.c`: `GOLDEN_MEM` in `pool_buf` and `push_stage_h`, one line in `tex_res`, `DM_VK_DEVICE` in `mtl_init` |
| `dm/pc/` | the replay layer for x86-64 Linux: `compat/pc_compat.h` (the PC's `darwin.h`), `pc_host.c` (the host symbols the layer needs, weak), `Makefile`, `build.sh` → `out/pc-<engine>/libnvnvk.a` for any PC tool |
| `golden_play.c` | the player: recording → PNG |
| `golden_check.py` | the checker: `run`, `accept`, `synth` |
| `golden_synth.c`, `synth/*.vert/.frag` | a frame made on the PC with the recorder on: the round-trip test |

## Recording format (`golden_fmt.h`)

`golden_file_hdr` (`NXGOLDN1`, version 1), then chunks `{type, flags, raw, stored}` + payload (zlib when `flags & 1`), in replay order. What `POOL`, `MEM`,
`TREG`, `SREG`, `SHDR`, `PROG`, `LIVE`, `CPUW` and `TEXI` set up applies before the next `PROL` / `SUBM` / `PRES`.

- `META`: `title` (the running game, `DM_PROGRAM_ID`), `engine` (the build's game: picks the player), `present`, `label`, `gpu`, `date`, the NVN object sizes, `origin_ll`, `depth_zero`, `tex_pool_n`.
- `POOL`: memory pools (address, size), as `mtl_pool_add` had them; more before a submission that added some.
- `MEM`: runs `{address, length, bytes}`: the memory as the next submission reads it, in 4 KB pages, at the phone's addresses: pools (vertex / index / uniform / storage data, texture storage the CPU fills, indirect arguments, copy sources), command memory, NVN objects (textures, programs, syncs, pool objects). A page is written again only when it changed since it was last written (the CPU fills uniform blocks between submissions).
- `TREG` / `SREG`: texture / sampler descriptor registrations (`tex_t` + `view_t` copies, `samp_par`): the whole table at the start, changes before each submission.
- `SHDR`: a shader by its 8-byte key: stage, translated SPIR-V, reflection (`.vkr`), GL-depth SPIR-V. `PROG`: a program serial's per-stage keys.
- `LIVE` / `CPUW`: texture serials that were initialized (`mtl_tex_init`) / filled by the CPU (`mtl_tex_cpu_written`).
- `TEXI`: an image whose content comes from earlier frames (a render target sampled before it is drawn, a texture filled by a copy): all levels and layers, read before the frame touched it. Colour as raw texels with their VkFormat; depth as float32 0..1 plus stencil bytes, so any backend can load it.
- `PROL`: command records that rebuild the replay state the frame starts from (programs, vertex layout, uniform / storage / texture bindings, blend / depth / raster state, viewports, scissor, render targets, pending clears): a frame relies on state earlier frames left bound.
- `SUBM`: one queue submission: its command handles (records followed through `JUMP` / `CALL`, exactly as `hle_nvn.c` wrote them, `nvn_obj.h`).
- `PRES`: the present that ends the frame: the texture to picture and its crop. `END`.

Addresses stay the phone's: the player maps memory at the same addresses before anything else takes address space (the phone's 39-bit user addresses fit the PC's 47-bit space).

While recording, each submission first waits for the one before (so memory read then is what it reads on the GPU) and `DM_VK_SPLIT` is 0. The recorder writes
`<title>-<label>-p<present>.nxgold` and, beside it, `<...>.phone.png`: the phone's own picture of that frame (for people; the expected picture comes from the PC).

## Build and run (WSL)

From Windows, write the commands into a script and run it with `wsl bash /mnt/c/.../script.sh` (Git Bash rewrites `/mnt/c` paths).

```bash
cd /mnt/c/Users/daniel/galar-golden/dm/pc
bash build.sh                       # Sword / Shield engine (GAME=01008DB008C2C000): out/pc-01008DB008C2C000/{libnvnvk.a,golden_play,golden_synth}
bash build.sh GAME=01008F6008C5E000 # Scarlet / Violet engine
cd ../gfx/golden
python3 golden_check.py synth                                  # the round trip without a phone (Shield's title)
python3 golden_check.py b10                                    # the Mali float-target tests (R11G11B10F), below
python3 golden_check.py mali                                   # the Mali white-screen check and its ladder over successive starts, below
python3 golden_check.py synth --title 0100ABF008968000 --out /tmp/gsyn-sword   # the same frame as Sword's
python3 golden_check.py run   ~/galar-golden-data/swsh         # every recording in the folder: PASS / FAIL / NEW / ERROR, exit 0 / 1 / 2
python3 golden_check.py accept ~/galar-golden-data/swsh [name] # this run's pictures become <name>.expected.png
../../pc/out/pc-01008DB008C2C000/golden_play <rec.nxgold> <out.png> [--info] [--keep <shader folder>]
```

`build.sh` takes the system's Vulkan headers, else the NDK's (`~/psa-build/android-ndk-r27c`), and `glslc` from `PATH`, else the NDK's shader tools. No
`libvulkan-dev` needed: the replay opens `libvulkan.so.1` (the loader) itself.

Tolerance: a pixel differs when a channel differs by more than `--threshold` (default 2); a recording fails above `--max-diff` percent (default 0.1);
`<folder>/<name>.tolerance` (`<threshold> <max-diff>`) overrides both for one recording. A failure writes `<out>/<name>.diff.png` (differing pixels red, the rest dimmed).

Devices and layers: the checker uses lavapipe unless `VK_ICD_FILENAMES` (or `VK_DRIVER_FILES`) is set. The player goes through the Vulkan loader, so
`VK_INSTANCE_LAYERS`, `VK_LOADER_LAYERS_ENABLE`, `VK_ADD_LAYER_PATH`, `VK_LOADER_DEBUG` work as for any program (checked with `VK_LAYER_MESA_overlay`):
run it under a Vulkan Profiles layer that makes lavapipe pose as Mali / PowerVR / Xclipse. `DM_VK_DEVICE=<index>` or `=<part of the device name>` picks the device
when several are listed. The engine's own switches (`DM_VK_AS_MALI=1`, `DM_VK_MIN_FEATURES=1`, `DM_VK_API=10`, `DM_VK_NO_PUSH=1`, ...) work too: the same frame
through the Mali or Vulkan 1.0 routes. Expected pictures belong to one device / route: keep a folder per combination. On the synthetic frame `DM_VK_MIN_FEATURES=1`,
`DM_VK_API=10`, `DM_VK_NO_PUSH=1`, `DM_VK_NO_DYNREN=1`, `DM_VK_NO_BC=1` and `DM_VK_POOL_SHADOW=1` give the identical picture. `DM_VK_NO_HOST_IMPORT=1` (and so
`DM_VK_AS_MALI=1`) does not: on lavapipe the exported-memory pool route (the one Mali / Xclipse phones take without host import) draws only the clear colour,
in the live run as well as in the replay (the replay matches its own live run). Not looked into further: lavapipe's opaque-fd export or the route itself.

`GOLDEN_SKIP=texi,prol,mem,treg,live,...` (player) leaves chunk types out: for diagnosis, and the synthetic test's proof that its picture depends on them.

## The synthetic round trip (no phone)

`golden_synth` plays the game's part through the PC replay with the recorder on: frame 1 draws a vertex-coloured triangle into an offscreen target and leaves
the lit program, vertex layout, uniform blocks, texture handle and depth state bound; frame 2 (recorded) copies a checkerboard from a buffer into a texture, clears,
draws a full-screen quad sampling frame 1's target, the CPU rewrites a uniform block in place between its two submissions, then an inline uniform update and a lit,
textured, depth-tested cube. `golden_check.py synth` replays the recording in a fresh process and requires the identical picture, and a different one with
`TEXI`, `PROL` or `MEM` left out. Result on lavapipe (Mesa 26, 2026-11-21): identical (0 of 76,800 pixels differ) for Shield, Sword and Scarlet titles; 67,641 /
76,800 / 76,800 pixels differ without TEXI / PROL / MEM.

## The float-target tests (R11G11B10F, no phone)

`golden_check.py b10` runs `golden_synth` with `GOLDEN_SYNTH_B10=1`, which adds to the same two frames the R11G11B10F paths the Mali float-target tests change: a lit
pass into an R11G11B10F target (a clear with a negative and a large channel, a draw writing alpha 0.25, then a white one blended by the target's alpha, which
R11G11B10F reads as 1: it must stay invisible), that target copied into the game's memory and back into another one, a texture clear, an RGBA8 checker copied into
R11G11B10F, a CPU-filled gradient and a CPU-filled texture holding every channel code; frame 2 shows four of them in the window's corners. One run with no test
switch, one with `DM_VK_B10_AS_RGBA16=1` (R11G11B10F drawn as RGBA16F), one with `DM_VK_B10_NOCOMPRESS=1` (R11G11B10F targets without framebuffer compression) and
one with both. Each run's own round trip must be identical and its four CPU reads bit for bit right (`b10 check` lines: the every-code texture read by the CPU, the
GPU's copy of it into the game's memory, the lit pass's copy, the clear); each picture must match the default one within the tolerance, and so must the test's
recording replayed without the test and the default's with it. Every colour shown is one R11G11B10F holds exactly, so the way a driver rounds into the format
cannot show (lavapipe truncates, the stand-in rounds to nearest even). Result on lavapipe (2026-11-22): every picture identical to the default (0 of 76,800 pixels
differ), every check exact; the engine before the stand-in's conversions (f0035ae) fails it.

Frame 1 also writes what reviewers asked to see (18 + 5 `b10 check` lines; `b10 checks: N run` ends them): a draw giving every pixel its own three values
(`b10v.frag`: negative, -0, -inf, NaN, +inf, past the range, any exponent), a draw blended by MAX with a negative channel and one past the range, one blended by
REVERSE_SUB, a compute shader's stores into an image declared `r11f_g11f_b10f` and one declared without a format (`b10img.comp`), a 2-layer array and a 2-slice 3D
texture copied in from the game's memory and out again, a copy in from memory only the CPU wrote, an RGBA8 2-layer array blitted into R11G11B10F, and a block-linear
texture the CPU filled. A CPU read packs, which hides a negative or out-of-range value the stand-in kept, so `b10probe.frag` also reads the draw, the MAX blend and
both images in a later pass into RGBA32F targets, as the game's later passes would. Row 0 of the draw holds values every rounding rule stores alike and is checked on
every route; the rest of it, with the stand-in (its one rule, round to nearest even). A fifth run, `DM_VK_B10_CPU_COPY=1` with the stand-in, takes the CPU route of
copies in from the game's memory (its picture is not compared: one corner is a copy that route cannot see). Then the pipeline lists: recorded with
`DM_VK_PSO_RECORD`, with the stand-in and without, they must be the same file, and a start that prewarms one (shaders from a pack, `spv_pak.py`, as on a phone),
with the stand-in or without, with extended dynamic state or without (`DM_VK_NO_EDS=1`), must find built every pipeline its draws ask for (nothing more recorded).
With the stand-in's new rules taken out (a scratch copy: no rounding of draws, of image stores, after dispatches, `b10_key_here` a no-op) it fails as it should.
`dm/gfx/tests/t_b10_spv.sh` checks the SPIR-V side alone: the patches on modules from glslc and by hand (spirv-val), and the store rule run by lavapipe over a
million texels against `b10_conv.h`.

## The Mali white-screen check and its ladder (no phone)

On Arm's own Mali driver the engine checks Sword / Shield's lit scene while playing (`nvn_vk.c` `mali_white_vk.inc`, rules in `mali_white.h`; who gets it by
default is the quirk table's `Q_MALI_WHITE_CHECK` / `Q_B10_STANDIN`: Arm's own driver, in a game whose profile says `mali_lit_standin`, which Shield's
`game.tsv` does and Sword takes through `tables_from`): route A (the RGBA16F
stand-in) first; white or broken → route B (no framebuffer compression) from the next start; that too → route A again and the app offers PanVK. White is quick to
call (3 samples in a row with a quarter of the texels at 1024+ or infinite, or 5 with 90 % of the picture shown white; or a 8 % mean over 40 samples); NaN is
counted apart (a quarter of the lit target NaN 3 times in a row, or 10 near-black samples in a row, is BROKEN: a black picture); fine is slow (20 samples over 3
minutes of play, none of them with 8 % NaN; those with half the picture shown white or more are bright, no evidence either way; kept only after two starts, then
re-checked in three starts); a start that reaches the cap with no verdict is counted unless its samples were mostly bright (three make the route broken, one mostly
black at once); a start that crashes on a route before 20 s of play or its first sample counts against the route, one that ends before its first picture only with
the crash flag, one that dies making the check's own compute pass never.
`golden_check.py mali` runs `golden_synth` with `GOLDEN_SYNTH_LIT=<kind>`: after the recorded frame (unchanged), `GOLDEN_SYNTH_FRAMES` (16) frames draw an R11G11B10F
target of 1280x720 with 240 draws, its `GOLDEN_SYNTH_LIT_SCALE` part white (65504), fine, black, fine then white, fine with a white corner
(`GOLDEN_SYNTH_LIT_CORNER`), white through two corner viewports only, NaN, fine with NaN spots, or fine with the window (the picture shown) then drawn white, all
of it or 60 %; `GOLDEN_SYNTH_LIT_DRAWS` / `GOLDEN_SYNTH_OTHER_DRAWS` split the frame's draws, `GOLDEN_SYNTH_LIT_ABORT=<n>` ends it with abort() after the n-th lit
frame (a crash on the route), `GOLDEN_SYNTH_ABORT_LOADING=1` before the first picture. Each run is one start of the game; a case's runs share the ladder's state
file (`DM_MALI_LADDER_FILE`), samples are taken at every eligible present (`DM_MALI_WHITE_EVERY_MS=0`, `DM_MALI_RECHECK_MS=0`; lavapipe signals a sample's fence a
few presents later, about one sample every two frames). Lavapipe is not Arm's driver: with no switch nothing may happen (no `[mali-white]` line, no state file, no
route); `DM_MALI_LADDER=1` forces the ladder on. The cases (51 starts): white goes A → B → A with the PanVK offer pending, and `DM_APP_VERSION=97` starts it over;
a white half-size part is judged on its drawn part; fine (made quick: `DM_MALI_FINE_N=5 DM_MALI_FINE_SPAN_MS=0`) is kept at the second start, re-checked in
three, then unchecked; a kept route re-checked on a white scene turns white; with the default rules fine samples then white end WHITE; a black target gives empty
samples, never fine: 10 in a row are BROKEN, and a cap reached with mostly empty ones makes the route broken at once; a white tenth is WHITE by the slow rule at the
40th sample; a white corner of 28 texels is capped with no verdict and the third such start makes the route broken; a lit target of NaN is BROKEN, NaN spots stay
fine; a white window over a fine lit target is WHITE after 5 samples, a 60 % white one is bright (never fine, a start capped on it not counted); 40 lit draws of 240 in the frame, or draws through two corner
viewports, are not sampled; an abort leaves the start marker, and the next start counts the route as closing the game with `DM_LAST_CRASH_DEVICE=1`, or after a
second early end, but not for a crash in the game's own code (`DM_LAST_CRASH_GAME=1`); starts that end before their first picture count only with the crash flag;
a start that dies making the check's own compute pass (`DM_MALI_WHITE_INIT_ABORT=1`) blames no route, and the check is off on that driver after it (its marker
beside `DM_VK_PCACHE`); `DM_VK_PROBE=f16target` under route B keeps its white verdict (both white, the offer), `DM_VK_FMZ=1` keeps a fine one out of the file, and
`DM_VK_B10_AS_RGBA16=0` keeps the verdict out with the settings note naming the switch; A+B white counts as both white; `DM_MALI_WHITE_SIM=1` alone counts a fine
scene as white; `DM_MALI_LADDER=0` wins over it. Every sample is the engine's own compute pass on the real target and the picture shown, read back after its
fence. Result on lavapipe (2026-11-22): PASS, 51 starts.

## Recording on the phone (S25+)

1. Install a build of this branch (the recorder is in the engine; off unless asked for).
2. In the game's folder on the phone, `Android/data/<the app's package>/files/` (Sword: `files/0100ABF008968000/`), add lines to `env.txt` (the app rewrites
   env.txt when settings change in its menu: check the lines are still there before starting):
   - `DM_GOLDEN_REC=req` and, optionally, `DM_GOLDEN_LABEL=overworld`, or
   - `DM_GOLDEN_REC=900` (the frame ending with present 900; `900,1800,2700` for several).
3. With `req`: start the game, go where the frame should be (title, overworld, battle, menu), then put `golden.req` in the same folder holding `<frames to wait> <label>`,
   e.g. `0 battle` (it is read every 30 presents and deleted). The log (`log.txt`) says `[golden] recording ...` then `[golden] recorded <path>: N submissions ...`.
   The game stalls for a moment while it records (each submission waits for the one before; images from earlier frames are read back).
4. The recordings are in `files/golden/` (`DM_GOLDEN_DIR` puts them elsewhere): `<title>-<label>-p<present>.nxgold` and `.phone.png`. Copy them to the PC, into a
   folder outside the repository (for example `~/galar-golden-data/swsh/`), one per game / scene set.
5. On the PC: `python3 golden_check.py run <folder>` (all NEW the first time), look at `<folder>/out/<name>.png` against `.phone.png`, then
   `python3 golden_check.py accept <folder>`; from then on `run` is the check.
6. Remove the `DM_GOLDEN_*` lines from env.txt afterwards.

Sword and Shield both: one engine build (`01008DB008C2C000`) runs both games; the recording's `title` names the game and the player checks it is one of its
family. Scarlet / Violet: `GAME=01008F6008C5E000` (Violet's engine, which runs Scarlet `0100A3D008C5C000`). Legends: Arceus (`01001F5010DFA000`): once its engine
is merged into this tree with the same hooks, `bash build.sh GAME=01001F5010DFA000`.

## Not tested yet

- Recording on a phone: the recorder only ran on the PC (golden_synth). The Android objects (`hle_nvn.o`, `nvn_vk.o`, `golden_rec.o`) compile with the NDK;
  nothing was installed or run on a phone.
- Real game frames: their size (expect tens to a few hundred MB per recording: images from earlier frames dominate), multisampled images (not recorded: they start
  cleared), shaders the pack lacks (named in the log, missing in the player), BC textures, texture buffers, transform feedback, indirect draws whose arguments
  the GPU writes (index ranges captured generously), uniform / storage blocks reached only through addresses in other buffers (the replay's own lookups are
  hooked, the shader's raw pointers are not).
- Phones whose kernel uses 48-bit user addresses: memory above 2^47 cannot be mapped on the PC (the recorder logs it).
- The Mac / Metal and iOS builds compile `golden_rec.c` (its backend functions are weak there: recording says it is unavailable); not built here.
- A GLES backend: it needs its own `golden_be_*` (the state as records, the tables, image read-back / fill, the picture) and the player's loop over its `mtl_*`.

# Game profiles

Each game's constants and shader tables live in `dm/games/<title ID>/`, named after the base game's title ID. This project carries one:
**Pokémon Shield**, `01008DB008C2C000` (update 1.3.2). Tools pick it with `DM_GAME=01008DB008C2C000`; `game.py` reads it and writes the
engine's `game_profile.h`.

| File | What it holds |
| --- | --- |
| `game.tsv` | The profile's keys: the game's modules and builds, its rendering options, the Android operation mode |
| `shader_map.tsv` | Every shader the game uses, by key (a hash of its code), and where the setup finds it in the game's files |
| `shader_facts.tsv`, `shader_facts_manual.tsv`, `shader_pairs.tsv` | What the shader translator needs to know about each shader |
| `pso_manifest.tsv`, `c1_full.tsv`, `bf_want.tsv` | Pipeline and constant-buffer facts the renderer uses |
| `dmsh_chain.h` | The shader chain version the engine expects (`dm/gfx/tools/dmsh_chain.py --header`) |

None of these contain game data: they are hashes, file names and facts measured from the game, from which the setup on the phone finds and
translates the player's own copy.

Keys for the one source tree that builds Galar Native, Arceus Native and Paldea Native (docs/ENGINE_UNIFY.md); none of them starts with `nvn_` / `shader_`, so none changes a shader chain:

- `port` (galar / arceus / paldea): the released app the profile ships in → `DM_PORT_*` in game_profile.h (android_main.c's per-app parts). A profile with `tables_from` takes it from there.
- `engine_quirks` (words): engine defaults kept per game where the merged engines disagreed → `DM_GAME_ENG_*` (the list and what each does: `ENGINE_QUIRKS` in game.py). The ones that decide a per-GPU default (qc11_no_eds, async_pso_off, driver_selftests, nvn_async_never) reach the GPU table, dm/gfx/nvn/vk_quirks.h, as `gpu_id.eng` bits; docs/ENGINE_UNIFY.md lists each game's rules.
- `vk_split` (draws, default 150): a submission not held whole goes to the GPU in parts of that many draws (nvn_vk.c sub_split; `DM_VK_SPLIT` overrides) → `DM_GAME_VK_SPLIT`. Scarlet / Violet keep 150 (they wait on the submission they just made); Sword / Shield 600. A profile with `tables_from` takes it from there.
- `vk_split_held` (split / whole, default split): a frame held whole until its present (`DM_VK_FRAME_HOLD`) split like any other (split) or kept in one piece (whole: Sword / Shield; `DM_VK_SPLIT_HELD=1` / `0` overrides) → `DM_GAME_VK_SPLIT_HELD_WHOLE`.
- `docs_dir`: the shared save folder `Documents/<docs_dir>/<title id>/save` (android_main.c save_place) → `DM_GAME_DOCS_DIR`; GameSettings.docsDir must agree.
- `xlate_chain`: the shader translation chain generation (dm/gfx/chains/chain_tree.py): no key = the live tools in dm/gfx/tools; `pre-fmz` = the translator before FMZ, which Arceus Native and Paldea Native ship. Check every profile with `python3 dm/gfx/chains/chain_tree.py --verify`.

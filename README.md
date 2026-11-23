# Paldea Native

**Pokémon Scarlet and Violet (Switch, update 4.0.0) running natively on Android.** Not emulation: the game's own ARM64 code runs on the phone, and its graphics (NVN) run on Vulkan. Built on the engine of [Galar Native](https://github.com/simipoB/Galar-Native-Android) (Sword and Shield).

> [!WARNING]
> **Work in progress.** On a Galaxy S25+ (Adreno 830) Violet and Scarlet boot, and Violet plays its opening at about 30 fps: language, look and name selection, the academy application with its ID photo, and the 3D classroom scene. Gameplay past the opening has not been tested yet. The current release, **v0.96, is a pre-release**: it brings Scarlet and Violet up to the engine Galar Native and Arceus Native share (one engine, one version number), and **Scarlet and Violet have not been played on our test phone with it.** See [Known problems](#known-problems).

> [!NOTE]
> **No game files, code, shaders or keys are included.** You need your own dump (base game + update 4.0.0) and your console's `prod.keys`.

## Install

1. Install `PaldeaNative.apk` from [Releases](https://github.com/simipoB/Paldea-Native-Android/releases) (or [build it](#building)).
2. Choose your **game files** (`.nsp` / `.xci`: base game, update 4.0.0, any DLC) and **keys**. Keys stay on the phone.
3. **Set up the game.** Setup detects Scarlet or Violet from your files. One app runs both, each with its own setup, saves, settings and mods, its own name and icon on its home screen shortcut.

- Official releases are signed with `SHA-256: a5f49b44b6181758d82504f3beb0851c92093f454c792b49688728130673a070`, the same key as Galar Native's and Arceus Native's. Any other build says "Unofficial build". An official update installs over the last one (v0.60 or v0.82) and keeps your setup and saves; v0.96 needs no new setup.
- **Updates:** ☰ → About → Updates checks this repository's releases for a newer full release (never a pre-release) and installs it after you confirm. It only installs the official build signed with the key above. Turn off **Look for updates when the app starts** to stop the check.

## Requirements (expected)

- Android 12+, arm64, Vulkan 1.0 or later (Vulkan 1.0 not yet tried on a real phone).
- About 11 GB free per game: the game data (9.3 GB) and its translated shaders, compressed (0.85 GB).
- 8 GB of RAM or more; 12 GB recommended. Scarlet and Violet are heavier than Sword and Shield: **Auto** settings pick one tier lower for them.

## What it shares with Galar Native

Everything that is not game-specific comes from the same engine and app:

- **In-game settings panel** (☰, or − and + held together on a controller): picture, upscaling (Sharp, SGSR, Smooth, FSR 1), colour, graphics, mode, frame rate, battery saver, textures, rumble, performance overlay, controller remapping, touch controls (with floating sticks), mods, saves, online room, graphics driver, language, quit the game, bug reports, updates.
- **Shaders built before you play:** every shader your phone has used is built before the game starts (☰ → Performance → Shaders at start → While playing for the old way). The very first play still builds them as it goes.
- **Auto settings** from the phone's CPU, GPU and memory tiers, and the display's refresh rates.
- **Mali, Xclipse and PowerVR:** the game's BC textures converted to ETC2 / EAC / ASTC once and kept on disk (the size they have on the Switch, not 4–8 times it).
- **Graphics drivers:** the phone's own, Turnip, PanVK or any driver package.
- **Mods:** RomFS and `.pchtxt`, load order, conflict warnings, notes; their shaders translated before the game starts, and shaders the game builds while it runs translated in the background.
- **Setup that fits the phone:** workers sized to the memory the phone has free (4 GB phones included), the shader pack compressed.
- **Languages:** English, Español, Português, Français, 日本語.

## What is specific to Scarlet and Violet

- **Trinity engine data.** Almost everything is in `arc/data.trpfs`, every file Oodle-compressed. Oodle's decoder is not open source, so setup decodes with the game's own decoder (`dmoodle`: the engine running the game's `OodleLZ_Decompress`), and only the files that hold shaders leave it. Violet has 35,937 shaders.
- **Memory.** The game reserves 12 GB of address space for its allocator and the SDK 1 GB more for its own bookkeeping, more than a phone process can map; both are cut (4 GB, 62 MB) as for Pokémon Legends: Z-A, which uses the same engine.
- **Scarlet and Violet share one engine build and one shader chain**, as Sword and Shield do.
- **Frame rate:** 30 is the default. 60 and Dynamic are Paldea Native's own frame-rate code (experimental, not played on a phone yet): the game's step follows the frame rate the phone really holds, so the game speed should stay right. A change applies from the next start.
- **Mode:** Auto, Quality (docked, 1080p) or Performance (handheld, 720p). Switching to handheld applies while playing; switching back to docked applies from the next start.
- **Frame generation** is switched off for Scarlet and Violet for now, including Lossless Scaling.

## Known problems

- **Not played on our test phone** with v0.96: it was built and checked on a PC.
- **Frame rate 60 and Dynamic** have not been played on a phone yet. If anything looks too fast, too slow or jumpy, go back to 30.
- **No 3D render scale, widescreen camera or heat guard yet** (Widescreen only stretches the picture, like Fill).
- **Rocks, walls, trees and building fronts may be invisible** on some phones; the fix needs a new setup and is not in this release.
- **Mali:** with Arm's own driver, a new route should help where characters were missing (not confirmed). With PanVK, new textures can load slowly, and a black screen was reported with a test build.
- **Xclipse 530:** about 20 to 27 fps, and the phone gets very hot.
- **Snapdragon:** Faster rendering is now on by default; if the game misbehaves or closes at a loading screen, set ☰ → System → Faster rendering → Off.
- Occasional short hitches during play.

Full notes for each version are on the [releases page](https://github.com/simipoB/Paldea-Native-Android/releases).

## Building

```sh
GAME=01008F6008C5E000 scripts/build_apk_pc.sh   # → PaldeaNative.apk (Scarlet + Violet)
```

## Credits

The engine and tools are Galar Native's: see its README. Pokémon Legends: Z-A's port in the same engine showed the way through Trinity's packs and its allocator.

Pokémon, Pokémon Scarlet and Pokémon Violet are trademarks of Nintendo, Creatures Inc. and GAME FREAK inc. Not affiliated with or endorsed by them.

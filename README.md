# Galar Native

*(formerly PKShield)*

**Pokémon Sword and Shield (Switch, update 1.3.2) running natively on Android.** Not emulation: the game's own ARM64 code runs on the phone, and its graphics (NVN) run on Vulkan.

> [!WARNING]
> **Beta.** See [Compatibility](#compatibility). Reports with logs help most ([CONTRIBUTING.md](CONTRIBUTING.md)).

> [!NOTE]
> **No game files, code, shaders or keys are included.** You need your own dump (base game + update 1.3.2) and your console's `prod.keys`.

## Install

1. Install `GalarNative.apk` from [Releases](https://github.com/simipoB/Galar-Native-Android/releases) (or [build it](docs/BUILDING.md)).
2. Choose your **game files** (`.nsp` / `.xci`, update 1.3.2, any DLC) and **keys**. Keys stay on the phone.
3. **Set up the game.** Setup detects Sword or Shield from your files. When it's done, the app opens straight into the game.

- One app runs both games, each with its own setup, saves, settings and mods. With both installed, the app icon lets you choose; each game's home screen shortcut has its own name and icon.
- It updates PKShield in place (package `com.nxnative.pokemonshield`), so your setup and saves are kept.
- Official releases are signed with `SHA-256: a5f49b44b6181758d82504f3beb0851c92093f454c792b49688728130673a070`. Any other build says "Unofficial build".

## Requirements

- Android 12+, arm64, Vulkan 1.0 or later.
- About 15 GB free per game. 8 GB of RAM or more is best; on 6 GB phones the app picks lighter settings for you.

## Compatibility

| GPU | Driver | Status |
| --- | --- | --- |
| **Adreno 6xx–8xx** (Snapdragon 845+) | Qualcomm or **Turnip** | Plays. Turnip fixes old-driver crashes (#44). |
| **Mali G57 / G68 / G77 / G78** | Stock or **PanVK** | Starts; stock driver has rendering gaps (#32). Try PanVK. |
| **Mali G52 / G72 / G76** | Stock or PanVK | Low settings. |
| **Mali G610–G925, Immortalis** | Stock only | White-screen fix (#40, #47), awaiting reports. |
| **Xclipse 540 / 940** | Stock | Crash fixes (#41, #45), awaiting reports. |
| **PowerVR** (Tensor G5 / G6) | Stock | Untested. |

## What's new

**v0.95**

- **A steady 30 fps.** When the phone can't keep up, the game lowers its 3D resolution step by step until it can, and raises it again once things calm down. The HUD and text stay sharp.
- **Heat guard.** When the phone gets hot, the 3D resolution drops before the phone throttles itself, on every Graphics setting.
- **Frame rate: Auto, 30, 60 or Dynamic**, changed while you play. *Dynamic* runs at 60 where the phone can and 30 where it can't, at the full picture.
- **Resolution: 720p, 1080p or 1440p** (from the next start), and a **3D render scale** slider.
- **Widescreen:** the HUD now stays 16:9 in the middle instead of stretching.
- **Frame generation** draws the in-between pictures without the HUD (no more doubled text) and uses depth to keep characters clean.
- **Older and less common phones:** Vulkan 1.0 drivers are supported, older Snapdragon drivers no longer show exploding polygons, PowerVR textures use a safer format, and older ARMv8.0 CPUs no longer crash on certain instructions.
- **Smoother and lighter:** faster texture loading, fixes found by Vulkan's own validation checks, and a memory report in the log.
- **Camera sensitivity is a slider** (50–200 %), like every other setting given in percent.
- **Setup** keeps the game's icon (home screen shortcut, recent apps) even when it stops partway.

## Playing

- **Touch:** on-screen controls, with **☰** (settings) next to −. Two-finger tap = B.
- **Controllers:** hide the touch controls automatically. Hold **−** and **+** together for 1 s for ☰, or **−** for 3 s to remap buttons. Rumble works on controllers and phones.
- **Handhelds** (AYN Odin / Thor, Retroid Pocket, AYANEO, Anbernic, Logitech G Cloud...): detected; the built-in controls are used from the start.
- **Logs:** `Download/PokemonSword-log.txt` or `Download/PokemonShield-log.txt`.

## Settings (☰)

| Setting | Choices |
| --- | --- |
| Picture | 16:9, fill screen, widescreen (the HUD stays 16:9) |
| Resolution | 720p, 1080p, 1440p (from the next start) |
| 3D render scale | Auto, or 50–100 % |
| Upscaling | Auto, Sharp, SGSR, Smooth |
| Graphics | Auto, High, Low, Adaptive |
| Mode | Auto, Quality, Performance |
| Power | Balanced, Battery saver |
| Textures | Auto, Full, Half |
| Frame rate | Auto, 30, 60, Dynamic (changes while you play) |
| Frame generation | Off, On, Lossless Scaling (your own `Lossless.dll`) |
| Graphics driver | Phone's own, Turnip, PanVK, or any driver package (with their options) |
| Controls | Button layout, remapping, camera sensitivity (50–200 %), touch look and layout, rumble |
| Performance overlay | fps, temperature, GPU, free memory, battery |
| Online room | Server and room code for local play over the internet |
| Mods, saves | Import, switch, order and annotate mods; export / import saves |
| Language | English, Español, Português, Français, 日本語 (Auto: the phone's) |

**Auto** rates the phone's CPU, GPU and RAM and picks graphics, textures and filtering from that; while you play it keeps the frame rate steady and the phone cool. The menu shows what it picked.

## Local play

Link battles, trades and Max Raids with nearby phones on the same Wi-Fi, or with friends anywhere through an **Online room** ([host your own server](dm/server/README.md), e.g. on Railway). Mods must be off. Nintendo's online services aren't available.

## Mods

Import a mod `.zip` (`romfs/` file replacements, `exefs/*.pchtxt` code patches for 1.3.2) and switch mods on or off in ☰ → Mods. **Order and notes** sets the load order (higher wins), warns when two mods replace the same files, and keeps each mod's readme as its notes. If the game crashes with mods on, they're switched off on the next start. When the mods change, the app translates any new shaders they bring before the game starts (a short wait, once).

## Known gaps

- Nintendo's online services (Surprise Trade, online battles and raids).
- Wild Area water renders darker than on the Switch.

## License

Galar Native is free software under the **GNU General Public License, version 3 or later** ([LICENSE](LICENSE), [NOTICE](NOTICE)): you may use, share and change it, and anything you distribute that is based on it must be under the same license with its source available. The code that came from d2r-mobile / nxnative, and Galar Native's code from before the switch, also stay under their original [MIT notice](LICENSE-MIT). Lossless Scaling is not included: its frame generation uses the `Lossless.dll` from your own copy.

## Credits

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** and **nxnative** (MIT): the engine this is built on.
- **Ryujinx** (MIT): shader translator. **[LibHac](https://github.com/Thealexbarney/LibHac)**: NSP / XCI reading.
- **[libadrenotools](https://github.com/bylaws/libadrenotools)** (BSD-2-Clause) and **Mesa Turnip / PanVK**: custom drivers.
- **[Snapdragon Game Super Resolution](https://github.com/SnapdragonStudios/snapdragon-gsr)** (Qualcomm, BSD-3-Clause): the SGSR upscaler.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**, **yuzu**: references.
- **SPIRV-Cross**, **CPython**, **capstone**, **LZ4**, **libvpx**, **stb_vorbis**, **Vulkan Headers**, **DejaVu Sans**, **native_app_glue**.

Pokémon, Pokémon Sword and Pokémon Shield are trademarks of Nintendo, Creatures Inc. and GAME FREAK inc. Not affiliated with or endorsed by them.

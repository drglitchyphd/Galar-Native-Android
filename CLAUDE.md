# CLAUDE.md — Galar Native Fork

Read `STATUS.md` first, then `NOTES.md`. Full plan and findings: `PLAN.md`.
Do not re-explore the repo if `NOTES.md` already answers the question.

## Scope
Pokémon Sword 1.3.2 only, Galaxy Z Fold 8 Ultra. Work only on the current milestone in
`STATUS.md`. Deep UI changes, true 60fps and per-screen performance are out of scope.

## Rules
- Every new feature is a setting that defaults to stock behavior.
- Fork code goes in new files where possible; keep edits to upstream files small and marked `// FORK`.
- The main app's extracted game files are **read-only**. Never write to them.
- Settings, saves, mods, control layouts and shader cache use the fork's own storage.
- Leave Shield code in place; don't target or test it.
- Never add game files, keys, shaders or dumps to the repo.
- Prefix all fork log lines with `[FORK]`.

## Build
Linux or WSL (Ubuntu). Details: `docs/BUILDING.md`.
- **Fork APK: `bash scripts/build_fork.sh`** → `~/psa-build/GalarFork.apk` (package `com.nxnative.galarfork`, data in `/storage/emulated/0/GalarFork`; settings at the top of the script)
- Stock APK: `bash scripts/build_apk_pc.sh` (first run fetches NDK, .NET, SDK tools into `~/psa-build`; custom-driver support is on by default, `TURNIP=0` turns it off)
- Stock output: `~/psa-build/GalarNative.apk` (name comes from `dm/games/game.py <GAME> app_name`)
- Engine only, after `dm/host`, `dm/gfx` or `dm/android/app/jni` changes:
  `cd dm/android && make -f Makefile.host GAME=01008DB008C2C000 NDK=~/psa-build/android-ndk-r27c lib`, then rerun the full script (finished parts are reused)
- Keep `~/psa-build/debug.keystore`: updates only install over a build signed with the same key.

## Test
Steven installs the APK on the Fold and reports back with:
screen + orientation, what he did, what happened, and `[FORK]` log lines from
`Download/PokemonSword-log.txt`.

## End of session
Overwrite `STATUS.md` (max ~30 lines): what works, what's broken, next step.

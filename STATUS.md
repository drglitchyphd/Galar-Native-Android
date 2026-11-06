# STATUS

**Milestone:** M1, fork identity and own setup. **Built** (GalarFork.apk, debug key from the cloud session), not yet tested on the Fold.

## Decision
- The fork does its **own setup** (no Shizuku, no sharing with the main app).
- All of its per-game files live in **`/storage/emulated/0/GalarFork/`**, outside scoped storage: game files, saves, settings, mods,
  shader caches and logs. Sword is in the `0100ABF008968000/` subfolder. Keys stay private (`filesDir/keys`).

## Changes
- `scripts/build_fork.sh` (new) sets the package `com.nxnative.galarfork`, the name "Galar Fork", a blue icon and the data folder.
  All of these can be changed with environment variables.
- `Fork.java` (new) resolves the data folder, adds `.nomedia`, and shows the "allow all-files access" screen before setup.
  It also passes `DM_DATA_ROOT` to the engine.
- Small `// FORK` edits:
  - `GameSettings.root`, `SetupActivity.onCreate/onResume`, `GameActivity.onCreate`.
  - `android_main.c host_start`: reads `DM_DATA_ROOT` and logs `[FORK] data folder: ...`.
  - `build_apk.py`: `--data-dir`, `--icon`. `build_apk_pc.sh`: `APK_ARGS`, `APK_OUT`.
- A stock build (`build_apk_pc.sh`) behaves exactly as before.

## Checked here
- All app Java compiles against Android 15's framework classes. `build_apk.py` and both scripts pass syntax checks.
- Full APK built in the cloud: package `com.nxnative.galarfork`, label "Galar Fork", `assets/data_dir`, and the `[FORK]` log line in `libnxnative.so` are all confirmed in the APK.

## Test on the Fold
1. `bash scripts/build_fork.sh` and install `GalarFork.apk` next to the main app.
2. Open it, allow all-files access, set up Sword. Check that `GalarFork/0100ABF008968000/` fills up and the main app is untouched.
3. Play, save, quit and reopen. Then check that the log has `[FORK] data folder:`.
4. **Watch for loading time or stutter**: shared storage can read slower than `Android/data`. Compare with the main app.

## Known gaps
- Two in-app hints still say `Android/data/...` (the mods-folder hint in `GameMenu`, and the setup-log path).
- Uninstalling leaves `GalarFork/`. That is intended; delete it by hand to start over.

## Next
- After a good test, M2 (fold/unfold).

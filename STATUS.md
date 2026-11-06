# STATUS

**Milestone:** M0 Recon, done (no code changes). Next is M1.

## Done
- `NOTES.md` written: covers all nine M0 topics.
- `CLAUDE.md`: build command and APK path filled in.

## Findings that change the plan
1. **Shared files are app-private.** The main app's Sword files are in
   `Android/data/com.nxnative.pokemonshield/files/0100ABF008968000/`. "All files access" can't read
   another app's `Android/data` on Android 11+, so M1 needs Shizuku, or the fork does its own setup (~15 GB).
2. **The shared-files version check already exists.** The `ready` marker holds a shader-chain string. The fork must
   match the main app's chain, or it treats the files as stale.
3. **Fold may already work:** the activity isn't recreated and the swapchain rebuilds on resize.
   Inner portrait is blocked by `screenOrientation=sensorLandscape`.
4. **The app's 60fps is not 2× speed.** It uses the same patch site, but `vstep_tick` re-times the game step.
   The crowd flicker belongs to that path.
5. **A touch-control editor already exists** (hold − for 2 s). It keeps one layout, not one per screen.
6. **Picture modes live in `dm_picture_box()`** (`gfx/nvn/nvn_ovl.h`). That is the hook point for M3.

## Not tested
- Nothing built or run this session. The cloud container has no NDK/SDK set up; build on the PC.

## Next (M1)
- Decide the shared-files route: Shizuku or own setup.
- New package (`build_apk.py --package`), name and icon. Check `--game 0100ABF008968000` for a Sword-only build.
- Steven: before M2 work, fold/unfold mid-battle on the current release and report the `log.txt` lines.

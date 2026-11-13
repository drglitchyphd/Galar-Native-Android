# STATUS

**Build:** v46 (not tested yet). Branch `fork/v20`, upstream merged to 4e0814b (check `git fetch upstream main` before every build).
Upstream policy (Steven, Nov 13): merge from upstream `main` only, when needed; no unmerged upstream branches or other forks. Watch: upstream branch `ultrawide-camera` patches the same addresses as Fill, corrected (fork_aspect.c); if it reaches main, keep ours.

## New in v46
- Fixed: since v44 a comment swallowed the line in `fork_ui.h` that saved the shrunk draw's shader and viewport (logs showed `ui draw kind: vertex ,`). So v44's spotted banner, v43's intro bars / divider never matched. First build where they run.
- Menu cleanup (Steven, Nov 13):
  - Speed → **Smoothness** = Auto / 30 / 60 (Lossless 2x) / 90 (Lossless 3x) / 60 fps mode. Writes DM_FPS_TARGET + DM_FORK_FG, clears DM_FRAMEGEN. Upstream frame generation and its DLL import are no longer offered.
  - The pacer counts in the screen's top display mode (DM_DISPLAY_HZ, 120 on the Fold) automatically. The Pacing refresh setting is gone (DM_FORK_MAX_HZ ignored).
  - Fork frame generation **3x** (fork_fg.h: 2 generated frames, step = interval / 3). It also asks Android for 120 Hz now, like upstream's did.
  - Controls → **Fork extras** (scheme, dead zone, colour, size, fast-forward, turbo) and **Battle controls: Same / Battle mode** (DM_FORK_BATTLE = hide + icons + own layout; old keys migrated once) + battle opacity / fade.
  - System → **Leaving the game**: Keep playing / Pause / Pause, stay open / Keep playing, stay open (DM_BACKGROUND + DM_FORK_KEEPALIVE).
  - "Capture the next odd frame" removed. The trainer capture stays until the banner is confirmed.

## Works (Steven)
- Setup in the shared folder, launch, battles, fold / unfold, portrait, Fill, corrected camera, DLC, layout editor + shapes, floating sticks.
- Fork frame generation 2x (Turnip, pacing Smooth): good. Fast-forward works on upstream's real-time step (v45).

## Open
- Trainer spotted banner: b6ff050a + 1200x180 texture, anchored to the bottom edge (log: `[FORK] spotted banner`). Untested because of the bug above.
- Intro bars / divider (fa0b9117, 63b6f752) full size: untested, same reason. Split-screen divider may be fixed by it.
- 60 fps mode "doesn't do anything" (without frame generation): needs that run's log.
- Upstream frame generation: built-in works but not smooth; upstream Lossless did nothing. Dropped from the menu.

## Next
1. Steven tests v46: banner, intro bars / divider, Smoothness 2x vs 3x, battle mode, menu.
2. Encounter info overlay (shiny / IVs / nature) from game memory, then the live HUD research. Save slots after.

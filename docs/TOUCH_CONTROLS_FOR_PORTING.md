# Touch controls: baseline, fork additions, and porting notes

What the touch controls in Galar Native (upstream) do, what this fork added, and how to carry the design to another game, such as a GBA recompilation. File and setting names refer to this repo (`dm/android/app/jni/touch_ui.c`, `ForkTouch.java`, `GameMenu.java`), so the details can be looked up.

---

## 1. Baseline (upstream Galar Native)

Upstream's own design notes say its proportions and look came from LAS Recomp's Android port.

### Look
- **Shape:** glossy "bubble" buttons drawn by the game's own renderer, over the game picture. There are no Android views, so no extra layers or lag.
  - Each button is drawn in layers: shadow, dark rim, body, glow, gloss spot, bright rim, then its label.
  - Labels come from a small glyph atlas: 64×64 cells, one grey channel.
- **Sizes** are in units of the screen height, so the layout scales with any screen.
- **Default layout** follows a Switch pad:
  - A B X Y diamond bottom-right.
  - L / R in the top corners.
  - − and + top centre.
  - Movement stick bottom-left.
  - Right stick below-left of the diamond.

### Input behaviour
- **Movement stick:** a ring that rests bottom-left and jumps to where the thumb lands in the lower-left area. The thumb's offset from that point is the tilt.
- **Camera:** dragging anywhere on the right half turns the camera at the finger's speed. It stops as soon as the finger stops.
- **Minimum press of 80 ms.** The game reads input once a frame (33 ms at 30 fps), so a quick tap must last long enough to be seen.
- **Touch areas are 1.3× the drawn button,** because thumbs miss.
- **Slide between face buttons.** A thumb sliding across A/B/X/Y presses whichever one it is over.
- **Controller detection.** The controls hide while a controller is used and come back on the next touch.

### Settings and editor
- **Settings:**
  - Opacity (20–100%).
  - Fade after N seconds without a touch.
  - Vibrate on press.
  - Optional L3 / R3 buttons.
- **Layout editor:**
  - Hold − for 2 s to open it.
  - Drag to move a control, pinch to resize it, double-tap to hide or show it.
  - Hold + to reset; hold − again to save and leave.
- **Layout file:** one per game, a text file with one line per control:
  `name fx fy fr shown`
  - `fx = x / screen width`
  - `fy = y / screen height`
  - `fr = radius / screen height`

---

## 2. What the fork added

Every addition is a setting, and every setting defaults to upstream's behaviour.

### Control schemes
- **Floating sticks** (`DM_FORK_TOUCH=float`):
  - The left half of the screen is the movement stick and the right half is the camera stick.
  - Each stick's centre is wherever the thumb lands; the tilt is the offset from there, capped at the stick radius.
  - The fixed sticks are hidden. Only the stick under a thumb is drawn, and it disappears when the thumb lifts.
- **Tap for A:** a quick tap that isn't on a button presses A. Quick means under 0.3 s and moving less than 2% of the screen height.
- **Two-finger tap for B:** two fingers landing within 0.4 s of each other press B. Single taps are muted for 0.3 s afterwards, so the second finger doesn't also press A.
- **Stick dead zone** (`DM_FORK_DEADZONE`, 0–40%, default 12%). It's applied to the tilt as a whole, not per axis, so diagonals still reach a full push.
- **Touch camera** (`DM_TOUCH_CAMERA`):
  - **Drag** (upstream): camera speed follows the finger's speed.
  - **Stick:** the camera keeps turning while the finger is held away from where it landed. Full speed is at 12% of the screen height, with an 8% dead zone.
- **Sensitivities:**
  - Movement stick, 100–200%. The push is scaled along its own direction, so you reach full speed with less travel.
  - Touch camera, 25–300%, separate from the controller stick's sensitivity.
- **Hide the right stick** (`DM_TOUCH_RSTICK=off`).

### Look and layout
- **Per-screen layouts.** Each screen and orientation keeps its own file: `touch_layout_<profile>.txt`, with profiles `inner_landscape`, `outer_landscape`, `inner_portrait`, `outer_portrait` (a foldable has two screens).
  - Portrait puts the game picture at the top and the controls in the space below it.
- **Context layouts:** a suffix per screen kind, e.g. `inner_landscape_battle`, `_menu`, `_dialogue` (see Context controls). Each starts as a copy of the normal layout.
- **Button shapes:** circle, rounded square, square, or pill. Stored as a sixth field in the layout file: `name fx fy fr shown shape`. Old five-field files still load as circles.
- **Colour** (blue, grey, white, dark, red, green, purple) and **overall size** (50–150%).
- **Opacity** down to 5% (upstream stops at 20%).
- **Fade behaviour:**
  - The D-pad and sticks fade with everything else; upstream's D-pad vanished on its own.
  - Battle mode has its own opacity and fade time.

### Editor
An Android bar floats over the editor (`ForkTouch.java`):
- **Save / Reset / Cancel** buttons.
- **Size slider** for the selected control, 2.5–25% of the screen height. It's more precise than pinching.
- **Shape** button: cycles the selected control's shape.
- The bar can be **dragged** by its label, and remembers its position per screen. It starts at the bottom centre, so it doesn't cover the top buttons.
- The overlay holds up to 320 drawn items, because the editor shows every control at once.
- **Stricter holds:** reset needs + and − held together and still. Dragging + alone no longer resets the layout.
- Controls that don't apply to this game are left out of the editor (e.g. the Legends: Arceus throw button in Sword).

### Context controls
The fork guesses which screen is showing from what the game draws: overworld, full-screen menu, battle, or dialogue.
- **Battle mode:**
  - Unused buttons hide.
  - Battle icons replace letters: X → Poké Ball, B → back arrow, Y → info.
  - Battles get their own layout.
- **Dialogue:** only A, B, the D-pad (for yes/no choices), turbo and fast-forward show.
- **Menus:** the sticks, ZL/ZR and turbo hide.
- **Overworld:** extras hide.
- **⊞ button:** brings every button and the normal layout back until the screen changes. It exists because the screen guess can be wrong.
- **Switching delay:** the layout changes only after the screen kind has held for 10 frames. Leaving a battle for the overworld waits 90 frames, because battle text phases briefly hide the battle HUD.

### Extra buttons
- **Fast-forward ⏩:** tap to toggle, hold for fast-forward while held. Speed 1.25–3×. Can be limited to battles.
- **Turbo A ⚡:** A pressed repeatedly while held, 3–20 presses a second.
- **☰ button:** opens the app menu. It's never fully faded, so it's always findable.
- **Gesture shortcuts** (optional): a three-finger tap opens the menu; a two-finger swipe down presses X.
- **Android Back** (button or gesture) presses B for 80 ms.

### Reliability fixes
- **Lost fingers:** a finger Android never sends a lift for is released. Every move event lists all fingers down, so a tracked finger missing from that list has gone.
- **Focus loss:** every finger is released when the app loses focus (notification shade, game overlays, the menu). Otherwise those buttons stay pressed.
- **Diagonals:** controller sticks also use a dead zone on the tilt, not per axis. Per-axis dead zones shorten diagonals to 85–94% of a full push.

---

## 3. Design principles worth keeping

1. **Draw the controls in the game's own renderer** (or one final overlay pass), not as Android views. This means no extra composition layer and no input lag from UI threads.
2. **Measure everything in units of the screen height,** and store layouts as fractions of the screen size. One layout then works on any resolution.
3. **One layout per screen and orientation,** plus optional per-context layouts. Foldables and portrait mode need this.
4. **Enforce a minimum press length** (≥ one game frame, ideally about 80 ms) and use touch areas larger than the drawn buttons (about 1.3×).
5. **Make taps on empty space useful** (tap = A, two fingers = B). Most input in turn-based or menu-heavy games is "confirm" and "back".
6. **Release everything on focus loss and for fingers Android forgets.** Ghost presses are the most common touch-control bug report.
7. **Every feature is optional and defaults to the simple behaviour.** Users who just want buttons get buttons.
8. **When context detection is a guess,** always give the player a one-tap "show everything" fallback.

---

## 4. Applying it to a GBA recompilation

GBA buttons: **D-pad, A, B, L, R, Start, Select**. There are no analogue sticks and no camera.

| This fork | GBA version |
|---|---|
| Floating movement stick | A **floating D-pad**: the left half of the screen, centred where the thumb lands. Map the tilt to 8 directions with 45° sectors and a dead zone of about 15–20%, so the thumb's angle presses one or two D-pad buttons. A fixed on-screen D-pad stays available as an alternative scheme. |
| Floating camera stick (right half) | Not needed. The right half becomes the **tap area**. |
| Tap empty space = A | Same: a quick tap on the right half = A. Optionally tapping the left half while the floating D-pad is idle = A too. |
| Two-finger tap = B | Same. |
| A B X Y diamond | **A and B** in the GBA's diagonal layout (B lower-left, A upper-right), bottom-right. |
| L / R top corners, − / + top centre | **L / R** top corners; **Select / Start** top centre (or bottom centre, GBA style). |
| Fast-forward ⏩, turbo | Very useful in GBA games: keep both. Turbo can apply to A and B. |
| Battle / dialogue / menu modes | Optional. Detecting screens needs game-specific knowledge (a memory flag or a known draw pattern); with a recomp, game RAM can be read directly. If added, keep the ⊞ fallback. |
| Per-screen / orientation layouts | Keep. Portrait suits GBA well: the picture at the top, the controls below, like a GBA SP. |
| Shapes, colour, size, opacity, fade | Keep as they are: they are game-independent. |
| Back gesture = B | Keep. |

### Suggested minimal set for a first GBA build
1. Fixed controls: D-pad, A, B, L, R, Start, Select. Use screen-height units, 1.3× touch areas and an 80 ms minimum press.
2. Layout editor: drag, size slider, hide, save / reset / cancel, one file per orientation (the `name fx fy fr shown shape` format).
3. Floating D-pad on the left half + tap = A + two-finger tap = B, as an optional scheme.
4. Fast-forward and turbo buttons.
5. Focus-loss and lost-finger release.
6. Opacity, size, colour, fade.

### Input pipeline (how this fork does it, to copy)
1. Android touch events go to one handler (`touch_ui_event`). Each finger is tracked by its pointer ID: what it landed on, where it started, where it is now, and when it landed.
2. Once per frame (`recompute_locked`), the button bits and stick values are rebuilt from the tracked fingers. Taps are converted to timed presses (`dm_input_press(button, 80 ms)`).
3. The result goes to the game's input state in one call (`dm_input_set(buttons, lx, ly, rx, ry)`). For a GBA recomp this would be the 10-bit KEYINPUT state.
4. Drawing builds a list of simple items (disc, ring, rounded rectangle, glyph) that the renderer draws in one pass after the game's frame.

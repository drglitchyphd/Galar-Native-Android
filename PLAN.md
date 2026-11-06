# Galar Native Fork — Plan (Sword only, Fold 8 Ultra)

Fork of [simipoB/Galar-Native-Android](https://github.com/simipoB/Galar-Native-Android) (MIT).
Target: Pokémon Sword 1.3.2 only (build ID `A3B75BCD3311385AEED67FBEEB79CBB7BF02F471`).
Device: Galaxy Z Fold 8 Ultra (outer + inner screen).

## Ground rules

- **Every feature is a toggle that defaults to stock behavior.**
- **Installs side by side** with the main app: own package name, app name and icon.
- **Shares the main app's extracted game files, read-only.** Never writes to them.
- **Everything else is separate:** settings, saves, mods, control layouts, shader cache.
- **Keep fork code isolated** (new files, minimal edits to upstream files) so upstream updates merge cleanly.
- Shield code stays in place but is not targeted or tested.
- No game files, keys, shaders or dumps in the repo, ever.

## Out of scope (for now)

- Deep UI changes (tap-to-select menus, battle tap menus, layout redesigns)
- True 60fps at normal game speed (needs game timing patches)
- Per-screen performance settings
- Save states (not feasible: native execution, no emulator snapshot)

---

## Findings so far

### 4:3 mod (`exefs/1.3.2.pchtxt`, by Fl4sh / TLin-Y)

Requires "stretch to window." The game still renders a 16:9 frame; the patch writes the
float **4/3 (`0x3FAAAAAB`)** into the camera aspect so the 3D is pre-squashed, and the
stretch to a 4:3 screen makes it correct. The UI is not pre-squashed, so the stretch
distorts it.

| Offset (shift 0x100) | Patched instruction | Notes |
|---|---|---|
| `00D93414` | `movk x12, #0x3faa, lsl #16` | High half of 4/3 float only; find the low-half instruction for arbitrary ratios |
| `00607DE8` | `movk x9, #0xd2ab, lsl #32` | Purpose unclear, verify in recon |
| `00607DEC` | `movk x9, #0x67aa, lsl #48` | Purpose unclear, verify in recon |
| `006076C4` | `b #0x607664` | Jump to code cave |
| `00607664` | `mov w9, #0xaaab` | Cave: w9 = 4/3 float |
| `00607668` | `movk w9, #0x3faa, lsl #16` | |
| `0060766C` | `b #0x6076c8` | Return |

**Plan:** generate the aspect value at runtime from the inner screen's real shape, then
add a UI projection patch that compresses the UI vertically by the same factor, centered.
After the stretch the UI keeps correct proportions (full width, small bands top/bottom).

### 60fps mod (`exefs/1.3.2.pchtxt`, by Morph / DuIslingr / theboy181)

`018A2C88`: `mov w1, #2` → `mov w1, #1`. This is the **present interval** (refreshes per
frame). Game logic advances a fixed step per frame, so interval 1 = 60fps at **2x speed**.
The mod's own notes call it "too fast." The app's fps setting can't override it.

**Plan:** don't ship this as 60fps. Use the same idea for **hold-to-fast-forward**: the fork
intercepts the game's present-interval request in the graphics layer and swaps 2 → 1 while
a button is held. No game code patching.

### App's own 60fps mode

Crowds flicker (likely objects updated every other frame). Not fixing in the fork;
**report upstream** with a log.

---

## Milestones

One milestone per Claude Code session. Each ends with a build, a test on the Fold,
and an updated `STATUS.md`.

### M0 — Recon (no code changes)
Write `NOTES.md` (file paths + function names only) covering:
- Package name / app ID and where it's set
- **Where extracted game files live** (shared storage vs. `Android/data` / internal)
- How setup records its version/format
- Settings storage, save storage, mod storage, shader cache location
- Touch controls: drawing, input mapping, layout settings
- Display/surface setup, "Picture" options (16:9 / fill / 20:9)
- How `.pchtxt` patches are parsed and applied
- NVN present / present-interval handling, and how the app's fps setting works
- Activity lifecycle: what happens on fold/unfold today

### M1 — Fork identity + shared files
- New package name, app name, icon
- Read extracted files from the main app's location, read-only
  - Shared storage → "All files access"
  - Private storage → Shizuku access; fallback is the fork's own setup (~15 GB)
- Own settings, **own saves**, own mods, own shader cache
- Version check: if the main app's extraction format changes, show a clear message instead of crashing
- **Done when:** both apps installed, fork launches Sword from the main app's files, neither touches the other's settings or saves

### M2 — Fold / unfold
- Screen change handled in place, game process never restarts
- Pause on fold and when backgrounded (toggle)
- **Done when:** folding and unfolding mid-battle and mid-overworld keeps the game running

### M3 — Display profiles (per screen)
- **Outer:** stock, unchanged
- **Inner landscape:**
  - Default: largest clean 16:9 picture, controls in the leftover space
  - Experimental toggle: fullscreen via runtime aspect patch + UI projection patch
- **Inner portrait:** 16:9 at full width on top, controls below
- **Done when:** each screen/orientation switches to its own profile automatically

### M4 — Control scheme
- Floating stick: appears under the thumb, follows it, dead zone setting
- Tap anywhere = A
- B, X, Y, +, −, L/R placed per layout
- Size, opacity, haptics settings
- Keep existing two-finger tap = B and controller auto-hide
- Toggle: new scheme vs. stock controls

### M5 — Control editor overhaul
- Drag-to-place over a live game frame
- Separate layouts: outer, inner landscape, inner portrait
- Per-element size and opacity, snap to grid, reset to default

### M6 — QoL
- **Turbo A:** hold to repeat A (toggle + rate)
- **Fast-forward:** hold button, present interval 2 → 1 (toggle)
- **Save backups:** keep last N copies on every game save (toggle + N)
- **Mod profiles:** one tap between "my mods" and "all off" for local play

### M7 — All-Pokémon-catchable mod
- Mod not found yet. Expected to be a romfs encounter-table mod, loaded through the existing mod manager
- Check for file conflicts with other mods; no fork code expected

---

## Settings (all default to stock)

| Group | Toggle |
|---|---|
| Display | Inner landscape mode (16:9 / experimental fullscreen), inner portrait layout |
| Controls | New control scheme, floating stick, tap-anywhere A, haptics |
| QoL | Turbo A, fast-forward, save backups, pause on fold/background |
| Mods | Mod profiles |

---

## Token-efficient workflow

- Work in **Claude Code** on the PC, on the cloned fork. Build locally, install the APK on the Fold.
- `CLAUDE.md` in repo root: build command, scope, rules, test steps. Under a page.
- `NOTES.md`: written once in M0, read instead of re-exploring the repo.
- `STATUS.md`: **overwritten** after each milestone (works / broken / next), ~30 lines max.
- Fresh session per milestone. No long running chats.
- Log tag `[FORK]` on all fork log lines. Bug reports paste only matching lines plus:
  1. Screen + orientation
  2. What I did
  3. What happened

## Report upstream

- App 60fps mode: crowd flicker (with log)

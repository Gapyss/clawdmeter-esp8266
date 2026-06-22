# Plan: Face screen — Claude brand reskin

Reskin the physical LCD `SCREEN_FACE` companion view (the animated 20×20 mascot) from its
black-background "OLED" look into the warm ivory Claude theme used by `SCREEN_CLAUDE`/`SCREEN_MAC`,
so the companion reads as part of the same brand family. Decided via grilling interview (2026-06-23).

## Decisions locked (grilling)

1. **Target:** physical LCD face screen only. The dashboard `view=face` is just relabeled
   generic panels (no mascot art) — explicitly **out of scope**.
2. **Intent:** all four — match ivory paper, add Claude branding/chrome, improve mascot,
   polish layout.
3. **Mascot scope:** *adapt palette only*. Keep the existing creature + 4 scenes
   (idle / working / sleep / monk); recolor to read on cream. No reshape, no redesign.
4. **Chrome:** *mirror the hero chrome* — rounded cream card, `drawClaudeIcon` burst + "Claude"
   wordmark, `C_TAN` hairline. Move mood + S/W% + clock into a footer band.
5. **Monk on cream:** *white robe + ink outline* (with documented fallback — see Risks).

## Scope

- **In scope:** the face block in `clawdmeter_esp8266.ino` —
  `faceCellColor`, `faceDrawChrome`, `faceDrawStatus`, `faceBegin`, palette `#define`s, and the
  `monkBase` art (outline cells). Animation engine (`faceTick`, `faceBuildGrid`, `faceRender`,
  frame tables) is untouched except where noted.
- **Out of scope:** dashboard `view=face` (stays as-is); the `INDEX_HTML_GZ` size guard is never
  touched, so no `gen_index_gz.py` run is needed.
- **Not a concern:** IRAM / `mmu=4816` — pure `fillRect`/`print` drawing, no new ISR/timer.

## Visual concept (240×240)

```
╭─────────────────────────────────╮  rounded card (C_MUTE double border, like hero)
│  ✳  Claude                       │  header: burst + wordmark
│ ────────────────────────────────│  C_TAN hairline at y33
│                                  │
│            [ mascot ]            │  20×20 grid, cells = C_CREAM empty, eye = C_INK,
│         clay body, ink eyes      │  body = C_CLAY (window rows 3–17 → y34–184)
│                                  │
│ ────────────────────────────────│  footer divider
│ ● HAPPY        S42 W08   14:32   │  footer band (BELOW y184): mood(orange) + S/W% + clock
╰─────────────────────────────────╯
```

## Palette changes

| Cell / element | Before (black bg) | After (cream bg) |
|---|---|---|
| `CELL_EMPTY` | `C_BLACK` | `C_CREAM` |
| `CELL_EYE`   | `C_BLACK` (shared default) | `C_INK` — **split from EMPTY** |
| `faceCellColor` `default:` | `C_BLACK` | `C_CREAM` (empty), no path returns black except eye→ink |
| Body `CELL_BODY` | `C_CLAY` | `C_CLAY` (unchanged — reads fine on cream) |
| Bezel | orange `drawRoundRect` | `C_MUTE` double rounded rect (mirror `drawClaudeChrome`) |
| Header rule / divider | `C_CLAUDE` on black | `C_TAN` hairline + `C_CLAUDE` accents on cream |
| Status/mood/clock bg | `C_BLACK` | `C_CREAM` (every `setTextColor(fg, C_BLACK)` → `C_CREAM`; clear `fillRect`s → `C_CREAM`) |
| Monk robe `CELL_ROBE` | `C_WHITE` | `C_WHITE` + new `CELL_ROBE_EDGE` (ink outline) baked into `monkBase` |
| Sleep Z particles | (verify) | ensure ink/orange, not black-on-cream invisible |

## Risks & sequencing

- **Build the MONK scene FIRST to validate, not last.** The 5×5 head dome has burst spokes
  reaching every edge (N spoke row4col10, arm tips row6 cols8/12), so there is *no empty ring*
  around the head to host a clean outline — the outline mostly serves the robe body, and the
  white "halo ring" that popped on black **will soften on cream**. The head must read via the
  **orange burst alone** (highest-contrast pair on cream), with the white halo demoted to accent.
  **Fallback if it looks muddy on-device:** switch robe to warm stone-grey (darker than cream)
  with existing `C_ROBE_SHADE` folds — no outline needed, sidesteps the no-room problem.
- **Headphone light** (`C_HP_LIGHT #d4dde2`) is low-contrast on cream but bounded by
  `C_HP_SHADOW` edge cells. Glance-check the working scene; darken `C_HP_LIGHT` a notch only if
  the headband washes out. Not a blocker.

## Implementation guardrails (CLAUDE.md discipline)

- `fillScreen(C_CREAM)` stays **only** in `faceBegin`. Never add a fillScreen/full fillRect onto
  the `faceTick` path — the per-cell diff in `faceRender` is the no-flash repaint. Same rule as MAC/DESK.
- `faceBegin` already `memset`s `facePrev` to `0xFF`, so a scene/`id` change force-repaints every
  cell — this is what clears black-era leftovers when the palette changes. Verify it still fires on
  `e.id` change in `faceTick`.
- Footer band must live **below y184** (window rows 3–17 = y34–184, header hairline y33) so chrome
  and cell-diff never paint over each other.
- Use hex literals (`0x0000`/`0xFFFF`), not `BLACK`/`WHITE` macros.

## Steps

1. Add palette: split eye/empty in `faceCellColor` (eye→`C_INK`, empty→`C_CREAM`, default→`C_CREAM`);
   add `CELL_ROBE_EDGE`/`C_ROBE_EDGE` (ink) for the monk outline.
2. **Monk first:** add outline cells to `monkBase`; flash & eyeball on-device. If muddy → stone-grey fallback.
3. Rewrite `faceDrawChrome` to mirror `drawClaudeChrome` (C_MUTE card, burst + "Claude", C_TAN hairline).
4. Reskin `faceDrawStatus` + `faceBegin` to cream backgrounds; lay out footer band (mood + S/W% + clock) below y184.
5. Verify each scene on-device: idle, working (headphone contrast), sleep (Z visibility), monk.
6. Compile `--fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f`; OTA flash; confirm no tick-flash.

## Verification

- Each of the 4 scenes legible on cream; no black flash on the per-second / per-push tick.
- Scene switch (`/face?state=`) force-repaints cleanly with no black-era cell leftovers.
- Header matches hero screen family; footer mood/usage/clock all on cream, no ghosting on width change.

## Implemented (2026-06-23)

- Monk outline is **auto-generated** in `faceBuildGrid` (empty cell 8-adjacent to a robe
  cell → `CELL_ROBE_EDGE` ink), not hand-baked into `monkBase` — deterministic, 1-cell thick,
  no PROGMEM hand-placement. Gated on `faceMode == FACE_MODE_MONK`.
- Palette split done (`CELL_EYE`→`C_INK`, empty/default→`C_CREAM`, `CELL_ROBE_EDGE`→`C_INK`).
- Chrome mirrors `drawClaudeChrome` (C_MUTE card, burst + "Claude", C_TAN hairlines); footer band
  below y190 holds mood (spark) + centred S/W% (mute) + clock (ink). `fillScreen(C_CREAM)` only in
  `faceBegin`. Compiles clean (IRAM 69%). Bin at `firmware/bin/clawdmeter_esp8266.ino.bin`.

### Open: monk needs an on-device eyeball (the visual gate a compile can't clear)

The auto-outline rings the **whole silhouette incl. the head**, putting an ink ring tight against
the burst spokes in the 5×5 dome. Intended steer was for the orange burst to carry the head alone.
If the head reads cluttered on-device, two levers (least change first):
1. Gate the outline on `r >= 9` in `faceBuildGrid` (body only; burst carries the head).
2. Full fallback: stone-grey robe (`CELL_ROBE`→ a warm grey darker than cream), drop the outline.

Also glance the **working** scene in the same pass — `C_HP_LIGHT` (#d4dde2) headband is low-contrast
on cream (bounded by `C_HP_SHADOW` edges); darken a notch only if it washes out.

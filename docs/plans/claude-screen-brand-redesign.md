# Plan: Claude screen — claude.ai brand redesign

Re-theme the physical LCD `SCREEN_CLAUDE` view from the abstract dark dashboard into
a warm, on-brand "claude.ai" panel. Decided via grilling interview (2026-06-22).

## Scope

- **In scope:** `SCREEN_CLAUDE` rendering in `firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino`
  (`drawMeter` claude path, `drawClaudeHero`, `drawClaudeWeekly`, `drawClaudeBar`,
  `drawClaudeIcon`, `drawClaudeStatusDot`, `tickDynamic` claude branch, `drawIpPanel`).
- **Out of scope (accepted desync):** the dashboard `view=claude` representation
  (`INDEX_HTML` JS, lines ~401/417) stays dark. The on-device screen and the dashboard's
  Claude view will not match visually — known, accepted gap.
- **Not a concern:** IRAM/`mmu=4816` — this is `fillRect`/`drawRoundRect`/`print` drawing
  code, no new ISR or timer.

## Visual concept

claude.ai brand: warm **cream/ivory** background, **coral** as accent only (burst icon +
bar fills), **dark warm ink** for text, a subtle **rounded card** frame, editorial layout.
Light screen also distinguishes Claude from the dark MAC/DESK retro screens.

## Layout (240×240)

```
╭─────────────────────────────────╮  rounded card border (thin coral/tan inset)
│  ✳  Claude    ● ALLOWED   14:32 │  header: burst + wordmark + status dot/label + clock
│ ────────────────────────────────│  divider rule
│ ▎Session 5h                     │  ▎ = coral binding accent when session is binding
│        42%                       │  big number = DARK INK (coral→ N/A; red in danger)
│  ▓▓▓▓▓▓▓░░░░░░░  reset 14:32 T-2h│  pill bar coral-on-tan + reset time + live countdown
│                                  │
│ ▎Weekly              18%         │  ▎ coral accent when weekly is binding
│  ▓▓▓░░░░░░░░░░░  reset Mon 09:00 │  pill bar + reset day/time
│                          IP …    │  footer IP panel
╰─────────────────────────────────╯
```

## Decisions (locked)

| Topic | Decision |
|---|---|
| Surface | Physical LCD `SCREEN_CLAUDE` only |
| Concept | claude.ai brand redesign (cream + coral accent) |
| Framing | Subtle rounded card border + header divider rule |
| Bars | Rounded **pill** bars, coral fill on tan track (red fill in danger) |
| Big number color | **Dark ink** (coral is accent only); tints **red** in danger |
| Danger signal | Bar fill goes red ≥ danger; number tints red. No bg wash. |
| Status dot + label | **Keep**, restyled for cream |
| Reset times + countdowns | **Keep** (session reset+live T-countdown, weekly reset day/time) |
| IP panel | **Keep**, restyled for cream |
| Binding marker | **Restyle**: coral left-accent (`▎`) / dot by the active block's label (was amber edge bar) |
| No-data / Claude down | Same cream card with `—` placeholders + empty (track-only) bars, `--:--` resets. No black flash. |
| Repaint | **Two-phase, no `fillScreen` on push** (chrome once + dynamic per push) |
| Wordmark | "Claude" mixed case (not "CLAUDE") |

## Palette (add `#define`s near existing C_* at line ~723)

Approx RGB565 (verify on panel, tweak to taste):
- `C_CREAM 0xF7BD` — background (#F5F4EE)
- `C_INK   0x18E3` — primary text / big number (#1F1E1D-ish warm near-black)
- `C_TAN   0xEF5C` — bar track / muted card border (#EDE9E0)
- `C_MUTE` — muted taupe for secondary labels/reset text (pick a mid value readable on cream; the existing `C_GRAY 0x8410` may read too cold — prefer a warm ~#8A8578)
- Reuse `C_CLAUDE 0xDBAA` (coral accent), `C_RED` (danger).

## Two-phase repaint (mirror `macChromeReady`)

1. Add file-scope `static bool claudeChromeReady = false;`.
2. In the `/mode` switch handler (line ~607-611): when switching **to** `SCREEN_CLAUDE`,
   set `claudeChromeReady = false` (parallel to the `SCREEN_MAC` line at 609).
3. `drawMeter` claude path:
   - if `!claudeChromeReady`: `fillScreen(C_CREAM)` → draw **chrome** (rounded card border,
     header divider, burst icon, "Claude" wordmark, static "Session 5h"/"Weekly" labels,
     IP-panel label frame), set `claudeChromeReady = true`.
   - then **always** run dynamic draw (no `fillScreen`).
   - **Remove** the `if (sessionPct<0 && weeklyPct<0) drawWaiting()` early return — let the
     dynamic path render `—`/empty-track placeholders instead (cream-themed no-data state).
4. Dynamic draw = status dot, clock, big %, both pill bars, reset + countdown lines, weekly %,
   binding accent, IP value — all with **opaque cream-bg** prints (`setTextColor(fg, C_CREAM)`).

### Black→cream touch-points (must flip, or they punch black holes in cream)

- `drawClaudeStatusDot` line 774: `fillRect(... C_BLACK)` → `C_CREAM`.
- `tickDynamic` claude branch lines 1188-1194: the two `fillRect(... C_BLACK)` → `C_CREAM`;
  clock/countdown text color `C_WHITE`/`C_GRAY` → `C_INK`/`C_MUTE`.
- `drawClaudeHero` / `drawClaudeWeekly`: every `setTextColor(..., C_BLACK)` → `C_CREAM`;
  `C_GRAY` labels → `C_MUTE`; big % → `C_INK` (or `C_RED` in danger).
- `drawIpPanel` line 883: `C_GRAY, C_BLACK` → `C_MUTE, C_CREAM`.
- `drawMeter` claude path: drop the per-push `fillScreen(C_BLACK)` (now gated by chrome flag).

## Component changes

- **`drawClaudeIcon`**: enlarge/clean the existing 8-ray star into a proper header burst
  (more spokes, balanced), coral. (Monk burst is grid-cell based — not reusable as a vector.)
- **`drawClaudeBar`**: rewrite as a pill — `drawRoundRect` tan track + `fillRoundRect` coral
  fill (red in danger). Redraw track each push before fill so a shrinking % leaves no
  leftover fill. `pct <= 0` → track only (no-data/zero).
- **Binding accent**: replace amber edge `fillRect` (lines 1130/1156) with a coral `▎` left
  accent (or dot) beside the active block's label.

## Verification

- `arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f firmware/clawdmeter_esp8266`
- Flash (USB or OTA), `GET /mode?screen=claude`, confirm: no flash on 60s pushes, cream
  card renders, big number readable across the room, danger state goes red, no-data shows
  `—` placeholders (test by stopping the daemon / Claude-down path), reset countdown ticks.

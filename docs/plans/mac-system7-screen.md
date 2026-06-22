# Plan: System-7 "About This Macintosh" redesign of the MAC screen

> **Status — IMPLEMENTED (2026-06-22).** Built as designed: firmware-only System-7 window
> with four thermometer rows **CPU / Memory / Disk / Battery** (Battery = the 4th row, as
> planned). Compiles clean at `mmu=4816,ip=hb2f` (IRAM 69%). The two-phase `macChromeReady`
> redraw is documented in `CLAUDE.md`.
>
> **Deviations from the scope below:**
> 1. A short-lived working-tree excursion replaced Battery with a "Top RAM" process row
>    (top-RSS process name + MB, fed by a new daemon `ps` scan). It was reverted — Battery
>    is a stabler, actionable glance signal (real danger threshold ≤20%); Top RAM churned
>    and had no danger semantics. No Top-RAM residue remains in firmware/daemon/dashboard.
> 2. "No daemon changes" held *almost*: one orthogonal robustness fix rode along and was
>    kept — `run_text()` gained a `subprocess` `timeout` and catches `TimeoutExpired`, so a
>    hung child (e.g. `pmset`) can't block the 60 s poll loop. Plus `bat=` added to log lines.
> 3. "No dashboard changes / no gz regen" held — the `mac` view already showed Battery; the
>    final `index_html_gz.h` is byte-identical to HEAD.

**Goal:** Replace the generic dark 2×2 gauge grid (`drawMacMeter`, ~line 1128) with a
classic Mac OS / System 7 *About This Macintosh* window. Optimised for **glance-all-day
readability**, thematically matched to the MacPaint desk screen.

**Scope:** Firmware only — `firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino`.
- Daemon: `cpu/mem/disk/bat` are already pushed via `/usage`; the only daemon change kept
  is the orthogonal `run_text()` timeout hardening (see deviation #2 above).
- No web dashboard changes — the `mac` view in `INDEX_HTML` already renders Battery as its
  4th stat, so **no `gen_index_gz.py` regen** is needed (the header stays identical to HEAD).
- No new ISR/timer code → **no IRAM concern** (the `:mmu=4816` rule is about ISR code only).
- White background does **not** add heat (backlight LED is lit regardless of pixel color).

---

## Decisions made up front (don't re-litigate during implementation)

1. **Redraw model — the critical one.** Today every `/usage` push sets
   `meterRedrawPending` (line 532) → `drawMeter()` → `drawMacMeter()` which does a full
   `fillScreen`. A white `fillScreen` flash every 60 s is jarring and kills the
   glance-all-day feel. Fix: **draw static chrome once on switch-in, repaint only dynamic
   regions on each push.** There is no existing switch-in-only hook for MAC, so add one
   (see "Redraw mechanism" below). This does NOT fall out of the desk-screen pattern for
   free — desk only repaints because of its 120 ms animation tick; MAC has no such tick.

2. **Keep one functional color.** Strict 1-bit would throw away the green→red severity
   signal the current screen has (`barColor` + `invertColor` for battery), which is real
   glance information. Keep the window monochrome (black on white) **except** the bar fill
   turns **red (`C_RED`)** when a metric is in the danger zone: CPU/MEM/DISK ≥ 85 %, or
   BAT ≤ 20 %. Everything else stays solid black. This is the one sanctioned color.

3. **Glanceable > pixel-perfect authenticity.** Use System-7 *styling* (white bg,
   pinstripe title bar, close-box, drop-shadow, 1-bit thermometer bars) but with **big
   sparse numerals**, not the dense small text of the real About box.

4. **No Chicago font in v1.** Keep the default GFX font. A Chicago-ish bitmap font is the
   only thing that would make it *truly* feel like System 7, but it's real added scope
   (font table in flash, metrics work). Leave it as a documented optional follow-up.

---

## Layout (240 × 240)

```
┌────────────────────────────────────────────┐  ← menu bar, white, y 0..14
│  ●  Finder                          3:47 PM │     black apple glyph L, clock R
├════════════════════════════════════════════┤  ← 1px black line at y=14
│ ▤ About This Macintosh              ▤▤▤▤▤▤▤ │  ← title bar: close-box + pinstripes
│                                              │
│   ┌───┐   System Software 7.1               │  ← compact-Mac icon + 2 info lines
│   │[…]│   Clawdmeter  •  up 3d 04h          │
│   └───┘                                      │
│ ─────────────────────────────────────────── │  ← divider
│   CPU      ▐█████░░░░░░░░░░░▌          47%   │  ← 4 thermometer rows
│   Memory   ▐████████░░░░░░░▌          61%   │
│   Disk     ▐███████████░░░░▌          72%   │
│   Battery  ▐█████████████░░▌          88%   │
│                                              │
└────────────────────────────────────────────┘  ← window border + drop shadow
```

Coordinates below are a starting point — tune on hardware.

### Menu bar (static)  y 0..14
- White `fillRect(0,0,240,14, C_WHITE)`, black `drawFastHLine(0,14,240, C_BLACK)`.
- **Apple glyph** at x≈4: draw a small solid-black apple as ~3 stacked filled circles +
  a bite (a white circle) + a leaf line. ~9 px tall. (Simple primitives; reuse the circle
  approach already used in `drawDeskToolGlyph`.) A plain black filled blob reads fine at
  glance distance if the apple is fiddly.
- **Clock** top-right (this is *where the Mac clock lives* — perfect for glance-all-day).
  `HH:MM` (12-hour with AM/PM if it fits; else 24h). Black, size 1. **Dynamic** (see below).

### Window frame (static)  y 16..238
- Outer `drawRect(6,16,228,222, C_BLACK)` with a 1-px **drop shadow**: black
  `drawFastVLine(234,18,221)` + `drawFastHLine(8,238,227)` offset bottom/right.
- **Title bar** inside top, ~16 px: 6 black pinstripe `drawFastHLine`s (reuse the look of
  `drawDeskPattern` case 3 — horizontal lines), a **close box** (small `drawRect` ~9×9 at
  left), and the title `About This Macintosh` centered on a **white knockout gap** so the
  stripes don't run through the text (fill a white rect behind the title first).

### Body (static labels, dynamic values)
- **Compact-Mac icon** (~28×34) drawn with rects: body outline, screen rect, a slot/disk
  line, two feet. Pure 1-bit, draw-once.
- Two info lines next to it: `System Software 7.1` and `Clawdmeter • up <uptime>`
  (reuse `uptimeText()`). The uptime portion is **dynamic** (repaint that line).
- Divider `drawFastHLine`.
- **Four metric rows.** For each row at its baseline `ry`:
  - Label (static): `CPU` / `Memory` / `Disk` / `Battery`, black size 1 or 2, left-aligned.
  - **Thermometer bar** (dynamic): `drawRect(barX, ry, barW, 14, C_BLACK)` outline drawn
    once as chrome; the **interior** is the dynamic region. Repaint = `fillRect` interior
    white, then `fillRect` the filled portion `(barW-4)*pct/100` wide in **black**, or
    **`C_RED`** if in the danger zone (decision #2). Battery uses inverted threshold.
  - **Percentage** (dynamic): right-aligned big numeral, size 2, black, opaque white bg
    (`setTextColor(C_BLACK, C_WHITE)`) so it overwrites cleanly with no clear-flash.

### STALE indicator (dynamic)
Replace the old red "STALE" header text. When `lastUpdateMs != 0 && millis()-lastUpdateMs
> 120000`, draw a small black **`!` in a box** at the right end of the title bar (or flip
the title to `About This Macintosh — stale`). Clear it (repaint white) when fresh. Keep it
1-bit; it's a status not a severity, so no red needed.

---

## Redraw mechanism (implement exactly this)

Add a file-scope flag:

```c
bool macChromeReady = false;   // false → next drawMacMeter draws full chrome
```

1. **On switch into MAC** (mode handler, line 606–610, where `lcdScreen = nextScreen`):
   set `macChromeReady = false;` whenever `nextScreen == SCREEN_MAC`. Also fine to clear it
   anywhere the screen is forced to MAC. This guarantees a full chrome draw on entry.

2. **`drawMacMeter()` becomes two-phase:**
   ```
   void drawMacMeter() {
     if (!macChromeReady) {
       gfx->fillScreen(C_WHITE);
       drawMacChrome();          // menu bar, window, title, icon, labels, bar outlines
       macChromeReady = true;
     }
     drawMacDynamic();           // clock, uptime, 4 bar interiors, 4 numerals, stale flag
   }
   ```
   - On **switch-in**: `macChromeReady` is false → full chrome + dynamic. One white flash,
     unavoidable and acceptable (it's a deliberate screen change).
   - On **`/usage` push** (`meterRedrawPending`, line 2033): `macChromeReady` is already
     true → **no `fillScreen`**, only `drawMacDynamic()` repaints the 4 bars + numbers +
     uptime. No flash. ← this is the whole point.

3. **Per-minute clock tick:** line 2063–2067 currently calls `tickDynamic()` once a minute
   for MAC. Point it at the new menu-bar clock position (or just call `drawMacDynamic()`,
   which is cheap and also refreshes the stale flag). Make sure `tickDynamic()`'s old MAC
   branch (lines 1112–1117, the old top-right clock coords) is updated/removed so it
   doesn't paint stale pixels in the menu bar.

4. **Dynamic repaints must be opaque-bg, no `fillRect` of the whole body** — same
   discipline as `drawDeskStatusText` (line 984): print numerals with `C_WHITE` background,
   fill only each bar's interior. Never `fillScreen` in `drawMacDynamic`.

---

## Functions to add / change

| Function | Action |
|---|---|
| `macChromeReady` (file scope) | **new** flag |
| mode handler (line ~606) | **edit**: `macChromeReady = false` on switch into MAC |
| `drawMacChrome()` | **new**: all static System-7 chrome (menu bar, apple, window, title bar, close box, compact-Mac icon, divider, labels, bar outlines) |
| `drawMacDynamic()` | **new**: clock, uptime, 4 bar interiors (black/red), 4 numerals, stale flag |
| `drawMacMeter()` (line 1128) | **rewrite**: two-phase dispatch above |
| `drawMacUsageCell` (line 924) | **delete** (replaced by the row logic in `drawMacChrome`/`drawMacDynamic`) |
| `tickDynamic()` MAC branch (lines 1112–1117) | **edit**: update clock coords or delegate to `drawMacDynamic()` |
| `drawMacClock` / `drawMacStaleMarker` (lines 894–908) | **edit/fold** into the new dynamic path (new positions) |
| `metricColor`/`barColor` | **reuse** for the danger-zone red decision (or inline a `pct ≥ 85` test) |

Keep using existing helpers: `uptimeText()`, `printRight`, `printCentered`, `pctText`,
`nowEpoch()`, `hhmm()`, hex color literals (`C_WHITE 0xFFFF`, `C_BLACK 0x0000`, `C_RED`).

---

## Verification

1. Compile: `arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f firmware/clawdmeter_esp8266`
2. Flash (USB or OTA per CLAUDE.md).
3. Switch to MAC: `curl "http://clawdmeter.local/mode?screen=mac"` — confirm the window
   draws once.
4. Push data: `curl "http://clawdmeter.local/usage?cpu=47&mem=61&disk=72&bat=88..."` and
   watch for **no white flash** — only bars/numbers should update.
5. Danger color: push `cpu=92` → that bar fills red; `bat=15` → battery bar red.
6. Stale: stop pushing for >120 s → `!`/stale marker appears; resume → clears.
7. Confirm `/usage.json` still reports `mac` and the dashboard `mac` view still works
   (untouched, but verify nothing regressed).

## Optional follow-ups (out of v1 scope)
- Chicago-style bitmap font for true System-7 typography.
- Tiny per-metric icons (chip/RAM/disk/battery) left of each label.
- A subtle blinking caret or 1 Hz colon in the clock for "alive" feedback.

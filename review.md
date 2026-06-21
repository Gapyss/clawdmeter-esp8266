# Claude Review Request: MAC Ring Gauges + DESK LCD Rewrite

Please review the firmware LCD-rendering change in:

- `firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino`
- `CLAUDE.md`

## Goal

Redesign two physical LCD screens on the 240x240 TFT:

- `MAC`: replace horizontal resource bars with four circular ring gauges.
- `DESK`: replace the simple centered status sign with a 30-row ASCII-grid
  status display that has a header, framed status block, live clock/date rows,
  and uptime footer.

## Scope

This intentionally touches only the firmware LCD render path and the local
developer docs for the `/desk` endpoint. No dashboard HTML changed, so
`index_html_gz.h` was not regenerated.

### MAC Screen

- Adds `C_CYAN` for the MAC footer IP address.
- Adds helpers:
  - `drawMacClock()` for white `HH:MM:SS` plus green `TZ_LABEL`.
  - `uptimeText()` using `millis()` as the device uptime source.
  - `drawRingGauge()` using Arduino_GFX `fillArc()`.
- Updates `tickDynamic()` so the MAC clock refresh preserves the green timezone
  label.
- Replaces `drawMacMeter()` with:
  - Claude-orange `MAC` header.
  - Thin separator line.
  - Four ring gauges:
    - CPU = `macCpuPct`
    - MEM = `macMemPct`
    - DISK = `macDiskPct`
    - BAT = `macBatteryPct`, colored with `barColor(100 - macBatteryPct)`
  - Footer separator.
  - Device uptime bottom-left from `millis()`.
  - `WiFi.localIP()` bottom-right in cyan.

### DESK Screen

- Replaces the old solid-header, large centered-text `drawDeskSign()` layout
  with a 40-column by 30-row grid layout using default size-1 GLCD text.
- Adds grid helpers:
  - `fitGridLine()` pads/truncates each row to exactly 40 characters.
  - `centeredGridLine()` centers short strings before fitting them.
  - `drawGridLine()` clears and redraws one 8px-tall text row.
  - `withRightText()` builds left/right-aligned 40-character rows.
- Adds DESK-specific dynamic helpers:
  - `desktopDateLine()` renders a TZ-adjusted full date.
  - `drawDeskDynamicLines()` updates only the live rows: header time, framed
    `now HH:MM:SS TZ_LABEL`, full date, and ASCII uptime footer.
  - `uptimeTextAscii()` formats uptime as `up <days>d <hours>h` because the
    DESK grid is plain ASCII art.
- Updates `tickDynamic()` so `SCREEN_DESK` refreshes those four grid rows once
  per second instead of clearing only the previous centered clock line.
- Preserves the documented `/desk` color contract:
  - Presets still render as `coding` green, `busy` red, `break` amber, and
    `claude` Claude-orange.
  - Custom `/desk?text=...&color=...` colors are honored even if custom text is
    `CODING`, `BUSY`, `BREAK`, or `CLAUDE`.
- Updates `CLAUDE.md` to document the `claude` preset/color and the custom-color
  precedence for `/desk`.

## Design Notes

- The installed `GFX Library for Arduino` exposes:
  - `gfx->drawArc(...)`
  - `gfx->fillArc(...)`
- The ring background uses `fillArc(cx, cy, outerR, innerR, 0, 360, C_LINE)`.
- The filled arc starts at 12 o'clock via `270` degrees and advances clockwise:
  `270 + pct * 3.6`.
- Values below 0 display through existing `pctText()` as `--` and draw no filled
  arc.
- `100%` switches to text size 2 so it does not collide with the ring; shorter
  values use text size 3.
- Uptime uses CP437 character `\x18` for the up-arrow glyph, formatted like
  `↑3d 04h` on the device font.
- DESK uptime intentionally uses ASCII `up 3d 04h` to keep all rows in the
  grid display printable as plain 40-column text.
- DESK grid rows are exactly 40 characters wide. At text size 1, 40 GLCD
  characters occupy the full 240px width without wrapping.
- `drawDeskSign()` leaves dynamic rows blank during the static pass and calls
  `drawDeskDynamicLines()` once at the end, avoiding double-drawing those rows
  during a full repaint.

## Please Check

### MAC

- Whether the `fillArc()` angle behavior is correct on this library/panel:
  start at top, fill clockwise, full ring at 100%.
- Whether the 2x2 geometry is visually balanced:
  centers `(60,78)`, `(180,78)`, `(60,166)`, `(180,166)`, radius `38`.
- Whether `100%`, `--`, and label text fit cleanly on the actual TFT.
- Whether adding `C_CYAN` is acceptable versus using an existing color.
- Whether the MAC `tickDynamic()` clear rectangle fully covers old clock text
  while not touching the `MAC` title.
- Whether `millis()` uptime rollover behavior is acceptable for this display.

### DESK

- Whether all 30 grid rows fit cleanly on the actual TFT with no wrap,
  clipping, or row overlap.
- Whether the dynamic row update path avoids visible flicker on hardware.
- Whether the DESK preset colors still match the documented `/desk` API:
  `coding` green, `busy` red, `break` amber, `claude` Claude-orange.
- Whether custom color precedence is correct for reserved-looking text, e.g.
  `/desk?text=BUSY&color=blue` should render blue because this is custom mode.
- Whether the full date, clock, and uptime lines remain legible with the chosen
  ASCII-art density.

## Verification

Command run:

```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f \
  firmware/clawdmeter_esp8266
```

Compile result: passed.

Memory usage:

```text
Variables and constants in RAM (global, static), used 42608 / 80192 bytes (53%)
Instruction RAM (IRAM_ATTR, ICACHE_RAM_ATTR), used 45291 / 65536 bytes (69%)
Code in flash (default, ICACHE_FLASH_ATTR), used 401672 / 1048576 bytes (38%)
```

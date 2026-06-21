# Screen Layout

240×240 ST7789 TFT. This is the **`api`-mode dashboard**: every field maps to a
real Anthropic rate-limit response header (see the daemon's `extra_from_headers`).
Clock + reset times are shown in **Thailand time (ICT, UTC+7)** — the device clock
is synced from the `Date` response header the daemon pushes (no RTC/NTP), then
ticks once per second off `millis()`. The `TZ_OFFSET`/`TZ_LABEL` `#define`s set the
zone (durations/countdowns stay raw, so they are timezone-independent).

## Normal state (data received)

Below, the example header values are 08:43 / 13:20 / Tue 18:00 **UTC**, shown as
their ICT equivalents (+7h).

```
┌────────────────────────────────────────┐ y=0
│ ✣ CLAUDE  ● ALLOWED       15:43:41 ICT  │  spark icon + title (orange) + dot/text = unified-status + clock
├────────────────────────────────────────┤ y=29   orange separator
│▌SESSION 5h                              │  ▌ amber stripe if this is the binding limit
│▌                                      │
│▌                 25%                    │  large hero percentage = unified-5h-utilization (orange < 60%)
│▌                                      │
│▌ ▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░               │  bar = unified-5h-utilization (Claude orange < 60%)
│ reset 20:20              T-4h37m        │  reset HH:MM ICT + live countdown
│                                        │
│ WEEKLY                            12%   │
│ ▓▓▓░░░░░░░░░░░░░░░░░░░░░░               │  bar = unified-7d-utilization (Claude orange < 60%)
│ reset Wed 01:00                         │  unified-7d-reset (Dow HH:MM ICT)
│                         IP 192.168.1.42 │
└────────────────────────────────────────┘ y=240
```

This screen is Claude-themed: a small Claude spark icon (`drawClaudeIcon`) and
orange `CLAUDE` title at the top, an orange separator, and progress bars
(`drawClaudeBar`) that render in Claude orange until usage hits 60%, then switch
to the yellow/red warning colors. The big hero percentage follows the same rule.

Claude geometry: spark icon centered at `(10,13)`, title `CLAUDE` at `x=24` `y=9`,
status dot at `(72,13)` with its word at `x=82`; separator `y=29`; session label
`y=40`, big percentage `textSize 5` at `y=57`, session bar `16,112` size `208×18`,
reset line `y=138`; weekly label `y=160`, weekly bar `16,184` size `208×12`, reset
line `y=204`; compact IP readout at `y=230`. The clock and the session countdown
are the only per-second redraws (`tickDynamic`); everything else redraws on a
daemon push.

## Field → header map (`api` mode)

| Screen | Header | Notes |
|--------|--------|-------|
| Status dot/word (after the `CLAUDE` title) | `anthropic-ratelimit-unified-status` | `allowed` → green, else red |
| Clock | `Date` response header | pushed as epoch `t`, ticked from `millis()`, shown in ICT |
| SESSION % + bar | `anthropic-ratelimit-unified-5h-utilization` | 0..1 → ×100 |
| Reset HH:MM + countdown | `anthropic-ratelimit-unified-5h-reset` | unix epoch |
| WEEKLY % + bar | `anthropic-ratelimit-unified-7d-utilization` | |
| Reset Dow HH:MM | `anthropic-ratelimit-unified-7d-reset` | |
| Amber stripe | `anthropic-ratelimit-unified-representative-claim` | `five_hour`→session, `seven_day`→weekly |

In `local` mode the daemon sends only `s/w/st/wt`, so the reset lines show
`reset --:--`, the status band shows just the `CLAUDE` title (no status
dot/word), and there is no clock/countdown.

## Waiting state (no data yet)

Shown when both `sessionPct` and `weeklyPct` are still −1. The device has no
daemon time yet, so the clock comes from **NTP** (`configTime`, synced once WiFi
is up) — `nowEpoch()` falls back to it until the first push. The big clock/date
refresh **once a minute** (`drawWaitingTime` from `loop`); both are in ICT.

```
┌────────────────────────────────────────┐
│                                        │
│                14:06                   │  textSize 4, NTP clock (ICT)
│             sat 20 jun                 │  textSize 2, gray (civil_from_days)
│         ──────────────────             │
│           waiting for data             │
│                 G4PYS                  │
│                         IP 192.168.1.42│  same compact IP readout
└────────────────────────────────────────┘
```

## Mac monitor screen

```
┌────────────────────────────────────────┐
│ MAC                         15:43:41 ICT│
├────────────────────────────────────────┤
│ CPU                              42%    │
│        ▓▓▓▓▓▓░░░░░░░░░░                 │
│ MEM                              68%    │
│        ▓▓▓▓▓▓▓▓▓▓░░░░░                  │
│ DISK                             55%    │
│        ▓▓▓▓▓▓▓▓░░░░░░                   │
│        ────────────────────            │
│ BATTERY                         91%     │
│                         IP 192.168.1.42 │
└────────────────────────────────────────┘
```

CPU, memory, and disk use compact rows. Battery is separated as a footer because
it changes more slowly and should be scannable at a glance.

## Desk status screen

```
┌████████████████████████████████████████┐  colored header = desk color
│ DESK                                   │
│                                        │
│                CODING                  │  large centered status text
│             ─────────────              │
│             15:43:41 ICT               │
│                 G4PYS                  │
│                         IP 192.168.1.42│
└────────────────────────────────────────┘
```

The dashboard can set preset states (`coding`, `claude`, `busy`, `break`) or
custom text and color. The physical screen intentionally omits setup/help copy so
it reads as a status sign from desk distance.

Claude desk preset (`/desk?status=claude`) is static: Claude orange header,
large `CLAUDE` text, clock, G4PYS footer, and IP.

```
┌████████████████████████████████████████┐  Claude orange header
│ DESK                                   │
│                                        │
│                CLAUDE                  │
│             ─────────────              │
│             15:43:41 ICT               │
│                 G4PYS                  │
│                         IP 192.168.1.42│
└────────────────────────────────────────┘
```

## Bar / status colors

| Usage  | Color  | RGB565   |
|--------|--------|----------|
| < 60%  | Green  | `0x07E0` |
| 60–89% | Yellow | `0xFFE0` |
| ≥ 90%  | Red    | `0xF800` |

| Status     | Dot/text | RGB565   |
|------------|----------|----------|
| `allowed`  | Green    | `0x07E0` |
| other/deny | Red      | `0xF800` |
| (none)     | Gray     | `0x8410` |

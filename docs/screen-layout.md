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
│ CLAUDE USAGE  ● ALLOWED   15:43:41 ICT  │  title + dot/text color = unified-status + clock
├────────────────────────────────────────┤ y=27   separator
│▌SESSION 5h                        25%   │  ▌ amber stripe if this is the binding limit
│▌▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░             │  bar = unified-5h-utilization
│▌Reset 20:20              T-4h37m        │  unified-5h-reset (HH:MM ICT) + live countdown
├────────────────────────────────────────┤ y=113  separator
│ WEEKLY 7d                         12%   │
│ ▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░             │  bar = unified-7d-utilization
│ Reset Wed 01:00                         │  unified-7d-reset (Dow HH:MM ICT)
├════════════════════════════════════════┤ y=198/200  double rule
│ IP  192.168.1.42                        │  dedicated panel, white, prominent
└────────────────────────────────────────┘ y=240
```

Block geometry (`drawBlock`, top edge `y`): label/pct at `y+6` (textSize 2),
bar `12,y+30` size `216×18`, reset line `12,y+54` (textSize 1). Session block
`y=28`, weekly block `y=114`. The clock and the session countdown are the only
per-second redraws (`tickDynamic`); everything else redraws on a daemon push.

## Field → header map (`api` mode)

| Screen | Header | Notes |
|--------|--------|-------|
| Status dot/word (after the `CLAUDE USAGE` title) | `anthropic-ratelimit-unified-status` | `allowed` → green, else red |
| Clock | `Date` response header | pushed as epoch `t`, ticked from `millis()`, shown in ICT |
| SESSION % + bar | `anthropic-ratelimit-unified-5h-utilization` | 0..1 → ×100 |
| Reset HH:MM + countdown | `anthropic-ratelimit-unified-5h-reset` | unix epoch |
| WEEKLY % + bar | `anthropic-ratelimit-unified-7d-utilization` | |
| Reset Dow HH:MM | `anthropic-ratelimit-unified-7d-reset` | |
| Amber stripe | `anthropic-ratelimit-unified-representative-claim` | `five_hour`→session, `seven_day`→weekly |

In `local` mode the daemon sends only `s/w/st/wt`, so the reset lines show
`Reset --:--`, the status band shows just the `CLAUDE USAGE` title (no status
dot/word), and there is no clock/countdown.

## Waiting state (no data yet)

Shown when both `sessionPct` and `weeklyPct` are still −1. The status band and
IP panel still render; the middle shows `waiting for / daemon...`.

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

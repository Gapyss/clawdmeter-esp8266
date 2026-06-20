# Screen Layout

240×240 ST7789 TFT display. Each row ≈ 8 px, each column ≈ 6 px.

## Normal state (data received from daemon)

```
┌──────────────────────────────────────────┐  y=0
│                                          │
│  Clawdmeter                 ← orange     │  y=8   textSize 2
│                                          │
│  local Claude Code logs     ← gray       │  y=30  textSize 1
│                                          │
│  ─────────────────────────────────────   │  ── Session block ──
│  Session 5h            72%  ← white      │  y=52  textSize 2
│  21.6K tok              ← gray           │  y=74  textSize 1
│                                          │
│  ┌──────────────────────────────────┐    │  y=88  bar outline (216×24)
│  │████████████████████████░░░░░░░░░│    │        fill = green/yellow/red
│  └──────────────────────────────────┘    │  y=112
│                                          │
│  ─────────────────────────────────────   │  ── Weekly block ──
│  Weekly 7d              38%  ← white     │  y=132 textSize 2
│  38.0K tok              ← gray           │  y=154 textSize 1
│                                          │
│  ┌──────────────────────────────────┐    │  y=168 bar outline (216×24)
│  │███████████████░░░░░░░░░░░░░░░░░░│    │        fill = green (< 60 %)
│  └──────────────────────────────────┘    │  y=192
│                                          │
│                                          │
│  192.168.1.42           ← gray           │  y=226 textSize 1
└──────────────────────────────────────────┘  y=240
```

## Waiting state (no data yet)

Shown when both `sessionPct` and `weeklyPct` are still -1.

```
┌──────────────────────────────────────────┐
│  Clawdmeter            ← orange          │
│  local Claude Code logs ← gray           │
│                                          │
│       waiting for      ← white           │
│       daemon...        ← white           │
│                                          │
│  192.168.1.42          ← gray            │
└──────────────────────────────────────────┘
```

## Bar fill color (from `barColor()`)

| Usage  | Color  | RGB565   |
|--------|--------|----------|
| < 60%  | Green  | `0x07E0` |
| 60–89% | Yellow | `0xFFE0` |
| ≥ 90%  | Red    | `0xF800` |

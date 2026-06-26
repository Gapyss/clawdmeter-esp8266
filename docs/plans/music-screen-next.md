# Plan: Music Screen — Bug Fixes + iOS Redesign

Two sequential PRs. Land PR 1 first; PR 2 builds on a working progress bar.

---

## PR 1 — Bug Fixes (branch: `fix/music-screen-bugs`)

### Bug A: wrong start position (shows full-song elapsed at track open)

**Root cause:** When a new song is pushed via `/nowplaying`, the firmware sets
`npPos` to whatever value the daemon sent. On a new-track identity change
(`identityChanged = true`), the daemon's `pos` from JS may reflect the previous
song's `currentTime` (video element hasn't reset yet), so the firmware starts
counting up from mid-track or the previous song's end.

**Fix — firmware only** (`clawdmeter_esp8266.ino`, around line 685):
```cpp
bool identityChanged = oldTitle != npTitle || oldArtist != npArtist || oldDur != npDur;
// ADD: on identity change, reset position to zero regardless of what daemon sent.
if (identityChanged) {
  npPos = 0;
  npPosBaseMs = millis();
}
```
Place this block immediately after `identityChanged` is computed, before the
`drawMusic()` call. This means the device always starts a new song at 0:00 and
counts forward; the 30 s resync will correct it if the user joins mid-song.

---

### Bug B: "-0:0X" display at end of track

**Root cause:** `musicDrawTime()` (line 2383) always prepends `"-"` to the
remaining time string, even when remaining = 0. `musicClock(0)` returns `"0:00"`,
producing `"-0:00"`.

**Fix — firmware** (`musicDrawTime()`, line 2383):
```cpp
// BEFORE:
printRight(234, 224, 1, "-" + musicClock(npDur - pos), tc, C_MUSIC_BG);

// AFTER:
int rem = npDur - pos;
printRight(234, 224, 1, rem > 0 ? "-" + musicClock(rem) : "0:00", tc, C_MUSIC_BG);
```

---

### Bug C (optional, same PR): 30 s resync lag feels long

**Fix — daemon** (`claudemeter_daemon.py`, line 31):
```python
NOWPLAYING_RESYNC = 10   # was 30; shorter keeps pos accurate through scrubs
```
Low risk — the push is tiny (one HTTP query string). The 10 s window means at
worst 10 s of drift after a user scrubs or pauses.

---

### Test checklist (PR 1)
- [ ] Play a 3-min song from the start → elapsed begins at 0:00
- [ ] Let it run to the end → remaining shows `0:00`, not `-0:00`
- [ ] Skip to a new song mid-playback → device resets to 0:00 immediately
- [ ] Pause mid-song → elapsed freezes, remaining holds

---

## PR 2 — iOS Music Card Redesign (branch: `feat/music-ios-card`)

Depends on PR 1 being merged first.

### Visual target: iOS Music app "Now Playing" card

240×240 layout (all Y values are pixel rows from top, 0-indexed):

```
0   ┌──────────────────────────────┐
    │  HH:MM               ♩  ⋯  │  status bar (12 px tall)
12  ├──────────────────────────────┤
    │                              │
    │      ╭──────────────────╮    │
    │      │                  │    │  album art: 120×120 rounded rect
    │      │    EQ bars here  │    │  centered horizontally: x=60, y=20
    │      │                  │    │  rounded corner r=10
    │      ╰──────────────────╯    │  simulated shadow: darker +2px rect behind
    │                              │
148 │  Song Title (bold, marquee)  │  title band Y=148..188 (40px)
    │                              │
188 │  Artist Name  (grey, marq.)  │  artist band Y=188..210 (22px)
    │                              │
210 │  ────────────●───────────   │  progress bar Y=210 (3px), dot playhead
    │  1:02                -3:41  │  time row Y=216
    │                              │
228 │       ◁      ▐▐      ▷      │  transport row (decorative, no input)
240 └──────────────────────────────┘
```

### Color changes

Replace these `#define` constants near line 818:

| Constant | Old value | New value | Note |
|---|---|---|---|
| `C_MUSIC_HDR` | `0x3AF1` (iPod blue-grey) | `0xF800` (iOS red `#FF0000` approx) | progress fill + accent |
| `C_MUSIC_ART` | `0xAD75` (grey) | `0xD6BA` (lighter grey `#D0D0D0`) | art placeholder bg |
| `C_MUSIC_FILL` | `0x8410` (grey) | `0xF800` | same red as HDR — iOS Music accent |
| `C_MUSIC_LINE` | `0xCE59` | `0xE71C` (light grey `#E0E0E0`) | progress track |

Keep `C_MUSIC_BG`, `C_MUSIC_FG`, `C_MUSIC_DIM` unchanged.

Add new constant:
```cpp
#define C_MUSIC_SHADOW 0xC618  // #C0C0C0 — shadow simulation for art card
```

### Layout constant changes

Near line 2061–2082:

```cpp
// Art card — enlarged and recentered
#define MUSIC_ART_X   60   // (240-120)/2
#define MUSIC_ART_Y   20   // below status bar + small gap
#define MUSIC_ART_W   120
#define MUSIC_ART_H   120
#define MUSIC_ART_R   10   // corner radius for fillRoundRect

// Title / artist bands — shifted down to make room for larger art
#define MUSIC_TITLE_BAND_Y   148
#define MUSIC_TITLE_BAND_H   40
#define MUSIC_ARTIST_BAND_Y  188
#define MUSIC_ARTIST_BAND_H  22

// Footer row positions
#define MUSIC_PROGRESS_Y     211   // 3px bar
#define MUSIC_TIME_Y         218
#define MUSIC_TRANSPORT_Y    230
```

### Chrome changes (`drawMusic()` / static chrome function)

The static chrome (drawn once via `musicChromeReady`) must be updated:

1. **Status bar** (rows 0–11): fill `C_MUSIC_BG`, draw current time
   `hhmmss(nowEpoch() + TZ_OFFSET)` truncated to HH:MM on the left in
   `C_MUSIC_DIM`. Right side: draw a static music note glyph `\266b` (♩) and
   ellipsis — decorative only.

2. **Album art card with shadow**: draw the shadow rect first, then the art:
   ```cpp
   // shadow: +2px offset, same size, darker color
   gfx->fillRoundRect(MUSIC_ART_X + 2, MUSIC_ART_Y + 2,
                      MUSIC_ART_W, MUSIC_ART_H, MUSIC_ART_R, C_MUSIC_SHADOW);
   // art placeholder
   gfx->fillRoundRect(MUSIC_ART_X, MUSIC_ART_Y,
                      MUSIC_ART_W, MUSIC_ART_H, MUSIC_ART_R, C_MUSIC_ART);
   ```
   EQ bars remain inside the art square — recalculate `MUSIC_EQ_*` offsets
   relative to new `MUSIC_ART_X/Y/W/H`.

3. **Transport row** (row ~228–238): draw `◁  ▐▐  ▷` centered in `C_MUSIC_DIM`.
   These are static chrome — no interaction. Draw once with the rest of chrome.

### Dynamic repaint changes

`musicDrawTitle()` and `musicDrawArtist()` — no logic change, only Y constants
(already parameterized via `MUSIC_TITLE_BAND_Y` etc., so constant changes are
sufficient).

`musicDrawFooter()` — update `fillRect` clear area to use new
`MUSIC_PROGRESS_Y`, `MUSIC_TIME_Y` constants.

`musicDrawTime()` — update cursor Y to `MUSIC_TIME_Y`.

`musicDrawProgress()` — update `y` to `MUSIC_PROGRESS_Y`. Change dot radius from
4 → 5 to feel more iOS. Change fill color from `C_MUSIC_FILL` (already updated
to red) — no code change needed if constant is updated.

### What to keep unchanged

- `musicDrawText()` / Thai UTF-8 glyph blit — do not touch
- Marquee scroll logic — do not touch
- EQ bar animation math — only update x/y offsets for new art position
- `musicStopped()` / `musicPausedKnown()` / `musicProgressReliable()` — no change
- `C_MUSIC_DIM` pause dimming logic — no change

### Test checklist (PR 2)

- [ ] Static chrome matches layout sketch above (art card centered, shadow visible)
- [ ] EQ bars animate inside the new art square
- [ ] Title marquee scrolls a long Thai/Latin title correctly
- [ ] Artist marquee scrolls a long artist name correctly
- [ ] Progress bar red accent fills left-to-right, dot tracks correctly
- [ ] Time row shows `0:00` → `4:33` elapsed and `-4:33` → `0:00` remaining
- [ ] Transport row glyphs visible and centered
- [ ] Status bar shows current HH:MM
- [ ] Paused state: accent dims to `C_MUSIC_DIM` throughout
- [ ] Track change: chrome stays, title/artist repaint without flash
- [ ] `gzip` page still under `MAX_GZ_BYTES` after any dashboard label changes

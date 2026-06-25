# MUSIC screen (YouTube Music now-playing) — IMPLEMENTED ✓

> **Status:** fully implemented and shipped. Daemon + firmware changes are in `main`.
> This doc is retained as a reference/post-mortem; no further work needed.

---

# Implementation Handoff: MUSIC screen (YouTube Music now-playing)

**Audience:** an implementing agent starting cold. Everything you need is in this doc; read
`CLAUDE.md` for the device-wide gotchas it references. **Two parts do the work:** the macOS
daemon `daemon/claudemeter_daemon.py` (read the song, push it) and the firmware
`firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino` (new screen + Thai font). Line numbers are
anchors from the current `main`; re-grep if they have drifted.

> **Scope note up front:** this is NOT a daemon-only afternoon. The user listens mostly to **Thai**
> songs, so the firmware must render Thai (U+0E00–U+0E7F) — that means bundling a bitmap font and a
> UTF-8 decoder, on top of the new screen and the daemon changes. Plan for a two-part build.

---

## 1. What you are building

A 6th physical LCD mode, `music`, joining `claude | mac | desk | face | timer`. Its job: show the
**song currently loaded in a YouTube Music browser tab**, glanceably, as a Mac-style
"SoundJam/iTunes" window — big scrolling title, artist underneath. The Mac daemon reads the song
from Chrome and pushes it; the device renders it and also exposes it in `/usage.json`.

### Target layout (240×240) — System-7 / MacPaint aesthetic

```
┌────────────────────────────┐
│ ♪  Now Playing         ─ □ │  pinstripe title bar (drawn once, like mac/desk chrome)
├────────────────────────────┤
│                            │
│  « Everybody Wants To Ru » │  HERO — title MARQUEE, scrolls right→left (millis poll)
│                            │
│         Tears for Fears    │  artist — static, auto-shrink (size 3→2) to fit 240px
│                            │
│        ▶  ████░░░░░         │  (optional) static "playing" glyph; NO real progress bar
└────────────────────────────┘
```

Idle (no YT Music tab / Chrome closed): title shows **`— Not Playing —`**, artist blank.

---

## 2. Decisions already made (do not relitigate)

| # | Decision | Why |
|---|----------|-----|
| Source | **AppleScript reading Chrome tab titles**, scan ALL tabs/windows for one ending `- YouTube Music` | matches "the page I see"; no MediaRemote (locked down on macOS 26); no extension |
| Browser | **Google Chrome** only (Chromium scripting) | user's browser |
| Launch guard | check `System Events` that Chrome is **already running** before scripting it | never let AppleScript spawn Chrome from a background daemon |
| Play/pause | **ignored** for v1 (tab title = loaded song) | title carries no transport state without JS injection |
| Endpoint | **new `/nowplaying?title=&artist=`**, runtime-only state | keeps "query args, no JSON" rule; separate from `/desk?text=` (12-char cap too small) |
| Encoding | daemon URL-encodes, sends **UTF-8** (Thai preserved, NOT stripped to ASCII) | user's library is mostly Thai |
| Screen | **standalone `music` mode**, manually selected via `/mode?screen=music` | respects per-mode chrome/dynamic architecture; no auto-switch off usage views |
| Title overflow | **marquee via `millis()` poll in `loop()`** (the `face` pattern) | classic now-playing; NO timer ISR (PWM ISR already starves WiFi; IRAM tight) |
| Artist | static, **auto-shrink** size 3→2 (like `deskDisplayText`) | fits 240px without animation |
| Thai font | **bundle Thai bitmap font** (U+0E00–U+0E7F, ~5 KB in **flash/PROGMEM**) + UTF-8 decode | flash has room; the memory wall is IRAM/ISR, which font tables don't touch |
| Thai marquee | **attempt marquee for Thai too**; static-wrapped Thai is the known fallback (option C) if scrolling stacked tone-marks looks wrong | user chose to try it; easy to switch later |
| Cadence | daemon loop ticks **~8s**; song every tick, **usage poll every ~8th tick (60s)** | songs change ~3 min; 60s lag too coarse for now-playing |
| Push | only POST `/nowplaying` when the title **changed** (change-detection) | keep the device quiet between songs |
| Idle | no YT Music tab → show **`— Not Playing —`** | |
| `/usage.json` | **echo `title`/`artist`** | trivial, good for `curl` debugging |
| Dashboard HTML | **no** now-playing widget for v1 | stay under `MAX_GZ_BYTES` (5120 B) gzip build guard |
| TCC | run daemon **by hand once** to approve the "control Chrome" Automation prompt | launchd can't answer GUI dialogs (same as the Keychain step) |

---

## 3. Daemon work (`daemon/claudemeter_daemon.py`)

1. **Read the song (AppleScript, stdlib `subprocess`).**
   - First check Chrome is running, e.g. via System Events, to avoid launching it:
     ```applescript
     tell application "System Events" to (name of processes) contains "Google Chrome"
     ```
   - If running, enumerate tabs and return the first title ending in `- YouTube Music`:
     ```applescript
     tell application "Google Chrome"
       repeat with w in windows
         repeat with t in tabs of w
           set ti to title of t
           if ti ends with "- YouTube Music" then return ti
         end repeat
       end repeat
     end tell
     return ""
     ```
   - Run with `subprocess.run(["osascript", "-e", script], ...)`, short timeout, swallow errors → treat as "Not Playing".
2. **Parse defensively.** Strip trailing `- YouTube Music`, then `rsplit(" - ", 1)` the remainder into
   `(title, artist)`; if no ` - `, artist is empty. Trim whitespace. (YT Music's exact title format
   varies — do not hardcode a strict pattern.)
3. **Cadence.** Restructure the main loop to tick every **~8s** with a counter; fire the existing usage
   poll every ~8th tick (preserve the current 60s usage behavior). Each tick reads the song.
4. **Change-detection.** Keep `last_title`/`last_artist`; only POST `/nowplaying` when changed (or on
   first run / transition to "Not Playing").
5. **Push.** `POST {DEVICE_URL}/nowplaying?title=<q>&artist=<q>` with `urllib.parse.quote` on each
   field (UTF-8 — do **not** ASCII-strip). Reuse the existing device-push retry/timeout helpers.

> Keep it stdlib-only (no pip) — the no-dependencies rule still holds. Do NOT add `pythainlp`; Thai is
> handled on the device, not transliterated in the daemon.

---

## 4. Firmware work (`firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino`)

1. **State.** Add file-scope `String npTitle`, `String npArtist`, a `musicChromeReady` flag (cleared
   when the screen is forced to MUSIC, mirroring `macChromeReady`), and marquee scroll-offset +
   last-frame `millis()` vars.
2. **Endpoint.** Register `/nowplaying` (GET+POST). Read `title`/`artist` args (the framework
   URL-decodes), store into `npTitle`/`npArtist`, reset the marquee offset, switch LCD to MUSIC mode
   if not already. Mirror the `/desk` handler.
3. **Mode plumbing.** Add `music` to `/mode?screen=` parsing and the mode enum/dispatch in `loop()`.
   Report `music` in the mode field of `/usage.json`, and add `title`/`artist` fields there.
4. **Render — two-phase (same discipline as `mac`/`desk`).**
   - `drawMusicChrome()`: pinstripe title bar + window frame, drawn **once** on switch-in, gated by
     `musicChromeReady`. **No `fillScreen` on a push.**
   - `drawMusicDynamic()`: repaint only the title (marquee) + artist region with **opaque white-bg
     prints** (no per-frame clear/`fillRect` of the whole canvas).
   - **Marquee:** advance scroll offset from measured `millis()` delta in `loop()` (the `face`-screen
     pattern), NOT a timer ISR. If the title fits, draw it static/centered; only scroll when wider
     than the canvas.
   - **Artist:** auto-shrink 3→2 like `deskDisplayText` to fit 240px.
5. **Thai font + UTF-8 (the real lift).**
   - Bundle a Thai GFXfont covering U+0E00–U+0E7F in a new PROGMEM header (e.g. `thai_font.h`).
     `Adafruit fontconvert` can rasterize a Thai TTF; ~87 glyphs, ~5 KB in flash.
   - Add a UTF-8 decoder: Thai code points are **3-byte** sequences (`0xE0 0xB8/0xB9 ...`). Map each
     decoded code point to either the ASCII font or the Thai font glyph.
   - **Combining marks:** Thai tone marks + upper/lower vowels are zero-width — Arduino_GFX advances
     the cursor per glyph, so naive rendering puts marks in their own cell (broken). Add cursor logic
     that draws combining code points over the **previous** base consonant without advancing X.
     "Legible but placement may be slightly imperfect" is acceptable for v1.
   - **Known fallback:** if the Thai marquee (scrolling stacked marks) looks wrong in practice, switch
     Thai titles to **static wrapped** text (option C) while keeping the marquee for Latin titles.
6. **Memory.** Font data is PROGMEM/flash — fine. Do **not** add ISR/timer code. Build with
   `:mmu=4816,ip=hb2f` and confirm IRAM headroom is unchanged. Keep the gzipped dashboard under
   `MAX_GZ_BYTES` (we are NOT adding a now-playing widget to the HTML).

---

## 5. Acceptance checks

- `osascript` one-liner returns the current YT Music tab title when Chrome is open on a song, `""`
  when closed — and does **not** launch Chrome when it is closed.
- Daemon ticks ~8s, pushes `/nowplaying` only on track change, keeps usage on 60s.
- `curl 'http://clawdmeter.local/nowplaying?title=Test&artist=Me'` switches the panel to MUSIC and
  shows the song; switching tabs to a different song updates it within ~8s.
- A **Latin** title longer than the canvas scrolls smoothly (millis marquee, no reboot/WiFi stall
  under load — the PWM-ISR failure mode).
- A **Thai** title renders as readable Thai (not boxes), tone marks roughly stacked.
- `/usage.json` includes `title`/`artist`; the HTML dashboard is unchanged and still builds under the
  gzip guard.
- No `fillScreen` in the per-tick MUSIC path (no white flash every update).

---

## 6. Open risk to watch

Scrolling Thai with stacked combining marks is the single most likely thing to look wrong. If it
does, the documented fallback is static-wrapped Thai (option C) — keep that switch cheap.

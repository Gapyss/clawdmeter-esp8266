# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A two-part "Claude usage meter" for a **GeekMagic HelloCubic Lite / SmallTV-Ultra**
(ESP8266 + ST7789 240×240 TFT). A Python daemon on a Mac reads the user's Claude
Code usage and pushes it to the device, which shows it on the TFT *and* serves a
web dashboard. Adapted from `HermannBjorgvin/Clawdmeter` (which targets ESP32 +
BLE); this repo replaces BLE with WiFi/HTTP for the ESP8266. The display driver
mirrors `Times-Z/GeekMagic-Open-Firmware`.

```
Mac daemon ──poll 60s──> api.anthropic.com   (reads usage from rate-limit HEADERS)
   └── HTTP POST /usage?... ──> ESP8266 ──> ST7789 panel  +  web dashboard
```

The OAuth token never leaves the Mac; the device only receives usage numbers,
reset metadata, and simple Mac system metrics.

## Architecture / non-obvious facts

- **Two usage sources, controlled by `CLAWDMETER_USAGE_SOURCE`.** Default is `api`:
  makes a real 1-token `POST /v1/messages` every 60s to read
  `anthropic-ratelimit-unified-5h-utilization` and `-7d-utilization` response
  headers (0..1 fraction → ×100). This reflects actual server-side quotas and costs
  a negligible-but-nonzero amount per poll. Set to `local` to instead scan Claude
  Code's JSONL transcripts in `~/.claude/projects` and
  `~/Library/Developer/Xcode/CodingAssistant/ClaudeAgentConfig/projects` —
  no API calls, but percentages are relative to the configurable `*_TOKEN_LIMIT`
  constants rather than real server limits. The local scanner deduplicates streaming
  records by `message.id`, keeping only the final token tally per message.
- **Auth is the Claude Code OAuth token**, read from the macOS Keychain item
  `Claude Code-credentials` via `security find-generic-password`. Requests use
  `Authorization: Bearer <token>` **plus** `anthropic-beta: oauth-2025-04-20`
  (OAuth tokens are not `x-api-key`). On 401 the token expired — running any
  `claude` command refreshes it. There is no token-refresh logic by design.
  Claude/API failures are not allowed to block Mac metrics: the daemon falls back
  to a best-effort `/usage` push with unavailable usage fields plus live
  CPU/memory/disk/battery values, and tags `stat` with a `claude_*` reason.
- **Device transport is HTTP query args, not JSON.** The daemon pushes
  `POST /usage?s=<int>&w=<int>...` so the firmware needs no JSON parser. Keep it
  this way unless you add ArduinoJson for a reason.
- **Display init mirrors the GeekMagic open firmware and the exact values matter.**
  In `clawdmeter_esp8266.ino`: backlight on GPIO5 is **ACTIVE-LOW** (`LOW` = on),
  panel is initialized at **SPI mode 3, 40 MHz**, pins DC=GPIO0 / RST=GPIO2 /
  CS→GND / MOSI=GPIO13 / SCK=GPIO14. Getting backlight polarity or SPI mode wrong
  = blank screen. `#define LCD_ROT` is the per-product orientation knob (HelloCubic
  Lite vs SmallTV-Ultra differ only here; try 0/2/4/6).
- **Backlight is PWM-dimmed, not just on/off.** `setBacklight(uint8_t)` drives GPIO5
  with software PWM at **`LCD_PWM_FREQ` (1 kHz)**; because the pin is active-low it
  inverts the duty (`analogWrite(LCD_BL, 255 - brightness)`), so brightness 255 = full
  on, 0 = off. `#define LCD_BRIGHTNESS` (default 90) is the first-boot/default level;
  after that `/brightness?value=0..255` and the dashboard slider can change it at
  runtime. The value is persisted in EEPROM with a small marker. Full brightness (255)
  ran the panel hot, since the LED string behind the glass is the main heat source.
  The PWM is a software waveform on an IRAM timer ISR (~2 edges/period), so its CPU
  cost scales with frequency. **Keep `LCD_PWM_FREQ` low.** It was 20 kHz, whose
  ~40k interrupts/sec starved the WiFi/TCP stack — the device answered ping but
  crash-rebooted under any HTTP load (dashboard, OTA). 1 kHz is 1/20th the rate, still
  well above flicker fusion. Build with `:mmu=4816` to keep comfortable IRAM headroom.
- **Silence the PWM ISR around every SPI-flash write.** A PWM timer interrupt firing
  during a flash erase/write resets the ESP8266 — OTA died at a deterministic ~127 KB
  offset until fixed. `backlightStopForFlash()` (`analogWrite(LCD_BL, 0)` → detaches
  the pin from the waveform generator, backlight steady full-on) is called before
  `Update.begin` in the OTA upload handler and around `EEPROM.commit()` in
  `saveBrightness`. If you add any new flash write, wrap it the same way. Recovery
  trick for a board *already* stuck on the old 20 kHz firmware: `GET /brightness?value=255`
  stops the waveform, then the OTA upload completes.
- **WiFi uses modem-sleep + a `delay(2)` in `loop()`.** `WiFi.setSleepMode(WIFI_MODEM_SLEEP)`
  lets the radio idle between AP beacons, but it only engages because `loop()` now
  yields via `delay(2)` — without that yield the non-blocking `handleClient()` spins
  the core flat out and the radio never sleeps (hotter, more current). Don't remove
  the `delay()`; it's load-bearing for thermal/power, not a throttle.
- **The dashboard `/` page is served gzipped, and that's load-bearing for
  reliability — not just bandwidth.** The page is sent in one blocking
  `server.send_P`. With the default `ip=lm2f` (Lower Memory) lwIP the TCP send
  buffer is only `2×MSS` (1072 B); under `WIFI_MODEM_SLEEP` ACKs return slowly, the
  buffer can't drain, the write stalls and closes the connection mid-page — the raw
  15 KB page truncated at an MSS multiple ~90% of the time and Chrome showed a failed
  load (looked unreachable even though ping/`usage.json` were fine; `/usage.json` and
  the other small endpoints fit one segment and were never affected). The shipped fix
  (`2742d66`, see `docs/postmortems/dashboard-truncated-send.md`) is three layers:
  (1) build with `ip=hb2f` (Higher-Bandwidth lwIP, bigger send buffer/window);
  (2) `handleRoot` serves a **gzipped** blob (`INDEX_HTML_GZ` in `index_html_gz.h`,
  ~15469→4424 B, `Content-Encoding: gzip`) so the whole body fits one send-buffer
  fill and never stalls mid-write; (3) `handleRoot` flips `WIFI_NONE_SLEEP` for the
  send and `server.client().flush()`es before `Connection: close` to win the
  close-race on the final segment. **`INDEX_HTML` (the raw string) stays the editable
  source; after editing it run `python3 firmware/tools/gen_index_gz.py` to regenerate
  the header.** That generator is also a **guard**: it fails the build if the gzipped
  page exceeds `MAX_GZ_BYTES` (5120 B, safely under the ~5840 B `hb2f` one-buffer
  ceiling). If you bust it, trim the dashboard HTML — don't raise the cap, that
  reintroduces the multi-buffer stall.
- **Arduino_GFX draws directly to the panel (no canvas/framebuffer)** — a 240×240×2
  buffer (115 KB) would not fit ESP8266 RAM. Use the ESP8266 `16KB cache + 48KB IRAM`
  MMU layout (`:mmu=4816` in the FQBN, or Tools → MMU in Arduino IDE). The balanced
  default layout is tight at ~94% instruction RAM; `:mmu=4816` builds at ~69%
  instruction RAM with the same code. Adding `ICACHE_RAM_ATTR`/`IRAM_ATTR` code can
  still overflow `iram1`, so avoid new ISR/timer-heavy features.
- **The MAC screen repaints in two phases — never `fillScreen` on a `/usage` push.**
  The MAC view is a **Tend "paper" system card** (warm cream `C_TND_PAPER`, the shared
  Tend palette aliased from the music screen's `C_MUS_*` tokens): an ember hearth-flame
  mark + `YOUR MAC` eyebrow + wall clock in the header, a `clawdmeter` title with an
  uptime caption, then four calm metric rows — cpu / memory / disk / battery, each a
  lowercase label, a numeral, and a slim rounded Tend progress bar. Per Tend, **ember is
  the one loud color, reserved for the danger state**: a healthy bar fills quiet moss
  (`C_TND_MOSS`) and only turns ember (`C_TND_EMBER`, numeral too) in the danger zone —
  CPU/MEM/DISK ≥ 85 % or Battery ≤ 20 %, via `macMetricDanger`. A full `fillScreen` every
  60 s flashes the panel and kills the glance-all-day feel, so `drawMacMeter()` is split:
  the static chrome (`drawMacChrome`, which paints the paper background itself) is drawn
  **once** on switch-in, gated by the file-scope `macChromeReady` flag (cleared to `false`
  whenever the screen is forced to MAC); every `/usage` push and the per-minute tick then
  run only `drawMacDynamic()`, which repaints the clock, uptime, and four numerals + bar
  fills with **opaque paper-bg prints** (no clear-flash). When data goes stale (>2 min
  with no push) a faint lowercase `stale` word appears by the uptime — Tend **bans
  exclamation marks**, so there is no `!` badge. Same discipline as the desk card: don't
  reintroduce a `fillScreen`/full-body `fillRect` into the dynamic path.
- **Use hex color literals (`0x0000`/`0xFFFF`), not Arduino_GFX `BLACK`/`WHITE`.**
  The named macros fail to resolve inside the non-capturing lambda used for
  `wm.setAPCallback`.
- **WiFi is configured at runtime, not compiled in.** WiFiManager opens a
  `Clawdmeter-setup` hotspot when it can't connect (captive portal). mDNS exposes
  the device as `clawdmeter.local`, which is what `DEVICE_URL` in the daemon points
  at (macOS resolves `.local` natively). No static IP, no hardcoded SSID.
- **GPIO0 (DC) and GPIO2 (RST) are ESP8266 boot-strapping pins.** They are only
  driven by the display after boot, so flashing/booting is unaffected — don't
  "fix" this.

## Device HTTP endpoints (`clawdmeter_esp8266.ino`)

`GET /` dashboard ·
`GET|POST /usage?s=&w=&st=&wt=&sr=&wr=&stat=&bind=&t=&cpu=&mem=&disk=&bat=` push values
(s/w = %, st/wt = raw token counts, sr/wr = 5h/7d reset epochs, stat = unified-status,
bind = binding limit 1=session/2=weekly, t = server epoch for the UTC clock,
cpu/mem/disk/bat = Mac system percentages) · `GET /usage.json` current state for
the page poller, including `bl` current brightness ·
`GET|POST /brightness?value=0..255` sets/persists TFT brightness and returns JSON ·
`GET|POST /mode?screen=mac|claude|desk|face|music|timer` switches the physical LCD mode
(`face` = the animated Claude block-mascot companion; it animates via a `millis()`
poll in `loop()`, **not** a timer ISR, so it adds ~0 IRAM, and its frame interval
is recomputed from each frame's measured render time so FPS tracks board headroom) ·
`GET|POST /face?state=idle|working|sleep|monk|love|toggle` flips the companion between the
idle look-around mascot, a desk-coding scene (headphones on, typing at a laptop),
a sleeping scene (closed eyes, breathing nod, drifting Z particles), and a meditating
"claude monk" scene (a white stone statue with grey robe-fold shadows and a raised
palm; the head is a white circular dome with the orange Claude burst overlaid so the
white shows as a halo ring around the spokes — the body holds still while the burst
spokes radiate), plus a love scene (idle mascot, small laptop foreground, pulsing
Claude-orange hearts, footer mood `I LOVE MY JOB`), and switches the LCD to face mode if it isn't already. Extra scenes
reuse the same grid renderer: `deskBase` + `WORK_FRAMES` for coding, `SLEEP_FRAMES`
over the idle base for sleep, `monkBase` + `MONK_FRAMES` for the monk, and
`LOVE_FRAMES` over the idle base for love. State is
runtime-only (not persisted), defaults to idle, and is reported as `face` in
`/usage.json` ·
`GET|POST /desk?status=coding|meeting|busy|break|claude` pushes a preset desk status word;
`/desk?text=<up-to-12-safe-chars>&color=green|red|amber|blue|white|claude` pushes custom text.
The physical desk screen is a **Tend "paper" status card** (`drawDeskSign`, same warm cream
palette as the MAC/music screens): an ember hearth-flame mark + `STATUS` eyebrow in the header,
the status word centered in the body, and a short color-tinted accent rule beneath it. The
status word — preset or custom — is **typed in lowercase deep-olive ink on the paper**
(`C_TND_INK`, default GFX font, single centered line, typewriter + blinking cursor via
`deskDisplayText`; size auto-drops 4→3→2 so 12 chars fit). Unlike the old monochrome MacPaint
sign, the `color` param (and preset status color) now **also tints the physical accent rule**
via `deskAccentColor` (busy/red→ember, break/amber→marigold, meeting/blue→sky, claude→Claude
orange, white→ink, green/default→moss) — as well as the dashboard dot. The card is fully
redrawn by `drawDeskSign` on switch-in and on every `/desk` push (the push runs through
`drawMeter`→`drawDeskSign`); only the word region animates in place on the ~120 ms tick
(opaque paper-bg in-place print, no per-frame clear), so don't reintroduce a full `fillRect`
into that tick path. No clock on this screen — it stays deliberately uncrowded, matching Tend's
calm ethos (device remains reachable at `clawdmeter.local`) ·
`GET|POST /nowplaying?title=&artist=&pos=&dur=&paused=&lyric=&lyric2=&lyric3=&lt=&lt2=` pushes the
**YouTube Music now-playing** song (the daemon reads it from the Chrome tab's `navigator.mediaSession`
metadata via AppleScript, falling back to the tab *title* when page JS is blocked — the bare tab
title often stays "YouTube Music" when a song is played from the home feed, so MediaSession is
preferred; values are URL-encoded UTF-8 — Thai is preserved, not ASCII-stripped). `lyric`, `lyric2`, and `lyric3`
are the current/upcoming/look-ahead lyric lines from lrclib.net; `lt` and `lt2` are the next two
line playback positions in seconds, or `-1` when the daemon is driving plain-lyric fallback timing.
**Now-playing is a web-only
feature: the endpoint stores the song but does NOT switch the physical LCD** — the dashboard's
"Now Playing" panel (toggled by a top-bar button) reads the state from `/usage.json`. The physical
`music` screen is selected via `/mode?screen=music` — either directly, or by **opening** the
dashboard's "Now Playing" panel, whose toggle button now also requests `/mode?screen=music` (the
daemon's `/nowplaying` push still never steals focus on its own); if MUSIC happens to be the
current screen a track/pause change repaints it in place (chrome stays put, no flash), while
position/lyric resyncs repaint only the progress/time footer and lyric band. The physical screen
is the dark "Arduino Music Display" layout: warm-ink background, cream title, dim artist,
waveform mark in the header, a two-line lyric zone with a large scrolling current lyric and
dim upcoming lyric, plus a bottom elapsed/progress/total zone. There is no wall
clock and no `NOW PLAYING` / `PAUSED` eyebrow in this layout; pause state is conveyed by dimming
the footer/progress accents. Title, artist, and current-lyric marquees are advanced by a
`millis()` poll in `loop()` — **not** a timer ISR, same reason as `face`.
Title/artist/lyric lines are echoed in `/usage.json` (JSON-escaped) for `curl` debugging. **Thai/Latin text
is drawn from bundled Ayuthaya GFXfont tables (`thai_font.h`)**, not the built-in 5×7 font: that
font is ASCII-only and Arduino_GFX's `drawChar()` can't index code points > 255, so
`musicDrawText()` decodes UTF-8 and blits glyphs itself, indexing by full code point. Thai
combining marks (tone marks, upper/lower vowels) carry `xAdvance==0` with negative `xOffset` in
this font, so a faithful per-glyph blit stacks them over the base consonant with **no special
combining logic** (tone marks on a bare consonant float a touch high — legible, acceptable for
v1). Regenerate the font tables with `firmware/tools/gen_thai_font.sh` (needs Adafruit
`fontconvert` built against freetype); `#define MUSIC_TITLE_SCROLL 0` is the documented fallback
to stop scrolling if the Thai marquee ever looks wrong ·
`/update` firmware-only OTA upload form.

## Commands

**Toolchain (one-time):** `arduino-cli` + the esp8266 core + two libraries:
```sh
arduino-cli core install esp8266:esp8266 --additional-urls http://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "WiFiManager" "GFX Library for Arduino"
```

**Compile** (the sketch folder name MUST match the `.ino` name — Arduino requirement):
```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f firmware/clawdmeter_esp8266
```

**Produce the OTA binary** (libraries are baked in, so the resulting `.bin` can be
flashed at `/update` without any IDE/library install):
```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f --output-dir firmware/bin firmware/clawdmeter_esp8266
```

**Flash over USB:**
```sh
arduino-cli board list                                  # find the port
arduino-cli upload -p <PORT> --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f firmware/clawdmeter_esp8266
```
**Flash OTA:** upload `firmware/bin/clawdmeter_esp8266.ino.bin` to `/update`,
either via the browser form or with curl:
```sh
curl -F "image=@firmware/bin/clawdmeter_esp8266.ino.bin" \
     http://clawdmeter.local/update
# -> "Update OK. Rebooting..."; device is back in ~8 s. Re-poll to confirm:
#    curl -s http://clawdmeter.local/usage.json   (check "up" reset to a low value)
```
Do the *first* flash over USB — an existing `/update` from other firmware
(Tasmota/ESPHome) may reject a plain Arduino binary.

**Daemon:**
```sh
python3 -m py_compile daemon/claudemeter_daemon.py      # syntax check (stdlib only, no venv)
python3 daemon/claudemeter_daemon.py                    # run (approve the Keychain prompt once)
```

**Auto-start the daemon (macOS launchd):**
```sh
cp daemon/com.user.clawdmeter.plist ~/Library/LaunchAgents/
launchctl load -w ~/Library/LaunchAgents/com.user.clawdmeter.plist     # unload -w to stop
```
The plist uses absolute paths and `/usr/bin/python3`; update both if the project
moves or the Python changes. Run the daemon by hand once first so the Keychain
prompt is approved (launchd can't answer GUI dialogs).

**Add lyrics by hand (songs lrclib.net doesn't have):** `daemon/tools/add_lyrics.py`
writes an entry into the on-demand cache (`CLAWDMETER_LYRIC_CACHE`,
default `~/.clawdmeter/lyrics.sqlite3`), reusing the daemon's own
`lyric_key`/`lyric_cache_put` so the key always matches a real now-playing lookup.
```sh
# synced .lrc (auto-detected by [mm:ss.xx] stamps); duration accepts seconds or M:SS
python3 daemon/tools/add_lyrics.py --title "Song" --artist "Band" --duration 3:35 --lrc song.lrc
pbpaste | python3 daemon/tools/add_lyrics.py --title "Song" --duration 214      # plain via stdin
python3 daemon/tools/add_lyrics.py --title "Interlude" --duration 92 --instrumental
python3 daemon/tools/add_lyrics.py --list                                       # show cached entries
```
**The key includes the track length in whole seconds and is derived live, not from
what you type** — `dur = floor(YT Music progress-bar aria-valuemax)`, the **artist
is often empty**, and a `(ร่วมกับ …)`/`(feat …)` suffix is **part of the title**.
Guessing wrong = permanent miss. So capture the real key while the song plays:
```sh
python3 -c "import importlib.util as u; m=u.module_from_spec(s:=u.spec_from_file_location('d','daemon/claudemeter_daemon.py')); s.loader.exec_module(m); print(m.read_now_playing())"
#   -> (title, artist, pos, dur, paused)  — pass that title/artist/dur to add_lyrics.py
```
Title/artist are whitespace/case-normalized, so only the *shape* (empty artist,
feat. in title) and the duration must match. The **running** daemon checks its
in-memory `LRCLIB_CACHE` before disk, so if it already cached a miss this session,
restart it to pick up a freshly inserted entry.

## Constants worth knowing before editing

- Display pins / SPI / rotation / default brightness: `#define`s at the top of
  `clawdmeter_esp8266.ino` (`LCD_BRIGHTNESS` 0–255, default 90). Runtime
  brightness overrides are stored in EEPROM.
- `DEVICE_URL`, `POLL_INTERVAL`, `KEYCHAIN_SERVICE`, `API_BODY` (model
  `claude-haiku-4-5-20251001`): top of `claudemeter_daemon.py`.
- `CLAWDMETER_DEVICE_URL` (env override for `DEVICE_URL`),
  `CLAWDMETER_DEVICE_TIMEOUT` / `CLAWDMETER_DEVICE_PUSH_ATTEMPTS`
  (device push retry tuning),
  `CLAWDMETER_USAGE_SOURCE` (`api` or `local`),
  `CLAWDMETER_SESSION_TOKEN_LIMIT` / `CLAWDMETER_WEEKLY_TOKEN_LIMIT`
  (token budgets the progress bars represent in `local` mode; defaults 30M / 100M),
  `CLAWDMETER_LRCLIB_DB` (path to a downloaded lrclib SQLite dump from
  https://lrclib.net/db-dumps — when set, music lyrics are looked up locally via
  the stdlib `sqlite3` instead of `lrclib.net`: offline, sub-ms, no rate limits.
  `lrclib_json` dispatches `/api/get`/`/api/search` to the dump — exact `name_lower`
  match (+/-2s duration) then an FTS5 fuzzy fallback — and returns the same camelCase
  JSON shape as the API, so `fetch_lyrics` is unchanged. A clean miss stays local;
  only a sqlite *error* falls back to the network),
  `CLAWDMETER_LYRIC_CACHE` (path to a persistent on-demand lyric cache, default
  `~/.clawdmeter/lyrics.sqlite3` — set empty to disable). Every song fetched from
  lrclib.net is written through to this tiny SQLite (grows only with songs actually
  played), and `fetch_lyrics` reads it before the in-memory `LRCLIB_CACHE` misses to
  the network — so replays are offline/instant and the cache survives daemon
  restarts. Only real results are stored; "none found" stays session-only so a song
  missing today can be picked up later. This is the no-download alternative to the
  full `CLAWDMETER_LRCLIB_DB` dump and coexists with it.
- The compiled `.bin` is built for `nodemcuv2` (4 MB flash) with the `mmu=4816`
  and `ip=hb2f` options. A 1 MB board (e.g. ESP-01) needs a different FQBN/flash
  layout for OTA. Don't drop `ip=hb2f` — it's what makes the 15 KB dashboard load
  reliably (see the lwIP note above).

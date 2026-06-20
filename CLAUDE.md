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
- **The dashboard `/` page (~15 KB) needs the Higher-Bandwidth lwIP variant
  (`ip=hb2f`) AND `handleRoot` keeping the radio awake.** The page is sent in one
  blocking `server.send_P`. With the default `ip=lm2f` (Lower Memory) lwIP, the TCP
  send buffer is only `2×MSS` (1072 B); under `WIFI_MODEM_SLEEP` ACKs return slowly,
  the buffer can't drain, and the write stalls and closes the connection mid-page —
  the page truncated at an MSS multiple ~90% of the time and Chrome showed a failed
  load (looked like the device was unreachable even though ping/`usage.json` were
  fine). Fix is two-part: build with `ip=hb2f` (bigger send buffer/window — the
  dominant fix) **and** `handleRoot` flips `WiFi.setSleepMode(WIFI_NONE_SLEEP)` +
  `backlightStopForFlash()` (park the PWM ISR) for the duration of the send, then
  restores both. Small endpoints (`/usage.json`, ~275 B) fit one segment and were
  never affected, which is what made it look like a network problem. If you ever add
  a comparably large response, gzip it (`Content-Encoding: gzip`) rather than relying
  on the link sustaining a 15 KB blocking push.
- **Arduino_GFX draws directly to the panel (no canvas/framebuffer)** — a 240×240×2
  buffer (115 KB) would not fit ESP8266 RAM. Use the ESP8266 `16KB cache + 48KB IRAM`
  MMU layout (`:mmu=4816` in the FQBN, or Tools → MMU in Arduino IDE). The balanced
  default layout is tight at ~94% instruction RAM; `:mmu=4816` builds at ~69%
  instruction RAM with the same code. Adding `ICACHE_RAM_ATTR`/`IRAM_ATTR` code can
  still overflow `iram1`, so avoid new ISR/timer-heavy features.
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
`GET|POST /mode?screen=mac|claude|desk` switches the physical LCD mode ·
`GET|POST /desk?status=coding|busy|break` pushes a preset full-screen desk status sign;
`/desk?text=<up-to-12-safe-chars>&color=green|red|amber|blue|white` pushes custom text/color ·
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
  (token budgets the progress bars represent in `local` mode; defaults 30M / 100M).
- The compiled `.bin` is built for `nodemcuv2` (4 MB flash) with the `mmu=4816`
  and `ip=hb2f` options. A 1 MB board (e.g. ESP-01) needs a different FQBN/flash
  layout for OTA. Don't drop `ip=hb2f` — it's what makes the 15 KB dashboard load
  reliably (see the lwIP note above).

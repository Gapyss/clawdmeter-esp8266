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
   └── HTTP POST /usage?s=&w= ──> ESP8266 ──> ST7789 panel  +  web dashboard
```

The OAuth token never leaves the Mac; the device only ever receives two integers.

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
  `POST /usage?s=<int>&w=<int>` so the firmware needs no JSON parser. Keep it this
  way unless you add ArduinoJson for a reason.
- **Display init mirrors the GeekMagic open firmware and the exact values matter.**
  In `clawdmeter_esp8266.ino`: backlight on GPIO5 is **ACTIVE-LOW** (`LOW` = on),
  panel is initialized at **SPI mode 3, 40 MHz**, pins DC=GPIO0 / RST=GPIO2 /
  CS→GND / MOSI=GPIO13 / SCK=GPIO14. Getting backlight polarity or SPI mode wrong
  = blank screen. `#define LCD_ROT` is the per-product orientation knob (HelloCubic
  Lite vs SmallTV-Ultra differ only here; try 0/2/4/6).
- **Backlight is PWM-dimmed, not just on/off.** `setBacklight(uint8_t)` drives GPIO5
  with 20 kHz software PWM; because the pin is active-low it inverts the duty
  (`analogWrite(LCD_BL, 255 - brightness)`), so brightness 255 = full on, 0 = off.
  `#define LCD_BRIGHTNESS` (default 90) sets the level — full brightness (255) ran
  the panel hot, since the LED string behind the glass is the main heat source.
  The PWM timer ISR lives in IRAM, so this pushed the build from ~92% → ~94% IRAM.
- **WiFi uses modem-sleep + a `delay(2)` in `loop()`.** `WiFi.setSleepMode(WIFI_MODEM_SLEEP)`
  lets the radio idle between AP beacons, but it only engages because `loop()` now
  yields via `delay(2)` — without that yield the non-blocking `handleClient()` spins
  the core flat out and the radio never sleeps (hotter, more current). Don't remove
  the `delay()`; it's load-bearing for thermal/power, not a throttle.
- **Arduino_GFX draws directly to the panel (no canvas/framebuffer)** — a 240×240×2
  buffer (115 KB) would not fit ESP8266 RAM. **IRAM is at ~94%** (after the PWM ISR;
  ~3.8 KB headroom); adding `ICACHE_RAM_ATTR`/`IRAM_ATTR` code can overflow `iram1`
  and fail the link.
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

`GET /` dashboard · `GET|POST /usage?s=&w=&st=&wt=&sr=&wr=&stat=&bind=&t=` push values
(s/w = %, st/wt = raw token counts, sr/wr = 5h/7d reset epochs, stat = unified-status,
bind = binding limit 1=session/2=weekly, t = server epoch for the UTC clock) ·
`GET /usage.json` current state for the page poller · `/update` OTA upload form
(ESP8266HTTPUpdateServer).

## Commands

**Toolchain (one-time):** `arduino-cli` + the esp8266 core + two libraries:
```sh
arduino-cli core install esp8266:esp8266 --additional-urls http://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "WiFiManager" "GFX Library for Arduino"
```

**Compile** (the sketch folder name MUST match the `.ino` name — Arduino requirement):
```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware/clawdmeter_esp8266
```

**Produce the OTA binary** (libraries are baked in, so the resulting `.bin` can be
flashed at `/update` without any IDE/library install):
```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 --output-dir firmware/bin firmware/clawdmeter_esp8266
```

**Flash over USB:**
```sh
arduino-cli board list                                  # find the port
arduino-cli upload -p <PORT> --fqbn esp8266:esp8266:nodemcuv2 firmware/clawdmeter_esp8266
```
**Flash OTA:** upload `firmware/bin/clawdmeter_esp8266.ino.bin` at
`http://clawdmeter.local/update`. Do the *first* flash over USB — an existing
`/update` from other firmware (Tasmota/ESPHome) may reject a plain Arduino binary.

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

- Display pins / SPI / rotation / brightness: `#define`s at the top of
  `clawdmeter_esp8266.ino` (`LCD_BRIGHTNESS` 0–255, default 90).
- `DEVICE_URL`, `POLL_INTERVAL`, `KEYCHAIN_SERVICE`, `API_BODY` (model
  `claude-haiku-4-5-20251001`): top of `claudemeter_daemon.py`.
- `CLAWDMETER_DEVICE_URL` (env override for `DEVICE_URL`),
  `CLAWDMETER_USAGE_SOURCE` (`api` or `local`),
  `CLAWDMETER_SESSION_TOKEN_LIMIT` / `CLAWDMETER_WEEKLY_TOKEN_LIMIT`
  (token budgets the progress bars represent in `local` mode; defaults 30M / 100M).
- The compiled `.bin` is built for `nodemcuv2` (4 MB flash). A 1 MB board (e.g.
  ESP-01) needs a different FQBN/flash layout for OTA.

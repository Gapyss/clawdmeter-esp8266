# Post-mortem: PWM backlight ISR crashes ESP8266 under HTTP load / mid-OTA

**Date:** 2026-06-20 · **Owner:** Gapyss · **Board:** GeekMagic HelloCubic Lite (`clawdmeter.local` / 192.168.1.36)
**Offending change:** `5a16d6f` (+ an uncommitted WIP build flashed on top) · **Recovery:** rollback to `ce5887a` via OTA

## Summary

A 20 kHz software-PWM backlight ISR on the ESP8266 (`setBacklight` → `analogWrite(LCD_BL, …)`,
IRAM timer, introduced in `5a16d6f`) destabilized the device under any HTTP load and corrupted
OTA flash writes. The board answered ping but crash-rebooted whenever it had to serve HTTP, so the
web dashboard wouldn't open and OTA "refused." Recovered without USB by rolling the firmware back to
`ce5887a` (pre-PWM) over OTA, using a `GET /brightness?value=255` trick to silence the ISR mid-flight
so the upload could complete. The underlying firmware defect is now **fixed at the source** (PWM
dropped to 1 kHz + the ISR silenced around every flash write) and validated on hardware.

## Symptom

Device pingable but web-dead. `ping 192.168.1.36` → 0% loss, but `curl http://192.168.1.36/` → TCP
connect succeeds in ~10 ms, then no HTTP body, connection reset (`curl: (56)`) or timeout (`curl: (28)`).
Every endpoint (`/`, `/usage.json`, `/brightness`, `/mode`) returned `000`. On the TFT: a visible
`starting… → waiting for daemon… → (occasional usage) → reset` loop. OTA via `/update` aborted
mid-upload ("refused").

## Root cause

Commit `5a16d6f` added PWM backlight dimming: `setBacklight(uint8_t)` calls `analogWriteFreq(20000)` +
`analogWrite(LCD_BL, 255 - brightness)`, which drives the ESP8266 software-waveform generator — an IRAM
timer ISR firing ~40,000×/sec for a single pin. That ISR starves the non-blocking WiFi/TCP servicing in
`loop()` (`server.handleClient()` + `delay(2)`) badly enough that any sustained or concurrent HTTP
exchange crash-resets the chip. During OTA the same ISR collides with SPI-flash erase/write:
`handleFirmwareUpload`'s `Update.write()` path reset the device at a *fixed byte offset (~127 KB / 44%)
every time*, independent of upload speed — the signature of an interrupt-vs-flash-timing conflict, not
heap or bandwidth. The flashed image was an uncommitted WIP built on top of `5a16d6f` (its `/usage.json`
carried `screen`/`desk`/`bl` but no `rst`), so it inherited the defect.

## Why it produced the symptom

Idle, there's no HTTP work and the ISR's CPU theft is invisible — hence clean ping (ICMP is answered by
the SDK, not `loop()`) and a stable device when left alone. The moment a client drives real traffic — a
browser opening several connections for the dashboard, or a ~30 s OTA transfer — the ISR-starved stack
either drops the exchange (dashboard 000s) or the chip resets (flash-write collision during OTA). The
user saw "can't open web" and "update refused"; both are the same crash, surfaced under load. The reset
loop on-screen was the watchdog/exception recovering between load events.

## Fix

Recovery, not a code patch: OTA-flashed `ce5887a` (the last commit before PWM + modem-sleep — no IRAM
PWM ISR, standard `ESP8266HTTPUpdateServer`). The enabling step was `GET /brightness?value=255`: the
extreme value makes `analogWrite(LCD_BL, 0)` *stop the waveform generator* (ESP8266 `analogWrite`
removes a pin from the generator at 0/range), disabling the ISR. With the ISR silenced, the gzipped
`ce5887a` image (296 KB) uploaded to `/update` to 100% → HTTP 200 "Update OK. Rebooting…". This
addresses the symptom by removing the offending ISR from the running firmware entirely.

**Source fix (landed after recovery, on `feat/backlight-dimming-thermal`).** Two changes to
`clawdmeter_esp8266.ino`: (1) `setBacklight` now uses `LCD_PWM_FREQ` = 1 kHz instead of 20 kHz —
1/20th the interrupt rate, enough to keep the WiFi/TCP stack responsive under HTTP load while staying
above flicker fusion; (2) a new `backlightStopForFlash()` (`analogWrite(LCD_BL, 0)`, which detaches
the pin from the waveform generator) is called before `Update.begin` in `handleFirmwareUpload` and
around `EEPROM.commit()` in `saveBrightness`, so no PWM interrupt can ever collide with a flash
erase/write. This bakes the recovery trick into the firmware: OTA now silences its own ISR.

## How it was found

Repro narrowed from "can't open web, can ping" to a controlled differential (breadcrumb ledger R1–R16):

- *TCP accepts but HTTP never completes* (R2/R4) → not a network/WiFi-association problem.
- *Killed the daemon, freed its single-client slot → still 000* (R8) → rejected "daemon hogging the single-client server."
- *15-ping idle vs ping-during-HTTP-hammer*: 0% loss idle (R3/R12) vs **87.5% loss under HTTP load** (R11) → rejected "autonomous reboot loop"; isolated the trigger to **HTTP load**.
- *Caught one `/usage.json` reply in 40 s* (no `rst` field) → identified the flashed image as uncommitted WIP, and confirmed a ~1 s up-window per crash cycle.
- *OTA at full speed vs throttled 20 KB/s both die at the identical 127 KB offset* (R14/R15) → rejected heap/buffer-pressure and time-limit hypotheses; a fixed byte offset implicated interrupt-vs-flash timing.
- **Single confirming experiment:** land `GET /brightness?value=255` to stop the PWM waveform, then retry OTA → upload completed 100% (R16). Silencing the ISR removed the crash. Cause confirmed.

## Why it slipped through

Workload/coverage gap. `5a16d6f`'s commit message verified the build *links* with IRAM headroom, but
nothing exercised the firmware under concurrent HTTP load or an OTA transfer with the PWM ISR active.
The ISR is benign at idle, so a quick "does it boot and connect" check passes; the defect only appears
when a client drives sustained traffic. There's no automated/hardware-in-the-loop test for "serve the
dashboard under load" or "OTA succeeds," so the first real load — a browser page-load and an OTA attempt
— was the discovery vector. CLAUDE.md *did* warn that IRAM ISR features are risky; the warning wasn't
backed by a test.

## Validation

Post-rollback on the physical board (192.168.1.36 / `clawdmeter.local`):

- `GET /` → HTTP 200 in 0.6 s; `/usage.json` returns the `ce5887a` field set (confirms rollback live).
- Load test: 15 rapid dashboard fetches + concurrent 20-ping → **0% ping loss, no reboot**, 12/15 loads 200 (the 3×000 are the single-client server dropping *overlapping* requests — not crashes; the old firmware would have reset and dropped pings).
- Coverage honesty: validated only on this one board. Daemon was left stopped during validation; not yet re-verified end-to-end with the daemon pushing.

**Source-fix validation (on hardware, after OTA-flashing the patched WIP onto the recovered board):**
- HTTP load with PWM active at 1 kHz (`bl:120`): 20/20 dashboard loads succeeded, ping 25/25, uptime climbed 39→105 s with no reset. The 20 kHz build crash-rebooted under this exact load.
- OTA while running the patched firmware with PWM active, full speed, **no brightness trick**: completed 100% → clean reboot (`rst:"Software/System restart"`, uptime climbing). The old build reset at ~127 KB every time. This confirms the in-firmware flash guard works.

## Action items / follow-ups

- ~~Fix the real defect on `feat/backlight-dimming-thermal`.~~ **Done** — PWM → 1 kHz + `backlightStopForFlash()` around OTA and `EEPROM.commit()`; validated on hardware (see Validation). (Owner: Gapyss.) Not yet committed/PR'd.
- **Add a load/OTA smoke check** before flashing PWM firmware: serve `/` under concurrent load + complete one OTA, on hardware. No CI exists for this repo; at minimum a documented manual gate. (Owner: Gapyss.)
- **Document the recovery trick** — "to OTA a board stuck on PWM firmware, `GET /brightness?value=255` first to silence the ISR, then upload gzipped." Saved to project memory (`pwm-isr-breaks-wifi-and-ota.md`); fold into CLAUDE.md when the fix lands. (Owner: Gapyss.)
- **Re-verify end-to-end:** restart the daemon and confirm pushes render on `ce5887a`. (Owner: Gapyss.)

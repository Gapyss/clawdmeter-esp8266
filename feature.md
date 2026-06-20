# Feature proposal: backlight dimming + WiFi modem-sleep (thermal optimization)

## Problem

The device runs hot to the touch — specifically the screen glass. The TFT
backlight is a string of LEDs directly behind the panel and was driven fully on,
continuously, with no way to turn it down. Separately, the ESP8266 never let its
WiFi radio idle, so it sat in full active receive the whole time.

## Root cause (from tracing `clawdmeter_esp8266.ino`)

1. **Backlight hardwired to 100%.** `setup()` did `digitalWrite(LCD_BL, LOW)`
   (active-low = full brightness) and never touched the pin again. The LED string
   behind the glass is the dominant "screen is hot" heat source.
2. **`loop()` spun with no yield.** `MDNS.update()` + `server.handleClient()` are
   both non-blocking and there was no `delay()`, so the core ran flat out and the
   ESP8266 WiFi modem-sleep could never engage — the radio stayed in continuous
   active RX (~56–70 mA), heating the module and loading the regulator.

Not addressable in firmware: the AMS1117 linear regulator dropping 5V→3.3V is a
PCB hot spot, but lower backlight + modem-sleep both reduce its current load.
CPU is already at 80 MHz (`nodemcuv2` default), so no change there.

## Proposed change (implemented)

- **PWM backlight dimming.** New `setBacklight(uint8_t)` helper drives GPIO5 with
  20 kHz software PWM, correctly inverted for the active-low pin (brightness 255 =
  full on, 0 = off). New `#define LCD_BRIGHTNESS 90` knob (0–255), default ~90.
- **Runtime brightness control.** The web dashboard exposes a Brightness slider
  backed by `GET|POST /brightness?value=0..255`; the ESP8266 stores the selected
  value in EEPROM so the dimming level survives reboot.
- **WiFi modem-sleep.** `WiFi.setSleepMode(WIFI_MODEM_SLEEP)` after connect, plus
  a `delay(2)` at the end of `loop()` so the SDK can actually park the radio
  between AP beacons. CPU stays on, so the web server and 1 s clock are unaffected.

## Cost / risk

- **IRAM.** The PWM timer ISR lives in IRAM. With the ESP8266 default balanced
  MMU layout this links at **94% (61,659 / 65,536 bytes, ~3.8 KB headroom)**.
  Build with `:mmu=4816` (`16KB cache + 48KB IRAM`) to get **69%
  (45,291 / 65,536 bytes)** while keeping the dimmed backlight:
  `arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816`.
  Further ISR/timer-heavy additions are still risky — watch the link step.
- **Responsiveness.** `delay(2)` adds ≤2 ms latency to HTTP requests and clock
  ticks — invisible at the once-per-second cadence the dashboard polls.
- **Reversible.** Set the dashboard slider or `/brightness?value=255` to restore
  old brightness; remove the `setSleepMode`/`delay` lines to restore old WiFi
  behavior.

## How to verify (physical — needs a finger on the glass)

1. Flash, run ~15 min, feel the glass.
   - Cooler screen → backlight was the source (confirmed); tune the dashboard
     brightness slider.
   - Glass still hot, back cooler → ESP/regulator heat; modem-sleep is helping.
   - No change → regulator-dominated (normal warmth, not firmware-fixable).
2. Decisive A/B: set `LCD_BRIGHTNESS 0`, flash, feel after 10 min — if the glass
   goes cold, the backlight is isolated as the dominant source.

## Possible follow-ups

- Automatic night dimming on a schedule using the clock the firmware already has.

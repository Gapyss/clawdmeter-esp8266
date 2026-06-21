# Clawdmeter for ESP8266

A web-dashboard version of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
for a bare ESP8266. A Python daemon on your Mac reads your Claude Code usage and
Mac system metrics and pushes them to the ESP8266, which serves a live dashboard
at `http://clawdmeter.local/`.

```
Mac daemon ──poll 60s──> api.anthropic.com   (reads usage headers)
   └── HTTP POST usage + Mac metrics ──> ESP8266 ──serves──> dashboard
```

The OAuth token stays on the Mac. The ESP8266 receives usage percentages, reset
times, and simple Mac metrics only.

## Hardware

GeekMagic **HelloCubic Lite** / **SmallTV-Ultra** — ESP8266 + ST7789 240×240
color TFT. The firmware drives the screen *and* serves the web dashboard at the
same time, so you get the meter on the device and in the browser.

## 1. Flash the firmware

1. Open `firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino` in the Arduino IDE
   (Boards Manager → install "esp8266 by ESP8266 Community";
   Library Manager → install "WiFiManager" by tzapu **and**
   "GFX Library for Arduino" by moononournation).
2. Select your board (e.g. NodeMCU 1.0 / Wemos D1 mini), set
   **Tools → MMU → 16KB cache + 48KB IRAM**, select the right port, then Upload.
   No WiFi credentials to edit — you set those on first boot.
3. On first boot the device starts a hotspot called **Clawdmeter-setup**. Join it
   from your phone; the setup page pops up; pick your WiFi and enter the password.
   It saves and reboots onto your network.
4. Visit `http://clawdmeter.local/` (or the IP shown in Serial Monitor at 115200
   baud) — you should see the meter showing "waiting for daemon…". Click the
   mode button on the web dashboard to switch both the browser dashboard and the
   physical LCD between Claude usage, Mac system monitor, and Desk Status modes.
   Use the **Desk** buttons to push "Coding", "Busy", or "Break" to the physical
   screen, or enter custom text and choose a color. Use the **Brightness** slider
   in the Device panel to tune the TFT backlight without reflashing; the value is
   saved on the ESP8266 and survives reboot.

**Changing WiFi later:** if the device can't reach the saved network, it reopens
the **Clawdmeter-setup** hotspot on its own — rejoin and pick the new network.
No re-flashing.

### Updating the firmware later (OTA, no USB)

This firmware serves an update page at `http://clawdmeter.local/update`. To push
a new version over WiFi:

1. In the Arduino IDE: **Sketch → Export Compiled Binary** — this produces
   `clawdmeter_esp8266.ino.bin` (in the sketch folder, or a `build/` subfolder on
   IDE 2.x). The update page needs the **compiled `.bin`**, not the `.ino`.
2. Open `http://clawdmeter.local/update`, choose that `.bin`, click Upload.
   The device flashes and reboots on its own.

Do the **first** flash over USB (the existing `/update`, if any, may not accept a
plain Arduino binary). Every flash after that can be OTA.

For CLI builds, use the same IRAM-friendly MMU profile:

```sh
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816 --output-dir firmware/bin firmware/clawdmeter_esp8266
```

> The `/update` page has no password. On a trusted home LAN that's usually fine;
> to lock it, change `httpUpdater.setup(&server);` to
> `httpUpdater.setup(&server, "admin", "yourpassword");` and re-flash.

## 2. Run the daemon (Mac)

```sh
python3 daemon/claudemeter_daemon.py
```

- First run, macOS may pop a Keychain prompt for the `Claude Code-credentials`
  item — click **Always Allow**.
- It prints Claude usage plus Mac CPU/memory each minute; the dashboard updates
  within ~3s.
- If you see `401 Unauthorized`, your Claude Code login expired — run any
  `claude` command to refresh it, then restart the daemon.
- Device pushes default to a 5 second timeout and 3 attempts, which helps when the
  ESP8266 is still settling after a reboot. Override with
  `CLAWDMETER_DEVICE_TIMEOUT` or `CLAWDMETER_DEVICE_PUSH_ATTEMPTS` if your WiFi is
  unusually slow.

The daemon targets `http://clawdmeter.local` by default (macOS resolves `.local`
natively). If it can't resolve, set `CLAWDMETER_DEVICE_URL` to the device's IP
from Serial Monitor.

To use the real server-side Anthropic rate-limit headers explicitly:

```sh
CLAWDMETER_USAGE_SOURCE=server \
CLAWDMETER_DEVICE_URL=http://192.168.1.36 \
python3 daemon/claudemeter_daemon.py
```

`CLAWDMETER_USAGE_SOURCE=api`, `server`, and `headers` all mean the same thing.
Use `local` only when you want to estimate usage from local Claude Code logs.

### Status bar app

Build the native macOS menu bar app:

```sh
macos/build-statusbar-app.sh
```

Then launch:

```sh
open dist/Clawdmeter.app
```

The menu bar item can start, stop, and restart the daemon through launchd, open
the ESP8266 dashboard, and open the daemon log folder. On first start it writes
`~/Library/LaunchAgents/com.user.clawdmeter.plist` with paths for this checkout,
then bootstraps the agent.

Run `python3 daemon/claudemeter_daemon.py` once by hand first if you have not
already approved the Keychain prompt; click **Always Allow** for the
`Claude Code-credentials` item so the launch agent can run unattended.

To switch the physical LCD directly:

```text
http://clawdmeter.local/mode?screen=mac
http://clawdmeter.local/mode?screen=claude
http://clawdmeter.local/mode?screen=desk
```

To set brightness directly:

```text
http://clawdmeter.local/brightness?value=90
```

To reboot the device without changing settings:

```text
http://clawdmeter.local/restart
```

To clear saved WiFi and brightness settings, then reboot into the setup hotspot:

```text
http://clawdmeter.local/factory-reset
```

To push a desk status directly:

```text
http://clawdmeter.local/desk?status=coding
http://clawdmeter.local/desk?status=claude
http://clawdmeter.local/desk?status=busy
http://clawdmeter.local/desk?status=break
http://clawdmeter.local/desk?text=FOCUS&color=blue
```

## 3. Auto-start on login (optional)

Run the daemon once by hand first (step 2) and click **Always Allow** on the
Keychain prompt — otherwise launchd has no way to answer it.

```sh
cp daemon/com.user.clawdmeter.plist ~/Library/LaunchAgents/
launchctl load -w ~/Library/LaunchAgents/com.user.clawdmeter.plist
```

It now starts at login and restarts if it dies. Output goes to
`daemon/clawdmeter.log` (and `clawdmeter.err.log`):

```sh
tail -f daemon/clawdmeter.log
```

To stop and disable it:

```sh
launchctl unload -w ~/Library/LaunchAgents/com.user.clawdmeter.plist
```

The plist uses absolute paths to `/Users/g4pys/clawdmeter-esp8266/...` — if you
move the folder, edit the paths in the plist (and reload it).

## If the screen looks wrong

- **Blank / black:** check the backlight line — this firmware drives it active-LOW
  (`digitalWrite(LCD_BL, LOW)` = on), matching the GeekMagic wiring.
- **Rotated or mirrored:** change `#define LCD_ROT 4` near the top of the sketch
  (try `0`, `2`, or `6`) and re-flash. HelloCubic Lite and SmallTV-Ultra differ
  only in this value.
- **Garbled colors/pixels:** the panel is initialized at SPI mode 3, 40 MHz — the
  GeekMagic defaults. Don't change those unless your unit differs.

## Notes

- Each poll makes a real 1-token Haiku request — that's how the usage headers are
  returned, same as upstream Clawdmeter. Negligible cost, but not zero.
- To keep it running in the background, leave the terminal open, or wrap it in a
  `launchd` plist later.

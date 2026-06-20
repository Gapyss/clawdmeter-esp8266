# Post-mortem: dashboard page truncates mid-send (device pingable, web-dead)

**Date:** 2026-06-20 · **Owner:** Gapyss · **Board:** GeekMagic HelloCubic Lite (`clawdmeter.local` / 192.168.1.36)
**Offending change:** latent — `5a16d6f` added `WIFI_MODEM_SLEEP`; the dashboard HTML grew to ~15 KB on `feat/mac-monitor-desk-modes` · **Fix:** `2742d66` (PR #4)

## Summary

The web dashboard at `GET /` failed to load in a browser ~90% of the time. The device returned `200 OK` then dropped the connection after ~1–3 KB of the 15 KB body, which Chrome surfaced as `ERR_ADDRESS_UNREACHABLE` — making it look like a network/address problem even though ping, mDNS (`clawdmeter.local`), and the small `/usage.json` endpoint were all healthy. Root cause: the page is sent as one blocking `server.send_P`; with the default Lower-Memory lwIP the TCP send buffer is only `2×MSS` (1072 B), and under `WIFI_MODEM_SLEEP` ACKs return slowly so the buffer can't drain, the write stalls, and the socket closes mid-page. Fixed by building with the Higher-Bandwidth lwIP variant (`ip=hb2f`), serving the page gzipped (15469 → 4424 B), and `flush()`ing the TX buffer before `Connection: close`. Validated on hardware: full-page load reliability went from ~10% to 15/15 under realistic loads.

## Symptom

Device pingable but the dashboard wouldn't open. `ping 192.168.1.36` → 0% loss, `dns-sd` resolved `clawdmeter.local` → 192.168.1.36, `curl http://192.168.1.36/` connected and returned `HTTP/1.1 200 OK` with `Content-Length: 15469` — then died mid-body:

```
< HTTP/1.1 200 OK
< Content-Length: 15469
* transfer closed with 14397 bytes remaining to read
curl: (18) transfer closed with 14397 bytes remaining to read
```

Repeated fetches delivered a variable, always-partial prefix: 1072, 1608, 2744, 3055 bytes of 15469 — every truncation landing on a multiple of the 536-byte TCP MSS (2×, 3×, … ×536). The small `/usage.json` endpoint (275 B) returned reliably in ~35 ms, 5/5. Chrome rendered the failed transfer as `ERR_ADDRESS_UNREACHABLE`.

## Root cause

`handleRoot` sends the whole dashboard in one blocking call:

```c
server.send_P(200, "text/html", INDEX_HTML);   // INDEX_HTML is ~15 KB PROGMEM
```

`send_P` pushes the body into the lwIP TCP send buffer and blocks while it drains. Two conditions made that drain fail:

1. **Tiny send buffer.** The board builds with the default Lower-Memory lwIP variant (`ip=lm2f`), where `TCP_SND_BUF = 2×MSS = 1072 B`. Only ~1 KB can be in flight before the write must wait for ACKs to free buffer space.
2. **Slow ACKs under modem-sleep.** `5a16d6f` set `WiFi.setSleepMode(WIFI_MODEM_SLEEP)`, so the radio dozes between AP beacons. ACK round-trips lag, the 1072-byte buffer drains slowly, and the WebServer's blocking write times out mid-transfer (~3.5 s) and closes the connection — hence truncation at a whole number of MSS-sized segments.

The 1 kHz backlight PWM ISR (`setBacklight` → `analogWrite(LCD_BL, …)`, the same waveform generator from the [PWM-ISR post-mortem](pwm-isr-ota.md)) is a secondary contributor: it steals CPU during the multi-second send. A third, smaller factor is a **close-race**: even once the body fit the buffer, `Connection: close` tore the socket down before lwIP got the final segment ACKed on a lossy link, truncating the tail at a fixed `2920 = 2×1460`.

The dashboard was always served this way; it became a reliable failure on `feat/mac-monitor-desk-modes` once the page grew to ~15 KB (Mac/Desk screens, brightness slider, mode controls) — large enough to need many segments and many ACK round-trips, every one of them slowed by modem-sleep.

## Why it produced the symptom

`/usage.json` (275 B) and the other control endpoints fit in a single TCP segment, so they complete before any ACK-drain wait — they were never affected, which is exactly why the device looked "up." Only a response that spans multiple segments hits the slow-drain path. ICMP is answered by the SDK, not `loop()`, so ping stayed clean throughout. The browser, asking for the one large response on the device, got a connection that opened, delivered a partial body, and reset — and Chrome reported that as `ERR_ADDRESS_UNREACHABLE`, pointing the investigation at the network when the device and the LAN were fine.

## Fix

`2742d66` (PR #4), three layered changes — each one measured before adding the next:

1. **Higher-Bandwidth lwIP (`ip=hb2f`).** Larger `TCP_SND_BUF`/window. The FQBN is now `esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f` everywhere in `CLAUDE.md`. This alone took reliability ~10% → ~75%; remaining failures moved to larger offsets (`5840 = 4×1460`), confirming buffer size — not a crash — was the limiter.
2. **Serve the page gzipped.** `handleRoot` now sends a precompressed blob with `Content-Encoding: gzip` (15469 → 4424 B), so the whole body fits one send-buffer fill and the write never has to wait for a mid-transfer drain. `INDEX_HTML` stays the human-editable source; `firmware/tools/gen_index_gz.py` extracts it, gzips it, and regenerates `firmware/clawdmeter_esp8266/index_html_gz.h`. → ~92%.
3. **Drain before close.** After `send_P`, `handleRoot` calls `server.client().flush()` to block until the TX buffer is sent, closing the `Connection: close` race that dropped the final segment. `handleRoot` also flips `WIFI_NONE_SLEEP` for the brief send as cheap, invisible insurance (the earlier attempt also parked the PWM via `backlightStopForFlash()`, but that flashed the screen to full brightness on every load and is unnecessary once the page fits one buffer, so it was dropped). → 15/15 on realistic loads.

These address the root cause (a multi-segment response that can't drain) rather than the symptom: the gzipped body is small enough that the blocking send completes in one write, and the flush guarantees the tail is delivered before teardown.

## How it was found

Repro was immediate and high-rate: `curl http://192.168.1.36/` truncated on ~90% of fetches, with `size_download` giving an exact byte count each time.

- *Ping/mDNS/`curl` all reach the device; route to `.36` is via `en0`, no proxy/VPN* → rejected "network/address/proxy problem" despite the Chrome error string.
- *`/usage.json` 5/5 reliable, `up:1465`, 20/20 pings clean during small requests* → rejected "device is crash-rebooting" (the [prior PWM bug's](pwm-isr-ota.md) signature); isolated the failure to **large responses only**.
- *First hypothesis: PWM-ISR starvation.* Tested with `GET /brightness?value=255` to stop the waveform — but `applyBrightness` clamps to `LCD_MAX_BRIGHTNESS` (120), so the value never reached the 255/range that detaches the pin. **Invalid test.** Re-ran with `value=0` (which does stop the waveform, screen dark): 1 of 3 fetches delivered the full 15469 B — the first complete load ever observed. So PWM is *a* contributor but not the whole story.
- *Truncations land on exact MSS multiples (1072 = 2×536, 1608 = 3×536, …) and throughput collapses to ~1 KB/3 s* → implicated the TCP send buffer + slow ACK drain, not heap or CPU.
- **Confirming experiment:** rebuild with `ip=hb2f` (bigger buffer) → reliability jumped to 75% and failures moved to `4×1460` offsets; then gzip (one-buffer body) → 92%; then `flush()` → 15/15 spaced. Each step moved the number in the direction the buffer/drain model predicted, confirming the cause.

(A measurement trap worth recording: `wc -c < file` on macOS emits leading spaces, so `"   15469" != "15469"` reported false truncations in one test pass. The bytes were full; the comparison was wrong.)

## Why it slipped through

Workload/coverage gap plus latent growth. The dashboard was reliably served when it was smaller — the [PWM post-mortem's](pwm-isr-ota.md) validation recorded 20/20 dashboard loads on the recovered board. On `feat/mac-monitor-desk-modes` the page grew to ~15 KB (Mac/Desk screens, brightness slider, mode controls), crossing the send-reliability threshold, and no load test re-checked large-page delivery after that growth. The default Lower-Memory lwIP variant and `WIFI_MODEM_SLEEP` (`5a16d6f`) were each individually fine; their interaction only bites a multi-segment response, which nothing exercised. There is no automated/hardware-in-the-loop "serve the full dashboard" check for this repo — the first browser open was the discovery vector.

## Validation

On the physical board (192.168.1.36 / `clawdmeter.local`), after OTA-flashing `2742d66`:

- Headers correct: `Content-Encoding: gzip`, `Content-Length: 4424`; `curl --compressed` decodes to 15469 B ending in `</body></html>`.
- **Realistic loads (1.5–2 s apart, like a browser/refresh): 15/15 full pages**, ~0.3 s each. Prior firmware on this branch: ~10%.
- Intermediate steps measured on the same board: `ip=hb2f` alone 15/20; `+gzip` 23/25; `+flush()` 15/15 spaced.
- Coverage honesty: validated on this one board only. Under **rapid back-to-back** hammering (no gap, not browser behavior) reliability is ~87% (26/30) — the device closing/reopening connections with no settle time still occasionally truncates; not addressed because no real client does this. Daemon pushes to `/usage` and the other small endpoints were unaffected throughout and not re-tested.

## Action items / follow-ups

- ~~Make the dashboard load reliably.~~ **Done** — `ip=hb2f` + gzip + `flush()`; validated on hardware. (Owner: Gapyss, `2742d66` / PR #4.)
- ~~Document that `ip=hb2f` is load-bearing and the gz must be regenerated after HTML edits.~~ **Done** — `CLAUDE.md` updated; `firmware/tools/gen_index_gz.py` carries the regen note. (Owner: Gapyss.)
- **Add a large-response load smoke check** before flashing dashboard changes: fetch `/` N times on hardware and assert full `Content-Length`. No CI exists for this repo; at minimum a documented manual gate. Folds into the same "serve under load / OTA succeeds" gate the PWM post-mortem already asked for. (Owner: Gapyss.)
- ~~**Watch dashboard page growth.**~~ **Done** — `gen_index_gz.py` now hard-fails the build (`MAX_GZ_BYTES` = 5120, under the ~5840 B `hb2f` one-buffer ceiling) if the gzipped page grows past the reliable-send threshold, so a regression of this bug can't ship silently. (Owner: Gapyss.)

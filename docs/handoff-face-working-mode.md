# Handoff — Companion face mode + desk-coding "working" state (v0.2.0)

**Shipped:** commit `d66bf67` on `main`, tagged `v0.2.0`.
**Device flashed:** yes — OTA pushed to `clawdmeter.local` during the session
(`Update OK. Rebooting...`). Rebuild the binary before re-flashing (see below).

## What this is

`face` mode is the Claude block-mascot companion screen. It has two states:

- **idle** — the original 20×20 pixel creature: blinks, glances left/right/up,
  and recolors its mood label from live usage (`HAPPY → FOCUS → ALERT`, plus a
  `SLEEPY` look at night). Body stays the Claude clay color regardless.
- **working** — the same mascot wearing headphones, typing at a desk on a laptop:
  alternating-hand typing, a blink, a "thinking" pause with eyes glanced up and a
  cursor blinking on the laptop screen, then back to typing. Footer mood reads
  `CODING`.

You switch between them from the web dashboard (**Companion → Idle / Working**
buttons) or over HTTP. There is **no physical button** on this hardware — the
panel is non-touch — so "tap" means the dashboard button / HTTP call.

## How to use

```sh
# toggle / set state (also switches the LCD into face mode if it isn't already)
curl "http://clawdmeter.local/face?state=working"
curl "http://clawdmeter.local/face?state=idle"
curl "http://clawdmeter.local/face?state=toggle"

# state is reported back in usage.json as "face": "idle" | "working"
curl -s http://clawdmeter.local/usage.json | tr ',' '\n' | grep face
```

Or open `http://clawdmeter.local/`, cycle the mode button to **Face**, and tap
**Idle** / **Working** in the Companion control.

State is **runtime-only** (not persisted to EEPROM) and defaults to **idle** on
boot — consistent with `lcdScreen`, which also isn't persisted.

## How it's built (so the next change doesn't fight it)

All in `firmware/clawdmeter_esp8266/clawdmeter_esp8266.ino`, face section:

- **One renderer, two scenes.** The working state reuses the existing
  `faceBuildGrid` / `faceRender` engine. It adds:
  - `deskBase[F_N][F_N] PROGMEM` — the desk-coding base grid (the user's
    `work_coding` pixel art, compacted into the face animation window:
    rows `FACE_R0..FACE_R1` = 3..17, cols `FACE_C0..FACE_C1` = 2..18; content
    actually lives in rows 3..16 / cols 2..17). Cells outside that window are
    silently dropped by `faceRender`, so keep desk art inside it.
  - `WORK_FRAMES[]` — a patch-based frame list with **`dr=dc=0` (no base shift)**
    so the desk stays put while only hands / eyes / cursor move. (The idle list
    `FACE_FRAMES` *does* shift the whole base — that's why working frames must
    not.)
  - An extended `faceCellColor` palette: cell values `3..9` map to fixed
    headphone / laptop / desk RGB565 colors (`C_HP_LIGHT`, `C_SCREEN`, …);
    value `1` is still the mood body color, `2`/empty render black.
- **Scene selection:** `faceWorking` (global bool, declared next to `lcdScreen`),
  with helpers `faceFrameList()` / `faceFrameCount()` / `faceActiveBase()`.
- **Toggle handler:** `handleFace()` (defined after `drawMeter()`/`faceBegin()`
  so no forward prototype is needed) sets `faceWorking`, forces face mode, and
  calls `faceBegin()` for a clean restart in the new scene.
- **IRAM:** the new grid + frames are all PROGMEM, and the render path adds no
  ISR code, so instruction RAM stays at **69%** — the documented baseline. No
  regression.

### Gotcha already paid for

`FacePatch` and `FaceFrame` structs were moved up next to `FaceExpr` at the top
of the file. Reason: `faceFrameList()` returns `const FaceFrame *`, and Arduino
auto-generates that prototype at the top of the file — the struct must be visible
there. (`faceBuildGrid` escaped this only because its 2D-array parameter makes
ctags skip prototype generation.) Don't move them back down.

## Editing the art

The working scene is a C array literal — hard to eyeball as a picture. A
throwaway pure-stdlib PNG renderer (`/tmp/render_desk.py` during the session,
not committed) mirrors `deskBase` + the palette and writes a PNG you can view.
If you change `deskBase`, re-render and look before flashing — a clean compile
does **not** prove the art reads correctly. (No PIL on this machine; the script
hand-rolls PNG via `zlib`.)

## Build / flash / verify

```sh
# compile (exact FQBN matters — mmu + ip options are load-bearing)
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f \
  firmware/clawdmeter_esp8266

# if you edit the dashboard HTML (INDEX_HTML), regenerate the gzip header first.
# it also guards size: fails if the gzipped page exceeds MAX_GZ_BYTES (5120 B).
python3 firmware/tools/gen_index_gz.py        # current: 4826 B

# produce the OTA binary
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:mmu=4816,ip=hb2f \
  --output-dir firmware/bin firmware/clawdmeter_esp8266

# flash OTA
curl -F "image=@firmware/bin/clawdmeter_esp8266.ino.bin" \
     http://clawdmeter.local/update
# device back in ~8 s; confirm with: curl -s http://clawdmeter.local/usage.json
# (check "up" reset to a low value, and "face" reflects the state)
```

## Possible follow-ups (not done)

- **Auto idle↔working.** The daemon already polls usage every 60 s; it could push
  a "working" flag when tokens are actively changing and the device would flip
  automatically — no manual tap. Small daemon + `/usage` change.
- **Head-bob in the working scene.** Dropped for v0.2.0 (the idle engine's
  whole-grid shift doesn't suit the desk scene, which must keep the desk fixed).
  Would need a partial-row shift or a few extra patches.
- **Persist `faceWorking`** in EEPROM if you want it to survive reboots (mirror
  the brightness marker pattern; remember `backlightStopForFlash()` around the
  commit).

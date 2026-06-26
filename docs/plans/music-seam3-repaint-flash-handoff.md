# Handoff: Music Screen — Seam 3 (Repaint Flash)

**Scope:** the "no flash on `/nowplaying` push" guarantee for the physical MUSIC
screen. One of four "seamless" seams; this doc covers **Seam 3 only**.
(Seam 1 = track-skip latency, Seam 2 = position drift/resync, Seam 4 = lyric
timing — not in scope here.)

**Status: implemented in WIP, NOT yet hardware-verified.** The two-phase paint is
coded on branch `fix/music-scroll-centering-and-wrap` (uncommitted). Remaining
work is verification + one fallback-path check, not new design.

---

## What Seam 3 is

A full `fillScreen` on every 60 s / per-push repaint flashes the panel and kills
the glance-all-day feel (same problem solved earlier on the MAC and DESK
screens). For MUSIC, "seamless" means: a track change, pause, position resync, or
clock tick repaints **only the band that changed**, with an opaque background
fill — never a full-body clear. The only allowed `fillScreen` is the one-time
switch-in chrome paint.

## How it's implemented (code anchors — `clawdmeter_esp8266.ino`)

Two-phase paint, gated by the file-scope flag `musicChromeReady` (line ~151),
cleared to `false` whenever the screen is forced to MUSIC (line ~671).

- **`drawMusicChrome()` (line ~2574)** — the *only* `fillScreen(C_MUS_PAPER)`.
  Runs once on switch-in. Draws header + art, primes the lyric/time "dirty"
  sentinels (`musicLyricShown1/2 = "\x01"`, `musicTimeLastMs = 0`) so the dynamic
  bands repaint cleanly right after the clear.
- **`drawMusic()` (line ~2584)** — called on identity/pause change. If chrome is
  already up it does **not** clear; it refreshes only the header (play/pause
  eyebrow + clock) and the title/artist/footer/lyric bands via in-place opaque
  repaints.
- **`handleNowPlaying()` (line ~712)** — gates which path runs:
  - identity OR pause change → `drawMusic()` (band repaints, no clear)
  - otherwise (pos/lyric resync) → `musicDrawFooter()` + conditional
    `musicDrawLyrics()` only.
- **`musicTick()` (line ~2597)** — per-loop millis() poll: time/progress once a
  second, header only on minute rollover, synced-lyric promote only on change,
  title/artist/lyric marquees redraw their own band. No ISR, no full clear.

Each dynamic band clears itself with a band-scoped `fillRect(..., C_MUS_PAPER)`
or a canvas blit — e.g. header `fillRect(0,0,240,22,...)` (~2418), time
`fillRect(0, MUSIC_TIME_Y-1, 240, 10, ...)` (~2430), lyric band
`fillRect(0, MUSIC_LYRIC_Y, 240, MUSIC_LYRIC_H, ...)` (~2548).

## Remaining work (the actual handoff)

1. **Hardware verification.** The redesign is uncommitted and has not been
   flashed. Confirm visually on the device that none of the events below flash:
   track skip, pause/resume, 30 s position resync, minute rollover, lyric line
   change, marquee scroll.

2. **Canvas-allocation fallback flash-check.** Title/artist marquees use
   heap-allocated `Arduino_Canvas_Indexed` (`musicTitleCanvas` /
   `musicArtistCanvas`, with `*CanvasOk` flags). When allocation fails the code
   falls back to drawing straight to `gfx`. Confirm the fallback path
   (`musicDrawTitle` ~2315 / `musicDrawArtist` ~2338) clears only its **band**
   (`fillRect(MUSIC_META_X, MUSIC_TITLE_BAND_Y, MUSIC_META_W, MUSIC_TITLE_BAND_H,
   C_MUS_PAPER)`) and never a full screen. It currently looks band-scoped — verify
   under a forced-failure (low-heap) condition, not just the happy path.

3. **Build + gzip guard.** After any chrome/label change, rebuild with the
   documented FQBN (`...:mmu=4816,ip=hb2f`) and keep the dashboard under
   `MAX_GZ_BYTES` (run `firmware/tools/gen_index_gz.py`).

## Test checklist

- [ ] Switch into MUSIC → single clean paint (one allowed flash on switch-in)
- [ ] Track skip → title/artist/art repaint, **no** full-screen flash
- [ ] Pause then resume → eyebrow + progress update, no flash
- [ ] Let a song run 60 s+ → minute rollover repaints clock only
- [ ] Position resync push (every ~`NOWPLAYING_RESYNC`s) → footer only, no flash
- [ ] Synced lyric line change → lyric band only
- [ ] Long Thai + long Latin title/artist marquee → smooth, band-local
- [ ] Force low-heap / canvas alloc failure → fallback path still band-scoped

## Guardrail (do not regress)

Never reintroduce a `fillScreen` or full-body `fillRect` into the dynamic path
(`drawMusic` when chrome is ready, `musicTick`, `musicDraw*` band functions, or
the `handleNowPlaying` non-identity branch). The single `fillScreen` lives in
`drawMusicChrome()` and must stay there. Same discipline as MAC (`drawMacDynamic`)
and DESK (canvas text region) — see CLAUDE.md.

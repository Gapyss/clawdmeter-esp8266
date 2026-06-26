#!/usr/bin/env python3
"""Insert hand-written lyrics into the daemon's on-demand lyric cache.

Use this for songs lrclib.net doesn't have (or you want to override). The entry
is keyed *identically* to how the running daemon keys a now-playing song — it
reuses the daemon's own `lyric_key`/`lyric_cache_put` — so it will be served
offline on the next play with no network lookup.

The key includes the track duration in whole seconds (the daemon reads it as
floor(YouTube Music progress-bar length)). Pass the length shown in YT Music as
either seconds or M:SS. If your entry doesn't show up, the real length is off by
a second or two — re-run with --duration adjusted by +/-1.

Examples:
  # synced .lrc file (auto-detected by [mm:ss.xx] timestamps)
  add_lyrics.py --title "ลาลาลอย" --artist "ศิลปินไทย" --duration 3:35 --lrc song.lrc

  # plain lyrics from stdin
  pbpaste | add_lyrics.py --title "Some Song" --artist "Band" --duration 214

  # mark a track instrumental (no lyrics shown)
  add_lyrics.py --title "Interlude" --artist "X" --duration 92 --instrumental

  # list everything currently cached
  add_lyrics.py --list

The cache file is $CLAWDMETER_LYRIC_CACHE (default ~/.clawdmeter/lyrics.sqlite3),
the same one the daemon reads — set that env var if you customized it.
"""
import argparse
import importlib.util
import os
import re
import sys
from pathlib import Path

DAEMON = Path(__file__).resolve().parent.parent / "claudemeter_daemon.py"


def load_daemon():
    spec = importlib.util.spec_from_file_location("claudemeter_daemon", DAEMON)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # safe: daemon's main() is under __main__ guard
    return mod


def parse_duration(s):
    s = s.strip()
    if ":" in s:
        mins, secs = s.split(":", 1)
        return int(mins) * 60 + int(float(secs))
    return int(float(s))


def read_lyrics(args):
    if args.lrc:
        return Path(args.lrc).read_text(encoding="utf-8")
    if args.plain:
        return Path(args.plain).read_text(encoding="utf-8")
    if args.text is not None:
        return args.text
    if not sys.stdin.isatty():
        return sys.stdin.read()
    return ""


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--title")
    p.add_argument("--artist", default="")
    p.add_argument("--duration", default="0",
                   help="track length in seconds or M:SS (must match YT Music)")
    src = p.add_mutually_exclusive_group()
    src.add_argument("--lrc", help="path to a synced .lrc file")
    src.add_argument("--plain", help="path to a plain-text lyrics file")
    src.add_argument("--text", help="lyrics as a literal string")
    kind = p.add_mutually_exclusive_group()
    kind.add_argument("--synced", action="store_true", help="force synced (LRC) storage")
    kind.add_argument("--plain-lyrics", action="store_true", dest="force_plain",
                      help="force plain storage even if timestamps are present")
    p.add_argument("--instrumental", action="store_true",
                   help="store as instrumental (no lyric lines)")
    p.add_argument("--list", action="store_true", help="list cached entries and exit")
    args = p.parse_args()

    dm = load_daemon()

    if not dm.lyric_cache_db():
        sys.exit(f"lyric cache unavailable at {dm.LYRIC_CACHE_PATH!r} "
                 "(set CLAWDMETER_LYRIC_CACHE)")

    if args.list:
        rows = dm.lyric_cache_db().execute(
            "SELECT track, artist, duration, "
            "  CASE WHEN instrumental THEN 'instrumental' "
            "       WHEN synced_lyrics IS NOT NULL THEN 'synced' "
            "       WHEN plain_lyrics IS NOT NULL THEN 'plain' ELSE '-' END "
            "FROM lyrics_cache ORDER BY track").fetchall()
        for track, artist, dur, kindname in rows:
            print(f"  [{kindname:>12}] {track} — {artist}  ({int(dur or 0)}s)")
        print(f"{len(rows)} entr{'y' if len(rows) == 1 else 'ies'} in {dm.LYRIC_CACHE_PATH}")
        return

    if not args.title:
        p.error("--title is required (or use --list)")

    dur = parse_duration(args.duration)
    synced = plain = None
    if not args.instrumental:
        text = read_lyrics(args).strip("\n")
        if not text:
            p.error("no lyrics given — pass --lrc/--plain/--text or pipe via stdin "
                    "(or use --instrumental)")
        has_ts = bool(re.search(r"^\s*\[\d{1,2}:\d{2}", text, re.M))
        if args.synced or (has_ts and not args.force_plain):
            synced = text
        else:
            plain = text

    key = dm.lyric_key(args.title, args.artist, dur)
    if key is None:
        p.error("--title produced an empty key")
    payload = {
        "trackName": args.title,
        "artistName": args.artist,
        "duration": float(dur),
        "syncedLyrics": synced,
        "plainLyrics": plain,
        "instrumental": bool(args.instrumental),
    }
    dm.lyric_cache_put(key, payload)

    # Read it back through the daemon's own lookup to prove the key matches.
    got = dm.lyric_cache_get(key)
    kindname = ("instrumental" if got and got["instrumental"]
                else "synced" if got and got["synced"]
                else "plain" if got and got["plain"] else "EMPTY")
    nlines = len(got["synced"] or got["plain"]) if got else 0
    print(f"stored: {args.title} — {args.artist or '(no artist)'} @ {dur}s  "
          f"[{kindname}{', ' + str(nlines) + ' lines' if nlines else ''}]")
    print(f"cache:  {dm.LYRIC_CACHE_PATH}")
    print("It will be served on the next play of this exact title/artist/length.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Clawdmeter daemon (macOS).

Pushes two usage percentages to an ESP8266 web dashboard over HTTP.

By default it uses Anthropic response headers, which are the closest match for
Claude's real server-side usage limits. Set CLAWDMETER_USAGE_SOURCE=local to use
Claude Code's local JSONL transcripts instead.

Runs on system Python 3, no pip installs.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime
from pathlib import Path

# ---- Edit if needed ----
DEVICE_URL = os.environ.get("CLAWDMETER_DEVICE_URL", "http://clawdmeter.local")
POLL_INTERVAL = 60                       # seconds
RAW_USAGE_SOURCE = os.environ.get("CLAWDMETER_USAGE_SOURCE", "api").lower()
USAGE_SOURCE = {"server": "api", "headers": "api"}.get(RAW_USAGE_SOURCE, RAW_USAGE_SOURCE)
RATE_LIMIT_BACKOFF = 10 * 60             # seconds when API does not send Retry-After
DEVICE_TIMEOUT = float(os.environ.get("CLAWDMETER_DEVICE_TIMEOUT", "5"))
DEVICE_PUSH_ATTEMPTS = int(os.environ.get("CLAWDMETER_DEVICE_PUSH_ATTEMPTS", "3"))

# Local-log mode cannot know Anthropic's real server-side rate limit. Set these
# to the token budgets you want the ESP progress bars to represent.
SESSION_TOKEN_LIMIT = int(os.environ.get("CLAWDMETER_SESSION_TOKEN_LIMIT", "30000000"))
WEEKLY_TOKEN_LIMIT = int(os.environ.get("CLAWDMETER_WEEKLY_TOKEN_LIMIT", "100000000"))
# ------------------------

KEYCHAIN_SERVICE = "Claude Code-credentials"
API_URL = "https://api.anthropic.com/v1/messages"
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

PROJECT_DIRS = (
    Path.home() / ".claude" / "projects",
    Path.home() / "Library" / "Developer" / "Xcode" /
    "CodingAssistant" / "ClaudeAgentConfig" / "projects",
)


def parse_timestamp(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)
    except ValueError:
        return None


def turn_tokens(record):
    """Return (timestamp, token_count, message_id) for assistant usage records."""
    if record.get("type") != "assistant":
        return None

    msg = record.get("message") or {}
    usage = msg.get("usage") or {}
    tokens = (
        (usage.get("input_tokens") or 0) +
        (usage.get("output_tokens") or 0) +
        (usage.get("cache_read_input_tokens") or 0) +
        (usage.get("cache_creation_input_tokens") or 0)
    )
    if tokens <= 0:
        return None

    ts = parse_timestamp(record.get("timestamp"))
    if ts is None:
        return None

    return ts, tokens, msg.get("id") or ""


def get_token():
    """Read the Claude Code OAuth access token from the macOS Keychain."""
    out = subprocess.check_output(
        ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
        text=True,
    ).strip()
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return out
    stack = [data]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            tok = node.get("accessToken")
            if isinstance(tok, str):
                return tok
            stack.extend(node.values())
        elif isinstance(node, list):
            stack.extend(node)
    raise RuntimeError("accessToken not found in Keychain item")


def pct(value):
    """Header utilization is a 0..1 fraction; convert to an integer percent."""
    try:
        return round(float(value) * 100)
    except (TypeError, ValueError):
        return -1


def usage_from_headers(headers):
    return (
        pct(headers.get("anthropic-ratelimit-unified-5h-utilization")),
        pct(headers.get("anthropic-ratelimit-unified-7d-utilization")),
    )


def int_or_zero(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


# representative-claim tells which limit is the binding one; the device draws an
# amber stripe on that block. 1 = session (5h), 2 = weekly (7d), 0 = unknown.
_BIND = {"five_hour": 1, "seven_day": 2}


def extra_from_headers(headers):
    """Reset epochs, allow/deny status, binding limit, and server clock.

    These ride along with the two utilization headers and let the device show
    real reset times + a UTC clock without an RTC or NTP.
    """
    server_epoch = 0
    date_hdr = headers.get("Date")
    if date_hdr:
        try:
            server_epoch = int(parsedate_to_datetime(date_hdr).timestamp())
        except (TypeError, ValueError):
            server_epoch = 0
    return {
        "sr": int_or_zero(headers.get("anthropic-ratelimit-unified-5h-reset")),
        "wr": int_or_zero(headers.get("anthropic-ratelimit-unified-7d-reset")),
        "stat": (headers.get("anthropic-ratelimit-unified-status") or "").strip(),
        "bind": _BIND.get(
            (headers.get("anthropic-ratelimit-unified-representative-claim") or "").strip(), 0),
        "t": server_epoch,
    }


def retry_after_seconds(headers):
    try:
        return max(1, int(headers.get("retry-after", "")))
    except ValueError:
        return RATE_LIMIT_BACKOFF


def run_text(cmd):
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return ""


def bounded_pct(value):
    try:
        return max(0, min(100, round(float(value))))
    except (TypeError, ValueError):
        return -1


def cpu_percent():
    out = run_text(["ps", "-A", "-o", "%cpu="])
    total = 0.0
    for line in out.splitlines():
        try:
            total += float(line.strip())
        except ValueError:
            pass
    cores = os.cpu_count() or 1
    return bounded_pct(total / cores)


def memory_percent():
    vm = run_text(["vm_stat"])

    pages = {}
    for line in vm.splitlines():
        m = re.match(r"Pages ([^:]+):\s+([0-9.]+)", line)
        if m:
            pages[m.group(1)] = int(m.group(2).replace(".", ""))

    free = pages.get("free", 0) + pages.get("speculative", 0)
    used = (
        pages.get("active", 0) +
        pages.get("wired down", 0) +
        pages.get("occupied by compressor", 0)
    )
    total = free + used + pages.get("inactive", 0)
    if total <= 0:
        return -1
    return bounded_pct(used * 100 / total)


def disk_percent():
    try:
        usage = shutil.disk_usage(str(Path.home()))
    except OSError:
        return -1
    return bounded_pct(usage.used * 100 / usage.total)


def battery_percent():
    out = run_text(["pmset", "-g", "batt"])
    m = re.search(r"(\d+)%", out)
    return bounded_pct(m.group(1)) if m else -1


def mac_metrics():
    return {
        "cpu": cpu_percent(),
        "mem": memory_percent(),
        "disk": disk_percent(),
        "bat": battery_percent(),
    }


def base_result(**kw):
    """A push payload with every field defaulted; pollers fill what they have."""
    r = {"s": -1, "w": -1, "st": 0, "wt": 0,
         "sr": 0, "wr": 0, "stat": "", "bind": 0, "t": 0,
         "cpu": -1, "mem": -1, "disk": -1, "bat": -1,
         "sleep": POLL_INTERVAL}
    r.update(kw)
    return r


def poll_api_usage():
    """Make a 1-token request and read real server-side quota usage headers."""
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(API_BODY).encode(),
        headers={
            "content-type": "application/json",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "Authorization": f"Bearer {get_token()}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            s, w = usage_from_headers(resp.headers)
            return base_result(s=s, w=w, **extra_from_headers(resp.headers))
    except (TimeoutError, socket.timeout) as e:
        raise TimeoutError("Anthropic API poll timed out after 30s") from e
    except urllib.error.URLError as e:
        if isinstance(e.reason, socket.timeout):
            raise TimeoutError("Anthropic API poll timed out after 30s") from e
        raise


def scan_local_usage():
    """Read Claude Code JSONL transcripts and return token totals for 5h and 7d."""
    now = datetime.now(timezone.utc)
    session_start = now - timedelta(hours=5)
    weekly_start = now - timedelta(days=7)
    latest_by_message = {}
    turns_without_id = []

    for base in PROJECT_DIRS:
        if not base.exists():
            continue
        for path in base.rglob("*.jsonl"):
            try:
                with path.open(encoding="utf-8", errors="replace") as f:
                    for line in f:
                        try:
                            record = json.loads(line)
                        except json.JSONDecodeError:
                            continue

                        parsed = turn_tokens(record)
                        if not parsed:
                            continue

                        ts, tokens, message_id = parsed
                        if ts < weekly_start:
                            continue

                        # Claude Code can write multiple streaming records for
                        # one message; keep the latest tally for that message.
                        if message_id:
                            latest_by_message[message_id] = (ts, tokens)
                        else:
                            turns_without_id.append((ts, tokens))
            except OSError as e:
                print(f"warning: cannot read {path}: {e}", file=sys.stderr)

    turns = list(latest_by_message.values()) + turns_without_id
    session_tokens = sum(tokens for ts, tokens in turns if ts >= session_start)
    weekly_tokens = sum(tokens for _ts, tokens in turns)
    return session_tokens, weekly_tokens


def percent(tokens, limit):
    if limit <= 0:
        return -1
    return min(100, round(tokens * 100 / limit))


def poll_local_usage():
    session_tokens, weekly_tokens = scan_local_usage()
    return base_result(
        s=percent(session_tokens, SESSION_TOKEN_LIMIT),
        w=percent(weekly_tokens, WEEKLY_TOKEN_LIMIT),
        st=session_tokens,
        wt=weekly_tokens,
    )


def poll_usage():
    if USAGE_SOURCE == "local":
        return poll_local_usage()
    if USAGE_SOURCE != "api":
        raise RuntimeError("CLAWDMETER_USAGE_SOURCE must be 'api', 'server', 'headers', or 'local'")
    return poll_api_usage()


def push(r):
    url = (f"{DEVICE_URL}/usage?s={r['s']}&w={r['w']}&st={r['st']}&wt={r['wt']}"
           f"&sr={r['sr']}&wr={r['wr']}&stat={urllib.parse.quote(r['stat'])}"
           f"&bind={r['bind']}&t={r['t']}"
           f"&cpu={r['cpu']}&mem={r['mem']}&disk={r['disk']}&bat={r['bat']}")
    last_error = None
    attempts = max(1, DEVICE_PUSH_ATTEMPTS)
    for attempt in range(1, attempts + 1):
        try:
            urllib.request.urlopen(
                urllib.request.Request(url, method="POST"),
                timeout=DEVICE_TIMEOUT,
            ).read()
            return
        except urllib.error.HTTPError:
            raise
        except (TimeoutError, socket.timeout) as e:
            last_error = e
        except urllib.error.URLError as e:
            last_error = e
            if not isinstance(e.reason, socket.timeout) and attempt == attempts:
                raise

        if attempt < attempts:
            time.sleep(min(2.0, 0.5 * attempt))

    if isinstance(last_error, (TimeoutError, socket.timeout)):
        raise TimeoutError(
            f"device push timed out after {DEVICE_TIMEOUT:g}s "
            f"({attempts} attempts): {DEVICE_URL}/usage"
        ) from last_error
    if isinstance(last_error, urllib.error.URLError):
        if isinstance(last_error.reason, socket.timeout):
            raise TimeoutError(
                f"device push timed out after {DEVICE_TIMEOUT:g}s "
                f"({attempts} attempts): {DEVICE_URL}/usage"
            ) from last_error
        raise last_error


def main():
    print(f"Clawdmeter daemon -> {DEVICE_URL}, source={USAGE_SOURCE}, polling every {POLL_INTERVAL}s")
    while True:
        sleep_for = POLL_INTERVAL
        try:
            r = poll_usage()
            r.update(mac_metrics())
            sleep_for = r["sleep"]
            push(r)
            if USAGE_SOURCE == "local":
                print(f"session={r['s']}% ({r['st']} tok)  weekly={r['w']}% ({r['wt']} tok)  "
                      f"cpu={r['cpu']}% mem={r['mem']}%")
            else:
                print(f"session={r['s']}%  weekly={r['w']}%  status={r['stat'] or '?'}  "
                      f"cpu={r['cpu']}% mem={r['mem']}%")
        except urllib.error.HTTPError as e:
            if e.code == 401:
                print("401 Unauthorized: run any Claude Code command to refresh login.",
                      file=sys.stderr)
            elif e.code == 429:
                s, w = usage_from_headers(e.headers)
                sleep_for = retry_after_seconds(e.headers)
                if s >= 0 or w >= 0:
                    r = base_result(s=s, w=w, **extra_from_headers(e.headers))
                    r.update(mac_metrics())
                    try:
                        push(r)
                    except Exception as push_error:
                        print(f"device push failed: {push_error}", file=sys.stderr)
                    print(f"rate limited: session={s}% weekly={w}% "
                          f"(retrying in {sleep_for}s)", file=sys.stderr)
                else:
                    print(f"rate limited by Anthropic API "
                          f"(retrying in {sleep_for}s)", file=sys.stderr)
            else:
                print(f"API error {e.code}: {e.reason}", file=sys.stderr)
        except TimeoutError as e:
            print(f"timeout: {e}", file=sys.stderr)
        except Exception as e:
            print(f"error: {e}", file=sys.stderr)
        time.sleep(sleep_for)


if __name__ == "__main__":
    main()

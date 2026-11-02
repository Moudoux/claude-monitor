"""Shows your Claude 5-hour and weekly usage limits on the Claude Monitor display.

    python3 monitor/cli.py claude_usage                 # update once
    python3 monitor/cli.py claude_usage --schedule 60   # update every 60 s until Ctrl+C
    python3 monitor/cli.py claude_usage --display http://192.168.1.51 --schedule 120

Usage comes from running `claude -p /usage`, which prints what the /usage screen shows without calling
the model, so it costs no tokens. That text has no fixed format; if it changes, the screen says so.
"""

import json
import re
import shutil
import subprocess
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

# cron and systemd timers don't have ~/.local/bin on their PATH
CLAUDE = shutil.which("claude") or str(Path.home() / ".local" / "bin" / "claude")
USAGE_COMMAND = [CLAUDE, "-p", "/usage", "--output-format", "json", "--no-session-persistence"]

TITLE = "Claude usage"
CLAUDE_ORANGE = "#D97757"


def fetch_usage():
    """The text of `claude -p /usage`."""
    done = subprocess.run(USAGE_COMMAND, capture_output=True, text=True, timeout=60)
    try:
        output = json.loads(done.stdout)
    except ValueError:
        raise RuntimeError(f"`claude -p /usage` failed:\n{done.stderr.strip() or done.stdout.strip()}") from None
    if output.get("is_error"):
        raise RuntimeError(f"`claude -p /usage` failed:\n{output.get('result')}")
    return output["result"]


def parse_limit(text, name):
    """Finds e.g. 'Current session: 5% used · resets Nov 2, 10:49pm (Europe/Stockholm)'."""
    match = re.search(rf"{re.escape(name)}:\s*(\d+)% used(?: · resets (.+?) \((.+?)\))?", text)
    if not match:
        raise RuntimeError(f"Couldn't find '{name}' in:\n{text}")
    return {"utilization": int(match[1]), "resets": match[2], "zone": match[3]}


def parse_reset(resets, zone):
    """Turns 'Nov 2, 10:49pm' or 'Nov 3, 9am' into a datetime, or None if it looks different."""
    now = datetime.now(ZoneInfo(zone))
    for layout in ("%Y %b %d, %I:%M%p", "%Y %b %d, %I%p"):
        try:
            when = datetime.strptime(f"{now.year} {resets}", layout).replace(tzinfo=now.tzinfo)
        except ValueError:
            continue
        # no year in the text: a date well behind us is next January's
        return when.replace(year=now.year + 1) if when < now - timedelta(days=1) else when
    return None


def describe_reset(limit):
    """'resets 21:49 (in 3h 52m)', or with the weekday when it isn't today."""
    if not limit["resets"]:
        return ""
    when = parse_reset(limit["resets"], limit["zone"])
    if not when:
        return f"resets {limit['resets']}"
    when = when.astimezone()
    now = datetime.now().astimezone()
    minutes = max(0, int((when - now).total_seconds() // 60))
    days, hours, mins = minutes // 1440, minutes // 60 % 24, minutes % 60
    left = f"{days}d {hours}h" if days else f"{hours}h {mins}m"
    clock = when.strftime("%H:%M" if when.date() == now.date() else "%a %H:%M")
    return f"resets {clock} (in {left})"


def limit_lines(label, limit):
    """Lines for one limit: a bar with its name and percentage, then when it resets."""
    return [
        {"bar": limit["utilization"], "label": label, "value": f"{round(limit['utilization'])}%", "height": 30},
        {"text": describe_reset(limit), "size": 1, "color": "grey"},
        "",
    ]


def build_screen(args):
    usage = fetch_usage()
    return {
        "title": TITLE,
        "color": CLAUDE_ORANGE,
        "lines": limit_lines("5-hour", parse_limit(usage, "Current session"))
                 + limit_lines("Weekly", parse_limit(usage, "Current week (all models)"))
                 + [{"text": "updated " + datetime.now().strftime("%H:%M"), "size": 1, "color": "grey"}],
    }

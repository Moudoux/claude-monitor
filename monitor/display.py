"""Shared helpers for the screens: fetching JSON, posting to the display, and the update loop.

A screen builds a JSON document for the board (see `GET /` on the board for the format), and cli.py hands
the function that builds it to `run()`. `add_options()` gives every screen the --display/--schedule options.
"""

import json
import sys
import time
import urllib.parse
import urllib.request

import mdns

DEFAULT_DISPLAY = "http://claude-monitor.local"


def get_json(url, headers=None):
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=15) as response:
        return json.load(response)


def resolve_local(url):
    """`url` with a .local host replaced by its address; if nothing answers, the system gets to try the name."""
    parts = urllib.parse.urlsplit(url)
    address = parts.hostname and parts.hostname.lower().endswith(".local") and mdns.resolve(parts.hostname)
    if not address:
        return url
    return parts._replace(netloc=f"{address}:{parts.port}" if parts.port else address).geturl()


def post(display, screen):
    request = urllib.request.Request(
        resolve_local(display).rstrip("/") + "/display",
        data=json.dumps(screen).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return response.read().decode().strip()


def error_screen(title, message):
    return {"title": title, "color": "red", "text": message}


def seconds(text):
    value = int(text)
    if value <= 0:
        raise ValueError
    return value


def add_options(parser):
    """Adds the options every screen has to `parser`."""
    parser.add_argument("--display", default=DEFAULT_DISPLAY, help="the board's address (default %(default)s)")
    parser.add_argument("--schedule", type=seconds, metavar="SECONDS",
                        help="keep updating every SECONDS until Ctrl+C (default: update once and exit)")


def run(build_screen, args, title):
    """Calls `build_screen(args)` and posts the result, once or every `args.schedule` seconds.

    If building the screen fails, the error is shown on the display under `title` instead.
    With a schedule it keeps trying after an error; otherwise it returns the exit code, 1 if anything failed.
    """
    while True:
        failed = False
        try:
            screen = build_screen(args)
        except Exception as e:
            failed = True
            print(time.strftime("%H:%M:%S"), f"couldn't build the screen: {e}", file=sys.stderr)
            screen = error_screen(title, str(e))
        try:
            print(time.strftime("%H:%M:%S"), post(args.display, screen))
        except OSError as e:  # the display is unreachable
            failed = True
            print(time.strftime("%H:%M:%S"), f"couldn't reach the display: {e}", file=sys.stderr)
        if not args.schedule:
            return 1 if failed else 0
        time.sleep(args.schedule)

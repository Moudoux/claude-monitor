#!/usr/bin/env python3
"""Puts screens on the Claude Monitor display.

    python3 monitor/cli.py list                     # the screens there are
    python3 monitor/cli.py help weather             # what a screen does, and all its options
    python3 monitor/cli.py weather Amsterdam        # run a screen once, with its options
    python3 monitor/cli.py weather Amsterdam --schedule 600   # run it every 10 minutes until Ctrl+C

To add a screen, add a module to monitor/screens/ with a docstring (its first line is the summary here),
TITLE and build_screen(args), which returns the JSON document for the board. It can also have
add_arguments(parser) for its own options, and check_args(parser, args) to reject bad combinations of them.
"""

import argparse
import importlib
import pkgutil
import sys
from pathlib import Path

from display import add_options, run

SCREENS_DIR = Path(__file__).parent / "screens"


def load_screens():
    """The screen modules in monitor/screens/, by name."""
    names = sorted(module.name for module in pkgutil.iter_modules([str(SCREENS_DIR)]))
    return {name: importlib.import_module(f"screens.{name}") for name in names}


def summary(screen):
    return screen.__doc__.strip().splitlines()[0]


def new_parser(screens):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", metavar="<command>", title="commands")
    commands.add_parser("list", help="list the screens")
    help_command = commands.add_parser("help", help="show help for this tool, or for one screen")
    help_command.add_argument("screen", nargs="?", choices=sorted(screens), help="the screen to show help for")
    for name, screen in screens.items():
        screen_parser = commands.add_parser(
            name,
            help=summary(screen).replace("%", "%%"),  # argparse formats help strings
            description=screen.__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        add_options(screen_parser)
        if hasattr(screen, "add_arguments"):
            screen.add_arguments(screen_parser)
    return parser, commands.choices


def main():
    screens = load_screens()
    parser, commands = new_parser(screens)
    args = parser.parse_args()
    if args.command == "list":
        width = max(map(len, screens), default=0)
        for name, screen in screens.items():
            print(f"{name:<{width}}  {summary(screen)}")
    elif args.command == "help" and args.screen:
        commands[args.screen].print_help()
    elif args.command in screens:
        screen = screens[args.command]
        if hasattr(screen, "check_args"):
            screen.check_args(commands[args.command], args)
        return run(screen.build_screen, args, screen.TITLE)
    else:
        parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

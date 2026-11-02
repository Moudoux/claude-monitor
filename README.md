# Claude Monitor

A small Wi-Fi status display: an ESP-12F NodeMCU drives a 2.4" 320x240 ILI9341 TFT (the screen from a JYE Tech DSO138 oscilloscope) over an 8-bit parallel bus. Python scripts on your computer post screens to it over HTTP.

Two screens are included:

- `claude_usage`: your Claude 5-hour and weekly usage limits, read from `claude -p /usage`, which costs no tokens
- `weather <city>`: current weather from [Open-Meteo](https://open-meteo.com), which needs no API key

## Layout

| Path | What it is |
|---|---|
| `src/main.cpp` | Firmware: Wi-Fi setup, HTTP server, screen drawing |
| `src/tft.h`, `src/tft.cpp` | Display driver (an `Adafruit_GFX` subclass) |
| `src/logo.h` | Boot logo, generated from `assets/apple-logo.png` |
| `monitor/cli.py` | Command-line tool that posts screens to the board |
| `monitor/screens/` | The screens, found automatically |
| `tools/` | Serial reader and logo converter |
| `ESP8266_TFT_NOTES.md` | Hardware details, pinout and full wiring |
| `CLAUDE.md` | Flashing and serial procedures, including troubleshooting |

## Hardware

Wire the screen as described under "Wiring plan" in `ESP8266_TFT_NOTES.md`.

**Don't wire the screen's RD line (header pin 10) to the NodeMCU's RX pin.** It belongs on 3.3V; on RX it makes uploads fail. See `CLAUDE.md`.

## Flash the firmware

This needs [PlatformIO](https://platformio.org). `platformio.ini` sets the upload port to `/dev/ttyUSB0`, so change it if your board shows up elsewhere. Close any serial monitor first, because it blocks the upload.

```sh
pio run -t upload
```

To regenerate the logo after changing the image:

```sh
python3 -I tools/logo2header.py assets/apple-logo.png src/logo.h
```

## Connect it to Wi-Fi

Wi-Fi credentials aren't in the source. They're stored in the board's flash, and normal uploads keep them.

1. If the board has no saved network or can't reach it, it opens a hotspot called `Claude-Monitor-Setup` and shows a random password on the screen.
2. Join the hotspot from a phone. The setup page usually opens by itself; otherwise browse to `192.168.4.1`.
3. Pick your network and enter its password. The screen then shows the network name and the board's IP address.

To forget the saved network, run `pio run -t erase`, then upload again.

## Show a screen

Run these with plain `python3`. The `-I` flag stops the tool importing `monitor/display.py`.

```sh
python3 monitor/cli.py list                               # list the screens
python3 monitor/cli.py help weather                       # show a screen's options
python3 monitor/cli.py weather Amsterdam                  # update once
python3 monitor/cli.py weather Amsterdam --schedule 600   # update every 10 minutes until Ctrl+C
python3 monitor/cli.py claude_usage --schedule 60
```

The tool looks for the board at `http://claude-monitor.local`. To use another address, pass `--display`, for example `--display http://192.168.1.51`.

The `claude_usage` screen needs the [Claude Code](https://claude.com/claude-code) CLI installed and signed in.

## Add a screen

Add a module to `monitor/screens/` with:

- a docstring, whose first line becomes the summary in `list`
- `TITLE`
- `build_screen(args)`, which returns the JSON document for the board
- optionally, `add_arguments(parser)` for its own options and `check_args(parser, args)` to reject bad combinations of them

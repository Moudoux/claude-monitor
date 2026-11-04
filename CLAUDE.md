# Claude Monitor

PlatformIO project: an ESP-12F NodeMCU (CP2102 USB-serial) driving a JYE Tech DSO138 2.4" 320x240 ILI9341 TFT over an 8-bit parallel bus. The hardware, the full wiring and its history are in `ESP8266_TFT_NOTES.md`; read it before changing the wiring or `src/tft.cpp`.

- `src/tft.h`, `src/tft.cpp`: display driver (Adafruit_GFX subclass, plus register read-back for diagnostics)
- `src/main.cpp`: the current application
- `tools/readserial.py`: resets the board and prints its serial output
- `monitor/cli.py`: posts a screen to the board over HTTP. `list` shows the screens, `help <screen>` or `<screen> --help` shows a screen's options, and `<screen> [options]` runs it once (`--schedule <seconds>` repeats it until Ctrl+C; `--display` sets the address, default `http://claude-monitor.local`, which `monitor/mdns.py` resolves without avahi or nss-mdns; the board's IP is shown on its screen). Run it with plain `python3`, not `-I`, which stops it importing the shared `monitor/display.py`.
- `monitor/screens/`: the screens, found automatically; the docstring in `cli.py` says what a screen module needs
  - `claude_usage`: the user's Claude 5-hour and weekly usage limits
  - `weather <city>`: current weather from Open-Meteo (no API key)
  - `voice`: hands-free chat with an animated face and short replies. `parecord` audio goes through a loudness-based speech detector, then local Whisper (`faster-whisper`, in the git-ignored `.venv`, which the screen restarts itself into), then one long-running `claude -p` in stream-json mode. It deliberately doesn't use Claude Code's `/voice`, which is push-to-talk in the interactive app only. Claude runs with `--restricted --strict-mcp-config --tools WebSearch` from an empty folder, because anyone in earshot can talk to it; keep it that way by default. `monitor/voice_context.txt` is the user-editable context loaded at startup; the rules for fitting the screen stay in `voice.py`.
- `src/logo.h`: boot screen logo, generated from `assets/apple-logo.png` by `python3 -I tools/logo2header.py assets/apple-logo.png src/logo.h`; regenerate it rather than editing it

## Flashing

This is the procedure that works reliably (about 7 s per upload):

1. **Make sure nothing else has the port open.** This is the usual cause of uploads that connect and then time out at "Uploading stub" or the first block: another reader swallows the bootloader's replies. Check with:
   ```
   fuser -v /dev/ttyUSB0; pgrep -af "device monitor|miniterm|esptool|readserial"
   ```
   Both must print nothing. Culprits include `pio device monitor` in the user's VS Code terminal, the PlatformIO extension's serial monitor, a `readserial.py` still running in the background, and another Claude session flashing or reading at the same time. Don't kill the user's monitor yourself: ask them to close it.
2. **Upload:**
   ```
   cd "/home/user/Documents/Code/Claude Monitor" && ~/.local/bin/pio run -t upload
   ```
   `platformio.ini` already sets `upload_port = /dev/ttyUSB0` and `upload_speed = 460800`. The default esptool stub is fine; no `--no-stub`, no extra flags. Auto-reset over RTS/DTR works, and nobody needs to press the board's buttons.
3. **Check what was built.** The upload log's "Wrote N bytes" should match the source you meant to flash. Another session or the user's editor can change `src/main.cpp` between your edit and your build.

**Never wire the screen's RD (header pin 10) to the NodeMCU's RX pin.** It belongs on 3.3V. While RD was on RX, upload traffic on RX pulsed the screen's RD line, and the screen then drove its data bus, which includes the boot-strap pins GPIO0 and GPIO2. Uploads worked for a while and then failed every time with "Timed out waiting for packet header", whatever the baud rate. They worked again on the first try once the wire was unplugged (2026-10-30). If a diagnostic ever needs RD on RX (`PIN_RD` in `src/tft.cpp`), have the user unplug it before every upload.

If uploads still fail with the port free and RD not on RX:
- Retry once at 115200: `~/.local/bin/pio run -t upload --upload-speed 115200`.
- Ask the user to check the USB cable and port.
- `Permission denied` on `/dev/ttyUSB0`: the user is in the `uucp` group, but a process started before they joined it doesn't have it. Run the command through `echo "<cmd>" | newgrp uucp`, or ask the user to restart VS Code.

## Reading serial output

```
~/.local/share/pipx/venvs/platformio/bin/python -I tools/readserial.py /dev/ttyUSB0 6
```

It holds the port while it runs, so never run it at the same time as an upload. `pio device monitor` needs an interactive terminal, so leave that to the user (`pio device monitor -b 115200`, Ctrl+C to exit).

Serial logging is limited by the wiring: the screen's RS line is on TX (GPIO1). RX (GPIO3) is free.
- While the display is in use, `Serial` must be off. Turn it on only between display accesses with `Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY)` … `Serial.flush(); Serial.end();`, then call `tft.begin()` again before drawing, because `Serial.end()` releases the pin.
- Libraries that log to `Serial` by default (WiFiManager, for example) must have their debug output turned off.
- A junk byte or two at each switch between serial and RS is expected.

## Working with other sessions

More than one Claude session may work in this folder at once. Before flashing, say so in your reply to the user. Don't overwrite `src/main.cpp` with a different program without the user asking: check whether it changed since you last read it.

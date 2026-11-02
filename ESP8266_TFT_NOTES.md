# ESP8266-12F + JYE Tech 2.4" TFT (DSO138 screen) — Notes

Goal: drive the 2.4" TFT from the DSO138 oscilloscope with an ESP8266-12F and show "Test".

Status: **working** (2026-10-29). The chip reads back as ILI9341 (0xD3 -> 00 93 41), all 8 data lines pass a read-back test, and the colour test plus "Test" text display correctly. Code: `src/tft.{h,cpp}` (driver), `src/main.cpp` (demo).

## Hardware

### MCU
- ESP8266 ESP-12F (3.3V logic) on a USB dev board with a CP2102 USB-serial chip (`10c4:ea60`),
  shows up as `/dev/ttyUSB0`. Chip ID `000D447C`, 4MB flash, 80 MHz.
- Toolchain: PlatformIO (installed with pipx, `~/.local/bin/pio`), env `esp12f` in `platformio.ini`
  (board `nodemcuv2`). Build/upload/monitor: `pio run -t upload -t monitor`.
- Serial access needs the `uucp` group (user is in it). Processes started before joining
  (e.g. an old VS Code) lack it: restart them, or run `echo "<cmd>" | newgrp uucp`.
- Hello-world serial test at 115200 baud: **verified working** (2026-10-29). The garbage before the
  first line at reset is the ROM bootloader printing at 74880 baud, which is normal.
- Usable GPIOs: 0, 1 (TX), 2, 3 (RX), 4, 5, 12, 13, 14, 15, 16.
- Boot-strap pins: GPIO0 and GPIO2 must be HIGH at boot, GPIO15 must be LOW at boot.
  These are fine to use as LCD inputs because the LCD inputs are high-impedance.

### Display
- Board silkscreen: `2.4" TFT LCD`, `PN:109-13700-00A1`.
- Origin: the "Fully Welded Assembled DSO138 2.4" TFT Digital Oscilloscope (1Msps)" kit (JYE Tech) from AliExpress.
- Official schematic: "2.4" TFT BASE", document 105-13700-00A, rev 00A (2014-08-23):
  https://jyetech.com/wp-content/uploads/2018/07/dso138-schematic-lcd.pdf
- 320x240 pixels, ILI9341-compatible controller (some DSO138 batches use a different
  controller chip that still accepts ILI9341 commands).
- Interface: 8080-style parallel bus. SPI is **not** available: only IM0 and IM3 are on the header.
- Has a resistive touch panel (XL/XR/YU/YD), currently unused.
- Board connectors:
  - J1: 2x20 header (the one with pins fitted, on the long right side). Pin 1 is the square pad.
  - J4: 2x20 holes along the top edge with the same signals as J1 (unpopulated).
  - J2, J3: 2-pin headers, not connected to anything (mechanical only).
  - JP1 / JP2: solder jumpers on the back that connect GND1 (pin 5) and GND2 (pin 34) to
    board ground. They may be open, so wire pins 5 and 34 to GND directly.

### J1 / J4 pinout (passes straight through to the LCD pins)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | DB0 | 2 | DB1 |
| 3 | DB2 | 4 | DB3 |
| 5 | GND1 | 6 | VCC1 |
| 7 | nCS | 8 | RS (D/C) |
| 9 | nWR | 10 | nRD |
| 11 | IM0 | 12 | XL (touch) |
| 13 | YU (touch) | 14 | XR (touch) |
| 15 | YD (touch) | 16 | LED-A (backlight +) |
| 17 | LED-K1 | 18 | LED-K2 |
| 19 | LED-K3 | 20 | LED-K4 |
| 21 | IM3 | 22 | DB4 |
| 23 | DB10 | 24 | DB11 |
| 25 | DB12 | 26 | DB13 |
| 27 | DB14 | 28 | DB15 |
| 29 | DB16 | 30 | DB17 |
| 31 | nRESET | 32 | VCC2 |
| 33 | VCC3 | 34 | GND2 |
| 35 | DB5 | 36 | DB6 |
| 37 | DB7 | 38–40 | not connected |

## Wiring plan (8-bit bus on DB10–DB17, header pins 23–30)

| J1 pin | Signal | Connect to | NodeMCU label |
|---|---|---|---|
| 23 | DB10 (bus bit 0) | GPIO4 | D2 |
| 24 | DB11 (bus bit 1) | GPIO5 | D1 |
| 25 | DB12 (bus bit 2) | GPIO12 | D6 |
| 26 | DB13 (bus bit 3) | GPIO13 | D7 |
| 27 | DB14 (bus bit 4) | GPIO14 | D5 |
| 28 | DB15 (bus bit 5) | GPIO16 | D0 |
| 29 | DB16 (bus bit 6) | GPIO0 | D3 |
| 30 | DB17 (bus bit 7) | GPIO2 | D4 |
| 9 | nWR | GPIO15 | D8 |
| 8 | RS (D/C) | GPIO1 (TX) | TX |
| 7 | nCS | GND (always selected) | |
| 10 | nRD | 3.3V. Was on GPIO3 (RX) for the read-back diagnostic, but that broke uploads (see CLAUDE.md) | |
| 31 | nRESET | NodeMCU RST pin (display resets with the ESP) | RST |
| 11 | IM0 | 3.3V | |
| 21 | IM3 | GND | |
| 6, 32, 33 | VCC | 3.3V | |
| 5, 34 | GND | GND | |
| 17–20 | Backlight cathodes | GND, no resistors (they share pin 16's resistor; wiring all four spreads the light evenly) | |
| 16 | Backlight anode | 3.3V via 47 Ω (confirmed working; 22 Ω for brighter, larger values get dim fast) | |
| 12–15 | Touch | leave unconnected | |
| 1–4, 22, 35–37 | DB0–DB7 | leave unconnected (not used in this mode; wiring the bus here gave no response at all) | |

Notes:
- IM0=3.3V / IM3=GND with the 8-bit bus on header pins 23–30 (DB10–DB17) was confirmed working by another user
  on an ESP32 with TFT_eSPI:
  https://forum.arduino.cc/t/support-for-a-jyetech-dso138-tft-using-ili9341/1143547
- RS is on TX (GPIO1), not RX: RX is driven by the USB-serial chip, TX is not.
  So the sketch must **not** call `Serial.begin()`. Boot messages on TX only toggle RS, which is harmless.
- If an upload fails while the LCD is connected, unplug J1 pin 8 during the upload.
- TFT_eSPI does not support parallel displays on the ESP8266, so the sketch drives the bus itself
  and uses Adafruit_GFX for text and graphics.
- Pin budget: 10 GPIOs used (8 data + WR + RS). If more pins are needed, move the data bus to a
  74HC595 or MCP23017.

## Wi-Fi

- Uses WiFiManager (`tzapu/WiFiManager`). Credentials are **not** in the source: they are entered from a
  phone and stored in the ESP's own flash.
- First boot (or saved network unreachable): the ESP opens the open hotspot `Claude-Monitor-Setup`.
  Join it, the setup page should pop up (otherwise browse to `192.168.4.1`), pick the network and enter the password.
- To forget the saved network: `pio run -t erase`, then upload again.
- `wifiManager.setDebugOutput(false)` is required: its serial logging would toggle RS (TX) mid-transfer.

## Test sketch

Arduino IDE, ESP8266 core, library "Adafruit GFX Library".

```cpp
#include <Adafruit_GFX.h>

const uint8_t DATA_PINS[8] = {4, 5, 12, 13, 14, 16, 0, 2}; // D0..D7
const uint8_t PIN_WR = 15;
const uint8_t PIN_DC = 1; // TX pin - don't call Serial.begin()

class ParallelTFT : public Adafruit_GFX {
public:
  ParallelTFT() : Adafruit_GFX(240, 320) {}

  void begin() {
    for (uint8_t p : DATA_PINS) pinMode(p, OUTPUT);
    pinMode(PIN_WR, OUTPUT); digitalWrite(PIN_WR, HIGH);
    pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);

    cmd(0x01); delay(150);          // software reset
    cmd(0x11); delay(120);          // sleep out
    cmd(0x3A); data(0x55);          // 16-bit colour
    cmd(0x36); data(0x48);          // memory access control (orientation/BGR)
    cmd(0x29); delay(20);           // display on
  }

  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    setWindow(x, y, x, y);
    data(c >> 8); data(c);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    if (w <= 0 || h <= 0) return;
    setWindow(x, y, x + w - 1, y + h - 1);
    for (int16_t row = 0; row < h; row++) {
      for (int16_t col = 0; col < w; col++) { data(c >> 8); data(c); }
      yield(); // keep the watchdog happy
    }
  }

  void fillScreen(uint16_t c) override { fillRect(0, 0, _width, _height, c); }

private:
  void write8(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) digitalWrite(DATA_PINS[i], (b >> i) & 1);
    digitalWrite(PIN_WR, LOW);
    digitalWrite(PIN_WR, HIGH); // data latched on rising edge
  }
  void cmd(uint8_t c) { digitalWrite(PIN_DC, LOW); write8(c); digitalWrite(PIN_DC, HIGH); }
  void data(uint8_t d) { write8(d); }

  void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    cmd(0x2A); data(x0 >> 8); data(x0); data(x1 >> 8); data(x1);
    cmd(0x2B); data(y0 >> 8); data(y0); data(y1 >> 8); data(y1);
    cmd(0x2C);
  }
};

ParallelTFT tft;

void setup() {
  tft.begin();
  tft.fillScreen(0x0000);           // black (takes ~1-2 s with digitalWrite, that's fine)
  tft.setTextColor(0xFFFF);
  tft.setTextSize(4);
  tft.setCursor(60, 140);
  tft.print("Test");
}

void loop() {}
```

Expected result: black screen with white "Test" in the middle.

## Troubleshooting

- Backlight on, screen stays white: check the 8 data wires and WR/RS first, then try IM0 on GND.
- Image mirrored or red/blue swapped: change the `0x48` after `cmd(0x36)` to `0x08`, `0x88` or `0xC8`.
- Slow drawing: expected with `digitalWrite`. It can be sped up later with direct GPIO register
  writes (`GPOS` / `GPOC`).
- ESP won't boot with the LCD attached: check that GPIO15 still has its pull-down and GPIO0/GPIO2
  their pull-ups.

## References

- JYE Tech LCD board schematic: https://jyetech.com/wp-content/uploads/2018/07/dso138-schematic-lcd.pdf
- Working ESP32 wiring for this display: https://forum.arduino.cc/t/support-for-a-jyetech-dso138-tft-using-ili9341/1143547
- Discussion of the 40-pin board: https://forum.arduino.cc/t/2-4-tft-lcd-40-pin-is-it-a-wild-goose-chase/1340390
- ILI9341 datasheet: https://courses.cs.washington.edu/courses/cse474/18wi/labs/l3/ILI9341.pdf
- JYE Tech module listing (ILI9341, 37-pin FPC): https://circuitmall.ng/product/jyetech-2-4-inch-320x240-tft-lcd-module/

## Wi-Fi setup (src/main.cpp)

- Uses WiFiManager (`tzapu/WiFiManager`). On boot it tries the saved network for 20 s. If there is
  no saved network or it can't connect, it opens the hotspot **Claude-Monitor-Setup**
  and shows instructions on the screen. The hotspot has a fresh random 8-digit password each time,
  shown only on the screen. Join it from a phone, pick the network and enter the password
  in the portal (it usually opens by itself; otherwise browse to 192.168.4.1).
- After connecting, the screen shows the network name and the ESP's IP address. The setup hotspot is switched
  off explicitly once connected (station-only mode), so nobody can join it and change the network.
- Credentials live in the ESP8266 SDK's config sectors at the end of flash. Normal uploads don't touch
  them, so they survive reflashing. Only `pio run -t erase` (full erase) clears them.
- WiFiManager debug output is turned off because Serial would toggle the display's RS line (TX).

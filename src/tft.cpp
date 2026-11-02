#include "tft.h"

#include <Arduino.h>

// The panel's 8-bit bus is on DB10-DB17 (J1 pins 23-30), with IM0 high and IM3 low.
// Wiring (J1 pin -> NodeMCU): DB10 23->D2, DB11 24->D1, DB12 25->D6, DB13 26->D7,
// DB14 27->D5, DB15 28->D0, DB16 29->D3, DB17 30->D4, WR 9->D8, RS 8->TX, RD 10->3.3V.
static const uint8_t DATA_PINS[8] = {4, 5, 12, 13, 14, 16, 0, 2}; // GPIO numbers, bus bit 0..7
static const uint8_t PIN_WR = 15;
static const uint8_t PIN_RS = 1; // TX: Serial output only works while the display isn't being driven
// RD is normally tied to 3.3V. Wired to RX (GPIO3) it enables readCommand() for wiring diagnostics,
// but upload traffic on RX then pulses RD and the display drives the bus, which breaks uploads.
// Set to 3 only while diagnosing, and unplug the wire again before uploading.
static const int8_t PIN_RD = -1;

// Writes go straight to the GPIO set/clear registers: digitalWrite() per bit made a full-screen
// redraw take over 3 seconds. busMask() must match DATA_PINS; bit 5 is GPIO16, which has its own register.
static const uint32_t BUS_MASK = (1 << 4) | (1 << 5) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 0) | (1 << 2);
static const uint32_t WR_BIT = 1 << PIN_WR;
static const uint32_t RS_BIT = 1 << PIN_RS;

static inline uint32_t busMask(uint8_t b) {
  return ((b & 0x03) << 4)     // bits 0, 1 -> GPIO4, GPIO5
         | ((b & 0x1C) << 10)  // bits 2, 3, 4 -> GPIO12, GPIO13, GPIO14
         | ((b & 0x40) >> 6)   // bit 6 -> GPIO0
         | ((b & 0x80) >> 5);  // bit 7 -> GPIO2
}

static int16_t busValue = -1; // what's on the bus now; -1 if unknown

static inline void setBus(uint8_t b) {
  if (b == busValue) return; // runs of one colour, black especially, only need WR strobes
  busValue = b;
  uint32_t set = busMask(b);
  GPOC = BUS_MASK & ~set;
  GPOS = set;
  GP16O = (b >> 5) & 1;
}

// The display latches the bus on WR's rising edge
static inline void strobeWR() {
  GPOC = WR_BIT;
  GPOS = WR_BIT;
}

// ILI9341 commands
static const uint8_t CMD_SWRESET = 0x01;
static const uint8_t CMD_SLPOUT = 0x11;
static const uint8_t CMD_DISPON = 0x29;
static const uint8_t CMD_CASET = 0x2A;
static const uint8_t CMD_PASET = 0x2B;
static const uint8_t CMD_RAMWR = 0x2C;
static const uint8_t CMD_MADCTL = 0x36;
static const uint8_t CMD_PIXFMT = 0x3A;

// MADCTL bits
static const uint8_t MADCTL_MY = 0x80;
static const uint8_t MADCTL_MX = 0x40;
static const uint8_t MADCTL_MV = 0x20;
static const uint8_t MADCTL_BGR = 0x08;

ParallelTFT::ParallelTFT() : Adafruit_GFX(240, 320) {}

void ParallelTFT::begin() {
  for (uint8_t pin : DATA_PINS) pinMode(pin, OUTPUT);
  busValue = -1;
  pinMode(PIN_WR, OUTPUT);
  digitalWrite(PIN_WR, HIGH);
  pinMode(PIN_RS, OUTPUT);
  digitalWrite(PIN_RS, HIGH);
  if (PIN_RD >= 0) {
    pinMode(PIN_RD, OUTPUT);
    digitalWrite(PIN_RD, HIGH);
  }

  command(CMD_SWRESET);
  delay(150);
  command(CMD_SLPOUT);
  delay(120);
  command(CMD_PIXFMT);
  data(0x55); // 16 bits per pixel
  setRotation(0);
  fillScreen(0x0000); // the panel's RAM is random after reset; clear it before switching the display on
  command(CMD_DISPON);
  delay(20);
}

void ParallelTFT::writeCommand(uint8_t cmd, const uint8_t* params, uint8_t len) {
  command(cmd);
  for (uint8_t i = 0; i < len; i++) data(params[i]);
}

void ParallelTFT::readCommand(uint8_t cmd, uint8_t* out, uint8_t len) {
  if (PIN_RD < 0) {
    memset(out, 0, len); // RD isn't wired, so nothing can be read back
    return;
  }
  command(cmd);
  for (uint8_t pin : DATA_PINS) pinMode(pin, pin == 16 ? INPUT : INPUT_PULLUP);
  read8(); // dummy byte
  for (uint8_t i = 0; i < len; i++) out[i] = read8();
  for (uint8_t pin : DATA_PINS) pinMode(pin, OUTPUT);
  busValue = -1;
}

void ParallelTFT::driveBus(uint8_t value) {
  for (uint8_t i = 0; i < 8; i++) digitalWrite(DATA_PINS[i], (value >> i) & 1);
  busValue = -1;
}

void ParallelTFT::setRotation(uint8_t r) {
  rotation = r & 3;
  uint8_t madctl;
  switch (rotation) {
    case 0: madctl = MADCTL_MX | MADCTL_BGR; _width = WIDTH; _height = HEIGHT; break;
    case 1: madctl = MADCTL_MV | MADCTL_BGR; _width = HEIGHT; _height = WIDTH; break;
    case 2: madctl = MADCTL_MY | MADCTL_BGR; _width = WIDTH; _height = HEIGHT; break;
    default: madctl = MADCTL_MX | MADCTL_MY | MADCTL_MV | MADCTL_BGR; _width = HEIGHT; _height = WIDTH; break;
  }
  command(CMD_MADCTL);
  data(madctl);
}

void ParallelTFT::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  setWindow(x, y, x, y);
  data(color >> 8);
  data(color);
}

void ParallelTFT::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  // Clip to the screen
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > _width) w = _width - x;
  if (y + h > _height) h = _height - y;
  if (w <= 0 || h <= 0) return;

  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = color >> 8, lo = color;
  if (hi == lo) {
    // Black, white and the like: the bus value never changes, so only WR needs toggling
    setBus(hi);
    for (int16_t row = 0; row < h; row++) {
      for (int16_t col = 0; col < w; col++) {
        strobeWR();
        strobeWR();
      }
    }
    return;
  }
  for (int16_t row = 0; row < h; row++) {
    for (int16_t col = 0; col < w; col++) {
      write8(hi);
      write8(lo);
    }
  }
}

void ParallelTFT::drawMono(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t* bits, uint16_t fg,
                           uint16_t bg) {
  const int16_t stride = (w + 7) / 8;
  // Clip to the screen (left/top clipping isn't needed by any caller)
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  if (x + w > _width) w = _width - x;
  if (y + h > _height) h = _height - y;
  if (w <= 0 || h <= 0) return;

  setWindow(x, y, x + w - 1, y + h - 1);
  for (int16_t row = 0; row < h; row++) {
    const uint8_t* line = bits + row * stride;
    for (int16_t col = 0; col < w; col++) {
      uint16_t c = (line[col >> 3] & (0x80 >> (col & 7))) ? fg : bg;
      write8(c >> 8);
      write8(c);
    }
  }
}

void ParallelTFT::startPixels(int16_t x, int16_t y, int16_t w, int16_t h) { setWindow(x, y, x + w - 1, y + h - 1); }

void ParallelTFT::pushPixels(const uint16_t* pixels, uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    write8(pixels[i] >> 8);
    write8(pixels[i]);
  }
}

void ParallelTFT::fillScreen(uint16_t color) { fillRect(0, 0, _width, _height, color); }

void ParallelTFT::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) { fillRect(x, y, w, 1, color); }

void ParallelTFT::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) { fillRect(x, y, 1, h, color); }

void ParallelTFT::write8(uint8_t b) {
  setBus(b);
  strobeWR();
}

uint8_t ParallelTFT::read8() {
  digitalWrite(PIN_RD, LOW);
  delayMicroseconds(1); // the controller needs up to ~350ns to put data on the bus
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) b |= digitalRead(DATA_PINS[i]) << i;
  digitalWrite(PIN_RD, HIGH);
  delayMicroseconds(1);
  return b;
}

void ParallelTFT::command(uint8_t c) {
  GPOC = RS_BIT;
  write8(c);
  GPOS = RS_BIT;
}

void ParallelTFT::data(uint8_t d) { write8(d); }

void ParallelTFT::setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  command(CMD_CASET);
  data(x0 >> 8);
  data(x0);
  data(x1 >> 8);
  data(x1);
  command(CMD_PASET);
  data(y0 >> 8);
  data(y0);
  data(y1 >> 8);
  data(y1);
  command(CMD_RAMWR);
}

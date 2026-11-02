#pragma once

#include <Adafruit_GFX.h>

// ILI9341-compatible 240x320 TFT (JYE Tech 109-13700, from the DSO138) on an 8-bit 8080 parallel bus.
// CS is tied to GND, RD to 3.3V, and RESET follows the ESP's RST pin; the data bus, WR and RS are driven.
class ParallelTFT : public Adafruit_GFX {
public:
  ParallelTFT();

  void begin();
  void writeCommand(uint8_t cmd, const uint8_t* params, uint8_t len);
  // Sends a read command and returns `len` bytes, skipping the dummy byte the controller sends first.
  // Only works with RD wired to a GPIO (see PIN_RD in tft.cpp); otherwise it returns zeros.
  void readCommand(uint8_t cmd, uint8_t* out, uint8_t len);
  // Puts a value on the data bus without strobing WR, for checking the data wires with a meter
  void driveBus(uint8_t value);
  void setRotation(uint8_t r) override;

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void fillScreen(uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;

  // Draws a 1-bit bitmap (rows padded to whole bytes, most significant bit first, as in GFXcanvas1)
  // in one pass: set bits in `fg`, clear bits in `bg`. Drawing whole areas this way, background
  // included, means the screen never shows a cleared frame.
  void drawMono(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t* bits, uint16_t fg, uint16_t bg);

  // Full-colour drawing without a frame buffer: startPixels() opens an area on the screen, then
  // pushPixels() fills it left to right, top to bottom, in as many calls as needed (a row at a time, say).
  // Nothing is clipped, so the area must be on the screen.
  void startPixels(int16_t x, int16_t y, int16_t w, int16_t h);
  void pushPixels(const uint16_t* pixels, uint16_t count);

private:
  void write8(uint8_t b);
  uint8_t read8();
  void command(uint8_t c);
  void data(uint8_t d);
  void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
};

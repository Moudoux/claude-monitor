#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <initializer_list>

#include "logo.h"
#include "tft.h"

// The display's RS line is on the TX pin, so there is no serial logging in this sketch.
// Everything the ESP is doing is reported on the screen instead.
//
// Wi-Fi credentials are saved by the ESP8266 SDK in the config sectors at the end of flash.
// A normal firmware upload only writes the program area, so they survive reflashing; only a
// full erase (`pio run -t erase`) clears them.

static constexpr uint16_t rgb(uint32_t hex) {
  return ((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F);
}

static const uint16_t BLACK = 0x0000;
static const uint16_t WHITE = 0xFFFF;
static const uint16_t GREY = 0x8410;
static const uint16_t RED = 0xF800;
static const uint16_t GREEN = 0x07E0;
static const uint16_t YELLOW = 0xFFE0;
static const uint16_t CYAN = 0x07FF;

// Hotspot the ESP opens when it has no saved Wi-Fi network (or can't reach it). It gets a fresh
// random password each time, shown only on the screen, so only someone who can see the screen can
// join and change the network. It is switched off as soon as Wi-Fi is connected.
static const char* SETUP_AP_NAME = "Claude-Monitor-Setup";
static const uint8_t SETUP_PASSWORD_DIGITS = 8; // WPA2 needs at least 8 characters
// How long to try the saved network before falling back to the setup hotspot
static const uint16_t CONNECT_TIMEOUT_S = 20;
// With a saved network, the hotspot closes after this long and the saved network is retried
static const uint16_t PORTAL_TIMEOUT_S = 300;
// If the connection drops and doesn't come back within this time, restart (which reopens setup)
static const uint32_t LOST_RESTART_MS = 2UL * 60 * 1000;

// The board answers at http://claude-monitor.local/ as well as its IP address
static const char* HOSTNAME = "claude-monitor";
// Larger request bodies are refused, to keep the heap free for the Wi-Fi stack
static const size_t MAX_BODY_BYTES = 4096;

// How long the boot screen stays up. Wi-Fi connects in the background meanwhile.
static const uint32_t BOOT_SCREEN_MS = 5000;

// Screen layout (landscape 320x240): title at the top, then text rows below it
static const int16_t SCREEN_W = 320;
static const int16_t SCREEN_H = 240;
static const int16_t MARGIN_X = 10;
static const int16_t TITLE_Y = 12;
static const int16_t RULE_Y = TITLE_Y + 34;
static const int16_t FIRST_ROW_Y = 64;
static const int16_t ROW_HEIGHT = 28;
static const uint8_t MAX_CHARS = 25; // at text size 2 (12px per character)

ParallelTFT tft;
WiFiManager wifiManager;
ESP8266WebServer server(80);
static String shownJson; // the last JSON drawn with POST /display, redrawn after a Wi-Fi drop

static String fit(const String& text, uint8_t maxChars = MAX_CHARS) {
  return text.length() <= maxChars ? text : text.substring(0, maxChars - 1) + "~";
}

// The built-in font is the IBM PC character set (code page 437), one byte per character, while
// text arrives as UTF-8. This converts the characters code page 437 has (accented Latin letters
// like ä ö å é ü ß ñ, and ° £ ½ and a few more); anything else becomes '?'.
static String utf8ToCp437(const String& utf8) {
  static const struct { uint16_t unicode; uint8_t cp437; } TABLE[] = {
      {0xC7, 0x80}, {0xFC, 0x81}, {0xE9, 0x82}, {0xE2, 0x83}, {0xE4, 0x84}, {0xE0, 0x85}, {0xE5, 0x86},
      {0xE7, 0x87}, {0xEA, 0x88}, {0xEB, 0x89}, {0xE8, 0x8A}, {0xEF, 0x8B}, {0xEE, 0x8C}, {0xEC, 0x8D},
      {0xC4, 0x8E}, {0xC5, 0x8F}, {0xC9, 0x90}, {0xE6, 0x91}, {0xC6, 0x92}, {0xF4, 0x93}, {0xF6, 0x94},
      {0xF2, 0x95}, {0xFB, 0x96}, {0xF9, 0x97}, {0xFF, 0x98}, {0xD6, 0x99}, {0xDC, 0x9A}, {0xA2, 0x9B},
      {0xA3, 0x9C}, {0xA5, 0x9D}, {0xE1, 0xA0}, {0xED, 0xA1}, {0xF3, 0xA2}, {0xFA, 0xA3}, {0xF1, 0xA4},
      {0xD1, 0xA5}, {0xAA, 0xA6}, {0xBA, 0xA7}, {0xBF, 0xA8}, {0xAC, 0xAA}, {0xBD, 0xAB}, {0xBC, 0xAC},
      {0xA1, 0xAD}, {0xAB, 0xAE}, {0xBB, 0xAF}, {0xDF, 0xE1}, {0xB5, 0xE6}, {0xB1, 0xF1}, {0xF7, 0xF6},
      {0xB0, 0xF8}, {0xB7, 0xFA}, {0xB2, 0xFD},
      {0xF8, 0xED}, {0xD8, 0xE8}, // ø and Ø aren't in code page 437; φ and Φ look closest
  };
  String out;
  out.reserve(utf8.length());
  for (size_t i = 0; i < utf8.length();) {
    uint8_t b = utf8[i];
    if (b < 0x80) {
      out += char(b);
      i++;
      continue;
    }
    // Decode one multi-byte sequence (2 to 4 bytes) into its code point
    uint8_t extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    uint32_t cp = b & (0x3F >> extra);
    i++;
    for (uint8_t k = 0; k < extra && i < utf8.length() && (uint8_t(utf8[i]) & 0xC0) == 0x80; k++, i++) {
      cp = (cp << 6) | (utf8[i] & 0x3F);
    }
    char c = '?';
    for (auto& t : TABLE) {
      if (t.unicode == cp) c = char(t.cp437);
    }
    out += c;
  }
  return out;
}

// toUpperCase() for code page 437 text: also uppercases the accented letters that have capitals
static String cp437Upper(String text) {
  static const char LOWER[] = "\x84\x94\x86\x81\x82\x87\xA4\x91", UPPER[] = "\x8E\x99\x8F\x9A\x90\x80\xA5\x92";
  text.toUpperCase();
  for (size_t i = 0; i < text.length(); i++) {
    const char* hit = strchr(LOWER, text[i]);
    if (hit && text[i]) text[i] = UPPER[hit - LOWER];
  }
  return text;
}

// ---- Flicker-free drawing ----
//
// Nothing is drawn by clearing the screen and then drawing on top, which shows a black frame.
// Instead each full-width strip (title, a text line, a gap) is rendered into this 1-bit buffer and
// sent to the display in one pass, background included, so every pixel goes straight from its old
// colour to its new one. A strip has one foreground colour.

static GFXcanvas1 strip(SCREEN_W, 96); // tall enough for the tallest graph (see paintGraph())

// Prints code page 437 text into the strip with its top left corner at (x, y)
static void stripPrint(int16_t x, int16_t y, const String& text, uint8_t size) {
  strip.setTextWrap(false);
  strip.cp437(true); // without this the font is off by one from character 0xB0 up
  strip.setTextColor(1);
  strip.setTextSize(size);
  strip.setCursor(x, y);
  strip.print(text);
}

// Whether the strip has a pixel set at (x, y); false outside it
static bool stripBit(int16_t x, int16_t y) {
  if (x < 0 || y < 0 || x >= strip.width() || y >= strip.height()) return false;
  return strip.getBuffer()[y * ((SCREEN_W + 7) / 8) + x / 8] & (0x80 >> (x & 7));
}

// Paints the strip from `y`, `h` pixels high: `text` at `textY` within it, and a horizontal rule
// at `ruleY` if that isn't negative
static void paintStrip(int16_t y, int16_t h, const String& text, uint16_t color, uint8_t size = 2,
                       int16_t textY = 0, int16_t ruleY = -1) {
  if (h > strip.height()) h = strip.height();
  strip.fillScreen(0);
  if (text.length()) stripPrint(MARGIN_X, textY, text, size);
  if (ruleY >= 0) strip.drawFastHLine(MARGIN_X, ruleY, SCREEN_W - 2 * MARGIN_X, 1);
  tft.drawMono(0, y, SCREEN_W, h, strip.getBuffer(), color, BLACK);
}

struct Row {
  String text;
  uint16_t color = WHITE;
  uint8_t size = 2;
};

// Replaces one text row in place
static void showRow(uint8_t row, const String& text, uint16_t color = WHITE, uint8_t size = 2) {
  paintStrip(FIRST_ROW_Y + row * ROW_HEIGHT, ROW_HEIGHT, text, color, size);
}

// Draws a whole status screen: a coloured title with a rule under it, then rows ({} for a blank one)
static void showScreen(uint16_t color, const String& title, std::initializer_list<Row> rows) {
  paintStrip(0, FIRST_ROW_Y, title, color, 3, TITLE_Y, RULE_Y);
  uint8_t i = 0;
  for (const Row& r : rows) showRow(i++, r.text, r.color, r.size);
  int16_t y = FIRST_ROW_Y + i * ROW_HEIGHT;
  if (y < SCREEN_H) tft.fillRect(0, y, SCREEN_W, SCREEN_H - y, BLACK);
}

static String describeFailure(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL: return "network not found";
    case WL_WRONG_PASSWORD: return "wrong password";
    case WL_CONNECT_FAILED: return "connection refused";
    default: return "timed out";
  }
}

static void showConnected() {
  showScreen(GREEN, "Wi-Fi connected",
             {{"Network:", GREY},
              {fit(utf8ToCp437(WiFi.SSID()))},
              {},
              {"IP address:", GREY},
              {WiFi.localIP().toString(), CYAN, 3},
              {String(HOSTNAME) + ".local", GREY}});
}

// ---- Boot screen ----

// The logo is a bitmap made from assets/apple-logo.png by tools/logo2header.py, centred on `cx`
static void drawLogo(int16_t cx, int16_t top) {
  uint8_t row[(LOGO_W + 7) / 8];
  for (int16_t y = 0; y < LOGO_H; y++) {
    memcpy_P(row, LOGO_BITS + y * sizeof(row), sizeof(row));
    tft.drawMono(cx - LOGO_W / 2, top + y, LOGO_W, 1, row, pgm_read_word(LOGO_ROW_COLORS + y), BLACK);
  }
}

// Prints text one character at a time, like an old terminal
static void typeOut(const String& text, uint16_t msPerChar = 14) {
  for (char c : text) {
    tft.print(c);
    delay(msPerChar);
  }
}

// "LABEL ......... RESULT", padded to a fixed width
static String bootLine(const String& label, const String& result, uint8_t width = 36) {
  String line = label + ' ';
  while (line.length() + result.length() + 1 < width) line += '.';
  return line + ' ' + result;
}

static void showBootScreen() {
  const uint32_t start = millis();
  const uint16_t PHOSPHOR = rgb(0x33FF66);
  tft.fillScreen(BLACK);
  drawLogo(SCREEN_W / 2, 12);

  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(WHITE, BLACK);
  tft.setCursor((SCREEN_W - 14 * 12) / 2, 124);
  tft.print("CLAUDE MONITOR");

  const int16_t x = (SCREEN_W - 36 * 6) / 2;
  int16_t y = 152;
  tft.setTextSize(1);
  tft.setTextColor(PHOSPHOR, BLACK);
  auto nextLine = [&](const String& text) {
    tft.setCursor(x, y);
    typeOut(text);
    y += 11;
  };
  nextLine(bootLine("CPU  ESP8266 " + String(ESP.getCpuFreqMHz()) + "MHZ", "OK"));
  nextLine(bootLine("RAM  " + String(ESP.getFreeHeap() / 1024) + "K FREE", "OK"));
  nextLine(bootLine("ROM  " + String(ESP.getFlashChipRealSize() / 1024) + "K", "OK"));
  nextLine(bootLine("VIDEO ILI9341 320X240", "OK"));
  String ssid = cp437Upper(utf8ToCp437(WiFi.SSID()));
  nextLine(bootLine("NET  " + fit(ssid, 16), ssid.isEmpty() ? "SETUP" : "LINKING"));

  // A prompt with a blinking block cursor for the rest of the boot time
  tft.setCursor(x, y);
  tft.print(']');
  bool on = false;
  while (millis() - start < BOOT_SCREEN_MS) {
    on = !on;
    tft.fillRect(x + 6, y, 6, 8, on ? PHOSPHOR : BLACK);
    delay(250);
  }
}

// ---- Drawing posted JSON ----

static const int16_t FOOTER_Y = 228; // the strip with the IP address at the bottom of posted screens

// Accepts "#RRGGBB", "#RGB" or a colour name; anything else gives `fallback`
static uint16_t parseColor(const char* s, uint16_t fallback) {
  if (!s) return fallback;
  static const struct { const char* name; uint16_t color; } NAMED[] = {
      {"white", WHITE}, {"black", BLACK}, {"grey", GREY}, {"gray", GREY},
      {"red", RED}, {"green", GREEN}, {"yellow", YELLOW}, {"cyan", CYAN},
      {"blue", 0x001F}, {"magenta", 0xF81F}, {"orange", 0xFD20}};
  for (auto& n : NAMED) {
    if (strcasecmp(s, n.name) == 0) return n.color;
  }
  if (s[0] != '#') return fallback;
  size_t len = strlen(s + 1);
  if (len != 6 && len != 3) return fallback;
  char* end;
  uint32_t v = strtoul(s + 1, &end, 16);
  if (*end) return fallback;
  if (len == 3) v = ((v & 0xF00) * 0x1100) | ((v & 0x0F0) * 0x110) | ((v & 0x00F) * 0x11);
  return rgb(v);
}

// Paints one text item, word-wrapped to the screen width, as strips starting at `y`.
// Returns the y below it, or FOOTER_Y when the screen is full.
static int16_t paintWrapped(int16_t y, const char* text, uint16_t color, uint8_t size) {
  const int16_t lineHeight = 8 * size + 4;
  const uint8_t maxChars = (SCREEN_W - 2 * MARGIN_X) / (6 * size);

  String rest = utf8ToCp437(text);
  while (true) {
    // Take the next paragraph (up to a newline)
    int nl = rest.indexOf('\n');
    String para = nl < 0 ? rest : rest.substring(0, nl);
    // Split it at spaces into screen-width lines; an empty paragraph is a blank line
    do {
      if (y + 8 * size > FOOTER_Y) return FOOTER_Y;
      String line = para;
      if (para.length() > maxChars) {
        int cut = para.lastIndexOf(' ', maxChars);
        if (cut <= 0) cut = maxChars;
        line = para.substring(0, cut);
        para = para.substring(cut);
        para.trim();
      } else {
        para = String();
      }
      int16_t h = min<int16_t>(lineHeight, FOOTER_Y - y);
      paintStrip(y, h, line, color, size);
      y += h;
    } while (para.length());
    if (nl < 0) return y;
    rest = rest.substring(nl + 1);
  }
}

// ---- Bars and graphs ----
//
// These have many colours, gradients and anti-aliased edges, so they can't go through the 1-bit strip.
// Instead each one is composed a pixel row at a time into `rowPixels` and streamed to the display,
// which is flicker-free in the same way. Text on them is printed into the strip first and read back
// pixel by pixel while composing.

static uint16_t rowPixels[SCREEN_W];

static const int16_t WIDGET_GAP = 6; // blank rows below a bar or graph
static const uint16_t TRACK = rgb(0x262626);
static const uint16_t GRID = rgb(0x3A3A3A);
static const uint16_t LABEL = rgb(0xC8C8C8);
static const uint32_t BAR_HEAT[] = {0x22C55E, 0xEAB308, 0xEF4444};                     // green, yellow, red
static const uint32_t GRAPH_HEAT[] = {0x3B82F6, 0x06B6D4, 0x22C55E, 0xEAB308, 0xEF4444}; // blue ... red

// Mixes two colours: `t` = 0 gives `a`, 255 gives `b`
static uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  int32_t k = t + (t >> 7); // 0-256, so that 255 gives exactly `b`
  int32_t ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int32_t br = b >> 11, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  return ((ar + (((br - ar) * k) >> 8)) << 11) | ((ag + (((bg - ag) * k) >> 8)) << 5) | (ab + (((bb - ab) * k) >> 8));
}

// The colour `t` (0-255) of the way along a gradient through `n` evenly spaced 0xRRGGBB stops
static uint16_t gradient(const uint32_t* stops, uint8_t n, uint8_t t) {
  uint16_t pos = t * (n - 1);
  uint8_t i = min<uint8_t>(pos / 255, n - 2);
  int32_t f = pos - i * 255;
  uint32_t a = stops[i], b = stops[i + 1], out = 0;
  for (uint8_t shift = 0; shift <= 16; shift += 8) {
    int32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
    out |= uint32_t(ca + (cb - ca) * f / 255) << shift;
  }
  return rgb(out);
}

static uint8_t alpha(float coverage) { return coverage <= 0 ? 0 : coverage >= 1 ? 255 : uint8_t(coverage * 255); }

// How much of pixel (px, py) is inside a w x h rectangle with corners of radius r, from 0 to 255
static uint8_t roundRectCoverage(int16_t px, int16_t py, int16_t w, int16_t h, float r) {
  if (px >= r && px + 1 <= w - r) return 255; // between the rounded ends
  float qx = fabsf(px + 0.5f - w / 2.0f) - (w / 2.0f - r);
  float qy = fabsf(py + 0.5f - h / 2.0f) - (h / 2.0f - r);
  float d = qx > 0 && qy > 0 ? sqrtf(qx * qx + qy * qy) - r : max(qx, qy) - r;
  return alpha(0.5f - d);
}

// Lightens the top of a shape and darkens its bottom, for a rounded look
static uint16_t shade(uint16_t c, int16_t py, int16_t h) {
  int16_t t = py * 255 / (h - 1);
  if (t < 96) return mix(c, WHITE, (96 - t) * 3 / 4);
  if (t > 160) return mix(c, BLACK, (t - 160) * 3 / 4);
  return c;
}

// Text pixels from the strip on top of colour `c`: `fg` with a dark edge so it reads on anything.
// With `outline` the edge goes all round each letter, otherwise it's a drop shadow.
static uint16_t overlayText(uint16_t c, int16_t x, int16_t y, uint16_t fg, bool outline) {
  if (stripBit(x, y)) return fg;
  if (stripBit(x - 1, y - 1)) return mix(c, BLACK, 150);
  if (outline && (stripBit(x + 1, y) || stripBit(x - 1, y) || stripBit(x, y + 1) || stripBit(x, y - 1))) return BLACK;
  return c;
}

// Width of code page 437 text in pixels, without the blank column after the last letter
static int16_t textWidth(const String& text, uint8_t size) { return text.length() ? text.length() * 6 * size - size : 0; }

// A rounded bar across the screen, filled to {"bar": value} out of "max" (default 100), with
// "label" at its left and "value" (default the percentage) at its right. "color" is a colour, or
// "heat" (the default) for a green-yellow-red gradient along the bar. Returns the y below it.
static int16_t paintBar(int16_t y, JsonObjectConst item) {
  const int16_t h = constrain(item["height"] | 26, 12, 48);
  if (y + h > FOOTER_Y) return FOOTER_Y;
  const int16_t w = SCREEN_W - 2 * MARGIN_X;
  const float maxValue = item["max"] | 100.0f;
  const float fraction = maxValue > 0 ? constrain((item["bar"] | 0.0f) / maxValue, 0.0f, 1.0f) : 0.0f;
  const char* colorName = item["color"];
  const bool heat = !colorName || strcasecmp(colorName, "heat") == 0;
  const uint16_t solid = parseColor(colorName, WHITE);
  const uint16_t textColor = parseColor(item["textColor"], WHITE);

  String label = utf8ToCp437(item["label"] | "");
  String value = item["value"].is<const char*>() ? utf8ToCp437(item["value"].as<const char*>())
                                                 : String(int(fraction * 100 + 0.5f)) + "%";
  const uint8_t size = h >= 22 ? 2 : 1;
  const int16_t textY = (h - 7 * size) / 2, pad = max<int16_t>(h / 2, 8);
  strip.fillScreen(0);
  stripPrint(MARGIN_X + pad, textY, label, size);
  stripPrint(MARGIN_X + w - pad - textWidth(value, size), textY, value, size);

  const int16_t rows = min<int16_t>(h + WIDGET_GAP, FOOTER_Y - y);
  tft.startPixels(0, y, SCREEN_W, rows);
  for (int16_t py = 0; py < rows; py++) {
    for (int16_t x = 0; x < SCREEN_W; x++) {
      const int16_t px = x - MARGIN_X;
      uint16_t c = BLACK;
      if (py < h && px >= 0 && px < w) {
        uint16_t fill = heat ? gradient(BAR_HEAT, 3, px * 255 / (w - 1)) : solid;
        c = shade(mix(TRACK, fill, alpha(fraction * w - px)), py, h);
        c = overlayText(c, x, py, textColor, false);
        c = mix(BLACK, c, roundRectCoverage(px, py, w, h, h / 2.0f));
      }
      rowPixels[x] = c;
    }
    tft.pushPixels(rowPixels, SCREEN_W);
  }
  return y + rows;
}

static const uint8_t MAX_POINTS = 120;
static float points[MAX_POINTS];
static float curve[SCREEN_W + 1];      // the graph line's y at the left edge of each column
static uint16_t columnColor[SCREEN_W]; // the line's colour in each column

// The value at `x` (in points, 0 to n-1) on a smooth curve through `points` (a Catmull-Rom spline)
static float splineAt(uint8_t n, float x) {
  if (x <= 0) return points[0];
  if (x >= n - 1) return points[n - 1];
  const uint8_t i = x;
  const float t = x - i;
  const float p0 = points[i > 0 ? i - 1 : 0], p1 = points[i], p2 = points[i + 1], p3 = points[min<uint8_t>(i + 2, n - 1)];
  return 0.5f * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (3 * p1 - p0 - 3 * p2 + p3) * t * t * t);
}

// A smooth line graph of {"graph": [numbers]} with the area under it shaded, the highest and lowest
// values (or "min"/"max") marked on the right with "unit" after them, and optionally "labels" spread
// evenly under it, a dot on point number "marker", and "height" (24-96, default 60). "color" is a
// colour, or "heat" to colour the line by its height from blue to red. Returns the y below it.
static int16_t paintGraph(int16_t y, JsonObjectConst item) {
  uint8_t n = 0;
  for (JsonVariantConst v : item["graph"].as<JsonArrayConst>()) {
    if (n < MAX_POINTS && v.is<float>()) points[n++] = v.as<float>();
  }
  if (n == 0) return y;
  if (n == 1) points[n++] = points[0];
  const int16_t h = constrain(item["height"] | 60, 24, int(strip.height()));
  if (y + h > FOOTER_Y) return FOOTER_Y;
  const int16_t w = SCREEN_W - 2 * MARGIN_X;

  float lo = points[0], hi = points[0];
  for (uint8_t i = 1; i < n; i++) {
    lo = min(lo, points[i]);
    hi = max(hi, points[i]);
  }
  lo = item["min"] | lo;
  hi = item["max"] | hi;
  if (hi - lo < 1e-6f) {
    lo -= 1;
    hi += 1;
  }

  // Points sit INSET pixels in from each side so a marker on the first or last isn't cut off
  const float INSET = 5, top = 4, bottom = h - 4;
  auto xOf = [&](float i) { return INSET + i * (w - 2 * INSET) / (n - 1); };
  auto yOf = [&](float v) { return constrain(bottom - (v - lo) / (hi - lo) * (bottom - top), 0.0f, h - 1.0f); };
  for (int16_t e = 0; e <= w; e++) curve[e] = yOf(splineAt(n, (e - INSET) * (n - 1) / (w - 2 * INSET)));

  const char* colorName = item["color"];
  const bool heat = colorName && strcasecmp(colorName, "heat") == 0;
  const uint16_t solid = parseColor(colorName, CYAN);
  for (int16_t px = 0; px < w; px++) {
    float level = (bottom - (curve[px] + curve[px + 1]) / 2) / (bottom - top);
    columnColor[px] = heat ? gradient(GRAPH_HEAT, 5, alpha(level)) : solid;
  }

  // Dotted gridlines at the top, middle and bottom, and down from each label
  const int16_t gridRows[] = {int16_t(top + 0.5f), int16_t((top + bottom) / 2 + 0.5f), int16_t(bottom + 0.5f)};
  JsonArrayConst labels = item["labels"];
  const uint8_t labelCount = min<size_t>(labels.size(), 12);
  int16_t gridCols[12];
  for (uint8_t k = 0; k < labelCount; k++) {
    gridCols[k] = labelCount > 1 ? int16_t(xOf(float(k) * (n - 1) / (labelCount - 1))) : -1;
  }

  const int marker = item["marker"] | -1;
  const bool hasMarker = marker >= 0 && marker < n;
  const float mx = hasMarker ? xOf(marker) : 0, my = hasMarker ? yOf(points[marker]) : 0;

  // The scale, at the right end of the top and bottom gridlines
  const uint8_t decimals = hi - lo < 10 ? 1 : 0;
  const String unit = utf8ToCp437(item["unit"] | "");
  const String hiText = String(hi, decimals) + unit, loText = String(lo, decimals) + unit;
  strip.fillScreen(0);
  stripPrint(MARGIN_X + w - 2 - textWidth(hiText, 1), gridRows[0] + 3, hiText, 1);
  stripPrint(MARGIN_X + w - 2 - textWidth(loText, 1), gridRows[2] - 10, loText, 1);

  tft.startPixels(0, y, SCREEN_W, h);
  for (int16_t py = 0; py < h; py++) {
    const float cy = py + 0.5f;
    const bool gridRow = py == gridRows[0] || py == gridRows[1] || py == gridRows[2];
    for (int16_t x = 0; x < SCREEN_W; x++) {
      const int16_t px = x - MARGIN_X;
      uint16_t c = BLACK;
      if (px >= 0 && px < w) {
        bool grid = gridRow && px % 4 == 0;
        for (uint8_t k = 0; k < labelCount && !grid; k++) grid = px == gridCols[k] && py % 4 == 0;
        if (grid) c = GRID;

        // The shaded area fades out towards the bottom
        const uint16_t lc = columnColor[px];
        const float e0 = curve[px], e1 = curve[px + 1], mid = (e0 + e1) / 2;
        if (cy > mid) c = mix(c, lc, 100 - 85 * (cy - mid) / (h - mid));

        // The line, 2 pixels thick, anti-aliased by how much of each pixel it covers
        const float lineTop = min(e0, e1) - 1, lineBottom = max(e0, e1) + 1;
        c = mix(c, lc, alpha(min(py + 1.0f, lineBottom) - max(float(py), lineTop)));

        if (hasMarker && fabsf(px + 0.5f - mx) < 6 && fabsf(cy - my) < 6) {
          const float d = sqrtf(sq(px + 0.5f - mx) + sq(cy - my));
          c = mix(mix(c, BLACK, alpha(5.0f - d)), WHITE, alpha(3.5f - d));
        }
        c = overlayText(c, x, py, LABEL, true);
      }
      rowPixels[x] = c;
    }
    tft.pushPixels(rowPixels, SCREEN_W);
  }
  y += h;

  if (labelCount && y + 10 <= FOOTER_Y) {
    strip.fillScreen(0);
    for (uint8_t k = 0; k < labelCount; k++) {
      String text = utf8ToCp437(labels[k] | "");
      int16_t centre = labelCount > 1 ? MARGIN_X + gridCols[k] : SCREEN_W / 2;
      stripPrint(constrain(centre - textWidth(text, 1) / 2, 0, SCREEN_W - textWidth(text, 1)), 2, text, 1);
    }
    tft.drawMono(0, y, SCREEN_W, 10, strip.getBuffer(), GREY, BLACK);
    y += 10;
  }
  const int16_t gap = min<int16_t>(WIDGET_GAP, FOOTER_Y - y);
  if (gap > 0) tft.fillRect(0, y, SCREEN_W, gap, BLACK);
  return y + max<int16_t>(gap, 0);
}

// A face: two rounded eyes and a mouth, in "color" on black, "height" pixels high (40-140, default 90).
// {"face": {...}} sets its expression, so a script can animate it by posting frames. All are optional:
//   eyes  how open the eyes are, 0-1 (default 1)      lookX, lookY  where they look, -1 to 1 (default 0)
//   smile the mouth's curve, -1 (frown) to 1 (default 0)   mouth  how open the mouth is, 0-1 (default 0)
// Returns the y below it.
static int16_t paintFace(int16_t y, JsonObjectConst item) {
  const int16_t h = constrain(item["height"] | 90, 40, 140);
  if (y + h > FOOTER_Y) return FOOTER_Y;
  JsonObjectConst face = item["face"];
  const uint16_t color = parseColor(item["color"], WHITE);
  const float scale = h / 90.0f;

  const float lookX = constrain(face["lookX"] | 0.0f, -1.0f, 1.0f) * 10 * scale;
  const float lookY = constrain(face["lookY"] | 0.0f, -1.0f, 1.0f) * 6 * scale;
  const float eyeCentres[] = {SCREEN_W / 2 - 46 * scale + lookX, SCREEN_W / 2 + 46 * scale + lookX};
  const float eyeY = 30 * scale + lookY, eyeW = 15 * scale;
  const float eyeH = max(2.0f, constrain(face["eyes"] | 1.0f, 0.0f, 1.0f) * 20 * scale);
  const float eyeR = min(eyeW, eyeH);

  const float smile = constrain(face["smile"] | 0.0f, -1.0f, 1.0f) * 10 * scale;
  const float mouthOpen = constrain(face["mouth"] | 0.0f, 0.0f, 1.0f) * 8 * scale;
  const float mouthX = SCREEN_W / 2 + lookX / 3, mouthW = 26 * scale, mouthThick = 2 * scale;
  const float mouthY = 68 * scale - smile / 2; // the centre of the curve stays about where it is

  const int16_t rows = min<int16_t>(h + WIDGET_GAP, FOOTER_Y - y);
  tft.startPixels(0, y, SCREEN_W, rows);
  for (int16_t py = 0; py < rows; py++) {
    memset(rowPixels, 0, sizeof(rowPixels));
    const float cy = py + 0.5f;

    if (fabsf(cy - eyeY) < eyeH + 1) {
      for (float ex : eyeCentres) {
        const int16_t left = ex - eyeW - 1, right = ex + eyeW + 1;
        for (int16_t x = left; x <= right; x++) {
          const float qx = fabsf(x + 0.5f - ex) - (eyeW - eyeR), qy = fabsf(cy - eyeY) - (eyeH - eyeR);
          const float d = qx > 0 && qy > 0 ? sqrtf(qx * qx + qy * qy) - eyeR : max(qx, qy) - eyeR;
          rowPixels[x] = mix(BLACK, color, alpha(0.5f - d));
        }
      }
    }

    // The mouth is a parabola that opens up in the middle, with round ends
    const int16_t left = mouthX - mouthW, right = mouthX + mouthW;
    for (int16_t x = left; x <= right; x++) {
      const float dx = fabsf(x + 0.5f - mouthX), edge = mouthW - dx;
      if (edge <= 0) continue;
      const float bulge = 1 - sq(dx / mouthW);
      const float cap = edge < mouthThick ? sqrtf(sq(mouthThick) - sq(mouthThick - edge)) : mouthThick;
      const float centre = mouthY + smile * bulge, half = cap + mouthOpen * bulge;
      if (fabsf(cy - centre) > half + 1) continue;
      rowPixels[x] = mix(BLACK, color, alpha(min(py + 1.0f, centre + half) - max(float(py), centre - half)));
    }
    tft.pushPixels(rowPixels, SCREEN_W);
  }
  return y + rows;
}

// Draws a document like
//   {"title": "Build", "color": "green",
//    "lines": ["plain text", {"text": "big", "size": 3, "color": "#ff8800"}]}
// "text" may be given instead of "lines" for a single block (newlines allowed). All keys are optional.
// An entry in "lines" can also be a bar ({"bar": 62, ...}, see paintBar()), a graph ({"graph": [...]}, see paintGraph())
// or a face ({"face": {...}}, see paintFace()).
static void drawDocument(JsonDocument& doc) {
  const char* title = doc["title"];
  int16_t y;
  if (title) {
    y = RULE_Y + 12;
    paintStrip(0, y, fit(utf8ToCp437(title), (SCREEN_W - 2 * MARGIN_X) / 18), parseColor(doc["color"], WHITE), 3,
               TITLE_Y, RULE_Y);
  } else {
    y = TITLE_Y;
    tft.fillRect(0, 0, SCREEN_W, y, BLACK);
  }

  uint8_t defaultSize = constrain(doc["size"] | 2, 1, 4);
  uint16_t defaultColor = parseColor(doc["textColor"], WHITE);
  if (doc["text"].is<const char*>()) {
    y = paintWrapped(y, doc["text"], defaultColor, defaultSize);
  }
  for (JsonVariant item : doc["lines"].as<JsonArray>()) {
    if (item.is<const char*>()) {
      y = paintWrapped(y, item.as<const char*>(), defaultColor, defaultSize);
    } else if (item["bar"].is<float>()) {
      y = paintBar(y, item.as<JsonObjectConst>());
    } else if (item["graph"].is<JsonArray>()) {
      y = paintGraph(y, item.as<JsonObjectConst>());
    } else if (item["face"].is<JsonObject>()) {
      y = paintFace(y, item.as<JsonObjectConst>());
    } else if (item.is<JsonObject>()) {
      uint8_t size = constrain(item["size"] | defaultSize, 1, 4);
      y = paintWrapped(y, item["text"] | "", parseColor(item["color"], defaultColor), size);
    }
  }

  if (y < FOOTER_Y) tft.fillRect(0, y, SCREEN_W, FOOTER_Y - y, BLACK);
  paintStrip(FOOTER_Y, SCREEN_H - FOOTER_Y, WiFi.localIP().toString(), GREY, 1, 2);
}

// Parses and draws `json`; returns an error message, or an empty string on success
static String showJson(const String& json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return String("invalid JSON: ") + err.c_str();
  if (!doc.is<JsonObject>()) return "expected a JSON object";
  drawDocument(doc);
  return String();
}

// ---- HTTP server ----

static const char USAGE[] PROGMEM =
    "Claude Monitor\n"
    "\n"
    "POST /display   draw a JSON document on the screen\n"
    "DELETE /display go back to the Wi-Fi status screen\n"
    "GET /display    the JSON currently shown\n"
    "\n"
    "Example:\n"
    "  curl -X POST http://claude-monitor.local/display \\\n"
    "    -H 'Content-Type: application/json' \\\n"
    "    -d '{\"title\":\"Hello\",\"color\":\"green\",\"lines\":[\"Line one\",\n"
    "         {\"text\":\"Big\",\"size\":3,\"color\":\"#ff8800\"}]}'\n"
    "\n"
    "Keys (all optional): title, color (title colour), text, lines, size (1-4, default 2),\n"
    "textColor. Each entry in lines is a string or {text, size, color}. Colours are\n"
    "#RRGGBB, #RGB or white/grey/red/green/blue/yellow/cyan/magenta/orange.\n"
    "Text wraps at word boundaries; whatever doesn't fit is cut off. Accented Latin letters\n"
    "(ä ö å é ü ß ñ...) and ° work; other characters show as ?.\n"
    "\n"
    "Bars and graphs can go in lines too:\n"
    "  {\"bar\":62, \"max\":100, \"label\":\"5-hour\", \"value\":\"62%\", \"color\":\"heat\", \"height\":26}\n"
    "  {\"graph\":[3,5,4,8], \"color\":\"heat\", \"height\":60, \"unit\":\"°\", \"min\":0, \"max\":10,\n"
    "   \"labels\":[\"now\",\"+12h\",\"+24h\"], \"marker\":0}\n"
    "Only the bar or graph key is needed. color \"heat\" is a green-to-red gradient for bars\n"
    "(the default) and colours a graph's line by its height, blue to red. marker puts a dot\n"
    "on that point (counting from 0); labels are spread evenly under the graph.\n"
    "\n"
    "So can a face, which a script can animate by posting frames:\n"
    "  {\"face\":{\"eyes\":1, \"lookX\":0, \"lookY\":0, \"smile\":0.5, \"mouth\":0}, \"color\":\"#d97757\", \"height\":90}\n"
    "eyes and mouth are how open they are (0-1); lookX, lookY and smile go from -1 to 1.\n";

static void handleUsage() { server.send_P(200, "text/plain", USAGE); }

static void handlePostDisplay() {
  const String& body = server.arg("plain");
  if (body.isEmpty()) return server.send(400, "text/plain", "empty body\n");
  if (body.length() > MAX_BODY_BYTES) return server.send(413, "text/plain", "body too large\n");
  uint32_t start = millis();
  String err = showJson(body);
  if (err.length()) return server.send(400, "text/plain", err + "\n");
  shownJson = body;
  server.send(200, "text/plain", "ok, drawn in " + String(millis() - start) + " ms\n");
}

static void handleGetDisplay() {
  if (shownJson.isEmpty()) return server.send(204);
  server.send(200, "application/json", shownJson);
}

static void handleClearDisplay() {
  shownJson = String();
  showConnected();
  server.send(200, "text/plain", "ok\n");
}

static void startServer() {
  server.on("/", HTTP_GET, handleUsage);
  server.on("/display", HTTP_POST, handlePostDisplay);
  server.on("/display", HTTP_GET, handleGetDisplay);
  server.on("/display", HTTP_DELETE, handleClearDisplay);
  server.onNotFound([] { server.send(404, "text/plain", "not found; GET / for usage\n"); });
  server.begin();
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
}

// The SDK remembers the last Wi-Fi mode in flash, so make station-only explicit once connected
static void closeSetupHotspot() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

static String randomPassword() {
  String password;
  for (uint8_t i = 0; i < SETUP_PASSWORD_DIGITS; i++) password += char('0' + ESP.random() % 10);
  return password;
}

static bool connectStarted = false; // set when setup() already called WiFi.begin() before the boot screen

// Tries the network saved in flash, with a live seconds counter. Returns false with a reason
// when there is no saved network or it couldn't connect.
static bool connectToSaved(String& failure) {
  String ssid = WiFi.SSID();
  if (ssid.isEmpty()) {
    failure = "No network saved yet";
    return false;
  }
  if (WiFi.status() == WL_CONNECTED) return true;

  showScreen(WHITE, "Connecting", {{"Saved network:", GREY}, {fit(utf8ToCp437(ssid))}, {}, {"Waiting...", GREY}});
  if (!connectStarted) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
  }
  connectStarted = false; // a later attempt (after the setup hotspot times out) starts afresh

  for (uint16_t s = 0; s < CONNECT_TIMEOUT_S; s++) {
    showRow(3, "Waiting... " + String(s) + "s / " + String(CONNECT_TIMEOUT_S) + "s", GREY);
    for (uint8_t i = 0; i < 10; i++) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(100);
    }
  }

  failure = fit(utf8ToCp437(ssid), 12) + ": " + describeFailure(WiFi.status());
  return false;
}

static String portalFailure;  // why the setup hotspot was opened, shown on its screen
static String portalPassword; // the setup hotspot's password for this session

static void onPortalStarted(WiFiManager*) {
  showScreen(YELLOW, "Wi-Fi setup",
             {{fit(portalFailure), RED},
              {"1. Join this hotspot:", GREY},
              {SETUP_AP_NAME, CYAN},
              {"   Password: " + portalPassword, CYAN},
              {"2. Pick your network in", GREY},
              {"   the page (192.168.4.1)", GREY}});
}

static void onCredentialsEntered() {
  showScreen(WHITE, "Connecting",
             {{"Got the details from", GREY}, {"the setup page."}, {}, {"Trying the network...", GREY}});
}

void setup() {
  tft.begin();
  tft.setRotation(1); // landscape, 320x240
  tft.cp437(true);    // see utf8ToCp437()

  // Start connecting to the saved network now, so it happens while the boot screen is up
  if (!WiFi.SSID().isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    connectStarted = true;
  }
  showBootScreen();

  // WiFiManager logs to Serial by default, which would toggle RS mid-transfer
  wifiManager.setDebugOutput(false);
  wifiManager.setConnectTimeout(CONNECT_TIMEOUT_S);
  wifiManager.setAPCallback(onPortalStarted);
  wifiManager.setPreSaveConfigCallback(onCredentialsEntered);

  // Saved network first; if that fails, the setup hotspot. With a saved network the hotspot
  // times out so a router that was just slow to come up gets retried.
  while (true) {
    if (connectToSaved(portalFailure)) break;
    wifiManager.setConfigPortalTimeout(WiFi.SSID().isEmpty() ? 0 : PORTAL_TIMEOUT_S);
    portalPassword = randomPassword();
    if (wifiManager.startConfigPortal(SETUP_AP_NAME, portalPassword.c_str())) break;
  }

  closeSetupHotspot();
  showConnected();
  startServer();
}

void loop() {
  static bool connected = true;
  static uint32_t lostSince = 0;
  static uint32_t lastCheck = 0;

  server.handleClient();
  MDNS.update();
  if (millis() - lastCheck < 500) {
    delay(2);
    return;
  }
  lastCheck = millis();

  bool now = WiFi.status() == WL_CONNECTED;
  if (now && !connected) {
    // Put back what was posted; otherwise the status screen, as the IP address may have changed
    if (shownJson.isEmpty() || showJson(shownJson).length()) showConnected();
    MDNS.notifyAPChange();
  } else if (!now && connected) {
    lostSince = millis();
    showScreen(RED, "Wi-Fi lost",
               {{"Reconnecting to:", GREY},
                {fit(utf8ToCp437(WiFi.SSID()))},
                {},
                {"Setup hotspot opens if it", GREY},
                {"isn't back in 2 minutes.", GREY}});
  } else if (!now && millis() - lostSince > LOST_RESTART_MS) {
    ESP.restart();
  }
  connected = now;
}

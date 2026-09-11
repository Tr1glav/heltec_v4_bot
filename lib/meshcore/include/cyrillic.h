#pragma once

// ============================================================================
// UTF-8 -> CP866 decoder + 6x8 Cyrillic glyph rendering for the OLED.
//
// The Adafruit_GFX classic 5x7 font has no Cyrillic, so received messages
// (UTF-8) are transcoded stream-style to CP866 bytes in the display's
// write() hook; bytes >= 0x80 are drawn from CYRILLIC_CP866_GLYPHS instead
// of the built-in font. ASCII keeps the classic font untouched.
// ============================================================================

#include <stdint.h>
#include <string.h>
#include <Adafruit_GFX.h>
#include "cyrillic_glyphs.h"

namespace utf8cp866 {

// Hand-tuned overrides for TTF-derived glyphs that do not survive the 6x8
// raster well. Key = CP866 byte; value = 8 row bytes (MSB = col 0).
static const uint8_t OVERRIDE_SH[8] = { 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xFC, 0xFC };

inline void fetchGlyph(uint8_t cp866, uint8_t (&out)[8]) {
  if (cp866 < 0x80 || cp866 > 0xF1) {
    memset(out, 0, 8);
    return;
  }
  memcpy(out, CYRILLIC_CP866_GLYPHS[cp866 - 0x80], 8);
  if (cp866 == 0x98) memcpy(out, OVERRIDE_SH, 8);  // Ш
}

inline uint8_t unicodeToCp866(uint32_t cp) {
  if (cp >= 0x0410 && cp <= 0x041F) return 0x80 + (uint8_t)(cp - 0x0410);  // А-П
  if (cp >= 0x0420 && cp <= 0x042F) return 0x90 + (uint8_t)(cp - 0x0420);  // Р-Я
  if (cp >= 0x0430 && cp <= 0x043F) return 0xA0 + (uint8_t)(cp - 0x0430);  // а-п
  if (cp >= 0x0440 && cp <= 0x044F) return 0xE0 + (uint8_t)(cp - 0x0440);  // р-я
  if (cp == 0x0401) return 0xF0;                                           // Ё
  if (cp == 0x0451) return 0xF1;                                           // ё
  return '?';
}

// Streaming UTF-8 -> CP866 decoder. Feed bytes one at a time; returns true
// and fills `out` only when a whole character has been decoded.
struct Decoder {
  uint32_t cp = 0;
  uint8_t need = 0;

  void reset() { cp = 0; need = 0; }

  bool decode(uint8_t in, uint8_t &out) {
    if (need == 0) {
      if (in < 0x80) { out = in; return true; }               // ASCII
      if (in >= 0xC0 && in <= 0xDF) { cp = in & 0x1F; need = 1; return false; }
      if (in >= 0xE0 && in <= 0xEF) { cp = in & 0x0F; need = 2; return false; }
      if (in >= 0xF0 && in <= 0xF7) { cp = in & 0x07; need = 3; return false; }
      out = '?'; return true;                                 // stray continuation
    }
    if (in >= 0x80 && in <= 0xBF) {
      cp = (cp << 6) | (in & 0x3F);
      if (--need == 0) { out = unicodeToCp866(cp); return true; }
      return false;
    }
    // invalid continuation (e.g. text cut in the middle of a sequence):
    // drop what was pending and re-process the current byte.
    reset();
    return decode(in, out);
  }
};

// Draw one CP866 byte at (x,y). Bytes < 0x80 go to the classic GFX font.
inline void drawCp866Glyph(Adafruit_GFX &g, int16_t x, int16_t y, uint8_t cp866,
                           uint16_t color, uint8_t sx, uint8_t sy) {
  if (cp866 < 0x80) {
    g.drawChar(x, y, (unsigned char)cp866, color, 0, sx, sy);
    return;
  }
  uint8_t rows[8];
  fetchGlyph(cp866, rows);
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 6; c++) {
      if (rows[r] & (0x80 >> c)) {
        g.writePixel(x + c * sx, y + r * sy, color);
      }
    }
  }
}

// Body shared by every display class: decode one byte; swallow UTF-8
// continuations; handle newline; draw ASCII via the classic font and
// Cyrillic manually, with the classic 6px advance / 8px line height.
// NOTE: never calls display.write() here (would recurse through the
// virtual override) - the whole cursor/advance logic is replicated.
inline size_t processByte(Adafruit_GFX &display, Decoder &u8, uint8_t c,
                          uint16_t color, uint8_t sx, uint8_t sy) {
  uint8_t d;
  if (!u8.decode(c, d)) return 1;
  if (d == '\r') { u8.reset(); return 1; }
  if (d == '\n') {
    u8.reset();
    display.setCursor(0, display.getCursorY() + 8 * sy);
    return 1;
  }
  if (d < 0x20) { return 1; }  // skip other control chars (tab etc.)
  if (display.getCursorX() + 6 * sx > display.width()) {
    display.setCursor(0, display.getCursorY() + 8 * sy);
  }
  drawCp866Glyph(display, display.getCursorX(), display.getCursorY(), d, color, sx, sy);
  display.setCursor(display.getCursorX() + 6 * sx, display.getCursorY());
  return 1;
}

}  // namespace utf8cp866
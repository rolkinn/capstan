// ui_font.h — bit-packed pixel font glyphs (compiled from ASCII art in ui_font.cpp)
#pragma once
#include <stdint.h>

#if defined(ARDUINO_TEENSY_MICROMOD)
using GlyphRowBits = uint16_t;
constexpr int GLYPH_MAX_ROWS = 15;
#else
using GlyphRowBits = uint8_t;
constexpr int GLYPH_MAX_ROWS = 7;
#endif

struct GlyphRowList {
  const char *r[GLYPH_MAX_ROWS];
  constexpr GlyphRowList(
      const char *r0 = nullptr, const char *r1 = nullptr, const char *r2 = nullptr,
      const char *r3 = nullptr, const char *r4 = nullptr, const char *r5 = nullptr,
      const char *r6 = nullptr
#if defined(ARDUINO_TEENSY_MICROMOD)
      , const char *r7 = nullptr, const char *r8 = nullptr, const char *r9 = nullptr,
      const char *r10 = nullptr, const char *r11 = nullptr, const char *r12 = nullptr,
      const char *r13 = nullptr, const char *r14 = nullptr
#endif
  ) : r{r0, r1, r2, r3, r4, r5, r6
#if defined(ARDUINO_TEENSY_MICROMOD)
        , r7, r8, r9, r10, r11, r12, r13, r14
#endif
       } {}
};

struct Glyph {
  char c;
  uint8_t width;
  GlyphRowBits rows[GLYPH_MAX_ROWS];

  constexpr Glyph(char ch, GlyphRowList list) : c(ch), width(0), rows{} {
    if (list.r[0]) {
      uint8_t w = 0;
      while (list.r[0][w] != '\0') w++;
      width = w;
    }
    for (int i = 0; i < GLYPH_MAX_ROWS; i++) {
      GlyphRowBits bits = 0;
      const char *s = list.r[i];
      if (s) {
        for (int j = 0; s[j] != '\0'; j++) {
          if (s[j] == '#') {
            bits |= (GlyphRowBits)(1u << j);
          }
        }
      }
      rows[i] = bits;
    }
  }
};

const Glyph *glyphFor(char c);                              // nullptr: not in the font
inline int glyphWidth(const Glyph &g) { return g.width; }   // in glyph cells (multiply by FONT_SCALE for pixels)

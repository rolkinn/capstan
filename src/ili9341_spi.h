// ili9341_spi.h — 320x240 ILI9341 4-wire SPI display driver for M8 Model:01 (Teensy 4.0).
// Self-contained FLASHMEM replacement for ILI9341_t3n implementing only the primitives used
// by Capstan's Surface compositor (begin, setRotation, invertDisplay, fillScreen, fillRect,
// drawRect, writeRect, sendCmd), reclaiming ~26.9 KB ITCM + 1.28 KB DTCM on Model:01.
#pragma once
#include <Arduino.h>
#include <SPI.h>

class ILI9341Spi {
public:
  ILI9341Spi(SPIClass &spi, uint8_t cs, uint8_t dc, uint8_t rst)
    : _spi(spi), _cs(cs), _dc(dc), _rst(rst) {}
  void begin(uint32_t hz);                  // Hardware reset, register initialization, sleep out, display on
  void setRotation(uint8_t r);              // 0..3 rotation (1 and 3 are 320x240 landscape)
  void invertDisplay(bool on);
  void setClipRect() { _cx = 0; _cy = 0; _cw = _w; _ch = _h; }   // Full viewport
  void fillScreen(uint16_t c) { fillRect(0, 0, _w, _h, c); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    fillRect(x, y, w, 1, c);
    if (h > 1) fillRect(x, y + h - 1, w, 1, c);
    if (h > 2) {
      fillRect(x, y + 1, 1, h - 2, c);
      if (w > 1) fillRect(x + w - 1, y + 1, 1, h - 2, c);
    }
  }
  void writeRect(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px);   // RGB565, w x h, row by row
  void sendCmd(uint8_t cmd, const uint8_t *data, uint8_t n);   // Send one command with optional data payload
  int16_t width() const { return _w; }
  int16_t height() const { return _h; }

private:
  void start();                             // Begin SPI transaction and assert CS
  void stop();
  void cmd(uint8_t c);                      // Pull D/C low for command byte, then restore D/C high
  void data(const uint8_t *p, size_t n) { _spi.transfer(p, nullptr, n); }
  void window(int16_t x0, int16_t y0, int16_t x1, int16_t y1);   // CASET (0x2A), PASET (0x2B), RAMWR (0x2C)
  bool clip(int16_t &x, int16_t &y, int16_t &w, int16_t &h, int16_t &dx, int16_t &dy) const;

  SPIClass &_spi;
  uint8_t _cs, _dc, _rst;
  uint32_t _hz = 30000000;
  int16_t _pw = 240, _ph = 320;             // Physical panel dimensions (portrait until setRotation())
  int16_t _w = 240, _h = 320;               // Logical UI viewport dimensions (320x240 in landscape)
  int16_t _cx = 0, _cy = 0, _cw = 240, _ch = 320;
};

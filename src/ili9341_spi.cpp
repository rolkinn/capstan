// ili9341_spi.cpp — 320x240 ILI9341 SPI display driver implementation for M8 Model:01 (see ili9341_spi.h).
// Compiled only on Teensy 4.0 (Model:01); compiles to an empty unit on Teensy MicroMod (Model:02).
#if !defined(ARDUINO_TEENSY_MICROMOD)
#include "ili9341_spi.h"

// ILI9341 register initialization sequence (length-prefixed: [1 + data_len, cmd, data...], terminated by 0).
PROGMEM static const uint8_t INIT[] = {
  4, 0xEF, 0x03, 0x80, 0x02,
  4, 0xCF, 0x00, 0xC1, 0x30,
  5, 0xED, 0x64, 0x03, 0x12, 0x81,
  4, 0xE8, 0x85, 0x00, 0x78,
  6, 0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02,
  2, 0xF7, 0x20,
  3, 0xEA, 0x00, 0x00,
  2, 0xC0, 0x23,                       // Power Control 1 (PWCTR1)
  2, 0xC1, 0x10,                       // Power Control 2 (PWCTR2)
  3, 0xC5, 0x3E, 0x28,                 // VCOM Control 1 (VMCTR1)
  2, 0xC7, 0x86,                       // VCOM Control 2 (VMCTR2)
  2, 0x36, 0x48,                       // Memory Access Control (MADCTL; updated by setRotation())
  2, 0x3A, 0x55,                       // Interface Pixel Format (COLMOD): 16-bit/pixel (RGB565)
  3, 0xB1, 0x00, 0x18,                 // Frame Rate Control (Normal Mode)
  4, 0xB6, 0x08, 0x82, 0x27,           // Display Function Control (DFUNCTR)
  2, 0xF2, 0x00,                       // Gamma Function Disable (3Gamma)
  2, 0x26, 0x01,                       // Gamma Curve Selected (GAMMASET)
  16, 0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00, // PGAMCTRL
  16, 0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F, // NGAMCTRL
  3, 0xB1, 0x00, 0x10,                 // Frame Rate Control 119 Hz
  0
};

// MADCTL orientation values for rotations 0..3 (MX=0x40, MY=0x80, MV=0x20, BGR=0x08).
PROGMEM static const uint8_t MADCTL[4] = {0x48, 0x28, 0x88, 0xE8};

FLASHMEM void ILI9341Spi::start() {
  _spi.beginTransaction(SPISettings(_hz, MSBFIRST, SPI_MODE0));
  if (_cs != _dc) digitalWriteFast(_cs, LOW);
}

FLASHMEM void ILI9341Spi::stop() {
  if (_cs != _dc) digitalWriteFast(_cs, HIGH);
  _spi.endTransaction();
}

FLASHMEM void ILI9341Spi::cmd(uint8_t c) {
  digitalWriteFast(_dc, LOW);
  _spi.transfer(c);
  digitalWriteFast(_dc, HIGH);
}

FLASHMEM void ILI9341Spi::begin(uint32_t hz) {
  _hz = hz;
  pinMode(_dc, OUTPUT);
  digitalWriteFast(_dc, HIGH);
  if (_cs != _dc) {
    pinMode(_cs, OUTPUT);
    digitalWriteFast(_cs, HIGH);
  }
  if (_rst != 255) {                        // Hardware reset pulse: 5 ms HIGH, 20 ms LOW, 150 ms recovery
    pinMode(_rst, OUTPUT);
    digitalWriteFast(_rst, HIGH);
    delay(5);
    digitalWriteFast(_rst, LOW);
    delay(20);
    digitalWriteFast(_rst, HIGH);
    delay(150);
  }
  _spi.setMOSI(11);
  _spi.setSCK(13);
  _spi.setMISO(12);
  _spi.begin();
  start();
  for (const uint8_t *p = INIT; *p; p += 1 + *p) {
    cmd(p[1]);
    data(p + 2, *p - 1);
  }
  cmd(0x11);                                // Sleep Out (SLPOUT)
  stop();
  delay(120);
  start();
  cmd(0x29);                                // Display On (DISPON)
  stop();
}

FLASHMEM void ILI9341Spi::sendCmd(uint8_t c, const uint8_t *d, uint8_t n) {
  start();
  cmd(c);
  if (n) data(d, n);
  stop();
}

FLASHMEM void ILI9341Spi::setRotation(uint8_t r) {
  r &= 3;
  bool landscape = (r & 1) != 0;
  _pw = landscape ? 320 : 240;
  _ph = landscape ? 240 : 320;
  _w  = _pw;
  _h  = _ph;
  setClipRect();
  uint8_t m = MADCTL[r];
  sendCmd(0x36, &m, 1);
}

FLASHMEM void ILI9341Spi::invertDisplay(bool on) { sendCmd(on ? 0x21 : 0x20, nullptr, 0); }

FLASHMEM bool ILI9341Spi::clip(int16_t &x, int16_t &y, int16_t &w, int16_t &h, int16_t &dx, int16_t &dy) const {
  int16_t x0 = max(x, _cx), y0 = max(y, _cy);
  int16_t x1 = min((int)x + w, (int)_cx + _cw), y1 = min((int)y + h, (int)_cy + _ch);
  if (x1 <= x0 || y1 <= y0) return false;
  dx = x0 - x; dy = y0 - y;
  x = x0; y = y0; w = x1 - x0; h = y1 - y0;
  return true;
}

FLASHMEM void ILI9341Spi::window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  uint8_t c[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t p[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmd(0x2A); data(c, 4);
  cmd(0x2B); data(p, 4);
  cmd(0x2C);
}

FLASHMEM static inline void waitLpspi4Idle() {
  while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) != 0u) {}
  while (IMXRT_LPSPI4_S.SR & LPSPI_SR_MBF) {}
}

FLASHMEM static inline uint32_t enter16BitTxOnlyMode() {
  waitLpspi4Idle();
  const uint32_t origTcr = IMXRT_LPSPI4_S.TCR;
  IMXRT_LPSPI4_S.TCR = (origTcr & ~(LPSPI_TCR_FRAMESZ(0xFFF) | LPSPI_TCR_RXMSK))
                     | LPSPI_TCR_FRAMESZ(15) | LPSPI_TCR_RXMSK;
  return origTcr;
}

FLASHMEM static inline void exit16BitTxOnlyMode(uint32_t origTcr) {
  waitLpspi4Idle();
  IMXRT_LPSPI4_S.TCR = origTcr;
  waitLpspi4Idle();
  while (!(IMXRT_LPSPI4_S.RSR & LPSPI_RSR_RXEMPTY)) {
    (void)IMXRT_LPSPI4_S.RDR;
  }
}

FLASHMEM void ILI9341Spi::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t dx, dy;
  if (!clip(x, y, w, h, dx, dy)) return;
  start();
  window(x, y, x + w - 1, y + h - 1);
  const uint32_t origTcr = enter16BitTxOnlyMode();
  const int total = (int)w * (int)h;
  for (int i = 0; i < total; i++) {
    while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) >= 15u) {}
    IMXRT_LPSPI4_S.TDR = color;
  }
  exit16BitTxOnlyMode(origTcr);
  stop();
}

FLASHMEM void ILI9341Spi::writeRect(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
  const int16_t stride = w;
  int16_t dx, dy;
  if (!clip(x, y, w, h, dx, dy)) return;
  start();
  window(x, y, x + w - 1, y + h - 1);
  const uint32_t origTcr = enter16BitTxOnlyMode();
  for (int16_t r = 0; r < h; r++) {
    const uint16_t *row = px + (dy + r) * stride + dx;
    for (int16_t i = 0; i < w; i++) {
      while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) >= 15u) {}
      IMXRT_LPSPI4_S.TDR = row[i];
    }
  }
  exit16BitTxOnlyMode(origTcr);
  stop();
}

#endif  // !ARDUINO_TEENSY_MICROMOD

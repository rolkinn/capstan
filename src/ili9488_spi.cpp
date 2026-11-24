// ili9488_spi.cpp — 480x320 ILI9488 SPI display driver implementation for M8 Model:02 (see ili9488_spi.h).
// Compiled only on Teensy MicroMod (Model:02); compiles to an empty unit on Teensy 4.0 (Model:01).
#if defined(ARDUINO_TEENSY_MICROMOD)
#include "ili9488_spi.h"

// ILI9488 register initialization sequence (length-prefixed: [1 + data_len, cmd, data...], terminated by 0).
static const uint8_t INIT[] PROGMEM = {
  3, 0xC0, 0x01, 0x1F,                 // Power Control 1 (PWCTR1)
  2, 0xC1, 0x40,                       // Power Control 2 (PWCTR2)
  2, 0xC2, 0x00,                       // Power Control 3 (PWCTR3)
  4, 0xC5, 0x00, 0x48, 0x80,           // VCOM Control (VMCTR1)
  2, 0x36, 0x48,                       // Memory Access Control (MADCTL; updated by setRotation())
  2, 0x3A, 0x66,                       // Interface Pixel Format (COLMOD): 18-bit/pixel (RGB666) for SPI
  2, 0xB0, 0x00,                       // Interface Mode Control
  2, 0xB1, 0xC0,                       // Frame Rate Control (Normal Mode)
  2, 0xB4, 0x02,                       // Display Inversion Control (2-dot inversion)
  3, 0xB6, 0x02, 0x02,                 // Display Function Control
  16, 0xE0, 0x00, 0x08, 0x0C, 0x02, 0x0E, 0x04, 0x30, 0x45, 0x47, 0x04, 0x0C, 0x0A, 0x2E, 0x34, 0x0F,   // Positive Gamma Control (PGAMCTRL)
  16, 0xE1, 0x00, 0x11, 0x0D, 0x01, 0x0F, 0x05, 0x39, 0x36, 0x51, 0x06, 0x0F, 0x0D, 0x33, 0x37, 0x0F,   // Negative Gamma Control (NGAMCTRL)
  2, 0xE9, 0x00,                       // Set Image Function (24-bit data bus disable)
  5, 0xF7, 0xA9, 0x51, 0x2C, 0x82,     // Adjust Control 3 (DSI/SPI packet stream config)
  3, 0xF8, 0x21, 0x07,                 // Adjust Control 4
  0
};

// MADCTL orientation values for rotations 0..3 (MX=0x40, MY=0x80, MV=0x20, BGR=0x08).
PROGMEM static const uint8_t MADCTL[4] = {0x48, 0x28, 0x88, 0xE8};

FLASHMEM static inline uint32_t packRgb666(uint16_t c) { // Pack RGB565 into 24-bit RGB666 word (MSB = R[23:16], G[15:8], B[7:0])
  return ((uint32_t)(c & 0xF800u) << 8)
       | ((uint32_t)(c & 0x07E0u) << 5)
       | ((uint32_t)(c & 0x001Fu) << 3);
}

FLASHMEM static inline void waitLpspi4Idle() {
  while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) != 0u) {}
  while (IMXRT_LPSPI4_S.SR & LPSPI_SR_MBF) {}
}

FLASHMEM static inline uint32_t enter24BitTxOnlyMode() {
  waitLpspi4Idle();
  const uint32_t origTcr = IMXRT_LPSPI4_S.TCR;
  IMXRT_LPSPI4_S.TCR = (origTcr & ~(LPSPI_TCR_FRAMESZ(0xFFF) | LPSPI_TCR_RXMSK))
                     | LPSPI_TCR_FRAMESZ(23) | LPSPI_TCR_RXMSK;
  return origTcr;
}

FLASHMEM static inline void exit24BitTxOnlyMode(uint32_t origTcr) {
  waitLpspi4Idle();
  IMXRT_LPSPI4_S.TCR = origTcr;
  waitLpspi4Idle();
  while (!(IMXRT_LPSPI4_S.RSR & LPSPI_RSR_RXEMPTY)) {
    (void)IMXRT_LPSPI4_S.RDR;
  }
}

FLASHMEM void ILI9488Spi::start() {
  _spi.beginTransaction(SPISettings(_hz, MSBFIRST, SPI_MODE0));
  if (_cs != _dc) digitalWriteFast(_cs, LOW);
}
FLASHMEM void ILI9488Spi::stop() {
  if (_cs != _dc) digitalWriteFast(_cs, HIGH);
  _spi.endTransaction();
}
FLASHMEM void ILI9488Spi::cmd(uint8_t c) {
  digitalWriteFast(_dc, LOW);
  _spi.transfer(c);
  digitalWriteFast(_dc, HIGH);
}

FLASHMEM void ILI9488Spi::begin(uint32_t hz) {
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

FLASHMEM void ILI9488Spi::sendCmd(uint8_t c, const uint8_t *d, uint8_t n) {
  start();
  cmd(c);
  if (n) data(d, n);
  stop();
}

FLASHMEM void ILI9488Spi::setRotation(uint8_t r) {
  r &= 3;
  bool landscape = r & 1;
  _pw = landscape ? 480 : 320;
  _ph = landscape ? 320 : 480;
  _w  = _pw;
  _h  = _ph;
  _ox = 0;
  _oy = 0;
  setClipRect();
  uint8_t m = MADCTL[r];
  sendCmd(0x36, &m, 1);
}

FLASHMEM void ILI9488Spi::clearPanel(uint16_t color) {
  const uint32_t px24 = packRgb666(color);
  const int total = (int)_pw * (int)_ph;
  start();
  uint8_t c[4] = {0, 0, (uint8_t)((_pw - 1) >> 8), (uint8_t)(_pw - 1)};
  uint8_t p[4] = {0, 0, (uint8_t)((_ph - 1) >> 8), (uint8_t)(_ph - 1)};
  cmd(0x2A); data(c, 4);
  cmd(0x2B); data(p, 4);
  cmd(0x2C);
  const uint32_t origTcr = enter24BitTxOnlyMode();
  for (int i = 0; i < total; i++) {
    while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) >= 15u) {}
    IMXRT_LPSPI4_S.TDR = px24;
  }
  exit24BitTxOnlyMode(origTcr);
  stop();
}

FLASHMEM void ILI9488Spi::invertDisplay(bool on) { sendCmd(on ? 0x21 : 0x20, nullptr, 0); }

FLASHMEM bool ILI9488Spi::clip(int16_t &x, int16_t &y, int16_t &w, int16_t &h, int16_t &dx, int16_t &dy) const {
  int16_t x0 = max(x, _cx), y0 = max(y, _cy);
  int16_t x1 = min((int)x + w, (int)_cx + _cw), y1 = min((int)y + h, (int)_cy + _ch);
  if (x1 <= x0 || y1 <= y0) return false;
  dx = x0 - x; dy = y0 - y;                 // Offset into caller's source rectangle where visible region starts
  x = x0; y = y0; w = x1 - x0; h = y1 - y0;
  return true;
}

FLASHMEM void ILI9488Spi::window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  x0 += _ox; x1 += _ox;
  y0 += _oy; y1 += _oy;
  uint8_t c[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t p[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmd(0x2A); data(c, 4);
  cmd(0x2B); data(p, 4);
  cmd(0x2C);
}

FLASHMEM void ILI9488Spi::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t dx, dy;
  if (!clip(x, y, w, h, dx, dy)) return;
  const uint32_t px24 = packRgb666(color);
  start();
  window(x, y, x + w - 1, y + h - 1);
  const uint32_t origTcr = enter24BitTxOnlyMode();
  const int total = (int)w * (int)h;
  for (int i = 0; i < total; i++) {
    while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) >= 15u) {}
    IMXRT_LPSPI4_S.TDR = px24;
  }
  exit24BitTxOnlyMode(origTcr);
  stop();
}

FLASHMEM void ILI9488Spi::writeRect(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
  const int16_t stride = w;
  int16_t dx, dy;
  if (!clip(x, y, w, h, dx, dy)) return;
  start();
  window(x, y, x + w - 1, y + h - 1);
  const uint32_t origTcr = enter24BitTxOnlyMode();
  for (int16_t r = 0; r < h; r++) {
    const uint16_t *row = px + (dy + r) * stride + dx;
    for (int16_t i = 0; i < w; i++) {
      while ((IMXRT_LPSPI4_S.FSR & 0x1Fu) >= 15u) {}
      IMXRT_LPSPI4_S.TDR = packRgb666(row[i]);
    }
  }
  exit24BitTxOnlyMode(origTcr);
  stop();
}

#endif  // ARDUINO_TEENSY_MICROMOD

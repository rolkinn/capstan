// M8Board.cpp — Hardware abstraction implementation for M8 Model:01 & Model:02 (see M8Board.h and docs/HARDWARE.md)
#include "M8Board.h"
#include "config.h"

namespace m8 {

static constexpr uint8_t EXPANDER_ADDR = 0x20;
static constexpr uint8_t VARIANT_DETECT_PIN = 17;

//                      CS  DC RST  BL  BL-Hz    rot  keyBus   INT  SDdet HPsense HPen  MIDI out  MIDI in
const Pins PINS_A = {   10,  2,  3,  6,   261.0f,  3, &Wire1,    5,    4,    26,   30, &Serial7, &Serial7 };
const Pins PINS_B = {   10,  6,  5,  4, 44100.0f,  1, &Wire,    15,   17,    22,    9, &Serial3, &Serial1 };
// Model:02 pin configuration: shares Variant B peripheral routing with an ILI9488 480x320 SPI
// panel (D/C on pin 10, panel CS tied low so cs == dc == 10, RST on pin 5, landscape rotation 3).
#if M8_MODEL == 2
const Pins PINS_M02 = { 10, 10,  5,  4, 44100.0f,  3, &Wire,    15,   17,    22,    9, &Serial3, &Serial1 };
#endif

#if M8_MODEL == 2
class M8Display : public ILI9488Spi {
public:
  using ILI9488Spi::ILI9488Spi;
};
#else
// ILI9341_t3n wrapper exposing command transmission for Variant B IPS gamma/power register tuning.
class M8Display : public ILI9341_t3n {
public:
  using ILI9341_t3n::ILI9341_t3n;
  size_t write(uint8_t) override { return 1; }
  void sendCmd(uint8_t cmd, const uint8_t *data, uint8_t n) {
    beginSPITransaction(_SPI_CLOCK);
    if (n == 0) {
      writecommand_last(cmd);
    } else {
      writecommand_cont(cmd);
      for (uint8_t i = 0; i < n; i++) {
        if (i == n - 1) writedata8_last(data[i]);
        else            writedata8_cont(data[i]);
      }
    }
    endSPITransaction();
  }
};
#endif

FLASHMEM bool Board::begin(uint32_t spiHz) {
#if M8_MODEL == 2
  // --- 1. Select Model:02 board configuration ---
  (void)VARIANT_DETECT_PIN;
  _variant = VARIANT_B;
  _pins = &PINS_M02;
#else
  // --- 1. Detect Model:01 board revision via pin 17 pull-down check ---
  pinMode(VARIANT_DETECT_PIN, INPUT_PULLDOWN);
  delay(1);
  _variant = digitalRead(VARIANT_DETECT_PIN) ? VARIANT_A : VARIANT_B;
  _pins = (_variant == VARIANT_A) ? &PINS_A : &PINS_B;
#endif
  const Pins &p = *_pins;

  // --- 2. Early power-up rail sequencing ---
  if (_variant == VARIANT_B) {
    pinMode(1, OUTPUT);          // Keep board power enable line HIGH on Variant B / Model:02
    digitalWrite(1, HIGH);
    delay(30);
  }
#if M8_MODEL == 2
  // Model:02 hardware power latch (D flip-flop): pin 28 is Data (HIGH = ON,
  // LOW = OFF) and pin 6 is Clock (1 ms HIGH -> LOW pulse latches pin 28).
  pinMode(28, OUTPUT);
  digitalWrite(28, HIGH);
  pinMode(6, OUTPUT);
  digitalWrite(6, HIGH);
  delay(1);
  digitalWrite(6, LOW);
#endif
  pinMode(p.backlight, OUTPUT);  // Keep backlight off until the display panel is initialized
  digitalWrite(p.backlight, LOW);

  Wire.begin();
  Wire.setClock(400000);
  if (_variant == VARIANT_A) {
    Wire1.begin();
    Wire1.setClock(400000);
  }

#if M8_MODEL == 2
  pinMode(40, OUTPUT);           // Model:02 peripheral reset/enable pulse before charger init
  digitalWrite(40, LOW);
  delay(10);
  digitalWrite(40, HIGH);
#endif
  if (_variant == VARIANT_B) initChargerB();

  pinMode(p.hpEnable, OUTPUT);   // Keep speaker amp off until codec & coupling capacitors settle
  digitalWrite(p.hpEnable, LOW);
  pinMode(p.hpSense, INPUT_PULLDOWN);
  pinMode(p.sdDetect, INPUT_PULLUP);

  // --- 3. Key Expander ---
  pinMode(p.keyInt, _variant == VARIANT_A ? INPUT : INPUT_PULLUP);
  bool keysOk = (_variant == VARIANT_A) ? initMCP23008() : initSX1508();

  // --- 4. Display ---
#if M8_MODEL == 2
  // ILI9488 480x320 panel on LPSPI4 (MOSI 11, SCK 13, MISO 12) at 56 MHz SPI clock
  // with display inversion enabled for IPS color polarity.
  (void)spiHz;
  M8Display *d = new M8Display(SPI, p.tftCS, p.tftDC, p.tftRST);
  _tft = d;
  d->begin(56000000);
  d->invertDisplay(true);
#else
  // ILI9341 320x240 panel on LPSPI4 (MOSI 11, SCK 13, MISO 12)
  M8Display *d = new M8Display(p.tftCS, p.tftDC, p.tftRST, 11, 13, 12);
  _tft = d;
  d->begin(spiHz);
  if (_variant == VARIANT_B) {
    applyVariantBPanelTuning();
    // Variant B uses an IPS ILI9341 panel that requires display inversion (INVON 0x21).
    d->invertDisplay(true);
  }
#endif
  d->setRotation(p.tftRotation);
#if M8_MODEL == 2
  d->clearPanel(0);
#else
  d->fillScreen(0);
#endif

  // --- 5. Backlight PWM ---
  analogWriteFrequency(p.backlight, p.backlightPwmHz);
  setBacklight(125);

  return keysOk;
}

#if M8_MODEL != 2
FLASHMEM void Board::applyVariantBPanelTuning() {
  // ILI9341 power control (PWCTR1/PWCTR2) and positive/negative gamma curves (GMCTRP1/GMCTRN1)
  // calibrated for the Variant B IPS LCD panel.
  static const uint8_t pwctr1[] PROGMEM = {0x16};
  static const uint8_t pwctr2[] PROGMEM = {0x00};
  static const uint8_t gamset[] PROGMEM = {0x00};
  static const uint8_t gmctrp1[] PROGMEM = {0x0F,0x3F,0x2D,0x0C,0x10,0x0A,0x2C,0xD5,
                                    0x22,0x0A,0x13,0x03,0x08,0x03,0x00};
  static const uint8_t gmctrn1[] PROGMEM = {0x00,0x00,0x12,0x04,0x0F,0x05,0x2C,0xA2,
                                    0x3F,0x05,0x0E,0x0C,0x37,0x3C,0x0F};
  M8Display *d = static_cast<M8Display *>(_tft);
  d->sendCmd(0xC0, pwctr1, 1);
  d->sendCmd(0xC1, pwctr2, 1);
  d->sendCmd(0x26, gamset, 1);
  d->sendCmd(0xE0, gmctrp1, sizeof gmctrp1);
  d->sendCmd(0xE1, gmctrn1, sizeof gmctrn1);
}
#endif

FLASHMEM void Board::setBacklight(uint8_t level) { analogWrite(_pins->backlight, level); }

FLASHMEM HardwareSerial *Board::midiOutBegin() {
  HardwareSerial *port = _pins->midiOut;
  port->begin(31250);
  if (_variant == VARIANT_B) {
    pinMode(_pins->keyInt, INPUT_PULLUP);   // Reclaim pin 15 from Serial3 RX for key expander INT
    IMXRT_LPUART2.CTRL &= ~(LPUART_CTRL_RE | LPUART_CTRL_RIE | LPUART_CTRL_ILIE);
  }
  return port;
}

FLASHMEM HardwareSerial *Board::midiInBegin() {
  HardwareSerial *port = _pins->midiIn;
  if (port == _pins->midiOut) return port;         // Variant A shares Serial7 for both TX and RX
  port->begin(31250);
  if (_variant == VARIANT_B) {                     // Disable LPUART6 TX on pin 1 so pin 1 stays driven HIGH
    IMXRT_LPUART6.CTRL &= ~(LPUART_CTRL_TE | LPUART_CTRL_TIE | LPUART_CTRL_TCIE);
    pinMode(1, OUTPUT);
    digitalWrite(1, HIGH);
  }
  return port;
}

// ---------------- Power button (IMXRT1062 SNVS On/Off) ----------------
static constexpr uint32_t SNVS_LPSR_EO  = 1u << 17;  // Emergency-off status bit
static constexpr uint32_t SNVS_LPSR_SPO = 1u << 18;  // Set-power-off request bit (short press)
static constexpr uint32_t SNVS_HPSR_BTN = 1u << 6;   // Live button state bit

FLASHMEM bool Board::powerButtonPressed() {
  if (!(SNVS_LPSR & SNVS_LPSR_SPO)) return false;
  SNVS_LPSR = SNVS_LPSR_SPO | SNVS_LPSR_EO;  // Write-1-to-clear interrupt status flags
  return true;
}

// ---- Headphones
FLASHMEM int Board::headphoneSenseRaw() { return analogRead(_pins->hpSense); }

FLASHMEM bool Board::headphonesChanged(bool &in) {
  if (_hpState >= 0 && millis() - _hpPolledMs < HP_POLL_MS) return false;
  _hpPolledMs = millis();
  bool now = headphoneSenseRaw() < HP_SENSE_THRESHOLD;   // Headphone plug pulls sense line low
  if (_hpState < 0) {
    int hits = now ? 1 : 0;
    hits += (headphoneSenseRaw() < HP_SENSE_THRESHOLD) ? 1 : 0;
    hits += (headphoneSenseRaw() < HP_SENSE_THRESHOLD) ? 1 : 0;
    now = (hits >= 2);
    _hpLast = now;
    _hpCount = HP_DEBOUNCE;
    _hpState = (int8_t)now;
    in = now;
    return true;
  }
  _hpCount = now == _hpLast ? min(_hpCount + 1, 255) : 1;
  _hpLast = now;
  if (_hpCount < HP_DEBOUNCE || _hpState == (int8_t)now) return false;
  _hpState = now;
  in = now;
  return true;
}

FLASHMEM void Board::setSpeaker(bool on) { digitalWrite(_pins->hpEnable, on ? HIGH : LOW); }

FLASHMEM bool Board::powerButtonHeld() { return SNVS_HPSR & SNVS_HPSR_BTN; }

// Variant B / Model:02: TI BQ2589x battery charger @ 0x6A on Wire.
// Configures 512 mA fast-charge current, 3.3 V minimum system voltage, continuous ADC
// monitoring, and disables the 40 s I2C watchdog timer.
FLASHMEM static bool bqWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x6A);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
FLASHMEM static int bqRead(uint8_t reg) {
  Wire.beginTransmission(0x6A);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)0x6A, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

FLASHMEM void Board::powerOff() {
  setBacklight(0);
  if (_pins) {
    pinMode(_pins->backlight, OUTPUT);
    digitalWrite(_pins->backlight, LOW);
  }
  setSpeaker(false);

  if (_tft) {
    M8Display *d = static_cast<M8Display *>(_tft);
    d->sendCmd(0x28, nullptr, 0);  // Display Off
    d->sendCmd(0x10, nullptr, 0);  // Sleep In
  }

  // Shut down MIDI Out UART and de-energize MIDI Out TX pin immediately
  if (_pins && _pins->midiOut) {
    _pins->midiOut->end();
  }
  if (_variant == VARIANT_A) {
    pinMode(29, INPUT_DISABLE);
  } else {
    pinMode(14, INPUT_DISABLE);
    if (_pins) pinMode(_pins->keyInt, INPUT_PULLUP);
  }

  // Wait for the user to release the physical power button (plus debounce)
  // before shutting down board rails and asserting SNVS_LPCR_TOP so the
  // held button does not immediately re-trigger SNVS wakeup.
  while (powerButtonHeld()) {
  }
  delay(50);

  if (_pins && _pins->midiIn && _pins->midiIn != _pins->midiOut) {
    _pins->midiIn->end();
  }
  if (_variant == VARIANT_A) {
    pinMode(28, INPUT_DISABLE);
    pinMode(29, INPUT_DISABLE);
  } else {
    pinMode(0, INPUT_DISABLE);
    pinMode(14, INPUT_DISABLE);
  }

  delay(100);

  if (_variant == VARIANT_B) {
    // Stop BQ2589x continuous ADC conversion and disable 5V OTG boost converter
    // (OTG_CONFIG=0) while preserving autonomous USB battery charging.
    bqWrite(0x02, 0x00);
    bqWrite(0x03, _cgPaused ? 0x06 : 0x16);
#if M8_MODEL == 2
    // Model:02 power-latch shutdown sequence:
    // 1. De-assert pin 40 and wait 10 ms.
    // 2. Drive pin 28 (power-latch Data) LOW and pulse pin 6 (power-latch Clock)
    //    HIGH for 1 ms then LOW to unlatch mainboard battery power.
    pinMode(40, OUTPUT);
    digitalWrite(40, LOW);
    delay(10);
    pinMode(28, OUTPUT);
    digitalWrite(28, LOW);
    pinMode(6, OUTPUT);
    digitalWrite(6, HIGH);
    delay(1);
    digitalWrite(6, LOW);
    delay(1000);
#endif
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
  }

  SNVS_LPSR = SNVS_LPSR_SPO | SNVS_LPSR_EO;
  noInterrupts();
  SNVS_LPCR |= SNVS_LPCR_DP_EN | SNVS_LPCR_TOP;  // Assert SNVS Turn-Off-Power and wait for interrupt
  asm volatile("dsb");
  while (true) asm volatile("wfi");
}

// STMicroelectronics STC3115 fuel gauge @ 0x70 on Wire: read n consecutive registers starting at reg.
FLASHMEM static bool gaugeRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(0x70);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x70, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// Piecewise-linear Li-Ion state-of-charge lookup from open-circuit cell voltage (mV).
FLASHMEM static int percentForMv(int mv) {
  static const int16_t MV[] PROGMEM = {3300, 3500, 3600, 3650, 3700, 3750, 3800, 3900, 4000, 4100, 4200};
  static const int8_t PCT[] PROGMEM = {0, 5, 10, 20, 30, 40, 50, 65, 80, 90, 100};
  if (mv <= MV[0]) return 0;
  for (int i = 1; i < 11; i++)
    if (mv < MV[i]) return PCT[i - 1] + (PCT[i] - PCT[i - 1]) * (mv - MV[i - 1]) / (MV[i] - MV[i - 1]);
  return 100;
}

FLASHMEM int Board::batteryPercent() {
  if (_variant != VARIANT_B) return -1;
  if (_batPolled && millis() - _batPolledMs < BATTERY_POLL_MS) return _batPct;
  _batPolled = true;
  _batPolledMs = millis();
  int pct = -1, mv = -1;
  uint8_t g[2];
  // When STC3115 REG_MODE bit 4 (GG_RUN) is active, read state of charge from REG_SOC (1/512 % units).
  _batGaugeMode = gaugeRead(0x00, g, 1) ? g[0] : -1;
  _batSoc = -1;
  if (_batGaugeMode >= 0 && (_batGaugeMode & 0x10) && gaugeRead(0x02, g, 2)) {
    int soc = (g[0] | g[1] << 8) / 512;
    _batSoc = soc;
    if (soc >= 0 && soc <= 100) pct = soc;
    if (gaugeRead(0x06, g, 2)) _batCurrent = (int16_t)(g[0] | g[1] << 8);   // REG_CURRENT: signed (+ = into battery)
    if (gaugeRead(0x08, g, 2)) _batGaugeMv = (int16_t)((g[0] | g[1] << 8) * 22 / 10);   // REG_VOLTAGE: 2.2 mV/LSB
  }
  int st = bqRead(0x0B);                 // BQ2589x REG0B: CHRG_STAT bits 4..3 (1 = pre-charge, 2 = fast charge, 3 = done)
  _batStatus = st;
  int chg = st < 0 ? 0 : (st >> 3) & 3;
  // Determine charging state from STC3115 net current when running, or fallback to BQ2589x CHRG_STAT.
  constexpr int CHARGING_CURRENT = 50;
  _batCharging = _batSoc >= 0 ? _batCurrent > CHARGING_CURRENT : chg == 1 || chg == 2;
  int v = bqRead(0x0E);                  // BQ2589x REG0E: BATV (2.304 V base + 20 mV/LSB)
  if (v >= 0) mv = 2304 + (v & 0x7F) * 20;
  _batMv = mv;
  if (pct < 0 && mv > 0) pct = percentForMv(mv);
  bool changed = pct != _batPct;
  _batPct = pct;
  if (changed) batteryLog();
  return _batPct;
}

FLASHMEM void Board::batteryLog() {
#if CAPSTAN_DEBUG
  Serial.printf("battery %d%%: gauge mode 0x%02x%s, its charge %d%%, current %d (raw), %d mV; charger REG0B 0x%02x (charging: %s), %d mV (%d%% by voltage)\n",
                _batPct, _batGaugeMode & 0xFF, _batGaugeMode < 0 ? " (no answer)" : (_batGaugeMode & 0x10) ? " (running)" : " (standby)",
                _batSoc, _batCurrent, _batGaugeMv, _batStatus & 0xFF, _batCharging ? "yes" : "no", _batMv, _batMv > 0 ? percentForMv(_batMv) : -1);
#endif
}

FLASHMEM bool Board::initChargerB() {
  delay(2);
  int r14 = bqRead(0x14);
  if (r14 < 0) return false;             // Charger did not acknowledge
  bqWrite(0x14, (uint8_t)r14 | 0x80);    // REG14 REG_RST: reset registers to default
  delay(10);
  bqWrite(0x02, 0xE0);  delay(20);       // REG02: enable continuous ADC conversion
  bqWrite(0x03, 0x36);  delay(10);       // REG03: CHG_CONFIG=1, OTG_CONFIG=1, SYS_MIN=3.3 V
  bqWrite(0x04, 0x08);                   // REG04: ICHG = 8 x 64 mA = 512 mA fast-charge limit
  bqWrite(0x07, 0x8D);                   // REG07: EN_TERM=1, I2C watchdog disabled, 12 h safety timer
  bqWrite(0x09, 0x44);                   // REG09: normal BATFET / pump configuration
  bqWrite(0x0A, 0x03);                   // REG0A: boost mode voltage/current regulation
  delay(20);
  return true;
}

FLASHMEM void Board::chargeGuard() {
  if (_variant != VARIANT_B || _cgLatched) return;
  uint32_t ms = millis();
  if (ms - _cgCheckedMs < (_cgPaused ? 300000u : 5000u)) return;
  _cgCheckedMs = ms;

  bqRead(0x0C);                          // First read clears latched historical faults;
  int fault = bqRead(0x0C);              // second read reports active fault state
  int vb = bqRead(0x0E);                 // Bit 7 THERM_STAT, bits 6..0 BATV (2.304 V + 20 mV/LSB)
  int ic = bqRead(0x12);                 // Measured charge current (50 mA/LSB)
  if (fault < 0 || vb < 0) return;

  int mv = 2304 + (vb & 0x7F) * 20;
  int ima = ic < 0 ? 0 : (ic & 0x7F) * 50;
  const char *why = nullptr;
  if (fault & 0xFF) why = "charger fault";
  else if (vb & 0x80) why = "thermal regulation";
  else if (mv > 4250) why = "battery over 4.25 V";
  else if (ima > 900) why = "charge current high";

  if (why) {
    if (!_cgPaused) {
      bqWrite(0x03, 0x26);               // Disable CHG_CONFIG while keeping OTG and SYS_MIN=3.3 V
      _cgPaused = true;
      _cgPausedMs = ms;
      if (++_cgStrikes >= 3) _cgLatched = true;
#if CAPSTAN_DEBUG
      Serial.printf("charge guard: charging paused (%s; fault 0x%02X, %d mV, %d mA)%s\n",
                    why, fault, mv, ima, _cgLatched ? " - latched off until the next boot" : "");
#endif
    }
  } else if (_cgPaused) {
    bqWrite(0x03, 0x36);                 // Clear to resume charging
    _cgPaused = false;
#if CAPSTAN_DEBUG
    Serial.printf("charge guard: charging resumed (%d mV, %d mA)\n", mv, ima);
#endif
  }
}

// ---------------- I2C expander helpers ----------------
FLASHMEM bool Board::writeReg(uint8_t reg, uint8_t val) {
  TwoWire &w = *_pins->keyBus;
  w.beginTransmission(EXPANDER_ADDR);
  w.write(reg);
  w.write(val);
  return w.endTransmission() == 0;
}

FLASHMEM int Board::readReg(uint8_t reg) {
  TwoWire &w = *_pins->keyBus;
  w.beginTransmission(EXPANDER_ADDR);
  w.write(reg);
  if (w.endTransmission(false) != 0) return -1;
  if (w.requestFrom(EXPANDER_ADDR, (uint8_t)1) != 1) return -1;
  return w.read();
}

// Variant A: Microchip MCP23008 @ 0x20 on Wire1 (SDA 17 / SCL 16), INT -> pin 5
FLASHMEM bool Board::initMCP23008() {
  bool ok = true;
  ok &= writeReg(0x05, 0x00);  // IOCON: INT active-low, push-pull output
  ok &= writeReg(0x00, 0xFF);  // IODIR: all 8 pins configured as inputs
  ok &= writeReg(0x06, 0xFF);  // GPPU: enable internal pull-ups on all pins
  ok &= writeReg(0x04, 0x00);  // INTCON: interrupt on any pin state change
  ok &= writeReg(0x02, 0xFF);  // GPINTEN: enable interrupt-on-change for all pins
  readReg(0x09);               // Read GPIO register once to clear any pending interrupt
  return ok;
}

// Variant B / Model:02: Semtech SX1508 @ 0x20 on Wire (SDA 18 / SCL 19), INT -> pin 15
FLASHMEM bool Board::initSX1508() {
  bool ok = true;
  ok &= writeReg(0x07, 0xFF);  // RegDir: all 8 pins configured as inputs
  ok &= writeReg(0x03, 0xFF);  // RegPullUp: enable internal pull-ups on all pins
  ok &= writeReg(0x0F, 0x40);  // RegClock: enable internal 2 MHz oscillator
  ok &= writeReg(0x10, 0x10);  // RegMisc: clkX = fOSC/1 for hardware key debounce
  ok &= writeReg(0x12, 0x05);  // RegDebounceConfig: 16 ms debounce time
  ok &= writeReg(0x13, 0xFF);  // RegDebounceEnable: enable debounce on all pins
  ok &= writeReg(0x0A, 0xFF);  // RegSenseHigh: trigger interrupt on both edges (pins 7..4)
  ok &= writeReg(0x0B, 0xFF);  // RegSenseLow:  trigger interrupt on both edges (pins 3..0)
  ok &= writeReg(0x09, 0x00);  // RegInterruptMask: unmask all pin interrupts
  writeReg(0x0C, 0xFF);        // Clear RegInterruptSource
  return ok;
}

FLASHMEM int Board::readExpander() {
  int v;
  if (_variant == VARIANT_A) {
    v = readReg(0x09);                   // MCP23008 GPIO register
  } else {
    writeReg(0x0C, 0xFF);                // Clear interrupt source first so no concurrent edge is missed
    v = readReg(0x08);                   // SX1508 RegData register
  }
  if (v < 0) return -1;                  // Bus error: caller retains last valid key state
  return (uint8_t)~v;                    // Switches pull to GND when pressed -> invert
}

FLASHMEM uint8_t Board::keys() {
  // Poll the I2C expander when INT is asserted (LOW), while any key is currently held (_keys != 0),
  // or on the initial read, throttled by _keyGap.
  const uint32_t now = micros();
  if (_first || ((digitalReadFast(_pins->keyInt) == LOW || _keys != 0) && now - _keyReadAt >= _keyGap)) {
    _keyReadAt = now;
    _first = false;
    int v = readExpander();
    if (v >= 0) {
      _keys = (uint8_t)v;
      _keyGap = 1000;
    } else {
      _keyGap = 50000;
      _keyErrors++;
      if (millis() - _keyErrorLogAt >= 1000) {
        _keyErrorLogAt = millis();
#if CAPSTAN_DEBUG
        Serial.printf("keys: the key bus failed (%lu times so far; read %lu us)\n", (unsigned long)_keyErrors,
                      (unsigned long)(micros() - now));
#endif
      }
    }
  }
  return _keys;
}

FLASHMEM bool Board::keysChanged(uint8_t &now) {
  now = keys();
  if (now == _lastReported) return false;
  _lastReported = now;
  return true;
}

} // namespace m8

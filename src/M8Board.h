// M8Board.h — Hardware abstraction layer for M8 Model:01 (Teensy 4.0) and Model:02 (Teensy MicroMod)
//
// Model:01 has two hardware board revisions (Variant A and Variant B), detected at
// boot by reading pin 17 with a pull-down resistor:
//   - Pin 17 reads HIGH -> VARIANT_A (pin 17 is pulled up as SDA on Wire1)
//   - Pin 17 reads LOW  -> VARIANT_B
// Model:02 (Teensy MicroMod, M8_MODEL == 2) uses PINS_M02 with Variant B peripheral paths
// and a 480x320 ILI9488 SPI display.

#pragma once
#include <Arduino.h>
#include <Wire.h>
#if defined(ARDUINO_TEENSY_MICROMOD) && !defined(M8_MODEL)
#define M8_MODEL 2
#endif
#if M8_MODEL == 2
#include "ili9488_spi.h"                   // Model:02 480x320 ILI9488 SPI display driver
#else
#include <ILI9341_t3n.h>
#endif

namespace m8 {

enum Variant : uint8_t { VARIANT_A = 0, VARIANT_B = 1 };
// Model:02 (M8_MODEL == 2, Teensy MicroMod) shares its I2C/power/MIDI peripheral architecture
// with Variant B and uses PINS_M02 for its pin assignments and 480x320 ILI9488 panel.
#if M8_MODEL == 2
using Tft = ILI9488Spi;
#else
using Tft = ILI9341_t3n;
#endif

// Key bitmask (active-high: bit N corresponds to I2C GPIO expander pin GPn inverted).
enum Button : uint8_t {
  BTN_EDIT  = 1 << 0,
  BTN_OPT   = 1 << 1,
  BTN_RIGHT = 1 << 2,
  BTN_PLAY  = 1 << 3,
  BTN_SHIFT = 1 << 4,
  BTN_DOWN  = 1 << 5,
  BTN_UP    = 1 << 6,
  BTN_LEFT  = 1 << 7,
};

struct Pins {
  // Display: ILI9341 320x240 (Model:01) or ILI9488 480x320 (Model:02) on LPSPI4 (MOSI 11, SCK 13, MISO 12)
  uint8_t tftCS, tftDC, tftRST;
  uint8_t backlight;          // Backlight PWM pin
  float   backlightPwmHz;     // Backlight PWM carrier frequency (Hz)
  uint8_t tftRotation;        // Landscape display rotation index
  // Keys: 8-bit I2C GPIO expander at 0x20
  TwoWire *keyBus;
  uint8_t keyInt;             // Expander interrupt output (active low)
  // Audio / SD card sense & control pins
  uint8_t sdDetect;           // INPUT_PULLUP, HIGH = card removed
  uint8_t hpSense;            // Analog input, INPUT_PULLDOWN: headphone detect (low = plugged in)
  uint8_t hpEnable;           // Speaker amplifier enable (HIGH = on, LOW = muted)
  // TRS MIDI Out UART: Variant A uses Serial7; Variant B / Model:02 uses Serial3 (TX pin 14).
  // Initialize via midiOutBegin().
  HardwareSerial *midiOut;
  // TRS MIDI In UART: Variant B / Model:02 uses Serial1 (RX pin 0, with TX pin 1 held HIGH);
  // Variant A uses Serial7 (RX pin 28). Initialize via midiInBegin().
  HardwareSerial *midiIn;
};

extern const Pins PINS_A;
extern const Pins PINS_B;
#if M8_MODEL == 2
extern const Pins PINS_M02;                // Model:02 pin configuration
#endif

class Board {
public:
  // Detects hardware revision and initializes power rails, I2C, key expander, display, and backlight.
  // Returns false if the I2C key expander does not acknowledge.
  bool begin(uint32_t spiHz = 30000000);

  Variant variant() const { return _variant; }
  const Pins &pins() const { return *_pins; }
  Tft &tft() { return *_tft; }

  // Current key state (bit set = pressed). Cheap to call every loop;
  // only reads I2C when the expander's INT line asserts or a key is held.
  uint8_t keys();
  bool keysChanged(uint8_t &now);   // true once per state change

  void setBacklight(uint8_t level);  // 0..255

  // Headphone jack sense pin read as analog (0..1023): ~10 with headphones inserted,
  // ~580 without. Debounced across HP_DEBOUNCE polls at HP_POLL_MS intervals.
  static constexpr int HP_SENSE_THRESHOLD = 300, HP_DEBOUNCE = 3;
  static constexpr uint32_t HP_POLL_MS = 50;
  int headphoneSenseRaw();
  bool headphonesChanged(bool &in);  // true once per change (and once on initial poll)
  void setSpeaker(bool on);          // speaker amplifier enable pin (HIGH = on)

  // Battery state of charge (0..100%, or -1 on Variant A without I2C fuel gauge/charger).
  // Polled at most once per BATTERY_POLL_MS from the STC3115 fuel gauge (or estimated
  // from BQ2589x ADC battery voltage when the gauge is in standby).
  static constexpr uint32_t BATTERY_POLL_MS = 5000;
  int batteryPercent();
  // True when positive charge current is flowing into the battery (or charger CHRG_STAT indicates charging).
  bool batteryCharging() const { return _batCharging; }
  void batteryLog();                 // Logs raw gauge/charger telemetry when CAPSTAN_DEBUG is enabled

  // Periodic BQ2589x safety monitor (called from loop() every 5 s on Variant B / Model:02).
  // Checks fault registers, thermal regulation status, battery voltage (< 4.25 V), and charge
  // current, pausing charging if an anomaly is detected and retrying after 5 minutes (up to 3 strikes).
  void chargeGuard();

  // Initializes the TRS MIDI Out UART at 31,250 baud and returns the serial instance.
  // On Variant B / Model:02, restores pin 15 as INPUT_PULLUP for the key expander INT line.
  HardwareSerial *midiOutBegin();
  // Initializes the TRS MIDI In UART at 31,250 baud (RX only on Variant B / Model:02, keeping pin 1 HIGH).
  HardwareSerial *midiInBegin();

  // Hardware power button (Teensy 4 / MicroMod On/Off pin managed by the IMXRT1062 SNVS block).
  bool powerButtonPressed();         // true once per short press (clears SNVS_LPSR_SPO)
  bool powerButtonHeld();            // live physical button level
  [[noreturn]] void powerOff();

private:
  bool initMCP23008();
  bool initSX1508();
  int readExpander();                     // (-1 on I2C bus error)
  bool writeReg(uint8_t reg, uint8_t val);
  int  readReg(uint8_t reg);
  void applyVariantBPanelTuning();
  bool initChargerB();

  Variant _variant = VARIANT_A;
  const Pins *_pins = nullptr;
  Tft *_tft = nullptr;
  uint8_t _keys = 0, _lastReported = 0;
  bool _first = true;
  // Minimum interval between I2C key polls: 1 ms during normal operation,
  // backing off to 50 ms after a bus error so loop() and audio streaming never stall.
  uint32_t _keyReadAt = 0, _keyGap = 0, _keyErrors = 0, _keyErrorLogAt = 0;
  int8_t _hpState = -1;              // -1: not reported yet
  uint8_t _hpCount = 0;
  bool _hpLast = false;
  uint32_t _hpPolledMs = 0;
  int8_t _batPct = -1;
  bool _batCharging = false;
  int16_t _batMv = -1, _batGaugeMode = -1, _batSoc = -1, _batStatus = -1;
  int16_t _batCurrent = 0, _batGaugeMv = -1;
  bool _batPolled = false;
  uint32_t _batPolledMs = 0;
  uint32_t _cgCheckedMs = 0, _cgPausedMs = 0;   // charge guard timers
  uint8_t _cgStrikes = 0;
  bool _cgPaused = false, _cgLatched = false;
};

} // namespace m8

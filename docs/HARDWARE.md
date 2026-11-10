# M8 Model:01 & Model:02 Hardware Reference for Capstan

This document describes the hardware pinout, peripheral buses, display drivers, audio codec configuration, power management, and MIDI interfaces used by **Capstan** on the **M8 Model:01** (Teensy 4.0) and **M8 Model:02** (Teensy MicroMod).

---

## 1. MCU & Build Targets

| Model | MCU | Build Target (FQBN) | Display |
|---|---|---|---|
| **Model:01** | Teensy 4.0 (NXP i.MX RT1062, 600 MHz, 40 digital pins) | `teensy:avr:teensy40:usb=serialmsc` | 320×240 ILI9341 SPI TFT (`ILI9341_t3n` @ 70 MHz) |
| **Model:02** | Teensy MicroMod (NXP i.MX RT1062, 600 MHz, 16 MB Flash, 46 digital pins) | `teensy:avr:teensyMM:usb=serialmsc` | 480×320 ILI9488 SPI TFT (`ILI9488Spi` @ 56 MHz) |

### Model:01 Revision Detection (Variant A vs. Variant B)
At boot (`m8::Board::begin()` in [`src/M8Board.cpp`](../src/M8Board.cpp)), Model:01 configures pin `17` as `INPUT_PULLDOWN`, waits 1 ms, and reads `digitalRead(17)`:
- **Variant A (`digitalRead(17) == HIGH`)**: Pin 17 is pulled high by the external pull-up resistor on `Wire1` (`SDA1`).
- **Variant B (`digitalRead(17) == LOW`)**: Uses `Wire` (`SDA 18 / SCL 19`) for all I2C peripherals.

On **Model:02** (`ARDUINO_TEENSY_MICROMOD`), the board revision is fixed (`Variant::M02`).

---

## 2. Peripheral Pin Map

| Subsystem | Model:01 Variant A (`pin 17 == HIGH`) | Model:01 Variant B (`pin 17 == LOW`) | Model:02 (`ARDUINO_TEENSY_MICROMOD`) |
|---|---|---|---|
| **TFT Display (LPSPI4)** | `ILI9341_t3n` 320×240 @ 70 MHz<br>MOSI `11`, SCK `13`, MISO `12`<br>CS `10`, DC `2`, RST `3`, Rotation `3` | `ILI9341_t3n` 320×240 @ 70 MHz<br>MOSI `11`, SCK `13`, MISO `12`<br>CS `10`, DC `6`, RST `5`, Rotation `1` | `ILI9488Spi` 480×320 @ 56 MHz<br>MOSI `11`, SCK `13`, MISO `12`<br>DC `10` (no CS line), RST `5`, Rotation `3` (`COLMOD=0x66` RGB666) |
| **Backlight PWM** | Pin `6`, `261 Hz` (`0..255`) | Pin `4`, `44,100 Hz` (`0..255`) | Pin `4`, `44,100 Hz` (`0..255`) |
| **Key Expander (8 Keys)** | **MCP23008** @ `0x20` on `Wire1` (SDA `17` / SCL `16`, 400 kHz), INT pin `5` (`INPUT`) | **SX1508** @ `0x20` on `Wire` (SDA `18` / SCL `19`, 400 kHz), INT pin `15` (`INPUT_PULLUP`) | **SX1508** @ `0x20` on `Wire` (SDA `18` / SCL `19`, 400 kHz), INT pin `15` (`INPUT_PULLUP`) |
| **Audio I2S (SAI1)** | MCLK `23`, BCLK `21`, LRCLK `20`, TX `7`, RX `8` | Same (`SAI1`) | Same (`SAI1`) |
| **Audio Codec** | **NXP SGTL5000** @ `0x0A` on `Wire` (SDA `18` / SCL `19`) | Same | Same (also supports internal mic via `AUDIO_INPUT_MIC`, `micGain(30)`) |
| **SD Card** | `BUILTIN_SDCARD` (`USDHC1` SDIO in `DMA_SDIO` mode) | Same | Same |
| **TRS Serial MIDI (31,250 baud)** | Out: `Serial7` (TX `29`)<br>In: `Serial7` (RX `28`) | Out: `Serial3` (TX `14`; RX `15` stays GPIO for key INT)<br>In: `Serial1` (RX `0` receive-only; pin `1` stays GPIO `OUTPUT HIGH`) | Out: `Serial3` (TX `14`; RX `15` stays GPIO for key INT)<br>In: `Serial1` (RX `0` receive-only; pin `1` stays GPIO `OUTPUT HIGH`) |
| **Headphone Sense** | Pin `26` (`A12`, `INPUT_PULLDOWN`, low-pass filtered; `< 300` = plugged in) | Pin `22` (`A8`, `INPUT_PULLDOWN`, low-pass filtered; `< 300` = plugged in) | Pin `22` (`A8`, `INPUT_PULLDOWN`, low-pass filtered; `< 300` = plugged in) |
| **Speaker Amp Enable** | Pin `30` (`OUTPUT`, `HIGH` = on, `LOW` = off) | Pin `9` (`OUTPUT`, `HIGH` = on, `LOW` = off) | Pin `9` (`OUTPUT`, `HIGH` = on, `LOW` = off) |
| **Battery Charger** | Status pins `14`, `15` (`INPUT_PULLUP`); battery ADC on pin `22` (`A8`) | **TI BQ2589x** @ `0x6A` on `Wire`; INT pin `2` (`INPUT`) | **TI BQ2589x** @ `0x6A` on `Wire`; INT pin `2` (`INPUT`); peripheral enable pin `40` (`OUTPUT`) |
| **Fuel Gauge** | Analog ADC on pin `22` (`A8`) | **ST STC3115** @ `0x70` on `Wire` | **ST STC3115** @ `0x70` on `Wire` |
| **Power Rail / Latch** | — | Pin `1` driven `OUTPUT HIGH` at boot (30 ms settle delay); must remain `HIGH` to keep the SD card power rail enabled | Pin `1` `OUTPUT HIGH`; mainboard power D-latch: Data (`D`) on pin `28`, Clock (`CLK`) on pin `6` |

---

## 3. Key Scanner & Button Bitmask (`src/M8Board.h`)

The 8 mechanical keys are read as active-low GPIO inputs from the I2C port expander (`~rawGpio`) and exposed as an 8-bit bitmask:

| Constant | Bitmask | Button |
|---|---|---|
| `BTN_EDIT` | `1 << 0` (`0x01`) | `EDIT` |
| `BTN_OPT` | `1 << 1` (`0x02`) | `OPT` |
| `BTN_RIGHT` | `1 << 2` (`0x04`) | `RIGHT` |
| `BTN_PLAY` | `1 << 3` (`0x08`) | `PLAY` |
| `BTN_SHIFT` | `1 << 4` (`0x10`) | `SHIFT` |
| `BTN_DOWN` | `1 << 5` (`0x20`) | `DOWN` |
| `BTN_UP` | `1 << 6` (`0x40`) | `UP` |
| `BTN_LEFT` | `1 << 7` (`0x80`) | `LEFT` |

- **MCP23008 (Model:01 Variant A)**: `IODIR = 0xFF`, `GPPU = 0xFF`, `IOCON = 0x00`, `INTCON = 0x00`, `GPINTEN = 0xFF`; reads `GPIO` (`0x09`).
- **SX1508 (Model:01 Variant B & Model:02)**: `RegDir (0x07) = 0xFF`, `RegPullUp (0x03) = 0xFF`, internal 2 MHz oscillator (`RegClock 0x0F = 0x40`), 16 ms hardware debounce (`RegDebounceConfig 0x12 = 0x05`, `RegDebounceEnable 0x13 = 0xFF`), both-edge interrupts (`RegSenseHigh 0x0A = 0xFF`, `RegSenseLow 0x0B = 0xFF`, `RegInterruptMask 0x09 = 0x00`); reads `RegData` (`0x08`) and clears `RegInterruptSource` (`0x0C`).

---

## 4. Display Architecture (Model:01 ILI9341 vs. Model:02 ILI9488)

- **Model:01 (`320×240`)**: Driven by `ILI9341_t3n` in 16-bit `RGB565` mode at 70 MHz SPI. Uses a custom `3×5` pixel bitmap font scaled `2×` (`FONT_H = 10`, `PX = 2`).
- **Model:02 (`480×320`)**: Driven by [`m8::ILI9488Spi`](../src/ili9488_spi.h) at 56 MHz SPI. Because the ILI9488 controller only supports 18-bit (`RGB666`, `COLMOD = 0x66`, 3 bytes/pixel) over 4-wire SPI, `ILI9488Spi` accepts standard `RGB565` pixel values and expands them on the fly to 3-byte `RGB666` transfers over the LPSPI4 16-word transmit FIFO (`IMXRT_LPSPI4_S`). Uses a native `9×13` high-DPI bitmap font (`FONT_H = 13`, `FONT_GAP = 3`).
- **Coordinate Scaling**: All UI layouts use canonical `320×240` coordinates scaled at compile time via `SX(x) = (x * SCREEN_W) / 320` and `SY(y) = (y * SCREEN_H) / 240`.

---

## 5. Power Management, Charger (`BQ2589x`), & Shutdown Sequence

On Model:01 Variant B and Model:02:
1. **TI BQ2589x Charger (`0x6A` on `Wire`)**:
   - Initialized at boot with `REG14 = 0x80` (reset), `REG02 = 0xE0` (1 Hz continuous ADC), `REG03 = 0x36` (charge + OTG enabled, `SYS_MIN = 3.3 V`), `REG04 = 0x08` (`ICHG = 512 mA`), `REG07 = 0x8D` (charge termination enabled, I2C watchdog disabled, 12 h timer), `REG09 = 0x44`, and `REG0A = 0x03`.
   - `Board::chargeGuard()` monitors battery temperature via the STC3115 fuel gauge (`REG_TEMPERATURE = 0x0A`) and pauses charging (`REG03` bit 4 cleared) if temperature reaches `>= 50 °C`, resuming at `<= 45 °C`.
2. **Power Button & Clean Shutdown (`Board::powerOff()`)**:
   - The power button is sensed via `BQ2589x` `REG02` bit 0 (`INLIM` / power switch status). Holding the power button for 1 second triggers a clean shutdown:
   - Mutes the SGTL5000 codec and speaker amplifier, stops MIDI timers, flushes open WAV headers, and commits the SD card flash cache (`syncDevice()`).
   - On **Model:02**, pulses pin `40` `LOW` for 10 ms, sets the mainboard power D-latch data line (pin `28`) `LOW`, and clocks pin `6` `HIGH` (1 ms) -> `LOW` to unlatch mainboard power, then sets `BQ2589x` `REG09 |= 0x20` (`BATFET_DIS` ship mode) when running on battery.

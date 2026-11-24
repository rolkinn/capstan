# M8 Model:01 & Model:02 Hardware Reference for Capstan (`v1.5`)

This document describes the hardware pinout, peripheral buses, display drivers, audio codec configuration, power management, composite USB stack, and MIDI interfaces used by **Capstan** (`CAPSTAN_VERSION = "1.5"`) on the **M8 Model:01** (Teensy 4.0) and **M8 Model:02** (Teensy MicroMod).

> **Software Architecture**: For the 3-tier real-time execution model, module map, and Teensy 4.0 / MicroMod RAM1/RAM2 layout, see [Capstan Architecture](./architecture.md).

---

## 1. MCU & Build Targets

| Model | MCU | Build Target (FQBN) | Display |
|---|---|---|---|
| **Model:01** | Teensy 4.0 (NXP i.MX RT1062, 600 MHz, 40 digital pins) | `teensy:avr:teensy40:usb=serialmsc` | 320×240 ILI9341 SPI TFT (`ILI9341Spi` @ `30 MHz` default) |
| **Model:02** | Teensy MicroMod (NXP i.MX RT1062, 600 MHz, 16 MB Flash, 46 digital pins) | `teensy:avr:teensyMM:usb=serialmsc` | 480×320 ILI9488 SPI TFT (`ILI9488Spi` @ `60 MHz` RGB666) |

### Model:01 Revision Detection (Variant A vs. Variant B)
At boot (`m8::Board::begin()` in [`src/M8Board.cpp`](../src/M8Board.cpp)), Model:01 configures pin `17` (`VARIANT_DETECT_PIN`) as `INPUT_PULLDOWN`, waits `1 ms`, and reads `digitalRead(17)`:
- **Variant A (`digitalRead(17) == HIGH`)**: Pin `17` is pulled high by the external pull-up resistor on `Wire1` (`SDA1`).
- **Variant B (`digitalRead(17) == LOW`)**: Uses `Wire` (`SDA 18 / SCL 19`) for all I2C peripherals and drives `pin 1` `OUTPUT HIGH` (`+30 ms` settle) for the SD/peripheral rail.

On **Model:02** (`M8_MODEL == 2`, `ARDUINO_TEENSY_MICROMOD`), `Board::begin()` hard-selects `VARIANT_B` (`PINS_M02`).

---

## 2. Peripheral Pin Map

| Subsystem | Model:01 Variant A (`pin 17 == HIGH`) | Model:01 Variant B (`pin 17 == LOW`) | Model:02 (`ARDUINO_TEENSY_MICROMOD`) |
|---|---|---|---|
| **TFT Display (`LPSPI4`)** | `ILI9341Spi` 320×240 @ `30 MHz`<br>MOSI `11`, SCK `13`, MISO `12`<br>CS `10`, DC `2`, RST `3`, Rotation `3` | `ILI9341Spi` 320×240 @ `30 MHz`<br>MOSI `11`, SCK `13`, MISO `12`<br>CS `10`, DC `6`, RST `5`, Rotation `1` + IPS gamma/`INVON` tuning | `ILI9488Spi` 480×320 @ `60 MHz`<br>MOSI `11`, SCK `13`, MISO `12`<br>DC `10` (HW CS low, `cs == dc`), RST `5`, Rotation `3` (`COLMOD = 0x66` RGB666) |
| **Backlight PWM** | Pin `6`, `261 Hz` (`0..255`) | Pin `4`, `44,100 Hz` (`0..255`) | Pin `4`, `44,100 Hz` (`0..255`) |
| **Key Expander (8 Keys)** | **MCP23008** @ `0x20` on `Wire1` (`SDA 17 / SCL 16`, `400 kHz`), INT pin `5` (`INPUT`) | **SX1508** @ `0x20` on `Wire` (`SDA 18 / SCL 19`, `400 kHz`), 16 ms HW debounce, INT pin `15` (`INPUT_PULLUP`) | **SX1508** @ `0x20` on `Wire` (`SDA 18 / SCL 19`, `400 kHz`), 16 ms HW debounce, INT pin `15` (`INPUT_PULLUP`) |
| **Audio I2S (`SAI1`)** | MCLK `23`, BCLK `21`, LRCLK `20`, TX `7`, RX `8` | Same (`SAI1`) | Same (`SAI1`) |
| **Audio Codec (`Wire` `0x0A`)** | **NXP SGTL5000**: Line-In (`1.5 dB` HW steps + digital trim), Option A HP PGA (`0.5 dB` slewed) + Line Out (`13`) | Same | Same (also preserves backend `AUDIO_INPUT_MIC` / `micGain(30)` path) |
| **SD Card (`USDHC1`)** | `BUILTIN_SDCARD` (`DMA_SDIO`), detect pin `4` | `BUILTIN_SDCARD` (`DMA_SDIO`), detect pin `17`; **Pin `1` rail MUST stay `HIGH`** | `BUILTIN_SDCARD` (`DMA_SDIO`), detect pin `17`; **Pin `1` rail MUST stay `HIGH`** |
| **TRS Serial MIDI (`31,250` baud)** | Out: `Serial7` (TX `29`)<br>In: `Serial7` (RX `28`) | Out: `Serial3` (TX `14`; RX `15` stays GPIO for `keyInt`)<br>In: `Serial1` (RX `0`; `LPUART6_TX` disabled, pin `1` stays `OUTPUT HIGH`) | Same as Variant B |
| **Headphone Sense** | Pin `26` (`A12`, `INPUT_PULLDOWN`, `< 300` = plugged in, `50 ms` poll, `3`x debounce) | Pin `22` (`A8`, `INPUT_PULLDOWN`, `< 300` = plugged in) | Pin `22` (`A8`, `INPUT_PULLDOWN`, `< 300` = plugged in) |
| **Speaker Amp Enable** | Pin `30` (`OUTPUT`, `HIGH` = on, `LOW` = off) | Pin `9` (`OUTPUT`, `HIGH` = on, `LOW` = off) | Pin `9` (`OUTPUT`, `HIGH` = on, `LOW` = off) |
| **Battery Charger** | Status pins `14`, `15` (`INPUT_PULLUP`); battery ADC on pin `22` (`A8`) | **TI BQ2589x** @ `0x6A` on `Wire`; INT pin `2` (`INPUT`) | **TI BQ2589x** @ `0x6A` on `Wire`; INT pin `2` (`INPUT`); peripheral reset pin `40` (`OUTPUT`) |
| **Fuel Gauge** | Analog ADC on pin `22` (`A8`) | **ST STC3115** @ `0x70` on `Wire` | **ST STC3115** @ `0x70` on `Wire` |
| **Power Rail / Latch** | — | Pin `1` driven `OUTPUT HIGH` (`+30 ms` settle); powers SD card rail | Pin `1` `OUTPUT HIGH`; mainboard power D-latch: Data (`D`) on pin `28`, Clock (`CLK`) on pin `6` |

---

## 3. Key Scanner & Button Bitmask (`src/M8Board.h`)

The 8 mechanical keys are read as active-low GPIO inputs from the I2C port expander (`~rawGpio`) and exposed as an 8-bit active-high bitmask:

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
- **SX1508 (Model:01 Variant B & Model:02)**: `RegDir (0x07) = 0xFF`, `RegPullUp (0x03) = 0xFF`, internal 2 MHz oscillator (`RegClock 0x0F = 0x40`, `RegMisc 0x10 = 0x10`), 16 ms hardware debounce (`RegDebounceConfig 0x12 = 0x05`, `RegDebounceEnable 0x13 = 0xFF`), both-edge interrupts (`RegSenseHigh 0x0A = 0xFF`, `RegSenseLow 0x0B = 0xFF`, `RegInterruptMask 0x09 = 0x00`); clears `RegInterruptSource` (`0x0C`) and reads `RegData` (`0x08`).

---

## 4. Display Architecture & `v1.5` UI Coordinate Space

- **Model:01 (`320×240`)**: Driven by [`ILI9341Spi`](../src/ili9341_spi.h) at `30 MHz` (`Board::begin()` default) via direct 16-bit TX-only `LPSPI4` FIFO streaming (`LPSPI_TCR_FRAMESZ(15) | LPSPI_TCR_RXMSK`) in `RGB565` mode. Uses `3×5` bit-packed glyphs scaled `2×` (`FONT_H = 10`, `FONT_GAP = 2`). On Variant B, `applyVariantBPanelTuning()` applies IPS panel registers (`PWCTR1 0xC0`, `PWCTR2 0xC1`, `GAMSET 0x26`, `GMCTRP1 0xE0`, `GMCTRN1 0xE1`, `INVON 0x21`).
- **Model:02 (`480×320`)**: Driven by [`ILI9488Spi`](../src/ili9488_spi.h) at `60 MHz` SPI (`cs == dc == 10`). Converts `RGB565` pixels on the fly into 24-bit `RGB666` words (`COLMOD = 0x66`) pushed straight into `IMXRT_LPSPI4_S.TDR` in 24-bit TX-only FIFO mode (`LPSPI_TCR_FRAMESZ(23) | LPSPI_TCR_RXMSK`) with zero scanline buffer. Uses native `9×13` high-DPI bit-packed glyphs (`FONT_H = 13`, `FONT_GAP = 3`). Both models share `85` unique `PROGMEM` glyphs indexed via an `O(1)` `GLYPH_LUT[128]`.
- **Canonical `v1.5` UI Coordinates (`SX`/`SY`/`TY`)**: All UI positions scale from canonical `320×240` via `SX(x) = (x * SCREEN_W) / 320` and `SY(y) = (y * SCREEN_H) / 240`:
  - **Header Bar**: `y = SY(0)..SY(23)` (`hdrH = SY(24)` with `1 px` divider at `SY(23)`).
  - **Track Rows**: `y = SY(26) + visSlot * SY(41)`, `h = SY(39)` (`4` visible rows `visSlot = 0..3`).
  - **Shared Info Strip**: `1 px` divider at `SY(194)`, strip at `y = SY(195)`, `h = SY(16)`.
  - **Transport Buttons**: `y = SY(212)`, `h = SY(26)`.

---

## 5. Power Management, Charger (`BQ2589x`), & Shutdown Sequence

On Model:01 Variant B and Model:02:
1. **TI BQ2589x Charger (`0x6A` on `Wire`) & ST STC3115 Fuel Gauge (`0x70` on `Wire`)**:
   - Initialized at boot with `REG14 = 0x80` (reset), `REG02 = 0xE0` (1 Hz continuous ADC), `REG03 = 0x36` (charge + OTG enabled, `SYS_MIN = 3.3 V`), `REG04 = 0x08` (`ICHG = 512 mA`), `REG07 = 0x8D` (termination enabled, I2C watchdog disabled, 12 h timer), `REG09 = 0x44`, and `REG0A = 0x03`.
   - `Board::batteryPercent()` polls `STC3115` (`REG_MODE`, `SOC` `1/512 %`, `CURRENT`, `VOLTAGE`) + `BQ2589x` (`REG0B`, `REG0E`) every `5,000 ms`, falling back to an 11-point Li-Ion OCV curve if `GG_RUN == 0`.
   - `Board::chargeGuard()` checks `BQ2589x` faults (`REG0C` double-read), thermal regulation (`REG0E[7]`), overvoltage (`> 4,250 mV`), and overcurrent (`> 900 mA`) every `5,000 ms`, pausing charging (`REG03 = 0x26`) for `300,000 ms` and latching `_cgLatched = true` after 3 strikes.
   - **USB Charge While On (Model:01 Variant B & Model:02)**: `Board::setUsbChargeWhileOn(false)` lowers `REG00` `IINLIM` to `100 mA`, sets `REG03 = 0x26` (`CHG_CONFIG = 0`), and `appSyncUsbChargeEnumeration()` re-enumerates USB with `usb_self_powered_no_charge = 1` (`0xC0 / 0 mA` config descriptor).
2. **Power Button & Clean Shutdown (`Board::powerOff()` in [`src/M8Board.cpp`](../src/M8Board.cpp))**:
   - SNVS (`SNVS_LPSR` / `SNVS_HPSR`) monitors the power button: short tap (< `1,000 ms`) wakes a blanked screen or toggles Playlist Play Lock (`LOCK ->`); `>= 1,000 ms` hold triggers clean shutdown.
   - Shutdown mutes the SGTL5000 codec and speaker amplifier, stops TRS & USB MIDI, detaches USB (`usbShutdownDetach()`), flushes open WAV headers, and calls `syncDevice()` on the SD card.
   - Waits for power button release (`+50 ms`), sets MIDI UART pins to `INPUT_DISABLE`, restores `BQ2589x REG00 = _bqDefaultReg00`, clears `REG02 = 0x00`, and sets `REG03 = _cgPaused ? 0x06 : 0x16` (OTG off, autonomous off-state charging enabled unless paused; Capstan keeps `REG09 = 0x44` and intentionally **never sets `BATFET_DIS` `0x20` ship mode** so power-button wake and off-state USB charging remain operational).
   - On **Model:02**, drives `pin 40` `LOW` (`10 ms`), unlatches mainboard power via D-latch (`pin 28` `LOW`, `pin 6` `HIGH` `1 ms` -> `LOW`), waits `1,000 ms`, then pulls `pin 1` `LOW` before setting `SNVS_LPCR_DP_EN | SNVS_LPCR_TOP` and entering `wfi`.

---

## 6. Patched Composite USB Stack (`usb=serialmsc`)

Applied to Teensyduino 1.62.0 via [`tools/teensy_msc/patch.py`](../tools/teensy_msc/patch.py) (`VID 0x16C0`, serial `"CAPSTAN-01-<8HEX>"` / `"CAPSTAN-02-<8HEX>"` from `HW_OCOTP_MAC0` in [`src/usb_names.c`](../src/usb_names.c)):
- **Normal Mode (`PID 0x048A`, `NUM_INTERFACE = 6`)**:
  - `IF 0..1` (IAD): CDC Serial (`EP2` ACM, `EP3` IN/OUT Bulk; `2,048 B` `rx_buffer` in `RAM2`).
  - `IF 2`: USB MIDI 1.0 (`EP4` IN/OUT Bulk; `3,072 B` `rx_buffer` in uncached `DTCM`).
  - `IF 3..5` (IAD): UAC2 Audio (`AUDIO_CLOCK_ID = 9`, `44,100 Hz`, `2 ch` Master L/R Out on `EP7 IN`, `2 ch` In on `EP6 OUT` + `EP6 IN` explicit feedback `16.16` HS / `10.14` FS). `usb_audio_l7` (`IRQ_USB1` prio `64`) tracks `ARM_DWT_CYCCNT` against `USB1_FRINDEX` SOF checkpoints (`+/- 4.5 Hz` clamp), runs PI ring centering around `TX_MARK = 384` / `RX_MARK = 512` over `7,168 B` `RAM2` rings, and applies 127/129-frame linear slip interpolation outside `[256, 768]`.
- **Dedicated USB Storage Mode (`PID 0x04D5`)**:
  - `usbDriveMode()` ([`src/usb_drive.cpp`](../src/usb_drive.cpp)) stops transport, suspends MIDI In, flushes WAV headers, shares `16 KB` `RAM1` `looper::chunk` as `mediaDesc.work_buf`, and calls `usb_msc_set_mode(1, usbDriveTick)` (`350 ms` soft-detach/reattach exposing `CDC` + `BOT MSC` on `EP5`). Exiting via host eject or any key press switches back to `PID 0x048A` and reloads the SD card via `reloadFromCard()`.

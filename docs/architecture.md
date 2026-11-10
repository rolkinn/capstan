# Capstan Architecture (`v1.0`)

## 1. Overview

**Capstan** is a custom firmware for the **M8 Model:01** (Teensy 4.0) and **M8 Model:02** (Teensy MicroMod) implementing:
- **8-Track Linear Audio Tape Recorder**: 4 Stereo tracks (`A1`–`A4`, 16-bit 44.1 kHz stereo PCM) + 4 Mono tracks (`A5`–`A8`, 16-bit 44.1 kHz mono PCM), up to **666 seconds** per track, with variable-speed playback (`-5.00x` to `+5.00x`), sound-on-sound overdub, replace recording, and Pre/Post-FX master resampling.
- **8-Track Linear MIDI Sequencer & Piano Roll Editor**: `480 PPQN` resolution, up to `2,048` events per track (`128 KB` total in `DMAMEM`), recording Note On/Off, Control Change (CC), and Pitch Bend, with per-track input channel (`OFF`, `ALL`, `01`–`16`), output routing (`OFF`, external MIDI `C01`–`C16`, or internal instruments `I01`–`I08`), quantization, and an interactive Piano Roll note editor.
- **8-Slot Multi-Engine Internal Synthesizer & Sampler (`I01`–`I08`)**: 32-voice dynamically shared global polyphony pool (up to 8 voices per instrument) hosting four engines:
  1. **2-Operator FM (`FM2`)**: 3 algorithms (`[A][B]`, `[B]>[A]`, `[A]>[B]`), `0.00`–`24.00` continuous frequency ratios, feedback, and per-operator ADSR.
  2. **Phase Distortion (`PD`)**: 8 classic waveforms with dual-line configurations (`Line 1`, `Line 2`, `Line 2 > Line 1`) and DCW envelope modulation.
  3. **SD Card WAV Sampler (`SMP`)**: 4-octave (`C3`–`B6`, 48-key) keyboard with per-key SD WAV assignment, live audio-input sample recording, 4 trigger modes (`STOP`, `ONCE`, `REP`, `REPTHRU`), and 8 concurrent RAM1-staged SD streaming voices.
  4. **Noise Texture Drone (`NTD`)**: 3-layer drone synthesizer (`DRONE OSC` + `NOISE BED` + `CRACKLE & MOD`) with `Gate`, `Latch`, and `Auto` sustain modes.
- **5-Slot Master Effects Bus**: `Chorus -> Delay -> Reverb -> Tape -> Comp` + Master Volume, plus per-instrument `Drive` and shared Chorus/Delay/Reverb send bus.
- **Comprehensive MIDI Integration**: External MIDI Clock sync (`Internal`, `Ext Auto`, `Ext USB`, `Ext TRS`), MIDI Clock Out, Standard MIDI Transport (`0xFA`/`0xFB`/`0xFC`) & MMC SysEx control, Global MIDI CC Mappings (`SYSTEM.DAT` v4), and Per-Instrument MIDI CC Mappings (`INST.DAT` v3 / `.FMI`) with MIDI Learn.

---

## 2. Source Module Map

| File | Responsibility |
|---|---|
| [`Capstan.ino`](../Capstan.ino) | `setup()` / `loop()`, Teensy Audio graph (`AudioInputI2S -> LooperEngine -> AudioOutputI2S`), power button & hot-swap SD handling |
| [`build_all.py`](../build_all.py) | Unified build, verification, and release packaging script for Model:01 (`teensy40`) and Model:02 (`teensyMM`) |
| [`src/config.h`](../src/config.h) | Global constants (`CAPSTAN_VERSION = "1.0"`, `SAMPLE_RATE_HZ = 44100`, `MAX_TAPE_SECONDS = 666`, buffer sizes, mixer/pan steps, `MIDI_PPQN = 480`) |
| [`src/colors.h`](../src/colors.h) | RGB565 color palette and per-page / per-track accent colors |
| [`src/M8Board.h`](../src/M8Board.h), [`src/M8Board.cpp`](../src/M8Board.cpp), [`src/ili9488_spi.h`](../src/ili9488_spi.h), [`src/ili9488_spi.cpp`](../src/ili9488_spi.cpp) | Hardware abstraction for M8 Model:01 (Variants A & B) and Model:02: display, I2C keys, headphone sense, speaker amp, BQ2589x charger, STC3115 fuel gauge, TRS MIDI |
| [`src/engine.h`](../src/engine.h), [`src/engine.cpp`](../src/engine.cpp) | 44.1 kHz Audio ISR (`LooperEngine::update()`): 8-track playback, overdub/replace/resample capture, variable speed (`-5.00x..+5.00x`), punch/loop window, threshold trigger, LUT woodblock metronome, per-track Vol/Bal/Pan/Solo, 14-ch USB Audio UAC2 I/O |
| [`src/master_fx.h`](../src/master_fx.h), [`src/master_fx.cpp`](../src/master_fx.cpp) | 5-slot Master FX chain (`Chorus -> Delay -> Reverb -> Tape -> Comp`), per-instrument `Drive` with cached filter coefficients, and shared instrument FX send bus |
| [`src/midi_seq.h`](../src/midi_seq.h), [`src/midi_seq.cpp`](../src/midi_seq.cpp) | 8-track linear MIDI sequencer (`480 PPQN`, `2,048` events/track in `DMAMEM`), ISR playback & Live Thru, Piano Roll note editor helpers, `.mid` import/export |
| [`src/midi_in.h`](../src/midi_in.h), [`src/midi_in.cpp`](../src/midi_in.cpp) | External MIDI Clock slave (`0xF8`, `0xFA`, `0xFB`, `0xFC`, `0xF2`), MMC SysEx parser, and incoming TRS Serial MIDI parser |
| [`src/midi_usb.h`](../src/midi_usb.h), [`src/midi_usb.cpp`](../src/midi_usb.cpp) | Single 250 µs (`4 kHz`) `IntervalTimer` polling both TRS Serial MIDI and USB MIDI in/out/clock via lock-free queues |
| [`src/midi_map.h`](../src/midi_map.h), [`src/midi_map.cpp`](../src/midi_map.cpp) | Global MIDI CC Mapping table (7 categories, 128-bit `interceptedCcMask` fast reject) and Per-Instrument MIDI CC Mapping engine with MIDI Learn |
| [`src/storage.h`](../src/storage.h), [`src/storage.cpp`](../src/storage.cpp) | `DMA_SDIO` streaming (`AUDIO/TRACK1.WAV`–`TRACK8.WAV`), shared 16 KB `RAM1` `chunk` buffer, song/project/preset persistence (`SONG.DAT` v2, `MIDI.DAT` v2, `INST.DAT` v3, `SYSTEM.DAT` v4), master & stem bounce export |
| [`src/track_edit.h`](../src/track_edit.h), [`src/track_edit.cpp`](../src/track_edit.cpp) | Track Edit Mode (`SET PUNCH/LOOP REGION`, `QUANTIZE`, `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, `EXPORT/LOAD CLIP`) for Audio and MIDI tracks |
| [`src/instruments.h`](../src/instruments.h), [`src/instruments.cpp`](../src/instruments.cpp), [`src/inst_fm2op.*`](../src/inst_fm2op.h), [`src/inst_phasedist.*`](../src/inst_phasedist.h), [`src/inst_sampler.*`](../src/inst_sampler.h), [`src/inst_noisedrone.*`](../src/inst_noisedrone.h) | 8-slot internal instrument host (`InstEngineOps`, 32-voice global pool) with sub-block sample-accurate Note On/Off queueing |
| [`src/app.h`](../src/app.h), [`src/app.cpp`](../src/app.cpp) | Shared application state, 9-page navigation, cursor state, and transport helpers |
| [`src/pages.h`](../src/pages.h), `src/page_*.h`, `src/page_*.cpp` | Modular per-page UI rendering & key input handlers (`PageOps` descriptors for all 9 pages) |
| [`src/input.h`](../src/input.h), [`src/input.cpp`](../src/input.cpp) | Key scanner, modal on-screen keyboard, transport shortcuts, and `PageOps` input dispatcher |
| [`src/ui.h`](../src/ui.h), [`src/ui.cpp`](../src/ui.cpp), [`src/ui_font.*`](../src/ui_font.h), [`src/ui_splash.cpp`](../src/ui_splash.cpp) | Flicker-free `Surface`-composited UI shell, O(1) `GLYPH_LUT` bitmap font renderer, multi-tier UI frame scheduler |
| [`src/usb_drive.h`](../src/usb_drive.h), [`src/usb_drive.cpp`](../src/usb_drive.cpp), [`src/usb_names.c`](../src/usb_names.c) | USB Mass Storage Drive mode (`usbDriveMode()`) exposing the M8 SD card over USB |
| [`tools/teensy_msc/patch.py`](../tools/teensy_msc/patch.py) | Teensyduino core patch enabling composite `Serial + MIDI + Audio (14-ch Out / 2-ch In UAC2) + Mass Storage` (`usb=serialmsc`) |
| [`tools/recorder_sim/verify_firmware.py`](../tools/recorder_sim/verify_firmware.py) | Automated architectural, DMA-safety, WAV sector-alignment, ELF memory region, and release cleanliness verifier |

---

## 3. Three-Tier Real-Time Threading Architecture

1. **Tier 1 — Audio ISR (`LooperEngine::update()`, every 128 samples = 2.9 ms, priority 208)**:
   - Advances the linear tape clock (`transport.now`, `transport.tapeFrame`), evaluates count-in beats, audio/MIDI threshold triggers, and punch/loop window transitions.
   - Calls `midiSeqIsrTick()` at audio block rate to dispatch recorded sequencer notes with sub-block sample offsets (`0..127`).
   - Calls `fmSynthRenderBlock()` to render all active internal synthesizer/sampler voices with sub-block sample-accurate Note On/Off splitting and shared Chorus/Delay/Reverb send FX.
   - Reads the 8 audio track playback rings (`tracks[t].ring`), applies smoothed per-track volume/pan/solo, mixes live input monitoring, runs the 5-slot Master FX chain (`processMasterFx()`), captures overdub/replace/resample audio into `inputRing[INPUT_FRAMES]`, streams 14-channel USB Audio UAC2 output, and finishes with the master soft limiter.
2. **Tier 2 — Unified MIDI Poll ISR (`midi_usb.cpp`, every 250 µs = 4 kHz, priority 112)**:
   - Polls both hardware TRS Serial MIDI (`midiServiceTrsInOut()`) and USB MIDI (`usb_midi_read_message()`), stamping incoming messages with a sub-block sample offset (`0..127`) computed from `micros() - engineLastBlockMicros()`.
   - Performs immediate ISR-level Live MIDI Thru and internal synth voice triggering (`allowSdIo = false`) so external MIDI controllers play with sub-millisecond jitter even during SD card or TFT display writes.
3. **Tier 3 — Main Loop (`loop()`)**:
   - Calls `serviceStorage()` to top up the 8 audio track playback rings (`fillTrackRing`), service the 8 SD sampler streaming voices (`samplerServiceStreams()`), and drain recorded audio from `inputRing` to SD (`drainRecordingBuffer()`).
   - Calls `midiInService()` and `midiSeqService()` to commit recorded MIDI events into `eventPool`, handle any deferred uncached sampler Note Ons, and apply mapped MIDI CC updates.
   - Calls `pollKeys()` and `uiRefresh()` (scheduled via a 3-tier frame budget: `33 ms` idle, `50 ms` during playback, `66 ms` on Model:02 active recording, calling `keepStreaming()` between row blits).

---

## 4. Teensy 4.0 / MicroMod Memory Layout (512 KB RAM1 + 512 KB RAM2)

- **RAM1 (`ITCM` + `DTCM`, 512 KB, tightly coupled, uncached)**:
  - **ITCM (`code`)**: Hot DSP, ISR, and time-critical routines (`<= 5` 32 KB banks on Model:01, `4` banks on Model:02); cold UI, input, modal, and SD management functions are marked `FLASHMEM`.
  - **DTCM (`data` + `bss`)**:
    - `inputRing[8192]` (`32 KB`, ~185.8 ms stereo capture ring).
    - `chunk[4096]` (`16 KB` in `storage.cpp`): shared uncached `DMA_SDIO` transfer buffer (also reused by USB Mass Storage mode and `.DAT` serialization).
    - `samplerStreams[8]` (`32 KB`), `samplerDirs` (`6 KB`), and `samplerKeyDirIdx` (`384 B`).
    - Stack space (~`180 KB` free on Model:01, ~`197 KB` free on Model:02) holds temporary `Surface` compositing buffers during `uiRefresh()`.
- **RAM2 (`DMAMEM`, 512 KB, cached)**:
  - `rings` (`252 KB`): 4 Stereo audio track rings (`4 × 42 KB`) + 4 Mono audio track rings (`4 × 21 KB`), providing `10,752` frames (~244 ms) lookahead per track.
  - `eventPool[8][2048]` (`128 KB`): 8 MIDI tracks × `2,048` events × `8 bytes`.
  - `samplerKeyFile[8][48][44]` (`16.5 KB`): per-key `.WAV` filenames staged via `storageSharedChunkBuf()` in `RAM1` during SD save/load.
  - Delay lines (`delayBuf` + `instDelayBuf`, `75 KB`) and `sinLut` (`4 KB`).
  - **DMA Cache Rule**: Because `SdFat`'s `DMA_SDIO` mode transfers directly without cache maintenance on cached `RAM2`, all SD card reads and writes are strictly staged through `chunk` or a `RAM1` stack buffer.

# Capstan: Audio + MIDI Tape Recorder Firmware for M8

**Capstan** is a linear tape-style **4-stereo, 4-mono track audio recorder**, **8-track MIDI sequencer**, and **multi-engine synthesizer & sampler** firmware for the **M8 Model:01** and **M8 Model:02**.

**Video tutorial:** https://www.youtube.com/watch?v=xBZS2HxJms4

## 1. Installing Pre-Built Firmware (`.hex`)

Release builds provide two firmware images:
- **`M8_Capstan_v1.0_Model01.hex`** — for **M8 Model:01** (Teensy 4.0, 320×240 display)
- **`M8_Capstan_v1.0_Model02.hex`** — for **M8 Model:02** (Teensy MicroMod, 480×320 display)

To flash onto your hardware:
1. Install **TyUploader** if not already installed: https://github.com/Koromix/tytools/releases
2. Connect your M8 to your computer over USB.
3. Open **TyUploader**, and ensure your device is selected in the dropdown.
4. Click Upload and choose the Capstan .hex file matching your hardware model (`Model01` or `Model02`).

Notes:
  - On first boot, Capstan automatically creates the `/Capstan` directory structure on the SD card without modifying any other folders/files on the card.
  - If coming from a different firmware, may need to press the M8's reset button on the back. If TyUploader doesn't upload the Capstan .hex, use the reset button first then try again.
  - To revert back to a different firmware, simply use TyUploader again and choose the firmware you want to install.

## 2. Key Features

### 8-Track Linear Audio Tape Recorder
- **4 Stereo (`A1`–`A4`) + 4 Mono (`A5`–`A8`) Audio Tracks**: Up to **666 seconds** (11 minutes, 6 seconds) per track of uncompressed 44.1 kHz 16-bit audio streamed directly to and from the SD card.
- **Record, Overdub, & Master Resampling**:
  - Per-track `RECORD` and `OVERDUB`
  - **Master Resampling (`RES`)**: Bounce the **Pre-Master FX** or **Post-Master FX** onto any audio track.
- **Track Region Editing**: `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, and `.wav` audio clip import.
- **Variable Tape Speed & Reverse Playback**: Tape speed control from **`-5.00x - +5.00x`**, including **`-1.00x` reverse recording and overdubbing**.
- **Punch-In/Out & Looping**: independent **Punch In/Out** and **Loop** modes.
- **Audio & MIDI Threshold Triggered Recording**: Auto-start recording based on input threshold.
- **Configurable metronome with 0–4 bar count-in**.

### 8-Track Linear MIDI Recorder & Piano Roll Editor (`480 PPQN`)
- **8 MIDI Tracks (`M1`–`M8`)**: `Note On/Off`, `Velocity`, `Control Change (CC)`, and `Pitch Bend`.
- **MIDI Routing**: Per-track Input channel (`OFF`, `ALL`, `C01`–`C16`) and Output destination (`OFF`, MIDI channels `C01`–`C16`, or internal instruments `I01`–`I08`).
- **Interactive Piano Roll Editor**: Double-tap `EDIT` on any MIDI track waveform to open the 1/32nd-note grid Piano Roll editor.
- **Track Region Editing & Standard `.mid` MIDI File support**: `QUANTIZE` (`1/4`, `1/8`, `1/16`, `1/32`), `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, and MIDI `.mid` clip import/export.

### 8-Slot Multi-Engine Synthesizer & Sampler (`I01`–`I08`)
- **32-voice** shared global polyphony (up to 8 voices per instrument) with per-instrument **Amp** & **Filter ADSR**, **Drive**, **Chorus**, **Delay**, and **Reverb** sends.
- Current instruments include:
  1. **2-op FM synth**
  2. **Phase Distortion synth**
  3. **Noise Texture Drone synth**
  4. **Sampler**: 48 sample keys (`C3`–`B6`) per sampler instance, with live audio-input sample recording.

### Master Mixing, Effects, MIDI Mapping, & USB Connectivity
- **Master Page Track Control**: 8-track Audio (`VOL`, `BAL`/`PAN`, `M`, `S`) and MIDI (`IN`, `OUT`, `M`, `S`) console strips.
- **Master Page FX chain**:  `Chorus -> Delay -> Reverb -> Tape Saturator -> Compressor`.
- **Transport control, track mixer, navigation, & per-instrument MIDI CC Mapping** with learn assignment.
- **Composite USB Device**: **USB Audio**, **USB MIDI**, **USB Serial**, and **USB Mass Storage Drive** mode.
  - 14-channel output: Master L/R, Tracks 1–4 L/R, Tracks 5–8 Mono
  - 2-channel stereo input

## 3. Navigation & Core Controls

| Keys | Action |
|---|---|
| `SHIFT + UP/DOWN/LEFT/RIGHT` | Navigate between pages in the spatial map.<br>**NOTE:** When on `Audio` & `MIDI` pages, cursor first navigates to top timeline or bottom transport bars before moving to other pages above/below |
| in File browser, overlays, confirmations<br>- `OPT` | Cancel/exit |
| `PLAY` | Start / Stop transport at current playhead position |
| `SHIFT + PLAY` | Start transport from beginning |
| `EDIT + UP/DOWN/LEFT/RIGHT` | Coarse/fine adjust selected parameter |
| `EDIT` | Toggle button, cycle option, or open modal/editor |
| `OPT + EDIT` | Reset parameter to default |
| `OPT + LEFT/RIGHT`<br>`OPT + UP/DOWN` | Scrub timeline from any page or cell:<br>- `OPT + LEFT/RIGHT`: Move `±1 ms` (hold to scrub faster)<br>- `OPT + UP/DOWN`: Step `±1 beat` |
| on top timeline bar<br>- `EDIT+UP/DOWN/LEFT/RIGHT`<br>- `SHIFT+EDIT` | - Coarse/fine adjust selected time or beat.bar.8th.16th.32nd granularity<br>- Toggles seek to punch/loop start and end times |
| on `Transport bar` `Play`/`Stop` button<br>- `EDIT`<br>- `EDIT+LEFT/RIGHT`<br>- `EDIT+UP/DOWN`<br>- `OPT+EDIT` while playing<br>- `OPT+EDIT` when note playing | - Plays/stops<br>- Decrease/increase playback speed (will go negative for reverse playback)<br>- Immediately change to `-1x` or `1x` playback<br>- Sets playback to `1x`<br>- Return to zero |
| on `Transport bar` record button<br>- `SHIFT+EDIT` | Toggle `Punch In` |
| on `Transport bar` `<<` and `>>`<br>- `EDIT`<br>- `OPT`<br>- `SHIFT+EDIT` | - Move 1ms (hold to scrub faster)<br>Move `1/32nd` grid<br>- Seeks to punch/loop start/end times |
| on `Audio` & `MIDI` pages track waveform<br>- (1) `SHIFT + OPT`<br>- then (2) `OPT`<br>- then (3) `SHIT + OPT` | - (1) Enters highlight region start adjust (`EDIT+direction` to adjust time)<br>- (2) toggles between being able to set start and end highlight region window<br>- (3) present track edit options  |
| on `MIDI` track waveform<br>- `Double-Tap EDIT` | Opens **Piano Roll Note Editor**. from here,<br>- `UP/DOWN/LEFT/RIGHT` to navigate grid (hold `SHIFT` to move in larger increments)<br>- `EDIT` to begin note placement, then `LEFT/RIGHT` to adjust note length, `UP/DOWN` to change note, `SHIFT+UP/DOWN` adjusts note velocity (`EDIT` again confirms placement, `OPT` cancels note edit)<br>- place cursor on an existing note, then `EDIT` to begin editing that note, `OPT+EDIT` to clear that note<br>- `SHIFT + OPT` to exit piano roll  |
| on `Audio` & `MIDI` pages track waveform<br>- `Double-Tap OPT` | Toggle zoom levels of timeline |
| on 'Sampler' instrument<br>- `SHIFT+EDIT` | Load/record sample to highlighted key
| on `Master` page<br>- `Double-Tap OPT` | Toggle track strips between **Audio (`A1`–`A8`) & MIDI Tracks (`M1`–`M8`)** |
| on `Master` page<br>- `EDIT+LEFT/RIGHT` when on `(M)ute` or `(S)olo` | Toggle `M`/`S` of all track left/right |

## 4. SD Card Directory Structure (`/Capstan`)

Capstan stores all system settings, songs, projects, and presets inside `/Capstan`:

```text
/Capstan/
├── SYSTEM.DAT                        # Brightness, Metronome Vol, Last Song, Global MIDI CC Map
├── PROJECTS/
│   └── <PROJECT>.proj                # Ordered song setlists for album-style playback
├── INSTRUMENT_PRESETS/
│   └── <PRESET>.FMI                  # Global instrument patches
└── SONGS/
    └── <SONG_NAME>/
        ├── SONG.DAT                  # Tempo, Punch/Loop window, Master FX, 8-track mixer settings
        ├── MIDI.DAT                  # 8-track MIDI sequence events & routing
        ├── INST.DAT                  # Instrument settings, sampler key maps, & per-inst MIDI maps
        ├── <SONG_NAME>.wav           # Exported Master stereo mixdown (from Song Settings -> EXPORT)
        ├── AUDIO/
        │   ├── TRACK1.WAV-TRACK4.WAV  # 44.1 kHz 16-bit Stereo track recordings
        │   └── TRACK5.WAV-TRACK8.WAV  # 44.1 kHz 16-bit Mono track recordings
        ├── MIDI/
        │   └── *.mid                 # Exported Standard MIDI Files
        ├── INSTRUMENTS/
        │   └── *.FMI                 # Song-local instrument presets
        └── SAMPLES/
            └── *.WAV                 # Recorded sampler audio files
```
> **Note**: Songs **must be saved** from the **Main** page (`[ SAVE ]`).

## 5. Building from Source

### Prerequisites
- **Python 3.9+**
- **Arduino CLI** (`arduino-cli` in `PATH`, `ARDUINO_CLI` environment variable, or installed via Arduino IDE 2.x)
- **Teensyduino 1.62.0** board package (`teensy:avr`) installed in Arduino CLI

### Build Commands
The unified [`build_all.py`](build_all.py) script automatically applies the composite USB (`Serial + MIDI + 14-ch UAC2 Audio + Mass Storage`) Teensy core patch from [`tools/teensy_msc/patch.py`](tools/teensy_msc/patch.py), compiles both hardware targets, and runs the automated verification suite ([`tools/recorder_sim/verify_firmware.py`](tools/recorder_sim/verify_firmware.py)):

```bash
# Build and verify both Model:01 and Model:02 (.hex files written to build/)
python build_all.py

# Build a single target
python build_all.py model01
python build_all.py model02

# Package a versioned release bundle (.hex files and SHA256SUMS.txt in dist/)
python build_all.py --release

# Build with runtime Serial diagnostics and freeze watchdog enabled (CAPSTAN_DEBUG=1)
python build_all.py --debug
```

See [`docs/architecture.md`](docs/architecture.md) and [`docs/HARDWARE.md`](docs/HARDWARE.md) for detailed software architecture and hardware reference documentation.

---

## Shoutouts:
- [minusro's](https://github.com/minusro ) work done with their [L7 looping firmware](https://github.com/minusro/L7).
- Silen Audio's [Infinite Flow](https://www.silen.software/products/infinite-flow) software synth.

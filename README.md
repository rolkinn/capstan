# Capstan — Audio + MIDI Tape Recorder Firmware for M8

**Capstan** is a linear tape-style **4-stereo, 4-mono track audio recorder**, **8-track MIDI sequencer**, and **multi-engine synthesizer/sampler** firmware for the **M8 Model:01** and **M8 Model:02**.

---

## 1. Installing Pre-Built Firmware (`.hex`)

Release builds provide two firmware images:
- **`M8_Capstan_v1.0_Model01.hex`** — for **M8 Model:01** (Teensy 4.0, 320×240 display)
- **`M8_Capstan_v1.0_Model02.hex`** — for **M8 Model:02** (Teensy MicroMod, 480×320 display)

To flash onto your hardware:
1. Connect your M8 to your computer over USB.
2. Open **TyUploader**, select the `.hex` file matching your hardware model (`Model01` or `Model02`).
3. Press the small reset button on the back of the M8.
4. Allow the M8 and TyUploader to refresh, then upload.
3. On first boot, Capstan automatically creates the `/Capstan` directory structure on the SD card without modifying any other folders on the card.

---

## 2. Key Features

### 8-Track Linear Audio Tape Recorder (`44.1 kHz / 16-Bit`)
- **4 Stereo (`A1`–`A4`) + 4 Mono (`A5`–`A8`) Audio Tracks**: Up to **666 seconds** (11 minutes, 6 seconds) per track of uncompressed 44.1 kHz 16-bit audio streamed directly to and from the SD card.
- **Record, Overdub, & Master Resampling**:
  - Per-track `RECORD` and `OVERDUB`
  - **Master Resampling (`RES`)**: Bounce the **Pre-Master FX** or **Post-Master FX** onto any audio track.
- **Track Region Editing & Standard MIDI File support (`.mid`)**: `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, and `.wav` audio clip import.
- **Variable Tape Speed & Reverse Playback**: Tape speed control from **`-5.00x` to `+5.00x`**, including **`-1.00x` reverse recording and overdubbing**.
- **Punch-In/Out & Looping**: independent **Punch In/Out** (`|o|`) and **Loop** modes.
- **Audio & MIDI Threshold Triggered Recording**: Auto-start recording based on input threshold and configurable 1–4 bar woodblock metronome count-in.
- **Configurable 1–4 bar metronome count-in**.

### 8-Track Linear MIDI Recorder & Piano Roll Editor (`480 PPQN`)
- **8 MIDI Tracks (`M1`–`M8`)**: `Note On/Off`, `Velocity`, `Control Change (CC)`, and `Pitch Bend`.
- **MIDI Routing**: Per-track Input channel (`OFF`, `ALL`, `01`–`16`) and Output destination (`OFF`, external TRS/USB MIDI `C01`–`C16`, or internal instruments `I01`–`I08`).
- **Interactive Piano Roll Editor**: Double-tap `EDIT` on any MIDI track waveform to open the 1/32nd-note grid Piano Roll editor with note placement, length adjustment, velocity editing, and auditions.
- **Track Region Editing & Standard MIDI File support (`.mid`)**: `QUANTIZE` (`1/4`, `1/8`, `1/16`, `1/32`), `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, and MIDI `.mid` clip import/export.

### 8-Slot Multi-Engine Synthesizer & Sampler (`I01`–`I08`)
- **32-voice** dynamically shared global polyphony (up to 8 voices per instrument) with per-instrument **Amp** & **Filter ADSR**, **Drive**, **Chorus**, **Delay**, **Reverb** sends.
1. **2-Op FM synth**: Operator A & B with 3 algorithms (`[A] [B]`, `[B]>[A]`, `[A]>[B]`), `0.00`–`24.00` frequency ratios, feedback, and per-operator ADSR envelopes.
2. **Phase Distortion synth**: 8 waveforms (`Saw`, `Square`, `Pulse`, `Double Sine`, `Saw-Pulse`, `Resonance I/II/III`) with dual-line combinations (`Line 1`, `Line 2`, `Line 2 > Line 1`).
3. **4-Octave SD Card WAV Sampler**: 48 keys (`C3`–`B6`) per sampler instance with direct SD `.WAV` streaming, live audio-input sample recording, and 4 trigger modes (`STOP`, `ONCE`, `REP`, `REPTHRU`).
4. **Noise Texture Drone synth**: 3-section atmospheric synth (`DRONE OSC`, `NOISE BED`, `CRACKLE & MOD`) with `Gate`, `Latch`, and `Auto` drone modes.

### Master Mixing, Effects, MIDI Mapping, & USB Connectivity
- **Master Page Track Control**: 8-track Audio (`VOL`, `BAL`/`PAN`, `M`, `S`) and MIDI (`IN`, `OUT`, `M`, `S`) console strips.
- **Master Page FX chain**:  `CHOR -> DLAY -> D.FB -> RVRB -> TAPE -> COMP`) and Master `VOL`.
- **Global & Per-Instrument MIDI CC Mapping**: Map any external MIDI CC to Transport, Track Mixer, Master FX, Navigation, or any Synthesizer/Sampler parameter.
- **Composite USB Device**: Simultaneous **USB Audio** (14-channel output: Master L/R + Tracks 1–4 Stereo + Tracks 5–8 Mono; 2-channel stereo input), **USB MIDI**, **USB Serial**, and **USB Mass Storage Drive** mode.

---

## 3. Navigation & Core Controls

Hold **`SHIFT` + Directional Arrows** to navigate between pages

| Keys | Action |
|---|---|
| `SHIFT + UP / DOWN / LEFT / RIGHT` | Navigate between pages in the spatial map |
| `PLAY` | Start / Stop transport |
| `SHIFT + PLAY` | Start transport from beginning |
| `EDIT + UP / DOWN / LEFT / RIGHT` | Coarse/fine adjust selected parameter |
| `EDIT` | Toggle button, cycle option, or open modal/editor |
| `OPT + EDIT` | Reset parameter to default |
| `Double-Tap EDIT` (on `MIDI` track waveform) | Open **Piano Roll Note Editor** |
| `Double-Tap OPT` (on `Master` page) | Toggle track strips between **Audio (`A1`–`A8`) & MIDI Tracks (`M1`–`M8`)** |

---

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

---

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

# Package a versioned release bundle (.hex files, .zip archive, and SHA256SUMS.txt in dist/)
python build_all.py --release

# Build with runtime Serial diagnostics and freeze watchdog enabled (CAPSTAN_DEBUG=1)
python build_all.py --debug
```

See [`docs/architecture.md`](docs/architecture.md) and [`docs/HARDWARE.md`](docs/HARDWARE.md) for detailed software architecture and hardware reference documentation.

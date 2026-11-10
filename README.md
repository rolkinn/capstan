# Capstan — Audio + MIDI Tape Recorder Firmware for M8

**Capstan** is a linear tape-style **4-stereo, 4-mono track audio recorder**, **8-track MIDI sequencer**, and **multi-engine synthesizer/sampler** firmware for the **M8 Model:01** (Teensy 4.0) and **M8 Model:02** (Teensy MicroMod).

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
- **4 Stereo + 4 Mono Audio Tracks (`A1`–`A8`)**: Up to **666 seconds** (11 minutes, 6 seconds) of uncompressed 44.1 kHz 16-bit PCM audio per track streamed directly to and from the SD card.
- **Sound-on-Sound Overdub, Replace, & Master Resampling**:
  - Per-track `REC` (Replace) and `DUB` (Sound-on-Sound overdub) arming.
  - Mono tracks (`A5`–`A8`) support selectable input routing (`L`, `R`, or `L+R` stereo sum).
  - **Master Resampling (`RES`)**: Bounce the live mix of all playing audio tracks and internal instruments onto any audio track, selectable **Pre-Master FX** or **Post-Master FX**.
- **Variable Tape Speed & Reverse Playback**: Continuous quarter-step tape speed control from **`-5.00x` to `+5.00x`** (`0.25x` increments), including **`-1.00x` reverse recording and overdubbing**.
- **Punch-In / Punch-Out & Loop Window**: Sample-accurate shared `IN` / `OUT` window (`MM:SS.mmm` and `Bar.Beat.8th.16th.32nd`) with independent **Punch** (`|o|`) and **Loop** modes.
- **Audio & MIDI Threshold Auto-Start**: Per-track `MAN` or `THR` (`-80 dB` to `-6 dB`) auto-start trigger and configurable 1–4 bar woodblock metronome count-in.

### 8-Track Linear MIDI Recorder & Piano Roll Editor (`480 PPQN`)
- **8 MIDI Tracks (`M1`–`M8`)**: `Note On`, `Note Off`, `Velocity`, `Control Change (CC)`, and `Pitch Bend`.
- **Flexible Routing**: Per-track Input channel (`OFF`, `ALL`, `01`–`16`) and Output destination (`OFF`, external TRS/USB MIDI `C01`–`C16`, or internal instruments `I01`–`I08`).
- **Interactive Piano Roll Editor**: Double-tap `EDIT` on any MIDI track waveform cell to open the 1/32nd-note grid Piano Roll editor with note placement, length adjustment, velocity editing, and auditions.
- **Track Region Editing & Standard MIDI File support (`.mid`)**: Non-destructive region `SET PUNCH/LOOP`, `QUANTIZE` (`1/4`, `1/8`, `1/16`, `1/32`), `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, and `.mid` clip import/export.

### 8-Slot Multi-Engine Synthesizer & Sampler (`I01`–`I08`)
32-voice dynamically shared global polyphony pool (up to 8 voices per instrument) with per-instrument **Amp** & **Filter ADSR**, **Drive**, **Chorus**, **Delay**, **Reverb** sends:
1. **2-Op FM synth**: Operator A & B with 3 algorithms (`[A] [B]`, `[B]>[A]`, `[A]>[B]`), `0.00`–`24.00` frequency ratios, feedback, and per-operator ADSR envelopes.
2. **Phase Distortion synth**: 8 waveforms (`Saw`, `Square`, `Pulse`, `Double Sine`, `Saw-Pulse`, `Resonance I/II/III`) with dual-line combinations (`Line 1`, `Line 2`, `Line 2 > Line 1`).
3. **4-Octave SD Card WAV Sampler**: 48 keys (`C3`–`B6`) per sampler instance with direct SD `.WAV` streaming, live audio-input sample recording, and 4 trigger modes (`STOP`, `ONCE`, `REP`, `REPTHRU`).
4. **Noise Texture Drone synth**: 3-section atmospheric synth (`DRONE OSC`, `NOISE BED`, `CRACKLE & MOD`) with `Gate`, `Latch`, and `Auto` drone modes.

### Master Effects, Mixing, & USB Connectivity
- **Master Page**: 8-track Audio (`VOL`, `BAL`/`PAN`, `M`, `S`) and MIDI (`IN`, `OUT`, `M`, `S`) console strips plus a 5-slot Master FX chain (`CHOR -> DLAY -> D.FB -> RVRB -> TAPE -> COMP`) and Master `VOL`.
- **Global & Per-Instrument MIDI CC Mapping**: Map any external MIDI CC to Transport, Track Mixer, Master FX, Navigation, or any Synthesizer/Sampler parameter.
- **Composite USB Device**: Simultaneous **USB Audio** (14-channel output: Master L/R + Tracks 1–4 Stereo + Tracks 5–8 Mono; 2-channel stereo input), **USB MIDI**, **USB Serial**, and **USB Mass Storage Drive** mode.

---

## 3. Navigation & Core Controls

Hold **`SHIFT` + Directional Arrows** to navigate between pages

| Shortcut | Action |
|---|---|
| `SHIFT + UP / DOWN / LEFT / RIGHT` | Navigate between pages in the spatial map |
| `PLAY` | Start / Stop tape transport |
| `SHIFT + PLAY` | Toggle Record / Overdub on the currently focused or last-armed track |
| `OPT + LEFT / RIGHT` | Coarse/fine adjust selected parameter (hold `EDIT + OPT + Arrows` on time/locator fields) |
| `EDIT` | Toggle button, cycle option, or open modal/editor |
| `Double-Tap EDIT` (on Track Waveform) | Open **Piano Roll Note Editor** (on `MIDI` page) |
| `SHIFT + EDIT` (on Track Waveform) | Enter **Track Region Edit Mode** (`SET PUNCH/LOOP`, `QUANTIZE`, `COPY`, `CUT`, `PASTE`, `DUPL`, `CLR`, `CLIP`) |
| `OPT + EDIT` | Reset parameter to default, open Track Quick-Action menu on waveform cells, or start **MIDI Learn** on MIDI Map pages |
| `Double-Tap OPT` (on `Master` page) | Toggle Master mixer strips between **Audio Tracks (`A1`–`A8`)** and **MIDI Tracks (`M1`–`M8`)** |

---

## 4. SD Card Directory Structure (`/Capstan`)

Capstan stores all system settings, songs, projects, and presets inside `/Capstan`:

```text
/Capstan/
├── SYSTEM.DAT                        # Brightness, Metronome Vol, Last Song, Global MIDI CC Map
├── PROJECTS/
│   └── <PROJECT>.proj                # Ordered song setlists for live performance
├── INSTRUMENT_PRESETS/
│   └── <PRESET>.FMI                  # Global instrument patches (FM, PhaseDist, Sampler, NoiseDrone)
└── SONGS/
    └── <SONG_NAME>/
        ├── SONG.DAT                  # Tempo, Punch/Loop window, Master FX, 8-track Audio mixer settings
        ├── MIDI.DAT                  # 8-track MIDI sequence events & routing
        ├── INST.DAT                  # 8 song instrument patches, sampler key maps, & per-inst MIDI maps
        ├── <SONG_NAME>.wav           # Exported Master stereo mixdown (from Song Settings -> EXPORT)
        ├── AUDIO/
        │   ├── TRACK1.WAV..TRACK4.WAV # 44.1 kHz 16-bit Stereo track recordings
        │   └── TRACK5.WAV..TRACK8.WAV # 44.1 kHz 16-bit Mono track recordings
        ├── MIDI/
        │   └── *.mid                 # Exported / imported Standard MIDI Files
        ├── INSTRUMENTS/
        │   └── *.FMI                 # Song-local instrument presets
        └── SAMPLES/
            └── *.WAV                 # Recorded sampler key audio files
```

> **Note**: Songs use **manual save only** from the **Main (`SAVED SONGS`)** page (`[ SAVE ]`).

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

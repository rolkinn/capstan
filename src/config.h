// config.h — constants for Capstan: 8-Track Audio + 8-Track MIDI Tape Recorder
#pragma once
#include <stdint.h>

#ifndef FLASHMEM
#define FLASHMEM
#endif

// Firmware version formatted as "x.y" (updated only when explicitly requested)
#ifndef CAPSTAN_VERSION
#define CAPSTAN_VERSION "1.4"
#endif

// Compile-time debug flag (0 in normal/release builds, 1 via `python build_all.py --debug`)
#ifndef CAPSTAN_DEBUG
#define CAPSTAN_DEBUG 0
#endif

namespace looper {

constexpr const char *ROOT_DIR = "/Capstan";
constexpr const char *SONGS_DIR = "/Capstan/SONGS";
constexpr const char *PROJECTS_DIR = "/Capstan/PROJECTS";
constexpr const char *INST_PRESETS_DIR = "/Capstan/INSTRUMENT_PRESETS";
constexpr const char *SOUNDFONTS_DIR = "/Capstan/SOUNDFONTS";
constexpr int MAX_SONGS = 64;                      // Max named song folders listed from SD card
constexpr int MAX_SONG_NAME_LEN = 12;              // Max characters in a song folder name
constexpr int MAX_PROJECTS = 64;                   // Max .proj files listed from /Capstan/PROJECTS
constexpr int MAX_PROJECT_NAME_LEN = 12;           // Max characters in a project name
constexpr int MAX_SONGS_PER_PROJECT = 64;          // Max ordered song entries in a .proj file

// Track counts & Internal Instruments
constexpr int NUM_STEREO_AUDIO_TRACKS = 4;         // Tracks 1..4: 16-bit Stereo PCM (4 bytes/frame)
constexpr int NUM_MONO_AUDIO_TRACKS = 4;           // Tracks 5..8: 16-bit Mono PCM (2 bytes/frame)
constexpr int NUM_AUDIO_TRACKS = NUM_STEREO_AUDIO_TRACKS + NUM_MONO_AUDIO_TRACKS;
constexpr int NUM_MIDI_TRACKS = 8;                 // 8 MIDI tracks (M1..M8, default out -> I01..I08)
constexpr bool isAudioTrackMono(int t) { return t >= NUM_STEREO_AUDIO_TRACKS; }
constexpr uint32_t audioTrackBytesPerFrame(int t) { return isAudioTrackMono(t) ? 2u : 4u; }
constexpr uint32_t audioTrackChannels(int t) { return isAudioTrackMono(t) ? 1u : 2u; }
constexpr int NUM_INSTRUMENTS = 8;                 // 8 instrument slots per song (I01..I08)
constexpr int VOICES_PER_INSTRUMENT = 8;           // 8 dedicated polyphonic voices per instrument (I01..I08)
constexpr int NUM_FM_VOICES = 32;                  // 32-voice dynamically shared global polyphony pool
constexpr uint8_t MIDI_IN_OFF = 17;                // 0 = ALL (Omni), 1..16 = Ch 01..16, 17 = OFF
constexpr int MAX_MIDI_OUT_CHAN = 16 + NUM_INSTRUMENTS; // 0 = OFF, 1..16 = Ext MIDI C01..C16, 17..24 = Inst I01..I08

// Time & Tape length: up to 666 seconds per track at 44.1 kHz stereo
constexpr uint32_t SAMPLE_RATE_HZ = 44100;
constexpr uint32_t MAX_TAPE_SECONDS = 666;
constexpr uint32_t MAX_REC_FRAMES = MAX_TAPE_SECONDS * SAMPLE_RATE_HZ;   // 29,370,600 stereo frames (~117.48 MB per track)
constexpr int BEATS_PER_BAR = 4;                   // 4/4 time signature
constexpr uint32_t MIDI_PPQN = 480;                // 480 ticks per quarter note

// Streaming buffers (stereo frames packed as uint32_t: L in low 16, R in high 16; mono as uint16_t)
// 8 audio tracks (4 Stereo x 42 KB + 4 Mono x 21 KB = 252 KB total in DMAMEM / RAM2),
// plus a 32 KB (8,192 stereo frames) input & record history ring in RAM1.
constexpr uint32_t RING_FRAMES = 10752;            // ~244 ms lookahead per audio track (252 KB total in DMAMEM)
constexpr uint32_t STREAM_FRAMES = RING_FRAMES - 256;
constexpr uint32_t CHUNK_FRAMES = 4096;            // 16 KB per SD read/write transfer
constexpr uint32_t INPUT_FRAMES = 8192;            // 32 KB (~185.8 ms) input & record history ring in RAM1

constexpr bool LOG_ENGINE_LOAD = (CAPSTAN_DEBUG != 0);

// Tempo range
constexpr float MIN_BPM = 40.0f, MAX_BPM = 240.0f;
constexpr float DEFAULT_BPM = 120.0f;

// Playback speed & direction (in 0.25x quarter-steps: -20 = -5.00x .. 0 = 0.00x .. +4 = 1.00x .. +20 = 5.00x)
constexpr int MIN_SPEED_QUARTER = -20;
constexpr int MAX_SPEED_QUARTER = 20;
constexpr int DEFAULT_SPEED_QUARTER = 4;
constexpr int REVERSE_1X_SPEED_QUARTER = -4;

// MIDI hardware flags
constexpr bool MIDI_OUT_ENABLED = true;
constexpr bool MIDI_IN_ENABLED = true;
constexpr bool MIDI_USB_ENABLED = true;

// Waveform overview columns for each audio track on the Audio Page
constexpr int OVERVIEW_COLS = 180;

// Timeline zoom levels (Level 1 = 4 bars, Level 2 = 16 bars, Level 3 = 32 bars, Level 4 = Full)
enum TimelineZoom : uint8_t {
  ZOOM_4_BARS  = 0,
  ZOOM_16_BARS = 1,
  ZOOM_32_BARS = 2,
  ZOOM_FULL    = 3,
  TIMELINE_ZOOM_COUNT = 4
};

// Fixed-resolution audio waveform peak bins (8 bins/sec packed as 4-bit nibbles across 666s)
constexpr uint32_t WAVE_PEAK_FPS = 8;
constexpr uint32_t FRAMES_PER_PEAK_BIN = SAMPLE_RATE_HZ / WAVE_PEAK_FPS;
constexpr int WAVE_PEAK_BINS = MAX_TAPE_SECONDS * WAVE_PEAK_FPS;
constexpr int WAVE_PEAK_BYTES = (WAVE_PEAK_BINS + 1) / 2;

// Volume and Panning (Balance) steps
// Volume: 0 = Off (-inf), 1..80 = -23.7 dB .. 0 dB (unity at 80), 81..100 = +0.3 dB .. +6.0 dB
constexpr int VOLUME_MAX = 100, VOLUME_UNITY = 80;
constexpr float VOLUME_STEP_DB = 0.3f;
constexpr int PAN_MAX = 100;                       // -100 (full Left) .. 0 (Center) .. +100 (full Right)

// Master volume:
//   Speaker (unplugged): 0 = Off, 1..80 = -23.7..0.0 dB (0.3 dB/step, -12.0 dB at default 40), 81..100 = +0.6..+12.04 dB
//   Headphones (Option A): 0 = Off (MUTE_HP), 1..80 = -51.5..0.0 dB analog (~0.652 dB/step, -19.6 dB at default 50),
//                          81..100 = +0.7..+14.0 dB (+6.0 dB clean SGTL5000 analog ceiling + up to +8.0 dB limiter-guarded digital)
constexpr int MASTER_VOLUME_MAX = 100, MASTER_VOLUME_UNITY = 80;
constexpr int MASTER_VOLUME_SPK_DEFAULT = 40;
constexpr int MASTER_VOLUME_HP_DEFAULT = 50;
constexpr int MASTER_VOLUME_DEFAULT = MASTER_VOLUME_SPK_DEFAULT;
constexpr float MASTER_TOP_GAIN = 4.0f;
constexpr float MASTER_ATTEN_STEP_DB = 0.3f;
constexpr float MASTER_STEP_DB = 12.0412f / (MASTER_VOLUME_MAX - MASTER_VOLUME_UNITY);
constexpr float MASTER_HP_MIN_DB = -51.5f;
constexpr float MASTER_HP_MAX_ANA_DB = 6.0f;
constexpr float MASTER_HP_MAX_TOTAL_DB = 14.0f;
constexpr float MASTER_HP_ATTEN_STEP_DB = (-MASTER_HP_MIN_DB) / (float)(MASTER_VOLUME_UNITY - 1);
constexpr float MASTER_HP_BOOST_STEP_DB = MASTER_HP_MAX_TOTAL_DB / (float)(MASTER_VOLUME_MAX - MASTER_VOLUME_UNITY);

inline float masterStepDb(int step) {
  return (step <= MASTER_VOLUME_UNITY)
             ? (step - MASTER_VOLUME_UNITY) * MASTER_ATTEN_STEP_DB
             : (step - MASTER_VOLUME_UNITY) * MASTER_STEP_DB;
}

inline float masterHeadphoneStepDb(int step) {
  return (step <= MASTER_VOLUME_UNITY)
             ? (step - MASTER_VOLUME_UNITY) * MASTER_HP_ATTEN_STEP_DB
             : (step - MASTER_VOLUME_UNITY) * MASTER_HP_BOOST_STEP_DB;
}

// Screen brightness (5..100, scaled to 0..255 PWM), Metronome volume (0..100, default 80),
// & Line-In Gain (-12.0..+12.0 dB, default 0.0 dB)
constexpr int16_t MIN_LINE_IN_GAIN_TENTHS = -120;      // -12.0 dB
constexpr int16_t MAX_LINE_IN_GAIN_TENTHS = 120;       // +12.0 dB
constexpr int16_t DEFAULT_LINE_IN_GAIN_TENTHS = 0;     //   0.0 dB
constexpr uint8_t SGTL_UNITY_LINE_IN_LEVEL = 8;        // SGTL5000 0.0 dB step (0..15, 1.5 dB/step)
constexpr uint8_t SGTL_MAX_LINE_IN_LEVEL = 15;         // SGTL5000 +10.5 dB step

constexpr uint8_t MIN_BRIGHTNESS = 5;
constexpr uint8_t MAX_BRIGHTNESS = 100;
constexpr uint8_t DEFAULT_BRIGHTNESS = 50;
constexpr uint8_t DEFAULT_METRO_VOLUME = VOLUME_UNITY;

constexpr uint8_t brightnessToPwm(uint8_t pct) {
  return (uint8_t)(((uint32_t)(pct > MAX_BRIGHTNESS ? MAX_BRIGHTNESS : pct) * 255u + 50u) / 100u);
}

// Limiter & smoothing
constexpr float LIMIT_CEILING = 32000.0f;
constexpr float LIMIT_RELEASE_MS = 150.0f;
constexpr float GAIN_GLIDE_MS = 1.5f;
constexpr float MASTER_GLIDE_MS = 15.0f;

// Audio threshold range (dBFS)
constexpr int MIN_THRESH_DB = -80, MAX_THRESH_DB = -6, DEFAULT_THRESH_DB = -60;

// WAV header offset (sector aligned)
constexpr uint32_t WAV_DATA_OFFSET = 512;

// MIDI Sequencer capacity
constexpr uint32_t MAX_MIDI_EVENTS_PER_TRACK = 2048;   // 8 tracks x 2048 x 8 bytes = 128 KB in DMAMEM

// Screen blanking after inactivity (5 minutes)
constexpr uint32_t SCREEN_BLANK_TIMEOUT_MS = 5UL * 60UL * 1000UL;

}  // namespace looper

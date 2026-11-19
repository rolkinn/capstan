// tuner.h — Chromatic Tuner DSP, Lock-Free ISR Audio Tap, & Per-Song A4 Reference
#pragma once
#include <stdint.h>

namespace looper {

enum class TunerSource : uint8_t {
  Off    = 0,
  LineIn = 1,
  UsbIn  = 2,
  Inst1  = 3,
  Inst2  = 4,
  Inst3  = 5,
  Inst4  = 6,
  Inst5  = 7,
  Inst6  = 8,
  Inst7  = 9,
  Inst8  = 10,
  Audio1 = 11,
  Audio2 = 12,
  Audio3 = 13,
  Audio4 = 14,
  Audio5 = 15,
  Audio6 = 16,
  Audio7 = 17,
  Audio8 = 18
};

constexpr uint8_t TUNER_SELECTABLE_SOURCES = 18; // LineIn (1) .. Audio8 (18)

constexpr uint16_t MIN_TUNER_REF_TENTHS     = 4000; // 400.0 Hz
constexpr uint16_t MAX_TUNER_REF_TENTHS     = 4800; // 480.0 Hz
constexpr uint16_t DEFAULT_TUNER_REF_TENTHS = 4400; // 440.0 Hz

struct TunerReadout {
  bool locked;
  int8_t midiNote;      // Nearest MIDI note (69 = A4, scientific pitch)
  float hz;             // Smoothed fundamental frequency in Hz
  float cents;          // Offset from target note in cents (-50.0 .. +50.0)
  float levelNorm;      // 0.0 .. 1.0 dB-mapped input level (-60 dBFS .. 0 dBFS)
  bool clipping;        // True when input approaches 0 dBFS
  uint8_t strobePhase8; // 0..255 continuous phase offset for strobe rendering
};

// Lock-free state read by Audio ISR (`LooperEngine::update` & `fmSynthRenderBlock`)
extern volatile TunerSource tunerActiveSource;
extern volatile bool tunerMuteActive;
extern volatile bool tunerInstPushedThisBlock;

// ISR-callable capture taps (sum L+R and decimate 2x into RAM1 tunerRing)
void tunerPushStereo16(const int16_t *inL, const int16_t *inR, int numFrames);
void tunerPushPackedStereo16(const uint32_t *packedLR, int numFrames);
void tunerPushStereo32(const int32_t *inL, const int32_t *inR, int numFrames);
void tunerPushMono16(const int16_t *mono, int numFrames);
void tunerPushSilenceBlock(int numFrames);

// Per-song A4 reference frequency in tenths of Hz (4000..4800)
uint16_t tunerRefTenths();
void setTunerRefTenths(uint16_t tenths);

// Main-loop Tuner lifecycle & 20 Hz YIN / 30 Hz UI readout update
void tunerActivate(TunerSource src, bool muteOutput);
void tunerDeactivate();
void tunerResetDetector();
void tunerService(TunerReadout &out);
bool tunerMatchRefToCurrentPitch(uint16_t &outTenths);
const char *tunerSourceLabel(TunerSource src);

} // namespace looper

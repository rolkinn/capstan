// inst_phasedist.h — Modular Phase Distortion Synthesizer Instrument Engine
#pragma once
#include "instruments.h"

namespace looper {

constexpr int NUM_PD_WAVES = 8;
constexpr int NUM_PD_WAVE2_OPTS = 9;     // 0 = Off, 1..8 = PdWave 0..7
constexpr int NUM_PD_MOD_MODES = 4;      // Phase, Ring, Sync, Noise
constexpr uint16_t PD_TUNE_MIN = 0;      // -24.00 semitones
constexpr uint16_t PD_TUNE_ZERO = 2400;  //   0.00 semitones
constexpr uint16_t PD_TUNE_MAX = 4800;   // +24.00 semitones

enum class PdWave : uint8_t {
  Saw = 0,       // Sawtooth
  Square = 1,    // Square
  Pulse = 2,     // Pulse
  DblSine = 3,   // Double Sine
  SawPulse = 4,  // Saw-Pulse
  Reso1 = 5,     // Resonance I (Saw window)
  Reso2 = 6,     // Resonance II (Triangle window)
  Reso3 = 7      // Resonance III (Trapezoid window)
};

enum class PdModMode : uint8_t {
  Phase = 0,     // Line 2 phase-modulates Line 1
  Ring = 1,      // Ring Modulation (Line 1 * Line 2)
  Sync = 2,      // Windowed Hard Sync (Line 2 synced to Line 1)
  Noise = 3      // Noise + Line 2 Phase Modulation on Line 1
};

const char *pdSynthWave1Name(uint8_t wave);
const char *pdSynthWave2Name(uint8_t waveOpt);
const char *pdSynthModName(uint8_t mod);
float pdSynthTuneSemitones(uint16_t tuneX100);

// Returns the modular engine descriptor for InstType::PhaseDist
const InstEngineOps &instPhaseDistEngine();

}  // namespace looper

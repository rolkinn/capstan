// inst_fm2op.h — Modular 2-Operator FM Synthesizer Instrument Engine
#pragma once
#include "instruments.h"

namespace looper {

constexpr int NUM_FM_WAVES = 5;
constexpr int NUM_FM_ALGOS = 3;
constexpr uint16_t FM_RATIO_MIN = 0;     // 0.00
constexpr uint16_t FM_RATIO_MAX = 2400;  // 24.00

enum class FmAlgo : uint8_t {
  Parallel = 0,  // Algorithm 1: [A] [B]   — Operator A & Operator B are both carriers (no FM modulation)
  Fm       = 1,  // Algorithm 2: [B] > [A] — Operator A is carrier, Operator B is modulator
  FmAB     = 2   // Algorithm 3: [A] > [B] — Operator A is modulator, Operator B is carrier
};

enum class FmWave : uint8_t {
  Sine = 0,      // Pure Sine
  Triangle = 1,  // Triangle
  Saw = 2,       // Sawtooth
  Square = 3,    // Square
  Noise = 4      // White Noise
};

const char *fmSynthWaveName(uint8_t wave);
float fmSynthRatioFloat(uint16_t ratioX100);

// Returns the modular engine descriptor for InstType::Fm2Op
const InstEngineOps &instFm2OpEngine();

}  // namespace looper

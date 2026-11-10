// inst_noisedrone.h — Modular Noise Texture Drone Synthesizer Instrument Engine
#pragma once
#include "instruments.h"

namespace looper {

constexpr int NUM_ND_WAVES = 6;
constexpr int NUM_ND_NOISE_TYPES = 6;
constexpr int NUM_ND_TRIG_MODES = 3;
constexpr int NUM_ND_CRACKLE_TYPES = 4;

constexpr uint16_t ND_TUNE_MIN  = 0;     // -24.00 semitones
constexpr uint16_t ND_TUNE_ZERO = 2400;  //   0.00 semitones
constexpr uint16_t ND_TUNE_MAX  = 4800;  // +24.00 semitones

enum class NdWave : uint8_t {
  Sine   = 0,  // Warm Sine + harmonic fold
  SubSaw = 1,  // Dark Saw + Triangle blend
  Swarm  = 2,  // Detuned multi-oscillator supersaw drone
  Metal  = 3,  // Inharmonic metallic FM pipe/bell drone
  Grind  = 4,  // Wavefolded harsh industrial drone
  Pulse  = 5   // Hollow PWM pulse drone
};

enum class NdNoiseType : uint8_t {
  Tape  = 0,  // Warm analog tape hiss
  Pink  = 1,  // Natural -3 dB/oct ambient pink noise
  Brown = 2,  // Deep -6 dB/oct Brownian rumble / distant wind
  Wind  = 3,  // Resonant bandpass howling wind
  Vinyl = 4,  // Turntable platter rumble & groove friction hiss
  Crush = 5   // Sample-and-hold decimated digital static
};

enum class NdTrigMode : uint8_t {
  Gate  = 0,  // Standard MIDI note / audition gate
  Latch = 1,  // Toggle infinite hold on note / EDIT trigger
  Auto  = 2   // Automatically drones while tape transport is running
};

enum class NdCrackleType : uint8_t {
  Vinyl  = 0,  // Vinyl surface dust & warm stylus pops
  Fire   = 1,  // Bonfire clustered snapping twigs & embers
  Static = 2,  // Harsh electrical arcs & Geiger static clicks
  Drop   = 3   // Resonant damped raindrop / metallic pings
};

const char *ndSynthWaveName(uint8_t wave);
const char *ndSynthNoiseTypeName(uint8_t noiseType);
const char *ndSynthTrigModeName(uint8_t trigMode);
const char *ndSynthCrackleTypeName(uint8_t crackleType);
float ndSynthTuneSemitones(uint16_t tuneX100);

void ndSynthResetAllVoices();
void ndSynthResetVoice(int voiceIdx);

// Returns the modular engine descriptor for InstType::NoiseDrone
const InstEngineOps &instNoiseDroneEngine();

}  // namespace looper

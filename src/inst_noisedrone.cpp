// inst_noisedrone.cpp — Modular Noise Texture Drone Synthesizer Instrument Engine
#include "inst_noisedrone.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace looper {

PROGMEM static const char *const ND_WAVE_NAMES[NUM_ND_WAVES] = {
  "Sine", "SubSaw", "Swarm", "Metal", "Grind", "Pulse"
};

PROGMEM static const char *const ND_NOISE_TYPE_NAMES[NUM_ND_NOISE_TYPES] = {
  "Tape", "Pink", "Brown", "Wind", "Vinyl", "Crush"
};

PROGMEM static const char *const ND_TRIG_MODE_NAMES[NUM_ND_TRIG_MODES] = {
  "Gate", "Latch", "Auto"
};

PROGMEM static const char *const ND_CRACKLE_TYPE_NAMES[NUM_ND_CRACKLE_TYPES] = {
  "Vinyl", "Fire", "Static", "Drop"
};

// Packed helper accessors for POP (low byte of op[1].ratioX100) and DRIFT (high byte of op[1].ratioX100)
static inline uint8_t ndGetPop(const FmPatch &p) {
  return (uint8_t)(p.op[1].ratioX100 & 0xFFu);
}

static inline uint8_t ndGetDrift(const FmPatch &p) {
  return (uint8_t)((p.op[1].ratioX100 >> 8) & 0xFFu);
}

static inline void ndSetPop(FmPatch &p, uint8_t pop) {
  uint8_t drift = ndGetDrift(p);
  p.op[1].ratioX100 = (uint16_t)pop | ((uint16_t)drift << 8);
}

static inline void ndSetDrift(FmPatch &p, uint8_t drift) {
  uint8_t pop = ndGetPop(p);
  p.op[1].ratioX100 = (uint16_t)pop | ((uint16_t)drift << 8);
}

// Default Noise Texture Drone Patch in PROGMEM
// Parameter mapping inside 48-byte FmPatch:
//   Section 0 (DRONE OSC):
//     p.op[0].wave        = WAVE  (0..5: Sine, SubSaw, Swarm, Metal, Grind, Pulse)
//     p.op[0].ratioX100   = TUNE  (0..4800, 2400 = 0.00 st)
//     p.op[0].feedback    = SHAPE (0..100: detune / wavefold / metallic grit / PWM)
//     p.op[0].reserved[0] = SUB   (0..100: -1 octave sub-oscillator weight)
//     p.op[0].level       = LVL   (0..100: Drone Oscillator output level)
//   Section 1 (NOISE BED):
//     p.op[1].wave        = TYPE  (0..5: Tape, Pink, Brown, Wind, Vinyl, Crush)
//     p.op[1].reserved[0] = FREQ  (0..100: Noise Bed filter cutoff / center frequency)
//     p.op[1].feedback    = RESO  (0..100: Noise Bed filter resonance / wind whistle)
//     p.op[1].reserved[1] = KTRK  (0..100: MIDI note key tracking for noise/crackle filter)
//     p.op[1].level       = LVL   (0..100: Noise Bed output level)
//   Section 2 (CRACKLE & MOD):
//     p.algorithm         = TRIG  (0..2: Gate, Latch, Auto)
//     p.op[0].reserved[1] = CRKL  (0..3: Vinyl, Fire, Static, Drop)
//     p.op[0].reserved[2] = DENS  (0..100: Crackle/pop density & impulse rate)
//     p.op[1].ratioX100   = POP (low byte 0..100) | (DRIFT (high byte 0..100) << 8)
//     p.op[1].reserved[2] = PAN   (int8_t -100..+100: voice stereo pan)
PROGMEM static const FmPatch ND_DEFAULT_PATCH = {
  "ND_01", (uint8_t)InstType::NoiseDrone, 0, (uint8_t)NdTrigMode::Gate, {
    {(uint8_t)NdWave::Grind,     25, ND_TUNE_ZERO, 50, {0,   (uint8_t)NdCrackleType::Vinyl, 10}},
    {(uint8_t)NdNoiseType::Tape,  3, (uint16_t)(10u | (50u << 8)), 0, {100, 0, (uint8_t)(int8_t)0}}
  },
  {127, 17, 60, 127, 90},
  {127, 17, 60, 127, 90},
  {0, 0, 0, 0, 0}, 0
};

// Per-voice DSP state in cached RAM2 (DMAMEM) — 48 bytes/voice (1,536 B total), zero RAM1 footprint
struct NdVoiceState {
  union {
    float pink0;
    float crushHold;
  };
  float pink1;
  float pink2;
  float brown;
  float bedLp;
  float bedBp;
  float crklEnv;
  float crklBp;
  uint32_t lfoPhase;
  uint32_t dropPhase;
  uint32_t dropStep;
  uint16_t burstWait;
  uint8_t crushCnt;
  uint8_t burstRem : 2;
  uint8_t subFlip : 1;
  uint8_t popIsDeep : 1;
};

DMAMEM static NdVoiceState ndVoices[NUM_FM_VOICES];

FLASHMEM void ndSynthResetAllVoices() {
  memset(ndVoices, 0, sizeof(ndVoices));
}

FLASHMEM void ndSynthResetVoice(int voiceIdx) {
  if (voiceIdx < 0 || voiceIdx >= NUM_FM_VOICES) return;
  memset(&ndVoices[voiceIdx], 0, sizeof(NdVoiceState));
  ndVoices[voiceIdx].lfoPhase = (uint32_t)voiceIdx * 0x1F35A7BDu;
}

float ndSynthTuneSemitones(uint16_t tuneX100) {
  int clamped = constrain((int)tuneX100, (int)ND_TUNE_MIN, (int)ND_TUNE_MAX);
  return (float)(clamped - (int)ND_TUNE_ZERO) / 100.0f;
}

FLASHMEM const char *ndSynthWaveName(uint8_t wave) {
  return ND_WAVE_NAMES[constrain((int)wave, 0, NUM_ND_WAVES - 1)];
}

FLASHMEM const char *ndSynthNoiseTypeName(uint8_t noiseType) {
  return ND_NOISE_TYPE_NAMES[constrain((int)noiseType, 0, NUM_ND_NOISE_TYPES - 1)];
}

FLASHMEM const char *ndSynthTrigModeName(uint8_t trigMode) {
  return ND_TRIG_MODE_NAMES[constrain((int)trigMode, 0, NUM_ND_TRIG_MODES - 1)];
}

FLASHMEM const char *ndSynthCrackleTypeName(uint8_t crackleType) {
  return ND_CRACKLE_TYPE_NAMES[constrain((int)crackleType, 0, NUM_ND_CRACKLE_TYPES - 1)];
}

FLASHMEM static const FmPatch &noiseDroneDefaultPatch() {
  return ND_DEFAULT_PATCH;
}

FLASHMEM static void noiseDroneValidatePatch(FmPatch &p) {
  // Section 0 (DRONE OSC)
  p.op[0].wave        = (uint8_t)constrain((int)p.op[0].wave, 0, NUM_ND_WAVES - 1);
  p.op[0].ratioX100   = (uint16_t)constrain((int)p.op[0].ratioX100, (int)ND_TUNE_MIN, (int)ND_TUNE_MAX);
  p.op[0].feedback    = (uint8_t)constrain((int)p.op[0].feedback, 0, 100);
  p.op[0].reserved[0] = (uint8_t)constrain((int)p.op[0].reserved[0], 0, 100);
  p.op[0].level       = (uint8_t)constrain((int)p.op[0].level, 0, 100);

  // Section 1 (NOISE BED)
  p.op[1].wave        = (uint8_t)constrain((int)p.op[1].wave, 0, NUM_ND_NOISE_TYPES - 1);
  p.op[1].reserved[0] = (uint8_t)constrain((int)p.op[1].reserved[0], 0, 100);
  p.op[1].feedback    = (uint8_t)constrain((int)p.op[1].feedback, 0, 100);
  p.op[1].reserved[1] = (uint8_t)constrain((int)p.op[1].reserved[1], 0, 100);
  p.op[1].level       = (uint8_t)constrain((int)p.op[1].level, 0, 100);

  // Section 2 (CRACKLE & MOD)
  p.algorithm         = (uint8_t)constrain((int)p.algorithm, 0, NUM_ND_TRIG_MODES - 1);
  p.op[0].reserved[1] = (uint8_t)constrain((int)p.op[0].reserved[1], 0, NUM_ND_CRACKLE_TYPES - 1);
  p.op[0].reserved[2] = (uint8_t)constrain((int)p.op[0].reserved[2], 0, 100);
  uint8_t pop         = (uint8_t)constrain((int)ndGetPop(p), 0, 100);
  uint8_t drift       = (uint8_t)constrain((int)ndGetDrift(p), 0, 100);
  p.op[1].ratioX100   = (uint16_t)pop | ((uint16_t)drift << 8);
  int bal             = constrain((int)(int8_t)p.op[1].reserved[2], -100, 100);
  p.op[1].reserved[2] = (uint8_t)(int8_t)bal;
}

FLASHMEM static void noiseDroneFormatMainCell(const FmPatch &p, int opIdx, int col,
                                              const char *&outLabel, char *outVal, size_t valSz,
                                              int &outBarVal, int &outBarMax) {
  if (opIdx == 0) {
    // Section 0 (DRONE OSC): WAVE, SHAPE, TUNE, SUB, LVL
    const FmOpParams &op = p.op[0];
    if (col == 0) {
      outLabel = "WAVE";
      snprintf(outVal, valSz, "%s", ndSynthWaveName(op.wave));
      outBarVal = (int)op.wave + 1;
      outBarMax = NUM_ND_WAVES;
    } else if (col == 1) {
      outLabel = "SHAPE";
      uint8_t shp = constrain((int)op.feedback, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)shp);
      outBarVal = shp;
      outBarMax = 100;
    } else if (col == 2) {
      outLabel = "TUNE";
      int signedX100 = constrain((int)op.ratioX100, (int)ND_TUNE_MIN, (int)ND_TUNE_MAX) - (int)ND_TUNE_ZERO;
      int absX100 = abs(signedX100);
      const char *signStr = (signedX100 < 0) ? "-" : (signedX100 > 0 ? "+" : "");
      snprintf(outVal, valSz, "%s%d.%02d", signStr, absX100 / 100, absX100 % 100);
      outBarVal = constrain((int)op.ratioX100, (int)ND_TUNE_MIN, (int)ND_TUNE_MAX);
      outBarMax = ND_TUNE_MAX;
    } else if (col == 3) {
      outLabel = "SUB";
      uint8_t sub = constrain((int)op.reserved[0], 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)sub);
      outBarVal = sub;
      outBarMax = 100;
    } else {
      outLabel = "LVL";
      uint8_t lvl = constrain((int)op.level, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)lvl);
      outBarVal = lvl;
      outBarMax = 100;
    }
  } else if (opIdx == 1) {
    // Section 1 (NOISE BED): TYPE, FREQ, RESO, KTRK, LVL
    const FmOpParams &op = p.op[1];
    if (col == 0) {
      outLabel = "TYPE";
      snprintf(outVal, valSz, "%s", ndSynthNoiseTypeName(op.wave));
      outBarVal = (int)op.wave + 1;
      outBarMax = NUM_ND_NOISE_TYPES;
    } else if (col == 1) {
      outLabel = "FREQ";
      uint8_t freq = constrain((int)op.reserved[0], 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)freq);
      outBarVal = freq;
      outBarMax = 100;
    } else if (col == 2) {
      outLabel = "RESO";
      uint8_t reso = constrain((int)op.feedback, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)reso);
      outBarVal = reso;
      outBarMax = 100;
    } else if (col == 3) {
      outLabel = "KTRK";
      uint8_t ktrk = constrain((int)op.reserved[1], 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)ktrk);
      outBarVal = ktrk;
      outBarMax = 100;
    } else {
      outLabel = "LVL";
      uint8_t lvl = constrain((int)op.level, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)lvl);
      outBarVal = lvl;
      outBarMax = 100;
    }
  } else {
    // Section 2 (CRACKLE & MOD): CRKL, DENS, POP, DRIFT, PAN, TRIG
    if (col == 0) {
      outLabel = "CRKL";
      uint8_t crkl = constrain((int)p.op[0].reserved[1], 0, NUM_ND_CRACKLE_TYPES - 1);
      snprintf(outVal, valSz, "%s", ndSynthCrackleTypeName(crkl));
      outBarVal = (int)crkl + 1;
      outBarMax = NUM_ND_CRACKLE_TYPES;
    } else if (col == 1) {
      outLabel = "DENS";
      uint8_t dens = constrain((int)p.op[0].reserved[2], 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)dens);
      outBarVal = dens;
      outBarMax = 100;
    } else if (col == 2) {
      outLabel = "POP";
      uint8_t pop = constrain((int)ndGetPop(p), 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)pop);
      outBarVal = pop;
      outBarMax = 100;
    } else if (col == 3) {
      outLabel = "DRIFT";
      uint8_t drift = constrain((int)ndGetDrift(p), 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)drift);
      outBarVal = drift;
      outBarMax = 100;
    } else if (col == 4) {
      outLabel = "PAN";
      int pan = constrain((int)(int8_t)p.op[1].reserved[2], -100, 100);
      if (pan > 0) snprintf(outVal, valSz, "+%d", pan);
      else snprintf(outVal, valSz, "%d", pan);
      outBarVal = pan + 100;
      outBarMax = 200;
    } else {
      outLabel = "TRIG";
      uint8_t trig = constrain((int)p.algorithm, 0, NUM_ND_TRIG_MODES - 1);
      snprintf(outVal, valSz, "%s", ndSynthTrigModeName(trig));
      outBarVal = (int)trig + 1;
      outBarMax = NUM_ND_TRIG_MODES;
    }
  }
}

FLASHMEM static void noiseDroneAdjustMainCell(FmPatch &p, int opIdx, int col, int dx, int dy, int delta, int step) {
  if (opIdx == 0) {
    FmOpParams &op = p.op[0];
    if (col == 0) {
      op.wave = (uint8_t)(((int)op.wave + delta + NUM_ND_WAVES) % NUM_ND_WAVES);
    } else if (col == 1) {
      op.feedback = (uint8_t)constrain((int)op.feedback + step, 0, 100);
    } else if (col == 2) {
      int tStep = (dy != 0) ? (dy * 100) : dx;
      op.ratioX100 = (uint16_t)constrain((int)op.ratioX100 + tStep, (int)ND_TUNE_MIN, (int)ND_TUNE_MAX);
    } else if (col == 3) {
      op.reserved[0] = (uint8_t)constrain((int)op.reserved[0] + step, 0, 100);
    } else if (col == 4) {
      op.level = (uint8_t)constrain((int)op.level + step, 0, 100);
    }
  } else if (opIdx == 1) {
    FmOpParams &op = p.op[1];
    if (col == 0) {
      op.wave = (uint8_t)(((int)op.wave + delta + NUM_ND_NOISE_TYPES) % NUM_ND_NOISE_TYPES);
    } else if (col == 1) {
      op.reserved[0] = (uint8_t)constrain((int)op.reserved[0] + step, 0, 100);
    } else if (col == 2) {
      op.feedback = (uint8_t)constrain((int)op.feedback + step, 0, 100);
    } else if (col == 3) {
      op.reserved[1] = (uint8_t)constrain((int)op.reserved[1] + step, 0, 100);
    } else if (col == 4) {
      op.level = (uint8_t)constrain((int)op.level + step, 0, 100);
    }
  } else {
    if (col == 0) {
      p.op[0].reserved[1] = (uint8_t)(((int)p.op[0].reserved[1] + delta + NUM_ND_CRACKLE_TYPES) % NUM_ND_CRACKLE_TYPES);
    } else if (col == 1) {
      p.op[0].reserved[2] = (uint8_t)constrain((int)p.op[0].reserved[2] + step, 0, 100);
    } else if (col == 2) {
      int nextPop = constrain((int)ndGetPop(p) + step, 0, 100);
      ndSetPop(p, (uint8_t)nextPop);
    } else if (col == 3) {
      int nextDrift = constrain((int)ndGetDrift(p) + step, 0, 100);
      ndSetDrift(p, (uint8_t)nextDrift);
    } else if (col == 4) {
      int pan = constrain((int)(int8_t)p.op[1].reserved[2] + step, -100, 100);
      p.op[1].reserved[2] = (uint8_t)(int8_t)pan;
    } else if (col == 5) {
      p.algorithm = (uint8_t)(((int)p.algorithm + delta + NUM_ND_TRIG_MODES) % NUM_ND_TRIG_MODES);
    }
  }
}

FLASHMEM static void noiseDroneResetMainCell(FmPatch &p, const FmPatch &defPatch, int opIdx, int col) {
  if (opIdx == 0) {
    FmOpParams &op = p.op[0];
    const FmOpParams &dOp = defPatch.op[0];
    if (col == 0) op.wave = dOp.wave;
    else if (col == 1) op.feedback = dOp.feedback;
    else if (col == 2) op.ratioX100 = dOp.ratioX100;
    else if (col == 3) op.reserved[0] = dOp.reserved[0];
    else if (col == 4) op.level = dOp.level;
  } else if (opIdx == 1) {
    FmOpParams &op = p.op[1];
    const FmOpParams &dOp = defPatch.op[1];
    if (col == 0) op.wave = dOp.wave;
    else if (col == 1) op.reserved[0] = dOp.reserved[0];
    else if (col == 2) op.feedback = dOp.feedback;
    else if (col == 3) op.reserved[1] = dOp.reserved[1];
    else if (col == 4) op.level = dOp.level;
  } else {
    if (col == 0) p.op[0].reserved[1] = defPatch.op[0].reserved[1];
    else if (col == 1) p.op[0].reserved[2] = defPatch.op[0].reserved[2];
    else if (col == 2) ndSetPop(p, ndGetPop(defPatch));
    else if (col == 3) ndSetDrift(p, ndGetDrift(defPatch));
    else if (col == 4) p.op[1].reserved[2] = defPatch.op[1].reserved[2];
    else if (col == 5) p.algorithm = defPatch.algorithm;
  }
}

static inline float fastSawFromPhase(uint32_t ph) {
  return ((float)(ph >> 16) * (1.0f / 32768.0f)) - 1.0f;
}

static inline float fastTriFromPhase(uint32_t ph) {
  uint32_t p = ph >> 16;
  return (p < 32768u) ? ((float)p * (1.0f / 16384.0f) - 1.0f)
                      : (3.0f - (float)p * (1.0f / 16384.0f));
}

static inline float ndFastExp2f(float x) {
  int32_t i = (int32_t)x;
  if (x < (float)i) i--;
  float f = x - (float)i;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * 0.0555041f));
  union { uint32_t u; float fl; } scale;
  scale.u = (uint32_t)((i + 127) << 23);
  return p * scale.fl;
}

static void noiseDroneRenderVoice(FmVoice &v, const FmPatch &p, const FmInstRuntime &rt,
                                  int32_t *outL, int32_t *outR, int numFrames) {
  const FmVoice *baseVoice = fmSynthVoiceAt(0);
  int vIdx = baseVoice ? (int)(&v - baseVoice) : 0;
  vIdx = constrain(vIdx, 0, NUM_FM_VOICES - 1);
  NdVoiceState &ns = ndVoices[vIdx];

  // Section 0: DRONE OSC parameters
  const uint8_t droneWave = constrain((int)p.op[0].wave, 0, NUM_ND_WAVES - 1);
  const float tuneSt = ndSynthTuneSemitones(p.op[0].ratioX100);
  const float basePitchRatio = ndFastExp2f(tuneSt * (1.0f / 12.0f)) * rt.pitchBendRatio;
  const float baseDroneStepF = (float)v.baseStep * basePitchRatio;
  const float shapeNorm = constrain(constrain((int)p.op[0].feedback, 0, 100) * 0.01f + rt.modWheelBoost * 0.30f, 0.0f, 1.0f);
  const float subLvl = constrain((int)p.op[0].reserved[0], 0, 100) * 0.01f;
  const float droneLvl = constrain((int)p.op[0].level, 0, 100) * 0.01f;
  const bool runDrone = (droneLvl > 0.0f || subLvl > 0.0f);

  // Section 1: NOISE BED parameters
  const uint8_t noiseType = constrain((int)p.op[1].wave, 0, NUM_ND_NOISE_TYPES - 1);
  const float freqBase = constrain((int)p.op[1].reserved[0], 0, 100) * 0.01f;
  const float resoNorm = constrain((int)p.op[1].feedback, 0, 100) * 0.01f;
  const float ktrkNorm = constrain((int)p.op[1].reserved[1], 0, 100) * 0.01f;
  const float noiseLvl = constrain((int)p.op[1].level, 0, 100) * 0.01f;
  const float keyOffsetNorm = ktrkNorm * ((float)((int)v.note - 60) * (1.0f / 48.0f));
  const bool runNoise = (noiseLvl > 0.0f);

  // Section 2: CRACKLE & MOD parameters
  const uint8_t crklType = constrain((int)p.op[0].reserved[1], 0, NUM_ND_CRACKLE_TYPES - 1);
  const float densNorm = constrain((int)p.op[0].reserved[2], 0, 100) * 0.01f;
  const float popLvl = constrain((int)ndGetPop(p), 0, 100) * 0.01f;
  const float driftNorm = constrain((int)ndGetDrift(p), 0, 100) * 0.01f;
  const float bal = constrain((int)(int8_t)p.op[1].reserved[2], -100, 100) * 0.01f;
  const float panL = (bal > 0.0f) ? (1.0f - bal) : 1.0f;
  const float panR = (bal < 0.0f) ? (1.0f + bal) : 1.0f;
  const bool runCrackle = (densNorm > 0.0f && popLvl > 0.0f);

  // Normalize overall voice gain so layered Drone + Noise + Crackle maintains headroom
  float layerSum = droneLvl + 0.65f * subLvl + noiseLvl + 0.7f * popLvl;
  float normGain = (layerSum > 1.15f) ? (1.15f / (0.55f + 0.52f * layerSum)) : 1.0f;
  const float voiceGain = 10000.0f * normGain * v.velScale;

  // Control-rate variables updated every 8 samples (~5.5 kHz)
  uint32_t step0 = (uint32_t)baseDroneStepF;
  uint32_t step1 = step0;
  uint32_t stepSwarm2 = step0;
  uint32_t pulseThresh = 0x80000000u;
  float bedF = 0.15f;
  const float bedQ = 1.65f - 1.50f * resoNorm;
  uint8_t crushPeriod = 1;
  float trigProbThresh = 0.0f;
  const uint32_t lfoInc = (uint32_t)((0.15f + 2.15f * driftNorm * driftNorm) * (8.0f * 4294967296.0f / (float)SAMPLE_RATE_HZ));

  int startIdx = (v.startSampleOffset < (uint8_t)numFrames) ? (int)v.startSampleOffset : 0;
  v.startSampleOffset = 0;
  for (int i = startIdx; i < numFrames; i++) {
    // 1. Advance shared General FLT and AMP envelopes
    if (!instAdvanceVoiceEnvelopes(v)) {
      break;
    }

    // 2. Control-rate update every 8 samples (~5.5 kHz): Drift LFO, pitch steps, SVF cutoff & crackle rate
    if (i == startIdx || (i & 7) == 0) {
      ns.lfoPhase += lfoInc;
      uint32_t lfoIdx1 = (ns.lfoPhase >> (32 - INST_LUT_BITS)) & INST_LUT_MASK;
      uint32_t lfoIdx2 = (((ns.lfoPhase * 3u) >> (32 - INST_LUT_BITS)) + 256u) & INST_LUT_MASK;
      float lfoRaw = 0.65f * instSinLutAt(lfoIdx1) + 0.35f * instSinLutAt(lfoIdx2);
      float driftMod = driftNorm * (0.78f * lfoRaw + 0.22f * ns.brown);

      if (runDrone) {
        float pitchDrift = 1.0f + driftMod * 0.014f;
        float s0 = baseDroneStepF * pitchDrift;
        step0 = (uint32_t)s0;
        if (droneWave == (uint8_t)NdWave::SubSaw) {
          step1 = (uint32_t)(s0 * (1.0f + 0.008f * shapeNorm));
        } else if (droneWave == (uint8_t)NdWave::Swarm) {
          float spread = 0.003f + 0.022f * shapeNorm;
          step1 = (uint32_t)(s0 * (1.0f + spread));
          stepSwarm2 = (uint32_t)(s0 * (1.0f - 0.87f * spread));
        } else if (droneWave == (uint8_t)NdWave::Metal) {
          step1 = (uint32_t)(s0 * (1.4142f + 1.732f * shapeNorm));
        } else if (droneWave == (uint8_t)NdWave::Grind) {
          step1 = (uint32_t)(s0 * (1.0f + 0.498f * shapeNorm));
        } else if (droneWave == (uint8_t)NdWave::Pulse) {
          float pw = constrain(0.50f - 0.41f * shapeNorm + 0.14f * driftMod, 0.06f, 0.94f);
          pulseThresh = (uint32_t)(pw * 4294967296.0f);
          step1 = (uint32_t)(s0 * 1.4983f); // 5th harmonic undertone
        }
      }

      float cutoffNorm = constrain(freqBase + keyOffsetNorm + driftMod * 0.32f +
                                   (v.fltAmt * v.fltEnv - 0.5f) * 0.24f,
                                   0.02f, 0.98f);
      bedF = 0.012f + cutoffNorm * cutoffNorm * (0.28f + 0.58f * cutoffNorm);
      int cp = 1 + (int)((1.0f - cutoffNorm) * (1.0f - cutoffNorm) * 44.0f);
      crushPeriod = (uint8_t)constrain(cp, 1, 48);

      if (runCrackle) {
        // Map DENS (0..1) quadratically to per-sample trigger threshold (up to ~140 impulses/sec at 100)
        float d2 = densNorm * densNorm;
        float gust = 1.0f + 0.35f * driftMod;
        trigProbThresh = (0.00004f + 0.0032f * d2) * max(0.25f, gust);
      }
    }

    float wn = instNextWhiteNoise();
    // Maintain slow Brownian random walk state (used by Brown/Wind/Vinyl noise & organic Drift)
    ns.brown = constrain(0.986f * ns.brown + 0.11f * wn, -1.25f, 1.25f);

    // 3. Evaluate Section 0: DRONE OSC
    float droneOut = 0.0f;
    if (runDrone) {
      uint32_t ph0 = v.opPhase[0];
      uint32_t ph1 = v.opPhase[1];
      float osc = 0.0f;

      switch (droneWave) {
        case (uint8_t)NdWave::Sine:
        default: {
          uint32_t fbShift = (uint32_t)((int32_t)(v.opPrev[0] * shapeNorm * 750000000.0f));
          float s = instSinLutAt(((ph0 + fbShift) >> (32 - INST_LUT_BITS)) & INST_LUT_MASK);
          if (shapeNorm > 0.01f) {
            uint32_t foldIdx = ((uint32_t)((s * (1.0f + 1.8f * shapeNorm) + 2.0f) * (0.25f * (float)INST_LUT_SIZE))) & INST_LUT_MASK;
            s = (1.0f - 0.55f * shapeNorm) * s + (0.55f * shapeNorm) * instSinLutAt(foldIdx);
          }
          v.opPrev[0] = 0.5f * (v.opPrev[0] + s);
          osc = s;
          break;
        }

        case (uint8_t)NdWave::SubSaw: {
          float saw0 = fastSawFromPhase(ph0);
          float tri1 = fastTriFromPhase(ph1);
          osc = (1.0f - 0.55f * shapeNorm) * tri1 + (0.45f + 0.55f * shapeNorm) * (0.65f * saw0 + 0.35f * fastSawFromPhase(ph1));
          break;
        }

        case (uint8_t)NdWave::Swarm: {
          v.pdCycleParity += stepSwarm2;
          float s0 = fastSawFromPhase(ph0);
          float s1 = fastSawFromPhase(ph1);
          float s2 = fastSawFromPhase(v.pdCycleParity);
          osc = 0.42f * s0 + 0.33f * s1 + 0.25f * s2;
          break;
        }

        case (uint8_t)NdWave::Metal: {
          uint32_t fb = (uint32_t)((int32_t)(v.opPrev[0] * (0.15f + 0.35f * shapeNorm) * 1073741824.0f));
          float mod = instSinLutAt(((ph1 + fb) >> (32 - INST_LUT_BITS)) & INST_LUT_MASK);
          v.opPrev[0] = 0.5f * (v.opPrev[0] + mod);
          uint32_t pm = (uint32_t)((int32_t)(mod * (0.25f + 1.85f * shapeNorm) * 1073741824.0f));
          osc = instSinLutAt(((ph0 + pm) >> (32 - INST_LUT_BITS)) & INST_LUT_MASK);
          break;
        }

        case (uint8_t)NdWave::Grind: {
          float saw0 = fastSawFromPhase(ph0);
          float pls1 = (ph1 < 0x80000000u) ? 1.0f : -1.0f;
          float ring = (1.0f - 0.5f * shapeNorm) * saw0 + (0.5f * shapeNorm) * (saw0 * pls1);
          float drive = 0.25f + 1.65f * shapeNorm;
          uint32_t foldIdx = ((uint32_t)((ring * drive + 4.0f) * (float)INST_LUT_SIZE)) & INST_LUT_MASK;
          osc = instSinLutAt(foldIdx);
          break;
        }

        case (uint8_t)NdWave::Pulse: {
          float p0 = (ph0 < pulseThresh) ? 0.85f : -0.85f;
          float p1 = fastTriFromPhase(ph1);
          osc = p0 + (0.35f * shapeNorm) * p1;
          break;
        }
      }

      uint32_t nextPh0 = ph0 + step0;
      if (nextPh0 < ph0) {
        ns.subFlip ^= 1u;
      }
      v.opPhase[0] = nextPh0;
      v.opPhase[1] = ph1 + step1;

      float subSig = 0.0f;
      if (subLvl > 0.0f) {
        uint32_t subPh = (nextPh0 >> 1) | ((uint32_t)ns.subFlip << 31);
        subSig = instSinLutAt((subPh >> (32 - INST_LUT_BITS)) & INST_LUT_MASK);
      }
      droneOut = osc * droneLvl + subSig * subLvl;
    }

    // 4. Evaluate Section 1: NOISE BED
    float noiseOut = 0.0f;
    if (runNoise) {
      float pink = 0.0f;
      if (noiseType != (uint8_t)NdNoiseType::Brown && noiseType != (uint8_t)NdNoiseType::Crush) {
        // 3-stage cascaded one-pole pinking filter
        ns.pink0 = 0.992f * ns.pink0 + 0.028f * wn;
        ns.pink1 = 0.960f * ns.pink1 + 0.065f * wn;
        ns.pink2 = 0.820f * ns.pink2 + 0.145f * wn;
        pink = ns.pink0 + ns.pink1 + ns.pink2 + 0.09f * wn;
      }

      float bedSrc = pink;
      switch (noiseType) {
        case (uint8_t)NdNoiseType::Tape:
          // Warm analog tape hiss: high-mid air + subtle low head bump
          bedSrc = 0.55f * (wn - ns.pink1) + 0.45f * pink;
          break;
        case (uint8_t)NdNoiseType::Pink:
          bedSrc = pink;
          break;
        case (uint8_t)NdNoiseType::Brown:
          bedSrc = ns.brown * 1.15f;
          break;
        case (uint8_t)NdNoiseType::Wind:
          bedSrc = 0.65f * pink + 0.55f * ns.brown;
          break;
        case (uint8_t)NdNoiseType::Vinyl:
          // Turntable platter rumble + veiled groove surface friction
          bedSrc = 0.75f * ns.brown + 0.38f * (pink - ns.pink0);
          break;
        case (uint8_t)NdNoiseType::Crush:
          if (++ns.crushCnt >= crushPeriod) {
            ns.crushCnt = 0;
            // Quantize to 8 discrete levels for gritty digital static
            ns.crushHold = (float)((int)(wn * 4.0f)) * 0.25f;
          }
          bedSrc = ns.crushHold;
          break;
      }

      // 2-pole Chamberlin State Variable Filter for Noise Bed FREQ & RESO
      float hp = constrain(bedSrc - ns.bedLp - bedQ * ns.bedBp, -2.0f, 2.0f);
      ns.bedBp = constrain(ns.bedBp + bedF * hp, -2.0f, 2.0f);
      ns.bedLp = constrain(ns.bedLp + bedF * ns.bedBp, -2.0f, 2.0f);

      float shapedNoise;
      if (noiseType == (uint8_t)NdNoiseType::Wind) {
        shapedNoise = ns.bedBp * (1.25f + 0.65f * resoNorm) + 0.20f * ns.bedLp;
      } else if (noiseType == (uint8_t)NdNoiseType::Tape) {
        shapedNoise = 0.65f * ns.bedBp + 0.55f * ns.bedLp;
      } else {
        shapedNoise = ns.bedLp + (0.75f * resoNorm) * ns.bedBp;
      }
      noiseOut = shapedNoise * noiseLvl;
    }

    // 5. Evaluate Section 2: CRACKLE & MOD (Vinyl Pops, Bonfire Crackles, Static, Raindrops)
    float crackleOut = 0.0f;
    if (runCrackle) {
      bool triggerImpulse = false;
      if (ns.burstRem > 0) {
        if (ns.burstWait > 0) {
          ns.burstWait--;
        } else {
          ns.burstRem--;
          triggerImpulse = true;
        }
      } else if (fabsf(wn) < trigProbThresh) {
        triggerImpulse = true;
        if (crklType == (uint8_t)NdCrackleType::Fire && (wn > 0.0f)) {
          // Bonfire twig-snap micro-cluster: queue 1..3 rapid follow-up snaps (3..18 ms apart)
          ns.burstRem = 1u + ((uint32_t)(ns.lfoPhase >> 11) & 2u);
          ns.burstWait = (uint16_t)(130u + ((ns.lfoPhase >> 16) & 0x02FFu));
        }
      }

      if (triggerImpulse) {
        uint32_t rndBits = (uint32_t)((int32_t)(wn * 2147483647.0f)) ^ ns.lfoPhase;
        float randAmp = 0.40f + 0.60f * ((float)(rndBits & 0xFFu) * (1.0f / 255.0f));
        ns.crklEnv = randAmp;
        ns.popIsDeep = ((rndBits & 0x300u) == 0u) ? 1u : 0u; // 25% deep thump pop, 75% crisp dust tick
        float dropFreqNorm = constrain(freqBase + keyOffsetNorm + ((float)((rndBits >> 8) & 0x7Fu) * (1.0f / 220.0f)), 0.05f, 0.95f);
        float dropHz = 280.0f + dropFreqNorm * dropFreqNorm * 3400.0f;
        ns.dropStep = (uint32_t)(dropHz * (4294967296.0f / (float)SAMPLE_RATE_HZ));
      }

      if (ns.crklEnv > 0.001f) {
        float imp = 0.0f;
        switch (crklType) {
          case (uint8_t)NdCrackleType::Vinyl:
          default: {
            if (ns.popIsDeep) {
              // Warm low-mid stylus pop
              ns.crklEnv *= 0.986f;
              ns.dropPhase += (ns.dropStep >> 2);
              imp = instSinLutAt((ns.dropPhase >> (32 - INST_LUT_BITS)) & INST_LUT_MASK) * ns.crklEnv * 1.15f;
            } else {
              // Crisp high-passed surface dust tick (stores crklLp in v.opPrev[1])
              ns.crklEnv *= 0.935f;
              float tick = (wn - v.opPrev[1]) * ns.crklEnv;
              v.opPrev[1] += 0.35f * (wn - v.opPrev[1]);
              imp = tick * 1.35f;
            }
            break;
          }

          case (uint8_t)NdCrackleType::Fire: {
            // Snapping wood transient + warm ember body resonance
            ns.crklEnv *= (ns.popIsDeep ? 0.988f : 0.955f);
            float exc = wn * ns.crklEnv;
            float hp = exc - v.opPrev[1] - 0.35f * ns.crklBp;
            float cf = ns.popIsDeep ? 0.14f : 0.36f;
            ns.crklBp = constrain(ns.crklBp + cf * hp, -2.0f, 2.0f);
            v.opPrev[1] = constrain(v.opPrev[1] + cf * ns.crklBp, -2.0f, 2.0f);
            imp = (0.75f * ns.crklBp + 0.45f * exc) * 1.35f;
            if (ns.burstWait == 1 && ns.burstRem > 0) {
              ns.burstWait = (uint16_t)(110u + ((ns.dropPhase >> 19) & 0x027Fu));
            }
            break;
          }

          case (uint8_t)NdCrackleType::Static: {
            // Harsh clipped electrical arc / Geiger burst
            ns.crklEnv *= 0.968f;
            float arc = wn * ns.crklEnv * 2.4f;
            imp = constrain(arc, -1.1f, 1.1f);
            break;
          }

          case (uint8_t)NdCrackleType::Drop: {
            // Resonant damped sine droplet / metallic ping with slight pitch drop
            ns.crklEnv *= 0.9925f;
            ns.dropPhase += ns.dropStep;
            ns.dropStep -= (ns.dropStep >> 11);
            imp = instSinLutAt((ns.dropPhase >> (32 - INST_LUT_BITS)) & INST_LUT_MASK) * ns.crklEnv * 1.25f;
            break;
          }
        }
        crackleOut = imp * popLvl;
      } else {
        ns.crklEnv = 0.0f;
      }
    }

    // 6. Sum Drone Osc + Noise Bed + Crackle, apply shared Voice Filter & Amp, and pan L/R
    float rawMix = droneOut + noiseOut + crackleOut;
    float voiced = instApplyVoiceFilterAndAmp(v, rawMix, voiceGain);
    if (bal == 0.0f) {
      int32_t sInt = (int32_t)voiced;
      outL[i] += sInt;
      outR[i] += sInt;
    } else {
      outL[i] += (int32_t)(voiced * panL);
      outR[i] += (int32_t)(voiced * panR);
    }
  }
}

PROGMEM static const InstEngineOps NOISEDRONE_ENGINE_OPS = {
  InstType::NoiseDrone,
  "NOISE DRONE",
  "NTD",
  "ND",
  "DRONE OSC",
  "NOISE BED",
  "CRACKLE & MOD",
  noiseDroneDefaultPatch,
  noiseDroneValidatePatch,
  noiseDroneFormatMainCell,
  noiseDroneAdjustMainCell,
  noiseDroneResetMainCell,
  noiseDroneRenderVoice
};

FLASHMEM const InstEngineOps &instNoiseDroneEngine() {
  return NOISEDRONE_ENGINE_OPS;
}

}  // namespace looper

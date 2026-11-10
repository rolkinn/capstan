// inst_fm2op.cpp — Modular 2-Operator FM Synthesizer Instrument Engine
#include "inst_fm2op.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>

namespace looper {

PROGMEM static const char *const FM_WAVE_NAMES[NUM_FM_WAVES] = {
  "Sine", "Tri", "Saw", "Square", "Noise"
};

// Default 2-Op FM Patch in PROGMEM
// op[0] = Operator A, op[1] = Operator B
// algorithm: 0 = FmAlgo::Parallel ([A] [B]), 1 = FmAlgo::Fm ([B] > [A])
// FmOpParams:   { wave, level, ratioX100, feedback, {0, 0, 0} }
// InstEnvParams:{ amt, attack, decay, sustain, release } (0..127)
// InstFxParams: { drive, chorus, delay, delayFeedback, reverb } (0..100)
PROGMEM static const FmPatch FM_DEFAULT_PATCH = {
  "FM_01", (uint8_t)InstType::Fm2Op, 0, (uint8_t)FmAlgo::Fm, {
    {(uint8_t)FmWave::Sine,     85,  100,  4, {0, 0, 0}},
    {(uint8_t)FmWave::Sine,     42,  200, 18, {0, 0, 0}}
  },
  {127,  0, 27,  41, 16},
  {127,  0, 17,  15, 13},
  {0, 0, 0, 0, 0}, 0
};

FLASHMEM const char *fmSynthWaveName(uint8_t wave) {
  return FM_WAVE_NAMES[constrain((int)wave, 0, NUM_FM_WAVES - 1)];
}

float fmSynthRatioFloat(uint16_t ratioX100) {
  return (float)constrain((int)ratioX100, (int)FM_RATIO_MIN, (int)FM_RATIO_MAX) / 100.0f;
}

FLASHMEM static const FmPatch &fm2OpDefaultPatch() {
  return FM_DEFAULT_PATCH;
}

FLASHMEM static void fm2OpValidatePatch(FmPatch &p) {
  p.algorithm = (uint8_t)constrain((int)p.algorithm, 0, NUM_FM_ALGOS - 1);
  for (int k = 0; k < 2; k++) {
    p.op[k].wave = (uint8_t)constrain((int)p.op[k].wave, 0, NUM_FM_WAVES - 1);
    p.op[k].level = (uint8_t)constrain((int)p.op[k].level, 0, 100);
    p.op[k].ratioX100 = (uint16_t)constrain((int)p.op[k].ratioX100, (int)FM_RATIO_MIN, (int)FM_RATIO_MAX);
    p.op[k].feedback = (uint8_t)constrain((int)p.op[k].feedback, 0, 100);
  }
}

FLASHMEM static void fm2OpFormatMainCell(const FmPatch &p, int opIdx, int col,
                                         const char *&outLabel, char *outVal, size_t valSz,
                                         int &outBarVal, int &outBarMax) {
  const FmOpParams &op = p.op[constrain(opIdx, 0, 1)];
  if (col == 0) {
    outLabel = "WAVE";
    snprintf(outVal, valSz, "%s", fmSynthWaveName(op.wave));
    outBarVal = (int)op.wave + 1;
    outBarMax = NUM_FM_WAVES;
  } else if (col == 1) {
    outLabel = "RATIO";
    uint16_t r100 = constrain((int)op.ratioX100, (int)FM_RATIO_MIN, (int)FM_RATIO_MAX);
    snprintf(outVal, valSz, "%u.%02u", (unsigned)(r100 / 100), (unsigned)(r100 % 100));
    outBarVal = r100;
    outBarMax = FM_RATIO_MAX;
  } else if (col == 2) {
    outLabel = "LEVEL";
    snprintf(outVal, valSz, "%u", (unsigned)op.level);
    outBarVal = op.level;
    outBarMax = 100;
  } else {
    outLabel = "FDBK";
    snprintf(outVal, valSz, "%u", (unsigned)op.feedback);
    outBarVal = op.feedback;
    outBarMax = 100;
  }
}

FLASHMEM static void fm2OpAdjustMainCell(FmPatch &p, int opIdx, int col, int dx, int dy, int delta, int step) {
  FmOpParams &op = p.op[constrain(opIdx, 0, 1)];
  if (col == 0) {
    op.wave = (uint8_t)(((int)op.wave + delta + NUM_FM_WAVES) % NUM_FM_WAVES);
  } else if (col == 1) {
    int rStep = (dy != 0) ? (dy * 100) : dx;
    op.ratioX100 = (uint16_t)constrain((int)op.ratioX100 + rStep, (int)FM_RATIO_MIN, (int)FM_RATIO_MAX);
  } else if (col == 2) {
    op.level = (uint8_t)constrain((int)op.level + step, 0, 100);
  } else if (col == 3) {
    op.feedback = (uint8_t)constrain((int)op.feedback + step, 0, 100);
  }
}

FLASHMEM static void fm2OpResetMainCell(FmPatch &p, const FmPatch &defPatch, int opIdx, int col) {
  int k = constrain(opIdx, 0, 1);
  FmOpParams &op = p.op[k];
  const FmOpParams &dOp = defPatch.op[k];
  if (col == 0) op.wave = dOp.wave;
  else if (col == 1) op.ratioX100 = dOp.ratioX100;
  else if (col == 2) op.level = dOp.level;
  else if (col == 3) op.feedback = dOp.feedback;
}

// Evaluate FM oscillator waveform at 32-bit unsigned phase (returns -1.0f .. +1.0f)
static inline float evalOscWave(uint8_t wave, uint32_t phase) {
  uint32_t idx = (phase >> (32 - INST_LUT_BITS)) & INST_LUT_MASK;
  switch (wave) {
    case (uint8_t)FmWave::Sine:
    default:
      return instSinLutAt(idx);

    case (uint8_t)FmWave::Triangle: {
      uint32_t p = phase >> 16; // 0..65535
      return (p < 32768u) ? ((float)p / 16384.0f - 1.0f)
                          : (3.0f - (float)p / 16384.0f);
    }

    case (uint8_t)FmWave::Saw: {
      uint32_t p = phase >> 16; // 0..65535
      return ((float)p / 32768.0f) - 1.0f;
    }

    case (uint8_t)FmWave::Square:
      return (phase < 0x80000000u) ? 1.0f : -1.0f;

    case (uint8_t)FmWave::Noise:
      return instNextWhiteNoise();
  }
}

static void fm2OpRenderVoice(FmVoice &v, const FmPatch &p, const FmInstRuntime &rt,
                             int32_t *outL, int32_t *outR, int numFrames) {
  float aRatio = fmSynthRatioFloat(p.op[0].ratioX100) * rt.pitchBendRatio;
  float bRatio = fmSynthRatioFloat(p.op[1].ratioX100) * rt.pitchBendRatio;
  uint32_t aStep = (uint32_t)((float)v.baseStep * aRatio);
  uint32_t bStep = (uint32_t)((float)v.baseStep * bRatio);

  float opALvl = constrain((int)p.op[0].level, 0, 100) / 100.0f;
  float opBLvl = constrain((int)p.op[1].level, 0, 100) / 100.0f;

  // Self-feedback phase scale for Operator A (op[0]) and Operator B (op[1])
  float fbANorm = constrain((int)p.op[0].feedback, 0, 100) / 100.0f;
  float fbBNorm = constrain((int)p.op[1].feedback, 0, 100) / 100.0f;
  float fbAPhaseScale = (fbANorm * fbANorm) * 0.36f * 2147483648.0f;
  float fbBPhaseScale = (fbBNorm * fbBNorm) * 0.36f * 2147483648.0f;

  const uint8_t algo = p.algorithm;

  // Algorithm 2 ([B] > [A]): Operator B modulates Operator A
  // Algorithm 3 ([A] > [B]): Operator A modulates Operator B
  float modBaseLvl = (algo == (uint8_t)FmAlgo::FmAB) ? opALvl : opBLvl;
  float effModLvl = min(1.25f, modBaseLvl * v.velScale + rt.modWheelBoost * 0.35f);
  float modPhaseScale = effModLvl * 2.2f * 2147483648.0f;

  // Algorithm 1 ([A] [B]): Both Operator A and Operator B are independent carriers
  float sumLvl = opALvl + opBLvl;
  float invSum = (sumLvl > 0.0001f) ? (1.0f / sumLvl) : 0.0f;
  float parallelCarLvl = sumLvl / (1.0f + 0.45f * min(opALvl, opBLvl));
  float carLvl = (algo == (uint8_t)FmAlgo::Fm)   ? opALvl
               : (algo == (uint8_t)FmAlgo::FmAB) ? opBLvl
                                                 : parallelCarLvl;
  float voiceGain = 10000.0f * carLvl * v.velScale;

  int startIdx = (v.startSampleOffset < (uint8_t)numFrames) ? (int)v.startSampleOffset : 0;
  v.startSampleOffset = 0;
  for (int i = startIdx; i < numFrames; i++) {
    // 1. Advance shared General FLT and AMP envelopes
    if (!instAdvanceVoiceEnvelopes(v)) {
      break;
    }

    float rawOut = 0.0f;
    if (algo == (uint8_t)FmAlgo::FmAB) {
      // Algorithm 3 ([A] > [B]): Operator A is modulator, Operator B is carrier
      uint32_t fbAOffset = (uint32_t)((int32_t)(v.opPrev[0] * fbAPhaseScale));
      float opARaw = evalOscWave(p.op[0].wave, v.opPhase[0] + fbAOffset);
      v.opPrev[0] = 0.5f * (v.opPrev[0] + opARaw);
      v.opPhase[0] += aStep;

      float modSignal = opARaw * (v.fltAmt * v.fltEnv);
      uint32_t pmOffset = (uint32_t)((int32_t)(modSignal * modPhaseScale));
      uint32_t fbBOffset = (uint32_t)((int32_t)(v.opPrev[1] * fbBPhaseScale));
      float opBRaw = evalOscWave(p.op[1].wave, v.opPhase[1] + pmOffset + fbBOffset);
      v.opPrev[1] = 0.5f * (v.opPrev[1] + opBRaw);
      v.opPhase[1] += bStep;

      rawOut = opBRaw;
    } else {
      // Operator B (op[1]) with 1-sample averaged self-feedback
      uint32_t fbBOffset = (uint32_t)((int32_t)(v.opPrev[1] * fbBPhaseScale));
      float opBRaw = evalOscWave(p.op[1].wave, v.opPhase[1] + fbBOffset);
      v.opPrev[1] = 0.5f * (v.opPrev[1] + opBRaw);
      v.opPhase[1] += bStep;

      // Operator A (op[0]):
      //   - Algorithm 1 ([A] [B]): Pure carrier + OpA self-feedback, mixed with Operator B carrier
      //   - Algorithm 2 ([B] > [A]): Carrier phase-modulated by Operator B (shaped by FLT env) + OpA feedback
      uint32_t pmOffset = 0;
      if (algo == (uint8_t)FmAlgo::Fm) {
        float modSignal = opBRaw * (v.fltAmt * v.fltEnv);
        pmOffset = (uint32_t)((int32_t)(modSignal * modPhaseScale));
      }
      uint32_t fbAOffset = (uint32_t)((int32_t)(v.opPrev[0] * fbAPhaseScale));
      float opARaw = evalOscWave(p.op[0].wave, v.opPhase[0] + pmOffset + fbAOffset);
      v.opPrev[0] = 0.5f * (v.opPrev[0] + opARaw);
      v.opPhase[0] += aStep;

      rawOut = (algo == (uint8_t)FmAlgo::Fm) ? opARaw : ((opALvl * opARaw + opBLvl * opBRaw) * invSum);
    }

    float sample = instApplyVoiceFilterAndAmp(v, rawOut, voiceGain);
    int32_t sInt = (int32_t)sample;
    outL[i] += sInt;
    outR[i] += sInt;
  }
}

PROGMEM static const InstEngineOps FM2OP_ENGINE_OPS = {
  InstType::Fm2Op,
  "2-OP FM",
  "2OP",
  "FM",
  "OPERATOR A",
  "OPERATOR B",
  nullptr,
  fm2OpDefaultPatch,
  fm2OpValidatePatch,
  fm2OpFormatMainCell,
  fm2OpAdjustMainCell,
  fm2OpResetMainCell,
  fm2OpRenderVoice
};

FLASHMEM const InstEngineOps &instFm2OpEngine() {
  return FM2OP_ENGINE_OPS;
}

}  // namespace looper

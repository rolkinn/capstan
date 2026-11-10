// inst_phasedist.cpp — Modular Phase Distortion Synthesizer Instrument Engine
#include "inst_phasedist.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace looper {

PROGMEM static const char *const PD_WAVE_NAMES[NUM_PD_WAVES] = {
  "Saw", "Square", "Pulse", "DblSin", "SawPls", "Reso1", "Reso2", "Reso3"
};

PROGMEM static const char *const PD_MOD_NAMES[NUM_PD_MOD_MODES] = {
  "Phase", "Ring", "Sync", "Noise"
};

// Default Dual-Line Phase Distortion Patch in PROGMEM
// Per-Line storage in FmOpParams op[k] (k = 0: Line 1, k = 1: Line 2):
//   op[k].wave        = WAVE1 (0..7: PdWave::Saw .. PdWave::Reso3)
//   op[k].level       = LVL   (0..100: DCA output level for Line k)
//   op[k].ratioX100   = k==0 ? AMT (0..100 Line 2 > Line 1 mod depth)
//                            : TUNE (0..4800, 2400 = 0.00 st Line 2 pitch offset)
//   op[k].feedback    = ENV   (0..100: FLT envelope -> DCW depth for Line k)
//   op[k].reserved[0] = WAVE2 (0 = Off, 1..8 = PdWave + 1 alternating half-cycle wave)
//   op[k].reserved[1] = DCW   (0..100: base DCW timbre brightness for Line k)
//   op[k].reserved[2] = BAL   (int8_t -100..+100: stereo balance/pan for Line k)
// p.algorithm         = MOD   (0..3: PdModMode::Phase, Ring, Sync, Noise)
PROGMEM static const FmPatch PD_DEFAULT_PATCH = {
  "PD_01", (uint8_t)InstType::PhaseDist, 0, (uint8_t)PdModMode::Phase, {
    {(uint8_t)PdWave::Saw,      85,   12, 68, {1 + (uint8_t)PdWave::SawPulse, 22, (uint8_t)(int8_t)-15}},
    {(uint8_t)PdWave::Saw,      65, 2408, 60, {0,                             18, (uint8_t)(int8_t)+15}}
  },
  {127,  6, 20, 104, 13},
  {127, 10, 18,  74, 11},
  {0, 0, 0, 0, 0}, 0
};

float pdSynthTuneSemitones(uint16_t tuneX100) {
  int clamped = constrain((int)tuneX100, (int)PD_TUNE_MIN, (int)PD_TUNE_MAX);
  return (float)(clamped - (int)PD_TUNE_ZERO) / 100.0f;
}

FLASHMEM const char *pdSynthWave1Name(uint8_t wave) {
  return PD_WAVE_NAMES[constrain((int)wave, 0, NUM_PD_WAVES - 1)];
}

FLASHMEM const char *pdSynthWave2Name(uint8_t waveOpt) {
  if (waveOpt == 0) return "Off";
  return PD_WAVE_NAMES[constrain((int)waveOpt - 1, 0, NUM_PD_WAVES - 1)];
}

FLASHMEM const char *pdSynthModName(uint8_t mod) {
  return PD_MOD_NAMES[constrain((int)mod, 0, NUM_PD_MOD_MODES - 1)];
}

FLASHMEM static const FmPatch &phaseDistDefaultPatch() {
  return PD_DEFAULT_PATCH;
}

FLASHMEM static void phaseDistValidatePatch(FmPatch &p) {
  p.algorithm = (uint8_t)constrain((int)p.algorithm, 0, NUM_PD_MOD_MODES - 1);
  p.op[0].ratioX100 = (uint16_t)constrain((int)p.op[0].ratioX100, 0, 100);
  p.op[1].ratioX100 = (uint16_t)constrain((int)p.op[1].ratioX100, (int)PD_TUNE_MIN, (int)PD_TUNE_MAX);

  for (int k = 0; k < 2; k++) {
    p.op[k].wave = (uint8_t)constrain((int)p.op[k].wave, 0, NUM_PD_WAVES - 1);
    p.op[k].reserved[0] = (uint8_t)constrain((int)p.op[k].reserved[0], 0, NUM_PD_WAVE2_OPTS - 1);
    p.op[k].reserved[1] = (uint8_t)constrain((int)p.op[k].reserved[1], 0, 100);
    p.op[k].feedback = (uint8_t)constrain((int)p.op[k].feedback, 0, 100);
    p.op[k].level = (uint8_t)constrain((int)p.op[k].level, 0, 100);
    int bal = constrain((int)(int8_t)p.op[k].reserved[2], -100, 100);
    p.op[k].reserved[2] = (uint8_t)(int8_t)bal;
  }
}

FLASHMEM static void phaseDistFormatMainCell(const FmPatch &p, int opIdx, int col,
                                             const char *&outLabel, char *outVal, size_t valSz,
                                             int &outBarVal, int &outBarMax) {
  if (opIdx == 0 || opIdx == 1) {
    // Section 0 (LINE 1) & Section 1 (LINE 2): WAVE1, WAVE2, DCW, ENV, LVL, BAL
    const FmOpParams &op = p.op[opIdx];
    if (col == 0) {
      outLabel = "WAVE1";
      snprintf(outVal, valSz, "%s", pdSynthWave1Name(op.wave));
      outBarVal = (int)op.wave + 1;
      outBarMax = NUM_PD_WAVES;
    } else if (col == 1) {
      outLabel = "WAVE2";
      uint8_t w2 = constrain((int)op.reserved[0], 0, NUM_PD_WAVE2_OPTS - 1);
      snprintf(outVal, valSz, "%s", pdSynthWave2Name(w2));
      outBarVal = w2;
      outBarMax = NUM_PD_WAVE2_OPTS - 1;
    } else if (col == 2) {
      outLabel = "DCW";
      uint8_t dcw = constrain((int)op.reserved[1], 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)dcw);
      outBarVal = dcw;
      outBarMax = 100;
    } else if (col == 3) {
      outLabel = "ENV";
      uint8_t env = constrain((int)op.feedback, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)env);
      outBarVal = env;
      outBarMax = 100;
    } else if (col == 4) {
      outLabel = "LVL";
      uint8_t lvl = constrain((int)op.level, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)lvl);
      outBarVal = lvl;
      outBarMax = 100;
    } else {
      outLabel = "BAL";
      int bal = constrain((int)(int8_t)op.reserved[2], -100, 100);
      if (bal > 0) snprintf(outVal, valSz, "+%d", bal);
      else snprintf(outVal, valSz, "%d", bal);
      outBarVal = bal + 100;
      outBarMax = 200;
    }
  } else {
    // Section 2 (LINE 2 > LINE 1): TUNE, MOD, AMT
    if (col == 0) {
      outLabel = "TUNE";
      int signedX100 = constrain((int)p.op[1].ratioX100, (int)PD_TUNE_MIN, (int)PD_TUNE_MAX) - (int)PD_TUNE_ZERO;
      int absX100 = abs(signedX100);
      const char *signStr = (signedX100 < 0) ? "-" : (signedX100 > 0 ? "+" : "");
      snprintf(outVal, valSz, "%s%d.%02d", signStr, absX100 / 100, absX100 % 100);
      outBarVal = constrain((int)p.op[1].ratioX100, (int)PD_TUNE_MIN, (int)PD_TUNE_MAX);
      outBarMax = PD_TUNE_MAX;
    } else if (col == 1) {
      outLabel = "MOD";
      uint8_t mod = constrain((int)p.algorithm, 0, NUM_PD_MOD_MODES - 1);
      snprintf(outVal, valSz, "%s", pdSynthModName(mod));
      outBarVal = (int)mod + 1;
      outBarMax = NUM_PD_MOD_MODES;
    } else {
      outLabel = "AMT";
      uint16_t amt = constrain((int)p.op[0].ratioX100, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)amt);
      outBarVal = amt;
      outBarMax = 100;
    }
  }
}

FLASHMEM static void phaseDistAdjustMainCell(FmPatch &p, int opIdx, int col, int dx, int dy, int delta, int step) {
  if (opIdx == 0 || opIdx == 1) {
    FmOpParams &op = p.op[opIdx];
    if (col == 0) {
      op.wave = (uint8_t)(((int)op.wave + delta + NUM_PD_WAVES) % NUM_PD_WAVES);
    } else if (col == 1) {
      op.reserved[0] = (uint8_t)(((int)op.reserved[0] + delta + NUM_PD_WAVE2_OPTS) % NUM_PD_WAVE2_OPTS);
    } else if (col == 2) {
      op.reserved[1] = (uint8_t)constrain((int)op.reserved[1] + step, 0, 100);
    } else if (col == 3) {
      op.feedback = (uint8_t)constrain((int)op.feedback + step, 0, 100);
    } else if (col == 4) {
      op.level = (uint8_t)constrain((int)op.level + step, 0, 100);
    } else if (col == 5) {
      int bal = constrain((int)(int8_t)op.reserved[2] + step, -100, 100);
      op.reserved[2] = (uint8_t)(int8_t)bal;
    }
  } else {
    if (col == 0) {
      int tStep = (dy != 0) ? (dy * 100) : dx;
      p.op[1].ratioX100 = (uint16_t)constrain((int)p.op[1].ratioX100 + tStep, (int)PD_TUNE_MIN, (int)PD_TUNE_MAX);
    } else if (col == 1) {
      p.algorithm = (uint8_t)(((int)p.algorithm + delta + NUM_PD_MOD_MODES) % NUM_PD_MOD_MODES);
    } else if (col == 2) {
      p.op[0].ratioX100 = (uint16_t)constrain((int)p.op[0].ratioX100 + step, 0, 100);
    }
  }
}

FLASHMEM static void phaseDistResetMainCell(FmPatch &p, const FmPatch &defPatch, int opIdx, int col) {
  if (opIdx == 0 || opIdx == 1) {
    FmOpParams &op = p.op[opIdx];
    const FmOpParams &dOp = defPatch.op[opIdx];
    if (col == 0) op.wave = dOp.wave;
    else if (col == 1) op.reserved[0] = dOp.reserved[0];
    else if (col == 2) op.reserved[1] = dOp.reserved[1];
    else if (col == 3) op.feedback = dOp.feedback;
    else if (col == 4) op.level = dOp.level;
    else if (col == 5) op.reserved[2] = dOp.reserved[2];
  } else {
    if (col == 0) p.op[1].ratioX100 = defPatch.op[1].ratioX100;
    else if (col == 1) p.algorithm = defPatch.algorithm;
    else if (col == 2) p.op[0].ratioX100 = defPatch.op[0].ratioX100;
  }
}

// Phase Distortion cosine lookup: -cos(2 * pi * normPhase) using the shared 1024-point sinLut
static inline float pdCos(float normPhase) {
  uint32_t idx = ((uint32_t)(normPhase * (float)INST_LUT_SIZE) + 768u) & INST_LUT_MASK;
  return instSinLutAt(idx);
}

// Precomputed Phase Distortion reciprocal parameters (updated every 8 samples per voice line)
struct PdWaveCoeffs {
  uint8_t wave;
  float dcw;
  float p;
  float inv0;
  float inv1;
};

static inline PdWaveCoeffs makePdWaveCoeffs(uint8_t wave, float dcw) {
  PdWaveCoeffs c{wave, dcw, 0.5f, 1.0f, 1.0f};
  switch (wave) {
    case (uint8_t)PdWave::Saw:
    default:
      c.p = 0.5f - 0.47f * dcw;
      c.inv0 = 0.5f / c.p;
      c.inv1 = 0.5f / (1.0f - c.p);
      break;
    case (uint8_t)PdWave::Square:
      c.p = 1.0f - 0.88f * dcw;
      c.inv0 = 1.0f / c.p;
      break;
    case (uint8_t)PdWave::Pulse:
      c.p = 1.0f - 0.88f * dcw;
      c.inv0 = 1.0f / c.p;
      c.inv1 = 0.45f * dcw;
      break;
    case (uint8_t)PdWave::DblSine:
      c.p = 1.0f - 0.50f * dcw;
      c.inv0 = 1.0f / c.p;
      c.inv1 = 1.0f / max(0.02f, 1.0f - c.p);
      break;
    case (uint8_t)PdWave::SawPulse:
      c.p = 0.5f * (1.0f - 0.86f * dcw);
      c.inv0 = 0.5f / c.p;
      break;
    case (uint8_t)PdWave::Reso1:
    case (uint8_t)PdWave::Reso2:
    case (uint8_t)PdWave::Reso3:
      c.p = 1.0f + 15.0f * dcw;
      break;
  }
  return c;
}

// Evaluate Phase Distortion waveform at 32-bit phase using precomputed reciprocals (zero divisions in loop)
static inline float evalPdWaveFast(const PdWaveCoeffs &c, uint32_t phase) {
  float phi = (float)(phase >> 8) * (1.0f / 16777216.0f); // 0.0 .. 1.0
  switch (c.wave) {
    case (uint8_t)PdWave::Saw:
    default: {
      float phiPrime = (phi < c.p) ? (phi * c.inv0)
                                   : (0.5f + (phi - c.p) * c.inv1);
      return pdCos(phiPrime);
    }

    case (uint8_t)PdWave::Square: {
      float halfPhi = (phi < 0.5f) ? (phi * 2.0f) : ((phi - 0.5f) * 2.0f);
      float t = min(1.0f, halfPhi * c.inv0) * 0.5f;
      float phiPrime = (phi < 0.5f) ? t : (0.5f + t);
      return pdCos(phiPrime);
    }

    case (uint8_t)PdWave::Pulse: {
      float phiPrime = (phi < c.p) ? (phi * c.inv0) : 1.0f;
      float out = pdCos(phiPrime) + c.inv1;
      return constrain(out, -1.0f, 1.0f);
    }

    case (uint8_t)PdWave::DblSine: {
      float phiPrime = (phi < c.p) ? (phi * c.inv0)
                                   : ((phi - c.p) * c.inv1);
      return (phi < c.p) ? pdCos(phiPrime) : (pdCos(phiPrime) * c.dcw - (1.0f - c.dcw));
    }

    case (uint8_t)PdWave::SawPulse: {
      float phiPrime;
      if (phi < 0.5f) {
        phiPrime = phi;
      } else if (phi - 0.5f < c.p) {
        phiPrime = 0.5f + (phi - 0.5f) * c.inv0;
      } else {
        phiPrime = 1.0f;
      }
      return pdCos(phiPrime);
    }

    case (uint8_t)PdWave::Reso1: {
      float resPhase = phi * c.p;
      resPhase -= (float)(int)resPhase;
      float win = 1.0f - phi;
      return -pdCos(resPhase) * win;
    }

    case (uint8_t)PdWave::Reso2: {
      float resPhase = phi * c.p;
      resPhase -= (float)(int)resPhase;
      float win = 1.0f - fabsf(2.0f * phi - 1.0f);
      return -pdCos(resPhase) * win;
    }

    case (uint8_t)PdWave::Reso3: {
      float resPhase = phi * c.p;
      resPhase -= (float)(int)resPhase;
      float win = (phi < 0.5f) ? 1.0f : (2.0f * (1.0f - phi));
      return -pdCos(resPhase) * win;
    }
  }
}

static void phaseDistRenderVoice(FmVoice &v, const FmPatch &p, const FmInstRuntime &rt,
                                 int32_t *outL, int32_t *outR, int numFrames) {
  uint32_t step1 = (uint32_t)((float)v.baseStep * rt.pitchBendRatio);
  float tuneSt = pdSynthTuneSemitones(p.op[1].ratioX100);
  float ratio2 = powf(2.0f, tuneSt / 12.0f) * rt.pitchBendRatio;
  uint32_t step2 = (uint32_t)((float)v.baseStep * ratio2);

  // Line 1 (DCO1 -> DCW1 -> DCA1)
  uint8_t l1Wave1 = constrain((int)p.op[0].wave, 0, NUM_PD_WAVES - 1);
  uint8_t l1Wave2Opt = constrain((int)p.op[0].reserved[0], 0, NUM_PD_WAVE2_OPTS - 1);
  float l1BaseDcw = constrain((int)p.op[0].reserved[1], 0, 100) * 0.01f;
  float l1EnvDepth = constrain((int)p.op[0].feedback, 0, 100) * 0.01f;
  float l1Lvl = constrain((int)p.op[0].level, 0, 100) * 0.01f;
  float l1Bal = constrain((int)(int8_t)p.op[0].reserved[2], -100, 100) * 0.01f;
  float pan1L = (l1Bal > 0.0f) ? (1.0f - l1Bal) : 1.0f;
  float pan1R = (l1Bal < 0.0f) ? (1.0f + l1Bal) : 1.0f;

  // Line 2 (DCO2 -> DCW2 -> DCA2)
  uint8_t l2Wave1 = constrain((int)p.op[1].wave, 0, NUM_PD_WAVES - 1);
  uint8_t l2Wave2Opt = constrain((int)p.op[1].reserved[0], 0, NUM_PD_WAVE2_OPTS - 1);
  float l2BaseDcw = constrain((int)p.op[1].reserved[1], 0, 100) * 0.01f;
  float l2EnvDepth = constrain((int)p.op[1].feedback, 0, 100) * 0.01f;
  float l2Lvl = constrain((int)p.op[1].level, 0, 100) * 0.01f;
  float l2Bal = constrain((int)(int8_t)p.op[1].reserved[2], -100, 100) * 0.01f;
  float pan2L = (l2Bal > 0.0f) ? (1.0f - l2Bal) : 1.0f;
  float pan2R = (l2Bal < 0.0f) ? (1.0f + l2Bal) : 1.0f;

  // Line 2 > Line 1 Modulation (TUNE, MOD, AMT)
  uint8_t modMode = constrain((int)p.algorithm, 0, NUM_PD_MOD_MODES - 1);
  float modAmt = constrain((int)p.op[0].ratioX100, 0, 100) * 0.01f;
  const bool runLine2 = (l2Lvl > 0.0f || modAmt > 0.0f);

  float sumLvl = l1Lvl + l2Lvl;
  float normScale = (sumLvl > 1.0f) ? (1.0f / (0.65f + 0.35f * sumLvl)) : 1.0f;
  float voiceGain = 10000.0f * normScale * v.velScale;

  PdWaveCoeffs c1a{}, c1b{}, c2a{}, c2b{};

  int startIdx = (v.startSampleOffset < (uint8_t)numFrames) ? (int)v.startSampleOffset : 0;
  v.startSampleOffset = 0;
  for (int i = startIdx; i < numFrames; i++) {
    // 1. Advance shared General FLT and AMP envelopes
    if (!instAdvanceVoiceEnvelopes(v)) {
      break;
    }

    // Recompute DCW and wave reciprocals every 8 samples (~5.5 kHz control rate)
    if (i == startIdx || (i & 7) == 0) {
      float fltMod = (v.fltAmt * v.fltEnv) * v.velScale;
      float dcw1 = constrain(l1BaseDcw + l1EnvDepth * fltMod + rt.modWheelBoost * 0.35f, 0.0f, 1.0f);
      c1a = makePdWaveCoeffs(l1Wave1, dcw1);
      if (l1Wave2Opt > 0) c1b = makePdWaveCoeffs((uint8_t)(l1Wave2Opt - 1), dcw1);
      if (runLine2) {
        float dcw2 = constrain(l2BaseDcw + l2EnvDepth * fltMod + rt.modWheelBoost * 0.35f, 0.0f, 1.0f);
        c2a = makePdWaveCoeffs(l2Wave1, dcw2);
        if (l2Wave2Opt > 0) c2b = makePdWaveCoeffs((uint8_t)(l2Wave2Opt - 1), dcw2);
      }
    }

    // 2. Evaluate Line 2 (alternating Wave1 & Wave2 on cycle wrap when Wave2 != Off)
    float w2 = 0.0f;
    if (runLine2) {
      const PdWaveCoeffs &curC2 = (l2Wave2Opt > 0 && (v.pdCycleParity & 2u)) ? c2b : c2a;
      w2 = evalPdWaveFast(curC2, v.opPhase[1]);
    }

    // 3. Apply Line 2 > Line 1 Modulation and evaluate Line 1 (alternating Wave1 & Wave2)
    uint32_t ph1 = v.opPhase[0];
    if (modAmt > 0.0f) {
      if (modMode == (uint8_t)PdModMode::Phase) {
        ph1 += (uint32_t)((int32_t)(w2 * modAmt * 1073741824.0f));
      } else if (modMode == (uint8_t)PdModMode::Noise) {
        float nMod = instNextWhiteNoise() * (0.65f + 0.35f * w2);
        ph1 += (uint32_t)((int32_t)(nMod * modAmt * 536870912.0f));
      }
    }

    const PdWaveCoeffs &curC1 = (l1Wave2Opt > 0 && (v.pdCycleParity & 1u)) ? c1b : c1a;
    float w1 = evalPdWaveFast(curC1, ph1);

    if (modAmt > 0.0f) {
      if (modMode == (uint8_t)PdModMode::Ring) {
        w1 = (1.0f - modAmt) * w1 + modAmt * (w1 * w2);
      } else if (modMode == (uint8_t)PdModMode::Sync) {
        float win = 1.0f - (float)(v.opPhase[0] >> 8) * (1.0f / 16777216.0f);
        w1 = (1.0f - 0.55f * modAmt) * w1 + modAmt * (w2 * win);
      }
    }

    // 4. Advance oscillator phases and toggle Wave1/Wave2 cycle parity bits on wrap
    uint32_t nextPh0 = v.opPhase[0] + step1;
    if (nextPh0 < v.opPhase[0]) {
      v.pdCycleParity ^= 1u;
      if (modAmt > 0.0f && modMode == (uint8_t)PdModMode::Sync) {
        v.opPhase[1] = 0;
      }
    }
    v.opPhase[0] = nextPh0;

    uint32_t nextPh1 = v.opPhase[1] + step2;
    if (nextPh1 < v.opPhase[1]) {
      v.pdCycleParity ^= 2u;
    }
    v.opPhase[1] = nextPh1;

    // 5. Sum DCA1 + DCA2 with per-line LVL and stereo BAL
    float l1Out = w1 * l1Lvl;
    float l2Out = w2 * l2Lvl;
    float rawL = l1Out * pan1L + l2Out * pan2L;
    float rawR = l1Out * pan1R + l2Out * pan2R;
    float mid = 0.5f * (rawL + rawR);
    float filtMid = instApplyVoiceFilterAndAmp(v, mid, voiceGain);

    if (rawL == rawR) {
      int32_t sInt = (int32_t)filtMid;
      outL[i] += sInt;
      outR[i] += sInt;
    } else {
      float side = 0.5f * (rawL - rawR) * v.ampEnv * voiceGain;
      outL[i] += (int32_t)(filtMid + side);
      outR[i] += (int32_t)(filtMid - side);
    }
  }
}

PROGMEM static const InstEngineOps PHASEDIST_ENGINE_OPS = {
  InstType::PhaseDist,
  "PHASE DIST",
  "PD",
  "PD",
  "LINE 1",
  "LINE 2",
  "LINE 2 > LINE 1",
  phaseDistDefaultPatch,
  phaseDistValidatePatch,
  phaseDistFormatMainCell,
  phaseDistAdjustMainCell,
  phaseDistResetMainCell,
  phaseDistRenderVoice
};

FLASHMEM const InstEngineOps &instPhaseDistEngine() {
  return PHASEDIST_ENGINE_OPS;
}

}  // namespace looper

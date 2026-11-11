// master_fx.cpp — Modular Master Effects Bus (Chorus -> Delay -> Reverb -> Tape -> Comp)
#include "master_fx.h"
#include "engine.h"
#include <math.h>
#include <string.h>

namespace looper {

// ============================================================================
// Fast Real-Time DSP Math Primitives
// ============================================================================

// Fast 7th-order minimax sine on x in [-pi/2, +pi/2]
static inline float fastSinQuadrant(float x) {
  float x2 = x * x;
  return x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - 0.00019841f * x2)));
}

// Fast sine for normalized phase u in [0, 1)
static inline float fastSin01(float u) {
  float p = u - (int)u;
  if (p < 0.0f) p += 1.0f;
  float q;
  if (p < 0.25f) {
    q = p;
  } else if (p < 0.75f) {
    q = 0.5f - p;
  } else {
    q = p - 1.0f;
  }
  return fastSinQuadrant(q * 6.28318531f);
}

// Smooth, strictly monotonic C^1 soft saturator (rational tanh on [-3, +3]):
// zero wavefolding and zero hard-clipping discontinuities.
static inline float softTanhRational(float u) {
  if (u >= 3.0f) return 1.0f;
  if (u <= -3.0f) return -1.0f;
  float u2 = u * u;
  return u * (27.0f + u2) / (27.0f + 9.0f * u2);
}

// Asymmetric analog oxide/tube soft saturator: positive and negative halves saturate
// with slightly different knees to generate warm 2nd-harmonic fullness and 3rd-harmonic
// rounding while remaining strictly monotonic and zero-centered at x = 0.
static inline float warmAsymSaturate(float x, float drive, float invNorm, float asym) {
  float u = x * drive;
  float k = (u >= 0.0f) ? (1.0f + asym) : (1.0f - 0.65f * asym);
  return softTanhRational(u * k) * invNorm;
}

// Fast IEEE-754 + 4th-order minimax log2(x) for x > 0 (max error < 6e-5)
static inline float fastLog2f(float x) {
  union { float f; uint32_t u; } v;
  v.f = x;
  int32_t exp = (int32_t)((v.u >> 23) & 0xFF) - 127;
  v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
  float m = v.f;
  float log2m = -1.7417939f + m * (2.8212026f + m * (-1.4699568f + m * (0.44717955f - 0.056570851f * m)));
  return (float)exp + log2m;
}

// Fast IEEE-754 + 4th-order minimax 2^x (max error < 3e-5)
static inline float fastExp2f(float x) {
  if (x < -126.0f) return 0.0f;
  if (x > 126.0f) x = 126.0f;
  int32_t k = (int32_t)x;
  if (x < (float)k) k--;
  float f = x - (float)k;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0558011f + 0.0108252f * f)));
  union { uint32_t u; float flt; } scale;
  scale.u = (uint32_t)((k + 127) << 23);
  return p * scale.flt;
}

// Fast xorshift32 PRNG returning uniform float in [-1.0f, +1.0f]
static uint32_t masterFxRngState = 0x9E3779B9u;
static inline float nextFxRandMinus1To1() {
  uint32_t x = masterFxRngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  masterFxRngState = x;
  return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// ============================================================================
// 1. Chorus ("CHOR") — Stereo Quadrature Modulated Chorus (0% Dry .. 100% Wet)
// ============================================================================

static constexpr int CHORUS_BUF_LEN = 768; // ~17.4 ms max delay capacity at 44.1 kHz (max tap is 684 samples)
static int16_t chorBufL[CHORUS_BUF_LEN];
static int16_t chorBufR[CHORUS_BUF_LEN];

static inline int16_t floatToQ14(float x) {
  float c = constrain(x, -1.9999f, 1.9999f) * 16384.0f;
  return (int16_t)((c >= 0.0f) ? (c + 0.5f) : (c - 0.5f));
}

static inline float readChorusTapQ14(const int16_t *buf, int bufLen, int writeIdx, float delaySamples) {
  int dInt = (int)delaySamples;
  float dFrac = delaySamples - (float)dInt;
  int r0 = writeIdx + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  float s0 = (float)buf[r0] * (1.0f / 16384.0f);
  float s1 = (float)buf[r1] * (1.0f / 16384.0f);
  return s0 + dFrac * (s1 - s0);
}

struct ChorusState {
  int16_t *bufL = nullptr;
  int16_t *bufR = nullptr;
  int writeIdx = 0;
  float lfoPhase = 0.0f;
  float lpStateL = 0.0f;
  float lpStateR = 0.0f;
  float curWetMix = 0.0f;
  float curModDepth = 0.0f;
  uint8_t curPct = 0;
  volatile bool enabled = false;
  volatile float wetMix = 0.0f;             // 0.0f (100% dry) .. 1.0f (100% wet)
  volatile float baseDelaySamples = 529.2f; // 12.0 ms center tap at 44.1 kHz
  volatile float lfoInc = 0.0f;             // LFO phase increment per sample
  volatile float modDepthSamples = 0.0f;    // Modulation excursion in samples
};

static ChorusState chor = { chorBufL, chorBufR };
static uint8_t masterChorPct = 0;
static uint8_t instChorPct = 0;

FLASHMEM static void resetChorusUnit(ChorusState &u) {
  uint32_t irq = saveIrq();
  u.writeIdx = 0;
  u.lfoPhase = 0.0f;
  u.lpStateL = 0.0f;
  u.lpStateR = 0.0f;
  u.curWetMix = u.wetMix;
  u.curModDepth = u.modDepthSamples;
  restoreIrq(irq);

  if (u.bufL) memset(u.bufL, 0, CHORUS_BUF_LEN * sizeof(int16_t));
  if (u.bufR) memset(u.bufR, 0, CHORUS_BUF_LEN * sizeof(int16_t));
}

FLASHMEM static void configureChorusUnit(ChorusState &u, uint8_t pct, bool clearBuffers = true) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  u.curPct = pct;
  if (pct == 0) {
    bool wasEnabled = u.enabled;
    uint32_t irq = saveIrq();
    u.enabled = false;
    u.wetMix = 0.0f;
    u.curWetMix = 0.0f;
    u.lfoInc = 0.0f;
    u.modDepthSamples = 0.0f;
    u.curModDepth = 0.0f;
    u.lpStateL = 0.0f;
    u.lpStateR = 0.0f;
    restoreIrq(irq);
    if (wasEnabled && clearBuffers) {
      if (u.bufL) memset(u.bufL, 0, CHORUS_BUF_LEN * sizeof(int16_t));
      if (u.bufR) memset(u.bufR, 0, CHORUS_BUF_LEN * sizeof(int16_t));
    }
    return;
  }

  float c = (float)pct * 0.01f;
  float mix = c; // 0% = completely dry, 100% = completely wet
  float rateHz = 0.55f + 0.65f * c;
  float depthMs = 0.6f + 2.9f * c;
  float lfoStep = rateHz * (1.0f / (float)SAMPLE_RATE_HZ);
  float modSamples = depthMs * ((float)SAMPLE_RATE_HZ * 0.001f);

  uint32_t irq = saveIrq();
  if (!u.enabled) {
    u.curWetMix = mix;
    u.curModDepth = modSamples;
  }
  u.wetMix = mix;
  u.lfoInc = lfoStep;
  u.modDepthSamples = modSamples;
  u.enabled = true;
  restoreIrq(irq);
}

static inline void processChorusUnit(ChorusState &u, float &xL, float &xR) {
  u.lfoPhase += u.lfoInc;
  if (u.lfoPhase >= 1.0f) u.lfoPhase -= 1.0f;
  u.curWetMix += 0.002f * (u.wetMix - u.curWetMix);
  u.curModDepth += 0.002f * (u.modDepthSamples - u.curModDepth);

  // Quadrature stereo modulation (90-degree phase offset between Left and Right)
  const float baseDel = u.baseDelaySamples;
  const float modExc = u.curModDepth;
  float dL = baseDel + modExc * fastSin01(u.lfoPhase);
  float dR = baseDel + modExc * fastSin01(u.lfoPhase + 0.25f);

  float wetL = readChorusTapQ14(u.bufL, CHORUS_BUF_LEN, u.writeIdx, dL);
  float wetR = readChorusTapQ14(u.bufR, CHORUS_BUF_LEN, u.writeIdx, dR);

  u.bufL[u.writeIdx] = floatToQ14(xL);
  u.bufR[u.writeIdx] = floatToQ14(xR);
  if (--u.writeIdx < 0) u.writeIdx = CHORUS_BUF_LEN - 1;

  // Gentle warm smoothing (~10 kHz 1-pole LPF) on wet chorus tap
  u.lpStateL += 0.76f * (wetL - u.lpStateL);
  u.lpStateR += 0.76f * (wetR - u.lpStateR);

  const float mix = u.curWetMix;
  const float dryG = 1.0f - 0.25f * mix;
  const float wetG = 0.85f * mix;
  xL = dryG * xL + wetG * u.lpStateL;
  xR = dryG * xR + wetG * u.lpStateR;
}

static bool isChorusEnabled() {
  return masterChorPct > 0 && chor.enabled;
}

FLASHMEM static void resetChorusState() {
  resetChorusUnit(chor);
}

FLASHMEM void setMasterChorus(uint8_t pct) {
  masterChorPct = (uint8_t)constrain((int)pct, 0, 100);
  uint8_t eff = max(masterChorPct, instChorPct);
  if (eff != chor.curPct || eff == 0) {
    configureChorusUnit(chor, eff);
  }
}

static inline void processChorusSample(float &xL, float &xR) {
  processChorusUnit(chor, xL, xR);
}

// ============================================================================
// 2. Stereo Delay ("DLAY" & "D.FB") — Analog / Tape-Style Delay (~325 ms)
// ============================================================================

static constexpr int DELAY_BUF_LEN = 14500; // 328.8 ms capacity at 44.1 kHz (max tap with wow is 14,432 samples)
static constexpr float DELAY_BASE_L_SAMPLES = 14200.2f; // 322.0 ms Left tap
static constexpr float DELAY_BASE_R_SAMPLES = 14420.7f; // 327.0 ms Right tap (+5 ms stereo offset)

static int16_t delayBufL[DELAY_BUF_LEN];
DMAMEM static int16_t delayBufR[DELAY_BUF_LEN];

struct DelayState {
  int16_t *bufL = nullptr;
  int16_t *bufR = nullptr;
  int bufLen = DELAY_BUF_LEN;
  float baseL = DELAY_BASE_L_SAMPLES;
  float baseR = DELAY_BASE_R_SAMPLES;
  int writeIdx = 0;
  float wowPhase = 0.0f;
  float lpStateL = 0.0f, lpStateR = 0.0f;
  float hpStateL = 0.0f, hpStateR = 0.0f;
  float curWetMix = 0.0f;
  float curWowExc = 6.0f;
  uint8_t wetPct = 0;
  uint8_t fbPct = 0;
  volatile bool enabled = false;
  volatile float wetMix = 0.0f;
  volatile float fbGain = 0.0f;
  volatile float wowExcursion = 6.0f;
  volatile float lpAlpha = 0.56f;
  volatile float hpAlpha = 0.0064f;
  volatile float satDrive = 1.05f;
  volatile float satInvDrive = 0.95238095f;
  volatile float satAsym = 0.010f;
};

static DelayState dly = { delayBufL, delayBufR, DELAY_BUF_LEN, DELAY_BASE_L_SAMPLES, DELAY_BASE_R_SAMPLES };
static uint8_t masterDlyPct = 0;
static uint8_t masterDfbPct = 0;
static uint8_t instDlyPct = 0;
static uint8_t instDfbPct = 0;

static inline float readDelayTapQ15(const int16_t *buf, int bufLen, int writeIdx, float delaySamples) {
  int dInt = (int)delaySamples;
  float dFrac = delaySamples - (float)dInt;
  int r0 = writeIdx + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  float s0 = (float)buf[r0] * (1.0f / 32767.0f);
  float s1 = (float)buf[r1] * (1.0f / 32767.0f);
  return s0 + dFrac * (s1 - s0);
}

static inline int16_t floatToDelayQ15(float x) {
  float c = constrain(x, -1.0f, 1.0f) * 32767.0f;
  return (int16_t)((c >= 0.0f) ? (c + 0.5f) : (c - 0.5f));
}

static inline float warmDelaySaturate(float x, float drive, float invDrive, float asym) {
  float u = constrain(x * drive, -1.25f, 1.25f);
  float u2 = u * u;
  // Soft cubic tape knee with subtle even-harmonic warmth (zero DC at u = 0)
  float shaped = (u - 0.14814815f * u2 * u + asym * (u2 - 0.15f * u2 * u2)) * invDrive;
  return constrain(shaped, -1.0f, 1.0f);
}

FLASHMEM static void resetDelayUnit(DelayState &u) {
  uint32_t irq = saveIrq();
  u.writeIdx = 0;
  u.wowPhase = 0.0f;
  u.lpStateL = u.lpStateR = 0.0f;
  u.hpStateL = u.hpStateR = 0.0f;
  u.curWetMix = u.wetMix;
  u.curWowExc = u.wowExcursion;
  restoreIrq(irq);

  if (u.bufL) memset(u.bufL, 0, u.bufLen * sizeof(int16_t));
  if (u.bufR) memset(u.bufR, 0, u.bufLen * sizeof(int16_t));
}

FLASHMEM static void updateDelayUnitFeedback(DelayState &u, uint8_t fbPct) {
  fbPct = (uint8_t)constrain((int)fbPct, 0, 100);
  float fbNorm = (float)fbPct * 0.01f;
  float fb = 0.88f * fbNorm;
  float scale = (float)u.bufLen / (float)DELAY_BUF_LEN;
  float wowExc = (6.0f + 5.0f * fbNorm) * scale;

  // Warm degradation scaling gently with feedback:
  // LPF darkens from 5.8 kHz (0% FB) down to 3.4 kHz (100% FB)
  // HPF tightens sub-bass from 45 Hz (0% FB) up to 80 Hz (100% FB)
  float fcLp = 5800.0f - 2400.0f * fbNorm;
  float fcHp = 45.0f + 35.0f * fbNorm;
  float aLp = 1.0f - fastExp2f((-6.28318531f * 1.44269504f / (float)SAMPLE_RATE_HZ) * fcLp);
  float aHp = 1.0f - fastExp2f((-6.28318531f * 1.44269504f / (float)SAMPLE_RATE_HZ) * fcHp);

  float drive = 1.05f + 0.30f * fbNorm;
  float invDrive = 1.0f / drive;
  float asym = 0.010f + 0.018f * fbNorm;

  uint32_t irq = saveIrq();
  if (!u.enabled) {
    u.curWowExc = wowExc;
  }
  u.fbPct = fbPct;
  u.fbGain = fb;
  u.wowExcursion = wowExc;
  u.lpAlpha = aLp;
  u.hpAlpha = aHp;
  u.satDrive = drive;
  u.satInvDrive = invDrive;
  u.satAsym = asym;
  restoreIrq(irq);
}

FLASHMEM static void configureDelayUnit(DelayState &u, uint8_t pct, bool clearBuffers = true) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  u.wetPct = pct;
  if (pct == 0) {
    bool wasEnabled = u.enabled;
    uint32_t irq = saveIrq();
    u.enabled = false;
    u.wetMix = 0.0f;
    u.curWetMix = 0.0f;
    u.lpStateL = u.lpStateR = 0.0f;
    u.hpStateL = u.hpStateR = 0.0f;
    restoreIrq(irq);
    if (wasEnabled && clearBuffers) {
      if (u.bufL) memset(u.bufL, 0, u.bufLen * sizeof(int16_t));
      if (u.bufR) memset(u.bufR, 0, u.bufLen * sizeof(int16_t));
    }
    return;
  }

  float mix = (float)pct * 0.01f;
  uint32_t irq = saveIrq();
  if (!u.enabled) {
    u.curWetMix = mix;
    u.curWowExc = u.wowExcursion;
  }
  u.wetMix = mix;
  u.enabled = true;
  restoreIrq(irq);
}

static inline void processDelayUnit(DelayState &u, float &xL, float &xR) {
  // Slow tape capstan wow (~0.75 Hz) with slight stereo phase offset
  u.wowPhase += (0.75f / (float)SAMPLE_RATE_HZ);
  if (u.wowPhase >= 1.0f) u.wowPhase -= 1.0f;
  u.curWetMix += 0.002f * (u.wetMix - u.curWetMix);
  u.curWowExc += 0.002f * (u.wowExcursion - u.curWowExc);
  const float wowExc = u.curWowExc;
  float dL = u.baseL + wowExc * fastSin01(u.wowPhase);
  float dR = u.baseR + wowExc * fastSin01(u.wowPhase + 0.2f);

  float rawTapL = readDelayTapQ15(u.bufL, u.bufLen, u.writeIdx, dL);
  float rawTapR = readDelayTapQ15(u.bufR, u.bufLen, u.writeIdx, dR);

  // Warm analog/tape filtering + subtle saturation in the delay path
  const float aHp = u.hpAlpha;
  const float aLp = u.lpAlpha;
  const float drive = u.satDrive;
  const float invDrive = u.satInvDrive;
  const float asym = u.satAsym;

  float hpL = rawTapL - u.hpStateL;
  u.hpStateL += aHp * hpL;
  u.lpStateL += aLp * (hpL - u.lpStateL);
  float tapL = warmDelaySaturate(u.lpStateL, drive, invDrive, asym);

  float hpR = rawTapR - u.hpStateR;
  u.hpStateR += aHp * hpR;
  u.lpStateR += aLp * (hpR - u.lpStateR);
  float tapR = warmDelaySaturate(u.lpStateR, drive, invDrive, asym);

  // Slight stereo panning & cross-feed on the feedbacks
  const float fb = u.fbGain;
  float inL = 0.96f * xL + 0.04f * xR;
  float inR = 0.96f * xR + 0.04f * xL;
  float fbL = fb * (0.86f * tapL + 0.16f * tapR);
  float fbR = fb * (0.82f * tapR + 0.14f * tapL);

  u.bufL[u.writeIdx] = floatToDelayQ15(inL + fbL);
  u.bufR[u.writeIdx] = floatToDelayQ15(inR + fbR);
  if (--u.writeIdx < 0) u.writeIdx = u.bufLen - 1;

  const float mix = u.curWetMix;
  const float dryG = 1.0f - 0.12f * mix;
  const float wetG = 0.85f * mix;
  xL = dryG * xL + wetG * tapL;
  xR = dryG * xR + wetG * tapR;
}

static bool isDelayEnabled() {
  return masterDlyPct > 0 && dly.enabled;
}

static bool isDelayFeedbackActive() {
  return masterDfbPct > 0;
}

FLASHMEM static void resetDelayState() {
  resetDelayUnit(dly);
}

FLASHMEM void setMasterDelay(uint8_t pct) {
  masterDlyPct = (uint8_t)constrain((int)pct, 0, 100);
  uint8_t eff = max(masterDlyPct, instDlyPct);
  if (eff != dly.wetPct || eff == 0) {
    configureDelayUnit(dly, eff);
  }
}

FLASHMEM void setMasterDelayFeedback(uint8_t pct) {
  masterDfbPct = (uint8_t)constrain((int)pct, 0, 100);
  uint8_t eff = max(masterDfbPct, instDfbPct);
  if (eff != dly.fbPct || eff == 0) {
    updateDelayUnitFeedback(dly, eff);
  }
}

static inline void processDelaySample(float &xL, float &xR) {
  processDelayUnit(dly, xL, xR);
}

// ============================================================================
// 3. Reverb ("RVRB") — 3 Series Schroeder Allpasses/ch + Global Feedback + LPF
// ============================================================================

static constexpr int VERB_AP1_L_LEN = 1804; // 0.0408s * 44100 = 1799.28
static constexpr int VERB_AP2_L_LEN = 2100; // 0.0454s * 44100 = 2002.14 (+/- 52.92 mod)
static constexpr int VERB_AP3_L_LEN = 2308; // 0.0522s * 44100 = 2302.02
static constexpr int VERB_AP1_R_LEN = 1996; // 0.0451s * 44100 = 1988.91
static constexpr int VERB_AP2_R_LEN = 1936; // 0.0417s * 44100 = 1838.97 (+/- 52.92 mod)
static constexpr int VERB_AP3_R_LEN = 2532; // 0.0573s * 44100 = 2526.93

static int16_t verbAp1BufL[VERB_AP1_L_LEN];
static int16_t verbAp2BufL[VERB_AP2_L_LEN];
DMAMEM static int16_t verbAp3BufL[VERB_AP3_L_LEN];
static int16_t verbAp1BufR[VERB_AP1_R_LEN];
DMAMEM static int16_t verbAp2BufR[VERB_AP2_R_LEN];
DMAMEM static int16_t verbAp3BufR[VERB_AP3_R_LEN];

struct ReverbState {
  int16_t *ap1L = nullptr, *ap2L = nullptr, *ap3L = nullptr;
  int16_t *ap1R = nullptr, *ap2R = nullptr, *ap3R = nullptr;
  int idx1aL = 0, idx1bL = 0, idx1cL = 0;
  int idx2L = 0, idx3L = 0;
  int idx1aR = 0, idx1bR = 0, idx1cR = 0;
  int idx2R = 0, idx3R = 0;
  float lpfL = 0.0f, lpfR = 0.0f;
  float outLpL = 0.0f, outLpR = 0.0f;
  float lfoPhase = 0.0f;
  float curModExc = 0.0f;
  float curMix = 0.0f;
  uint8_t curPct = 0;
  volatile bool enabled = false;
  volatile float feedback = 0.0f;
  volatile float dampAlpha = 0.55f;
  volatile float inGain = 1.0f;
  volatile float modExcursion = 0.0f;
  volatile float mix = 0.0f;
};

static ReverbState verb = {
  verbAp1BufL, verbAp2BufL, verbAp3BufL,
  verbAp1BufR, verbAp2BufR, verbAp3BufR
};
static uint8_t masterVerbPct = 0;
static uint8_t instVerbPct = 0;

// Fixed-delay Schroeder allpass (integer tap -> zero fractional interpolation loss)
static inline float runFixedApQ14(int16_t *buf, int bufLen, int &writeIdx,
                                  int delaySamples, float g, float x) {
  int r0 = writeIdx + delaySamples;
  if (r0 >= bufLen) r0 -= bufLen;
  float yDel = (float)buf[r0] * (1.0f / 16384.0f);
  float vIn = x + g * yDel;
  buf[writeIdx] = floatToQ14(vIn);
  if (--writeIdx < 0) writeIdx = bufLen - 1;
  return yDel - g * vIn;
}

// Fixed-delay Schroeder allpass that also outputs the pure delayed tap (zero t=0 feedthrough)
static inline float runFixedApTapQ14(int16_t *buf, int bufLen, int &writeIdx,
                                     int delaySamples, float g, float x, float &outTap) {
  int r0 = writeIdx + delaySamples;
  if (r0 >= bufLen) r0 -= bufLen;
  float yDel = (float)buf[r0] * (1.0f / 16384.0f);
  outTap = yDel;
  float vIn = x + g * yDel;
  buf[writeIdx] = floatToQ14(vIn);
  if (--writeIdx < 0) writeIdx = bufLen - 1;
  return yDel - g * vIn;
}

// Modulated Schroeder allpass that also outputs the pure delayed tap (zero t=0 feedthrough)
static inline float runModApTapQ14(int16_t *buf, int bufLen, int &writeIdx,
                                   int dInt, float dFrac, float g, float x, float &outTap) {
  int r0 = writeIdx + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  float s0 = (float)buf[r0] * (1.0f / 16384.0f);
  float s1 = (float)buf[r1] * (1.0f / 16384.0f);
  float yDel = s0 + dFrac * (s1 - s0);
  outTap = yDel;
  float vIn = x + g * yDel;
  buf[writeIdx] = floatToQ14(vIn);
  if (--writeIdx < 0) writeIdx = bufLen - 1;
  return yDel - g * vIn;
}

FLASHMEM static void clearReverbUnitBuffers(ReverbState &u) {
  if (u.ap1L) memset(u.ap1L, 0, VERB_AP1_L_LEN * sizeof(int16_t));
  if (u.ap2L) memset(u.ap2L, 0, VERB_AP2_L_LEN * sizeof(int16_t));
  if (u.ap3L) memset(u.ap3L, 0, VERB_AP3_L_LEN * sizeof(int16_t));
  if (u.ap1R) memset(u.ap1R, 0, VERB_AP1_R_LEN * sizeof(int16_t));
  if (u.ap2R) memset(u.ap2R, 0, VERB_AP2_R_LEN * sizeof(int16_t));
  if (u.ap3R) memset(u.ap3R, 0, VERB_AP3_R_LEN * sizeof(int16_t));
}

FLASHMEM static void resetReverbUnit(ReverbState &u) {
  uint32_t irq = saveIrq();
  u.idx1aL = u.idx1bL = u.idx1cL = 0;
  u.idx2L = u.idx3L = 0;
  u.idx1aR = u.idx1bR = u.idx1cR = 0;
  u.idx2R = u.idx3R = 0;
  u.lpfL = u.lpfR = 0.0f;
  u.outLpL = u.outLpR = 0.0f;
  u.lfoPhase = 0.0f;
  u.curModExc = u.modExcursion;
  u.curMix = u.mix;
  restoreIrq(irq);

  clearReverbUnitBuffers(u);
}

FLASHMEM static void configureReverbUnit(ReverbState &u, uint8_t pct, bool clearBuffers = true) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  u.curPct = pct;
  if (pct == 0) {
    bool wasEnabled = u.enabled;
    uint32_t irq = saveIrq();
    u.enabled = false;
    u.feedback = 0.0f;
    u.modExcursion = 0.0f;
    u.curModExc = 0.0f;
    u.mix = 0.0f;
    u.curMix = 0.0f;
    u.lpfL = 0.0f;
    u.lpfR = 0.0f;
    u.outLpL = 0.0f;
    u.outLpR = 0.0f;
    restoreIrq(irq);
    if (wasEnabled && clearBuffers) {
      clearReverbUnitBuffers(u);
    }
    return;
  }

  float v = (float)pct * 0.01f;
  // Independent reverb decay curve:
  //   10% -> fb = 0.75 (~0.8s room)
  //   25% -> fb = 0.82 (~1.2s chamber)
  //   50% -> fb = 0.89 (~2.1s hall)
  //   75% -> fb = 0.95 (~4.2s large hall)
  //  100% -> fb = 0.974 (~8.5s lush cavern)
  float shape = 1.0f - (1.0f - v) * (1.0f - v);
  float fb = 0.70f + 0.274f * shape;
  float damp = 0.62f - 0.14f * v;
  float inG = 1.60f - 0.50f * shape;
  float modDepth = 0.50f + 0.25f * v;
  float modExc = ((float)SAMPLE_RATE_HZ / 500.0f) * modDepth;
  float mix = 0.14f + 0.72f * fastExp2f(0.75f * fastLog2f(v));

  uint32_t irq = saveIrq();
  if (!u.enabled) {
    u.curModExc = modExc;
    u.curMix = mix;
  }
  u.feedback = fb;
  u.dampAlpha = damp;
  u.inGain = inG;
  u.modExcursion = modExc;
  u.mix = mix;
  u.enabled = true;
  restoreIrq(irq);
}

static inline void processReverbUnit(ReverbState &u, float &xL, float &xR) {
  u.lfoPhase += (0.85f / (float)SAMPLE_RATE_HZ);
  if (u.lfoPhase >= 1.0f) u.lfoPhase -= 1.0f;
  u.curModExc += 0.002f * (u.modExcursion - u.curModExc);
  u.curMix += 0.002f * (u.mix - u.curMix);
  float lfoModL = u.curModExc * fastSin01(u.lfoPhase);
  float lfoModR = u.curModExc * fastSin01(u.lfoPhase + 0.25f);

  float d2L = 2002.14f + lfoModL;
  int d2IntL = (int)d2L;
  float d2FracL = d2L - (float)d2IntL;

  float d2R = 1838.97f + lfoModR;
  int d2IntR = (int)d2R;
  float d2FracR = d2R - (float)d2IntR;

  const float fb = u.feedback;
  const float damp = u.dampAlpha;
  const float inG = u.inGain;
  const float vMix = u.curMix;
  const float dryG = 1.0f - 0.18f * vMix;
  const float wetG = 1.25f * vMix;

  // 1. 3-stage short input diffuser per channel (partitions ap1L[1804] & ap1R[1996])
  // Immediately smears even ultra-short (<5 ms) plucks into a dense reflection cloud
  float inL = (0.92f * xL + 0.08f * xR) * inG;
  float inR = (0.92f * xR + 0.08f * xL) * inG;

  float dL = runFixedApQ14(u.ap1L,       220,  u.idx1aL, 211,  0.70f, inL);
  dL       = runFixedApQ14(u.ap1L + 220, 570,  u.idx1bL, 557,  0.68f, dL);
  dL       = runFixedApQ14(u.ap1L + 790, 1014, u.idx1cL, 997,  0.65f, dL);

  float dR = runFixedApQ14(u.ap1R,       272,  u.idx1aR, 263,  0.70f, inR);
  dR       = runFixedApQ14(u.ap1R + 272, 656,  u.idx1bR, 643,  0.68f, dR);
  dR       = runFixedApQ14(u.ap1R + 928, 1068, u.idx1cR, 1051, 0.65f, dR);

  // 2. Unitary 2D stereo tank rotation (0.8^2 + 0.6^2 = 1.0) + soft-saturated feedback tank
  float rotL = 0.80f * u.lpfL + 0.60f * u.lpfR;
  float rotR = 0.80f * u.lpfR - 0.60f * u.lpfL;

  float tankInL = softTanhRational(dL + fb * rotL);
  float tankInR = softTanhRational(dR + fb * rotR);

  float tap2L = 0.0f, tap3L = 0.0f;
  float y2L = runModApTapQ14(u.ap2L, VERB_AP2_L_LEN, u.idx2L, d2IntL, d2FracL, 0.60f, tankInL, tap2L);
  float y3L = runFixedApTapQ14(u.ap3L, VERB_AP3_L_LEN, u.idx3L, 2302, 0.55f, y2L, tap3L);
  u.lpfL += damp * (y3L - u.lpfL);

  float tap2R = 0.0f, tap3R = 0.0f;
  float y2R = runModApTapQ14(u.ap2R, VERB_AP2_R_LEN, u.idx2R, d2IntR, d2FracR, 0.60f, tankInR, tap2R);
  float y3R = runFixedApTapQ14(u.ap3R, VERB_AP3_R_LEN, u.idx3R, 2526, 0.55f, y2R, tap3R);
  u.lpfR += damp * (y3R - u.lpfR);

  // 3. Multi-tap pure tank output (zero t=0 dry feedthrough -> tail is decoupled from AMP release)
  float rawWetL = 0.55f * tap3L + 0.35f * tap2R + 0.25f * tap3R;
  float rawWetR = 0.55f * tap3R + 0.35f * tap2L + 0.25f * tap3L;
  u.outLpL += 0.52f * (rawWetL - u.outLpL);
  u.outLpR += 0.52f * (rawWetR - u.outLpR);

  xL = dryG * xL + wetG * u.outLpL;
  xR = dryG * xR + wetG * u.outLpR;
}

static bool isReverbEnabled() {
  return masterVerbPct > 0 && verb.enabled;
}

FLASHMEM static void resetReverbState() {
  resetReverbUnit(verb);
}

FLASHMEM void setMasterReverb(uint8_t pct) {
  masterVerbPct = (uint8_t)constrain((int)pct, 0, 100);
  uint8_t eff = max(masterVerbPct, instVerbPct);
  if (eff != verb.curPct || eff == 0) {
    configureReverbUnit(verb, eff);
  }
}

static inline void processReverbSample(float &xL, float &xR) {
  processReverbUnit(verb, xL, xR);
}

// ============================================================================
// 4. Tape ("TAPE") — Head FIR + Low-End Bump -> Gentle Wow/Flutter -> Warm Sat -> 2-Pole Head LPF
// ============================================================================

static constexpr int TAPE_FIR_TAPS = 17;
static const float TAPE_FIR_COEFFS[TAPE_FIR_TAPS] PROGMEM = {
  0.01259891f, 0.02360345f, 0.02426576f, 0.04210958f, 0.06238867f,
  0.06377593f, 0.10293065f, 0.10938368f, 0.10938368f, 0.07077847f,
  0.03080625f, 0.05302899f, 0.03496643f, 0.03092099f, 0.04925077f,
  0.03924405f, 0.02246044f
};

static constexpr int TAPE_WOW_DELAY_LEN = 128;    // max sweep = 1.0 + 85.0 = 86.0 samples at 44.1 kHz
static constexpr int TAPE_FLUTTER_DELAY_LEN = 64; // max sweep = 1.0 + 36.0 = 37.0 samples at 44.1 kHz
DMAMEM static float tapeWowBufL[TAPE_WOW_DELAY_LEN];
DMAMEM static float tapeWowBufR[TAPE_WOW_DELAY_LEN];
DMAMEM static float tapeFlutterBufL[TAPE_FLUTTER_DELAY_LEN];
DMAMEM static float tapeFlutterBufR[TAPE_FLUTTER_DELAY_LEN];

static float tapeFirBufL[TAPE_FIR_TAPS * 2] = {};
static float tapeFirBufR[TAPE_FIR_TAPS * 2] = {};
static float tapeFirWeights[TAPE_FIR_TAPS] = {};
static int tapeFirIdx = 0;

static int tapeWowIdx = 0;
static int tapeFlutterIdx = 0;
static float tapeWowPhase = 0.0f;
static float tapeFlutterPhase = 0.0f;
static float tapeFlutterRandPrev = 0.0f;
static float tapeFlutterRandNext = 0.0f;
static float tapeFlutterSmooth = 0.0f;
static float tapeWowCur = 0.0f;
static float tapeFlutterCur = 0.0f;

static float tapeSubHpState[2] = {};
static float tapeBumpLpState[2] = {};
static float tapeRwState[2] = {};
static float tapeNoiseX1[2] = {}, tapeNoiseX2[2] = {};
static float tapeNoiseY1[2] = {}, tapeNoiseY2[2] = {};
static float tapeLpf1State[2] = {}, tapeLpf2State[2] = {};
static uint8_t tapeModSubPhase = 0;
static float tapeCachedAlpha[2] = {0.85f, 0.85f};
static float tapeCachedDrop[2] = {1.0f, 1.0f};
static float tapeSmoothDrop[2] = {1.0f, 1.0f};

static volatile bool tapeEnabled = false;
static volatile float tapeSatMix = 0.0f;
static volatile float tapeHeadBumpGain = 0.0f;
static volatile float tapeWowTarget = 0.0f;
static volatile float tapeFlutterTarget = 0.0f;
static volatile float tapeNoisePole = 0.998f;
static volatile float tapeNoiseModDepth = 0.0f;
static volatile float tapeDropoutDepth = 0.0f;
static volatile float tapeBaseLpAlpha = 0.85f;
static volatile float tapeSatDrive = 1.0f;
static volatile float tapeInvSatNorm = 1.0f;
static volatile float tapeSatAsym = 0.0f;

static inline void runTapeDelayStereo(float *bufL, float *bufR, int bufLen, int &writeIdx,
                                      float delaySamples, float &xL, float &xR) {
  bufL[writeIdx] = xL;
  bufR[writeIdx] = xR;
  int dInt = (int)delaySamples;
  float dFrac = delaySamples - (float)dInt;
  int r0 = writeIdx + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  xL = bufL[r0] + dFrac * (bufL[r1] - bufL[r0]);
  xR = bufR[r0] + dFrac * (bufR[r1] - bufR[r0]);
  if (--writeIdx < 0) writeIdx = bufLen - 1;
}

static bool isTapeEnabled() {
  return tapeEnabled;
}

FLASHMEM static void resetTapeState() {
  noInterrupts();
  tapeFirIdx = 0;
  tapeWowIdx = 0;
  tapeFlutterIdx = 0;
  tapeWowPhase = 0.0f;
  tapeFlutterPhase = 0.0f;
  tapeFlutterRandPrev = 0.0f;
  tapeFlutterRandNext = 0.0f;
  tapeFlutterSmooth = 0.0f;
  tapeWowCur = tapeWowTarget;
  tapeFlutterCur = tapeFlutterTarget;
  memset(tapeFirBufL, 0, sizeof(tapeFirBufL));
  memset(tapeFirBufR, 0, sizeof(tapeFirBufR));
  memset(tapeSubHpState, 0, sizeof(tapeSubHpState));
  memset(tapeBumpLpState, 0, sizeof(tapeBumpLpState));
  memset(tapeRwState, 0, sizeof(tapeRwState));
  memset(tapeNoiseX1, 0, sizeof(tapeNoiseX1));
  memset(tapeNoiseX2, 0, sizeof(tapeNoiseX2));
  memset(tapeNoiseY1, 0, sizeof(tapeNoiseY1));
  memset(tapeNoiseY2, 0, sizeof(tapeNoiseY2));
  memset(tapeLpf1State, 0, sizeof(tapeLpf1State));
  memset(tapeLpf2State, 0, sizeof(tapeLpf2State));
  tapeModSubPhase = 0;
  tapeCachedAlpha[0] = tapeCachedAlpha[1] = tapeBaseLpAlpha;
  tapeCachedDrop[0] = tapeCachedDrop[1] = 1.0f;
  tapeSmoothDrop[0] = tapeSmoothDrop[1] = 1.0f;
  interrupts();

  memset(tapeWowBufL, 0, sizeof(tapeWowBufL));
  memset(tapeWowBufR, 0, sizeof(tapeWowBufR));
  memset(tapeFlutterBufL, 0, sizeof(tapeFlutterBufL));
  memset(tapeFlutterBufR, 0, sizeof(tapeFlutterBufR));
}

FLASHMEM void setMasterTape(uint8_t pct) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  if (pct == 0) {
    bool wasEnabled = tapeEnabled;
    noInterrupts();
    tapeEnabled = false;
    tapeSatMix = 0.0f;
    tapeHeadBumpGain = 0.0f;
    tapeWowTarget = 0.0f;
    tapeFlutterTarget = 0.0f;
    tapeWowCur = 0.0f;
    tapeFlutterCur = 0.0f;
    tapeNoisePole = 0.998f;
    tapeNoiseModDepth = 0.0f;
    tapeDropoutDepth = 0.0f;
    tapeBaseLpAlpha = 0.85f;
    tapeSatDrive = 1.0f;
    tapeInvSatNorm = 1.0f;
    tapeSatAsym = 0.0f;
    memset(tapeFirBufL, 0, sizeof(tapeFirBufL));
    memset(tapeFirBufR, 0, sizeof(tapeFirBufR));
    memset(tapeSubHpState, 0, sizeof(tapeSubHpState));
    memset(tapeBumpLpState, 0, sizeof(tapeBumpLpState));
    memset(tapeRwState, 0, sizeof(tapeRwState));
    memset(tapeNoiseX1, 0, sizeof(tapeNoiseX1));
    memset(tapeNoiseX2, 0, sizeof(tapeNoiseX2));
    memset(tapeNoiseY1, 0, sizeof(tapeNoiseY1));
    memset(tapeNoiseY2, 0, sizeof(tapeNoiseY2));
    memset(tapeLpf1State, 0, sizeof(tapeLpf1State));
    memset(tapeLpf2State, 0, sizeof(tapeLpf2State));
    tapeModSubPhase = 0;
    tapeCachedAlpha[0] = tapeCachedAlpha[1] = 0.85f;
    tapeCachedDrop[0] = tapeCachedDrop[1] = 1.0f;
    tapeSmoothDrop[0] = tapeSmoothDrop[1] = 1.0f;
    interrupts();
    if (wasEnabled) {
      memset(tapeWowBufL, 0, sizeof(tapeWowBufL));
      memset(tapeWowBufR, 0, sizeof(tapeWowBufR));
      memset(tapeFlutterBufL, 0, sizeof(tapeFlutterBufL));
      memset(tapeFlutterBufR, 0, sizeof(tapeFlutterBufR));
    }
    return;
  }

  float t = (float)pct * 0.01f;
  float satMix = min(1.0f, 1.8f * t);
  float irMix = 0.65f * sqrtf(t);
  float bumpGain = 0.40f * t; // +1.5..+3.3 dB warm low-end head bump around 60..130 Hz

  // Progressive, musical wow & flutter excursion that increases clearly with Tape %
  float wowTgt = 0.0f, flutTgt = 0.0f;
  if (t > 0.05f) {
    float s = (t - 0.05f) * 1.05263158f;
    float curve = s * (0.42f + 0.58f * s);
    wowTgt = 85.0f * curve;  // up to ~1.93 ms slow capstan drift at 100%
    flutTgt = 36.0f * curve; // up to ~0.82 ms smoothed flutter at 100%
  }

  float nPole = 0.998f + 0.0015f * t;
  float nMod = 0.012f * t;
  float dropDepth = 0.14f * t;

  // Post-saturation 2-pole tape head loss LPF: rolls off highs from 14 kHz (low %) down to 5.5 kHz (100%)
  float fcBase = 14000.0f - 8500.0f * powf(t, 0.75f);
  float baseAlpha = 1.0f - expf(-6.28318531f * fcBase / (float)SAMPLE_RATE_HZ);

  // Smooth non-folding oxide saturation drive (1.0x up to 2.50x = +8.0 dB) + warm 2nd-harmonic asymmetry
  float drive = 1.0f + 1.50f * t * (0.50f + 0.50f * t);
  float asym = 0.16f * t;
  // Normalize gain at nominal mix level so added saturation stays level-matched without volume jumps
  float refOut = softTanhRational(drive * 0.55f) / 0.55f;
  float invNorm = (refOut > 0.01f) ? (1.0f / refOut) : 1.0f;

  float weights[TAPE_FIR_TAPS];
  for (int j = 0; j < TAPE_FIR_TAPS; j++) {
    int k = (TAPE_FIR_TAPS - 1) - j;
    weights[j] = irMix * TAPE_FIR_COEFFS[k] + ((k == 7) ? (1.0f - irMix) : 0.0f);
  }

  noInterrupts();
  memcpy(tapeFirWeights, weights, sizeof(tapeFirWeights));
  tapeSatMix = satMix;
  tapeHeadBumpGain = bumpGain;
  tapeWowTarget = wowTgt;
  tapeFlutterTarget = flutTgt;
  tapeNoisePole = nPole;
  tapeNoiseModDepth = nMod;
  tapeDropoutDepth = dropDepth;
  tapeBaseLpAlpha = baseAlpha;
  tapeSatDrive = drive;
  tapeInvSatNorm = invNorm;
  tapeSatAsym = asym;
  if (!tapeEnabled) {
    tapeCachedAlpha[0] = tapeCachedAlpha[1] = baseAlpha;
    tapeCachedDrop[0] = tapeCachedDrop[1] = 1.0f;
    tapeSmoothDrop[0] = tapeSmoothDrop[1] = 1.0f;
  }
  tapeEnabled = true;
  interrupts();
}

static inline void processTapeSample(float &xL, float &xR) {
  // 4a. 17-tap Core Tape Head Impulse Response FIR (dry tap aligned at index 7)
  if (--tapeFirIdx < 0) tapeFirIdx = TAPE_FIR_TAPS - 1;
  tapeFirBufL[tapeFirIdx] = tapeFirBufL[tapeFirIdx + TAPE_FIR_TAPS] = xL;
  tapeFirBufR[tapeFirIdx] = tapeFirBufR[tapeFirIdx + TAPE_FIR_TAPS] = xR;
  const float *pL = &tapeFirBufL[tapeFirIdx];
  const float *pR = &tapeFirBufR[tapeFirIdx];
  float accL = 0.0f, accR = 0.0f;
  for (int j = 0; j < TAPE_FIR_TAPS; j++) {
    float w = tapeFirWeights[j];
    accL += pL[j] * w;
    accR += pR[j] * w;
  }
  xL = accL;
  xR = accR;

  // 4b. Warm Tape Head Bump (~95 Hz resonant bass fullness + 30 Hz sub-rumble tightener)
  const float bumpG = tapeHeadBumpGain;
  tapeSubHpState[0] += 0.00427f * (xL - tapeSubHpState[0]);
  float cleanL = xL - tapeSubHpState[0];
  tapeBumpLpState[0] += 0.01344f * (cleanL - tapeBumpLpState[0]);
  xL = cleanL + bumpG * tapeBumpLpState[0];

  tapeSubHpState[1] += 0.00427f * (xR - tapeSubHpState[1]);
  float cleanR = xR - tapeSubHpState[1];
  tapeBumpLpState[1] += 0.01344f * (cleanR - tapeBumpLpState[1]);
  xR = cleanR + bumpG * tapeBumpLpState[1];

  // 4c. Gentle Wow (1.6 Hz sine) & Smoothed Flutter (11.5 Hz) fractional delay lines
  tapeWowCur += (tapeWowTarget - tapeWowCur) * 0.0005f;
  tapeFlutterCur += (tapeFlutterTarget - tapeFlutterCur) * 0.0005f;

  tapeWowPhase += (1.6f / (float)SAMPLE_RATE_HZ);
  if (tapeWowPhase >= 1.0f) tapeWowPhase -= 1.0f;
  float wowLfo = 0.5f * fastSin01(tapeWowPhase) + 0.5f;
  float wowDelay = 1.0f + tapeWowCur * wowLfo;
  runTapeDelayStereo(tapeWowBufL, tapeWowBufR, TAPE_WOW_DELAY_LEN, tapeWowIdx, wowDelay, xL, xR);

  tapeFlutterPhase += (11.5f / (float)SAMPLE_RATE_HZ);
  if (tapeFlutterPhase >= 1.0f) {
    tapeFlutterPhase -= 1.0f;
    tapeFlutterRandPrev = tapeFlutterRandNext;
    tapeFlutterRandNext = nextFxRandMinus1To1();
  }
  float rawFlut = tapeFlutterRandPrev + tapeFlutterPhase * (tapeFlutterRandNext - tapeFlutterRandPrev);
  tapeFlutterSmooth += 0.08f * (rawFlut - tapeFlutterSmooth);
  float flutLfo = 0.5f * tapeFlutterSmooth + 0.5f;
  float flutDelay = 1.0f + tapeFlutterCur * flutLfo;
  runTapeDelayStereo(tapeFlutterBufL, tapeFlutterBufR, TAPE_FLUTTER_DELAY_LEN, tapeFlutterIdx, flutDelay, xL, xR);

  // 4d. Smooth Monotonic Asymmetric Tape Oxide Saturation (runs BEFORE head-loss LPF)
  const float drive = tapeSatDrive;
  const float invNorm = tapeInvSatNorm;
  const float asym = tapeSatAsym;
  const float sMix = tapeSatMix;

  float shapedL = warmAsymSaturate(xL, drive, invNorm, asym);
  float shapedR = warmAsymSaturate(xR, drive, invNorm, asym);
  xL = (1.0f - sMix) * xL + sMix * shapedL;
  xR = (1.0f - sMix) * xR + sMix * shapedR;

  // 4e. Post-Saturation 2-Pole Tape Head Loss LPF (14 kHz -> 5.5 kHz) + Smooth Brownian Drift & Dropouts
  //     Update sub-10 Hz Brownian random walk and dropout modulation at 1/8th control rate (~5.5 kHz)
  if ((tapeModSubPhase++ & 7u) == 0u) {
    constexpr float LWR_A0 = 0.0001117914f;
    constexpr float LWR_A1 = 0.0002235828f;
    constexpr float LWR_A2 = 0.0001117914f;
    constexpr float LWR_B1 = -1.9788855f;
    constexpr float LWR_B2 = 0.9793327f;
    const float nPole = tapeNoisePole;
    const float nMod = tapeNoiseModDepth;
    const float baseAlpha = tapeBaseLpAlpha;
    const float dropDepth = tapeDropoutDepth;

    for (int ch = 0; ch < 2; ch++) {
      float rw = constrain(nextFxRandMinus1To1() + tapeRwState[ch], -4.0f, 4.0f);
      tapeRwState[ch] = rw * nPole;

      float nz = LWR_A0 * rw + LWR_A1 * tapeNoiseX1[ch] + LWR_A2 * tapeNoiseX2[ch]
               - LWR_B1 * tapeNoiseY1[ch] - LWR_B2 * tapeNoiseY2[ch];
      nz = constrain(nz, -4.0f, 4.0f);
      tapeNoiseX2[ch] = tapeNoiseX1[ch];
      tapeNoiseX1[ch] = rw;
      tapeNoiseY2[ch] = tapeNoiseY1[ch];
      tapeNoiseY1[ch] = nz;

      // Smooth quadratic modulation (C^1 at zero, no sharp fabsf() corner)
      float modVal = nMod * nz;
      float modSq = (modVal * modVal) / (1.0f + modVal * modVal);
      tapeCachedAlpha[ch] = constrain(baseAlpha * (1.0f - 0.35f * modSq), 0.25f, 0.90f);
      tapeCachedDrop[ch] = 1.0f - dropDepth * modSq;
    }
  }

  float chIn[2] = {xL, xR};
  for (int ch = 0; ch < 2; ch++) {
    const float alpha = tapeCachedAlpha[ch];
    tapeSmoothDrop[ch] += 0.05f * (tapeCachedDrop[ch] - tapeSmoothDrop[ch]);
    // 2-pole cascaded warm high-frequency roll-off at full 44.1 kHz audio rate
    float s1 = tapeLpf1State[ch] + alpha * (chIn[ch] - tapeLpf1State[ch]);
    float s2 = tapeLpf2State[ch] + alpha * (s1 - tapeLpf2State[ch]);
    tapeLpf1State[ch] = s1;
    tapeLpf2State[ch] = s2;
    chIn[ch] = s2 * tapeSmoothDrop[ch];
  }
  xL = chIn[0];
  xR = chIn[1];
}

// ============================================================================
// 5. Comp ("COMP") — Warm Analog Bus / Opto Glue Compressor with Dynamic HF Silk
// ============================================================================

static float compEnvL = 0.0f, compEnvR = 0.0f;
static float compGainSmooth = 1.0f;
static float compTargetGainLin = 1.0f;
static float compSilkTrim = 0.20f;
static uint8_t compSubPhase = 0;
static float compScHpL = 0.0f, compScHpR = 0.0f;
static float compSilkLpL = 0.0f, compSilkLpR = 0.0f;
static float compBodyLpL = 0.0f, compBodyLpR = 0.0f;
static float compOutLp1L = 0.0f, compOutLp1R = 0.0f;
static float compOutLp2L = 0.0f, compOutLp2R = 0.0f;

static volatile bool compEnabled = false;
static volatile float compMix = 0.0f;
static volatile float compAmount = 0.0f;
static volatile float compAlphaAtk = 0.999f;
static volatile float compAlphaRel = 0.9998f;
static volatile float compBaseSlope = 0.5f;
static volatile float compBodyLiftMaxDb = 0.0f;
static volatile float compMakeupGain = 1.0f;
static volatile float compSatDrive = 1.0f;
static volatile float compSatInvNorm = 1.0f;
static volatile float compSatAsym = 0.0f;
static volatile float compOutLpAlpha = 0.82f;

static bool isCompEnabled() {
  return compEnabled;
}

FLASHMEM static void resetCompState() {
  noInterrupts();
  compEnvL = 0.0f;
  compEnvR = 0.0f;
  compGainSmooth = 1.0f;
  compTargetGainLin = 1.0f;
  compSilkTrim = 0.20f;
  compSubPhase = 0;
  compScHpL = 0.0f;
  compScHpR = 0.0f;
  compSilkLpL = 0.0f;
  compSilkLpR = 0.0f;
  compBodyLpL = 0.0f;
  compBodyLpR = 0.0f;
  compOutLp1L = 0.0f;
  compOutLp1R = 0.0f;
  compOutLp2L = 0.0f;
  compOutLp2R = 0.0f;
  interrupts();
}

FLASHMEM void setMasterComp(uint8_t pct) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  if (pct == 0) {
    noInterrupts();
    compEnabled = false;
    compMix = 0.0f;
    compAmount = 0.0f;
    compEnvL = 0.0f;
    compEnvR = 0.0f;
    compGainSmooth = 1.0f;
    compTargetGainLin = 1.0f;
    compSilkTrim = 0.20f;
    compSubPhase = 0;
    compScHpL = 0.0f;
    compScHpR = 0.0f;
    compSilkLpL = 0.0f;
    compSilkLpR = 0.0f;
    compBodyLpL = 0.0f;
    compBodyLpR = 0.0f;
    compOutLp1L = 0.0f;
    compOutLp1R = 0.0f;
    compOutLp2L = 0.0f;
    compOutLp2R = 0.0f;
    interrupts();
    return;
  }

  float c = (float)pct * 0.01f;
  float mix = (c < 0.4f) ? (2.5f * c) : 1.0f;
  float ratio = 1.8f + 3.7f * c; // 1.8:1 gentle glue up to 5.5:1 warm opto bus compression

  // True 1/e musical time constants: 35 ms -> 15 ms attack, 240 ms -> 140 ms release
  float atkMs = 35.0f - 20.0f * c;
  float relMs = 240.0f - 100.0f * c;
  float aAtk = expf(-1.0f / (atkMs * 0.001f * (float)SAMPLE_RATE_HZ));
  float aRel = expf(-1.0f / (relMs * 0.001f * (float)SAMPLE_RATE_HZ));

  float slope = 1.0f - (1.0f / ratio);
  float bodyLiftDb = 0.8f * c; // Subtle bounded body lift in [-48 dB, -14 dB] without volume inflation
  float makeup = 1.0f;         // Unity pre-saturator makeup; saturation stages provide natural density

  // Warm, thick vintage tube/transformer saturation that scales progressively with COMP %
  float drive = 1.0f + 3.20f * c * (0.40f + 0.60f * c); // up to 4.20x (+12.5 dB) into soft tube knee
  float asym = 0.28f * c;                                // rich 2nd-harmonic tube asymmetry
  float stage2Drive = 1.0f + 0.15f * c;                  // Stage 2 transformer core rounding drive
  // Calibrate combined 2-stage output trim at nominal mix level so volume stays controlled and never clips
  float ref1 = softTanhRational(drive * 0.34f);
  float ref2 = softTanhRational(ref1 * stage2Drive);
  float invNorm = (ref2 > 0.01f) ? (0.34f / ref2) : 1.0f;
  float outLpAlpha = 0.80f - 0.33f * c;                  // 2-pole post-saturation HF roll-off (12.5 kHz -> 4.8 kHz)

  noInterrupts();
  compMix = mix;
  compAmount = c;
  compAlphaAtk = aAtk;
  compAlphaRel = aRel;
  compBaseSlope = slope;
  compBodyLiftMaxDb = bodyLiftDb;
  compMakeupGain = makeup;
  compSatDrive = drive;
  compSatInvNorm = invNorm;
  compSatAsym = asym;
  compOutLpAlpha = outLpAlpha;
  compEnabled = true;
  interrupts();
}

static inline void processCompSample(float &xL, float &xR) {
  const float aAtk = compAlphaAtk;
  const float aRel = compAlphaRel;
  const float cAmt = compAmount;

  // 5a. Sidechain filtering: 90 Hz HPF (keeps low end big & punchy) + HF pre-emphasis
  //     (makes detector react smoothly to bright highs so top end is never spiky)
  compScHpL += 0.01274f * (xL - compScHpL);
  float scHpL = xL - compScHpL;
  compSilkLpL += 0.52f * (xL - compSilkLpL);
  float hfL = xL - compSilkLpL;
  float detL = scHpL + 0.45f * hfL;

  compScHpR += 0.01274f * (xR - compScHpR);
  float scHpR = xR - compScHpR;
  compSilkLpR += 0.52f * (xR - compSilkLpR);
  float hfR = xR - compSilkLpR;
  float detR = scHpR + 0.45f * hfR;

  // 5b. Smooth Mean-Square Envelope Detection (every sample)
  float x2L = detL * detL;
  float aL = (x2L > compEnvL) ? aAtk : aRel;
  compEnvL = constrain(x2L + aL * (compEnvL - x2L), 0.0f, 2.0f);

  float x2R = detR * detR;
  float aR = (x2R > compEnvR) ? aAtk : aRel;
  compEnvR = constrain(x2R + aR * (compEnvR - x2R), 0.0f, 2.0f);

  // 5c. Soft-Knee Downward Compression (-18 dB threshold, 12 dB knee) + Bounded Body Lift
  //     Evaluated every 4 samples (11.025 kHz) using log identity 20*log10(sqrt(msAvg)) = 10*log10(msAvg)
  //     to eliminate per-sample sqrtf() while slewing compGainSmooth on every sample.
  if ((compSubPhase++ & 3u) == 0u) {
    float msAvg = 0.5f * (compEnvL + compEnvR);
    float levelDb = (msAvg > 1e-10f) ? max(-96.0f, 3.01029995f * fastLog2f(msAvg)) : -96.0f;

    const float slope = compBaseSlope;
    float overDb = levelDb - (-18.0f);
    float downGainDb = 0.0f;
    if (overDb > -6.0f) {
      if (overDb < 6.0f) {
        float k = overDb + 6.0f;
        downGainDb = -slope * (k * k) * (1.0f / 24.0f);
      } else {
        downGainDb = -slope * overDb;
      }
    }

    // Smooth C^1 bell-shaped upward body lift between -48 dB and -14 dB (zero boost on noise floor)
    float bodyLiftDb = 0.0f;
    if (levelDb > -48.0f && levelDb < -14.0f) {
      float norm = (levelDb + 31.0f) * (1.0f / 17.0f);
      float win = 1.0f - norm * norm;
      if (win > 0.0f) {
        bodyLiftDb = compBodyLiftMaxDb * (win * win);
      }
    }

    float totalGainDb = downGainDb + bodyLiftDb;
    compTargetGainLin = fastExp2f(totalGainDb * 0.16609640f) * compMakeupGain;
    float grDb = -downGainDb;
    compSilkTrim = min(0.62f, (0.20f + 0.32f * cAmt) + 0.022f * grDb);
  }

  // Secondary gain-slewing filter prevents audio-rate intermodulation distortion
  compGainSmooth += 0.008f * (compTargetGainLin - compGainSmooth);

  // 5d. Pre-saturation HF Silk Taming + Low-Mid Transformer Weight + 2-Stage Warm Vintage Tube Saturation
  float compL = (xL - compSilkTrim * hfL) * compGainSmooth;
  float compR = (xR - compSilkTrim * hfR) * compGainSmooth;

  // Low-mid (~180 Hz) transformer body tilt into the saturation stage for thick analog weight
  compBodyLpL += 0.0253f * (compL - compBodyLpL);
  compBodyLpR += 0.0253f * (compR - compBodyLpR);
  float dryBody = 1.0f - 0.18f * cAmt;
  float bodyWeight = 0.50f * cAmt;
  float preSatL = dryBody * compL + bodyWeight * compBodyLpL;
  float preSatR = dryBody * compR + bodyWeight * compBodyLpR;

  // Stage 1: Asymmetric 2nd-harmonic tube saturation (unit ceiling into Stage 2)
  const float drive = compSatDrive;
  const float invNorm = compSatInvNorm;
  const float asym = compSatAsym;
  float sat1L = warmAsymSaturate(preSatL, drive, 1.0f, asym);
  float sat1R = warmAsymSaturate(preSatR, drive, 1.0f, asym);

  // Stage 2: Smooth transformer soft-clip rounding + calibrated output trim (prevents volume jumps & clipping)
  float stage2Drive = 1.0f + 0.15f * cAmt;
  float satL = softTanhRational(sat1L * stage2Drive) * invNorm;
  float satR = softTanhRational(sat1R * stage2Drive) * invNorm;

  // 2-pole cascaded post-saturation warm roll-off guarantees zero harsh top-end harmonics
  const float outAlpha = compOutLpAlpha;
  compOutLp1L += outAlpha * (satL - compOutLp1L);
  compOutLp2L += outAlpha * (compOutLp1L - compOutLp2L);
  compOutLp1R += outAlpha * (satR - compOutLp1R);
  compOutLp2R += outAlpha * (compOutLp1R - compOutLp2R);

  const float cMix = compMix;
  xL = (1.0f - cMix) * xL + cMix * compOutLp2L;
  xR = (1.0f - cMix) * xR + cMix * compOutLp2R;
}

// ============================================================================
// Modular Master FX Registry & Dispatch (Chorus -> Delay -> Reverb -> Tape -> Comp)
// ============================================================================

PROGMEM static const MasterFxOps MASTER_FX_TABLE[MASTER_FX_COUNT] = {
  { MASTER_FX_CHOR, "CHOR", setMasterChorus,        resetChorusState, isChorusEnabled,        processChorusSample },
  { MASTER_FX_DLAY, "DLAY", setMasterDelay,         resetDelayState,  isDelayEnabled,         processDelaySample  },
  { MASTER_FX_DFB,  "D.FB", setMasterDelayFeedback, nullptr,          isDelayFeedbackActive,  nullptr             },
  { MASTER_FX_RVRB, "RVRB", setMasterReverb,        resetReverbState, isReverbEnabled,        processReverbSample },
  { MASTER_FX_TAPE, "TAPE", setMasterTape,          resetTapeState,   isTapeEnabled,          processTapeSample   },
  { MASTER_FX_COMP, "COMP", setMasterComp,          resetCompState,   isCompEnabled,          processCompSample   },
};

FLASHMEM const MasterFxOps &masterFxOpsFor(int slot) {
  int idx = constrain(slot, 0, MASTER_FX_COUNT - 1);
  return MASTER_FX_TABLE[idx];
}

FLASHMEM const char *masterFxLabel(int slot) {
  return masterFxOpsFor(slot).shortLabel;
}

FLASHMEM void setMasterFxSlot(int slot, uint8_t pct) {
  const MasterFxOps &ops = masterFxOpsFor(slot);
  if (ops.setAmount) {
    ops.setAmount(pct);
  }
}

FLASHMEM void resetMasterFxState() {
  for (int i = 0; i < MASTER_FX_COUNT; i++) {
    if (MASTER_FX_TABLE[i].resetState) {
      MASTER_FX_TABLE[i].resetState();
    }
  }
}

struct InstDriveState {
  float hpState = 0.0f;
  float bodyLp = 0.0f;
  float postLp1 = 0.0f;
  float postLp2 = 0.0f;
  uint8_t cachedPct = 0;
  float bodyGain = 0.0f;
  float drive1 = 1.0f;
  float asym = 0.0f;
  float drive2 = 1.0f;
  float trim = 0.68f;
  float lpAlpha = 0.78f;
  float mix = 0.0f;
};
constexpr int INST_DRIVE_STATE_SLOTS = NUM_INSTRUMENTS * 2;
static InstDriveState instDrive[INST_DRIVE_STATE_SLOTS];

FLASHMEM void resetInstDriveState(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  instDrive[instIdx] = InstDriveState{};
  instDrive[NUM_INSTRUMENTS + instIdx] = InstDriveState{};
}

FLASHMEM void resetInstFxState() {
  for (int i = 0; i < INST_DRIVE_STATE_SLOTS; i++) {
    instDrive[i] = InstDriveState{};
  }
  instChorPct = 0;
  instDlyPct = 0;
  instDfbPct = 0;
  instVerbPct = 0;
  configureChorusUnit(chor, masterChorPct);
  updateDelayUnitFeedback(dly, masterDfbPct);
  configureDelayUnit(dly, masterDlyPct);
  configureReverbUnit(verb, masterVerbPct);
  if (masterChorPct == 0) resetChorusUnit(chor);
  if (masterDlyPct == 0)  resetDelayUnit(dly);
  if (masterVerbPct == 0) resetReverbUnit(verb);
}

// Per-instrument DRIVE (0..100%): warm oversaturated analog distortion
// Signal path: 35 Hz tightener HPF -> 220 Hz warm body lift -> 2-stage asymmetric tube/oxide
// oversaturation -> level-matched output trim -> 2-pole warm post-LPF roll-off.
float processInstDriveSample(int instIdx, uint8_t drivePct, float x) {
  if (drivePct == 0 || instIdx < 0 || instIdx >= INST_DRIVE_STATE_SLOTS) {
    return x;
  }
  InstDriveState &st = instDrive[instIdx];
  if (st.cachedPct != drivePct) {
    st.cachedPct = drivePct;
    float d = (float)constrain((int)drivePct, 0, 100) * 0.01f;
    st.bodyGain = 0.45f * d;
    st.drive1 = 1.0f + d * (3.5f + 6.5f * d);
    st.asym = 0.28f * d;
    st.drive2 = 1.0f + d * (1.2f + 1.8f * d);
    st.trim = 0.68f / (1.0f + 1.35f * d);
    st.lpAlpha = 0.78f - 0.36f * d;
    st.mix = min(1.0f, d * 2.5f);
  }

  // 1. Tight 35 Hz sub-bass HPF + 220 Hz low-mid body warmth into saturation
  st.hpState += 0.0050f * (x - st.hpState);
  float clean = x - st.hpState;
  st.bodyLp += 0.0308f * (clean - st.bodyLp);
  float pre = clean + st.bodyGain * st.bodyLp;

  // 2. Stage 1: Asymmetric analog tube/oxide overdrive (up to 11.0x = +20.8 dB)
  float s1 = warmAsymSaturate(pre, st.drive1, 1.0f, st.asym);

  // 3. Stage 2: Warm oversaturated harmonic rounding + level-matched gain compensation
  float shaped = softTanhRational(s1 * st.drive2) * st.trim;

  // 4. 2-pole warm post-saturation LPF (rolls off harsh fizz from ~11.5 kHz down to ~3.8 kHz)
  const float lpAlpha = st.lpAlpha;
  st.postLp1 += lpAlpha * (shaped - st.postLp1);
  st.postLp2 += lpAlpha * (st.postLp1 - st.postLp2);

  const float mix = st.mix;
  return (1.0f - mix) * x + mix * st.postLp2;
}

void setInstFxParams(uint8_t chorusPct, uint8_t delayPct, uint8_t delayFbPct, uint8_t reverbPct) {
  instChorPct = (uint8_t)constrain((int)chorusPct, 0, 100);
  instDlyPct  = (uint8_t)constrain((int)delayPct, 0, 100);
  instDfbPct  = (uint8_t)constrain((int)delayFbPct, 0, 100);
  instVerbPct = (uint8_t)constrain((int)reverbPct, 0, 100);

  uint8_t effChor = max(masterChorPct, instChorPct);
  uint8_t effDfb  = max(masterDfbPct, instDfbPct);
  uint8_t effDly  = max(masterDlyPct, instDlyPct);
  uint8_t effVerb = max(masterVerbPct, instVerbPct);

  if (effChor != chor.curPct) {
    configureChorusUnit(chor, effChor);
  }
  if (effDfb != dly.fbPct) {
    updateDelayUnitFeedback(dly, effDfb);
  }
  if (effDly != dly.wetPct) {
    configureDelayUnit(dly, effDly);
  }
  if (effVerb != verb.curPct) {
    configureReverbUnit(verb, effVerb);
  }
}

bool isInstFxChainEnabled() {
  return (instChorPct > 0 && masterChorPct == 0 && chor.enabled) ||
         (instDlyPct  > 0 && masterDlyPct  == 0 && dly.enabled)  ||
         (instVerbPct > 0 && masterVerbPct == 0 && verb.enabled);
}

void processInstFxSample(float &xL, float &xR) {
  if (instChorPct > 0 && masterChorPct == 0 && chor.enabled) processChorusUnit(chor, xL, xR);
  if (instDlyPct  > 0 && masterDlyPct  == 0 && dly.enabled)  processDelayUnit(dly, xL, xR);
  if (instVerbPct > 0 && masterVerbPct == 0 && verb.enabled) processReverbUnit(verb, xL, xR);
}

void processMasterFx(float &mixL, float &mixR) {
  const bool doChor = (masterChorPct > 0) && chor.enabled;
  const bool doDlay = (masterDlyPct  > 0) && dly.enabled;
  const bool doVerb = (masterVerbPct > 0) && verb.enabled;
  const bool doTape = tapeEnabled;
  const bool doComp = compEnabled;
  if (!doChor && !doDlay && !doVerb && !doTape && !doComp) return;

  // Normalize 16-bit full-scale mix bus to [-1.0f, +1.0f]
  float xL = mixL * (1.0f / 32768.0f);
  float xR = mixR * (1.0f / 32768.0f);

  // Signal chain: Chorus -> Delay -> Reverb -> Tape -> Comp
  if (doChor) processChorusSample(xL, xR);
  if (doDlay) processDelaySample(xL, xR);
  if (doVerb) processReverbSample(xL, xR);
  if (doTape) processTapeSample(xL, xR);
  if (doComp) processCompSample(xL, xR);

  mixL = xL * 32768.0f;
  mixR = xR * 32768.0f;
}

} // namespace looper

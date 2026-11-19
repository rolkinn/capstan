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

static inline float fastSqrtPos(float x) {
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
  float r;
  __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
  return r;
#else
  return sqrtf(x);
#endif
}

// Smooth, strictly monotonic C^inf algebraic saturator on (-inf, +inf):
// matches unity slope at 0 and approaches +/-1 asymptotically with zero flat-topping
// (no dead-zone derivative wall on loud polyphonic chords).
static inline float softTanhRational(float u) {
  return u / fastSqrtPos(1.0f + u * u);
}

// Rational C1-continuous soft-knee safety ceiling (exact unity below `knee`, asymptotic to `ceiling`)
__attribute__((noinline)) float softKneeClip(float x, float knee, float ceiling) {
  float ax = fabsf(x);
  if (ax <= knee) return x;
  float w = ceiling - knee;
  float u = ax - knee;
  float y = knee + (w * u) / (w + u);
  return (x >= 0.0f) ? y : -y;
}

FLASHMEM void LookaheadMonitorLimiter::reset() {
  memset(delayL, 0, sizeof(delayL));
  memset(delayR, 0, sizeof(delayR));
  for (uint32_t i = 0; i < LOOKAHEAD; i++) {
    boxBuf[i] = ONE_Q24;
  }
  boxSum = FULL_BOX;
  dqHead = 0;
  dqTail = 0;
  sampleTick = 0;
  pos = 0;
  relGain = 1.0f;
  slowGain = 1.0f;
}

__attribute__((noinline)) float LookaheadMonitorLimiter::stepGain(float pk) {
  constexpr float REL_FAST = 0.00075f;  // ~30 ms smooth transient recovery (zero chord modulation)
  constexpr float REL_SLOW = 0.00014f;  // ~160 ms sustain recovery
  constexpr float SLOW_ATK = 0.00070f;  // ~32 ms sustain detector

  const uint16_t t = sampleTick++;
  const uint32_t p = pos;
  pos = (p + 1u) & (LOOKAHEAD - 1u);

  if (pk <= thresh && dqHead == dqTail && relGain >= 1.0f && boxSum == FULL_BOX) {
    return 1.0f;
  }

  float req = (pk > thresh) ? (thresh / pk) : 1.0f;

  while (dqHead != dqTail && (uint16_t)(t - dqIdx[dqHead & DQ_MASK]) >= MIN_WIN) {
    dqHead++;
  }
  if (req < 1.0f || dqHead != dqTail) {
    while (dqHead != dqTail && dqVal[(dqTail - 1u) & DQ_MASK] >= req) {
      dqTail--;
    }
    dqIdx[dqTail & DQ_MASK] = t;
    dqVal[dqTail & DQ_MASK] = req;
    dqTail++;
  }
  float minG = (dqHead != dqTail) ? dqVal[dqHead & DQ_MASK] : 1.0f;

  if (minG <= relGain) {
    relGain = minG;
    slowGain += (minG - slowGain) * SLOW_ATK;
  } else {
    slowGain += (1.0f - slowGain) * REL_SLOW;
    if (slowGain > 0.9998f) slowGain = 1.0f;
    if (relGain < slowGain) {
      relGain += (slowGain - relGain) * REL_FAST;
      if (relGain > slowGain) relGain = slowGain;
    } else {
      relGain = slowGain;
    }
    if (relGain > 0.9998f) relGain = 1.0f;
  }

  uint32_t q24 = (relGain >= 1.0f) ? ONE_Q24 : (uint32_t)(relGain * (float)ONE_Q24);
  if (q24 > ONE_Q24) q24 = ONE_Q24;
  boxSum = boxSum + q24 - boxBuf[p];
  boxBuf[p] = q24;

  return (boxSum >= FULL_BOX) ? 1.0f : ((float)boxSum * INV_BOX_Q24);
}

__attribute__((noinline)) float LookaheadMonitorLimiter::run(float inL, float inR, float &outL, float &outR) {
  const uint32_t p = pos;
  float dL = delayL[p];
  float dR = delayR[p];
  delayL[p] = inL;
  delayR[p] = inR;

  float pk = max(fabsf(inL), fabsf(inR));
  float g = stepGain(pk);
  if (g < 1.0f) {
    dL *= g;
    dR *= g;
  }
  outL = (fabsf(dL) <= knee) ? dL : softKneeClip(dL, knee, ceil);
  outR = (fabsf(dR) <= knee) ? dR : softKneeClip(dR, knee, ceil);
  return g;
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

static constexpr int CHORUS_BUF_LEN = 688; // ~15.6 ms max delay capacity at 44.1 kHz (max tap is 684 samples)
static int16_t chorBufL[CHORUS_BUF_LEN];
static int16_t chorBufR[CHORUS_BUF_LEN];

// Smooth C^1 soft-knee ceiling for Q14 (+/-2.0) delay/allpass buffers:
// exact linear pass-through up to +/-1.40 (+2.9 dBFS), smoothly asymptotic to +/-1.999.
__attribute__((noinline)) static float softLimitQ14Slow(float x) {
  float ax = fabsf(x);
  constexpr float W = 0.599f;
  float u = ax - 1.40f;
  float y = 1.40f + (W * u) / (W + u);
  return (x >= 0.0f) ? y : -y;
}

static inline float softLimitQ14(float x) {
  return (fabsf(x) <= 1.40f) ? x : softLimitQ14Slow(x);
}

static inline int16_t floatToQ14(float x) {
  float c = constrain(x, -1.9999f, 1.9999f) * 16384.0f;
  return (int16_t)((c >= 0.0f) ? (c + 0.5f) : (c - 0.5f));
}

static inline float readChorusTapQ14(const int16_t *buf, int bufLen, int writeIdx, float delaySamples) {
  int dInt = (int)delaySamples;
  float dFrac = delaySamples - (float)dInt;
  if (dInt < 1) dInt = 1;
  else if (dInt > bufLen - 2) dInt = bufLen - 2;
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
static uint8_t trackChorPct = 0;
static uint8_t inChorPct = 0;
static float masterChorAuxBoostL = 0.0f;
static float masterChorAuxBoostR = 0.0f;

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

__attribute__((noinline)) static void stepChorusTank(ChorusState &u, float inL, float inR, float &wetL, float &wetR) {
  u.lfoPhase += u.lfoInc;
  if (u.lfoPhase >= 1.0f) u.lfoPhase -= 1.0f;
  u.curWetMix += 0.002f * (u.wetMix - u.curWetMix);
  u.curModDepth += 0.002f * (u.modDepthSamples - u.curModDepth);

  // Quadrature stereo modulation (90-degree phase offset between Left and Right)
  const float baseDel = u.baseDelaySamples;
  const float modExc = u.curModDepth;
  float dL = baseDel + modExc * fastSin01(u.lfoPhase);
  float dR = baseDel + modExc * fastSin01(u.lfoPhase + 0.25f);

  float tapL = readChorusTapQ14(u.bufL, CHORUS_BUF_LEN, u.writeIdx, dL);
  float tapR = readChorusTapQ14(u.bufR, CHORUS_BUF_LEN, u.writeIdx, dR);

  u.bufL[u.writeIdx] = floatToQ14(softLimitQ14(inL));
  u.bufR[u.writeIdx] = floatToQ14(softLimitQ14(inR));
  if (--u.writeIdx < 0) u.writeIdx = CHORUS_BUF_LEN - 1;

  // Gentle warm smoothing (~10 kHz 1-pole LPF) on wet chorus tap
  u.lpStateL += 0.76f * (tapL - u.lpStateL);
  u.lpStateR += 0.76f * (tapR - u.lpStateR);

  const float wetG = 0.85f * u.curWetMix;
  wetL = wetG * u.lpStateL;
  wetR = wetG * u.lpStateR;
}

static inline void processChorusUnit(ChorusState &u, float &xL, float &xR) {
  float wetL = 0.0f, wetR = 0.0f;
  stepChorusTank(u, xL + masterChorAuxBoostL, xR + masterChorAuxBoostR, wetL, wetR);
  const float dryG = 1.0f - 0.25f * u.curWetMix;
  xL = dryG * xL + wetL;
  xR = dryG * xR + wetR;
}

static bool isChorusEnabled() {
  return masterChorPct > 0 && chor.enabled;
}

FLASHMEM static void resetChorusState() {
  resetChorusUnit(chor);
}

FLASHMEM void setMasterChorus(uint8_t pct) {
  masterChorPct = (uint8_t)constrain((int)pct, 0, 100);
  uint8_t eff = max(max(masterChorPct, instChorPct), max(trackChorPct, inChorPct));
  if (eff != chor.curPct || eff == 0) {
    configureChorusUnit(chor, eff);
  }
}

__attribute__((noinline)) static void processChorusSample(float &xL, float &xR) {
  processChorusUnit(chor, xL, xR);
}

// ============================================================================
// 2. Stereo Delay ("DLAY" & "D.FB") — Analog / Tape-Style Delay (~325 ms)
// ============================================================================

static constexpr int DELAY_BUF_LEN = 14440; // 327.4 ms capacity at 44.1 kHz (max tap with wow is 14,432 samples)
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
static uint8_t trackDlyPct = 0;
static uint8_t trackDfbPct = 0;
static uint8_t inDlyPct = 0;
static uint8_t inDfbPct = 0;
static float masterDlyAuxBoostL = 0.0f;
static float masterDlyAuxBoostR = 0.0f;

static inline float readDelayTapQ14(const int16_t *buf, int bufLen, int writeIdx, float delaySamples) {
  int dInt = (int)delaySamples;
  float dFrac = delaySamples - (float)dInt;
  if (dInt < 1) dInt = 1;
  else if (dInt > bufLen - 2) dInt = bufLen - 2;
  int r0 = writeIdx + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  float s0 = (float)buf[r0] * (1.0f / 16384.0f);
  float s1 = (float)buf[r1] * (1.0f / 16384.0f);
  return s0 + dFrac * (s1 - s0);
}

static inline float warmDelaySaturate(float x, float drive, float invDrive, float asym) {
  constexpr float HEADROOM = 1.85f;
  constexpr float INV_HEADROOM = 1.0f / HEADROOM;
  float rawU = (x * drive) * INV_HEADROOM;
  float u = 1.45f * softTanhRational(rawU * (1.0f / 1.45f));
  float u2 = u * u;
  // Soft cubic tape knee scaled to Q14 (+6 dBFS) headroom with subtle even-harmonic warmth (zero DC at u = 0)
  float shaped = HEADROOM * (u - 0.14814815f * u2 * u + asym * (u2 - 0.15f * u2 * u2)) * invDrive;
  return softLimitQ14(shaped);
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

__attribute__((noinline)) static void stepDelayTank(DelayState &u, float srcL, float srcR, float &wetL, float &wetR) {
  // Slow tape capstan wow (~0.75 Hz) with slight stereo phase offset
  u.wowPhase += (0.75f / (float)SAMPLE_RATE_HZ);
  if (u.wowPhase >= 1.0f) u.wowPhase -= 1.0f;
  u.curWetMix += 0.002f * (u.wetMix - u.curWetMix);
  u.curWowExc += 0.002f * (u.wowExcursion - u.curWowExc);
  const float wowExc = u.curWowExc;
  float dL = u.baseL + wowExc * fastSin01(u.wowPhase);
  float dR = u.baseR + wowExc * fastSin01(u.wowPhase + 0.2f);

  float rawTapL = readDelayTapQ14(u.bufL, u.bufLen, u.writeIdx, dL);
  float rawTapR = readDelayTapQ14(u.bufR, u.bufLen, u.writeIdx, dR);

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
  float inL = 0.96f * srcL + 0.04f * srcR;
  float inR = 0.96f * srcR + 0.04f * srcL;
  float fbL = fb * (0.86f * tapL + 0.16f * tapR);
  float fbR = fb * (0.82f * tapR + 0.14f * tapL);

  u.bufL[u.writeIdx] = floatToQ14(softLimitQ14(inL + fbL));
  u.bufR[u.writeIdx] = floatToQ14(softLimitQ14(inR + fbR));
  if (--u.writeIdx < 0) u.writeIdx = u.bufLen - 1;

  const float wetG = 0.85f * u.curWetMix;
  wetL = wetG * tapL;
  wetR = wetG * tapR;
}

static inline void processDelayUnit(DelayState &u, float &xL, float &xR) {
  float wetL = 0.0f, wetR = 0.0f;
  stepDelayTank(u, xL + masterDlyAuxBoostL, xR + masterDlyAuxBoostR, wetL, wetR);
  const float dryG = 1.0f - 0.12f * u.curWetMix;
  xL = dryG * xL + wetL;
  xR = dryG * xR + wetR;
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
  uint8_t eff = max(max(masterDlyPct, instDlyPct), max(trackDlyPct, inDlyPct));
  if (eff != dly.wetPct || eff == 0) {
    configureDelayUnit(dly, eff);
  }
}

FLASHMEM void setMasterDelayFeedback(uint8_t pct) {
  masterDfbPct = (uint8_t)constrain((int)pct, 0, 100);
  uint8_t eff = max(max(masterDfbPct, instDfbPct), max(trackDfbPct, inDfbPct));
  if (eff != dly.fbPct || eff == 0) {
    updateDelayUnitFeedback(dly, eff);
  }
}

__attribute__((noinline)) static void processDelaySample(float &xL, float &xR) {
  processDelayUnit(dly, xL, xR);
}

// ============================================================================
// 3. Reverb ("RVRB") — 3 Series Schroeder Allpasses/ch + Global Feedback + LPF
// ============================================================================

static constexpr int VERB_AP1_L_LEN = 1804; // 0.0408s * 44100 = 1799.28
static constexpr int VERB_AP2_L_LEN = 2076; // 0.0454s * 44100 = 2002.14 (+/- 66.15 max mod -> max tap 2069)
static constexpr int VERB_AP3_L_LEN = 2308; // 0.0522s * 44100 = 2302.02
static constexpr int VERB_AP1_R_LEN = 1996; // 0.0451s * 44100 = 1988.91
static constexpr int VERB_AP2_R_LEN = 1912; // 0.0417s * 44100 = 1838.97 (+/- 66.15 max mod -> max tap 1906)
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
enum VerbOwner : uint8_t {
  VERB_OWNER_MASTER = 0,
  VERB_OWNER_INST   = 1,
  VERB_OWNER_TRACK  = 2,
  VERB_OWNER_INPUT  = 3
};
static uint8_t masterVerbPct = 0;
static uint8_t instVerbPct = 0;
static uint8_t trackVerbPct = 0;
static uint8_t inVerbPct = 0;
static volatile uint8_t verbOwner = VERB_OWNER_MASTER;
static float masterVerbAuxBoostL = 0.0f;
static float masterVerbAuxBoostR = 0.0f;

static inline void refreshVerbOwner() {
  if (masterVerbPct > 0)                           verbOwner = VERB_OWNER_MASTER;
  else if (trackVerbPct > 0)                       verbOwner = VERB_OWNER_TRACK;
  else if (instVerbPct > 0 && inVerbPct > 0)       verbOwner = VERB_OWNER_TRACK;
  else if (instVerbPct > 0)                        verbOwner = VERB_OWNER_INST;
  else if (inVerbPct > 0)                          verbOwner = VERB_OWNER_INPUT;
}

static inline bool isMasterReverbBusActive() {
  return verb.enabled &&
         (masterVerbPct > 0 ||
          (verbOwner == VERB_OWNER_MASTER && instVerbPct == 0 && trackVerbPct == 0 && inVerbPct == 0));
}

static inline bool isInstReverbBusActive() {
  return verb.enabled && masterVerbPct == 0 && trackVerbPct == 0 && inVerbPct == 0 &&
         (instVerbPct > 0 || verbOwner == VERB_OWNER_INST);
}

static inline bool isInputReverbInlineActive() {
  return verb.enabled && masterVerbPct == 0 && trackVerbPct == 0 && instVerbPct == 0 &&
         (inVerbPct > 0 || verbOwner == VERB_OWNER_INPUT);
}

// Fixed-delay Schroeder allpass (integer tap -> zero fractional interpolation loss)
__attribute__((noinline)) static float runFixedApQ14(int16_t *buf, int bufLen, int &writeIdx,
                                                     int delaySamples, float g, float x) {
  int r0 = writeIdx + delaySamples;
  if (r0 >= bufLen) r0 -= bufLen;
  float yDel = (float)buf[r0] * (1.0f / 16384.0f);
  float vIn = softLimitQ14(x + g * yDel);
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
  float vIn = softLimitQ14(x + g * yDel);
  buf[writeIdx] = floatToQ14(vIn);
  if (--writeIdx < 0) writeIdx = bufLen - 1;
  return yDel - g * vIn;
}

// Modulated Schroeder allpass that also outputs the pure delayed tap (zero t=0 feedthrough)
static inline float runModApTapQ14(int16_t *buf, int bufLen, int &writeIdx,
                                   int dInt, float dFrac, float g, float x, float &outTap) {
  if (dInt < 1) dInt = 1;
  else if (dInt > bufLen - 2) dInt = bufLen - 2;
  int r0 = writeIdx + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  float s0 = (float)buf[r0] * (1.0f / 16384.0f);
  float s1 = (float)buf[r1] * (1.0f / 16384.0f);
  float yDel = s0 + dFrac * (s1 - s0);
  outTap = yDel;
  float vIn = softLimitQ14(x + g * yDel);
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
  u.enabled = (u.curPct > 0);
  restoreIrq(irq);

  clearReverbUnitBuffers(u);
}

FLASHMEM static void configureReverbUnit(ReverbState &u, uint8_t pct, bool clearBuffers = true) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  u.curPct = pct;
  if (pct == 0) {
    if (!u.enabled) return;
    // Keep u.enabled true while stepReverbTank smoothly glides curMix -> 0 (~95 ms, zero clicks/pops)
    uint32_t irq = saveIrq();
    u.mix = 0.0f;
    if (u.feedback > 0.85f) u.feedback = 0.85f;
    restoreIrq(irq);
    (void)clearBuffers;
    return;
  }

  float v = (float)pct * 0.01f;
  // Independent reverb decay curve:
  //   10% -> fb = 0.75 (room)
  //   25% -> fb = 0.82 (chamber)
  //   50% -> fb = 0.90 (hall)
  //   75% -> fb = 0.957 (large hall)
  //  100% -> fb = 0.974 (lush cavern)
  float shape = 1.0f - (1.0f - v) * (1.0f - v);
  float fb = 0.70f + 0.274f * shape;
  float damp = 0.62f - 0.14f * v;
  float inG = 0.80f - 0.25f * shape; // -6 dB diffuser/tank headroom (restored by 2.50x wetG)
  float modDepth = 0.50f + 0.25f * v;
  float modExc = ((float)SAMPLE_RATE_HZ / 500.0f) * modDepth;
  float mix = 0.14f + 0.72f * fastExp2f(0.75f * fastLog2f(v));

  if (!u.enabled) {
    // Tank is guaranteed idle in ISR while !u.enabled: clear stale samples before smooth curMix 0->mix ramp
    clearReverbUnitBuffers(u);
    uint32_t irq = saveIrq();
    u.idx1aL = u.idx1bL = u.idx1cL = 0;
    u.idx2L = u.idx3L = 0;
    u.idx1aR = u.idx1bR = u.idx1cR = 0;
    u.idx2R = u.idx3R = 0;
    u.lpfL = u.lpfR = 0.0f;
    u.outLpL = u.outLpR = 0.0f;
    u.lfoPhase = 0.0f;
    u.curModExc = modExc;
    u.curMix = 0.0f;
    u.feedback = fb;
    u.dampAlpha = damp;
    u.inGain = inG;
    u.modExcursion = modExc;
    u.mix = mix;
    u.enabled = true;
    restoreIrq(irq);
    return;
  }

  uint32_t irq = saveIrq();
  u.feedback = fb;
  u.dampAlpha = damp;
  u.inGain = inG;
  u.modExcursion = modExc;
  u.mix = mix;
  restoreIrq(irq);
}

static inline float computeReverbMix(uint8_t pct) {
  if (pct == 0) return 0.0f;
  float v = (float)constrain((int)pct, 0, 100) * 0.01f;
  return 0.14f + 0.72f * fastExp2f(0.75f * fastLog2f(v));
}

__attribute__((noinline)) static void stepReverbTank(ReverbState &u, float srcL, float srcR, float &wetL, float &wetR) {
  u.curMix += 0.002f * (u.mix - u.curMix);
  if (u.mix == 0.0f && u.curMix < 1.5e-4f) {
    u.curMix = 0.0f;
    u.enabled = false;
    wetL = 0.0f;
    wetR = 0.0f;
    return;
  }
  u.lfoPhase += (0.85f / (float)SAMPLE_RATE_HZ);
  if (u.lfoPhase >= 1.0f) u.lfoPhase -= 1.0f;
  u.curModExc += 0.002f * (u.modExcursion - u.curModExc);
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
  const float wetG = 2.50f * vMix;

  // 1. 3-stage short input diffuser per channel (partitions ap1L[1804] & ap1R[1996])
  // Immediately smears even ultra-short (<5 ms) plucks into a dense reflection cloud
  float inL = (0.92f * srcL + 0.08f * srcR) * inG;
  float inR = (0.92f * srcR + 0.08f * srcL) * inG;

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

  wetL = wetG * u.outLpL;
  wetR = wetG * u.outLpR;
}

static inline void processReverbUnit(ReverbState &u, float &xL, float &xR) {
  float wetL = 0.0f, wetR = 0.0f;
  stepReverbTank(u, xL + masterVerbAuxBoostL, xR + masterVerbAuxBoostR, wetL, wetR);
  const float dryG = 1.0f - 0.18f * u.curMix;
  xL = dryG * xL + wetL;
  xR = dryG * xR + wetR;
}

static bool isReverbEnabled() {
  return isMasterReverbBusActive();
}

FLASHMEM static void resetReverbState() {
  resetReverbUnit(verb);
}

FLASHMEM void setMasterReverb(uint8_t pct) {
  masterVerbPct = (uint8_t)constrain((int)pct, 0, 100);
  refreshVerbOwner();
  uint8_t eff = max(max(masterVerbPct, instVerbPct), max(trackVerbPct, inVerbPct));
  if (eff != verb.curPct || eff == 0) {
    configureReverbUnit(verb, eff);
  }
}

__attribute__((noinline)) static void processReverbSample(float &xL, float &xR) {
  processReverbUnit(verb, xL, xR);
}

// ============================================================================
// 4. Tape ("TAPE") — Head FIR + Low-End Bump -> Gentle Wow/Flutter -> Warm Sat -> 2-Pole Head LPF
// ============================================================================

static constexpr int TAPE_FIR_TAPS = 17;
// Reversed 17-tap Core Tape Head IR in fast RAM1 (68 B total, shared by Master + all 8 Audio Tracks;
// dry impulse tap k == 7 aligns at reversed index j == 9)
static const float TAPE_FIR_REV[TAPE_FIR_TAPS] = {
  0.02246044f, 0.03924405f, 0.04925077f, 0.03092099f, 0.03496643f,
  0.05302899f, 0.03080625f, 0.07077847f, 0.10938368f, 0.10938368f,
  0.10293065f, 0.06377593f, 0.06238867f, 0.04210958f, 0.02426576f,
  0.02360345f, 0.01259891f
};

static constexpr int TAPE_WOW_DELAY_LEN     = 88; // max sweep = 1.0 + 85.0 = 86.0 samples at 44.1 kHz
static constexpr int TAPE_FLUTTER_DELAY_LEN = 40; // max sweep = 1.0 + 36.0 = 37.0 samples at 44.1 kHz
static constexpr int TOTAL_TAPE_CHANNELS    = 2 + NUM_STEREO_AUDIO_TRACKS * 2 + NUM_MONO_AUDIO_TRACKS; // 14

// Split Q14 wow/flutter buffers across RAM2 (11 ch = 2,816 B) and RAM1 (3 ch = 768 B) to preserve
// both >= 32 KB RAM2 headroom and >= 64 KB RAM1 headroom across Model:01 and Model:02.
DMAMEM static int16_t tapeWowPoolDma[11][TAPE_WOW_DELAY_LEN];
DMAMEM static int16_t tapeFlutPoolDma[11][TAPE_FLUTTER_DELAY_LEN];
static int16_t tapeWowPoolRam1[3][TAPE_WOW_DELAY_LEN];
static int16_t tapeFlutPoolRam1[3][TAPE_FLUTTER_DELAY_LEN];
static int16_t tapeFirPool[TOTAL_TAPE_CHANNELS][TAPE_FIR_TAPS];

struct TapeChanState {
  float subHpState = 0.0f;
  float bumpLpState = 0.0f;
  float rwState = 0.0f;
  float noiseX1 = 0.0f, noiseX2 = 0.0f;
  float noiseY1 = 0.0f, noiseY2 = 0.0f;
  float lpf1State = 0.0f, lpf2State = 0.0f;
  float cachedAlpha = 0.85f;
  float cachedDrop = 1.0f;
  float smoothDrop = 1.0f;
};

static TapeChanState tapeChanPool[TOTAL_TAPE_CHANNELS];

struct TapeUnit {
  int16_t *firL = nullptr;
  int16_t *firR = nullptr;
  int16_t *wowL = nullptr;
  int16_t *wowR = nullptr;
  int16_t *flutL = nullptr;
  int16_t *flutR = nullptr;
  TapeChanState *chState = nullptr;
  int16_t firIdx = 0;
  int16_t wowIdx = 0;
  int16_t flutIdx = 0;
  uint8_t modSubPhase = 0;
  volatile bool enabled = false;
  float wowPhase = 0.0f;
  float flutPhase = 0.0f;
  float flutRandPrev = 0.0f;
  float flutRandNext = 0.0f;
  float flutSmooth = 0.0f;
  float wowCur = 0.0f;
  float flutCur = 0.0f;
  volatile float irMix = 0.0f;
  volatile float satMix = 0.0f;
  volatile float headBumpGain = 0.0f;
  volatile float wowTarget = 0.0f;
  volatile float flutTarget = 0.0f;
  volatile float noisePole = 0.998f;
  volatile float noiseModDepth = 0.0f;
  volatile float dropoutDepth = 0.0f;
  volatile float baseLpAlpha = 0.85f;
  volatile float satDrive = 1.0f;
  volatile float invSatNorm = 1.0f;
  volatile float satAsym = 0.0f;
};

static TapeUnit masterTape = {
  tapeFirPool[0],     tapeFirPool[1],
  tapeWowPoolDma[0],  tapeWowPoolDma[1],
  tapeFlutPoolDma[0], tapeFlutPoolDma[1],
  &tapeChanPool[0]
};

static TapeUnit trackTape[NUM_AUDIO_TRACKS] = {
  { tapeFirPool[2],  tapeFirPool[3],  tapeWowPoolDma[2],  tapeWowPoolDma[3],  tapeFlutPoolDma[2],  tapeFlutPoolDma[3], &tapeChanPool[2]  },
  { tapeFirPool[4],  tapeFirPool[5],  tapeWowPoolDma[4],  tapeWowPoolDma[5],  tapeFlutPoolDma[4],  tapeFlutPoolDma[5], &tapeChanPool[4]  },
  { tapeFirPool[6],  tapeFirPool[7],  tapeWowPoolDma[6],  tapeWowPoolDma[7],  tapeFlutPoolDma[6],  tapeFlutPoolDma[7], &tapeChanPool[6]  },
  { tapeFirPool[8],  tapeFirPool[9],  tapeWowPoolDma[8],  tapeWowPoolDma[9],  tapeFlutPoolDma[8],  tapeFlutPoolDma[9], &tapeChanPool[8]  },
  { tapeFirPool[10], nullptr,         tapeWowPoolDma[10], nullptr,            tapeFlutPoolDma[10], nullptr,            &tapeChanPool[10] },
  { tapeFirPool[11], nullptr,         tapeWowPoolRam1[0], nullptr,            tapeFlutPoolRam1[0], nullptr,            &tapeChanPool[11] },
  { tapeFirPool[12], nullptr,         tapeWowPoolRam1[1], nullptr,            tapeFlutPoolRam1[1], nullptr,            &tapeChanPool[12] },
  { tapeFirPool[13], nullptr,         tapeWowPoolRam1[2], nullptr,            tapeFlutPoolRam1[2], nullptr,            &tapeChanPool[13] }
};

static inline void runTapeDelayQ14(int16_t *bufL, int16_t *bufR, int bufLen, int16_t &writeIdx,
                                   float delaySamples, float &xL, float &xR, bool stereo) {
  int w = (int)writeIdx;
  bufL[w] = floatToQ14(softLimitQ14(xL));
  if (stereo) {
    bufR[w] = floatToQ14(softLimitQ14(xR));
  }
  int dInt = (int)delaySamples;
  if (dInt > bufLen - 2) dInt = bufLen - 2;
  float dFrac = delaySamples - (float)dInt;
  int r0 = w + dInt;
  if (r0 >= bufLen) r0 -= bufLen;
  int r1 = r0 + 1;
  if (r1 >= bufLen) r1 -= bufLen;
  float s0L = (float)bufL[r0] * (1.0f / 16384.0f);
  float s1L = (float)bufL[r1] * (1.0f / 16384.0f);
  xL = s0L + dFrac * (s1L - s0L);
  if (stereo) {
    float s0R = (float)bufR[r0] * (1.0f / 16384.0f);
    float s1R = (float)bufR[r1] * (1.0f / 16384.0f);
    xR = s0R + dFrac * (s1R - s0R);
  }
  if (--w < 0) w = bufLen - 1;
  writeIdx = (int16_t)w;
}

FLASHMEM static void clearTapeUnitBuffers(TapeUnit &u) {
  if (u.firL)  memset(u.firL,  0, TAPE_FIR_TAPS * sizeof(int16_t));
  if (u.firR)  memset(u.firR,  0, TAPE_FIR_TAPS * sizeof(int16_t));
  if (u.wowL)  memset(u.wowL,  0, TAPE_WOW_DELAY_LEN * sizeof(int16_t));
  if (u.wowR)  memset(u.wowR,  0, TAPE_WOW_DELAY_LEN * sizeof(int16_t));
  if (u.flutL) memset(u.flutL, 0, TAPE_FLUTTER_DELAY_LEN * sizeof(int16_t));
  if (u.flutR) memset(u.flutR, 0, TAPE_FLUTTER_DELAY_LEN * sizeof(int16_t));
}

FLASHMEM static void resetTapeChanSlice(TapeUnit &u, float baseAlpha) {
  int nCh = (u.firR != nullptr) ? 2 : 1;
  for (int ch = 0; ch < nCh; ch++) {
    u.chState[ch] = TapeChanState{};
    u.chState[ch].cachedAlpha = baseAlpha;
  }
}

FLASHMEM static void resetTapeUnit(TapeUnit &u) {
  uint32_t irq = saveIrq();
  u.firIdx = 0;
  u.wowIdx = 0;
  u.flutIdx = 0;
  u.wowPhase = 0.0f;
  u.flutPhase = 0.0f;
  u.flutRandPrev = 0.0f;
  u.flutRandNext = 0.0f;
  u.flutSmooth = 0.0f;
  u.wowCur = u.wowTarget;
  u.flutCur = u.flutTarget;
  u.modSubPhase = 0;
  resetTapeChanSlice(u, u.baseLpAlpha);
  restoreIrq(irq);

  clearTapeUnitBuffers(u);
}

FLASHMEM static void configureTapeUnit(TapeUnit &u, uint8_t pct) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  if (pct == 0) {
    bool wasEnabled = u.enabled;
    uint32_t irq = saveIrq();
    u.enabled = false;
    u.irMix = 0.0f;
    u.satMix = 0.0f;
    u.headBumpGain = 0.0f;
    u.wowTarget = 0.0f;
    u.flutTarget = 0.0f;
    u.wowCur = 0.0f;
    u.flutCur = 0.0f;
    u.noisePole = 0.998f;
    u.noiseModDepth = 0.0f;
    u.dropoutDepth = 0.0f;
    u.baseLpAlpha = 0.85f;
    u.satDrive = 1.0f;
    u.invSatNorm = 1.0f;
    u.satAsym = 0.0f;
    u.modSubPhase = 0;
    resetTapeChanSlice(u, 0.85f);
    restoreIrq(irq);
    if (wasEnabled) {
      clearTapeUnitBuffers(u);
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
  float refOut = softTanhRational(drive * 0.55f) / 0.55f;
  float invNorm = (refOut > 0.01f) ? (1.0f / refOut) : 1.0f;

  uint32_t irq = saveIrq();
  u.irMix = irMix;
  u.satMix = satMix;
  u.headBumpGain = bumpGain;
  u.wowTarget = wowTgt;
  u.flutTarget = flutTgt;
  u.noisePole = nPole;
  u.noiseModDepth = nMod;
  u.dropoutDepth = dropDepth;
  u.baseLpAlpha = baseAlpha;
  u.satDrive = drive;
  u.invSatNorm = invNorm;
  u.satAsym = asym;
  if (!u.enabled) {
    int nCh = (u.firR != nullptr) ? 2 : 1;
    for (int ch = 0; ch < nCh; ch++) {
      u.chState[ch].cachedAlpha = baseAlpha;
      u.chState[ch].cachedDrop = 1.0f;
      u.chState[ch].smoothDrop = 1.0f;
    }
  }
  u.enabled = true;
  restoreIrq(irq);
}

__attribute__((noinline)) static void processTapeUnit(TapeUnit &u, float &xL, float &xR, bool stereo) {
  const bool st = stereo && (u.firR != nullptr);

  // 4a. 17-tap Core Tape Head Impulse Response FIR (dry tap aligned at reversed offset 9)
  int fIdx = (int)u.firIdx - 1;
  if (fIdx < 0) fIdx = TAPE_FIR_TAPS - 1;
  u.firIdx = (int16_t)fIdx;

  u.firL[fIdx] = floatToQ14(softLimitQ14(xL));
  if (st) {
    u.firR[fIdx] = floatToQ14(softLimitQ14(xR));
  }
  const float ir = u.irMix;
  const float dryIr = 1.0f - ir;
  float accL = 0.0f, accR = 0.0f;
  int rIdx = fIdx;
#pragma GCC unroll 0
  for (int j = 0; j < TAPE_FIR_TAPS; j++) {
    float w = TAPE_FIR_REV[j];
    accL += (float)u.firL[rIdx] * w;
    if (st) accR += (float)u.firR[rIdx] * w;
    if (++rIdx >= TAPE_FIR_TAPS) rIdx = 0;
  }
  int dryIdx = fIdx + 9;
  if (dryIdx >= TAPE_FIR_TAPS) dryIdx -= TAPE_FIR_TAPS;
  xL = (ir * accL + dryIr * (float)u.firL[dryIdx]) * (1.0f / 16384.0f);
  if (st) {
    xR = (ir * accR + dryIr * (float)u.firR[dryIdx]) * (1.0f / 16384.0f);
  }

  // 4b. Warm Tape Head Bump (~95 Hz resonant bass fullness + 30 Hz sub-rumble tightener)
  TapeChanState *cs = u.chState;
  const float bumpG = u.headBumpGain;
  cs[0].subHpState += 0.00427f * (xL - cs[0].subHpState);
  float cleanL = xL - cs[0].subHpState;
  cs[0].bumpLpState += 0.01344f * (cleanL - cs[0].bumpLpState);
  xL = cleanL + bumpG * cs[0].bumpLpState;

  if (st) {
    cs[1].subHpState += 0.00427f * (xR - cs[1].subHpState);
    float cleanR = xR - cs[1].subHpState;
    cs[1].bumpLpState += 0.01344f * (cleanR - cs[1].bumpLpState);
    xR = cleanR + bumpG * cs[1].bumpLpState;
  }

  // 4c. Gentle Wow (1.6 Hz sine) & Smoothed Flutter (11.5 Hz) fractional delay lines
  u.wowCur += (u.wowTarget - u.wowCur) * 0.0005f;
  u.flutCur += (u.flutTarget - u.flutCur) * 0.0005f;

  u.wowPhase += (1.6f / (float)SAMPLE_RATE_HZ);
  if (u.wowPhase >= 1.0f) u.wowPhase -= 1.0f;
  float wowLfo = 0.5f * fastSin01(u.wowPhase) + 0.5f;
  float wowDelay = 1.0f + u.wowCur * wowLfo;
  runTapeDelayQ14(u.wowL, u.wowR, TAPE_WOW_DELAY_LEN, u.wowIdx, wowDelay, xL, xR, st);

  u.flutPhase += (11.5f / (float)SAMPLE_RATE_HZ);
  if (u.flutPhase >= 1.0f) {
    u.flutPhase -= 1.0f;
    u.flutRandPrev = u.flutRandNext;
    u.flutRandNext = nextFxRandMinus1To1();
  }
  float rawFlut = u.flutRandPrev + u.flutPhase * (u.flutRandNext - u.flutRandPrev);
  u.flutSmooth += 0.08f * (rawFlut - u.flutSmooth);
  float flutLfo = 0.5f * u.flutSmooth + 0.5f;
  float flutDelay = 1.0f + u.flutCur * flutLfo;
  runTapeDelayQ14(u.flutL, u.flutR, TAPE_FLUTTER_DELAY_LEN, u.flutIdx, flutDelay, xL, xR, st);

  // 4d. Smooth Monotonic Asymmetric Tape Oxide Saturation (runs BEFORE head-loss LPF)
  const float drive = u.satDrive;
  const float invNorm = u.invSatNorm;
  const float asym = u.satAsym;
  const float sMix = u.satMix;

  float shapedL = warmAsymSaturate(xL, drive, invNorm, asym);
  xL = (1.0f - sMix) * xL + sMix * shapedL;
  if (st) {
    float shapedR = warmAsymSaturate(xR, drive, invNorm, asym);
    xR = (1.0f - sMix) * xR + sMix * shapedR;
  }

  // 4e. Post-Saturation 2-Pole Tape Head Loss LPF (14 kHz -> 5.5 kHz) + Smooth Brownian Drift & Dropouts
  const int nCh = st ? 2 : 1;
  if ((u.modSubPhase++ & 7u) == 0u) {
    constexpr float LWR_A0 = 0.0001117914f;
    constexpr float LWR_A1 = 0.0002235828f;
    constexpr float LWR_A2 = 0.0001117914f;
    constexpr float LWR_B1 = -1.9788855f;
    constexpr float LWR_B2 = 0.9793327f;
    const float nPole = u.noisePole;
    const float nMod = u.noiseModDepth;
    const float baseAlpha = u.baseLpAlpha;
    const float dropDepth = u.dropoutDepth;

    for (int ch = 0; ch < nCh; ch++) {
      TapeChanState &c = cs[ch];
      float rw = constrain(nextFxRandMinus1To1() + c.rwState, -4.0f, 4.0f);
      c.rwState = rw * nPole;

      float nz = LWR_A0 * rw + LWR_A1 * c.noiseX1 + LWR_A2 * c.noiseX2
               - LWR_B1 * c.noiseY1 - LWR_B2 * c.noiseY2;
      nz = constrain(nz, -4.0f, 4.0f);
      c.noiseX2 = c.noiseX1;
      c.noiseX1 = rw;
      c.noiseY2 = c.noiseY1;
      c.noiseY1 = nz;

      float modVal = nMod * nz;
      float modSq = (modVal * modVal) / (1.0f + modVal * modVal);
      c.cachedAlpha = constrain(baseAlpha * (1.0f - 0.35f * modSq), 0.25f, 0.90f);
      c.cachedDrop = 1.0f - dropDepth * modSq;
    }
  }

  float chIn[2] = {xL, xR};
  for (int ch = 0; ch < nCh; ch++) {
    TapeChanState &c = cs[ch];
    const float alpha = c.cachedAlpha;
    c.smoothDrop += 0.05f * (c.cachedDrop - c.smoothDrop);
    float s1 = c.lpf1State + alpha * (chIn[ch] - c.lpf1State);
    float s2 = c.lpf2State + alpha * (s1 - c.lpf2State);
    c.lpf1State = s1;
    c.lpf2State = s2;
    chIn[ch] = s2 * c.smoothDrop;
  }
  xL = chIn[0];
  xR = st ? chIn[1] : chIn[0];
}

static bool isTapeEnabled() {
  return masterTape.enabled;
}

FLASHMEM static void resetTapeState() {
  resetTapeUnit(masterTape);
}

FLASHMEM void setMasterTape(uint8_t pct) {
  configureTapeUnit(masterTape, pct);
}

static void processTapeSample(float &xL, float &xR) {
  processTapeUnit(masterTape, xL, xR, true);
}

// ============================================================================
// 5. Comp ("COMP") — Warm Analog Bus / Opto Glue Compressor with Dynamic HF Silk
// ============================================================================

struct CompChanState {
  float env = 0.0f;
  float scHp = 0.0f;
  float silkLp = 0.0f;
  float bodyLp = 0.0f;
  float outLp1 = 0.0f;
  float outLp2 = 0.0f;
};

static CompChanState compChanPool[TOTAL_TAPE_CHANNELS];

struct CompUnit {
  CompChanState *chState = nullptr;
  uint8_t numCh = 2;
  uint8_t subPhase = 0;
  volatile bool enabled = false;
  float gainSmooth = 1.0f;
  float targetGainLin = 1.0f;
  float silkTrim = 0.20f;
  volatile float mix = 0.0f;
  volatile float amount = 0.0f;
  volatile float alphaAtk = 0.999f;
  volatile float alphaRel = 0.9998f;
  volatile float baseSlope = 0.5f;
  volatile float bodyLiftMaxDb = 0.0f;
  volatile float makeupGain = 1.0f;
  volatile float satDrive = 1.0f;
  volatile float satInvNorm = 1.0f;
  volatile float satAsym = 0.0f;
  volatile float outLpAlpha = 0.82f;
};

static CompUnit masterComp = { &compChanPool[0], 2 };
static CompUnit trackComp[NUM_AUDIO_TRACKS] = {
  { &compChanPool[2],  2 },
  { &compChanPool[4],  2 },
  { &compChanPool[6],  2 },
  { &compChanPool[8],  2 },
  { &compChanPool[10], 1 },
  { &compChanPool[11], 1 },
  { &compChanPool[12], 1 },
  { &compChanPool[13], 1 }
};

FLASHMEM static void resetCompUnit(CompUnit &u) {
  uint32_t irq = saveIrq();
  for (int ch = 0; ch < (int)u.numCh; ch++) {
    u.chState[ch] = CompChanState{};
  }
  u.gainSmooth = 1.0f;
  u.targetGainLin = 1.0f;
  u.silkTrim = 0.20f;
  u.subPhase = 0;
  restoreIrq(irq);
}

FLASHMEM static void configureCompUnit(CompUnit &u, uint8_t pct) {
  pct = (uint8_t)constrain((int)pct, 0, 100);
  if (pct == 0) {
    uint32_t irq = saveIrq();
    u.enabled = false;
    u.mix = 0.0f;
    u.amount = 0.0f;
    restoreIrq(irq);
    resetCompUnit(u);
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
  float makeup = 1.0f;

  // Warm, thick vintage tube/transformer saturation that scales progressively with COMP %
  float drive = 1.0f + 3.20f * c * (0.40f + 0.60f * c);
  float asym = 0.28f * c;
  float stage2Drive = 1.0f + 0.15f * c;
  float ref1 = softTanhRational(drive * 0.34f);
  float ref2 = softTanhRational(ref1 * stage2Drive);
  float invNorm = (ref2 > 0.01f) ? (0.34f / ref2) : 1.0f;
  float outLpAlpha = 0.80f - 0.33f * c;

  uint32_t irq = saveIrq();
  u.mix = mix;
  u.amount = c;
  u.alphaAtk = aAtk;
  u.alphaRel = aRel;
  u.baseSlope = slope;
  u.bodyLiftMaxDb = bodyLiftDb;
  u.makeupGain = makeup;
  u.satDrive = drive;
  u.satInvNorm = invNorm;
  u.satAsym = asym;
  u.outLpAlpha = outLpAlpha;
  u.enabled = true;
  restoreIrq(irq);
}

__attribute__((noinline)) static void processCompUnit(CompUnit &u, float &xL, float &xR, bool stereo) {
  const float aAtk = u.alphaAtk;
  const float aRel = u.alphaRel;
  const float cAmt = u.amount;
  const bool st = stereo && (u.numCh > 1);
  const int nCh = st ? 2 : 1;
  CompChanState *cs = u.chState;
  float inCh[2] = {xL, xR};
  float hf[2] = {};

  // 5a & 5b. Sidechain filtering (90 Hz HPF + HF silk pre-emphasis) + Mean-Square Detector
  for (int ch = 0; ch < nCh; ch++) {
    CompChanState &c = cs[ch];
    float x = inCh[ch];
    c.scHp += 0.01274f * (x - c.scHp);
    float scHp = x - c.scHp;
    c.silkLp += 0.52f * (x - c.silkLp);
    float h = x - c.silkLp;
    hf[ch] = h;
    float det = scHp + 0.45f * h;
    float x2 = det * det;
    float a = (x2 > c.env) ? aAtk : aRel;
    c.env = constrain(x2 + a * (c.env - x2), 0.0f, 2.0f);
  }

  // 5c. Soft-Knee Downward Compression (-18 dB threshold, 12 dB knee) + Bounded Body Lift (1/4 rate)
  if ((u.subPhase++ & 3u) == 0u) {
    float msAvg = st ? (0.5f * (cs[0].env + cs[1].env)) : cs[0].env;
    float levelDb = (msAvg > 1e-10f) ? max(-96.0f, 3.01029995f * fastLog2f(msAvg)) : -96.0f;

    const float slope = u.baseSlope;
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

    float bodyLiftDb = 0.0f;
    if (levelDb > -48.0f && levelDb < -14.0f) {
      float norm = (levelDb + 31.0f) * (1.0f / 17.0f);
      float win = 1.0f - norm * norm;
      if (win > 0.0f) {
        bodyLiftDb = u.bodyLiftMaxDb * (win * win);
      }
    }

    float totalGainDb = downGainDb + bodyLiftDb;
    u.targetGainLin = fastExp2f(totalGainDb * 0.16609640f) * u.makeupGain;
    float grDb = -downGainDb;
    u.silkTrim = min(0.62f, (0.20f + 0.32f * cAmt) + 0.022f * grDb);
  }

  u.gainSmooth += 0.008f * (u.targetGainLin - u.gainSmooth);

  // 5d. Pre-saturation HF Silk Taming + Low-Mid Transformer Weight + 2-Stage Warm Vintage Tube Saturation
  const float gSmooth = u.gainSmooth;
  const float silkT = u.silkTrim;
  const float dryBody = 1.0f - 0.18f * cAmt;
  const float bodyWeight = 0.50f * cAmt;
  const float drive = u.satDrive;
  const float invNorm = u.satInvNorm;
  const float asym = u.satAsym;
  const float stage2Drive = 1.0f + 0.15f * cAmt;
  const float outAlpha = u.outLpAlpha;
  const float cMix = u.mix;

  float outCh[2] = {xL, xR};
  for (int ch = 0; ch < nCh; ch++) {
    CompChanState &c = cs[ch];
    float x = inCh[ch];
    float comp = (x - silkT * hf[ch]) * gSmooth;
    c.bodyLp += 0.0253f * (comp - c.bodyLp);
    float preSat = dryBody * comp + bodyWeight * c.bodyLp;
    float sat1 = warmAsymSaturate(preSat, drive, 1.0f, asym);
    float sat2 = softTanhRational(sat1 * stage2Drive) * invNorm;
    c.outLp1 += outAlpha * (sat2 - c.outLp1);
    c.outLp2 += outAlpha * (c.outLp1 - c.outLp2);
    outCh[ch] = (1.0f - cMix) * x + cMix * c.outLp2;
  }
  xL = outCh[0];
  xR = st ? outCh[1] : outCh[0];
}

static bool isCompEnabled() {
  return masterComp.enabled;
}

FLASHMEM static void resetCompState() {
  resetCompUnit(masterComp);
}

FLASHMEM void setMasterComp(uint8_t pct) {
  configureCompUnit(masterComp, pct);
}

static void processCompSample(float &xL, float &xR) {
  processCompUnit(masterComp, xL, xR, true);
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
  masterChorAuxBoostL = masterChorAuxBoostR = 0.0f;
  masterDlyAuxBoostL = masterDlyAuxBoostR = 0.0f;
  masterVerbAuxBoostL = masterVerbAuxBoostR = 0.0f;
}

// ============================================================================
// Shared Drive Engine (Per-Instrument + Per-Audio-Track Stereo/Mono Channels)
// ============================================================================

struct DriveChanState {
  float hpState = 0.0f;
  float bodyLp = 0.0f;
  float postLp1 = 0.0f;
  float postLp2 = 0.0f;
};

struct DriveCoeffs {
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
static DriveChanState instDriveCh[INST_DRIVE_STATE_SLOTS][2];
static DriveCoeffs instDriveCoeffs[NUM_INSTRUMENTS];

static constexpr int TRACK_DRIVE_CHANNELS = NUM_STEREO_AUDIO_TRACKS * 2 + NUM_MONO_AUDIO_TRACKS; // 12
static DriveChanState trackDrivePool[TRACK_DRIVE_CHANNELS];
static DriveCoeffs trackDriveCoeffs[NUM_AUDIO_TRACKS];

static inline int trackDriveBaseChan(int t) {
  return (t < NUM_STEREO_AUDIO_TRACKS) ? (t * 2) : (NUM_STEREO_AUDIO_TRACKS * 2 + (t - NUM_STEREO_AUDIO_TRACKS));
}

FLASHMEM __attribute__((noinline)) static void configureDriveCoeffs(DriveCoeffs &c, uint8_t drivePct) {
  uint8_t clamped = (uint8_t)constrain((int)drivePct, 0, 100);
  c.cachedPct = clamped;
  if (clamped == 0) {
    c.bodyGain = 0.0f;
    c.drive1 = 1.0f;
    c.asym = 0.0f;
    c.drive2 = 1.0f;
    c.trim = 0.68f;
    c.lpAlpha = 0.78f;
    c.mix = 0.0f;
    return;
  }
  float d = (float)clamped * 0.01f;
  c.bodyGain = 0.45f * d;
  c.drive1 = 1.0f + d * (3.5f + 6.5f * d);
  c.asym = 0.28f * d;
  c.drive2 = 1.0f + d * (1.2f + 1.8f * d);
  c.trim = 0.68f / (1.0f + 1.35f * d);
  c.lpAlpha = 0.78f - 0.36f * d;
  c.mix = min(1.0f, d * 2.5f);
}

__attribute__((noinline)) static float stepDriveChannel(DriveChanState &st, const DriveCoeffs &c, float x) {
  st.hpState += 0.0050f * (x - st.hpState);
  float clean = x - st.hpState;
  st.bodyLp += 0.0308f * (clean - st.bodyLp);
  float pre = clean + c.bodyGain * st.bodyLp;

  float s1 = warmAsymSaturate(pre, c.drive1, 1.0f, c.asym);
  float shaped = softTanhRational(s1 * c.drive2) * c.trim;

  const float lpAlpha = c.lpAlpha;
  st.postLp1 += lpAlpha * (shaped - st.postLp1);
  st.postLp2 += lpAlpha * (st.postLp1 - st.postLp2);

  const float mix = c.mix;
  return (1.0f - mix) * x + mix * st.postLp2;
}

FLASHMEM void resetInstDriveState(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  instDriveCh[instIdx][0] = DriveChanState{};
  instDriveCh[instIdx][1] = DriveChanState{};
  instDriveCoeffs[instIdx] = DriveCoeffs{};
  int capSlot = NUM_INSTRUMENTS + instIdx;
  instDriveCh[capSlot][0] = DriveChanState{};
  instDriveCh[capSlot][1] = DriveChanState{};
}

FLASHMEM static void syncSharedAuxTanksSlow() {
  refreshVerbOwner();
  uint8_t effChor = max(max(masterChorPct, instChorPct), max(trackChorPct, inChorPct));
  uint8_t effDfb  = max(max(masterDfbPct,  instDfbPct),  max(trackDfbPct,  inDfbPct));
  uint8_t effDly  = max(max(masterDlyPct,  instDlyPct),  max(trackDlyPct,  inDlyPct));
  uint8_t effVerb = max(max(masterVerbPct, instVerbPct), max(trackVerbPct, inVerbPct));

  if (effChor != chor.curPct || effChor == 0) {
    configureChorusUnit(chor, effChor, effChor == 0);
  }
  if (effDfb != dly.fbPct || effDfb == 0) {
    updateDelayUnitFeedback(dly, effDfb);
  }
  if (effDly != dly.wetPct || effDly == 0) {
    configureDelayUnit(dly, effDly, effDly == 0);
  }
  if (effVerb != verb.curPct || effVerb == 0) {
    configureReverbUnit(verb, effVerb, effVerb == 0);
  }
}

static int16_t instWowPoolRam1[2][TAPE_WOW_DELAY_LEN];
static int16_t instFlutPoolRam1[2][TAPE_FLUTTER_DELAY_LEN];
static int16_t instFirPool[2][TAPE_FIR_TAPS];
static TapeChanState instTapeChanPool[2];
static CompChanState instCompChanPool[2];
static TapeUnit instTape = {
  instFirPool[0], instFirPool[1],
  instWowPoolRam1[0], instWowPoolRam1[1],
  instFlutPoolRam1[0], instFlutPoolRam1[1],
  &instTapeChanPool[0]
};
static CompUnit instComp = { &instCompChanPool[0], 2 };
static uint8_t instTapeBusPct = 0;
static uint8_t instTapeCfgPct = 0;
static uint8_t instCompBusPct = 0;
static uint8_t instCompCfgPct = 0;

FLASHMEM void resetInstFxState() {
  for (int i = 0; i < INST_DRIVE_STATE_SLOTS; i++) {
    instDriveCh[i][0] = DriveChanState{};
    instDriveCh[i][1] = DriveChanState{};
  }
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    instDriveCoeffs[i] = DriveCoeffs{};
  }
  instChorPct = 0;
  instDlyPct = 0;
  instDfbPct = 0;
  instVerbPct = 0;
  instTapeBusPct = 0;
  instTapeCfgPct = 0;
  instCompBusPct = 0;
  instCompCfgPct = 0;
  configureTapeUnit(instTape, 0);
  configureCompUnit(instComp, 0);
  syncSharedAuxTanksSlow();
  if (masterChorPct == 0 && trackChorPct == 0 && inChorPct == 0) resetChorusUnit(chor);
  if (masterDlyPct == 0  && trackDlyPct == 0  && inDlyPct == 0)  resetDelayUnit(dly);
  if (masterVerbPct == 0 && trackVerbPct == 0 && inVerbPct == 0) resetReverbUnit(verb);
}

// Per-instrument DRIVE (0..100%): warm oversaturated analog distortion
float processInstDriveChannelSample(int instIdx, int ch, uint8_t drivePct, float x) {
  if (drivePct == 0 || instIdx < 0 || instIdx >= INST_DRIVE_STATE_SLOTS) {
    return x;
  }
  DriveCoeffs &c = instDriveCoeffs[instIdx & (NUM_INSTRUMENTS - 1)];
  if (c.cachedPct != drivePct) {
    configureDriveCoeffs(c, drivePct);
  }
  return stepDriveChannel(instDriveCh[instIdx][ch & 1], c, x);
}

float processInstDriveSample(int instIdx, uint8_t drivePct, float x) {
  return processInstDriveChannelSample(instIdx, 0, drivePct, x);
}

FLASHMEM __attribute__((noinline)) static void applyInstFxParamsSlow(
    uint8_t chorusPct, uint8_t delayPct, uint8_t delayFbPct, uint8_t reverbPct,
    uint8_t tapePct, uint8_t compPct) {
  const bool auxChanged = (chorusPct != instChorPct || delayPct != instDlyPct ||
                           delayFbPct != instDfbPct || reverbPct != instVerbPct);
  instChorPct = chorusPct;
  instDlyPct  = delayPct;
  instDfbPct  = delayFbPct;
  instVerbPct = reverbPct;
  if (auxChanged) {
    syncSharedAuxTanksSlow();
  }

  if (tapePct > 0) {
    if (tapePct != instTapeCfgPct) {
      configureTapeUnit(instTape, tapePct);
      instTapeCfgPct = tapePct;
    } else if (!instTape.enabled) {
      uint32_t irq = saveIrq();
      instTape.enabled = true;
      restoreIrq(irq);
    }
    instTapeBusPct = tapePct;
  } else if (instTapeBusPct > 0) {
    uint32_t irq = saveIrq();
    instTapeBusPct = 0;
    instTape.enabled = false;
    resetTapeChanSlice(instTape, instTape.baseLpAlpha);
    restoreIrq(irq);
    clearTapeUnitBuffers(instTape);
  }

  if (compPct > 0) {
    if (compPct != instCompCfgPct) {
      configureCompUnit(instComp, compPct);
      instCompCfgPct = compPct;
    } else if (!instComp.enabled) {
      uint32_t irq = saveIrq();
      instComp.enabled = true;
      restoreIrq(irq);
    }
    instCompBusPct = compPct;
  } else if (instCompBusPct > 0) {
    uint32_t irq = saveIrq();
    instCompBusPct = 0;
    instComp.enabled = false;
    restoreIrq(irq);
    resetCompUnit(instComp);
  }
}

FLASHMEM void setInstFxParams(uint8_t chorusPct, uint8_t delayPct, uint8_t delayFbPct, uint8_t reverbPct,
                              uint8_t tapePct, uint8_t compPct) {
  uint8_t c = (chorusPct > 100u) ? 100u : chorusPct;
  uint8_t d = (delayPct > 100u) ? 100u : delayPct;
  uint8_t f = (delayFbPct > 100u) ? 100u : delayFbPct;
  uint8_t r = (reverbPct > 100u) ? 100u : reverbPct;
  uint8_t t = (tapePct > 100u) ? 100u : tapePct;
  uint8_t m = (compPct > 100u) ? 100u : compPct;
  if (c == instChorPct && d == instDlyPct && f == instDfbPct && r == instVerbPct &&
      t == instTapeBusPct && m == instCompBusPct) {
    return;
  }
  applyInstFxParamsSlow(c, d, f, r, t, m);
}

bool isInstFxChainEnabled() {
  return (instChorPct > 0 && masterChorPct == 0 && trackChorPct == 0 && inChorPct == 0 && chor.enabled) ||
         (instDlyPct  > 0 && masterDlyPct  == 0 && trackDlyPct  == 0 && inDlyPct  == 0 && dly.enabled)  ||
         isInstReverbBusActive() ||
         (instTapeBusPct > 0 && instTape.enabled) ||
         (instCompBusPct > 0 && instComp.enabled);
}

bool instNeedsSharedAuxSend() {
  const bool chorDeferred = (instChorPct > 0) &&
                            !(masterChorPct == 0 && trackChorPct == 0 && inChorPct == 0 && chor.enabled);
  const bool dlyDeferred  = (instDlyPct > 0) &&
                            !(masterDlyPct == 0 && trackDlyPct == 0 && inDlyPct == 0 && dly.enabled);
  const bool verbDeferred = (instVerbPct > 0) && !isInstReverbBusActive();
  return chorDeferred || dlyDeferred || verbDeferred;
}

void processInstFxSample(float &xL, float &xR) {
  if (instChorPct > 0 && masterChorPct == 0 && trackChorPct == 0 && inChorPct == 0 && chor.enabled) processChorusSample(xL, xR);
  if (instDlyPct  > 0 && masterDlyPct  == 0 && trackDlyPct  == 0 && inDlyPct  == 0 && dly.enabled)  processDelaySample(xL, xR);
  if (isInstReverbBusActive()) processReverbSample(xL, xR);
  if (instTapeBusPct > 0 && instTape.enabled) processTapeUnit(instTape, xL, xR, true);
  if (instCompBusPct > 0 && instComp.enabled) processCompUnit(instComp, xL, xR, true);
}

// ============================================================================
// Audio Input FX Engine (Line In + USB In: Drive -> Chor -> Dly -> Verb -> Tape -> Comp)
// ============================================================================

DMAMEM static AudioTrackFxParams inputFxCfg;
static DriveChanState inputDrivePool[4]; // [0..1] = Line In L/R, [2..3] = USB In L/R
static DriveCoeffs inputDriveCoeffs;
DMAMEM static int16_t inputWowPoolDma[2][TAPE_WOW_DELAY_LEN];
DMAMEM static int16_t inputFlutPoolDma[2][TAPE_FLUTTER_DELAY_LEN];
static int16_t inputWowPoolRam1[2][TAPE_WOW_DELAY_LEN];
static int16_t inputFlutPoolRam1[2][TAPE_FLUTTER_DELAY_LEN];
static int16_t inputFirPool[4][TAPE_FIR_TAPS];
static TapeChanState inputTapeChanPool[4];
static CompChanState inputCompChanPool[4];

static TapeUnit inputTape[2] = {
  { inputFirPool[0], inputFirPool[1], inputWowPoolDma[0],  inputWowPoolDma[1],  inputFlutPoolDma[0],  inputFlutPoolDma[1],  &inputTapeChanPool[0] },
  { inputFirPool[2], inputFirPool[3], inputWowPoolRam1[0], inputWowPoolRam1[1], inputFlutPoolRam1[0], inputFlutPoolRam1[1], &inputTapeChanPool[2] }
};

static CompUnit inputComp[2] = {
  { &inputCompChanPool[0], 2 },
  { &inputCompChanPool[2], 2 }
};

static int32_t usbInputTailSamples = 0;
static uint8_t lastInputInlineStrip = 0; // 0 = Line In, 1 = USB Audio In
static uint8_t inputOutVolStep = VOLUME_UNITY;
static float inputOutTargetGain = 1.0f;
static float inputOutCurGain[2] = {1.0f, 1.0f}; // [0] = Line In, [1] = USB Audio In

FLASHMEM void resetAudioInputFxState() {
  uint32_t irq = saveIrq();
  for (int i = 0; i < 4; i++) {
    inputDrivePool[i] = DriveChanState{};
  }
  usbInputTailSamples = 0;
  lastInputInlineStrip = 0;
  inputOutCurGain[0] = inputOutTargetGain;
  inputOutCurGain[1] = inputOutTargetGain;
  inChorPct = 0;
  inDlyPct = 0;
  inDfbPct = 0;
  inVerbPct = 0;
  restoreIrq(irq);
  resetTapeUnit(inputTape[0]);
  resetTapeUnit(inputTape[1]);
  resetCompUnit(inputComp[0]);
  resetCompUnit(inputComp[1]);
  syncSharedAuxTanksSlow();
  if (masterChorPct == 0 && instChorPct == 0 && trackChorPct == 0) resetChorusUnit(chor);
  if (masterDlyPct == 0  && instDlyPct == 0  && trackDlyPct == 0)  resetDelayUnit(dly);
  if (masterVerbPct == 0 && instVerbPct == 0 && trackVerbPct == 0) resetReverbUnit(verb);
}

FLASHMEM void setAudioInputOutGain(uint8_t volStep, float targetGain, bool snap) {
  uint32_t irq = saveIrq();
  inputOutVolStep = volStep;
  inputOutTargetGain = targetGain;
  if (snap) {
    inputOutCurGain[0] = targetGain;
    inputOutCurGain[1] = targetGain;
  }
  restoreIrq(irq);
}

FLASHMEM void syncAudioInputFxParams(const AudioTrackFxParams &fx) {
  AudioTrackFxParams clamped;
  clamped.drive         = (uint8_t)constrain((int)fx.drive, 0, 100);
  clamped.chorus        = (uint8_t)constrain((int)fx.chorus, 0, 100);
  clamped.delay         = (uint8_t)constrain((int)fx.delay, 0, 100);
  clamped.delayFeedback = (uint8_t)constrain((int)fx.delayFeedback, 0, 100);
  clamped.reverb        = (uint8_t)constrain((int)fx.reverb, 0, 100);
  clamped.tape          = (uint8_t)constrain((int)fx.tape, 0, 100);
  clamped.comp          = (uint8_t)constrain((int)fx.comp, 0, 100);

  uint32_t irq = saveIrq();
  inputFxCfg = clamped;
  restoreIrq(irq);

  configureDriveCoeffs(inputDriveCoeffs, clamped.drive);
  if (clamped.drive == 0) {
    uint32_t irq2 = saveIrq();
    for (int i = 0; i < 4; i++) {
      inputDrivePool[i] = DriveChanState{};
    }
    restoreIrq(irq2);
  }
  configureTapeUnit(inputTape[0], clamped.tape);
  configureTapeUnit(inputTape[1], clamped.tape);
  configureCompUnit(inputComp[0], clamped.comp);
  configureCompUnit(inputComp[1], clamped.comp);
}

FLASHMEM void updateAudioInputFxTargets(bool inputBusEnabled) {
  const AudioTrackFxParams &fx = inputFxCfg;
  uint8_t wantChor = inputBusEnabled ? fx.chorus : 0;
  uint8_t wantDly  = inputBusEnabled ? fx.delay  : 0;
  uint8_t wantDfb  = (inputBusEnabled && fx.delay > 0) ? fx.delayFeedback : 0;
  uint8_t wantVerb = inputBusEnabled ? fx.reverb : 0;
  if (wantChor != inChorPct || wantDly != inDlyPct || wantDfb != inDfbPct || wantVerb != inVerbPct) {
    inChorPct = wantChor;
    inDlyPct  = wantDly;
    inDfbPct  = wantDfb;
    inVerbPct = wantVerb;
    syncSharedAuxTanksSlow();
  }
}

static inline int16_t floatToClippedInt16(float x) {
  float s = x * 32768.0f;
  if (s >= -30000.0f && s <= 30000.0f) {
    return (int16_t)((s >= 0.0f) ? (s + 0.5f) : (s - 0.5f));
  }
  float y = softKneeClip(s, 30000.0f, 32766.0f);
  return (int16_t)((y >= 0.0f) ? (y + 0.5f) : (y - 0.5f));
}

FLASHMEM void processAudioInputFxBlock(AudioInputFxBlockCtx &inCtx,
                                       bool inputBusEnabled,
                                       bool lineActive, int16_t *lineBufL, int16_t *lineBufR,
                                       bool usbStreamActive, bool usbWanted, uint32_t *usbFrames,
                                       bool preferUsbInline) {
  inCtx.usbTailActive = false;
  if (!inputBusEnabled) {
    usbInputTailSamples = 0;
    inputOutCurGain[0] = inputOutTargetGain;
    inputOutCurGain[1] = inputOutTargetGain;
    return;
  }

  const AudioTrackFxParams &fx = inputFxCfg;
  const float tgtGain = inputOutTargetGain;
  const bool gainAtUnity = (inputOutVolStep == VOLUME_UNITY) &&
                           (fabsf(inputOutCurGain[0] - 1.0f) < 1e-4f) &&
                           (fabsf(inputOutCurGain[1] - 1.0f) < 1e-4f);
  const bool hasInsert = (fx.drive > 0 || fx.tape > 0 || fx.comp > 0);
  const bool hasOutGain = !gainAtUnity;
  const bool anyInlineTank = inCtx.inlineChor || inCtx.inlineDly || inCtx.inlineVerb;
  if (!hasInsert && !hasOutGain && !anyInlineTank && usbInputTailSamples <= 0) {
    inputOutCurGain[0] = 1.0f;
    inputOutCurGain[1] = 1.0f;
    return;
  }

  uint32_t usbOr = 0;
  if (usbStreamActive && usbFrames) {
    for (int i = 0; i < (int)AUDIO_BLOCK_SAMPLES; i++) {
      usbOr |= usbFrames[i];
    }
  }
  const bool usbHasSignal = (usbOr != 0);

  int16_t lineOr = 0;
  if (lineActive && lineBufL && lineBufR) {
    for (int i = 0; i < (int)AUDIO_BLOCK_SAMPLES; i++) {
      lineOr |= lineBufL[i] | lineBufR[i];
    }
  }
  const bool lineHasSignal = (lineOr != 0);

  uint8_t inlineStrip = lastInputInlineStrip;
  if (preferUsbInline && usbWanted) {
    inlineStrip = 1;
  } else if (!lineActive && usbWanted) {
    inlineStrip = 1;
  } else if (lineActive && !usbWanted) {
    inlineStrip = 0;
  } else if (usbHasSignal && !lineHasSignal) {
    inlineStrip = 1;
  } else if (lineHasSignal && !usbHasSignal) {
    inlineStrip = 0;
  }
  lastInputInlineStrip = inlineStrip;

  if (usbWanted && usbHasSignal &&
      (fx.tape > 0 || (inlineStrip == 1 && (inCtx.inlineChor || inCtx.inlineDly || inCtx.inlineVerb)))) {
    int32_t dlyT = (inlineStrip == 1 && inCtx.inlineDly)
                       ? (int32_t)(SAMPLE_RATE_HZ * (0.6f + 0.044f * (float)fx.delayFeedback))
                       : 0;
    int32_t revT = (inlineStrip == 1 && inCtx.inlineVerb)
                       ? (int32_t)(SAMPLE_RATE_HZ * (0.8f + 0.25f * (float)fx.reverb))
                       : 0;
    int32_t minT = (int32_t)CHORUS_BUF_LEN;
    int32_t reqT = max(minT, max(dlyT, revT));
    if (reqT > usbInputTailSamples) usbInputTailSamples = reqT;
  } else if (usbInputTailSamples > 0) {
    if (!usbWanted || (fx.delay == 0 && fx.reverb == 0 && fx.chorus == 0 && fx.tape == 0)) {
      usbInputTailSamples = 0;
    } else {
      usbInputTailSamples = max((int32_t)0, usbInputTailSamples - (int32_t)AUDIO_BLOCK_SAMPLES);
    }
  }

  const bool doLine = lineActive && lineBufL && lineBufR &&
                      (hasInsert || hasOutGain || (anyInlineTank && inlineStrip == 0));
  const bool usbHasTail = usbWanted && (usbInputTailSamples > 0 || (inlineStrip == 1 && anyInlineTank));
  const bool doUsb = usbWanted && usbFrames && (usbStreamActive || usbHasTail) &&
                     (hasInsert || hasOutGain || (anyInlineTank && inlineStrip == 1));
  if (!doLine) inputOutCurGain[0] = tgtGain;
  if (!doUsb)  inputOutCurGain[1] = tgtGain;

  if (fx.drive > 0 && inputDriveCoeffs.cachedPct != fx.drive) {
    configureDriveCoeffs(inputDriveCoeffs, fx.drive);
  }

  constexpr float IN_GAIN_GLIDE = 1.0f - __builtin_expf(-1000.0f / (5.0f * (float)SAMPLE_RATE_HZ));

  if (doLine) {
    const bool runTank = anyInlineTank && (inlineStrip == 0);
    float g = inputOutCurGain[0];
    const bool applyGain = (g != 1.0f || tgtGain != 1.0f);
    for (int i = 0; i < (int)AUDIO_BLOCK_SAMPLES; i++) {
      float xL = (float)lineBufL[i] * (1.0f / 32768.0f);
      float xR = (float)lineBufR[i] * (1.0f / 32768.0f);
      if (fx.drive > 0) {
        xL = stepDriveChannel(inputDrivePool[0], inputDriveCoeffs, xL);
        xR = stepDriveChannel(inputDrivePool[1], inputDriveCoeffs, xR);
      }
      if (runTank) {
        if (inCtx.inlineChor) processChorusSample(xL, xR);
        if (inCtx.inlineDly)  processDelaySample(xL, xR);
        if (inCtx.inlineVerb) processReverbSample(xL, xR);
      }
      if (fx.tape > 0 && inputTape[0].enabled) {
        processTapeUnit(inputTape[0], xL, xR, true);
      }
      if (fx.comp > 0 && inputComp[0].enabled) {
        processCompUnit(inputComp[0], xL, xR, true);
      }
      if (applyGain) {
        g += (tgtGain - g) * IN_GAIN_GLIDE;
        xL *= g;
        xR *= g;
      }
      lineBufL[i] = floatToClippedInt16(xL);
      lineBufR[i] = floatToClippedInt16(xR);
    }
    if (fabsf(tgtGain - g) < 1e-4f) g = tgtGain;
    inputOutCurGain[0] = g;
  }

  if (doUsb) {
    const bool runTank = anyInlineTank && (inlineStrip == 1);
    float g = inputOutCurGain[1];
    const bool applyGain = (g != 1.0f || tgtGain != 1.0f);
    for (int i = 0; i < (int)AUDIO_BLOCK_SAMPLES; i++) {
      int16_t uL0 = usbStreamActive ? (int16_t)(usbFrames[i] & 0xFFFF) : 0;
      int16_t uR0 = usbStreamActive ? (int16_t)(usbFrames[i] >> 16) : 0;
      float xL = (float)uL0 * (1.0f / 32768.0f);
      float xR = (float)uR0 * (1.0f / 32768.0f);
      if (fx.drive > 0) {
        xL = stepDriveChannel(inputDrivePool[2], inputDriveCoeffs, xL);
        xR = stepDriveChannel(inputDrivePool[3], inputDriveCoeffs, xR);
      }
      if (runTank) {
        if (inCtx.inlineChor) processChorusSample(xL, xR);
        if (inCtx.inlineDly)  processDelaySample(xL, xR);
        if (inCtx.inlineVerb) processReverbSample(xL, xR);
      }
      if (fx.tape > 0 && inputTape[1].enabled) {
        processTapeUnit(inputTape[1], xL, xR, true);
      }
      if (fx.comp > 0 && inputComp[1].enabled) {
        processCompUnit(inputComp[1], xL, xR, true);
      }
      if (applyGain) {
        g += (tgtGain - g) * IN_GAIN_GLIDE;
        xL *= g;
        xR *= g;
      }
      int16_t outUL = floatToClippedInt16(xL);
      int16_t outUR = floatToClippedInt16(xR);
      usbFrames[i] = (uint16_t)outUL | ((uint32_t)(uint16_t)outUR << 16);
    }
    if (fabsf(tgtGain - g) < 1e-4f) g = tgtGain;
    inputOutCurGain[1] = g;
    inCtx.usbTailActive = usbHasTail;
  }
}

// ============================================================================
// Per-Audio-Track FX Engine (Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp)
// ============================================================================

DMAMEM static AudioTrackFxParams trackFxCfg[NUM_AUDIO_TRACKS];
static int32_t trackChorTailSamples = 0;
static int32_t trackDlyTailSamples = 0;
static int32_t trackVerbTailSamples = 0;
static uint8_t tailHoldChorPct = 0;
static uint8_t tailHoldDlyPct = 0;
static uint8_t tailHoldDfbPct = 0;
static uint8_t tailHoldVerbPct = 0;

FLASHMEM void resetAudioTrackFxState(int trackIdx) {
  if (trackIdx < 0 || trackIdx >= NUM_AUDIO_TRACKS) return;
  int baseCh = trackDriveBaseChan(trackIdx);
  uint32_t irq = saveIrq();
  trackDrivePool[baseCh] = DriveChanState{};
  if (!isAudioTrackMono(trackIdx)) {
    trackDrivePool[baseCh + 1] = DriveChanState{};
  }
  restoreIrq(irq);
  resetTapeUnit(trackTape[trackIdx]);
  resetCompUnit(trackComp[trackIdx]);
}

FLASHMEM void resetAllAudioTrackFxState() {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    resetAudioTrackFxState(t);
  }
  uint32_t irq = saveIrq();
  trackChorTailSamples = 0;
  trackDlyTailSamples = 0;
  trackVerbTailSamples = 0;
  tailHoldChorPct = 0;
  tailHoldDlyPct = 0;
  tailHoldDfbPct = 0;
  tailHoldVerbPct = 0;
  trackChorPct = 0;
  trackDlyPct = 0;
  trackDfbPct = 0;
  trackVerbPct = 0;
  masterChorAuxBoostL = masterChorAuxBoostR = 0.0f;
  masterDlyAuxBoostL = masterDlyAuxBoostR = 0.0f;
  masterVerbAuxBoostL = masterVerbAuxBoostR = 0.0f;
  restoreIrq(irq);
  syncSharedAuxTanksSlow();
  if (masterChorPct == 0 && instChorPct == 0 && inChorPct == 0) resetChorusUnit(chor);
  if (masterDlyPct == 0  && instDlyPct == 0  && inDlyPct == 0)  resetDelayUnit(dly);
  if (masterVerbPct == 0 && instVerbPct == 0 && inVerbPct == 0) resetReverbUnit(verb);
}

FLASHMEM void syncAudioTrackFxParams(int trackIdx, const AudioTrackFxParams &fx) {
  if (trackIdx < 0 || trackIdx >= NUM_AUDIO_TRACKS) return;
  AudioTrackFxParams clamped;
  clamped.drive         = (uint8_t)constrain((int)fx.drive, 0, 100);
  clamped.chorus        = (uint8_t)constrain((int)fx.chorus, 0, 100);
  clamped.delay         = (uint8_t)constrain((int)fx.delay, 0, 100);
  clamped.delayFeedback = (uint8_t)constrain((int)fx.delayFeedback, 0, 100);
  clamped.reverb        = (uint8_t)constrain((int)fx.reverb, 0, 100);
  clamped.tape          = (uint8_t)constrain((int)fx.tape, 0, 100);
  clamped.comp          = (uint8_t)constrain((int)fx.comp, 0, 100);

  uint32_t irq = saveIrq();
  trackFxCfg[trackIdx] = clamped;
  restoreIrq(irq);

  configureDriveCoeffs(trackDriveCoeffs[trackIdx], clamped.drive);
  if (clamped.drive == 0) {
    int baseCh = trackDriveBaseChan(trackIdx);
    uint32_t irq2 = saveIrq();
    trackDrivePool[baseCh] = DriveChanState{};
    if (!isAudioTrackMono(trackIdx)) {
      trackDrivePool[baseCh + 1] = DriveChanState{};
    }
    restoreIrq(irq2);
  }
  configureTapeUnit(trackTape[trackIdx], clamped.tape);
  configureCompUnit(trackComp[trackIdx], clamped.comp);
}

FLASHMEM bool anyAudioTrackDelayOrReverbActive() {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (trackFxCfg[t].delay > 0 || trackFxCfg[t].reverb > 0) return true;
  }
  return trackDlyTailSamples > 0 || trackVerbTailSamples > 0 ||
         (verb.enabled && verbOwner == VERB_OWNER_TRACK);
}

FLASHMEM __attribute__((noinline)) static void updateTrackAuxTankTargets(uint8_t wantChor, uint8_t wantDly,
                                                                         uint8_t wantDfb, uint8_t wantVerb) {
  trackChorPct = wantChor;
  trackDlyPct  = wantDly;
  trackDfbPct  = wantDfb;
  trackVerbPct = wantVerb;
  syncSharedAuxTanksSlow();
}

FLASHMEM void prepareAudioTrackFxBlock(AudioTrackFxBlockCtx &ctx, uint8_t processTrackMask,
                                       uint8_t audibleTrackMask, bool instAuxAvailable,
                                       AudioInputFxBlockCtx *inCtx) {
  ctx = AudioTrackFxBlockCtx{};
  if (inCtx) {
    *inCtx = AudioInputFxBlockCtx{};
    inCtx->dryTrim = 1.0f;
  }

  uint8_t maxChor = 0, maxDly = 0, maxDfb = 0, maxVerb = 0;
  bool anyCfgChor = false, anyCfgDly = false, anyCfgVerb = false;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    ctx.dryTrim[t] = 1.0f;
    const AudioTrackFxParams &fx = trackFxCfg[t];
    if (fx.chorus > 0) anyCfgChor = true;
    if (fx.delay > 0)  anyCfgDly  = true;
    if (fx.reverb > 0) anyCfgVerb = true;
    const uint8_t bit = (uint8_t)(1u << t);
    if ((processTrackMask & bit) == 0) continue;
    if (fx.drive > 0 || fx.tape > 0 || fx.comp > 0) {
      ctx.insertMask |= bit;
    }
    if ((audibleTrackMask & bit) != 0 && (fx.chorus > 0 || fx.delay > 0 || fx.reverb > 0)) {
      ctx.auxSendMask |= bit;
      if (fx.chorus > maxChor) maxChor = fx.chorus;
      if (fx.delay > maxDly)   maxDly = fx.delay;
      if (fx.delay > 0 && fx.delayFeedback > maxDfb) maxDfb = fx.delayFeedback;
      if (fx.reverb > maxVerb) maxVerb = fx.reverb;
    }
  }

  // Refresh Aux tail timers when audible tracks actively feed Chor / Dly / Verb
  // (if the user zeroed all tracks' FX amount, release the tail hold so it fades out cleanly)
  if (!anyCfgChor) trackChorTailSamples = 0;
  if (!anyCfgDly)  trackDlyTailSamples  = 0;
  if (!anyCfgVerb) trackVerbTailSamples = 0;

  if (maxChor > 0) {
    tailHoldChorPct = maxChor;
    trackChorTailSamples = max(trackChorTailSamples, (int32_t)CHORUS_BUF_LEN);
  } else if (trackChorTailSamples > 0) {
    trackChorTailSamples = max((int32_t)0, trackChorTailSamples - (int32_t)AUDIO_BLOCK_SAMPLES);
    if (trackChorTailSamples > 0) maxChor = tailHoldChorPct;
  }

  if (maxDly > 0) {
    tailHoldDlyPct = maxDly;
    tailHoldDfbPct = maxDfb;
    int32_t reqTail = (int32_t)(SAMPLE_RATE_HZ * (0.6f + 0.044f * (float)maxDfb));
    if (reqTail > trackDlyTailSamples) trackDlyTailSamples = reqTail;
  } else if (trackDlyTailSamples > 0) {
    trackDlyTailSamples = max((int32_t)0, trackDlyTailSamples - (int32_t)AUDIO_BLOCK_SAMPLES);
    if (trackDlyTailSamples > 0) {
      maxDly = tailHoldDlyPct;
      maxDfb = tailHoldDfbPct;
    }
  }

  if (maxVerb > 0) {
    tailHoldVerbPct = maxVerb;
    int32_t reqTail = (int32_t)(SAMPLE_RATE_HZ * (0.8f + 0.25f * (float)maxVerb));
    if (reqTail > trackVerbTailSamples) trackVerbTailSamples = reqTail;
  } else if (trackVerbTailSamples > 0) {
    trackVerbTailSamples = max((int32_t)0, trackVerbTailSamples - (int32_t)AUDIO_BLOCK_SAMPLES);
    if (trackVerbTailSamples > 0) maxVerb = tailHoldVerbPct;
  }

  if (maxChor != trackChorPct || maxDly != trackDlyPct || maxDfb != trackDfbPct || maxVerb != trackVerbPct) {
    updateTrackAuxTankTargets(maxChor, maxDly, maxDfb, maxVerb);
  }

  bool inSharedChor = false, inSharedDly = false, inSharedVerb = false;
  if (inCtx) {
    inCtx->inlineChor = (inChorPct > 0) && (masterChorPct == 0) && (maxChor == 0) && (instChorPct == 0) && chor.enabled;
    inCtx->inlineDly  = (inDlyPct  > 0) && (masterDlyPct  == 0) && (maxDly  == 0) && (instDlyPct  == 0) && dly.enabled;
    inCtx->inlineVerb = isInputReverbInlineActive() && (maxVerb == 0);
    inSharedChor = (inChorPct > 0) && !inCtx->inlineChor;
    inSharedDly  = (inDlyPct  > 0) && !inCtx->inlineDly;
    inSharedVerb = (inVerbPct > 0) && !inCtx->inlineVerb;
  }

  const bool verbFadeTrack = verb.enabled && masterVerbPct == 0 && instVerbPct == 0 && inVerbPct == 0 &&
                             verbOwner == VERB_OWNER_TRACK;
  const bool auxChor = ((maxChor > 0) || (inSharedChor && masterChorPct == 0)) && (masterChorPct == 0) && chor.enabled;
  const bool auxDly  = ((maxDly  > 0) || (inSharedDly  && masterDlyPct  == 0)) && (masterDlyPct  == 0) && dly.enabled;
  const bool auxVerb = (((maxVerb > 0) || (inSharedVerb && masterVerbPct == 0)) && (masterVerbPct == 0) && verb.enabled) || verbFadeTrack;

  if (maxChor == 0 && maxDly == 0 && maxVerb == 0 && !inSharedChor && !inSharedDly && !inSharedVerb) {
    ctx.runVerbAux = verbFadeTrack;
    return;
  }

  ctx.runChorAux = auxChor;
  ctx.runDlyAux  = auxDly;
  ctx.runVerbAux = auxVerb;

  const uint8_t effChor = max(max(masterChorPct, instChorPct), max(maxChor, inChorPct));
  const uint8_t effDly  = max(max(masterDlyPct,  instDlyPct),  max(maxDly,  inDlyPct));
  const uint8_t effVerb = max(max(masterVerbPct, instVerbPct), max(maxVerb, inVerbPct));
  const float effChorMix = (effChor > 0) ? ((float)effChor * 0.01f) : 1.0f;
  const float effDlyMix  = (effDly  > 0) ? ((float)effDly  * 0.01f) : 1.0f;
  const float effVerbMix = (effVerb > 0) ? computeReverbMix(effVerb) : 1.0f;

  float sumC = 0.0f, sumD = 0.0f;
  float sumCD = 0.0f, sumCV = 0.0f, sumDV = 0.0f;

  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if ((ctx.auxSendMask & (uint8_t)(1u << t)) == 0) continue;
    const AudioTrackFxParams &fx = trackFxCfg[t];
    float c = (float)fx.chorus * 0.01f;
    float d = (float)fx.delay  * 0.01f;
    float v = computeReverbMix(fx.reverb);

    // Dry trim only attenuates for Aux stages not already attenuated by Master FX
    float dryG = 1.0f;
    if (ctx.runChorAux && c > 0.0f) dryG *= (1.0f - 0.25f * c);
    if (ctx.runDlyAux  && d > 0.0f) dryG *= (1.0f - 0.12f * d);
    if (ctx.runVerbAux && v > 0.0f) dryG *= (1.0f - 0.18f * v);
    ctx.dryTrim[t] = dryG;

    float cEff = (masterChorPct == 0) ? c : max(0.0f, c - (float)masterChorPct * 0.01f);
    float dEff = (masterDlyPct  == 0) ? d : max(0.0f, d - (float)masterDlyPct  * 0.01f);
    float vEff = (masterVerbPct == 0) ? v : max(0.0f, v - computeReverbMix(masterVerbPct));

    if (cEff > 0.0f && effChor > 0) {
      ctx.chorSend[t] = cEff / effChorMix;
      sumC += cEff;
      sumCD += cEff * dEff;
      sumCV += cEff * (1.0f - 0.12f * dEff) * vEff;
    }
    if (dEff > 0.0f && effDly > 0) {
      ctx.dlySend[t] = (dEff * (1.0f - 0.25f * cEff)) / effDlyMix;
      sumD += dEff;
      sumDV += dEff * vEff;
    }
    if (vEff > 0.0f && effVerb > 0) {
      ctx.verbSend[t] = (vEff * (1.0f - 0.25f * cEff) * (1.0f - 0.12f * dEff)) / effVerbMix;
    }
  }

  // Merge any deferred Instrument FX sends when Track Aux owns the shared tank
  if (instAuxAvailable) {
    float ic = (ctx.runChorAux && instChorPct > 0) ? ((float)instChorPct * 0.01f) : 0.0f;
    float id = (ctx.runDlyAux  && instDlyPct  > 0) ? ((float)instDlyPct  * 0.01f) : 0.0f;
    float iv = (ctx.runVerbAux && instVerbPct > 0) ? computeReverbMix(instVerbPct) : 0.0f;
    if (ic > 0.0f || id > 0.0f || iv > 0.0f) {
      ctx.hasInstAux = true;
      if (ic > 0.0f && effChor > 0) {
        ctx.instChorSend = ic / effChorMix;
        sumC += ic;
        sumCD += ic * id;
        sumCV += ic * (1.0f - 0.12f * id) * iv;
      }
      if (id > 0.0f && effDly > 0) {
        ctx.instDlySend = (id * (1.0f - 0.25f * ic)) / effDlyMix;
        sumD += id;
        sumDV += id * iv;
      }
      if (iv > 0.0f && effVerb > 0) {
        ctx.instVerbSend = (iv * (1.0f - 0.25f * ic) * (1.0f - 0.12f * id)) / effVerbMix;
      }
    }
  }

  // Merge any shared Audio Input FX sends when Track/Inst/Master also uses Chorus/Delay/Reverb
  if (inCtx && (inSharedChor || inSharedDly || inSharedVerb)) {
    float c = inSharedChor ? ((float)inChorPct * 0.01f) : 0.0f;
    float d = inSharedDly  ? ((float)inDlyPct  * 0.01f) : 0.0f;
    float v = inSharedVerb ? computeReverbMix(inVerbPct) : 0.0f;
    float dryG = 1.0f;
    if (ctx.runChorAux && c > 0.0f) dryG *= (1.0f - 0.25f * c);
    if (ctx.runDlyAux  && d > 0.0f) dryG *= (1.0f - 0.12f * d);
    if (ctx.runVerbAux && v > 0.0f) dryG *= (1.0f - 0.18f * v);
    inCtx->dryTrim = dryG;

    float cEff = (masterChorPct == 0) ? c : max(0.0f, c - (float)masterChorPct * 0.01f);
    float dEff = (masterDlyPct  == 0) ? d : max(0.0f, d - (float)masterDlyPct  * 0.01f);
    float vEff = (masterVerbPct == 0) ? v : max(0.0f, v - computeReverbMix(masterVerbPct));
    if (cEff > 0.0f || dEff > 0.0f || vEff > 0.0f) {
      inCtx->hasAuxSend = true;
      if (cEff > 0.0f && effChor > 0) {
        inCtx->chorSend = cEff / effChorMix;
        sumC += cEff;
        sumCD += cEff * dEff;
        sumCV += cEff * (1.0f - 0.12f * dEff) * vEff;
      }
      if (dEff > 0.0f && effDly > 0) {
        inCtx->dlySend = (dEff * (1.0f - 0.25f * cEff)) / effDlyMix;
        sumD += dEff;
        sumDV += dEff * vEff;
      }
      if (vEff > 0.0f && effVerb > 0) {
        inCtx->verbSend = (vEff * (1.0f - 0.25f * cEff) * (1.0f - 0.12f * dEff)) / effVerbMix;
      }
    }
  }

  if (sumC > 1e-5f && effDly > 0) {
    ctx.chorToDly = (sumCD / sumC) / effDlyMix;
  }
  if (sumC > 1e-5f && effVerb > 0) {
    ctx.chorToVerb = (sumCV / sumC) / effVerbMix;
  }
  if (sumD > 1e-5f && effVerb > 0) {
    ctx.dlyToVerb = (sumDV / sumD) / effVerbMix;
  }
}

void processAudioTrackInsertSample(int trackIdx, bool stereo, float &sl, float &sr) {
  if (trackIdx < 0 || trackIdx >= NUM_AUDIO_TRACKS) return;
  const AudioTrackFxParams &fx = trackFxCfg[trackIdx];
  float xL = sl * (1.0f / 32768.0f);
  float xR = sr * (1.0f / 32768.0f);

  if (fx.drive > 0) {
    DriveCoeffs &c = trackDriveCoeffs[trackIdx];
    if (c.cachedPct != fx.drive) {
      configureDriveCoeffs(c, fx.drive);
    }
    int baseCh = trackDriveBaseChan(trackIdx);
    xL = stepDriveChannel(trackDrivePool[baseCh], c, xL);
    xR = stereo ? stepDriveChannel(trackDrivePool[baseCh + 1], c, xR) : xL;
  }
  if (fx.tape > 0 && trackTape[trackIdx].enabled) {
    processTapeUnit(trackTape[trackIdx], xL, xR, stereo);
  }
  if (fx.comp > 0 && trackComp[trackIdx].enabled) {
    processCompUnit(trackComp[trackIdx], xL, xR, stereo);
  }

  sl = xL * 32768.0f;
  sr = xR * 32768.0f;
}

void processAudioTrackAuxSample(const AudioTrackFxBlockCtx &ctx, uint32_t instPackedSample,
                                float chorInL, float chorInR,
                                float dlyInL, float dlyInR,
                                float verbInL, float verbInR,
                                float &wetOutL, float &wetOutR) {
  if (ctx.hasInstAux) {
    constexpr float INST_BUS_SCALE = (2.0f * 0.70794578f / 32768.0f); // unpacks +6 dB >>1 aux bus into -3 dB mix scale
    float iL = (float)(int16_t)(instPackedSample & 0xFFFF) * INST_BUS_SCALE;
    float iR = (float)(int16_t)(instPackedSample >> 16) * INST_BUS_SCALE;
    chorInL += ctx.instChorSend * iL;
    chorInR += ctx.instChorSend * iR;
    dlyInL  += ctx.instDlySend  * iL;
    dlyInR  += ctx.instDlySend  * iR;
    verbInL += ctx.instVerbSend * iL;
    verbInR += ctx.instVerbSend * iR;
  }

  float wL = 0.0f, wR = 0.0f;
  float chorWetL = 0.0f, chorWetR = 0.0f;
  if (ctx.runChorAux) {
    stepChorusTank(chor, chorInL, chorInR, chorWetL, chorWetR);
    wL += chorWetL;
    wR += chorWetR;
    dlyInL  += ctx.chorToDly  * chorWetL;
    dlyInR  += ctx.chorToDly  * chorWetR;
    verbInL += ctx.chorToVerb * chorWetL;
    verbInR += ctx.chorToVerb * chorWetR;
  } else {
    masterChorAuxBoostL = chorInL;
    masterChorAuxBoostR = chorInR;
  }

  float dlyWetL = 0.0f, dlyWetR = 0.0f;
  if (ctx.runDlyAux) {
    stepDelayTank(dly, dlyInL, dlyInR, dlyWetL, dlyWetR);
    wL += dlyWetL;
    wR += dlyWetR;
    verbInL += ctx.dlyToVerb * dlyWetL;
    verbInR += ctx.dlyToVerb * dlyWetR;
  } else {
    masterDlyAuxBoostL = dlyInL;
    masterDlyAuxBoostR = dlyInR;
  }

  if (ctx.runVerbAux) {
    float verbWetL = 0.0f, verbWetR = 0.0f;
    stepReverbTank(verb, verbInL, verbInR, verbWetL, verbWetR);
    wL += verbWetL;
    wR += verbWetR;
  } else {
    masterVerbAuxBoostL = verbInL;
    masterVerbAuxBoostR = verbInR;
  }

  wetOutL = wL * 32768.0f;
  wetOutR = wR * 32768.0f;
}

void processMasterFx(float &mixL, float &mixR) {
  const bool doChor = (masterChorPct > 0) && chor.enabled;
  const bool doDlay = (masterDlyPct  > 0) && dly.enabled;
  const bool doVerb = isMasterReverbBusActive();
  const bool doTape = masterTape.enabled;
  const bool doComp = masterComp.enabled;
  if (!doChor && !doDlay && !doVerb && !doTape && !doComp) {
    masterChorAuxBoostL = masterChorAuxBoostR = 0.0f;
    masterDlyAuxBoostL = masterDlyAuxBoostR = 0.0f;
    masterVerbAuxBoostL = masterVerbAuxBoostR = 0.0f;
    return;
  }

  // Normalize 16-bit full-scale mix bus to [-1.0f, +1.0f]
  float xL = mixL * (1.0f / 32768.0f);
  float xR = mixR * (1.0f / 32768.0f);

  // Signal chain: Chorus -> Delay -> Reverb -> Tape -> Comp
  if (doChor) processChorusSample(xL, xR);
  if (doDlay) processDelaySample(xL, xR);
  if (doVerb) processReverbSample(xL, xR);
  if (doTape) processTapeSample(xL, xR);
  if (doComp) processCompSample(xL, xR);

  masterChorAuxBoostL = masterChorAuxBoostR = 0.0f;
  masterDlyAuxBoostL = masterDlyAuxBoostR = 0.0f;
  masterVerbAuxBoostL = masterVerbAuxBoostR = 0.0f;

  mixL = xL * 32768.0f;
  mixR = xR * 32768.0f;
}

} // namespace looper

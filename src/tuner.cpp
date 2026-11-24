// tuner.cpp — Chromatic Tuner DSP (2x Decimated Two-Band YIN Pitch Detector)
#include <Arduino.h>
#include <AudioStream.h>
#include "tuner.h"
#include "config.h"
#include "instruments.h"
#include "storage.h"
#include <math.h>
#include <string.h>

namespace looper {

// 4 KB uncached RAM1 ring buffer (2,048 samples @ 22.05 kHz = ~92.9 ms window)
static constexpr uint32_t TUNER_RING_SAMPLES = 2048;
static constexpr uint32_t TUNER_RING_MASK    = TUNER_RING_SAMPLES - 1u;
static constexpr int TUNER_SNAP_LEN          = 1920; // ~87.1 ms snapshot

// Two-band YIN search ranges (Fs_dec = 22,050 Hz, Fs_low = 11,025 Hz):
// - High/Mid band: lags 8..62 @ 22,050 Hz (~355 Hz .. 2,756 Hz, up to C7 = 2,093 Hz+)
// - Low/Mid band:  lags 30..410 @ 11,025 Hz (~26.9 Hz .. 367 Hz, down to A0 = 27.5 Hz)
static constexpr int YIN_HI_TAU_MIN = 8;
static constexpr int YIN_HI_TAU_MAX = 62;
static constexpr int YIN_HI_WIN     = 512;

static constexpr int YIN_LO_TAU_MIN = 30;
static constexpr int YIN_LO_TAU_MAX = 410;
static constexpr int YIN_LO_WIN     = 480;

static constexpr int YIN_REFINE_WIN = 1024;

// Input gate (~ -58.5 dBFS RMS) with brief holdover to bridge vibrato zero-crossings
static constexpr float TUNER_GATE_RMS      = 0.0012f;
static constexpr float TUNER_GATE_PEAK     = 0.0025f;
static constexpr uint8_t TUNER_HOLD_TICKS  = 4; // 4 x 50 ms = 200 ms holdover

volatile TunerSource tunerActiveSource = TunerSource::Off;
volatile bool tunerMuteActive          = false;
volatile bool tunerInstPushedThisBlock = false;

static int16_t tunerRing[TUNER_RING_SAMPLES];
static volatile uint32_t tunerWriteHead = 0;
static volatile uint16_t tunerBlockPeak = 0;
static int16_t tunerPrevM1              = 0;

static uint16_t songTunerRefTenths = DEFAULT_TUNER_REF_TENTHS;

// Main-loop detector & smoothing state
static uint32_t lastDetectMs = 0;
static uint32_t lastUiTickMs = 0;
static float rawHzHist[3]    = {};
static uint8_t rawHzCount    = 0;
static float smoothHz        = 0.0f;
static float smoothCents     = 0.0f;
static int8_t lockedMidiNote = -1;
static bool pitchLocked      = false;
static uint8_t holdTicksLeft = 0;
static float meterEnvLin     = 0.0f;
static bool meterClip        = false;
static float strobePhase     = 0.0f; // [0.0, 1.0) normalized band cycle

PROGMEM static const char *const TUNER_SRC_LABELS[TUNER_SELECTABLE_SOURCES + 1] = {
  "OFF",
  "LINE IN",
  "USB IN",
  "I01", "I02", "I03", "I04", "I05", "I06", "I07", "I08",
  "A01", "A02", "A03", "A04", "A05", "A06", "A07", "A08"
};

static inline int16_t clampSample16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

static inline uint16_t absSample16(int16_t s) {
  int32_t v = (int32_t)s;
  if (v < 0) v = -v;
  return (uint16_t)(v > 32767 ? 32767 : v);
}

void tunerPushStereo16(const int16_t *inL, const int16_t *inR, int numFrames) {
  uint32_t w = tunerWriteHead;
  int16_t prev = tunerPrevM1;
  uint16_t pk = tunerBlockPeak;
  const int pairs = numFrames >> 1;
  for (int k = 0; k < pairs; k++) {
    const int i0 = k << 1;
    const int16_t m0 = (int16_t)(((int32_t)inL[i0] + (int32_t)inR[i0]) >> 1);
    const int16_t m1 = (int16_t)(((int32_t)inL[i0 + 1] + (int32_t)inR[i0 + 1]) >> 1);
    const uint16_t a0 = absSample16(m0);
    const uint16_t a1 = absSample16(m1);
    if (a0 > pk) pk = a0;
    if (a1 > pk) pk = a1;
    const int16_t dec = (int16_t)(((int32_t)prev + ((int32_t)m0 << 1) + (int32_t)m1) >> 2);
    prev = m1;
    tunerRing[(w + (uint32_t)k) & TUNER_RING_MASK] = dec;
  }
  __asm__ volatile("" ::: "memory");
  tunerWriteHead = w + (uint32_t)pairs;
  tunerPrevM1 = prev;
  tunerBlockPeak = pk;
}

void tunerPushPackedStereo16(const uint32_t *packedLR, int numFrames) {
  uint32_t w = tunerWriteHead;
  int16_t prev = tunerPrevM1;
  uint16_t pk = tunerBlockPeak;
  const int pairs = numFrames >> 1;
  for (int k = 0; k < pairs; k++) {
    const int i0 = k << 1;
    const uint32_t p0 = packedLR[i0];
    const uint32_t p1 = packedLR[i0 + 1];
    const int16_t m0 = (int16_t)(((int32_t)(int16_t)(p0 & 0xFFFFu) + (int32_t)(int16_t)(p0 >> 16)) >> 1);
    const int16_t m1 = (int16_t)(((int32_t)(int16_t)(p1 & 0xFFFFu) + (int32_t)(int16_t)(p1 >> 16)) >> 1);
    const uint16_t a0 = absSample16(m0);
    const uint16_t a1 = absSample16(m1);
    if (a0 > pk) pk = a0;
    if (a1 > pk) pk = a1;
    const int16_t dec = (int16_t)(((int32_t)prev + ((int32_t)m0 << 1) + (int32_t)m1) >> 2);
    prev = m1;
    tunerRing[(w + (uint32_t)k) & TUNER_RING_MASK] = dec;
  }
  __asm__ volatile("" ::: "memory");
  tunerWriteHead = w + (uint32_t)pairs;
  tunerPrevM1 = prev;
  tunerBlockPeak = pk;
}

void tunerPushStereo32(const int32_t *inL, const int32_t *inR, int numFrames) {
  uint32_t w = tunerWriteHead;
  int16_t prev = tunerPrevM1;
  uint16_t pk = tunerBlockPeak;
  const int pairs = numFrames >> 1;
  for (int k = 0; k < pairs; k++) {
    const int i0 = k << 1;
    const int16_t m0 = clampSample16((inL[i0] + inR[i0]) >> 1);
    const int16_t m1 = clampSample16((inL[i0 + 1] + inR[i0 + 1]) >> 1);
    const uint16_t a0 = absSample16(m0);
    const uint16_t a1 = absSample16(m1);
    if (a0 > pk) pk = a0;
    if (a1 > pk) pk = a1;
    const int16_t dec = (int16_t)(((int32_t)prev + ((int32_t)m0 << 1) + (int32_t)m1) >> 2);
    prev = m1;
    tunerRing[(w + (uint32_t)k) & TUNER_RING_MASK] = dec;
  }
  __asm__ volatile("" ::: "memory");
  tunerWriteHead = w + (uint32_t)pairs;
  tunerPrevM1 = prev;
  tunerBlockPeak = pk;
}

void tunerPushMono16(const int16_t *mono, int numFrames) {
  uint32_t w = tunerWriteHead;
  int16_t prev = tunerPrevM1;
  uint16_t pk = tunerBlockPeak;
  const int pairs = numFrames >> 1;
  for (int k = 0; k < pairs; k++) {
    const int i0 = k << 1;
    const int16_t m0 = mono[i0];
    const int16_t m1 = mono[i0 + 1];
    const uint16_t a0 = absSample16(m0);
    const uint16_t a1 = absSample16(m1);
    if (a0 > pk) pk = a0;
    if (a1 > pk) pk = a1;
    const int16_t dec = (int16_t)(((int32_t)prev + ((int32_t)m0 << 1) + (int32_t)m1) >> 2);
    prev = m1;
    tunerRing[(w + (uint32_t)k) & TUNER_RING_MASK] = dec;
  }
  __asm__ volatile("" ::: "memory");
  tunerWriteHead = w + (uint32_t)pairs;
  tunerPrevM1 = prev;
  tunerBlockPeak = pk;
}

void tunerPushSilenceBlock(int numFrames) {
  uint32_t w = tunerWriteHead;
  int16_t prev = tunerPrevM1;
  const int pairs = numFrames >> 1;
  for (int k = 0; k < pairs; k++) {
    const int16_t dec = (int16_t)(prev >> 2);
    prev = 0;
    tunerRing[(w + (uint32_t)k) & TUNER_RING_MASK] = dec;
  }
  __asm__ volatile("" ::: "memory");
  tunerWriteHead = w + (uint32_t)pairs;
  tunerPrevM1 = 0;
}

FLASHMEM uint16_t tunerRefTenths() {
  return songTunerRefTenths;
}

FLASHMEM void setTunerRefTenths(uint16_t tenths) {
  if (tenths == 0) {
    songTunerRefTenths = DEFAULT_TUNER_REF_TENTHS;
  } else {
    songTunerRefTenths = (uint16_t)constrain((int)tenths,
                                             (int)MIN_TUNER_REF_TENTHS,
                                             (int)MAX_TUNER_REF_TENTHS);
  }
}

FLASHMEM const char *tunerSourceLabel(TunerSource src) {
  uint8_t idx = (uint8_t)src;
  if (idx > TUNER_SELECTABLE_SOURCES) idx = 0;
  return TUNER_SRC_LABELS[idx];
}

FLASHMEM void tunerResetDetector() {
  noInterrupts();
  memset(tunerRing, 0, sizeof(tunerRing));
  tunerWriteHead = 0;
  tunerBlockPeak = 0;
  tunerPrevM1 = 0;
  interrupts();

  lastDetectMs   = 0;
  lastUiTickMs   = 0;
  rawHzCount     = 0;
  smoothHz       = 0.0f;
  smoothCents    = 0.0f;
  lockedMidiNote = -1;
  pitchLocked    = false;
  holdTicksLeft  = 0;
  meterEnvLin    = 0.0f;
  meterClip      = false;
  strobePhase    = 0.0f;
}

FLASHMEM static void releasePrevTunerInstIfNeeded(TunerSource prevSrc) {
  if (prevSrc >= TunerSource::Inst1 && prevSrc <= TunerSource::Inst8) {
    fmSynthReleaseDirectVoices((int)prevSrc - (int)TunerSource::Inst1);
  }
}

FLASHMEM void tunerActivate(TunerSource src, bool muteOutput) {
  uint8_t idx = (uint8_t)src;
  if (idx < 1 || idx > TUNER_SELECTABLE_SOURCES) {
    src = TunerSource::LineIn;
  }
  const TunerSource prevSrc = tunerActiveSource;
  const bool srcChanged = (prevSrc != src);
  noInterrupts();
  tunerActiveSource = src;
  tunerMuteActive = muteOutput;
  interrupts();
  if (srcChanged) {
    releasePrevTunerInstIfNeeded(prevSrc);
    tunerResetDetector();
  }
}

FLASHMEM void tunerDeactivate() {
  const TunerSource prevSrc = tunerActiveSource;
  noInterrupts();
  tunerActiveSource = TunerSource::Off;
  tunerMuteActive = false;
  interrupts();
  releasePrevTunerInstIfNeeded(prevSrc);
  pitchLocked    = false;
  holdTicksLeft  = 0;
  rawHzCount     = 0;
  meterEnvLin    = 0.0f;
  meterClip      = false;
}

FLASHMEM static float median3f(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

FLASHMEM static float evalFullDiff(const float *sig, int startIdx, int winLen, int tau) {
  float sum = 0.0f;
  const float *p0 = sig + startIdx;
  const float *p1 = sig + startIdx + tau;
  for (int i = 0; i < winLen; i++) {
    float d = p0[i] - p1[i];
    sum += d * d;
  }
  return sum;
}

FLASHMEM static float parabolicRefineLag(float dMinus, float dZero, float dPlus, int tauInt) {
  float denom = dMinus - 2.0f * dZero + dPlus;
  if (fabsf(denom) < 1e-12f) return (float)tauInt;
  float delta = 0.5f * (dMinus - dPlus) / denom;
  delta = constrain(delta, -0.75f, 0.75f);
  return (float)tauInt + delta;
}

FLASHMEM static void updateLockedNoteAndCents(float hz) {
  const float refHz = (float)songTunerRefTenths * 0.1f;
  const float midiFloat = 69.0f + 12.0f * log2f(hz / refHz);
  int candMidi = constrain((int)lroundf(midiFloat), 0, 127);

  // +/- 4 cent hysteresis around the +/- 50 cent note boundary
  if (lockedMidiNote >= 0 && fabsf(midiFloat - (float)lockedMidiNote) <= 0.54f) {
    candMidi = (int)lockedMidiNote;
  }
  lockedMidiNote = (int8_t)candMidi;
  float c = (midiFloat - (float)candMidi) * 100.0f;
  smoothCents = constrain(c, -54.0f, 54.0f);
}

FLASHMEM static void runYinDetectStep() {
  float x[TUNER_SNAP_LEN];
  constexpr int GATE_WIN = 1024;
  const int gateStart = TUNER_SNAP_LEN - GATE_WIN;
  int64_t sumRaw = 0;
  bool snapOk = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t headSnap = tunerWriteHead;
    __asm__ volatile("" ::: "memory");
    uint32_t startPos = headSnap - (uint32_t)TUNER_SNAP_LEN;
    sumRaw = 0;
    for (int i = 0; i < TUNER_SNAP_LEN; i++) {
      int16_t s = tunerRing[(startPos + (uint32_t)i) & TUNER_RING_MASK];
      x[i] = (float)s;
      if (i >= gateStart) sumRaw += s;
    }
    __asm__ volatile("" ::: "memory");
    if ((uint32_t)(tunerWriteHead - headSnap) <= (TUNER_RING_SAMPLES - (uint32_t)TUNER_SNAP_LEN)) {
      snapOk = true;
      break;
    }
  }
  if (!snapOk) return;

  // Compute mean over recent 1024 samples for DC removal, plus RMS & peak for gate
  const float mean = (float)sumRaw * (1.0f / (float)GATE_WIN);
  const float invScale = 1.0f / 32768.0f;
  float energy = 0.0f;
  float peak = 0.0f;
  for (int i = 0; i < TUNER_SNAP_LEN; i++) {
    float v = (x[i] - mean) * invScale;
    x[i] = v;
    if (i >= gateStart) {
      energy += v * v;
      float av = fabsf(v);
      if (av > peak) peak = av;
    }
  }
  const float rms = sqrtf(energy * (1.0f / (float)GATE_WIN));

  if (rms < TUNER_GATE_RMS || peak < TUNER_GATE_PEAK) {
    if (holdTicksLeft > 0) {
      holdTicksLeft--;
    } else {
      pitchLocked    = false;
      rawHzCount     = 0;
      lockedMidiNote = -1;
    }
    return;
  }

  int bestTauFull = -1;
  float bestLagFrac = 0.0f;
  float bestCmnd = 1.0f;

  // 1. High/Mid Band YIN @ 22,050 Hz (tau = 1 .. YIN_HI_TAU_MAX + 2, recent window)
  {
    constexpr int HI_MAX_EVAL = YIN_HI_TAU_MAX + 2;
    const int hiStart = TUNER_SNAP_LEN - (YIN_HI_WIN + HI_MAX_EVAL + 2);
    float dHi[HI_MAX_EVAL + 1];
    float cmndHi[HI_MAX_EVAL + 1];
    dHi[0] = 0.0f;
    cmndHi[0] = 1.0f;
    float runSumHi = 0.0f;
    for (int tau = 1; tau <= HI_MAX_EVAL; tau++) {
      float df = evalFullDiff(x, hiStart, YIN_HI_WIN, tau);
      dHi[tau] = df;
      runSumHi += df;
      cmndHi[tau] = (runSumHi > 1e-12f) ? (df * (float)tau / runSumHi) : 1.0f;
    }

    for (int tau = YIN_HI_TAU_MIN; tau <= YIN_HI_TAU_MAX; tau++) {
      if (cmndHi[tau] < 0.14f) {
        while (tau + 1 <= YIN_HI_TAU_MAX && cmndHi[tau + 1] <= cmndHi[tau]) {
          tau++;
        }
        bestTauFull = tau;
        bestCmnd = cmndHi[tau];
        bestLagFrac = parabolicRefineLag(dHi[tau - 1], dHi[tau], dHi[tau + 1], tau);
        break;
      }
    }
  }

  // 2. Low/Mid Band YIN @ 11,025 Hz if no high-band fundamental found
  if (bestTauFull < 0) {
    constexpr int LO_LEN = TUNER_SNAP_LEN / 2; // 960 samples @ 11,025 Hz
    float xLo[LO_LEN];
    for (int i = 0; i < LO_LEN; i++) {
      xLo[i] = 0.5f * (x[2 * i] + x[2 * i + 1]);
    }

    constexpr int LO_MAX_EVAL = YIN_LO_TAU_MAX + 2;
    const int loStart = LO_LEN - (YIN_LO_WIN + LO_MAX_EVAL + 2);
    float cmndLo[LO_MAX_EVAL + 1];
    cmndLo[0] = 1.0f;
    float runSumLo = 0.0f;
    for (int tau2 = 1; tau2 <= LO_MAX_EVAL; tau2++) {
      float df = evalFullDiff(xLo, loStart, YIN_LO_WIN, tau2);
      runSumLo += df;
      cmndLo[tau2] = (runSumLo > 1e-12f) ? (df * (float)tau2 / runSumLo) : 1.0f;
    }

    int bestTau2 = -1;
    for (int tau2 = YIN_LO_TAU_MIN; tau2 <= YIN_LO_TAU_MAX; tau2++) {
      if (cmndLo[tau2] < 0.18f) {
        while (tau2 + 1 <= YIN_LO_TAU_MAX && cmndLo[tau2 + 1] <= cmndLo[tau2]) {
          tau2++;
        }
        bestTau2 = tau2;
        bestCmnd = cmndLo[tau2];
        break;
      }
    }

    // Fallback to global minimum if clean enough (< 0.26)
    if (bestTau2 < 0) {
      float minVal = 0.26f;
      for (int tau2 = YIN_LO_TAU_MIN; tau2 <= YIN_LO_TAU_MAX; tau2++) {
        if (cmndLo[tau2] < minVal &&
            cmndLo[tau2] <= cmndLo[tau2 - 1] &&
            cmndLo[tau2] <= cmndLo[tau2 + 1]) {
          minVal = cmndLo[tau2];
          bestTau2 = tau2;
          bestCmnd = minVal;
        }
      }
    }

    if (bestTau2 > 0) {
      // Refine around 2 * bestTau2 directly on the full-rate 22,050 Hz buffer
      const int centerTau = 2 * bestTau2;
      const int tMin = max(16, centerTau - 4);
      const int tMax = min(TUNER_SNAP_LEN - YIN_REFINE_WIN - 2, centerTau + 4);
      if (tMax >= tMin + 2) {
        const int refStart = TUNER_SNAP_LEN - (YIN_REFINE_WIN + tMax + 2);
        float dLocal[11];
        int bestLocalIdx = 0;
        float bestLocalD = 1e30f;
        const int count = tMax - tMin + 1;
        for (int k = 0; k < count; k++) {
          dLocal[k] = evalFullDiff(x, refStart, YIN_REFINE_WIN, tMin + k);
          if (dLocal[k] < bestLocalD) {
            bestLocalD = dLocal[k];
            bestLocalIdx = k;
          }
        }
        int refIdx = constrain(bestLocalIdx, 1, count - 2);
        bestTauFull = tMin + refIdx;
        bestLagFrac = parabolicRefineLag(dLocal[refIdx - 1], dLocal[refIdx], dLocal[refIdx + 1], bestTauFull);
      }
    }
  }

  if (bestTauFull <= 0 || bestLagFrac <= 1.0f || bestCmnd > 0.28f) {
    if (holdTicksLeft > 0) {
      holdTicksLeft--;
    } else {
      pitchLocked    = false;
      rawHzCount     = 0;
      lockedMidiNote = -1;
    }
    return;
  }

  const float fsDec = 0.5f * (float)AUDIO_SAMPLE_RATE_EXACT;
  const float detectedHz = fsDec / bestLagFrac;
  if (detectedHz < 26.0f || detectedHz > 2800.0f) {
    if (holdTicksLeft > 0) {
      holdTicksLeft--;
    } else {
      pitchLocked    = false;
      rawHzCount     = 0;
      lockedMidiNote = -1;
    }
    return;
  }

  // Push into 3-point median history to reject single-frame transient octave glitches
  rawHzHist[0] = rawHzHist[1];
  rawHzHist[1] = rawHzHist[2];
  rawHzHist[2] = detectedHz;
  if (rawHzCount < 3) rawHzCount++;

  const float medHz = (rawHzCount >= 3) ? median3f(rawHzHist[0], rawHzHist[1], rawHzHist[2])
                                        : detectedHz;

  if (!pitchLocked || smoothHz <= 0.0f) {
    smoothHz = medHz;
  } else {
    float diffCents = fabsf(1200.0f * log2f(medHz / smoothHz));
    if (diffCents > 150.0f) {
      // Snap immediately when playing a new note (> 1.5 semitones)
      smoothHz = medHz;
      lockedMidiNote = -1;
    } else {
      float alpha = (diffCents > 15.0f) ? 0.45f : ((diffCents > 4.0f) ? 0.30f : 0.20f);
      smoothHz += alpha * (medHz - smoothHz);
    }
  }

  pitchLocked = true;
  holdTicksLeft = TUNER_HOLD_TICKS;
  updateLockedNoteAndCents(smoothHz);
}

FLASHMEM void tunerService(TunerReadout &out) {
  const uint32_t nowMs = millis();

  // Sample & reset ISR peak since last UI frame for 30 fps level meter
  noInterrupts();
  uint16_t rawPk = tunerBlockPeak;
  tunerBlockPeak = 0;
  interrupts();

  float instPk = (float)rawPk * (1.0f / 32767.0f);
  uint32_t dtUiMs = (lastUiTickMs == 0) ? 33u : min(200u, nowMs - lastUiTickMs);
  lastUiTickMs = nowMs;
  const float dtSec = (float)dtUiMs * 0.001f;

  if (instPk >= meterEnvLin) {
    meterEnvLin = instPk;
  } else {
    float decay = expf(-dtSec * 8.5f);
    meterEnvLin *= decay;
    if (meterEnvLin < 1e-5f) meterEnvLin = 0.0f;
  }
  meterClip = (instPk >= 0.965f);

  // Run YIN pitch detector at ~20 Hz (every 50 ms) with SD streaming keep-alive around it
  if (lastDetectMs == 0 || (nowMs - lastDetectMs) >= 50u) {
    lastDetectMs = nowMs;
    keepStreaming();
    runYinDetectStep();
    keepStreaming();
  } else if (pitchLocked && smoothHz > 0.0f) {
    // Keep cents updated immediately if user adjusts A4 REF between 50 ms detector ticks
    updateLockedNoteAndCents(smoothHz);
  }

  // Advance continuous phase strobe (capped below 6.5 cycles/sec to prevent 30 fps wagon-wheel aliasing)
  if (pitchLocked) {
    float effCents = (fabsf(smoothCents) < 0.08f) ? 0.0f : constrain(smoothCents, -30.0f, 30.0f);
    strobePhase += effCents * 0.21f * dtSec;
    strobePhase -= floorf(strobePhase);
  }

  // Map linear envelope to -60 dBFS .. 0 dBFS normalized bar [0..1]
  float lvlNorm = 0.0f;
  if (meterEnvLin > 0.001f) {
    float db = 20.0f * log10f(meterEnvLin);
    lvlNorm = constrain((db + 60.0f) * (1.0f / 60.0f), 0.0f, 1.0f);
  }

  out.locked       = pitchLocked;
  out.midiNote     = pitchLocked ? lockedMidiNote : (int8_t)-1;
  out.hz           = pitchLocked ? smoothHz : 0.0f;
  out.cents        = pitchLocked ? smoothCents : 0.0f;
  out.levelNorm    = lvlNorm;
  out.clipping     = meterClip;
  out.strobePhase8 = (uint8_t)((int)(strobePhase * 256.0f) & 0xFF);
}

FLASHMEM bool tunerMatchRefToCurrentPitch(uint16_t &outTenths) {
  if (!pitchLocked || smoothHz <= 0.0f || lockedMidiNote < 0) {
    return false;
  }
  // Compute exact A4 Hz that places smoothHz at 0.0 cents for lockedMidiNote:
  // smoothHz = newA4 * 2^((lockedMidiNote - 69) / 12)  =>  newA4 = smoothHz * 2^((69 - lockedMidiNote) / 12)
  float matchedA4 = smoothHz * powf(2.0f, (float)(69 - (int)lockedMidiNote) / 12.0f);
  int tenths = (int)lroundf(matchedA4 * 10.0f);
  outTenths = (uint16_t)constrain(tenths, (int)MIN_TUNER_REF_TENTHS, (int)MAX_TUNER_REF_TENTHS);
  return true;
}

} // namespace looper

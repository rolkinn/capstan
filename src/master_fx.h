// master_fx.h — Modular Master Effects Bus Architecture (Chorus -> Delay -> Reverb -> Tape -> Comp)
#pragma once
#include <Arduino.h>
#include "config.h"

namespace looper {

enum MasterFxCol : int {
  MASTER_FX_CHOR  = 0,
  MASTER_FX_DLAY  = 1,
  MASTER_FX_DFB   = 2,
  MASTER_FX_RVRB  = 3,
  MASTER_FX_TAPE  = 4,
  MASTER_FX_COMP  = 5,
  MASTER_FX_COUNT = 6
};

// Modular Stereo Effect Unit Descriptor
struct MasterFxOps {
  MasterFxCol id;
  const char *shortLabel;                      // 4-char UI badge ("CHOR", "DLAY", "D.FB", "RVRB", "TAPE", "COMP")
  void (*setAmount)(uint8_t pct);              // Configure DSP parameters for 0..100% control
  void (*resetState)();                        // Clear delay lines, filters, and envelopes
  bool (*isEnabled)();                         // True when control/effect is active (pct > 0)
  void (*processSample)(float &xL, float &xR); // Process normalized [-1..+1] stereo frame in-place
};

const MasterFxOps &masterFxOpsFor(int slot);
const char *masterFxLabel(int slot);
void setMasterFxSlot(int slot, uint8_t pct);

// Per-effect parameter setters (0..100%) and master bus entry points
void setMasterChorus(uint8_t pct);
void setMasterDelay(uint8_t pct);
void setMasterDelayFeedback(uint8_t pct);
void setMasterReverb(uint8_t pct);
void setMasterTape(uint8_t pct);
void setMasterComp(uint8_t pct);
void resetMasterFxState();
void processMasterFx(float &mixL, float &mixR);

// Shared rational C1 soft-knee ceiling & 32-sample (0.73 ms) zero-overshoot lookahead linear limiter
float softKneeClip(float x, float knee, float ceiling);

struct LookaheadMonitorLimiter {
  static constexpr uint32_t LOOKAHEAD = 32;            // 0.725 ms at 44.1 kHz
  static constexpr uint32_t MIN_WIN   = LOOKAHEAD + 1; // 33 samples -> zero overshoot at tap 32
  static constexpr uint32_t DQ_MASK   = 63;            // power-of-2 monotonic deque capacity
  static constexpr uint32_t ONE_Q24   = 1u << 24;
  static constexpr uint32_t FULL_BOX  = LOOKAHEAD * ONE_Q24; // 0x20000000u
  static constexpr float INV_BOX_Q24  = 1.0f / (float)FULL_BOX;

  float thresh = 30500.0f;
  float knee   = 31600.0f;
  float ceil   = 32766.0f;

  float delayL[LOOKAHEAD] = {};
  float delayR[LOOKAHEAD] = {};
  uint32_t boxBuf[LOOKAHEAD] = {
    ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24,
    ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24,
    ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24,
    ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24, ONE_Q24
  };
  uint32_t boxSum = FULL_BOX;
  uint32_t dqIdx[DQ_MASK + 1] = {};
  float dqVal[DQ_MASK + 1] = {};
  uint32_t dqHead = 0;
  uint32_t dqTail = 0;
  uint32_t sampleTick = 0;
  uint32_t pos = 0;
  float relGain = 1.0f;
  float slowGain = 1.0f;

  constexpr LookaheadMonitorLimiter(float t = 30500.0f, float k = 31600.0f, float c = 32766.0f)
      : thresh(t), knee(k), ceil(c) {}

  void reset();
  float stepGain(float pk);
  float run(float inL, float inR, float &outL, float &outR);
};

// Shared Instrument FX Bus (Drive -> Chorus -> Delay -> Reverb)
// Uses the exact same Chorus, Delay, Delay Feedback, and Reverb implementations as Master FX,
// plus per-instrument warm oversaturated Drive distortion.
void resetInstFxState();
void resetInstDriveState(int instIdx);
float processInstDriveSample(int instIdx, uint8_t drivePct, float x);
void setInstFxParams(uint8_t chorusPct, uint8_t delayPct, uint8_t delayFbPct, uint8_t reverbPct);
bool isInstFxChainEnabled();
void processInstFxSample(float &xL, float &xR);

} // namespace looper

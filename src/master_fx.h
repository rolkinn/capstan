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

enum AudioTrackFxSlot : uint8_t {
  AUD_FX_DRIVE = 0,
  AUD_FX_CHOR  = 1,
  AUD_FX_DLAY  = 2,
  AUD_FX_DFB   = 3,
  AUD_FX_RVRB  = 4,
  AUD_FX_TAPE  = 5,
  AUD_FX_COMP  = 6,
  AUD_FX_COUNT = 7
};

struct AudioTrackFxParams {
  uint8_t drive = 0;         // 0..100%
  uint8_t chorus = 0;        // 0..100%
  uint8_t delay = 0;         // 0..100%
  uint8_t delayFeedback = 0; // 0..100%
  uint8_t reverb = 0;        // 0..100%
  uint8_t tape = 0;          // 0..100%
  uint8_t comp = 0;          // 0..100%
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
  uint16_t dqIdx[DQ_MASK + 1] = {};
  float dqVal[DQ_MASK + 1] = {};
  uint32_t dqHead = 0;
  uint32_t dqTail = 0;
  uint16_t sampleTick = 0;
  uint32_t pos = 0;
  float relGain = 1.0f;
  float slowGain = 1.0f;

  constexpr LookaheadMonitorLimiter(float t = 30500.0f, float k = 31600.0f, float c = 32766.0f)
      : thresh(t), knee(k), ceil(c) {}

  void reset();
  float stepGain(float pk);
  float run(float inL, float inR, float &outL, float &outR);
};

// Shared Instrument FX Bus (Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp)
// Uses the exact same Chorus, Delay, Delay Feedback, Reverb, Tape, and Comp implementations as Master FX,
// plus per-instrument warm oversaturated Drive distortion.
void resetInstFxState();
void resetInstDriveState(int instIdx);
float processInstDriveSample(int instIdx, uint8_t drivePct, float x);
float processInstDriveChannelSample(int instIdx, int ch, uint8_t drivePct, float x);
void setInstFxParams(uint8_t chorusPct, uint8_t delayPct, uint8_t delayFbPct, uint8_t reverbPct,
                     uint8_t tapePct = 0, uint8_t compPct = 0);
bool isInstFxChainEnabled();
bool instNeedsSharedAuxSend();
void processInstFxSample(float &xL, float &xR);

// Per-Audio-Track FX Chain (Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp):
// - Independent per-track Drive, Tape, and Comp inserts (Stereo on A1..A4, Mono on A5..A8)
// - Pure-wet Aux Send/Return routing into Chorus -> Delay -> Reverb
struct AudioTrackFxBlockCtx {
  uint8_t insertMask;
  uint8_t auxSendMask;
  bool runChorAux;
  bool runDlyAux;
  bool runVerbAux;
  bool hasInstAux;
  float instChorSend;
  float instDlySend;
  float instVerbSend;
  float dryTrim[NUM_AUDIO_TRACKS];
  float chorSend[NUM_AUDIO_TRACKS];
  float dlySend[NUM_AUDIO_TRACKS];
  float verbSend[NUM_AUDIO_TRACKS];
  float chorToDly;
  float chorToVerb;
  float dlyToVerb;
};

struct AudioInputFxBlockCtx {
  bool inlineChor;
  bool inlineDly;
  bool inlineVerb;
  bool hasAuxSend;
  bool usbTailActive;
  float dryTrim;
  float chorSend;
  float dlySend;
  float verbSend;
};

void resetAudioInputFxState();
void syncAudioInputFxParams(const AudioTrackFxParams &fx);
void setAudioInputOutGain(uint8_t volStep, float targetGain, bool snap = false);
void updateAudioInputFxTargets(bool inputBusEnabled);
void processAudioInputFxBlock(AudioInputFxBlockCtx &inCtx,
                              bool inputBusEnabled,
                              bool lineActive, int16_t *lineBufL, int16_t *lineBufR,
                              bool usbStreamActive, bool usbWanted, uint32_t *usbFrames,
                              bool preferUsbInline);

void resetAudioTrackFxState(int trackIdx);
void resetAllAudioTrackFxState();
void syncAudioTrackFxParams(int trackIdx, const AudioTrackFxParams &fx);
bool anyAudioTrackDelayOrReverbActive();
void prepareAudioTrackFxBlock(AudioTrackFxBlockCtx &ctx, uint8_t processTrackMask, uint8_t audibleTrackMask,
                              bool instAuxAvailable, AudioInputFxBlockCtx *inCtx = nullptr);
void processAudioTrackInsertSample(int trackIdx, bool stereo, float &sl, float &sr);
void processAudioTrackAuxSample(const AudioTrackFxBlockCtx &ctx, uint32_t instPackedSample,
                                float chorInL, float chorInR,
                                float dlyInL, float dlyInR,
                                float verbInL, float verbInR,
                                float &wetOutL, float &wetOutR);

} // namespace looper

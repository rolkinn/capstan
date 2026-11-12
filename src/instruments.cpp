// instruments.cpp — Modular Multi-Engine Instrument Host & Voice Manager
#include "instruments.h"
#include "inst_fm2op.h"
#include "inst_phasedist.h"
#include "inst_sampler.h"
#include "inst_noisedrone.h"
#include "midi_seq.h"
#include "midi_map.h"
#include "master_fx.h"
#include "storage.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

namespace looper {

// 1024-point sine lookup table in cached RAM2 (DMAMEM) — initialized once in fmSynthInit()
DMAMEM static float sinLut[INST_LUT_SIZE];

// Exact 32-bit phase steps at 44.1 kHz for Octave 9 (MIDI notes 108..119 = C8..B8);
// all other octaves are derived in O(1) via exact power-of-2 bit shifts in noteToBaseStep().
static const uint32_t semitoneStepOct9[12] = {
  407680855u, // C8  (108)
  431922813u, // C#8 (109)
  457606307u, // D8  (110)
  484816982u, // D#8 (111)
  513645694u, // E8  (112)
  544188687u, // F8  (113)
  576547803u, // F#8 (114)
  610830693u, // G8  (115)
  647151059u, // G#8 (116)
  685630106u, // A8  (117, 7040 Hz)
  726395894u, // A#8 (118)
  769584355u  // B8  (119)
};

// Lock-free SynthEvent ring buffer in RAM1 consumed inside fmSynthRenderBlock() (Audio ISR)
enum class SynthEvType : uint8_t {
  NoteOn        = 0,
  NoteOff       = 1,
  ControlChange = 2,
  PitchBend     = 3
};

struct SynthEvent {
  uint8_t type;         // SynthEvType
  uint8_t instIdx;      // 0..7
  uint8_t d1;           // note / cc / pb_lsb
  uint8_t d2;           // velocity / cc_val / pb_msb
  uint8_t sampleOffset; // 0..127 sub-block sample offset within the 128-sample block
  uint8_t streamIdx;    // 0..3 Sampler stream slot (or 0xFF)
  uint16_t streamGen;   // Sampler stream generation (1..65535)
};
static_assert(sizeof(SynthEvent) == 8, "SynthEvent must be 8 bytes");

static constexpr uint32_t SYNTH_EV_RING_SIZE = 128;
static constexpr uint32_t SYNTH_EV_RING_MASK = SYNTH_EV_RING_SIZE - 1;
static SynthEvent synthEvRing[SYNTH_EV_RING_SIZE];
static volatile uint32_t synthEvHead = 0;
static volatile uint32_t synthEvTail = 0;

// Active song's 8 instrument slots (all start blank with InstType::None)
static FmPatch patches[NUM_INSTRUMENTS];

// Copy/Paste clipboard for Instruments page
DMAMEM static FmPatch clipboardPatch;
static bool clipboardValid = false;

// Per-instrument runtime state (pitch bend, CC, telemetry) and cached envelope rates
struct FmInstEnvRates {
  InstEnvParams lastAmp;
  InstEnvParams lastFlt;
  float ampAmt;
  float ampAtkInc;
  float ampDecMul;
  float ampSusLvl;
  float ampRelMul;
  float fltAmt;
  float fltAtkInc;
  float fltDecMul;
  float fltSusLvl;
  float fltRelMul;
};

static FmInstRuntime instRt[NUM_INSTRUMENTS];
DMAMEM static FmInstEnvRates instEnvRates[NUM_INSTRUMENTS];

// 64-voice polyphonic pool: 8 dedicated voices per instrument slot (I01..I08)
static FmVoice voices[NUM_FM_VOICES];
static uint32_t voiceCounter = 0;
static uint32_t noiseRng = 0x9E3779B9u;
static int32_t instFxTailFrames = 0;
static int32_t instFxSilentBlocks = 0;
static int8_t lastFxInst = -1;
static int8_t lastFxMidiTrack = -1;

// Audition auto-note-off & Noise Drone Latch/Auto state
static int auditionInst = -1;
static uint8_t auditionNote = 60;
static uint32_t auditionOffMs = 0;
static uint8_t latchedDroneMask = 0;
static uint8_t autoDroneMask = 0;

// SD Card Instrument File Browser Modal state (.FMI files) & shared modal browser pool (32-Entry Sliding Window)
static constexpr int MAX_INST_BROWSER_ENTRIES = 32;
static constexpr size_t SHARED_BROWSER_POOL_BYTES = 32 * 64; // 2,048 B shared across FMI, MIDI, & WAV browsers
DMAMEM static uint8_t sharedBrowserPool[SHARED_BROWSER_POOL_BYTES] __attribute__((aligned(4)));
static InstBrowserEntry *const instBrowserEntries = reinterpret_cast<InstBrowserEntry *>(sharedBrowserPool);
static int instBrowserCount = 0;
static int instBrowserWinStart = 0;
static int instBrowserWinCount = 0;
static int instBrowserCur = 0;
static int instBrowserScr = 0;
static int instBrowserSlot = 0;
static bool instBrowserPresetMode = false;
static bool instBrowserModalOpen = false;
static bool sysPresetsModalOpen = false;
static bool sysPresetsSongMode = false;
static bool sysPresetDelConfirmOpen = false;
static int sysPresetDelConfirmCur = 0; // 0 = YES, 1 = NO
static bool typePresetModalOpen = false;
static bool typePresetIsGlobal = false;
static char instBrowserDir[INST_BROWSER_DIR_MAX] = "";

void *sharedBrowserPoolBuf() {
  return sharedBrowserPool;
}

char *sharedBrowserDirBuf() {
  return instBrowserDir;
}

// ============================================================================
// Modular Instrument Engine Registry
// To add a new instrument in the future:
//   1. Create src/inst_<name>.h and src/inst_<name>.cpp exporting an InstEngineOps
//   2. Add its enum value to InstType, increment NUM_INST_TYPES, and list it here.
// ============================================================================
const InstEngineOps *instEngineAt(int typeIdx) {
  switch (constrain(typeIdx, 0, NUM_INST_TYPES - 1)) {
    case 0:
    default:
      return &instFm2OpEngine();
    case 1:
      return &instPhaseDistEngine();
    case 2:
      return &instSamplerEngine();
    case 3:
      return &instNoiseDroneEngine();
  }
}

FLASHMEM InstType instTypeAt(int typeIdx) {
  const InstEngineOps *ops = instEngineAt(typeIdx);
  return ops ? ops->type : InstType::Fm2Op;
}

const InstEngineOps *instEngineForType(InstType type) {
  for (int i = 0; i < NUM_INST_TYPES; i++) {
    const InstEngineOps *ops = instEngineAt(i);
    if (ops && ops->type == type) {
      return ops;
    }
  }
  return nullptr;
}

FLASHMEM const char *instTypeName(InstType type) {
  const InstEngineOps *ops = instEngineForType(type);
  return ops ? ops->typeName : "---";
}

// ============================================================================
// Shared DSP Utilities for Instrument Engines
// ============================================================================
float instSinLutAt(uint32_t idx) {
  return sinLut[idx & INST_LUT_MASK];
}

float instNextWhiteNoise() {
  uint32_t x = noiseRng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  noiseRng = x;
  return ((float)(int32_t)x) * (1.0f / 2147483648.0f);
}

void instAdvanceEnvelope(EnvStage &stage, float &env, float atkInc, float decMul, float susLvl, float relMul) {
  switch (stage) {
    case EnvStage::Attack:
      env += atkInc;
      if (env >= 1.0f) {
        env = 1.0f;
        stage = EnvStage::Decay;
      }
      break;
    case EnvStage::Decay:
      env = susLvl + (env - susLvl) * decMul;
      if (fabsf(env - susLvl) < 0.0005f) {
        env = susLvl;
        stage = EnvStage::Sustain;
      }
      break;
    case EnvStage::Sustain:
      env = susLvl;
      break;
    case EnvStage::Release:
      env *= relMul;
      if (env < 0.0008f) {
        env = 0.0f;
        stage = EnvStage::Idle;
      }
      break;
    default:
      env = 0.0f;
      break;
  }
}

bool instAdvanceVoiceEnvelopes(FmVoice &v) {
  const FmInstEnvRates &er = instEnvRates[constrain((int)v.instIdx, 0, NUM_INSTRUMENTS - 1)];
  v.fltAmt = er.fltAmt;
  instAdvanceEnvelope(v.fltStage, v.fltEnv,
                      er.fltAtkInc, er.fltDecMul, er.fltSusLvl, er.fltRelMul);
  instAdvanceEnvelope(v.ampStage, v.ampEnv,
                      er.ampAtkInc, er.ampDecMul, er.ampSusLvl, er.ampRelMul);
  if (v.ampStage == EnvStage::Idle ||
      er.ampAmt <= 0.0001f ||
      (v.ampStage == EnvStage::Sustain && v.ampEnv <= 0.0005f && !v.gate)) {
    v.active = false;
    return false;
  }
  return true;
}

static inline void updateVoiceSvfCoeffs(FmVoice &v, const FmInstEnvRates &er) {
  if ((v.svfSubPhase++ & 3u) == 0u) {
    float fltMod = constrain(er.fltAmt * v.fltEnv, 0.0f, 1.0f);
    if (fltMod < 0.995f) {
      float m2 = fltMod * fltMod;
      v.svfAlpha = 0.028f + m2 * (0.22f + 0.752f * m2);
      v.svfFbG = 0.20f * (1.0f - 0.5f * fltMod);
    } else {
      v.svfAlpha = 1.0f;
      v.svfFbG = 0.0f;
    }
  }
}

float instApplyVoiceFilterAndAmp(FmVoice &v, float rawSample, float voiceGain) {
  const FmInstEnvRates &er = instEnvRates[constrain((int)v.instIdx, 0, NUM_INSTRUMENTS - 1)];
  // General Filter Envelope (FLT: AMT, Attack, Decay, Sustain, Release in 0..127)
  // Drives a warm 2-pole resonant low-pass filter from ~200 Hz (0) to wide-open (~22 kHz at 127)
  updateVoiceSvfCoeffs(v, er);
  float filtered = rawSample;
  if (v.svfAlpha < 0.999f) {
    const float alpha = v.svfAlpha;
    float fb = v.svfFbG * (v.fltLp1 - v.fltLp2);
    float in = constrain(rawSample + fb, -1.5f, 1.5f);
    v.fltLp1 += alpha * (in - v.fltLp1);
    v.fltLp2 += alpha * (v.fltLp1 - v.fltLp2);
    filtered = v.fltLp2;
  } else {
    v.fltLp1 = rawSample;
    v.fltLp2 = rawSample;
  }

  // General Amplitude Envelope (AMP: AMT, Attack, Decay, Sustain, Release in 0..127)
  return filtered * (er.ampAmt * v.ampEnv) * voiceGain;
}

void instApplyVoiceStereoFilterAndAmp(FmVoice &v, float rawL, float rawR, float voiceGain,
                                      float &outL, float &outR) {
  const FmInstEnvRates &er = instEnvRates[constrain((int)v.instIdx, 0, NUM_INSTRUMENTS - 1)];
  updateVoiceSvfCoeffs(v, er);
  float filtL = rawL;
  float filtR = rawR;
  if (v.svfAlpha < 0.999f) {
    const float alpha = v.svfAlpha;
    const float fbG = v.svfFbG;

    float fbL = fbG * (v.fltLp1 - v.fltLp2);
    float inL = constrain(rawL + fbL, -1.5f, 1.5f);
    v.fltLp1 += alpha * (inL - v.fltLp1);
    v.fltLp2 += alpha * (v.fltLp1 - v.fltLp2);
    filtL = v.fltLp2;

    if (rawR == rawL) {
      v.opPrev[0] = v.fltLp1;
      v.opPrev[1] = v.fltLp2;
      filtR = filtL;
    } else {
      float fbR = fbG * (v.opPrev[0] - v.opPrev[1]);
      float inR = constrain(rawR + fbR, -1.5f, 1.5f);
      v.opPrev[0] += alpha * (inR - v.opPrev[0]);
      v.opPrev[1] += alpha * (v.opPrev[0] - v.opPrev[1]);
      filtR = v.opPrev[1];
    }
  } else {
    v.fltLp1 = rawL;
    v.fltLp2 = rawL;
    v.opPrev[0] = rawR;
    v.opPrev[1] = rawR;
  }

  float g = (er.ampAmt * v.ampEnv) * voiceGain;
  outL = filtL * g;
  outR = filtR * g;
}

const FmVoice *fmSynthVoiceAt(int voiceIdx) {
  if (voiceIdx < 0 || voiceIdx >= NUM_FM_VOICES) return nullptr;
  return &voices[voiceIdx];
}

FLASHMEM bool fmSynthIsNoteActive(int instIdx, uint8_t note) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    if (voices[i].active && voices[i].instIdx == (uint8_t)instIdx && voices[i].note == note) return true;
  }
  return false;
}

// ============================================================================
// Slot, Preset & Lifecycle Management
// ============================================================================
FLASHMEM static void sanitizeInstName(const char *in, char *out, size_t outSz, int fallbackSlot = 0, InstType t = InstType::Fm2Op) {
  if (outSz == 0) return;
  sanitizeIdentifierName(in, out, outSz, false);
  if (out[0] == '\0') {
    const InstEngineOps *ops = instEngineForType(t);
    const char *pfx = ops ? ops->defaultPrefix : "FM";
    snprintf(out, outSz, "%s_%02d", pfx, constrain(fallbackSlot + 1, 1, 99));
  }
}

FLASHMEM const FmPatch &fmSynthDefaultPatch(InstType type) {
  const InstEngineOps *ops = instEngineForType(type);
  if (!ops) ops = &instFm2OpEngine();
  return ops->defaultPatch();
}

FmPatch &fmSynthPatch(int instIdx) {
  return patches[constrain(instIdx, 0, NUM_INSTRUMENTS - 1)];
}

FLASHMEM bool fmSynthIsSlotEmpty(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return true;
  return instEngineForType((InstType)patches[instIdx].type) == nullptr;
}

FLASHMEM void fmSynthResetToDefault(int instIdx, bool keepName) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  InstType curType = (InstType)patches[instIdx].type;
  const InstEngineOps *ops = instEngineForType(curType);
  if (!ops) {
    ops = &instFm2OpEngine();
    curType = ops->type;
  }

  char savedName[MAX_SONG_NAME_LEN + 1] = {};
  if (keepName && patches[instIdx].name[0] != '\0') {
    memcpy(savedName, patches[instIdx].name, sizeof(savedName));
  }
  const FmPatch &src = ops->defaultPatch();
  memcpy(&patches[instIdx], &src, sizeof(FmPatch));
  patches[instIdx].type = (uint8_t)curType;
  if (keepName && savedName[0] != '\0') {
    memcpy(patches[instIdx].name, savedName, sizeof(savedName));
  } else {
    snprintf(patches[instIdx].name, sizeof(patches[instIdx].name), "%s_%02d", ops->defaultPrefix, instIdx + 1);
  }
  if (curType == InstType::Sampler) {
    samplerClearSlot(instIdx);
  }
  resetInstDriveState(instIdx);
}

FLASHMEM void fmSynthCreateInSlot(int instIdx, InstType type) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  fmSynthSilenceInst(instIdx);
  resetInstDriveState(instIdx);
  samplerClearSlot(instIdx);
  instMidiMapClearInst(instIdx);
  const InstEngineOps *ops = instEngineForType(type);
  if (ops) {
    const FmPatch &src = ops->defaultPatch();
    memcpy(&patches[instIdx], &src, sizeof(FmPatch));
    patches[instIdx].type = (uint8_t)ops->type;
    snprintf(patches[instIdx].name, sizeof(patches[instIdx].name), "%s_%02d", ops->defaultPrefix, instIdx + 1);
  } else {
    memset(&patches[instIdx], 0, sizeof(FmPatch));
    patches[instIdx].type = (uint8_t)InstType::None;
  }
}

FLASHMEM void fmSynthDeleteSlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  fmSynthSilenceInst(instIdx);
  resetInstDriveState(instIdx);
  samplerClearSlot(instIdx);
  instMidiMapClearInst(instIdx);
  memset(&patches[instIdx], 0, sizeof(FmPatch));
  patches[instIdx].type = (uint8_t)InstType::None;
}

FLASHMEM bool fmSynthCopySlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if (fmSynthIsSlotEmpty(instIdx)) return false;
  clipboardPatch = patches[instIdx];
  samplerCopySlot(instIdx);
  instMidiMapCopyInst(instIdx);
  clipboardValid = true;
  return true;
}

FLASHMEM bool fmSynthPasteSlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !clipboardValid) return false;
  fmSynthSilenceInst(instIdx);
  resetInstDriveState(instIdx);
  patches[instIdx] = clipboardPatch;
  samplerPasteSlot(instIdx);
  instMidiMapPasteInst(instIdx);
  memset(&clipboardPatch, 0, sizeof(clipboardPatch));
  clipboardValid = false;
  return true;
}

FLASHMEM bool fmSynthHasClipboard() {
  return clipboardValid;
}

FLASHMEM void fmSynthResetAll() {
  instBrowserModalOpen = false;
  sysPresetsModalOpen = false;
  sysPresetDelConfirmOpen = false;
  typePresetModalOpen = false;
  fmSynthAllNotesOff();
  samplerResetAll();
  instMidiMapResetAll();
  ndSynthResetAllVoices();
  resetInstFxState();
  instFxTailFrames = 0;
  instFxSilentBlocks = 0;
  lastFxInst = -1;
  lastFxMidiTrack = -1;
  latchedDroneMask = 0;
  autoDroneMask = 0;
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    memset(&patches[i], 0, sizeof(FmPatch));
    patches[i].type = (uint8_t)InstType::None;
    instRt[i].pitchBendRatio = 1.0f;
    instRt[i].modWheelBoost = 0.0f;
    instRt[i].sustainPedal = false;
    memset(&instEnvRates[i].lastAmp, 0xFF, sizeof(InstEnvParams));
    memset(&instEnvRates[i].lastFlt, 0xFF, sizeof(InstEnvParams));
  }
}

FLASHMEM void fmSynthInit() {
  for (uint32_t i = 0; i < INST_LUT_SIZE; i++) {
    sinLut[i] = sinf(2.0f * (float)M_PI * (float)i / (float)INST_LUT_SIZE);
  }
  fmSynthResetAll();
}

// ============================================================================
// Voice Allocation, Envelope Rates (0..127) & Real-Time MIDI Control
// ============================================================================
FLASHMEM uint16_t instEnvAttackMs(uint8_t param) {
  float norm = constrain((int)param, 0, (int)INST_ENV_MAX) * (1.0f / (float)INST_ENV_MAX);
  float ms = 0.8f + 4999.2f * (norm * norm);
  return (uint16_t)(ms + 0.5f);
}

FLASHMEM uint16_t instEnvDecayReleaseMs(uint8_t param) {
  float norm = constrain((int)param, 0, (int)INST_ENV_MAX) * (1.0f / (float)INST_ENV_MAX);
  float ms = 4.0f + 9996.0f * (norm * norm);
  return (uint16_t)(ms + 0.5f);
}

FLASHMEM static float attackIncrementForParam127(uint8_t param) {
  float norm = constrain((int)param, 0, (int)INST_ENV_MAX) * (1.0f / (float)INST_ENV_MAX);
  // Quadratic control law for strong early response (max 5000 ms):
  // 0 -> 0.8 ms, 16 -> 80 ms, 32 -> 318 ms, 64 -> 1.27 s, 100 -> 3.10 s, 127 -> 5.00 s
  float sec = 0.0008f + 4.9992f * (norm * norm);
  return 1.0f / max(1.0f, sec * (float)SAMPLE_RATE_HZ);
}

FLASHMEM static float decayMultiplierForParam127(uint8_t param) {
  float norm = constrain((int)param, 0, (int)INST_ENV_MAX) * (1.0f / (float)INST_ENV_MAX);
  // Quadratic control law for strong early response (max 10000 ms to -60 dB = exp(-6.9)):
  // 0 -> 4.0 ms, 16 -> 163 ms, 32 -> 639 ms, 64 -> 2.54 s, 100 -> 6.20 s, 127 -> 10.00 s
  float sec = 0.004f + 9.996f * (norm * norm);
  return expf(-6.907755f / max(1.0f, sec * (float)SAMPLE_RATE_HZ));
}

FLASHMEM static void refreshInstEnvelopeRates(int instIdx, const FmPatch &p) {
  FmInstEnvRates &er = instEnvRates[constrain(instIdx, 0, NUM_INSTRUMENTS - 1)];
  constexpr float invMax = 1.0f / (float)INST_ENV_MAX;
  if (memcmp(&er.lastAmp, &p.amp, sizeof(InstEnvParams)) != 0) {
    er.lastAmp   = p.amp;
    er.ampAmt    = constrain((int)p.amp.amt, 0, (int)INST_ENV_MAX) * invMax;
    er.ampAtkInc = attackIncrementForParam127(p.amp.attack);
    er.ampDecMul = decayMultiplierForParam127(p.amp.decay);
    er.ampSusLvl = constrain((int)p.amp.sustain, 0, (int)INST_ENV_MAX) * invMax;
    er.ampRelMul = decayMultiplierForParam127(p.amp.release);
  }
  if (memcmp(&er.lastFlt, &p.flt, sizeof(InstEnvParams)) != 0) {
    er.lastFlt   = p.flt;
    er.fltAmt    = constrain((int)p.flt.amt, 0, (int)INST_ENV_MAX) * invMax;
    er.fltAtkInc = attackIncrementForParam127(p.flt.attack);
    er.fltDecMul = decayMultiplierForParam127(p.flt.decay);
    er.fltSusLvl = constrain((int)p.flt.sustain, 0, (int)INST_ENV_MAX) * invMax;
    er.fltRelMul = decayMultiplierForParam127(p.flt.release);
  }
}

static inline uint32_t noteToBaseStep(uint8_t note) {
  uint32_t n = note & 0x7Fu;
  uint32_t oct = n / 12u;
  uint32_t semi = n - oct * 12u;
  uint32_t step9 = semitoneStepOct9[semi];
  if (oct < 9u) {
    uint32_t sh = 9u - oct;
    return (step9 + (1u << (sh - 1u))) >> sh;
  }
  if (oct > 9u) {
    return step9 << 1;
  }
  return step9;
}

static inline float fastExp2Small(float x) {
  return 1.0f + x * (0.6931472f + x * (0.2402265f + x * 0.0555041f));
}

static inline uint8_t packSynthInstTrack(int instIdx, uint8_t midiTrack) {
  uint8_t mNib = (midiTrack < NUM_MIDI_TRACKS) ? midiTrack : 0x0Fu;
  return (uint8_t)(((uint8_t)instIdx & 0x07u) | (mNib << 4));
}

static inline int unpackSynthInst(uint8_t packed) {
  return (int)(packed & 0x07u);
}

static inline uint8_t unpackSynthMidiTrack(uint8_t packed) {
  uint8_t mNib = (packed >> 4) & 0x0Fu;
  return (mNib < NUM_MIDI_TRACKS) ? mNib : 0xFFu;
}

static inline bool enqueueSynthEvent(SynthEvType type, uint8_t packedInstTrack, uint8_t d1, uint8_t d2,
                                     uint8_t sampleOffset = 0, uint8_t streamIdx = 0xFFu, uint16_t streamGen = 0) {
  uint32_t irq = saveIrq();
  uint32_t h = synthEvHead;
  if (h - synthEvTail >= SYNTH_EV_RING_SIZE) {
    restoreIrq(irq);
    return false;
  }
  SynthEvent &ev = synthEvRing[h & SYNTH_EV_RING_MASK];
  ev.type = (uint8_t)type;
  ev.instIdx = packedInstTrack;
  ev.d1 = d1;
  ev.d2 = d2;
  ev.sampleOffset = sampleOffset;
  ev.streamIdx = streamIdx;
  ev.streamGen = streamGen;
  __asm__ volatile("" ::: "memory");
  synthEvHead = h + 1;
  restoreIrq(irq);
  return true;
}

static void applySilenceInstInIsr(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  latchedDroneMask &= ~(uint8_t)(1u << instIdx);
  autoDroneMask &= ~(uint8_t)(1u << instIdx);
  instRt[instIdx].sustainPedal = false;
  instRt[instIdx].pitchBendRatio = 1.0f;
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    if (voices[i].active && voices[i].instIdx == (uint8_t)instIdx) {
      voices[i].gate = false;
      voices[i].sustained = false;
      voices[i].ampStage = EnvStage::Release;
      voices[i].fltStage = EnvStage::Release;
    }
  }
  samplerSilenceInst(instIdx);
}

static void applySilenceMidiTrackInIsr(int midiTrack) {
  if (midiTrack < 0 || midiTrack >= NUM_MIDI_TRACKS) return;
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    if (voices[i].active && voices[i].midiTrack == (uint8_t)midiTrack) {
      voices[i].gate = false;
      voices[i].sustained = false;
      voices[i].ampStage = EnvStage::Release;
      voices[i].fltStage = EnvStage::Release;
    }
  }
}

static void applyNoteOnInIsr(int instIdx, uint8_t note, uint8_t velocity,
                             uint8_t sampleOffset, uint32_t samplerStreamIdx, uint32_t samplerStreamGen,
                             uint8_t midiTrack = 0xFFu) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  const FmPatch &p = patches[instIdx];
  const InstType iType = (InstType)p.type;
  if (iType == InstType::None) return;

  uint32_t bStep = noteToBaseStep(note);
  float velNorm = constrain((int)velocity, 1, 127) * (1.0f / 127.0f);
  float vScale = 0.35f + 0.65f * velNorm;
  const bool isNoiseDrone = (iType == InstType::NoiseDrone);
  const bool isDroneLatch = isNoiseDrone && (p.algorithm == (uint8_t)NdTrigMode::Latch);
  const bool isDroneAuto  = isNoiseDrone && (p.algorithm == (uint8_t)NdTrigMode::Auto);

  // In Noise Drone LATCH or AUTO mode, manage the continuous drone voice cleanly:
  //   - LATCH: pressing the same active gated note toggles the drone off; pressing a new note retunes it.
  //   - AUTO:  incoming MIDI notes retune an already-gated transport drone without retriggering envelopes.
  if (isDroneLatch || isDroneAuto) {
    int gatedIdx = -1;
    for (int i = 0; i < NUM_FM_VOICES; i++) {
      if (voices[i].active && voices[i].instIdx == (uint8_t)instIdx && voices[i].gate) {
        gatedIdx = i;
        break;
      }
    }
    if (gatedIdx >= 0) {
      FmVoice &gv = voices[gatedIdx];
      if (isDroneLatch && gv.note == note) {
        gv.gate = false;
        gv.sustained = false;
        gv.ampStage = EnvStage::Release;
        gv.fltStage = EnvStage::Release;
        latchedDroneMask &= ~(uint8_t)(1u << instIdx);
        return;
      }
      gv.note = note;
      gv.velocity = velocity;
      gv.baseStep = bStep;
      gv.velScale = vScale;
      if (midiTrack < NUM_MIDI_TRACKS) {
        gv.midiTrack = midiTrack;
      }
      if (isDroneLatch) {
        latchedDroneMask |= (uint8_t)(1u << instIdx);
      }
      return;
    }
  }
  if (isDroneLatch) {
    latchedDroneMask |= (uint8_t)(1u << instIdx);
  }

  int chosen = -1;
  int instActiveCount = 0;

  // 1. Retrigger if the same note is already active on this instrument/track, and count active voices
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    if (voices[i].active && voices[i].instIdx == (uint8_t)instIdx) {
      instActiveCount++;
      if (chosen < 0 && voices[i].note == note &&
          (midiTrack >= NUM_MIDI_TRACKS || voices[i].midiTrack == midiTrack || voices[i].midiTrack >= NUM_MIDI_TRACKS)) {
        chosen = i;
      }
    }
  }

  // 2. If below per-instrument polyphony cap (8), pick any idle voice in the 32-voice global pool
  if (chosen < 0 && instActiveCount < VOICES_PER_INSTRUMENT) {
    for (int i = 0; i < NUM_FM_VOICES; i++) {
      if (!voices[i].active) {
        chosen = i;
        break;
      }
    }
  }

  // 3. If this instrument already has 8 active voices, steal the quietest / releasing voice on THIS instrument
  if (chosen < 0 && instActiveCount >= VOICES_PER_INSTRUMENT) {
    float bestScore = 1e9f;
    for (int i = 0; i < NUM_FM_VOICES; i++) {
      if (voices[i].active && voices[i].instIdx == (uint8_t)instIdx) {
        float score = voices[i].ampEnv + (voices[i].gate ? 2.0f : 0.0f);
        if (score < bestScore) {
          bestScore = score;
          chosen = i;
        }
      }
    }
  }

  // 4. Otherwise all 32 global voices are busy; steal the quietest / releasing voice across the pool
  if (chosen < 0) {
    float bestScore = 1e9f;
    chosen = 0;
    for (int i = 0; i < NUM_FM_VOICES; i++) {
      float score = voices[i].ampEnv + (voices[i].gate ? 2.0f : 0.0f);
      if (score < bestScore) {
        bestScore = score;
        chosen = i;
      }
    }
  }

  FmVoice &v = voices[chosen];
  const bool wasQuiet = (!v.active || v.ampEnv < 0.02f);
  const bool changedInst = (v.active && v.instIdx != (uint8_t)instIdx);
  v.active = true;
  v.instIdx = (uint8_t)instIdx;
  v.midiTrack = (midiTrack < NUM_MIDI_TRACKS) ? midiTrack : 0xFFu;
  v.note = note;
  v.velocity = velocity;
  v.gate = true;
  v.sustained = false;
  v.startSampleOffset = (wasQuiet || changedInst || iType == InstType::Sampler)
                            ? ((sampleOffset < AUDIO_BLOCK_SAMPLES) ? sampleOffset : 0)
                            : 0;
  v.age = ++voiceCounter;
  v.baseStep = bStep;
  v.velScale = vScale;
  v.fltAmt = instEnvRates[instIdx].fltAmt;
  v.svfSubPhase = 0;

  v.ampStage = EnvStage::Attack;
  v.fltStage = EnvStage::Attack;
  if (iType == InstType::Sampler) {
    v.opPhase[0] = samplerStreamIdx;
    v.opPhase[1] = samplerStreamGen;
    v.pdCycleParity = 0;
    v.opPrev[0] = 0.0f;
    v.opPrev[1] = 0.0f;
    v.fltLp1 = 0.0f;
    v.fltLp2 = 0.0f;
    samplerActivateStream(samplerStreamIdx, samplerStreamGen, chosen);
  } else if (wasQuiet || changedInst) {
    v.opPhase[0] = 0;
    v.opPhase[1] = 0;
    v.pdCycleParity = 0;
    v.opPrev[0] = 0.0f;
    v.opPrev[1] = 0.0f;
    v.fltLp1 = 0.0f;
    v.fltLp2 = 0.0f;
    if (isNoiseDrone) {
      ndSynthResetVoice(chosen);
    }
  }
}

static void applyNoteOffInIsr(int instIdx, uint8_t note, uint8_t midiTrack = 0xFFu) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  const FmPatch &p = patches[instIdx];
  const InstType iType = (InstType)p.type;
  if (iType == InstType::NoiseDrone &&
      (p.algorithm == (uint8_t)NdTrigMode::Latch || p.algorithm == (uint8_t)NdTrigMode::Auto)) {
    return;
  }
  const bool isSampler = (iType == InstType::Sampler);
  const bool playToEndOnRelease = isSampler &&
      (p.algorithm == (uint8_t)SamplerPressMode::Once ||
       p.algorithm == (uint8_t)SamplerPressMode::RepThru);

  bool pedal = instRt[instIdx].sustainPedal;
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    FmVoice &v = voices[i];
    if (v.active && v.instIdx == (uint8_t)instIdx && v.note == note && v.gate &&
        (midiTrack >= NUM_MIDI_TRACKS || v.midiTrack == midiTrack || v.midiTrack >= NUM_MIDI_TRACKS)) {
      v.gate = false;
      if (pedal) {
        v.sustained = true;
      } else if (!playToEndOnRelease) {
        v.ampStage = EnvStage::Release;
        v.fltStage = EnvStage::Release;
      }
    }
  }
}

static void applyCcInIsr(int instIdx, uint8_t cc, uint8_t val) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  val &= 0x7F;
  if (cc == 1) {
    instRt[instIdx].modWheelBoost = (float)val * (1.0f / 127.0f);
  } else if (cc == 64) {
    bool down = (val >= 64);
    const FmPatch &p = patches[instIdx];
    const bool isSampler = ((InstType)p.type == InstType::Sampler);
    const bool playToEndOnRelease = isSampler &&
        (p.algorithm == (uint8_t)SamplerPressMode::Once ||
         p.algorithm == (uint8_t)SamplerPressMode::RepThru);
    instRt[instIdx].sustainPedal = down;
    if (!down) {
      for (int i = 0; i < NUM_FM_VOICES; i++) {
        FmVoice &v = voices[i];
        if (v.active && v.instIdx == (uint8_t)instIdx && v.sustained) {
          v.sustained = false;
          if (!v.gate && !playToEndOnRelease) {
            v.ampStage = EnvStage::Release;
            v.fltStage = EnvStage::Release;
          }
        }
      }
    }
  } else if (cc == 120 || cc == 123) {
    applySilenceInstInIsr(instIdx);
  }
}

static void applyPitchBendInIsr(int instIdx, uint8_t lsb, uint8_t msb) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  int bend = ((int)(msb & 0x7F) << 7) | (int)(lsb & 0x7F);
  float norm = (float)(bend - 8192) * (1.0f / 8192.0f); // -1.0 .. +1.0 (+/- 2 semitones)
  instRt[instIdx].pitchBendRatio = fastExp2Small(norm * (2.0f / 12.0f));
}

static void fmSynthProcessQueuedEvents() {
  while (synthEvTail != synthEvHead) {
    uint32_t t = synthEvTail;
    SynthEvent ev = synthEvRing[t & SYNTH_EV_RING_MASK];
    __asm__ volatile("" ::: "memory");
    synthEvTail = t + 1;

    int instIdx = unpackSynthInst(ev.instIdx);
    uint8_t midiTrack = unpackSynthMidiTrack(ev.instIdx);
    switch ((SynthEvType)ev.type) {
      case SynthEvType::NoteOn:
        applyNoteOnInIsr(instIdx, ev.d1, ev.d2, ev.sampleOffset, ev.streamIdx, ev.streamGen, midiTrack);
        break;
      case SynthEvType::NoteOff:
        applyNoteOffInIsr(instIdx, ev.d1, midiTrack);
        break;
      case SynthEvType::ControlChange:
        applyCcInIsr(instIdx, ev.d1, ev.d2);
        break;
      case SynthEvType::PitchBend:
        applyPitchBendInIsr(instIdx, ev.d1, ev.d2);
        break;
    }
  }
}

bool fmSynthNoteOn(int instIdx, uint8_t note, uint8_t velocity, uint8_t sampleOffset,
                   bool allowSdIo, uint8_t midiTrack) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return true;
  const FmPatch &p = patches[instIdx];
  const InstType iType = (InstType)p.type;
  if (iType == InstType::None) return true;
  if (velocity == 0) {
    fmSynthNoteOff(instIdx, note, sampleOffset, midiTrack);
    return true;
  }
  if (synthEvHead - synthEvTail >= SYNTH_EV_RING_SIZE - 16) {
    return true;
  }

  uint32_t samplerStreamIdx = 0xFFu;
  uint32_t samplerStreamGen = 0;
  if (iType == InstType::Sampler) {
    int keyIdx = samplerNoteToKeyForInst(instIdx, note);
    if (keyIdx < 0 || !samplerHasSample(instIdx, keyIdx)) {
      return true;
    }
    if (!samplerPrepareVoiceOn(instIdx, note, samplerStreamIdx, samplerStreamGen, allowSdIo)) {
      return allowSdIo; // false when allowSdIo == false (ISR cache miss -> defer to main thread)
    }
  }
  if (allowSdIo) {
    refreshInstEnvelopeRates(instIdx, p);
  }

  enqueueSynthEvent(SynthEvType::NoteOn, packSynthInstTrack(instIdx, midiTrack),
                    note & 0x7Fu, velocity & 0x7Fu,
                    sampleOffset, (uint8_t)samplerStreamIdx, (uint16_t)samplerStreamGen);
  return true;
}

void fmSynthNoteOff(int instIdx, uint8_t note, uint8_t sampleOffset, uint8_t midiTrack) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  const FmPatch &p = patches[instIdx];
  const InstType iType = (InstType)p.type;
  if (iType == InstType::None) return;
  if (iType == InstType::NoiseDrone &&
      (p.algorithm == (uint8_t)NdTrigMode::Latch || p.algorithm == (uint8_t)NdTrigMode::Auto)) {
    return;
  }
  if (!enqueueSynthEvent(SynthEvType::NoteOff, packSynthInstTrack(instIdx, midiTrack),
                         note & 0x7Fu, 0, sampleOffset)) {
    uint32_t irq = saveIrq();
    applyNoteOffInIsr(instIdx, note & 0x7Fu, midiTrack);
    restoreIrq(irq);
  }
}

void fmSynthPitchBend(int instIdx, uint8_t lsb, uint8_t msb, uint8_t midiTrack) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  enqueueSynthEvent(SynthEvType::PitchBend, packSynthInstTrack(instIdx, midiTrack), lsb & 0x7Fu, msb & 0x7Fu);
}

void fmSynthCC(int instIdx, uint8_t cc, uint8_t val, uint8_t midiTrack) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  instMidiMapApplyRoutedCc(instIdx, cc & 0x7Fu, val & 0x7Fu);
  enqueueSynthEvent(SynthEvType::ControlChange, packSynthInstTrack(instIdx, midiTrack), cc & 0x7Fu, val & 0x7Fu);
}

FLASHMEM void fmSynthSilenceInst(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  noInterrupts();
  fmSynthProcessQueuedEvents();
  applySilenceInstInIsr(instIdx);
  interrupts();
}

FLASHMEM void fmSynthSilenceMidiTrack(int midiTrack) {
  if (midiTrack < 0 || midiTrack >= NUM_MIDI_TRACKS) return;
  noInterrupts();
  fmSynthProcessQueuedEvents();
  applySilenceMidiTrackInIsr(midiTrack);
  interrupts();
}

FLASHMEM void fmSynthAllNotesOff() {
  const bool keepAutoDrones = (transport.mode == TransportMode::Running);
  noInterrupts();
  fmSynthProcessQueuedEvents();
  latchedDroneMask = 0;
  if (!keepAutoDrones) {
    autoDroneMask = 0;
  }
  for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
    instRt[inst].sustainPedal = false;
    instRt[inst].pitchBendRatio = 1.0f;
    instRt[inst].modWheelBoost = 0.0f;
  }
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    uint8_t inst = voices[i].instIdx;
    if (keepAutoDrones && inst < NUM_INSTRUMENTS && (autoDroneMask & (uint8_t)(1u << inst)) != 0) {
      continue;
    }
    voices[i].gate = false;
    voices[i].sustained = false;
    voices[i].ampStage = EnvStage::Release;
    voices[i].fltStage = EnvStage::Release;
  }
  samplerStopAllStreams();
  interrupts();
  auditionInst = -1;
}

FLASHMEM void fmSynthAudition(int instIdx, uint8_t note, uint8_t velocity, uint32_t durationMs) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if (fmSynthIsSlotEmpty(instIdx)) return;
  const FmPatch &p = patches[instIdx];
  if ((InstType)p.type == InstType::NoiseDrone && p.algorithm == (uint8_t)NdTrigMode::Latch) {
    // In Latch mode, pressing EDIT toggles the latched drone ON or OFF without auto-note-off
    fmSynthNoteOn(instIdx, note, velocity);
    return;
  }
  if (auditionInst >= 0) {
    fmSynthNoteOff(auditionInst, auditionNote);
  }
  auditionInst = instIdx;
  auditionNote = note;
  auditionOffMs = millis() + durationMs;
  fmSynthNoteOn(instIdx, note, velocity);
}

FLASHMEM static bool isInstAudibleOnMidiTracks(int instIdx) {
  const uint8_t dest = (uint8_t)(17 + instIdx);
  bool anyRouted = false;
  const bool anySolo = anyTrackSolo();
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    if (midiTracks[t].outChannel == dest) {
      anyRouted = true;
      if (!midiTracks[t].muted && (!anySolo || midiTracks[t].solo)) {
        return true;
      }
    }
  }
  return !anyRouted && !anySolo;
}

static bool exportTailPhase = false;

FLASHMEM void fmSynthSetExportTailPhase(bool inTail) {
  exportTailPhase = inTail;
  if (inTail) {
    noInterrupts();
    fmSynthProcessQueuedEvents();
    uint8_t droneMask = latchedDroneMask | autoDroneMask;
    latchedDroneMask = 0;
    autoDroneMask = 0;
    for (int i = 0; i < NUM_FM_VOICES; i++) {
      uint8_t inst = voices[i].instIdx;
      if (voices[i].active && inst < NUM_INSTRUMENTS && (droneMask & (uint8_t)(1u << inst)) != 0) {
        voices[i].gate = false;
        voices[i].sustained = false;
        voices[i].ampStage = EnvStage::Release;
        voices[i].fltStage = EnvStage::Release;
      }
    }
    interrupts();
  }
}

FLASHMEM bool fmSynthHasActiveOutput() {
  if (synthEvHead != synthEvTail) return true;
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    if (voices[i].active) return true;
  }
  return instFxTailFrames > 0;
}

FLASHMEM void fmSynthService() {
  if (auditionInst >= 0 && (int32_t)(millis() - auditionOffMs) >= 0) {
    int aInst = auditionInst;
    uint8_t aNote = auditionNote;
    auditionInst = -1;
    const FmPatch &ap = patches[constrain(aInst, 0, NUM_INSTRUMENTS - 1)];
    if ((InstType)ap.type == InstType::NoiseDrone &&
        ap.algorithm == (uint8_t)NdTrigMode::Auto &&
        transport.mode != TransportMode::Running) {
      fmSynthSilenceInst(aInst);
    } else {
      fmSynthNoteOff(aInst, aNote);
    }
  }

  const bool running = (transport.mode == TransportMode::Running);
  for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
    const uint8_t bit = (uint8_t)(1u << inst);
    const FmPatch &p = patches[inst];
    refreshInstEnvelopeRates(inst, p);
    const bool isNd = ((InstType)p.type == InstType::NoiseDrone);

    if ((latchedDroneMask & bit) != 0 && (!isNd || p.algorithm != (uint8_t)NdTrigMode::Latch)) {
      fmSynthSilenceInst(inst);
    }

    const bool wantAuto = running && !exportTailPhase && isNd &&
                          (p.algorithm == (uint8_t)NdTrigMode::Auto) && isInstAudibleOnMidiTracks(inst);
    if (wantAuto) {
      const uint8_t dest = (uint8_t)(17 + inst);
      const bool anySolo = anyTrackSolo();
      const int prefTrk = midiSeqAudioRecTargetTrack();
      uint8_t autoMidiTrk = 0xFFu;
      if (prefTrk >= 0 && prefTrk < NUM_MIDI_TRACKS &&
          midiTracks[prefTrk].outChannel == dest &&
          !midiTracks[prefTrk].muted && (!anySolo || midiTracks[prefTrk].solo)) {
        autoMidiTrk = (uint8_t)prefTrk;
      } else {
        for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
          if (midiTracks[t].outChannel == dest &&
              !midiTracks[t].muted && (!anySolo || midiTracks[t].solo)) {
            autoMidiTrk = (uint8_t)t;
            break;
          }
        }
      }
      bool hasGated = ((autoDroneMask & bit) != 0);
      for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
        if (voices[vIdx].active && voices[vIdx].instIdx == (uint8_t)inst && voices[vIdx].gate) {
          hasGated = true;
          if (autoMidiTrk < NUM_MIDI_TRACKS) {
            voices[vIdx].midiTrack = autoMidiTrk;
          }
          break;
        }
      }
      if (!hasGated) {
        autoDroneMask |= bit;
        fmSynthNoteOn(inst, 60, 104, 0, true, autoMidiTrk);
      }
    } else if ((autoDroneMask & bit) != 0 && !exportTailPhase) {
      fmSynthSilenceInst(inst);
    }
  }

  samplerServiceStreams();
}

// ============================================================================
// Audio ISR Block Renderer — Dispatches Active Voices + General FX Bus
// ============================================================================
bool fmSynthRenderBlock(int32_t *outL, int32_t *outR, int numFrames,
                        int captureMidiTrack, int32_t *capOutL, int32_t *capOutR) {
  fmSynthProcessQueuedEvents();

  const bool wantCap = (captureMidiTrack >= 0 && captureMidiTrack < NUM_MIDI_TRACKS &&
                        capOutL != nullptr && capOutR != nullptr);
  if (wantCap) {
    memset(capOutL, 0, numFrames * sizeof(int32_t));
    memset(capOutR, 0, numFrames * sizeof(int32_t));
  }

  bool anyActive = false;
  for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
    if (voices[vIdx].active) {
      anyActive = true;
      break;
    }
  }
  if (!anyActive && instFxTailFrames <= 0) return false;

  memset(outL, 0, numFrames * sizeof(int32_t));
  memset(outR, 0, numFrames * sizeof(int32_t));

  int32_t fxSendL[AUDIO_BLOCK_SAMPLES];
  int32_t fxSendR[AUDIO_BLOCK_SAMPLES];
  int32_t capDryFxL[AUDIO_BLOCK_SAMPLES] = {};
  int32_t capDryFxR[AUDIO_BLOCK_SAMPLES] = {};
  bool anyFxSend = false;
  bool capUsedFxSend = false;
  bool otherUsedFxSend = false;
  uint8_t maxChor = 0, maxDly = 0, maxDfb = 0, maxVerb = 0;

  if (anyActive) {
    for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
      bool hasCapVoice = false;
      bool hasOtherVoice = false;
      for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
        if (voices[vIdx].active && voices[vIdx].instIdx == (uint8_t)inst) {
          if (wantCap && voices[vIdx].midiTrack == (uint8_t)captureMidiTrack) {
            hasCapVoice = true;
          } else {
            hasOtherVoice = true;
          }
        }
      }
      if (!hasCapVoice && !hasOtherVoice) continue;

      const FmPatch &p = patches[inst];
      const InstEngineOps *ops = instEngineForType((InstType)p.type);
      if (!ops || !ops->renderVoice) {
        for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
          if (voices[vIdx].instIdx == (uint8_t)inst) {
            voices[vIdx].active = false;
          }
        }
        continue;
      }

      const bool hasDrive = (p.fx.drive > 0);
      const bool hasBusFx = (p.fx.chorus > 0 || p.fx.delay > 0 || p.fx.reverb > 0);
      const FmInstRuntime &rt = instRt[inst];

      if (!hasCapVoice && !hasDrive && !hasBusFx) {
        for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
          FmVoice &v = voices[vIdx];
          if (v.active && v.instIdx == (uint8_t)inst) {
            ops->renderVoice(v, p, rt, outL, outR, numFrames);
          }
        }
        continue;
      }

      int32_t instL[AUDIO_BLOCK_SAMPLES] = {};
      int32_t instR[AUDIO_BLOCK_SAMPLES] = {};
      if (hasCapVoice && hasOtherVoice) {
        // Shared instrument across multiple MIDI tracks: render captured track separately
        int32_t capInstL[AUDIO_BLOCK_SAMPLES] = {};
        int32_t capInstR[AUDIO_BLOCK_SAMPLES] = {};
        for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
          FmVoice &v = voices[vIdx];
          if (v.active && v.instIdx == (uint8_t)inst) {
            if (v.midiTrack == (uint8_t)captureMidiTrack) {
              ops->renderVoice(v, p, rt, capInstL, capInstR, numFrames);
            } else {
              ops->renderVoice(v, p, rt, instL, instR, numFrames);
            }
          }
        }
        for (int i = 0; i < numFrames; i++) {
          instL[i] += capInstL[i];
          instR[i] += capInstR[i];
        }
        if (hasDrive) {
          const int capDrvSlot = NUM_INSTRUMENTS + inst;
          for (int i = 0; i < numFrames; i++) {
            float xL = (float)capInstL[i] * (1.0f / 32768.0f);
            float drvL = processInstDriveSample(capDrvSlot, p.fx.drive, xL);
            int32_t sL = (int32_t)(drvL * 32768.0f);
            if (capInstR[i] == capInstL[i]) {
              capInstL[i] = sL;
              capInstR[i] = sL;
            } else {
              float xR = (float)capInstR[i] * (1.0f / 32768.0f);
              capInstL[i] = sL;
              capInstR[i] = (int32_t)(processInstDriveSample(capDrvSlot, p.fx.drive, xR) * 32768.0f);
            }
          }
        }
        for (int i = 0; i < numFrames; i++) {
          capOutL[i] += capInstL[i];
          capOutR[i] += capInstR[i];
        }
      } else {
        for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
          FmVoice &v = voices[vIdx];
          if (v.active && v.instIdx == (uint8_t)inst) {
            ops->renderVoice(v, p, rt, instL, instR, numFrames);
          }
        }
      }

      if (hasDrive) {
        for (int i = 0; i < numFrames; i++) {
          float xL = (float)instL[i] * (1.0f / 32768.0f);
          float drvL = processInstDriveSample(inst, p.fx.drive, xL);
          int32_t sL = (int32_t)(drvL * 32768.0f);
          if (instR[i] == instL[i]) {
            instL[i] = sL;
            instR[i] = sL;
          } else {
            float xR = (float)instR[i] * (1.0f / 32768.0f);
            instL[i] = sL;
            instR[i] = (int32_t)(processInstDriveSample(inst, p.fx.drive, xR) * 32768.0f);
          }
        }
      }

      if (hasBusFx) {
        if (!anyFxSend) {
          memset(fxSendL, 0, numFrames * sizeof(int32_t));
          memset(fxSendR, 0, numFrames * sizeof(int32_t));
          anyFxSend = true;
        }
        if (hasCapVoice && !hasOtherVoice) {
          capUsedFxSend = true;
          for (int i = 0; i < numFrames; i++) {
            capDryFxL[i] += instL[i];
            capDryFxR[i] += instR[i];
          }
        } else {
          otherUsedFxSend = true;
        }
        lastFxInst = (int8_t)inst;
        if (p.fx.chorus > maxChor) maxChor = p.fx.chorus;
        if (p.fx.delay > maxDly) maxDly = p.fx.delay;
        if (p.fx.delayFeedback > maxDfb) maxDfb = p.fx.delayFeedback;
        if (p.fx.reverb > maxVerb) maxVerb = p.fx.reverb;
        if (p.fx.delay > 0 || p.fx.reverb > 0) {
          int32_t dlyTail = (p.fx.delay > 0)
                                ? (int32_t)(SAMPLE_RATE_HZ * (0.6f + 0.044f * (float)p.fx.delayFeedback))
                                : 0;
          int32_t revTail = (p.fx.reverb > 0)
                                ? (int32_t)(SAMPLE_RATE_HZ * (0.8f + 0.082f * (float)p.fx.reverb))
                                : 0;
          int32_t reqTail = max(dlyTail, revTail);
          if (reqTail > instFxTailFrames) instFxTailFrames = reqTail;
        } else if (instFxTailFrames < numFrames) {
          instFxTailFrames = numFrames;
        }
        instFxSilentBlocks = 0;
        for (int i = 0; i < numFrames; i++) {
          fxSendL[i] += instL[i];
          fxSendR[i] += instR[i];
        }
      } else {
        for (int i = 0; i < numFrames; i++) {
          outL[i] += instL[i];
          outR[i] += instR[i];
        }
        if (hasCapVoice && !hasOtherVoice) {
          for (int i = 0; i < numFrames; i++) {
            capOutL[i] += instL[i];
            capOutR[i] += instR[i];
          }
        }
      }
    }
  }

  if (anyFxSend || instFxTailFrames > 0) {
    if (anyFxSend) {
      lastFxMidiTrack = (capUsedFxSend && !otherUsedFxSend) ? (int8_t)captureMidiTrack : (int8_t)-1;
    }
    const bool routeFxToCap = wantCap &&
                              ((capUsedFxSend && !otherUsedFxSend) ||
                               (!anyFxSend && lastFxMidiTrack == (int8_t)captureMidiTrack));
    const bool routeDryFxToCap = wantCap && capUsedFxSend && otherUsedFxSend;

    if (lastFxInst >= 0 && lastFxInst < NUM_INSTRUMENTS && !fmSynthIsSlotEmpty(lastFxInst)) {
      const InstFxParams &tfx = patches[lastFxInst].fx;
      if (tfx.chorus > maxChor) maxChor = tfx.chorus;
      if (tfx.delay > maxDly) maxDly = tfx.delay;
      if (tfx.delayFeedback > maxDfb) maxDfb = tfx.delayFeedback;
      if (tfx.reverb > maxVerb) maxVerb = tfx.reverb;
    }
    setInstFxParams(maxChor, maxDly, maxDfb, maxVerb);

    float tailPeak = 0.0f;
    if (!isInstFxChainEnabled()) {
      if (anyFxSend) {
        for (int i = 0; i < numFrames; i++) {
          outL[i] += fxSendL[i];
          outR[i] += fxSendR[i];
          if (routeFxToCap) {
            capOutL[i] += fxSendL[i];
            capOutR[i] += fxSendR[i];
          }
        }
      }
    } else {
      for (int i = 0; i < numFrames; i++) {
        float xL = anyFxSend ? ((float)fxSendL[i] * (1.0f / 32768.0f)) : 0.0f;
        float xR = anyFxSend ? ((float)fxSendR[i] * (1.0f / 32768.0f)) : 0.0f;
        processInstFxSample(xL, xR);
        float absSum = fabsf(xL) + fabsf(xR);
        if (absSum > tailPeak) tailPeak = absSum;
        int32_t wetL = (int32_t)(xL * 32768.0f);
        int32_t wetR = (int32_t)(xR * 32768.0f);
        outL[i] += wetL;
        outR[i] += wetR;
        if (routeFxToCap) {
          capOutL[i] += wetL;
          capOutR[i] += wetR;
        }
      }
    }
    if (routeDryFxToCap) {
      for (int i = 0; i < numFrames; i++) {
        capOutL[i] += capDryFxL[i];
        capOutR[i] += capDryFxR[i];
      }
    }
    if (!anyFxSend) {
      instFxTailFrames = max((int32_t)0, instFxTailFrames - (int32_t)numFrames);
      if (tailPeak < 1.5e-4f) {
        // Allow at least 128 blocks (~370 ms) when Delay is active so a 327 ms delay tap isn't gated early
        int32_t gateBlocks = (maxDly > 0) ? 132 : 8;
        if (++instFxSilentBlocks >= gateBlocks) {
          instFxTailFrames = 0;
        }
      } else {
        instFxSilentBlocks = 0;
      }
      if (instFxTailFrames == 0) {
        instFxSilentBlocks = 0;
        lastFxMidiTrack = -1;
        setInstFxParams(0, 0, 0, 0);
      }
    }
  }

  return true;
}

// ============================================================================
// SD Card Persistence (`INST.DAT` v3 in song folder + `/Capstan/INSTRUMENTS/<NAME>.FMI`)
// ============================================================================
struct DiskInstFile {
  char magic[4];        // "M8FM"
  uint16_t version;     // 3
  uint16_t count;       // NUM_INSTRUMENTS (8)
  FmPatch patches[NUM_INSTRUMENTS];
};
static_assert(sizeof(DiskInstFile) <= 512, "DiskInstFile must fit in one 512-byte sector");

// Legacy v2 structures for seamless upgrade of existing saved songs
struct LegacyFmOpParamsV2 {
  uint8_t wave;
  uint8_t level;
  uint16_t ratioX100;
  uint8_t feedback;
  uint8_t attack;
  uint8_t decay;
  uint8_t sustain;
  uint8_t release;
  uint8_t velocity;
  uint8_t reserved[2];
};
struct LegacyFmPatchV2 {
  char name[MAX_SONG_NAME_LEN + 1];
  uint8_t type;
  uint8_t preset;
  uint8_t reserved;
  LegacyFmOpParamsV2 op[2];
};
struct LegacyDiskInstFileV2 {
  char magic[4];
  uint16_t version;
  uint16_t count;
  LegacyFmPatchV2 patches[NUM_INSTRUMENTS];
};

FLASHMEM static uint8_t scalePctTo127(uint8_t pct) {
  return (uint8_t)constrain(((int)constrain((int)pct, 0, 100) * 127 + 50) / 100, 0, 127);
}

FLASHMEM static void validateAndClampPatch(FmPatch &p, int slotIdx) {
  InstType iType = (InstType)p.type;
  const InstEngineOps *ops = instEngineForType(iType);
  if (!ops) {
    memset(&p, 0, sizeof(FmPatch));
    p.type = (uint8_t)InstType::None;
    return;
  }

  char clean[MAX_SONG_NAME_LEN + 1];
  sanitizeInstName(p.name, clean, sizeof(clean), slotIdx, iType);
  snprintf(p.name, sizeof(p.name), "%s", clean);
  p.preset = 0;

  if (ops->validatePatch) {
    ops->validatePatch(p);
  }

  p.amp.amt     = (uint8_t)constrain((int)p.amp.amt,     0, (int)INST_ENV_MAX);
  p.amp.attack  = (uint8_t)constrain((int)p.amp.attack,  0, (int)INST_ENV_MAX);
  p.amp.decay   = (uint8_t)constrain((int)p.amp.decay,   0, (int)INST_ENV_MAX);
  p.amp.sustain = (uint8_t)constrain((int)p.amp.sustain, 0, (int)INST_ENV_MAX);
  p.amp.release = (uint8_t)constrain((int)p.amp.release, 0, (int)INST_ENV_MAX);

  p.flt.amt     = (uint8_t)constrain((int)p.flt.amt,     0, (int)INST_ENV_MAX);
  p.flt.attack  = (uint8_t)constrain((int)p.flt.attack,  0, (int)INST_ENV_MAX);
  p.flt.decay   = (uint8_t)constrain((int)p.flt.decay,   0, (int)INST_ENV_MAX);
  p.flt.sustain = (uint8_t)constrain((int)p.flt.sustain, 0, (int)INST_ENV_MAX);
  p.flt.release = (uint8_t)constrain((int)p.flt.release, 0, (int)INST_ENV_MAX);

  p.fx.drive         = (uint8_t)constrain((int)p.fx.drive,         0, (int)INST_FX_MAX);
  p.fx.chorus        = (uint8_t)constrain((int)p.fx.chorus,        0, (int)INST_FX_MAX);
  p.fx.delay         = (uint8_t)constrain((int)p.fx.delay,         0, (int)INST_FX_MAX);
  p.fx.delayFeedback = (uint8_t)constrain((int)p.fx.delayFeedback, 0, (int)INST_FX_MAX);
  p.fx.reverb        = (uint8_t)constrain((int)p.fx.reverb,        0, (int)INST_FX_MAX);
}

FLASHMEM bool fmSynthSaveSong(FsFile &f) {
  if (!f.isOpen()) return false;
  // Stage in RAM1 stack struct for DMA_SDIO cache safety
  DiskInstFile df{};
  memcpy(df.magic, "M8FM", 4);
  df.version = 3;
  df.count = NUM_INSTRUMENTS;
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    df.patches[i] = patches[i];
  }
  if (!f.seekSet(0)) return false;
  if (f.write(&df, sizeof(df)) != (int)sizeof(df)) return false;
  if (!samplerSaveSongData(f)) return false;
  if (!instMidiMapSaveSongData(f)) return false;
  f.truncate();
  f.sync();
  return true;
}

FLASHMEM bool fmSynthLoadSong(FsFile &f) {
  instMidiMapResetAll();
  if (!f.isOpen() || !f.seekSet(0)) return false;
  DiskInstFile df{};
  int rd = f.read(&df, sizeof(df));
  if (rd < 8 || memcmp(df.magic, "M8FM", 4) != 0) {
    return false;
  }
  if (df.version == 3) {
    int n = min((int)df.count, NUM_INSTRUMENTS);
    for (int i = 0; i < n; i++) {
      patches[i] = df.patches[i];
      validateAndClampPatch(patches[i], i);
    }
    samplerLoadSongData(f);
    instMidiMapLoadSongData(f);
    return true;
  }
  if (df.version == 2) {
    LegacyDiskInstFileV2 v2{};
    memcpy(&v2, &df, min(sizeof(v2), sizeof(df)));
    int n = min((int)v2.count, NUM_INSTRUMENTS);
    for (int i = 0; i < n; i++) {
      const LegacyFmPatchV2 &src = v2.patches[i];
      FmPatch dst{};
      memcpy(dst.name, src.name, sizeof(dst.name));
      dst.type = src.type;
      dst.preset = src.preset;
      dst.algorithm = (src.type == (uint8_t)InstType::Fm2Op) ? (uint8_t)FmAlgo::Fm : 0;
      for (int k = 0; k < 2; k++) {
        dst.op[k].wave = src.op[k].wave;
        dst.op[k].level = src.op[k].level;
        dst.op[k].ratioX100 = src.op[k].ratioX100;
        dst.op[k].feedback = src.op[k].feedback;
      }
      int ampIdx = (src.type == (uint8_t)InstType::PhaseDist) ? 1 : 0;
      int fltIdx = 1 - ampIdx;
      dst.amp = { 127,
                  scalePctTo127(src.op[ampIdx].attack),
                  scalePctTo127(src.op[ampIdx].decay),
                  scalePctTo127(src.op[ampIdx].sustain),
                  scalePctTo127(src.op[ampIdx].release) };
      dst.flt = { 127,
                  scalePctTo127(src.op[fltIdx].attack),
                  scalePctTo127(src.op[fltIdx].decay),
                  scalePctTo127(src.op[fltIdx].sustain),
                  scalePctTo127(src.op[fltIdx].release) };
      dst.fx = { 0, 0, 0, 0, 0 };
      patches[i] = dst;
      validateAndClampPatch(patches[i], i);
    }
    return true;
  }
  return false;
}

struct SingleInstDisk {
  char magic[4];
  uint16_t version;
  uint16_t reserved;
  FmPatch patch;
};

FLASHMEM static bool saveInstrumentFileToDir(int instIdx, const char *dirPath) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !dirPath || !dirPath[0]) return false;
  if (fmSynthIsSlotEmpty(instIdx)) return false;

  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  if (!SD.sdfs.exists(dirPath)) {
    SD.sdfs.mkdir(dirPath);
  }

  InstType iType = (InstType)patches[instIdx].type;
  char clean[MAX_SONG_NAME_LEN + 1];
  sanitizeInstName(patches[instIdx].name, clean, sizeof(clean), instIdx, iType);
  snprintf(patches[instIdx].name, sizeof(patches[instIdx].name), "%s", clean);

  char path[128];
  snprintf(path, sizeof(path), "%s/%s.FMI", dirPath, clean);
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return false;

  // Stage in RAM1 stack struct for DMA_SDIO cache safety
  SingleInstDisk sid{};
  memcpy(sid.magic, "M8FI", 4);
  sid.version = 3;
  sid.patch = patches[instIdx];

  bool ok = (f.write(&sid, sizeof(sid)) == (int)sizeof(sid));
  if (ok) {
    ok = samplerSaveSingleInstData(f, instIdx);
  }
  if (ok) {
    ok = instMidiMapSaveSingleInstData(f, instIdx);
  }
  f.truncate();
  f.sync();
  f.close();
  if (SD.sdfs.card()) {
    SD.sdfs.card()->syncDevice();
  }
  return ok;
}

FLASHMEM bool fmSynthSaveInstrumentToSong(int instIdx) {
  const char *song = currentSongName();
  if (!song || !song[0]) song = "INIT";
  char songDir[64];
  snprintf(songDir, sizeof(songDir), "%s/%s", SONGS_DIR, song);
  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  if (!SD.sdfs.exists(SONGS_DIR)) {
    SD.sdfs.mkdir(SONGS_DIR);
  }
  if (!SD.sdfs.exists(songDir)) {
    SD.sdfs.mkdir(songDir);
  }
  char instDir[80];
  snprintf(instDir, sizeof(instDir), "%s/INSTRUMENTS", songDir);
  return saveInstrumentFileToDir(instIdx, instDir);
}

FLASHMEM bool fmSynthSaveInstrumentPreset(int instIdx) {
  return saveInstrumentFileToDir(instIdx, INST_PRESETS_DIR);
}

FLASHMEM bool fmSynthLoadInstrumentFile(int instIdx, const char *fullPath) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !fullPath || !fullPath[0]) return false;
  FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
  if (!f.isOpen()) return false;

  // Stage in RAM1 stack struct for DMA_SDIO cache safety
  SingleInstDisk sid{};
  int rd = f.read(&sid, sizeof(sid));
  if (rd != (int)sizeof(sid) || memcmp(sid.magic, "M8FI", 4) != 0 || sid.version != 3) {
    f.close();
    return false;
  }
  if (instEngineForType((InstType)sid.patch.type) == nullptr) {
    f.close();
    return false;
  }

  fmSynthSilenceInst(instIdx);
  resetInstDriveState(instIdx);
  patches[instIdx] = sid.patch;
  validateAndClampPatch(patches[instIdx], instIdx);
  if ((InstType)patches[instIdx].type == InstType::Sampler) {
    samplerLoadSingleInstData(f, instIdx);
  } else {
    samplerClearSlot(instIdx);
  }
  instMidiMapLoadSingleInstData(f, instIdx);
  f.close();
  return true;
}

// ============================================================================
// SD Card Instrument File Browser Modal (`.FMI` files, 32-Entry Sliding Window)
// ============================================================================
FLASHMEM static bool hasFmiExtension(const char *name) {
  if (!name) return false;
  size_t len = strlen(name);
  if (len < 5) return false;
  return strcasecmp(name + len - 4, ".fmi") == 0;
}

static inline bool instDirEntryComesBefore(const InstBrowserEntry &a, const InstBrowserEntry &b) {
  bool aParent = (a.isDir != 0) && (strcmp(a.name, "..") == 0);
  bool bParent = (b.isDir != 0) && (strcmp(b.name, "..") == 0);
  if (aParent != bParent) return aParent;
  if ((a.isDir != 0) != (b.isDir != 0)) return a.isDir != 0;
  int cmp = strcasecmp(a.name, b.name);
  if (cmp != 0) return cmp < 0;
  return strcmp(a.name, b.name) < 0;
}

static inline bool instPresetEntryComesBefore(const InstBrowserEntry &a, const InstBrowserEntry &b) {
  int cmp = strcasecmp(a.instName, b.instName);
  if (cmp != 0) return cmp < 0;
  cmp = strcasecmp(a.name, b.name);
  if (cmp != 0) return cmp < 0;
  return strcmp(a.name, b.name) < 0;
}

static inline bool instEntryComesBefore(const InstBrowserEntry &a, const InstBrowserEntry &b, bool presetMode) {
  return presetMode ? instPresetEntryComesBefore(a, b) : instDirEntryComesBefore(a, b);
}

FLASHMEM static void insertInstSmallest(InstBrowserEntry *arr, int &count, int cap,
                                        const InstBrowserEntry &item, bool presetMode) {
  if (cap <= 0) return;
  if (count >= cap && !instEntryComesBefore(item, arr[cap - 1], presetMode)) return;
  int ins = (count < cap) ? count++ : (cap - 1);
  while (ins > 0 && instEntryComesBefore(item, arr[ins - 1], presetMode)) {
    arr[ins] = arr[ins - 1];
    ins--;
  }
  arr[ins] = item;
}

FLASHMEM static void insertInstLargest(InstBrowserEntry *arr, int &count, int cap,
                                       const InstBrowserEntry &item, bool presetMode) {
  if (cap <= 0) return;
  if (count >= cap && !instEntryComesBefore(arr[0], item, presetMode)) return;
  if (count < cap) {
    int ins = count++;
    while (ins > 0 && instEntryComesBefore(item, arr[ins - 1], presetMode)) {
      arr[ins] = arr[ins - 1];
      ins--;
    }
    arr[ins] = item;
  } else {
    int ins = 0;
    while (ins + 1 < cap && instEntryComesBefore(arr[ins + 1], item, presetMode)) {
      arr[ins] = arr[ins + 1];
      ins++;
    }
    arr[ins] = item;
  }
}

FLASHMEM static void scanInstBrowserDirectoryWindow(const InstBrowserEntry *pivot, int pivotIdx, int wantStart,
                                                    const char *matchFile = nullptr, int *outMatchIdx = nullptr,
                                                    InstBrowserEntry *outMatchEntry = nullptr) {
  instBrowserPresetMode = false;
  if (outMatchIdx) *outMatchIdx = -1;

  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  if (!SD.sdfs.exists(INST_PRESETS_DIR)) {
    SD.sdfs.mkdir(INST_PRESETS_DIR);
  }
  if (!SD.sdfs.exists(instBrowserDir)) {
    snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", INST_PRESETS_DIR);
  }

  InstBrowserEntry pivotCopy{};
  const bool usePivot = (pivot != nullptr);
  int pOff = 0;
  int leftCap = 0;
  int rightCap = MAX_INST_BROWSER_ENTRIES;
  int leftCount = 0;
  int rightCount = 0;
  int totalCount = 0;

  if (usePivot) {
    pivotCopy = *pivot;
    pOff = constrain(pivotIdx - wantStart, 0, MAX_INST_BROWSER_ENTRIES - 1);
    leftCap = pOff;
    rightCap = (MAX_INST_BROWSER_ENTRIES - 1) - pOff;
  }

  InstBrowserEntry matchTarget{};
  const bool hasMatchTarget = (matchFile != nullptr && matchFile[0] != '\0' &&
                               strlen(matchFile) < sizeof(matchTarget.name));
  int matchRank = 0;
  bool matchFound = false;
  if (hasMatchTarget) {
    snprintf(matchTarget.name, sizeof(matchTarget.name), "%s", matchFile);
    matchTarget.isDir = 0;
  }

  auto considerItem = [&](const InstBrowserEntry &item) {
    totalCount++;
    if (hasMatchTarget) {
      if (!item.isDir && strcasecmp(item.name, matchTarget.name) == 0) {
        matchFound = true;
        if (outMatchEntry) *outMatchEntry = item;
      } else if (instDirEntryComesBefore(item, matchTarget)) {
        matchRank++;
      }
    }
    if (!usePivot) {
      insertInstSmallest(instBrowserEntries, rightCount, rightCap, item, false);
    } else if (instDirEntryComesBefore(item, pivotCopy)) {
      insertInstLargest(instBrowserEntries, leftCount, leftCap, item, false);
    } else if (instDirEntryComesBefore(pivotCopy, item)) {
      insertInstSmallest(&instBrowserEntries[pOff + 1], rightCount, rightCap, item, false);
    }
  };

  // Add ".." parent folder entry whenever not at SD root "/"
  if (strcmp(instBrowserDir, "/") != 0) {
    InstBrowserEntry parentItem{};
    snprintf(parentItem.name, sizeof(parentItem.name), "..");
    parentItem.isDir = 1;
    considerItem(parentItem);
  }

  FsFile dir = SD.sdfs.open(instBrowserDir, O_RDONLY);
  if (dir.isOpen() && dir.isDir()) {
    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
      char nameBuf[64] = {};
      entry.getName(nameBuf, sizeof(nameBuf));
      bool isSubDir = entry.isDir();
      uint64_t fSize = isSubDir ? 0 : entry.fileSize();
      bool isHidden = entry.isHidden() || (nameBuf[0] == '.') ||
                      (strcasecmp(nameBuf, "System Volume Information") == 0);
      entry.close();

      if (isHidden || nameBuf[0] == '\0') continue;
      if (!isSubDir && (!hasFmiExtension(nameBuf) || fSize < sizeof(SingleInstDisk))) continue;
      if (strlen(nameBuf) >= sizeof(instBrowserEntries[0].name)) continue;

      InstBrowserEntry item{};
      snprintf(item.name, sizeof(item.name), "%s", nameBuf);
      item.isDir = isSubDir ? 1 : 0;
      considerItem(item);
      keepStreaming();
    }
  }
  if (dir.isOpen()) dir.close();

  instBrowserCount = totalCount;
  if (!usePivot) {
    instBrowserWinStart = 0;
    instBrowserWinCount = rightCount;
  } else {
    instBrowserEntries[leftCount] = pivotCopy;
    if (leftCount < pOff && rightCount > 0) {
      memmove(&instBrowserEntries[leftCount + 1], &instBrowserEntries[pOff + 1],
              (size_t)rightCount * sizeof(InstBrowserEntry));
    }
    instBrowserWinStart = max(0, pivotIdx - leftCount);
    instBrowserWinCount = leftCount + 1 + rightCount;
  }

  if (matchFound && outMatchIdx) {
    *outMatchIdx = matchRank;
  }
}

FLASHMEM static void scanInstBrowserDirectory() {
  instBrowserCur = 0;
  instBrowserScr = 0;
  scanInstBrowserDirectoryWindow(nullptr, 0, 0);
}

FLASHMEM static void getSongInstrumentsDir(char *outDir, size_t outCap) {
  const char *song = currentSongName();
  if (!song || !song[0]) song = "INIT";
  snprintf(outDir, outCap, "%s/%s/INSTRUMENTS", SONGS_DIR, song);
}

FLASHMEM static void scanPresetsDirectoryWindow(const char *dirPath, bool createIfMissing,
                                                const InstBrowserEntry *pivot, int pivotIdx, int wantStart) {
  instBrowserPresetMode = true;
  if (!dirPath || !dirPath[0]) {
    instBrowserCount = 0;
    instBrowserWinStart = 0;
    instBrowserWinCount = 0;
    return;
  }
  if (createIfMissing) {
    if (!SD.sdfs.exists(ROOT_DIR)) {
      SD.sdfs.mkdir(ROOT_DIR);
    }
    if (!SD.sdfs.exists(dirPath)) {
      SD.sdfs.mkdir(dirPath);
    }
  } else if (!SD.sdfs.exists(dirPath)) {
    instBrowserCount = 0;
    instBrowserWinStart = 0;
    instBrowserWinCount = 0;
    return;
  }

  FsFile dir = SD.sdfs.open(dirPath, O_RDONLY);
  if (!dir.isOpen() || !dir.isDir()) {
    if (dir.isOpen()) dir.close();
    instBrowserCount = 0;
    instBrowserWinStart = 0;
    instBrowserWinCount = 0;
    return;
  }

  InstBrowserEntry pivotCopy{};
  const bool usePivot = (pivot != nullptr);
  int pOff = 0;
  int leftCap = 0;
  int rightCap = MAX_INST_BROWSER_ENTRIES;
  int leftCount = 0;
  int rightCount = 0;
  int totalCount = 0;

  if (usePivot) {
    pivotCopy = *pivot;
    pOff = constrain(pivotIdx - wantStart, 0, MAX_INST_BROWSER_ENTRIES - 1);
    leftCap = pOff;
    rightCap = (MAX_INST_BROWSER_ENTRIES - 1) - pOff;
  }

  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    char nameBuf[64] = {};
    entry.getName(nameBuf, sizeof(nameBuf));
    bool isSubDir = entry.isDir();
    uint64_t fSize = isSubDir ? 0 : entry.fileSize();
    bool isHidden = entry.isHidden() || (nameBuf[0] == '.');

    SingleInstDisk sid{};
    bool validFmi = false;
    if (!isSubDir && !isHidden && hasFmiExtension(nameBuf) &&
        fSize >= sizeof(SingleInstDisk) && strlen(nameBuf) < sizeof(instBrowserEntries[0].name)) {
      if (entry.read(&sid, sizeof(sid)) == (int)sizeof(sid) &&
          memcmp(sid.magic, "M8FI", 4) == 0 && sid.version == 3 &&
          instEngineForType((InstType)sid.patch.type) != nullptr) {
        validFmi = true;
      }
    }
    entry.close();

    if (!validFmi) continue;

    InstBrowserEntry item{};
    snprintf(item.name, sizeof(item.name), "%s", nameBuf);
    item.isDir = 0;
    item.instType = sid.patch.type;
    sanitizeInstName(sid.patch.name, item.instName, sizeof(item.instName), 0, (InstType)sid.patch.type);

    totalCount++;
    if (!usePivot) {
      insertInstSmallest(instBrowserEntries, rightCount, rightCap, item, true);
    } else if (instPresetEntryComesBefore(item, pivotCopy)) {
      insertInstLargest(instBrowserEntries, leftCount, leftCap, item, true);
    } else if (instPresetEntryComesBefore(pivotCopy, item)) {
      insertInstSmallest(&instBrowserEntries[pOff + 1], rightCount, rightCap, item, true);
    }
    keepStreaming();
  }
  dir.close();

  instBrowserCount = totalCount;
  if (!usePivot) {
    instBrowserWinStart = 0;
    instBrowserWinCount = rightCount;
  } else {
    instBrowserEntries[leftCount] = pivotCopy;
    if (leftCount < pOff && rightCount > 0) {
      memmove(&instBrowserEntries[leftCount + 1], &instBrowserEntries[pOff + 1],
              (size_t)rightCount * sizeof(InstBrowserEntry));
    }
    instBrowserWinStart = max(0, pivotIdx - leftCount);
    instBrowserWinCount = leftCount + 1 + rightCount;
  }
}

FLASHMEM static void syncInstBrowserScroll() {
  if (instBrowserCount <= 0) {
    instBrowserCur = 0;
    instBrowserScr = 0;
    instBrowserWinStart = 0;
    instBrowserWinCount = 0;
    return;
  }
  instBrowserCur = constrain(instBrowserCur, 0, instBrowserCount - 1);
  if (instBrowserCur < instBrowserScr) {
    instBrowserScr = instBrowserCur;
  } else if (instBrowserCur >= instBrowserScr + INST_BROWSER_VISIBLE_ROWS) {
    instBrowserScr = instBrowserCur - INST_BROWSER_VISIBLE_ROWS + 1;
  }
  int maxScroll = max(0, instBrowserCount - INST_BROWSER_VISIBLE_ROWS);
  instBrowserScr = constrain(instBrowserScr, 0, maxScroll);

  int visEnd = min(instBrowserCount - 1, instBrowserScr + INST_BROWSER_VISIBLE_ROWS - 1);
  if (instBrowserScr < instBrowserWinStart || visEnd >= instBrowserWinStart + instBrowserWinCount) {
    int maxWinStart = max(0, instBrowserCount - MAX_INST_BROWSER_ENTRIES);
    int wantStart = constrain(instBrowserScr - 12, 0, maxWinStart);
    const InstBrowserEntry *pivotPtr = nullptr;
    InstBrowserEntry pivot{};
    int pivotIdx = 0;
    if (wantStart > 0) {
      int overlapLo = max(instBrowserWinStart, wantStart);
      int overlapHi = min(instBrowserWinStart + instBrowserWinCount - 1, wantStart + MAX_INST_BROWSER_ENTRIES - 1);
      if (instBrowserWinCount > 0 && overlapLo <= overlapHi) {
        pivotIdx = (wantStart < instBrowserWinStart) ? overlapHi : overlapLo;
        pivot = instBrowserEntries[pivotIdx - instBrowserWinStart];
        pivotPtr = &pivot;
      } else {
        wantStart = 0;
      }
    }
    if (instBrowserPresetMode) {
      bool useGlobal = sysPresetsModalOpen ? !sysPresetsSongMode : typePresetIsGlobal;
      if (useGlobal) {
        scanPresetsDirectoryWindow(INST_PRESETS_DIR, false, pivotPtr, pivotIdx, wantStart);
      } else {
        char songDir[80];
        getSongInstrumentsDir(songDir, sizeof(songDir));
        scanPresetsDirectoryWindow(songDir, false, pivotPtr, pivotIdx, wantStart);
      }
    } else {
      scanInstBrowserDirectoryWindow(pivotPtr, pivotIdx, wantStart);
    }
  }
}

FLASHMEM bool isInstBrowserOpen() {
  return instBrowserModalOpen;
}

FLASHMEM int instBrowserTargetSlot() {
  return instBrowserSlot;
}

FLASHMEM const char *instBrowserCurrentDir() {
  return instBrowserDir;
}

FLASHMEM int instBrowserEntryCount() {
  return instBrowserCount;
}

FLASHMEM int instBrowserCursor() {
  return instBrowserCur;
}

FLASHMEM int instBrowserScroll() {
  return instBrowserScr;
}

FLASHMEM const InstBrowserEntry *instBrowserEntryAt(int idx) {
  if (idx < instBrowserWinStart || idx >= instBrowserWinStart + instBrowserWinCount) return nullptr;
  return &instBrowserEntries[idx - instBrowserWinStart];
}

FLASHMEM void openInstBrowser(int instIdx) {
  instBrowserSlot = constrain(instIdx, 0, NUM_INSTRUMENTS - 1);

  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  if (!SD.sdfs.exists(INST_PRESETS_DIR)) {
    SD.sdfs.mkdir(INST_PRESETS_DIR);
  }

  const char *song = currentSongName();
  if (!song || !song[0]) song = "INIT";
  char songInstDir[80];
  snprintf(songInstDir, sizeof(songInstDir), "%s/%s/INSTRUMENTS", SONGS_DIR, song);

  if (instBrowserDir[0] == '\0' || !SD.sdfs.exists(instBrowserDir)) {
    if (SD.sdfs.exists(songInstDir)) {
      snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", songInstDir);
    } else {
      snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", INST_PRESETS_DIR);
    }
  }

  char targetFile[24] = {};
  if (!fmSynthIsSlotEmpty(instBrowserSlot) && patches[instBrowserSlot].name[0] != '\0') {
    snprintf(targetFile, sizeof(targetFile), "%s.FMI", patches[instBrowserSlot].name);
  }

  int matchedIdx = -1;
  InstBrowserEntry matchedEntry{};
  instBrowserCur = 0;
  instBrowserScr = 0;
  scanInstBrowserDirectoryWindow(nullptr, 0, 0, targetFile[0] ? targetFile : nullptr, &matchedIdx, &matchedEntry);

  // If the current song's INSTRUMENTS folder is empty, automatically check INSTRUMENT_PRESETS
  if (instBrowserCount <= 1 && strcasecmp(instBrowserDir, songInstDir) == 0 &&
      SD.sdfs.exists(INST_PRESETS_DIR)) {
    snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", INST_PRESETS_DIR);
    scanInstBrowserDirectoryWindow(nullptr, 0, 0, targetFile[0] ? targetFile : nullptr, &matchedIdx, &matchedEntry);
  } else if (instBrowserCount <= 1 && strcasecmp(instBrowserDir, INST_PRESETS_DIR) == 0 &&
             SD.sdfs.exists(songInstDir)) {
    snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", songInstDir);
    scanInstBrowserDirectoryWindow(nullptr, 0, 0, targetFile[0] ? targetFile : nullptr, &matchedIdx, &matchedEntry);
    if (instBrowserCount <= 1) {
      snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", INST_PRESETS_DIR);
      scanInstBrowserDirectoryWindow(nullptr, 0, 0, targetFile[0] ? targetFile : nullptr, &matchedIdx, &matchedEntry);
    }
  }

  // Highlight the current slot's instrument file if present in the directory
  if (matchedIdx >= 0) {
    instBrowserCur = matchedIdx;
    if (matchedIdx >= instBrowserWinCount) {
      int maxScroll = max(0, instBrowserCount - INST_BROWSER_VISIBLE_ROWS);
      instBrowserScr = constrain(instBrowserCur - INST_BROWSER_VISIBLE_ROWS + 1, 0, maxScroll);
      int maxWinStart = max(0, instBrowserCount - MAX_INST_BROWSER_ENTRIES);
      int wantStart = constrain(instBrowserScr - 12, 0, maxWinStart);
      scanInstBrowserDirectoryWindow(&matchedEntry, matchedIdx, wantStart);
    }
  } else if (instBrowserCount > 1 && instBrowserWinCount > 0 && strcmp(instBrowserEntries[0].name, "..") == 0) {
    instBrowserCur = 1;
  }
  syncInstBrowserScroll();
  instBrowserModalOpen = true;
}

FLASHMEM void closeInstBrowser() {
  instBrowserModalOpen = false;
}

FLASHMEM void instBrowserMove(int dy, int pageDelta) {
  if (!instBrowserModalOpen || instBrowserCount <= 0) return;
  int step = dy + pageDelta * INST_BROWSER_VISIBLE_ROWS;
  if (step == 0) return;
  instBrowserCur = constrain(instBrowserCur + step, 0, instBrowserCount - 1);
  syncInstBrowserScroll();
}

FLASHMEM bool instBrowserConfirm() {
  if (!instBrowserModalOpen || instBrowserCount <= 0) return false;
  const int idx = constrain(instBrowserCur, 0, instBrowserCount - 1);
  const InstBrowserEntry *ent = instBrowserEntryAt(idx);
  if (!ent) return false;
  char entName[sizeof(instBrowserEntries[0].name)];
  const bool isDir = (ent->isDir != 0);
  memcpy(entName, ent->name, sizeof(entName));
  entName[sizeof(entName) - 1] = '\0';

  if (isDir) {
    if (strcmp(entName, "..") == 0) {
      char *lastSlash = strrchr(instBrowserDir, '/');
      if (!lastSlash || lastSlash == instBrowserDir) {
        snprintf(instBrowserDir, sizeof(instBrowserDir), "/");
      } else {
        *lastSlash = '\0';
      }
    } else {
      char nextDir[INST_BROWSER_DIR_MAX];
      if (strcmp(instBrowserDir, "/") == 0) {
        snprintf(nextDir, sizeof(nextDir), "/%.42s", entName);
      } else {
        snprintf(nextDir, sizeof(nextDir), "%.51s/%.42s", instBrowserDir, entName);
      }
      snprintf(instBrowserDir, sizeof(instBrowserDir), "%s", nextDir);
    }
    scanInstBrowserDirectory();
    if (instBrowserCount > 1 && instBrowserWinCount > 0 && strcmp(instBrowserEntries[0].name, "..") == 0) {
      instBrowserCur = 1;
    }
    syncInstBrowserScroll();
    return false;
  }

  // Selected an .FMI instrument file: load it into the target instrument slot
  char fullPath[144];
  if (strcmp(instBrowserDir, "/") == 0) {
    snprintf(fullPath, sizeof(fullPath), "/%.42s", entName);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%.95s/%.42s", instBrowserDir, entName);
  }
  if (!fmSynthLoadInstrumentFile(instBrowserSlot, fullPath)) {
    return false;
  }
  instBrowserModalOpen = false;
  return true;
}

// ============================================================================
// System Settings & Instrument Type Preset Scanners & Modals
// ============================================================================
FLASHMEM static void scanPresetsInDirectory(const char *dirPath, bool createIfMissing,
                                            const InstBrowserEntry *delPrevPivot = nullptr, int delPrevIdx = -1) {
  scanPresetsDirectoryWindow(dirPath, createIfMissing, nullptr, 0, 0);
  if (delPrevPivot != nullptr && delPrevIdx >= 0 && instBrowserCount > MAX_INST_BROWSER_ENTRIES) {
    instBrowserCur = constrain(instBrowserCur, 0, instBrowserCount - 1);
    int maxScroll = max(0, instBrowserCount - INST_BROWSER_VISIBLE_ROWS);
    instBrowserScr = constrain(instBrowserScr, 0, maxScroll);
    if (instBrowserCur < instBrowserScr) {
      instBrowserScr = instBrowserCur;
    } else if (instBrowserCur >= instBrowserScr + INST_BROWSER_VISIBLE_ROWS) {
      instBrowserScr = instBrowserCur - INST_BROWSER_VISIBLE_ROWS + 1;
    }
    int visEnd = min(instBrowserCount - 1, instBrowserScr + INST_BROWSER_VISIBLE_ROWS - 1);
    if (visEnd >= instBrowserWinCount) {
      int maxWinStart = max(0, instBrowserCount - MAX_INST_BROWSER_ENTRIES);
      int wantStart = constrain(instBrowserScr - 12, 0, maxWinStart);
      if (delPrevIdx >= wantStart && delPrevIdx < wantStart + MAX_INST_BROWSER_ENTRIES) {
        scanPresetsDirectoryWindow(dirPath, false, delPrevPivot, delPrevIdx, wantStart);
      }
    }
  }
  syncInstBrowserScroll();
}

FLASHMEM bool isSysPresetsModalOpen() {
  return sysPresetsModalOpen;
}

FLASHMEM bool isSysPresetsSongMode() {
  return sysPresetsSongMode;
}

FLASHMEM bool isSysPresetDeleteConfirmOpen() {
  return sysPresetsModalOpen && sysPresetDelConfirmOpen;
}

FLASHMEM int sysPresetDeleteConfirmCursor() {
  return sysPresetDelConfirmCur;
}

FLASHMEM void openSysPresetsModal(bool songMode) {
  instBrowserModalOpen = false;
  typePresetModalOpen = false;
  sysPresetsSongMode = songMode;
  sysPresetDelConfirmOpen = false;
  sysPresetDelConfirmCur = 0;
  instBrowserCur = 0;
  instBrowserScr = 0;
  if (songMode) {
    char songDir[80];
    getSongInstrumentsDir(songDir, sizeof(songDir));
    scanPresetsInDirectory(songDir, false);
  } else {
    scanPresetsInDirectory(INST_PRESETS_DIR, true);
  }
  sysPresetsModalOpen = true;
}

FLASHMEM void closeSysPresetsModal() {
  sysPresetsModalOpen = false;
  sysPresetsSongMode = false;
  sysPresetDelConfirmOpen = false;
}

FLASHMEM void closeSysPresetDeleteConfirm() {
  sysPresetDelConfirmOpen = false;
}

FLASHMEM void sysPresetsMove(int dy, int pageDelta) {
  if (!sysPresetsModalOpen || sysPresetDelConfirmOpen || instBrowserCount <= 0) return;
  int step = dy + pageDelta * INST_BROWSER_VISIBLE_ROWS;
  if (step == 0) return;
  instBrowserCur = constrain(instBrowserCur + step, 0, instBrowserCount - 1);
  syncInstBrowserScroll();
}

FLASHMEM void sysPresetsRequestDelete() {
  if (!sysPresetsModalOpen || instBrowserCount <= 0) return;
  sysPresetDelConfirmCur = 0; // Default to YES
  sysPresetDelConfirmOpen = true;
}

FLASHMEM void sysPresetDeleteConfirmMove(int delta) {
  if (!sysPresetDelConfirmOpen || delta == 0) return;
  sysPresetDelConfirmCur = (sysPresetDelConfirmCur + delta + 2) % 2;
}

FLASHMEM bool sysPresetDeleteConfirmCommit() {
  if (!sysPresetDelConfirmOpen) return false;
  if (sysPresetDelConfirmCur != 0) {
    sysPresetDelConfirmOpen = false;
    return false;
  }
  sysPresetDelConfirmOpen = false;
  if (instBrowserCount <= 0) return false;

  int idx = constrain(instBrowserCur, 0, instBrowserCount - 1);
  const InstBrowserEntry *ent = instBrowserEntryAt(idx);
  if (!ent) return false;
  char fName[sizeof(instBrowserEntries[0].name)];
  memcpy(fName, ent->name, sizeof(fName));
  fName[sizeof(fName) - 1] = '\0';

  InstBrowserEntry prevPivot{};
  const InstBrowserEntry *prevEnt = (idx > 0) ? instBrowserEntryAt(idx - 1) : nullptr;
  if (prevEnt) {
    prevPivot = *prevEnt;
  }

  char fullPath[128];
  bool ok = false;
  if (sysPresetsSongMode) {
    char songDir[80];
    getSongInstrumentsDir(songDir, sizeof(songDir));
    snprintf(fullPath, sizeof(fullPath), "%s/%s", songDir, fName);
    ok = SD.sdfs.remove(fullPath);
    if (SD.sdfs.card()) {
      SD.sdfs.card()->syncDevice();
    }
    scanPresetsInDirectory(songDir, false, prevEnt ? &prevPivot : nullptr, idx - 1);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%s/%s", INST_PRESETS_DIR, fName);
    ok = SD.sdfs.remove(fullPath);
    if (SD.sdfs.card()) {
      SD.sdfs.card()->syncDevice();
    }
    scanPresetsInDirectory(INST_PRESETS_DIR, true, prevEnt ? &prevPivot : nullptr, idx - 1);
  }
  return ok;
}

FLASHMEM bool isTypePresetModalOpen() {
  return typePresetModalOpen;
}

FLASHMEM bool isTypePresetGlobal() {
  return typePresetIsGlobal;
}

FLASHMEM void openTypePresetModal(bool globalPresets) {
  instBrowserModalOpen = false;
  sysPresetsModalOpen = false;
  sysPresetDelConfirmOpen = false;
  typePresetIsGlobal = globalPresets;
  instBrowserCur = 0;
  instBrowserScr = 0;
  if (globalPresets) {
    scanPresetsInDirectory(INST_PRESETS_DIR, true);
  } else {
    char songInstDir[64];
    getSongInstrumentsDir(songInstDir, sizeof(songInstDir));
    scanPresetsInDirectory(songInstDir, false);
  }
  typePresetModalOpen = true;
}

FLASHMEM void closeTypePresetModal() {
  typePresetModalOpen = false;
}

FLASHMEM void typePresetModalMove(int dy, int pageDelta) {
  if (!typePresetModalOpen || instBrowserCount <= 0) return;
  int step = dy + pageDelta * INST_BROWSER_VISIBLE_ROWS;
  if (step == 0) return;
  instBrowserCur = constrain(instBrowserCur + step, 0, instBrowserCount - 1);
  syncInstBrowserScroll();
}

FLASHMEM bool typePresetModalConfirm(int targetSlot) {
  if (!typePresetModalOpen || instBrowserCount <= 0) return false;
  int idx = constrain(instBrowserCur, 0, instBrowserCount - 1);
  const InstBrowserEntry *ent = instBrowserEntryAt(idx);
  if (!ent) return false;
  char fName[sizeof(instBrowserEntries[0].name)];
  memcpy(fName, ent->name, sizeof(fName));
  fName[sizeof(fName) - 1] = '\0';

  char fullPath[128];
  if (typePresetIsGlobal) {
    snprintf(fullPath, sizeof(fullPath), "%s/%s", INST_PRESETS_DIR, fName);
  } else {
    char songInstDir[64];
    getSongInstrumentsDir(songInstDir, sizeof(songInstDir));
    snprintf(fullPath, sizeof(fullPath), "%s/%s", songInstDir, fName);
  }
  if (!fmSynthLoadInstrumentFile(targetSlot, fullPath)) {
    return false;
  }
  typePresetModalOpen = false;
  return true;
}

}  // namespace looper

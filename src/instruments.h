// instruments.h — Modular Multi-Engine Instrument Host & Voice Manager
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <SD.h>
#include "config.h"

namespace looper {

constexpr int NUM_INST_TYPES = 4;  // 0: 2-Op FM, 1: Phase Dist, 2: Sampler, 3: Noise Texture Drone

enum class InstType : uint8_t {
  None = 0,        // Empty / unallocated instrument slot
  Fm2Op = 1,       // 2-Operator FM Synthesizer (src/inst_fm2op.*)
  PhaseDist = 2,   // Phase Distortion Synthesizer (src/inst_phasedist.*)
  Sampler = 3,     // SD Card WAV Sampler (src/inst_sampler.*)
  NoiseDrone = 4   // Noise Texture Drone Synthesizer (src/inst_noisedrone.*)
};

// Per-operator / per-section specific parameter block (8 bytes)
struct FmOpParams {
  uint8_t wave;          // Waveform selector
  uint8_t level;         // 0..100
  uint16_t ratioX100;    // Ratio or Tuning parameter (engine-specific range)
  uint8_t feedback;      // Feedback / Mod mode / Line 2 level
  uint8_t reserved[3];   // Pad to 8 bytes
};
static_assert(sizeof(FmOpParams) == 8, "FmOpParams must be 8 bytes");

constexpr uint8_t INST_ENV_MAX = 127;
constexpr uint8_t INST_FX_MAX  = 100;

enum InstEnvCol : int {
  INST_ENV_AMT   = 0,
  INST_ENV_ATK   = 1,
  INST_ENV_DEC   = 2,
  INST_ENV_SUS   = 3,
  INST_ENV_REL   = 4,
  INST_ENV_COUNT = 5
};

enum InstFxCol : int {
  INST_FX_DRIVE  = 0,
  INST_FX_CHORUS = 1,
  INST_FX_DELAY  = 2,
  INST_FX_DFB    = 3,
  INST_FX_REVERB = 4,
  INST_FX_COUNT  = 5
};

// General Instrument View Envelope parameter block (5 bytes, all 0..127)
struct InstEnvParams {
  uint8_t amt;      // 0..127 (Envelope depth / amount)
  uint8_t attack;   // 0..127 (Envelope Attack)
  uint8_t decay;    // 0..127 (Envelope Decay)
  uint8_t sustain;  // 0..127 (Envelope Sustain)
  uint8_t release;  // 0..127 (Envelope Release)
};
static_assert(sizeof(InstEnvParams) == 5, "InstEnvParams must be 5 bytes");

// General Instrument View Effects parameter block (5 bytes, all 0..100%)
struct InstFxParams {
  uint8_t drive;          // 0..100 (Warm oversaturated distortion)
  uint8_t chorus;         // 0..100 (Stereo quadrature chorus, same as Master page)
  uint8_t delay;          // 0..100 (Stereo analog/tape delay, same as Master page)
  uint8_t delayFeedback;  // 0..100 (Delay feedback, same as Master page)
  uint8_t reverb;         // 0..127/100 (Stereo Schroeder reverb, same as Master page)
};
static_assert(sizeof(InstFxParams) == 5, "InstFxParams must be 5 bytes");

// 48-byte Patch stored per Instrument slot (I01..I08)
struct FmPatch {
  char name[MAX_SONG_NAME_LEN + 1];     // 13 bytes (null-terminated uppercase A-Z, 0-9, '_', '-')
  uint8_t type;                         // InstType (0 = None, 1 = Fm2Op, 2 = PhaseDist)
  uint8_t preset;                       // 0 .. NUM_FM_PRESETS - 1
  uint8_t algorithm;                    // Engine algorithm selector (Fm2Op: 0 = [A][B] parallel, 1 = [B]>[A] FM)
  FmOpParams op[2];                     // 16 bytes: Two 8-byte specific section parameter blocks
  InstEnvParams amp;                    // 5 bytes: General AMP envelope (AMT, Attack, Decay, Sustain, Release 0..127)
  InstEnvParams flt;                    // 5 bytes: General FLT envelope (AMT, Attack, Decay, Sustain, Release 0..127)
  InstFxParams fx;                      // 5 bytes: General FX row (DRIVE, CHORUS, DELAY, DELAY.FEEDBACK, REVERB 0..100)
  uint8_t pad;                          // 1 byte: Pad struct to 48 bytes
};
static_assert(sizeof(FmPatch) == 48, "FmPatch must be 48 bytes");

// Shared voice & runtime state passed to modular instrument engines
enum class EnvStage : uint8_t { Idle = 0, Attack = 1, Decay = 2, Sustain = 3, Release = 4 };

struct FmInstRuntime {
  float pitchBendRatio = 1.0f;       // +/- 2 semitones
  float modWheelBoost = 0.0f;        // CC 1 (0..1)
  bool sustainPedal = false;         // CC 64
};

struct FmVoice {
  bool active = false;
  uint8_t instIdx = 0;
  uint8_t midiTrack = 0xFF;
  uint8_t note = 60;
  uint8_t velocity = 100;
  bool gate = false;
  bool sustained = false;
  uint8_t startSampleOffset = 0;
  uint32_t age = 0;

  uint32_t baseStep = 0;
  uint32_t opPhase[2] = {0, 0};
  uint32_t pdCycleParity = 0;
  float opPrev[2] = {0.0f, 0.0f};
  EnvStage ampStage = EnvStage::Idle;
  EnvStage fltStage = EnvStage::Idle;
  float ampEnv = 0.0f;
  float fltEnv = 0.0f;
  float velScale = 1.0f;
  float fltLp1 = 0.0f;
  float fltLp2 = 0.0f;
  float fltAmt = 1.0f;
  float svfAlpha = 1.0f;
  float svfFbG = 0.0f;
  uint8_t svfSubPhase = 0;
};

// Shared DSP utilities available to all modular instrument engines
constexpr uint32_t INST_LUT_BITS = 10;
constexpr uint32_t INST_LUT_SIZE = 1u << INST_LUT_BITS;
constexpr uint32_t INST_LUT_MASK = INST_LUT_SIZE - 1;

float instSinLutAt(uint32_t idx);
float instNextWhiteNoise();
uint16_t instEnvAttackMs(uint8_t param);
uint16_t instEnvDecayReleaseMs(uint8_t param);
void instAdvanceEnvelope(EnvStage &stage, float &env, float atkInc, float decMul, float susLvl, float relMul);
bool instAdvanceVoiceEnvelopes(FmVoice &v);
float instApplyVoiceFilterAndAmp(FmVoice &v, float rawSample, float voiceGain);
void instApplyVoiceStereoFilterAndAmp(FmVoice &v, float rawL, float rawR, float voiceGain,
                                      float &outL, float &outR);
const FmVoice *fmSynthVoiceAt(int voiceIdx);
bool fmSynthIsNoteActive(int instIdx, uint8_t note);

// Modular Instrument Engine Interface — each instrument (.h/.cpp) exports one InstEngineOps descriptor
struct InstEngineOps {
  InstType type;
  const char *typeName;      // Shown on Page::Instruments and Page::InstrumentType (e.g. "2-OP FM", "PHASE DIST")
  const char *shortBadge;    // Shown in Header Bar on Page::InstrumentEdit (e.g. "2OP", "PD")
  const char *defaultPrefix; // Prefix for default slot names (e.g. "FM", "PD")
  const char *sec0Title;     // Section 0 heading on Specific Instrument View
  const char *sec1Title;     // Section 1 heading on Specific Instrument View
  const char *sec2Title;     // Optional Section 2 heading on Specific Instrument View (nullptr if 2 sections)

  const FmPatch &(*defaultPatch)();
  void (*validatePatch)(FmPatch &p);

  // UI formatting & input editing hooks for the 4 main cells of each section (opIdx = 0..1, col = 0..3)
  void (*formatMainCell)(const FmPatch &p, int opIdx, int col,
                         const char *&outLabel, char *outVal, size_t valSz,
                         int &outBarVal, int &outBarMax);
  void (*adjustMainCell)(FmPatch &p, int opIdx, int col, int dx, int dy, int delta, int step);
  void (*resetMainCell)(FmPatch &p, const FmPatch &defPatch, int opIdx, int col);

  // Audio ISR per-voice block renderer (44.1 kHz, 128 frames)
  void (*renderVoice)(FmVoice &v, const FmPatch &p, const FmInstRuntime &rt,
                      int32_t *outL, int32_t *outR, int numFrames);
};

// Engine Registry Lookups
const InstEngineOps *instEngineForType(InstType type);
const InstEngineOps *instEngineAt(int typeIdx);
InstType instTypeAt(int typeIdx);
const char *instTypeName(InstType type);

// Lifecycle & Slot / Default Patch management
void fmSynthInit();
void fmSynthResetAll();
bool fmSynthIsSlotEmpty(int instIdx);
void fmSynthCreateInSlot(int instIdx, InstType type = InstType::Fm2Op);
void fmSynthDeleteSlot(int instIdx);
bool fmSynthCopySlot(int instIdx);
bool fmSynthPasteSlot(int instIdx);
bool fmSynthHasClipboard();

void fmSynthResetToDefault(int instIdx, bool keepName = true);

// Patch accessors
FmPatch &fmSynthPatch(int instIdx);
const FmPatch &fmSynthDefaultPatch(InstType type);

// Real-time MIDI / Note control (ISR-safe & loop-safe via lock-free SynthEvent queue)
bool fmSynthNoteOn(int instIdx, uint8_t note, uint8_t velocity, uint8_t sampleOffset = 0,
                   bool allowSdIo = true, uint8_t midiTrack = 0xFF);
void fmSynthNoteOff(int instIdx, uint8_t note, uint8_t sampleOffset = 0, uint8_t midiTrack = 0xFF);
void fmSynthPitchBend(int instIdx, uint8_t lsb, uint8_t msb, uint8_t midiTrack = 0xFF);
void fmSynthCC(int instIdx, uint8_t cc, uint8_t val, uint8_t midiTrack = 0xFF);
void fmSynthSilenceInst(int instIdx);
void fmSynthSilenceMidiTrack(int midiTrack);
void fmSynthAllNotesOff();

// Audition helper (triggers a preview note and schedules automatic release)
void fmSynthAudition(int instIdx, uint8_t note = 60, uint8_t velocity = 104, uint32_t durationMs = 360);
void fmSynthService();
void fmSynthSetExportTailPhase(bool inTail);
bool fmSynthHasActiveOutput();

// Audio ISR block renderer (called from LooperEngine::update() at 44.1 kHz, 128 samples)
bool fmSynthRenderBlock(int32_t *outL, int32_t *outR, int numFrames,
                        int captureMidiTrack = -1, int32_t *capOutL = nullptr, int32_t *capOutR = nullptr);

// SD Card Persistence: Song instruments (`INST.DAT`) and individual `.FMI` files
bool fmSynthSaveSong(FsFile &f);
bool fmSynthLoadSong(FsFile &f);
bool fmSynthSaveInstrumentToSong(int instIdx);
bool fmSynthSaveInstrumentPreset(int instIdx);
bool fmSynthLoadInstrumentFile(int instIdx, const char *fullPath);

// SD Card Instrument File Browser Modal (`.FMI` files)
constexpr int INST_BROWSER_VISIBLE_ROWS = 7;
constexpr int INST_BROWSER_DIR_MAX = 96;

struct InstBrowserEntry {
  char name[43];
  uint8_t isDir;
  char instName[MAX_SONG_NAME_LEN + 1];
  uint8_t instType;
};

bool isInstBrowserOpen();
int instBrowserTargetSlot();
const char *instBrowserCurrentDir();
int instBrowserEntryCount();
int instBrowserCursor();
int instBrowserScroll();
const InstBrowserEntry *instBrowserEntryAt(int idx);

void openInstBrowser(int instIdx);
void closeInstBrowser();
void instBrowserMove(int dy, int pageDelta = 0);
bool instBrowserConfirm(); // returns true if an instrument file was loaded and browser closed

// System Settings Instrument Presets (`/Capstan/INSTRUMENT_PRESETS`) & Song Saved Instruments (`/Capstan/SONGS/<Song>/INSTRUMENTS`) Viewer & Delete Modal
bool isSysPresetsModalOpen();
bool isSysPresetsSongMode();
bool isSysPresetDeleteConfirmOpen();
int sysPresetDeleteConfirmCursor();
void openSysPresetsModal(bool songMode = false);
void closeSysPresetsModal();
void closeSysPresetDeleteConfirm();
void sysPresetsMove(int dy, int pageDelta = 0);
void sysPresetsRequestDelete();
void sysPresetDeleteConfirmMove(int delta);
bool sysPresetDeleteConfirmCommit(); // returns true if preset was deleted

// Instrument Type Page Preset Selection Modal (`Load song preset...` / `Load global preset...`)
bool isTypePresetModalOpen();
bool isTypePresetGlobal();
void openTypePresetModal(bool globalPresets);
void closeTypePresetModal();
void typePresetModalMove(int dy, int pageDelta = 0);
bool typePresetModalConfirm(int targetSlot); // returns true if preset was loaded into targetSlot

// Shared modal browser pool (2,048 B in DMAMEM) & directory buffer (96 B in RAM1)
void *sharedBrowserPoolBuf();
char *sharedBrowserDirBuf();

}  // namespace looper

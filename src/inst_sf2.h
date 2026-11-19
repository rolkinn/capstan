// inst_sf2.h — SoundFont 2.x (.sf2) Internal Instrument Engine
// Hybrid RAM1 Attack/Micro-Loop Pre-Roll Cache + Shared Ring-Locked SDIO Streamer
#pragma once
#include "instruments.h"

namespace looper {

static constexpr uint16_t SF2_TUNE_MIN  = 0;     // -24.00 semitones
static constexpr uint16_t SF2_TUNE_ZERO = 2400;  //   0.00 semitones
static constexpr uint16_t SF2_TUNE_MAX  = 4800;  // +24.00 semitones

static constexpr int NUM_SF2_LOOP_MODES   = 4;
static constexpr int NUM_SF2_ENV_MODES    = 2;
static constexpr int NUM_SF2_INTERP_MODES = 2;

enum class Sf2LoopOverride : uint8_t {
  FromFile = 0, // Honor per-zone SF2 sampleModes (0=None, 1=Cont, 3=UntilRel)
  Off      = 1, // Force one-shot playback
  Cont     = 2, // Force continuous sustain loop
  UntilRel = 3  // Force loop while held, play to end on release
};

enum class Sf2EnvMode : uint8_t {
  Sf2Trim     = 0, // Authored per-zone SF2 volEnv scaled by AMP/FLT macro trims (default)
  CapstanAdsr = 1  // Global Capstan AMP/FLT ADSR override
};

enum class Sf2InterpMode : uint8_t {
  HermiteHq = 0, // 4-point Catmull-Rom Hermite interpolation (default)
  Linear    = 1  // 2-point Linear interpolation
};

const InstEngineOps &instSoundFontEngine();

// Parameter label helpers
const char *sf2LoopModeName(uint8_t mode);
const char *sf2EnvModeName(uint8_t mode);
const char *sf2InterpModeName(uint8_t mode);

// Per-slot status & preset selection
bool sf2HasFile(int instIdx);
const char *sf2FileDisplayName(int instIdx);
int sf2PresetCount(int instIdx);
uint16_t sf2CurrentBank(int instIdx);
uint16_t sf2CurrentPresetNum(int instIdx);
const char *sf2CurrentPresetName(int instIdx);
int sf2ActiveZoneCount(int instIdx);

bool sf2AssignFile(int instIdx, const char *fullOrRelPath, int initialBank = -1, int initialPreset = -1);
bool sf2StepPreset(int instIdx, int delta);
bool sf2SelectPresetByIndex(int instIdx, int presetListIdx);
void sf2RequestCcPreset(int instIdx, uint8_t ccVal127);
void sf2UnloadFile(int instIdx);

// SoundFont 2.01 Custom Modulators (pmod / imod) UI & Real-Time MIDI Hooks
static constexpr int SF2_MOD_COLS_PER_ROW = 4;
static constexpr int SF2_MAX_UI_MOD_CCS   = 16;

int sf2PresetModCount(int instIdx);
int sf2InstModCount(int instIdx);
int sf2PresetModRowCount(int instIdx);
int sf2InstModRowCount(int instIdx);
bool sf2GetModCellInfo(int instIdx, bool isPresetSection, int idx,
                       char *outLabel, size_t lblSz, uint8_t &outCc, uint8_t &outVal);
bool sf2AdjustModCell(int instIdx, bool isPresetSection, int idx, int step);
bool sf2ResetModCell(int instIdx, bool isPresetSection, int idx);

void sf2HandleCcInIsr(int instIdx, uint8_t cc, uint8_t val);
void sf2HandlePitchBendInIsr(int instIdx, uint8_t lsb, uint8_t msb);
void sf2ResetPerformanceControllersInIsr(int instIdx);
void sf2RefreshLivePatchParams(int instIdx);

// Slot lifecycle & clipboard hooks (called by instruments.cpp & storage.cpp)
void sf2Init();
void sf2ResetAll();
void sf2ClearSlot(int instIdx);
void sf2CopySlot(int instIdx);
void sf2PasteSlot(int instIdx);
void sf2SilenceInst(int instIdx);
void sf2StopAllStreams();
void sf2RemapSongFolder(const char *oldSongName, const char *newSongName);

// Real-time Note-On / Note-Off & background stream handoff hooks
int sf2MatchNoteZones(int instIdx, uint8_t note, uint8_t velocity, uint8_t outZoneIds[2]);
bool sf2PrepareZoneVoiceOn(int instIdx, uint8_t zonePoolIdx, bool allowSdIo,
                           uint8_t &outEvStreamTag, uint16_t &outEvStreamGen);
void sf2InitVoiceInIsr(int voiceIdx, FmVoice &v, const FmPatch &p,
                       uint8_t evStreamTag, uint16_t evStreamGen,
                       bool wasActiveSf2);
bool sf2VoiceMatchesZone(int voiceIdx, uint8_t zonePoolIdx);
bool sf2PlayToEndOnRelease(int instIdx, int voiceIdx, const FmPatch &p);
void sf2ServiceStreams(bool fromCheckpoint = false);

// SD Card Persistence hooks (appended to INST.DAT and .FMI via RAM1 staging)
bool sf2SaveSongData(FsFile &f);
bool sf2LoadSongData(FsFile &f);
bool sf2SaveSingleInstData(FsFile &f, int instIdx);
bool sf2LoadSingleInstData(FsFile &f, int instIdx);

// Unified SD .sf2 File Browser & Preset List Browser Modal (shares DMAMEM sharedBrowserPool)
static constexpr int SF2_BROWSER_VISIBLE_ROWS = 7;

enum class Sf2BrowserMode : uint8_t {
  Closed     = 0,
  FileSelect = 1, // Browse folders & .sf2 files (defaults to /Capstan/SOUNDFONTS)
  PresetList = 2  // Browse BBB:PPP presets inside the slot's loaded .sf2 file
};

struct Sf2BrowserRowInfo {
  char label[44];
  char rightBadge[12];
  bool isDir;
  bool valid;
};

bool isSf2BrowserOpen();
Sf2BrowserMode sf2BrowserMode();
int sf2BrowserTargetSlot();
const char *sf2BrowserCurrentDir();
int sf2BrowserEntryCount();
int sf2BrowserCursor();
int sf2BrowserScroll();
bool sf2BrowserRowAt(int idx, Sf2BrowserRowInfo &outRow);

void openSf2FileBrowser(int instIdx);
bool openSf2PresetBrowser(int instIdx);
void closeSf2Browser();
void sf2BrowserMove(int dy, int pageDelta = 0);
bool sf2BrowserConfirm(); // returns true if file/preset was loaded and modal closed

} // namespace looper

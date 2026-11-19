// inst_sampler.h — SD Card WAV Sampler Instrument Engine (4-Octave Keyboard C3..B6)
#pragma once
#include "instruments.h"

namespace looper {

constexpr int SAMPLER_NUM_KEYS = 48;          // 4 octaves: C3..B6
constexpr uint8_t SAMPLER_BASE_NOTE = 36;     // MIDI note 36 = C3 .. 83 = B6
constexpr int SAMPLER_DIR_MAX = 96;           // Max directory path length (up to 95 chars)
constexpr int SAMPLER_FILE_MAX = 44;          // Max .WAV filename length (up to 43 chars)
constexpr int SAMPLER_PATH_MAX = 140;         // Combined full path buffer size
constexpr int NUM_SAMPLER_PRESS_MODES = 4;

enum class SamplerPressMode : uint8_t {
  Stop    = 0,  // Stops sample playing when key is released
  Once    = 1,  // Plays sample one time when key is pressed and released
  Rep     = 2,  // Loops sample when key is held down and stops playing when released
  RepThru = 3   // Loops sample when key is held down and continues playing to end when released
};

// Shared SD sample stream modes (used by both WAV Sampler and SoundFont .sf2 engine)
enum class SharedStreamKind : uint8_t {
  WavSampler    = 0,
  Sf2Mono16     = 1, // ring holds 2048 int16_t mono frames
  Sf2StereoLink = 2  // ring holds 1024 packed uint32_t (L | R<<16) stereo frames
};

static constexpr int SHARED_NUM_STREAMS = 8;
static constexpr uint32_t SHARED_STEREO_RING_FRAMES = 1024;
static constexpr uint32_t SHARED_MONO_RING_FRAMES   = 2048;

struct SharedSf2StreamReq {
  int instIdx;
  int voiceIdx;               // 0..31 if attaching directly to an active voice, or -1 for pendingVoice
  const char *sf2Path;        // Full path to .sf2 file
  uint16_t sf2FileTag;        // Non-zero tag identifying (instIdx, open .sf2 file)
  SharedStreamKind kind;      // Sf2Mono16 or Sf2StereoLink
  uint8_t sf2LoopMode;        // 0 = NoLoop, 1 = ContLoop, 3 = LoopUntilRel
  uint32_t dataOffsetL;       // Byte offset of sampleStart (Left or Mono) inside .sf2
  uint32_t dataOffsetR;       // Byte offset of rightStart inside .sf2 (Sf2StereoLink)
  uint32_t totalFrames;       // sampleEnd - sampleStart
  uint32_t startFrame;        // Initial fileFrame (0 or SF2_PREROLL_LEN)
  uint32_t loopStartFrame;    // Relative to sampleStart (0..totalFrames)
  uint32_t loopEndFrame;      // Relative to sampleStart (0..totalFrames)
};

struct SamplerStream {
  FsFile file;
  int8_t cachedInstIdx = -1;
  int8_t cachedKeyIdx = -1;
  uint16_t cachedSf2Tag = 0;
  uint32_t dataOffset = 0;
  uint32_t dataOffsetR = 0;
  uint32_t totalFrames = 0;
  uint32_t loopStartFrame = 0;
  uint32_t loopEndFrame = 0;
  uint32_t sampleRate = SAMPLE_RATE_HZ;
  uint32_t baseStepFp = 65536u; // 16.16 fixed-point step (65536 = 1.0x at 44.1 kHz)
  uint8_t channels = 2;
  uint8_t bitsPerSample = 16;
  uint8_t audioFormat = 1;      // 1 = PCM, 3 = IEEE float
  uint8_t bytesPerFrame = 4;
  uint8_t streamKind = 0;       // SharedStreamKind
  uint8_t sf2LoopMode = 0;      // 0 = NoLoop, 1 = ContLoop, 3 = LoopUntilRel

  volatile bool active = false;
  volatile bool pendingVoice = false;
  volatile bool prewarmed = false;
  volatile bool eofReached = false;
  volatile bool loopLockedInRing = false;
  volatile uint8_t instIdx = 0;
  volatile uint8_t keyIdx = 0;
  volatile uint8_t voiceIdx = 0;
  uint16_t ringLoopLen = 0;
  uint32_t ringLoopStart = 0;
  volatile uint32_t generation = 0;
  volatile uint32_t head = 0;
  volatile uint32_t tail = 0;
  uint32_t fileFrame = 0;
  uint32_t fracFp = 0;
  int16_t lastL = 0;
  int16_t lastR = 0;
  int16_t prevOutL = 0;
  int16_t prevOutR = 0;
  int16_t declickL = 0;
  int16_t declickR = 0;
  uint8_t declickRem = 0;

  uint32_t ring[SHARED_STEREO_RING_FRAMES];
};

const InstEngineOps &instSamplerEngine();

// Press mode label ("STOP", "ONCE", "REP", "REPTHRU")
const char *samplerPressModeName(uint8_t mode);

// Key note name ("C3" .. "B6"), short black-key label ("C#" .. "A#"), and black/white query
const char *samplerKeyNoteName(int keyIdx);
const char *samplerKeyShortLabel(int keyIdx);
bool samplerIsBlackKey(int keyIdx);
int samplerNoteToKey(uint8_t midiNote);
int samplerNoteToKeyForInst(int instIdx, uint8_t midiNote);

// Per-slot, per-key sample assignment & status
bool samplerHasSample(int instIdx, int keyIdx);
uint64_t samplerKeyMask(int instIdx);
uint64_t samplerPlayingKeyMask(int instIdx);
const char *samplerSampleDisplayName(int instIdx, int keyIdx);
bool samplerAssignSample(int instIdx, int keyIdx, const char *fullOrRelPath);
void samplerClearKey(int instIdx, int keyIdx);
bool samplerIsKeyPlaying(int instIdx, int keyIdx);

// Slot lifecycle & clipboard hooks (called by instruments.cpp)
void samplerResetAll();
void samplerClearSlot(int instIdx);
void samplerCopySlot(int instIdx);
void samplerPasteSlot(int instIdx);
void samplerSilenceInst(int instIdx);
void samplerStopAllStreams();
void samplerRemapSongFolder(const char *oldSongName, const char *newSongName);

// Real-time voice & SD stream hooks (called by instruments.cpp, inst_sf2.cpp & storage.cpp)
SamplerStream *samplerStreamSlot(uint32_t streamIdx);
bool samplerOpenSf2Stream(const SharedSf2StreamReq &req, uint32_t &outStreamIdx, uint32_t &outStreamGen, int initialPasses = 3);
void samplerPrewarmSf2File(const char *sf2Path, uint16_t sf2FileTag);
void samplerInvalidateSf2Tag(uint16_t sf2FileTag);
bool samplerPrepareVoiceOn(int instIdx, uint8_t note, uint32_t &outStreamIdx, uint32_t &outStreamGen, bool allowSdIo = true);
void samplerActivateStream(uint32_t streamIdx, uint32_t streamGen, int voiceIdx);
void samplerServiceActiveStreams(bool topUp = false);
void samplerServiceStreams();

// SD Card Persistence hooks (appended to INST.DAT and .FMI, staged in RAM1)
bool samplerSaveSongData(FsFile &f);
bool samplerLoadSongData(FsFile &f);
bool samplerSaveSingleInstData(FsFile &f, int instIdx);
bool samplerLoadSingleInstData(FsFile &f, int instIdx);

// SD Card Sample Browser Modal state & actions
constexpr int SAMPLER_BROWSER_VISIBLE_ROWS = 7;

struct SamplerBrowserEntry {
  char name[43];
  uint8_t isDir;
};

bool isSamplerBrowserOpen();
int samplerBrowserTargetKey();
const char *samplerBrowserCurrentDir();
int samplerBrowserEntryCount();
int samplerBrowserCursor();
int samplerBrowserScroll();
const SamplerBrowserEntry *samplerBrowserEntryAt(int idx);

void openSamplerBrowser(int instIdx, int keyIdx);
void closeSamplerBrowser();
void samplerBrowserMove(int dy, int pageDelta = 0);
bool samplerBrowserConfirm(); // returns true if a sample was assigned and browser closed

}  // namespace looper

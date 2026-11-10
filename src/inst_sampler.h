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

// Real-time voice & SD stream hooks (called by instruments.cpp & storage.cpp)
bool samplerPrepareVoiceOn(int instIdx, uint8_t note, uint32_t &outStreamIdx, uint32_t &outStreamGen, bool allowSdIo = true);
void samplerActivateStream(uint32_t streamIdx, uint32_t streamGen, int voiceIdx);
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

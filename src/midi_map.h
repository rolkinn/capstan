// midi_map.h — Configurable Global & Per-Instrument MIDI CC Mappings & MIDI Learn
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "config.h"

class FsFile;

namespace looper {

enum class InstType : uint8_t;

// Per-Instrument MIDI CC Mapping slot layout (up to 16 Engine-specific + 15 Shared AMP/FLT/FX = 31 slots)
static constexpr int INST_MIDI_MAP_ENGINE_SLOTS = 16;
static constexpr int INST_MIDI_MAP_SHARED_SLOTS = 15;
static constexpr int INST_MIDI_MAP_TOTAL_SLOTS  = INST_MIDI_MAP_ENGINE_SLOTS + INST_MIDI_MAP_SHARED_SLOTS; // 31

// Categories shown in the System Settings -> MIDI MAPPINGS modal
enum MidiMapCategory : uint8_t {
  MIDI_CAT_TRANSPORT  = 0,
  MIDI_CAT_AUDIO      = 1,
  MIDI_CAT_MIDI       = 2,
  MIDI_CAT_MASTER     = 3,
  MIDI_CAT_NAVIGATION = 4,
  MIDI_CAT_COUNT      = 5
};

// Stable 1-byte target IDs for all mappable controls
enum MidiTargetId : uint8_t {
  // 0. Transport & Recording (11 targets: 0..10)
  MIDI_TGT_PLAY_TOGGLE  = 0,
  MIDI_TGT_PLAY         = 1,
  MIDI_TGT_STOP         = 2,
  MIDI_TGT_REC_ARM      = 3,
  MIDI_TGT_DISARM_ALL   = 4,
  MIDI_TGT_PUNCH_TOGGLE = 5,
  MIDI_TGT_LOOP_TOGGLE  = 6,
  MIDI_TGT_RTZ          = 7,
  MIDI_TGT_REW          = 8,
  MIDI_TGT_FF           = 9,
  MIDI_TGT_PLAY_SPEED   = 10,

  // 1. Audio Tracks 1..8 (40 targets: 11..50, 5 per track: ARM, MUTE, SOLO, VOL, BAL/PAN)
  MIDI_TGT_AUD_BASE     = 11,
  MIDI_TGT_AUD_PER_TRK  = 5,

  // 2. MIDI Tracks 1..8 (24 targets: 51..74, 3 per track: ARM, MUTE, SOLO)
  MIDI_TGT_MID_BASE     = MIDI_TGT_AUD_BASE + NUM_AUDIO_TRACKS * MIDI_TGT_AUD_PER_TRK, // 51
  MIDI_TGT_MID_PER_TRK  = 3,

  // 3. Master Bus & Tempo (9 targets: 75..83)
  MIDI_TGT_MASTER_BASE  = MIDI_TGT_MID_BASE + NUM_MIDI_TRACKS * MIDI_TGT_MID_PER_TRK,  // 75
  MIDI_TGT_MASTER_VOL   = MIDI_TGT_MASTER_BASE + 0,
  MIDI_TGT_MASTER_CHOR  = MIDI_TGT_MASTER_BASE + 1,
  MIDI_TGT_MASTER_DLAY  = MIDI_TGT_MASTER_BASE + 2,
  MIDI_TGT_MASTER_DFB   = MIDI_TGT_MASTER_BASE + 3,
  MIDI_TGT_MASTER_RVRB  = MIDI_TGT_MASTER_BASE + 4,
  MIDI_TGT_MASTER_TAPE  = MIDI_TGT_MASTER_BASE + 5,
  MIDI_TGT_MASTER_COMP  = MIDI_TGT_MASTER_BASE + 6,
  MIDI_TGT_TEMPO_BPM    = MIDI_TGT_MASTER_BASE + 7,
  MIDI_TGT_METRO_MODE   = MIDI_TGT_MASTER_BASE + 8,

  // 4. Navigation (5 targets: 84..88)
  MIDI_TGT_NAV_BASE     = MIDI_TGT_MASTER_BASE + 9, // 84
  MIDI_TGT_NAV_UP       = MIDI_TGT_NAV_BASE + 0,    // 84
  MIDI_TGT_NAV_DOWN     = MIDI_TGT_NAV_BASE + 1,    // 85
  MIDI_TGT_NAV_LEFT     = MIDI_TGT_NAV_BASE + 2,    // 86
  MIDI_TGT_NAV_RIGHT    = MIDI_TGT_NAV_BASE + 3,    // 87
  MIDI_TGT_NAV_EDIT     = MIDI_TGT_NAV_BASE + 4,    // 88

  MIDI_TARGET_COUNT     = MIDI_TGT_NAV_BASE + 5     // 89
};

enum MidiAudTrackOffset : uint8_t {
  MIDI_AUD_OFF_ARM  = 0,
  MIDI_AUD_OFF_MUTE = 1,
  MIDI_AUD_OFF_SOLO = 2,
  MIDI_AUD_OFF_VOL  = 3,
  MIDI_AUD_OFF_PAN  = 4
};

enum MidiMidTrackOffset : uint8_t {
  MIDI_MID_OFF_ARM  = 0,
  MIDI_MID_OFF_MUTE = 1,
  MIDI_MID_OFF_SOLO = 2
};

// Channel encoding in MidiCcMapEntry::channel
constexpr uint8_t MIDI_MAP_CH_OFF = 0;  // Unmapped / disabled
constexpr uint8_t MIDI_MAP_CH_ALL = 17; // Omni (matches MIDI Ch 1..16)

struct MidiCcMapEntry {
  uint8_t channel; // 0 = OFF, 1..16 = Ch 01..16, 17 = ALL
  uint8_t cc;      // 0..127 CC number
};
static_assert(sizeof(MidiCcMapEntry) == 2, "MidiCcMapEntry must be 2 bytes");

void midiMapInit();
void midiMapResetGlobal();
void midiMapRebuildMask();
void midiMapFlushSaveIfDirty();

const MidiCcMapEntry *midiMapGlobalTable();
MidiCcMapEntry *midiMapGlobalTableMut();

MidiCcMapEntry midiMapEffective(int targetId);

void midiMapSetEntry(int targetId, uint8_t channel, uint8_t cc);
void midiMapClearEntry(int targetId);
void midiMapClearCategory(MidiMapCategory cat);
void midiMapClearAll();
void midiMapStepChannel(int targetId, int delta);
void midiMapStepCc(int targetId, int delta);

// Category & label metadata for UI
const char *midiMapCategoryLabel(MidiMapCategory cat);
void midiMapCategoryBounds(MidiMapCategory cat, int &outFirstTarget, int &outCount);
void midiMapFormatTargetName(int targetId, char *out, int outSz);
bool midiMapIsTriggerTarget(int targetId);

// MIDI Learn state (active while listening for a CC inside the MIDI Mappings modal)
void midiMapStartLearn(int targetId);
void midiMapCancelLearn();
bool midiMapIsLearning();
int midiMapLearningTarget();

// Fast ISR-safe check (RAM1) used by midiSeqInputEvent() so mapped CCs do not trigger MIDI threshold start
bool midiMapIsCcIntercepted(uint8_t inCh1To16, uint8_t cc);

// Called from loop() in midiSeqService() when a 0xB0 Control Change arrives.
// Returns true if consumed by MIDI Learn or an active CC mapping.
bool midiMapHandleCc(uint8_t inCh1To16, uint8_t cc, uint8_t val);

// Periodic service called from midiSeqService() to handle held REW / FF playhead scrubbing
void midiMapService();

// ============================================================================
// Per-Instrument MIDI CC Parameter Mapping (Persisted in INST.DAT & .FMI)
// ============================================================================
void instMidiMapResetAll();
void instMidiMapClearInst(int instIdx);
void instMidiMapCopyInst(int srcInstIdx);
void instMidiMapPasteInst(int dstInstIdx);

int instMidiMapRowCount(InstType type);
int instMidiMapRowToSlot(InstType type, int rowIdx);
const char *instMidiMapRowLabel(InstType type, int rowIdx);

MidiCcMapEntry instMidiMapGetSlot(int instIdx, int slot);
void instMidiMapSetSlot(int instIdx, int slot, uint8_t channel, uint8_t cc);
void instMidiMapClearSlot(int instIdx, int slot);
void instMidiMapStepChannel(int instIdx, int slot, int delta);
void instMidiMapStepCc(int instIdx, int slot, int delta);

void instMidiMapStartLearn(int instIdx, int slot);
void instMidiMapCancelLearn();
bool instMidiMapIsLearning();
int instMidiMapLearningInst();
int instMidiMapLearningSlot();

// Called from fmSynthCC() when a MIDI track routed to I01..I08 sends a CC
void instMidiMapApplyRoutedCc(int instIdx, uint8_t cc, uint8_t val);

// SD Card Persistence (RAM1-staged chunks appended to INST.DAT and .FMI)
bool instMidiMapSaveSongData(FsFile &f);
bool instMidiMapLoadSongData(FsFile &f);
bool instMidiMapSaveSingleInstData(FsFile &f, int instIdx);
bool instMidiMapLoadSingleInstData(FsFile &f, int instIdx);

} // namespace looper

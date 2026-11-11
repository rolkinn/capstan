// midi_seq.h — 8-Track Linear MIDI Recorder & Sequencer (Notes, CC, Pitch Bend)
#pragma once
#include <stdint.h>
#include <SD.h>
#include "config.h"
#include "engine.h"

namespace looper {

// 8-byte compact MIDI event stored in DMAMEM (RAM2) and persisted in MIDI.DAT
struct MidiEvent {
  uint32_t tick;       // musical tick (480 PPQN from tape start 0:00.0)
  uint8_t status;      // high nibble: 0x80 (NoteOff), 0x90 (NoteOn), 0xB0 (CC), 0xE0 (PitchBend)
  uint8_t data1;       // note (0..127), CC number (0..127), or PitchBend LSB (0..127)
  uint8_t data2;       // velocity (0..127), CC value (0..127), or PitchBend MSB (0..127)
  uint8_t reserved;
};
static_assert(sizeof(MidiEvent) == 8, "MidiEvent must be 8 bytes");

struct MidiTrack {
  ArmMode armMode = ArmMode::Off;
  TrigMode trigMode = TrigMode::Manual;   // Manual or Threshold (MIDI input trigger)
  bool muted = false;
  volatile bool solo = false;
  uint8_t inChannel = 0;                  // 0 = ALL (1..16), 1..16 = specific MIDI channel, 17 (MIDI_IN_OFF) = OFF
  uint8_t outChannel = 17;                // 0 = OFF, 1..16 = Ext MIDI C01..C16, 17..24 = Internal Inst I01..I08

  uint32_t eventCount = 0;
  uint32_t playIndex = 0;                 // next event index to play during transport run
  bool recWindowActive = false;           // currently inside active/punch recording window
  uint32_t recEraseCursorTick = 0;        // in Record (Replace) mode: ticks erased up to here

  // Visual version counter and live activity indicator for the MIDI Page UI
  uint8_t overviewVersion = 0;
  uint8_t lastStatus = 0;
  uint8_t lastData1 = 0;
  uint8_t lastData2 = 0;
  uint32_t lastActivityMs = 0;
  uint32_t activeNoteMask[4] = {};        // 128-bit mask of currently active/playing notes (0..127)
};

enum class MidiClearMode : uint8_t {
  Notes = 0,
  CcPb  = 1,
  All   = 2
};

extern MidiTrack midiTracks[NUM_MIDI_TRACKS];

inline bool anyTrackSolo() {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].solo) return true;
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    if (midiTracks[t].solo) return true;
  }
  return false;
}

void midiSeqInit();
void midiSeqClearAll();
void midiSeqClearTrack(int t);
void midiSeqClearTrackMode(int t, MidiClearMode mode);

// Called from ISR or poll when a channel voice message arrives (TRS Serial or USB MIDI)
// status: 0x80..0xEF (includes channel 0..15 in low nibble), fromSource: 0 = TRS Serial, 1 = USB MIDI
void midiSeqInputEvent(uint8_t status, uint8_t d1, uint8_t d2, uint8_t fromSource = 0);

// Called from loop() every pass to advance MIDI playback, handle punch-in/out & replace,
// and flush newly arrived MIDI input events into the armed MIDI tracks.
int midiSeqFocusedTrack();
int midiSeqAudioRecTargetTrack();
void midiSeqService(float bpm, int selectedMidiTrack, bool midiThruEnabled);
void midiSeqFastService();
void midiSeqIsrTick();

// Called when transport starts, stops, or seeks to a new tapeFrame
void midiSeqOnTransportStart(uint32_t tapeFrame, float bpm);
void midiSeqOnTransportStop();
void midiSeqOnSeek(uint32_t tapeFrame, float bpm);
void midiSeqAllNotesOff();
void midiSeqSilenceMutedTracks();
void midiSeqStepInChannel(int t, int delta);
void midiSeqCycleInChannel(int t);
void midiSeqSetOutChannel(int t, uint8_t dest);
void midiSeqStepOutChannel(int t, int dy, int delta);
void midiSeqCycleOutChannel(int t);

// Increment overview version counters for all MIDI tracks
void midiSeqRebuildAllOverviews(float bpm, uint32_t displayMaxFrames);

// SD card persistence for a song's 8 MIDI tracks
bool midiSeqSave(FsFile &f);
bool midiSeqLoad(FsFile &f, float bpm, uint32_t displayMaxFrames);

// Access events for inspection/UI
const MidiEvent *midiSeqEvents(int t, uint32_t &count);

// Region editing (Copy/Cut/Paste/Duplicate/Delete/Quantize via SD temp file /Capstan/_CLIPMID.TMP)
bool midiSeqCopyRegionToClip(int srcTrack, uint32_t startFrame, uint32_t endFrame, float bpm);
bool midiSeqPasteClipToTrack(int dstTrack, uint32_t destStartFrame, uint32_t regionFrames, float bpm);
bool midiSeqDeleteRegion(int track, uint32_t startFrame, uint32_t endFrame, float bpm,
                         MidiClearMode mode = MidiClearMode::All);
bool midiSeqQuantizeRegion(int track, uint32_t startFrame, uint32_t endFrame, uint32_t gridTicks, float bpm);

// Note-level sequence editing (Piano Roll MIDI Note Editor)
bool midiSeqFindNoteAt(int track, uint8_t note, uint32_t tick,
                       uint32_t &outStartTick, uint32_t &outEndTick, uint8_t &outVelocity);
bool midiSeqInsertNote(int track, uint8_t note, uint32_t startTick, uint32_t endTick, uint8_t velocity, float bpm);
bool midiSeqDeleteNoteAt(int track, uint8_t note, uint32_t tick, float bpm);

// Standard MIDI File (.mid) export & import (/Capstan/SONGS/<SongName>/MIDI/)
bool midiSeqExportAllTracksMid(float bpm);
bool midiSeqExportClipMid(int track, uint32_t startFrame, uint32_t endFrame, float bpm);
bool midiSeqLoadMidFileToTrack(int dstTrack, uint32_t destStartFrame, const char *filePath, float bpm);

// SD Card MIDI Clip Browser Modal state & actions
constexpr int MIDI_BROWSER_VISIBLE_ROWS = 7;

struct MidiBrowserEntry {
  char name[63];
  uint8_t isDir;
};

bool isMidiClipBrowserOpen();
bool isAudioClipBrowserMode();
int midiClipBrowserTargetTrack();
const char *midiClipBrowserCurrentDir();
int midiClipBrowserEntryCount();
int midiClipBrowserCursor();
int midiClipBrowserScroll();
const MidiBrowserEntry *midiClipBrowserEntryAt(int idx);

void openMidiClipBrowser(int track, uint32_t startFrame);
void openAudioClipBrowser(int track, uint32_t startFrame);
void closeMidiClipBrowser();
void midiClipBrowserMove(int dy, int pageDelta = 0);
bool midiClipBrowserConfirm(float bpm); // returns true if a .mid file was loaded and browser closed

}  // namespace looper

// storage.h — SD card streaming, 666s linear WAV recording/overdubbing, and manual song persistence
#pragma once
#include "engine.h"
#include "midi_seq.h"

namespace looper {

struct SongSettings {
  float bpm = DEFAULT_BPM;
  ClockSource clockSource = ClockSource::Internal;
  MetroMode metroMode = MetroMode::RecOnly;
  uint8_t countInBeats = 4;              // 0 (Off), 4 (1 bar), 8 (2 bars), 12 (3 bars), 16 (4 bars)
  bool punchEnabled = false;
  uint32_t punchInFrame = 0;             // in 44.1 kHz stereo frames
  uint32_t punchOutFrame = 4 * 22050;    // default 2.0 s (1 bar at 120 BPM)
  uint8_t masterChorus = 0;              // 0..100% Stereo Chorus macro (intensity & wet mix)
  uint8_t masterDelay = 0;               // 0..100% Stereo Delay dry/wet mix
  uint8_t masterDelayFeedback = 0;       // 0..100% Stereo Delay feedback amount
  uint8_t masterReverb = 0;              // 0..100% Lofi Oddity Reverb macro
  uint8_t masterTape = 0;                // 0..100% Lofi Oddity Tape macro
  uint8_t masterComp = 0;                // 0..100% Lofi Oddity Comp macro
  uint8_t masterVolume = MASTER_VOLUME_DEFAULT; // 0 (Off) .. MASTER_VOLUME_MAX (100), default 80
  AudioInputSource audioInputSource = AudioInputSource::LineAndSynth;
  bool inputMonitor = true;              // Legacy v1 field retained for SONG.DAT v2 layout compatibility (superseded by lineMonitor)
  bool usbInputMonitor = false;          // Legacy v1 field retained for SONG.DAT v2 layout compatibility (superseded by usbMonitor)
  int8_t defaultThreshDb = DEFAULT_THRESH_DB;
  bool midiThru = false;
  MidiClockOut midiClockOut = MidiClockOut::Off;
  bool loopEnabled = false;
  bool recLineIn = true;
  bool recUsbIn = false;
  bool recInternalInstr = true;
  MonitorMode lineMonitor = MonitorMode::On;
  MonitorMode usbMonitor = MonitorMode::Off;
  MidiTransportIn midiTransportIn = MidiTransportIn::Auto;
};

struct SystemSettings {
  uint8_t brightness = DEFAULT_BRIGHTNESS;    // 5..100 (default 50, scaled to 0..255 PWM)
  uint8_t metroVolume = DEFAULT_METRO_VOLUME; // 0..100 (default 80)
};

bool storageBegin(void (*progress)(const char *));
bool cardResponds();
bool cardRestart();
void storageForget();
void serviceStorage();
void keepStreaming();

// Named song management (/Capstan/SONGS/<NAME>) with manual save only
void sanitizeIdentifierName(const char *in, char *out, size_t outSz, bool trimTrailingPunct = true);
const char *currentSongName();
bool songIsSaved();
bool songHasUnsavedChanges();
void markSongDirty();

int songCount();
const char *songNameAt(int index);
int findSongIndex(const char *name);
void scanSongFolders();

bool loadSongByName(const char *name);
bool loadSong(int slot);
bool saveCurrentSong();
bool saveSongAs(const char *newName);
bool createNewSong();
bool deleteSongByName(const char *name);
bool deleteSongAt(int index);
uint32_t songMaxFrames();                // max recordedFrames across tracks (or at least 16 bars for ruler)

// Project files (/Capstan/PROJECTS/<NAME>.proj — ordered list of song names)
int projectCount();
const char *projectNameAt(int index);
int findProjectIndex(const char *name);
void scanProjectFiles();
bool createEmptyProjectFile(const char *name, char *outCleanName = nullptr, size_t outSz = 0);
bool renameProjectFile(const char *oldName, const char *newName, char *outCleanName = nullptr, size_t outSz = 0);
bool deleteProjectFile(const char *name);
int loadProjectSongs(const char *projName, char outSongs[][MAX_SONG_NAME_LEN + 1], int maxSongs);
bool saveProjectSongs(const char *projName, const char songs[][MAX_SONG_NAME_LEN + 1], int count);

// Tape transport & recording control from app / UI
void prefillTrackStreams(uint32_t fromTapeFrame);
void storageStartTransport(bool countIn, bool waitAudio, bool waitMidi);
void storageStopTransport();
void storageSeekTape(uint32_t tapeFrame);
void storageSetPlaybackSpeedQuarter(int8_t speedQuarter);
void storageSyncArmedAudioTrack();
void storageSyncLoopState();
void clearAudioTrack(int t);
enum class MasterExportResult : uint8_t {
  Ok        = 0,
  Empty     = 1,
  Cancelled = 2,
  Error     = 3
};

bool storageCopyAudioRegionToClip(int srcTrack, uint32_t startFrame, uint32_t endFrame);
bool storagePasteAudioClipToTrack(int dstTrack, uint32_t destStartFrame, uint32_t frameCount);
bool storageLoadWavFileToTrack(int dstTrack, uint32_t destStartFrame, const char *fullPath);
bool storageDeleteAudioRegion(int t, uint32_t startFrame, uint32_t endFrame);
void storageRemoveClipboardFiles();
uint32_t storageSongMaxAudibleFrame();
MasterExportResult storageExportMasterAudioWav();
enum class SamplerRecMode : uint8_t {
  ThreshAutoStartStop  = 0,
  ThreshAutoManualStop = 1,
  ManualStartStop      = 2
};

bool storageStartSamplerKeyRecord(int instIdx, int keyIdx,
                                  SamplerRecMode mode = SamplerRecMode::ThreshAutoStartStop);
void storageTriggerSamplerKeyRecordNow();
void storageCancelSamplerKeyRecord();
bool storageFinishSamplerKeyRecordNow();
bool storageIsSamplerKeyRecordingOpen();
bool storageIsSamplerKeyRecordTriggered();
SamplerRecMode storageSamplerKeyRecMode();
void flushHeaders();

// System settings persistence
bool saveSystemSettings();

// Shared 16 KB RAM1 staging buffer (also reused by USB Mass Storage mode when transport is stopped)
uint32_t *storageSharedChunkBuf();

extern volatile const char *debugStage;

}  // namespace looper

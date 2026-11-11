// app.h — shared application state, 9-page navigation, settings, and transport controls
#pragma once
#include "M8Board.h"
#include "engine.h"
#include "storage.h"
#include "midi_seq.h"
#include "instruments.h"
#include "colors.h"

using namespace looper;

extern m8::Board board;
extern AudioControlSGTL5000 codec;
extern float bpm;

// UI pages (9 pages: Audio, Midi, SongSettings, Main, SystemSettings, Instruments, InstrumentType, InstrumentEdit, Master)
enum class Page : uint8_t {
  Audio = 0,          // 1. Audio recording page (8 tracks: 4 stereo + 4 mono, + transport)
  Midi = 1,           // 2. MIDI recording page (8 MIDI tracks + Piano Roll editor + transport)
  SongSettings = 2,   // 3. Song settings page (Audio Input, Monitor, Thresh, MIDI Clock/Thru, Export)
  Main = 3,           // 4. Main screen (Current Song + Save/New/Load/Delete + Saved Songs List)
  SystemSettings = 4, // 5. System settings page (Projects, Brightness, Metro Vol, Presets, Global MIDI Map, USB Drive)
  Instruments = 5,    // 6. Instruments page (table of 8 song instruments I01..I08 + Copy/Paste/Delete/Load/Save)
  InstrumentType = 6, // 7. New instrument type selection page (2-Op FM, PhaseDist, Sampler, NoiseDrone)
  InstrumentEdit = 7, // 8. Synthesizer/Sampler parameter editor & Per-Instrument MIDI CC Map
  Master = 8          // 9. Master FX page (8-track Audio/MIDI mixer + Chorus -> Delay -> Reverb -> Tape -> Comp + Master Vol)
};

// Audio / MIDI page rows & cells
constexpr int HEADER_ROW = -1;
constexpr int VISIBLE_TRACK_ROWS = 4;
constexpr int TRACK_ROWS = 8;
constexpr int INFO_ROW = 8;
constexpr int TRANSPORT_ROW = 9;
constexpr int PAGE_ROWS = 10;

// Top Header Bar editable fields (MM : SS . mmm   BBB . B . 8th . 16th . 32nd)
enum HeaderCell : int {
  HDR_CELL_MIN   = 0,
  HDR_CELL_SEC   = 1,
  HDR_CELL_MS    = 2,
  HDR_CELL_BAR   = 3,
  HDR_CELL_BEAT  = 4,
  HDR_CELL_8TH   = 5,
  HDR_CELL_16TH  = 6,
  HDR_CELL_32ND  = 7,
  HEADER_CELL_COUNT = 8
};

enum InfoCell : int {
  INFO_CELL_BPM   = 0,
  INFO_CELL_SLOT1 = 1,
  INFO_CELL_SLOT2 = 2,
  INFO_CELL_COUNT = 3
};

// When Punch Window is ON, Info Bar exposes either 6 time-granularity cells
// (IN MM:SS.mmm = 0..2, OUT MM:SS.mmm = 3..5) or 10 bar/beat subdivision cells
// (IN BBB.B.E.S.T = 0..4, OUT BBB.B.E.S.T = 5..9):
constexpr int PUNCH_INFO_CELL_COUNT = 6;
constexpr int PUNCH_INFO_TIME_CELL_COUNT = 6;
constexpr int PUNCH_INFO_BAR_CELL_COUNT = 10;

enum AudioCell : int {
  AUD_CELL_ARM  = 0,
  AUD_CELL_SRC  = 1,
  AUD_CELL_TRIG = 2,
  AUD_CELL_WAVE = 3,
  AUD_CELL_VOL  = 4,
  AUD_CELL_PAN  = 5,
  AUD_CELL_COUNT = 6
};

enum MidiCell : int {
  MID_CELL_ARM   = 0,
  MID_CELL_TRIG  = 1,
  MID_CELL_WAVE  = 2,
  MID_CELL_INCH  = 3,
  MID_CELL_OUTCH = 4,
  MID_CELL_COUNT = 5
};

enum TransportCell : int {
  TR_CELL_REC   = 0,
  TR_CELL_LOOP  = 1,
  TR_CELL_RTZ   = 2,
  TR_CELL_REW   = 3,
  TR_CELL_PLAY  = 4,
  TR_CELL_FF    = 5,
  TR_CELL_COUNT = 6
};

enum SongSettingItem : int {
  PSET_REC_SRC      = 0,
  PSET_MONITOR      = 1,
  PSET_DEF_THRESH   = 2,
  PSET_CLOCK_OUT    = 3,
  PSET_CLOCK        = 4,
  PSET_TRANSPORT_IN = 5,
  PSET_MIDI_THRU    = 6,
  PSET_SAVED_INST   = 7,
  PSET_EXPORT_MIDI  = 8,
  PSET_EXPORT_AUDIO = 9,
  PSET_COUNT        = 10
};

enum SysSettingItem : int {
  SSET_BRIGHTNESS   = 0,
  SSET_METRO_VOL    = 1,
  SSET_INST_PRESETS = 2,
  SSET_PROJECTS     = 3,
  SSET_MIDI_MAP     = 4,
  SSET_USB_DRIVE    = 5,
  SSET_COUNT        = 6
};

// Main (Songs) page interactive zones & action buttons
enum MainRow : int {
  MAIN_ROW_SESSION = 0,
  MAIN_ROW_LIST    = 1
};

enum MainSessionAction : int {
  MAIN_SESS_SAVE  = 0,
  MAIN_SESS_COUNT = 1
};

enum MainRowAction : int {
  MAIN_ROW_ACT_LOAD   = 0,
  MAIN_ROW_ACT_DELETE = 1,
  MAIN_ROW_ACT_COUNT  = 2
};

constexpr int MAIN_LIST_VISIBLE_ROWS = 7;

// Instruments page inline row actions ("COPY", "PASTE", "NEW", "CLR")
enum InstRowAction : int {
  INST_ROW_ACT_COPY  = 0,
  INST_ROW_ACT_PASTE = 1,
  INST_ROW_ACT_NEW   = 2,
  INST_ROW_ACT_CLR   = 3,
  INST_ROW_ACT_COUNT = 4
};

constexpr int INST_LIST_VISIBLE_ROWS = 8;

// Instrument Type page rows ("Load song preset...", "Load global preset...", then engines)
enum InstTypeRow : int {
  INST_TYPE_ROW_LOAD_SONG   = 0,
  INST_TYPE_ROW_LOAD_GLOBAL = 1,
  INST_TYPE_ROW_FIRST_ENG   = 2,
  INST_TYPE_ROW_COUNT       = 2 + NUM_INST_TYPES
};

enum InstHeaderCol : int {
  INST_HDR_NAME      = 0,
  INST_HDR_LOAD      = 1,
  INST_HDR_SAVE_TO   = 2,
  INST_HDR_COL_COUNT = 3
};

// Instrument Editor (Page::InstrumentEdit) rows & columns
// Unified scrollable page with Specific controls first, followed by General controls (AMP, FLT, FX):
//   2-Op FM & Phase Dist (7 rows):
//     Row 0 (FM_ROW_HEADER):    Col 0: Name, Col 1: Load, Col 2: Save To...
//     Row 1 (FM_ROW_ALGO):      2-Op FM Algorithm toggle (1 col) / Phase Dist LINE 1 (6 cols)
//     Row 2 (FM_ROW_OP1_MAIN):  2-Op FM Operator A (4 cols) / Phase Dist LINE 2 (6 cols)
//     Row 3 (FM_ROW_OP2_MAIN):  2-Op FM Operator B (4 cols) / Phase Dist LINE 2 > LINE 1 (3 cols)
//     Row 4 (INST_GEN_ROW_AMP): AMP Envelope (5 cols: AMT, ATK, DEC, SUS, REL 0..127)
//     Row 5 (INST_GEN_ROW_FLT): FLT Envelope (5 cols: AMT, ATK, DEC, SUS, REL 0..127)
//     Row 6 (INST_GEN_ROW_FX):  Instrument FX (5 cols: DRIVE, CHORUS, DELAY, D.FB, REVERB 0..100)
//   Sampler (6 rows):
//     Row 0 (SMP_ROW_HEADER):   Col 0: Name, Col 1: Load, Col 2: Save To...
//     Row 1 (SMP_ROW_PRESS):    Press Mode toggle (1 col) + Key Sample Status
//     Row 2 (SMP_ROW_KEYS):     4-Octave Keyboard C3..B6 (48 cols)
//     Row 3 (SMP_ROW_AMP):      AMP Envelope (5 cols: AMT, ATK, DEC, SUS, REL 0..127)
//     Row 4 (SMP_ROW_FLT):      FLT Envelope (5 cols: AMT, ATK, DEC, SUS, REL 0..127)
//     Row 5 (SMP_ROW_FX):       Instrument FX (5 cols: DRIVE, CHORUS, DELAY, D.FB, REVERB 0..100)
enum FmEditRow : int {
  FM_ROW_HEADER    = 0,
  FM_ROW_ALGO      = 1,
  FM_ROW_OP1_MAIN  = 2,
  FM_ROW_OP2_MAIN  = 3,
  INST_GEN_ROW_AMP = 4,
  INST_GEN_ROW_FLT = 5,
  INST_GEN_ROW_FX  = 6,
  FM_ROW_COUNT     = 7,

  SMP_ROW_HEADER   = 0,
  SMP_ROW_PRESS    = 1,
  SMP_ROW_KEYS     = 2,
  SMP_ROW_AMP      = 3,
  SMP_ROW_FLT      = 4,
  SMP_ROW_FX       = 5,
  SMP_ROW_COUNT    = 6
};

int instGenBaseRow();
int instEditRowCount();
int fmRowColCount(int row);
void syncInstEditScroll();

// Master page rows & columns
// Top 8 Track Blocks (01..08 left-to-right, Audio or MIDI toggled with double-tap OPT):
//   Row 0: Audio V### / MIDI IN (8 cols: 0..7)
//   Row 1: Audio BAL/PAN / MIDI OUT (8 cols: 0..7)
//   Row 2: Mute (M) & Solo (S) buttons (16 cols: 0..15, where 2*t = M, 2*t+1 = S)
// Bottom FX & Master Volume Row:
//   Row 3: [ CHOR ] [ DLAY ] [ D.FB ] [ RVRB ] [ TAPE ] [ COMP ] [ VOL ] (7 cols: 0..6)
enum MasterRow : int {
  MASTER_ROW_TRK_PARAM1 = 0,
  MASTER_ROW_TRK_PARAM2 = 1,
  MASTER_ROW_TRK_MS     = 2,
  MASTER_ROW_FX         = 3,
  MASTER_ROW_INFO       = 4,
  MASTER_ROW_TRANSPORT  = 5,
  MASTER_ROW_COUNT      = 6
};

constexpr int MASTER_COL_VOL      = MASTER_FX_COUNT;     // col 6 on MASTER_ROW_FX
constexpr int MASTER_FX_ROW_COLS  = MASTER_FX_COUNT + 1; // 7 boxes (6 FX + 1 VOL)

enum class KeyboardTarget : uint8_t {
  Song = 0,
  Instrument = 1,
  ProjectNew = 2,
  ProjectRename = 3
};

// On-screen keyboard constants for naming a song or instrument on Save
constexpr int KB_GRID_ROWS = 4;
constexpr int KB_GRID_COLS = 10;
constexpr int KB_BTN_ROW   = 4;

Page currentPage();
Page lastRow1Page();
Page lastRow2Page();
void setPage(Page p);

// Per-page cursor state
extern int headerCell;
extern int audioRow, audioCell, audioScroll;
extern int midiRow, midiCell, midiScroll;
void syncAudioScroll();
void syncMidiScroll();
extern uint8_t timelineZoom;
void cycleTimelineZoom();
bool isHeaderRowSelected();
bool isPunchOrLoopWindowActive();
bool isPunchBarBeatMode();
void togglePunchBarBeatMode();
int activeInfoCellCount();
void clampInfoCellSelection();
extern int masterRow, masterCell;
bool isMasterMidiMode();
void toggleMasterTrackMode();
extern int songSetRow;
extern int sysSetRow;
extern int mainRow, mainCol;
extern int songListCursor;
extern int songListScroll;

extern int instCol;
extern int instListCursor;
extern int instTypeCursor;
extern int fmEditRow, fmEditCol, instEditScroll;
void syncInstListCursor();

// On-screen keyboard state for saving songs or naming instruments
extern char saveKeyboardBuf[MAX_SONG_NAME_LEN + 1];
extern int saveKeyboardRow;
extern int saveKeyboardCol;

bool isSaveKeyboardOpen();
KeyboardTarget saveKeyboardTarget();
void openSaveKeyboard(bool startInGrid = false);
void openInstrumentKeyboard(int instIdx, bool startInGrid = false);
void openProjectKeyboard(bool isRename, const char *initialText, bool startInGrid = true);
void closeSaveKeyboard();

// Clear Track Recording confirmation & quick transport/punch/loop modal state
bool isClearTrackModalOpen();
bool isClearTrackModalRevealing();
int clearTrackModalRow();
int clearTrackModalCursor();
Page clearTrackModalPage();
int clearTrackModalTrack();
void openClearTrackModal(Page page, int track);
void closeClearTrackModal();
void clearTrackModalMove(int dx, int dy);
void clearTrackModalConfirm();
void clearTrackModalTick();

void applyPunchInWithAutoOut(SongSettings &ps, uint32_t nextInFrame, bool barBeatMode);
void applyPunchOutWithAutoIn(SongSettings &ps, uint32_t nextOutFrame, bool barBeatMode);

// Instrument Editor "SAVE TO..." overlay state (0 = SONG, 1 = PRESET)
bool isInstSaveToModalOpen();
int instSaveToModalCursor();
void openInstSaveToModal();
void closeInstSaveToModal();
void instSaveToModalMove(int delta);
bool instSaveToModalConfirm();

// Load / Delete confirmation state on Main page
bool isLoadConfirmActive();
bool isDeleteConfirmActive();
int deleteConfirmTargetIndex();
void armLoadConfirm(int songIdx);
void armDeleteConfirm(int songIdx);
void cancelDeleteConfirm();
void syncSongListCursor(bool snapToActive = false);

// Settings accessors
SongSettings &songSettings();
SystemSettings &systemSettings();
void applySystemSettings();
void applySongSettings();
void applyMasterFxSettings();

// Tempo & Transport controls
bool setTempo(float b);
void followTempo(uint32_t spb, float shownBpm);

bool anyAudioTrackArmed();
bool anyMidiTrackArmed();
bool anyTrackArmed();
void armAudioTrack(int t, ArmMode mode);   // ensures at most 1 audio track is armed at a time
const char *audioTrackArmTag(int t);       // returns "L", "R", or "LR" for armed mono tracks (t >= 4), else nullptr
void stepAudioTrackArm(int t, int delta);  // cycles stereo (Off/Rec/Dub) or mono (Off/Rec L/R/LR/Dub L/R/LR)
const char *audioRecSrcLabel(looper::AudioRecSrc s);
looper::AudioRecSrc stepAudioRecSrc(int trackIdx, looper::AudioRecSrc cur, int dy, int delta, bool wrap = false);
void armMidiTrack(int t, ArmMode mode);
void toggleLastArmedTrack();
void toggleCursorOrLastArmedTrack();

void appStartTransport(bool useCountInAndThresh = true);
void appStopTransport();
void appToggleTransport();
void appSetPlaybackSpeedQuarter(int speedQuarter);
void appStepPlaybackSpeed(int deltaSteps);
void appEnsureValidRecordingSpeed();
void appSeekTape(uint32_t frame);
void appStepTape(int32_t deltaFrames);
void appStepTapeMs(int32_t deltaMs);
void appStepTapeBar32nd(int dir);
void appStepTapeBeat(int dir, uint32_t fromFrame);
void appStepTapeBy32nds(int32_t delta32nds);
void appSeekTapeTo32nd(int64_t idx32);
uint32_t frameForBar32ndIndex(int64_t idx32);

void updateMetronomeState();
void beginSession();
inline void saveSessionWhenIdle() { markSongDirty(); }

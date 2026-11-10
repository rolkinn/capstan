// app.cpp — shared application state, 9-page navigation, settings, and transport controls
#include "app.h"
#include "pages.h"
#include "page_audio.h"
#include "page_midi.h"
#include "page_system_settings.h"
#include "page_instrument_edit.h"
#include "inst_sampler.h"
#include "track_edit.h"
#include <Arduino.h>
#include <string.h>
#include <math.h>

m8::Board board;
float bpm = DEFAULT_BPM;

static Page activePage = Page::Audio;
static Page prevRow1Page = Page::SongSettings;
static Page prevRow2Page = Page::Audio;
int headerCell = 0;
int audioRow = 0, audioCell = AUD_CELL_WAVE, audioScroll = 0;
int midiRow = 0, midiCell = MID_CELL_WAVE, midiScroll = 0;
uint8_t timelineZoom = ZOOM_16_BARS;

FLASHMEM void syncAudioScroll() {
  if (audioRow >= 0 && audioRow < TRACK_ROWS) {
    if (audioRow < audioScroll) {
      audioScroll = audioRow;
    } else if (audioRow >= audioScroll + VISIBLE_TRACK_ROWS) {
      audioScroll = audioRow - VISIBLE_TRACK_ROWS + 1;
    }
  }
  audioScroll = constrain(audioScroll, 0, max(0, TRACK_ROWS - VISIBLE_TRACK_ROWS));
}

FLASHMEM void syncMidiScroll() {
  if (midiRow >= 0 && midiRow < TRACK_ROWS) {
    if (midiRow < midiScroll) {
      midiScroll = midiRow;
    } else if (midiRow >= midiScroll + VISIBLE_TRACK_ROWS) {
      midiScroll = midiRow - VISIBLE_TRACK_ROWS + 1;
    }
  }
  midiScroll = constrain(midiScroll, 0, max(0, TRACK_ROWS - VISIBLE_TRACK_ROWS));
}

FLASHMEM void cycleTimelineZoom() {
  timelineZoom = (uint8_t)((timelineZoom + 1) % TIMELINE_ZOOM_COUNT);
  PROGMEM static const char *const ZOOM_NAMES[TIMELINE_ZOOM_COUNT] = {
    "ZOOM: 4 BARS",
    "ZOOM: 16 BARS",
    "ZOOM: 32 BARS",
    "ZOOM: FULL"
  };
  uiShowToast(ZOOM_NAMES[timelineZoom]);
  uiForceRedraw();
}

FLASHMEM bool isHeaderRowSelected() {
  if (activePage == Page::Audio) return audioRow == HEADER_ROW;
  if (activePage == Page::Midi) return midiRow == HEADER_ROW;
  if (activePage == Page::Master) return masterRow == HEADER_ROW;
  return false;
}

FLASHMEM bool isPunchOrLoopWindowActive() {
  const SongSettings &ps = songSettings();
  return ps.punchEnabled || ps.loopEnabled;
}

static bool punchBarBeatMode = false;

FLASHMEM bool isPunchBarBeatMode() {
  return punchBarBeatMode;
}

FLASHMEM int activeInfoCellCount() {
  if (!isPunchOrLoopWindowActive()) return INFO_CELL_COUNT;
  return punchBarBeatMode ? PUNCH_INFO_BAR_CELL_COUNT : PUNCH_INFO_TIME_CELL_COUNT;
}

FLASHMEM void togglePunchBarBeatMode() {
  int oldPerBox = punchBarBeatMode ? 5 : 3;
  punchBarBeatMode = !punchBarBeatMode;
  int newPerBox = punchBarBeatMode ? 5 : 3;

  auto remapCell = [&](int &cell) {
    int box = (cell >= oldPerBox) ? 1 : 0;
    cell = box * newPerBox;
  };
  if (audioRow == INFO_ROW) remapCell(audioCell);
  if (midiRow == INFO_ROW) remapCell(midiCell);
  if (masterRow == MASTER_ROW_INFO) remapCell(masterCell);
  clampInfoCellSelection();
  uiForceRedraw();
}

FLASHMEM void clampInfoCellSelection() {
  int maxCell = activeInfoCellCount() - 1;
  if (audioRow == INFO_ROW) audioCell = constrain(audioCell, 0, maxCell);
  if (midiRow == INFO_ROW) midiCell = constrain(midiCell, 0, maxCell);
  if (masterRow == MASTER_ROW_INFO) masterCell = constrain(masterCell, 0, maxCell);
}
int masterRow = 0, masterCell = 0;
static bool masterMidiMode = false;
static int lastArmedAudioTrack = 0;
static int lastArmedMidiTrack = 0;
static bool lastArmedIsMidi = false;

bool isMasterMidiMode() {
  return masterMidiMode;
}

FLASHMEM void toggleMasterTrackMode() {
  masterMidiMode = !masterMidiMode;
  lastArmedIsMidi = masterMidiMode;
  uiForceRedraw();
}
int songSetRow = 0;
int sysSetRow = 0;
int mainRow = MAIN_ROW_SESSION, mainCol = MAIN_SESS_SAVE;
int songListCursor = 0;
int songListScroll = 0;

int instCol = 0;
int instListCursor = 0;
int instTypeCursor = 0;
int fmEditRow = FM_ROW_OP1_MAIN, fmEditCol = 1, instEditScroll = 0; // default focus on Op1 Ratio

FLASHMEM int instGenBaseRow() {
  const FmPatch &p = fmSynthPatch(constrain(instListCursor, 0, NUM_INSTRUMENTS - 1));
  return ((InstType)p.type == InstType::Sampler) ? SMP_ROW_AMP : INST_GEN_ROW_AMP;
}

FLASHMEM int instEditRowCount() {
  return instGenBaseRow() + 3;
}

FLASHMEM int fmRowColCount(int row) {
  const FmPatch &p = fmSynthPatch(constrain(instListCursor, 0, NUM_INSTRUMENTS - 1));
  InstType iType = (InstType)p.type;
  if (row == FM_ROW_HEADER) {
    return INST_HDR_COL_COUNT;
  }
  if (row >= instGenBaseRow()) {
    return 5;
  }
  if (iType == InstType::Sampler) {
    return (row == SMP_ROW_PRESS) ? 1 : SAMPLER_NUM_KEYS;
  }
  if (iType == InstType::PhaseDist) {
    return (row == 3) ? 3 : 6;
  }
  if (iType == InstType::NoiseDrone) {
    return (row == 3) ? 6 : 5;
  }
  if (row == FM_ROW_ALGO) return 1;
  return 4;
}

FLASHMEM void syncInstEditScroll() {
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const InstType iType = (InstType)fmSynthPatch(instIdx).type;
  fmEditRow = constrain(fmEditRow, 0, instEditRowCount() - 1);
  if (iType == InstType::Sampler) {
    instEditScroll = (fmEditRow >= SMP_ROW_AMP) ? 1 : 0;
    return;
  }
  if (fmEditRow == FM_ROW_HEADER) {
    instEditScroll = 0;
    return;
  }
  int bodyIdx = fmEditRow - 1;
  if (bodyIdx < instEditScroll) {
    instEditScroll = bodyIdx;
  } else if (bodyIdx >= instEditScroll + 3) {
    instEditScroll = bodyIdx - 3 + 1;
  }
  instEditScroll = constrain(instEditScroll, 0, 3);
}

char saveKeyboardBuf[MAX_SONG_NAME_LEN + 1] = "SONG_001";
int saveKeyboardRow = KB_BTN_ROW;
int saveKeyboardCol = 0;
static bool saveKeyboardActive = false;
static KeyboardTarget kbTarget = KeyboardTarget::Song;

static bool resampleModalActive = false;
static int resampleModalCursorIdx = 0; // 0 = PRE-MASTER FX, 1 = POST-MASTER FX
static int resampleModalTrackIdx = 0;
static bool resampleModalStartTransport = true;
static bool resampleModalUseCountIn = true;
static bool resamplePostFxChoice = false;

static bool clearTrackModalActive = false;
static bool clearTrackModalReveal = false;
static uint32_t clearTrackModalRevealUntilMs = 0;
static int clearTrackModalRowIdx = 0;    // 0 = CLEAR, 1 = RTZ/SEEK IN/SEEK OUT, 2 = PUNCH/LOOP/SET IN/SET OUT
static int clearTrackModalCursorIdx = 0; // Row 0: Audio 0=YES, 1=NO; MIDI 0=ALL, 1=NOTES, 2=CC/PB
static Page clearTrackModalPageVal = Page::Audio;
static int clearTrackModalTrackIdx = 0;
static constexpr uint32_t CLEAR_MODAL_REVEAL_MS = 420;

static bool instSaveToModalActive = false;
static int instSaveToModalCursorIdx = 0; // 0 = SONG, 1 = PRESET

static bool deleteConfirmPending = false;
static uint8_t deleteConfirmAct = MAIN_ROW_ACT_DELETE;
static int deleteConfirmIdx = -1;
static uint32_t deleteConfirmUntilMs = 0;

static SongSettings songCfg;
static SystemSettings sysCfg;

Page currentPage() { return activePage; }
FLASHMEM Page lastRow1Page() { return prevRow1Page; }
FLASHMEM Page lastRow2Page() { return prevRow2Page; }

FLASHMEM void syncInstListCursor() {
  instListCursor = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
}

FLASHMEM void setPage(Page p) {
  if (activePage == p) return;
  const Page prevPage = activePage;
  if (prevPage == Page::Audio) {
    audioRememberTrackCursor();
  } else if (prevPage == Page::Midi) {
    midiRememberTrackCursor();
  }
  saveKeyboardActive = false;
  resampleModalActive = false;
  clearTrackModalActive = false;
  clearTrackModalReveal = false;
  instSaveToModalActive = false;
  deleteConfirmPending = false;
  closeSamplerBrowser();
  closeSamplerKeyModal();
  closeInstBrowser();
  closeSysPresetsModal();
  closeSysProjectsModal();
  closeSysMidiMapModal();
  closeUsbSavePromptModal();
  closeTypePresetModal();
  closeMidiNoteEditor();
  trackEditReset();
  if (p == Page::SystemSettings || p == Page::SongSettings || p == Page::Main) {
    prevRow1Page = p;
  } else if (p == Page::Audio || p == Page::Midi || p == Page::Instruments ||
             p == Page::InstrumentType || p == Page::InstrumentEdit) {
    prevRow2Page = p;
  }
  if (p == Page::Audio && prevPage != Page::Midi) {
    audioRestoreLastTrackCursor();
  } else if (p == Page::Midi && prevPage != Page::Audio) {
    midiRestoreLastTrackCursor();
  }
  activePage = p;
  const PageOps &ops = pageOpsFor(p);
  if (ops.onEnter) {
    ops.onEnter();
  }
  uiGeneration++;
}

FLASHMEM bool isSaveKeyboardOpen() {
  return saveKeyboardActive;
}

FLASHMEM KeyboardTarget saveKeyboardTarget() {
  return kbTarget;
}

FLASHMEM static void openKeyboardModal(KeyboardTarget target, const char *initialText, bool startInGrid) {
  deleteConfirmPending = false;
  resampleModalActive = false;
  instSaveToModalActive = false;
  closeSamplerBrowser();
  closeSamplerKeyModal();
  closeInstBrowser();
  closeSysPresetsModal();
  closeSysProjectsModal();
  closeSysMidiMapModal();
  closeUsbSavePromptModal();
  closeTypePresetModal();
  kbTarget = target;
  snprintf(saveKeyboardBuf, sizeof(saveKeyboardBuf), "%s", initialText ? initialText : "");
  saveKeyboardRow = startInGrid ? 0 : KB_BTN_ROW;
  saveKeyboardCol = 0;
  saveKeyboardActive = true;
  uiForceRedraw();
}

FLASHMEM void openSaveKeyboard(bool startInGrid) {
  openKeyboardModal(KeyboardTarget::Song, currentSongName(), startInGrid);
}

FLASHMEM void openInstrumentKeyboard(int instIdx, bool startInGrid) {
  instListCursor = constrain(instIdx, 0, NUM_INSTRUMENTS - 1);
  openKeyboardModal(KeyboardTarget::Instrument, fmSynthPatch(instListCursor).name, startInGrid);
}

FLASHMEM void openProjectKeyboard(bool isRename, const char *initialText, bool startInGrid) {
  deleteConfirmPending = false;
  resampleModalActive = false;
  instSaveToModalActive = false;
  closeSamplerBrowser();
  closeSamplerKeyModal();
  closeInstBrowser();
  closeSysPresetsModal();
  closeUsbSavePromptModal();
  closeTypePresetModal();
  kbTarget = isRename ? KeyboardTarget::ProjectRename : KeyboardTarget::ProjectNew;
  snprintf(saveKeyboardBuf, sizeof(saveKeyboardBuf), "%s", initialText ? initialText : "");
  saveKeyboardRow = startInGrid ? 0 : KB_BTN_ROW;
  saveKeyboardCol = 0;
  saveKeyboardActive = true;
  uiForceRedraw();
}

FLASHMEM void closeSaveKeyboard() {
  if (!saveKeyboardActive) return;
  saveKeyboardActive = false;
  uiForceRedraw();
}

FLASHMEM bool isResampleModalOpen() {
  return resampleModalActive;
}

FLASHMEM int resampleModalCursor() {
  return resampleModalCursorIdx;
}

FLASHMEM int resampleModalTrack() {
  return resampleModalTrackIdx;
}

FLASHMEM bool isResamplePostFx() {
  return resamplePostFxChoice;
}

FLASHMEM static void startTransportConfirmed(bool useCountInAndThresh) {
  if (anyTrackArmed()) {
    appEnsureValidRecordingSpeed();
  }
  bool waitAudio = false;
  bool waitMidi = false;
  if (useCountInAndThresh) {
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].armMode != ArmMode::Off && tracks[t].trigMode == TrigMode::Threshold) {
        waitAudio = true;
        break;
      }
    }
    for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
      if (midiTracks[t].armMode != ArmMode::Off &&
          midiTracks[t].trigMode == TrigMode::Threshold &&
          midiTracks[t].inChannel != MIDI_IN_OFF) {
        waitMidi = true;
        break;
      }
    }
  }
  bool useCountIn = useCountInAndThresh && anyTrackArmed() && !waitAudio && !waitMidi && (songCfg.countInBeats > 0);
  storageStartTransport(useCountIn, waitAudio, waitMidi);
}

FLASHMEM void openResampleModal(int track, bool startTransportOnConfirm, bool useCountInAndThresh) {
  deleteConfirmPending = false;
  saveKeyboardActive = false;
  resampleModalTrackIdx = constrain(track, 0, NUM_AUDIO_TRACKS - 1);
  resampleModalStartTransport = startTransportOnConfirm;
  resampleModalUseCountIn = useCountInAndThresh;
  resampleModalCursorIdx = resamplePostFxChoice ? 1 : 0;
  resampleModalActive = true;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void closeResampleModal(bool cancelled) {
  if (!resampleModalActive) return;
  bool wasRunningStart = !resampleModalStartTransport;
  int trk = resampleModalTrackIdx;
  resampleModalActive = false;
  if (cancelled && wasRunningStart && trk >= 0 && trk < NUM_AUDIO_TRACKS) {
    tracks[trk].armMode = ArmMode::Off;
    storageSyncArmedAudioTrack();
    updateMetronomeState();
  }
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void resampleModalMove(int delta) {
  if (!resampleModalActive || delta == 0) return;
  resampleModalCursorIdx = (resampleModalCursorIdx + delta + 2) % 2;
  uiForceRedraw();
}

FLASHMEM void resampleModalConfirm() {
  if (!resampleModalActive) return;
  resamplePostFxChoice = (resampleModalCursorIdx == 1);
  bool doStart = resampleModalStartTransport;
  bool useCi = resampleModalUseCountIn;
  resampleModalActive = false;
  inputSwallowModalKeys();
  if (doStart) {
    startTransportConfirmed(useCi);
  } else {
    storageSyncArmedAudioTrack();
    updateMetronomeState();
  }
  uiForceRedraw();
}

FLASHMEM bool isClearTrackModalOpen() {
  return clearTrackModalActive;
}

FLASHMEM bool isInstSaveToModalOpen() {
  return instSaveToModalActive;
}

FLASHMEM int instSaveToModalCursor() {
  return instSaveToModalCursorIdx;
}

FLASHMEM void openInstSaveToModal() {
  saveKeyboardActive = false;
  closeSamplerBrowser();
  closeSamplerKeyModal();
  closeInstBrowser();
  instSaveToModalCursorIdx = 0;
  instSaveToModalActive = true;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void closeInstSaveToModal() {
  if (!instSaveToModalActive) return;
  instSaveToModalActive = false;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void instSaveToModalMove(int delta) {
  if (!instSaveToModalActive || delta == 0) return;
  instSaveToModalCursorIdx = (instSaveToModalCursorIdx + delta + 2) % 2;
  uiForceRedraw();
}

FLASHMEM bool instSaveToModalConfirm() {
  if (!instSaveToModalActive) return false;
  int choice = instSaveToModalCursorIdx;
  instSaveToModalActive = false;
  inputSwallowModalKeys();
  int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  bool ok = false;
  if (choice == 0) {
    ok = fmSynthSaveInstrumentToSong(instIdx);
    if (ok) {
      saveSessionWhenIdle();
      uiShowToast("SAVED TO SONG");
    } else {
      uiShowToast("SAVE FAILED");
    }
  } else {
    ok = fmSynthSaveInstrumentPreset(instIdx);
    if (ok) {
      uiShowToast("SAVED TO PRESETS");
    } else {
      uiShowToast("SAVE PRST FAILED");
    }
  }
  uiForceRedraw();
  return ok;
}

FLASHMEM bool isClearTrackModalRevealing() {
  return clearTrackModalReveal;
}

FLASHMEM int clearTrackModalRow() {
  return clearTrackModalRowIdx;
}

FLASHMEM int clearTrackModalCursor() {
  return clearTrackModalCursorIdx;
}

FLASHMEM Page clearTrackModalPage() {
  return clearTrackModalPageVal;
}

FLASHMEM int clearTrackModalTrack() {
  return clearTrackModalTrackIdx;
}

FLASHMEM static int clearTrackModalRowCols(int row, Page pg) {
  if (row == 0) return (pg == Page::Midi) ? 3 : 2;
  if (row == 1) return 3;
  return 4;
}

FLASHMEM void openClearTrackModal(Page page, int track) {
  deleteConfirmPending = false;
  saveKeyboardActive = false;
  resampleModalActive = false;
  clearTrackModalPageVal = page;
  clearTrackModalTrackIdx = constrain(track, 0, TRACK_ROWS - 1);
  clearTrackModalRowIdx = 0;
  clearTrackModalCursorIdx = 0; // Audio: 0 = YES, 1 = NO; MIDI: 0 = ALL, 1 = NOTES, 2 = CC/PB
  clearTrackModalReveal = false;
  clearTrackModalActive = true;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void closeClearTrackModal() {
  if (!clearTrackModalActive && !clearTrackModalReveal) return;
  clearTrackModalActive = false;
  clearTrackModalReveal = false;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void clearTrackModalMove(int dx, int dy) {
  if (clearTrackModalReveal) {
    clearTrackModalReveal = false;
    clearTrackModalActive = true;
    uiForceRedraw();
  }
  if (!clearTrackModalActive || (dx == 0 && dy == 0)) return;
  if (dy != 0) {
    clearTrackModalRowIdx = (clearTrackModalRowIdx + dy + 3) % 3;
    int cols = clearTrackModalRowCols(clearTrackModalRowIdx, clearTrackModalPageVal);
    clearTrackModalCursorIdx = constrain(clearTrackModalCursorIdx, 0, cols - 1);
  }
  if (dx != 0) {
    int cols = clearTrackModalRowCols(clearTrackModalRowIdx, clearTrackModalPageVal);
    clearTrackModalCursorIdx = (clearTrackModalCursorIdx + dx + cols) % cols;
  }
  uiForceRedraw();
}

FLASHMEM void clearTrackModalTick() {
  if (clearTrackModalReveal) {
    if ((int32_t)(millis() - clearTrackModalRevealUntilMs) >= 0) {
      clearTrackModalReveal = false;
      clearTrackModalActive = true;
      uiForceRedraw();
    }
  }
}

FLASHMEM void clearTrackModalConfirm() {
  if (clearTrackModalReveal) {
    clearTrackModalReveal = false;
    clearTrackModalActive = true;
    inputSwallowModalKeys();
    uiForceRedraw();
    return;
  }
  if (!clearTrackModalActive) return;
  int row = constrain(clearTrackModalRowIdx, 0, 2);
  Page pg = clearTrackModalPageVal;
  int cols = clearTrackModalRowCols(row, pg);
  int choice = constrain(clearTrackModalCursorIdx, 0, cols - 1);
  int trk = clearTrackModalTrackIdx;

  if (row == 0) {
    clearTrackModalActive = false;
    clearTrackModalReveal = false;
    inputSwallowModalKeys();
    if (pg == Page::Midi) {
      if (looper::transport.mode != looper::TransportMode::Stopped) {
        uiShowToast("STOP TRANSPORT TO CLEAR");
      } else if (choice == 0) {
        midiSeqClearTrackMode(trk, MidiClearMode::All);
        saveSessionWhenIdle();
        uiShowToast("MIDI TRACK CLEARED");
      } else if (choice == 1) {
        midiSeqClearTrackMode(trk, MidiClearMode::Notes);
        saveSessionWhenIdle();
        uiShowToast("MIDI NOTES CLEARED");
      } else {
        midiSeqClearTrackMode(trk, MidiClearMode::CcPb);
        saveSessionWhenIdle();
        uiShowToast("MIDI CC/PB CLEARED");
      }
    } else if (choice == 0) {
      if (looper::transport.mode != looper::TransportMode::Stopped) {
        uiShowToast("STOP TRANSPORT TO CLEAR");
      } else {
        clearAudioTrack(trk);
        uiShowToast("AUDIO TRACK CLEARED");
      }
    }
    uiForceRedraw();
    return;
  }

  if (row == 1) {
    clearTrackModalActive = false;
    clearTrackModalReveal = false;
    inputSwallowModalKeys();
    const SongSettings &ps = songSettings();
    if (choice == 0) {
      appSeekTape(0);
    } else if (choice == 1) {
      appSeekTape(ps.punchInFrame);
    } else {
      appSeekTape(ps.punchOutFrame);
    }
    uiForceRedraw();
    return;
  }

  // Row 2: [PUNCH ON/OFF]  [LOOP ON/OFF]  [SET IN]  [SET OUT]
  // Perform action, briefly close the overlay, then open the overlay back up
  SongSettings &ps = songSettings();
  if (choice == 0) {
    ps.punchEnabled = !ps.punchEnabled;
    storageSyncLoopState();
    clampInfoCellSelection();
    saveSessionWhenIdle();
  } else if (choice == 1) {
    ps.loopEnabled = !ps.loopEnabled;
    storageSyncLoopState();
    clampInfoCellSelection();
    saveSessionWhenIdle();
  } else if (choice == 2) {
    applyPunchInWithAutoOut(ps, transport.tapeFrame, isPunchBarBeatMode());
    storageSyncLoopState();
    saveSessionWhenIdle();
  } else {
    if (transport.tapeFrame > 0) {
      applyPunchOutWithAutoIn(ps, transport.tapeFrame, isPunchBarBeatMode());
      storageSyncLoopState();
      saveSessionWhenIdle();
    } else {
      uiShowToast("MUST BE AFTER ZERO");
    }
  }
  clearTrackModalActive = false;
  clearTrackModalReveal = true;
  clearTrackModalRevealUntilMs = millis() + CLEAR_MODAL_REVEAL_MS;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static bool isMainRowConfirmActive(uint8_t act) {
  if (!deleteConfirmPending || deleteConfirmAct != act) return false;
  if ((int32_t)(deleteConfirmUntilMs - millis()) <= 0) {
    deleteConfirmPending = false;
    return false;
  }
  return true;
}

FLASHMEM bool isLoadConfirmActive() {
  return isMainRowConfirmActive(MAIN_ROW_ACT_LOAD);
}

FLASHMEM bool isDeleteConfirmActive() {
  return isMainRowConfirmActive(MAIN_ROW_ACT_DELETE);
}

FLASHMEM int deleteConfirmTargetIndex() {
  return deleteConfirmIdx;
}

FLASHMEM static void armMainRowConfirm(int songIdx, uint8_t act) {
  deleteConfirmPending = true;
  deleteConfirmAct = act;
  deleteConfirmIdx = songIdx;
  deleteConfirmUntilMs = millis() + 3000;
}

FLASHMEM void armLoadConfirm(int songIdx) {
  armMainRowConfirm(songIdx, MAIN_ROW_ACT_LOAD);
}

FLASHMEM void armDeleteConfirm(int songIdx) {
  armMainRowConfirm(songIdx, MAIN_ROW_ACT_DELETE);
}

FLASHMEM void cancelDeleteConfirm() {
  deleteConfirmPending = false;
}

FLASHMEM void syncSongListCursor(bool snapToActive) {
  int total = songCount();
  int listRows = total + 1;
  if (snapToActive) {
    int idx = findSongIndex(currentSongName());
    if (idx >= 0) {
      songListCursor = idx;
    }
  }
  songListCursor = constrain(songListCursor, 0, listRows - 1);
  if (songListCursor < songListScroll) {
    songListScroll = songListCursor;
  } else if (songListCursor >= songListScroll + MAIN_LIST_VISIBLE_ROWS) {
    songListScroll = songListCursor - MAIN_LIST_VISIBLE_ROWS + 1;
  }
  int maxScroll = max(0, listRows - MAIN_LIST_VISIBLE_ROWS);
  songListScroll = constrain(songListScroll, 0, maxScroll);
}

SongSettings &songSettings() { return songCfg; }
SystemSettings &systemSettings() { return sysCfg; }

FLASHMEM void applySystemSettings() {
  sysCfg.brightness = (uint8_t)constrain((int)sysCfg.brightness, (int)MIN_BRIGHTNESS, (int)MAX_BRIGHTNESS);
  sysCfg.metroVolume = (uint8_t)constrain((int)sysCfg.metroVolume, 0, (int)VOLUME_MAX);
  if (!isScreenBlanked()) {
    board.setBacklight(brightnessToPwm(sysCfg.brightness));
  }
  setMetronomeVolume(sysCfg.metroVolume);
}

FLASHMEM void applyMasterFxSettings() {
  setMasterChorus(songCfg.masterChorus);
  setMasterDelay(songCfg.masterDelay);
  setMasterDelayFeedback(songCfg.masterDelayFeedback);
  setMasterReverb(songCfg.masterReverb);
  setMasterTape(songCfg.masterTape);
  setMasterComp(songCfg.masterComp);
  setMasterVolume(songCfg.masterVolume);
}

FLASHMEM void applySongSettings() {
  songCfg.inputMonitor = (songCfg.lineMonitor != MonitorMode::Off);
  songCfg.usbInputMonitor = (songCfg.usbMonitor != MonitorMode::Off);
  lineMonitorMode = songCfg.lineMonitor;
  usbMonitorMode = songCfg.usbMonitor;
  recLineInEnabled = songCfg.recLineIn;
  recUsbInEnabled = songCfg.recUsbIn;
  recInstrEnabled = songCfg.recInternalInstr;
  if (songCfg.audioInputSource != AudioInputSource::MicAndSynth &&
      songCfg.audioInputSource != AudioInputSource::MicOnly) {
    if (songCfg.recLineIn && songCfg.recUsbIn) {
      songCfg.audioInputSource = AudioInputSource::All;
    } else if (songCfg.recLineIn && songCfg.recInternalInstr) {
      songCfg.audioInputSource = AudioInputSource::LineAndSynth;
    } else if (songCfg.recUsbIn && songCfg.recInternalInstr) {
      songCfg.audioInputSource = AudioInputSource::UsbAndSynth;
    } else if (songCfg.recLineIn) {
      songCfg.audioInputSource = AudioInputSource::LineOnly;
    } else if (songCfg.recUsbIn) {
      songCfg.audioInputSource = AudioInputSource::UsbOnly;
    } else {
      songCfg.audioInputSource = AudioInputSource::SynthOnly;
    }
  }
  audioInputSource = songCfg.audioInputSource;

  static int8_t lastCodecMicMode = 0;
  int8_t wantMic = (songCfg.audioInputSource == AudioInputSource::MicAndSynth ||
                    songCfg.audioInputSource == AudioInputSource::MicOnly) ? 1 : 0;
  if (wantMic != lastCodecMicMode) {
    lastCodecMicMode = wantMic;
    if (wantMic) {
      codec.inputSelect(AUDIO_INPUT_MIC);
      codec.micGain(30);
    } else {
      codec.inputSelect(AUDIO_INPUT_LINEIN);
      codec.micGain(0);
      codec.lineInLevel(8);
    }
  }

  applyMasterFxSettings();
}

FLASHMEM bool setTempo(float b) {
  b = roundf(constrain(b, MIN_BPM, MAX_BPM) * 10.0f) / 10.0f;
  if (b == bpm && songCfg.bpm == b) return false;
  bpm = b;
  songCfg.bpm = b;
  transport.pendingSpb = spbForBpm(b);
  midiSeqRebuildAllOverviews(bpm, songMaxFrames());
  markSongDirty();
  return true;
}

FLASHMEM void followTempo(uint32_t spb, float shownBpm) {
  bpm = shownBpm;
  if (spb != transport.spb) {
    transport.pendingSpb = spb;
  }
}

FLASHMEM bool anyAudioTrackArmed() {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].armMode != ArmMode::Off) return true;
  }
  return false;
}

FLASHMEM bool anyMidiTrackArmed() {
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    if (midiTracks[t].armMode != ArmMode::Off) return true;
  }
  return false;
}

FLASHMEM bool anyTrackArmed() {
  return anyAudioTrackArmed() || anyMidiTrackArmed();
}

FLASHMEM void armAudioTrack(int t, ArmMode mode) {
  if (t < 0 || t >= NUM_AUDIO_TRACKS) return;
  ArmMode prevMode = tracks[t].armMode;
  if (mode != ArmMode::Off) {
    // Only one audio track records at a time
    for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
      if (i != t) tracks[i].armMode = ArmMode::Off;
    }
    lastArmedAudioTrack = t;
    lastArmedIsMidi = false;
  }
  tracks[t].armMode = mode;
  if (mode != ArmMode::Off) {
    appEnsureValidRecordingSpeed();
  }
  if (transport.mode != TransportMode::Stopped && mode != ArmMode::Off &&
      prevMode == ArmMode::Off && tracks[t].trigMode == TrigMode::Resample) {
    openResampleModal(t, false, false);
    return;
  }
  storageSyncArmedAudioTrack();
  updateMetronomeState();
  saveSessionWhenIdle();
}

FLASHMEM const char *audioTrackArmTag(int t) {
  if (!isAudioTrackMono(t)) return nullptr;
  if (tracks[t].armMode == ArmMode::Off) return nullptr;
  switch (tracks[t].monoInput) {
    case MonoInput::Left:  return "L";
    case MonoInput::Right: return "R";
    case MonoInput::SumLR:
    default:               return "LR";
  }
}

FLASHMEM void stepAudioTrackArm(int t, int delta) {
  if (t < 0 || t >= NUM_AUDIO_TRACKS || delta == 0) return;
  if (!isAudioTrackMono(t)) {
    int m = ((int)tracks[t].armMode + delta) % 3;
    if (m < 0) m += 3;
    armAudioTrack(t, (ArmMode)m);
    return;
  }

  // Mono tracks (05..08) cycle through 7 states:
  // 0: Off, 1: Rec L, 2: Rec R, 3: Rec LR, 4: Dub L, 5: Dub R, 6: Dub LR
  int curState = 0;
  int mIn = constrain((int)tracks[t].monoInput, 0, 2);
  if (tracks[t].armMode == ArmMode::Record) {
    curState = 1 + mIn;
  } else if (tracks[t].armMode == ArmMode::Overdub) {
    curState = 4 + mIn;
  }

  int nextState = (curState + delta) % 7;
  if (nextState < 0) nextState += 7;

  if (nextState == 0) {
    armAudioTrack(t, ArmMode::Off);
  } else if (nextState <= 3) {
    tracks[t].monoInput = (MonoInput)(nextState - 1);
    armAudioTrack(t, ArmMode::Record);
    saveSessionWhenIdle();
  } else {
    tracks[t].monoInput = (MonoInput)(nextState - 4);
    armAudioTrack(t, ArmMode::Overdub);
    saveSessionWhenIdle();
  }
}

FLASHMEM void armMidiTrack(int t, ArmMode mode) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  ArmMode prevMode = midiTracks[t].armMode;
  if (mode != ArmMode::Off) {
    lastArmedMidiTrack = t;
    lastArmedIsMidi = true;
  }
  midiTracks[t].armMode = mode;
  if (mode != ArmMode::Off) {
    appEnsureValidRecordingSpeed();
    if (prevMode == ArmMode::Off && !midiTracks[t].recWindowActive) {
      midiTracks[t].recEraseCursorTick = framesToMidiTicks(transport.tapeFrame, bpm);
    }
  }
  updateMetronomeState();
  saveSessionWhenIdle();
}

FLASHMEM void toggleLastArmedTrack() {
  bool armedRec = false;
  bool armedDub = false;
  for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
    if (tracks[i].armMode == ArmMode::Record) {
      armedRec = true;
      lastArmedAudioTrack = i;
    } else if (tracks[i].armMode == ArmMode::Overdub) {
      armedDub = true;
      lastArmedAudioTrack = i;
    }
  }
  for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
    if (midiTracks[i].armMode == ArmMode::Record) {
      armedRec = true;
      lastArmedMidiTrack = i;
    } else if (midiTracks[i].armMode == ArmMode::Overdub) {
      armedDub = true;
      lastArmedMidiTrack = i;
    }
  }

  // If any Audio or MIDI track is currently armed, step all armed track(s)
  // through Record -> Overdub -> Off regardless of which page is active.
  if (armedRec || armedDub) {
    ArmMode next = armedRec ? ArmMode::Overdub : ArmMode::Off;
    for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
      if (tracks[i].armMode != ArmMode::Off) {
        armAudioTrack(i, next);
      }
    }
    for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
      if (midiTracks[i].armMode != ArmMode::Off) {
        armMidiTrack(i, next);
      }
    }
    return;
  }

  Page pg = currentPage();
  bool isMidi = (pg == Page::Midi)  ? true
              : (pg == Page::Audio) ? false
                                    : lastArmedIsMidi;
  if (isMidi) {
    int target = constrain(lastArmedMidiTrack, 0, NUM_MIDI_TRACKS - 1);
    armMidiTrack(target, ArmMode::Record);
  } else {
    int target = constrain(lastArmedAudioTrack, 0, NUM_AUDIO_TRACKS - 1);
    armAudioTrack(target, ArmMode::Record);
  }
}

FLASHMEM void toggleCursorOrLastArmedTrack() {
  Page pg = currentPage();
  int curAudTrk = -1;
  int curMidTrk = -1;

  if (pg == Page::Audio) {
    if (audioRow >= 0 && audioRow < NUM_AUDIO_TRACKS) {
      curAudTrk = audioRow;
    }
  } else if (pg == Page::Midi) {
    if (midiRow >= 0 && midiRow < NUM_MIDI_TRACKS) {
      curMidTrk = midiRow;
    }
  } else if (pg == Page::Master) {
    int trk = -1;
    if (masterRow == MASTER_ROW_TRK_PARAM1 || masterRow == MASTER_ROW_TRK_PARAM2) {
      trk = constrain(masterCell, 0, TRACK_ROWS - 1);
    } else if (masterRow == MASTER_ROW_TRK_MS) {
      trk = constrain(masterCell / 2, 0, TRACK_ROWS - 1);
    }
    if (trk >= 0) {
      if (isMasterMidiMode()) {
        curMidTrk = trk;
      } else {
        curAudTrk = trk;
      }
    }
  }

  if (curAudTrk >= 0) {
    ArmMode cur = tracks[curAudTrk].armMode;
    ArmMode next = (cur == ArmMode::Off)    ? ArmMode::Record
                 : (cur == ArmMode::Record) ? ArmMode::Overdub
                                            : ArmMode::Off;
    armAudioTrack(curAudTrk, next);
    return;
  }

  if (curMidTrk >= 0) {
    ArmMode cur = midiTracks[curMidTrk].armMode;
    ArmMode next = (cur == ArmMode::Off)    ? ArmMode::Record
                 : (cur == ArmMode::Record) ? ArmMode::Overdub
                                            : ArmMode::Off;
    armMidiTrack(curMidTrk, next);
    return;
  }

  // Cursor is not on an audio or MIDI track: arm/cycle the last armed track
  for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
    if (tracks[i].armMode != ArmMode::Off) {
      lastArmedAudioTrack = i;
      lastArmedIsMidi = false;
    }
  }
  for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
    if (midiTracks[i].armMode != ArmMode::Off) {
      lastArmedMidiTrack = i;
      lastArmedIsMidi = true;
    }
  }
  if (lastArmedIsMidi) {
    int t = constrain(lastArmedMidiTrack, 0, NUM_MIDI_TRACKS - 1);
    ArmMode cur = midiTracks[t].armMode;
    ArmMode next = (cur == ArmMode::Off)    ? ArmMode::Record
                 : (cur == ArmMode::Record) ? ArmMode::Overdub
                                            : ArmMode::Off;
    armMidiTrack(t, next);
  } else {
    int t = constrain(lastArmedAudioTrack, 0, NUM_AUDIO_TRACKS - 1);
    ArmMode cur = tracks[t].armMode;
    ArmMode next = (cur == ArmMode::Off)    ? ArmMode::Record
                 : (cur == ArmMode::Record) ? ArmMode::Overdub
                                            : ArmMode::Off;
    armAudioTrack(t, next);
  }
}

FLASHMEM void appStartTransport(bool useCountInAndThresh) {
  if (useCountInAndThresh) {
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].armMode != ArmMode::Off && tracks[t].trigMode == TrigMode::Resample) {
        openResampleModal(t, true, useCountInAndThresh);
        return;
      }
    }
  }
  startTransportConfirmed(useCountInAndThresh);
}

FLASHMEM void appStopTransport() {
  storageStopTransport();
}

FLASHMEM void appToggleTransport() {
  if (transport.mode == TransportMode::Stopped) {
    appStartTransport(true);
  } else {
    appStopTransport();
  }
}

FLASHMEM void appSetPlaybackSpeedQuarter(int speedQuarter) {
  int clamped = constrain(speedQuarter, (int)MIN_SPEED_QUARTER, (int)MAX_SPEED_QUARTER);
  if (anyTrackArmed() && clamped != DEFAULT_SPEED_QUARTER && clamped != REVERSE_1X_SPEED_QUARTER) {
    clamped = (clamped < 0) ? REVERSE_1X_SPEED_QUARTER : DEFAULT_SPEED_QUARTER;
  }
  storageSetPlaybackSpeedQuarter((int8_t)clamped);
  updateMetronomeState();
}

FLASHMEM void appEnsureValidRecordingSpeed() {
  int sq = (int)transport.speedQuarter;
  if (sq != DEFAULT_SPEED_QUARTER && sq != REVERSE_1X_SPEED_QUARTER) {
    appSetPlaybackSpeedQuarter(DEFAULT_SPEED_QUARTER);
    uiShowToast("SPEED: 1.00X");
  }
}

FLASHMEM void appStepPlaybackSpeed(int deltaSteps) {
  if (deltaSteps == 0) return;
  if (anyTrackArmed()) {
    int nextSq = (deltaSteps < 0) ? REVERSE_1X_SPEED_QUARTER : DEFAULT_SPEED_QUARTER;
    appSetPlaybackSpeedQuarter(nextSq);
    return;
  }
  int curSq = (int)transport.speedQuarter;
  int nextSq = constrain(curSq + deltaSteps, (int)MIN_SPEED_QUARTER, (int)MAX_SPEED_QUARTER);
  appSetPlaybackSpeedQuarter(nextSq);
}

FLASHMEM void appSeekTape(uint32_t frame) {
  storageSeekTape(frame);
}

FLASHMEM void appStepTape(int32_t deltaFrames) {
  int64_t next = (int64_t)transport.tapeFrame + (int64_t)deltaFrames;
  if (next < 0) next = 0;
  if (next > (int64_t)MAX_REC_FRAMES) next = MAX_REC_FRAMES;
  storageSeekTape((uint32_t)next);
}

FLASHMEM void appStepTapeMs(int32_t deltaMs) {
  int64_t curMs = (int64_t)(((uint64_t)transport.tapeFrame * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
  int64_t nextMs = constrain(curMs + (int64_t)deltaMs, (int64_t)0, (int64_t)MAX_TAPE_SECONDS * 1000LL);
  uint32_t nextFrame = (uint32_t)(((uint64_t)nextMs * SAMPLE_RATE_HZ + 500) / 1000);
  if (nextFrame > MAX_REC_FRAMES) nextFrame = MAX_REC_FRAMES;
  storageSeekTape(nextFrame);
}

FLASHMEM static uint32_t frameForBar32nd(int64_t idx32, float curBpm) {
  if (idx32 <= 0) return 0;
  double beats = (double)idx32 / 8.0;
  double sec = beats * (60.0 / (double)max(1.0f, curBpm));
  uint64_t f = (uint64_t)ceil(sec * (double)SAMPLE_RATE_HZ - 1e-9);
  return (uint32_t)min(f, (uint64_t)MAX_REC_FRAMES);
}

FLASHMEM void appStepTapeBar32nd(int dir) {
  if (dir == 0) return;
  uint32_t curFrame = transport.tapeFrame;
  double totalBeats = framesToBeats(curFrame, bpm);
  int64_t curIdx32 = max((int64_t)0, (int64_t)(totalBeats * 8.0 + 1e-6));
  uint32_t gridFrame = frameForBar32nd(curIdx32, bpm);
  bool onGrid = (curFrame <= gridFrame + 1);

  int64_t nextIdx32 = (dir > 0) ? (curIdx32 + 1)
                                : (onGrid ? max((int64_t)0, curIdx32 - 1) : curIdx32);
  storageSeekTape(frameForBar32nd(nextIdx32, bpm));
}

FLASHMEM void appStepTapeBeat(int dir, uint32_t fromFrame) {
  if (dir == 0) return;
  double totalBeats = framesToBeats(fromFrame, bpm);
  int64_t curBeat = max((int64_t)0, (int64_t)(totalBeats + 1e-6));
  uint32_t gridFrame = frameForBar32nd(curBeat * 8, bpm);
  bool onGrid = (fromFrame <= gridFrame + 1);

  int64_t nextBeat = (dir > 0) ? (curBeat + 1)
                               : (onGrid ? max((int64_t)0, curBeat - 1) : curBeat);
  storageSeekTape(frameForBar32nd(nextBeat * 8, bpm));
}

FLASHMEM void appStepTapeBy32nds(int32_t delta32nds) {
  if (delta32nds == 0) return;
  uint32_t curFrame = transport.tapeFrame;
  double totalBeats = framesToBeats(curFrame, bpm);
  int64_t curIdx32 = max((int64_t)0, (int64_t)(totalBeats * 8.0 + 1e-6));
  int64_t nextIdx32 = max((int64_t)0, curIdx32 + (int64_t)delta32nds);
  storageSeekTape(frameForBar32nd(nextIdx32, bpm));
}

FLASHMEM void appSeekTapeTo32nd(int64_t idx32) {
  storageSeekTape(frameForBar32nd(max((int64_t)0, idx32), bpm));
}

FLASHMEM uint32_t frameForBar32ndIndex(int64_t idx32) {
  return frameForBar32nd(max((int64_t)0, idx32), bpm);
}

FLASHMEM void updateMetronomeState() {
  TransportMode tm = transport.mode;
  if (tm == TransportMode::CountIn) {
    metronomeOn = true;
    return;
  }
  if (tm != TransportMode::Running) {
    metronomeOn = false;
    return;
  }
  switch (songCfg.metroMode) {
    case MetroMode::Off:
      metronomeOn = false;
      break;
    case MetroMode::RecOnly:
      metronomeOn = anyTrackArmed();
      break;
    case MetroMode::AlwaysOn:
      metronomeOn = true;
      break;
  }
}

FLASHMEM void beginSession() {
  applySystemSettings();
  applySongSettings();
  noteUserActivity();
  syncSongListCursor(true);
  mainRow = MAIN_ROW_SESSION;
  mainCol = MAIN_SESS_SAVE;
}

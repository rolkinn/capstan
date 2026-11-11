// input.cpp — Core key scanner, modal keyboard, and PageOps input dispatcher
#include "pages.h"
#include "page_audio.h"
#include "page_midi.h"
#include "page_system_settings.h"
#include "page_instruments.h"
#include "page_instrument_edit.h"
#include "track_edit.h"
#include "inst_sampler.h"
#include <stdio.h>
#include <string.h>

using namespace m8;
using namespace looper;

static uint8_t prevKeys = 0;
static uint8_t repeatDir = 0;
static uint32_t repeatAt = 0;
static uint32_t lastOptTapMs = 0;
static bool optUsedAsModifier = false;
static bool editUsedAsModifier = false;
static bool editSawShift = false;
static bool shiftUsedAsModifier = false;
static bool playUsedAsModifier = false;
static bool swallowWakeKeys = false;
static bool swallowModalKeys = false;
static bool midiEditHeld = false;
static bool midiEditUsedAsModifier = false;

FLASHMEM bool inputOptHeld() {
  return (board.keys() & BTN_OPT) != 0;
}

FLASHMEM bool inputEditHeld() {
  return ((board.keys() & BTN_EDIT) != 0) || midiEditHeld;
}

FLASHMEM void inputSwallowModalKeys() {
  swallowModalKeys = true;
}

FLASHMEM void inputClearRepeat() {
  repeatDir = 0;
}

// ============================================================================
// On-Screen Keyboard Helpers (Song & Instrument Naming Modal)
// ============================================================================
PROGMEM static const char KB_CHARS[KB_GRID_ROWS][KB_GRID_COLS] = {
  {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'},
  {'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T'},
  {'U', 'V', 'W', 'X', 'Y', 'Z', '_', '-', '0', '1'},
  {'2', '3', '4', '5', '6', '7', '8', '9', '\b', '\0'}
};

FLASHMEM static void keyboardBackspace() {
  size_t len = strlen(saveKeyboardBuf);
  if (len > 0) {
    saveKeyboardBuf[len - 1] = '\0';
    uiForceRedraw();
  }
}

FLASHMEM static void commitSaveKeyboard() {
  const KeyboardTarget tgt = saveKeyboardTarget();
  if (tgt == KeyboardTarget::ProjectNew || tgt == KeyboardTarget::ProjectRename) {
    commitProjectKeyboard(saveKeyboardBuf, tgt == KeyboardTarget::ProjectRename);
    swallowModalKeys = true;
    return;
  }
  if (tgt == KeyboardTarget::Instrument) {
    FmPatch &p = fmSynthPatch(instListCursor);
    if (saveKeyboardBuf[0] != '\0') {
      snprintf(p.name, sizeof(p.name), "%s", saveKeyboardBuf);
    }
    saveSessionWhenIdle();
    closeSaveKeyboard();
    swallowModalKeys = true;
    uiForceRedraw();
    return;
  }

  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
  }
  bool ok = saveSongAs(saveKeyboardBuf);
  closeSaveKeyboard();
  swallowModalKeys = true;
  syncSongListCursor(true);
  if (ok) {
    char msg[32];
    snprintf(msg, sizeof(msg), "SAVED: %s", currentSongName());
    uiShowToast(msg);
  } else {
    uiShowToast("SAVE FAILED");
  }
  uiForceRedraw();
}

FLASHMEM static void handleKeyboardDir(uint8_t dir) {
  int dx = ((dir & BTN_RIGHT) ? 1 : 0) - ((dir & BTN_LEFT) ? 1 : 0);
  int dy = ((dir & BTN_DOWN) ? 1 : 0) - ((dir & BTN_UP) ? 1 : 0);

  if (dy != 0) {
    if (saveKeyboardRow == KB_BTN_ROW && dy < 0) {
      saveKeyboardRow = KB_GRID_ROWS - 1;
      saveKeyboardCol = (saveKeyboardCol == 0) ? 2 : 7;
    } else if (saveKeyboardRow == KB_GRID_ROWS - 1 && dy > 0) {
      saveKeyboardRow = KB_BTN_ROW;
      saveKeyboardCol = (saveKeyboardCol < 5) ? 0 : 1;
    } else {
      saveKeyboardRow = constrain(saveKeyboardRow + dy, 0, KB_BTN_ROW);
    }
  }
  if (dx != 0) {
    if (saveKeyboardRow == KB_BTN_ROW) {
      saveKeyboardCol = constrain(saveKeyboardCol + dx, 0, 1);
    } else {
      saveKeyboardCol = (saveKeyboardCol + dx + KB_GRID_COLS) % KB_GRID_COLS;
    }
  }
  uiForceRedraw();
}

FLASHMEM static void handleKeyboardEditPress() {
  if (saveKeyboardRow == KB_BTN_ROW) {
    if (saveKeyboardCol == 0) {
      commitSaveKeyboard();
    } else {
      bool isSong = (saveKeyboardTarget() == KeyboardTarget::Song);
      closeSaveKeyboard();
      swallowModalKeys = true;
      if (isSong) {
        uiShowToast("SAVE CANCELLED");
      }
    }
    return;
  }

  if (saveKeyboardRow == 3 && saveKeyboardCol == 8) {
    keyboardBackspace();
    return;
  }
  if (saveKeyboardRow == 3 && saveKeyboardCol == 9) {
    saveKeyboardBuf[0] = '\0';
    uiForceRedraw();
    return;
  }

  char ch = KB_CHARS[saveKeyboardRow][saveKeyboardCol];
  size_t len = strlen(saveKeyboardBuf);
  if (ch != '\0' && len < MAX_SONG_NAME_LEN) {
    saveKeyboardBuf[len] = ch;
    saveKeyboardBuf[len + 1] = '\0';
    uiForceRedraw();
  }
}

// ============================================================================
// Shared Information Bar & Transport Row Actions (Audio, MIDI & Master Pages)
// ============================================================================
FLASHMEM static int64_t timeGranularityDeltaMs(int fld, int dx, int dy) {
  if (dx != 0) {
    if (fld == 0) return (int64_t)dx * 60000LL; // +/- 1 minute
    if (fld == 1) return (int64_t)dx * 1000LL;  // +/- 1 second
    return (int64_t)dx * 1LL;                   // +/- 1 millisecond
  }
  if (dy != 0) {
    if (fld == 0) return (int64_t)dy * 2LL * 60000LL; // +/- 2 minutes
    if (fld == 1) return (int64_t)dy * 5LL * 1000LL;  // +/- 5 seconds
    return (int64_t)dy * 100LL;                       // +/- 0.100 seconds (100 ms)
  }
  return 0;
}

FLASHMEM void handleHeaderCellOptAdjust(int cell, int dx, int dy) {
  int c = constrain(cell, 0, HEADER_CELL_COUNT - 1);
  if (c <= HDR_CELL_MS) {
    int64_t deltaMs = timeGranularityDeltaMs(c, dx, dy);
    if (deltaMs != 0) {
      appStepTapeMs((int32_t)deltaMs);
    }
  } else {
    int step = (dx != 0) ? dx : dy;
    if (step != 0) {
      PROGMEM static const int32_t SUB32_MULT[5] = {32, 8, 4, 2, 1}; // Bar, Beat, 8th, 16th, 32nd
      appStepTapeBy32nds((int32_t)step * SUB32_MULT[c - HDR_CELL_BAR]);
    }
  }
  trackEditOnPlayheadAdjusted();
  uiForceRedraw();
}

static bool headerSnapNextOut = false;

FLASHMEM void handleHeaderCellEditPress(bool shiftHeld) {
  if (trackEditPhase() == TrackEditPhase::PickPastePos) {
    if (!shiftHeld) {
      trackEditOpenPasteMenu();
    }
    return;
  }
  if (!shiftHeld) return;
  const SongSettings &ps = songSettings();
  bool goOut = headerSnapNextOut;
  if (transport.mode == TransportMode::Stopped) {
    goOut = (transport.tapeFrame == ps.punchInFrame);
  }
  if (goOut) {
    appSeekTape(ps.punchOutFrame);
    headerSnapNextOut = false;
    uiShowToast("SEEK TO PUNCH-OUT");
  } else {
    appSeekTape(ps.punchInFrame);
    headerSnapNextOut = true;
    uiShowToast("SEEK TO PUNCH-IN");
  }
  uiForceRedraw();
}

FLASHMEM void handleHeaderCellReset(int cell) {
  int c = constrain(cell, 0, HEADER_CELL_COUNT - 1);
  if (c <= HDR_CELL_MS) {
    uint32_t tf = transport.tapeFrame;
    uint32_t totalMs = (uint32_t)(((uint64_t)tf * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
    uint32_t totalSec = totalMs / 1000;
    uint32_t ms = totalMs % 1000;
    uint32_t mm = totalSec / 60;
    uint32_t ss = totalSec % 60;

    if (c == HDR_CELL_MIN) mm = 0;
    else if (c == HDR_CELL_SEC) ss = 0;
    else ms = 0;

    uint64_t nextMs = ((uint64_t)mm * 60ULL + (uint64_t)ss) * 1000ULL + (uint64_t)ms;
    uint64_t maxMs = (uint64_t)MAX_TAPE_SECONDS * 1000ULL;
    if (nextMs > maxMs) nextMs = maxMs;
    uint32_t nextFrame = (uint32_t)((nextMs * SAMPLE_RATE_HZ + 500ULL) / 1000ULL);
    if (nextFrame > MAX_REC_FRAMES) nextFrame = MAX_REC_FRAMES;
    appSeekTape(nextFrame);
  } else {
    double totalBeats = framesToBeats(transport.tapeFrame, bpm);
    int64_t curIdx32 = max((int64_t)0, (int64_t)(totalBeats * 8.0 + 1e-6));
    int64_t nextIdx32 = 0;
    switch (c) {
      case HDR_CELL_BAR:  nextIdx32 = 0; break;
      case HDR_CELL_BEAT: nextIdx32 = (curIdx32 / 32) * 32; break;
      case HDR_CELL_8TH:  nextIdx32 = (curIdx32 / 8) * 8; break;
      case HDR_CELL_16TH: nextIdx32 = (curIdx32 / 4) * 4; break;
      case HDR_CELL_32ND: nextIdx32 = (curIdx32 / 2) * 2; break;
    }
    appSeekTapeTo32nd(nextIdx32);
  }
  trackEditOnPlayheadAdjusted();
  uiForceRedraw();
}

FLASHMEM void applyPunchInWithAutoOut(SongSettings &ps, uint32_t nextInFrame, bool barBeatMode) {
  uint32_t clampedIn = min(nextInFrame, (uint32_t)MAX_REC_FRAMES - 1u);
  ps.punchInFrame = clampedIn;
  if (ps.punchOutFrame <= ps.punchInFrame) {
    if (barBeatMode) {
      int64_t inIdx32 = max((int64_t)0, (int64_t)(framesToBeats(ps.punchInFrame, bpm) * 8.0 + 1e-6));
      ps.punchOutFrame = max(ps.punchInFrame + 1u, frameForBar32ndIndex(inIdx32 + 32));
    } else {
      uint32_t oneBarFrames = max(1u, beatsToFrames(BEATS_PER_BAR, bpm));
      ps.punchOutFrame = max(ps.punchInFrame + 1u,
                             min((uint32_t)MAX_REC_FRAMES, ps.punchInFrame + oneBarFrames));
    }
  }
}

FLASHMEM void applyPunchOutWithAutoIn(SongSettings &ps, uint32_t nextOutFrame, bool barBeatMode) {
  uint32_t clampedOut = constrain(nextOutFrame, 1u, (uint32_t)MAX_REC_FRAMES);
  ps.punchOutFrame = clampedOut;
  if (ps.punchOutFrame <= ps.punchInFrame) {
    if (barBeatMode) {
      int64_t outIdx32 = max((int64_t)1, (int64_t)(framesToBeats(ps.punchOutFrame, bpm) * 8.0 + 1e-6));
      int64_t nextIn32 = max((int64_t)0, outIdx32 - 32);
      ps.punchInFrame = min(ps.punchOutFrame - 1u, frameForBar32ndIndex(nextIn32));
    } else {
      uint32_t oneBarFrames = max(1u, beatsToFrames(BEATS_PER_BAR, bpm));
      uint32_t sub = (ps.punchOutFrame > oneBarFrames) ? (ps.punchOutFrame - oneBarFrames) : 0u;
      ps.punchInFrame = min(ps.punchOutFrame - 1u, sub);
    }
  }
}

FLASHMEM void handleInfoCellOptAdjust(int cell, int dx, int dy, int delta) {
  SongSettings &ps = songSettings();
  if (isPunchOrLoopWindowActive()) {
    if (isPunchBarBeatMode()) {
      int c = constrain(cell, 0, PUNCH_INFO_BAR_CELL_COUNT - 1);
      int b = c / 5;
      int fld = c % 5;
      int step = (dx != 0) ? dx : dy;
      if (step != 0) {
        PROGMEM static const int32_t SUB32_MULT[5] = {32, 8, 4, 2, 1}; // Bar, Beat, 8th, 16th, 32nd
        int64_t delta32 = (int64_t)step * SUB32_MULT[fld];
        int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));
        if (b == 0) {
          int64_t inIdx32 = max((int64_t)0, (int64_t)(framesToBeats(ps.punchInFrame, bpm) * 8.0 + 1e-6));
          int64_t nextIn32 = constrain(inIdx32 + delta32, 0LL, max(0LL, maxIdx32 - 1LL));
          applyPunchInWithAutoOut(ps, frameForBar32ndIndex(nextIn32), true);
        } else {
          int64_t outIdx32 = max((int64_t)1, (int64_t)(framesToBeats(ps.punchOutFrame, bpm) * 8.0 + 1e-6));
          int64_t nextOut32 = constrain(outIdx32 + delta32, 1LL, maxIdx32);
          applyPunchOutWithAutoIn(ps, frameForBar32ndIndex(nextOut32), true);
        }
        storageSyncLoopState();
        saveSessionWhenIdle();
      }
      uiForceRedraw();
      return;
    }

    int c = constrain(cell, 0, PUNCH_INFO_TIME_CELL_COUNT - 1);
    int fld = c % 3;
    int64_t deltaMs = timeGranularityDeltaMs(fld, dx, dy);
    if (deltaMs != 0) {
      int64_t maxMs = (int64_t)MAX_TAPE_SECONDS * 1000LL;
      if (c < 3) {
        int64_t inMs = (int64_t)(((uint64_t)ps.punchInFrame * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
        int64_t nextInMs = constrain(inMs + deltaMs, 0LL, maxMs - 1LL);
        uint32_t f = (uint32_t)min((uint64_t)MAX_REC_FRAMES - 1ULL, ((uint64_t)nextInMs * SAMPLE_RATE_HZ + 500) / 1000);
        applyPunchInWithAutoOut(ps, f, false);
      } else {
        int64_t outMs = (int64_t)(((uint64_t)ps.punchOutFrame * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
        int64_t nextOutMs = constrain(outMs + deltaMs, 1LL, maxMs);
        uint32_t f = (uint32_t)min((uint64_t)MAX_REC_FRAMES, ((uint64_t)nextOutMs * SAMPLE_RATE_HZ + 500) / 1000);
        applyPunchOutWithAutoIn(ps, max(1u, f), false);
      }
      storageSyncLoopState();
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  switch (cell) {
    case INFO_CELL_BPM: {
      float step = (dx != 0) ? (dx * 0.1f) : (dy * 1.0f);
      setTempo(ps.bpm + step);
      break;
    }
    case INFO_CELL_SLOT1: {
      int m = ((int)ps.metroMode + delta + 3) % 3;
      ps.metroMode = (MetroMode)m;
      updateMetronomeState();
      saveSessionWhenIdle();
      break;
    }
    case INFO_CELL_SLOT2: {
      int next = (int)ps.countInBeats + delta * 4;
      ps.countInBeats = (uint8_t)constrain(next, 0, 16);
      saveSessionWhenIdle();
      break;
    }
  }
  uiForceRedraw();
}

FLASHMEM void handleInfoCellEditPress(int cell, bool shiftHeld) {
  if (!isPunchOrLoopWindowActive() || !shiftHeld) {
    return;
  }

  SongSettings &ps = songSettings();
  bool barBeatMode = isPunchBarBeatMode();
  int cellsPerBox = barBeatMode ? 5 : 3;
  if (cell < cellsPerBox) {
    applyPunchInWithAutoOut(ps, transport.tapeFrame, barBeatMode);
    storageSyncLoopState();
    saveSessionWhenIdle();
    uiShowToast("PUNCH-IN SET TO PLAYHEAD");
  } else {
    if (transport.tapeFrame > 0) {
      applyPunchOutWithAutoIn(ps, transport.tapeFrame, barBeatMode);
      storageSyncLoopState();
      saveSessionWhenIdle();
      uiShowToast("PUNCH-OUT SET TO PLAYHEAD");
    } else {
      uiShowToast("MUST BE AFTER ZERO");
    }
  }
  uiForceRedraw();
}

FLASHMEM void handleInfoCellReset(int cell) {
  SongSettings &ps = songSettings();
  if (isPunchOrLoopWindowActive()) {
    uint32_t oneBarFrames = max(1u, beatsToFrames(BEATS_PER_BAR, bpm));
    uint32_t defaultOutFrame = max(ps.punchInFrame + 1u,
                                   min((uint32_t)MAX_REC_FRAMES, ps.punchInFrame + oneBarFrames));

    if (isPunchBarBeatMode()) {
      int c = constrain(cell, 0, PUNCH_INFO_BAR_CELL_COUNT - 1);
      int b = c / 5;
      int fld = c % 5;
      uint32_t targetFrame = (b == 0) ? ps.punchInFrame : ps.punchOutFrame;
      int64_t curIdx32 = max((int64_t)0, (int64_t)(framesToBeats(targetFrame, bpm) * 8.0 + 1e-6));
      int64_t nextIdx32 = 0;
      switch (fld) {
        case 0: nextIdx32 = 0; break;
        case 1: nextIdx32 = (curIdx32 / 32) * 32; break;
        case 2: nextIdx32 = (curIdx32 / 8) * 8; break;
        case 3: nextIdx32 = (curIdx32 / 4) * 4; break;
        case 4: nextIdx32 = (curIdx32 / 2) * 2; break;
      }
      uint32_t nextFrame = frameForBar32ndIndex(nextIdx32);
      if (b == 0) {
        applyPunchInWithAutoOut(ps, nextFrame, true);
      } else {
        if (fld == 0 || nextFrame == 0) {
          ps.punchOutFrame = defaultOutFrame;
        } else {
          applyPunchOutWithAutoIn(ps, nextFrame, true);
        }
      }
    } else {
      int c = constrain(cell, 0, PUNCH_INFO_TIME_CELL_COUNT - 1);
      int b = c / 3;
      int fld = c % 3;
      uint32_t targetFrame = (b == 0) ? ps.punchInFrame : ps.punchOutFrame;
      uint32_t totalMs = (uint32_t)(((uint64_t)targetFrame * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
      uint32_t totalSec = totalMs / 1000;
      uint32_t ms = totalMs % 1000;
      uint32_t mm = totalSec / 60;
      uint32_t ss = totalSec % 60;

      if (fld == 0) mm = 0;
      else if (fld == 1) ss = 0;
      else ms = 0;

      uint64_t nextMs = ((uint64_t)mm * 60ULL + (uint64_t)ss) * 1000ULL + (uint64_t)ms;
      uint64_t maxMs = (uint64_t)MAX_TAPE_SECONDS * 1000ULL;
      if (nextMs > maxMs) nextMs = maxMs;
      uint32_t nextFrame = (uint32_t)((nextMs * SAMPLE_RATE_HZ + 500ULL) / 1000ULL);
      if (nextFrame > MAX_REC_FRAMES) nextFrame = MAX_REC_FRAMES;

      if (b == 0) {
        applyPunchInWithAutoOut(ps, nextFrame, false);
      } else {
        if (nextFrame == 0) {
          ps.punchOutFrame = defaultOutFrame;
        } else {
          applyPunchOutWithAutoIn(ps, nextFrame, false);
        }
      }
    }

    storageSyncLoopState();
    saveSessionWhenIdle();
    uiForceRedraw();
    return;
  }

  switch (cell) {
    case INFO_CELL_BPM:
      setTempo(DEFAULT_BPM);
      break;
    case INFO_CELL_SLOT1:
      ps.metroMode = MetroMode::RecOnly;
      updateMetronomeState();
      break;
    case INFO_CELL_SLOT2:
      ps.countInBeats = 4;
      break;
  }
  saveSessionWhenIdle();
  uiForceRedraw();
}

FLASHMEM static bool isActivePunchInfoCell() {
  if (!isPunchOrLoopWindowActive()) return false;
  Page pg = currentPage();
  if (pg == Page::Audio) return audioRow == INFO_ROW;
  if (pg == Page::Midi) return midiRow == INFO_ROW;
  if (pg == Page::Master) return masterRow == MASTER_ROW_INFO;
  return false;
}

FLASHMEM static int activeTransportScrubDir() {
  Page pg = currentPage();
  int row = -1, cell = -1;
  if (pg == Page::Audio) {
    row = audioRow;
    cell = audioCell;
  } else if (pg == Page::Midi) {
    row = midiRow;
    cell = midiCell;
  } else if (pg == Page::Master) {
    if (masterRow == MASTER_ROW_TRANSPORT) {
      row = TRANSPORT_ROW;
      cell = masterCell;
    }
  }
  if (row != TRANSPORT_ROW) return 0;
  if (cell == TR_CELL_REW) return -1;
  if (cell == TR_CELL_FF) return 1;
  return 0;
}

static int scrubHoldDir = 0;
static int waveOptScrubDir = 0;
static int optHeaderHighlightCell = -1;
static uint32_t scrubHoldStartMs = 0;
static uint32_t scrubLastTickMs = 0;
static float scrubFracFrames = 0.0f;

FLASHMEM static bool isCursorOnTrackWaveCell() {
  if (trackEditPhase() != TrackEditPhase::Off || isMidiNoteEditorOpen() ||
      isClearTrackModalOpen() || isClearTrackModalRevealing()) {
    return false;
  }
  Page pg = currentPage();
  if (pg == Page::Audio) {
    return audioRow >= 0 && audioRow < TRACK_ROWS && audioCell == AUD_CELL_WAVE;
  }
  if (pg == Page::Midi) {
    return midiRow >= 0 && midiRow < TRACK_ROWS && midiCell == MID_CELL_WAVE;
  }
  return false;
}

FLASHMEM int inputOptHeaderHighlightCell() {
  if (!inputOptHeld() || !isCursorOnTrackWaveCell()) return -1;
  return optHeaderHighlightCell;
}

static int samplerHeldInst = -1;
static int samplerHeldNote = -1;
static uint32_t samplerHeldMinUntilMs = 0;

FLASHMEM static void releaseSamplerEditHold() {
  if (samplerHeldInst >= 0 && samplerHeldNote >= 0) {
    fmSynthNoteOff(samplerHeldInst, (uint8_t)samplerHeldNote);
    samplerHeldInst = -1;
    samplerHeldNote = -1;
    samplerHeldMinUntilMs = 0;
    uiForceRedraw();
  }
}

FLASHMEM void handleTransportCellAction(int cell, bool shiftHeld) {
  switch (cell) {
    case TR_CELL_REC:
      if (shiftHeld) {
        SongSettings &ps = songSettings();
        ps.punchEnabled = !ps.punchEnabled;
        storageSyncLoopState();
        clampInfoCellSelection();
        saveSessionWhenIdle();
        uiShowToast(ps.punchEnabled ? "PUNCH WINDOW ON" : "PUNCH WINDOW OFF");
        uiForceRedraw();
      } else {
        toggleLastArmedTrack();
        uiForceRedraw();
      }
      break;
    case TR_CELL_LOOP: {
      if (shiftHeld) break;
      SongSettings &ps = songSettings();
      ps.loopEnabled = !ps.loopEnabled;
      storageSyncLoopState();
      clampInfoCellSelection();
      saveSessionWhenIdle();
      uiShowToast(ps.loopEnabled ? "LOOP ON" : "LOOP OFF");
      uiForceRedraw();
      break;
    }
    case TR_CELL_RTZ:
      appSeekTape(0);
      uiForceRedraw();
      break;
    case TR_CELL_REW:
      if (shiftHeld) {
        appSeekTape(songSettings().punchInFrame);
        uiShowToast("SEEK TO PUNCH-IN");
        uiForceRedraw();
      } else {
        appStepTapeMs(-1);
      }
      break;
    case TR_CELL_PLAY:
      if (!shiftHeld) {
        appToggleTransport();
      }
      break;
    case TR_CELL_FF:
      if (shiftHeld) {
        appSeekTape(songSettings().punchOutFrame);
        uiShowToast("SEEK TO PUNCH-OUT");
        uiForceRedraw();
      } else {
        appStepTapeMs(1);
      }
      break;
  }
}

FLASHMEM void handleTransportCellOptAdjust(int cell, int dx, int dy, int delta) {
  if (cell == TR_CELL_REC && delta != 0) {
    toggleLastArmedTrack();
    uiForceRedraw();
  } else if (cell == TR_CELL_PLAY) {
    if (dx != 0) {
      appStepPlaybackSpeed(dx);
      uiForceRedraw();
    } else if (dy > 0) {
      appSetPlaybackSpeedQuarter(DEFAULT_SPEED_QUARTER);
      uiForceRedraw();
    } else if (dy < 0) {
      appSetPlaybackSpeedQuarter(REVERSE_1X_SPEED_QUARTER);
      uiForceRedraw();
    }
  }
}

FLASHMEM void handleTransportCellReset(int cell) {
  if (cell == TR_CELL_REC) {
    for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
      if (tracks[i].armMode != ArmMode::Off) armAudioTrack(i, ArmMode::Off);
    }
    for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
      if (midiTracks[i].armMode != ArmMode::Off) armMidiTrack(i, ArmMode::Off);
    }
    uiForceRedraw();
  } else if (cell == TR_CELL_PLAY) {
    appSetPlaybackSpeedQuarter(DEFAULT_SPEED_QUARTER);
    if (transport.mode == TransportMode::Stopped) {
      appSeekTape(0);
    } else {
      uiShowToast("SPEED: 1.00X");
    }
    uiForceRedraw();
  }
}

FLASHMEM static uint8_t pollDirectionalRepeat(uint8_t pressed, uint8_t now, uint32_t ms,
                                     uint32_t repeatIntervalMs) {
  constexpr uint8_t DIR_MASK = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
  uint8_t dirPressed = pressed & DIR_MASK;
  uint8_t dirHeld = now & DIR_MASK;
  uint8_t activeDir = 0;
  if (dirPressed) {
    activeDir = dirPressed;
    repeatDir = dirPressed;
    repeatAt = ms + 220;
  } else if (dirHeld && (dirHeld == repeatDir) && (int32_t)(ms - repeatAt) >= 0) {
    activeDir = repeatDir;
    repeatAt = ms + repeatIntervalMs;
  } else if (!dirHeld) {
    repeatDir = 0;
  }
  return activeDir;
}

FLASHMEM static void confirmSamplerKeyModalOrRecord() {
  if (storageIsSamplerKeyRecordingOpen()) {
    const SamplerRecMode recMode = storageSamplerKeyRecMode();
    if (!storageIsSamplerKeyRecordTriggered()) {
      if (recMode == SamplerRecMode::ManualStartStop) {
        storageTriggerSamplerKeyRecordNow();
        swallowModalKeys = true;
      }
    } else if (recMode == SamplerRecMode::ThreshAutoManualStop ||
               recMode == SamplerRecMode::ManualStartStop) {
      storageFinishSamplerKeyRecordNow();
      swallowModalKeys = true;
    }
  } else {
    samplerKeyModalConfirm();
  }
}

FLASHMEM static void confirmSamplerBrowserSelection() {
  int targetKey = samplerBrowserTargetKey();
  if (samplerBrowserConfirm()) {
    swallowModalKeys = true;
    saveSessionWhenIdle();
    fmSynthAudition(instListCursor, (uint8_t)(SAMPLER_BASE_NOTE + targetKey), 110, 600);
    if (!samplerIsKeyPlaying(instListCursor, targetKey)) {
      uiShowToast("WAV FORMAT ERR");
    }
  }
}

FLASHMEM static void confirmInstBrowserSelection() {
  int targetSlot = instBrowserTargetSlot();
  const InstBrowserEntry *ent = instBrowserEntryAt(instBrowserCursor());
  bool wasFile = (ent != nullptr && !ent->isDir);
  if (instBrowserConfirm()) {
    swallowModalKeys = true;
    saveSessionWhenIdle();
    instListCursor = constrain(targetSlot, 0, NUM_INSTRUMENTS - 1);
    fmEditRow = constrain(fmEditRow, 0, instEditRowCount() - 1);
    syncInstEditScroll();
    fmEditCol = constrain(fmEditCol, 0, fmRowColCount(fmEditRow) - 1);
    const FmPatch &p = fmSynthPatch(instListCursor);
    if ((InstType)p.type != InstType::Sampler) {
      fmSynthAudition(instListCursor);
    }
    char msg[32];
    snprintf(msg, sizeof(msg), "LOADED %.16s", p.name);
    uiShowToast(msg);
  } else if (wasFile) {
    uiShowToast("INVALID .FMI FILE");
  }
  uiForceRedraw();
}

FLASHMEM static void confirmTypePresetSelection() {
  int slot = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  if (instBrowserEntryCount() > 0) {
    if (typePresetModalConfirm(slot)) {
      saveSessionWhenIdle();
      InstType loadedType = (InstType)fmSynthPatch(slot).type;
      fmEditRow = (loadedType == InstType::Sampler) ? SMP_ROW_KEYS
                : (loadedType == InstType::Fm2Op)   ? FM_ROW_OP1_MAIN
                                                    : 1;
      fmEditCol = 0;
      instEditScroll = 0;
      syncInstEditScroll();
      char msg[32];
      snprintf(msg, sizeof(msg), "LOADED: %s", fmSynthPatch(slot).name);
      uiShowToast(msg);
      setPage(Page::InstrumentEdit);
    } else {
      uiShowToast("LOAD FAILED");
    }
  }
  swallowModalKeys = true;
  uiForceRedraw();
}

FLASHMEM static void confirmMidiClipBrowserSelection() {
  const MidiBrowserEntry *ent = midiClipBrowserEntryAt(midiClipBrowserCursor());
  bool wasFile = (ent != nullptr && !ent->isDir);
  bool isAudio = isAudioClipBrowserMode();
  if (midiClipBrowserConfirm(bpm)) {
    trackEditCancel();
    uiShowToast(isAudio ? "LOADED AUDIO CLIP" : "LOADED MIDI CLIP");
  } else if (wasFile) {
    uiShowToast(isAudio ? "INVALID .WAV FILE" : "INVALID .MID FILE");
  }
  swallowModalKeys = true;
  uiForceRedraw();
}

FLASHMEM static void triggerSamplerKeyAuditionHold(uint32_t ms) {
  releaseSamplerEditHold();
  int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  int keyIdx = constrain(fmEditCol, 0, SAMPLER_NUM_KEYS - 1);
  uint8_t note = (uint8_t)(SAMPLER_BASE_NOTE + keyIdx);
  if (samplerHasSample(instIdx, keyIdx)) {
    fmSynthNoteOn(instIdx, note, 110);
    if (samplerIsKeyPlaying(instIdx, keyIdx)) {
      samplerHeldInst = instIdx;
      samplerHeldNote = (int)note;
      samplerHeldMinUntilMs = ms + 160;
    } else {
      uiShowToast("WAV FILE ERR");
    }
  } else {
    uiShowToast("NO SAMPLE (SHIFT+EDIT)");
  }
  uiForceRedraw();
}

// ============================================================================
// Main Key Polling Entry Point
// ============================================================================
FLASHMEM void pollKeys() {
  uint8_t now = board.keys();
  uint8_t pressed = now & ~prevKeys;
  uint8_t released = prevKeys & ~now;
  uint32_t ms = millis();

  // When the screen is blank due to inactivity, any key press wakes the screen
  // and consumes the wake gesture until all keys are released.
  if (isScreenBlanked()) {
    if (now != 0 || pressed != 0) {
      noteUserActivity();
      swallowWakeKeys = true;
      repeatDir = 0;
    }
    prevKeys = now;
    return;
  }

  if (swallowWakeKeys) {
    if (now != 0) {
      noteUserActivity();
    } else {
      swallowWakeKeys = false;
    }
    repeatDir = 0;
    prevKeys = now;
    return;
  }

  if (now != 0 || pressed != 0 || released != 0) {
    noteUserActivity();
  }

  // When Project Play button lock is active, ignore all 8 front-panel buttons
  // (UP/DOWN/LEFT/RIGHT/SHIFT/PLAY/OPT/EDIT); only a quick press of the power button unlocks.
  if (isSysProjectPlayLocked()) {
    releaseSamplerEditHold();
    repeatDir = 0;
    scrubHoldDir = 0;
    waveOptScrubDir = 0;
    optHeaderHighlightCell = -1;
    optUsedAsModifier = false;
    editUsedAsModifier = false;
    shiftUsedAsModifier = false;
    playUsedAsModifier = false;
    lastOptTapMs = 0;
    prevKeys = now;
    return;
  }

  if (swallowModalKeys) {
    if (now == 0) {
      swallowModalKeys = false;
    }
    repeatDir = 0;
    prevKeys = now;
    return;
  }

  constexpr uint8_t DIR_MASK = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;

  // Modal On-Screen Keyboard input handling when saving a song or naming an instrument
  if (isSaveKeyboardOpen()) {
    releaseSamplerEditHold();
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    if (activeDir) {
      handleKeyboardDir(activeDir);
    }
    if (pressed & BTN_OPT) {
      if (now & BTN_SHIFT) {
        bool isSong = (saveKeyboardTarget() == KeyboardTarget::Song);
        closeSaveKeyboard();
        swallowModalKeys = true;
        if (isSong) {
          uiShowToast("SAVE CANCELLED");
        }
      } else {
        keyboardBackspace();
      }
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    if (pressed & BTN_EDIT) {
      handleKeyboardEditPress();
      editUsedAsModifier = true;
    }
    if (released & BTN_PLAY) {
      commitSaveKeyboard();
      playUsedAsModifier = true;
    }
    prevKeys = now;
    return;
  }

  // Modal Sampler Piano Key Action ("[Load from file]" / "[Record from audio in]") & Threshold Recording Overlay
  if (isSamplerKeyModalOpen()) {
    releaseSamplerEditHold();
    if (storageIsSamplerKeyRecordingOpen()) {
      if (pressed & BTN_OPT) {
        closeSamplerKeyModal();
        optUsedAsModifier = true;
        lastOptTapMs = 0;
      } else if (pressed & BTN_EDIT) {
        confirmSamplerKeyModalOrRecord();
        editUsedAsModifier = true;
      }
      prevKeys = now;
      return;
    }

    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 85);
    if (activeDir) {
      int step = ((activeDir & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                 ((activeDir & (BTN_UP | BTN_LEFT)) ? 1 : 0);
      if (step != 0) {
        samplerKeyModalMove(step);
      }
    }
    if (pressed & BTN_EDIT) {
      samplerKeyModalConfirm();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      closeSamplerKeyModal();
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    prevKeys = now;
    return;
  }

  // Modal SD Card Sample Browser input handling (Sampler instrument page)
  if (isSamplerBrowserOpen()) {
    releaseSamplerEditHold();
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    if (activeDir) {
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      samplerBrowserMove(dy, dx);
    }
    if (pressed & BTN_EDIT) {
      confirmSamplerBrowserSelection();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      closeSamplerBrowser();
      swallowModalKeys = true;
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    prevKeys = now;
    return;
  }

  // Modal SD Card Instrument File Browser input handling (Instruments & Instrument Edit pages)
  if (isInstBrowserOpen()) {
    releaseSamplerEditHold();
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    if (activeDir) {
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      instBrowserMove(dy, dx);
      uiForceRedraw();
    }
    if (pressed & BTN_EDIT) {
      confirmInstBrowserSelection();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      closeInstBrowser();
      swallowModalKeys = true;
      optUsedAsModifier = true;
      lastOptTapMs = 0;
      uiForceRedraw();
    }
    prevKeys = now;
    return;
  }

  // Modal Instrument "SAVE TO..." Overlay input handling (0 = SONG, 1 = PRESET)
  if (isInstSaveToModalOpen()) {
    releaseSamplerEditHold();
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 85);
    if (activeDir) {
      int step = ((activeDir & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                 ((activeDir & (BTN_UP | BTN_LEFT)) ? 1 : 0);
      if (step != 0) {
        instSaveToModalMove(step);
      }
    }
    if (pressed & BTN_EDIT) {
      instSaveToModalConfirm();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      closeInstSaveToModal();
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    prevKeys = now;
    return;
  }

  // Modal System Settings Instrument Presets Viewer & Delete Confirmation input handling
  if (isSysPresetsModalOpen()) {
    if (isSysPresetDeleteConfirmOpen()) {
      uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 85);
      if (activeDir) {
        int step = ((activeDir & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                   ((activeDir & (BTN_UP | BTN_LEFT)) ? 1 : 0);
        if (step != 0) {
          sysPresetDeleteConfirmMove(step);
          uiForceRedraw();
        }
      }
      if (pressed & BTN_EDIT) {
        bool wasSongMode = isSysPresetsSongMode();
        if (sysPresetDeleteConfirmCommit()) {
          uiShowToast(wasSongMode ? "INSTRUMENT DELETED" : "PRESET DELETED");
        }
        swallowModalKeys = true;
        editUsedAsModifier = true;
        uiForceRedraw();
      } else if (pressed & BTN_OPT) {
        closeSysPresetDeleteConfirm();
        swallowModalKeys = true;
        optUsedAsModifier = true;
        lastOptTapMs = 0;
        uiForceRedraw();
      }
      prevKeys = now;
      return;
    }

    if ((pressed & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT))) {
      closeSysPresetsModal();
      swallowModalKeys = true;
      shiftUsedAsModifier = true;
      uiForceRedraw();
      prevKeys = now;
      return;
    }

    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    if (activeDir && !(now & BTN_SHIFT)) {
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      sysPresetsMove(dy, dx);
      uiForceRedraw();
    }
    if ((pressed & BTN_EDIT) && !(now & BTN_SHIFT)) {
      sysPresetsRequestDelete();
      swallowModalKeys = true;
      editUsedAsModifier = true;
      uiForceRedraw();
    }
    prevKeys = now;
    return;
  }

  // Modal System Settings Projects Management (/Capstan/PROJECTS/*.proj) input handling
  if (isSysProjectsModalOpen()) {
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    uint8_t effPressed = (pressed & ~DIR_MASK) | activeDir;
    uint8_t effNow = now | ((activeDir && midiEditHeld) ? BTN_EDIT : 0);
    if (activeDir && midiEditHeld) midiEditUsedAsModifier = true;
    handleSysProjectsModalInput(effPressed, effNow, ms);
    if (pressed & BTN_EDIT) editUsedAsModifier = true;
    if (pressed & BTN_OPT) {
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    if ((pressed & BTN_LEFT) && (now & BTN_SHIFT)) shiftUsedAsModifier = true;
    prevKeys = now;
    return;
  }

  // Modal System Settings MIDI Mappings input handling
  if (isSysMidiMapModalOpen()) {
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    uint8_t effPressed = (pressed & ~DIR_MASK) | activeDir;
    uint8_t effNow = now | ((activeDir && midiEditHeld) ? BTN_EDIT : 0);
    if (activeDir && midiEditHeld) midiEditUsedAsModifier = true;
    handleSysMidiMapModalInput(effPressed, effNow, ms);
    if (pressed & BTN_EDIT) editUsedAsModifier = true;
    if (pressed & BTN_OPT) {
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    if ((pressed & (BTN_LEFT | BTN_DOWN)) && (now & BTN_SHIFT)) shiftUsedAsModifier = true;
    prevKeys = now;
    return;
  }

  // Modal USB Storage Drive Save Prompt ("Save song?  YES  NO") input handling
  if (isUsbSavePromptModalOpen()) {
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 85);
    if (activeDir) {
      int step = ((activeDir & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                 ((activeDir & (BTN_UP | BTN_LEFT)) ? 1 : 0);
      if (step != 0) {
        usbSavePromptModalMove(step);
      }
    }
    if (pressed & BTN_EDIT) {
      editUsedAsModifier = true;
      usbSavePromptModalConfirm();
    } else if (pressed & BTN_OPT) {
      closeUsbSavePromptModal();
      swallowModalKeys = true;
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    prevKeys = now;
    return;
  }

  // Modal Instrument Type Page Preset Selection (`Load song preset...` / `Load global preset...`)
  if (isTypePresetModalOpen()) {
    if ((pressed & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT))) {
      closeTypePresetModal();
      swallowModalKeys = true;
      shiftUsedAsModifier = true;
      uiForceRedraw();
      prevKeys = now;
      return;
    }

    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    if (activeDir && !(now & BTN_SHIFT)) {
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      typePresetModalMove(dy, dx);
      uiForceRedraw();
    }
    if ((pressed & BTN_EDIT) && !(now & BTN_SHIFT)) {
      confirmTypePresetSelection();
      editUsedAsModifier = true;
    }
    prevKeys = now;
    return;
  }

  // Modal Clear Track Recording & Quick Transport/Punch/Loop Overlay input handling
  if (isClearTrackModalOpen() || isClearTrackModalRevealing()) {
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 85);
    if (activeDir) {
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      if (dx != 0 || dy != 0) {
        clearTrackModalMove(dx, dy);
      }
    }
    if (pressed & BTN_EDIT) {
      clearTrackModalConfirm();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      closeClearTrackModal();
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    prevKeys = now;
    return;
  }

  // Modal SD Card MIDI Clip Browser input handling ("LOAD MIDI CLIP" on MIDI Track Edit overlay)
  if (isMidiClipBrowserOpen()) {
    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
    if (activeDir) {
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      midiClipBrowserMove(dy, dx);
      uiForceRedraw();
    }
    if (pressed & BTN_EDIT) {
      confirmMidiClipBrowserSelection();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      closeMidiClipBrowser();
      invalidateTrackEditOverlayCache();
      swallowModalKeys = true;
      optUsedAsModifier = true;
      lastOptTapMs = 0;
      uiForceRedraw();
    }
    prevKeys = now;
    return;
  }

  // Modal Track Edit Overlay (ActionMenu / PasteMenu / DuplReveal) input handling
  if (trackEditIsModalOpen() || trackEditPhase() == TrackEditPhase::DuplReveal) {
    if (trackEditIsModalOpen()) {
      uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 65);
      if (activeDir) {
        trackEditModalDir(activeDir);
      }
    }
    if (pressed & BTN_EDIT) {
      trackEditModalEditPress();
      editUsedAsModifier = true;
    } else if (pressed & BTN_OPT) {
      trackEditCancel();
      optUsedAsModifier = true;
      lastOptTapMs = 0;
    }
    prevKeys = now;
    return;
  }

  // Modal MIDI Note Sequence Editor (2.5-Octave Piano Roll + 1/32nd Timeline Grid) input handling
  if (isMidiNoteEditorOpen()) {
    if (pressed & BTN_OPT) optUsedAsModifier = false;
    if (pressed & BTN_EDIT) editUsedAsModifier = false;
    if (pressed & BTN_SHIFT) shiftUsedAsModifier = false;
    if (pressed & BTN_PLAY) playUsedAsModifier = false;

    // SHIFT + OPT: Close the MIDI Note Sequence Editor
    if ((((pressed & BTN_OPT) && (now & BTN_SHIFT)) || ((pressed & BTN_SHIFT) && (now & BTN_OPT))) &&
        !(now & (BTN_EDIT | BTN_PLAY | DIR_MASK))) {
      optUsedAsModifier = true;
      shiftUsedAsModifier = true;
      lastOptTapMs = 0;
      closeMidiNoteEditor();
      prevKeys = now;
      return;
    }

    if ((((pressed & BTN_EDIT) && (now & BTN_OPT)) || ((pressed & BTN_OPT) && (now & BTN_EDIT))) &&
        !(now & BTN_SHIFT)) {
      optUsedAsModifier = true;
      editUsedAsModifier = true;
      lastOptTapMs = 0;
      midiNoteEditorResetPress();
      prevKeys = now;
      return;
    }

    // Single tap OPT does nothing; double-tap OPT cycles timeline zoom levels
    if (released & BTN_OPT) {
      if (!optUsedAsModifier && !(now & (BTN_SHIFT | BTN_EDIT | BTN_PLAY | DIR_MASK))) {
        if (ms - lastOptTapMs <= 260) {
          cycleTimelineZoom();
          lastOptTapMs = 0;
        } else {
          lastOptTapMs = ms;
        }
      } else {
        lastOptTapMs = 0;
      }
    }

    uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 55);
    if (activeDir) {
      bool shiftHeld = (now & BTN_SHIFT) != 0;
      if (shiftHeld) shiftUsedAsModifier = true;
      midiNoteEditorDir(activeDir, shiftHeld);
    }

    if ((pressed & BTN_EDIT) && !(now & BTN_OPT)) {
      bool shiftHeld = (now & BTN_SHIFT) != 0;
      if (shiftHeld) shiftUsedAsModifier = true;
      editUsedAsModifier = true;
      midiNoteEditorEditPress(shiftHeld);
    }

    if ((pressed & BTN_PLAY) && (now & BTN_SHIFT)) {
      shiftUsedAsModifier = true;
      playUsedAsModifier = true;
      midiNoteEditorCommitIfPlacing();
      appSeekTape(0);
      appStartTransport(true);
    } else if ((released & BTN_PLAY) && !playUsedAsModifier) {
      midiNoteEditorCommitIfPlacing();
      appToggleTransport();
    }

    prevKeys = now;
    return;
  }

  // Track modifier state
  if (pressed & BTN_OPT) {
    optUsedAsModifier = false;
    uiForceRedraw();
    if (!(now & (BTN_SHIFT | BTN_EDIT | BTN_PLAY | DIR_MASK))) {
      if (currentPage() == Page::Main && (isLoadConfirmActive() || isDeleteConfirmActive())) {
        cancelDeleteConfirm();
        uiShowToast("", 0);
        optUsedAsModifier = true;
        lastOptTapMs = 0;
        prevKeys = now;
        return;
      }
      if (currentPage() == Page::Instruments && isAnyInstDelConfirmActive()) {
        cancelInstDelConfirm();
        uiShowToast("", 0);
        optUsedAsModifier = true;
        lastOptTapMs = 0;
        prevKeys = now;
        return;
      }
      if (currentPage() == Page::InstrumentEdit && cancelInstMidiMapLearnOrConfirm()) {
        optUsedAsModifier = true;
        lastOptTapMs = 0;
        prevKeys = now;
        return;
      }
    }
  }
  if (released & BTN_OPT) {
    uiForceRedraw();
  }
  if (pressed & BTN_SHIFT) {
    shiftUsedAsModifier = false;
  }
  if (pressed & BTN_EDIT) {
    editUsedAsModifier = false;
    editSawShift = (now & BTN_SHIFT) != 0 || ((released & BTN_SHIFT) && !shiftUsedAsModifier);
    uiForceRedraw();
  }
  if ((now & BTN_EDIT) && (now & BTN_SHIFT)) {
    editSawShift = true;
  }
  if (released & BTN_EDIT) {
    uiForceRedraw();
  }
  if (pressed & BTN_PLAY) {
    playUsedAsModifier = false;
  }

  const PageOps &ops = pageOpsFor(currentPage());

  // SHIFT + OPT: Enter Track Edit Mode on a track timeline window, or open the Track Edit Overlay
  if ((((pressed & BTN_OPT) && (now & BTN_SHIFT)) || ((pressed & BTN_SHIFT) && (now & BTN_OPT))) &&
      !(now & (BTN_EDIT | BTN_PLAY | DIR_MASK))) {
    optUsedAsModifier = true;
    shiftUsedAsModifier = true;
    lastOptTapMs = 0;
    TrackEditPhase tep = trackEditPhase();
    if (tep == TrackEditPhase::SelectRegion) {
      trackEditOpenActionMenu();
      prevKeys = now;
      return;
    } else if (tep == TrackEditPhase::PickPastePos) {
      trackEditOpenPasteMenu();
      prevKeys = now;
      return;
    } else if (tep == TrackEditPhase::PickPasteTrack) {
      trackEditCancel();
      prevKeys = now;
      return;
    } else if (tep == TrackEditPhase::Off) {
      Page pg = currentPage();
      if (pg == Page::Audio && audioRow >= 0 && audioRow < TRACK_ROWS && audioCell == AUD_CELL_WAVE) {
        trackEditEnter(Page::Audio, audioRow);
        prevKeys = now;
        return;
      } else if (pg == Page::Midi && midiRow >= 0 && midiRow < TRACK_ROWS && midiCell == MID_CELL_WAVE) {
        trackEditEnter(Page::Midi, midiRow);
        prevKeys = now;
        return;
      }
    }
  }

  // Detect OPT tap on << / >> (step by 1/32nd bar), OPT tap on Punch Info cells (toggle Time / Bars.Beats),
  // OPT tap in Track Edit Mode (toggle Start / End highlight endpoint), or double-tap OPT timeline zoom
  if (released & BTN_OPT) {
    if (!optUsedAsModifier && !(now & (BTN_SHIFT | BTN_EDIT | BTN_PLAY | DIR_MASK))) {
      TrackEditPhase tep = trackEditPhase();
      if (tep == TrackEditPhase::SelectRegion) {
        trackEditToggleEndpoint();
        lastOptTapMs = 0;
      } else if (tep == TrackEditPhase::PickPasteTrack || tep == TrackEditPhase::PickPastePos) {
        trackEditCancel();
        lastOptTapMs = 0;
      } else {
        int optScrubDir = activeTransportScrubDir();
        if (optScrubDir != 0) {
          appStepTapeBar32nd(optScrubDir);
          lastOptTapMs = 0;
        } else if (isActivePunchInfoCell()) {
          togglePunchBarBeatMode();
          lastOptTapMs = 0;
        } else if (ms - lastOptTapMs <= 260) {
          Page pg = currentPage();
          if (pg == Page::Audio || pg == Page::Midi) {
            cycleTimelineZoom();
          } else if (pg == Page::Master &&
                     masterRow >= MASTER_ROW_TRK_PARAM1 &&
                     masterRow <= MASTER_ROW_TRK_MS) {
            toggleMasterTrackMode();
          }
          lastOptTapMs = 0;
        } else {
          lastOptTapMs = ms;
        }
      }
    } else {
      lastOptTapMs = 0;
    }
  }

  // Handle directional presses & key repeat
  uint8_t dirPressed = pressed & DIR_MASK;
  uint8_t activeDir = pollDirectionalRepeat(pressed, now, ms, 55);

  if (activeDir) {
    if ((now & BTN_PLAY) && !((now & (BTN_SHIFT | BTN_OPT | BTN_EDIT)) || midiEditHeld)) {
      // PLAY + Left/Right: tape rewind / fast-forward while held
      playUsedAsModifier = true;
      if (activeDir & BTN_LEFT) appStepTape(-(int32_t)(SAMPLE_RATE_HZ * 2));
      if (activeDir & BTN_RIGHT) appStepTape((int32_t)(SAMPLE_RATE_HZ * 2));
      trackEditOnPlayheadAdjusted();
    } else if ((now & BTN_SHIFT) && !((now & (BTN_OPT | BTN_EDIT)) || midiEditHeld) && dirPressed) {
      // SHIFT + Direction: Spatial Page Navigation (only on fresh press)
      shiftUsedAsModifier = true;
      if (ops.onShiftNavigate) {
        ops.onShiftNavigate(dirPressed);
      }
    } else if (now & BTN_OPT) {
      optUsedAsModifier = true;
      lastOptTapMs = 0;
      if (isCursorOnTrackWaveCell() && !(now & (BTN_SHIFT | BTN_EDIT | BTN_PLAY)) && !midiEditHeld) {
        if (activeDir & (BTN_UP | BTN_DOWN)) {
          waveOptScrubDir = 0;
          optHeaderHighlightCell = HDR_CELL_BEAT;
          int beatDir = ((activeDir & BTN_UP) ? 1 : 0) - ((activeDir & BTN_DOWN) ? 1 : 0);
          if (beatDir != 0) {
            appStepTapeBeat(beatDir, transport.tapeFrame);
          }
          uiForceRedraw();
        }
      }
    } else if ((now & BTN_EDIT) || midiEditHeld) {
      // EDIT + Direction: Adjust parameter value
      editUsedAsModifier = true;
      midiEditUsedAsModifier = true;
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      int dy = ((activeDir & BTN_UP) ? 1 : 0) - ((activeDir & BTN_DOWN) ? 1 : 0);
      int delta = (dx != 0) ? dx : dy;
      if (delta != 0 && ops.onOptAdjust) {
        ops.onOptAdjust(dx, dy, delta);
      }
    } else {
      // Plain directional navigation
      int dx = ((activeDir & BTN_RIGHT) ? 1 : 0) - ((activeDir & BTN_LEFT) ? 1 : 0);
      int dy = ((activeDir & BTN_DOWN) ? 1 : 0) - ((activeDir & BTN_UP) ? 1 : 0);
      if (ops.onCursorMove) {
        ops.onCursorMove(dx, dy, activeDir);
      }
    }
  }

  // Handle EDIT (A) press / hold / release
  const PageOps &curOps = pageOpsFor(currentPage());

  // Release any live Sampler key gate if EDIT is released, modifiers are pressed, or row/page changed
  if (samplerHeldNote >= 0) {
    bool onSmpKeys = (currentPage() == Page::InstrumentEdit &&
                      !isInstMidiMapOpen() &&
                      fmSynthPatch(instListCursor).type == (uint8_t)InstType::Sampler &&
                      fmEditRow == SMP_ROW_KEYS);
    bool forceImmediate = (now & (BTN_SHIFT | BTN_OPT | DIR_MASK)) != 0 || !onSmpKeys;
    bool minHoldElapsed = (int32_t)(ms - samplerHeldMinUntilMs) >= 0;
    if (forceImmediate || (!((now & BTN_EDIT) || midiEditHeld) && minHoldElapsed)) {
      releaseSamplerEditHold();
    }
  }

  // On Sampler 1.5-octave keyboard row, pressing EDIT immediately gates the selected key
  if ((pressed & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT)) &&
      currentPage() == Page::InstrumentEdit &&
      !isInstMidiMapOpen() &&
      fmSynthPatch(instListCursor).type == (uint8_t)InstType::Sampler &&
      fmEditRow == SMP_ROW_KEYS) {
    triggerSamplerKeyAuditionHold(ms);
    editUsedAsModifier = true;
  }

  int scrubDir = activeTransportScrubDir();
  if ((pressed & BTN_EDIT) && scrubDir != 0 && !(now & (BTN_SHIFT | BTN_OPT))) {
    // Initial EDIT press on << or >>: step 0.001s (1 ms) immediately and arm ramping hold
    appStepTapeMs(scrubDir);
    scrubHoldDir = scrubDir;
    scrubHoldStartMs = ms;
    scrubLastTickMs = ms;
    scrubFracFrames = 0.0f;
    editUsedAsModifier = true;
  } else if (((now & BTN_EDIT) || midiEditHeld) && scrubHoldDir != 0) {
    if (scrubDir != scrubHoldDir || (now & (BTN_SHIFT | BTN_OPT | DIR_MASK))) {
      scrubHoldDir = 0;
    } else if ((int32_t)(ms - scrubHoldStartMs) >= 200) {
      uint32_t dtMs = ms - scrubLastTickMs;
      if (dtMs > 0) {
        scrubLastTickMs = ms;
        float holdSec = (float)(ms - scrubHoldStartMs - 200) / 1000.0f;
        float speed = constrain(0.5f + holdSec * 1.75f, 0.5f, 4.0f);
        scrubFracFrames += ((float)dtMs / 1000.0f) * speed * (float)SAMPLE_RATE_HZ;
        int32_t stepFrames = (int32_t)scrubFracFrames;
        if (stepFrames > 0) {
          scrubFracFrames -= (float)stepFrames;
          appStepTape(scrubHoldDir * stepFrames);
        }
      }
    } else {
      scrubLastTickMs = ms;
    }
  } else {
    scrubHoldDir = 0;
  }

  // OPT + LEFT/RIGHT on track waveform cell 2: replicates << / >> behavior and highlights top bar ms cell
  if (!(now & BTN_OPT) || !isCursorOnTrackWaveCell() || (now & (BTN_SHIFT | BTN_EDIT | BTN_PLAY)) || midiEditHeld) {
    if (optHeaderHighlightCell >= 0 || waveOptScrubDir != 0) {
      optHeaderHighlightCell = -1;
      waveOptScrubDir = 0;
      uiForceRedraw();
    }
  } else {
    if (pressed & (BTN_LEFT | BTN_RIGHT)) {
      int dir = (pressed & BTN_RIGHT) ? 1 : -1;
      optUsedAsModifier = true;
      lastOptTapMs = 0;
      optHeaderHighlightCell = HDR_CELL_MS;
      appStepTapeMs(dir);
      waveOptScrubDir = dir;
      scrubHoldStartMs = ms;
      scrubLastTickMs = ms;
      scrubFracFrames = 0.0f;
      uiForceRedraw();
    } else if (waveOptScrubDir != 0) {
      uint8_t wantBit = (waveOptScrubDir > 0) ? BTN_RIGHT : BTN_LEFT;
      if (!(now & wantBit) || (now & (BTN_UP | BTN_DOWN))) {
        waveOptScrubDir = 0;
      } else if ((int32_t)(ms - scrubHoldStartMs) >= 200) {
        uint32_t dtMs = ms - scrubLastTickMs;
        if (dtMs > 0) {
          scrubLastTickMs = ms;
          float holdSec = (float)(ms - scrubHoldStartMs - 200) / 1000.0f;
          float speed = constrain(0.5f + holdSec * 1.75f, 0.5f, 4.0f);
          scrubFracFrames += ((float)dtMs / 1000.0f) * speed * (float)SAMPLE_RATE_HZ;
          int32_t stepFrames = (int32_t)scrubFracFrames;
          if (stepFrames > 0) {
            scrubFracFrames -= (float)stepFrames;
            appStepTape(waveOptScrubDir * stepFrames);
            uiForceRedraw();
          }
        }
      } else {
        scrubLastTickMs = ms;
      }
    }
  }

  if ((((pressed & BTN_EDIT) && (now & BTN_OPT)) ||
       ((pressed & BTN_OPT) && ((now & BTN_EDIT) || midiEditHeld))) &&
      !(now & BTN_SHIFT)) {
    optUsedAsModifier = true;
    editUsedAsModifier = true;
    midiEditUsedAsModifier = true;
    lastOptTapMs = 0;
    if (curOps.onResetParam) {
      curOps.onResetParam();
    }
  } else if ((((pressed & BTN_EDIT) && ((now & BTN_SHIFT) || editSawShift)) ||
              ((pressed & BTN_SHIFT) && (now & BTN_EDIT))) &&
             !(now & BTN_OPT)) {
    releaseSamplerEditHold();
    shiftUsedAsModifier = true;
    editUsedAsModifier = true;
    editSawShift = true;
    if (curOps.onEditPress) {
      curOps.onEditPress(true);
    }
  } else if ((released & BTN_EDIT) && !editUsedAsModifier) {
    bool shiftHeld = (now & BTN_SHIFT) != 0 || editSawShift;
    editSawShift = false;
    if (shiftHeld) shiftUsedAsModifier = true;
    if (curOps.onEditPress) {
      curOps.onEditPress(shiftHeld);
    }
  } else if (released & BTN_EDIT) {
    editSawShift = false;
  }

  // Handle PLAY button
  if (pressed & BTN_PLAY) {
    if (now & BTN_SHIFT) {
      // SHIFT + PLAY: Return to zero and start transport immediately
      shiftUsedAsModifier = true;
      playUsedAsModifier = true;
      appSeekTape(0);
      appStartTransport(true);
    }
  }
  if ((released & BTN_PLAY) && !playUsedAsModifier) {
    appToggleTransport();
  }

  serviceMidiPendingWaveMute(now, editUsedAsModifier);
  prevKeys = now;
}

// ============================================================================
// MIDI-Mapped Navigation Helpers (Tap, Double-Tap SHIFT+Dir, Hold Repeat)
// ============================================================================
struct NavCursorSnapshot {
  bool valid;
  bool inMidiNoteEd;
  Page page;
  int headerCell;
  int audioRow, audioCell, audioScroll;
  int midiRow, midiCell, midiScroll;
  int masterRow, masterCell;
  int songSetRow, sysSetRow;
  int mainRow, mainCol, songListCursor, songListScroll;
  int instCol, instListCursor, instTypeCursor;
  int fmEditRow, fmEditCol, instEditScroll;
};

static NavCursorSnapshot navSnapshot = {};

FLASHMEM uint32_t inputCurrentRepeatIntervalMs() {
  if (isSamplerKeyModalOpen() || isInstSaveToModalOpen() ||
      (isSysPresetsModalOpen() && isSysPresetDeleteConfirmOpen()) ||
      isUsbSavePromptModalOpen() || isClearTrackModalOpen()) {
    return 85u;
  }
  if (isSaveKeyboardOpen() || isSamplerBrowserOpen() || isInstBrowserOpen() ||
      isSysPresetsModalOpen() || isSysProjectsModalOpen() || isSysMidiMapModalOpen() ||
      isTypePresetModalOpen() || isMidiClipBrowserOpen() || trackEditIsModalOpen()) {
    return 65u;
  }
  return 55u;
}

FLASHMEM void inputSaveNavSnapshot() {
  if ((board.keys() & (BTN_SHIFT | BTN_OPT | BTN_EDIT | BTN_PLAY)) != 0) {
    navSnapshot.valid = false;
    return;
  }
  navSnapshot.valid = true;
  navSnapshot.inMidiNoteEd = isMidiNoteEditorOpen();
  navSnapshot.page = currentPage();
  navSnapshot.headerCell = headerCell;
  navSnapshot.audioRow = audioRow;
  navSnapshot.audioCell = audioCell;
  navSnapshot.audioScroll = audioScroll;
  navSnapshot.midiRow = midiRow;
  navSnapshot.midiCell = midiCell;
  navSnapshot.midiScroll = midiScroll;
  navSnapshot.masterRow = masterRow;
  navSnapshot.masterCell = masterCell;
  navSnapshot.songSetRow = songSetRow;
  navSnapshot.sysSetRow = sysSetRow;
  navSnapshot.mainRow = mainRow;
  navSnapshot.mainCol = mainCol;
  navSnapshot.songListCursor = songListCursor;
  navSnapshot.songListScroll = songListScroll;
  navSnapshot.instCol = instCol;
  navSnapshot.instListCursor = instListCursor;
  navSnapshot.instTypeCursor = instTypeCursor;
  navSnapshot.fmEditRow = fmEditRow;
  navSnapshot.fmEditCol = fmEditCol;
  navSnapshot.instEditScroll = instEditScroll;
}

FLASHMEM void inputRestoreNavSnapshot(uint8_t dirBit) {
  if (!navSnapshot.valid) return;
  navSnapshot.valid = false;

  if (navSnapshot.inMidiNoteEd && isMidiNoteEditorOpen()) {
    uint8_t oppDir = 0;
    if (dirBit & BTN_UP) oppDir |= BTN_DOWN;
    if (dirBit & BTN_DOWN) oppDir |= BTN_UP;
    if (dirBit & BTN_LEFT) oppDir |= BTN_RIGHT;
    if (dirBit & BTN_RIGHT) oppDir |= BTN_LEFT;
    if (oppDir != 0) {
      midiNoteEditorDir(oppDir, false);
    }
    return;
  }

  if (currentPage() == navSnapshot.page) {
    headerCell = navSnapshot.headerCell;
    audioRow = navSnapshot.audioRow;
    audioCell = navSnapshot.audioCell;
    audioScroll = navSnapshot.audioScroll;
    midiRow = navSnapshot.midiRow;
    midiCell = navSnapshot.midiCell;
    midiScroll = navSnapshot.midiScroll;
    masterRow = navSnapshot.masterRow;
    masterCell = navSnapshot.masterCell;
    songSetRow = navSnapshot.songSetRow;
    sysSetRow = navSnapshot.sysSetRow;
    mainRow = navSnapshot.mainRow;
    mainCol = navSnapshot.mainCol;
    songListCursor = navSnapshot.songListCursor;
    songListScroll = navSnapshot.songListScroll;
    instCol = navSnapshot.instCol;
    instListCursor = navSnapshot.instListCursor;
    instTypeCursor = navSnapshot.instTypeCursor;
    fmEditRow = navSnapshot.fmEditRow;
    fmEditCol = navSnapshot.fmEditCol;
    instEditScroll = navSnapshot.instEditScroll;
    audioRememberTrackCursor();
    midiRememberTrackCursor();
  }
}

FLASHMEM void inputTriggerNav(uint8_t dirBit, bool withShift) {
  constexpr uint8_t DIR_MASK = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
  dirBit &= DIR_MASK;
  if (dirBit == 0) return;

  uint32_t ms = millis();
  if (isScreenBlanked()) {
    noteUserActivity();
    return;
  }
  noteUserActivity();
  if (isSysProjectPlayLocked()) {
    return;
  }

  uint8_t hwNow = board.keys();
  uint8_t now = (hwNow & ~DIR_MASK) | dirBit | (withShift ? BTN_SHIFT : 0) | (midiEditHeld ? BTN_EDIT : 0);
  uint8_t pressed = dirBit;
  if (midiEditHeld) {
    midiEditUsedAsModifier = true;
  }

  if (isSaveKeyboardOpen()) {
    if (!withShift) {
      handleKeyboardDir(dirBit);
    }
    return;
  }

  if (isSamplerKeyModalOpen()) {
    if (!storageIsSamplerKeyRecordingOpen() && !withShift) {
      int step = ((dirBit & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                 ((dirBit & (BTN_UP | BTN_LEFT)) ? 1 : 0);
      if (step != 0) samplerKeyModalMove(step);
    }
    return;
  }

  if (isSamplerBrowserOpen()) {
    if (!withShift) {
      int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
      int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
      samplerBrowserMove(dy, dx);
    }
    return;
  }

  if (isInstBrowserOpen()) {
    if (!withShift) {
      int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
      int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
      instBrowserMove(dy, dx);
      uiForceRedraw();
    }
    return;
  }

  if (isInstSaveToModalOpen()) {
    if (!withShift) {
      int step = ((dirBit & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                 ((dirBit & (BTN_UP | BTN_LEFT)) ? 1 : 0);
      if (step != 0) instSaveToModalMove(step);
    }
    return;
  }

  if (isSysPresetsModalOpen()) {
    if (isSysPresetDeleteConfirmOpen()) {
      if (!withShift) {
        int step = ((dirBit & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                   ((dirBit & (BTN_UP | BTN_LEFT)) ? 1 : 0);
        if (step != 0) {
          sysPresetDeleteConfirmMove(step);
          uiForceRedraw();
        }
      }
      return;
    }
    if ((dirBit & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT))) {
      closeSysPresetsModal();
      swallowModalKeys = true;
      shiftUsedAsModifier = true;
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
      int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
      sysPresetsMove(dy, dx);
      uiForceRedraw();
    }
    return;
  }

  if (isSysProjectsModalOpen()) {
    handleSysProjectsModalInput(pressed, now, ms);
    if ((pressed & BTN_LEFT) && (now & BTN_SHIFT)) shiftUsedAsModifier = true;
    return;
  }

  if (isSysMidiMapModalOpen()) {
    handleSysMidiMapModalInput(pressed, now, ms);
    if ((pressed & (BTN_LEFT | BTN_DOWN)) && (now & BTN_SHIFT)) shiftUsedAsModifier = true;
    return;
  }

  if (isUsbSavePromptModalOpen()) {
    if (!withShift) {
      int step = ((dirBit & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
                 ((dirBit & (BTN_UP | BTN_LEFT)) ? 1 : 0);
      if (step != 0) usbSavePromptModalMove(step);
    }
    return;
  }

  if (isTypePresetModalOpen()) {
    if ((dirBit & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT))) {
      closeTypePresetModal();
      swallowModalKeys = true;
      shiftUsedAsModifier = true;
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
      int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
      typePresetModalMove(dy, dx);
      uiForceRedraw();
    }
    return;
  }

  if (isClearTrackModalOpen() || isClearTrackModalRevealing()) {
    if (!withShift) {
      int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
      int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
      if (dx != 0 || dy != 0) clearTrackModalMove(dx, dy);
    }
    return;
  }

  if (isMidiClipBrowserOpen()) {
    if (!withShift) {
      int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
      int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
      midiClipBrowserMove(dy, dx);
      uiForceRedraw();
    }
    return;
  }

  if (trackEditIsModalOpen() || trackEditPhase() == TrackEditPhase::DuplReveal) {
    if (trackEditIsModalOpen() && !withShift) {
      trackEditModalDir(dirBit);
    }
    return;
  }

  if (isMidiNoteEditorOpen()) {
    bool shiftHeld = (now & BTN_SHIFT) != 0;
    if (shiftHeld) shiftUsedAsModifier = true;
    midiNoteEditorDir(dirBit, shiftHeld);
    return;
  }

  const PageOps &ops = pageOpsFor(currentPage());
  if ((now & BTN_PLAY) && !(now & (BTN_SHIFT | BTN_OPT | BTN_EDIT))) {
    playUsedAsModifier = true;
    if (dirBit & BTN_LEFT) appStepTape(-(int32_t)(SAMPLE_RATE_HZ * 2));
    if (dirBit & BTN_RIGHT) appStepTape((int32_t)(SAMPLE_RATE_HZ * 2));
    trackEditOnPlayheadAdjusted();
  } else if ((now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT))) {
    shiftUsedAsModifier = true;
    if (ops.onShiftNavigate) {
      ops.onShiftNavigate(dirBit);
    }
  } else if (now & BTN_OPT) {
    optUsedAsModifier = true;
    lastOptTapMs = 0;
    if (isCursorOnTrackWaveCell() && !(now & (BTN_SHIFT | BTN_EDIT | BTN_PLAY))) {
      if (dirBit & (BTN_LEFT | BTN_RIGHT)) {
        optHeaderHighlightCell = HDR_CELL_MS;
        int dir = (dirBit & BTN_RIGHT) ? 1 : -1;
        appStepTapeMs(dir);
        uiForceRedraw();
      } else if (dirBit & (BTN_UP | BTN_DOWN)) {
        optHeaderHighlightCell = HDR_CELL_BEAT;
        int beatDir = ((dirBit & BTN_UP) ? 1 : 0) - ((dirBit & BTN_DOWN) ? 1 : 0);
        if (beatDir != 0) {
          appStepTapeBeat(beatDir, transport.tapeFrame);
        }
        uiForceRedraw();
      }
    }
  } else if (now & BTN_EDIT) {
    editUsedAsModifier = true;
    int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
    int dy = ((dirBit & BTN_UP) ? 1 : 0) - ((dirBit & BTN_DOWN) ? 1 : 0);
    int delta = (dx != 0) ? dx : dy;
    if (delta != 0 && ops.onOptAdjust) {
      ops.onOptAdjust(dx, dy, delta);
    }
  } else {
    int dx = ((dirBit & BTN_RIGHT) ? 1 : 0) - ((dirBit & BTN_LEFT) ? 1 : 0);
    int dy = ((dirBit & BTN_DOWN) ? 1 : 0) - ((dirBit & BTN_UP) ? 1 : 0);
    if (ops.onCursorMove) {
      ops.onCursorMove(dx, dy, dirBit);
    }
  }
}

FLASHMEM void inputSetMidiEditHeld(bool held, bool usedAsMod) {
  midiEditHeld = held;
  midiEditUsedAsModifier = usedAsMod;
  uiForceRedraw();
}

FLASHMEM bool inputIsMidiEditUsedAsModifier() {
  return midiEditUsedAsModifier;
}

FLASHMEM bool inputFinishMidiEditHold() {
  bool wasUsed = midiEditUsedAsModifier;
  midiEditHeld = false;
  midiEditUsedAsModifier = false;
  if (!(board.keys() & BTN_EDIT)) {
    releaseSamplerEditHold();
    scrubHoldDir = 0;
  }
  uiForceRedraw();
  return wasUsed;
}

FLASHMEM bool inputStartMidiEditHoldAction() {
  if (isScreenBlanked() || isSysProjectPlayLocked()) return false;
  uint32_t ms = millis();
  uint8_t hwNow = board.keys();
  if (hwNow & (BTN_SHIFT | BTN_OPT)) return false;

  if (!isSaveKeyboardOpen() && !isSamplerKeyModalOpen() && !isSamplerBrowserOpen() &&
      !isInstBrowserOpen() && !isInstSaveToModalOpen() && !isSysPresetsModalOpen() &&
      !isSysProjectsModalOpen() && !isSysMidiMapModalOpen() && !isUsbSavePromptModalOpen() &&
      !isTypePresetModalOpen() && !isClearTrackModalOpen() &&
      !isMidiClipBrowserOpen() && !trackEditIsModalOpen() &&
      trackEditPhase() != TrackEditPhase::DuplReveal && !isMidiNoteEditorOpen()) {
    if (currentPage() == Page::InstrumentEdit &&
        !isInstMidiMapOpen() &&
        fmSynthPatch(instListCursor).type == (uint8_t)InstType::Sampler &&
        fmEditRow == SMP_ROW_KEYS) {
      triggerSamplerKeyAuditionHold(ms);
      midiEditUsedAsModifier = true;
      return true;
    }

    int scrubDir = activeTransportScrubDir();
    if (scrubDir != 0) {
      appStepTapeMs(scrubDir);
      scrubHoldDir = scrubDir;
      scrubHoldStartMs = ms - 200;
      scrubLastTickMs = ms;
      scrubFracFrames = 0.0f;
      midiEditUsedAsModifier = true;
      uiForceRedraw();
      return true;
    }
  }
  return false;
}

FLASHMEM void inputTriggerMidiOptEdit() {
  uint32_t ms = millis();
  if (isScreenBlanked()) {
    noteUserActivity();
    return;
  }
  noteUserActivity();
  if (isSysProjectPlayLocked()) {
    return;
  }

  if (isSaveKeyboardOpen() || isSamplerKeyModalOpen() || isSamplerBrowserOpen() ||
      isInstBrowserOpen() || isInstSaveToModalOpen() || isSysPresetsModalOpen() ||
      isUsbSavePromptModalOpen() || isTypePresetModalOpen() ||
      isClearTrackModalOpen() || isMidiClipBrowserOpen() || trackEditIsModalOpen() ||
      trackEditPhase() == TrackEditPhase::DuplReveal) {
    return;
  }

  if (isSysProjectsModalOpen()) {
    handleSysProjectsModalInput(BTN_EDIT | BTN_OPT, BTN_EDIT | BTN_OPT, ms);
    return;
  }

  if (isSysMidiMapModalOpen()) {
    handleSysMidiMapModalInput(BTN_EDIT | BTN_OPT, BTN_EDIT | BTN_OPT, ms);
    return;
  }

  if (isMidiNoteEditorOpen()) {
    midiNoteEditorResetPress();
    return;
  }

  const PageOps &curOps = pageOpsFor(currentPage());
  if (curOps.onResetParam) {
    curOps.onResetParam();
  }
}

FLASHMEM void inputTriggerMidiEditTap() {
  uint32_t ms = millis();
  if (isScreenBlanked()) {
    noteUserActivity();
    return;
  }
  noteUserActivity();
  if (isSysProjectPlayLocked()) {
    return;
  }

  uint8_t hwNow = board.keys();
  if ((hwNow & BTN_OPT) && !(hwNow & BTN_SHIFT)) {
    optUsedAsModifier = true;
    lastOptTapMs = 0;
    inputTriggerMidiOptEdit();
    return;
  }

  bool shiftHeld = (hwNow & BTN_SHIFT) != 0;
  if (shiftHeld) shiftUsedAsModifier = true;

  if (isSaveKeyboardOpen()) {
    handleKeyboardEditPress();
    return;
  }

  if (isSamplerKeyModalOpen()) {
    confirmSamplerKeyModalOrRecord();
    return;
  }

  if (isSamplerBrowserOpen()) {
    confirmSamplerBrowserSelection();
    return;
  }

  if (isInstBrowserOpen()) {
    confirmInstBrowserSelection();
    return;
  }

  if (isInstSaveToModalOpen()) {
    instSaveToModalConfirm();
    return;
  }

  if (isSysPresetsModalOpen()) {
    if (isSysPresetDeleteConfirmOpen()) {
      bool wasSongMode = isSysPresetsSongMode();
      if (sysPresetDeleteConfirmCommit()) {
        uiShowToast(wasSongMode ? "INSTRUMENT DELETED" : "PRESET DELETED");
      }
      swallowModalKeys = true;
      uiForceRedraw();
    } else if (!shiftHeld) {
      sysPresetsRequestDelete();
      swallowModalKeys = true;
      uiForceRedraw();
    }
    return;
  }

  if (isSysProjectsModalOpen()) {
    handleSysProjectsModalInput(BTN_EDIT, (hwNow & ~BTN_EDIT) | BTN_EDIT, ms);
    if (isSysProjectsModalOpen()) {
      handleSysProjectsModalInput(0, hwNow & ~BTN_EDIT, ms);
    }
    return;
  }

  if (isSysMidiMapModalOpen()) {
    handleSysMidiMapModalInput(BTN_EDIT, (hwNow & ~BTN_EDIT) | BTN_EDIT, ms);
    if (isSysMidiMapModalOpen()) {
      handleSysMidiMapModalInput(0, hwNow & ~BTN_EDIT, ms);
    }
    return;
  }

  if (isUsbSavePromptModalOpen()) {
    usbSavePromptModalConfirm();
    return;
  }

  if (isTypePresetModalOpen()) {
    if (!shiftHeld) {
      confirmTypePresetSelection();
    }
    return;
  }

  if (isClearTrackModalOpen() || isClearTrackModalRevealing()) {
    clearTrackModalConfirm();
    return;
  }

  if (isMidiClipBrowserOpen()) {
    confirmMidiClipBrowserSelection();
    return;
  }

  if (trackEditIsModalOpen() || trackEditPhase() == TrackEditPhase::DuplReveal) {
    trackEditModalEditPress();
    return;
  }

  if (isMidiNoteEditorOpen()) {
    midiNoteEditorEditPress(shiftHeld);
    return;
  }

  const PageOps &curOps = pageOpsFor(currentPage());
  if (!shiftHeld && currentPage() == Page::InstrumentEdit &&
      !isInstMidiMapOpen() &&
      fmSynthPatch(instListCursor).type == (uint8_t)InstType::Sampler &&
      fmEditRow == SMP_ROW_KEYS) {
    triggerSamplerKeyAuditionHold(ms);
    return;
  }

  if (!shiftHeld && activeTransportScrubDir() != 0) {
    appStepTapeMs(activeTransportScrubDir());
    return;
  }

  if (curOps.onEditPress) {
    curOps.onEditPress(shiftHeld);
  }
}

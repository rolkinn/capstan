// page_system_settings.cpp — System Settings Page UI & Input
#include "page_system_settings.h"
#include "midi_map.h"
#include "usb_drive.h"
#include <Arduino.h>
#include <stdio.h>

using namespace m8;
using namespace looper;

static uint64_t prevSysSettingsKey[SSET_COUNT] = {};
static uint64_t prevSysPrstBannerKey = 0;
static uint64_t prevSysPrstRowKey[INST_BROWSER_VISIBLE_ROWS] = {};
static uint64_t prevSysPrstDelConfirmKey = 0;
static uint64_t prevUsbSavePromptKey = 0;
static bool usbSavePromptOpen = false;
static int usbSavePromptCursor = 0; // 0 = YES, 1 = NO

// Projects Modal State (/Capstan/PROJECTS/*.proj)
enum class ProjModalView : uint8_t {
  Closed      = 0,
  ManageList  = 1,
  EditProject = 2,
  AddSong     = 3,
  PlayProject = 4
};

enum ProjManageAction : int {
  PROJ_ACT_PLAY   = 0,
  PROJ_ACT_EDIT   = 1,
  PROJ_ACT_RENAME = 2,
  PROJ_ACT_DELETE = 3,
  PROJ_ACT_COUNT  = 4
};

enum ProjEditRowAction : int {
  PROJ_EDIT_ACT_ADD    = 0,
  PROJ_EDIT_ACT_REMOVE = 1,
  PROJ_EDIT_ACT_COUNT  = 2
};

enum ProjPlayZone : uint8_t {
  PROJ_PLAY_ZONE_CONTROLS = 0,
  PROJ_PLAY_ZONE_LIST     = 1,
  PROJ_PLAY_ZONE_EXIT     = 2
};

enum ProjPlayCtrlCol : uint8_t {
  PROJ_PLAY_CTRL_PREV  = 0,
  PROJ_PLAY_CTRL_PLAY  = 1,
  PROJ_PLAY_CTRL_NEXT  = 2,
  PROJ_PLAY_CTRL_VOL   = 3,
  PROJ_PLAY_CTRL_COUNT = 4
};

static constexpr int PROJ_MANAGE_VISIBLE_ROWS = 7;
static constexpr int PROJ_EDIT_VISIBLE_ROWS   = 6;
static constexpr int PROJ_ADD_VISIBLE_ROWS    = 7;
static constexpr int PROJ_PLAY_VISIBLE_ROWS   = 5;

static ProjModalView projModalView = ProjModalView::Closed;
static int projManageCursor = 0;
static int projManageScroll = 0;
static int projManageCol = PROJ_ACT_PLAY;

static bool projConfirmOpen = false;
static int projConfirmCursor = 0; // 0 = YES, 1 = NO

static char projEditingName[MAX_PROJECT_NAME_LEN + 1] = "";
static char (*const projEditSongs)[MAX_SONG_NAME_LEN + 1] =
    reinterpret_cast<char (*)[MAX_SONG_NAME_LEN + 1]>(sharedBrowserPoolBuf());
static_assert(MAX_SONGS_PER_PROJECT * (MAX_SONG_NAME_LEN + 1) <= 2048, "projEditSongs must fit in sharedBrowserPool");
static int projEditSongCount = 0;
static int projEditCursor = 0;
static int projEditScroll = 0;
static int projEditCol = PROJ_EDIT_ACT_ADD;
static bool projEditInBottomRow = false;
static int projEditBottomCol = 0; // 0 = SAVE, 1 = CANCEL

static int projAddSongCursor = 0;
static int projAddSongScroll = 0;

// Project Play state
static int projPlayPlayingIdx = -1;
static int projPlayListCursor = 0;
static int projPlayListScroll = 0;
static uint8_t projPlayZone = PROJ_PLAY_ZONE_CONTROLS;
static uint8_t projPlayCtrlCol = PROJ_PLAY_CTRL_PLAY;
static uint8_t projPlayVol = MASTER_VOLUME_DEFAULT;
static int8_t projPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
static bool projPlayLocked = false;
static bool projPlayEditHeld = false;
static bool projPlayEditModUsed = false;
static bool projPlayActive = false;
static bool projPlayTailPhase = false;
static uint32_t projPlayMaxDataFrame = 0;
static uint32_t projPlaySilentFrames = 0;
static uint32_t projPlayLastNow = 0;
static uint32_t projPlayAnimMs = 0;
static uint32_t projPlayLastTickMs = 0;

static uint64_t prevProjBannerKey = 0;
static uint64_t prevProjTapeKey = 0;
static uint64_t prevProjTapeStaticKey = 0;
static uint64_t prevProjLockKey = 0;
static uint64_t prevProjCtrlKey = 0;
static uint64_t prevProjRowKey[7] = {};
static uint64_t prevProjBtnKey[2] = {};
static uint64_t prevProjConfirmKey = 0;

// MIDI Mappings Modal State
enum MidiMapZone : uint8_t {
  MIDI_MAP_ZONE_CAT  = 0,
  MIDI_MAP_ZONE_CLR  = 1,
  MIDI_MAP_ZONE_LIST = 2
};

static constexpr int MIDI_MAP_VISIBLE_ROWS = 6;
static bool midiMapModalOpen = false;
static uint8_t midiMapCat = MIDI_CAT_TRANSPORT;
static uint8_t midiMapZone = MIDI_MAP_ZONE_LIST;
static int midiMapClrCol = 0;            // 0 = CLR CAT, 1 = CLR ALL
static uint8_t midiMapClrConfirmMode = 0; // 0 = none, 1 = CLR CAT SURE?, 2 = CLR ALL SURE?
static uint32_t midiMapClrConfirmUntilMs = 0;
static int midiMapCursor = 0;
static int midiMapScroll = 0;
static int midiMapCol = 0;               // 0 = CH, 1 = CC, 2 = LRN, 3 = CLR
static bool midiMapEditHeld = false;
static bool midiMapEditModUsed = false;
static uint64_t prevMidiMapBannerKey = 0;
static uint64_t prevMidiMapClrRowKey = 0;
static uint64_t prevMidiMapRowKey[MIDI_MAP_VISIBLE_ROWS] = {};

FLASHMEM static void invalidateSysMidiMapModalCache() {
  prevMidiMapBannerKey = 0;
  prevMidiMapClrRowKey = 0;
  memset(prevMidiMapRowKey, 0, sizeof(prevMidiMapRowKey));
}

FLASHMEM static void invalidateSysProjectsModalCache() {
  prevProjBannerKey = 0;
  prevProjTapeKey = 0;
  prevProjTapeStaticKey = 0;
  prevProjLockKey = 0;
  prevProjCtrlKey = 0;
  memset(prevProjRowKey, 0, sizeof(prevProjRowKey));
  memset(prevProjBtnKey, 0, sizeof(prevProjBtnKey));
  prevProjConfirmKey = 0;
  invalidateSysMidiMapModalCache();
}

FLASHMEM static const char *sysSettingsHeaderTitle() {
  return "SYS | v" CAPSTAN_VERSION;
}

FLASHMEM void invalidateSysPresetsModalCache() {
  prevSysPrstBannerKey = 0;
  memset(prevSysPrstRowKey, 0, sizeof(prevSysPrstRowKey));
  prevSysPrstDelConfirmKey = 0;
  prevUsbSavePromptKey = 0;
  invalidateSysProjectsModalCache();
}

FLASHMEM static void sysSettingsInvalidateCache() {
  memset(prevSysSettingsKey, 0, sizeof(prevSysSettingsKey));
  invalidateSysPresetsModalCache();
}

FLASHMEM bool isUsbSavePromptModalOpen() {
  return usbSavePromptOpen;
}

FLASHMEM void openUsbSavePromptModal() {
  usbSavePromptCursor = 0;
  usbSavePromptOpen = true;
  prevUsbSavePromptKey = 0;
  uiForceRedraw();
}

FLASHMEM void closeUsbSavePromptModal() {
  if (!usbSavePromptOpen) return;
  usbSavePromptOpen = false;
  prevUsbSavePromptKey = 0;
  uiForceRedraw();
}

FLASHMEM void usbSavePromptModalMove(int delta) {
  if (!usbSavePromptOpen || delta == 0) return;
  usbSavePromptCursor = (usbSavePromptCursor + delta + 2) % 2;
  uiForceRedraw();
}

FLASHMEM void drawUsbSavePromptModal(bool full) {
  if (!usbSavePromptOpen) return;

  const int cur = constrain(usbSavePromptCursor, 0, 1);
  const int ovX = SX(12);
  const int ovY = SY(94);
  const int ovW = SX(296);
  const int ovH = SY(24);

  uint64_t k = hashMix(hashInit(), (uint64_t)ACCENT | ((uint64_t)cur << 16));
  if (full || k != prevUsbSavePromptKey) {
    prevUsbSavePromptKey = k;
    Surface s(ovW, ovH);
    s.drawBox(DARK, ACCENT);
    s.drawText(SX(10), TY(ovH), "Save song?", INK);

    PROGMEM static const char *const OPT_LABELS[2] = {"YES", "NO"};
    const int pillW = SX(42);
    const int pillH = SY(16);
    const int pillY = max(1, (ovH - pillH) / 2);
    const int pillX[2] = {SX(196), SX(244)};

    for (int i = 0; i < 2; i++) {
      bool sel = (cur == i);
      uint16_t pBg = sel ? (i == 0 ? COLOR_PLAY : COLOR_WARN) : PANEL;
      uint16_t pFg = sel ? BG : INK;
      uint16_t pBd = sel ? INK : MID;
      s.drawPill(pillX[i], pillY, pillW, pillH, OPT_LABELS[i], pFg, pBg, pBd);
    }
    s.blit(ovX, ovY);
  }
}

FLASHMEM void drawSysPresetDeleteConfirmModal(bool full) {
  if (!isSysPresetDeleteConfirmOpen()) return;

  const int cur = constrain(sysPresetDeleteConfirmCursor(), 0, 1);
  const int ovX = SX(12);
  const int ovY = SY(94);
  const int ovW = SX(296);
  const int ovH = SY(24);
  const bool songMode = isSysPresetsSongMode();

  uint64_t k = hashMix(hashInit(), (uint64_t)ACCENT | ((uint64_t)cur << 16) |
                                   ((uint64_t)instBrowserCursor() << 24) |
                                   ((uint64_t)songMode << 32));
  if (full || k != prevSysPrstDelConfirmKey) {
    prevSysPrstDelConfirmKey = k;
    Surface s(ovW, ovH);
    s.drawBox(DARK, ACCENT);
    s.drawText(SX(10), TY(ovH), songMode ? "Delete instrument?" : "Delete preset?", INK);

    static const char *const OPT_LABELS[2] = {"YES", "NO"};
    const int pillW = SX(42);
    const int pillH = SY(16);
    const int pillY = max(1, (ovH - pillH) / 2);
    const int pillX[2] = {SX(196), SX(244)};

    for (int i = 0; i < 2; i++) {
      bool sel = (cur == i);
      uint16_t pBg = sel ? (i == 0 ? COLOR_WARN : ACCENT) : PANEL;
      uint16_t pFg = sel ? BG : INK;
      uint16_t pBd = sel ? INK : MID;
      s.drawPill(pillX[i], pillY, pillW, pillH, OPT_LABELS[i], pFg, pBg, pBd);
    }
    s.blit(ovX, ovY);
  }
}

FLASHMEM void drawSysPresetsModal(bool full) {
  const bool songMode = isSysPresetsSongMode();
  // 1. Header Banner: [INSTRUMENT PRESETS] or [SONG INSTRUMENTS]
  {
    const char *leftTxt = songMode ? "SONG INSTRUMENTS" : "INSTRUMENT PRESETS";
    uint64_t bk = hashStr(hashInit(), leftTxt);
    if (full || bk != prevSysPrstBannerKey) {
      prevSysPrstBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), leftTxt, ACCENT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Instrument Presets Table
  const int total = instBrowserEntryCount();
  const int cur = instBrowserCursor();
  const int scroll = instBrowserScroll();
  const char *emptyTxt = songMode ? "NO SAVED INSTRUMENTS" : "NO SAVED PRESETS";

  for (int r = 0; r < INST_BROWSER_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const InstBrowserEntry *ent = instBrowserEntryAt(idx);
    const bool sel = (ent != nullptr) && (idx == cur);

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = !ent ? DIM : (sel ? ACCENT : COLOR_PLAY);

    const char *nameTxt = ent ? ent->instName : ((total == 0 && r == 0) ? emptyTxt : "");
    const char *typeTxt = ent ? instTypeName((InstType)ent->instType) : "";

    uint64_t rk = hashStr(hashInit(), nameTxt);
    rk = hashStr(rk, typeTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)sel << 48) | ((uint64_t)r << 52));
    if (full || rk != prevSysPrstRowKey[r]) {
      prevSysPrstRowKey[r] = rk;
      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      if (nameTxt[0] != '\0') {
        uint16_t fg = !ent ? DIM : (sel ? INK : ACCENT);
        s.drawText(SX(10), TY(rh), nameTxt, fg);
      }
      if (ent && typeTxt[0] != '\0') {
        s.drawText(SX(136), TY(rh), typeTxt, COLOR_PLAY);
      }
      if (ent) {
        const int pw = SX(44), ph = SY(16);
        const int px = rw - SX(6) - pw, py = (rh - ph) / 2;
        uint16_t pBg = sel ? ACCENT : PANEL;
        uint16_t pFg = sel ? BG : LIGHT;
        uint16_t pBd = sel ? INK : MID;
        s.drawPill(px, py, pw, ph, "DEL", pFg, pBg, pBd);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

// ============================================================================
// Projects Management Modal (/Capstan/PROJECTS/*.proj)
// ============================================================================
static inline int projManageRowCount() {
  return min(projectCount() + 1, MAX_PROJECTS);
}

FLASHMEM static void syncProjManageScroll() {
  int rowCnt = projManageRowCount();
  projManageCursor = constrain(projManageCursor, 0, rowCnt - 1);
  if (projManageCursor < projManageScroll) {
    projManageScroll = projManageCursor;
  } else if (projManageCursor >= projManageScroll + PROJ_MANAGE_VISIBLE_ROWS) {
    projManageScroll = projManageCursor - PROJ_MANAGE_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, rowCnt - PROJ_MANAGE_VISIBLE_ROWS);
  projManageScroll = constrain(projManageScroll, 0, maxScr);
  projManageCol = constrain(projManageCol, 0, PROJ_ACT_COUNT - 1);
}

static inline int projEditRowCount() {
  return min(projEditSongCount + 1, MAX_SONGS_PER_PROJECT);
}

FLASHMEM static void syncProjEditScroll() {
  int rowCnt = projEditRowCount();
  projEditCursor = constrain(projEditCursor, 0, rowCnt - 1);
  if (projEditCursor < projEditScroll) {
    projEditScroll = projEditCursor;
  } else if (projEditCursor >= projEditScroll + PROJ_EDIT_VISIBLE_ROWS) {
    projEditScroll = projEditCursor - PROJ_EDIT_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, rowCnt - PROJ_EDIT_VISIBLE_ROWS);
  projEditScroll = constrain(projEditScroll, 0, maxScr);
  if (projEditCursor >= projEditSongCount) {
    projEditCol = PROJ_EDIT_ACT_ADD;
  } else {
    projEditCol = constrain(projEditCol, 0, PROJ_EDIT_ACT_COUNT - 1);
  }
}

FLASHMEM static void syncProjAddSongScroll() {
  int total = songCount();
  if (total <= 0) {
    projAddSongCursor = 0;
    projAddSongScroll = 0;
    return;
  }
  projAddSongCursor = constrain(projAddSongCursor, 0, total - 1);
  if (projAddSongCursor < projAddSongScroll) {
    projAddSongScroll = projAddSongCursor;
  } else if (projAddSongCursor >= projAddSongScroll + PROJ_ADD_VISIBLE_ROWS) {
    projAddSongScroll = projAddSongCursor - PROJ_ADD_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, total - PROJ_ADD_VISIBLE_ROWS);
  projAddSongScroll = constrain(projAddSongScroll, 0, maxScr);
}

FLASHMEM static void syncProjPlayScroll() {
  int total = projEditSongCount;
  if (total <= 0) {
    projPlayListCursor = 0;
    projPlayListScroll = 0;
    return;
  }
  projPlayListCursor = constrain(projPlayListCursor, 0, total - 1);
  if (projPlayListCursor < projPlayListScroll) {
    projPlayListScroll = projPlayListCursor;
  } else if (projPlayListCursor >= projPlayListScroll + PROJ_PLAY_VISIBLE_ROWS) {
    projPlayListScroll = projPlayListCursor - PROJ_PLAY_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, total - PROJ_PLAY_VISIBLE_ROWS);
  projPlayListScroll = constrain(projPlayListScroll, 0, maxScr);
}

FLASHMEM static void projPlayStopAndRestoreLoadedSong() {
  projPlayActive = false;
  projPlayTailPhase = false;
  fmSynthSetExportTailPhase(false);
  storageStopTransport();
  fmSynthAllNotesOff();
  resetMasterFxState();
  transport.speedQuarter = DEFAULT_SPEED_QUARTER;
  projPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
  storageSeekTape(0);
  transport.loopEnabled = songSettings().loopEnabled;
  transport.punchEnabled = songSettings().punchEnabled;
  updateMetronomeState();
  syncSongListCursor(true);
}

FLASHMEM static bool projPlayStartSong(int startIdx, bool autoPlay) {
  projPlayActive = false;
  projPlayTailPhase = false;
  projPlaySilentFrames = 0;
  fmSynthSetExportTailPhase(false);
  storageStopTransport();
  fmSynthAllNotesOff();
  resetMasterFxState();

  if (projEditSongCount <= 0 || startIdx < 0 || startIdx >= projEditSongCount) {
    projPlayPlayingIdx = -1;
    uiForceRedraw();
    return false;
  }

  int chosenIdx = -1;
  int maxTries = autoPlay ? (projEditSongCount - startIdx) : 1;
  for (int attempt = 0; attempt < maxTries; attempt++) {
    int tryIdx = startIdx + attempt;
    if (loadSongByName(projEditSongs[tryIdx])) {
      chosenIdx = tryIdx;
      break;
    }
  }

  if (chosenIdx < 0) {
    uiShowToast("SONG NOT FOUND");
    uiForceRedraw();
    return false;
  }

  projPlayPlayingIdx = chosenIdx;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    tracks[t].armMode = ArmMode::Off;
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    midiTracks[t].armMode = ArmMode::Off;
  }
  songSettings().masterVolume = projPlayVol;
  setMasterVolume(projPlayVol);

  transport.speedQuarter = projPlaySpeedQuarter;
  transport.loopEnabled = false;
  transport.punchEnabled = false;
  metronomeOn = false;
  storageSeekTape(0);
  projPlayMaxDataFrame = storageSongMaxAudibleFrame();

  float pkL = 0.0f, pkR = 0.0f;
  takeOutputPeaks(pkL, pkR);
  projPlayLastNow = transport.now;

  if (autoPlay) {
    projPlayActive = true;
    storageStartTransport(false, false, false);
    noInterrupts();
    metronomeOn = false;
    projPlayLastNow = transport.now;
    interrupts();
  }

  noteUserActivity();
  invalidateSysProjectsModalCache();
  uiForceRedraw();
  return true;
}

FLASHMEM void sysProjectPlayService() {
  if (projModalView != ProjModalView::PlayProject || !projPlayActive) return;

  if (transport.mode != TransportMode::Running) {
    const bool finishedSong = (transport.speedQuarter < 0)
                                ? (transport.tapeFrame == 0)
                                : (transport.tapeFrame >= projPlayMaxDataFrame);
    if (finishedSong) {
      int nextIdx = projPlayPlayingIdx + 1;
      if (nextIdx < projEditSongCount) {
        if (projPlayListCursor == projPlayPlayingIdx) {
          projPlayListCursor = nextIdx;
          syncProjPlayScroll();
        }
        projPlayStartSong(nextIdx, true);
      } else {
        projPlayStopAndRestoreLoadedSong();
        uiForceRedraw();
      }
    } else {
      projPlayActive = false;
      fmSynthSetExportTailPhase(false);
      uiForceRedraw();
    }
    return;
  }

  if (transport.speedQuarter <= 0) {
    projPlayTailPhase = false;
    projPlaySilentFrames = 0;
    return;
  }

  uint32_t curNow = transport.now;
  uint32_t dtFrames = curNow - projPlayLastNow;
  if (dtFrames < 256u) return;
  projPlayLastNow = curNow;

  float pkL = 0.0f, pkR = 0.0f;
  takeOutputPeaks(pkL, pkR);

  uint32_t tf = transport.tapeFrame;
  if (tf < projPlayMaxDataFrame) {
    projPlayTailPhase = false;
    projPlaySilentFrames = 0;
    return;
  }

  if (!projPlayTailPhase) {
    projPlayTailPhase = true;
    projPlaySilentFrames = 0;
    fmSynthSetExportTailPhase(true);
  }

  const SongSettings &ps = songSettings();
  const bool hasDelayOrVerb = (ps.masterDelay > 0 || ps.masterReverb > 0 || fmSynthHasActiveOutput());
  const uint32_t reqSilent = hasDelayOrVerb ? 16384u : 2048u;

  if (pkL <= (4.5f / 32768.0f) && pkR <= (4.5f / 32768.0f)) {
    projPlaySilentFrames += dtFrames;
  } else {
    projPlaySilentFrames = 0;
  }

  const bool tailTimeout = (tf > projPlayMaxDataFrame) &&
                           ((tf - projPlayMaxDataFrame) >= 15u * SAMPLE_RATE_HZ);
  if ((projPlaySilentFrames >= reqSilent && !fmSynthHasActiveOutput()) || tailTimeout) {
    int nextIdx = projPlayPlayingIdx + 1;
    if (nextIdx < projEditSongCount) {
      if (projPlayListCursor == projPlayPlayingIdx) {
        projPlayListCursor = nextIdx;
        syncProjPlayScroll();
      }
      projPlayStartSong(nextIdx, true);
    } else {
      projPlayStopAndRestoreLoadedSong();
      uiForceRedraw();
    }
  }
}

FLASHMEM bool isSysProjectsModalOpen() {
  return projModalView != ProjModalView::Closed;
}

FLASHMEM uint8_t sysProjectsModalState() {
  if (projModalView == ProjModalView::Closed) return 0;
  return (uint8_t)projModalView | (projConfirmOpen ? 0x10u : 0x00u);
}

FLASHMEM bool isSysProjectsConfirmOpen() {
  return (projModalView != ProjModalView::Closed) && projConfirmOpen;
}

FLASHMEM bool isSysProjectPlayLocked() {
  return currentPage() == Page::SystemSettings &&
         projModalView == ProjModalView::PlayProject &&
         projPlayLocked;
}

FLASHMEM void toggleSysProjectPlayLock() {
  if (currentPage() != Page::SystemSettings || projModalView != ProjModalView::PlayProject) {
    return;
  }
  projPlayLocked = !projPlayLocked;
  projPlayEditHeld = false;
  projPlayEditModUsed = false;
  inputSwallowModalKeys();
  inputClearRepeat();
  uiForceRedraw();
}

FLASHMEM static void openSysProjectsModal() {
  scanProjectFiles();
  projModalView = ProjModalView::ManageList;
  projConfirmOpen = false;
  projConfirmCursor = 0;
  projPlayLocked = false;
  if (projManageCursor >= projectCount()) {
    projManageCursor = max(0, projectCount() - 1);
  }
  projManageCol = PROJ_ACT_PLAY;
  syncProjManageScroll();
  invalidateSysProjectsModalCache();
}

FLASHMEM void closeSysProjectsModal() {
  if (projModalView == ProjModalView::Closed) return;
  if (projModalView == ProjModalView::PlayProject) {
    projPlayStopAndRestoreLoadedSong();
  }
  projModalView = ProjModalView::Closed;
  projConfirmOpen = false;
  projPlayLocked = false;
  invalidateSysProjectsModalCache();
}

FLASHMEM void commitProjectKeyboard(const char *enteredName, bool isRename) {
  char outName[MAX_PROJECT_NAME_LEN + 1] = {};
  bool ok = false;
  if (isRename) {
    const char *oldName = projectNameAt(projManageCursor);
    ok = renameProjectFile(oldName, enteredName, outName, sizeof(outName));
  } else {
    ok = createEmptyProjectFile(enteredName, outName, sizeof(outName));
  }
  closeSaveKeyboard();
  projModalView = ProjModalView::ManageList;
  projConfirmOpen = false;
  if (ok && outName[0] != '\0') {
    int idx = findProjectIndex(outName);
    if (idx >= 0) projManageCursor = idx;
    projManageCol = PROJ_ACT_EDIT;
    syncProjManageScroll();
    char msg[32];
    snprintf(msg, sizeof(msg), "%s: %s", isRename ? "RENAMED" : "CREATED", outName);
    uiShowToast(msg);
  } else {
    syncProjManageScroll();
    uiShowToast(isRename ? "RENAME FAILED" : "CREATE FAILED");
  }
  invalidateSysProjectsModalCache();
  uiForceRedraw();
}

FLASHMEM void drawSysProjectsConfirmModal(bool full) {
  if (!isSysProjectsConfirmOpen()) return;

  const int cur = constrain(projConfirmCursor, 0, 1);
  const int ovX = SX(12);
  const int ovY = SY(94);
  const int ovW = SX(296);
  const int ovH = SY(24);
  const bool isDelProj = (projModalView == ProjModalView::ManageList);

  uint64_t k = hashMix(hashInit(), (uint64_t)ACCENT | ((uint64_t)cur << 16) |
                                   ((uint64_t)isDelProj << 24));
  if (full || k != prevProjConfirmKey) {
    prevProjConfirmKey = k;
    Surface s(ovW, ovH);
    s.drawBox(DARK, ACCENT);
    s.drawText(SX(10), TY(ovH), isDelProj ? "Delete project?" : "Remove song?", INK);

    const char *const OPT_LABELS[2] = {"YES", "NO"};
    const int pillW = SX(42);
    const int pillH = SY(16);
    const int pillY = max(1, (ovH - pillH) / 2);
    const int pillX[2] = {SX(196), SX(244)};

    for (int i = 0; i < 2; i++) {
      bool sel = (cur == i);
      uint16_t pBg = sel ? (i == 0 ? COLOR_WARN : ACCENT) : PANEL;
      uint16_t pFg = sel ? BG : INK;
      uint16_t pBd = sel ? INK : MID;
      s.drawPill(pillX[i], pillY, pillW, pillH, OPT_LABELS[i], pFg, pBg, pBd);
    }
    s.blit(ovX, ovY);
  }
}

FLASHMEM static void drawProjManageListModal(bool full) {
  syncProjManageScroll();
  const int total = projectCount();
  const int cur = projManageCursor;
  const int scroll = projManageScroll;

  // 1. Header Banner: [PROJECTS]
  {
    uint64_t bk = hashStr(hashInit(), "PROJECTS");
    bk = hashMix(bk, (uint64_t)total);
    if (full || bk != prevProjBannerKey) {
      prevProjBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), "PROJECTS", ACCENT);
      char cntTxt[20];
      snprintf(cntTxt, sizeof(cntTxt), "%d SAVED", total);
      int tw = textWidth(cntTxt);
      s.drawText(bw - SX(6) - tw, TY(bh), cntTxt, DIM);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Projects Table (populated projects + next empty row to create a project)
  const char *const ACT_LABELS[PROJ_ACT_COUNT] = {"PLAY", "EDIT", "RENAME", "DEL"};
  const int pillW[PROJ_ACT_COUNT] = {SX(36), SX(36), SX(58), SX(32)};

  for (int r = 0; r < PROJ_MANAGE_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const bool hasProj = (idx < total);
    const bool isNextEmpty = (idx == total && idx < MAX_PROJECTS);
    const bool isValidRow = hasProj || isNextEmpty;
    const bool sel = isValidRow && (idx == cur);
    const int activeCol = sel ? (hasProj ? projManageCol : 0) : -1;

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = sel ? ACCENT : (hasProj ? COLOR_PLAY : DIM);

    const char *nameTxt = hasProj ? projectNameAt(idx)
                        : isNextEmpty ? ((total == 0) ? "NO SAVED PROJECTS" : "---")
                                      : "";

    uint64_t rk = hashStr(hashInit(), nameTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)(activeCol + 2) << 48) | ((uint64_t)hasProj << 54) |
                     ((uint64_t)isNextEmpty << 55) | ((uint64_t)r << 56));
    if (full || rk != prevProjRowKey[r]) {
      prevProjRowKey[r] = rk;
      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      int pillX[PROJ_ACT_COUNT] = {};
      if (sel && hasProj) {
        int curRight = rw - SX(5);
        for (int c = PROJ_ACT_COUNT - 1; c >= 0; c--) {
          curRight -= pillW[c];
          pillX[c] = curRight;
          curRight -= SX(3);
        }
      }

      if (nameTxt[0] != '\0') {
        char dispName[32];
        snprintf(dispName, sizeof(dispName), "%s", nameTxt);
        if (sel && hasProj) {
          int maxTxtW = max(SX(40), pillX[0] - SX(12));
          while (textWidth(dispName) > maxTxtW && strlen(dispName) > 3) {
            dispName[strlen(dispName) - 1] = '\0';
          }
        }
        uint16_t fg = !hasProj ? ((sel && total > 0) ? LIGHT : DIM) : (sel ? INK : ACCENT);
        s.drawText(SX(8), TY(rh), dispName, fg);
      }

      if (sel) {
        const int ph = SY(16);
        const int py = (rh - ph) / 2;
        if (!hasProj) {
          int pw = SX(42);
          int px = rw - SX(6) - pw;
          s.drawPill(px, py, pw, ph, "NEW", BG, ACCENT, INK);
        } else {
          for (int c = 0; c < PROJ_ACT_COUNT; c++) {
            bool cSel = (activeCol == c);
            uint16_t pBg = cSel ? ((c == PROJ_ACT_DELETE) ? COLOR_WARN
                                 : (c == PROJ_ACT_PLAY)   ? COLOR_PLAY : ACCENT) : PANEL;
            uint16_t pFg = cSel ? BG : LIGHT;
            uint16_t pBd = cSel ? INK : MID;
            s.drawPill(pillX[c], py, pillW[c], ph, ACT_LABELS[c], pFg, pBg, pBd);
          }
        }
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM static void drawProjEditModal(bool full) {
  syncProjEditScroll();
  const int total = projEditSongCount;
  const int cur = projEditCursor;
  const int scroll = projEditScroll;

  // 1. Header Banner: [PROJECT: <name>]
  {
    char leftTxt[32];
    snprintf(leftTxt, sizeof(leftTxt), "PROJECT: %s", projEditingName);
    uint64_t bk = hashStr(hashInit(), leftTxt);
    bk = hashMix(bk, (uint64_t)total);
    if (full || bk != prevProjBannerKey) {
      prevProjBannerKey = bk;
      const int bw = SX(308), bh = SY(20);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), leftTxt, ACCENT);
      char cntTxt[20];
      snprintf(cntTxt, sizeof(cntTxt), "%d %s", total, (total == 1) ? "SONG" : "SONGS");
      int tw = textWidth(cntTxt);
      s.drawText(bw - SX(6) - tw, TY(bh), cntTxt, LIGHT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Numbered 6-Row Song List (populated songs + next empty row to add a song)
  for (int r = 0; r < PROJ_EDIT_VISIBLE_ROWS; r++) {
    const int y = SY(48) + r * SY(24);
    const int rw = SX(308), rh = SY(22);
    const int idx = scroll + r;
    const bool hasSong = (idx < total);
    const bool isNextEmpty = (idx == total && idx < MAX_SONGS_PER_PROJECT);
    const bool isValidRow = hasSong || isNextEmpty;
    const bool sel = !projEditInBottomRow && isValidRow && (idx == cur);
    const int activeCol = sel ? (hasSong ? projEditCol : PROJ_EDIT_ACT_ADD) : -1;

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = sel ? ACCENT : (hasSong ? COLOR_PLAY : DIM);

    char leftTxt[32];
    if (hasSong) {
      snprintf(leftTxt, sizeof(leftTxt), "%02d  %s", idx + 1, projEditSongs[idx]);
    } else if (isNextEmpty) {
      snprintf(leftTxt, sizeof(leftTxt), "%02d  ---", idx + 1);
    } else {
      leftTxt[0] = '\0';
    }

    uint64_t rk = hashStr(hashInit(), leftTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)(activeCol + 2) << 48) | ((uint64_t)hasSong << 54) |
                     ((uint64_t)isNextEmpty << 55) | ((uint64_t)r << 56));
    if (full || rk != prevProjRowKey[r]) {
      prevProjRowKey[r] = rk;
      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      if (leftTxt[0] != '\0') {
        uint16_t fg = !hasSong ? (sel ? LIGHT : DIM) : (sel ? INK : ACCENT);
        s.drawText(SX(10), TY(rh), leftTxt, fg);
      }

      if (sel) {
        const int ph = SY(15);
        const int py = (rh - ph) / 2;
        if (!hasSong) {
          int pw = SX(42);
          int px = rw - SX(6) - pw;
          s.drawPill(px, py, pw, ph, "ADD", BG, ACCENT, INK);
        } else {
          const char *const ELABELS[2] = {"ADD", "REMOVE"};
          const int eW[2] = {SX(42), SX(56)};
          const int eX[2] = {rw - SX(6) - eW[1] - SX(4) - eW[0], rw - SX(6) - eW[1]};
          for (int c = 0; c < 2; c++) {
            bool cSel = (activeCol == c);
            uint16_t pBg = cSel ? ((c == PROJ_EDIT_ACT_REMOVE) ? COLOR_WARN : ACCENT) : PANEL;
            uint16_t pFg = cSel ? BG : LIGHT;
            uint16_t pBd = cSel ? INK : MID;
            s.drawPill(eX[c], py, eW[c], ph, ELABELS[c], pFg, pBg, pBd);
          }
        }
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }

  // 3. Bottom Action Row: [ SAVE ] and [ CANCEL ]
  for (int b = 0; b < 2; b++) {
    const char *btnTitle = (b == 0) ? "SAVE" : "CANCEL";
    int x = (b == 0) ? SX(6) : SX(164);
    int w = SX(150), h = SY(28);
    bool sel = (projEditInBottomRow && projEditBottomCol == b);
    uint16_t bg = sel ? (b == 0 ? COLOR_PLAY : COLOR_WARN) : PANEL;
    uint16_t fg = sel ? BG : (b == 0 ? COLOR_PLAY : LIGHT);
    uint16_t border = sel ? INK : (b == 0 ? COLOR_PLAY : MID);

    uint64_t bk = hashStr(hashInit(), btnTitle);
    bk = hashMix(bk, (uint64_t)bg | ((uint64_t)fg << 16) | ((uint64_t)border << 32));
    if (full || bk != prevProjBtnKey[b]) {
      prevProjBtnKey[b] = bk;
      Surface s(w, h);
      s.drawBox(bg, border);
      int tw = textWidth(btnTitle);
      s.drawText(max(4, (w - tw) / 2), TY(h), btnTitle, fg);
      s.blit(x, SY(196));
    }
  }
}

FLASHMEM static void drawProjAddSongModal(bool full) {
  syncProjAddSongScroll();
  const int total = songCount();
  const int cur = projAddSongCursor;
  const int scroll = projAddSongScroll;

  // 1. Header Banner: [ADD SONG TO PROJECT]
  {
    uint64_t bk = hashStr(hashInit(), "ADD SONG TO PROJECT");
    bk = hashMix(bk, (uint64_t)total);
    if (full || bk != prevProjBannerKey) {
      prevProjBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), "ADD SONG TO PROJECT", ACCENT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Songs Table (/Capstan/SONGS/)
  for (int r = 0; r < PROJ_ADD_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const bool hasSong = (total > 0 && idx < total);
    const bool sel = hasSong && (idx == cur);

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = !hasSong ? DIM : (sel ? ACCENT : COLOR_PLAY);

    char leftTxt[32];
    if (hasSong) {
      snprintf(leftTxt, sizeof(leftTxt), "%02d  %s", idx + 1, songNameAt(idx));
    } else if (total == 0 && r == 0) {
      snprintf(leftTxt, sizeof(leftTxt), "NO SAVED SONGS");
    } else {
      leftTxt[0] = '\0';
    }

    uint64_t rk = hashStr(hashInit(), leftTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)sel << 48) | ((uint64_t)r << 52));
    if (full || rk != prevProjRowKey[r]) {
      prevProjRowKey[r] = rk;
      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      if (leftTxt[0] != '\0') {
        uint16_t fg = !hasSong ? DIM : (sel ? INK : ACCENT);
        s.drawText(SX(10), TY(rh), leftTxt, fg);
      }
      if (hasSong) {
        const int pw = SX(44), ph = SY(16);
        const int px = rw - SX(6) - pw, py = (rh - ph) / 2;
        uint16_t pBg = sel ? ACCENT : PANEL;
        uint16_t pFg = sel ? BG : LIGHT;
        uint16_t pBd = sel ? INK : MID;
        s.drawPill(px, py, pw, ph, "ADD", pFg, pBg, pBd);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM static void drawProjPlayModal(bool full) {
  syncProjPlayScroll();
  const int total = projEditSongCount;
  const int cur = projPlayListCursor;
  const int scroll = projPlayListScroll;
  const bool isPlaying = projPlayActive && (transport.mode == TransportMode::Running);

  // 1. Startup Tape Graphic (scaled larger to SY(114) tall, with project name inside)
  {
    uint32_t nowMs = millis();
    if (isPlaying) {
      if (nowMs - projPlayLastTickMs >= 100) {
        int32_t dt = (int32_t)(nowMs - projPlayLastTickMs);
        projPlayAnimMs = (uint32_t)((int32_t)projPlayAnimMs + (dt * (int32_t)transport.speedQuarter) / 4);
        projPlayLastTickMs = nowMs;
      }
    } else {
      projPlayLastTickMs = nowMs;
    }
    uint16_t tapeCol = isPlaying ? LOGO_COLOR : blend565(LOGO_COLOR, DIM, 170);
    uint64_t staticKey = hashStr(hashInit(), projEditingName);
    staticKey = hashMix(staticKey, (uint64_t)tapeCol | ((uint64_t)isPlaying << 16));
    uint64_t tk = hashMix(staticKey, (uint64_t)projPlayAnimMs);
    if (full || tk != prevProjTapeKey) {
      bool reelsOnly = (!full && staticKey == prevProjTapeStaticKey);
      prevProjTapeKey = tk;
      prevProjTapeStaticKey = staticKey;
      const int deckH = SY(114);
      const int deckW = (deckH * 176) / 108;
      const int deckX = (SCREEN_W - deckW) / 2;
      const int deckY = SY(2);
      drawTapeDeckGraphic(deckX, deckY, deckW, deckH, projPlayAnimMs, tapeCol, 255, projEditingName, false, reelsOnly);
      if (!reelsOnly) {
        prevProjLockKey = 0;
      }
      keepStreaming();
    }
  }

  // 2. Top-Right Power-Button Lock Hint ("LOCK ->" when unlocked, "UNLOCK ->" when locked)
  {
    uint64_t lk = hashMix(hashInit(), projPlayLocked ? 2ULL : 1ULL);
    if (full || lk != prevProjLockKey) {
      prevProjLockKey = lk;
      const int w = SX(74), h = SY(14);
      Surface s(w, h);
      s.fillRect(0, 0, w, h, BG);
      const char *lockTxt = projPlayLocked ? "UNLOCK ->" : "LOCK ->";
      int tw = textWidth(lockTxt);
      s.drawText(max(0, w - SX(4) - tw), TY(h), lockTxt, projPlayLocked ? LOGO_COLOR : LIGHT);
      s.blit(SCREEN_W - w, SY(4));
    }
  }

  // 3. Controls Row: [ |< , Play/Stop + Speed , >| , VOL control ]
  //    Plain text on black; border drawn ONLY around the control where the cursor is.
  {
    const bool inCtrl = !projPlayLocked && (projPlayZone == PROJ_PLAY_ZONE_CONTROLS);
    const int selCol = inCtrl ? (int)projPlayCtrlCol : -1;
    const int sqVal = (int)transport.speedQuarter;
    uint64_t ck = hashMix(hashInit(), (uint64_t)(selCol + 2) | ((uint64_t)isPlaying << 8) |
                                      ((uint64_t)projPlayVol << 16) |
                                      ((uint64_t)(uint8_t)sqVal << 24) |
                                      ((uint64_t)projPlayLocked << 32));
    if (full || ck != prevProjCtrlKey) {
      prevProjCtrlKey = ck;
      const int rw = SX(200), rh = SY(18);
      Surface s(rw, rh);
      s.fillRect(0, 0, rw, rh, BG);

      int absQ = abs(sqVal);
      int whole = absQ / 4;
      int frac = (absQ % 4) * 25;
      char speedTxt[12];
      snprintf(speedTxt, sizeof(speedTxt), "%s%d.%02dX", (sqVal < 0) ? "-" : "", whole, frac);
      char playLabel[16];
      if (isPlaying) {
        snprintf(playLabel, sizeof(playLabel), "%s", speedTxt);
      } else {
        snprintf(playLabel, sizeof(playLabel), "%c %s", (sqVal < 0) ? '<' : '>', speedTxt);
      }

      const int boxX[PROJ_PLAY_CTRL_COUNT] = {0, SX(30), SX(104), SX(134)};
      const int boxW[PROJ_PLAY_CTRL_COUNT] = {SX(24), SX(68), SX(24), SX(66)};
      char volTxt[16];
      snprintf(volTxt, sizeof(volTxt), "VOL: %d", (int)projPlayVol);
      const char *const labels[PROJ_PLAY_CTRL_COUNT] = {
        "|<",
        playLabel,
        ">|",
        volTxt
      };

      for (int c = 0; c < PROJ_PLAY_CTRL_COUNT; c++) {
        bool cSel = (selCol == c);
        bool isPlayBtn = (c == PROJ_PLAY_CTRL_PLAY);
        int bx = boxX[c], bw = boxW[c];
        if (cSel) {
          s.fillRect(bx, 0, bw, rh, LOGO_COLOR);
          s.fillRect(bx + PX, PX, bw - 2 * PX, rh - 2 * PX, BG);
        }
        uint16_t fg = (isPlayBtn && isPlaying) ? LOGO_COLOR : (cSel ? INK : LIGHT);
        int tw = textWidth(labels[c]);
        if (isPlayBtn && isPlaying) {
          const int sq = SY(10);
          const int gapSq = SX(4);
          int totalW = sq + gapSq + tw;
          int startX = bx + max(2, (bw - totalW) / 2);
          s.fillRect(startX, (rh - sq) / 2, sq, sq, fg);
          s.drawText(startX + sq + gapSq, TY(rh), labels[c], fg);
        } else {
          s.drawText(bx + max(2, (bw - tw) / 2), TY(rh), labels[c], fg);
        }
      }
      s.blit((SCREEN_W - rw) / 2, SY(120));
      keepStreaming();
    }
  }

  // 4. Scrollable Song List below the cassette (with "-->" to the left of the playing song)
  //    Faint outer border around the table; cursor border drawn ONLY around the active row.
  for (int r = 0; r < PROJ_PLAY_VISIBLE_ROWS; r++) {
    const int y = SY(142) + r * SY(18);
    const int rw = SX(200), rh = SY(18);
    const int idx = scroll + r;
    const bool hasSong = (total > 0 && idx < total);
    const bool isCurPlayingSong = hasSong && (idx == projPlayPlayingIdx);
    const bool sel = !projPlayLocked && (projPlayZone == PROJ_PLAY_ZONE_LIST) && hasSong && (idx == cur);

    char songTxt[32];
    if (hasSong) {
      snprintf(songTxt, sizeof(songTxt), "%d.  %s", idx + 1, projEditSongs[idx]);
    } else if (total == 0 && r == 0) {
      snprintf(songTxt, sizeof(songTxt), "NO SONGS IN PROJECT");
    } else {
      songTxt[0] = '\0';
    }

    uint64_t rk = hashStr(hashInit(), songTxt);
    rk = hashMix(rk, (uint64_t)sel | ((uint64_t)isCurPlayingSong << 8) |
                     ((uint64_t)isPlaying << 16) | ((uint64_t)r << 24));
    if (full || rk != prevProjRowKey[r]) {
      prevProjRowKey[r] = rk;
      Surface s(rw, rh);
      s.fillRect(0, 0, rw, rh, BG);

      // Faint outer border around the scrollable song table
      constexpr uint16_t tableBd = DARK;
      s.fillRect(0, 0, PX, rh, tableBd);
      s.fillRect(rw - PX, 0, PX, rh, tableBd);
      if (r == 0) {
        s.fillRect(0, 0, rw, PX, tableBd);
      }
      if (r == PROJ_PLAY_VISIBLE_ROWS - 1) {
        s.fillRect(0, rh - PX, rw, PX, tableBd);
      }

      if (sel) {
        s.fillRect(2 * PX, PX, rw - 4 * PX, rh - 2 * PX, LOGO_COLOR);
        s.fillRect(3 * PX, 2 * PX, rw - 6 * PX, rh - 4 * PX, BG);
      }

      if (isCurPlayingSong) {
        s.drawText(SX(8), TY(rh), "-->", isPlaying ? LOGO_COLOR : LIGHT);
      }
      if (songTxt[0] != '\0') {
        uint16_t fg = !hasSong ? DIM
                    : isCurPlayingSong ? (isPlaying ? LOGO_COLOR : INK)
                    : (sel ? INK : LIGHT);
        int textX = hasSong ? SX(36) : SX(8);
        s.drawText(textX, TY(rh), songTxt, fg);
      }
      s.blit((SCREEN_W - rw) / 2, y);
    }
    keepStreaming();
  }

  // 5. Bottom-Right EXIT Button (plain text on black; border drawn ONLY when cursor is on EXIT)
  {
    const bool sel = !projPlayLocked && (projPlayZone == PROJ_PLAY_ZONE_EXIT);
    const int w = SX(44), h = SY(18);

    uint64_t bk = hashMix(hashInit(), (uint64_t)sel);
    if (full || bk != prevProjBtnKey[0]) {
      prevProjBtnKey[0] = bk;
      Surface s(w, h);
      if (sel) {
        s.drawBox(BG, LOGO_COLOR);
      } else {
        s.fillRect(0, 0, w, h, BG);
      }
      const char *btnTitle = "EXIT";
      int tw = textWidth(btnTitle);
      s.drawText(max(2, (w - tw) / 2), TY(h), btnTitle, sel ? INK : LIGHT);
      s.blit(SX(266), SY(214));
    }
  }
}

FLASHMEM void handleSysProjectsModalInput(uint8_t pressed, uint8_t now, uint32_t ms) {
  (void)ms;
  if (projModalView == ProjModalView::Closed) return;

  // 1. Confirmation Overlay ("Delete project?" or "Remove song?")
  if (projConfirmOpen) {
    int step = ((pressed & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
               ((pressed & (BTN_UP | BTN_LEFT)) ? 1 : 0);
    if (step != 0) {
      projConfirmCursor = (projConfirmCursor + step + 2) % 2;
      uiForceRedraw();
    }
    if (pressed & BTN_EDIT) {
      bool yes = (projConfirmCursor == 0);
      projConfirmOpen = false;
      if (yes) {
        if (projModalView == ProjModalView::ManageList) {
          const char *pName = projectNameAt(projManageCursor);
          if (deleteProjectFile(pName)) {
            if (projManageCursor >= projectCount() && projectCount() > 0) {
              projManageCursor = projectCount() - 1;
            }
            syncProjManageScroll();
            uiShowToast("PROJECT DELETED");
          } else {
            uiShowToast("DELETE FAILED");
          }
        } else if (projModalView == ProjModalView::EditProject) {
          if (projEditSongCount > 0 && projEditCursor >= 0 && projEditCursor < projEditSongCount) {
            for (int i = projEditCursor; i < projEditSongCount - 1; i++) {
              memcpy(projEditSongs[i], projEditSongs[i + 1], sizeof(projEditSongs[i]));
            }
            projEditSongCount--;
            syncProjEditScroll();
            uiShowToast("SONG REMOVED");
          }
        }
      }
      inputSwallowModalKeys();
      uiForceRedraw();
    } else if (pressed & BTN_OPT) {
      projConfirmOpen = false;
      inputSwallowModalKeys();
      uiForceRedraw();
    }
    return;
  }

  const int dy = ((pressed & BTN_DOWN) ? 1 : 0) - ((pressed & BTN_UP) ? 1 : 0);
  const int dx = ((pressed & BTN_RIGHT) ? 1 : 0) - ((pressed & BTN_LEFT) ? 1 : 0);
  const bool shiftLeft = (pressed & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT));

  // 2. Manage Projects Table
  if (projModalView == ProjModalView::ManageList) {
    if (shiftLeft || (pressed & BTN_OPT)) {
      closeSysProjectsModal();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      int total = projectCount();
      int maxRow = projManageRowCount() - 1;
      if (dy != 0) {
        projManageCursor = constrain(projManageCursor + dy, 0, maxRow);
        syncProjManageScroll();
        uiForceRedraw();
      }
      if (dx != 0 && projManageCursor < total) {
        projManageCol = constrain(projManageCol + dx, 0, PROJ_ACT_COUNT - 1);
        uiForceRedraw();
      }
      if (pressed & BTN_EDIT) {
        if (projManageCursor >= total) {
          openProjectKeyboard(false, "", true);
          inputSwallowModalKeys();
        } else if (projManageCol == PROJ_ACT_PLAY) {
          snprintf(projEditingName, sizeof(projEditingName), "%s", projectNameAt(projManageCursor));
          projEditSongCount = loadProjectSongs(projEditingName, projEditSongs, MAX_SONGS_PER_PROJECT);
          projPlayVol = songSettings().masterVolume;
          projPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
          projPlayLocked = false;
          projPlayEditHeld = false;
          projPlayEditModUsed = false;
          projPlayListCursor = 0;
          projPlayListScroll = 0;
          projPlayPlayingIdx = -1;
          projPlayZone = (projEditSongCount > 0) ? PROJ_PLAY_ZONE_CONTROLS : PROJ_PLAY_ZONE_EXIT;
          projPlayCtrlCol = PROJ_PLAY_CTRL_PLAY;
          projPlayAnimMs = 0;
          projPlayLastTickMs = millis();
          projModalView = ProjModalView::PlayProject;
          invalidateSysProjectsModalCache();
          inputSwallowModalKeys();
          if (projEditSongCount > 0) {
            projPlayStartSong(0, true);
          } else {
            uiShowToast("PROJECT IS EMPTY");
            uiForceRedraw();
          }
        } else if (projManageCol == PROJ_ACT_EDIT) {
          snprintf(projEditingName, sizeof(projEditingName), "%s", projectNameAt(projManageCursor));
          projEditSongCount = loadProjectSongs(projEditingName, projEditSongs, MAX_SONGS_PER_PROJECT);
          projEditCursor = 0;
          projEditScroll = 0;
          projEditCol = PROJ_EDIT_ACT_ADD;
          projEditInBottomRow = false;
          projEditBottomCol = 0;
          projModalView = ProjModalView::EditProject;
          invalidateSysProjectsModalCache();
          inputSwallowModalKeys();
          uiForceRedraw();
        } else if (projManageCol == PROJ_ACT_RENAME) {
          openProjectKeyboard(true, projectNameAt(projManageCursor), false);
          inputSwallowModalKeys();
        } else if (projManageCol == PROJ_ACT_DELETE) {
          projConfirmCursor = 0;
          projConfirmOpen = true;
          inputSwallowModalKeys();
          uiForceRedraw();
        }
      }
    }
    return;
  }

  // 3. Edit Project Table
  if (projModalView == ProjModalView::EditProject) {
    if (shiftLeft) {
      projModalView = ProjModalView::ManageList;
      invalidateSysProjectsModalCache();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      if (dy != 0) {
        int maxRow = projEditRowCount() - 1;
        if (projEditInBottomRow) {
          if (dy < 0) {
            projEditInBottomRow = false;
            projEditCursor = maxRow;
            syncProjEditScroll();
          }
        } else {
          if (dy > 0 && projEditCursor >= maxRow) {
            projEditInBottomRow = true;
          } else {
            projEditCursor = constrain(projEditCursor + dy, 0, maxRow);
            syncProjEditScroll();
          }
        }
        uiForceRedraw();
      }
      if (dx != 0) {
        if (projEditInBottomRow) {
          projEditBottomCol = constrain(projEditBottomCol + dx, 0, 1);
        } else if (projEditCursor < projEditSongCount) {
          projEditCol = constrain(projEditCol + dx, 0, PROJ_EDIT_ACT_COUNT - 1);
        }
        uiForceRedraw();
      }
      if (pressed & BTN_EDIT) {
        if (projEditInBottomRow) {
          if (projEditBottomCol == 0) {
            if (saveProjectSongs(projEditingName, projEditSongs, projEditSongCount)) {
              uiShowToast("PROJECT SAVED");
            } else {
              uiShowToast("SAVE FAILED");
            }
          }
          projModalView = ProjModalView::ManageList;
          invalidateSysProjectsModalCache();
          inputSwallowModalKeys();
          uiForceRedraw();
        } else {
          int act = (projEditCursor >= projEditSongCount) ? PROJ_EDIT_ACT_ADD : projEditCol;
          if (act == PROJ_EDIT_ACT_ADD) {
            scanSongFolders();
            projAddSongCursor = 0;
            projAddSongScroll = 0;
            projModalView = ProjModalView::AddSong;
            invalidateSysProjectsModalCache();
            inputSwallowModalKeys();
            uiForceRedraw();
          } else if (act == PROJ_EDIT_ACT_REMOVE && projEditCursor < projEditSongCount) {
            projConfirmCursor = 0;
            projConfirmOpen = true;
            inputSwallowModalKeys();
            uiForceRedraw();
          }
        }
      }
    }
    return;
  }

  // 4. Add Song Picker Table (/Capstan/SONGS/)
  if (projModalView == ProjModalView::AddSong) {
    if (shiftLeft || (pressed & BTN_OPT)) {
      projModalView = ProjModalView::EditProject;
      invalidateSysProjectsModalCache();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      int total = songCount();
      int step = dy + dx * PROJ_ADD_VISIBLE_ROWS;
      if (step != 0 && total > 0) {
        projAddSongCursor = constrain(projAddSongCursor + step, 0, total - 1);
        syncProjAddSongScroll();
        uiForceRedraw();
      }
      if (pressed & BTN_EDIT) {
        if (total <= 0) {
          uiShowToast("NO SAVED SONGS");
        } else if (projEditSongCount >= MAX_SONGS_PER_PROJECT) {
          uiShowToast("PROJECT FULL");
        } else {
          const char *sName = songNameAt(projAddSongCursor);
          int insIdx = (projEditCursor >= projEditSongCount)
                         ? projEditSongCount
                         : constrain(projEditCursor + 1, 0, projEditSongCount);
          for (int i = projEditSongCount; i > insIdx; i--) {
            memcpy(projEditSongs[i], projEditSongs[i - 1], sizeof(projEditSongs[i]));
          }
          snprintf(projEditSongs[insIdx], sizeof(projEditSongs[insIdx]), "%s", sName);
          projEditSongCount++;
          projEditCursor = insIdx;
          projEditCol = PROJ_EDIT_ACT_ADD;
          syncProjEditScroll();
          char msg[32];
          snprintf(msg, sizeof(msg), "ADDED: %s", sName);
          uiShowToast(msg);
        }
        projModalView = ProjModalView::EditProject;
        invalidateSysProjectsModalCache();
        inputSwallowModalKeys();
        uiForceRedraw();
      }
    }
    return;
  }

  // 5. Project Play View
  if (projModalView == ProjModalView::PlayProject) {
    if (shiftLeft) {
      projPlayStopAndRestoreLoadedSong();
      projModalView = ProjModalView::ManageList;
      invalidateSysProjectsModalCache();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }

    const bool onPlayCell = (projPlayZone == PROJ_PLAY_ZONE_CONTROLS &&
                             projPlayCtrlCol == PROJ_PLAY_CTRL_PLAY);
    const bool onVolCell = (projPlayZone == PROJ_PLAY_ZONE_CONTROLS &&
                            projPlayCtrlCol == PROJ_PLAY_CTRL_VOL);
    const bool editJustPressed = (pressed & BTN_EDIT) != 0;
    const bool editJustReleased = projPlayEditHeld && !(now & BTN_EDIT);
    if (editJustPressed) {
      projPlayEditHeld = true;
      projPlayEditModUsed = false;
    }
    if (!(now & BTN_EDIT)) {
      projPlayEditHeld = false;
    }

    // OPT + EDIT on Play/Stop button resets playback speed to 1.00x (same as transport bar)
    if (onPlayCell && (((pressed & BTN_EDIT) && (now & BTN_OPT)) ||
                       ((pressed & BTN_OPT) && (now & BTN_EDIT)))) {
      projPlayEditModUsed = true;
      projPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
      appSetPlaybackSpeedQuarter(DEFAULT_SPEED_QUARTER);
      if (transport.mode == TransportMode::Stopped) {
        appSeekTape(0);
      } else {
        uiShowToast("SPEED: 1.00X");
      }
      uiForceRedraw();
      return;
    }

    // EDIT + LEFT/RIGHT on Play/Stop button adjusts playback speed & direction (-5.00x..+5.00x);
    // EDIT + UP jumps directly to 1.00x playback; EDIT + DOWN jumps directly to -1.00x playback.
    if (onPlayCell && (now & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT))) {
      if (dx != 0) {
        projPlayEditModUsed = true;
        appStepPlaybackSpeed(dx);
        projPlaySpeedQuarter = (int8_t)transport.speedQuarter;
        uiForceRedraw();
      } else if (dy < 0) {
        projPlayEditModUsed = true;
        projPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
        appSetPlaybackSpeedQuarter(DEFAULT_SPEED_QUARTER);
        uiForceRedraw();
      } else if (dy > 0) {
        projPlayEditModUsed = true;
        projPlaySpeedQuarter = REVERSE_1X_SPEED_QUARTER;
        appSetPlaybackSpeedQuarter(REVERSE_1X_SPEED_QUARTER);
        uiForceRedraw();
      }
      return;
    }

    // OPT + EDIT on VOL resets master volume to default
    if (onVolCell && (((pressed & BTN_EDIT) && (now & BTN_OPT)) ||
                      ((pressed & BTN_OPT) && (now & BTN_EDIT)))) {
      projPlayEditModUsed = true;
      projPlayVol = MASTER_VOLUME_DEFAULT;
      songSettings().masterVolume = projPlayVol;
      setMasterVolume(projPlayVol);
      uiForceRedraw();
      return;
    }

    // EDIT + Direction on VOL adjusts master output volume (0..100)
    if (onVolCell && (now & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT))) {
      int step = (dx != 0) ? dx : (-dy * 5);
      if (step != 0) {
        projPlayEditModUsed = true;
        projPlayVol = (uint8_t)constrain((int)projPlayVol + step, 0, (int)MASTER_VOLUME_MAX);
        songSettings().masterVolume = projPlayVol;
        setMasterVolume(projPlayVol);
        uiForceRedraw();
      }
      return;
    }

    auto toggleProjPlayTransport = [&]() {
      if (projPlayActive && transport.mode == TransportMode::Running) {
        projPlayActive = false;
        fmSynthSetExportTailPhase(false);
        storageStopTransport();
        fmSynthAllNotesOff();
        uiForceRedraw();
      } else if (projEditSongCount > 0) {
        if (projPlayPlayingIdx < 0 || transport.tapeFrame >= MAX_REC_FRAMES) {
          projPlayStartSong((projPlayPlayingIdx >= 0) ? projPlayPlayingIdx : 0, true);
        } else {
          projPlayActive = true;
          transport.speedQuarter = projPlaySpeedQuarter;
          transport.loopEnabled = false;
          transport.punchEnabled = false;
          metronomeOn = false;
          storageStartTransport(false, false, false);
          noInterrupts();
          metronomeOn = false;
          projPlayLastNow = transport.now;
          interrupts();
          uiForceRedraw();
        }
      }
    };

    // Hardware PLAY button toggles Play/Stop from anywhere on the Project Play screen
    if ((pressed & BTN_PLAY) && !(now & (BTN_SHIFT | BTN_OPT | BTN_EDIT))) {
      toggleProjPlayTransport();
      return;
    }

    if (!(now & (BTN_SHIFT | BTN_EDIT | BTN_OPT))) {
      if (dy != 0) {
        if (projPlayZone == PROJ_PLAY_ZONE_CONTROLS) {
          if (dy > 0) {
            if (projEditSongCount > 0) {
              projPlayZone = PROJ_PLAY_ZONE_LIST;
              syncProjPlayScroll();
            } else {
              projPlayZone = PROJ_PLAY_ZONE_EXIT;
            }
          }
        } else if (projPlayZone == PROJ_PLAY_ZONE_LIST) {
          if (dy < 0 && projPlayListCursor <= 0) {
            projPlayZone = PROJ_PLAY_ZONE_CONTROLS;
          } else if (dy > 0 && projPlayListCursor >= projEditSongCount - 1) {
            projPlayZone = PROJ_PLAY_ZONE_EXIT;
          } else {
            projPlayListCursor = constrain(projPlayListCursor + dy, 0, max(0, projEditSongCount - 1));
            syncProjPlayScroll();
          }
        } else if (projPlayZone == PROJ_PLAY_ZONE_EXIT) {
          if (dy < 0) {
            if (projEditSongCount > 0) {
              projPlayZone = PROJ_PLAY_ZONE_LIST;
              projPlayListCursor = constrain(projPlayListCursor, 0, projEditSongCount - 1);
              syncProjPlayScroll();
            } else {
              projPlayZone = PROJ_PLAY_ZONE_CONTROLS;
            }
          }
        }
        uiForceRedraw();
      }

      if (dx != 0) {
        if (projPlayZone == PROJ_PLAY_ZONE_CONTROLS) {
          projPlayCtrlCol = (uint8_t)constrain((int)projPlayCtrlCol + dx, 0, PROJ_PLAY_CTRL_COUNT - 1);
          uiForceRedraw();
        } else if (projPlayZone == PROJ_PLAY_ZONE_LIST && dx > 0) {
          projPlayZone = PROJ_PLAY_ZONE_EXIT;
          uiForceRedraw();
        } else if (projPlayZone == PROJ_PLAY_ZONE_EXIT && dx < 0) {
          projPlayZone = (projEditSongCount > 0) ? PROJ_PLAY_ZONE_LIST : PROJ_PLAY_ZONE_CONTROLS;
          uiForceRedraw();
        }
      }
    }

    // On Play/Stop cell, trigger toggle on EDIT release (when not used as a modifier for EDIT+L/R or OPT+EDIT)
    if (onPlayCell && editJustReleased && !projPlayEditModUsed && !(now & (BTN_SHIFT | BTN_OPT))) {
      toggleProjPlayTransport();
      return;
    }

    if (!onPlayCell && (pressed & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT))) {
      if (projPlayZone == PROJ_PLAY_ZONE_CONTROLS) {
        if (projPlayCtrlCol == PROJ_PLAY_CTRL_PREV && projEditSongCount > 0) {
          int prevIdx = (projPlayPlayingIdx > 0) ? (projPlayPlayingIdx - 1) : 0;
          projPlayListCursor = prevIdx;
          syncProjPlayScroll();
          projPlayStartSong(prevIdx, true);
        } else if (projPlayCtrlCol == PROJ_PLAY_CTRL_NEXT && projEditSongCount > 0) {
          int nextIdx = (projPlayPlayingIdx >= 0 && projPlayPlayingIdx + 1 < projEditSongCount)
                          ? (projPlayPlayingIdx + 1) : 0;
          projPlayListCursor = nextIdx;
          syncProjPlayScroll();
          projPlayStartSong(nextIdx, true);
        }
      } else if (projPlayZone == PROJ_PLAY_ZONE_LIST) {
        if (projEditSongCount > 0 && projPlayListCursor >= 0 && projPlayListCursor < projEditSongCount) {
          projPlayStartSong(projPlayListCursor, true);
        }
        inputSwallowModalKeys();
      } else if (projPlayZone == PROJ_PLAY_ZONE_EXIT) {
        projPlayStopAndRestoreLoadedSong();
        closeSysProjectsModal();
        setPage(Page::Audio);
        inputSwallowModalKeys();
        uiForceRedraw();
      }
    }
    return;
  }
}

// ============================================================================
// MIDI Mappings Modal (Global SYSTEM.DAT)
// ============================================================================
FLASHMEM static uint8_t activeMidiMapClrConfirm() {
  if (midiMapClrConfirmMode == 0) return 0;
  if ((int32_t)(midiMapClrConfirmUntilMs - millis()) <= 0) {
    midiMapClrConfirmMode = 0;
    return 0;
  }
  return midiMapClrConfirmMode;
}

FLASHMEM static void syncMidiMapScroll(int catCount) {
  midiMapCursor = constrain(midiMapCursor, 0, max(0, catCount - 1));
  if (midiMapCursor < midiMapScroll) {
    midiMapScroll = midiMapCursor;
  } else if (midiMapCursor >= midiMapScroll + MIDI_MAP_VISIBLE_ROWS) {
    midiMapScroll = midiMapCursor - MIDI_MAP_VISIBLE_ROWS + 1;
  }
  midiMapScroll = constrain(midiMapScroll, 0, max(0, catCount - MIDI_MAP_VISIBLE_ROWS));
}

FLASHMEM bool isSysMidiMapModalOpen() {
  return midiMapModalOpen;
}

FLASHMEM void openSysMidiMapModal() {
  midiMapCancelLearn();
  midiMapModalOpen = true;
  midiMapZone = MIDI_MAP_ZONE_LIST;
  midiMapClrCol = 0;
  midiMapClrConfirmMode = 0;
  int firstTarget = 0, catCount = 0;
  midiMapCategoryBounds((MidiMapCategory)midiMapCat, firstTarget, catCount);
  syncMidiMapScroll(catCount);
  midiMapEditHeld = false;
  midiMapEditModUsed = false;
  invalidateSysMidiMapModalCache();
  uiForceRedraw();
}

FLASHMEM void closeSysMidiMapModal() {
  if (!midiMapModalOpen) return;
  midiMapCancelLearn();
  midiMapFlushSaveIfDirty();
  midiMapClrConfirmMode = 0;
  midiMapModalOpen = false;
  midiMapEditHeld = false;
  midiMapEditModUsed = false;
  invalidateSysMidiMapModalCache();
  memset(prevSysSettingsKey, 0, sizeof(prevSysSettingsKey));
  uiForceRedraw();
}

FLASHMEM static void drawSysMidiMapModal(bool full) {
  int firstTarget = 0, catCount = 0;
  midiMapCategoryBounds((MidiMapCategory)midiMapCat, firstTarget, catCount);
  syncMidiMapScroll(catCount);
  const uint8_t confirmMode = activeMidiMapClrConfirm();

  // 1. Top Control Banner: [MIDI MAP]   [CAT: ...]
  {
    const char *catLbl = midiMapCategoryLabel((MidiMapCategory)midiMapCat);
    const bool inCatRow = (midiMapZone == MIDI_MAP_ZONE_CAT);
    uint64_t bk = hashStr(hashInit(), catLbl);
    bk = hashMix(bk, (uint64_t)midiMapCat |
                     ((uint64_t)inCatRow << 8) |
                     ((uint64_t)ACCENT << 16));
    if (full || bk != prevMidiMapBannerKey) {
      prevMidiMapBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, inCatRow ? ACCENT : MID);
      s.drawText(SX(6), TY(bh), "MIDI MAP", ACCENT);

      const int py = max(1, (bh - SY(16)) / 2);
      const int ph = SY(16);
      s.drawPill(SX(154), py, SX(149), ph, catLbl,
                 inCatRow ? BG : INK,
                 inCatRow ? ACCENT : PANEL,
                 inCatRow ? INK : ACCENT);

      s.blit(SX(6), SY(26));
    }
  }

  // 2. Clear Action Row: [CLR CAT]   [CLR ALL]
  {
    const bool inClrRow = (midiMapZone == MIDI_MAP_ZONE_CLR);
    uint64_t ck = hashMix(hashInit(),
                          (uint64_t)inClrRow |
                          ((uint64_t)(midiMapClrCol & 1) << 4) |
                          ((uint64_t)confirmMode << 8) |
                          ((uint64_t)ACCENT << 16));
    if (full || ck != prevMidiMapClrRowKey) {
      prevMidiMapClrRowKey = ck;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, inClrRow ? ACCENT : MID);

      const int py = max(1, (bh - SY(16)) / 2);
      const int ph = SY(16);
      const int btnX[2] = {SX(6), SX(158)};
      const int btnW[2] = {SX(144), SX(144)};

      for (int b = 0; b < 2; b++) {
        const bool bSel = inClrRow && (midiMapClrCol == b);
        const bool isConfirm = (confirmMode == (uint8_t)(b + 1));
        const char *lbl = isConfirm ? "SURE?" : ((b == 0) ? "CLR CAT" : "CLR ALL");

        uint16_t pBg = bSel ? (isConfirm ? COLOR_REC : COLOR_WARN)
                            : (isConfirm ? COLOR_REC_DIM : PANEL);
        uint16_t pFg = bSel ? BG : (isConfirm ? COLOR_REC : LIGHT);
        uint16_t pBd = bSel ? INK : (isConfirm ? COLOR_REC : MID);
        s.drawPill(btnX[b], py, btnW[b], ph, lbl, pFg, pBg, pBd);
      }

      s.blit(SX(6), SY(50));
    }
  }

  // 3. Scrollable 6-Row MIDI Mapping Table
  for (int vis = 0; vis < MIDI_MAP_VISIBLE_ROWS; vis++) {
    const int idx = midiMapScroll + vis;
    const int y = SY(74) + vis * SY(26);
    const int rw = SX(308), rh = SY(24);

    if (idx >= catCount) {
      uint64_t rk = hashMix(hashInit(), (uint64_t)(vis + 1) | 0x80000000ULL);
      if (full || rk != prevMidiMapRowKey[vis]) {
        prevMidiMapRowKey[vis] = rk;
        Surface s(rw, rh);
        s.drawBox(PANEL, MID);
        s.fillRect(PX, PX, SX(3), rh - 2 * PX, DIM);
        s.blit(SX(6), y);
      }
      keepStreaming();
      continue;
    }

    const int targetId = firstTarget + idx;
    const bool rowSel = (midiMapZone == MIDI_MAP_ZONE_LIST && midiMapCursor == idx);
    const MidiCcMapEntry eff = midiMapEffective(targetId);
    const bool isLearningRow = midiMapIsLearning() && (midiMapLearningTarget() == targetId);

    uint64_t rk = hashMix(hashInit(),
                          (uint64_t)targetId |
                          ((uint64_t)rowSel << 8) |
                          ((uint64_t)(midiMapCol & 3) << 12) |
                          ((uint64_t)isLearningRow << 20) |
                          ((uint64_t)eff.channel << 24) |
                          ((uint64_t)eff.cc << 32));
    rk = hashMix(rk, (uint64_t)ACCENT);

    if (full || rk != prevMidiMapRowKey[vis]) {
      prevMidiMapRowKey[vis] = rk;
      Surface s(rw, rh);
      const bool activeMap = (eff.channel != MIDI_MAP_CH_OFF);
      const uint16_t bg = rowSel ? DARK : PANEL;
      const uint16_t border = rowSel ? ACCENT : MID;
      const uint16_t barCol = isLearningRow ? COLOR_REC
                            : rowSel        ? ACCENT
                            : activeMap     ? LIGHT : DIM;
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      char nameBuf[28];
      midiMapFormatTargetName(targetId, nameBuf, sizeof(nameBuf));
      uint16_t nameFg = rowSel ? INK : (activeMap ? LIGHT : DIM);
      s.drawText(SX(8), TY(rh), nameBuf, nameFg);

      // 4 Columns on the right: [CH] [CC] [LRN] [CLR]
      char chStr[10];
      if (eff.channel == MIDI_MAP_CH_OFF) {
        snprintf(chStr, sizeof(chStr), "OFF");
      } else if (eff.channel == MIDI_MAP_CH_ALL) {
        snprintf(chStr, sizeof(chStr), "ALL");
      } else {
        snprintf(chStr, sizeof(chStr), "C%02u", (unsigned)eff.channel);
      }

      char ccStr[10];
      if (!activeMap) {
        snprintf(ccStr, sizeof(ccStr), "---");
      } else {
        snprintf(ccStr, sizeof(ccStr), "CC%03u", (unsigned)eff.cc);
      }

      const char *lrnStr = isLearningRow ? "..." : "LRN";
      const char *clrStr = "CLR";
      const char *colTexts[4] = {chStr, ccStr, lrnStr, clrStr};

      const int colX[4] = {SX(154), SX(189), SX(240), SX(273)};
      const int colW[4] = {SX(32),  SX(48),  SX(30),  SX(30)};
      const int py = max(1, (rh - SY(16)) / 2);
      const int ph = SY(16);

      for (int c = 0; c < 4; c++) {
        bool cSel = (rowSel && midiMapCol == c);
        uint16_t pBg = PANEL;
        uint16_t pFg = activeMap ? LIGHT : DIM;
        uint16_t pBd = MID;

        if (c == 2 && isLearningRow) {
          pBg = COLOR_REC;
          pFg = INK;
          pBd = INK;
        } else if (cSel) {
          uint16_t selCol = (c == 3) ? COLOR_WARN
                          : (c == 2) ? COLOR_PLAY
                                     : ACCENT;
          pBg = selCol;
          pFg = BG;
          pBd = INK;
        } else if (c >= 2) {
          pFg = rowSel ? LIGHT : DIM;
        }

        s.drawPill(colX[c], py, colW[c], ph, colTexts[c], pFg, pBg, pBd);
      }

      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM void handleSysMidiMapModalInput(uint8_t pressed, uint8_t now, uint32_t ms) {
  (void)ms;
  if (!midiMapModalOpen) return;

  constexpr uint8_t DIR_MASK = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
  uint8_t dir = pressed & DIR_MASK;
  int dx = ((dir & BTN_RIGHT) ? 1 : 0) - ((dir & BTN_LEFT) ? 1 : 0);
  int dy = ((dir & BTN_DOWN) ? 1 : 0) - ((dir & BTN_UP) ? 1 : 0);

  if (pressed & BTN_EDIT) {
    midiMapEditHeld = true;
    midiMapEditModUsed = false;
  }
  bool editJustReleased = midiMapEditHeld && !(now & BTN_EDIT);
  if (!(now & BTN_EDIT)) {
    midiMapEditHeld = false;
  }

  int firstTarget = 0, catCount = 0;
  midiMapCategoryBounds((MidiMapCategory)midiMapCat, firstTarget, catCount);
  syncMidiMapScroll(catCount);
  int targetId = firstTarget + midiMapCursor;

  // SHIFT + DOWN: Navigate away from the page (to lastRow2Page)
  if ((pressed & BTN_DOWN) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT))) {
    midiMapCancelLearn();
    midiMapFlushSaveIfDirty();
    midiMapClrConfirmMode = 0;
    setPage(lastRow2Page());
    inputSwallowModalKeys();
    return;
  }

  // OPT + EDIT: Reset current row mapping to OFF
  if (midiMapZone == MIDI_MAP_ZONE_LIST &&
      (((pressed & BTN_EDIT) && (now & BTN_OPT)) || ((pressed & BTN_OPT) && (now & BTN_EDIT)))) {
    midiMapEditModUsed = true;
    midiMapCancelLearn();
    midiMapClrConfirmMode = 0;
    midiMapClearEntry(targetId);
    uiShowToast("MAPPING CLEARED");
    uiForceRedraw();
    return;
  }

  // OPT or SHIFT + LEFT: Cancel active Learn / SURE? confirm first, or close modal
  if (((pressed & BTN_OPT) && !(now & BTN_EDIT)) ||
      ((pressed & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT)))) {
    if (midiMapIsLearning()) {
      midiMapCancelLearn();
      uiShowToast("LEARN CANCELLED");
      uiForceRedraw();
      return;
    }
    if (activeMidiMapClrConfirm() != 0) {
      midiMapClrConfirmMode = 0;
      uiShowToast("", 0);
      uiForceRedraw();
      return;
    }
    closeSysMidiMapModal();
    inputSwallowModalKeys();
    return;
  }

  // EDIT + Direction: Adjust Category in Header, or Channel/CC in Table Row
  if ((now & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT)) && (dx != 0 || dy != 0)) {
    midiMapEditModUsed = true;
    midiMapCancelLearn();
    midiMapClrConfirmMode = 0;
    int delta = (dx != 0) ? dx : -dy;
    if (midiMapZone == MIDI_MAP_ZONE_CAT) {
      midiMapCat = (uint8_t)((midiMapCat + delta + MIDI_CAT_COUNT) % MIDI_CAT_COUNT);
      midiMapCursor = 0;
      midiMapScroll = 0;
    } else if (midiMapZone == MIDI_MAP_ZONE_LIST) {
      if (midiMapCol == 0) {
        midiMapStepChannel(targetId, delta);
      } else if (midiMapCol == 1) {
        int ccStep = (dx != 0) ? dx : (-dy * 10);
        midiMapStepCc(targetId, ccStep);
      }
    }
    uiForceRedraw();
    return;
  }

  // Plain Directional Navigation
  if (!(now & (BTN_SHIFT | BTN_OPT | BTN_EDIT))) {
    if (dy != 0) {
      midiMapCancelLearn();
      midiMapClrConfirmMode = 0;
      if (midiMapZone == MIDI_MAP_ZONE_CAT) {
        if (dy > 0) {
          midiMapZone = MIDI_MAP_ZONE_CLR;
        }
      } else if (midiMapZone == MIDI_MAP_ZONE_CLR) {
        if (dy < 0) {
          midiMapZone = MIDI_MAP_ZONE_CAT;
        } else if (dy > 0 && catCount > 0) {
          midiMapZone = MIDI_MAP_ZONE_LIST;
          midiMapCursor = 0;
          midiMapScroll = 0;
        }
      } else {
        if (dy < 0 && midiMapCursor <= 0) {
          midiMapZone = MIDI_MAP_ZONE_CLR;
        } else {
          midiMapCursor = constrain(midiMapCursor + dy, 0, max(0, catCount - 1));
          syncMidiMapScroll(catCount);
        }
      }
      uiForceRedraw();
    }
    if (dx != 0) {
      midiMapClrConfirmMode = 0;
      if (midiMapZone == MIDI_MAP_ZONE_CLR) {
        midiMapClrCol = constrain(midiMapClrCol + dx, 0, 1);
      } else if (midiMapZone == MIDI_MAP_ZONE_LIST) {
        midiMapCol = constrain(midiMapCol + dx, 0, 3);
      }
      uiForceRedraw();
    }
  }

  // Immediate EDIT press on [CLR CAT] / [CLR ALL] (Zone 1) or [LRN] / [CLR] (Zone 2)
  if ((pressed & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT))) {
    if (midiMapZone == MIDI_MAP_ZONE_CLR) {
      midiMapEditModUsed = true;
      midiMapCancelLearn();
      uint8_t curConfirm = activeMidiMapClrConfirm();
      if (midiMapClrCol == 0) {
        if (curConfirm == 1) {
          midiMapClrConfirmMode = 0;
          midiMapClearCategory((MidiMapCategory)midiMapCat);
          uiShowToast("CATEGORY CLEARED");
        } else {
          midiMapClrConfirmMode = 1;
          midiMapClrConfirmUntilMs = millis() + 3000u;
          uiShowToast("CLR CATEGORY? EDIT AGAIN", 3000);
        }
      } else {
        if (curConfirm == 2) {
          midiMapClrConfirmMode = 0;
          midiMapClearAll();
          uiShowToast("ALL MAPPINGS CLEARED");
        } else {
          midiMapClrConfirmMode = 2;
          midiMapClrConfirmUntilMs = millis() + 3000u;
          uiShowToast("CLR ALL MAPS? EDIT AGAIN", 3000);
        }
      }
      uiForceRedraw();
      return;
    }

    if (midiMapZone == MIDI_MAP_ZONE_LIST) {
      if (midiMapCol == 2) {
        midiMapEditModUsed = true;
        midiMapClrConfirmMode = 0;
        if (midiMapIsLearning() && midiMapLearningTarget() == targetId) {
          midiMapCancelLearn();
          uiShowToast("LEARN CANCELLED");
        } else {
          midiMapStartLearn(targetId);
          uiShowToast("SEND MIDI CC...");
        }
        uiForceRedraw();
        return;
      }
      if (midiMapCol == 3) {
        midiMapEditModUsed = true;
        midiMapCancelLearn();
        midiMapClrConfirmMode = 0;
        midiMapClearEntry(targetId);
        uiShowToast("MAPPING CLEARED");
        uiForceRedraw();
        return;
      }
    }
  }

  // EDIT release (when not used with arrows) cycles Category pill or Channel pill
  if (editJustReleased && !midiMapEditModUsed && !(now & (BTN_SHIFT | BTN_OPT))) {
    midiMapCancelLearn();
    midiMapClrConfirmMode = 0;
    if (midiMapZone == MIDI_MAP_ZONE_CAT) {
      midiMapCat = (uint8_t)((midiMapCat + 1) % MIDI_CAT_COUNT);
      midiMapCursor = 0;
      midiMapScroll = 0;
      uiForceRedraw();
    } else if (midiMapZone == MIDI_MAP_ZONE_LIST && midiMapCol == 0) {
      midiMapStepChannel(targetId, 1);
      uiForceRedraw();
    }
  }
}

FLASHMEM static void sysSettingsDrawPage(bool full) {
  if (isSaveKeyboardOpen()) {
    drawSaveKeyboardModal(full);
    return;
  }
  if (isSysPresetsModalOpen()) {
    drawSysPresetsModal(full);
    return;
  }
  if (projModalView == ProjModalView::ManageList) {
    drawProjManageListModal(full);
    return;
  }
  if (projModalView == ProjModalView::EditProject) {
    drawProjEditModal(full);
    return;
  }
  if (projModalView == ProjModalView::AddSong) {
    drawProjAddSongModal(full);
    return;
  }
  if (projModalView == ProjModalView::PlayProject) {
    drawProjPlayModal(full);
    return;
  }
  if (midiMapModalOpen) {
    drawSysMidiMapModal(full);
    return;
  }

  sysSetRow = constrain(sysSetRow, 0, SSET_COUNT - 1);
  const SystemSettings &ss = systemSettings();
  PROGMEM static const char *const LABELS[SSET_COUNT] = {
    "SCREEN BRIGHTNESS",
    "METRONOME VOLUME",
    "INSTRUMENT PRESETS",
    "PROJECTS",
    "MIDI MAPPINGS",
    "USB STORAGE DRIVE"
  };

  for (int i = 0; i < SSET_COUNT; i++) {
    int y = SY(28) + i * SY(26);
    bool sel = (sysSetRow == i);

    char val[32];
    val[0] = '\0';
    bool isOff = false;
    bool isPill = false;

    switch (i) {
      case SSET_BRIGHTNESS:
        snprintf(val, sizeof(val), "%d", (int)ss.brightness);
        break;
      case SSET_METRO_VOL:
        isOff = (ss.metroVolume == 0);
        snprintf(val, sizeof(val), "%d", (int)ss.metroVolume);
        break;
      case SSET_INST_PRESETS:
      case SSET_PROJECTS:
      case SSET_MIDI_MAP:
        isPill = true;
        snprintf(val, sizeof(val), "MANAGE");
        break;
      case SSET_USB_DRIVE:
        isPill = true;
        snprintf(val, sizeof(val), "MOUNT SD");
        break;
    }

    drawSettingsTableRow(y, (uint32_t)i, LABELS[i], val, sel, isOff, isPill,
                         prevSysSettingsKey[i], full);
    keepStreaming();
  }
}

FLASHMEM static void sysSettingsOnShiftNavigate(uint8_t dir) {
  if (dir & BTN_RIGHT) setPage(Page::SongSettings);
  else if (dir & BTN_DOWN) setPage(lastRow2Page());
}

FLASHMEM static void sysSettingsOnOptAdjust(int dx, int dy, int delta) {
  int step = (dx != 0) ? dx : (dy != 0 ? dy * 5 : delta * 5);
  SystemSettings &ss = systemSettings();
  switch (sysSetRow) {
    case SSET_BRIGHTNESS:
      ss.brightness = (uint8_t)constrain((int)ss.brightness + step, (int)MIN_BRIGHTNESS, (int)MAX_BRIGHTNESS);
      break;
    case SSET_METRO_VOL:
      ss.metroVolume = (uint8_t)constrain((int)ss.metroVolume + step, 0, (int)VOLUME_MAX);
      break;
    case SSET_INST_PRESETS:
    case SSET_PROJECTS:
    case SSET_MIDI_MAP:
    case SSET_USB_DRIVE:
      return;
  }
  applySystemSettings();
  saveSystemSettings();
  uiForceRedraw();
}

FLASHMEM static void sysSettingsOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dx;
  (void)dir;
  if (dy != 0) {
    sysSetRow = constrain(sysSetRow + dy, 0, SSET_COUNT - 1);
    uiForceRedraw();
  }
}

FLASHMEM void usbSavePromptModalConfirm() {
  if (!usbSavePromptOpen) return;
  bool doSave = (usbSavePromptCursor == 0);
  usbSavePromptOpen = false;
  prevUsbSavePromptKey = 0;

  // Redraw underlying System Settings page before saving / showing USB overlay
  board.tft().fillRect(0, SY(24), SCREEN_W, SCREEN_H - SY(24), BG);
  sysSettingsInvalidateCache();
  sysSettingsDrawPage(true);

  if (doSave) {
    saveCurrentSong();
  }
  if (!usbDriveMode()) {
    uiShowToast("BUILD WITH USB SERIAL+MSC");
  }
  inputSwallowModalKeys();
  inputClearRepeat();
  uiForceRedraw();
}

FLASHMEM static void sysSettingsOnEditPress(bool shiftHeld) {
  (void)shiftHeld;
  if (sysSetRow == SSET_USB_DRIVE) {
    if (transport.mode == TransportMode::Stopped) {
      if (songHasUnsavedChanges()) {
        openUsbSavePromptModal();
      } else if (!usbDriveMode()) {
        uiShowToast("BUILD WITH USB SERIAL+MSC");
      }
      inputSwallowModalKeys();
    } else {
      uiShowToast("STOP TRANSPORT FIRST");
    }
    inputClearRepeat();
    uiForceRedraw();
  } else if (sysSetRow == SSET_INST_PRESETS) {
    openSysPresetsModal();
    inputSwallowModalKeys();
    uiForceRedraw();
  } else if (sysSetRow == SSET_PROJECTS) {
    openSysProjectsModal();
    inputSwallowModalKeys();
    uiForceRedraw();
  } else if (sysSetRow == SSET_MIDI_MAP) {
    openSysMidiMapModal();
    inputSwallowModalKeys();
    uiForceRedraw();
  }
}

FLASHMEM static void sysSettingsOnResetParam() {
  SystemSettings &ss = systemSettings();
  if (sysSetRow == SSET_BRIGHTNESS) {
    ss.brightness = DEFAULT_BRIGHTNESS;
  } else if (sysSetRow == SSET_METRO_VOL) {
    ss.metroVolume = DEFAULT_METRO_VOLUME;
  } else {
    return;
  }
  applySystemSettings();
  saveSystemSettings();
  uiForceRedraw();
}

PROGMEM static const PageOps SYS_SETTINGS_PAGE_OPS = {
  Page::SystemSettings,
  sysSettingsHeaderTitle,
  nullptr,
  sysSettingsInvalidateCache,
  sysSettingsDrawPage,
  sysSettingsOnShiftNavigate,
  sysSettingsOnCursorMove,
  sysSettingsOnOptAdjust,
  sysSettingsOnEditPress,
  sysSettingsOnResetParam
};

FLASHMEM const PageOps &pageSystemSettingsOps() {
  return SYS_SETTINGS_PAGE_OPS;
}

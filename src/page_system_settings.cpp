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

// Playlists Modal State (/Capstan/PLAYLISTS/*.plst)
enum class PlaylistModalView : uint8_t {
  Closed       = 0,
  ManageList   = 1,
  EditPlaylist = 2,
  AddSong      = 3,
  PlayPlaylist = 4
};

enum PlaylistManageAction : int {
  PLAYLIST_ACT_PLAY   = 0,
  PLAYLIST_ACT_EDIT   = 1,
  PLAYLIST_ACT_RENAME = 2,
  PLAYLIST_ACT_DELETE = 3,
  PLAYLIST_ACT_COUNT  = 4
};

enum PlaylistEditRowAction : int {
  PLAYLIST_EDIT_ACT_ADD    = 0,
  PLAYLIST_EDIT_ACT_REMOVE = 1,
  PLAYLIST_EDIT_ACT_COUNT  = 2
};

enum PlaylistPlayZone : uint8_t {
  PLAYLIST_PLAY_ZONE_CONTROLS = 0,
  PLAYLIST_PLAY_ZONE_LIST     = 1,
  PLAYLIST_PLAY_ZONE_EXIT     = 2
};

enum PlaylistPlayCtrlCol : uint8_t {
  PLAYLIST_PLAY_CTRL_PREV  = 0,
  PLAYLIST_PLAY_CTRL_PLAY  = 1,
  PLAYLIST_PLAY_CTRL_NEXT  = 2,
  PLAYLIST_PLAY_CTRL_VOL   = 3,
  PLAYLIST_PLAY_CTRL_COUNT = 4
};

static constexpr int PLAYLIST_MANAGE_VISIBLE_ROWS = 7;
static constexpr int PLAYLIST_EDIT_VISIBLE_ROWS   = 6;
static constexpr int PLAYLIST_ADD_VISIBLE_ROWS    = 7;
static constexpr int PLAYLIST_PLAY_VISIBLE_ROWS   = 5;

static PlaylistModalView playlistModalView = PlaylistModalView::Closed;
static int playlistManageCursor = 0;
static int playlistManageScroll = 0;
static int playlistManageCol = PLAYLIST_ACT_PLAY;

enum class PlaylistConfirmKind : uint8_t {
  DeletePlaylist = 0,
  RemoveSong     = 1,
  SaveBeforePlay = 2
};
static bool playlistConfirmOpen = false;
static PlaylistConfirmKind playlistConfirmKind = PlaylistConfirmKind::DeletePlaylist;
static int playlistConfirmCursor = 0; // 0 = YES, 1 = NO

static char playlistEditingName[MAX_PLAYLIST_NAME_LEN + 1] = "";
static char (*const playlistEditSongs)[MAX_SONG_NAME_LEN + 1] =
    reinterpret_cast<char (*)[MAX_SONG_NAME_LEN + 1]>(sharedBrowserPool);
static_assert(MAX_SONGS_PER_PLAYLIST * (MAX_SONG_NAME_LEN + 1) <= 2048, "playlistEditSongs must fit in sharedBrowserPool");
static int playlistEditSongCount = 0;
static int playlistEditCursor = 0;
static int playlistEditScroll = 0;
static int playlistEditCol = PLAYLIST_EDIT_ACT_ADD;
static bool playlistEditInBottomRow = false;
static int playlistEditBottomCol = 0; // 0 = SAVE, 1 = CANCEL

static int playlistAddSongCursor = 0;
static int playlistAddSongScroll = 0;

// Playlist Play state
static int playlistPlayPlayingIdx = -1;
static int playlistPlayListCursor = 0;
static int playlistPlayListScroll = 0;
static uint8_t playlistPlayZone = PLAYLIST_PLAY_ZONE_CONTROLS;
static uint8_t playlistPlayCtrlCol = PLAYLIST_PLAY_CTRL_PLAY;
static uint8_t playlistPlayVol = MASTER_VOLUME_SPK_DEFAULT;
static uint8_t playlistPlayVolHp = MASTER_VOLUME_HP_DEFAULT;
static uint8_t playlistPlayVolSpk = MASTER_VOLUME_SPK_DEFAULT;
static int8_t playlistPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
static bool playlistPlayLocked = false;
static bool playlistPlayEditHeld = false;
static bool playlistPlayEditModUsed = false;
static bool playlistPlayActive = false;
static bool playlistPlayTailPhase = false;
static uint32_t playlistPlayMaxDataFrame = 0;
static uint32_t playlistPlaySilentFrames = 0;
static uint32_t playlistPlayLastNow = 0;
static uint32_t playlistPlayAnimMs = 0;
static uint32_t playlistPlayLastTickMs = 0;

static uint64_t prevPlaylistBannerKey = 0;
static uint64_t prevPlaylistTapeKey = 0;
static uint64_t prevPlaylistTapeStaticKey = 0;
static uint64_t prevPlaylistLockKey = 0;
static uint64_t prevPlaylistCtrlKey = 0;
static uint64_t prevPlaylistRowKey[7] = {};
static uint64_t prevPlaylistBtnKey[2] = {};
static uint64_t prevPlaylistConfirmKey = 0;

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

FLASHMEM static void invalidateSysPlaylistsModalCache() {
  prevPlaylistBannerKey = 0;
  prevPlaylistTapeKey = 0;
  prevPlaylistTapeStaticKey = 0;
  prevPlaylistLockKey = 0;
  prevPlaylistCtrlKey = 0;
  memset(prevPlaylistRowKey, 0, sizeof(prevPlaylistRowKey));
  memset(prevPlaylistBtnKey, 0, sizeof(prevPlaylistBtnKey));
  prevPlaylistConfirmKey = 0;
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
  invalidateSysPlaylistsModalCache();
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
// Playlists Management Modal (/Capstan/PLAYLISTS/*.plst)
// ============================================================================
static inline int playlistManageRowCount() {
  return min(playlistCount() + 1, MAX_PLAYLISTS);
}

FLASHMEM static void syncPlaylistManageScroll() {
  int rowCnt = playlistManageRowCount();
  playlistManageCursor = constrain(playlistManageCursor, 0, rowCnt - 1);
  if (playlistManageCursor < playlistManageScroll) {
    playlistManageScroll = playlistManageCursor;
  } else if (playlistManageCursor >= playlistManageScroll + PLAYLIST_MANAGE_VISIBLE_ROWS) {
    playlistManageScroll = playlistManageCursor - PLAYLIST_MANAGE_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, rowCnt - PLAYLIST_MANAGE_VISIBLE_ROWS);
  playlistManageScroll = constrain(playlistManageScroll, 0, maxScr);
  playlistManageCol = constrain(playlistManageCol, 0, PLAYLIST_ACT_COUNT - 1);
}

static inline int playlistEditRowCount() {
  return min(playlistEditSongCount + 1, MAX_SONGS_PER_PLAYLIST);
}

FLASHMEM static void syncPlaylistEditScroll() {
  int rowCnt = playlistEditRowCount();
  playlistEditCursor = constrain(playlistEditCursor, 0, rowCnt - 1);
  if (playlistEditCursor < playlistEditScroll) {
    playlistEditScroll = playlistEditCursor;
  } else if (playlistEditCursor >= playlistEditScroll + PLAYLIST_EDIT_VISIBLE_ROWS) {
    playlistEditScroll = playlistEditCursor - PLAYLIST_EDIT_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, rowCnt - PLAYLIST_EDIT_VISIBLE_ROWS);
  playlistEditScroll = constrain(playlistEditScroll, 0, maxScr);
  if (playlistEditCursor >= playlistEditSongCount) {
    playlistEditCol = PLAYLIST_EDIT_ACT_ADD;
  } else {
    playlistEditCol = constrain(playlistEditCol, 0, PLAYLIST_EDIT_ACT_COUNT - 1);
  }
}

FLASHMEM static void syncPlaylistAddSongScroll() {
  int total = songCount();
  if (total <= 0) {
    playlistAddSongCursor = 0;
    playlistAddSongScroll = 0;
    return;
  }
  playlistAddSongCursor = constrain(playlistAddSongCursor, 0, total - 1);
  if (playlistAddSongCursor < playlistAddSongScroll) {
    playlistAddSongScroll = playlistAddSongCursor;
  } else if (playlistAddSongCursor >= playlistAddSongScroll + PLAYLIST_ADD_VISIBLE_ROWS) {
    playlistAddSongScroll = playlistAddSongCursor - PLAYLIST_ADD_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, total - PLAYLIST_ADD_VISIBLE_ROWS);
  playlistAddSongScroll = constrain(playlistAddSongScroll, 0, maxScr);
}

FLASHMEM static void syncPlaylistPlayScroll() {
  int total = playlistEditSongCount;
  if (total <= 0) {
    playlistPlayListCursor = 0;
    playlistPlayListScroll = 0;
    return;
  }
  playlistPlayListCursor = constrain(playlistPlayListCursor, 0, total - 1);
  if (playlistPlayListCursor < playlistPlayListScroll) {
    playlistPlayListScroll = playlistPlayListCursor;
  } else if (playlistPlayListCursor >= playlistPlayListScroll + PLAYLIST_PLAY_VISIBLE_ROWS) {
    playlistPlayListScroll = playlistPlayListCursor - PLAYLIST_PLAY_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, total - PLAYLIST_PLAY_VISIBLE_ROWS);
  playlistPlayListScroll = constrain(playlistPlayListScroll, 0, maxScr);
}

FLASHMEM static void playlistPlayStopAndRestoreLoadedSong() {
  playlistPlayActive = false;
  playlistPlayTailPhase = false;
  fmSynthSetExportTailPhase(false);
  storageStopTransport();
  fmSynthAllNotesOff();
  resetMasterFxState();
  transport.speedQuarter = DEFAULT_SPEED_QUARTER;
  playlistPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
  storageSeekTape(0);
  transport.loopEnabled = songSettings().loopEnabled;
  transport.punchEnabled = songSettings().punchEnabled;
  updateMetronomeState();
  syncSongListCursor(true);
}

FLASHMEM static bool playlistPlayStartSong(int startIdx, bool autoPlay) {
  playlistPlayActive = false;
  playlistPlayTailPhase = false;
  playlistPlaySilentFrames = 0;
  fmSynthSetExportTailPhase(false);
  storageStopTransport();
  fmSynthAllNotesOff();
  resetMasterFxState();

  if (playlistEditSongCount <= 0 || startIdx < 0 || startIdx >= playlistEditSongCount) {
    playlistPlayPlayingIdx = -1;
    uiForceRedraw();
    return false;
  }

  int chosenIdx = -1;
  int maxTries = autoPlay ? (playlistEditSongCount - startIdx) : 1;
  for (int attempt = 0; attempt < maxTries; attempt++) {
    int tryIdx = startIdx + attempt;
    if (loadSongByName(playlistEditSongs[tryIdx])) {
      chosenIdx = tryIdx;
      break;
    }
  }

  if (chosenIdx < 0) {
    uiShowToast("SONG NOT FOUND");
    uiForceRedraw();
    return false;
  }

  playlistPlayPlayingIdx = chosenIdx;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    tracks[t].armMode = ArmMode::Off;
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    midiTracks[t].armMode = ArmMode::Off;
  }
  songSettings().masterVolHpEnc = (uint8_t)(playlistPlayVolHp + 1);
  songSettings().masterVolSpkEnc = (uint8_t)(playlistPlayVolSpk + 1);
  playlistPlayVol = appAreHeadphonesConnected() ? playlistPlayVolHp : playlistPlayVolSpk;
  appSetMasterVolume(playlistPlayVol);

  transport.speedQuarter = playlistPlaySpeedQuarter;
  transport.loopEnabled = false;
  transport.punchEnabled = false;
  metronomeOn = false;
  storageSeekTape(0);
  playlistPlayMaxDataFrame = storageSongMaxAudibleFrame();

  float pkL = 0.0f, pkR = 0.0f;
  takeOutputPeaks(pkL, pkR);
  playlistPlayLastNow = transport.now;

  if (autoPlay) {
    playlistPlayActive = true;
    storageStartTransport(false, false, false);
    noInterrupts();
    metronomeOn = false;
    playlistPlayLastNow = transport.now;
    interrupts();
  }

  noteUserActivity();
  invalidateSysPlaylistsModalCache();
  uiForceRedraw();
  return true;
}

FLASHMEM void sysPlaylistPlayOnOutputSwitched(bool phonesConnected) {
  if (playlistModalView != PlaylistModalView::PlayPlaylist) return;
  playlistPlayVol = phonesConnected ? playlistPlayVolHp : playlistPlayVolSpk;
  prevPlaylistCtrlKey = 0;
}

FLASHMEM void sysPlaylistPlayOnMasterVolChanged(uint8_t vol) {
  if (playlistModalView != PlaylistModalView::PlayPlaylist) return;
  playlistPlayVol = vol;
  if (appAreHeadphonesConnected()) playlistPlayVolHp = vol;
  else playlistPlayVolSpk = vol;
  prevPlaylistCtrlKey = 0;
}

FLASHMEM void sysPlaylistPlayOnSpeedChanged(int8_t speedQuarter) {
  if (playlistModalView != PlaylistModalView::PlayPlaylist) return;
  playlistPlaySpeedQuarter = speedQuarter;
  prevPlaylistCtrlKey = 0;
}

FLASHMEM void sysPlaylistPlayService() {
  if (playlistModalView != PlaylistModalView::PlayPlaylist || !playlistPlayActive) return;

  if (transport.mode != TransportMode::Running) {
    const bool finishedSong = (transport.speedQuarter < 0)
                                ? (transport.tapeFrame == 0)
                                : (transport.tapeFrame >= playlistPlayMaxDataFrame);
    if (finishedSong) {
      int nextIdx = playlistPlayPlayingIdx + 1;
      if (nextIdx < playlistEditSongCount) {
        if (playlistPlayListCursor == playlistPlayPlayingIdx) {
          playlistPlayListCursor = nextIdx;
          syncPlaylistPlayScroll();
        }
        playlistPlayStartSong(nextIdx, true);
      } else {
        playlistPlayStopAndRestoreLoadedSong();
        uiForceRedraw();
      }
    } else {
      playlistPlayActive = false;
      fmSynthSetExportTailPhase(false);
      uiForceRedraw();
    }
    return;
  }

  if (transport.speedQuarter <= 0) {
    playlistPlayTailPhase = false;
    playlistPlaySilentFrames = 0;
    return;
  }

  uint32_t curNow = transport.now;
  uint32_t dtFrames = curNow - playlistPlayLastNow;
  if (dtFrames < 256u) return;
  playlistPlayLastNow = curNow;

  float pkL = 0.0f, pkR = 0.0f;
  takeOutputPeaks(pkL, pkR);

  uint32_t tf = transport.tapeFrame;
  if (tf < playlistPlayMaxDataFrame) {
    playlistPlayTailPhase = false;
    playlistPlaySilentFrames = 0;
    return;
  }

  if (!playlistPlayTailPhase) {
    playlistPlayTailPhase = true;
    playlistPlaySilentFrames = 0;
    fmSynthSetExportTailPhase(true);
  }

  const uint32_t reqSilent = storageAnyTailFxActive() ? 16384u : 2048u;

  if (pkL <= (4.5f / 32768.0f) && pkR <= (4.5f / 32768.0f)) {
    playlistPlaySilentFrames += dtFrames;
  } else {
    playlistPlaySilentFrames = 0;
  }

  const bool tailTimeout = (tf > playlistPlayMaxDataFrame) &&
                           ((tf - playlistPlayMaxDataFrame) >= 15u * SAMPLE_RATE_HZ);
  if ((playlistPlaySilentFrames >= reqSilent && !fmSynthHasActiveOutput()) || tailTimeout) {
    int nextIdx = playlistPlayPlayingIdx + 1;
    if (nextIdx < playlistEditSongCount) {
      if (playlistPlayListCursor == playlistPlayPlayingIdx) {
        playlistPlayListCursor = nextIdx;
        syncPlaylistPlayScroll();
      }
      playlistPlayStartSong(nextIdx, true);
    } else {
      playlistPlayStopAndRestoreLoadedSong();
      uiForceRedraw();
    }
  }
}

FLASHMEM bool isSysPlaylistsModalOpen() {
  return playlistModalView != PlaylistModalView::Closed;
}

FLASHMEM uint8_t sysPlaylistsModalState() {
  if (playlistModalView == PlaylistModalView::Closed) return 0;
  return (uint8_t)playlistModalView | (playlistConfirmOpen ? (0x10u | ((uint8_t)playlistConfirmKind << 5)) : 0x00u);
}

FLASHMEM bool isSysPlaylistsConfirmOpen() {
  return (playlistModalView != PlaylistModalView::Closed) && playlistConfirmOpen;
}

FLASHMEM bool isSysPlaylistPlayLocked() {
  return currentPage() == Page::SystemSettings &&
         playlistModalView == PlaylistModalView::PlayPlaylist &&
         playlistPlayLocked;
}

FLASHMEM void toggleSysPlaylistPlayLock() {
  if (currentPage() != Page::SystemSettings || playlistModalView != PlaylistModalView::PlayPlaylist) {
    return;
  }
  playlistPlayLocked = !playlistPlayLocked;
  playlistPlayEditHeld = false;
  playlistPlayEditModUsed = false;
  inputSwallowModalKeys();
  inputClearRepeat();
  uiForceRedraw();
}

FLASHMEM static void openSysPlaylistsModal() {
  scanPlaylistFiles();
  playlistModalView = PlaylistModalView::ManageList;
  playlistConfirmOpen = false;
  playlistConfirmCursor = 0;
  playlistPlayLocked = false;
  if (playlistManageCursor >= playlistCount()) {
    playlistManageCursor = max(0, playlistCount() - 1);
  }
  playlistManageCol = PLAYLIST_ACT_PLAY;
  syncPlaylistManageScroll();
  invalidateSysPlaylistsModalCache();
}

FLASHMEM void closeSysPlaylistsModal() {
  if (playlistModalView == PlaylistModalView::Closed) return;
  if (playlistModalView == PlaylistModalView::PlayPlaylist) {
    playlistPlayStopAndRestoreLoadedSong();
  }
  playlistModalView = PlaylistModalView::Closed;
  playlistConfirmOpen = false;
  playlistPlayLocked = false;
  invalidateSysPlaylistsModalCache();
}

FLASHMEM void commitPlaylistKeyboard(const char *enteredName, bool isRename) {
  char outName[MAX_PLAYLIST_NAME_LEN + 1] = {};
  bool ok = false;
  if (isRename) {
    const char *oldName = playlistNameAt(playlistManageCursor);
    ok = renamePlaylistFile(oldName, enteredName, outName, sizeof(outName));
  } else {
    ok = createEmptyPlaylistFile(enteredName, outName, sizeof(outName));
  }
  closeSaveKeyboard();
  playlistModalView = PlaylistModalView::ManageList;
  playlistConfirmOpen = false;
  if (ok && outName[0] != '\0') {
    int idx = findPlaylistIndex(outName);
    if (idx >= 0) playlistManageCursor = idx;
    playlistManageCol = PLAYLIST_ACT_EDIT;
    syncPlaylistManageScroll();
    char msg[32];
    snprintf(msg, sizeof(msg), "%s: %s", isRename ? "RENAMED" : "CREATED", outName);
    uiShowToast(msg);
  } else {
    syncPlaylistManageScroll();
    uiShowToast(isRename ? "RENAME FAILED" : "CREATE FAILED");
  }
  invalidateSysPlaylistsModalCache();
  uiForceRedraw();
}

FLASHMEM void drawSysPlaylistsConfirmModal(bool full) {
  if (!isSysPlaylistsConfirmOpen()) return;

  const int cur = constrain(playlistConfirmCursor, 0, 1);
  const int ovX = SX(12);
  const int ovY = SY(94);
  const int ovW = SX(296);
  const int ovH = SY(24);
  const uint8_t kindVal = (uint8_t)playlistConfirmKind;

  uint64_t k = hashMix(hashInit(), (uint64_t)ACCENT | ((uint64_t)cur << 16) |
                                   ((uint64_t)kindVal << 24));
  if (full || k != prevPlaylistConfirmKey) {
    prevPlaylistConfirmKey = k;
    Surface s(ovW, ovH);
    s.drawBox(DARK, ACCENT);
    const char *promptTxt = (playlistConfirmKind == PlaylistConfirmKind::SaveBeforePlay) ? "Save song?"
                          : (playlistConfirmKind == PlaylistConfirmKind::DeletePlaylist) ? "Delete playlist?"
                                                                                         : "Remove song?";
    s.drawText(SX(10), TY(ovH), promptTxt, INK);

    const char *const OPT_LABELS[2] = {"YES", "NO"};
    const int pillW = SX(42);
    const int pillH = SY(16);
    const int pillY = max(1, (ovH - pillH) / 2);
    const int pillX[2] = {SX(196), SX(244)};

    for (int i = 0; i < 2; i++) {
      bool sel = (cur == i);
      uint16_t pBg = (playlistConfirmKind == PlaylistConfirmKind::SaveBeforePlay)
                         ? (sel ? (i == 0 ? COLOR_PLAY : COLOR_WARN) : PANEL)
                         : (sel ? (i == 0 ? COLOR_WARN : ACCENT) : PANEL);
      uint16_t pFg = sel ? BG : INK;
      uint16_t pBd = sel ? INK : MID;
      s.drawPill(pillX[i], pillY, pillW, pillH, OPT_LABELS[i], pFg, pBg, pBd);
    }
    s.blit(ovX, ovY);
  }
}

FLASHMEM static void drawPlaylistManageListModal(bool full) {
  syncPlaylistManageScroll();
  const int total = playlistCount();
  const int cur = playlistManageCursor;
  const int scroll = playlistManageScroll;

  // 1. Header Banner: [PLAYLISTS]
  {
    uint64_t bk = hashStr(hashInit(), "PLAYLISTS");
    bk = hashMix(bk, (uint64_t)total);
    if (full || bk != prevPlaylistBannerKey) {
      prevPlaylistBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), "PLAYLISTS", ACCENT);
      char cntTxt[20];
      snprintf(cntTxt, sizeof(cntTxt), "%d SAVED", total);
      int tw = textWidth(cntTxt);
      s.drawText(bw - SX(6) - tw, TY(bh), cntTxt, DIM);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Playlists Table (populated playlists + next empty row to create a playlist)
  const char *const ACT_LABELS[PLAYLIST_ACT_COUNT] = {"PLAY", "EDIT", "RENAME", "DEL"};
  const int pillW[PLAYLIST_ACT_COUNT] = {SX(36), SX(36), SX(58), SX(32)};

  for (int r = 0; r < PLAYLIST_MANAGE_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const bool hasPlaylist = (idx < total);
    const bool isNextEmpty = (idx == total && idx < MAX_PLAYLISTS);
    const bool isValidRow = hasPlaylist || isNextEmpty;
    const bool sel = isValidRow && (idx == cur);
    const int activeCol = sel ? (hasPlaylist ? playlistManageCol : 0) : -1;

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = sel ? ACCENT : (hasPlaylist ? COLOR_PLAY : DIM);

    const char *nameTxt = hasPlaylist ? playlistNameAt(idx)
                        : isNextEmpty ? ((total == 0) ? "NO SAVED PLAYLISTS" : "---")
                                      : "";

    uint64_t rk = hashStr(hashInit(), nameTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)(activeCol + 2) << 48) | ((uint64_t)hasPlaylist << 54) |
                     ((uint64_t)isNextEmpty << 55) | ((uint64_t)r << 56));
    if (full || rk != prevPlaylistRowKey[r]) {
      prevPlaylistRowKey[r] = rk;
      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      int pillX[PLAYLIST_ACT_COUNT] = {};
      if (sel && hasPlaylist) {
        int curRight = rw - SX(5);
        for (int c = PLAYLIST_ACT_COUNT - 1; c >= 0; c--) {
          curRight -= pillW[c];
          pillX[c] = curRight;
          curRight -= SX(3);
        }
      }

      if (nameTxt[0] != '\0') {
        char dispName[32];
        snprintf(dispName, sizeof(dispName), "%s", nameTxt);
        if (sel && hasPlaylist) {
          int maxTxtW = max(SX(40), pillX[0] - SX(12));
          while (textWidth(dispName) > maxTxtW && strlen(dispName) > 3) {
            dispName[strlen(dispName) - 1] = '\0';
          }
        }
        uint16_t fg = !hasPlaylist ? ((sel && total > 0) ? LIGHT : DIM) : (sel ? INK : ACCENT);
        s.drawText(SX(8), TY(rh), dispName, fg);
      }

      if (sel) {
        const int ph = SY(16);
        const int py = (rh - ph) / 2;
        if (!hasPlaylist) {
          int pw = SX(42);
          int px = rw - SX(6) - pw;
          s.drawPill(px, py, pw, ph, "NEW", BG, ACCENT, INK);
        } else {
          for (int c = 0; c < PLAYLIST_ACT_COUNT; c++) {
            bool cSel = (activeCol == c);
            uint16_t pBg = cSel ? ((c == PLAYLIST_ACT_DELETE) ? COLOR_WARN
                                 : (c == PLAYLIST_ACT_PLAY)   ? COLOR_PLAY : ACCENT) : PANEL;
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

FLASHMEM static void drawPlaylistEditModal(bool full) {
  syncPlaylistEditScroll();
  const int total = playlistEditSongCount;
  const int cur = playlistEditCursor;
  const int scroll = playlistEditScroll;

  // 1. Header Banner: [PLAYLIST: <name>]
  {
    char leftTxt[32];
    snprintf(leftTxt, sizeof(leftTxt), "PLAYLIST: %s", playlistEditingName);
    uint64_t bk = hashStr(hashInit(), leftTxt);
    bk = hashMix(bk, (uint64_t)total);
    if (full || bk != prevPlaylistBannerKey) {
      prevPlaylistBannerKey = bk;
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
  for (int r = 0; r < PLAYLIST_EDIT_VISIBLE_ROWS; r++) {
    const int y = SY(48) + r * SY(24);
    const int rw = SX(308), rh = SY(22);
    const int idx = scroll + r;
    const bool hasSong = (idx < total);
    const bool isNextEmpty = (idx == total && idx < MAX_SONGS_PER_PLAYLIST);
    const bool isValidRow = hasSong || isNextEmpty;
    const bool sel = !playlistEditInBottomRow && isValidRow && (idx == cur);
    const int activeCol = sel ? (hasSong ? playlistEditCol : PLAYLIST_EDIT_ACT_ADD) : -1;

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = sel ? ACCENT : (hasSong ? COLOR_PLAY : DIM);

    char leftTxt[32];
    if (hasSong) {
      snprintf(leftTxt, sizeof(leftTxt), "%02d  %s", idx + 1, playlistEditSongs[idx]);
    } else if (isNextEmpty) {
      snprintf(leftTxt, sizeof(leftTxt), "%02d  ---", idx + 1);
    } else {
      leftTxt[0] = '\0';
    }

    uint64_t rk = hashStr(hashInit(), leftTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)(activeCol + 2) << 48) | ((uint64_t)hasSong << 54) |
                     ((uint64_t)isNextEmpty << 55) | ((uint64_t)r << 56));
    if (full || rk != prevPlaylistRowKey[r]) {
      prevPlaylistRowKey[r] = rk;
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
            uint16_t pBg = cSel ? ((c == PLAYLIST_EDIT_ACT_REMOVE) ? COLOR_WARN : ACCENT) : PANEL;
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
    bool sel = (playlistEditInBottomRow && playlistEditBottomCol == b);
    uint16_t bg = sel ? (b == 0 ? COLOR_PLAY : COLOR_WARN) : PANEL;
    uint16_t fg = sel ? BG : (b == 0 ? COLOR_PLAY : LIGHT);
    uint16_t border = sel ? INK : (b == 0 ? COLOR_PLAY : MID);

    uint64_t bk = hashStr(hashInit(), btnTitle);
    bk = hashMix(bk, (uint64_t)bg | ((uint64_t)fg << 16) | ((uint64_t)border << 32));
    if (full || bk != prevPlaylistBtnKey[b]) {
      prevPlaylistBtnKey[b] = bk;
      Surface s(w, h);
      s.drawBox(bg, border);
      int tw = textWidth(btnTitle);
      s.drawText(max(4, (w - tw) / 2), TY(h), btnTitle, fg);
      s.blit(x, SY(196));
    }
  }
}

FLASHMEM static void drawPlaylistAddSongModal(bool full) {
  syncPlaylistAddSongScroll();
  const int total = songCount();
  const int cur = playlistAddSongCursor;
  const int scroll = playlistAddSongScroll;

  // 1. Header Banner: [ADD SONG TO PLAYLIST]
  {
    uint64_t bk = hashStr(hashInit(), "ADD SONG TO PLAYLIST");
    bk = hashMix(bk, (uint64_t)total);
    if (full || bk != prevPlaylistBannerKey) {
      prevPlaylistBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), "ADD SONG TO PLAYLIST", ACCENT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Songs Table (/Capstan/SONGS/)
  for (int r = 0; r < PLAYLIST_ADD_VISIBLE_ROWS; r++) {
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
    if (full || rk != prevPlaylistRowKey[r]) {
      prevPlaylistRowKey[r] = rk;
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

FLASHMEM static void drawPlaylistPlayModal(bool full) {
  syncPlaylistPlayScroll();
  const int total = playlistEditSongCount;
  const int cur = playlistPlayListCursor;
  const int scroll = playlistPlayListScroll;
  const bool isPlaying = playlistPlayActive && (transport.mode == TransportMode::Running);

  // 1. Startup Tape Graphic (scaled larger to SY(114) tall, with playlist name inside)
  {
    uint32_t nowMs = millis();
    if (isPlaying) {
      if (nowMs - playlistPlayLastTickMs >= 100) {
        int32_t dt = (int32_t)(nowMs - playlistPlayLastTickMs);
        playlistPlayAnimMs = (uint32_t)((int32_t)playlistPlayAnimMs + (dt * (int32_t)transport.speedQuarter) / 4);
        playlistPlayLastTickMs = nowMs;
      }
    } else {
      playlistPlayLastTickMs = nowMs;
    }
    uint16_t tapeCol = isPlaying ? LOGO_COLOR : blend565(LOGO_COLOR, DIM, 170);
    uint64_t staticKey = hashStr(hashInit(), playlistEditingName);
    staticKey = hashMix(staticKey, (uint64_t)tapeCol | ((uint64_t)isPlaying << 16));
    uint64_t tk = hashMix(staticKey, (uint64_t)playlistPlayAnimMs);
    if (full || tk != prevPlaylistTapeKey) {
      bool reelsOnly = (!full && staticKey == prevPlaylistTapeStaticKey);
      prevPlaylistTapeKey = tk;
      prevPlaylistTapeStaticKey = staticKey;
      const int deckH = SY(114);
      const int deckW = (deckH * 176) / 108;
      const int deckX = (SCREEN_W - deckW) / 2;
      const int deckY = SY(2);
      drawTapeDeckGraphic(deckX, deckY, deckW, deckH, playlistPlayAnimMs, tapeCol, 255, playlistEditingName, false, reelsOnly);
      if (!reelsOnly) {
        prevPlaylistLockKey = 0;
      }
      keepStreaming();
    }
  }

  // 2. Top-Right Power-Button Lock Hint ("LOCK ->" when unlocked, "UNLOCK ->" when locked)
  {
    uint64_t lk = hashMix(hashInit(), playlistPlayLocked ? 2ULL : 1ULL);
    if (full || lk != prevPlaylistLockKey) {
      prevPlaylistLockKey = lk;
      const int w = SX(74), h = SY(14);
      Surface s(w, h);
      s.fillRect(0, 0, w, h, BG);
      const char *lockTxt = playlistPlayLocked ? "UNLOCK ->" : "LOCK ->";
      int tw = textWidth(lockTxt);
      s.drawText(max(0, w - SX(4) - tw), TY(h), lockTxt, playlistPlayLocked ? LOGO_COLOR : LIGHT);
      s.blit(SCREEN_W - w, SY(4));
    }
  }

  // 3. Controls Row: [ |< , Play/Stop + Speed , >| , VOL control ]
  //    Plain text on black; border drawn ONLY around the control where the cursor is.
  {
    const bool inCtrl = !playlistPlayLocked && (playlistPlayZone == PLAYLIST_PLAY_ZONE_CONTROLS);
    const int selCol = inCtrl ? (int)playlistPlayCtrlCol : -1;
    const int sqVal = (int)transport.speedQuarter;
    uint64_t ck = hashMix(hashInit(), (uint64_t)(selCol + 2) | ((uint64_t)isPlaying << 8) |
                                      ((uint64_t)playlistPlayVol << 16) |
                                      ((uint64_t)(uint8_t)sqVal << 24) |
                                      ((uint64_t)playlistPlayLocked << 32));
    if (full || ck != prevPlaylistCtrlKey) {
      prevPlaylistCtrlKey = ck;
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

      const int boxX[PLAYLIST_PLAY_CTRL_COUNT] = {0, SX(30), SX(104), SX(134)};
      const int boxW[PLAYLIST_PLAY_CTRL_COUNT] = {SX(24), SX(68), SX(24), SX(66)};
      char volTxt[16];
      snprintf(volTxt, sizeof(volTxt), "VOL: %d", (int)playlistPlayVol);
      const char *const labels[PLAYLIST_PLAY_CTRL_COUNT] = {
        "|<",
        playLabel,
        ">|",
        volTxt
      };

      for (int c = 0; c < PLAYLIST_PLAY_CTRL_COUNT; c++) {
        bool cSel = (selCol == c);
        bool isPlayBtn = (c == PLAYLIST_PLAY_CTRL_PLAY);
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
  for (int r = 0; r < PLAYLIST_PLAY_VISIBLE_ROWS; r++) {
    const int y = SY(142) + r * SY(18);
    const int rw = SX(200), rh = SY(18);
    const int idx = scroll + r;
    const bool hasSong = (total > 0 && idx < total);
    const bool isCurPlayingSong = hasSong && (idx == playlistPlayPlayingIdx);
    const bool sel = !playlistPlayLocked && (playlistPlayZone == PLAYLIST_PLAY_ZONE_LIST) && hasSong && (idx == cur);

    char songTxt[32];
    if (hasSong) {
      snprintf(songTxt, sizeof(songTxt), "%d.  %s", idx + 1, playlistEditSongs[idx]);
    } else if (total == 0 && r == 0) {
      snprintf(songTxt, sizeof(songTxt), "NO SONGS IN PLAYLIST");
    } else {
      songTxt[0] = '\0';
    }

    uint64_t rk = hashStr(hashInit(), songTxt);
    rk = hashMix(rk, (uint64_t)sel | ((uint64_t)isCurPlayingSong << 8) |
                     ((uint64_t)isPlaying << 16) | ((uint64_t)r << 24));
    if (full || rk != prevPlaylistRowKey[r]) {
      prevPlaylistRowKey[r] = rk;
      Surface s(rw, rh);
      s.fillRect(0, 0, rw, rh, BG);

      // Faint outer border around the scrollable song table
      constexpr uint16_t tableBd = DARK;
      s.fillRect(0, 0, PX, rh, tableBd);
      s.fillRect(rw - PX, 0, PX, rh, tableBd);
      if (r == 0) {
        s.fillRect(0, 0, rw, PX, tableBd);
      }
      if (r == PLAYLIST_PLAY_VISIBLE_ROWS - 1) {
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
    const bool sel = !playlistPlayLocked && (playlistPlayZone == PLAYLIST_PLAY_ZONE_EXIT);
    const int w = SX(44), h = SY(18);

    uint64_t bk = hashMix(hashInit(), (uint64_t)sel);
    if (full || bk != prevPlaylistBtnKey[0]) {
      prevPlaylistBtnKey[0] = bk;
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

FLASHMEM static void enterPlaylistPlayView() {
  snprintf(playlistEditingName, sizeof(playlistEditingName), "%s", playlistNameAt(playlistManageCursor));
  playlistEditSongCount = loadPlaylistSongs(playlistEditingName, playlistEditSongs, MAX_SONGS_PER_PLAYLIST);
  {
    const SongSettings &curPs = songSettings();
    playlistPlayVolHp = (curPs.masterVolHpEnc >= 1 && curPs.masterVolHpEnc <= MASTER_VOLUME_MAX + 1)
                        ? (uint8_t)(curPs.masterVolHpEnc - 1)
                        : curPs.masterVolume;
    playlistPlayVolSpk = (curPs.masterVolSpkEnc >= 1 && curPs.masterVolSpkEnc <= MASTER_VOLUME_MAX + 1)
                         ? (uint8_t)(curPs.masterVolSpkEnc - 1)
                         : curPs.masterVolume;
    playlistPlayVol = appAreHeadphonesConnected() ? playlistPlayVolHp : playlistPlayVolSpk;
  }
  playlistPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
  playlistPlayLocked = false;
  playlistPlayEditHeld = false;
  playlistPlayEditModUsed = false;
  playlistPlayListCursor = 0;
  playlistPlayListScroll = 0;
  playlistPlayPlayingIdx = -1;
  playlistPlayZone = (playlistEditSongCount > 0) ? PLAYLIST_PLAY_ZONE_CONTROLS : PLAYLIST_PLAY_ZONE_EXIT;
  playlistPlayCtrlCol = PLAYLIST_PLAY_CTRL_PLAY;
  playlistPlayAnimMs = 0;
  playlistPlayLastTickMs = millis();
  playlistModalView = PlaylistModalView::PlayPlaylist;
  invalidateSysPlaylistsModalCache();
  inputSwallowModalKeys();
  if (playlistEditSongCount > 0) {
    playlistPlayStartSong(0, true);
  } else {
    uiShowToast("PLAYLIST IS EMPTY");
    uiForceRedraw();
  }
}

FLASHMEM void handleSysPlaylistsModalInput(uint8_t pressed, uint8_t now, uint32_t ms) {
  (void)ms;
  if (playlistModalView == PlaylistModalView::Closed) return;

  // 1. Confirmation Overlay ("Delete playlist?", "Remove song?", or "Save song?")
  if (playlistConfirmOpen) {
    int step = ((pressed & (BTN_DOWN | BTN_RIGHT)) ? 1 : 0) -
               ((pressed & (BTN_UP | BTN_LEFT)) ? 1 : 0);
    if (step != 0) {
      playlistConfirmCursor = (playlistConfirmCursor + step + 2) % 2;
      uiForceRedraw();
    }
    if (pressed & BTN_EDIT) {
      bool yes = (playlistConfirmCursor == 0);
      playlistConfirmOpen = false;
      if (playlistConfirmKind == PlaylistConfirmKind::SaveBeforePlay) {
        if (yes && !saveCurrentSong()) {
          uiShowToast("SAVE FAILED");
          inputSwallowModalKeys();
          inputClearRepeat();
          uiForceRedraw();
          return;
        }
        enterPlaylistPlayView();
        return;
      }
      if (yes) {
        if (playlistConfirmKind == PlaylistConfirmKind::DeletePlaylist) {
          const char *pName = playlistNameAt(playlistManageCursor);
          if (deletePlaylistFile(pName)) {
            if (playlistManageCursor >= playlistCount() && playlistCount() > 0) {
              playlistManageCursor = playlistCount() - 1;
            }
            syncPlaylistManageScroll();
            uiShowToast("PLAYLIST DELETED");
          } else {
            uiShowToast("DELETE FAILED");
          }
        } else if (playlistConfirmKind == PlaylistConfirmKind::RemoveSong) {
          if (playlistEditSongCount > 0 && playlistEditCursor >= 0 && playlistEditCursor < playlistEditSongCount) {
            for (int i = playlistEditCursor; i < playlistEditSongCount - 1; i++) {
              memcpy(playlistEditSongs[i], playlistEditSongs[i + 1], sizeof(playlistEditSongs[i]));
            }
            playlistEditSongCount--;
            syncPlaylistEditScroll();
            uiShowToast("SONG REMOVED");
          }
        }
      }
      inputSwallowModalKeys();
      uiForceRedraw();
    } else if (pressed & BTN_OPT) {
      playlistConfirmOpen = false;
      inputSwallowModalKeys();
      uiForceRedraw();
    }
    return;
  }

  const int dy = ((pressed & BTN_DOWN) ? 1 : 0) - ((pressed & BTN_UP) ? 1 : 0);
  const int dx = ((pressed & BTN_RIGHT) ? 1 : 0) - ((pressed & BTN_LEFT) ? 1 : 0);
  const bool shiftLeft = (pressed & BTN_LEFT) && (now & BTN_SHIFT) && !(now & (BTN_OPT | BTN_EDIT));

  // 2. Manage Playlists Table
  if (playlistModalView == PlaylistModalView::ManageList) {
    if (shiftLeft || (pressed & BTN_OPT)) {
      closeSysPlaylistsModal();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      int total = playlistCount();
      int maxRow = playlistManageRowCount() - 1;
      if (dy != 0) {
        playlistManageCursor = constrain(playlistManageCursor + dy, 0, maxRow);
        syncPlaylistManageScroll();
        uiForceRedraw();
      }
      if (dx != 0 && playlistManageCursor < total) {
        playlistManageCol = constrain(playlistManageCol + dx, 0, PLAYLIST_ACT_COUNT - 1);
        uiForceRedraw();
      }
      if (pressed & BTN_EDIT) {
        if (playlistManageCursor >= total) {
          openPlaylistKeyboard(false, "", true);
          inputSwallowModalKeys();
        } else if (playlistManageCol == PLAYLIST_ACT_PLAY) {
          if (transport.mode != TransportMode::Stopped) {
            storageStopTransport();
          }
          if (songHasUnsavedChanges()) {
            playlistConfirmKind = PlaylistConfirmKind::SaveBeforePlay;
            playlistConfirmCursor = 0;
            playlistConfirmOpen = true;
            inputSwallowModalKeys();
            uiForceRedraw();
          } else {
            enterPlaylistPlayView();
          }
        } else if (playlistManageCol == PLAYLIST_ACT_EDIT) {
          snprintf(playlistEditingName, sizeof(playlistEditingName), "%s", playlistNameAt(playlistManageCursor));
          playlistEditSongCount = loadPlaylistSongs(playlistEditingName, playlistEditSongs, MAX_SONGS_PER_PLAYLIST);
          playlistEditCursor = 0;
          playlistEditScroll = 0;
          playlistEditCol = PLAYLIST_EDIT_ACT_ADD;
          playlistEditInBottomRow = false;
          playlistEditBottomCol = 0;
          playlistModalView = PlaylistModalView::EditPlaylist;
          invalidateSysPlaylistsModalCache();
          inputSwallowModalKeys();
          uiForceRedraw();
        } else if (playlistManageCol == PLAYLIST_ACT_RENAME) {
          openPlaylistKeyboard(true, playlistNameAt(playlistManageCursor), false);
          inputSwallowModalKeys();
        } else if (playlistManageCol == PLAYLIST_ACT_DELETE) {
          playlistConfirmKind = PlaylistConfirmKind::DeletePlaylist;
          playlistConfirmCursor = 0;
          playlistConfirmOpen = true;
          inputSwallowModalKeys();
          uiForceRedraw();
        }
      }
    }
    return;
  }

  // 3. Edit Playlist Table
  if (playlistModalView == PlaylistModalView::EditPlaylist) {
    if (shiftLeft) {
      playlistModalView = PlaylistModalView::ManageList;
      invalidateSysPlaylistsModalCache();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      if (dy != 0) {
        int maxRow = playlistEditRowCount() - 1;
        if (playlistEditInBottomRow) {
          if (dy < 0) {
            playlistEditInBottomRow = false;
            playlistEditCursor = maxRow;
            syncPlaylistEditScroll();
          }
        } else {
          if (dy > 0 && playlistEditCursor >= maxRow) {
            playlistEditInBottomRow = true;
          } else {
            playlistEditCursor = constrain(playlistEditCursor + dy, 0, maxRow);
            syncPlaylistEditScroll();
          }
        }
        uiForceRedraw();
      }
      if (dx != 0) {
        if (playlistEditInBottomRow) {
          playlistEditBottomCol = constrain(playlistEditBottomCol + dx, 0, 1);
        } else if (playlistEditCursor < playlistEditSongCount) {
          playlistEditCol = constrain(playlistEditCol + dx, 0, PLAYLIST_EDIT_ACT_COUNT - 1);
        }
        uiForceRedraw();
      }
      if (pressed & BTN_EDIT) {
        if (playlistEditInBottomRow) {
          if (playlistEditBottomCol == 0) {
            if (savePlaylistSongs(playlistEditingName, playlistEditSongs, playlistEditSongCount)) {
              uiShowToast("PLAYLIST SAVED");
            } else {
              uiShowToast("SAVE FAILED");
            }
          }
          playlistModalView = PlaylistModalView::ManageList;
          invalidateSysPlaylistsModalCache();
          inputSwallowModalKeys();
          uiForceRedraw();
        } else {
          int act = (playlistEditCursor >= playlistEditSongCount) ? PLAYLIST_EDIT_ACT_ADD : playlistEditCol;
          if (act == PLAYLIST_EDIT_ACT_ADD) {
            scanSongFolders();
            playlistAddSongCursor = 0;
            playlistAddSongScroll = 0;
            playlistModalView = PlaylistModalView::AddSong;
            invalidateSysPlaylistsModalCache();
            inputSwallowModalKeys();
            uiForceRedraw();
          } else if (act == PLAYLIST_EDIT_ACT_REMOVE && playlistEditCursor < playlistEditSongCount) {
            playlistConfirmKind = PlaylistConfirmKind::RemoveSong;
            playlistConfirmCursor = 0;
            playlistConfirmOpen = true;
            inputSwallowModalKeys();
            uiForceRedraw();
          }
        }
      }
    }
    return;
  }

  // 4. Add Song Picker Table (/Capstan/SONGS/)
  if (playlistModalView == PlaylistModalView::AddSong) {
    if (shiftLeft || (pressed & BTN_OPT)) {
      playlistModalView = PlaylistModalView::EditPlaylist;
      invalidateSysPlaylistsModalCache();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }
    if (!(now & BTN_SHIFT)) {
      int total = songCount();
      int step = dy + dx * PLAYLIST_ADD_VISIBLE_ROWS;
      if (step != 0 && total > 0) {
        playlistAddSongCursor = constrain(playlistAddSongCursor + step, 0, total - 1);
        syncPlaylistAddSongScroll();
        uiForceRedraw();
      }
      if (pressed & BTN_EDIT) {
        if (total <= 0) {
          uiShowToast("NO SAVED SONGS");
        } else if (playlistEditSongCount >= MAX_SONGS_PER_PLAYLIST) {
          uiShowToast("PLAYLIST FULL");
        } else {
          const char *sName = songNameAt(playlistAddSongCursor);
          int insIdx = (playlistEditCursor >= playlistEditSongCount)
                         ? playlistEditSongCount
                         : constrain(playlistEditCursor + 1, 0, playlistEditSongCount);
          for (int i = playlistEditSongCount; i > insIdx; i--) {
            memcpy(playlistEditSongs[i], playlistEditSongs[i - 1], sizeof(playlistEditSongs[i]));
          }
          snprintf(playlistEditSongs[insIdx], sizeof(playlistEditSongs[insIdx]), "%s", sName);
          playlistEditSongCount++;
          playlistEditCursor = insIdx;
          playlistEditCol = PLAYLIST_EDIT_ACT_ADD;
          syncPlaylistEditScroll();
          char msg[32];
          snprintf(msg, sizeof(msg), "ADDED: %s", sName);
          uiShowToast(msg);
        }
        playlistModalView = PlaylistModalView::EditPlaylist;
        invalidateSysPlaylistsModalCache();
        inputSwallowModalKeys();
        uiForceRedraw();
      }
    }
    return;
  }

  // 5. Playlist Play View
  if (playlistModalView == PlaylistModalView::PlayPlaylist) {
    if (shiftLeft) {
      playlistPlayStopAndRestoreLoadedSong();
      playlistModalView = PlaylistModalView::ManageList;
      invalidateSysPlaylistsModalCache();
      inputSwallowModalKeys();
      uiForceRedraw();
      return;
    }

    const bool onPlayCell = (playlistPlayZone == PLAYLIST_PLAY_ZONE_CONTROLS &&
                             playlistPlayCtrlCol == PLAYLIST_PLAY_CTRL_PLAY);
    const bool onVolCell = (playlistPlayZone == PLAYLIST_PLAY_ZONE_CONTROLS &&
                            playlistPlayCtrlCol == PLAYLIST_PLAY_CTRL_VOL);
    const bool editJustPressed = (pressed & BTN_EDIT) != 0;
    const bool editJustReleased = playlistPlayEditHeld && !(now & BTN_EDIT);
    if (editJustPressed) {
      playlistPlayEditHeld = true;
      playlistPlayEditModUsed = false;
    }
    if (!(now & BTN_EDIT)) {
      playlistPlayEditHeld = false;
    }

    // OPT + EDIT on Play/Stop button resets playback speed to 1.00x (same as transport bar)
    if (onPlayCell && (((pressed & BTN_EDIT) && (now & BTN_OPT)) ||
                       ((pressed & BTN_OPT) && (now & BTN_EDIT)))) {
      playlistPlayEditModUsed = true;
      playlistPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
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
        playlistPlayEditModUsed = true;
        appStepPlaybackSpeed(dx);
        playlistPlaySpeedQuarter = (int8_t)transport.speedQuarter;
        uiForceRedraw();
      } else if (dy < 0) {
        playlistPlayEditModUsed = true;
        playlistPlaySpeedQuarter = DEFAULT_SPEED_QUARTER;
        appSetPlaybackSpeedQuarter(DEFAULT_SPEED_QUARTER);
        uiForceRedraw();
      } else if (dy > 0) {
        playlistPlayEditModUsed = true;
        playlistPlaySpeedQuarter = REVERSE_1X_SPEED_QUARTER;
        appSetPlaybackSpeedQuarter(REVERSE_1X_SPEED_QUARTER);
        uiForceRedraw();
      }
      return;
    }

    // OPT + EDIT on VOL resets master volume to default (50 on Headphones, 40 on Speaker)
    if (onVolCell && (((pressed & BTN_EDIT) && (now & BTN_OPT)) ||
                      ((pressed & BTN_OPT) && (now & BTN_EDIT)))) {
      playlistPlayEditModUsed = true;
      playlistPlayVol = appDefaultMasterVolume();
      if (appAreHeadphonesConnected()) playlistPlayVolHp = playlistPlayVol;
      else playlistPlayVolSpk = playlistPlayVol;
      appSetMasterVolume(playlistPlayVol);
      uiForceRedraw();
      return;
    }

    // EDIT + Direction on VOL adjusts master output volume (0..100)
    if (onVolCell && (now & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT))) {
      int step = (dx != 0) ? dx : (-dy * 5);
      if (step != 0) {
        playlistPlayEditModUsed = true;
        playlistPlayVol = (uint8_t)constrain((int)playlistPlayVol + step, 0, (int)MASTER_VOLUME_MAX);
        if (appAreHeadphonesConnected()) playlistPlayVolHp = playlistPlayVol;
        else playlistPlayVolSpk = playlistPlayVol;
        appSetMasterVolume(playlistPlayVol);
        uiForceRedraw();
      }
      return;
    }

    auto togglePlaylistPlayTransport = [&]() {
      if (playlistPlayActive && transport.mode == TransportMode::Running) {
        playlistPlayActive = false;
        fmSynthSetExportTailPhase(false);
        storageStopTransport();
        fmSynthAllNotesOff();
        uiForceRedraw();
      } else if (playlistEditSongCount > 0) {
        if (playlistPlayPlayingIdx < 0 || transport.tapeFrame >= MAX_REC_FRAMES) {
          playlistPlayStartSong((playlistPlayPlayingIdx >= 0) ? playlistPlayPlayingIdx : 0, true);
        } else {
          playlistPlayActive = true;
          transport.speedQuarter = playlistPlaySpeedQuarter;
          transport.loopEnabled = false;
          transport.punchEnabled = false;
          metronomeOn = false;
          storageStartTransport(false, false, false);
          noInterrupts();
          metronomeOn = false;
          playlistPlayLastNow = transport.now;
          interrupts();
          uiForceRedraw();
        }
      }
    };

    // Hardware PLAY button toggles Play/Stop from anywhere on the Playlist Play screen
    if ((pressed & BTN_PLAY) && !(now & (BTN_SHIFT | BTN_OPT | BTN_EDIT))) {
      togglePlaylistPlayTransport();
      return;
    }

    if (!(now & (BTN_SHIFT | BTN_EDIT | BTN_OPT))) {
      if (dy != 0) {
        if (playlistPlayZone == PLAYLIST_PLAY_ZONE_CONTROLS) {
          if (dy > 0) {
            if (playlistEditSongCount > 0) {
              playlistPlayZone = PLAYLIST_PLAY_ZONE_LIST;
              syncPlaylistPlayScroll();
            } else {
              playlistPlayZone = PLAYLIST_PLAY_ZONE_EXIT;
            }
          }
        } else if (playlistPlayZone == PLAYLIST_PLAY_ZONE_LIST) {
          if (dy < 0 && playlistPlayListCursor <= 0) {
            playlistPlayZone = PLAYLIST_PLAY_ZONE_CONTROLS;
          } else if (dy > 0 && playlistPlayListCursor >= playlistEditSongCount - 1) {
            playlistPlayZone = PLAYLIST_PLAY_ZONE_EXIT;
          } else {
            playlistPlayListCursor = constrain(playlistPlayListCursor + dy, 0, max(0, playlistEditSongCount - 1));
            syncPlaylistPlayScroll();
          }
        } else if (playlistPlayZone == PLAYLIST_PLAY_ZONE_EXIT) {
          if (dy < 0) {
            if (playlistEditSongCount > 0) {
              playlistPlayZone = PLAYLIST_PLAY_ZONE_LIST;
              playlistPlayListCursor = constrain(playlistPlayListCursor, 0, playlistEditSongCount - 1);
              syncPlaylistPlayScroll();
            } else {
              playlistPlayZone = PLAYLIST_PLAY_ZONE_CONTROLS;
            }
          }
        }
        uiForceRedraw();
      }

      if (dx != 0) {
        if (playlistPlayZone == PLAYLIST_PLAY_ZONE_CONTROLS) {
          playlistPlayCtrlCol = (uint8_t)constrain((int)playlistPlayCtrlCol + dx, 0, PLAYLIST_PLAY_CTRL_COUNT - 1);
          uiForceRedraw();
        } else if (playlistPlayZone == PLAYLIST_PLAY_ZONE_LIST && dx > 0) {
          playlistPlayZone = PLAYLIST_PLAY_ZONE_EXIT;
          uiForceRedraw();
        } else if (playlistPlayZone == PLAYLIST_PLAY_ZONE_EXIT && dx < 0) {
          playlistPlayZone = (playlistEditSongCount > 0) ? PLAYLIST_PLAY_ZONE_LIST : PLAYLIST_PLAY_ZONE_CONTROLS;
          uiForceRedraw();
        }
      }
    }

    // On Play/Stop cell, trigger toggle on EDIT release (when not used as a modifier for EDIT+L/R or OPT+EDIT)
    if (onPlayCell && editJustReleased && !playlistPlayEditModUsed && !(now & (BTN_SHIFT | BTN_OPT))) {
      togglePlaylistPlayTransport();
      return;
    }

    if (!onPlayCell && (pressed & BTN_EDIT) && !(now & (BTN_SHIFT | BTN_OPT))) {
      if (playlistPlayZone == PLAYLIST_PLAY_ZONE_CONTROLS) {
        if (playlistPlayCtrlCol == PLAYLIST_PLAY_CTRL_PREV && playlistEditSongCount > 0) {
          int prevIdx = (playlistPlayPlayingIdx > 0) ? (playlistPlayPlayingIdx - 1) : 0;
          playlistPlayListCursor = prevIdx;
          syncPlaylistPlayScroll();
          playlistPlayStartSong(prevIdx, true);
        } else if (playlistPlayCtrlCol == PLAYLIST_PLAY_CTRL_NEXT && playlistEditSongCount > 0) {
          int nextIdx = (playlistPlayPlayingIdx >= 0 && playlistPlayPlayingIdx + 1 < playlistEditSongCount)
                          ? (playlistPlayPlayingIdx + 1) : 0;
          playlistPlayListCursor = nextIdx;
          syncPlaylistPlayScroll();
          playlistPlayStartSong(nextIdx, true);
        }
      } else if (playlistPlayZone == PLAYLIST_PLAY_ZONE_LIST) {
        if (playlistEditSongCount > 0 && playlistPlayListCursor >= 0 && playlistPlayListCursor < playlistEditSongCount) {
          playlistPlayStartSong(playlistPlayListCursor, true);
        }
        inputSwallowModalKeys();
      } else if (playlistPlayZone == PLAYLIST_PLAY_ZONE_EXIT) {
        playlistPlayStopAndRestoreLoadedSong();
        closeSysPlaylistsModal();
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

    const int targetId = midiMapCategoryRowTarget((MidiMapCategory)midiMapCat, idx);
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
  int targetId = midiMapCategoryRowTarget((MidiMapCategory)midiMapCat, midiMapCursor);

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
  if (playlistModalView == PlaylistModalView::ManageList) {
    drawPlaylistManageListModal(full);
    return;
  }
  if (playlistModalView == PlaylistModalView::EditPlaylist) {
    drawPlaylistEditModal(full);
    return;
  }
  if (playlistModalView == PlaylistModalView::AddSong) {
    drawPlaylistAddSongModal(full);
    return;
  }
  if (playlistModalView == PlaylistModalView::PlayPlaylist) {
    drawPlaylistPlayModal(full);
    return;
  }
  if (midiMapModalOpen) {
    drawSysMidiMapModal(full);
    return;
  }

  sysSetRow = constrain(sysSetRow, 0, SSET_COUNT - 1);
  if (!board.supportsUsbChargeControl() && sysSetRow == SSET_USB_CHARGE_ON) {
    sysSetRow = SSET_MIDI_MAP;
  }
  const SystemSettings &ss = systemSettings();
  PROGMEM static const char *const LABELS[SSET_COUNT] = {
    "SCREEN BRIGHTNESS",
    "METRONOME VOLUME",
    "INSTRUMENT PRESETS",
    "PLAYLISTS",
    "MIDI MAPPINGS",
    "USB CHARGE WHILE ON",
    "USB STORAGE DRIVE"
  };

  int vis = 0;
  for (int i = 0; i < SSET_COUNT; i++) {
    if (i == SSET_USB_CHARGE_ON && !board.supportsUsbChargeControl()) {
      continue;
    }
    int y = SY(28) + vis * SY(26);
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
      case SSET_PLAYLISTS:
      case SSET_MIDI_MAP:
        isPill = true;
        snprintf(val, sizeof(val), "MANAGE");
        break;
      case SSET_USB_CHARGE_ON:
        isOff = !ss.usbChargeWhileOn;
        snprintf(val, sizeof(val), ss.usbChargeWhileOn ? "ON" : "OFF");
        break;
      case SSET_USB_DRIVE:
        isPill = true;
        snprintf(val, sizeof(val), "MOUNT SD");
        break;
    }

    drawSettingsTableRow(y, (uint32_t)i, LABELS[i], val, sel, isOff, isPill,
                         prevSysSettingsKey[vis], full);
    vis++;
    keepStreaming();
  }
}

FLASHMEM static void sysUsbChargeReenumTick() {
  keepStreaming();
  if (usbDriveTick) usbDriveTick();
}

FLASHMEM static void applyAndCommitUsbChargeSetting(bool nextEnable) {
  if (!board.supportsUsbChargeControl()) return;
  if (transport.mode != TransportMode::Stopped || recording.mode != Recording::Idle) {
    uiShowToast("STOP TRANSPORT FIRST");
    inputClearRepeat();
    uiForceRedraw();
    return;
  }
  SystemSettings &ss = systemSettings();
  if (ss.usbChargeWhileOn == nextEnable) return;
  ss.usbChargeWhileOn = nextEnable;
  applySystemSettings();
  saveSystemSettings();
  uiForceRedraw();
  uiRefresh();
  appSyncUsbChargeEnumeration(sysUsbChargeReenumTick);
  inputClearRepeat();
  uiForceRedraw();
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
    case SSET_USB_CHARGE_ON:
      applyAndCommitUsbChargeSetting(!ss.usbChargeWhileOn);
      return;
    case SSET_INST_PRESETS:
    case SSET_PLAYLISTS:
    case SSET_MIDI_MAP:
    case SSET_USB_DRIVE:
      return;
  }
  applySystemSettings();
  markSystemSettingsDirty();
  uiForceRedraw();
}

FLASHMEM static void sysSettingsOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dx;
  (void)dir;
  if (dy != 0) {
    int step = (dy > 0) ? 1 : -1;
    int next = constrain(sysSetRow + step, 0, SSET_COUNT - 1);
    if (!board.supportsUsbChargeControl() && next == SSET_USB_CHARGE_ON) {
      next = constrain(next + step, 0, SSET_COUNT - 1);
    }
    if (next != sysSetRow) {
      sysSetRow = next;
      uiForceRedraw();
    }
  }
}

FLASHMEM void usbSavePromptModalConfirm() {
  if (!usbSavePromptOpen) return;
  bool doSave = (usbSavePromptCursor == 0);
  usbSavePromptOpen = false;
  prevUsbSavePromptKey = 0;

  // Redraw underlying System Settings page before saving / showing USB overlay
  uiClearRegionStreaming(SY(24), SCREEN_H - SY(24), BG);
  sysSettingsInvalidateCache();
  sysSettingsDrawPage(true);

  if (doSave && !saveCurrentSong()) {
    uiShowToast("SAVE FAILED");
    inputSwallowModalKeys();
    inputClearRepeat();
    uiForceRedraw();
    return;
  }
  if (!usbDriveMode()) {
    uiShowToast("BUILD WITH USB SERIAL+MSC");
  }
  inputSwallowModalKeys();
  inputClearRepeat();
  uiForceRedraw();
}

FLASHMEM static void sysSettingsOnEditPress(bool shiftHeld) {
  if (shiftHeld) return;
  if (sysSetRow == SSET_USB_CHARGE_ON) {
    applyAndCommitUsbChargeSetting(!systemSettings().usbChargeWhileOn);
  } else if (sysSetRow == SSET_USB_DRIVE) {
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
  } else if (sysSetRow == SSET_PLAYLISTS) {
    openSysPlaylistsModal();
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
  } else if (sysSetRow == SSET_USB_CHARGE_ON) {
    applyAndCommitUsbChargeSetting(true);
    return;
  } else {
    return;
  }
  applySystemSettings();
  markSystemSettingsDirty();
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

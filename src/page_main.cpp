// page_main.cpp — Main (Songs) Page UI & Input
//
// Layout:
//   - Top Session Bar (mainRow == MAIN_ROW_SESSION):
//       Left card shows active song name + status (ACTIVE / MODIFIED* / UNSAVED*),
//       Right buttons provide session-level [ SAVE ] and [ NEW ] actions.
//   - Saved Songs Table (mainRow == MAIN_ROW_LIST):
//       Scrollable 7-row list where cursor = target song, displaying inline
//       [ LOAD ] and [ DEL ] pills on the highlighted row.
#include "page_main.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

using namespace m8;
using namespace looper;

static uint64_t prevMainBannerKey = 0;
static uint64_t prevMainSessBtnKey[MAIN_SESS_COUNT] = {};
static uint64_t prevMainSectionKey = 0;
static uint64_t prevMainListKey[MAIN_LIST_VISIBLE_ROWS] = {};
static uint64_t prevMainScrollKey = 0;

FLASHMEM static const char *mainHeaderTitle() {
  return "MAIN";
}

FLASHMEM static void mainOnEnter() {
  cancelDeleteConfirm();
  syncSongListCursor(false);
  mainRow = MAIN_ROW_SESSION;
  mainCol = MAIN_SESS_SAVE;
}

FLASHMEM static void mainInvalidateCache() {
  prevMainBannerKey = 0;
  memset(prevMainSessBtnKey, 0, sizeof(prevMainSessBtnKey));
  prevMainSectionKey = 0;
  memset(prevMainListKey, 0, sizeof(prevMainListKey));
  prevMainScrollKey = 0;
}

FLASHMEM static void mainDrawPage(bool full) {
  if (isSaveKeyboardOpen()) {
    drawSaveKeyboardModal(full);
    return;
  }

  syncSongListCursor(false);

  const char *curName = currentSongName();
  bool isSaved = songIsSaved();
  bool isDirty = songHasUnsavedChanges();
  int totalSongs = songCount();
  bool loadConfirm = isLoadConfirmActive();
  bool delConfirm = isDeleteConfirmActive();
  int confirmIdx = deleteConfirmTargetIndex();
  bool inSessionRow = (mainRow == MAIN_ROW_SESSION);

  // 1. Top Session Row: Active Song Card + [ SAVE ] button
  {
    const int sessY = SY(26), sessH = SY(24);
    // 1a. Active Song Status Card
    const char *statusTxt = !isSaved ? "UNSAVED*" : (isDirty ? "MOD*" : "ACTIVE");
    uint16_t statusCol = (!isSaved || isDirty) ? COLOR_WARN : ACCENT;
    uint16_t cardBorder = MID;

    uint64_t bk = hashStr(hashInit(), curName);
    bk = hashStr(bk, statusTxt);
    bk = hashMix(bk, (uint64_t)cardBorder | ((uint64_t)statusCol << 16) | ((uint64_t)ACCENT << 32));
    if (full || bk != prevMainBannerKey) {
      prevMainBannerKey = bk;
      const int cardW = SX(246);
      Surface s(cardW, sessH);
      s.drawBox(PANEL, cardBorder);
      s.fillRect(PX, PX, SX(3), sessH - 2 * PX, statusCol);
      s.drawText(SX(9), TY(sessH), curName, INK);
      int sw = textWidth(statusTxt);
      s.drawText(cardW - SX(6) - sw, TY(sessH), statusTxt, statusCol);
      s.blit(SX(6), sessY);
    }

    // 1b. Session Action Button: [ SAVE ]
    static const char *const SESS_LABELS[MAIN_SESS_COUNT] = {"SAVE"};
    static const int sessX[MAIN_SESS_COUNT] = {SX(256)};
    static const int sessW[MAIN_SESS_COUNT] = {SX(58)};

    for (int c = 0; c < MAIN_SESS_COUNT; c++) {
      bool sel = inSessionRow && (mainCol == c);
      const char *lbl = SESS_LABELS[c];
      uint16_t bg = sel ? ACCENT : PANEL;
      uint16_t fg = sel ? BG : LIGHT;
      uint16_t border = sel ? INK : MID;

      uint64_t sk = hashStr(hashInit(), lbl);
      sk = hashMix(sk, (uint64_t)bg | ((uint64_t)fg << 16) | ((uint64_t)border << 32));
      if (full || sk != prevMainSessBtnKey[c]) {
        prevMainSessBtnKey[c] = sk;
        Surface s(sessW[c], sessH);
        s.drawBox(bg, border);
        int tw = textWidth(lbl);
        s.drawText(max(2, (sessW[c] - tw) / 2), TY(sessH), lbl, fg);
        s.blit(sessX[c], sessY);
      }
    }
  }

  // 2. Table Header: SAVED SONGS
  {
    uint64_t sk = (uint64_t)(!inSessionRow ? 2 : 1);
    if (full || sk != prevMainSectionKey) {
      prevMainSectionKey = sk;
      const int secW = SX(308), secH = SY(12);
      Surface s(secW, secH);
      s.fillRect(0, 0, secW, secH, BG);
      s.drawText(SX(2), TY(secH), "SAVED SONGS", !inSessionRow ? LIGHT : DIM);
      s.blit(SX(6), SY(55));
    }
  }

  // 3. Scrollable 7-Row Song Table (saved songs + next empty row with [ NEW ] button)
  const int listRows = totalSongs + 1;
  for (int r = 0; r < MAIN_LIST_VISIBLE_ROWS; r++) {
    int y = SY(70) + r * SY(23);
    int w = SX(302), h = SY(21);
    int idx = songListScroll + r;

    if (idx > totalSongs) {
      uint64_t lk = hashMix(hashInit(), 0xDEAD0000u + (uint32_t)r);
      if (full || lk != prevMainListKey[r]) {
        prevMainListKey[r] = lk;
        Surface s(w, h);
        s.drawBox(BG, DARK);
        s.blit(SX(6), y);
      }
      continue;
    }

    if (idx == totalSongs) {
      bool hasCursor = (!inSessionRow && idx == songListCursor);
      uint16_t bg = hasCursor ? DARK : PANEL;
      uint16_t border = hasCursor ? ACCENT : MID;
      uint16_t barCol = hasCursor ? ACCENT : DIM;
      uint64_t lk = hashMix(hashInit(), 0xBEEF0000u | ((uint32_t)idx << 8) | (hasCursor ? 1u : 0u));
      if (full || lk != prevMainListKey[r]) {
        prevMainListKey[r] = lk;
        Surface s(w, h);
        s.drawBox(bg, border);
        s.fillRect(PX, PX, SX(3), h - 2 * PX, barCol);

        char leftTxt[28];
        snprintf(leftTxt, sizeof(leftTxt), "%02d  ---", idx + 1);
        s.drawText(SX(10), TY(h), leftTxt, hasCursor ? LIGHT : DIM);

        const int pillY = SY(3), pillH = SY(15);
        const int newX  = SX(248), newW  = SX(48);
        uint16_t newBg = hasCursor ? ACCENT : PANEL;
        uint16_t newFg = hasCursor ? BG : LIGHT;
        uint16_t newBd = hasCursor ? INK : MID;
        s.drawPill(newX, pillY, newW, pillH, "NEW", newFg, newBg, newBd);
        s.blit(SX(6), y);
      }
      continue;
    }

    const char *pName = songNameAt(idx);
    bool hasCursor = (!inSessionRow && idx == songListCursor);
    bool isActiveSong = isSaved && (strcasecmp(pName, curName) == 0);
    bool isLoadRow = loadConfirm && (confirmIdx == idx);
    bool isDelRow = delConfirm && (confirmIdx == idx);
    bool isConfirmRow = isLoadRow || isDelRow;
    int rowActionCol = hasCursor ? constrain(mainCol, 0, MAIN_ROW_ACT_COUNT - 1) : -1;

    uint16_t bg = isConfirmRow ? COLOR_REC_DIM
                : hasCursor    ? DARK
                               : PANEL;
    uint16_t border = isConfirmRow ? COLOR_REC
                    : hasCursor    ? ACCENT
                    : isActiveSong ? ACCENT_DARK
                                   : MID;
    uint16_t barCol = isConfirmRow ? COLOR_REC
                    : hasCursor    ? ACCENT
                    : isActiveSong ? COLOR_PLAY
                                   : DIM;

    uint64_t lk = hashStr(hashInit(), pName);
    lk = hashMix(lk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                         ((uint64_t)idx << 48) | ((uint64_t)(rowActionCol + 2) << 56) |
                         ((uint64_t)isActiveSong << 60) | ((uint64_t)isDelRow << 61) |
                         ((uint64_t)isDirty << 62) | ((uint64_t)isLoadRow << 63));
    if (full || lk != prevMainListKey[r]) {
      prevMainListKey[r] = lk;
      Surface s(w, h);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), h - 2 * PX, barCol);

      char leftTxt[28];
      snprintf(leftTxt, sizeof(leftTxt), "%02d  %s", idx + 1, pName);
      uint16_t nameCol = hasCursor ? INK : (isActiveSong ? ACCENT : LIGHT);
      s.drawText(SX(10), TY(h), leftTxt, nameCol);

      if (hasCursor) {
        // Inline row action pills: [ LOAD / SURE? ] and [ DEL / SURE? ]
        const int pillY = SY(3), pillH = SY(15);
        const int loadX = SX(196), loadW = SX(48);
        const int delX  = SX(248), delW  = SX(48);

        bool loadSel = (rowActionCol == MAIN_ROW_ACT_LOAD);
        bool delSel  = (rowActionCol == MAIN_ROW_ACT_DELETE);

        const char *loadLbl = isLoadRow ? "SURE?" : "LOAD";
        uint16_t loadBg = loadSel ? (isLoadRow ? COLOR_REC : ACCENT) : (isLoadRow ? COLOR_REC_DIM : PANEL);
        uint16_t loadFg = loadSel ? BG : (isLoadRow ? COLOR_REC : DIM);
        uint16_t loadBd = loadSel ? INK : (isLoadRow ? COLOR_REC : MID);
        s.drawPill(loadX, pillY, loadW, pillH, loadLbl, loadFg, loadBg, loadBd);

        const char *delLbl = isDelRow ? "SURE?" : "DEL";
        uint16_t delBg = delSel ? (isDelRow ? COLOR_REC : ACCENT) : (isDelRow ? COLOR_REC_DIM : PANEL);
        uint16_t delFg = delSel ? BG : (isDelRow ? COLOR_REC : DIM);
        uint16_t delBd = delSel ? INK : (isDelRow ? COLOR_REC : MID);
        s.drawPill(delX, pillY, delW, pillH, delLbl, delFg, delBg, delBd);
      } else if (isActiveSong) {
        const char *actTag = isDirty ? "MOD*" : "ACTIVE";
        uint16_t actCol = isDirty ? COLOR_WARN : ACCENT;
        int aw = textWidth(actTag);
        s.drawText(w - SX(8) - aw, TY(h), actTag, actCol);
      }

      s.blit(SX(6), y);
    }
  }

  // 4. Scrollbar Indicator
  {
    uint64_t sck = hashMix(hashInit(), (uint64_t)listRows | ((uint64_t)songListScroll << 16) | ((uint64_t)songListCursor << 32));
    if (full || sck != prevMainScrollKey) {
      prevMainScrollKey = sck;
      const int sbW = SX(4);
      const int trackH = (MAIN_LIST_VISIBLE_ROWS - 1) * SY(23) + SY(21);
      Surface s(sbW, trackH);
      s.fillRect(0, 0, sbW, trackH, DARK);
      if (listRows > 0) {
        int thumbH = (listRows <= MAIN_LIST_VISIBLE_ROWS)
                         ? trackH
                         : max(SY(14), trackH * MAIN_LIST_VISIBLE_ROWS / listRows);
        int maxScroll = max(1, listRows - MAIN_LIST_VISIBLE_ROWS);
        int thumbY = (listRows <= MAIN_LIST_VISIBLE_ROWS)
                         ? 0
                         : (songListScroll * (trackH - thumbH)) / maxScroll;
        s.fillRect(0, constrain(thumbY, 0, trackH - thumbH), sbW, thumbH, ACCENT);
      }
      s.blit(SX(310), SY(70));
    }
  }
}

FLASHMEM static void triggerLoadCursorSong() {
  if (transport.mode != TransportMode::Stopped) {
    cancelDeleteConfirm();
    uiShowToast("STOP TRANSPORT FIRST");
    return;
  }
  int total = songCount();
  if (total <= 0) {
    cancelDeleteConfirm();
    uiShowToast("NO SAVED SONGS");
    return;
  }
  int targetIdx = constrain(songListCursor, 0, total - 1);
  if (isLoadConfirmActive() && deleteConfirmTargetIndex() == targetIdx) {
    cancelDeleteConfirm();
    if (loadSong(targetIdx)) {
      applySongSettings();
      updateMetronomeState();
      syncSongListCursor(true);
      char msg[32];
      snprintf(msg, sizeof(msg), "LOADED: %s", currentSongName());
      uiShowToast(msg);
      setPage(Page::Audio);
    } else {
      uiShowToast("LOAD FAILED");
      uiForceRedraw();
    }
  } else {
    armLoadConfirm(targetIdx);
    char msg[36];
    snprintf(msg, sizeof(msg), "LOAD %s? EDIT AGAIN", songNameAt(targetIdx));
    uiShowToast(msg, 3000);
    uiForceRedraw();
  }
}

FLASHMEM static void triggerDeleteCursorSong() {
  if (transport.mode != TransportMode::Stopped) {
    cancelDeleteConfirm();
    uiShowToast("STOP TRANSPORT FIRST");
    return;
  }
  int total = songCount();
  if (total <= 0) {
    cancelDeleteConfirm();
    uiShowToast("NO SAVED SONGS");
    return;
  }
  int targetIdx = constrain(songListCursor, 0, total - 1);
  if (isDeleteConfirmActive() && deleteConfirmTargetIndex() == targetIdx) {
    char delName[MAX_SONG_NAME_LEN + 1];
    snprintf(delName, sizeof(delName), "%s", songNameAt(targetIdx));
    cancelDeleteConfirm();
    if (deleteSongAt(targetIdx)) {
      songListCursor = min(songListCursor, max(0, songCount() - 1));
      syncSongListCursor(false);
      if (songCount() <= 0) {
        mainRow = MAIN_ROW_SESSION;
        mainCol = MAIN_SESS_SAVE;
      } else {
        mainCol = MAIN_ROW_ACT_LOAD;
      }
      char msg[32];
      snprintf(msg, sizeof(msg), "DELETED: %s", delName);
      uiShowToast(msg);
    } else {
      uiShowToast("DELETE FAILED");
    }
    uiForceRedraw();
  } else {
    armDeleteConfirm(targetIdx);
    char msg[36];
    snprintf(msg, sizeof(msg), "DEL %s? EDIT AGAIN", songNameAt(targetIdx));
    uiShowToast(msg, 3000);
    uiForceRedraw();
  }
}

FLASHMEM static void mainOnShiftNavigate(uint8_t dir) {
  if (dir & BTN_LEFT) setPage(Page::SongSettings);
  else if (dir & BTN_DOWN) setPage(lastRow2Page());
}

FLASHMEM static void mainOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  cancelDeleteConfirm();
  int total = songCount();

  if (dy != 0) {
    if (mainRow == MAIN_ROW_SESSION) {
      if (dy > 0) {
        mainRow = MAIN_ROW_LIST;
        mainCol = MAIN_ROW_ACT_LOAD;
        syncSongListCursor(false);
      }
    } else {
      if (dy < 0) {
        if (songListCursor > 0) {
          songListCursor--;
          mainCol = MAIN_ROW_ACT_LOAD;
          syncSongListCursor(false);
        } else {
          mainRow = MAIN_ROW_SESSION;
          mainCol = MAIN_SESS_SAVE;
        }
      } else if (dy > 0) {
        if (songListCursor < total) {
          songListCursor++;
          mainCol = MAIN_ROW_ACT_LOAD;
          syncSongListCursor(false);
        }
      }
    }
  }

  if (dx != 0) {
    if (mainRow == MAIN_ROW_SESSION) {
      mainCol = constrain(mainCol + dx, 0, MAIN_SESS_COUNT - 1);
    } else if (songListCursor < total) {
      mainCol = constrain(mainCol + dx, 0, MAIN_ROW_ACT_COUNT - 1);
    }
  }

  uiForceRedraw();
}

FLASHMEM static void mainOnOptAdjust(int dx, int dy, int delta) {
  (void)delta;
  cancelDeleteConfirm();
  int total = songCount();
  if (dy > 0) {
    mainRow = MAIN_ROW_SESSION;
    mainCol = MAIN_SESS_SAVE;
    uiForceRedraw();
  } else if (dy < 0) {
    mainRow = MAIN_ROW_LIST;
    mainCol = MAIN_ROW_ACT_LOAD;
    syncSongListCursor(false);
    uiForceRedraw();
  } else if (dx != 0 && mainRow == MAIN_ROW_LIST) {
    songListCursor = constrain(songListCursor + dx * MAIN_LIST_VISIBLE_ROWS, 0, total);
    mainCol = MAIN_ROW_ACT_LOAD;
    syncSongListCursor(false);
    uiForceRedraw();
  }
}

FLASHMEM static void mainOnEditPress(bool shiftHeld) {
  if (mainRow == MAIN_ROW_SESSION) {
    openSaveKeyboard(shiftHeld);
    inputSwallowModalKeys();
  } else if (songListCursor >= songCount()) {
    cancelDeleteConfirm();
    if (transport.mode == TransportMode::Stopped) {
      createNewSong();
      syncSongListCursor(false);
      char msg[32];
      snprintf(msg, sizeof(msg), "NEW: %s", currentSongName());
      uiShowToast(msg);
      uiForceRedraw();
    } else {
      uiShowToast("STOP TRANSPORT FIRST");
    }
  } else {
    if (mainCol == MAIN_ROW_ACT_LOAD) {
      triggerLoadCursorSong();
    } else if (mainCol == MAIN_ROW_ACT_DELETE) {
      triggerDeleteCursorSong();
    }
  }
}

PROGMEM static const PageOps MAIN_PAGE_OPS = {
  Page::Main,
  mainHeaderTitle,
  mainOnEnter,
  mainInvalidateCache,
  mainDrawPage,
  mainOnShiftNavigate,
  mainOnCursorMove,
  mainOnOptAdjust,
  mainOnEditPress,
  nullptr
};

FLASHMEM const PageOps &pageMainOps() {
  return MAIN_PAGE_OPS;
}

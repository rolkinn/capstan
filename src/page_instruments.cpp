// page_instruments.cpp — Instruments Table Page UI & Input
//
// Full 8-row instrument table (I01..I08 all visible at once):
//   - UP / DOWN moves cursor across I01..I08.
//   - Highlighted row displays inline [ COPY ], [ PASTE ], [ NEW ], and [ CLR ] pills.
//   - LEFT / RIGHT selects the active inline action; EDIT triggers it.
//   - SHIFT + RIGHT creates a new instrument (if empty) or opens the editor (if populated).
#include "page_instruments.h"
#include "page_instrument_edit.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

using namespace m8;
using namespace looper;

static uint64_t prevInstListKey[INST_LIST_VISIBLE_ROWS] = {};

static int instDelConfirmIdx = -1;
static uint32_t instDelConfirmUntilMs = 0;

FLASHMEM static bool isInstDelConfirmActive(int idx) {
  if (instDelConfirmIdx != idx) return false;
  if ((int32_t)(instDelConfirmUntilMs - millis()) <= 0) {
    instDelConfirmIdx = -1;
    return false;
  }
  return true;
}

FLASHMEM bool isAnyInstDelConfirmActive() {
  return instDelConfirmIdx >= 0 && isInstDelConfirmActive(instDelConfirmIdx);
}

FLASHMEM void cancelInstDelConfirm() {
  instDelConfirmIdx = -1;
}

FLASHMEM static bool isInstActionEnabled(int slotIdx, int actCol) {
  bool empty = fmSynthIsSlotEmpty(slotIdx);
  if (actCol == INST_ROW_ACT_COPY) return !empty;
  if (actCol == INST_ROW_ACT_PASTE) return fmSynthHasClipboard();
  if (actCol == INST_ROW_ACT_NEW) return true;
  if (actCol == INST_ROW_ACT_CLR) return !empty;
  return false;
}

FLASHMEM static int defaultActionForSlot(int slotIdx) {
  for (int c = 0; c < INST_ROW_ACT_COUNT; c++) {
    if (isInstActionEnabled(slotIdx, c)) return c;
  }
  return INST_ROW_ACT_NEW;
}

FLASHMEM static int stepInstActionCol(int slotIdx, int curCol, int dx) {
  if (curCol < 0 || curCol >= INST_ROW_ACT_COUNT || !isInstActionEnabled(slotIdx, curCol)) {
    return defaultActionForSlot(slotIdx);
  }
  int step = (dx > 0) ? 1 : -1;
  for (int c = curCol + step; c >= 0 && c < INST_ROW_ACT_COUNT; c += step) {
    if (isInstActionEnabled(slotIdx, c)) {
      return c;
    }
  }
  return curCol;
}

FLASHMEM static const char *instrumentsHeaderTitle() {
  return "INSTR";
}

FLASHMEM static void instrumentsOnEnter() {
  cancelInstDelConfirm();
  syncInstListCursor();
  instCol = defaultActionForSlot(instListCursor);
}

FLASHMEM static void instrumentsInvalidateCache() {
  memset(prevInstListKey, 0, sizeof(prevInstListKey));
  invalidateInstBrowserCache();
}

FLASHMEM static void enterSlotCreateOrEdit(int slotIdx) {
  cancelInstDelConfirm();
  instListCursor = constrain(slotIdx, 0, NUM_INSTRUMENTS - 1);
  if (fmSynthIsSlotEmpty(instListCursor)) {
    instTypeCursor = 0;
    setPage(Page::InstrumentType);
  } else {
    setPage(Page::InstrumentEdit);
  }
}

FLASHMEM static void instrumentsDrawPage(bool full) {
  if (isSaveKeyboardOpen()) {
    drawSaveKeyboardModal(full);
    return;
  }
  if (isInstBrowserOpen()) {
    drawInstBrowserModal(full);
    return;
  }

  syncInstListCursor();
  const int curIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const bool hasClip = fmSynthHasClipboard();

  // 8-Row Instrument Table
  for (int r = 0; r < INST_LIST_VISIBLE_ROWS; r++) {
    int y = SY(28) + r * SY(26);
    int w = SX(308), h = SY(24);
    int idx = r;
    if (idx >= NUM_INSTRUMENTS) continue;

    const FmPatch &p = fmSynthPatch(idx);
    bool empty = fmSynthIsSlotEmpty(idx);
    bool hasCursor = (idx == curIdx);
    bool isDelRow = isInstDelConfirmActive(idx);
    if (hasCursor && (instCol < 0 || instCol >= INST_ROW_ACT_COUNT || !isInstActionEnabled(idx, instCol))) {
      instCol = defaultActionForSlot(idx);
    }
    int rowActionCol = hasCursor ? instCol : -1;

    uint16_t bg = isDelRow  ? COLOR_REC_DIM
                : hasCursor ? DARK
                            : PANEL;
    uint16_t border = isDelRow  ? COLOR_REC
                    : hasCursor ? ACCENT
                    : !empty    ? ACCENT_DARK
                                : MID;
    uint16_t barCol = isDelRow  ? COLOR_REC
                    : hasCursor ? ACCENT
                    : !empty    ? COLOR_PLAY
                                : DIM;

    const char *typeStr = empty ? "" : instTypeName((InstType)p.type);

    uint64_t lk = hashStr(hashInit(), empty ? "---" : p.name);
    lk = hashStr(lk, typeStr);
    lk = hashMix(lk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                         ((uint64_t)idx << 48) | ((uint64_t)(rowActionCol + 2) << 56) |
                         ((uint64_t)empty << 60) | ((uint64_t)isDelRow << 61) |
                         ((uint64_t)hasClip << 62));
    if (full || lk != prevInstListKey[r]) {
      prevInstListKey[r] = lk;
      Surface s(w, h);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), h - 2 * PX, barCol);

      static const int pillW[INST_ROW_ACT_COUNT] = {SX(40), SX(44), SX(38), SX(42)};
      int pillX[INST_ROW_ACT_COUNT] = {};
      int firstPillX = w - SX(6);
      if (hasCursor) {
        int curRight = w - SX(6);
        for (int c = INST_ROW_ACT_COUNT - 1; c >= 0; c--) {
          if (!isInstActionEnabled(idx, c)) continue;
          curRight -= pillW[c];
          pillX[c] = curRight;
          firstPillX = curRight;
          curRight -= SX(4);
        }
      }

      char leftTxt[32];
      snprintf(leftTxt, sizeof(leftTxt), "I%02d  %s", idx + 1, empty ? "---" : p.name);
      if (hasCursor) {
        int maxTxtW = max(SX(60), firstPillX - SX(14));
        while (textWidth(leftTxt) > maxTxtW && strlen(leftTxt) > 5) {
          leftTxt[strlen(leftTxt) - 1] = '\0';
        }
      }
      uint16_t nameCol = empty ? (hasCursor ? LIGHT : DIM) : (hasCursor ? INK : ACCENT);
      s.drawText(SX(10), TY(h), leftTxt, nameCol);

      if (hasCursor) {
        const int pillY = SY(4), pillH = SY(16);

        for (int c = 0; c < INST_ROW_ACT_COUNT; c++) {
          if (!isInstActionEnabled(idx, c)) continue;
          bool sel = (rowActionCol == c);
          const char *lbl = "";
          if (c == INST_ROW_ACT_COPY) {
            lbl = "COPY";
          } else if (c == INST_ROW_ACT_PASTE) {
            lbl = "PASTE";
          } else if (c == INST_ROW_ACT_NEW) {
            lbl = "NEW";
          } else {
            lbl = isDelRow ? "SURE?" : "CLR";
          }

          bool isDelPill = (c == INST_ROW_ACT_CLR && isDelRow);
          uint16_t pBg = sel ? (isDelPill ? COLOR_REC : ACCENT) : (isDelPill ? COLOR_REC_DIM : PANEL);
          uint16_t pFg = sel ? BG : (isDelPill ? COLOR_REC : LIGHT);
          uint16_t pBd = sel ? INK : (isDelPill ? COLOR_REC : MID);

          s.drawPill(pillX[c], pillY, pillW[c], pillH, lbl, pFg, pBg, pBd);
        }
      } else if (!empty) {
        int tw = textWidth(typeStr);
        s.drawText(w - SX(8) - tw, TY(h), typeStr, COLOR_PLAY);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM static void instrumentsOnShiftNavigate(uint8_t dir) {
  cancelInstDelConfirm();
  if (dir & BTN_LEFT) {
    setPage(Page::Midi);
  } else if (dir & BTN_RIGHT) {
    enterSlotCreateOrEdit(instListCursor);
  } else if (dir & BTN_UP) {
    setPage(lastRow1Page());
  } else if (dir & BTN_DOWN) {
    setPage(Page::Master);
  }
}

FLASHMEM static void instrumentsOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  cancelInstDelConfirm();

  if (dy != 0) {
    int nextIdx = constrain(instListCursor + dy, 0, NUM_INSTRUMENTS - 1);
    if (nextIdx != instListCursor) {
      instListCursor = nextIdx;
      instCol = defaultActionForSlot(instListCursor);
      syncInstListCursor();
    }
  }

  if (dx != 0) {
    instCol = stepInstActionCol(instListCursor, instCol, dx);
  }

  uiForceRedraw();
}

FLASHMEM static void instrumentsOnOptAdjust(int dx, int dy, int delta) {
  (void)delta;
  cancelInstDelConfirm();
  if (dy != 0) {
    instListCursor = (dy > 0) ? 0 : (NUM_INSTRUMENTS - 1);
    instCol = defaultActionForSlot(instListCursor);
    syncInstListCursor();
    uiForceRedraw();
  } else if (dx != 0) {
    instCol = stepInstActionCol(instListCursor, instCol, dx);
    uiForceRedraw();
  }
}

FLASHMEM static void triggerCopySlot(int targetIdx) {
  cancelInstDelConfirm();
  if (fmSynthCopySlot(targetIdx)) {
    char msg[32];
    snprintf(msg, sizeof(msg), "COPIED I%02d", targetIdx + 1);
    uiShowToast(msg);
  } else {
    uiShowToast("CANNOT COPY EMPTY SLOT");
  }
  uiForceRedraw();
}

FLASHMEM static void triggerPasteSlot(int targetIdx) {
  cancelInstDelConfirm();
  if (fmSynthPasteSlot(targetIdx)) {
    saveSessionWhenIdle();
    instCol = defaultActionForSlot(targetIdx);
    char msg[32];
    snprintf(msg, sizeof(msg), "PASTED TO I%02d", targetIdx + 1);
    uiShowToast(msg);
  } else {
    uiShowToast("NOTHING COPIED");
  }
  uiForceRedraw();
}

FLASHMEM static void triggerDeleteSlot(int targetIdx) {
  if (fmSynthIsSlotEmpty(targetIdx)) {
    cancelInstDelConfirm();
    uiShowToast("SLOT ALREADY EMPTY");
    uiForceRedraw();
    return;
  }
  if (isInstDelConfirmActive(targetIdx)) {
    cancelInstDelConfirm();
    fmSynthDeleteSlot(targetIdx);
    saveSessionWhenIdle();
    instCol = defaultActionForSlot(targetIdx);
    char msg[32];
    snprintf(msg, sizeof(msg), "CLEARED I%02d", targetIdx + 1);
    uiShowToast(msg);
    uiForceRedraw();
  } else {
    instDelConfirmIdx = targetIdx;
    instDelConfirmUntilMs = millis() + 3000;
    char msg[36];
    snprintf(msg, sizeof(msg), "CLR I%02d? EDIT AGAIN", targetIdx + 1);
    uiShowToast(msg, 3000);
    uiForceRedraw();
  }
}

FLASHMEM static void instrumentsOnEditPress(bool shiftHeld) {
  (void)shiftHeld;
  int targetIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  if (instCol < 0 || instCol >= INST_ROW_ACT_COUNT || !isInstActionEnabled(targetIdx, instCol)) {
    return;
  }

  switch (instCol) {
    case INST_ROW_ACT_COPY:
      triggerCopySlot(targetIdx);
      break;
    case INST_ROW_ACT_PASTE:
      triggerPasteSlot(targetIdx);
      break;
    case INST_ROW_ACT_NEW:
      cancelInstDelConfirm();
      instListCursor = targetIdx;
      instTypeCursor = 0;
      setPage(Page::InstrumentType);
      break;
    case INST_ROW_ACT_CLR:
      triggerDeleteSlot(targetIdx);
      break;
  }
}

FLASHMEM static void instrumentsOnResetParam() {
  int targetIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  if (!fmSynthIsSlotEmpty(targetIdx)) {
    instCol = INST_ROW_ACT_CLR;
    triggerDeleteSlot(targetIdx);
  }
}

PROGMEM static const PageOps INSTRUMENTS_PAGE_OPS = {
  Page::Instruments,
  instrumentsHeaderTitle,
  instrumentsOnEnter,
  instrumentsInvalidateCache,
  instrumentsDrawPage,
  instrumentsOnShiftNavigate,
  instrumentsOnCursorMove,
  instrumentsOnOptAdjust,
  instrumentsOnEditPress,
  instrumentsOnResetParam
};

FLASHMEM const PageOps &pageInstrumentsOps() {
  return INSTRUMENTS_PAGE_OPS;
}

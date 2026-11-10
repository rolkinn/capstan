// page_instrument_type.cpp — New Instrument Type Selection Page UI & Input
#include "page_instrument_type.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

using namespace m8;
using namespace looper;

static uint64_t prevInstTypeSectionKey = 0;
static uint64_t prevInstTypeKey[INST_TYPE_ROW_COUNT] = {};
static uint64_t prevTypePrstBannerKey = 0;
static uint64_t prevTypePrstRowKey[INST_BROWSER_VISIBLE_ROWS] = {};

FLASHMEM static const char *instTypeHeaderTitle() {
  return "TYPE";
}

FLASHMEM static void instTypeInvalidateCache() {
  prevInstTypeSectionKey = 0;
  memset(prevInstTypeKey, 0, sizeof(prevInstTypeKey));
  prevTypePrstBannerKey = 0;
  memset(prevTypePrstRowKey, 0, sizeof(prevTypePrstRowKey));
}

FLASHMEM static void drawTypePresetModal(bool full) {
  const bool globalMode = isTypePresetGlobal();

  // 1. Header Banner: [SONG PRESETS] or [GLOBAL PRESETS]
  {
    const char *leftTxt = globalMode ? "GLOBAL PRESETS" : "SONG PRESETS";
    uint64_t bk = hashStr(hashInit(), leftTxt);
    if (full || bk != prevTypePrstBannerKey) {
      prevTypePrstBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), leftTxt, ACCENT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Presets Table
  const int total = instBrowserEntryCount();
  const int cur = instBrowserCursor();
  const int scroll = instBrowserScroll();

  for (int r = 0; r < INST_BROWSER_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const InstBrowserEntry *ent = instBrowserEntryAt(idx);
    const bool sel = (ent != nullptr) && (idx == cur);

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = !ent ? DIM : (sel ? ACCENT : COLOR_PLAY);

    const char *emptyMsg = globalMode ? "NO GLOBAL PRESETS" : "NO SONG PRESETS";
    const char *nameTxt = ent ? ent->instName : ((total == 0 && r == 0) ? emptyMsg : "");
    const char *typeTxt = ent ? instTypeName((InstType)ent->instType) : "";

    uint64_t rk = hashStr(hashInit(), nameTxt);
    rk = hashStr(rk, typeTxt);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)sel << 48) | ((uint64_t)r << 52));
    if (full || rk != prevTypePrstRowKey[r]) {
      prevTypePrstRowKey[r] = rk;
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
        s.drawPill(px, py, pw, ph, "LOAD", pFg, pBg, pBd);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM static void instTypeDrawPage(bool full) {
  if (isTypePresetModalOpen()) {
    drawTypePresetModal(full);
    return;
  }

  const int slotIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  instTypeCursor = constrain(instTypeCursor, 0, INST_TYPE_ROW_COUNT - 1);

  // 1. Section Header: NEW INSTRUMENT
  {
    char headerTxt[36];
    snprintf(headerTxt, sizeof(headerTxt), "NEW INSTRUMENT: SLOT I%02d", slotIdx + 1);
    uint64_t sk = hashStr(hashInit(), headerTxt);
    if (full || sk != prevInstTypeSectionKey) {
      prevInstTypeSectionKey = sk;
      const int secW = SX(308), secH = SY(12);
      Surface s(secW, secH);
      s.fillRect(0, 0, secW, secH, BG);
      s.drawText(SX(2), TY(secH), headerTxt, LIGHT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Preset Load Options + Available Instrument Types Table
  for (int i = 0; i < INST_TYPE_ROW_COUNT; i++) {
    const char *rowLabel = "---";
    if (i == INST_TYPE_ROW_LOAD_SONG) {
      rowLabel = "Load song preset...";
    } else if (i == INST_TYPE_ROW_LOAD_GLOBAL) {
      rowLabel = "Load global preset...";
    } else {
      const InstEngineOps *ops = instEngineAt(i - INST_TYPE_ROW_FIRST_ENG);
      if (ops) {
        rowLabel = (ops->type == InstType::NoiseDrone) ? "NOISE TEXTURE DRONE" : ops->typeName;
      }
    }

    int y = SY(40) + i * SY(24);
    const int rw = SX(308), rh = SY(22);
    bool sel = (instTypeCursor == i);

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = DIM;
    uint16_t idxCol = sel ? INK : DIM;
    uint16_t nameCol = sel ? ACCENT : LIGHT;

    uint64_t rk = hashStr(hashInit(), rowLabel);
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)nameCol << 32) |
                         ((uint64_t)i << 48) | ((uint64_t)sel << 56));
    if (full || rk != prevInstTypeKey[i]) {
      prevInstTypeKey[i] = rk;
      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      char idxTxt[8];
      snprintf(idxTxt, sizeof(idxTxt), "%02d", i + 1);
      s.drawText(SX(10), TY(rh), idxTxt, idxCol);
      s.drawText(SX(34), TY(rh), rowLabel, nameCol);
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM static void selectInstTypeRowAction() {
  int cur = constrain(instTypeCursor, 0, INST_TYPE_ROW_COUNT - 1);
  if (cur == INST_TYPE_ROW_LOAD_SONG) {
    openTypePresetModal(false);
    inputSwallowModalKeys();
    uiForceRedraw();
    return;
  }
  if (cur == INST_TYPE_ROW_LOAD_GLOBAL) {
    openTypePresetModal(true);
    inputSwallowModalKeys();
    uiForceRedraw();
    return;
  }

  InstType chosen = instTypeAt(cur - INST_TYPE_ROW_FIRST_ENG);
  fmSynthCreateInSlot(instListCursor, chosen);
  saveSessionWhenIdle();
  fmEditRow = (chosen == InstType::Sampler) ? SMP_ROW_KEYS
            : (chosen == InstType::Fm2Op)   ? FM_ROW_OP1_MAIN
                                            : 1;
  fmEditCol = 0;
  instEditScroll = 0;
  syncInstEditScroll();
  setPage(Page::InstrumentEdit);
}

FLASHMEM static void instTypeOnShiftNavigate(uint8_t dir) {
  if (dir & BTN_LEFT) {
    setPage(Page::Instruments);
  } else if (dir & BTN_RIGHT) {
    selectInstTypeRowAction();
  } else if (dir & BTN_UP) {
    setPage(lastRow1Page());
  } else if (dir & BTN_DOWN) {
    setPage(Page::Master);
  }
}

FLASHMEM static void instTypeOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dx;
  (void)dir;
  if (dy != 0) {
    instTypeCursor = constrain(instTypeCursor + dy, 0, INST_TYPE_ROW_COUNT - 1);
    uiForceRedraw();
  }
}

FLASHMEM static void instTypeOnEditPress(bool shiftHeld) {
  (void)shiftHeld;
  selectInstTypeRowAction();
}

PROGMEM static const PageOps INST_TYPE_PAGE_OPS = {
  Page::InstrumentType,
  instTypeHeaderTitle,
  nullptr,
  instTypeInvalidateCache,
  instTypeDrawPage,
  instTypeOnShiftNavigate,
  instTypeOnCursorMove,
  nullptr,
  instTypeOnEditPress,
  nullptr
};

FLASHMEM const PageOps &pageInstrumentTypeOps() {
  return INST_TYPE_PAGE_OPS;
}

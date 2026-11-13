// page_instrument_edit.cpp — Synthesizer & Sampler Parameter Editor Page UI & Input
// Unified scrollable editor per instrument:
//   1. Specific Instrument Controls first (2-Op FM, Phase Dist, Sampler 4-Octave Keyboard, or Noise Drone)
//   2. General Instrument Controls below as the cursor scrolls down (AMP, FLT, FX)
#include "page_instrument_edit.h"
#include "inst_fm2op.h"
#include "inst_sampler.h"
#include "midi_map.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

using namespace m8;
using namespace looper;

static constexpr int FM_MAX_COLS = 6;
static uint64_t prevFmCellKey[FM_ROW_COUNT][FM_MAX_COLS] = {};
static uint64_t prevSectionTitleKey[3] = {};
static bool prevSamplerKbValid = false;
static int prevSamplerKbInst = -1;
static int prevSamplerKbScrollW = -1;
static bool prevSamplerKbRowSel = false;
static int prevSamplerKbSelKey = -1;
static uint64_t prevSamplerKbHasMask = 0;
static uint64_t prevSamplerKbPlayMask = 0;
static uint64_t prevBrowserBannerKey = 0;
static uint64_t prevBrowserRowKey[SAMPLER_BROWSER_VISIBLE_ROWS] = {};
static uint64_t prevInstBrowserBannerKey = 0;
static uint64_t prevInstBrowserRowKey[INST_BROWSER_VISIBLE_ROWS] = {};
static uint64_t prevSaveToBannerKey = 0;
static uint64_t prevSaveToRowKey[2] = {};
static uint64_t prevSmpKeyRowKey[3] = {};
static uint64_t prevSmpKeyRecKey = 0;
static bool smpKeyModalActive = false;
static bool smpKeyRecModeActive = false;
static int smpKeyModalCursor = 0;   // 0 = Load from file, 1 = Record from audio in
static int smpKeyRecModeCursor = 0; // 0 = ThreshAutoStartStop, 1 = ThreshAutoManualStop, 2 = ManualStartStop
static int smpKeyModalInst = 0;
static int smpKeyModalKey = 0;
static uint8_t samplerSelectedKey[NUM_INSTRUMENTS] = {};
static uint8_t samplerKbScrollWhite[NUM_INSTRUMENTS] = {};
static int prevDrawnInstIdx = -1;
static uint8_t prevDrawnInstType = 0;
static int prevDrawnScroll = -1;

// Per-Instrument MIDI CC Mapping Subview State (SHIFT + RIGHT from Instrument Edit)
static constexpr int INST_MIDI_MAP_VISIBLE_ROWS = 7;
static constexpr uint8_t INST_MAP_ZONE_LIST = 0;
static constexpr uint8_t INST_MAP_ZONE_CLR  = 1;
static bool instMidiMapOpen = false;
static uint8_t instMidiMapZone = INST_MAP_ZONE_LIST;
static int instMidiMapCursor = 0;
static int instMidiMapScroll = 0;
static int instMidiMapCol = 0;
static bool instMidiMapClrConfirm = false;
static uint32_t instMidiMapClrConfirmUntilMs = 0;
static uint64_t prevInstMidiMapBannerKey = 0;
static uint64_t prevInstMidiMapRowKey[INST_MIDI_MAP_VISIBLE_ROWS] = {};

FLASHMEM static const char *instEditHeaderTitle() {
  const InstEngineOps *ops = instEngineForType((InstType)fmSynthPatch(instListCursor).type);
  return ops ? ops->shortBadge : "INST";
}

FLASHMEM static void instEditOnEnter() {
  instMidiMapCancelLearn();
  instMidiMapOpen = false;
  instMidiMapClrConfirm = false;
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const InstType iType = (InstType)fmSynthPatch(instIdx).type;
  fmEditRow = constrain(fmEditRow, 0, instEditRowCount() - 1);
  syncInstEditScroll();
  if (iType == InstType::Sampler && fmEditRow == SMP_ROW_KEYS) {
    fmEditCol = constrain((int)samplerSelectedKey[instIdx], 0, SAMPLER_NUM_KEYS - 1);
  } else {
    fmEditCol = constrain(fmEditCol, 0, fmRowColCount(fmEditRow) - 1);
  }
}

FLASHMEM void invalidateInstBrowserCache() {
  prevInstBrowserBannerKey = 0;
  memset(prevInstBrowserRowKey, 0, sizeof(prevInstBrowserRowKey));
}

FLASHMEM static void instEditInvalidateCache() {
  memset(prevFmCellKey, 0, sizeof(prevFmCellKey));
  memset(prevSectionTitleKey, 0, sizeof(prevSectionTitleKey));
  prevSamplerKbValid = false;
  prevBrowserBannerKey = 0;
  memset(prevBrowserRowKey, 0, sizeof(prevBrowserRowKey));
  prevSaveToBannerKey = 0;
  memset(prevSaveToRowKey, 0, sizeof(prevSaveToRowKey));
  memset(prevSmpKeyRowKey, 0, sizeof(prevSmpKeyRowKey));
  prevSmpKeyRecKey = 0;
  prevInstMidiMapBannerKey = 0;
  memset(prevInstMidiMapRowKey, 0, sizeof(prevInstMidiMapRowKey));
  prevDrawnInstIdx = -1;
  prevDrawnInstType = 0;
  prevDrawnScroll = -1;
  invalidateInstBrowserCache();
}

FLASHMEM static void blitBgStrip(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  const int maxStepH = SY(24);
  while (h > 0) {
    int stepH = min(h, maxStepH);
    Surface s(w, stepH);
    s.fillRect(0, 0, w, stepH, BG);
    s.blit(x, y);
    y += stepH;
    h -= stepH;
  }
}

FLASHMEM static void drawSectionTitle(int slotIdx, int y, int h, const char *title, uint16_t color, bool force) {
  uint64_t tk = hashStr(hashInit(), title ? title : "");
  tk = hashMix(tk, (uint64_t)color | ((uint64_t)y << 16) | ((uint64_t)h << 32));
  if (!force && tk == prevSectionTitleKey[slotIdx]) return;
  prevSectionTitleKey[slotIdx] = tk;
  Surface s(SX(308), h);
  s.fillRect(0, 0, SX(308), h, BG);
  if (title && title[0]) {
    s.drawText(0, 0, title, color);
  }
  s.blit(SX(6), y);
}

FLASHMEM static void drawFmParamCell(int r, int c, int x, int y, int w, int h, int gapAfterW,
                            const char *label, const char *valStr,
                            int barVal, int barMax, uint16_t accentCol, bool full) {
  bool sel = (fmEditRow == r && fmEditCol == c);
  bool editHeld = sel && inputEditHeld();
  uint16_t bg = sel ? DARK : PANEL;
  uint16_t border = sel ? (editHeld ? ACCENT : INK) : DARK;

  uint64_t ck = hashStr(hashInit(), label);
  ck = hashStr(ck, valStr);
  ck = hashMix(ck, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)accentCol << 32) | ((uint64_t)barVal << 48));
  ck = hashMix(ck, (uint64_t)y | ((uint64_t)h << 16) | ((uint64_t)gapAfterW << 32));
  if (!full && ck == prevFmCellKey[r][c]) return;
  prevFmCellKey[r][c] = ck;

  Surface s(w + gapAfterW, h);
  if (gapAfterW > 0) {
    s.fillRect(w, 0, gapAfterW, h, BG);
  }
  s.fillRect(0, 0, w, PX, border);
  s.fillRect(0, h - PX, w, PX, border);
  s.fillRect(0, PX, PX, h - 2 * PX, border);
  s.fillRect(w - PX, PX, PX, h - 2 * PX, border);
  s.fillRect(PX, PX, w - 2 * PX, h - 2 * PX, bg);
  const int padX = (w <= SX(52)) ? SX(2) : SX(4);
  s.drawText(padX, SY(3), label, sel ? LIGHT : DIM);
  int vw = textWidth(valStr);
  s.drawText(max(padX, w - padX - vw), SY(14), valStr, sel ? INK : accentCol);

  if (barMax > 0) {
    int trackW = w - 2 * padX;
    int barY = h - SY(6);
    int barH = SY(3);
    s.fillRect(padX, barY, trackW, barH, BG);
    int fillW = constrain(barVal * trackW / barMax, 0, trackW);
    if (fillW > 0) s.fillRect(padX, barY, fillW, barH, accentCol);
  }
  s.blit(x, y);
}

FLASHMEM static uint8_t getEnvField(const InstEnvParams &env, int col) {
  switch (col) {
    case INST_ENV_AMT: default: return env.amt;
    case INST_ENV_ATK: return env.attack;
    case INST_ENV_DEC: return env.decay;
    case INST_ENV_SUS: return env.sustain;
    case INST_ENV_REL: return env.release;
  }
}

FLASHMEM static void setEnvField(InstEnvParams &env, int col, uint8_t val) {
  val = (uint8_t)constrain((int)val, 0, (int)INST_ENV_MAX);
  switch (col) {
    case INST_ENV_AMT: env.amt = val; break;
    case INST_ENV_ATK: env.attack = val; break;
    case INST_ENV_DEC: env.decay = val; break;
    case INST_ENV_SUS: env.sustain = val; break;
    case INST_ENV_REL: env.release = val; break;
  }
}

FLASHMEM static uint8_t getFxField(const InstFxParams &fx, int col) {
  switch (col) {
    case INST_FX_DRIVE:  default: return fx.drive;
    case INST_FX_CHORUS: return fx.chorus;
    case INST_FX_DELAY:  return fx.delay;
    case INST_FX_DFB:    return fx.delayFeedback;
    case INST_FX_REVERB: return fx.reverb;
  }
}

FLASHMEM static void setFxField(InstFxParams &fx, int col, uint8_t val) {
  val = (uint8_t)constrain((int)val, 0, (int)INST_FX_MAX);
  switch (col) {
    case INST_FX_DRIVE:  fx.drive = val; break;
    case INST_FX_CHORUS: fx.chorus = val; break;
    case INST_FX_DELAY:  fx.delay = val; break;
    case INST_FX_DFB:    fx.delayFeedback = val; break;
    case INST_FX_REVERB: fx.reverb = val; break;
  }
}

PROGMEM static const char *const ENV_COL_LABELS[INST_ENV_COUNT] = {
  "AMT", "ATK", "DEC", "SUS", "REL"
};

PROGMEM static const char *const FX_COL_LABELS[INST_FX_COUNT] = {
  "DRIVE", "CHORUS", "DELAY", "D.FB", "REVERB"
};

FLASHMEM static void formatEnvCellValue(int col, uint8_t v, char *outVal, size_t valSz) {
  if (col == INST_ENV_ATK) {
    snprintf(outVal, valSz, "%u", (unsigned)instEnvAttackMs(v));
  } else if (col == INST_ENV_DEC || col == INST_ENV_REL) {
    snprintf(outVal, valSz, "%u", (unsigned)instEnvDecayReleaseMs(v));
  } else {
    snprintf(outVal, valSz, "%u", (unsigned)v);
  }
}

FLASHMEM static void drawFmAlgoToggle(uint8_t algo, bool full) {
  const int x = SX(6), y = SY(54), w = SX(96), h = SY(24);
  const bool sel = (fmEditRow == FM_ROW_ALGO);
  const bool editHeld = sel && inputEditHeld();
  const uint16_t bg = sel ? DARK : PANEL;
  const uint16_t border = sel ? (editHeld ? ACCENT : INK) : DARK;

  uint64_t ck = hashMix(hashInit(), (uint64_t)bg | ((uint64_t)border << 16) |
                                        ((uint64_t)algo << 32) | ((uint64_t)sel << 40));
  if (!full && ck == prevFmCellKey[FM_ROW_ALGO][0]) return;
  prevFmCellKey[FM_ROW_ALGO][0] = ck;

  Surface s(w, h);
  s.drawBox(bg, border);
  s.drawText(SX(6), TY(h), "ALG", sel ? LIGHT : DIM);

  const int opW = SX(18), opH = SY(16);
  const int opY = (h - opH) / 2;
  const int leftX = SX(38);
  const int rightX = SX(72);

  if (algo == (uint8_t)FmAlgo::Fm) {
    // Algorithm 2: Two small boxes with an arrow between the two: [B] > [A]
    s.drawPill(leftX, opY, opW, opH, "B", sel ? INK : COLOR_MIDI, BG, COLOR_MIDI);
    int arrowW = textWidth(">");
    int gapX = leftX + opW;
    int gapW = rightX - gapX;
    s.drawText(gapX + max(0, (gapW - arrowW) / 2), TY(h), ">", sel ? INK : LIGHT);
    s.drawPill(rightX, opY, opW, opH, "A", sel ? INK : ACCENT, BG, ACCENT);
  } else if (algo == (uint8_t)FmAlgo::FmAB) {
    // Algorithm 3: Two small boxes with an arrow between the two: [A] > [B]
    s.drawPill(leftX, opY, opW, opH, "A", sel ? INK : ACCENT, BG, ACCENT);
    int arrowW = textWidth(">");
    int gapX = leftX + opW;
    int gapW = rightX - gapX;
    s.drawText(gapX + max(0, (gapW - arrowW) / 2), TY(h), ">", sel ? INK : LIGHT);
    s.drawPill(rightX, opY, opW, opH, "B", sel ? INK : COLOR_MIDI, BG, COLOR_MIDI);
  } else {
    // Algorithm 1: Two independent small boxes labeled A and B: [A]   [B]
    s.drawPill(leftX, opY, opW, opH, "A", sel ? INK : ACCENT, BG, ACCENT);
    s.drawPill(rightX, opY, opW, opH, "B", sel ? INK : COLOR_MIDI, BG, COLOR_MIDI);
  }
  s.blit(x, y);
}

// ============================================================================
// Sampler Specific View: PRESS Toggle, Key Sample Status & Scrollable 4-Octave Keyboard (C3..B6)
// ============================================================================
static constexpr int SAMPLER_TOTAL_WHITE_KEYS = 28;   // 4 octaves (C3..B6) * 7 white keys
static constexpr int SAMPLER_VISIBLE_WHITE_KEYS = 11; // 11 white keys visible across SX(308)

PROGMEM static const uint8_t WHITE_SEMI_OFFSETS[7] = {
  0, 2, 4, 5, 7, 9, 11
};

PROGMEM static const uint8_t SEMI_TO_LEFT_WHITE_DEG[12] = {
  0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6
};

static inline int whiteKeyToKeyIdx(int wGlobal) {
  return (wGlobal / 7) * 12 + (int)WHITE_SEMI_OFFSETS[wGlobal % 7];
}

FLASHMEM static void drawSamplerPressAndKeyStatus(int instIdx, uint8_t pressMode, int curKey, bool full) {
  const int y = SY(54), h = SY(24);

  // 1. Left Box: Cycling Toggle [PRESS: STOP | ONCE | REP | REPTHRU]
  {
    const int x = SX(6), w = SX(128);
    const bool sel = (fmEditRow == SMP_ROW_PRESS);
    const bool editHeld = sel && inputEditHeld();
    const uint16_t bg = sel ? DARK : PANEL;
    const uint16_t border = sel ? (editHeld ? ACCENT : INK) : MID;
    const char *modeStr = samplerPressModeName(pressMode);

    uint64_t ck = hashStr(hashInit(), modeStr);
    ck = hashMix(ck, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)sel << 32));
    if (full || ck != prevFmCellKey[SMP_ROW_PRESS][0]) {
      prevFmCellKey[SMP_ROW_PRESS][0] = ck;
      Surface s(w, h);
      s.drawBox(bg, border);
      s.drawText(SX(6), TY(h), "PRESS", sel ? INK : LIGHT);

      const int pw = SX(68), ph = SY(16);
      const int px = w - SX(4) - pw, py = (h - ph) / 2;
      uint16_t pBg = sel ? ACCENT : DARK;
      uint16_t pFg = sel ? BG : ACCENT;
      uint16_t pBd = sel ? INK : ACCENT;
      s.drawPill(px, py, pw, ph, modeStr, pFg, pBg, pBd);
      s.blit(x, y);
    }
  }

  // 2. Right Box: Selected Key & Connected Sample Filename ("C4: KICK.WAV" or "C4: ---")
  {
    const int x = SX(138), w = SX(176);
    const bool hasSmp = samplerHasSample(instIdx, curKey);
    const char *noteStr = samplerKeyNoteName(curKey);
    const char *smpName = samplerSampleDisplayName(instIdx, curKey);

    char infoTxt[32];
    snprintf(infoTxt, sizeof(infoTxt), "%s: %s", noteStr, smpName);
    while (textWidth(infoTxt) > w - SX(12) && strlen(infoTxt) > 6) {
      infoTxt[strlen(infoTxt) - 1] = '\0';
    }

    uint16_t bg = PANEL;
    uint16_t border = DARK;
    uint64_t ck = hashStr(hashInit(), infoTxt);
    ck = hashMix(ck, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)hasSmp << 32));
    if (full || ck != prevFmCellKey[SMP_ROW_PRESS][1]) {
      prevFmCellKey[SMP_ROW_PRESS][1] = ck;
      Surface s(w, h);
      s.drawBox(bg, border);
      s.drawText(SX(6), TY(h), infoTxt, hasSmp ? ACCENT : DIM);
      s.blit(x, y);
    }
  }
}

static inline void fillStripRectLocal(Surface &s, int stripY0, int x, int y, int rw, int rh, uint16_t col) {
  int y0 = max(stripY0, y) - stripY0;
  int y1 = min(stripY0 + s.h, y + rh) - stripY0;
  if (y0 < y1) {
    s.fillRect(x, y0, rw, y1 - y0, col);
  }
}

static inline void drawStripBoxLocal(Surface &s, int stripY0, int x, int y, int rw, int rh,
                                     uint16_t fill, uint16_t border) {
  fillStripRectLocal(s, stripY0, x, y, rw, rh, border);
  if (rw > 2 * PX && rh > 2 * PX) {
    fillStripRectLocal(s, stripY0, x + PX, y + PX, rw - 2 * PX, rh - 2 * PX, fill);
  }
}

static inline void drawStripTextLocal(Surface &s, int stripY0, int x, int y, const char *txt, uint16_t ink) {
  if (y + FONT_H <= stripY0 || y >= stripY0 + s.h) return;
  s.drawText(x, y - stripY0, txt, ink);
}

FLASHMEM static void drawSamplerKeyboard(int instIdx, int selKey, bool rowSel, bool full) {
  selKey = constrain(selKey, 0, SAMPLER_NUM_KEYS - 1);
  int wLeft = (selKey / 12) * 7 + (int)SEMI_TO_LEFT_WHITE_DEG[selKey % 12];
  int wRight = samplerIsBlackKey(selKey) ? (wLeft + 1) : wLeft;
  int scrollW = (int)samplerKbScrollWhite[instIdx];
  if (wLeft < scrollW) {
    scrollW = wLeft;
  } else if (wRight > scrollW + (SAMPLER_VISIBLE_WHITE_KEYS - 1)) {
    scrollW = wRight - (SAMPLER_VISIBLE_WHITE_KEYS - 1);
  }
  scrollW = constrain(scrollW, 0, SAMPLER_TOTAL_WHITE_KEYS - SAMPLER_VISIBLE_WHITE_KEYS);
  samplerKbScrollWhite[instIdx] = (uint8_t)scrollW;

  const uint64_t hasMask = samplerKeyMask(instIdx);
  const uint64_t playMask = samplerPlayingKeyMask(instIdx);

  const int kbX = SX(6);
  const int kbY = SY(84);
  const int kbW = SX(308); // 11 white keys * SX(28) = SX(308)
  const int kbH = SY(144); // 6 strips * SY(24) = SY(144)
  const int stripH = SY(24);
  const int whiteW = SX(28);
  const int blackW = SX(22);
  const int blackH = SY(86);

  const bool fullKb = full || !prevSamplerKbValid ||
                      (prevSamplerKbInst != instIdx) ||
                      (prevSamplerKbScrollW != scrollW);

  int sliceX0 = 0;
  int sliceX1 = kbW;
  bool dirtyWhiteVis = true;

  if (!fullKb) {
    uint64_t dirtyKeys = (hasMask ^ prevSamplerKbHasMask) | (playMask ^ prevSamplerKbPlayMask);
    if (rowSel != prevSamplerKbRowSel || selKey != prevSamplerKbSelKey) {
      dirtyKeys |= (1ULL << selKey);
      if (prevSamplerKbSelKey >= 0 && prevSamplerKbSelKey < SAMPLER_NUM_KEYS) {
        dirtyKeys |= (1ULL << prevSamplerKbSelKey);
      }
    }
    if (dirtyKeys == 0) return;

    sliceX0 = kbW;
    sliceX1 = 0;
    dirtyWhiteVis = false;

    for (int wVis = 0; wVis < SAMPLER_VISIBLE_WHITE_KEYS; wVis++) {
      int k = whiteKeyToKeyIdx(scrollW + wVis);
      if (dirtyKeys & (1ULL << k)) {
        sliceX0 = min(sliceX0, wVis * whiteW);
        sliceX1 = max(sliceX1, (wVis + 1) * whiteW);
        dirtyWhiteVis = true;
      }
    }
    for (int wVis = 0; wVis < SAMPLER_VISIBLE_WHITE_KEYS - 1; wVis++) {
      int wGlobal = scrollW + wVis;
      int deg = wGlobal % 7;
      if (deg == 2 || deg == 6) continue;
      int k = whiteKeyToKeyIdx(wGlobal) + 1;
      if (dirtyKeys & (1ULL << k)) {
        int bx = (wVis + 1) * whiteW - blackW / 2;
        sliceX0 = min(sliceX0, bx);
        sliceX1 = max(sliceX1, bx + blackW);
      }
    }

    sliceX0 = max(0, sliceX0);
    sliceX1 = min(kbW, sliceX1);
  }

  prevSamplerKbValid = true;
  prevSamplerKbInst = instIdx;
  prevSamplerKbScrollW = scrollW;
  prevSamplerKbRowSel = rowSel;
  prevSamplerKbSelKey = selKey;
  prevSamplerKbHasMask = hasMask;
  prevSamplerKbPlayMask = playMask;

  if (sliceX0 >= sliceX1) return;
  const int sliceW = sliceX1 - sliceX0;

  for (int strip = 0; strip < 6; strip++) {
    const int stripY0 = strip * stripH;
    if (!fullKb && !dirtyWhiteVis && stripY0 >= blackH) {
      continue;
    }
    Surface s(sliceW, stripH);
    s.fillRect(0, 0, sliceW, stripH, BG);

    // 1. Draw visible White Keys overlapping [sliceX0, sliceX1)
    for (int wVis = 0; wVis < SAMPLER_VISIBLE_WHITE_KEYS; wVis++) {
      int wx = wVis * whiteW - sliceX0;
      if (wx + whiteW <= 0 || wx >= sliceW) continue;
      int wGlobal = scrollW + wVis;
      int k = whiteKeyToKeyIdx(wGlobal);
      bool isCur = rowSel && (selKey == k);
      bool isRef = !rowSel && (selKey == k);
      bool isPlay = (playMask & (1ULL << k)) != 0;
      bool hasSmp = (hasMask & (1ULL << k)) != 0;

      uint16_t fill = isCur  ? (isPlay ? COLOR_PLAY : ACCENT)
                    : isPlay ? COLOR_PLAY
                    : hasSmp ? LIGHT
                             : PANEL;
      uint16_t border = isCur ? INK : (isRef ? ACCENT : DARK);
      drawStripBoxLocal(s, stripY0, wx, 0, whiteW, kbH, fill, border);

      if (hasSmp) {
        uint16_t dotCol = (isCur || isPlay) ? BG : ACCENT_DARK;
        int dotW = SX(14), dotH = SY(4);
        fillStripRectLocal(s, stripY0, wx + (whiteW - dotW) / 2, SY(114), dotW, dotH, dotCol);
      }

      const char *lbl = samplerKeyNoteName(k);
      int lw = textWidth(lbl);
      uint16_t txtCol = (isCur || isPlay || hasSmp) ? BG : LIGHT;
      drawStripTextLocal(s, stripY0, wx + max(1, (whiteW - lw) / 2), SY(126), lbl, txtCol);
    }

    // 2. Draw Black Keys overlapping [sliceX0, sliceX1) across top SY(86)
    if (stripY0 < blackH) {
      for (int wVis = 0; wVis < SAMPLER_VISIBLE_WHITE_KEYS - 1; wVis++) {
        int wGlobal = scrollW + wVis;
        int deg = wGlobal % 7;
        if (deg == 2 || deg == 6) continue; // No black key after E or B
        int center = (wVis + 1) * whiteW;
        int bx = center - blackW / 2 - sliceX0;
        if (bx + blackW <= 0 || bx >= sliceW) continue;
        int k = whiteKeyToKeyIdx(wGlobal) + 1;
        bool isCur = rowSel && (selKey == k);
        bool isRef = !rowSel && (selKey == k);
        bool isPlay = (playMask & (1ULL << k)) != 0;
        bool hasSmp = (hasMask & (1ULL << k)) != 0;

        uint16_t fill = isCur  ? (isPlay ? COLOR_PLAY : ACCENT)
                      : isPlay ? COLOR_PLAY
                      : hasSmp ? DARK
                               : BG;
        uint16_t border = isCur ? INK : ((isRef || hasSmp) ? ACCENT : MID);
        drawStripBoxLocal(s, stripY0, bx, 0, blackW, blackH, fill, border);

        if (hasSmp) {
          uint16_t dotCol = (isCur || isPlay) ? BG : ACCENT;
          int dotW = SX(10), dotH = SY(4);
          fillStripRectLocal(s, stripY0, bx + (blackW - dotW) / 2, SY(52), dotW, dotH, dotCol);
        }

        const char *lbl = samplerKeyShortLabel(k);
        int lw = textWidth(lbl);
        uint16_t txtCol = (isCur || isPlay) ? BG : (hasSmp ? ACCENT : DIM);
        drawStripTextLocal(s, stripY0, bx + max(1, (blackW - lw) / 2), SY(64), lbl, txtCol);
      }
    }

    s.blit(kbX + sliceX0, kbY + stripY0);
    keepStreaming();
  }
}

FLASHMEM void drawSamplerBrowserModal(bool full) {
  if (!isSamplerBrowserOpen()) return;

  const int keyIdx = samplerBrowserTargetKey();
  const char *noteStr = samplerKeyNoteName(keyIdx);
  const char *curDir = samplerBrowserCurrentDir();

  // 1. Header Banner: [C4 SAMPLE] and current directory path
  {
    char leftTxt[20];
    snprintf(leftTxt, sizeof(leftTxt), "%s SAMPLE:", noteStr);

    const char *dirDisp = curDir;
    int maxDirW = SX(308) - SX(16) - textWidth(leftTxt);
    while (*dirDisp && textWidth(dirDisp) > maxDirW) {
      dirDisp++;
    }

    uint64_t bk = hashStr(hashInit(), leftTxt);
    bk = hashStr(bk, dirDisp);
    if (full || bk != prevBrowserBannerKey) {
      prevBrowserBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), leftTxt, ACCENT);
      int dw = textWidth(dirDisp);
      s.drawText(max(SX(80), bw - SX(6) - dw), TY(bh), dirDisp, LIGHT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Directory & .WAV File List
  const int total = samplerBrowserEntryCount();
  const int cur = samplerBrowserCursor();
  const int scroll = samplerBrowserScroll();

  for (int r = 0; r < SAMPLER_BROWSER_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const SamplerBrowserEntry *ent = samplerBrowserEntryAt(idx);
    const bool sel = (ent != nullptr) && (idx == cur);

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? ACCENT : MID;
    uint16_t barCol = !ent ? DIM : (sel ? ACCENT : (ent->isDir ? COLOR_MIDI : COLOR_PLAY));

    uint64_t rk = hashStr(hashInit(), ent ? ent->name : "");
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)sel << 48) | ((uint64_t)r << 52) |
                     ((uint64_t)(ent ? ent->isDir : 2) << 56) | ((uint64_t)(total == 0) << 58));
    if (full || rk != prevBrowserRowKey[r]) {
      prevBrowserRowKey[r] = rk;
      char rowTxt[56] = "";
      const char *pillLbl = nullptr;
      if (ent) {
        if (ent->isDir) {
          if (strcmp(ent->name, "..") == 0) {
            snprintf(rowTxt, sizeof(rowTxt), "[..]  PARENT FOLDER");
          } else {
            snprintf(rowTxt, sizeof(rowTxt), "[DIR] %.42s", ent->name);
          }
          pillLbl = "OPEN";
        } else {
          snprintf(rowTxt, sizeof(rowTxt), "%.42s", ent->name);
          pillLbl = "LOAD";
        }
      } else if (total == 0 && r == 0) {
        snprintf(rowTxt, sizeof(rowTxt), "NO .WAV FILES IN FOLDER");
      }
      int maxRowTxtW = rw - (sel ? SX(64) : SX(16));
      while (textWidth(rowTxt) > maxRowTxtW && strlen(rowTxt) > 4) {
        rowTxt[strlen(rowTxt) - 1] = '\0';
      }

      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      if (rowTxt[0] != '\0') {
        uint16_t fg = !ent ? DIM : (sel ? INK : (ent->isDir ? COLOR_MIDI : LIGHT));
        s.drawText(SX(10), TY(rh), rowTxt, fg);
      }

      if (sel && pillLbl) {
        const int pw = SX(48), ph = SY(16);
        const int px = rw - SX(6) - pw, py = (rh - ph) / 2;
        s.drawPill(px, py, pw, ph, pillLbl, BG, ACCENT, INK);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM void drawInstBrowserModal(bool full) {
  if (!isInstBrowserOpen()) return;

  const int slotIdx = instBrowserTargetSlot();
  const char *curDir = instBrowserCurrentDir();

  // 1. Header Banner: [I01 LOAD:] and current directory path
  {
    char leftTxt[20];
    snprintf(leftTxt, sizeof(leftTxt), "I%02d LOAD:", slotIdx + 1);

    const char *dirDisp = curDir;
    int maxDirW = SX(308) - SX(16) - textWidth(leftTxt);
    while (*dirDisp && textWidth(dirDisp) > maxDirW) {
      dirDisp++;
    }

    uint64_t bk = hashStr(hashInit(), leftTxt);
    bk = hashStr(bk, dirDisp);
    if (full || bk != prevInstBrowserBannerKey) {
      prevInstBrowserBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(6), TY(bh), leftTxt, ACCENT);
      int dw = textWidth(dirDisp);
      s.drawText(max(SX(72), bw - SX(6) - dw), TY(bh), dirDisp, LIGHT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Directory & .FMI Instrument File List
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
    uint16_t barCol = !ent ? DIM : (sel ? ACCENT : (ent->isDir ? COLOR_MIDI : COLOR_PLAY));

    uint64_t rk = hashStr(hashInit(), ent ? ent->name : "");
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)sel << 48) | ((uint64_t)r << 52) |
                     ((uint64_t)(ent ? ent->isDir : 2) << 56) | ((uint64_t)(total == 0) << 58));
    if (full || rk != prevInstBrowserRowKey[r]) {
      prevInstBrowserRowKey[r] = rk;
      char rowTxt[56] = "";
      const char *pillLbl = nullptr;
      if (ent) {
        if (ent->isDir) {
          if (strcmp(ent->name, "..") == 0) {
            snprintf(rowTxt, sizeof(rowTxt), "[..]  PARENT FOLDER");
          } else {
            snprintf(rowTxt, sizeof(rowTxt), "[DIR] %.42s", ent->name);
          }
          pillLbl = "OPEN";
        } else {
          snprintf(rowTxt, sizeof(rowTxt), "%.42s", ent->name);
          pillLbl = "LOAD";
        }
      } else if (total == 0 && r == 0) {
        snprintf(rowTxt, sizeof(rowTxt), "NO .FMI FILES IN FOLDER");
      }
      int maxRowTxtW = rw - (sel ? SX(64) : SX(16));
      while (textWidth(rowTxt) > maxRowTxtW && strlen(rowTxt) > 4) {
        rowTxt[strlen(rowTxt) - 1] = '\0';
      }

      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      if (rowTxt[0] != '\0') {
        uint16_t fg = !ent ? DIM : (sel ? INK : (ent->isDir ? COLOR_MIDI : LIGHT));
        s.drawText(SX(10), TY(rh), rowTxt, fg);
      }

      if (sel && pillLbl) {
        const int pw = SX(48), ph = SY(16);
        const int px = rw - SX(6) - pw, py = (rh - ph) / 2;
        s.drawPill(px, py, pw, ph, pillLbl, BG, ACCENT, INK);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

FLASHMEM void drawInstSaveToModal(bool full) {
  if (!isInstSaveToModalOpen()) return;

  const int ovX = SX(40);
  const int ovW = SX(240);
  const int hdrY = SY(68);
  const int hdrH = SY(22);
  const int rowH = SY(24);
  const int cur = constrain(instSaveToModalCursor(), 0, 1);
  const int slot = constrain(instListCursor + 1, 1, NUM_INSTRUMENTS);

  // 1. Header Banner Strip
  {
    char hdrTitle[28];
    snprintf(hdrTitle, sizeof(hdrTitle), "SAVE I%02d TO...", slot);
    uint64_t bk = hashStr(hashInit(), hdrTitle);
    bk = hashMix(bk, (uint64_t)ACCENT);
    if (full || bk != prevSaveToBannerKey) {
      prevSaveToBannerKey = bk;
      Surface s(ovW, hdrH);
      s.fillRect(0, 0, ovW, hdrH, DARK);
      s.fillRect(0, 0, ovW, PX, ACCENT);
      s.fillRect(0, 0, PX, hdrH, ACCENT);
      s.fillRect(ovW - PX, 0, PX, hdrH, ACCENT);
      s.fillRect(PX, hdrH - 1, ovW - 2 * PX, 1, MID);
      int tw = textWidth(hdrTitle);
      s.drawText(max(SX(6), (ovW - tw) / 2), TY(hdrH), hdrTitle, ACCENT);
      s.blit(ovX, hdrY);
    }
  }

  // 2. Option Pill Rows: [ SONG ] and [ PRESET ]
  static const char *const SAVE_TO_LABELS[2] = {
    "SONG",
    "PRESET"
  };
  for (int i = 0; i < 2; i++) {
    const bool sel = (cur == i);
    const bool isLast = (i == 1);
    const int ry = hdrY + hdrH + i * rowH;

    uint16_t pillBg = sel ? ACCENT : PANEL;
    uint16_t pillFg = sel ? BG : INK;
    uint16_t pillBd = sel ? INK : MID;

    uint64_t rk = hashStr(hashInit(), SAVE_TO_LABELS[i]);
    rk = hashMix(rk, (uint64_t)pillBg | ((uint64_t)pillFg << 16) |
                     ((uint64_t)pillBd << 32) | ((uint64_t)sel << 48));
    if (full || rk != prevSaveToRowKey[i]) {
      prevSaveToRowKey[i] = rk;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, ACCENT);
      s.fillRect(ovW - PX, 0, PX, rowH, ACCENT);
      if (isLast) {
        s.fillRect(0, rowH - PX, ovW, PX, ACCENT);
      }

      const int pw = ovW - SX(20);
      const int ph = rowH - SY(4);
      const int px = SX(10);
      const int py = (rowH - ph) / 2 - (isLast ? (PX / 2) : 0);
      s.drawPill(px, max(1, py), pw, ph, SAVE_TO_LABELS[i], pillFg, pillBg, pillBd);
      s.blit(ovX, ry);
    }
  }
}

FLASHMEM bool isSamplerKeyModalOpen() {
  return smpKeyModalActive || storageIsSamplerKeyRecordingOpen();
}

FLASHMEM uint8_t samplerKeyModalState() {
  if (storageIsSamplerKeyRecordingOpen()) return 3;
  if (smpKeyModalActive && smpKeyRecModeActive) return 2;
  if (smpKeyModalActive) return 1;
  return 0;
}

FLASHMEM void openSamplerKeyModal(int instIdx, int keyIdx) {
  closeSamplerBrowser();
  closeInstBrowser();
  closeInstSaveToModal();
  storageCancelSamplerKeyRecord();
  smpKeyModalInst = constrain(instIdx, 0, NUM_INSTRUMENTS - 1);
  smpKeyModalKey = constrain(keyIdx, 0, SAMPLER_NUM_KEYS - 1);
  smpKeyModalCursor = 0;
  smpKeyRecModeCursor = 0;
  smpKeyRecModeActive = false;
  smpKeyModalActive = true;
  memset(prevSmpKeyRowKey, 0, sizeof(prevSmpKeyRowKey));
  prevSmpKeyRecKey = 0;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void closeSamplerKeyModal() {
  if (!smpKeyModalActive && !storageIsSamplerKeyRecordingOpen()) return;
  storageCancelSamplerKeyRecord();
  smpKeyModalActive = false;
  smpKeyRecModeActive = false;
  memset(prevSmpKeyRowKey, 0, sizeof(prevSmpKeyRowKey));
  prevSmpKeyRecKey = 0;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void samplerKeyModalMove(int delta) {
  if (!smpKeyModalActive || storageIsSamplerKeyRecordingOpen() || delta == 0) return;
  if (smpKeyRecModeActive) {
    smpKeyRecModeCursor = (smpKeyRecModeCursor + delta + 3) % 3;
  } else {
    smpKeyModalCursor = (smpKeyModalCursor + delta + 2) % 2;
  }
  uiForceRedraw();
}

FLASHMEM void samplerKeyModalConfirm() {
  if (!smpKeyModalActive || storageIsSamplerKeyRecordingOpen()) return;
  int instIdx = constrain(smpKeyModalInst, 0, NUM_INSTRUMENTS - 1);
  int keyIdx = constrain(smpKeyModalKey, 0, SAMPLER_NUM_KEYS - 1);
  if (!smpKeyRecModeActive) {
    if (smpKeyModalCursor == 0) {
      smpKeyModalActive = false;
      openSamplerBrowser(instIdx, keyIdx);
      inputSwallowModalKeys();
    } else {
      smpKeyRecModeActive = true;
      smpKeyRecModeCursor = 0;
      memset(prevSmpKeyRowKey, 0, sizeof(prevSmpKeyRowKey));
      inputSwallowModalKeys();
    }
  } else {
    SamplerRecMode mode = (SamplerRecMode)constrain(smpKeyRecModeCursor, 0, 2);
    if (!storageStartSamplerKeyRecord(instIdx, keyIdx, mode)) {
      smpKeyModalActive = false;
      smpKeyRecModeActive = false;
      uiShowToast("REC INIT ERR");
    }
    inputSwallowModalKeys();
  }
  uiForceRedraw();
}

PROGMEM static const char *const SMP_KEY_ACT_LABELS[2] = {
  "Load from file",
  "Record from audio in"
};

PROGMEM static const char *const SMP_REC_MODE_LABELS[3] = {
  "THRESH AUTO START STOP",
  "THRESH AUTO START, MANUAL STOP",
  "MANUAL START STOP"
};

FLASHMEM void drawSamplerKeyModal(bool full) {
  if (!isSamplerKeyModalOpen()) return;

  const uint16_t accent = ACCENT;

  if (storageIsSamplerKeyRecordingOpen()) {
    const int ovX = SX(40);
    const int ovW = SX(240);
    const int ovY = SY(102);
    const int ovH = SY(28);
    const bool triggered = storageIsSamplerKeyRecordTriggered();
    const SamplerRecMode recMode = storageSamplerKeyRecMode();
    const bool blinkOn = triggered || (((millis() / 250) % 2) == 0);
    const char *txt = "Waiting for audio...";
    if (triggered) {
      txt = (recMode == SamplerRecMode::ThreshAutoStartStop)
                ? "Recording..."
                : "Recording... EDIT to stop";
    } else if (recMode == SamplerRecMode::ManualStartStop) {
      txt = "Waiting for EDIT...";
    }
    uint64_t rk = hashStr(hashInit(), txt);
    rk = hashMix(rk, (uint64_t)accent |
                     ((uint64_t)blinkOn << 16) |
                     ((uint64_t)triggered << 24) |
                     ((uint64_t)recMode << 32) |
                     (1ULL << 40));
    if (full || rk != prevSmpKeyRecKey) {
      prevSmpKeyRecKey = rk;
      Surface s(ovW, ovH);
      s.drawBox(DARK, accent);
      const int r = SY(4);
      const int gap = SX(6);
      const int tw = textWidth(txt);
      const int totalW = 2 * r + gap + tw;
      const int startX = max(SX(8), (ovW - totalW) / 2);
      const int cx = startX + r;
      const int cy = ovH / 2;
      s.drawCircle(cx, cy, r, blinkOn ? COLOR_REC : COLOR_REC_DIM);
      s.drawText(startX + 2 * r + gap, TY(ovH), txt, INK);
      s.blit(ovX, ovY);
    }
    return;
  }

  if (smpKeyRecModeActive) {
    const int ovW = SX(268);
    const int ovX = (SCREEN_W - ovW) / 2;
    const int r0Y = SY(80);
    const int rowH = SY(24);
    const int cur = constrain(smpKeyRecModeCursor, 0, 2);
    const bool blinkOn = (((millis() / 250) % 2) == 0);

    for (int i = 0; i < 3; i++) {
      const bool sel = (cur == i);
      const bool isFirst = (i == 0);
      const bool isLast = (i == 2);
      const int ry = r0Y + i * rowH;

      uint16_t pillBg = sel ? accent : PANEL;
      uint16_t pillFg = sel ? BG : INK;
      uint16_t pillBd = sel ? INK : MID;

      uint64_t rk = hashStr(hashInit(), SMP_REC_MODE_LABELS[i]);
      rk = hashMix(rk, (uint64_t)pillBg | ((uint64_t)pillFg << 16) |
                       ((uint64_t)pillBd << 32) | ((uint64_t)sel << 48) |
                       ((uint64_t)(isFirst && blinkOn) << 56));
      if (full || rk != prevSmpKeyRowKey[i]) {
        prevSmpKeyRowKey[i] = rk;
        Surface s(ovW, rowH);
        s.fillRect(0, 0, ovW, rowH, PANEL);
        s.fillRect(0, 0, PX, rowH, accent);
        s.fillRect(ovW - PX, 0, PX, rowH, accent);
        if (isFirst) {
          s.fillRect(0, 0, ovW, PX, accent);
        }
        if (isLast) {
          s.fillRect(0, rowH - PX, ovW, PX, accent);
        }

        const int ph = rowH - SY(4);
        const int py = max(1, (rowH - ph) / 2 + (isFirst ? (PX / 2) : (isLast ? -(PX / 2) : 0)));
        if (isFirst) {
          const int r = SY(4);
          const int cx = SX(10) + r;
          const int cy = py + ph / 2;
          s.drawCircle(cx, cy, r, blinkOn ? LIGHT : MID);
          const int px = SX(10) + 2 * r + SX(6);
          const int pw = ovW - px - SX(10);
          s.drawPill(px, py, pw, ph, SMP_REC_MODE_LABELS[i], pillFg, pillBg, pillBd);
        } else {
          const int px = SX(10);
          const int pw = ovW - SX(20);
          s.drawPill(px, py, pw, ph, SMP_REC_MODE_LABELS[i], pillFg, pillBg, pillBd);
        }
        s.blit(ovX, ry);
      }
    }
    return;
  }

  const int ovX = SX(40);
  const int ovW = SX(240);
  const int r0Y = SY(92);
  const int rowH = SY(24);
  const int cur = constrain(smpKeyModalCursor, 0, 1);

  for (int i = 0; i < 2; i++) {
    const bool sel = (cur == i);
    const bool isFirst = (i == 0);
    const bool isLast = (i == 1);
    const int ry = r0Y + i * rowH;

    uint16_t pillBg = sel ? accent : PANEL;
    uint16_t pillFg = sel ? BG : INK;
    uint16_t pillBd = sel ? INK : MID;

    uint64_t rk = hashStr(hashInit(), SMP_KEY_ACT_LABELS[i]);
    rk = hashMix(rk, (uint64_t)pillBg | ((uint64_t)pillFg << 16) |
                     ((uint64_t)pillBd << 32) | ((uint64_t)sel << 48));
    if (full || rk != prevSmpKeyRowKey[i]) {
      prevSmpKeyRowKey[i] = rk;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, accent);
      s.fillRect(ovW - PX, 0, PX, rowH, accent);
      if (isFirst) {
        s.fillRect(0, 0, ovW, PX, accent);
      }
      if (isLast) {
        s.fillRect(0, rowH - PX, ovW, PX, accent);
      }

      const int pw = ovW - SX(20);
      const int ph = rowH - SY(4);
      const int px = SX(10);
      const int py = (rowH - ph) / 2 + (isFirst ? (PX / 2) : (isLast ? -(PX / 2) : 0));
      s.drawPill(px, max(1, py), pw, ph, SMP_KEY_ACT_LABELS[i], pillFg, pillBg, pillBd);
      s.blit(ovX, ry);
    }
  }
}

FLASHMEM static void drawSectionRowInSlot(const FmPatch &p, InstType iType, const InstEngineOps *ops,
                                          int slotIdx, int rowIdx, bool forceFull) {
  const int titleY  = SY(54) + slotIdx * SY(56);
  const int cellY   = SY(66) + slotIdx * SY(56);
  const int titleH  = cellY - titleY;
  const int cellH   = SY(34);
  const int genBase = (iType == InstType::Sampler) ? SMP_ROW_AMP : INST_GEN_ROW_AMP;

  if (rowIdx == genBase) {
    drawSectionTitle(slotIdx, titleY, titleH, "AMP: ENVELOPE", COLOR_PLAY, forceFull);
    for (int c = 0; c < INST_ENV_COUNT; c++) {
      int x = SX(6) + c * SX(62);
      int w = SX(60);
      int gapW = (c + 1 < INST_ENV_COUNT) ? SX(2) : 0;
      uint8_t v = getEnvField(p.amp, c);
      char val[12];
      formatEnvCellValue(c, v, val, sizeof(val));
      drawFmParamCell(rowIdx, c, x, cellY, w, cellH, gapW,
                      ENV_COL_LABELS[c], val, v, INST_ENV_MAX, COLOR_PLAY, forceFull);
    }
    return;
  }

  if (rowIdx == genBase + 1) {
    drawSectionTitle(slotIdx, titleY, titleH, "FLT: ENVELOPE", COLOR_PUNCH, forceFull);
    for (int c = 0; c < INST_ENV_COUNT; c++) {
      int x = SX(6) + c * SX(62);
      int w = SX(60);
      int gapW = (c + 1 < INST_ENV_COUNT) ? SX(2) : 0;
      uint8_t v = getEnvField(p.flt, c);
      char val[12];
      formatEnvCellValue(c, v, val, sizeof(val));
      drawFmParamCell(rowIdx, c, x, cellY, w, cellH, gapW,
                      ENV_COL_LABELS[c], val, v, INST_ENV_MAX, COLOR_PUNCH, forceFull);
    }
    return;
  }

  if (rowIdx == genBase + 2) {
    drawSectionTitle(slotIdx, titleY, titleH, "FX: INSTRUMENT EFFECTS", ACCENT, forceFull);
    for (int c = 0; c < INST_FX_COUNT; c++) {
      int x = SX(6) + c * SX(62);
      int w = SX(60);
      int gapW = (c + 1 < INST_FX_COUNT) ? SX(2) : 0;
      uint8_t v = getFxField(p.fx, c);
      char val[12];
      snprintf(val, sizeof(val), "%u%%", (unsigned)v);
      drawFmParamCell(rowIdx, c, x, cellY, w, cellH, gapW,
                      FX_COL_LABELS[c], val, v, INST_FX_MAX, ACCENT, forceFull);
    }
    return;
  }

  if (iType == InstType::PhaseDist || iType == InstType::NoiseDrone) {
    int opIdx = rowIdx - 1;
    const char *title = (opIdx == 0) ? ops->sec0Title : (opIdx == 1) ? ops->sec1Title : ops->sec2Title;
    uint16_t mainCol = (opIdx == 0) ? ACCENT : (opIdx == 1) ? COLOR_MIDI : COLOR_PUNCH;
    drawSectionTitle(slotIdx, titleY, titleH, title, mainCol, forceFull);

    int numCols = fmRowColCount(rowIdx);
    for (int c = 0; c < numCols; c++) {
      int x = SX(6);
      int w = SX(60);
      int gapW = 0;
      if (numCols == 6) {
        x = SX(6) + (c * SX(258)) / 5;
        int nextX = (c + 1 < 6) ? (SX(6) + ((c + 1) * SX(258)) / 5) : SX(314);
        w = SX(50);
        gapW = max(0, nextX - (x + w));
      } else if (numCols == 5) {
        x = SX(6) + c * SX(62);
        w = SX(60);
        gapW = (c + 1 < 5) ? SX(2) : 0;
      } else {
        x = SX(6) + c * SX(104);
        w = SX(100);
        gapW = (c + 1 < 3) ? SX(4) : 0;
      }
      const char *lbl = "";
      char val[16] = "";
      int barV = 0, barMax = 100;
      if (ops && ops->formatMainCell) {
        ops->formatMainCell(p, opIdx, c, lbl, val, sizeof(val), barV, barMax);
      }
      drawFmParamCell(rowIdx, c, x, cellY, w, cellH, gapW, lbl, val, barV, barMax, mainCol, forceFull);
    }
    return;
  }

  // 2-Op FM Operator A (rowIdx == 2) or Operator B (rowIdx == 3)
  int opIdx = (rowIdx == FM_ROW_OP1_MAIN) ? 0 : 1;
  const char *title = (opIdx == 0) ? ops->sec0Title : ops->sec1Title;
  uint16_t mainCol = (opIdx == 0) ? ACCENT : COLOR_MIDI;
  drawSectionTitle(slotIdx, titleY, titleH, title, mainCol, forceFull);
  for (int c = 0; c < 4; c++) {
    int x = SX(6) + c * SX(78);
    int w = SX(74);
    int gapW = (c + 1 < 4) ? SX(4) : 0;
    const char *lbl = "";
    char val[16] = "";
    int barV = 0, barMax = 100;
    if (ops && ops->formatMainCell) {
      ops->formatMainCell(p, opIdx, c, lbl, val, sizeof(val), barV, barMax);
    }
    drawFmParamCell(rowIdx, c, x, cellY, w, cellH, gapW, lbl, val, barV, barMax, mainCol, forceFull);
  }
}

FLASHMEM static bool activeInstMidiMapClrConfirm() {
  if (!instMidiMapClrConfirm) return false;
  if ((int32_t)(instMidiMapClrConfirmUntilMs - millis()) <= 0) {
    instMidiMapClrConfirm = false;
    return false;
  }
  return true;
}

FLASHMEM static void syncInstMidiMapScroll(int rowCount) {
  instMidiMapCursor = constrain(instMidiMapCursor, 0, max(0, rowCount - 1));
  if (instMidiMapCursor < instMidiMapScroll) {
    instMidiMapScroll = instMidiMapCursor;
  } else if (instMidiMapCursor >= instMidiMapScroll + INST_MIDI_MAP_VISIBLE_ROWS) {
    instMidiMapScroll = instMidiMapCursor - INST_MIDI_MAP_VISIBLE_ROWS + 1;
  }
  instMidiMapScroll = constrain(instMidiMapScroll, 0, max(0, rowCount - INST_MIDI_MAP_VISIBLE_ROWS));
  instMidiMapCol = constrain(instMidiMapCol, 0, 3);
}

FLASHMEM bool isInstMidiMapOpen() {
  return instMidiMapOpen;
}

FLASHMEM static void openInstMidiMap() {
  instMidiMapCancelLearn();
  instMidiMapOpen = true;
  instMidiMapZone = INST_MAP_ZONE_LIST;
  instMidiMapClrConfirm = false;
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const InstType iType = (InstType)fmSynthPatch(instIdx).type;
  syncInstMidiMapScroll(instMidiMapRowCount(iType));
  instEditInvalidateCache();
  uiForceRedraw();
}

FLASHMEM void closeInstMidiMap() {
  if (!instMidiMapOpen) return;
  instMidiMapCancelLearn();
  instMidiMapClrConfirm = false;
  instMidiMapOpen = false;
  instEditInvalidateCache();
  uiForceRedraw();
}

FLASHMEM bool cancelInstMidiMapLearnOrConfirm() {
  if (!instMidiMapOpen) return false;
  if (instMidiMapIsLearning()) {
    instMidiMapCancelLearn();
    uiShowToast("LEARN CANCELLED");
    uiForceRedraw();
    return true;
  }
  if (activeInstMidiMapClrConfirm()) {
    instMidiMapClrConfirm = false;
    uiShowToast("", 0);
    uiForceRedraw();
    return true;
  }
  return false;
}

FLASHMEM static void drawInstMidiMapView(bool full) {
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const FmPatch &p = fmSynthPatch(instIdx);
  const InstType iType = (InstType)p.type;
  const int rowCount = instMidiMapRowCount(iType);
  syncInstMidiMapScroll(rowCount);
  const bool confirmClr = activeInstMidiMapClrConfirm();

  // 1. Top Control Banner: [I01 MIDI MAP: <NAME>]   [CLR ALL]
  {
    const bool inClrZone = (instMidiMapZone == INST_MAP_ZONE_CLR);
    char leftTxt[32];
    snprintf(leftTxt, sizeof(leftTxt), "I%02d MIDI MAP: %s", instIdx + 1, p.name);
    while (textWidth(leftTxt) > SX(216) && strlen(leftTxt) > 6) {
      leftTxt[strlen(leftTxt) - 1] = '\0';
    }

    uint64_t bk = hashStr(hashInit(), leftTxt);
    bk = hashMix(bk, (uint64_t)inClrZone |
                     ((uint64_t)confirmClr << 4) |
                     ((uint64_t)ACCENT << 16));
    if (full || bk != prevInstMidiMapBannerKey) {
      prevInstMidiMapBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, inClrZone ? ACCENT : MID);
      s.drawText(SX(6), TY(bh), leftTxt, ACCENT);

      const int py = max(1, (bh - SY(16)) / 2);
      const int ph = SY(16);
      const char *clrLbl = confirmClr ? "SURE?" : "CLR ALL";
      uint16_t pBg = inClrZone ? (confirmClr ? COLOR_REC : COLOR_WARN)
                               : (confirmClr ? COLOR_REC_DIM : PANEL);
      uint16_t pFg = inClrZone ? BG : (confirmClr ? COLOR_REC : LIGHT);
      uint16_t pBd = inClrZone ? INK : (confirmClr ? COLOR_REC : MID);
      s.drawPill(SX(228), py, SX(74), ph, clrLbl, pFg, pBg, pBd);

      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Parameter Mapping Table
  for (int vis = 0; vis < INST_MIDI_MAP_VISIBLE_ROWS; vis++) {
    const int idx = instMidiMapScroll + vis;
    const int y = SY(50) + vis * SY(26);
    const int rw = SX(308), rh = SY(24);

    if (idx >= rowCount) {
      uint64_t rk = hashMix(hashInit(), (uint64_t)(vis + 1) | 0x80000000ULL);
      if (full || rk != prevInstMidiMapRowKey[vis]) {
        prevInstMidiMapRowKey[vis] = rk;
        Surface s(rw, rh);
        s.drawBox(PANEL, MID);
        s.fillRect(PX, PX, SX(3), rh - 2 * PX, DIM);
        s.blit(SX(6), y);
      }
      keepStreaming();
      continue;
    }

    const int slot = instMidiMapRowToSlot(iType, idx);
    const bool rowSel = (instMidiMapZone == INST_MAP_ZONE_LIST && instMidiMapCursor == idx);
    const MidiCcMapEntry eff = instMidiMapGetSlot(instIdx, slot);
    const bool isLearningRow = instMidiMapIsLearning() &&
                               (instMidiMapLearningInst() == instIdx) &&
                               (instMidiMapLearningSlot() == slot);
    const char *paramLbl = instMidiMapRowLabel(iType, idx);

    uint64_t rk = hashStr(hashInit(), paramLbl);
    rk = hashMix(rk, (uint64_t)instIdx |
                     ((uint64_t)iType << 4) |
                     ((uint64_t)idx << 8) |
                     ((uint64_t)rowSel << 16) |
                     ((uint64_t)(instMidiMapCol & 3) << 20) |
                     ((uint64_t)isLearningRow << 24) |
                     ((uint64_t)eff.channel << 32) |
                     ((uint64_t)eff.cc << 40));
    rk = hashMix(rk, (uint64_t)ACCENT);

    if (full || rk != prevInstMidiMapRowKey[vis]) {
      prevInstMidiMapRowKey[vis] = rk;
      Surface s(rw, rh);
      const bool activeMap = (eff.channel != MIDI_MAP_CH_OFF);
      const uint16_t bg = rowSel ? DARK : PANEL;
      const uint16_t border = rowSel ? ACCENT : MID;
      const uint16_t barCol = isLearningRow ? COLOR_REC
                            : rowSel        ? ACCENT
                            : activeMap     ? LIGHT : DIM;
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      uint16_t nameFg = rowSel ? INK : (activeMap ? LIGHT : DIM);
      s.drawText(SX(8), TY(rh), paramLbl, nameFg);

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
        bool cSel = (rowSel && instMidiMapCol == c);
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

FLASHMEM static void instEditDrawPage(bool full) {
  if (isSaveKeyboardOpen()) {
    drawSaveKeyboardModal(full);
    return;
  }
  if (isSamplerBrowserOpen()) {
    drawSamplerBrowserModal(full);
    return;
  }
  if (isInstBrowserOpen()) {
    drawInstBrowserModal(full);
    return;
  }
  if (instMidiMapOpen) {
    drawInstMidiMapView(full);
    return;
  }

  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const FmPatch &p = fmSynthPatch(instIdx);
  const InstType iType = (InstType)p.type;
  const InstEngineOps *ops = instEngineForType(iType);
  if (!ops) {
    ops = instEngineAt(0);
  }
  syncInstEditScroll();

  bool scrollOrTypeChanged = full ||
                             (instIdx != prevDrawnInstIdx) ||
                             ((uint8_t)iType != prevDrawnInstType) ||
                             (instEditScroll != prevDrawnScroll);
  if (scrollOrTypeChanged) {
    prevDrawnInstIdx = instIdx;
    prevDrawnInstType = (uint8_t)iType;
    prevDrawnScroll = instEditScroll;
    for (int r = 1; r < FM_ROW_COUNT; r++) {
      memset(prevFmCellKey[r], 0, sizeof(prevFmCellKey[r]));
    }
    memset(prevSectionTitleKey, 0, sizeof(prevSectionTitleKey));
    prevSamplerKbValid = false;
  }

  // Row 0: [0: Instrument Name] [1: Load] [2: Save To...]
  {
    const int y0 = SY(26), h0 = SY(24);

    // (0, 0) Instrument Slot & Name
    {
      const int x0 = SX(6), w0 = SX(152);
      bool sel = (fmEditRow == FM_ROW_HEADER && fmEditCol == INST_HDR_NAME);
      uint16_t bg = sel ? DARK : PANEL;
      uint16_t border = sel ? (inputEditHeld() ? ACCENT : INK) : MID;
      char title[24];
      snprintf(title, sizeof(title), "I%02d: %s", instIdx + 1, p.name);
      while (textWidth(title) > w0 - SX(8) && strlen(title) > 5) {
        title[strlen(title) - 1] = '\0';
      }
      uint64_t k = hashStr(hashInit(), title);
      k = hashMix(k, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)sel << 32));
      if (full || k != prevFmCellKey[0][INST_HDR_NAME]) {
        prevFmCellKey[0][INST_HDR_NAME] = k;
        Surface s(w0, h0);
        s.drawBox(bg, border);
        s.drawText(SX(6), TY(h0), title, sel ? INK : ACCENT);
        s.blit(x0, y0);
      }
    }

    // (0, 1..2) Action Buttons: [ LOAD ] [ SAVE TO... ]
    static const int btnX[2] = { SX(162), SX(224) };
    static const int btnW[2] = { SX(58),  SX(90)  };
    static const char *const btnLbl[2] = { "LOAD", "SAVE TO..." };

    for (int b = 0; b < 2; b++) {
      int colIdx = INST_HDR_LOAD + b;
      int bx = btnX[b];
      int bw = btnW[b];
      bool sel = (fmEditRow == FM_ROW_HEADER && fmEditCol == colIdx);
      bool editHeld = sel && inputEditHeld();
      uint16_t bg = sel ? (editHeld ? INK : ACCENT) : PANEL;
      uint16_t border = sel ? INK : MID;
      uint16_t fg = sel ? BG : LIGHT;

      uint64_t k = hashStr(hashInit(), btnLbl[b]);
      k = hashMix(k, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)fg << 32));
      if (full || k != prevFmCellKey[0][colIdx]) {
        prevFmCellKey[0][colIdx] = k;
        Surface s(bw, h0);
        s.drawBox(bg, border);
        int lw = textWidth(btnLbl[b]);
        s.drawText(max(SX(2), (bw - lw) / 2), TY(h0), btnLbl[b], fg);
        s.blit(bx, y0);
      }
    }
  }

  // Body Viewport (y = SY(54)..SY(84)+6*SY(24)): Specific controls at top, scrolling down to AMP / FLT / FX
  const int viewportBottomY = SY(84) + 6 * SY(24);
  if (iType == InstType::Sampler && instEditScroll == 0) {
    if (scrollOrTypeChanged && !full) {
      const int leftEnd = SX(6) + SX(128);
      blitBgStrip(leftEnd, SY(54), SX(138) - leftEnd, SY(24));
      const int pressBottomY = SY(54) + SY(24);
      blitBgStrip(SX(6), pressBottomY, SX(308), SY(84) - pressBottomY);
    }
    if (fmEditRow == SMP_ROW_KEYS) {
      samplerSelectedKey[instIdx] = (uint8_t)constrain(fmEditCol, 0, SAMPLER_NUM_KEYS - 1);
    }
    int curKey = constrain((int)samplerSelectedKey[instIdx], 0, SAMPLER_NUM_KEYS - 1);
    drawSamplerPressAndKeyStatus(instIdx, p.algorithm, curKey, scrollOrTypeChanged);
    drawSamplerKeyboard(instIdx, curKey, fmEditRow == SMP_ROW_KEYS, scrollOrTypeChanged);
  } else if (iType == InstType::Fm2Op && instEditScroll == 0) {
    const int algoY      = SY(54);
    const int algoH      = SY(24);
    const int sec0TitleY = SY(84);
    const int sec0CellY  = SY(97);
    const int sec1TitleY = SY(145);
    const int sec1CellY  = SY(158);
    const int cellH      = SY(40);

    if (scrollOrTypeChanged && !full) {
      const int algoRightX = SX(6) + SX(96);
      blitBgStrip(algoRightX, algoY, (SX(6) + SX(308)) - algoRightX, algoH);
      blitBgStrip(SX(6), algoY + algoH, SX(308), sec0TitleY - (algoY + algoH));
      blitBgStrip(SX(6), sec0CellY + cellH, SX(308), sec1TitleY - (sec0CellY + cellH));
      blitBgStrip(SX(6), sec1CellY + cellH, SX(308), viewportBottomY - (sec1CellY + cellH));
    }

    drawFmAlgoToggle(p.algorithm, scrollOrTypeChanged);
    drawSectionTitle(0, sec0TitleY, sec0CellY - sec0TitleY, ops->sec0Title, ACCENT, scrollOrTypeChanged);
    drawSectionTitle(1, sec1TitleY, sec1CellY - sec1TitleY, ops->sec1Title, COLOR_MIDI, scrollOrTypeChanged);

    for (int opIdx = 0; opIdx < 2; opIdx++) {
      const int mainRow = (opIdx == 0) ? FM_ROW_OP1_MAIN : FM_ROW_OP2_MAIN;
      const int mainY   = (opIdx == 0) ? sec0CellY : sec1CellY;
      const uint16_t mainCol = (opIdx == 0) ? ACCENT : COLOR_MIDI;

      for (int c = 0; c < 4; c++) {
        int x = SX(6) + c * SX(78);
        int w = SX(74);
        int gapW = (c + 1 < 4) ? SX(4) : 0;
        const char *lbl = "";
        char val[16] = "";
        int barV = 0, barMax = 100;
        if (ops && ops->formatMainCell) {
          ops->formatMainCell(p, opIdx, c, lbl, val, sizeof(val), barV, barMax);
        }
        drawFmParamCell(mainRow, c, x, mainY, w, cellH, gapW, lbl, val, barV, barMax, mainCol, scrollOrTypeChanged);
      }
      keepStreaming();
    }
  } else {
    if (scrollOrTypeChanged && !full) {
      const int slot0BottomY = SY(66) + SY(34);
      const int slot1TopY    = SY(54) + SY(56);
      const int slot1BottomY = (SY(66) + SY(56)) + SY(34);
      const int slot2TopY    = SY(54) + 2 * SY(56);
      const int slot2BottomY = (SY(66) + 2 * SY(56)) + SY(34);
      blitBgStrip(SX(6), slot0BottomY, SX(308), slot1TopY - slot0BottomY);
      blitBgStrip(SX(6), slot1BottomY, SX(308), slot2TopY - slot1BottomY);
      blitBgStrip(SX(6), slot2BottomY, SX(308), viewportBottomY - slot2BottomY);
    }
    for (int slotIdx = 0; slotIdx < 3; slotIdx++) {
      int rowIdx = (iType == InstType::Sampler)
                       ? (SMP_ROW_AMP + slotIdx)
                       : (1 + instEditScroll + slotIdx);
      drawSectionRowInSlot(p, iType, ops, slotIdx, rowIdx, scrollOrTypeChanged);
      keepStreaming();
    }
  }
}

FLASHMEM static void instEditOnShiftNavigate(uint8_t dir) {
  if (instMidiMapOpen) {
    if (dir & BTN_LEFT) {
      if (!cancelInstMidiMapLearnOrConfirm()) {
        closeInstMidiMap();
      }
    } else if (dir & BTN_UP) {
      instMidiMapCancelLearn();
      instMidiMapClrConfirm = false;
      instMidiMapOpen = false;
      setPage(lastRow1Page());
    } else if (dir & BTN_DOWN) {
      instMidiMapCancelLearn();
      instMidiMapClrConfirm = false;
      instMidiMapOpen = false;
      setPage(Page::Master);
    }
    return;
  }

  if (dir & BTN_LEFT) setPage(Page::Instruments);
  else if (dir & BTN_RIGHT) openInstMidiMap();
  else if (dir & BTN_UP) setPage(lastRow1Page());
  else if (dir & BTN_DOWN) setPage(Page::Master);
}

FLASHMEM static void instEditOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  const InstType iType = (InstType)fmSynthPatch(instIdx).type;

  if (instMidiMapOpen) {
    const int rowCount = instMidiMapRowCount(iType);
    if (dy != 0) {
      instMidiMapCancelLearn();
      instMidiMapClrConfirm = false;
      if (instMidiMapZone == INST_MAP_ZONE_CLR) {
        if (dy > 0 && rowCount > 0) {
          instMidiMapZone = INST_MAP_ZONE_LIST;
          instMidiMapCursor = 0;
          instMidiMapScroll = 0;
        }
      } else {
        if (dy < 0 && instMidiMapCursor <= 0) {
          instMidiMapZone = INST_MAP_ZONE_CLR;
        } else {
          instMidiMapCursor = constrain(instMidiMapCursor + dy, 0, max(0, rowCount - 1));
          syncInstMidiMapScroll(rowCount);
        }
      }
    }
    if (dx != 0) {
      instMidiMapClrConfirm = false;
      if (instMidiMapZone == INST_MAP_ZONE_LIST) {
        instMidiMapCol = constrain(instMidiMapCol + dx, 0, 3);
      }
    }
    uiForceRedraw();
    return;
  }

  const bool isSmp = (iType == InstType::Sampler);

  int maxRow = instEditRowCount() - 1;
  if (dy != 0) {
    if (isSmp && fmEditRow == SMP_ROW_KEYS) {
      samplerSelectedKey[instIdx] = (uint8_t)constrain(fmEditCol, 0, SAMPLER_NUM_KEYS - 1);
    }
    fmEditRow = constrain(fmEditRow + dy, 0, maxRow);
    syncInstEditScroll();
    int maxCol = fmRowColCount(fmEditRow) - 1;
    if (isSmp && fmEditRow == SMP_ROW_KEYS) {
      fmEditCol = constrain((int)samplerSelectedKey[instIdx], 0, maxCol);
    } else {
      fmEditCol = constrain(fmEditCol, 0, maxCol);
    }
  }
  if (dx != 0) {
    int maxCol = fmRowColCount(fmEditRow) - 1;
    fmEditCol = constrain(fmEditCol + dx, 0, maxCol);
    if (isSmp && fmEditRow == SMP_ROW_KEYS) {
      samplerSelectedKey[instIdx] = (uint8_t)fmEditCol;
    }
  }
  uiForceRedraw();
}

FLASHMEM static void instEditOnOptAdjust(int dx, int dy, int delta) {
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  FmPatch &p = fmSynthPatch(instIdx);
  const InstType iType = (InstType)p.type;

  if (instMidiMapOpen) {
    instMidiMapCancelLearn();
    instMidiMapClrConfirm = false;
    if (instMidiMapZone == INST_MAP_ZONE_LIST) {
      const int rowCount = instMidiMapRowCount(iType);
      syncInstMidiMapScroll(rowCount);
      const int slot = instMidiMapRowToSlot(iType, instMidiMapCursor);
      if (instMidiMapCol == 0) {
        instMidiMapStepChannel(instIdx, slot, delta);
      } else if (instMidiMapCol == 1) {
        int ccStep = (dx != 0) ? dx : (dy * 10);
        instMidiMapStepCc(instIdx, slot, ccStep);
      }
    }
    uiForceRedraw();
    return;
  }

  const InstEngineOps *ops = instEngineForType(iType);
  const int genBase = instGenBaseRow();
  int step = (dx != 0) ? dx : (dy * 5);

  if (fmEditRow == FM_ROW_HEADER) {
    if (dx != 0) {
      fmEditCol = constrain(fmEditCol + dx, 0, INST_HDR_COL_COUNT - 1);
    }
  } else if (fmEditRow == genBase) {
    int cur = (int)getEnvField(p.amp, fmEditCol);
    setEnvField(p.amp, fmEditCol, (uint8_t)constrain(cur + step, 0, (int)INST_ENV_MAX));
    saveSessionWhenIdle();
  } else if (fmEditRow == genBase + 1) {
    int cur = (int)getEnvField(p.flt, fmEditCol);
    setEnvField(p.flt, fmEditCol, (uint8_t)constrain(cur + step, 0, (int)INST_ENV_MAX));
    saveSessionWhenIdle();
  } else if (fmEditRow == genBase + 2) {
    int cur = (int)getFxField(p.fx, fmEditCol);
    setFxField(p.fx, fmEditCol, (uint8_t)constrain(cur + step, 0, (int)INST_FX_MAX));
    saveSessionWhenIdle();
  } else if (iType == InstType::Sampler) {
    if (fmEditRow == SMP_ROW_PRESS) {
      p.algorithm = (uint8_t)(((int)p.algorithm + delta + NUM_SAMPLER_PRESS_MODES) % NUM_SAMPLER_PRESS_MODES);
      saveSessionWhenIdle();
    } else if (fmEditRow == SMP_ROW_KEYS) {
      int octDir = (dx != 0) ? dx : dy;
      fmEditCol = constrain(fmEditCol + octDir * 12, 0, SAMPLER_NUM_KEYS - 1);
      samplerSelectedKey[instIdx] = (uint8_t)fmEditCol;
    }
  } else if (iType == InstType::PhaseDist || iType == InstType::NoiseDrone) {
    int opIdx = fmEditRow - 1;
    if (ops && ops->adjustMainCell) {
      ops->adjustMainCell(p, opIdx, fmEditCol, dx, dy, delta, step);
    }
    saveSessionWhenIdle();
  } else if (fmEditRow == FM_ROW_ALGO) {
    p.algorithm = (uint8_t)(((int)p.algorithm + delta + NUM_FM_ALGOS) % NUM_FM_ALGOS);
    saveSessionWhenIdle();
  } else {
    int opIdx = (fmEditRow == FM_ROW_OP1_MAIN) ? 0 : 1;
    if (ops && ops->adjustMainCell) {
      ops->adjustMainCell(p, opIdx, fmEditCol, dx, dy, delta, step);
    }
    saveSessionWhenIdle();
  }
  uiForceRedraw();
}

FLASHMEM static void instEditOnEditPress(bool shiftHeld) {
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  FmPatch &p = fmSynthPatch(instIdx);
  const InstType iType = (InstType)p.type;

  if (instMidiMapOpen) {
    const int rowCount = instMidiMapRowCount(iType);
    syncInstMidiMapScroll(rowCount);
    const int slot = instMidiMapRowToSlot(iType, instMidiMapCursor);

    if (instMidiMapZone == INST_MAP_ZONE_CLR) {
      instMidiMapCancelLearn();
      if (activeInstMidiMapClrConfirm()) {
        instMidiMapClrConfirm = false;
        instMidiMapClearInst(instIdx);
        uiShowToast("INST MAPS CLEARED");
      } else {
        instMidiMapClrConfirm = true;
        instMidiMapClrConfirmUntilMs = millis() + 3000u;
        uiShowToast("CLR INST MAPS? EDIT AGAIN", 3000);
      }
      uiForceRedraw();
      return;
    }

    if (instMidiMapZone == INST_MAP_ZONE_LIST && slot >= 0) {
      if (instMidiMapCol == 0) {
        instMidiMapCancelLearn();
        instMidiMapClrConfirm = false;
        instMidiMapStepChannel(instIdx, slot, 1);
      } else if (instMidiMapCol == 1) {
        instMidiMapCancelLearn();
        instMidiMapClrConfirm = false;
        instMidiMapStepCc(instIdx, slot, 1);
      } else if (instMidiMapCol == 2) {
        instMidiMapClrConfirm = false;
        if (instMidiMapIsLearning() &&
            instMidiMapLearningInst() == instIdx &&
            instMidiMapLearningSlot() == slot) {
          instMidiMapCancelLearn();
          uiShowToast("LEARN CANCELLED");
        } else {
          instMidiMapStartLearn(instIdx, slot);
          uiShowToast("SEND MIDI CC...");
        }
      } else if (instMidiMapCol == 3) {
        instMidiMapCancelLearn();
        instMidiMapClrConfirm = false;
        instMidiMapClearSlot(instIdx, slot);
        uiShowToast("MAPPING CLEARED");
      }
      uiForceRedraw();
    }
    return;
  }

  if (fmEditRow == FM_ROW_HEADER) {
    if (fmEditCol == INST_HDR_NAME) {
      openInstrumentKeyboard(instIdx, shiftHeld);
      inputSwallowModalKeys();
    } else if (fmEditCol == INST_HDR_LOAD) {
      openInstBrowser(instIdx);
      inputSwallowModalKeys();
      uiForceRedraw();
    } else if (fmEditCol == INST_HDR_SAVE_TO) {
      openInstSaveToModal();
    }
    return;
  } else if (iType == InstType::Sampler && fmEditRow < SMP_ROW_AMP) {
    if (fmEditRow == SMP_ROW_PRESS) {
      p.algorithm = (uint8_t)(((int)p.algorithm + 1) % NUM_SAMPLER_PRESS_MODES);
      saveSessionWhenIdle();
      uiForceRedraw();
      return;
    }
    if (fmEditRow == SMP_ROW_KEYS) {
      int keyIdx = constrain(fmEditCol, 0, SAMPLER_NUM_KEYS - 1);
      samplerSelectedKey[instIdx] = (uint8_t)keyIdx;
      if (shiftHeld) {
        openSamplerKeyModal(instIdx, keyIdx);
      }
      uiForceRedraw();
      return;
    }
  } else if (iType == InstType::Fm2Op && fmEditRow == FM_ROW_ALGO) {
    p.algorithm = (uint8_t)(((int)p.algorithm + 1) % NUM_FM_ALGOS);
    saveSessionWhenIdle();
  }

  if (iType == InstType::Sampler) {
    int keyIdx = constrain((int)samplerSelectedKey[instIdx], 0, SAMPLER_NUM_KEYS - 1);
    fmSynthAudition(instIdx, (uint8_t)(SAMPLER_BASE_NOTE + keyIdx));
  } else {
    fmSynthAudition(instIdx);
  }
  uiForceRedraw();
}

FLASHMEM static void instEditOnResetParam() {
  const int instIdx = constrain(instListCursor, 0, NUM_INSTRUMENTS - 1);
  FmPatch &p = fmSynthPatch(instIdx);
  InstType iType = (InstType)p.type;

  if (instMidiMapOpen) {
    const int rowCount = instMidiMapRowCount(iType);
    syncInstMidiMapScroll(rowCount);
    const int slot = instMidiMapRowToSlot(iType, instMidiMapCursor);
    instMidiMapCancelLearn();
    instMidiMapClrConfirm = false;
    if (instMidiMapZone == INST_MAP_ZONE_LIST && slot >= 0) {
      instMidiMapClearSlot(instIdx, slot);
      uiShowToast("MAPPING CLEARED");
    }
    uiForceRedraw();
    return;
  }

  const InstEngineOps *ops = instEngineForType(iType);
  const FmPatch &def = fmSynthDefaultPatch(iType);
  const int genBase = instGenBaseRow();

  if (fmEditRow == FM_ROW_HEADER) {
    fmSynthResetToDefault(instIdx, true);
    uiShowToast("INST RESET");
  } else if (fmEditRow == genBase) {
    setEnvField(p.amp, fmEditCol, getEnvField(def.amp, fmEditCol));
  } else if (fmEditRow == genBase + 1) {
    setEnvField(p.flt, fmEditCol, getEnvField(def.flt, fmEditCol));
  } else if (fmEditRow == genBase + 2) {
    setFxField(p.fx, fmEditCol, getFxField(def.fx, fmEditCol));
  } else if (iType == InstType::Sampler) {
    if (fmEditRow == SMP_ROW_PRESS) {
      p.algorithm = (uint8_t)SamplerPressMode::Stop;
    } else if (fmEditRow == SMP_ROW_KEYS) {
      int keyIdx = constrain(fmEditCol, 0, SAMPLER_NUM_KEYS - 1);
      samplerClearKey(instIdx, keyIdx);
      uiShowToast("SAMPLE CLEARED");
    }
  } else if (iType == InstType::PhaseDist || iType == InstType::NoiseDrone) {
    int opIdx = fmEditRow - 1;
    if (ops && ops->resetMainCell) {
      ops->resetMainCell(p, def, opIdx, fmEditCol);
    }
  } else if (fmEditRow == FM_ROW_ALGO) {
    p.algorithm = def.algorithm;
  } else {
    int opIdx = (fmEditRow == FM_ROW_OP1_MAIN) ? 0 : 1;
    if (ops && ops->resetMainCell) {
      ops->resetMainCell(p, def, opIdx, fmEditCol);
    }
  }
  saveSessionWhenIdle();
  uiForceRedraw();
}

PROGMEM static const PageOps INST_EDIT_PAGE_OPS = {
  Page::InstrumentEdit,
  instEditHeaderTitle,
  instEditOnEnter,
  instEditInvalidateCache,
  instEditDrawPage,
  instEditOnShiftNavigate,
  instEditOnCursorMove,
  instEditOnOptAdjust,
  instEditOnEditPress,
  instEditOnResetParam
};

FLASHMEM const PageOps &pageInstrumentEditOps() {
  return INST_EDIT_PAGE_OPS;
}

// page_song_settings.cpp — Song Settings Page & Chromatic Tuner Subview UI & Input
#include "page_song_settings.h"
#include "page_system_settings.h"
#include "midi_in.h"
#include "tuner.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace m8;
using namespace looper;

static constexpr int PSET_VISIBLE_ROWS = 7;
static uint64_t prevSongSettingsKey[PSET_VISIBLE_ROWS] = {};
static int songSetCol = 0;
static int songSetScroll = 0;

// Chromatic Tuner subview UI state & per-band dirty cache keys
static bool songTunerActive = false;
static int tunerCol = 0; // 0 = SOURCE, 1 = A4 REF, 2 = LIVE/MUTE
static TunerSource tunerSelSource = TunerSource::LineIn;
static bool tunerMuteOut = false;

static uint64_t prevTunerCtrlKey[3] = {};
static uint64_t prevTunerNoteKey    = 0;
static uint64_t prevTunerHzKey      = 0;
static uint64_t prevTunerScaleKey   = 0;
static uint64_t prevTunerNeedleKey  = 0;
static uint64_t prevTunerStrobeKey  = 0;
static uint64_t prevTunerMeterKey   = 0;

// Audio Input FX subview UI state & per-box dirty cache keys
static constexpr int INPUT_FX_BOX_COUNT = AUD_FX_COUNT + 1; // 8: DRIVE..COMP + OUT VOL
static constexpr int INPUT_FX_VOL_IDX   = AUD_FX_COUNT;     // 7
static bool songInputFxActive = false;
static int inputFxCursor = 0; // 0..7 (AUD_FX_DRIVE .. AUD_FX_COMP, INPUT_FX_VOL_IDX)
static uint64_t prevInputFxHdrKey = 0;
static uint64_t prevInputFxBoxKey[INPUT_FX_BOX_COUNT] = {};

PROGMEM static const char *const INPUT_FX_BOX_LABELS[INPUT_FX_BOX_COUNT] = {
  "DRIVE",
  "CHORUS",
  "DELAY",
  "D.FB",
  "REVERB",
  "TAPE",
  "COMP",
  "OUT VOL"
};

PROGMEM static const char *const SONG_SET_LABELS[PSET_COUNT] = {
  "LINE IN GAIN",
  "AUDIO INPUT FX",
  "REC TRIG THRESH",
  "RECORD LINE/USB",
  "MONITOR LINE/USB",
  "CHROMATIC TUNER",
  "MIDI CLOCK OUT",
  "MIDI CLOCK SOURCE",
  "MIDI TRANSPORT IN",
  "MIDI THRU",
  "SONG INSTRUMENTS",
  "EXPORT MIDI TRACKS",
  "EXPORT MASTER OUT"
};

PROGMEM static const char *const TUNER_NOTE_NAMES[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

FLASHMEM static void invalidateInputFxCache() {
  prevInputFxHdrKey = 0;
  memset(prevInputFxBoxKey, 0, sizeof(prevInputFxBoxKey));
}

FLASHMEM bool isSongInputFxOpen() {
  return songInputFxActive;
}

FLASHMEM static void openSongInputFx() {
  closeSongTuner();
  songInputFxActive = true;
  inputFxCursor = constrain(inputFxCursor, 0, INPUT_FX_BOX_COUNT - 1);
  invalidateInputFxCache();
  uiForceRedraw();
}

FLASHMEM void closeSongInputFx() {
  if (!songInputFxActive) return;
  songInputFxActive = false;
  memset(prevSongSettingsKey, 0, sizeof(prevSongSettingsKey));
  invalidateInputFxCache();
  uiForceRedraw();
}

FLASHMEM static void invalidateTunerCache() {
  memset(prevTunerCtrlKey, 0, sizeof(prevTunerCtrlKey));
  prevTunerNoteKey   = 0;
  prevTunerHzKey     = 0;
  prevTunerScaleKey  = 0;
  prevTunerNeedleKey = 0;
  prevTunerStrobeKey = 0;
  prevTunerMeterKey  = 0;
}

FLASHMEM bool isSongTunerOpen() {
  return songTunerActive;
}

FLASHMEM static void openSongTuner() {
  closeSongInputFx();
  songTunerActive = true;
  tunerCol = constrain(tunerCol, 0, 2);
  tunerMuteOut = false;
  if ((uint8_t)tunerSelSource < 1 || (uint8_t)tunerSelSource > TUNER_SELECTABLE_SOURCES) {
    tunerSelSource = TunerSource::LineIn;
  }
  tunerResetDetector();
  tunerActivate(tunerSelSource, tunerMuteOut);
  invalidateTunerCache();
  uiForceRedraw();
}

FLASHMEM void closeSongTuner() {
  if (!songTunerActive && tunerActiveSource == TunerSource::Off && !tunerMuteActive) {
    return;
  }
  songTunerActive = false;
  tunerMuteOut = false;
  tunerDeactivate();
  memset(prevSongSettingsKey, 0, sizeof(prevSongSettingsKey));
  invalidateTunerCache();
  uiForceRedraw();
}

FLASHMEM static int songSettingColCount(int row) {
  if (row == PSET_REC_SRC) return 2;
  if (row == PSET_MONITOR) return 2;
  return 1;
}

FLASHMEM static const char *monitorModeLabel(MonitorMode m) {
  switch (m) {
    case MonitorMode::On:  return "ON";
    case MonitorMode::Arm: return "ARM";
    case MonitorMode::Off:
    default:               return "OFF";
  }
}

FLASHMEM static MonitorMode stepMonitorMode(MonitorMode cur, int delta) {
  // Cycle order: 0 = "ON", 1 = "ARM", 2 = "OFF"
  int idx = (cur == MonitorMode::On)  ? 0
          : (cur == MonitorMode::Arm) ? 1 : 2;
  int next = (idx + delta + 3) % 3;
  return (next == 0) ? MonitorMode::On
       : (next == 1) ? MonitorMode::Arm : MonitorMode::Off;
}

FLASHMEM static void drawMultiToggleSettingsRow(int y, uint32_t rowId, const char *label,
                                                int numCols, const char *const colVals[],
                                                const bool colOff[], bool sel, int selCol,
                                                uint64_t &cacheKey, bool full) {
  const int rw = SX(308), rh = SY(24);
  uint16_t bg = sel ? DARK : PANEL;
  uint16_t border = sel ? ACCENT : MID;
  uint16_t barCol = DIM;

  uint64_t rk = hashStr(hashInit(), label);
  for (int c = 0; c < numCols; c++) {
    rk = hashStr(rk, colVals[c]);
    rk = hashMix(rk, colOff[c] ? 1 : 0);
  }
  rk = hashMix(rk, (uint64_t)rowId | ((uint64_t)bg << 8) | ((uint64_t)border << 24) |
                   ((uint64_t)sel << 40) | ((uint64_t)(selCol + 1) << 44) |
                   ((uint64_t)numCols << 48));
  if (!full && rk == cacheKey) return;
  cacheKey = rk;

  Surface s(rw, rh);
  s.drawBox(bg, border);
  s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);
  s.drawText(SX(10), TY(rh), label, sel ? INK : LIGHT);

  const int pw = SX(34), ph = SY(16), gap = SX(4);
  const int totalW = numCols * pw + (numCols - 1) * gap;
  const int startX = rw - SX(6) - totalW;
  const int py = (rh - ph) / 2;

  for (int c = 0; c < numCols; c++) {
    const int px = startX + c * (pw + gap);
    const bool colSel = sel && (selCol == c);
    uint16_t pBg = colSel ? ACCENT : PANEL;
    uint16_t pFg = colSel ? BG : (colOff[c] ? DIM : LIGHT);
    uint16_t pBd = colSel ? INK : (colOff[c] ? DIM : MID);
    s.drawPill(px, py, pw, ph, colVals[c], pFg, pBg, pBd);
  }
  s.blit(SX(6), y);
}

// Integer-multiplied bitmap glyph helpers for 2x / 3x Chromatic Tuner Note readout
FLASHMEM static int scaledTextWidth(const char *text, int mult) {
  const int sc = FONT_SCALE * mult;
  const int gp = FONT_GAP * mult;
  int w = 0;
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (g) {
      w += (int)g->width * sc + gp;
    }
  }
  return (w > 0) ? (w - gp) : 0;
}

FLASHMEM static void drawScaledText(Surface &s, int x, int y, const char *text,
                                    uint16_t ink, int mult) {
  const int sc = FONT_SCALE * mult;
  const int gp = FONT_GAP * mult;
  int cx = x;
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    const int gw = (int)g->width;
    for (int r = 0; r < FONT_ROWS; r++) {
      const GlyphRowBits bits = g->rows[r];
      if (!bits) continue;
      const int py = y + r * sc;
      for (int c = 0; c < gw; c++) {
        if (bits & ((GlyphRowBits)1u << c)) {
          s.fillRect(cx + c * sc, py, sc, sc, ink);
        }
      }
    }
    cx += gw * sc + gp;
  }
}

FLASHMEM static void drawTunerCtrlBox(int x, int y, int w, int h, const char *txt,
                                      uint16_t fg, uint16_t bg, uint16_t border,
                                      uint64_t &cacheKey, bool full) {
  uint64_t k = hashStr(hashInit(), txt);
  k = hashMix(k, (uint64_t)fg | ((uint64_t)bg << 16) | ((uint64_t)border << 32));
  if (!full && k == cacheKey) return;
  cacheKey = k;

  Surface s(w, h);
  s.drawBox(bg, border);
  int tw = textWidth(txt);
  s.drawText(max(PX + 1, (w - tw) / 2), TY(h), txt, fg);
  s.blit(x, y);
}

FLASHMEM static void drawSongTunerView(bool full) {
  TunerReadout rd{};
  tunerService(rd);

  tunerCol = constrain(tunerCol, 0, 2);
  const uint16_t refT = tunerRefTenths();

  // 1. Top Control Row: [ SOURCE: LINE IN ]  [ A4 REF: 440.0 HZ ]  [ LIVE / MUTE ]
  {
    const int cy = SY(28), ch = SY(22);
    char srcTxt[24];
    snprintf(srcTxt, sizeof(srcTxt), "SOURCE: %s", tunerSourceLabel(tunerSelSource));
    const bool sel0 = (tunerCol == 0);
    drawTunerCtrlBox(SX(6), cy, SX(122), ch, srcTxt,
                     sel0 ? ACCENT : LIGHT,
                     sel0 ? DARK : PANEL,
                     sel0 ? ACCENT : MID,
                     prevTunerCtrlKey[0], full);

    char refTxt[24];
    snprintf(refTxt, sizeof(refTxt), "A4 REF: %u.%u HZ",
             (unsigned)(refT / 10u), (unsigned)(refT % 10u));
    const bool sel1 = (tunerCol == 1);
    drawTunerCtrlBox(SX(132), cy, SX(124), ch, refTxt,
                     sel1 ? ACCENT : LIGHT,
                     sel1 ? DARK : PANEL,
                     sel1 ? ACCENT : MID,
                     prevTunerCtrlKey[1], full);

    const bool sel2 = (tunerCol == 2);
    const char *lmTxt = tunerMuteOut ? "MUTE" : "LIVE";
    uint16_t lmBg = tunerMuteOut ? COLOR_REC_DIM : (sel2 ? DARK : PANEL);
    uint16_t lmBd = tunerMuteOut ? (sel2 ? INK : COLOR_REC) : (sel2 ? ACCENT : MID);
    uint16_t lmFg = tunerMuteOut ? (sel2 ? INK : COLOR_REC) : (sel2 ? ACCENT : COLOR_PLAY);
    drawTunerCtrlBox(SX(260), cy, SX(54), ch, lmTxt, lmFg, lmBg, lmBd,
                     prevTunerCtrlKey[2], full);
  }
  keepStreaming();

  const int centTenths = rd.locked ? (int)lroundf(rd.cents * 10.0f) : 0;
  const int absCentTenths = (centTenths >= 0) ? centTenths : -centTenths;
  const bool inTune  = rd.locked && (absCentTenths <= 20);  // +/- 2.0 cents
  const bool nearTune = rd.locked && (absCentTenths <= 100); // +/- 10.0 cents

  // 2. Large 3x Note + Octave with flanking << and >>
  {
    const int nw = SX(190), nh = SY(38);
    const int nx = (SCREEN_W - nw) / 2, ny = SY(56);
    char noteStr[8];
    uint16_t noteCol = DIM;
    uint16_t leftCol = DARK;
    uint16_t rightCol = DARK;

    if (rd.locked && rd.midiNote >= 0) {
      int semi = ((int)rd.midiNote % 12 + 12) % 12;
      int oct = ((int)rd.midiNote / 12) - 1;
      const char *nm = TUNER_NOTE_NAMES[semi];
      if (nm[1] == '#') {
        snprintf(noteStr, sizeof(noteStr), "%s%d", nm, oct);
      } else {
        snprintf(noteStr, sizeof(noteStr), "%s %d", nm, oct);
      }
      if (inTune) {
        noteCol  = COLOR_PLAY;
        leftCol  = COLOR_PLAY;
        rightCol = COLOR_PLAY;
      } else if (centTenths < 0) {
        noteCol  = nearTune ? COLOR_WARN : INK;
        leftCol  = nearTune ? COLOR_WARN : COLOR_OVERDUB;
        rightCol = MID;
      } else {
        noteCol  = nearTune ? COLOR_WARN : INK;
        leftCol  = MID;
        rightCol = nearTune ? COLOR_WARN : COLOR_OVERDUB;
      }
    } else {
      snprintf(noteStr, sizeof(noteStr), "- -");
    }

    uint64_t nk = hashStr(hashInit(), noteStr);
    nk = hashMix(nk, (uint64_t)noteCol | ((uint64_t)leftCol << 16) | ((uint64_t)rightCol << 32));
    if (full || nk != prevTunerNoteKey) {
      prevTunerNoteKey = nk;
      Surface s(nw, nh);
      s.fillRect(0, 0, nw, nh, BG);

      const int chevW = scaledTextWidth("<<", 2);
      const int chevY = (nh - FONT_H * 2) / 2;
      drawScaledText(s, SX(4), chevY, "<<", leftCol, 2);
      drawScaledText(s, nw - SX(4) - chevW, chevY, ">>", rightCol, 2);

      const int bigW = scaledTextWidth(noteStr, 3);
      const int bigY = (nh - FONT_H * 3) / 2;
      drawScaledText(s, (nw - bigW) / 2, bigY, noteStr, noteCol, 3);
      s.blit(nx, ny);
    }
  }

  // 3. Live Hz & Cents Readout: "439.8 HZ    -0.8 CENTS"
  {
    const int hw = SX(248), hh = SY(16);
    const int hx = (SCREEN_W - hw) / 2, hy = SY(98);
    char line[40];
    uint16_t fg = DIM;
    if (rd.locked && rd.hz > 0.0f) {
      int hzTenths = max(0, (int)lroundf(rd.hz * 10.0f));
      char centPart[20];
      if (centTenths == 0) {
        snprintf(centPart, sizeof(centPart), "0.0 CENTS");
      } else {
        snprintf(centPart, sizeof(centPart), "%c%d.%d CENTS",
                 (centTenths > 0) ? '+' : '-', absCentTenths / 10, absCentTenths % 10);
      }
      snprintf(line, sizeof(line), "%d.%d HZ    %s", hzTenths / 10, hzTenths % 10, centPart);
      fg = inTune ? COLOR_PLAY : LIGHT;
    } else {
      snprintf(line, sizeof(line), "---.- HZ    --- CENTS");
    }

    uint64_t hk = hashStr(hashInit(), line);
    hk = hashMix(hk, fg);
    if (full || hk != prevTunerHzKey) {
      prevTunerHzKey = hk;
      Surface s(hw, hh);
      s.fillRect(0, 0, hw, hh, BG);
      int tw = textWidth(line);
      s.drawText(max(0, (hw - tw) / 2), TY(hh), line, fg);
      s.blit(hx, hy);
    }
  }
  keepStreaming();

  // 4. Cent Scale Labels & Needle Bar: -50   -25   [ | ]   +25   +50
  const int barW = SX(308);
  const int barPad = SX(14);
  const int xMin = barPad;
  const int xMid = barW / 2;
  const int xMax = barW - 1 - barPad;
  const int xQ1  = (xMin + xMid) / 2;
  const int xQ3  = (xMid + xMax) / 2;

  {
    const int sy = SY(122), sh = SY(14);
    const uint16_t midTagCol = inTune ? COLOR_PLAY : (rd.locked ? INK : LIGHT);
    uint64_t sk = hashMix(hashInit(), midTagCol);
    if (full || sk != prevTunerScaleKey) {
      prevTunerScaleKey = sk;
      Surface s(barW, sh);
      s.fillRect(0, 0, barW, sh, BG);
      const int ty = TY(sh);
      s.drawText(max(0, xMin - textWidth("-50") / 2), ty, "-50", DIM);
      s.drawText(xQ1 - textWidth("-25") / 2, ty, "-25", DIM);
      s.drawText(xMid - textWidth("[ | ]") / 2, ty, "[ | ]", midTagCol);
      s.drawText(xQ3 - textWidth("+25") / 2, ty, "+25", DIM);
      s.drawText(min(barW - textWidth("+50"), xMax - textWidth("+50") / 2), ty, "+50", DIM);
      s.blit(SX(6), sy);
    }
  }

  {
    const int ny = SY(138), nh = SY(20);
    const int halfSpan = xMid - xMin;
    int needleX = xMid;
    uint16_t needleCol = DIM;
    if (rd.locked) {
      needleX = xMid + (int)lroundf((rd.cents / 50.0f) * (float)halfSpan);
      needleX = constrain(needleX, xMin, xMax);
      needleCol = inTune ? COLOR_PLAY : (nearTune ? COLOR_WARN : COLOR_OVERDUB);
    }

    uint64_t nk = hashMix(hashInit(), (uint64_t)(rd.locked ? 1u : 0u) |
                                          ((uint64_t)(uint16_t)needleX << 4) |
                                          ((uint64_t)needleCol << 24));
    if (full || nk != prevTunerNeedleKey) {
      prevTunerNeedleKey = nk;
      Surface s(barW, nh);
      s.drawBox(PANEL, MID);

      // Deflection fill between center and live needle
      if (rd.locked && needleX != xMid) {
        int fx0 = min(xMid, needleX);
        int fx1 = max(xMid, needleX);
        uint16_t fillCol = blend565(needleCol, PANEL, 90);
        s.fillRect(fx0, SY(5), fx1 - fx0 + 1, nh - 2 * SY(5), fillCol);
      }

      // 5-cent minor ticks & 25-cent major ticks
      for (int c = -50; c <= 50; c += 5) {
        int tx = xMid + (c * halfSpan) / 50;
        bool major = (c == -50 || c == -25 || c == 0 || c == 25 || c == 50);
        int th = major ? SY(5) : SY(3);
        uint16_t tc = (c == 0) ? LIGHT : (major ? DIM : MID);
        s.fillRect(tx, PX + 1, max(1, PX / 2), th, tc);
        s.fillRect(tx, nh - PX - 1 - th, max(1, PX / 2), th, tc);
      }

      // Center zero hairline
      s.fillRect(xMid, PX, 1, nh - 2 * PX, inTune ? COLOR_PLAY : MID);

      // Live needle indicator
      if (rd.locked) {
        const int nw = max(2, SX(3));
        s.fillRect(needleX - nw / 2, PX + 1, nw, nh - 2 * (PX + 1), needleCol);
      }
      s.blit(SX(6), ny);
    }
  }
  keepStreaming();

  // 5. Phase Strobe Strip
  {
    const int sy = SY(162), sh = SY(22);
    uint16_t hiCol = DIM;
    if (rd.locked) {
      hiCol = inTune ? COLOR_PLAY : (nearTune ? ACCENT : COLOR_WARN);
    } else {
      hiCol = blend565(DIM, DARK, 120);
    }
    const uint8_t ph8 = rd.locked ? rd.strobePhase8 : 0u;
    uint64_t sk = hashMix(hashInit(), (uint64_t)ph8 | ((uint64_t)hiCol << 16) |
                                          ((uint64_t)(rd.locked ? 1u : 0u) << 32));
    if (full || sk != prevTunerStrobeKey) {
      prevTunerStrobeKey = sk;
      Surface s(barW, sh);
      s.drawBox(PANEL, MID);

      const int ix0 = 2 * PX;
      const int ix1 = barW - 2 * PX;
      const int iy0 = 2 * PX;
      const int iy1 = sh - 2 * PX;
      const int bandW = max(8, SX(32));

      const uint16_t shadeLut[4] = {
        BG,
        DARK,
        rd.locked ? MID : blend565(MID, DARK, 110),
        hiCol
      };

      if (iy1 > iy0 && ix1 > ix0) {
        uint16_t *firstRow = s.px + iy0 * s.w;
        for (int cx = ix0; cx < ix1; cx++) {
          uint8_t p = (uint8_t)((((cx - ix0) * 256) / bandW - (int)ph8) & 0xFF);
          int tri = (p < 128u) ? ((int)p << 1) : ((255 - (int)p) << 1);
          int lvl = (tri >> 6) & 3; // 0..3 stepped shading bands
          firstRow[cx] = shadeLut[lvl];
        }
        const size_t rowBytes = (size_t)(ix1 - ix0) * sizeof(uint16_t);
        for (int ry = iy0 + 1; ry < iy1; ry++) {
          memcpy(s.px + ry * s.w + ix0, firstRow + ix0, rowBytes);
        }
      }
      // Subtle center reference ticks on strobe frame
      s.fillRect(xMid, PX, max(1, PX), PX, inTune ? COLOR_PLAY : LIGHT);
      s.fillRect(xMid, sh - 2 * PX, max(1, PX), PX, inTune ? COLOR_PLAY : LIGHT);
      s.blit(SX(6), sy);
    }
  }

  // 6. Input Level Meter: IN LVL [████████████░░░░░░░░░░░░░░]
  {
    const int my = SY(192), mh = SY(22);
    const int innerX = SX(58);
    const int innerW = barW - innerX - SX(8);
    const int segStep = max(3, SX(5));
    const int segW    = max(2, segStep - SX(1));
    const int numSegs = max(1, innerW / segStep);
    const int litSegs = constrain((int)lroundf(rd.levelNorm * (float)numSegs), 0, numSegs);

    uint64_t mk = hashMix(hashInit(), (uint64_t)(uint16_t)litSegs |
                                          ((uint64_t)(rd.clipping ? 1u : 0u) << 16));
    if (full || mk != prevTunerMeterKey) {
      prevTunerMeterKey = mk;
      Surface s(barW, mh);
      s.drawBox(PANEL, MID);
      s.drawText(SX(8), TY(mh), "IN LVL", LIGHT);

      const int by = SY(5);
      const int bh = mh - 2 * by;
      s.fillRect(innerX - PX, by - 1, innerW + 2 * PX, bh + 2, BG);

      for (int idx = 0; idx < numSegs; idx++) {
        int sx = innerX + idx * segStep;
        uint16_t col = DARK;
        if (idx < litSegs) {
          if (rd.clipping && idx >= numSegs - 2) {
            col = COLOR_REC;
          } else if (idx * 100 >= numSegs * 92) {
            col = COLOR_REC;
          } else if (idx * 100 >= numSegs * 76) {
            col = COLOR_WARN;
          } else {
            col = ACCENT;
          }
        }
        s.fillRect(sx, by, segW, bh, col);
      }

      // Gate threshold marker tick (~ -52 dBFS = 13% of -60..0 dB scale)
      const int gateX = innerX + (innerW * 13) / 100;
      s.fillRect(gateX, by - 1, 1, bh + 2, DIM);
      s.blit(SX(6), my);
    }
  }
  keepStreaming();
}

FLASHMEM static void drawInputFxParamBox(int x, int y, int w, int h,
                                         const char *label, uint8_t val, bool isVol,
                                         bool sel, uint64_t &cacheKey, bool full) {
  char valStr[10];
  if (isVol) {
    if (val == 0) {
      snprintf(valStr, sizeof(valStr), "OFF");
    } else {
      snprintf(valStr, sizeof(valStr), "%02u", (unsigned)val);
    }
  } else {
    snprintf(valStr, sizeof(valStr), "%u%%", (unsigned)val);
  }

  uint16_t bg = sel ? DARK : PANEL;
  uint16_t border = sel ? ACCENT : MID;
  uint16_t lblCol = sel ? INK : LIGHT;
  uint16_t valCol = sel ? ACCENT : (val > 0 ? LIGHT : DIM);

  uint64_t k = hashStr(hashInit(), label);
  k = hashStr(k, valStr);
  k = hashMix(k, (uint64_t)val | ((uint64_t)sel << 8) | ((uint64_t)isVol << 12) |
                     ((uint64_t)bg << 16) | ((uint64_t)border << 32));
  if (!full && k == cacheKey) return;
  cacheKey = k;

  Surface s(w, h);
  s.drawBox(bg, border);
  int lw = textWidth(label);
  s.drawText(max(PX + 1, (w - lw) / 2), SY(7), label, lblCol);
  int vw = textWidth(valStr);
  s.drawText(max(PX + 1, (w - vw) / 2), SY(21), valStr, valCol);

  const int barX = SX(6);
  const int barW = max(2, w - 2 * barX);
  const int barH = max(2, SY(4));
  const int barY = h - barH - SY(5);
  s.fillRect(barX, barY, barW, barH, sel ? BG : DARK);
  if (val > 0) {
    int fillW = max(1, (barW * (int)val) / 100);
    s.fillRect(barX, barY, fillW, barH, ACCENT);
  }
  if (isVol) {
    int uX = barX + (barW * VOLUME_UNITY) / 100;
    s.fillRect(uX, barY - 1, 1, barH + 2, sel ? INK : LIGHT);
  }
  s.blit(x, y);
}

FLASHMEM static void drawSongInputFxView(bool full) {
  inputFxCursor = constrain(inputFxCursor, 0, INPUT_FX_BOX_COUNT - 1);

  // Top banner: "AUDIO INPUT FX  (LINE + USB)"
  {
    const int bx = SX(6), by = SY(32), bw = SX(308), bh = SY(22);
    uint64_t hk = hashMix(hashInit(), 0x494E4658u);
    if (full || hk != prevInputFxHdrKey) {
      prevInputFxHdrKey = hk;
      Surface s(bw, bh);
      s.drawBox(PANEL, MID);
      const char *hdrTxt = "AUDIO INPUT FX  (LINE + USB)";
      s.drawText(max(SX(6), (bw - textWidth(hdrTxt)) / 2), TY(bh), hdrTxt, LIGHT);
      s.blit(bx, by);

      Surface sHint(bw, SY(18));
      sHint.fillRect(0, 0, bw, SY(18), BG);
      sHint.drawText(SX(2), TY(SY(18)), "PRE-MONITOR & PRE-RECORD", DIM);
      const char *backTxt = "OPT: BACK";
      sHint.drawText(bw - SX(2) - textWidth(backTxt), TY(SY(18)), backTxt, DIM);
      sHint.blit(bx, SY(188));
    }
  }
  keepStreaming();

  // Row 0: [ DRIVE ] [ CHORUS ] [ DELAY ] [ D.FB ] (4 boxes across SX(308))
  const int boxH = SY(48);
  const int boxW = SX(74), boxPitch = SX(78);
  const int r0Y = SY(68);
  for (int c = 0; c < 4; c++) {
    uint8_t pct = getAudioInputFx((AudioTrackFxSlot)c);
    drawInputFxParamBox(SX(6) + c * boxPitch, r0Y, boxW, boxH,
                        INPUT_FX_BOX_LABELS[c], pct, false, inputFxCursor == c,
                        prevInputFxBoxKey[c], full);
  }
  keepStreaming();

  // Row 1: [ REVERB ] [ TAPE ] [ COMP ] [ OUT VOL ] (4 boxes across SX(308))
  const int r1Y = SY(124);
  for (int c = 4; c < INPUT_FX_BOX_COUNT; c++) {
    const bool isVol = (c == INPUT_FX_VOL_IDX);
    uint8_t val = isVol ? getAudioInputVolume() : getAudioInputFx((AudioTrackFxSlot)c);
    drawInputFxParamBox(SX(6) + (c - 4) * boxPitch, r1Y, boxW, boxH,
                        INPUT_FX_BOX_LABELS[c], val, isVol, inputFxCursor == c,
                        prevInputFxBoxKey[c], full);
  }
  keepStreaming();
}

FLASHMEM static const char *songSettingsHeaderTitle() {
  return songInputFxActive ? "IN FX" : (songTunerActive ? "TUNER" : "SONG");
}

FLASHMEM static void songSettingsInvalidateCache() {
  memset(prevSongSettingsKey, 0, sizeof(prevSongSettingsKey));
  invalidateTunerCache();
  invalidateInputFxCache();
  invalidateSysPresetsModalCache();
}

FLASHMEM static void songSettingsDrawPage(bool full) {
  if (isSysPresetsModalOpen()) {
    drawSysPresetsModal(full);
    return;
  }
  if (songInputFxActive) {
    drawSongInputFxView(full);
    return;
  }
  if (songTunerActive) {
    drawSongTunerView(full);
    return;
  }

  songSetRow = constrain(songSetRow, 0, PSET_COUNT - 1);
  songSetCol = constrain(songSetCol, 0, songSettingColCount(songSetRow) - 1);
  if (songSetRow < songSetScroll) {
    songSetScroll = songSetRow;
  } else if (songSetRow >= songSetScroll + PSET_VISIBLE_ROWS) {
    songSetScroll = songSetRow - PSET_VISIBLE_ROWS + 1;
  }
  songSetScroll = constrain(songSetScroll, 0, max(0, PSET_COUNT - PSET_VISIBLE_ROWS));

  const SongSettings &ps = songSettings();

  for (int vis = 0; vis < PSET_VISIBLE_ROWS; vis++) {
    int i = songSetScroll + vis;
    int y = SY(28) + vis * SY(26);
    bool sel = (songSetRow == i);

    if (i == PSET_REC_SRC) {
      const char *vals[2] = {
        ps.recLineIn ? "ON" : "OFF",
        ps.recUsbIn ? "ON" : "OFF"
      };
      const bool offs[2] = {
        !ps.recLineIn,
        !ps.recUsbIn
      };
      drawMultiToggleSettingsRow(y, (uint32_t)i, SONG_SET_LABELS[i], 2, vals, offs,
                                 sel, songSetCol, prevSongSettingsKey[vis], full);
      keepStreaming();
      continue;
    }

    if (i == PSET_MONITOR) {
      const char *vals[2] = {
        monitorModeLabel(ps.lineMonitor),
        monitorModeLabel(ps.usbMonitor)
      };
      const bool offs[2] = {
        ps.lineMonitor == MonitorMode::Off,
        ps.usbMonitor == MonitorMode::Off
      };
      drawMultiToggleSettingsRow(y, (uint32_t)i, SONG_SET_LABELS[i], 2, vals, offs,
                                 sel, songSetCol, prevSongSettingsKey[vis], full);
      keepStreaming();
      continue;
    }

    char val[36];
    val[0] = '\0';
    bool isOff = false;
    bool isPill = false;

    switch (i) {
      case PSET_LINE_IN_GAIN: {
        int tenths = constrain((int)ps.lineInGainTenths,
                               (int)MIN_LINE_IN_GAIN_TENTHS,
                               (int)MAX_LINE_IN_GAIN_TENTHS);
        if (tenths == 0) {
          snprintf(val, sizeof(val), "0.0 DB");
        } else {
          int absT = (tenths > 0) ? tenths : -tenths;
          snprintf(val, sizeof(val), "%c%d.%d DB",
                   (tenths > 0) ? '+' : '-', absT / 10, absT % 10);
        }
        break;
      }
      case PSET_DEF_THRESH:
        snprintf(val, sizeof(val), "%d DB", (int)ps.defaultThreshDb);
        break;
      case PSET_TUNER:
      case PSET_AUDIO_INPUT_FX:
        isPill = true;
        snprintf(val, sizeof(val), "OPEN");
        break;
      case PSET_CLOCK_OUT:
        switch (ps.midiClockOut) {
          case MidiClockOut::Off:
            isOff = true;
            snprintf(val, sizeof(val), "OFF");
            break;
          case MidiClockOut::Usb:
            snprintf(val, sizeof(val), "USB ONLY");
            break;
          case MidiClockOut::Trs:
            snprintf(val, sizeof(val), "TRS ONLY");
            break;
          case MidiClockOut::Both:
            snprintf(val, sizeof(val), "BOTH (USB+TRS)");
            break;
        }
        break;
      case PSET_CLOCK: {
        bool locked = midiClockFollowing();
        switch (ps.clockSource) {
          case ClockSource::Internal:
            snprintf(val, sizeof(val), "INTERNAL");
            break;
          case ClockSource::ExternalAuto:
            if (locked) {
              snprintf(val, sizeof(val), midiFollowSource() == MIDI_USB ? "EXT AUTO (USB)" : "EXT AUTO (TRS)");
            } else {
              snprintf(val, sizeof(val), "EXT AUTO (WAITING)");
            }
            isOff = !locked;
            break;
          case ClockSource::ExternalUsb:
            snprintf(val, sizeof(val), locked ? "EXT USB (LOCKED)" : "EXT USB (WAITING)");
            isOff = !locked;
            break;
          case ClockSource::ExternalTrs:
            snprintf(val, sizeof(val), locked ? "EXT TRS (LOCKED)" : "EXT TRS (WAITING)");
            isOff = !locked;
            break;
        }
        break;
      }
      case PSET_TRANSPORT_IN:
        switch (ps.midiTransportIn) {
          case MidiTransportIn::Auto:
            snprintf(val, sizeof(val), "AUTO (USB+TRS)");
            break;
          case MidiTransportIn::Usb:
            snprintf(val, sizeof(val), "USB ONLY");
            break;
          case MidiTransportIn::Trs:
            snprintf(val, sizeof(val), "TRS ONLY");
            break;
          case MidiTransportIn::Off:
            isOff = true;
            snprintf(val, sizeof(val), "OFF");
            break;
        }
        break;
      case PSET_MIDI_THRU:
        isOff = !ps.midiThru;
        snprintf(val, sizeof(val), ps.midiThru ? "ON" : "OFF");
        break;
      case PSET_SAVED_INST:
        isPill = true;
        snprintf(val, sizeof(val), "MANAGE");
        break;
      case PSET_EXPORT_MIDI:
      case PSET_EXPORT_AUDIO:
        isPill = true;
        snprintf(val, sizeof(val), "EXPORT");
        break;
    }

    drawSettingsTableRow(y, (uint32_t)i, SONG_SET_LABELS[i], val, sel, isOff, isPill,
                         prevSongSettingsKey[vis], full);
    keepStreaming();
  }
}

FLASHMEM static void songSettingsOnShiftNavigate(uint8_t dir) {
  if (songInputFxActive) {
    if (dir & BTN_LEFT) {
      closeSongInputFx();
      inputSwallowModalKeys();
    } else if (dir & BTN_RIGHT) {
      closeSongInputFx();
      setPage(Page::Main);
    } else if (dir & BTN_DOWN) {
      closeSongInputFx();
      setPage(lastRow2Page());
    }
    return;
  }
  if (songTunerActive) {
    if (dir & BTN_LEFT) {
      closeSongTuner();
      inputSwallowModalKeys();
    } else if (dir & BTN_RIGHT) {
      closeSongTuner();
      setPage(Page::Main);
    } else if (dir & BTN_DOWN) {
      closeSongTuner();
      setPage(lastRow2Page());
    }
    return;
  }

  if (dir & BTN_LEFT) setPage(Page::SystemSettings);
  else if (dir & BTN_RIGHT) setPage(Page::Main);
  else if (dir & BTN_DOWN) setPage(lastRow2Page());
}

FLASHMEM static void stepTunerSource(int step) {
  int cur = constrain((int)tunerSelSource, 1, (int)TUNER_SELECTABLE_SOURCES) - 1;
  int next = (cur + step + (int)TUNER_SELECTABLE_SOURCES) % (int)TUNER_SELECTABLE_SOURCES;
  tunerSelSource = (TunerSource)(next + 1);
  tunerActivate(tunerSelSource, tunerMuteOut);
}

FLASHMEM static void songSettingsOnOptAdjust(int dx, int dy, int delta) {
  if (songInputFxActive) {
    int step = (dx != 0) ? dx : (dy != 0 ? dy * 5 : delta * 5);
    int idx = constrain(inputFxCursor, 0, INPUT_FX_BOX_COUNT - 1);
    if (idx == INPUT_FX_VOL_IDX) {
      int cur = (int)getAudioInputVolume();
      int next = constrain(cur + step, 0, VOLUME_MAX);
      if (next != cur) {
        setAudioInputVolume(next);
        saveSessionWhenIdle();
        uiForceRedraw();
      }
    } else {
      AudioTrackFxSlot slot = (AudioTrackFxSlot)idx;
      int cur = (int)getAudioInputFx(slot);
      int next = constrain(cur + step, 0, 100);
      if (next != cur) {
        setAudioInputFx(slot, next);
        saveSessionWhenIdle();
        uiForceRedraw();
      }
    }
    return;
  }
  if (songTunerActive) {
    if (tunerCol == 0) {
      int step = (dx != 0) ? dx : dy;
      if (step == 0) step = delta;
      stepTunerSource(step);
    } else if (tunerCol == 1) {
      int step = (dx != 0) ? dx : (dy != 0 ? dy * 10 : delta * 10);
      uint16_t nextRef = (uint16_t)constrain((int)tunerRefTenths() + step,
                                             (int)MIN_TUNER_REF_TENTHS,
                                             (int)MAX_TUNER_REF_TENTHS);
      if (nextRef != tunerRefTenths()) {
        setTunerRefTenths(nextRef);
        saveSessionWhenIdle();
      }
    } else if (tunerCol == 2) {
      tunerMuteOut = !tunerMuteOut;
      tunerActivate(tunerSelSource, tunerMuteOut);
    }
    uiForceRedraw();
    return;
  }

  SongSettings &ps = songSettings();
  switch (songSetRow) {
    case PSET_REC_SRC: {
      int col = constrain(songSetCol, 0, 1);
      if (col == 0) ps.recLineIn = !ps.recLineIn;
      else ps.recUsbIn = !ps.recUsbIn;
      applySongSettings();
      saveSessionWhenIdle();
      break;
    }
    case PSET_MONITOR: {
      int col = constrain(songSetCol, 0, 1);
      if (col == 0) {
        ps.lineMonitor = stepMonitorMode(ps.lineMonitor, delta);
      } else {
        ps.usbMonitor = stepMonitorMode(ps.usbMonitor, delta);
      }
      applySongSettings();
      saveSessionWhenIdle();
      break;
    }
    case PSET_LINE_IN_GAIN: {
      int step = (dx != 0) ? dx : (dy != 0 ? dy * 10 : delta * 10);
      ps.lineInGainTenths = (int16_t)constrain((int)ps.lineInGainTenths + step,
                                               (int)MIN_LINE_IN_GAIN_TENTHS,
                                               (int)MAX_LINE_IN_GAIN_TENTHS);
      applySongSettings();
      saveSessionWhenIdle();
      break;
    }
    case PSET_DEF_THRESH: {
      int8_t oldDef = ps.defaultThreshDb;
      ps.defaultThreshDb = (int8_t)constrain((int)ps.defaultThreshDb + delta * 2, MIN_THRESH_DB, MAX_THRESH_DB);
      for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
        if (tracks[t].recordedFrames == 0 || tracks[t].threshDb == oldDef) {
          tracks[t].threshDb = ps.defaultThreshDb;
        }
      }
      if (transport.waitAudioThresh) {
        transport.audioThreshLinear = powf(10.0f, (float)ps.defaultThreshDb / 20.0f);
      }
      saveSessionWhenIdle();
      break;
    }
    case PSET_CLOCK_OUT: {
      int next = ((int)ps.midiClockOut + delta + 4) % 4;
      ps.midiClockOut = (MidiClockOut)next;
      saveSessionWhenIdle();
      break;
    }
    case PSET_CLOCK: {
      int next = ((int)ps.clockSource + delta + 4) % 4;
      ps.clockSource = (ClockSource)next;
      midiInResetClockState();
      if (ps.clockSource == ClockSource::Internal) {
        followTempo(spbForBpm(ps.bpm), ps.bpm);
      }
      saveSessionWhenIdle();
      break;
    }
    case PSET_TRANSPORT_IN: {
      int next = ((int)ps.midiTransportIn + delta + 4) % 4;
      ps.midiTransportIn = (MidiTransportIn)next;
      saveSessionWhenIdle();
      break;
    }
    case PSET_MIDI_THRU:
      ps.midiThru = !ps.midiThru;
      saveSessionWhenIdle();
      break;
    case PSET_TUNER:
    case PSET_AUDIO_INPUT_FX:
    case PSET_SAVED_INST:
    case PSET_EXPORT_MIDI:
    case PSET_EXPORT_AUDIO:
      return;
  }
  uiForceRedraw();
}

FLASHMEM static void songSettingsOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  if (songInputFxActive) {
    int next = constrain(inputFxCursor, 0, INPUT_FX_BOX_COUNT - 1);
    if (dy > 0 && next < 4) {
      next += 4;
    } else if (dy < 0 && next >= 4) {
      next -= 4;
    }
    if (dx != 0) {
      next = constrain(next + dx, 0, INPUT_FX_BOX_COUNT - 1);
    }
    if (next != inputFxCursor) {
      inputFxCursor = next;
      uiForceRedraw();
    }
    return;
  }
  if (songTunerActive) {
    if (dx != 0) {
      int nextCol = constrain(tunerCol + dx, 0, 2);
      if (nextCol != tunerCol) {
        tunerCol = nextCol;
        uiForceRedraw();
      }
    }
    return;
  }

  bool moved = false;
  if (dy != 0) {
    int nextRow = constrain(songSetRow + dy, 0, PSET_COUNT - 1);
    if (nextRow != songSetRow) {
      songSetRow = nextRow;
      songSetCol = constrain(songSetCol, 0, songSettingColCount(songSetRow) - 1);
      moved = true;
    }
  }
  if (dx != 0) {
    int maxCol = songSettingColCount(songSetRow) - 1;
    int nextCol = constrain(songSetCol + dx, 0, maxCol);
    if (nextCol != songSetCol) {
      songSetCol = nextCol;
      moved = true;
    }
  }
  if (moved) {
    uiForceRedraw();
  }
}

FLASHMEM static void songSettingsOnEditPress(bool shiftHeld) {
  if (songInputFxActive) {
    return;
  }
  if (songTunerActive) {
    if (shiftHeld) {
      uint16_t matched = DEFAULT_TUNER_REF_TENTHS;
      if (tunerMatchRefToCurrentPitch(matched)) {
        setTunerRefTenths(matched);
        saveSessionWhenIdle();
        char msg[28];
        snprintf(msg, sizeof(msg), "MATCHED A4: %u.%u HZ",
                 (unsigned)(matched / 10u), (unsigned)(matched % 10u));
        uiShowToast(msg);
      } else {
        uiShowToast("NO PITCH LOCKED");
      }
      uiForceRedraw();
      return;
    }
    if (tunerCol == 0) {
      stepTunerSource(1);
      uiForceRedraw();
    } else if (tunerCol == 2) {
      tunerMuteOut = !tunerMuteOut;
      tunerActivate(tunerSelSource, tunerMuteOut);
      uiForceRedraw();
    }
    return;
  }

  if (shiftHeld) return;

  if (songSetRow == PSET_TUNER) {
    openSongTuner();
    inputSwallowModalKeys();
  } else if (songSetRow == PSET_AUDIO_INPUT_FX) {
    openSongInputFx();
    inputSwallowModalKeys();
  } else if (songSetRow == PSET_SAVED_INST) {
    openSysPresetsModal(true);
    inputSwallowModalKeys();
    uiForceRedraw();
  } else if (songSetRow == PSET_EXPORT_MIDI) {
    if (transport.mode != TransportMode::Stopped) {
      uiShowToast("STOP TRANSPORT FIRST");
      return;
    }
    bool ok = midiSeqExportAllTracksMid(bpm);
    uiShowToast(ok ? "EXPORTED MIDI TRACKS" : "MIDI EXPORT FAILED");
    uiForceRedraw();
  } else if (songSetRow == PSET_EXPORT_AUDIO) {
    if (transport.mode != TransportMode::Stopped) {
      uiShowToast("STOP TRANSPORT FIRST");
      return;
    }
    MasterExportResult res = storageExportMasterAudioWav();
    if (res == MasterExportResult::Ok) {
      uiShowToast("EXPORTED MASTER WAV");
    } else if (res == MasterExportResult::Empty) {
      uiShowToast("SONG IS EMPTY");
    } else if (res == MasterExportResult::Cancelled) {
      uiShowToast("EXPORT CANCELLED");
    } else {
      uiShowToast("EXPORT FAILED");
    }
    uiForceRedraw();
  } else if (songSetRow == PSET_REC_SRC || songSetRow == PSET_MONITOR || songSetRow == PSET_MIDI_THRU) {
    songSettingsOnOptAdjust(0, 0, 1);
  }
}

FLASHMEM static void songSettingsOnResetParam() {
  if (songInputFxActive) {
    int idx = constrain(inputFxCursor, 0, INPUT_FX_BOX_COUNT - 1);
    if (idx == INPUT_FX_VOL_IDX) {
      if (getAudioInputVolume() != VOLUME_UNITY) {
        setAudioInputVolume(VOLUME_UNITY);
        saveSessionWhenIdle();
        uiForceRedraw();
      }
    } else {
      AudioTrackFxSlot slot = (AudioTrackFxSlot)idx;
      if (getAudioInputFx(slot) != 0) {
        setAudioInputFx(slot, 0);
        saveSessionWhenIdle();
        uiForceRedraw();
      }
    }
    return;
  }
  if (songTunerActive) {
    if (tunerCol == 0) {
      tunerSelSource = TunerSource::LineIn;
      tunerActivate(tunerSelSource, tunerMuteOut);
    } else if (tunerCol == 1) {
      if (tunerRefTenths() != DEFAULT_TUNER_REF_TENTHS) {
        setTunerRefTenths(DEFAULT_TUNER_REF_TENTHS);
        saveSessionWhenIdle();
      }
    } else if (tunerCol == 2) {
      tunerMuteOut = false;
      tunerActivate(tunerSelSource, tunerMuteOut);
    }
    uiForceRedraw();
    return;
  }

  if (songSetRow == PSET_TUNER || songSetRow == PSET_AUDIO_INPUT_FX ||
      songSetRow == PSET_SAVED_INST ||
      songSetRow == PSET_EXPORT_MIDI || songSetRow == PSET_EXPORT_AUDIO) {
    return;
  }
  SongSettings &ps = songSettings();
  switch (songSetRow) {
    case PSET_REC_SRC: {
      int col = constrain(songSetCol, 0, 1);
      if (col == 0) ps.recLineIn = true;
      else ps.recUsbIn = true;
      applySongSettings();
      break;
    }
    case PSET_MONITOR: {
      int col = constrain(songSetCol, 0, 1);
      if (col == 0) ps.lineMonitor = MonitorMode::On;
      else ps.usbMonitor = MonitorMode::On;
      applySongSettings();
      break;
    }
    case PSET_LINE_IN_GAIN:
      ps.lineInGainTenths = DEFAULT_LINE_IN_GAIN_TENTHS;
      applySongSettings();
      break;
    case PSET_DEF_THRESH: {
      int8_t oldDef = ps.defaultThreshDb;
      ps.defaultThreshDb = DEFAULT_THRESH_DB;
      for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
        if (tracks[t].recordedFrames == 0 || tracks[t].threshDb == oldDef) {
          tracks[t].threshDb = ps.defaultThreshDb;
        }
      }
      if (transport.waitAudioThresh) {
        transport.audioThreshLinear = powf(10.0f, (float)ps.defaultThreshDb / 20.0f);
      }
      break;
    }
    case PSET_CLOCK_OUT:
      ps.midiClockOut = MidiClockOut::Off;
      break;
    case PSET_CLOCK:
      ps.clockSource = ClockSource::Internal;
      midiInResetClockState();
      followTempo(spbForBpm(ps.bpm), ps.bpm);
      break;
    case PSET_TRANSPORT_IN:
      ps.midiTransportIn = MidiTransportIn::Auto;
      break;
    case PSET_MIDI_THRU:
      ps.midiThru = false;
      break;
  }
  saveSessionWhenIdle();
  uiForceRedraw();
}

PROGMEM static const PageOps SONG_SETTINGS_PAGE_OPS = {
  Page::SongSettings,
  songSettingsHeaderTitle,
  nullptr,
  songSettingsInvalidateCache,
  songSettingsDrawPage,
  songSettingsOnShiftNavigate,
  songSettingsOnCursorMove,
  songSettingsOnOptAdjust,
  songSettingsOnEditPress,
  songSettingsOnResetParam
};

FLASHMEM const PageOps &pageSongSettingsOps() {
  return SONG_SETTINGS_PAGE_OPS;
}

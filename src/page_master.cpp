// page_master.cpp — Master Page (8 Audio/MIDI Track Strips + 6 Master FX + Master VOL + Transport) UI & Input
#include "page_master.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

using namespace m8;
using namespace looper;

static uint64_t prevMasterTrackHdrKey[TRACK_ROWS] = {};
static uint64_t prevMasterTrackBodyKey[TRACK_ROWS] = {};
static uint64_t prevMasterFxKey[MASTER_FX_COUNT] = {};
static uint64_t prevMasterVolKey = 0;
static float masterSmoothPeakL[NUM_AUDIO_TRACKS] = {};
static float masterSmoothPeakR[NUM_AUDIO_TRACKS] = {};

static int lastMasterTrack = 0;
static int lastMasterMsSide = 0; // 0 = M (Mute), 1 = S (Solo)

FLASHMEM static const char *masterHeaderTitle() {
  return "MASTER";
}

FLASHMEM static void masterInvalidateCache() {
  memset(prevMasterTrackHdrKey, 0, sizeof(prevMasterTrackHdrKey));
  memset(prevMasterTrackBodyKey, 0, sizeof(prevMasterTrackBodyKey));
  memset(prevMasterFxKey, 0, sizeof(prevMasterFxKey));
  prevMasterVolKey = 0;
}

FLASHMEM static uint8_t getMasterFxPct(const SongSettings &ps, int col) {
  switch (col) {
    case MASTER_FX_CHOR: return ps.masterChorus;
    case MASTER_FX_DLAY: return ps.masterDelay;
    case MASTER_FX_DFB:  return ps.masterDelayFeedback;
    case MASTER_FX_RVRB: return ps.masterReverb;
    case MASTER_FX_TAPE: return ps.masterTape;
    case MASTER_FX_COMP: return ps.masterComp;
    default:             return 0;
  }
}

FLASHMEM static void setMasterFxPct(SongSettings &ps, int col, int nextPct) {
  uint8_t clamped = (uint8_t)constrain(nextPct, 0, 100);
  switch (col) {
    case MASTER_FX_CHOR:
      if (ps.masterChorus == clamped) return;
      ps.masterChorus = clamped;
      break;
    case MASTER_FX_DLAY:
      if (ps.masterDelay == clamped) return;
      ps.masterDelay = clamped;
      break;
    case MASTER_FX_DFB:
      if (ps.masterDelayFeedback == clamped) return;
      ps.masterDelayFeedback = clamped;
      break;
    case MASTER_FX_RVRB:
      if (ps.masterReverb == clamped) return;
      ps.masterReverb = clamped;
      break;
    case MASTER_FX_TAPE:
      if (ps.masterTape == clamped) return;
      ps.masterTape = clamped;
      break;
    case MASTER_FX_COMP:
      if (ps.masterComp == clamped) return;
      ps.masterComp = clamped;
      break;
    default:
      return;
  }
  setMasterFxSlot(col, clamped);
  saveSessionWhenIdle();
}

FLASHMEM static int activeMasterTrackIndex() {
  if (masterRow == MASTER_ROW_TRK_PARAM1 || masterRow == MASTER_ROW_TRK_PARAM2) {
    return constrain(masterCell, 0, TRACK_ROWS - 1);
  }
  if (masterRow == MASTER_ROW_TRK_MS) {
    return constrain(masterCell / 2, 0, TRACK_ROWS - 1);
  }
  return -1;
}

FLASHMEM static void drawMsToggleBtn(Surface &s, int bx, int by, int bw, int bh,
                                     const char *lbl, bool active, bool sel,
                                     uint16_t activeColor, uint16_t trkAccent) {
  uint16_t bg = active ? activeColor : (sel ? DARK : PANEL);
  uint16_t border = active ? (sel ? INK : activeColor) : (sel ? trkAccent : MID);
  uint16_t fg = active ? BG : (sel ? trkAccent : DIM);

  if (active && sel) {
    s.fillRect(bx, by, bw, bh, border);
    s.fillRect(bx + PX, by + PX, bw - 2 * PX, bh - 2 * PX, BG);
    s.fillRect(bx + 2 * PX, by + 2 * PX, bw - 4 * PX, bh - 4 * PX, bg);
  } else {
    s.fillRect(bx, by, bw, bh, border);
    s.fillRect(bx + PX, by + PX, bw - 2 * PX, bh - 2 * PX, bg);
  }
  int tw = textWidth(lbl);
  s.drawText(bx + max(1, (bw - tw) / 2), by + TY(bh), lbl, fg);
}

FLASHMEM static void drawMasterTrackBlock(int t, int x, int y, int w, int h,
                                          bool midiMode, bool anySolo, bool full) {
  const uint16_t trkAccent = midiMode ? COLOR_MIDI : ACCENT;
  const int focusTrk = activeMasterTrackIndex();
  const bool trkFocused = (focusTrk == t);
  const bool selParam1 = (masterRow == MASTER_ROW_TRK_PARAM1 && masterCell == t);
  const bool selParam2 = (masterRow == MASTER_ROW_TRK_PARAM2 && masterCell == t);
  const bool selM      = (masterRow == MASTER_ROW_TRK_MS && masterCell == t * 2);
  const bool selS      = (masterRow == MASTER_ROW_TRK_MS && masterCell == t * 2 + 1);

  bool isMuted = false;
  bool isSolo = false;
  bool row0Active = true;
  bool row1Active = true;
  bool row1Highlighted = false;
  char numStr[8];
  char row0Str[10];
  char row1Str[10];

  snprintf(numStr, sizeof(numStr), "%c%d", midiMode ? 'M' : 'A', t + 1);

  const bool isMono = isAudioTrackMono(t);
  const int hdrH = SY(25);
  const int barTopY = PX + SY(2);
  const int barMaxH = max(4, (hdrH - SY(2)) - barTopY);
  int meterHL = 0, meterHR = 0;
  bool clipL = false, clipR = false;
  bool midiAct = false;

  if (!midiMode) {
    const Track &tr = tracks[t];
    isMuted = (tr.state == TrackState::Muted);
    isSolo = tr.solo;
    row0Active = (tr.volume > 0);
    if (tr.volume <= 0) {
      snprintf(row0Str, sizeof(row0Str), "VOFF");
    } else {
      snprintf(row0Str, sizeof(row0Str), "V%02d", (int)tr.volume);
    }

    const char *panLbl = isMono ? "PAN" : "BAL";
    row1Highlighted = (tr.balance != 0);
    if (tr.balance == 0) {
      snprintf(row1Str, sizeof(row1Str), "%s", panLbl);
    } else if (tr.balance <= -PAN_MAX) {
      snprintf(row1Str, sizeof(row1Str), "L");
    } else if (tr.balance >= PAN_MAX) {
      snprintf(row1Str, sizeof(row1Str), "R");
    } else if (tr.balance < 0) {
      snprintf(row1Str, sizeof(row1Str), "L%02d", -(int)tr.balance);
    } else {
      snprintf(row1Str, sizeof(row1Str), "R%02d", (int)tr.balance);
    }

    float pL = 0.0f, pR = 0.0f;
    takeTrackPeaks(t, pL, pR);
    masterSmoothPeakL[t] = max(pL, masterSmoothPeakL[t] * 0.78f);
    masterSmoothPeakR[t] = max(pR, masterSmoothPeakR[t] * 0.78f);
    if (masterSmoothPeakL[t] < 0.002f) masterSmoothPeakL[t] = 0.0f;
    if (masterSmoothPeakR[t] < 0.002f) masterSmoothPeakR[t] = 0.0f;

    if (isMono) {
      float pk = max(masterSmoothPeakL[t], masterSmoothPeakR[t]);
      meterHL = constrain((int)(sqrtf(pk) * (float)barMaxH + 0.5f), 0, barMaxH);
      clipL = (pk > 0.9f);
    } else {
      meterHL = constrain((int)(sqrtf(masterSmoothPeakL[t]) * (float)barMaxH + 0.5f), 0, barMaxH);
      meterHR = constrain((int)(sqrtf(masterSmoothPeakR[t]) * (float)barMaxH + 0.5f), 0, barMaxH);
      clipL = (masterSmoothPeakL[t] > 0.9f);
      clipR = (masterSmoothPeakR[t] > 0.9f);
    }
  } else {
    const MidiTrack &mt = midiTracks[t];
    isMuted = mt.muted;
    isSolo = mt.solo;
    row0Active = (mt.inChannel != MIDI_IN_OFF);
    row1Active = (mt.outChannel != 0);
    if (mt.inChannel == MIDI_IN_OFF) {
      snprintf(row0Str, sizeof(row0Str), "OFF");
    } else if (mt.inChannel == 0) {
      snprintf(row0Str, sizeof(row0Str), "ALL");
    } else {
      snprintf(row0Str, sizeof(row0Str), "IN%02d", (int)mt.inChannel);
    }
    if (mt.outChannel == 0) {
      snprintf(row1Str, sizeof(row1Str), "OFF");
    } else if (mt.outChannel >= 17) {
      snprintf(row1Str, sizeof(row1Str), "I%02d", (int)mt.outChannel - 16);
    } else {
      snprintf(row1Str, sizeof(row1Str), "C%02d", (int)mt.outChannel);
    }
    midiAct = !isMuted && !(anySolo && !isSolo) &&
              (mt.lastStatus != 0) && ((uint32_t)(millis() - mt.lastActivityMs) < 140);
  }

  const bool silencedBySolo = anySolo && !isSolo;
  const bool effectivelyMuted = isMuted || silencedBySolo;
  const uint16_t outerBorder = trkFocused ? trkAccent : MID;
  const int topH = SY(26);
  const int botH = h - topH;

  // 1. Track Number Header ("A1".."A8" / "M1".."M8") + Left Audio Level Bars / MIDI Event Square
  uint64_t hk = hashStr(hashInit(), numStr);
  hk = hashMix(hk, (uint64_t)midiMode |
                   ((uint64_t)trkFocused << 1) |
                   ((uint64_t)effectivelyMuted << 2) |
                   ((uint64_t)midiAct << 3) |
                   ((uint64_t)clipL << 4) |
                   ((uint64_t)clipR << 5) |
                   ((uint64_t)(uint8_t)meterHL << 8) |
                   ((uint64_t)(uint8_t)meterHR << 16) |
                   ((uint64_t)outerBorder << 32));
  if (full || hk != prevMasterTrackHdrKey[t]) {
    prevMasterTrackHdrKey[t] = hk;
    Surface sTop(w, topH);
    sTop.fillRect(0, 0, w, topH, PANEL);
    sTop.fillRect(0, 0, w, PX, outerBorder);
    sTop.fillRect(0, 0, PX, topH, outerBorder);
    sTop.fillRect(w - PX, 0, PX, topH, outerBorder);
    sTop.fillRect(PX, hdrH, w - 2 * PX, 1, MID);

    uint16_t hdrFg = trkFocused ? trkAccent : (effectivelyMuted ? DIM : LIGHT);
    int numW = textWidth(numStr);
    int numX = max(PX, (w - numW) / 2);
    sTop.drawText(numX, TY(hdrH), numStr, hdrFg);

    const int leftSlotX = PX + 1;
    const int leftSlotW = max(1, numX - leftSlotX - 1);
    if (!midiMode) {
      const int barW = SX(2); // 2px on Model:01, 3px on Model:02
      if (isMono) {
        int bx = leftSlotX + max(0, (leftSlotW - barW) / 2);
        sTop.fillRect(bx, barTopY, barW, barMaxH, DARK);
        if (meterHL > 0) {
          uint16_t c = effectivelyMuted ? DIM : (clipL ? COLOR_REC : ACCENT);
          sTop.fillRect(bx, barTopY + (barMaxH - meterHL), barW, meterHL, c);
        }
      } else {
        const int gap = max(1, SX(1));
        const int pairW = 2 * barW + gap;
        int bxL = leftSlotX + max(0, (leftSlotW - pairW) / 2);
        int bxR = bxL + barW + gap;
        sTop.fillRect(bxL, barTopY, barW, barMaxH, DARK);
        sTop.fillRect(bxR, barTopY, barW, barMaxH, DARK);
        if (meterHL > 0) {
          uint16_t cL = effectivelyMuted ? DIM : (clipL ? COLOR_REC : ACCENT);
          sTop.fillRect(bxL, barTopY + (barMaxH - meterHL), barW, meterHL, cL);
        }
        if (meterHR > 0) {
          uint16_t cR = effectivelyMuted ? DIM : (clipR ? COLOR_REC : ACCENT);
          sTop.fillRect(bxR, barTopY + (barMaxH - meterHR), barW, meterHR, cR);
        }
      }
    } else if (midiAct) {
      const int sqSz = SX(4); // 4px on Model:01, 6px on Model:02
      int sqX = leftSlotX + max(0, (leftSlotW - sqSz) / 2);
      int sqY = TY(hdrH) + (FONT_H - sqSz) / 2;
      sTop.fillRect(sqX, sqY, sqSz, sqSz, COLOR_MIDI);
    }
    sTop.blit(x, y);
  }

  // 2. Lower Controls Body (5 Hz tier in steady-state): Row 0 (V### / IN), Row 1 (BAL/PAN / OUT), Row 2 (M / S)
  if (full || prevMasterTrackBodyKey[t] == 0 || uiTierDue(6)) {
    uint64_t bk = hashStr(hashInit(), row0Str);
    bk = hashStr(bk, row1Str);
    bk = hashMix(bk, (uint64_t)midiMode |
                     ((uint64_t)trkFocused << 1) |
                     ((uint64_t)selParam1 << 2) |
                     ((uint64_t)selParam2 << 3) |
                     ((uint64_t)selM << 4) |
                     ((uint64_t)selS << 5) |
                     ((uint64_t)isMuted << 6) |
                     ((uint64_t)isSolo << 7) |
                     ((uint64_t)effectivelyMuted << 8) |
                     ((uint64_t)row0Active << 9) |
                     ((uint64_t)row1Highlighted << 10) |
                     ((uint64_t)row1Active << 11) |
                     ((uint64_t)outerBorder << 16));
    if (full || bk != prevMasterTrackBodyKey[t]) {
      prevMasterTrackBodyKey[t] = bk;
      Surface sBot(w, botH);
      sBot.fillRect(0, 0, w, botH, PANEL);
      sBot.fillRect(0, botH - PX, w, PX, outerBorder);
      sBot.fillRect(0, 0, PX, botH, outerBorder);
      sBot.fillRect(w - PX, 0, PX, botH, outerBorder);

      // Row 0: Audio V### / MIDI IN
      const int r0Y = 0;
      const int r0H = SY(30);
      if (selParam1) {
        sBot.fillRect(0, r0Y, w, r0H, trkAccent);
        sBot.fillRect(PX, r0Y + PX, w - 2 * PX, r0H - 2 * PX, DARK);
      } else {
        sBot.fillRect(PX, r0Y + r0H - 1, w - 2 * PX, 1, MID);
      }
      uint16_t r0Fg = selParam1 ? trkAccent : ((row0Active && !effectivelyMuted) ? LIGHT : DIM);
      int r0W = textWidth(row0Str);
      sBot.drawText(max(PX, (w - r0W) / 2), r0Y + TY(r0H), row0Str, r0Fg);

      // Row 1: Audio BAL/PAN / MIDI OUT
      const int r1Y = SY(56) - topH;
      const int r1H = SY(30);
      if (selParam2) {
        sBot.fillRect(0, r1Y, w, r1H, trkAccent);
        sBot.fillRect(PX, r1Y + PX, w - 2 * PX, r1H - 2 * PX, DARK);
      } else {
        sBot.fillRect(PX, r1Y + r1H - 1, w - 2 * PX, 1, MID);
      }
      uint16_t r1Fg = selParam2                        ? trkAccent
                    : (!row1Active || effectivelyMuted) ? DIM
                    : row1Highlighted                  ? trkAccent
                                                       : LIGHT;
      int r1W = textWidth(row1Str);
      sBot.drawText(max(PX, (w - r1W) / 2), r1Y + TY(r1H), row1Str, r1Fg);

      // Row 2: Mute (M) and Solo (S) buttons
      const int msY = SY(89) - topH;
      const int msH = SY(29);
      const int msW = SX(16);
      const int mX  = SX(2);
      const int sX  = w - SX(2) - msW;

      drawMsToggleBtn(sBot, mX, msY, msW, msH, "M", isMuted, selM, COLOR_REC, trkAccent);
      drawMsToggleBtn(sBot, sX, msY, msW, msH, "S", isSolo,  selS, COLOR_WARN, trkAccent);

      sBot.blit(x, y + topH);
    }
  }
}

FLASHMEM static void masterDrawPage(bool full) {
  const SongSettings &ps = songSettings();
  const bool midiMode = isMasterMidiMode();
  const bool anySolo = anyTrackSolo();

  // Top Section: 8 Track Blocks left-to-right
  const int trkW = SX(38);
  const int trkH = SY(122);
  const int trkPitch = SX(40);
  const int trkX0 = (SCREEN_W - (7 * trkPitch + trkW)) / 2;
  const int trkY = SY(26);

  for (int t = 0; t < TRACK_ROWS; t++) {
    drawMasterTrackBlock(t, trkX0 + t * trkPitch, trkY, trkW, trkH, midiMode, anySolo, full);
    if (t == 3) {
      keepStreaming();
    }
  }
  keepStreaming();

  // Bottom Section (5 Hz tier in steady-state): 7 Boxes ([ CHOR ] [ DLAY ] [ D.FB ] [ RVRB ] [ TAPE ] [ COMP ] [ VOL ])
  if (full || prevMasterVolKey == 0 || uiTierDue(6)) {
    const int fxW = SX(43);
    const int fxH = SY(37);
    const int fxPitch = SX(45);
    const int fxX0 = (SCREEN_W - ((MASTER_FX_ROW_COLS - 1) * fxPitch + fxW)) / 2;
    const int fxY = SY(153);

    for (int c = 0; c < MASTER_FX_COUNT; c++) {
      uint8_t pct = getMasterFxPct(ps, c);
      char valStr[10];
      snprintf(valStr, sizeof(valStr), "%u%%", (unsigned)pct);
      bool sel = (masterRow == MASTER_ROW_FX && masterCell == c);
      drawTwoLineParamBox(fxX0 + c * fxPitch, fxY, fxW, fxH,
                          masterFxLabel(c), valStr, sel, pct > 0, ACCENT,
                          SY(6), SY(20), prevMasterFxKey[c], full);
    }

    {
      char volStr[16];
      if (ps.masterVolume == 0) {
        snprintf(volStr, sizeof(volStr), "OFF");
      } else {
        snprintf(volStr, sizeof(volStr), "%02u", (unsigned)ps.masterVolume);
      }
      bool sel = (masterRow == MASTER_ROW_FX && masterCell == MASTER_COL_VOL);
      drawTwoLineParamBox(fxX0 + MASTER_COL_VOL * fxPitch, fxY, fxW, fxH,
                          "VOL", volStr, sel, ps.masterVolume > 0, ACCENT,
                          SY(6), SY(20), prevMasterVolKey, full);
    }

    keepStreaming();
  }

  // Shared Bottom Info & Transport Bar
  drawTransportBar(masterRow == MASTER_ROW_TRANSPORT, masterCell, full);
}

FLASHMEM static void masterOnShiftNavigate(uint8_t dir) {
  if (dir & BTN_UP) {
    setPage(lastRow2Page());
  }
}

FLASHMEM static void masterOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  if (dy != 0) {
    int prevRow = masterRow;
    int nextRow = constrain(masterRow + dy, HEADER_ROW, MASTER_ROW_COUNT - 1);
    if (nextRow != prevRow) {
      // Save current track index when leaving a track row
      if (prevRow == MASTER_ROW_TRK_PARAM1 || prevRow == MASTER_ROW_TRK_PARAM2) {
        lastMasterTrack = constrain(masterCell, 0, TRACK_ROWS - 1);
      } else if (prevRow == MASTER_ROW_TRK_MS) {
        lastMasterTrack = constrain(masterCell / 2, 0, TRACK_ROWS - 1);
        lastMasterMsSide = masterCell & 1;
      }

      masterRow = nextRow;
      if (masterRow == HEADER_ROW) {
        headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
      } else if (masterRow == MASTER_ROW_TRK_PARAM1 || masterRow == MASTER_ROW_TRK_PARAM2) {
        masterCell = constrain(lastMasterTrack, 0, TRACK_ROWS - 1);
      } else if (masterRow == MASTER_ROW_TRK_MS) {
        if (prevRow == MASTER_ROW_FX) {
          int fxCol = constrain(masterCell, 0, MASTER_FX_ROW_COLS - 1);
          int mappedTrk = (fxCol == MASTER_COL_VOL) ? (TRACK_ROWS - 1) : fxCol;
          if (constrain((lastMasterTrack * MASTER_FX_ROW_COLS) / TRACK_ROWS, 0, MASTER_FX_ROW_COLS - 1) != fxCol) {
            lastMasterTrack = mappedTrk;
          }
        }
        masterCell = constrain(lastMasterTrack * 2 + lastMasterMsSide, 0, TRACK_ROWS * 2 - 1);
      } else if (masterRow == MASTER_ROW_FX) {
        if (prevRow == MASTER_ROW_TRK_MS) {
          masterCell = constrain((lastMasterTrack * MASTER_FX_ROW_COLS) / TRACK_ROWS, 0, MASTER_FX_ROW_COLS - 1);
        } else {
          masterCell = constrain(masterCell, 0, MASTER_FX_ROW_COLS - 1);
        }
      } else if (masterRow == MASTER_ROW_INFO) {
        masterCell = constrain(masterCell, 0, activeInfoCellCount() - 1);
      } else if (masterRow == MASTER_ROW_TRANSPORT) {
        masterCell = constrain(masterCell, 0, TR_CELL_COUNT - 1);
      }
    }
  }
  if (dx != 0) {
    if (masterRow == HEADER_ROW) {
      headerCell = constrain(headerCell + dx, 0, HEADER_CELL_COUNT - 1);
    } else if (masterRow == MASTER_ROW_TRK_PARAM1 || masterRow == MASTER_ROW_TRK_PARAM2) {
      masterCell = constrain(masterCell + dx, 0, TRACK_ROWS - 1);
      lastMasterTrack = masterCell;
    } else if (masterRow == MASTER_ROW_TRK_MS) {
      masterCell = constrain(masterCell + dx, 0, TRACK_ROWS * 2 - 1);
      lastMasterTrack = masterCell / 2;
      lastMasterMsSide = masterCell & 1;
    } else if (masterRow == MASTER_ROW_FX) {
      masterCell = constrain(masterCell + dx, 0, MASTER_FX_ROW_COLS - 1);
    } else if (masterRow == MASTER_ROW_INFO) {
      masterCell = constrain(masterCell + dx, 0, activeInfoCellCount() - 1);
    } else if (masterRow == MASTER_ROW_TRANSPORT) {
      masterCell = constrain(masterCell + dx, 0, TR_CELL_COUNT - 1);
    }
  }
  uiForceRedraw();
}

FLASHMEM static void setAudioTrackMuted(int t, bool mute) {
  Track &tr = tracks[t];
  if (recording.mode != Recording::Idle && recording.track == t && recording.autoMute) {
    tr.state = TrackState::Muted;
    return;
  }
  if (mute) {
    tr.state = TrackState::Muted;
  } else {
    tr.state = (tr.recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
  }
}

FLASHMEM static void toggleAudioTrackMute(int t) {
  setAudioTrackMuted(t, tracks[t].state != TrackState::Muted);
  saveSessionWhenIdle();
}

FLASHMEM static void masterOnOptAdjust(int dx, int dy, int delta) {
  if (masterRow == HEADER_ROW) {
    handleHeaderCellOptAdjust(headerCell, dx, dy);
    return;
  }

  const bool midiMode = isMasterMidiMode();

  if (masterRow == MASTER_ROW_TRK_PARAM1) {
    int t = constrain(masterCell, 0, TRACK_ROWS - 1);
    if (!midiMode) {
      Track &tr = tracks[t];
      int step = (dx != 0) ? dx : (dy * 5);
      int v = constrain((int)tr.volume + step, 0, VOLUME_MAX);
      setTrackMix(t, v, tr.balance);
      saveSessionWhenIdle();
    } else {
      midiSeqStepInChannel(t, delta);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_TRK_PARAM2) {
    int t = constrain(masterCell, 0, TRACK_ROWS - 1);
    if (!midiMode) {
      Track &tr = tracks[t];
      int step = (dx != 0) ? dx : (dy * 5);
      int b = constrain((int)tr.balance + step, -PAN_MAX, PAN_MAX);
      setTrackMix(t, tr.volume, b);
      saveSessionWhenIdle();
    } else {
      midiSeqStepOutChannel(t, dy, delta);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_TRK_MS) {
    if (dx != 0) {
      inputClearRepeat();
      int t = constrain(masterCell / 2, 0, TRACK_ROWS - 1);
      bool isSoloBtn = (masterCell & 1) != 0;
      int i0 = (dx < 0) ? 0 : t;
      int i1 = (dx < 0) ? t : (TRACK_ROWS - 1);
      if (!isSoloBtn) {
        if (!midiMode) {
          bool allMuted = true;
          for (int i = i0; i <= i1; i++) {
            if (tracks[i].state != TrackState::Muted) {
              allMuted = false;
              break;
            }
          }
          bool targetMute = !allMuted;
          for (int i = i0; i <= i1; i++) {
            setAudioTrackMuted(i, targetMute);
          }
        } else {
          bool allMuted = true;
          for (int i = i0; i <= i1; i++) {
            if (!midiTracks[i].muted) {
              allMuted = false;
              break;
            }
          }
          bool targetMute = !allMuted;
          for (int i = i0; i <= i1; i++) {
            midiTracks[i].muted = targetMute;
          }
          if (targetMute) {
            midiSeqSilenceMutedTracks();
          }
        }
      } else {
        if (!midiMode) {
          bool allSolo = true;
          for (int i = i0; i <= i1; i++) {
            if (!tracks[i].solo) {
              allSolo = false;
              break;
            }
          }
          bool targetSolo = !allSolo;
          for (int i = i0; i <= i1; i++) {
            tracks[i].solo = targetSolo;
          }
        } else {
          bool allSolo = true;
          for (int i = i0; i <= i1; i++) {
            if (!midiTracks[i].solo) {
              allSolo = false;
              break;
            }
          }
          bool targetSolo = !allSolo;
          for (int i = i0; i <= i1; i++) {
            midiTracks[i].solo = targetSolo;
          }
        }
        midiSeqSilenceMutedTracks();
      }
      saveSessionWhenIdle();
      uiForceRedraw();
    }
    return;
  }

  if (masterRow == MASTER_ROW_FX) {
    SongSettings &ps = songSettings();
    int col = constrain(masterCell, 0, MASTER_FX_ROW_COLS - 1);
    int step = (dx != 0) ? dx : (dy * 5);
    if (col == MASTER_COL_VOL) {
      uint8_t nextVol = (uint8_t)constrain((int)ps.masterVolume + step, 0, MASTER_VOLUME_MAX);
      if (ps.masterVolume != nextVol) {
        ps.masterVolume = nextVol;
        setMasterVolume(ps.masterVolume);
        saveSessionWhenIdle();
      }
    } else {
      int cur = (int)getMasterFxPct(ps, col);
      setMasterFxPct(ps, col, cur + step);
    }
    uiForceRedraw();
  } else if (masterRow == MASTER_ROW_INFO) {
    handleInfoCellOptAdjust(masterCell, dx, dy, delta);
  } else if (masterRow == MASTER_ROW_TRANSPORT) {
    handleTransportCellOptAdjust(masterCell, dx, dy, delta);
  }
}

FLASHMEM static void handleAudioTrackSoloPress(int t, bool shiftHeld) {
  if (shiftHeld) {
    tracks[t].solo = !tracks[t].solo;
    midiSeqSilenceMutedTracks();
    saveSessionWhenIdle();
    return;
  }
  bool onlyThisSolo = tracks[t].solo;
  if (onlyThisSolo) {
    for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
      if (i != t && tracks[i].solo) {
        onlyThisSolo = false;
        break;
      }
    }
    if (onlyThisSolo) {
      for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
        if (midiTracks[i].solo) {
          onlyThisSolo = false;
          break;
        }
      }
    }
  }
  if (onlyThisSolo) {
    tracks[t].solo = false;
  } else {
    for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
      tracks[i].solo = (i == t);
    }
    for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
      midiTracks[i].solo = false;
    }
  }
  midiSeqSilenceMutedTracks();
  saveSessionWhenIdle();
}

FLASHMEM static void handleMidiTrackSoloPress(int t, bool shiftHeld) {
  if (shiftHeld) {
    midiTracks[t].solo = !midiTracks[t].solo;
    midiSeqSilenceMutedTracks();
    saveSessionWhenIdle();
    return;
  }
  bool onlyThisSolo = midiTracks[t].solo;
  if (onlyThisSolo) {
    for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
      if (i != t && midiTracks[i].solo) {
        onlyThisSolo = false;
        break;
      }
    }
    if (onlyThisSolo) {
      for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
        if (tracks[i].solo) {
          onlyThisSolo = false;
          break;
        }
      }
    }
  }
  if (onlyThisSolo) {
    midiTracks[t].solo = false;
  } else {
    for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
      tracks[i].solo = false;
    }
    for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
      midiTracks[i].solo = (i == t);
    }
  }
  midiSeqSilenceMutedTracks();
  saveSessionWhenIdle();
}

FLASHMEM static void masterOnEditPress(bool shiftHeld) {
  if (masterRow == HEADER_ROW) {
    handleHeaderCellEditPress(shiftHeld);
    return;
  }
  if (masterRow == MASTER_ROW_TRANSPORT) {
    handleTransportCellAction(masterCell, shiftHeld);
    return;
  }
  if (masterRow == MASTER_ROW_INFO) {
    handleInfoCellEditPress(masterCell, shiftHeld);
    return;
  }

  const bool midiMode = isMasterMidiMode();

  if (masterRow == MASTER_ROW_TRK_PARAM1) {
    int t = constrain(masterCell, 0, TRACK_ROWS - 1);
    if (!midiMode) {
      toggleAudioTrackMute(t);
    } else {
      midiSeqCycleInChannel(t);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_TRK_PARAM2) {
    int t = constrain(masterCell, 0, TRACK_ROWS - 1);
    if (!midiMode) {
      setTrackMix(t, tracks[t].volume, 0);
      saveSessionWhenIdle();
    } else {
      MidiTrack &mt = midiTracks[t];
      if (shiftHeld && mt.outChannel >= 17) {
        instListCursor = constrain((int)mt.outChannel - 17, 0, NUM_INSTRUMENTS - 1);
        if (fmSynthIsSlotEmpty(instListCursor)) {
          instTypeCursor = 0;
          setPage(Page::InstrumentType);
        } else {
          setPage(Page::InstrumentEdit);
        }
        return;
      }
      midiSeqCycleOutChannel(t);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_TRK_MS) {
    int t = constrain(masterCell / 2, 0, TRACK_ROWS - 1);
    bool isSoloBtn = (masterCell & 1) != 0;
    if (!isSoloBtn) {
      if (!midiMode) {
        toggleAudioTrackMute(t);
      } else {
        MidiTrack &mt = midiTracks[t];
        mt.muted = !mt.muted;
        if (mt.muted) midiSeqSilenceMutedTracks();
        saveSessionWhenIdle();
      }
    } else {
      if (!midiMode) {
        handleAudioTrackSoloPress(t, shiftHeld);
      } else {
        handleMidiTrackSoloPress(t, shiftHeld);
      }
    }
    uiForceRedraw();
    return;
  }

  // MASTER_ROW_FX (cols 0..5 = FX, col 6 = VOL)
  SongSettings &ps = songSettings();
  int col = constrain(masterCell, 0, MASTER_FX_ROW_COLS - 1);
  if (col == MASTER_COL_VOL) {
    ps.masterVolume = (ps.masterVolume > 0) ? 0 : MASTER_VOLUME_DEFAULT;
    setMasterVolume(ps.masterVolume);
    saveSessionWhenIdle();
    uiForceRedraw();
    return;
  }
  if (shiftHeld) {
    ps.masterChorus = 0;
    ps.masterDelay = 0;
    ps.masterDelayFeedback = 0;
    ps.masterReverb = 0;
    ps.masterTape = 0;
    ps.masterComp = 0;
    applyMasterFxSettings();
    resetMasterFxState();
    saveSessionWhenIdle();
    uiShowToast("MASTER FX BYPASSED");
    uiForceRedraw();
    return;
  }
  uint8_t cur = getMasterFxPct(ps, col);
  uint8_t next = (cur < 25)  ? 25
               : (cur < 50)  ? 50
               : (cur < 75)  ? 75
               : (cur < 100) ? 100
                             : 0;
  setMasterFxPct(ps, col, next);
  uiForceRedraw();
}

FLASHMEM static void masterOnResetParam() {
  if (masterRow == HEADER_ROW) {
    handleHeaderCellReset(headerCell);
    return;
  }

  const bool midiMode = isMasterMidiMode();

  if (masterRow == MASTER_ROW_TRK_PARAM1) {
    int t = constrain(masterCell, 0, TRACK_ROWS - 1);
    if (!midiMode) {
      setTrackMix(t, VOLUME_UNITY, tracks[t].balance);
      saveSessionWhenIdle();
    } else {
      midiTracks[t].inChannel = 0;
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_TRK_PARAM2) {
    int t = constrain(masterCell, 0, TRACK_ROWS - 1);
    if (!midiMode) {
      setTrackMix(t, tracks[t].volume, 0);
      saveSessionWhenIdle();
    } else {
      midiSeqSetOutChannel(t, (uint8_t)(17 + t));
      saveSessionWhenIdle();
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_TRK_MS) {
    int t = constrain(masterCell / 2, 0, TRACK_ROWS - 1);
    bool isSoloBtn = (masterCell & 1) != 0;
    if (!isSoloBtn) {
      if (!midiMode) {
        if (tracks[t].state == TrackState::Muted &&
            !(recording.mode != Recording::Idle && recording.track == t && recording.autoMute)) {
          tracks[t].state = (tracks[t].recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
          saveSessionWhenIdle();
        }
      } else if (midiTracks[t].muted) {
        midiTracks[t].muted = false;
        saveSessionWhenIdle();
      }
    } else {
      if (!midiMode) {
        if (tracks[t].solo) {
          tracks[t].solo = false;
          midiSeqSilenceMutedTracks();
          saveSessionWhenIdle();
        }
      } else if (midiTracks[t].solo) {
        midiTracks[t].solo = false;
        midiSeqSilenceMutedTracks();
        saveSessionWhenIdle();
      }
    }
    uiForceRedraw();
    return;
  }

  if (masterRow == MASTER_ROW_FX) {
    int col = constrain(masterCell, 0, MASTER_FX_ROW_COLS - 1);
    if (col == MASTER_COL_VOL) {
      SongSettings &ps = songSettings();
      if (ps.masterVolume != MASTER_VOLUME_DEFAULT) {
        ps.masterVolume = MASTER_VOLUME_DEFAULT;
        setMasterVolume(ps.masterVolume);
        saveSessionWhenIdle();
      }
    } else {
      setMasterFxPct(songSettings(), col, 0);
    }
    uiForceRedraw();
  } else if (masterRow == MASTER_ROW_INFO) {
    handleInfoCellReset(masterCell);
  } else if (masterRow == MASTER_ROW_TRANSPORT) {
    handleTransportCellReset(masterCell);
  }
}

PROGMEM static const PageOps MASTER_PAGE_OPS = {
  Page::Master,
  masterHeaderTitle,
  nullptr,
  masterInvalidateCache,
  masterDrawPage,
  masterOnShiftNavigate,
  masterOnCursorMove,
  masterOnOptAdjust,
  masterOnEditPress,
  masterOnResetParam
};

FLASHMEM const PageOps &pageMasterOps() {
  return MASTER_PAGE_OPS;
}

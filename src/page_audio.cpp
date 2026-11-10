// page_audio.cpp — Audio Recording Page (8 Audio Tracks: 4 Stereo + 4 Mono, 4-Row Viewport + Transport) UI & Input
#include "page_audio.h"
#include "page_midi.h"
#include "track_edit.h"
#include <Arduino.h>
#include <stdio.h>
#include <math.h>

using namespace m8;
using namespace looper;

static uint64_t prevAudioTrackKey[VISIBLE_TRACK_ROWS][AUD_CELL_COUNT] = {};
static uint64_t prevAudioMeterKey[VISIBLE_TRACK_ROWS] = {};
static float trSmooth[NUM_AUDIO_TRACKS] = {};
static int lastAudioTrackRow = 0;
static int lastAudioTrackCell = AUD_CELL_WAVE;

FLASHMEM static const char *audioHeaderTitle() {
  const char *teTitle = trackEditHeaderTitle(Page::Audio);
  return teTitle ? teTitle : "AUDIO";
}

FLASHMEM static void audioInvalidateCache() {
  memset(prevAudioTrackKey, 0, sizeof(prevAudioTrackKey));
  memset(prevAudioMeterKey, 0, sizeof(prevAudioMeterKey));
}

FLASHMEM static void drawAudioTrackRow(int visSlot, int t, const TimelineFrameCtx &ctx, bool full) {
  const Track &tr = tracks[t];
  const int y = SY(26) + visSlot * SY(41);
  const int h = SY(39);
  const bool rowSel = (audioRow == t);
  const int selC = rowSel ? audioCell : -1;
  const bool staticDue = full || uiTierDue(6);

  // Cell 0: Track Badge & Arm Mode ("01".."08" + record circle & mono L/R/LR tag when armed)
  if (staticDue || prevAudioTrackKey[visSlot][AUD_CELL_ARM] == 0) {
    drawTrackArmBadgeCell(SX(2), y, SX(28), h, t, tr.armMode,
                          tr.state == TrackState::Muted, selC == AUD_CELL_ARM,
                          ACCENT, prevAudioTrackKey[visSlot][AUD_CELL_ARM], full,
                          audioTrackArmTag(t));
  }

  // Cell 1: Trigger Mode ("TRG", highlighted only when ON)
  if (staticDue || prevAudioTrackKey[visSlot][AUD_CELL_TRIG] == 0) {
    drawTrackTrigBadgeCell(SX(32), y, SX(32), h, tr.trigMode, selC == AUD_CELL_TRIG,
                           ACCENT, prevAudioTrackKey[visSlot][AUD_CELL_TRIG], full);
  }

  // Cell 2: Scrolling Waveform Timeline (15 Hz 2-row round-robin while running) + 30 Hz Level Meter
  {
    const int cx = SX(66), cw = SX(184);
    const int innerW = cw - 4;
    const int innerX = 2;
    const int innerY = 2;
    const int innerH = h - SY(8); // leave bottom strip for live track level meter
    const int meterH = SY(3);
    const int meterY = h - SY(5);

    float pL, pR;
    takeTrackPeaks(t, pL, pR);
    float pk = max(pL, pR);
    trSmooth[t] = max(pk, trSmooth[t] * 0.78f);
    if (trSmooth[t] < 0.002f) trSmooth[t] = 0.0f;
    int mw = constrain((int)(sqrtf(trSmooth[t]) * (float)innerW), 0, innerW);
    const uint16_t meterFg = (tr.state == TrackState::Muted) ? DIM : ACCENT;
    const uint64_t meterKey = ((uint64_t)(uint32_t)t << 32) | ((uint64_t)(uint16_t)mw << 16) | (uint64_t)meterFg;

    const bool waveDue = full || (prevAudioTrackKey[visSlot][AUD_CELL_WAVE] == 0) ||
                         uiIsImmediateFrame() || (transport.mode != TransportMode::Running) ||
                         ((visSlot >> 1) == (int)uiTimelineRrSlot());

    bool drewFullWave = false;
    if (waveDue) {
      const int playCol = innerW / 2;
      const bool sel = (selC == AUD_CELL_WAVE);
      uint32_t hlStart = 0, hlEnd = 0;
      bool hlEditEnd = false;
      const bool hlActive = trackEditGetHighlight(Page::Audio, t, hlStart, hlEnd, hlEditEnd);
      uint16_t border = sel ? ACCENT : (hlActive ? COLOR_WARN : MID);
      const int64_t curFrame = ctx.curFrame;
      const uint8_t zoom = ctx.zoom;
      const uint32_t winFrames = ctx.winFrames;
      const bool punchOn = ctx.punchOn;
      const int punchX0 = ctx.punchX0;
      const int punchX1 = ctx.punchX1;

      int hlX0 = -9999, hlX1 = -9999;
      if (hlActive) {
        hlX0 = timelineFrameToCol((int64_t)hlStart, curFrame, playCol, innerW, winFrames);
        hlX1 = timelineFrameToCol((int64_t)hlEnd, curFrame, playCol, innerW, winFrames);
      }

      bool recHere = recording.activeInWindow && recording.track == t;
      uint16_t waveColor = (tr.state == TrackState::Muted) ? DIM
                         : (tr.armMode == ArmMode::Record && recHere) ? COLOR_REC
                         : (tr.armMode == ArmMode::Overdub && recHere) ? COLOR_OVERDUB
                         : ACCENT;
      uint16_t phColor = recHere ? COLOR_REC : INK;

      uint32_t recTenths = (uint32_t)((uint64_t)tr.recordedFrames * 10 / SAMPLE_RATE_HZ);

      uint64_t k = hashInit();
      k = hashMix(k, (uint32_t)t);
      k = hashMix(k, border);
      k = hashMix(k, ctx.scrollStep);
      k = hashMix(k, zoom);
      k = hashMix(k, ctx.bpmKey);
      k = hashMix(k, tr.recordedFrames);
      k = hashMix(k, punchOn ? ((uint64_t)(uint32_t)(punchX0 + 10000) | ((uint64_t)(uint32_t)(punchX1 + 10000) << 32)) : 0ULL);
      k = hashMix(k, (uint32_t)waveColor | ((uint32_t)phColor << 16));
      k = hashMix(k, tr.overviewVersion);
      k = hashMix(k, (uint8_t)tr.state);
      k = hashMix(k, recTenths);
      k = hashMix(k, hlActive ? ((uint64_t)(uint32_t)(hlX0 + 10000) |
                                 ((uint64_t)(uint32_t)(hlX1 + 10000) << 32) |
                                 ((uint64_t)hlEditEnd << 62) | (1ULL << 63))
                              : 0ULL);

      if (full || k != prevAudioTrackKey[visSlot][AUD_CELL_WAVE]) {
        prevAudioTrackKey[visSlot][AUD_CELL_WAVE] = k;
        prevAudioMeterKey[visSlot] = meterKey;
        drewFullWave = true;
        Surface s(cw, h);
        s.drawBox(BG, border);

        const int midY = innerY + innerH / 2;
        const uint16_t hlFill = blend565(ACCENT, BG, 56);

        if (punchOn) {
          int px0 = max(0, punchX0);
          int px1 = min(innerW - 1, punchX1);
          if (px0 <= px1) {
            s.fillRect(innerX + px0, innerY, px1 - px0 + 1, innerH, GREY_12);
          }
        }
        if (hlActive) {
          int hx0 = max(0, hlX0);
          int hx1 = min(innerW - 1, hlX1);
          if (hx0 <= hx1) {
            int hw = hx1 - hx0 + 1;
            s.fillRect(innerX + hx0, innerY, hw, innerH, hlFill);
            s.fillRect(innerX + hx0, innerY, hw, 1, ACCENT);
            s.fillRect(innerX + hx0, innerY + innerH - 1, hw, 1, ACCENT);
          }
        }

        drawTimelineTicksInWindow(s, innerX, innerY, innerW, innerH, curFrame, playCol, winFrames, bpm, zoom);

        if (tr.recordedFrames == 0) {
          int zeroCol = max(0, timelineFrameToCol(0, curFrame, playCol, innerW, winFrames));
          if (zeroCol < innerW) {
            s.fillRect(innerX + zeroCol, midY, innerW - zeroCol, 1, DARK);
          }
        } else {
          for (int col = 0; col < innerW; col++) {
            int px = innerX + col;
            int64_t f0 = curFrame + ((int64_t)(col - playCol) * (int64_t)winFrames) / innerW;
            int64_t f1 = curFrame + ((int64_t)(col + 1 - playCol) * (int64_t)winFrames) / innerW;
            if (f1 <= f0) f1 = f0 + 1;

            uint8_t ov = 0;
            if (f1 > 0 && f0 < (int64_t)tr.recordedFrames) {
              int64_t cf0 = max((int64_t)0, f0);
              int64_t cf1 = min((int64_t)tr.recordedFrames - 1, max(cf0, f1 - 1));
              int b0 = constrain((int)(cf0 / FRAMES_PER_PEAK_BIN), 0, WAVE_PEAK_BINS - 1);
              int b1 = constrain((int)(cf1 / FRAMES_PER_PEAK_BIN), b0, WAVE_PEAK_BINS - 1);
              for (int b = b0; b <= b1; b++) {
                uint8_t pkVal = tr.getWavePeak(b);
                if (pkVal > ov) ov = pkVal;
              }
            }

            if (ov > 0) {
              int barH = max(2, (int)ov * (innerH - 2) / 255);
              int top = midY - barH / 2;
              s.fillRect(px, top, 1, barH, waveColor);
            } else if (f1 > 0) {
              s.drawPixel(px, midY, DARK);
            }
          }
        }

        if (punchOn) {
          if (punchX0 >= 0 && punchX0 < innerW) s.drawDottedVLine(innerX + punchX0, innerY, innerH, COLOR_PUNCH);
          if (punchX1 >= 0 && punchX1 < innerW) s.drawDottedVLine(innerX + punchX1, innerY, innerH, COLOR_PUNCH);
        }
        if (hlActive) {
          if (hlX0 >= 0 && hlX0 < innerW) {
            bool isActiveEdge = (trackEditPhase() == TrackEditPhase::SelectRegion) ? !hlEditEnd : true;
            s.fillRect(innerX + hlX0, innerY, 1, innerH, isActiveEdge ? COLOR_WARN : ACCENT);
          }
          if (hlX1 >= 0 && hlX1 < innerW) {
            bool isActiveEdge = (trackEditPhase() == TrackEditPhase::SelectRegion) ? hlEditEnd : true;
            s.fillRect(innerX + hlX1, innerY, 1, innerH, isActiveEdge ? COLOR_WARN : ACCENT);
          }
        }
        s.fillRect(innerX + playCol, innerY, 1, innerH, phColor);

        if (tr.state == TrackState::Empty) {
          s.drawText(SX(6), SY(8), "EMPTY", DIM, BG);
        } else if (tr.state == TrackState::Muted) {
          s.drawText(SX(6), SY(8), "MUTED", DIM, BG);
        }

        // Bottom stereo level meter bar
        s.fillRect(innerX, meterY, innerW, meterH, DARK);
        if (mw > 0) {
          s.fillRect(innerX, meterY, mw, meterH, meterFg);
        }
        s.blit(cx, y);
      }
    }

    if (!drewFullWave && meterKey != prevAudioMeterKey[visSlot]) {
      prevAudioMeterKey[visSlot] = meterKey;
      Surface ms(innerW, meterH);
      ms.fillRect(0, 0, innerW, meterH, DARK);
      if (mw > 0) {
        ms.fillRect(0, 0, mw, meterH, meterFg);
      }
      ms.blit(cx + innerX, y + meterY);
    }
  }

  // Cell 3: Volume (2-line "VOL" / "01".."100" or "OFF")
  if (staticDue || prevAudioTrackKey[visSlot][AUD_CELL_VOL] == 0) {
    char vStr[8];
    if (tr.volume <= 0) snprintf(vStr, sizeof(vStr), "OFF");
    else snprintf(vStr, sizeof(vStr), "%02d", (int)tr.volume);
    drawTwoLineParamBox(SX(252), y, SX(32), h, "VOL", vStr,
                        selC == AUD_CELL_VOL, tr.volume > 0, ACCENT,
                        SY(7), SY(21), prevAudioTrackKey[visSlot][AUD_CELL_VOL], full);
  }

  // Cell 4: Balance ("BAL" on stereo 01..04) / Pan ("PAN" on mono 05..08)
  if (staticDue || prevAudioTrackKey[visSlot][AUD_CELL_PAN] == 0) {
    const char *panLbl = isAudioTrackMono(t) ? "PAN" : "BAL";
    char pStr[8];
    if (tr.balance == 0) snprintf(pStr, sizeof(pStr), "C");
    else if (tr.balance <= -PAN_MAX) snprintf(pStr, sizeof(pStr), "L");
    else if (tr.balance >= PAN_MAX) snprintf(pStr, sizeof(pStr), "R");
    else if (tr.balance < 0) snprintf(pStr, sizeof(pStr), "L%02d", -(int)tr.balance);
    else snprintf(pStr, sizeof(pStr), "R%02d", (int)tr.balance);
    drawTwoLineParamBox(SX(286), y, SX(32), h, panLbl, pStr,
                        selC == AUD_CELL_PAN, true, ACCENT,
                        SY(7), SY(21), prevAudioTrackKey[visSlot][AUD_CELL_PAN], full);
  }
}

FLASHMEM static void audioDrawPage(bool full) {
  syncAudioScroll();
  const int cw = SX(184);
  const int innerW = cw - 4;
  TimelineFrameCtx ctx;
  computeTimelineFrameCtx(ctx, innerW, innerW / 2);
  for (int vis = 0; vis < VISIBLE_TRACK_ROWS; vis++) {
    drawAudioTrackRow(vis, audioScroll + vis, ctx, full);
    keepStreaming();
  }
  drawTransportBar(audioRow == TRANSPORT_ROW, audioCell, full);
}

FLASHMEM void audioRememberTrackCursor() {
  if (audioRow >= 0 && audioRow < TRACK_ROWS) {
    lastAudioTrackRow = audioRow;
    lastAudioTrackCell = (audioCell >= 0 && audioCell < AUD_CELL_COUNT) ? audioCell : -1;
  }
}

FLASHMEM void audioRestoreLastTrackCursor() {
  int row = (lastAudioTrackRow >= 0 && lastAudioTrackRow < TRACK_ROWS) ? lastAudioTrackRow : 0;
  int cell = (lastAudioTrackRow >= 0 && lastAudioTrackRow < TRACK_ROWS &&
              lastAudioTrackCell >= 0 && lastAudioTrackCell < AUD_CELL_COUNT)
                 ? lastAudioTrackCell
                 : AUD_CELL_WAVE;
  audioRow = row;
  audioCell = cell;
  lastAudioTrackRow = row;
  lastAudioTrackCell = cell;
  syncAudioScroll();
}

FLASHMEM void audioSyncCursorFromMidi() {
  audioRememberTrackCursor();
  if (midiRow == HEADER_ROW) {
    audioRow = HEADER_ROW;
    headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
  } else if (midiRow == INFO_ROW) {
    audioRow = INFO_ROW;
    audioCell = constrain(midiCell, 0, activeInfoCellCount() - 1);
  } else if (midiRow == TRANSPORT_ROW) {
    audioRow = TRANSPORT_ROW;
    audioCell = constrain(midiCell, 0, TR_CELL_COUNT - 1);
  } else {
    audioRestoreLastTrackCursor();
  }
}

FLASHMEM static void audioOnShiftNavigate(uint8_t dir) {
  if (trackEditIsActive()) {
    if (dir & BTN_DOWN) {
      trackEditCancel();
    }
    return;
  }
  if (dir & BTN_RIGHT) {
    audioRememberTrackCursor();
    midiSyncCursorFromAudio();
    setPage(Page::Midi);
  } else if (dir & BTN_UP) {
    if (audioRow >= INFO_ROW) {
      audioRestoreLastTrackCursor();
      uiForceRedraw();
    } else if (audioRow >= 0) {
      audioRememberTrackCursor();
      audioRow = HEADER_ROW;
      headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
      uiForceRedraw();
    } else {
      setPage(lastRow1Page());
    }
  } else if (dir & BTN_DOWN) {
    if (audioRow == HEADER_ROW) {
      audioRestoreLastTrackCursor();
      uiForceRedraw();
    } else if (audioRow < INFO_ROW) {
      audioRememberTrackCursor();
      audioRow = INFO_ROW;
      audioCell = constrain(audioCell, 0, activeInfoCellCount() - 1);
      uiForceRedraw();
    } else {
      setPage(Page::Master);
    }
  }
}

FLASHMEM static void audioOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  if (trackEditPhase() == TrackEditPhase::PickPasteTrack) {
    if (dy != 0) {
      audioRow = constrain(audioRow + dy, 0, TRACK_ROWS - 1);
      syncAudioScroll();
    }
    audioCell = AUD_CELL_WAVE;
    lastAudioTrackRow = audioRow;
    lastAudioTrackCell = audioCell;
    uiForceRedraw();
    return;
  }
  if (dy != 0) {
    if (audioRow == HEADER_ROW) {
      // Top Bar has 1 row; require SHIFT+DOWN to enter Timeline
    } else if (audioRow >= INFO_ROW) {
      audioRow = constrain(audioRow + dy, INFO_ROW, TRANSPORT_ROW);
    } else {
      audioRow = constrain(audioRow + dy, 0, TRACK_ROWS - 1);
      syncAudioScroll();
    }
    if (audioRow == HEADER_ROW) {
      headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
    } else if (audioRow == TRANSPORT_ROW) {
      audioCell = constrain(audioCell, 0, TR_CELL_COUNT - 1);
    } else if (audioRow == INFO_ROW) {
      audioCell = constrain(audioCell, 0, activeInfoCellCount() - 1);
    } else {
      audioCell = constrain(audioCell, 0, AUD_CELL_COUNT - 1);
    }
  }
  if (dx != 0) {
    if (audioRow == HEADER_ROW) {
      headerCell = constrain(headerCell + dx, 0, HEADER_CELL_COUNT - 1);
    } else {
      int maxCell = (audioRow == TRANSPORT_ROW) ? (TR_CELL_COUNT - 1)
                  : (audioRow == INFO_ROW)      ? (activeInfoCellCount() - 1)
                                                : (AUD_CELL_COUNT - 1);
      audioCell = constrain(audioCell + dx, 0, maxCell);
    }
  }
  if (audioRow >= 0 && audioRow < TRACK_ROWS) {
    lastAudioTrackRow = audioRow;
    lastAudioTrackCell = audioCell;
  }
  uiForceRedraw();
}

FLASHMEM static void audioOnOptAdjust(int dx, int dy, int delta) {
  if (audioRow == HEADER_ROW) {
    handleHeaderCellOptAdjust(headerCell, dx, dy);
    return;
  }
  if (audioRow < TRACK_ROWS) {
    int t = audioRow;
    Track &tr = tracks[t];
    if (audioCell == AUD_CELL_ARM) {
      stepAudioTrackArm(t, delta);
    } else if (audioCell == AUD_CELL_TRIG) {
      TrigMode prevTrig = tr.trigMode;
      int m = ((int)tr.trigMode + delta + 3) % 3;
      tr.trigMode = (TrigMode)m;
      if (transport.mode != TransportMode::Stopped && tr.armMode != ArmMode::Off) {
        if (tr.trigMode == TrigMode::Resample && prevTrig != TrigMode::Resample) {
          openResampleModal(t, false, false);
        } else {
          storageSyncArmedAudioTrack();
        }
      }
      saveSessionWhenIdle();
    } else if (audioCell == AUD_CELL_VOL) {
      int step = (dx != 0) ? dx : (dy * 5);
      int v = constrain((int)tr.volume + step, 0, VOLUME_MAX);
      setTrackMix(t, v, tr.balance);
      saveSessionWhenIdle();
    } else if (audioCell == AUD_CELL_PAN) {
      int step = (dx != 0) ? dx : (dy * 5);
      int b = constrain((int)tr.balance + step, -PAN_MAX, PAN_MAX);
      setTrackMix(t, tr.volume, b);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
  } else if (audioRow == INFO_ROW) {
    handleInfoCellOptAdjust(audioCell, dx, dy, delta);
  } else if (audioRow == TRANSPORT_ROW) {
    handleTransportCellOptAdjust(audioCell, dx, dy, delta);
  }
}

FLASHMEM static void audioOnEditPress(bool shiftHeld) {
  if (trackEditPhase() == TrackEditPhase::PickPasteTrack) {
    if (!shiftHeld && audioRow >= 0 && audioRow < TRACK_ROWS) {
      trackEditBeginPastePositioning(audioRow);
    }
    return;
  }
  if (audioRow == HEADER_ROW) {
    handleHeaderCellEditPress(shiftHeld);
    return;
  }
  if (audioRow == TRANSPORT_ROW) {
    handleTransportCellAction(audioCell, shiftHeld);
    return;
  }
  if (audioRow == INFO_ROW) {
    handleInfoCellEditPress(audioCell, shiftHeld);
    return;
  }
  int t = audioRow;
  Track &tr = tracks[t];
  if (audioCell == AUD_CELL_WAVE) {
    if (shiftHeld) {
      appSeekTape(0);
      uiForceRedraw();
    } else {
      if (tr.recordedFrames > 0) {
        if (recording.mode != Recording::Idle && recording.track == t && recording.resample) {
          tr.state = TrackState::Muted;
        } else {
          tr.state = (tr.state == TrackState::Muted) ? TrackState::Playing : TrackState::Muted;
          saveSessionWhenIdle();
        }
        uiForceRedraw();
      } else {
        armAudioTrack(t, (tr.armMode == ArmMode::Off) ? ArmMode::Record : ArmMode::Off);
        uiForceRedraw();
      }
    }
  } else if (audioCell == AUD_CELL_ARM) {
    stepAudioTrackArm(t, 1);
    uiForceRedraw();
  } else if (audioCell == AUD_CELL_TRIG) {
    TrigMode prevTrig = tr.trigMode;
    tr.trigMode = (tr.trigMode == TrigMode::Manual)    ? TrigMode::Threshold
                : (tr.trigMode == TrigMode::Threshold) ? TrigMode::Resample
                                                       : TrigMode::Manual;
    if (transport.mode != TransportMode::Stopped && tr.armMode != ArmMode::Off) {
      if (tr.trigMode == TrigMode::Resample && prevTrig != TrigMode::Resample) {
        openResampleModal(t, false, false);
      } else {
        storageSyncArmedAudioTrack();
      }
    }
    saveSessionWhenIdle();
    uiForceRedraw();
  } else if (audioCell == AUD_CELL_VOL) {
    if (tr.recordedFrames > 0 || tr.state == TrackState::Muted) {
      if (recording.mode != Recording::Idle && recording.track == t && recording.resample) {
        tr.state = TrackState::Muted;
      } else {
        tr.state = (tr.state == TrackState::Muted)
                     ? ((tr.recordedFrames > 0) ? TrackState::Playing : TrackState::Empty)
                     : TrackState::Muted;
        saveSessionWhenIdle();
      }
      uiForceRedraw();
    }
  } else if (audioCell == AUD_CELL_PAN) {
    setTrackMix(t, tr.volume, 0);
    saveSessionWhenIdle();
    uiForceRedraw();
  }
}

FLASHMEM static void audioOnResetParam() {
  if (audioRow == HEADER_ROW) {
    handleHeaderCellReset(headerCell);
    return;
  }
  if (audioRow < TRACK_ROWS) {
    int t = audioRow;
    if (audioCell == AUD_CELL_WAVE) {
      openClearTrackModal(Page::Audio, t);
    } else if (audioCell == AUD_CELL_ARM) {
      armAudioTrack(t, ArmMode::Off);
    } else if (audioCell == AUD_CELL_TRIG) {
      tracks[t].trigMode = TrigMode::Manual;
      tracks[t].threshDb = songSettings().defaultThreshDb;
      if (transport.mode != TransportMode::Stopped && tracks[t].armMode != ArmMode::Off) {
        storageSyncArmedAudioTrack();
      }
      saveSessionWhenIdle();
    } else if (audioCell == AUD_CELL_VOL) {
      setTrackMix(t, VOLUME_UNITY, tracks[t].balance);
      saveSessionWhenIdle();
    } else if (audioCell == AUD_CELL_PAN) {
      setTrackMix(t, tracks[t].volume, 0);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
  } else if (audioRow == INFO_ROW) {
    handleInfoCellReset(audioCell);
  } else if (audioRow == TRANSPORT_ROW) {
    handleTransportCellReset(audioCell);
  }
}

PROGMEM static const PageOps AUDIO_PAGE_OPS = {
  Page::Audio,
  audioHeaderTitle,
  nullptr,
  audioInvalidateCache,
  audioDrawPage,
  audioOnShiftNavigate,
  audioOnCursorMove,
  audioOnOptAdjust,
  audioOnEditPress,
  audioOnResetParam
};

FLASHMEM const PageOps &pageAudioOps() {
  return AUDIO_PAGE_OPS;
}

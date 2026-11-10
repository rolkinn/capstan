// page_midi.cpp — MIDI Recording Page (8 MIDI Tracks, 4-Row Viewport + Transport) UI & Input
#include "page_midi.h"
#include "page_audio.h"
#include "track_edit.h"
#include <Arduino.h>
#include <stdio.h>

using namespace m8;
using namespace looper;

static uint64_t prevMidiTrackKey[VISIBLE_TRACK_ROWS][MID_CELL_COUNT] = {};
static int lastMidiTrackRow = -1;
static int lastMidiTrackCell = -1;

// MIDI Note Sequence Editor (1.5-Octave Piano Roll + 1/32nd Timeline Grid Overlay) state
static constexpr int PIANO_VIS_KEYS = 18;        // 1.5 octaves (18 semitones) visible vertically
static constexpr int PIANO_KEYS_PER_STRIP = 1;   // 1 key row per offscreen Surface strip
static constexpr int PIANO_STRIP_COUNT = PIANO_VIS_KEYS / PIANO_KEYS_PER_STRIP;
static constexpr int PIANO_CURSOR_ROW = PIANO_VIS_KEYS / 2; // Fixed middle row (row 9 of 0..17)
static bool noteEdActive = false;
static int noteEdTrack = 0;
static int noteEdCursorNote = 60;
static int noteEdBaseNote = 48;
static int64_t noteEdIdx32 = 0;
static bool noteEdPlacing = false;
static int64_t noteEdStartIdx32 = 0;
static int noteEdVelocity = 100;
static uint64_t prevPianoStripKey[PIANO_STRIP_COUNT] = {};

static constexpr uint32_t WAVE_EDIT_MUTE_DELAY_MS = 160;
static constexpr uint32_t WAVE_EDIT_DOUBLE_TAP_MAX_MS = 350;
static uint32_t pendingWaveEditTapMs = 0;
static int pendingWaveEditTapTrack = -1;

FLASHMEM static void cancelMidiPendingWaveMute() {
  pendingWaveEditTapTrack = -1;
  pendingWaveEditTapMs = 0;
}

FLASHMEM static void applyPendingMidiWaveMute() {
  if (pendingWaveEditTapTrack < 0 || pendingWaveEditTapTrack >= NUM_MIDI_TRACKS) {
    cancelMidiPendingWaveMute();
    return;
  }
  int t = pendingWaveEditTapTrack;
  cancelMidiPendingWaveMute();
  MidiTrack &tr = midiTracks[t];
  if (tr.eventCount > 0) {
    tr.muted = !tr.muted;
    if (tr.muted) midiSeqSilenceMutedTracks();
    saveSessionWhenIdle();
  } else {
    armMidiTrack(t, (tr.armMode == ArmMode::Off) ? ArmMode::Record : ArmMode::Off);
  }
  uiForceRedraw();
}

FLASHMEM void serviceMidiPendingWaveMute(uint8_t currentKeys, bool editUsedAsMod) {
  if (pendingWaveEditTapTrack < 0) return;
  if (currentPage() != Page::Midi || noteEdActive || trackEditPhase() != TrackEditPhase::Off) {
    cancelMidiPendingWaveMute();
    return;
  }
  if (currentKeys & (BTN_OPT | BTN_SHIFT)) {
    cancelMidiPendingWaveMute();
    return;
  }
  uint32_t elapsed = (uint32_t)(millis() - pendingWaveEditTapMs);
  if (currentKeys & BTN_EDIT) {
    if (editUsedAsMod || elapsed > WAVE_EDIT_DOUBLE_TAP_MAX_MS) {
      cancelMidiPendingWaveMute();
    }
    return;
  }
  if (elapsed >= WAVE_EDIT_MUTE_DELAY_MS) {
    applyPendingMidiWaveMute();
  }
}

FLASHMEM static const char *midiHeaderTitle() {
  if (noteEdActive) {
    static char seqTitle[12];
    snprintf(seqTitle, sizeof(seqTitle), "M%d:SEQ", constrain(noteEdTrack + 1, 1, 8));
    return seqTitle;
  }
  const char *teTitle = trackEditHeaderTitle(Page::Midi);
  return teTitle ? teTitle : "MIDI";
}

FLASHMEM void invalidateMidiNoteEditorCache() {
  memset(prevPianoStripKey, 0, sizeof(prevPianoStripKey));
}

FLASHMEM static void midiInvalidateCache() {
  memset(prevMidiTrackKey, 0, sizeof(prevMidiTrackKey));
  invalidateMidiNoteEditorCache();
}

FLASHMEM static void formatMidiNoteName(uint8_t note, char *out, size_t sz) {
  PROGMEM static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
  };
  uint8_t n = note & 0x7F;
  snprintf(out, sz, "%s%u", NOTE_NAMES[n % 12], (unsigned)(n / 12));
}

FLASHMEM static void drawMidiTrackRow(int visSlot, int t, const TimelineFrameCtx &ctx, bool full) {
  const MidiTrack &mt = midiTracks[t];
  const int y = SY(26) + visSlot * SY(41);
  const int h = SY(39);
  const bool rowSel = (midiRow == t);
  const int selC = rowSel ? midiCell : -1;
  const bool staticDue = full || uiTierDue(6);

  // Cell 0: Track Badge & Arm Mode ("01".."08" + record circle when armed)
  if (staticDue || prevMidiTrackKey[visSlot][MID_CELL_ARM] == 0) {
    drawTrackArmBadgeCell(SX(2), y, SX(28), h, t, mt.armMode,
                          mt.muted, selC == MID_CELL_ARM,
                          COLOR_MIDI, prevMidiTrackKey[visSlot][MID_CELL_ARM], full);
  }

  // Cell 1: Trigger Mode ("TRG", highlighted only when ON)
  if (staticDue || prevMidiTrackKey[visSlot][MID_CELL_TRIG] == 0) {
    drawTrackTrigBadgeCell(SX(32), y, SX(32), h, mt.trigMode, selC == MID_CELL_TRIG,
                           COLOR_MIDI, prevMidiTrackKey[visSlot][MID_CELL_TRIG], full);
  }

  // Cell 2: Scrolling MIDI Timeline (15 Hz 2-row round-robin while running) + Playhead + Activity
  const bool waveDue = full || (prevMidiTrackKey[visSlot][MID_CELL_WAVE] == 0) ||
                       uiIsImmediateFrame() || (transport.mode != TransportMode::Running) ||
                       ((visSlot >> 1) == (int)uiTimelineRrSlot());
  if (waveDue) {
    const int cx = SX(66), cw = SX(184);
    const int innerW = cw - 4;
    const int playCol = innerW / 2;
    const bool sel = (selC == MID_CELL_WAVE);
    uint32_t hlStart = 0, hlEnd = 0;
    bool hlEditEnd = false;
    const bool hlActive = trackEditGetHighlight(Page::Midi, t, hlStart, hlEnd, hlEditEnd);
    uint16_t border = sel ? COLOR_MIDI : (hlActive ? COLOR_WARN : MID);
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

    uint16_t evColor = mt.muted ? DIM
                     : (mt.armMode == ArmMode::Record && mt.recWindowActive) ? COLOR_REC
                     : (mt.armMode == ArmMode::Overdub && mt.recWindowActive) ? COLOR_OVERDUB
                     : COLOR_MIDI;

    uint64_t k = hashInit();
    k = hashMix(k, (uint32_t)t);
    k = hashMix(k, border);
    k = hashMix(k, ctx.scrollStep);
    k = hashMix(k, zoom);
    k = hashMix(k, ctx.bpmKey);
    k = hashMix(k, punchOn ? ((uint64_t)(uint32_t)(punchX0 + 10000) | ((uint64_t)(uint32_t)(punchX1 + 10000) << 32)) : 0ULL);
    k = hashMix(k, (uint32_t)evColor | ((uint32_t)mt.recWindowActive << 16) | ((uint32_t)transport.mode << 24));
    k = hashMix(k, mt.overviewVersion);
    k = hashMix(k, mt.eventCount);
    k = hashMix(k, mt.muted ? 1 : 0);
    k = hashMix(k, (uint64_t)mt.activeNoteMask[0] | ((uint64_t)mt.activeNoteMask[1] << 32));
    k = hashMix(k, (uint64_t)mt.activeNoteMask[2] | ((uint64_t)mt.activeNoteMask[3] << 32));
    k = hashMix(k, hlActive ? ((uint64_t)(uint32_t)(hlX0 + 10000) |
                               ((uint64_t)(uint32_t)(hlX1 + 10000) << 32) |
                               ((uint64_t)hlEditEnd << 62) | (1ULL << 63))
                            : 0ULL);

    if (full || k != prevMidiTrackKey[visSlot][MID_CELL_WAVE]) {
      prevMidiTrackKey[visSlot][MID_CELL_WAVE] = k;
      Surface s(cw, h);
      s.drawBox(BG, border);

      const int innerX = 2;
      const int innerY = 2;
      const int innerH = h - 4;
      const int midY = innerY + innerH / 2;
      const uint16_t hlFill = blend565(COLOR_MIDI, BG, 56);

      // 1. Background, punch/selection regions, timeline ticks, and center reference line
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
          s.fillRect(innerX + hx0, innerY, hw, 1, COLOR_MIDI);
          s.fillRect(innerX + hx0, innerY + innerH - 1, hw, 1, COLOR_MIDI);
        }
      }

      drawTimelineTicksInWindow(s, innerX, innerY, innerW, innerH, curFrame, playCol, winFrames, bpm, zoom);

      int zeroCol = max(0, timelineFrameToCol(0, curFrame, playCol, innerW, winFrames));
      if (zeroCol < innerW) {
        s.fillRect(innerX + zeroCol, midY, innerW - zeroCol, 1, DARK);
      }

      // 2. Recorded MIDI notes rendered as 2px horizontal lines from Note-On to Note-Off,
      //    placed vertically within the waveform view according to note number (0..127)
      uint32_t evCnt = 0;
      const MidiEvent *evs = midiSeqEvents(t, evCnt);
      if (evs && evCnt > 0) {
        const uint32_t startTick = ctx.startTick;
        const uint32_t endTick = ctx.endTick;
        const uint32_t curTick = ctx.curTick;
        const int64_t windowTicks = ctx.windowTicks;

        const int noteH = 2;
        const int topY = innerY + 1;
        const int botY = innerY + innerH - 1 - noteH;
        const int spanY = max(1, botY - topY);

        auto drawNoteSpan = [&](uint8_t note, uint32_t t0, uint32_t t1) FLASHMEM {
          if (t1 < t0) t1 = t0;
          if (t1 < startTick || t0 > endTick) return;
          int64_t rel0 = (int64_t)t0 - (int64_t)curTick;
          int64_t rel1 = (int64_t)t1 - (int64_t)curTick;
          int x0 = playCol + (int)((rel0 * innerW) / windowTicks);
          int x1 = playCol + (int)((rel1 * innerW) / windowTicks);
          if (x1 < 0 || x0 >= innerW) return;
          int cx0 = constrain(x0, 0, innerW - 1);
          int cx1 = constrain(x1, cx0, innerW - 1);
          int barW = max(1, cx1 - cx0 + 1);
          int ny = botY - ((int)(note & 0x7F) * spanY + 63) / 127;
          s.fillRect(innerX + cx0, ny, barW, noteH, evColor);
        };

        uint32_t noteOnTick[128];
        uint32_t openMask[4] = {};
        int openCount = 0;

        auto isOpen = [&](uint8_t n) -> bool {
          return (openMask[n >> 5] & (1u << (n & 31))) != 0;
        };
        auto setOpen = [&](uint8_t n, uint32_t tk) {
          uint32_t bit = 1u << (n & 31);
          if (!(openMask[n >> 5] & bit)) {
            openMask[n >> 5] |= bit;
            openCount++;
          }
          noteOnTick[n] = tk;
        };
        auto clearOpen = [&](uint8_t n) {
          uint32_t bit = 1u << (n & 31);
          if (openMask[n >> 5] & bit) {
            openMask[n >> 5] &= ~bit;
            openCount--;
          }
        };

        uint32_t startIdx = 0;
        const uint32_t lookbehindTicks = MIDI_PPQN * 32u; // 8 bars lookbehind for sustained notes
        if (evCnt > 64 && startTick > lookbehindTicks) {
          uint32_t seekTick = startTick - lookbehindTicks;
          uint32_t lo = 0, hi = evCnt;
          while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            if (evs[mid].tick < seekTick) lo = mid + 1;
            else hi = mid;
          }
          startIdx = lo;
        }

        // 2a. Faint 1px line for recorded CC (0xB0) and Pitch Bend (0xE0) events while values are changing
        const uint16_t ccColor = blend565(evColor, BG, 115);
        const int ccTopY = innerY + 1;
        const int ccBotY = innerY + innerH - 2;
        const int ccSpanY = max(1, ccBotY - ccTopY);
        constexpr uint32_t CC_ACTIVE_GAP_TICKS = MIDI_PPQN; // 1 beat max gap while actively changing

        auto drawCcSegment = [&](uint32_t t0, int y0, uint32_t t1, int y1) FLASHMEM {
          if (t1 < t0) t1 = t0;
          if (t1 < startTick || t0 > endTick) return;
          int64_t rel0 = (int64_t)t0 - (int64_t)curTick;
          int64_t rel1 = (int64_t)t1 - (int64_t)curTick;
          int x0 = playCol + (int)((rel0 * innerW) / windowTicks);
          int x1 = playCol + (int)((rel1 * innerW) / windowTicks);
          if (x1 < 0 || x0 >= innerW) return;
          if (x0 == x1) {
            int ymin = min(y0, y1);
            int ymax = max(y0, y1);
            s.fillRect(innerX + x0, ymin, 1, ymax - ymin + 1, ccColor);
            return;
          }
          int cx0 = max(0, x0);
          int cx1 = min(innerW - 1, x1);
          int dx = x1 - x0;
          int dy = y1 - y0;
          int prevY = y0 + (cx0 - x0) * dy / dx;
          for (int cx = cx0; cx <= cx1; cx++) {
            int curY = y0 + (cx - x0) * dy / dx;
            int ymin = min(prevY, curY);
            int ymax = max(prevY, curY);
            s.fillRect(innerX + cx, ymin, 1, ymax - ymin + 1, ccColor);
            prevY = curY;
          }
        };

        struct CcLineSlot {
          uint8_t ccNum;
          uint8_t lastVal;
          uint32_t lastTick;
          bool active;
        };
        CcLineSlot ccSlots[4] = {};
        bool pbActive = false;
        uint16_t pbLastVal14 = 8192;
        uint32_t pbLastTick = 0;

        for (uint32_t i = startIdx; i < evCnt; i++) {
          const MidiEvent &e = evs[i];
          if (e.tick > endTick + CC_ACTIVE_GAP_TICKS) break;
          uint8_t hi = e.status & 0xF0;
          if (hi == 0xB0) {
            uint8_t ccNum = e.data1 & 0x7F;
            uint8_t val = e.data2 & 0x7F;
            int slot = -1;
            int oldestSlot = 0;
            for (int sIdx = 0; sIdx < 4; sIdx++) {
              if (ccSlots[sIdx].active && ccSlots[sIdx].ccNum == ccNum) {
                slot = sIdx;
                break;
              }
              if (!ccSlots[sIdx].active) {
                oldestSlot = sIdx;
              } else if (ccSlots[oldestSlot].active && ccSlots[sIdx].lastTick < ccSlots[oldestSlot].lastTick) {
                oldestSlot = sIdx;
              }
            }
            if (slot >= 0) {
              uint32_t dt = (e.tick >= ccSlots[slot].lastTick) ? (e.tick - ccSlots[slot].lastTick) : (CC_ACTIVE_GAP_TICKS + 1u);
              if (dt <= CC_ACTIVE_GAP_TICKS && val != ccSlots[slot].lastVal) {
                int y0 = ccBotY - ((int)ccSlots[slot].lastVal * ccSpanY + 63) / 127;
                int y1 = ccBotY - ((int)val * ccSpanY + 63) / 127;
                drawCcSegment(ccSlots[slot].lastTick, y0, e.tick, y1);
              }
              ccSlots[slot].lastVal = val;
              ccSlots[slot].lastTick = e.tick;
            } else {
              ccSlots[oldestSlot] = {ccNum, val, e.tick, true};
            }
          } else if (hi == 0xE0) {
            uint16_t val14 = (uint16_t)(((e.data2 & 0x7F) << 7) | (e.data1 & 0x7F));
            if (pbActive) {
              uint32_t dt = (e.tick >= pbLastTick) ? (e.tick - pbLastTick) : (CC_ACTIVE_GAP_TICKS + 1u);
              if (dt <= CC_ACTIVE_GAP_TICKS && val14 != pbLastVal14) {
                int y0 = ccBotY - ((int)pbLastVal14 * ccSpanY + 8191) / 16383;
                int y1 = ccBotY - ((int)val14 * ccSpanY + 8191) / 16383;
                drawCcSegment(pbLastTick, y0, e.tick, y1);
              }
            }
            pbActive = true;
            pbLastVal14 = val14;
            pbLastTick = e.tick;
          }
        }

        // 2b. Recorded MIDI notes rendered on top as 2px horizontal lines from Note-On to Note-Off
        for (uint32_t i = startIdx; i < evCnt; i++) {
          const MidiEvent &e = evs[i];
          if (e.tick > endTick && openCount <= 0) break;

          uint8_t hi = e.status & 0xF0;
          uint8_t note = e.data1 & 0x7F;
          bool isNoteOn = (hi == 0x90);
          bool isNoteOff = (hi == 0x80);

          if (isNoteOn) {
            if (isOpen(note)) {
              drawNoteSpan(note, noteOnTick[note], e.tick);
              if (e.tick > endTick) {
                clearOpen(note);
                continue;
              }
            }
            if (e.tick <= endTick) {
              setOpen(note, e.tick);
            }
          } else if (isNoteOff) {
            if (isOpen(note)) {
              drawNoteSpan(note, noteOnTick[note], e.tick);
              clearOpen(note);
            }
          }
        }

        if (openCount > 0) {
          for (uint8_t n = 0; n < 128; n++) {
            if (isOpen(n)) {
              uint32_t t0 = noteOnTick[n];
              uint32_t t1 = (transport.mode == TransportMode::Running && curTick > t0) ? curTick : t0;
              drawNoteSpan(n, t0, t1);
            }
          }
        }
      }

      // 3. Punch boundaries, region edit edges, and center playhead
      if (punchOn) {
        if (punchX0 >= 0 && punchX0 < innerW) {
          s.drawDottedVLine(innerX + punchX0, innerY, innerH, COLOR_MIDI);
        }
        if (punchX1 >= 0 && punchX1 < innerW) {
          s.drawDottedVLine(innerX + punchX1, innerY, innerH, COLOR_MIDI);
        }
      }
      if (hlActive) {
        if (hlX0 >= 0 && hlX0 < innerW) {
          bool isActiveEdge = (trackEditPhase() == TrackEditPhase::SelectRegion) ? !hlEditEnd : true;
          s.fillRect(innerX + hlX0, innerY, 1, innerH, isActiveEdge ? COLOR_WARN : COLOR_MIDI);
        }
        if (hlX1 >= 0 && hlX1 < innerW) {
          bool isActiveEdge = (trackEditPhase() == TrackEditPhase::SelectRegion) ? hlEditEnd : true;
          s.fillRect(innerX + hlX1, innerY, 1, innerH, isActiveEdge ? COLOR_WARN : COLOR_MIDI);
        }
      }
      s.fillRect(innerX + playCol, innerY, 1, innerH, mt.recWindowActive ? COLOR_REC : INK);

      if (mt.muted) {
        s.drawText(SX(6), SY(8), "MUTED", DIM, BG);
      } else if (mt.eventCount == 0) {
        s.drawText(SX(6), SY(8), "EMPTY", DIM, BG);
      }

      const bool anyNoteActive = (mt.activeNoteMask[0] | mt.activeNoteMask[1] |
                                  mt.activeNoteMask[2] | mt.activeNoteMask[3]) != 0;
      if (anyNoteActive) {
        char act[48];
        act[0] = '\0';
        size_t pos = 0;
        const int maxTextW = cw - SX(10);
        for (uint8_t n = 0; n < 128; n++) {
          if (mt.activeNoteMask[n >> 5] & (1u << (n & 31))) {
            char noteName[8];
            formatMidiNoteName(n, noteName, sizeof(noteName));
            char cand[8];
            int cLen = snprintf(cand, sizeof(cand), "%s%s", (pos > 0) ? " " : "", noteName);
            if (cLen <= 0 || pos + (size_t)cLen >= sizeof(act)) break;
            if (pos > 0 && textWidth(act) + textWidth(cand) > maxTextW) break;
            memcpy(act + pos, cand, (size_t)cLen + 1);
            pos += (size_t)cLen;
          }
        }
        if (act[0] != '\0') {
          s.drawText(cw - textWidth(act) - SX(4), SY(8), act, COLOR_MIDI, BG);
        }
      }
      s.blit(cx, y);
    }
  }

  // Cell 3: MIDI Input Channel (2-line "IN" / "OFF", "ALL", "C01".."C16")
  if (staticDue || prevMidiTrackKey[visSlot][MID_CELL_INCH] == 0) {
    char chStr[8];
    if (mt.inChannel == MIDI_IN_OFF) snprintf(chStr, sizeof(chStr), "OFF");
    else if (mt.inChannel == 0) snprintf(chStr, sizeof(chStr), "ALL");
    else snprintf(chStr, sizeof(chStr), "C%02d", (int)mt.inChannel);
    drawTwoLineParamBox(SX(252), y, SX(32), h, "IN", chStr,
                        selC == MID_CELL_INCH, mt.inChannel != MIDI_IN_OFF, COLOR_MIDI,
                        SY(7), SY(21), prevMidiTrackKey[visSlot][MID_CELL_INCH], full);
  }

  // Cell 4: MIDI Output Destination (2-line "OUT" / "OFF", "C01".."C16", "I01".."I08")
  if (staticDue || prevMidiTrackKey[visSlot][MID_CELL_OUTCH] == 0) {
    char chStr[8];
    if (mt.outChannel == 0) {
      snprintf(chStr, sizeof(chStr), "OFF");
    } else if (mt.outChannel >= 17) {
      snprintf(chStr, sizeof(chStr), "I%02d", (int)mt.outChannel - 16);
    } else {
      snprintf(chStr, sizeof(chStr), "C%02d", (int)mt.outChannel);
    }
    drawTwoLineParamBox(SX(286), y, SX(32), h, "OUT", chStr,
                        selC == MID_CELL_OUTCH, mt.outChannel != 0, COLOR_MIDI,
                        SY(7), SY(21), prevMidiTrackKey[visSlot][MID_CELL_OUTCH], full);
  }
}

FLASHMEM static void midiDrawPage(bool full) {
  syncMidiScroll();
  const int cw = SX(184);
  const int innerW = cw - 4;
  TimelineFrameCtx ctx;
  computeTimelineFrameCtx(ctx, innerW, innerW / 2);
  for (int vis = 0; vis < VISIBLE_TRACK_ROWS; vis++) {
    drawMidiTrackRow(vis, midiScroll + vis, ctx, full);
    keepStreaming();
  }
  drawTransportBar(midiRow == TRANSPORT_ROW, midiCell, full);
}

FLASHMEM void midiRememberTrackCursor() {
  if (midiRow >= 0 && midiRow < TRACK_ROWS) {
    lastMidiTrackRow = midiRow;
    lastMidiTrackCell = (midiCell >= 0 && midiCell < MID_CELL_COUNT) ? midiCell : -1;
  }
}

FLASHMEM void midiRestoreLastTrackCursor() {
  int row = (lastMidiTrackRow >= 0 && lastMidiTrackRow < TRACK_ROWS) ? lastMidiTrackRow : 0;
  int cell = (lastMidiTrackRow >= 0 && lastMidiTrackRow < TRACK_ROWS &&
              lastMidiTrackCell >= 0 && lastMidiTrackCell < MID_CELL_COUNT)
                 ? lastMidiTrackCell
                 : MID_CELL_WAVE;
  midiRow = row;
  midiCell = cell;
  lastMidiTrackRow = row;
  lastMidiTrackCell = cell;
  syncMidiScroll();
}

FLASHMEM void midiSyncCursorFromAudio() {
  if (audioRow == HEADER_ROW) {
    midiRow = HEADER_ROW;
    headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
  } else if (audioRow == INFO_ROW) {
    midiRow = INFO_ROW;
    midiCell = constrain(audioCell, 0, activeInfoCellCount() - 1);
  } else if (audioRow == TRANSPORT_ROW) {
    midiRow = TRANSPORT_ROW;
    midiCell = constrain(audioCell, 0, TR_CELL_COUNT - 1);
  } else {
    midiRestoreLastTrackCursor();
  }
}

FLASHMEM static void midiOnShiftNavigate(uint8_t dir) {
  applyPendingMidiWaveMute();
  if (trackEditIsActive()) {
    if (dir & BTN_DOWN) {
      trackEditCancel();
    }
    return;
  }
  if (dir & BTN_LEFT) {
    midiRememberTrackCursor();
    audioSyncCursorFromMidi();
    setPage(Page::Audio);
  } else if (dir & BTN_RIGHT) {
    midiRememberTrackCursor();
    int refTrack = (midiRow >= 0 && midiRow < TRACK_ROWS)
                       ? midiRow
                       : constrain(lastMidiTrackRow, 0, TRACK_ROWS - 1);
    if (midiTracks[refTrack].outChannel >= 17) {
      instListCursor = constrain((int)midiTracks[refTrack].outChannel - 17, 0, NUM_INSTRUMENTS - 1);
    }
    setPage(Page::Instruments);
  } else if (dir & BTN_UP) {
    if (midiRow >= INFO_ROW) {
      midiRestoreLastTrackCursor();
      uiForceRedraw();
    } else if (midiRow >= 0) {
      midiRememberTrackCursor();
      midiRow = HEADER_ROW;
      headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
      uiForceRedraw();
    } else {
      setPage(lastRow1Page());
    }
  } else if (dir & BTN_DOWN) {
    if (midiRow == HEADER_ROW) {
      midiRestoreLastTrackCursor();
      uiForceRedraw();
    } else if (midiRow < INFO_ROW) {
      midiRememberTrackCursor();
      midiRow = INFO_ROW;
      midiCell = constrain(midiCell, 0, activeInfoCellCount() - 1);
      uiForceRedraw();
    } else {
      setPage(Page::Master);
    }
  }
}

FLASHMEM static void midiOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
  applyPendingMidiWaveMute();
  if (trackEditPhase() == TrackEditPhase::PickPasteTrack) {
    if (dy != 0) {
      midiRow = constrain(midiRow + dy, 0, TRACK_ROWS - 1);
      syncMidiScroll();
    }
    midiCell = MID_CELL_WAVE;
    lastMidiTrackRow = midiRow;
    lastMidiTrackCell = midiCell;
    uiForceRedraw();
    return;
  }
  if (dy != 0) {
    if (midiRow == HEADER_ROW) {
      // Top Bar has 1 row; require SHIFT+DOWN to enter Timeline
    } else if (midiRow >= INFO_ROW) {
      midiRow = constrain(midiRow + dy, INFO_ROW, TRANSPORT_ROW);
    } else {
      midiRow = constrain(midiRow + dy, 0, TRACK_ROWS - 1);
      syncMidiScroll();
    }
    if (midiRow == HEADER_ROW) {
      headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
    } else if (midiRow == TRANSPORT_ROW) {
      midiCell = constrain(midiCell, 0, TR_CELL_COUNT - 1);
    } else if (midiRow == INFO_ROW) {
      midiCell = constrain(midiCell, 0, activeInfoCellCount() - 1);
    } else {
      midiCell = constrain(midiCell, 0, MID_CELL_COUNT - 1);
    }
  }
  if (dx != 0) {
    if (midiRow == HEADER_ROW) {
      headerCell = constrain(headerCell + dx, 0, HEADER_CELL_COUNT - 1);
    } else {
      int maxCell = (midiRow == TRANSPORT_ROW) ? (TR_CELL_COUNT - 1)
                  : (midiRow == INFO_ROW)      ? (activeInfoCellCount() - 1)
                                               : (MID_CELL_COUNT - 1);
      midiCell = constrain(midiCell + dx, 0, maxCell);
    }
  }
  if (midiRow >= 0 && midiRow < TRACK_ROWS) {
    lastMidiTrackRow = midiRow;
    lastMidiTrackCell = midiCell;
  }
  uiForceRedraw();
}

FLASHMEM static void midiOnOptAdjust(int dx, int dy, int delta) {
  cancelMidiPendingWaveMute();
  if (midiRow == HEADER_ROW) {
    handleHeaderCellOptAdjust(headerCell, dx, dy);
    return;
  }
  if (midiRow < TRACK_ROWS) {
    int t = midiRow;
    MidiTrack &tr = midiTracks[t];
    if (midiCell == MID_CELL_ARM) {
      int m = ((int)tr.armMode + delta + 3) % 3;
      armMidiTrack(t, (ArmMode)m);
    } else if (midiCell == MID_CELL_TRIG) {
      tr.trigMode = (tr.trigMode == TrigMode::Manual) ? TrigMode::Threshold : TrigMode::Manual;
      saveSessionWhenIdle();
    } else if (midiCell == MID_CELL_INCH) {
      midiSeqStepInChannel(t, delta);
      saveSessionWhenIdle();
    } else if (midiCell == MID_CELL_OUTCH) {
      midiSeqStepOutChannel(t, dy, delta);
      saveSessionWhenIdle();
    }
    uiForceRedraw();
  } else if (midiRow == INFO_ROW) {
    handleInfoCellOptAdjust(midiCell, dx, dy, delta);
  } else if (midiRow == TRANSPORT_ROW) {
    handleTransportCellOptAdjust(midiCell, dx, dy, delta);
  }
}

FLASHMEM static void midiOnEditPress(bool shiftHeld) {
  if (trackEditPhase() == TrackEditPhase::PickPasteTrack) {
    if (!shiftHeld && midiRow >= 0 && midiRow < TRACK_ROWS) {
      trackEditBeginPastePositioning(midiRow);
    }
    return;
  }
  if (midiRow == HEADER_ROW) {
    handleHeaderCellEditPress(shiftHeld);
    return;
  }
  if (midiRow == TRANSPORT_ROW) {
    handleTransportCellAction(midiCell, shiftHeld);
    return;
  }
  if (midiRow == INFO_ROW) {
    handleInfoCellEditPress(midiCell, shiftHeld);
    return;
  }
  int t = midiRow;
  MidiTrack &tr = midiTracks[t];
  if (midiCell == MID_CELL_WAVE) {
    if (shiftHeld) {
      cancelMidiPendingWaveMute();
      appSeekTape(0);
      uiForceRedraw();
    } else {
      uint32_t nowMs = millis();
      if (pendingWaveEditTapTrack == t &&
          (uint32_t)(nowMs - pendingWaveEditTapMs) <= WAVE_EDIT_DOUBLE_TAP_MAX_MS) {
        cancelMidiPendingWaveMute();
        openMidiNoteEditor(t);
        return;
      }
      if (pendingWaveEditTapTrack >= 0 && pendingWaveEditTapTrack != t) {
        applyPendingMidiWaveMute();
      }
      pendingWaveEditTapTrack = t;
      pendingWaveEditTapMs = nowMs;
    }
  } else if (midiCell == MID_CELL_ARM) {
    ArmMode next = (tr.armMode == ArmMode::Off)      ? ArmMode::Record
                 : (tr.armMode == ArmMode::Record)   ? ArmMode::Overdub
                                                     : ArmMode::Off;
    armMidiTrack(t, next);
    uiForceRedraw();
  } else if (midiCell == MID_CELL_TRIG) {
    tr.trigMode = (tr.trigMode == TrigMode::Manual) ? TrigMode::Threshold : TrigMode::Manual;
    saveSessionWhenIdle();
    uiForceRedraw();
  } else if (midiCell == MID_CELL_INCH) {
    midiSeqCycleInChannel(t);
    saveSessionWhenIdle();
    uiForceRedraw();
  } else if (midiCell == MID_CELL_OUTCH) {
    if (shiftHeld && tr.outChannel >= 17) {
      instListCursor = constrain((int)tr.outChannel - 17, 0, NUM_INSTRUMENTS - 1);
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
    uiForceRedraw();
  }
}

FLASHMEM static void midiOnResetParam() {
  cancelMidiPendingWaveMute();
  if (midiRow == HEADER_ROW) {
    handleHeaderCellReset(headerCell);
    return;
  }
  if (midiRow < TRACK_ROWS) {
    int t = midiRow;
    if (midiCell == MID_CELL_WAVE) {
      openClearTrackModal(Page::Midi, t);
    } else if (midiCell == MID_CELL_ARM) {
      armMidiTrack(t, ArmMode::Off);
    } else if (midiCell == MID_CELL_TRIG) {
      midiTracks[t].trigMode = TrigMode::Manual;
      saveSessionWhenIdle();
    } else if (midiCell == MID_CELL_INCH) {
      midiTracks[t].inChannel = 0;  // Omni
      saveSessionWhenIdle();
    } else if (midiCell == MID_CELL_OUTCH) {
      midiSeqSetOutChannel(t, (uint8_t)(17 + t));
      saveSessionWhenIdle();
    }
    uiForceRedraw();
  } else if (midiRow == INFO_ROW) {
    handleInfoCellReset(midiCell);
  } else if (midiRow == TRANSPORT_ROW) {
    handleTransportCellReset(midiCell);
  }
}

PROGMEM static const PageOps MIDI_PAGE_OPS = {
  Page::Midi,
  midiHeaderTitle,
  nullptr,
  midiInvalidateCache,
  midiDrawPage,
  midiOnShiftNavigate,
  midiOnCursorMove,
  midiOnOptAdjust,
  midiOnEditPress,
  midiOnResetParam
};

FLASHMEM const PageOps &pageMidiOps() {
  return MIDI_PAGE_OPS;
}

// ============================================================================
// MIDI Note Sequence Editor (1.5-Octave Piano Roll + 1/32nd Timeline Grid)
// ============================================================================
FLASHMEM bool isMidiNoteEditorOpen() {
  return noteEdActive;
}

FLASHMEM void openMidiNoteEditor(int track) {
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
  }
  timelineZoom = ZOOM_4_BARS;
  noteEdTrack = constrain(track, 0, NUM_MIDI_TRACKS - 1);
  double totalBeats = framesToBeats(transport.tapeFrame, bpm);
  int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));
  noteEdIdx32 = constrain((int64_t)llround(totalBeats * 8.0), 0LL, maxIdx32 - 1LL);
  appSeekTapeTo32nd(noteEdIdx32);

  uint32_t evCnt = 0;
  const MidiEvent *evs = midiSeqEvents(noteEdTrack, evCnt);
  uint32_t curTick = (uint32_t)(noteEdIdx32 * (MIDI_PPQN / 8));
  int bestNote = -1;
  uint32_t bestDist = 0xFFFFFFFFu;
  if (evs && evCnt > 0) {
    for (uint32_t i = 0; i < evCnt; i++) {
      if ((evs[i].status & 0xF0) == 0x90 && evs[i].data2 > 0) {
        uint32_t dist = (evs[i].tick >= curTick) ? (evs[i].tick - curTick) : (curTick - evs[i].tick);
        if (dist < bestDist) {
          bestDist = dist;
          bestNote = evs[i].data1 & 0x7F;
        }
      }
    }
  }

  uint8_t lastHi = midiTracks[noteEdTrack].lastStatus & 0xF0;
  if (bestNote >= 0) {
    noteEdCursorNote = bestNote;
  } else if (lastHi == 0x90 || lastHi == 0x80) {
    noteEdCursorNote = midiTracks[noteEdTrack].lastData1 & 0x7F;
  } else {
    noteEdCursorNote = 60; // C5 (in C0 = 0 convention)
  }

  noteEdBaseNote = noteEdCursorNote - (PIANO_VIS_KEYS - 1 - PIANO_CURSOR_ROW);

  noteEdPlacing = false;
  noteEdVelocity = 100;
  noteEdStartIdx32 = noteEdIdx32;
  noteEdActive = true;
  invalidateMidiNoteEditorCache();
  uiShowToast("", 0);
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void midiNoteEditorCommitIfPlacing() {
  if (!noteEdActive || !noteEdPlacing) return;
  const uint32_t stepTicks = MIDI_PPQN / 8;
  int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));
  int64_t endIdx32 = max(noteEdStartIdx32 + 1LL, noteEdIdx32);
  uint32_t startTick = (uint32_t)(noteEdStartIdx32 * stepTicks);
  uint32_t endTick = (uint32_t)(endIdx32 * stepTicks);
  midiSeqInsertNote(noteEdTrack, (uint8_t)noteEdCursorNote, startTick, endTick,
                    (uint8_t)constrain(noteEdVelocity, 0, 127), bpm);
  noteEdPlacing = false;
  noteEdIdx32 = constrain(noteEdStartIdx32, 0LL, maxIdx32 - 1LL);
  appSeekTapeTo32nd(noteEdIdx32);
}

FLASHMEM void closeMidiNoteEditor() {
  if (!noteEdActive) return;
  midiNoteEditorCommitIfPlacing();
  noteEdActive = false;
  noteEdPlacing = false;
  midiRow = constrain(noteEdTrack, 0, TRACK_ROWS - 1);
  midiCell = MID_CELL_WAVE;
  syncMidiScroll();
  invalidateMidiNoteEditorCache();
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void midiNoteEditorDir(uint8_t dir, bool shiftHeld) {
  if (!noteEdActive) return;
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
    double totalBeats = framesToBeats(transport.tapeFrame, bpm);
    int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));
    noteEdIdx32 = constrain((int64_t)llround(totalBeats * 8.0), 0LL, maxIdx32 - 1LL);
  }

  int dx = ((dir & BTN_RIGHT) ? 1 : 0) - ((dir & BTN_LEFT) ? 1 : 0);
  int dy = ((dir & BTN_UP) ? 1 : 0) - ((dir & BTN_DOWN) ? 1 : 0);
  int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));

  if (noteEdPlacing) {
    bool changed = false;
    if (dx != 0) {
      int64_t step = (int64_t)dx * (shiftHeld ? 8 : 1);
      noteEdIdx32 = constrain(noteEdIdx32 + step, noteEdStartIdx32, maxIdx32);
      appSeekTapeTo32nd(noteEdIdx32);
      changed = true;
    }
    if (dy != 0) {
      if (shiftHeld) {
        int nextVel = constrain(noteEdVelocity + dy, 0, 127);
        if (nextVel != noteEdVelocity) {
          noteEdVelocity = nextVel;
          changed = true;
        }
      } else {
        int nextNote = constrain(noteEdCursorNote + dy, 0, 127);
        if (nextNote != noteEdCursorNote) {
          noteEdCursorNote = nextNote;
          noteEdBaseNote = noteEdCursorNote - (PIANO_VIS_KEYS - 1 - PIANO_CURSOR_ROW);
          uint8_t outCh = midiTracks[noteEdTrack].outChannel;
          if (outCh >= 17 && outCh <= MAX_MIDI_OUT_CHAN && noteEdVelocity > 0) {
            fmSynthAudition((int)outCh - 17, (uint8_t)noteEdCursorNote, (uint8_t)noteEdVelocity, 220);
          }
          changed = true;
        }
      }
    }
    if (changed) {
      uiForceRedraw();
    }
    return;
  }

  if (dx != 0) {
    int64_t step = (int64_t)dx * (shiftHeld ? 8 : 1);
    noteEdIdx32 = constrain(noteEdIdx32 + step, 0LL, maxIdx32 - 1LL);
    appSeekTapeTo32nd(noteEdIdx32);
  }
  if (dy != 0) {
    int pitchStep = dy * (shiftHeld ? 5 : 1);
    noteEdCursorNote = constrain(noteEdCursorNote + pitchStep, 0, 127);
    noteEdBaseNote = noteEdCursorNote - (PIANO_VIS_KEYS - 1 - PIANO_CURSOR_ROW);
  }
  uiForceRedraw();
}

FLASHMEM void midiNoteEditorEditPress(bool shiftHeld) {
  if (!noteEdActive) return;
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
    double totalBeats = framesToBeats(transport.tapeFrame, bpm);
    int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));
    noteEdIdx32 = constrain((int64_t)llround(totalBeats * 8.0), 0LL, maxIdx32 - 1LL);
  }

  const uint32_t stepTicks = MIDI_PPQN / 8; // 60 ticks per 1/32nd
  int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));

  if (shiftHeld && !noteEdPlacing) {
    uint32_t curTick = (uint32_t)(noteEdIdx32 * stepTicks);
    midiSeqDeleteNoteAt(noteEdTrack, (uint8_t)noteEdCursorNote, curTick, bpm);
    uiForceRedraw();
    return;
  }

  if (!noteEdPlacing) {
    uint32_t curTick = (uint32_t)(noteEdIdx32 * stepTicks);
    uint32_t foundStartTick = 0, foundEndTick = 0;
    uint8_t foundVel = 100;
    if (midiSeqFindNoteAt(noteEdTrack, (uint8_t)noteEdCursorNote, curTick,
                          foundStartTick, foundEndTick, foundVel)) {
      // Cursor is on an already placed note: pick it up for editing and jump cursor to note end
      midiSeqDeleteNoteAt(noteEdTrack, (uint8_t)noteEdCursorNote, curTick, bpm);
      noteEdPlacing = true;
      noteEdVelocity = constrain((int)foundVel, 0, 127);
      noteEdStartIdx32 = constrain((int64_t)(foundStartTick / stepTicks), 0LL, maxIdx32 - 1LL);
      int64_t durSteps = max((int64_t)1, (int64_t)((foundEndTick - foundStartTick + stepTicks / 2) / stepTicks));
      noteEdIdx32 = constrain(noteEdStartIdx32 + durSteps, min(maxIdx32, noteEdStartIdx32 + 1LL), maxIdx32);
      appSeekTapeTo32nd(noteEdIdx32);
    } else {
      // First EDIT press on empty cell: anchor Note-On at current cursor cell without advancing cursor
      noteEdPlacing = true;
      noteEdVelocity = 100;
      noteEdStartIdx32 = constrain(noteEdIdx32, 0LL, maxIdx32 - 1LL);
      noteEdIdx32 = noteEdStartIdx32;
    }
    uint8_t outCh = midiTracks[noteEdTrack].outChannel;
    if (outCh >= 17 && outCh <= MAX_MIDI_OUT_CHAN && noteEdVelocity > 0) {
      fmSynthAudition((int)outCh - 17, (uint8_t)noteEdCursorNote, (uint8_t)noteEdVelocity, 220);
    }
    uiForceRedraw();
  } else {
    // Second EDIT press: commit Note-Off at current cursor position and return cursor to note start
    midiNoteEditorCommitIfPlacing();
    uiForceRedraw();
  }
}

FLASHMEM void midiNoteEditorResetPress() {
  if (!noteEdActive) return;
  if (noteEdPlacing) {
    noteEdPlacing = false;
    noteEdIdx32 = noteEdStartIdx32;
    appSeekTapeTo32nd(noteEdIdx32);
    uiForceRedraw();
    return;
  }
  uint32_t curTick = (uint32_t)(noteEdIdx32 * (MIDI_PPQN / 8));
  midiSeqDeleteNoteAt(noteEdTrack, (uint8_t)noteEdCursorNote, curTick, bpm);
  uiForceRedraw();
}

// Compact 5x7 bitmap glyphs (7 rows x 5 cols packed into 35 bits) for 18-row Piano Roll key labels
#if defined(ARDUINO_TEENSY_MICROMOD)
static constexpr int MINI_KEY_FONT_H = 9;
#else
static constexpr int MINI_KEY_FONT_H = 7;
#endif

FLASHMEM static uint64_t miniKeyGlyphBits(char c) {
  switch (c) {
    case '0': return 0b01110100011000110001100011000101110ULL;
    case '1': return 0b00100011000010000100001000010001110ULL;
    case '2': return 0b01110100010000101110100001000011111ULL;
    case '3': return 0b01110100010000100110000011000101110ULL;
    case '4': return 0b10001100011000111111000010000100001ULL;
    case '5': return 0b11111100001000011110000011000101110ULL;
    case '6': return 0b01110100001000011110100011000101110ULL;
    case '7': return 0b11111000010001000100001000010000100ULL;
    case '8': return 0b01110100011000101110100011000101110ULL;
    case '9': return 0b01110100011000101111000010000101110ULL;
    case 'A': return 0b01110100011000111111100011000110001ULL;
    case 'B': return 0b11110100011000111110100011000111110ULL;
    case 'C': return 0b01110100011000010000100001000101110ULL;
    case 'D': return 0b11110100011000110001100011000111110ULL;
    case 'E': return 0b11111100001000011110100001000011111ULL;
    case 'F': return 0b11111100001000011110100001000010000ULL;
    case 'G': return 0b01110100011000010111100011000101110ULL;
    case '#': return 0b01010010101111101010111110101001010ULL;
    default:  return 0ULL;
  }
}

FLASHMEM static void drawMiniKeyLabel(Surface &s, int x, int y, const char *str, uint16_t color) {
#if defined(ARDUINO_TEENSY_MICROMOD)
  constexpr int gap = 2;
#else
  constexpr int gap = 1;
#endif
  int curX = x;
  for (const char *p = str; *p; p++) {
    uint64_t bits = miniKeyGlyphBits(*p);
    int py = y;
    for (int r = 0; r < 7; r++) {
#if defined(ARDUINO_TEENSY_MICROMOD)
      int rh = (r == 2 || r == 4) ? 2 : 1;
#else
      constexpr int rh = 1;
#endif
      uint8_t rowBits = (uint8_t)((bits >> ((6 - r) * 5)) & 0x1Fu);
      int px = curX;
      for (int c = 0; c < 5; c++) {
#if defined(ARDUINO_TEENSY_MICROMOD)
        int cw = (c == 1 || c == 3) ? 2 : 1;
#else
        constexpr int cw = 1;
#endif
        if (rowBits & (1u << (4 - c))) {
          s.fillRect(px, py, cw, rh, color);
        }
        px += cw;
      }
      py += rh;
    }
#if defined(ARDUINO_TEENSY_MICROMOD)
    curX += 7 + gap;
#else
    curX += 5 + gap;
#endif
  }
}

FLASHMEM void drawMidiNoteEditorOverlay(bool full) {
  if (!noteEdActive) return;

  const uint32_t stepTicks = MIDI_PPQN / 8; // 60 ticks per 1/32nd
  if (transport.mode == TransportMode::Running && !noteEdPlacing) {
    double totalBeats = framesToBeats(transport.tapeFrame, bpm);
    int64_t maxIdx32 = max((int64_t)1, (int64_t)(framesToBeats(MAX_REC_FRAMES, bpm) * 8.0 + 1e-6));
    noteEdIdx32 = constrain((int64_t)(totalBeats * 8.0 + 1e-6), 0LL, maxIdx32 - 1LL);
  }

  constexpr int pKeyH = SY(12);
  constexpr int STRIP_H = PIANO_KEYS_PER_STRIP * pKeyH;
  constexpr int PIANO_TOP_Y = SY(24) + (SCREEN_H - SY(24) - PIANO_VIS_KEYS * pKeyH) / 2;
  constexpr int PIANO_W = SX(34);
  constexpr int GRID_X = PIANO_W;
  constexpr int GRID_W = SCREEN_W - PIANO_W;
  constexpr int GRID_INNER_W = GRID_W - PX;
  constexpr int PLAY_COL = GRID_INNER_W / 2;

  const uint8_t zoom = timelineZoom;
  const uint32_t winFrames = timelineWindowFrames(zoom, bpm);
  const int64_t windowTicks = max((int64_t)1, (int64_t)framesToMidiTicks(winFrames, bpm));
  const uint32_t curTick = (transport.mode == TransportMode::Running && !noteEdPlacing)
                               ? framesToMidiTicks(transport.tapeFrame, bpm)
                               : (uint32_t)(noteEdIdx32 * stepTicks);
  const int64_t halfWinTicks = windowTicks / 2;
  const uint32_t startTick = (curTick > (uint32_t)halfWinTicks) ? (curTick - (uint32_t)halfWinTicks) : 0u;
  const uint32_t endTick = curTick + (uint32_t)halfWinTicks + stepTicks;
  const int stepPxW = constrain((int)((stepTicks * (int64_t)GRID_INNER_W + windowTicks / 2) / windowTicks),
                                2, GRID_INNER_W / 2);
  const uint32_t bpmKey = (uint32_t)(bpm * 10.0f + 0.5f);
  const bool trackMuted = midiTracks[noteEdTrack].muted;

  struct VisNoteBox {
    uint8_t note;
    uint8_t cursorHit;
    int16_t x0;
    int16_t x1;
  };
  static constexpr int MAX_VIS_NOTES = 160;
  VisNoteBox visNotes[MAX_VIS_NOTES];
  int visNoteCount = 0;

  uint32_t evCnt = 0;
  const MidiEvent *evs = midiSeqEvents(noteEdTrack, evCnt);
  if (evs && evCnt > 0) {
    auto addVisSpan = [&](uint8_t note, uint32_t t0, uint32_t t1) FLASHMEM {
      if ((int)note < noteEdBaseNote || (int)note >= noteEdBaseNote + PIANO_VIS_KEYS) return;
      if (t1 < t0) t1 = t0;
      if (t1 < startTick || t0 > endTick) return;
      int64_t rel0 = (int64_t)t0 - (int64_t)curTick;
      int64_t rel1 = (int64_t)t1 - (int64_t)curTick;
      int px0 = PLAY_COL + (int)((rel0 * GRID_INNER_W) / windowTicks);
      int px1 = PLAY_COL + (int)((rel1 * GRID_INNER_W) / windowTicks);
      if (px1 <= px0) px1 = px0 + max(2, stepPxW);
      if (px1 <= 0 || px0 >= GRID_INNER_W) return;
      int16_t cx0 = (int16_t)constrain(px0, 0, GRID_INNER_W - 1);
      int16_t cx1 = (int16_t)constrain(px1, (int)cx0 + 1, GRID_INNER_W);
      uint32_t spanEnd = max(t0 + 1u, t1);
      uint8_t hit = (!noteEdPlacing && note == (uint8_t)noteEdCursorNote &&
                     t0 < curTick + stepTicks && spanEnd > curTick) ? 1 : 0;
      if (visNoteCount < MAX_VIS_NOTES) {
        visNotes[visNoteCount++] = {note, hit, cx0, cx1};
      }
    };

    uint32_t noteOnTick[128];
    uint32_t openMask[4] = {};
    int openCount = 0;
    auto isOpen = [&](uint8_t n) -> bool {
      return (openMask[n >> 5] & (1u << (n & 31))) != 0;
    };
    auto setOpen = [&](uint8_t n, uint32_t tk) {
      uint32_t bit = 1u << (n & 31);
      if (!(openMask[n >> 5] & bit)) {
        openMask[n >> 5] |= bit;
        openCount++;
      }
      noteOnTick[n] = tk;
    };
    auto clearOpen = [&](uint8_t n) {
      uint32_t bit = 1u << (n & 31);
      if (openMask[n >> 5] & bit) {
        openMask[n >> 5] &= ~bit;
        openCount--;
      }
    };

    uint32_t startIdx = 0;
    const uint32_t lookbehindTicks = MIDI_PPQN * 32u; // 8 bars lookbehind for sustained notes
    if (evCnt > 64 && startTick > lookbehindTicks) {
      uint32_t seekTick = startTick - lookbehindTicks;
      uint32_t lo = 0, hi = evCnt;
      while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (evs[mid].tick < seekTick) lo = mid + 1;
        else hi = mid;
      }
      startIdx = lo;
    }

    for (uint32_t i = startIdx; i < evCnt; i++) {
      const MidiEvent &e = evs[i];
      if (e.tick > endTick && openCount <= 0) break;

      uint8_t hi = e.status & 0xF0;
      uint8_t note = e.data1 & 0x7F;
      bool isNoteOn = (hi == 0x90);
      bool isNoteOff = (hi == 0x80);

      if (isNoteOn) {
        if (isOpen(note)) {
          addVisSpan(note, noteOnTick[note], e.tick);
          if (e.tick > endTick) {
            clearOpen(note);
            continue;
          }
        }
        if (e.tick <= endTick) {
          setOpen(note, e.tick);
        }
      } else if (isNoteOff) {
        if (isOpen(note)) {
          addVisSpan(note, noteOnTick[note], e.tick);
          clearOpen(note);
        }
      }
    }

    if (openCount > 0) {
      for (uint8_t n = 0; n < 128; n++) {
        if (isOpen(n)) {
          uint32_t t0 = noteOnTick[n];
          uint32_t t1 = (transport.mode == TransportMode::Running && curTick > t0) ? curTick : (t0 + stepTicks);
          addVisSpan(n, t0, t1);
        }
      }
    }
  }

  // Determine vertical grid subdivision tick step for the current zoom level
  const uint32_t barTicks = BEATS_PER_BAR * MIDI_PPQN;
  const uint32_t beatTicks = MIDI_PPQN;
  const uint32_t eighthTicks = MIDI_PPQN / 2;
  uint32_t gridStepTicks = stepTicks;        // ZOOM_4_BARS: 1/32nd
  if (zoom == ZOOM_16_BARS) {
    gridStepTicks = beatTicks;               // ZOOM_16_BARS: bars & beats only
  } else if (zoom == ZOOM_32_BARS) {
    gridStepTicks = barTicks;                // ZOOM_32_BARS: bars only
  } else if (zoom == ZOOM_FULL) {
    gridStepTicks = 16 * barTicks;           // ZOOM_FULL: every 16 bars only
  }
  const uint32_t firstGridTick = (startTick / gridStepTicks) * gridStepTicks;
  const int zeroCol = PLAY_COL - (int)(((int64_t)curTick * GRID_INNER_W) / windowTicks);
  const uint32_t scrollStep = (uint32_t)(((uint64_t)curTick * GRID_INNER_W) / (uint64_t)windowTicks);

  // Bottom-right "[note name], VEL###" box when cursor is inside a note box or editing a note
  constexpr int TOTAL_OVERLAY_H = PIANO_VIS_KEYS * pKeyH;
  constexpr int INFO_BOX_H = SY(18);
  constexpr int INFO_BOX_Y = TOTAL_OVERLAY_H - 1 - SY(2) - INFO_BOX_H;
  bool showNoteInfoBox = false;
  uint8_t infoNote = (uint8_t)constrain(noteEdCursorNote, 0, 127);
  uint8_t infoVel = (uint8_t)constrain(noteEdVelocity, 0, 127);
  if (noteEdPlacing) {
    showNoteInfoBox = true;
  } else {
    uint32_t fStart = 0, fEnd = 0;
    uint8_t fVel = 100;
    if (midiSeqFindNoteAt(noteEdTrack, infoNote, curTick, fStart, fEnd, fVel)) {
      showNoteInfoBox = true;
      infoVel = fVel;
    }
  }
  char infoBoxStr[20] = "";
  int infoBoxX = 0, infoBoxW = 0;
  if (showNoteInfoBox) {
    char noteLbl[8];
    formatMidiNoteName(infoNote, noteLbl, sizeof(noteLbl));
    snprintf(infoBoxStr, sizeof(infoBoxStr), "%s, VEL%03u", noteLbl, (unsigned)infoVel);
    infoBoxW = max(SX(86), textWidth(infoBoxStr) + SX(12));
    infoBoxX = SCREEN_W - PX - SX(3) - infoBoxW;
  }

  for (int st = 0; st < PIANO_STRIP_COUNT; st++) {
    int stripHighNote = noteEdBaseNote + (PIANO_VIS_KEYS - 1 - st * PIANO_KEYS_PER_STRIP);
    int stripLowNote = stripHighNote - (PIANO_KEYS_PER_STRIP - 1);
    bool stripHasCursor = (noteEdCursorNote >= stripLowNote && noteEdCursorNote <= stripHighNote);
    int stripY0 = st * STRIP_H;
    int stripY1 = stripY0 + STRIP_H;
    bool stripInInfoBoxZone = (stripY1 > INFO_BOX_Y && stripY0 < INFO_BOX_Y + INFO_BOX_H);

    uint64_t hk = hashInit();
    hk = hashMix(hk, (uint32_t)st | ((uint32_t)noteEdTrack << 8) | ((uint32_t)(noteEdBaseNote & 0xFF) << 16) | ((uint32_t)zoom << 24));
    hk = hashMix(hk, scrollStep);
    hk = hashMix(hk, bpmKey | ((uint32_t)trackMuted << 24));
    hk = hashMix(hk, midiTracks[noteEdTrack].overviewVersion);
    if (stripHasCursor) {
      hk = hashMix(hk, (uint64_t)(uint32_t)noteEdCursorNote | ((uint64_t)noteEdPlacing << 16) |
                       ((uint64_t)(uint32_t)noteEdStartIdx32 << 32));
      if (noteEdPlacing) {
        hk = hashMix(hk, (uint64_t)noteEdIdx32);
      }
    }
    if (stripInInfoBoxZone && showNoteInfoBox) {
      hk = hashMix(hk, (uint64_t)infoNote | ((uint64_t)infoVel << 8) |
                       ((uint64_t)noteEdPlacing << 16) | (1ULL << 24));
    }
    for (int v = 0; v < visNoteCount; v++) {
      if (visNotes[v].note >= stripLowNote && visNotes[v].note <= stripHighNote) {
        hk = hashMix(hk, (uint64_t)visNotes[v].note |
                         ((uint64_t)visNotes[v].cursorHit << 8) |
                         ((uint64_t)(uint16_t)visNotes[v].x0 << 16) |
                         ((uint64_t)(uint16_t)visNotes[v].x1 << 32));
      }
    }

    if (!full && hk == prevPianoStripKey[st]) continue;
    prevPianoStripKey[st] = hk;

    Surface s(SCREEN_W, STRIP_H);

    // 1. Draw the Piano Keys & Grid Row Backgrounds in this strip
    for (int kIdx = 0; kIdx < PIANO_KEYS_PER_STRIP; kIdx++) {
      int ry = kIdx * pKeyH;
      int rowIdx = st * PIANO_KEYS_PER_STRIP + kIdx;
      int note = noteEdBaseNote + (PIANO_VIS_KEYS - 1 - rowIdx);
      if (note < 0 || note > 127) {
        s.fillRect(0, ry, SCREEN_W, pKeyH, BG);
        s.fillRect(0, ry, PX, pKeyH, COLOR_MIDI);
        s.fillRect(PIANO_W - PX, ry, PX, pKeyH, MID);
        s.fillRect(SCREEN_W - PX, ry, PX, pKeyH, COLOR_MIDI);
        continue;
      }
      int semi = note % 12;
      bool isBlack = (semi == 1 || semi == 3 || semi == 6 || semi == 8 || semi == 10);
      bool isCurNote = (note == noteEdCursorNote);

      // Left Piano Roll Key
      s.fillRect(0, ry, PX, pKeyH, COLOR_MIDI);
      s.fillRect(PIANO_W - PX, ry, PX, pKeyH, isCurNote ? INK : MID);
      uint16_t keyFg = BG;
      if (isCurNote) {
        uint16_t curKeyBg = noteEdPlacing ? COLOR_WARN : COLOR_MIDI;
        s.fillRect(PX, ry, PIANO_W - 2 * PX, pKeyH - 1, curKeyBg);
        keyFg = BG;
      } else if (isBlack) {
        int capW = PIANO_W - 2 * PX - SX(6);
        s.fillRect(PX, ry, capW, pKeyH - 1, BG);
        s.fillRect(PX + capW, ry, 1, pKeyH - 1, MID);
        s.fillRect(PX + capW + 1, ry, PIANO_W - 2 * PX - capW - 1, pKeyH - 1, DARK);
        keyFg = LIGHT;
      } else {
        uint16_t whiteBg = blend565(LIGHT, PANEL, 175);
        s.fillRect(PX, ry, PIANO_W - 2 * PX, pKeyH - 1, whiteBg);
        keyFg = BG;
      }
      s.fillRect(PX, ry + pKeyH - 1, PIANO_W - 2 * PX, 1, BG);

      char keyLbl[8];
      formatMidiNoteName((uint8_t)note, keyLbl, sizeof(keyLbl));
      int lblY = ry + max(0, (pKeyH - 1 - MINI_KEY_FONT_H) / 2);
      drawMiniKeyLabel(s, PX + SX(2), lblY, keyLbl, keyFg);

      // Right Timeline Grid Row Background
      uint16_t rowBg = isCurNote ? blend565(COLOR_MIDI, BG, 36) : (isBlack ? GREY_12 : BG);
      s.fillRect(GRID_X, ry, GRID_INNER_W, pKeyH - 1, rowBg);
      uint16_t divCol = (semi == 0) ? MID : DARK;
      s.fillRect(GRID_X, ry + pKeyH - 1, GRID_INNER_W, 1, divCol);
      s.fillRect(SCREEN_W - PX, ry, PX, pKeyH, COLOR_MIDI);
    }

    // 2. Darken pre-zero region (left of 00:00.000) and draw vertical musical grid lines
    constexpr int barLineW = SX(3);
    constexpr int beatLineW = SX(2);
    if (zeroCol > 0) {
      int preW = min(GRID_INNER_W, zeroCol);
      for (int py = 0; py < STRIP_H; py += 2) {
        for (int cx = (py & 1); cx < preW; cx += 2) {
          s.drawPixel(GRID_X + cx, py, BG);
        }
      }
      if (zeroCol < GRID_INNER_W) {
        s.fillRect(GRID_X + zeroCol, 0, min(barLineW, GRID_INNER_W - zeroCol), STRIP_H, MID);
      }
    }

    int lastDrawnGridCol = -9999;
    int barLineEndCol = -9999;
    int majorLineEndCol = -9999;
    for (uint32_t tk = firstGridTick; tk <= endTick; tk += gridStepTicks) {
      int64_t rel = (int64_t)tk - (int64_t)curTick;
      int col = PLAY_COL + (int)((rel * GRID_INNER_W) / windowTicks);
      if (col < 0 || col >= GRID_INNER_W || col == lastDrawnGridCol) continue;
      lastDrawnGridCol = col;
      int px = GRID_X + col;

      if (zoom == ZOOM_FULL || (tk % barTicks) == 0) {
        int c1 = min(GRID_INNER_W, col + barLineW);
        s.fillRect(px, 0, c1 - col, STRIP_H, MID);
        barLineEndCol = c1;
        majorLineEndCol = max(majorLineEndCol, c1);
      } else if ((tk % beatTicks) == 0) {
        int c0 = max(col, barLineEndCol);
        int c1 = min(GRID_INNER_W, col + beatLineW);
        if (c0 < c1) {
          s.fillRect(GRID_X + c0, 0, c1 - c0, STRIP_H, DARK);
        }
        majorLineEndCol = max(majorLineEndCol, c1);
      } else if (col >= majorLineEndCol) {
        if ((tk % eighthTicks) == 0) {
          for (int py = 0; py < STRIP_H; py += 2) {
            s.drawPixel(px, py, DARK);
          }
        } else {
          for (int kIdx = 0; kIdx < PIANO_KEYS_PER_STRIP; kIdx++) {
            int ry = kIdx * pKeyH;
            int rowIdx = st * PIANO_KEYS_PER_STRIP + kIdx;
            int note = noteEdBaseNote + (PIANO_VIS_KEYS - 1 - rowIdx);
            if (note == noteEdCursorNote) {
              s.drawPixel(px, ry + pKeyH / 2, DARK);
            }
          }
        }
      }
    }

    // 3. Draw Recorded Horizontal Rectangular Note Boxes
    for (int kIdx = 0; kIdx < PIANO_KEYS_PER_STRIP; kIdx++) {
      int ry = kIdx * pKeyH;
      int rowIdx = st * PIANO_KEYS_PER_STRIP + kIdx;
      int note = noteEdBaseNote + (PIANO_VIS_KEYS - 1 - rowIdx);
      if (note < 0 || note > 127) continue;
      int by = ry + 1;
      int bh = max(2, pKeyH - 2);

      for (int v = 0; v < visNoteCount; v++) {
        if (visNotes[v].note != (uint8_t)note) continue;
        int bx = GRID_X + visNotes[v].x0;
        int bw = max(2, (int)(visNotes[v].x1 - visNotes[v].x0));
        if (bx + bw > GRID_X + GRID_INNER_W) bw = GRID_X + GRID_INNER_W - bx;
        if (bw <= 0) continue;

        bool cursorInside = (visNotes[v].cursorHit != 0);
        uint16_t boxBorder = cursorInside ? INK : (trackMuted ? DIM : COLOR_MIDI);
        uint16_t boxFill = trackMuted ? DARK : blend565(COLOR_MIDI, BG, 168);

        s.fillRect(bx, by, bw, bh, boxBorder);
        if (bw > 2 && bh > 2) {
          s.fillRect(bx + 1, by + 1, bw - 2, bh - 2, boxFill);
        }
        s.fillRect(bx, by, 1, bh, cursorInside ? COLOR_WARN : INK);
      }

      // 4. Draw In-Progress Note Preview Box when defining note length (noteEdPlacing)
      if (noteEdPlacing && note == noteEdCursorNote) {
        uint32_t t0 = (uint32_t)(noteEdStartIdx32 * stepTicks);
        uint32_t t1 = (uint32_t)(max(noteEdStartIdx32 + 1LL, noteEdIdx32) * stepTicks);
        int64_t rel0 = (int64_t)t0 - (int64_t)curTick;
        int64_t rel1 = (int64_t)t1 - (int64_t)curTick;
        int px0 = PLAY_COL + (int)((rel0 * GRID_INNER_W) / windowTicks);
        int px1 = PLAY_COL + (int)((rel1 * GRID_INNER_W) / windowTicks);
        if (px1 <= px0) px1 = px0 + max(2, stepPxW);
        int cx0 = constrain(px0, 0, GRID_INNER_W - 1);
        int cx1 = constrain(px1, cx0 + 1, GRID_INNER_W);
        int bx = GRID_X + cx0;
        int bw = max(2, cx1 - cx0);
        s.fillRect(bx, by, bw, bh, INK);
        if (bw > 2 && bh > 2) {
          s.fillRect(bx + 1, by + 1, bw - 2, bh - 2, COLOR_WARN);
        }
      }
    }

    // 5. Center Playhead Line & 1/32nd Grid Cursor Cell
    s.fillRect(GRID_X + PLAY_COL, 0, 1, STRIP_H, MID);
    for (int kIdx = 0; kIdx < PIANO_KEYS_PER_STRIP; kIdx++) {
      int ry = kIdx * pKeyH;
      int rowIdx = st * PIANO_KEYS_PER_STRIP + kIdx;
      int note = noteEdBaseNote + (PIANO_VIS_KEYS - 1 - rowIdx);
      if (note < 0 || note > 127) {
        s.fillRect(PX, ry, PIANO_W - 2 * PX, pKeyH, BG);
        s.fillRect(GRID_X, ry, GRID_INNER_W, pKeyH, BG);
        continue;
      }
      if (note == noteEdCursorNote) {
        int cx = GRID_X + PLAY_COL;
        int curW = constrain(stepPxW, SX(3), GRID_INNER_W - PLAY_COL);
        uint16_t curCol = noteEdPlacing ? COLOR_WARN : INK;
        if (!noteEdPlacing) {
          s.fillRect(cx, ry, curW, 1, curCol);
          s.fillRect(cx, ry + pKeyH - 1, curW, 1, curCol);
          s.fillRect(cx, ry, 1, pKeyH, curCol);
          s.fillRect(cx + curW - 1, ry, 1, pKeyH, curCol);
        } else {
          s.fillRect(cx, ry, PX, pKeyH, INK);
        }
      }
    }

    // 6. Bottom-Right Note & Velocity Info Box ("[note name], VEL###")
    if (stripInInfoBoxZone && showNoteInfoBox) {
      int relY = INFO_BOX_Y - stripY0;
      uint16_t boxBorder = noteEdPlacing ? COLOR_WARN : COLOR_MIDI;
      uint16_t boxFg = noteEdPlacing ? COLOR_WARN : INK;
      s.fillRect(infoBoxX, relY, infoBoxW, INFO_BOX_H, boxBorder);
      s.fillRect(infoBoxX + 1, relY + 1, infoBoxW - 2, INFO_BOX_H - 2, PANEL);
      int tx = infoBoxX + max(2, (infoBoxW - textWidth(infoBoxStr)) / 2);
      s.drawText(tx, relY + TY(INFO_BOX_H), infoBoxStr, boxFg);
    }

    // 7. Top & Bottom Outer Overlay Frame Borders
    if (st == 0) {
      s.fillRect(0, 0, SCREEN_W, 1, COLOR_MIDI);
    }
    if (st == PIANO_STRIP_COUNT - 1) {
      s.fillRect(0, STRIP_H - 1, SCREEN_W, 1, COLOR_MIDI);
    }

    s.blit(0, PIANO_TOP_Y + st * STRIP_H);
    keepStreaming();
  }
}

// ui.cpp — Flicker-free UI shell & shared widgets for M8 Capstan
//
// Core frame lifecycle (uiRefresh, screen blanking, toast notifications),
// shared top header bar, transport bar, modal on-screen keyboard, and
// dispatch to per-page modules (src/page_*.cpp via PageOps).
#include "pages.h"
#include "page_audio.h"
#include "page_midi.h"
#include "page_song_settings.h"
#include "page_main.h"
#include "page_system_settings.h"
#include "page_instruments.h"
#include "page_instrument_type.h"
#include "page_instrument_edit.h"
#include "page_master.h"
#include "track_edit.h"
#include "inst_sampler.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

uint32_t uiGeneration = 1;

static uint32_t lastDrawnGen = 0;
static Page lastDrawnPage = Page::Audio;
static bool uiImmediate = true;
static bool uiCurFrameImmediate = true;
static uint32_t uiFramePhase = 0;
static uint32_t lastActivityMs = 0;
static bool screenBlanked = false;

static char toastMsg[36] = "";
static uint32_t toastUntilMs = 0;

// Shared offscreen compositing buffer in fast DTCM (RAM1): fits up to 308x24 (7,392 px) or 184x39 (7,176 px)
uint16_t cellBuf[CELL_BUF_MAX];

// Shared header, transport, and modal keyboard state caches
static uint64_t prevHeaderKey = 0;
static uint64_t prevHeaderTagKey = 0;
static TransportMode lastHeaderTm = TransportMode::Stopped;
static bool lastHeaderArmed = false;
static int lastBatteryPct = -2;
static bool lastBatteryChg = false;
static uint64_t prevStripKey = 0;
static uint64_t prevInOutMeterKey = 0;
static float trSmoothInL = 0.0f, trSmoothInR = 0.0f;
static float trSmoothOutL = 0.0f, trSmoothOutR = 0.0f;
static uint64_t prevBtnKey[TR_CELL_COUNT] = {};
static uint64_t prevKbBannerKey = 0;
static uint64_t prevKbKey[KB_GRID_ROWS][KB_GRID_COLS] = {};
static uint64_t prevKbBtnKey[2] = {};
static uint64_t prevResBannerKey = 0;
static uint64_t prevResRowKey[2] = {};
static uint64_t prevClearTrackRowKey[3] = {};

bool uiIsImmediateFrame() {
  return uiCurFrameImmediate;
}

bool uiTierDue(uint8_t periodTicks) {
  if (uiCurFrameImmediate || periodTicks <= 1) return true;
  return (uiFramePhase % periodTicks) == 0;
}

uint8_t uiTimelineRrSlot() {
  return (uint8_t)(uiFramePhase & 1u);
}

FLASHMEM void computeTimelineFrameCtx(TimelineFrameCtx &ctx, int innerW, int playCol) {
  ctx.curFrame = (int64_t)transport.tapeFrame;
  ctx.zoom = timelineZoom;
  ctx.bpmKey = (uint32_t)(bpm * 10.0f + 0.5f);
  ctx.winFrames = timelineWindowFrames(ctx.zoom, bpm);
  ctx.halfWinFrames = ctx.winFrames / 2u;
  ctx.scrollStep = (uint32_t)(((uint64_t)transport.tapeFrame * (uint64_t)innerW) / max(1u, ctx.winFrames));
  ctx.curTick = framesToMidiTicks((uint32_t)ctx.curFrame, bpm);
  ctx.windowTicks = max((int64_t)1, (int64_t)framesToMidiTicks(ctx.winFrames, bpm));
  int64_t winStartF = max((int64_t)0, ctx.curFrame - (int64_t)ctx.halfWinFrames);
  int64_t winEndF = max((int64_t)0, ctx.curFrame + (int64_t)ctx.halfWinFrames);
  ctx.startTick = framesToMidiTicks((uint32_t)winStartF, bpm);
  ctx.endTick = framesToMidiTicks((uint32_t)winEndF, bpm);
  ctx.punchOn = isPunchOrLoopWindowActive();
  if (ctx.punchOn) {
    ctx.punchX0 = timelineFrameToCol((int64_t)transport.punchInFrame, ctx.curFrame, playCol, innerW, ctx.winFrames);
    ctx.punchX1 = timelineFrameToCol((int64_t)transport.punchOutFrame, ctx.curFrame, playCol, innerW, ctx.winFrames);
  } else {
    ctx.punchX0 = -9999;
    ctx.punchX1 = -9999;
  }
}

FLASHMEM const PageOps &pageOpsFor(Page p) {
  switch (p) {
    case Page::Audio:           return pageAudioOps();
    case Page::Midi:            return pageMidiOps();
    case Page::SongSettings: return pageSongSettingsOps();
    case Page::Main:            return pageMainOps();
    case Page::SystemSettings:  return pageSystemSettingsOps();
    case Page::Instruments:     return pageInstrumentsOps();
    case Page::InstrumentType:  return pageInstrumentTypeOps();
    case Page::InstrumentEdit:  return pageInstrumentEditOps();
    case Page::Master:          return pageMasterOps();
  }
  return pageAudioOps();
}

FLASHMEM void invalidateAllPagesCache() {
  for (int i = 0; i < NUM_UI_PAGES; i++) {
    const PageOps &ops = pageOpsFor((Page)i);
    if (ops.invalidateCache) {
      ops.invalidateCache();
    }
  }
}

FLASHMEM static void invalidateUiCache() {
  prevHeaderKey = 0;
  prevHeaderTagKey = 0;
  prevStripKey = 0;
  prevInOutMeterKey = 0;
  memset(prevBtnKey, 0, sizeof(prevBtnKey));
  prevKbBannerKey = 0;
  memset(prevKbKey, 0, sizeof(prevKbKey));
  memset(prevKbBtnKey, 0, sizeof(prevKbBtnKey));
  prevResBannerKey = 0;
  memset(prevResRowKey, 0, sizeof(prevResRowKey));
  memset(prevClearTrackRowKey, 0, sizeof(prevClearTrackRowKey));
  invalidateTrackEditOverlayCache();
  invalidateAllPagesCache();
}

FLASHMEM void uiForceRedraw() {
  uiImmediate = true;
}

FLASHMEM void noteUserActivity() {
  lastActivityMs = millis();
  if (screenBlanked) {
    screenBlanked = false;
    uiGeneration++;
    uiImmediate = true;
    board.setBacklight(brightnessToPwm(systemSettings().brightness));
  }
}

FLASHMEM bool isScreenBlanked() {
  return screenBlanked;
}

FLASHMEM void uiShowToast(const char *msg, uint32_t durationMs) {
  snprintf(toastMsg, sizeof(toastMsg), "%s", msg ? msg : "");
  toastUntilMs = millis() + durationMs;
  uiImmediate = true;
}

// Format stereo frame index as MM:SS.mmm (up to 666 seconds = 11:06.000)
FLASHMEM void formatTime(uint32_t frames, char *out, size_t sz) {
  uint32_t totalMs = (uint32_t)(((uint64_t)frames * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
  uint32_t totalSec = totalMs / 1000;
  uint32_t ms = totalMs % 1000;
  uint32_t mm = totalSec / 60;
  uint32_t ss = totalSec % 60;
  snprintf(out, sz, "%02lu:%02lu.%03lu", (unsigned long)mm, (unsigned long)ss, (unsigned long)ms);
}

// Format stereo frame index as BBB.B.E.S.T (Bars.Beats.8ths.16ths.32nds in 4/4)
// where Beats = 1..4, 8ths = 1..8, 16ths = 1..16, 32nds = 1..32 within each bar.
FLASHMEM void formatBarBeat(uint32_t frames, float curBpm, char *out, size_t sz) {
  int64_t total32nds = framesTo32nds(frames, curBpm);
  constexpr int32_t SUB32_PER_BAR = BEATS_PER_BAR * 8; // 32 thirty-second notes per 4/4 bar
  int32_t bar = (int32_t)(total32nds / SUB32_PER_BAR) + 1;
  int32_t sub32InBar = (int32_t)(total32nds % SUB32_PER_BAR); // 0..31
  int32_t beatInBar = (sub32InBar / 8) + 1;                   // 1..4  (1/4 bar)
  int32_t eighthInBar = (sub32InBar / 4) + 1;                 // 1..8  (1/8 bar)
  int32_t sixteenthInBar = (sub32InBar / 2) + 1;              // 1..16 (1/16 bar)
  int32_t thirtySecondInBar = sub32InBar + 1;                 // 1..32 (1/32 bar)
  snprintf(out, sz, "%03ld.%ld.%ld.%ld.%ld",
           (long)bar, (long)beatInBar, (long)eighthInBar, (long)sixteenthInBar, (long)thirtySecondInBar);
}

FLASHMEM void drawBatteryIcon(bool all, uint16_t ink, uint16_t ground) {
  if (!all && !uiTierDue(6)) return;
  int pct = board.batteryPercent();
  bool chg = board.batteryCharging();
  if (!all && pct == lastBatteryPct && chg == lastBatteryChg) return;
  lastBatteryPct = pct;
  lastBatteryChg = chg;

  auto &t = board.tft();
  const int bw = SX(16), bh = SY(8);
  const int padX = SX(2), padY = SY(2);
  const int x = SCREEN_W - SX(22), y = SY(7);
  t.fillRect(x - padX, y - padY, SX(22), SY(12), ground);
  uint16_t c = (pct >= 0 && pct <= 15 && !chg) ? COLOR_REC : ink;
  t.drawRect(x, y, bw, bh, c);
  t.fillRect(x + bw, y + padY, padX, bh - 2 * padY, c);
  const int maxFill = bw - 2 * padX;
  int fillW = pct < 0 ? SX(10) : constrain(pct * maxFill / 100, 1, maxFill);
  t.fillRect(x + padX, y + padY, fillW, bh - 2 * padY, chg ? COLOR_PLAY : c);
}

// Draw the 3-row M8 spatial page map at (x, y)
// Row 1: [SYSTEM] [SONG] [MAIN]
// Row 2: [AUDIO] [MIDI] [INSTR] [TYPE/EDIT]
// Row 3: [MASTER]
FLASHMEM static void drawPageMap(int x, int y, Page active) {
  auto &t = board.tft();
  t.fillRect(x, y, SX(28), SY(18), PANEL);
  const int nw = SX(5), nh = SY(4);
  auto node = [&](int nx, int ny, bool isActive) {
    t.fillRect(nx, ny, nw, nh, isActive ? ACCENT : DIM);
  };
  // Row 1 (top): SYSTEM, SONG, MAIN
  node(x + SX(1),  y + SY(1),  active == Page::SystemSettings);
  node(x + SX(8),  y + SY(1),  active == Page::SongSettings);
  node(x + SX(15), y + SY(1),  active == Page::Main);

  // Row 2 (middle): AUDIO, MIDI, INST, TYPE/EDIT
  node(x + SX(1),  y + SY(7),  active == Page::Audio);
  node(x + SX(8),  y + SY(7),  active == Page::Midi);
  node(x + SX(15), y + SY(7),  active == Page::Instruments);
  node(x + SX(22), y + SY(7),  active == Page::InstrumentType || active == Page::InstrumentEdit);

  // Row 3 (bottom): MASTER FX
  node(x + SX(1),  y + SY(13), active == Page::Master);
}

// Top Header Bar: Page title, Playhead MM:SS.mmm + Bar.Beat, Page map, Battery
FLASHMEM static void drawHeaderBar(bool full) {
  auto &t = board.tft();
  Page pg = currentPage();
  const PageOps &ops = pageOpsFor(pg);
  const char *title = (ops.headerTitle) ? ops.headerTitle() : "AUDIO";
  const bool teActive = (trackEditIsActive() && (pg == trackEditPage())) ||
                        (pg == Page::Midi && isMidiNoteEditorOpen());
  uint64_t tk = hashStr(hashMix(hashInit(), (uint64_t)pg | ((uint64_t)teActive << 8)), title);
  const int hdrH = SY(24);
  const int boxH = SY(20);
  const int tagW = max(SX(62), textWidth(title) + SX(8));
  const int midX = SX(2) + tagW + SX(2);
  const int midW = SX(264) - midX;
  const int boxY = SY(2);

  if (full) {
    t.fillRect(0, 0, SCREEN_W, hdrH, PANEL);
    t.fillRect(0, hdrH - 1, SCREEN_W, 1, DARK);
    drawPageMap(SX(266), SY(3), pg);
    drawBatteryIcon(true, LIGHT, PANEL);
    prevHeaderKey = 0;
    prevHeaderTagKey = 0;
  } else {
    drawBatteryIcon(false, LIGHT, PANEL);
  }

  // Toast notification or live playhead counter in the middle of the header bar
  bool hasToast = toastMsg[0] && (int32_t)(toastUntilMs - millis()) > 0;
  if (!hasToast && toastMsg[0]) {
    toastMsg[0] = '\0';
    prevHeaderKey = 0;
    prevHeaderTagKey = 0;
    Surface clearS(midW, boxH);
    clearS.fillRect(0, 0, midW, boxH, PANEL);
    clearS.blit(midX, boxY);
    drawPageMap(SX(266), SY(3), pg);
    drawBatteryIcon(true, LIGHT, PANEL);
  }

  if (full || tk != prevHeaderTagKey) {
    prevHeaderTagKey = tk;
    Surface s(tagW, boxH);
    s.fillRect(0, 0, tagW, boxH, PANEL);
    s.drawText(SX(4), TY(boxH), title, teActive ? COLOR_WARN : INK);
    s.blit(SX(2), boxY);
  }

  if (hasToast) {
    uint64_t hk = hashMix(hashStr(hashInit(), toastMsg), 1);
    if (hk != prevHeaderKey) {
      prevHeaderKey = hk;
      Surface s(midW, boxH);
      s.fillRect(0, 0, midW, boxH, PANEL);
      int msgW = textWidth(toastMsg);
      int pillW = constrain(msgW + SX(12), SX(64), midW - SX(4));
      int pillX = (midW - pillW) / 2;
      int pillY = SY(2);
      int pillH = boxH - 2 * pillY;
      s.fillRect(pillX, pillY, pillW, pillH, COLOR_WARN);
      int tx = pillX + max(2, (pillW - msgW) / 2);
      s.drawText(tx, TY(boxH), toastMsg, BG);
      s.blit(midX, boxY);
    }
    return;
  }

  TransportMode tm = transport.mode;
  const bool armedNow = anyTrackArmed();
  const bool hdrSel = isHeaderRowSelected();
  const int selField = hdrSel ? constrain(headerCell, 0, HEADER_CELL_COUNT - 1) : -1;
  const int optHlField = hdrSel ? -1 : inputOptHeaderHighlightCell();

  // During steady-state playback, update the live MM:SS.mmm & BBB.B.E.S.T header counter at 10 Hz
  if (!full && prevHeaderKey != 0 && tm == TransportMode::Running &&
      lastHeaderTm == TransportMode::Running && armedNow == lastHeaderArmed &&
      !hdrSel && optHlField < 0 && !uiTierDue(3)) {
    return;
  }
  lastHeaderTm = tm;
  lastHeaderArmed = armedNow;

  char timeStr[16], barStr[20], combined[40];
  uint16_t c = LIGHT;
  bool isStatusMsg = false;
  uint32_t tf = transport.tapeFrame;
  if (tm == TransportMode::CountIn) {
    snprintf(combined, sizeof(combined), "COUNT-IN %ld", (long)(transport.countInRemainingBeats + 1));
    c = COLOR_WARN;
    isStatusMsg = true;
  } else if (tm == TransportMode::WaitThreshold) {
    snprintf(combined, sizeof(combined), "%s", transport.waitAudioThresh ? "WAIT AUDIO THR" : "WAIT MIDI IN");
    c = ((millis() / 250) % 2) ? COLOR_WARN : DIM;
    isStatusMsg = true;
  } else {
    formatTime(tf, timeStr, sizeof(timeStr));
    formatBarBeat(tf, bpm, barStr, sizeof(barStr));
    snprintf(combined, sizeof(combined), "%s %s", timeStr, barStr);
    c = (tm == TransportMode::Running)
            ? (armedNow ? COLOR_REC : COLOR_PLAY)
            : LIGHT;
  }

  const uint16_t pageAccent = (pg == Page::Midi) ? COLOR_MIDI : ACCENT;

  uint64_t hk = hashMix(hashStr(hashInit(), combined), c);
  hk = hashMix(hk, (uint64_t)(selField + 2) | ((uint64_t)(optHlField + 2) << 4) | ((uint64_t)pageAccent << 8));
  if (hk != prevHeaderKey) {
    prevHeaderKey = hk;
    Surface s(midW, boxH);
    s.fillRect(0, 0, midW, boxH, PANEL);
    int cw = textWidth(combined);
    int refW = isStatusMsg ? cw : textWidth("00:00.000 001.4.8.16.32");
    int targetX = (SX(66) + SX(198) / 2) - midX - refW / 2;
    int tx = constrain(targetX, SX(2), max(SX(2), midW - cw - SX(2)));

    if (isStatusMsg || (selField < 0 && optHlField < 0)) {
      s.drawText(tx, TY(boxH), combined, c);
    } else {
      uint32_t totalMs = (uint32_t)(((uint64_t)tf * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
      uint32_t totalSec = totalMs / 1000;
      uint32_t ms = totalMs % 1000;
      uint32_t mm = totalSec / 60;
      uint32_t ss = totalSec % 60;

      int64_t total32nds = framesTo32nds(tf, bpm);
      constexpr int32_t SUB32_PER_BAR = BEATS_PER_BAR * 8;
      int32_t bar = (int32_t)(total32nds / SUB32_PER_BAR) + 1;
      int32_t sub32InBar = (int32_t)(total32nds % SUB32_PER_BAR);
      int32_t beatInBar = (sub32InBar / 8) + 1;
      int32_t eighthInBar = (sub32InBar / 4) + 1;
      int32_t sixteenthInBar = (sub32InBar / 2) + 1;
      int32_t thirtySecondInBar = sub32InBar + 1;

      char tok[HEADER_CELL_COUNT][16];
      snprintf(tok[0], sizeof(tok[0]), "%02lu", (unsigned long)mm);
      snprintf(tok[1], sizeof(tok[1]), "%02lu", (unsigned long)ss);
      snprintf(tok[2], sizeof(tok[2]), "%03lu", (unsigned long)ms);
      snprintf(tok[3], sizeof(tok[3]), "%03ld", (long)bar);
      snprintf(tok[4], sizeof(tok[4]), "%ld", (long)beatInBar);
      snprintf(tok[5], sizeof(tok[5]), "%ld", (long)eighthInBar);
      snprintf(tok[6], sizeof(tok[6]), "%ld", (long)sixteenthInBar);
      snprintf(tok[7], sizeof(tok[7]), "%ld", (long)thirtySecondInBar);

      static const char *const SEP[HEADER_CELL_COUNT] = {":", ".", " ", ".", ".", ".", ".", ""};
      int curX = tx;
      int ty = TY(boxH);
      int pillY = max(0, ty - SY(2));
      int pillH = min(boxH - pillY, FONT_H + SY(4));

      for (int i = 0; i < HEADER_CELL_COUNT; i++) {
        int tw = textWidth(tok[i]);
        if (i == selField) {
          s.fillRect(max(0, curX - PX), pillY, tw + 2 * PX, pillH, pageAccent);
          s.drawText(curX, ty, tok[i], BG);
        } else if (i == optHlField) {
          s.fillRect(max(0, curX - PX), pillY, tw + 2 * PX, pillH, LIGHT);
          s.drawText(curX, ty, tok[i], BG);
        } else {
          s.drawText(curX, ty, tok[i], c);
        }
        curX += tw + FONT_GAP;
        if (SEP[i][0] != '\0') {
          s.drawText(curX, ty, SEP[i], c);
          curX += textWidth(SEP[i]) + FONT_GAP;
        }
      }
    }
    s.blit(midX, boxY);
  }
}

// Shared Track Arm Badge Cell (Cell 0 on Audio & MIDI Pages)
FLASHMEM void drawTrackArmBadgeCell(int x, int y, int w, int h, int trackIdx,
                           ArmMode armMode, bool muted, bool sel,
                           uint16_t pageAccent, uint64_t &cacheKey, bool full,
                           const char *armTag) {
  uint16_t badgeBg = sel ? DARK : PANEL;
  uint16_t badgeFg = sel ? pageAccent : (muted ? DIM : LIGHT);
  uint16_t badgeBorder = sel ? pageAccent : MID;
  uint16_t circleCol = 0;

  if (armMode == ArmMode::Record) {
    badgeBg = COLOR_REC_DIM;
    badgeFg = sel ? INK : COLOR_REC;
    badgeBorder = sel ? pageAccent : COLOR_REC;
    circleCol = COLOR_REC;
  } else if (armMode == ArmMode::Overdub) {
    badgeBg = COLOR_DUB_DIM;
    badgeFg = sel ? INK : COLOR_OVERDUB;
    badgeBorder = sel ? pageAccent : COLOR_OVERDUB;
    circleCol = COLOR_OVERDUB;
  }

  uint64_t k = hashMix(hashInit(), (uint64_t)badgeBg | ((uint64_t)badgeFg << 16) |
                                       ((uint64_t)badgeBorder << 32) | ((uint64_t)circleCol << 48));
  k = hashMix(k, (uint32_t)trackIdx);
  if (armTag && armTag[0]) {
    k = hashStr(k, armTag);
  }
  if (!full && k == cacheKey) return;
  cacheKey = k;

  char badge[12];
  char prefix = (pageAccent == COLOR_MIDI) ? 'M' : 'A';
  snprintf(badge, sizeof(badge), "%c%u", prefix, (unsigned)constrain(trackIdx + 1, 1, 99));
  Surface s(w, h);
  s.drawBox(badgeBg, badgeBorder);
  int tx = (w - textWidth(badge)) / 2;
  if (circleCol != 0) {
    if (armTag && armTag[0]) {
      const int r = SY(3);
      const int diam = 2 * r + 1;
      const int gap = SX(2);
      const int tagW = textWidth(armTag);
      const int totW = diam + gap + tagW;
      const int startX = max(PX + 1, (w - totW) / 2);
      const int topY = SY(5);
      s.drawCircle(startX + r, topY + FONT_H / 2, r, circleCol);
      s.drawText(startX + diam + gap, topY, armTag, badgeFg);
    } else {
      s.drawCircle(w / 2, SY(10), SY(3), circleCol);
    }
    s.drawText(tx, SY(21), badge, badgeFg);
  } else {
    s.drawText(tx, TY(h), badge, badgeFg);
  }
  s.blit(x, y);
}

// Shared Track Trigger Badge Cell (Cell 1 on Audio & MIDI Pages)
FLASHMEM void drawTrackTrigBadgeCell(int x, int y, int w, int h,
                            TrigMode trigMode, bool sel,
                            uint16_t pageAccent, uint64_t &cacheKey, bool full) {
  const bool isRes = (trigMode == TrigMode::Resample);
  const bool trigOn = (trigMode == TrigMode::Threshold) || isRes;
  const char *lbl = isRes ? "RES" : "TRG";
  uint16_t bg = trigOn ? pageAccent : (sel ? DARK : PANEL);
  uint16_t border = trigOn ? (sel ? INK : pageAccent) : (sel ? pageAccent : MID);
  uint16_t fg = trigOn ? BG : (sel ? LIGHT : DIM);

  uint64_t k = hashMix(hashInit(), (uint64_t)bg | ((uint64_t)border << 16) |
                                       ((uint64_t)fg << 32) | ((uint64_t)trigMode << 48) |
                                       ((uint64_t)sel << 50));
  if (!full && k == cacheKey) return;
  cacheKey = k;

  Surface s(w, h);
  if (trigOn && sel) {
    s.drawBox(BG, border);
    s.fillRect(2 * PX, 2 * PX, w - 4 * PX, h - 4 * PX, bg);
  } else {
    s.drawBox(bg, border);
  }
  int tw = textWidth(lbl);
  s.drawText((w - tw) / 2, TY(h), lbl, fg);
  s.blit(x, y);
}

// Shared Two-Line Labeled Parameter Box (Audio VOL/PAN, MIDI IN/OUT, Master FX/VOL)
FLASHMEM void drawTwoLineParamBox(int x, int y, int w, int h,
                         const char *label, const char *valStr,
                         bool sel, bool active, uint16_t pageAccent,
                         int lblY, int valY, uint64_t &cacheKey, bool full) {
  uint16_t bg = sel ? DARK : PANEL;
  uint16_t border = sel ? pageAccent : MID;
  uint16_t lblCol = sel ? INK : LIGHT;
  uint16_t valCol = sel ? pageAccent : (active ? LIGHT : DIM);

  uint64_t k = hashStr(hashInit(), label);
  k = hashStr(k, valStr);
  k = hashMix(k, (uint64_t)bg | ((uint64_t)border << 16) |
                     ((uint64_t)lblCol << 32) | ((uint64_t)valCol << 48));
  if (!full && k == cacheKey) return;
  cacheKey = k;

  Surface s(w, h);
  s.drawBox(bg, border);
  int lw = textWidth(label);
  s.drawText((w - lw) / 2, lblY, label, lblCol);
  int vw = textWidth(valStr);
  s.drawText((w - vw) / 2, valY, valStr, valCol);
  s.blit(x, y);
}

// Shared Settings Table Row (Song Settings & System Settings Pages)
FLASHMEM void drawSettingsTableRow(int y, uint32_t rowId, const char *label, const char *valStr,
                          bool sel, bool isOff, bool isPill,
                          uint64_t &cacheKey, bool full) {
  const int rw = SX(308), rh = SY(24);
  uint16_t vColor = sel ? ACCENT : (isOff ? DIM : LIGHT);
  uint16_t bg = sel ? DARK : PANEL;
  uint16_t border = sel ? ACCENT : MID;
  uint16_t barCol = DIM;

  uint64_t rk = hashStr(hashInit(), label);
  rk = hashStr(rk, valStr);
  rk = hashMix(rk, (uint64_t)rowId | ((uint64_t)bg << 8) | ((uint64_t)border << 24) |
                       ((uint64_t)vColor << 40) | ((uint64_t)sel << 56) |
                       ((uint64_t)barCol << 57) | ((uint64_t)isPill << 62));
  if (!full && rk == cacheKey) return;
  cacheKey = rk;

  Surface s(rw, rh);
  s.drawBox(bg, border);
  s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);
  s.drawText(SX(10), TY(rh), label, sel ? INK : LIGHT);

  if (isPill) {
    const int pw = SX(80), ph = SY(16);
    const int px = rw - SX(6) - pw, py = (rh - ph) / 2;
    uint16_t pBg = sel ? ACCENT : PANEL;
    uint16_t pFg = sel ? BG : LIGHT;
    uint16_t pBd = sel ? INK : MID;
    s.drawPill(px, py, pw, ph, valStr, pFg, pBg, pBd);
  } else {
    int vw = textWidth(valStr);
    s.drawText(rw - SX(8) - vw, TY(rh), valStr, vColor);
  }
  s.blit(SX(6), y);
}

// Draw a clockwise circular loop arrow icon centered at (cx, cy)
FLASHMEM static void drawLoopArrowIcon(Surface &s, int cx, int cy, uint16_t color) {
  const int rOut = SY(6);       // 6 on Model:01, 8 on Model:02
  const int rIn = rOut - PX;    // 4 on Model:01, 6 on Model:02
  const int rMid = rOut - 1;    // 5 on Model:01, 7 on Model:02
  const int rOut2 = rOut * rOut + rOut / 2;
  const int rIn2 = rIn * rIn - rIn / 2;
  const int gapMaxY = (rOut >= 8) ? 3 : 2;

  // 1. Circular arc (~300 degrees), leaving a gap on the lower-right below the arrowhead
  for (int dy = -rOut; dy <= rOut; dy++) {
    for (int dx = -rOut; dx <= rOut; dx++) {
      int d2 = dx * dx + dy * dy;
      if (d2 >= rIn2 && d2 <= rOut2) {
        if (dx > 0 && dy >= -1 && dy <= gapMaxY) continue;
        s.drawPixel(cx + dx, cy + dy, color);
      }
    }
  }

  // 2. Clockwise downward-pointing triangular arrowhead on the right side of the ring
  const int arrowRows = (rOut >= 8) ? 4 : 3;
  for (int row = 0; row < arrowRows; row++) {
    int py = cy - row;
    int halfW = row;
    for (int dx = -halfW; dx <= halfW; dx++) {
      s.drawPixel(cx + rMid + dx, py, color);
    }
  }
}

// Shared Bottom Transport & Locator Bar for Page::Audio, Page::Midi, and Page::Master
FLASHMEM void drawTransportBar(bool isSelectedRow, int selCell, bool full) {
  auto &t = board.tft();
  if (full) {
    t.fillRect(0, SY(194), SCREEN_W, 1, DARK);
  }

  // 1. Interactive Information Bar (5 Hz tier in steady-state) + Live stereo input/output meter strip (30 Hz)
  const SongSettings &ps = songSettings();
  const Page pg = currentPage();
  const bool isMidiPage = (pg == Page::Midi);
  const uint16_t pageAccent = isMidiPage ? COLOR_MIDI : ACCENT;
  const int stripH = SY(16);
  const int infoStripW = SX(244);
  const int meterStripX = infoStripW;
  const int meterStripW = SCREEN_W - meterStripX;

  if (full || prevStripKey == 0 || uiTierDue(6)) {
    const bool winOn = isPunchOrLoopWindowActive();
    const bool isInfoRow = (pg == Page::Midi)   ? (midiRow == INFO_ROW)
                         : (pg == Page::Audio)  ? (audioRow == INFO_ROW)
                         : (pg == Page::Master) ? (masterRow == MASTER_ROW_INFO)
                                                : false;
    const int maxInfoCell = activeInfoCellCount();
    const int infoSelCell = isInfoRow ? constrain(selCell, 0, maxInfoCell - 1) : -1;
    const bool barBeatMode = isPunchBarBeatMode();

    char cellTxt[INFO_CELL_COUNT][24];
    bool cellDim[INFO_CELL_COUNT] = {false, false, false};

    int bpmTenths = (int)lrintf(bpm * 10.0f);
    snprintf(cellTxt[INFO_CELL_BPM], sizeof(cellTxt[INFO_CELL_BPM]), "%3d.%dBPM",
             bpmTenths / 10, bpmTenths % 10);

    if (winOn) {
      char inT[20], outT[20];
      if (barBeatMode) {
        formatBarBeat(ps.punchInFrame, bpm, inT, sizeof(inT));
        formatBarBeat(ps.punchOutFrame, bpm, outT, sizeof(outT));
      } else {
        formatTime(ps.punchInFrame, inT, sizeof(inT));
        formatTime(ps.punchOutFrame, outT, sizeof(outT));
      }
      snprintf(cellTxt[INFO_CELL_SLOT1], sizeof(cellTxt[INFO_CELL_SLOT1]), "IN:%s", inT);
      snprintf(cellTxt[INFO_CELL_SLOT2], sizeof(cellTxt[INFO_CELL_SLOT2]), "OUT:%s", outT);
    } else {
      const char *met = (ps.metroMode == MetroMode::AlwaysOn) ? "MET:ON"
                      : (ps.metroMode == MetroMode::RecOnly)  ? "MET:REC" : "MET:OFF";
      snprintf(cellTxt[INFO_CELL_SLOT1], sizeof(cellTxt[INFO_CELL_SLOT1]), "%s", met);
      cellDim[INFO_CELL_SLOT1] = (ps.metroMode == MetroMode::Off);

      int countInBars = (int)ps.countInBeats / 4;
      snprintf(cellTxt[INFO_CELL_SLOT2], sizeof(cellTxt[INFO_CELL_SLOT2]), "CIN:%dB", countInBars);
      cellDim[INFO_CELL_SLOT2] = (countInBars == 0);
    }

    uint64_t sk = hashInit();
    for (int c = 0; c < INFO_CELL_COUNT; c++) {
      sk = hashStr(sk, cellTxt[c]);
      sk = hashMix(sk, cellDim[c] ? 1 : 0);
    }
    sk = hashMix(sk, (uint64_t)(infoSelCell + 2) | ((uint64_t)pageAccent << 8) |
                     ((uint64_t)winOn << 24) | ((uint64_t)barBeatMode << 25));

    if (full || sk != prevStripKey) {
      prevStripKey = sk;
      Surface s(infoStripW, stripH);
      s.fillRect(0, 0, infoStripW, stripH, BG);

      if (winOn) {
        static const int punchX[2] = {SX(2), SX(123)};
        static const int punchW[2] = {SX(119), SX(119)};
        static const char *const PUNCH_PREFIX[2] = {"IN:", "OUT:"};
        uint32_t punchFrames[2] = {ps.punchInFrame, ps.punchOutFrame};
        const int cellsPerBox = barBeatMode ? 5 : 3;

        for (int b = 0; b < 2; b++) {
          int baseCell = b * cellsPerBox;
          bool boxSel = (infoSelCell >= baseCell && infoSelCell < baseCell + cellsPerBox);
          int subSel = boxSel ? (infoSelCell - baseCell) : -1;
          int cx = punchX[b], cw = punchW[b];
          uint16_t bg = boxSel ? DARK : PANEL;
          uint16_t border = boxSel ? pageAccent : MID;
          uint16_t fg = boxSel ? INK : LIGHT;

          s.fillRect(cx, 0, cw, stripH, border);
          s.fillRect(cx + 1, 1, cw - 2, stripH - 2, bg);

          char tok[5][16];
          static const char *const TIME_SEP[3] = {":", ".", ""};
          static const char *const BAR_SEP[5] = {".", ".", ".", ".", ""};
          const char *const *sep = barBeatMode ? BAR_SEP : TIME_SEP;

          if (barBeatMode) {
            int64_t total32nds = framesTo32nds(punchFrames[b], bpm);
            constexpr int32_t SUB32_PER_BAR = BEATS_PER_BAR * 8;
            int32_t bar = (int32_t)(total32nds / SUB32_PER_BAR) + 1;
            int32_t sub32InBar = (int32_t)(total32nds % SUB32_PER_BAR);
            int32_t beatInBar = (sub32InBar / 8) + 1;
            int32_t eighthInBar = (sub32InBar / 4) + 1;
            int32_t sixteenthInBar = (sub32InBar / 2) + 1;
            int32_t thirtySecondInBar = sub32InBar + 1;

            snprintf(tok[0], sizeof(tok[0]), "%03ld", (long)bar);
            snprintf(tok[1], sizeof(tok[1]), "%ld", (long)beatInBar);
            snprintf(tok[2], sizeof(tok[2]), "%ld", (long)eighthInBar);
            snprintf(tok[3], sizeof(tok[3]), "%ld", (long)sixteenthInBar);
            snprintf(tok[4], sizeof(tok[4]), "%ld", (long)thirtySecondInBar);
          } else {
            uint32_t totalMs = (uint32_t)(((uint64_t)punchFrames[b] * 1000 + SAMPLE_RATE_HZ / 2) / SAMPLE_RATE_HZ);
            uint32_t totalSec = totalMs / 1000;
            uint32_t ms = totalMs % 1000;
            uint32_t mm = totalSec / 60;
            uint32_t ss = totalSec % 60;

            snprintf(tok[0], sizeof(tok[0]), "%02lu", (unsigned long)mm);
            snprintf(tok[1], sizeof(tok[1]), "%02lu", (unsigned long)ss);
            snprintf(tok[2], sizeof(tok[2]), "%03lu", (unsigned long)ms);
          }

          int totalW = textWidth(cellTxt[b + 1]);
          int curX = cx + max(2, (cw - totalW) / 2);
          int ty = TY(stripH);
          int pillY = max(1, ty - SY(2));
          int pillH = min(stripH - 1 - pillY, FONT_H + SY(4));

          s.drawText(curX, ty, PUNCH_PREFIX[b], boxSel ? pageAccent : LIGHT);
          curX += textWidth(PUNCH_PREFIX[b]) + FONT_GAP;

          for (int i = 0; i < cellsPerBox; i++) {
            int tw = textWidth(tok[i]);
            if (i == subSel) {
              s.fillRect(curX - PX, pillY, tw + 2 * PX, pillH, pageAccent);
              s.drawText(curX, ty, tok[i], BG);
            } else {
              s.drawText(curX, ty, tok[i], fg);
            }
            curX += tw + FONT_GAP;
            if (sep[i][0] != '\0') {
              s.drawText(curX, ty, sep[i], fg);
              curX += textWidth(sep[i]) + FONT_GAP;
            }
          }
        }
      } else {
        static const int infoX[INFO_CELL_COUNT] = {SX(2), SX(76), SX(160)};
        static const int infoW[INFO_CELL_COUNT] = {SX(72), SX(82), SX(82)};

        for (int c = 0; c < INFO_CELL_COUNT; c++) {
          bool sel = (infoSelCell == c);
          int cx = infoX[c], cw = infoW[c];
          uint16_t bg = sel ? DARK : PANEL;
          uint16_t border = sel ? pageAccent : MID;
          uint16_t fg = sel ? pageAccent : (cellDim[c] ? DIM : LIGHT);

          s.fillRect(cx, 0, cw, stripH, border);
          s.fillRect(cx + 1, 1, cw - 2, stripH - 2, bg);
          int tw = textWidth(cellTxt[c]);
          s.drawText(cx + max(2, (cw - tw) / 2), TY(stripH), cellTxt[c], fg);
        }
      }
      s.blit(0, SY(195));
    }
  }

  float inL, inR;
  takeInputPeaks(inL, inR);
  trSmoothInL = max(inL, trSmoothInL * 0.8f);
  trSmoothInR = max(inR, trSmoothInR * 0.8f);
  if (trSmoothInL < 0.002f) trSmoothInL = 0.0f;
  if (trSmoothInR < 0.002f) trSmoothInR = 0.0f;

  float outL, outR;
  takeOutputPeaks(outL, outR);
  trSmoothOutL = max(outL, trSmoothOutL * 0.8f);
  trSmoothOutR = max(outR, trSmoothOutR * 0.8f);
  if (trSmoothOutL < 0.002f) trSmoothOutL = 0.0f;
  if (trSmoothOutR < 0.002f) trSmoothOutR = 0.0f;

  const int barMaxH = SY(12);
  int inHL  = constrain((int)(sqrtf(trSmoothInL)  * (float)barMaxH + 0.5f), 0, barMaxH);
  int inHR  = constrain((int)(sqrtf(trSmoothInR)  * (float)barMaxH + 0.5f), 0, barMaxH);
  int outHL = constrain((int)(sqrtf(trSmoothOutL) * (float)barMaxH + 0.5f), 0, barMaxH);
  int outHR = constrain((int)(sqrtf(trSmoothOutR) * (float)barMaxH + 0.5f), 0, barMaxH);
  bool clipInL  = trSmoothInL  > 0.9f;
  bool clipInR  = trSmoothInR  > 0.9f;
  bool clipOutL = trSmoothOutL > 0.9f;
  bool clipOutR = trSmoothOutR > 0.9f;

  uint64_t mk = hashMix(hashInit(),
                        (uint64_t)inHL | ((uint64_t)inHR << 8) |
                        ((uint64_t)outHL << 16) | ((uint64_t)outHR << 24) |
                        ((uint64_t)clipInL << 32) | ((uint64_t)clipInR << 33) |
                        ((uint64_t)clipOutL << 34) | ((uint64_t)clipOutR << 35) |
                        ((uint64_t)pageAccent << 40));
  if (full || mk != prevInOutMeterKey) {
    prevInOutMeterKey = mk;
    Surface ms(meterStripW, stripH);
    ms.fillRect(0, 0, meterStripW, stripH, BG);

    const int barW = SX(3);
    const int barGap = SX(2);
    const int barY = (stripH - barMaxH) / 2;

    const int inLblX = SX(248) - meterStripX;
    ms.drawText(inLblX, TY(stripH), "IN", DIM);
    const int inBarX0 = inLblX + textWidth("IN") + SX(3);
    const int inBarX1 = inBarX0 + barW + barGap;
    ms.fillRect(inBarX0, barY, barW, barMaxH, DARK);
    ms.fillRect(inBarX1, barY, barW, barMaxH, DARK);
    if (inHL > 0) ms.fillRect(inBarX0, barY + (barMaxH - inHL), barW, inHL, clipInL ? COLOR_REC : pageAccent);
    if (inHR > 0) ms.fillRect(inBarX1, barY + (barMaxH - inHR), barW, inHR, clipInR ? COLOR_REC : pageAccent);

    const int outLblX = SX(281) - meterStripX;
    ms.drawText(outLblX, TY(stripH), "OUT", DIM);
    const int outBarX0 = outLblX + textWidth("OUT") + SX(3);
    const int outBarX1 = outBarX0 + barW + barGap;
    ms.fillRect(outBarX0, barY, barW, barMaxH, DARK);
    ms.fillRect(outBarX1, barY, barW, barMaxH, DARK);
    if (outHL > 0) ms.fillRect(outBarX0, barY + (barMaxH - outHL), barW, outHL, clipOutL ? COLOR_REC : pageAccent);
    if (outHR > 0) ms.fillRect(outBarX1, barY + (barMaxH - outHR), barW, outHR, clipOutR ? COLOR_REC : pageAccent);
    ms.blit(meterStripX, SY(195));
  }

  // 2. Six Transport Buttons: [ REC/PUNCH ] [ LOOP ] [ |< ] [ << ] [ PLAY ] [ >> ]
  bool armedRec = false;
  bool armedDub = false;
  for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
    if (tracks[i].armMode == ArmMode::Record) armedRec = true;
    else if (tracks[i].armMode == ArmMode::Overdub) armedDub = true;
  }
  for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
    if (midiTracks[i].armMode == ArmMode::Record) armedRec = true;
    else if (midiTracks[i].armMode == ArmMode::Overdub) armedDub = true;
  }

  const int btnY = SY(212);
  const int btnH = SY(26);
  constexpr int gap = SX(2);
  constexpr int recLoopW = btnH + SX(2);
  constexpr int rtzX = 3 * gap + 2 * recLoopW;
  constexpr int remW = (SCREEN_W - gap) - rtzX - 3 * gap;
  constexpr int navW = (remW - SX(22)) / 4;
  constexpr int playW = remW - 3 * navW;
  static const int btnX[TR_CELL_COUNT] = {
    gap,
    2 * gap + recLoopW,
    rtzX,
    rtzX + navW + gap,
    rtzX + 2 * (navW + gap),
    rtzX + 2 * (navW + gap) + playW + gap
  };
  static const int btnW[TR_CELL_COUNT] = {recLoopW, recLoopW, navW, navW, playW, navW};
  TransportMode tm = transport.mode;

  for (int c = 0; c < TR_CELL_COUNT; c++) {
    bool sel = isSelectedRow && (selCell == c);

    if (c == TR_CELL_REC) {
      uint16_t aBg = sel ? DARK : PANEL;
      uint16_t aBorder = sel ? pageAccent : MID;
      uint16_t aCircle = DIM;
      if (armedRec) {
        aBg = COLOR_REC_DIM;
        aBorder = sel ? pageAccent : COLOR_REC;
        aCircle = COLOR_REC;
      } else if (armedDub) {
        aBg = COLOR_DUB_DIM;
        aBorder = sel ? pageAccent : COLOR_OVERDUB;
        aCircle = COLOR_OVERDUB;
      }
      uint16_t punchFg = sel ? pageAccent : LIGHT;

      uint64_t bk = hashMix(hashInit(),
                            (uint64_t)aBg | ((uint64_t)aBorder << 16) |
                            ((uint64_t)aCircle << 32) | ((uint64_t)punchFg << 48));
      bk = hashMix(bk, ps.punchEnabled ? 1ULL : 0ULL);
      if (full || bk != prevBtnKey[c]) {
        prevBtnKey[c] = bk;
        const int bw = btnW[c];
        Surface s(bw, btnH);
        s.drawBox(aBg, aBorder);
        const int cx = bw / 2;
        const int cy = btnH / 2;
        const int r = SY(6) - 1;
        s.drawCircle(cx, cy, r, aCircle);
        if (ps.punchEnabled) {
          int barW = textWidth("|");
          int leftX = max(PX + 1, cx - r - SX(2) - barW);
          int rightX = min(bw - PX - 1 - barW, cx + r + SX(2));
          s.drawText(leftX, TY(btnH), "|", punchFg);
          s.drawText(rightX, TY(btnH), "|", punchFg);
        }
        s.blit(btnX[c], btnY);
      }
      continue;
    }

    if (c == TR_CELL_LOOP) {
      const bool loopOn = ps.loopEnabled;
      uint16_t lBg = loopOn ? pageAccent : (sel ? DARK : PANEL);
      uint16_t lBorder = loopOn ? (sel ? INK : pageAccent) : (sel ? pageAccent : MID);
      uint16_t lFg = loopOn ? BG : (sel ? pageAccent : LIGHT);

      uint64_t bk = hashMix(hashInit(),
                            (uint64_t)lBg | ((uint64_t)lBorder << 16) |
                            ((uint64_t)lFg << 32) | ((uint64_t)loopOn << 48) |
                            ((uint64_t)sel << 49));
      if (full || bk != prevBtnKey[c]) {
        prevBtnKey[c] = bk;
        const int bw = btnW[c];
        Surface s(bw, btnH);
        if (loopOn && sel) {
          s.drawBox(BG, lBorder);
          s.fillRect(2 * PX, 2 * PX, bw - 4 * PX, btnH - 4 * PX, lBg);
        } else {
          s.drawBox(lBg, lBorder);
        }
        drawLoopArrowIcon(s, bw / 2, btnH / 2, lFg);
        s.blit(btnX[c], btnY);
      }
      continue;
    }

    const char *label = "";
    char playLabel[16];
    uint16_t fg = LIGHT, bg = PANEL, border = sel ? pageAccent : MID;

    switch (c) {
      case TR_CELL_RTZ:
        label = "|<";
        break;
      case TR_CELL_REW:
        label = "<<";
        break;
      case TR_CELL_PLAY: {
        int sqVal = (int)transport.speedQuarter;
        int absQ = abs(sqVal);
        int whole = absQ / 4;
        int frac = (absQ % 4) * 25;
        char speedTxt[12];
        snprintf(speedTxt, sizeof(speedTxt), "%s%d.%02dX", (sqVal < 0) ? "-" : "", whole, frac);
        if (tm == TransportMode::Running) {
          snprintf(playLabel, sizeof(playLabel), "%s", speedTxt);
          label = playLabel;
          bg = pageAccent;
          fg = BG;
          if (sel) border = INK;
        } else if (tm == TransportMode::CountIn) {
          label = "CNT";
          bg = COLOR_WARN;
          fg = BG;
          if (sel) border = INK;
        } else if (tm == TransportMode::WaitThreshold) {
          label = "WAIT";
          bg = COLOR_WARN;
          fg = BG;
          if (sel) border = INK;
        } else {
          snprintf(playLabel, sizeof(playLabel), "%c %s", (sqVal < 0) ? '<' : '>', speedTxt);
          label = playLabel;
        }
        break;
      }
      case TR_CELL_FF:
        label = ">>";
        break;
    }

    if (sel && bg == PANEL) {
      bg = DARK;
      if (fg == LIGHT) fg = pageAccent;
    }

    uint64_t bk = hashStr(hashInit(), label);
    bk = hashMix(bk, (uint64_t)bg | ((uint64_t)fg << 16) | ((uint64_t)border << 32) | ((uint64_t)sel << 48));
    if (full || bk != prevBtnKey[c]) {
      prevBtnKey[c] = bk;
      Surface s(btnW[c], btnH);
      if (sel && bg != DARK && bg != PANEL) {
        s.drawBox(BG, border);
        s.fillRect(2 * PX, 2 * PX, btnW[c] - 4 * PX, btnH - 4 * PX, bg);
      } else {
        s.drawBox(bg, border);
      }
      if (c == TR_CELL_PLAY && tm == TransportMode::Running) {
        const int sq = SY(10);
        const int gapSq = SX(4);
        int tw = textWidth(label);
        int totalW = sq + gapSq + tw;
        int startX = max(2, (btnW[c] - totalW) / 2);
        s.fillRect(startX, (btnH - sq) / 2, sq, sq, fg);
        s.drawText(startX + sq + gapSq, TY(btnH), label, fg);
      } else {
        int tw = textWidth(label);
        s.drawText(max(2, (btnW[c] - tw) / 2), TY(btnH), label, fg);
      }
      s.blit(btnX[c], btnY);
    }
  }
}

// Shared On-Screen Keyboard Modal for Naming & Saving a Song or Instrument
static const char *const KB_LABELS[KB_GRID_ROWS][KB_GRID_COLS] = {
  {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J"},
  {"K", "L", "M", "N", "O", "P", "Q", "R", "S", "T"},
  {"U", "V", "W", "X", "Y", "Z", "_", "-", "0", "1"},
  {"2", "3", "4", "5", "6", "7", "8", "9", "DEL", "CLR"}
};

FLASHMEM void drawSaveKeyboardModal(bool full) {
  const KeyboardTarget tgt = saveKeyboardTarget();
  const bool isInst = (tgt == KeyboardTarget::Instrument);
  const bool isProj = (tgt == KeyboardTarget::ProjectNew || tgt == KeyboardTarget::ProjectRename);

  // 1. Name Input Box
  {
    int len = (int)strlen(saveKeyboardBuf);
    char disp[24];
    snprintf(disp, sizeof(disp), "%s%s", saveKeyboardBuf, (len < MAX_SONG_NAME_LEN) ? "_" : "");
    char countStr[12];
    snprintf(countStr, sizeof(countStr), "%d/%d", len, MAX_SONG_NAME_LEN);
    const char *prompt = isInst ? "INST NAME:"
                       : (tgt == KeyboardTarget::ProjectNew) ? "NEW PROJ:"
                       : (tgt == KeyboardTarget::ProjectRename) ? "RENAME:"
                                                                : "SAVE AS:";

    uint64_t bk = hashStr(hashInit(), disp);
    bk = hashStr(bk, prompt);
    bk = hashMix(bk, (uint64_t)len | ((uint64_t)ACCENT << 16));
    if (full || bk != prevKbBannerKey) {
      prevKbBannerKey = bk;
      const int bw = SX(308), bh = SY(24);
      Surface s(bw, bh);
      s.drawBox(DARK, ACCENT);
      s.drawText(SX(8), TY(bh), prompt, LIGHT);
      s.drawText(SX(86), TY(bh), disp, ACCENT);
      int cw = textWidth(countStr);
      s.drawText(bw - SX(8) - cw, TY(bh), countStr, DIM);
      s.blit(SX(6), SY(28));
    }
  }

  // 2. 10x4 Character Grid
  for (int r = 0; r < KB_GRID_ROWS; r++) {
    int y = SY(60) + r * SY(33);
    int h = SY(29);
    for (int c = 0; c < KB_GRID_COLS; c++) {
      int x = (c < 8) ? (SX(6) + c * SX(30)) : (c == 8 ? SX(246) : SX(282));
      int w = (c < 8) ? SX(28) : SX(32);
      bool sel = (saveKeyboardRow == r && saveKeyboardCol == c);
      const char *lbl = KB_LABELS[r][c];
      bool isSpecial = (r == 3 && c >= 8);

      uint16_t bg = sel ? ACCENT : (isSpecial ? DARK : PANEL);
      uint16_t fg = sel ? BG : (isSpecial ? COLOR_WARN : INK);
      uint16_t border = sel ? INK : MID;

      uint64_t kk = hashMix(hashInit(), (uint64_t)bg | ((uint64_t)fg << 16) | ((uint64_t)border << 32));
      if (full || kk != prevKbKey[r][c]) {
        prevKbKey[r][c] = kk;
        Surface s(w, h);
        s.drawBox(bg, border);
        int tw = textWidth(lbl);
        s.drawText(max(2, (w - tw) / 2), TY(h), lbl, fg);
        s.blit(x, y);
      }
    }
  }

  // 3. Bottom Action Row: [ SAVE SONG / SAVE NAME / SAVE ] and [ CANCEL ]
  const char *btn0 = isInst ? "SAVE NAME" : (isProj ? "SAVE" : "SAVE SONG");
  for (int b = 0; b < 2; b++) {
    const char *btnTitle = (b == 0) ? btn0 : "CANCEL";
    int x = (b == 0) ? SX(6) : SX(164);
    int w = SX(150), h = SY(32);
    bool sel = (saveKeyboardRow == KB_BTN_ROW && saveKeyboardCol == b);
    uint16_t bg = sel ? (b == 0 ? COLOR_PLAY : COLOR_WARN) : PANEL;
    uint16_t fg = sel ? BG : (b == 0 ? COLOR_PLAY : LIGHT);
    uint16_t border = sel ? INK : (b == 0 ? COLOR_PLAY : MID);

    uint64_t bk = hashStr(hashInit(), btnTitle);
    bk = hashMix(bk, (uint64_t)bg | ((uint64_t)fg << 16) | ((uint64_t)border << 32));
    if (full || bk != prevKbBtnKey[b]) {
      prevKbBtnKey[b] = bk;
      Surface s(w, h);
      s.drawBox(bg, border);
      int tw = textWidth(btnTitle);
      s.drawText(max(4, (w - tw) / 2), TY(h), btnTitle, fg);
      s.blit(x, SY(198));
    }
  }
}

// Resample Pre/Post Master FX Selection Overlay
FLASHMEM static void drawResampleModal(bool full) {
  if (!isResampleModalOpen()) return;

  const int ovX = SX(40);
  const int ovW = SX(240);
  const int hdrY = SY(68);
  const int hdrH = SY(22);
  const int rowH = SY(24);
  const int cur = constrain(resampleModalCursor(), 0, 1);
  const int trk = constrain(resampleModalTrack() + 1, 1, 8);

  // 1. Header Banner Strip
  {
    char hdrTitle[28];
    snprintf(hdrTitle, sizeof(hdrTitle), "AUDIO A%d RESAMPLE", trk);
    uint64_t bk = hashStr(hashInit(), hdrTitle);
    bk = hashMix(bk, (uint64_t)ACCENT);
    if (full || bk != prevResBannerKey) {
      prevResBannerKey = bk;
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

  // 2. Option Pill Rows: [ PRE-MASTER FX ] and [ POST-MASTER FX ]
  static const char *const RES_LABELS[2] = {
    "PRE-MASTER FX",
    "POST-MASTER FX"
  };
  for (int i = 0; i < 2; i++) {
    const bool sel = (cur == i);
    const bool isLast = (i == 1);
    const int ry = hdrY + hdrH + i * rowH;

    uint16_t pillBg = sel ? ACCENT : PANEL;
    uint16_t pillFg = sel ? BG : INK;
    uint16_t pillBd = sel ? INK : MID;

    uint64_t rk = hashStr(hashInit(), RES_LABELS[i]);
    rk = hashMix(rk, (uint64_t)pillBg | ((uint64_t)pillFg << 16) |
                     ((uint64_t)pillBd << 32) | ((uint64_t)sel << 48));
    if (full || rk != prevResRowKey[i]) {
      prevResRowKey[i] = rk;
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
      s.drawPill(px, max(1, py), pw, ph, RES_LABELS[i], pillFg, pillBg, pillBd);
      s.blit(ovX, ry);
    }
  }
}

// Clear Track Recording & Quick Transport/Punch/Loop Overlay
// Row 0 (Audio): "Clear track recording?  [YES]  [NO]"
// Row 0 (MIDI):  "CLEAR:    [ALL]   [NOTES]   [CC/PB]"
// Row 1:         "[RETURN TO ZERO]   [SEEK TO IN]   [SEEK TO OUT]"
// Row 2:         "[PUNCH ON/OFF]   [LOOP ON/OFF]   [SET IN]   [SET OUT]"
FLASHMEM static void drawClearTrackModal(bool full) {
  if (!isClearTrackModalOpen()) return;

  const bool isMidi = (clearTrackModalPage() == Page::Midi);
  const uint16_t pageAccent = isMidi ? COLOR_MIDI : ACCENT;
  const int curRow = constrain(clearTrackModalRow(), 0, 2);
  const int curCol = clearTrackModalCursor();
  const SongSettings &ps = songSettings();

  const int ovX = SX(6);
  const int baseY = SY(70);
  const int ovW = SX(308);
  const int rowH = SY(24);
  const int pillH = SY(16);
  const int pillY = max(1, (rowH - pillH) / 2);

  // Row 0: Clear Track Recording
  {
    const int rowSel = (curRow == 0) ? curCol : -1;
    uint64_t k0 = hashMix(hashInit(), (uint64_t)pageAccent | ((uint64_t)(rowSel + 2) << 16) |
                                      ((uint64_t)clearTrackModalTrack() << 24) |
                                      ((uint64_t)isMidi << 32));
    if (full || k0 != prevClearTrackRowKey[0]) {
      prevClearTrackRowKey[0] = k0;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, DARK);
      s.fillRect(0, 0, ovW, PX, pageAccent);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);

      if (isMidi) {
        s.drawText(SX(8), TY(rowH), "CLEAR:", INK);
        static const char *const MIDI_OPT_LABELS[3] = {"ALL", "NOTES", "CC/PB"};
        const int pillW = SX(74);
        const int pillX[3] = {SX(72), SX(150), SX(228)};
        for (int i = 0; i < 3; i++) {
          bool sel = (rowSel == i);
          uint16_t pBg = sel ? COLOR_WARN : PANEL;
          uint16_t pFg = sel ? BG : INK;
          uint16_t pBd = sel ? INK : MID;
          s.drawPill(pillX[i], pillY, pillW, pillH, MIDI_OPT_LABELS[i], pFg, pBg, pBd);
        }
      } else {
        s.drawText(SX(8), TY(rowH), "Clear track recording?", INK);
        static const char *const OPT_LABELS[2] = {"YES", "NO"};
        const int pillW = SX(48);
        const int pillX[2] = {SX(200), SX(254)};
        for (int i = 0; i < 2; i++) {
          bool sel = (rowSel == i);
          uint16_t pBg = sel ? (i == 0 ? COLOR_WARN : pageAccent) : PANEL;
          uint16_t pFg = sel ? BG : INK;
          uint16_t pBd = sel ? INK : MID;
          s.drawPill(pillX[i], pillY, pillW, pillH, OPT_LABELS[i], pFg, pBg, pBd);
        }
      }
      s.blit(ovX, baseY);
    }
  }

  // Row 1: [RETURN TO ZERO]   [SEEK TO IN]   [SEEK TO OUT]
  {
    const int rowSel = (curRow == 1) ? curCol : -1;
    uint64_t k1 = hashMix(hashInit(), (uint64_t)pageAccent | ((uint64_t)(rowSel + 2) << 16));
    if (full || k1 != prevClearTrackRowKey[1]) {
      prevClearTrackRowKey[1] = k1;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, DARK);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);

      static const char *const SEEK_LABELS[3] = {"RETURN TO ZERO", "SEEK TO IN", "SEEK TO OUT"};
      static const int seekX[3] = {SX(6), SX(126), SX(214)};
      static const int seekW[3] = {SX(116), SX(84), SX(88)};
      for (int i = 0; i < 3; i++) {
        bool sel = (rowSel == i);
        uint16_t pBg = sel ? pageAccent : PANEL;
        uint16_t pFg = sel ? BG : INK;
        uint16_t pBd = sel ? INK : MID;
        s.drawPill(seekX[i], pillY, seekW[i], pillH, SEEK_LABELS[i], pFg, pBg, pBd);
      }
      s.blit(ovX, baseY + rowH);
    }
  }

  // Row 2: [PUNCH ON/OFF]   [LOOP ON/OFF]   [SET IN]   [SET OUT]
  {
    const int rowSel = (curRow == 2) ? curCol : -1;
    const bool punchOn = ps.punchEnabled;
    const bool loopOn = ps.loopEnabled;
    uint64_t k2 = hashMix(hashInit(), (uint64_t)pageAccent | ((uint64_t)(rowSel + 2) << 16) |
                                      ((uint64_t)punchOn << 24) | ((uint64_t)loopOn << 25));
    if (full || k2 != prevClearTrackRowKey[2]) {
      prevClearTrackRowKey[2] = k2;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, DARK);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);
      s.fillRect(0, rowH - PX, ovW, PX, pageAccent);

      const char *row2Labels[4] = {
        punchOn ? "PUNCH ON" : "PUNCH OFF",
        loopOn ? "LOOP ON" : "LOOP OFF",
        "SET IN",
        "SET OUT"
      };
      static const int row2X[4] = {SX(6), SX(90), SX(166), SX(234)};
      static const int row2W[4] = {SX(80), SX(72), SX(64), SX(68)};
      for (int i = 0; i < 4; i++) {
        bool sel = (rowSel == i);
        uint16_t pBg = sel ? pageAccent : PANEL;
        uint16_t pFg = sel ? BG : INK;
        uint16_t pBd = sel ? INK : MID;
        s.drawPill(row2X[i], pillY, row2W[i], pillH, row2Labels[i], pFg, pBg, pBd);
      }
      s.blit(ovX, baseY + 2 * rowH);
    }
  }
}

FLASHMEM void uiRefresh() {
  static uint32_t lastRefreshMs = 0;
  sysProjectPlayService();
  uint32_t now = millis();
  if (lastActivityMs == 0) {
    lastActivityMs = now;
  }
  if (!screenBlanked && (uint32_t)(now - lastActivityMs) >= SCREEN_BLANK_TIMEOUT_MS) {
    screenBlanked = true;
    board.setBacklight(0);
    board.tft().fillScreen(BG);
    return;
  }
  if (screenBlanked) {
    return;
  }

  trackEditTick();
  clearTrackModalTick();

  Page pg = currentPage();
  static bool lastKeyboardOpen = false;
  static bool lastTeModalOpen = false;
  static bool lastResModalOpen = false;
  static bool lastClearModalOpen = false;
  static bool lastNoteEdOpen = false;
  static bool lastSmpBrowserOpen = false;
  static uint8_t lastSmpKeyModalState = 0;
  static bool lastInstBrowserOpen = false;
  static bool lastInstSaveToOpen = false;
  static bool lastSysPresetsOpen = false;
  static bool lastSysPresetDelOpen = false;
  static bool lastUsbSavePromptOpen = false;
  static uint8_t lastSysProjState = 0;
  static bool lastTypePresetOpen = false;
  static bool lastMidiBrowserOpen = false;
  bool kbOpen = isSaveKeyboardOpen() &&
                (pg == Page::Main || pg == Page::Instruments || pg == Page::InstrumentEdit ||
                 pg == Page::SystemSettings);
  bool teModalOpen = trackEditIsModalOpen() && (pg == Page::Audio || pg == Page::Midi);
  bool resModalOpen = isResampleModalOpen();
  bool clearModalOpen = isClearTrackModalOpen() && (pg == Page::Audio || pg == Page::Midi);
  bool noteEdOpen = isMidiNoteEditorOpen() && (pg == Page::Midi);
  bool smpBrowserOpen = isSamplerBrowserOpen() && (pg == Page::InstrumentEdit);
  uint8_t smpKeyModalSt = (pg == Page::InstrumentEdit) ? samplerKeyModalState() : 0;
  bool instBrowserOpen = isInstBrowserOpen() &&
                         (pg == Page::Instruments || pg == Page::InstrumentEdit);
  bool instSaveToOpen = isInstSaveToModalOpen() && (pg == Page::InstrumentEdit);
  bool sysPresetsOpen = isSysPresetsModalOpen() &&
                        (pg == Page::SystemSettings || pg == Page::SongSettings);
  bool sysPresetDelOpen = isSysPresetDeleteConfirmOpen() &&
                          (pg == Page::SystemSettings || pg == Page::SongSettings);
  bool usbSavePromptOpen = isUsbSavePromptModalOpen() && (pg == Page::SystemSettings);
  uint8_t sysProjState = (pg == Page::SystemSettings) ? sysProjectsModalState() : 0;
  bool sysProjConfirmOpen = isSysProjectsConfirmOpen() && (pg == Page::SystemSettings);
  bool typePresetOpen = isTypePresetModalOpen() && (pg == Page::InstrumentType);
  bool midiBrowserOpen = isMidiClipBrowserOpen() && (pg == Page::Midi || pg == Page::Audio);
  bool pageOrScreenChanged = (lastDrawnGen != uiGeneration) || (lastDrawnPage != pg);
  bool modalChanged = (!pageOrScreenChanged) &&
                      ((kbOpen != lastKeyboardOpen) || (teModalOpen != lastTeModalOpen) ||
                       (resModalOpen != lastResModalOpen) || (clearModalOpen != lastClearModalOpen) ||
                       (noteEdOpen != lastNoteEdOpen) || (smpBrowserOpen != lastSmpBrowserOpen) ||
                       (smpKeyModalSt != lastSmpKeyModalState) ||
                       (instBrowserOpen != lastInstBrowserOpen) || (instSaveToOpen != lastInstSaveToOpen) ||
                       (sysPresetsOpen != lastSysPresetsOpen) || (sysPresetDelOpen != lastSysPresetDelOpen) ||
                       (usbSavePromptOpen != lastUsbSavePromptOpen) || (sysProjState != lastSysProjState) ||
                       (typePresetOpen != lastTypePresetOpen) || (midiBrowserOpen != lastMidiBrowserOpen));

  if (!pageOrScreenChanged && !modalChanged && !uiImmediate && (now - lastRefreshMs < 33)) return;
  lastRefreshMs = now;
  uiCurFrameImmediate = pageOrScreenChanged || modalChanged || uiImmediate;
  uiImmediate = false;
  if (!uiCurFrameImmediate) {
    uiFramePhase++;
  }

  const bool projPlayChanged = ((sysProjState == 4) != (lastSysProjState == 4));
  if (pageOrScreenChanged) {
    lastDrawnGen = uiGeneration;
    lastDrawnPage = pg;
    lastKeyboardOpen = kbOpen;
    lastTeModalOpen = teModalOpen;
    lastResModalOpen = resModalOpen;
    lastClearModalOpen = clearModalOpen;
    lastNoteEdOpen = noteEdOpen;
    lastSmpBrowserOpen = smpBrowserOpen;
    lastSmpKeyModalState = smpKeyModalSt;
    lastInstBrowserOpen = instBrowserOpen;
    lastInstSaveToOpen = instSaveToOpen;
    lastSysPresetsOpen = sysPresetsOpen;
    lastSysPresetDelOpen = sysPresetDelOpen;
    lastUsbSavePromptOpen = usbSavePromptOpen;
    lastSysProjState = sysProjState;
    lastTypePresetOpen = typePresetOpen;
    lastMidiBrowserOpen = midiBrowserOpen;
    invalidateUiCache();
    board.tft().fillScreen(BG);
  } else if (modalChanged) {
    lastKeyboardOpen = kbOpen;
    lastTeModalOpen = teModalOpen;
    lastResModalOpen = resModalOpen;
    lastClearModalOpen = clearModalOpen;
    lastNoteEdOpen = noteEdOpen;
    lastSmpBrowserOpen = smpBrowserOpen;
    lastSmpKeyModalState = smpKeyModalSt;
    lastInstBrowserOpen = instBrowserOpen;
    lastInstSaveToOpen = instSaveToOpen;
    lastSysPresetsOpen = sysPresetsOpen;
    lastSysPresetDelOpen = sysPresetDelOpen;
    lastUsbSavePromptOpen = usbSavePromptOpen;
    lastSysProjState = sysProjState;
    lastTypePresetOpen = typePresetOpen;
    lastMidiBrowserOpen = midiBrowserOpen;
    invalidateUiCache();
    if (projPlayChanged) {
      board.tft().fillScreen(BG);
    } else {
      board.tft().fillRect(0, SY(24), SCREEN_W, SCREEN_H - SY(24), BG);
    }
  }

  if (sysProjState != 4) {
    drawHeaderBar(pageOrScreenChanged || projPlayChanged);
  }
  keepStreaming();

  const PageOps &ops = pageOpsFor(pg);
  if (resModalOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawResampleModal(pageOrScreenChanged || modalChanged);
  } else if (smpKeyModalSt != 0) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawSamplerKeyModal(pageOrScreenChanged || modalChanged);
  } else if (instSaveToOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawInstSaveToModal(pageOrScreenChanged || modalChanged);
  } else if (sysPresetDelOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawSysPresetDeleteConfirmModal(pageOrScreenChanged || modalChanged);
  } else if (sysProjConfirmOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawSysProjectsConfirmModal(pageOrScreenChanged || modalChanged);
  } else if (usbSavePromptOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawUsbSavePromptModal(pageOrScreenChanged || modalChanged);
  } else if (clearModalOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawClearTrackModal(pageOrScreenChanged || modalChanged);
  } else if (midiBrowserOpen) {
    drawMidiClipBrowserModal(pageOrScreenChanged || modalChanged);
  } else if (teModalOpen) {
    if ((pageOrScreenChanged || modalChanged) && ops.drawPage) {
      ops.drawPage(true);
    }
    drawTrackEditOverlay(pageOrScreenChanged || modalChanged);
  } else if (noteEdOpen) {
    drawMidiNoteEditorOverlay(pageOrScreenChanged || modalChanged);
  } else if (ops.drawPage) {
    ops.drawPage(pageOrScreenChanged || modalChanged);
  }
}

FLASHMEM void Surface::drawDottedVLine(int x, int y, int rh, uint16_t c) {
  const int dotH = max(1, SY(2));
  const int step = dotH * 2;
  for (int dy = 0; dy < rh; dy += step) {
    fillRect(x, y + dy, 1, min(dotH, rh - dy), c);
  }
}

FLASHMEM void Surface::drawCircle(int cx, int cy, int r, uint16_t c) {
  int r2Max = r * r + r / 2;
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy <= r2Max) {
        drawPixel(cx + dx, cy + dy, c);
      }
    }
  }
}

FLASHMEM void Surface::drawBox(uint16_t fill, uint16_t border) {
  if (border != BG && border != fill) {
    fillRect(0, 0, w, PX, border);
    fillRect(0, h - PX, w, PX, border);
    fillRect(0, PX, PX, h - 2 * PX, border);
    fillRect(w - PX, PX, PX, h - 2 * PX, border);
    fillRect(PX, PX, w - 2 * PX, h - 2 * PX, fill);
  } else {
    fillRect(0, 0, w, h, fill);
  }
}

FLASHMEM void Surface::drawText(int x, int y, const char *text, uint16_t ink, int bgColor) {
  if (bgColor >= 0) {
    int tw = textWidth(text);
    fillRect(x - 1, y - 1, tw + 2, FONT_H + 2, (uint16_t)bgColor);
  }
  int curX = x;
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    const int gw = glyphWidth(*g);
    const int gwPx = gw * FONT_SCALE;
    if (curX >= 0 && curX + gwPx <= w && y >= 0 && y + FONT_H <= h) {
      for (int r = 0; r < FONT_ROWS; r++) {
        uint32_t bits = (uint32_t)g->rows[r];
        if (!bits) continue;
        if constexpr (FONT_SCALE == 1) {
          uint16_t *rowPtr = px + (y + r) * w + curX;
          while (bits) {
            int c = __builtin_ctz(bits);
            rowPtr[c] = ink;
            bits &= bits - 1u;
          }
        } else {
          uint16_t *row0 = px + (y + r * 2) * w + curX;
          uint16_t *row1 = row0 + w;
          while (bits) {
            int c2 = __builtin_ctz(bits) << 1;
            row0[c2]     = ink;
            row0[c2 + 1] = ink;
            row1[c2]     = ink;
            row1[c2 + 1] = ink;
            bits &= bits - 1u;
          }
        }
      }
    } else if (curX < w && curX + gwPx > 0 && y < h && y + FONT_H > 0) {
      for (int r = 0; r < FONT_ROWS; r++) {
        uint32_t bits = (uint32_t)g->rows[r];
        while (bits) {
          int c = __builtin_ctz(bits);
          fillRect(curX + c * FONT_SCALE, y + r * FONT_SCALE, FONT_SCALE, FONT_SCALE, ink);
          bits &= bits - 1u;
        }
      }
    }
    curX += gwPx + FONT_GAP;
  }
}

FLASHMEM void Surface::drawPill(int x, int y, int pw, int ph, const char *label,
                                uint16_t fg, uint16_t bg, uint16_t border) {
  fillRect(x, y, pw, ph, border);
  fillRect(x + 1, y + 1, pw - 2, ph - 2, bg);
  int tw = textWidth(label);
  drawText(x + max(1, (pw - tw) / 2), y + TY(ph), label, fg);
}

FLASHMEM void Surface::blit(int screenX, int screenY) {
  board.tft().writeRect(screenX, screenY, w, h, px);
}

FLASHMEM static int timelineSearchCol(int64_t fSearch, int64_t curFrame, int playCol, int innerW, uint32_t winFrames) {
  int col = playCol + (int)(((fSearch - curFrame) * innerW) / (int64_t)winFrames);
  while (col < innerW && (curFrame + ((int64_t)(col + 1 - playCol) * (int64_t)winFrames) / innerW) <= fSearch) col++;
  while (col >= 0 && (curFrame + ((int64_t)(col - playCol) * (int64_t)winFrames) / innerW) > fSearch) col--;
  return col;
}

FLASHMEM void drawTimelineTicksInWindow(Surface &s, int innerX, int innerY, int innerW, int innerH,
                                        int64_t curFrame, int playCol, uint32_t winFrames,
                                        float curBpm, uint8_t zoom) {
  if (innerW <= 0 || winFrames == 0) return;
  int64_t winStartF = curFrame - ((int64_t)playCol * (int64_t)winFrames) / innerW;
  int64_t winEndF   = curFrame + ((int64_t)(innerW - playCol) * (int64_t)winFrames) / innerW;
  if (winEndF <= 0 || winStartF > (int64_t)looper::MAX_REC_FRAMES) return;

  int64_t cfStart = max((int64_t)0, winStartF);
  int64_t cfEnd   = min((int64_t)looper::MAX_REC_FRAMES, winEndF);

  uint32_t smallTimeFrames = looper::SAMPLE_RATE_HZ;
  uint32_t largeTimeMult   = 5;
  uint32_t smallBeatTicks  = looper::MIDI_PPQN;
  uint32_t largeBeatMult   = looper::BEATS_PER_BAR;

  switch (zoom) {
    case looper::ZOOM_4_BARS:
      smallTimeFrames = looper::SAMPLE_RATE_HZ / 10;
      largeTimeMult   = 10;
      smallBeatTicks  = looper::MIDI_PPQN;
      largeBeatMult   = looper::BEATS_PER_BAR;
      break;
    case looper::ZOOM_16_BARS:
      smallTimeFrames = looper::SAMPLE_RATE_HZ;
      largeTimeMult   = 5;
      smallBeatTicks  = looper::MIDI_PPQN;
      largeBeatMult   = looper::BEATS_PER_BAR;
      break;
    case looper::ZOOM_32_BARS:
      smallTimeFrames = 5 * looper::SAMPLE_RATE_HZ;
      largeTimeMult   = 2;
      smallBeatTicks  = looper::BEATS_PER_BAR * looper::MIDI_PPQN;
      largeBeatMult   = 4;
      break;
    case looper::ZOOM_FULL:
    default:
      smallTimeFrames = 30 * looper::SAMPLE_RATE_HZ;
      largeTimeMult   = 2;
      smallBeatTicks  = 16 * looper::BEATS_PER_BAR * looper::MIDI_PPQN;
      largeBeatMult   = 2;
      break;
  }

  uint32_t uMin = (uint32_t)(cfStart / smallTimeFrames);
  uint32_t uMax = (uint32_t)((cfEnd + smallTimeFrames - 1) / smallTimeFrames);
  int lastTimeCol = -1;
  bool lastTimeLarge = false;
  for (uint32_t u = uMin; u <= uMax; u++) {
    int64_t fTick = (int64_t)u * smallTimeFrames;
    if (fTick > (int64_t)looper::MAX_REC_FRAMES) break;
    int col = timelineSearchCol(u == 0 ? 0 : (fTick - 1), curFrame, playCol, innerW, winFrames);
    if (col < 0 || col >= innerW) continue;
    bool isLarge = ((u % largeTimeMult) == 0);
    if (col == lastTimeCol && (lastTimeLarge || !isLarge)) continue;
    lastTimeCol = col;
    lastTimeLarge = isLarge;
    int tickH = isLarge ? SY(6) : SY(3);
    s.fillRect(innerX + col, innerY, 1, tickH, COLOR_TICK_TIME);
  }

  uint32_t tMin = looper::framesToMidiTicks((uint32_t)cfStart, curBpm);
  uint32_t tMax = looper::framesToMidiTicks((uint32_t)cfEnd, curBpm);
  uint32_t bMin = tMin / smallBeatTicks;
  uint32_t bMax = (tMax + smallBeatTicks - 1) / smallBeatTicks;
  int lastBeatCol = -1;
  bool lastBeatLarge = false;
  for (uint32_t b = bMin; b <= bMax; b++) {
    uint32_t tk = b * smallBeatTicks;
    int64_t fTick = (int64_t)looper::midiTicksToFrames(tk, curBpm);
    if (fTick > (int64_t)looper::MAX_REC_FRAMES) break;
    int col = timelineSearchCol(b == 0 ? 0 : max((int64_t)0, fTick - 1), curFrame, playCol, innerW, winFrames);
    if (col < 0 || col >= innerW) continue;
    bool isLarge = ((b % largeBeatMult) == 0);
    if (col == lastBeatCol && (lastBeatLarge || !isLarge)) continue;
    lastBeatCol = col;
    lastBeatLarge = isLarge;
    int tickH = isLarge ? SY(6) : SY(3);
    s.fillRect(innerX + col, innerY + innerH - tickH, 1, tickH, COLOR_TICK_BEAT);
  }
}

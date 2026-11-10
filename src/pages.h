// pages.h — Modular Per-Page UI & Input Architecture
//
// Each UI page in the firmware (Audio, Midi, SongSettings, Main, SystemSettings,
// Instruments, InstrumentType, InstrumentEdit, Master) is implemented in its own
// src/page_<name>.h / src/page_<name>.cpp module and exports a PageOps descriptor
// uniting its rendering, cache invalidation, spatial navigation, and key handling.
#pragma once
#include "app.h"
#include "ui.h"
#include "ui_font.h"
#include "input.h"

// Shared offscreen compositing buffer in fast DTCM (RAM1):
// Model:01 (320x240): fits up to 308x24 (7,392 px) or 184x39 (7,176 px)
// Model:02 (480x320): fits up to 462x32 (14,784 px) or 276x52 (14,352 px)
#if defined(ARDUINO_TEENSY_MICROMOD)
constexpr int CELL_BUF_MAX = 15000;
#else
constexpr int CELL_BUF_MAX = 7400;
#endif
extern uint16_t cellBuf[CELL_BUF_MAX];

// FNV-1a 64-bit hash helpers for per-cell dirty state tracking
inline uint64_t hashInit() { return 14695981039346656037ULL; }
inline uint64_t hashMix(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 1099511628211ULL;
  return h;
}
inline uint64_t hashStr(uint64_t h, const char *s) {
  while (*s) {
    h = hashMix(h, (uint8_t)*s++);
  }
  return h;
}

// Offscreen 2D drawing surface backed by shared cellBuf[] in RAM1
struct Surface {
  uint16_t *px;
  int w, h;

  Surface(int width, int height)
      : px(cellBuf),
        w(constrain(width, 1, SCREEN_W)),
        h(constrain(height, 1, CELL_BUF_MAX / max(1, constrain(width, 1, SCREEN_W)))) {}

  inline void fillRect(int x, int y, int rw, int rh, uint16_t c) {
    int x0 = max(0, x), y0 = max(0, y);
    int x1 = min(w, x + rw), y1 = min(h, y + rh);
    for (int py = y0; py < y1; py++) {
      uint16_t *row = px + py * w;
      for (int cx = x0; cx < x1; cx++) row[cx] = c;
    }
  }

  inline void drawPixel(int x, int y, uint16_t c) {
    if (x >= 0 && x < w && y >= 0 && y < h) px[y * w + x] = c;
  }

  void drawDottedVLine(int x, int y, int rh, uint16_t c);
  void drawCircle(int cx, int cy, int r, uint16_t c);
  void drawBox(uint16_t fill, uint16_t border);
  void drawText(int x, int y, const char *text, uint16_t ink, int bgColor = -1);
  void drawPill(int x, int y, int pw, int ph, const char *label,
                uint16_t fg, uint16_t bg, uint16_t border);
  void blit(int screenX, int screenY);
};

// Return the visible timeline window span in stereo frames for the given zoom level
inline uint32_t timelineWindowFrames(uint8_t zoom, float curBpm) {
  switch (zoom) {
    case looper::ZOOM_4_BARS:
      return max(1u, looper::midiTicksToFrames(4u * looper::BEATS_PER_BAR * looper::MIDI_PPQN, curBpm));
    case looper::ZOOM_16_BARS:
      return max(1u, looper::midiTicksToFrames(16u * looper::BEATS_PER_BAR * looper::MIDI_PPQN, curBpm));
    case looper::ZOOM_32_BARS:
      return max(1u, looper::midiTicksToFrames(32u * looper::BEATS_PER_BAR * looper::MIDI_PPQN, curBpm));
    case looper::ZOOM_FULL:
    default:
      return looper::MAX_REC_FRAMES;
  }
}

// Convert stereo frames to 1/32nd-note index using integer fixed-point math (0.1 BPM resolution)
inline int64_t framesTo32nds(uint32_t frames, float curBpm) {
  uint32_t bpmTenths = (uint32_t)constrain((int)(curBpm * 10.0f + 0.5f), 400, 2400);
  return (int64_t)(((uint64_t)frames * (uint64_t)bpmTenths + (uint64_t)(bpmTenths >> 1)) / 3307500ULL);
}

// Map a tape frame to a pixel column in the scrolling window centered on curFrame
inline int timelineFrameToCol(int64_t targetFrame, int64_t curFrame, int playCol, int innerW, uint32_t winFrames) {
  int64_t rel = targetFrame - curFrame;
  return playCol + (int)((rel * innerW) / (int64_t)max(1u, winFrames));
}

// Precomputed per-frame timeline geometry and integer fixed-point tick context
// (computed once per UI frame instead of 4x per track row)
struct TimelineFrameCtx {
  int64_t curFrame;
  uint8_t zoom;
  uint32_t bpmKey;
  uint32_t winFrames;
  uint32_t halfWinFrames;
  uint32_t scrollStep;
  uint32_t curTick;
  int64_t windowTicks;
  uint32_t startTick;
  uint32_t endTick;
  bool punchOn;
  int punchX0;
  int punchX1;
};

void computeTimelineFrameCtx(TimelineFrameCtx &ctx, int innerW, int playCol);

// UI update throttle tier & timeline round-robin helpers:
// - uiIsImmediateFrame(): true on user key/modal/page-change frames (bypasses throttling)
// - uiTierDue(periodTicks): true on immediate frames or every `periodTicks` 33 ms ticks
//   (1 = 30 Hz, 2 = 15 Hz, 3 = 10 Hz, 6 = 5 Hz)
// - uiTimelineRrSlot(): 0..1 alternating each 33 ms tick for 2-row timeline round-robin
bool uiIsImmediateFrame();
bool uiTierDue(uint8_t periodTicks);
uint8_t uiTimelineRrSlot();

// Fast direct tick-to-pixel timeline renderer: loops only over the time ticks and
// beat/bar ticks that fall inside the visible window instead of per-column division/BPM conversion.
void drawTimelineTicksInWindow(Surface &s, int innerX, int innerY, int innerW, int innerH,
                               int64_t curFrame, int playCol, uint32_t winFrames,
                               float curBpm, uint8_t zoom);

// Shared time / bar formatting and common UI/Input widgets (implemented in ui.cpp / input.cpp)
void formatTime(uint32_t frames, char *out, size_t sz);
void formatBarBeat(uint32_t frames, float curBpm, char *out, size_t sz);
void drawTransportBar(bool isSelectedRow, int selCell, bool full);
void drawSaveKeyboardModal(bool full);

void drawTrackArmBadgeCell(int x, int y, int w, int h, int trackIdx,
                           ArmMode armMode, bool muted, bool sel,
                           uint16_t pageAccent, uint64_t &cacheKey, bool full,
                           const char *armTag = nullptr);
void drawTrackTrigBadgeCell(int x, int y, int w, int h,
                            TrigMode trigMode, bool sel,
                            uint16_t pageAccent, uint64_t &cacheKey, bool full);
void drawTwoLineParamBox(int x, int y, int w, int h,
                         const char *label, const char *valStr,
                         bool sel, bool active, uint16_t pageAccent,
                         int lblY, int valY, uint64_t &cacheKey, bool full);
void drawSettingsTableRow(int y, uint32_t rowId, const char *label, const char *valStr,
                          bool sel, bool isOff, bool isPill,
                          uint64_t &cacheKey, bool full);

void handleHeaderCellOptAdjust(int cell, int dx, int dy);
void handleHeaderCellEditPress(bool shiftHeld);
void handleHeaderCellReset(int cell);
void handleInfoCellOptAdjust(int cell, int dx, int dy, int delta);
void handleInfoCellEditPress(int cell, bool shiftHeld);
void handleInfoCellReset(int cell);
void handleTransportCellAction(int cell, bool shiftHeld);
void handleTransportCellOptAdjust(int cell, int dx, int dy, int delta);
void handleTransportCellReset(int cell);
void inputSwallowModalKeys();
void inputClearRepeat();

// Modular Page Interface — each page module exports one PageOps descriptor
struct PageOps {
  Page id;
  const char *(*headerTitle)();                       // Short title badge shown in top header bar
  void (*onEnter)();                                  // Optional hook invoked by setPage() on page entry
  void (*invalidateCache)();                          // Clears per-cell dirty state hashes for this page
  void (*drawPage)(bool full);                        // Renders the page body (y = 24..239)
  void (*onShiftNavigate)(uint8_t dir);               // Handles SHIFT + Directional Arrows page navigation
  void (*onCursorMove)(int dx, int dy, uint8_t dir);  // Handles plain Directional Arrows cursor movement
  void (*onOptAdjust)(int dx, int dy, int delta);     // Handles EDIT + Directional Arrows value adjustment
  void (*onEditPress)(bool shiftHeld);                // Handles EDIT (or SHIFT+EDIT) key press
  void (*onResetParam)();                             // Handles OPT + EDIT parameter reset
};

constexpr int NUM_UI_PAGES = 9;
const PageOps &pageOpsFor(Page p);
void invalidateAllPagesCache();

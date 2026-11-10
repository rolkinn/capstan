// track_edit.h — Modular Track Timeline Region Highlighting & Editing (Audio & MIDI)
#pragma once
#include "app.h"

enum class TrackEditPhase : uint8_t {
  Off            = 0,  // Normal operation
  SelectRegion   = 1,  // Cursor in top bar adjusting highlight start/end (OPT toggles start/end, SHIFT+OPT opens menu)
  ActionMenu     = 2,  // Overlay open: SET PUNCH REGION, COPY, CUT, DUPL, DEL
  DuplReveal     = 3,  // Overlay briefly hidden to show appended duplicate, then returns to ActionMenu on DUPL
  PickPasteTrack = 4,  // After COPY/CUT: cursor on track timeline window, UP/DOWN selects track, EDIT enters PickPastePos
  PickPastePos   = 5,  // Cursor in top bar at copied region start; playhead adjustable, EDIT opens PasteMenu
  PasteMenu      = 6   // Overlay open with only "PASTE" able to be pressed
};

enum TrackEditOpId : uint8_t {
  TE_OP_SET_PUNCH   = 0,
  TE_OP_QUANTIZE    = 1,
  TE_OP_COPY        = 2,
  TE_OP_CUT         = 3,
  TE_OP_PASTE       = 4,
  TE_OP_DUPL        = 5,
  TE_OP_DEL         = 6,
  TE_OP_CLR_NOTES   = 7,
  TE_OP_CLR_CC_PB   = 8,
  TE_OP_CLR_ALL     = 9,
  TE_OP_EXPORT_CLIP = 10,
  TE_OP_LOAD_CLIP   = 11,
  TE_OP_COUNT       = 12
};

struct TrackEditContext {
  Page page;
  int track;
  uint32_t startFrame;
  uint32_t endFrame;
  uint32_t playheadFrame;
};

struct TrackEditOpDesc {
  TrackEditOpId id;
  const char *label;
  bool (*isEnabled)(const TrackEditContext &ctx);
  void (*execute)(const TrackEditContext &ctx);
};

// State queries
TrackEditPhase trackEditPhase();
bool trackEditIsActive();
bool trackEditIsModalOpen();
Page trackEditPage();
const char *trackEditHeaderTitle(Page page);

// Returns true if `track` on `page` currently has an active highlight region `[outStartFrame, outEndFrame]`
bool trackEditGetHighlight(Page page, int track,
                           uint32_t &outStartFrame, uint32_t &outEndFrame,
                           bool &outEditingEnd);

// Lifecycle & input handlers
void trackEditEnter(Page page, int track);
void trackEditToggleEndpoint();
void trackEditOnPlayheadAdjusted();
void trackEditOpenActionMenu();
void trackEditBeginPastePositioning(int targetTrack);
void trackEditOpenPasteMenu();
void trackEditCancel();
void trackEditReset();
void trackEditTick();

void trackEditModalDir(uint8_t dir);
void trackEditModalEditPress();

// Rendering & cache invalidation
void invalidateTrackEditOverlayCache();
void invalidateMidiClipBrowserCache();
void drawTrackEditOverlay(bool full);
void drawMidiClipBrowserModal(bool full);

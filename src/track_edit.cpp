// track_edit.cpp — Modular Track Timeline Region Highlighting & Editing (Audio & MIDI)
#include "track_edit.h"
#include "pages.h"
#include "page_audio.h"
#include "page_midi.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

using namespace m8;
using namespace looper;

static TrackEditPhase tePhase = TrackEditPhase::Off;
static Page tePage = Page::Audio;
static int teTrack = 0;
static uint32_t hlStartFrame = 0;
static uint32_t hlEndFrame = 0;
static bool editingEnd = false;
static int teMenuCursor = TE_OP_SET_PUNCH;
static int teQuantCursor = 0;
static int teBottomCursor = TE_OP_COPY;
static int teClearCursor = TE_OP_CLR_NOTES;

static constexpr int QUANT_OPT_COUNT = 4;
PROGMEM static const char *const QUANT_LABELS[QUANT_OPT_COUNT] = {
  "4th", "8th", "16th", "32nd"
};
PROGMEM static const uint32_t QUANT_GRID_TICKS[QUANT_OPT_COUNT] = {
  MIDI_PPQN,       // 4th  (480 ticks)
  MIDI_PPQN / 2u,  // 8th  (240 ticks)
  MIDI_PPQN / 4u,  // 16th (120 ticks)
  MIDI_PPQN / 8u   // 32nd (60 ticks)
};

// Clipboard state for COPY / CUT -> PASTE workflow (backed by SD temp files)
static bool clipValid = false;
static bool clipIsCut = false;
static Page clipPage = Page::Audio;
static int clipSourceTrack = 0;
static uint32_t clipStartFrame = 0;
static uint32_t clipEndFrame = 0;

// Reveal timer (hides overlay briefly to show duplicate or quantized events, then brings overlay back)
static constexpr uint32_t DUPL_REVEAL_MS = 420;
static constexpr uint32_t QUANT_REVEAL_MS = 1500;
static uint32_t duplRevealUntilMs = 0;
static int revealReturnCursor = TE_OP_DUPL;

// Overlay dirty-check cache keys (up to 6 visual rows: Punch, Quantize, Copy/Cut/Paste/Dupl, Clear, Export, Load)
static constexpr int TE_MAX_VIS_ROWS = 6;
static uint64_t prevTeBannerKey = 0;
static uint64_t prevTeRowKey[TE_MAX_VIS_ROWS] = {};
static uint64_t prevMidiBrowserBannerKey = 0;
static uint64_t prevMidiBrowserRowKey[MIDI_BROWSER_VISIBLE_ROWS] = {};

FLASHMEM void invalidateMidiClipBrowserCache() {
  prevMidiBrowserBannerKey = 0;
  memset(prevMidiBrowserRowKey, 0, sizeof(prevMidiBrowserRowKey));
}

FLASHMEM void invalidateTrackEditOverlayCache() {
  prevTeBannerKey = 0;
  memset(prevTeRowKey, 0, sizeof(prevTeRowKey));
  invalidateMidiClipBrowserCache();
}

FLASHMEM static uint32_t twoBarsAfterFrame(uint32_t frame, bool useBarGrid) {
  uint32_t clamped = min(frame, (uint32_t)MAX_REC_FRAMES - 1u);
  if (useBarGrid) {
    int64_t idx32 = max((int64_t)0, (int64_t)(framesToBeats(clamped, bpm) * 8.0 + 1e-6));
    return max(clamped + 1u, frameForBar32ndIndex(idx32 + 64)); // +2 bars (64 x 32nds)
  }
  uint32_t twoBars = max(1u, beatsToFrames(2 * BEATS_PER_BAR, bpm));
  return max(clamped + 1u, min((uint32_t)MAX_REC_FRAMES, clamped + twoBars));
}

FLASHMEM static uint32_t twoBarsBeforeFrame(uint32_t frame, bool useBarGrid) {
  uint32_t clamped = constrain(frame, 1u, (uint32_t)MAX_REC_FRAMES);
  if (useBarGrid) {
    int64_t idx32 = max((int64_t)1, (int64_t)(framesToBeats(clamped, bpm) * 8.0 + 1e-6));
    int64_t prev32 = max((int64_t)0, idx32 - 64); // -2 bars (64 x 32nds)
    return min(clamped - 1u, frameForBar32ndIndex(prev32));
  }
  uint32_t twoBars = max(1u, beatsToFrames(2 * BEATS_PER_BAR, bpm));
  uint32_t sub = (clamped > twoBars) ? (clamped - twoBars) : 0u;
  return min(clamped - 1u, sub);
}

FLASHMEM static bool isFrameOn32ndGrid(uint32_t frame) {
  int64_t idx32 = max((int64_t)0, (int64_t)(framesToBeats(frame, bpm) * 8.0 + 1e-6));
  uint32_t gf = frameForBar32ndIndex(idx32);
  return (frame <= gf + 1u) && (gf <= frame + 1u);
}

FLASHMEM static void discardClipboard() {
  clipValid = false;
  clipIsCut = false;
  storageRemoveClipboardFiles();
}

FLASHMEM static void placeCursorOnTrackTimeline(Page page, int track) {
  int t = constrain(track, 0, TRACK_ROWS - 1);
  if (page == Page::Midi) {
    midiRow = t;
    midiCell = MID_CELL_WAVE;
    midiRememberTrackCursor();
    syncMidiScroll();
  } else {
    audioRow = t;
    audioCell = AUD_CELL_WAVE;
    audioRememberTrackCursor();
    syncAudioScroll();
  }
}

FLASHMEM static void placeCursorInHeader(Page page) {
  headerCell = constrain(headerCell, 0, HEADER_CELL_COUNT - 1);
  if (page == Page::Midi) {
    midiRow = HEADER_ROW;
  } else {
    audioRow = HEADER_ROW;
  }
}

// ============================================================================
// Modular Track Edit Operation Handlers
// ============================================================================
FLASHMEM static bool opEnabledInActionMenu(const TrackEditContext &ctx) {
  (void)ctx;
  return tePhase == TrackEditPhase::ActionMenu || tePhase == TrackEditPhase::DuplReveal;
}

FLASHMEM static bool opQuantizeEnabled(const TrackEditContext &ctx) {
  return (ctx.page == Page::Midi) &&
         (tePhase == TrackEditPhase::ActionMenu || tePhase == TrackEditPhase::DuplReveal);
}

FLASHMEM static bool opCutEnabled(const TrackEditContext &ctx) {
  (void)ctx;
  return (tePhase == TrackEditPhase::ActionMenu || tePhase == TrackEditPhase::DuplReveal) && !clipValid;
}

FLASHMEM static bool opPasteEnabled(const TrackEditContext &ctx) {
  (void)ctx;
  return (tePhase == TrackEditPhase::PasteMenu) && clipValid;
}

FLASHMEM static void opExecSetPunch(const TrackEditContext &ctx) {
  SongSettings &ps = songSettings();
  ps.punchInFrame = min(ctx.startFrame, (uint32_t)MAX_REC_FRAMES - 1u);
  ps.punchOutFrame = constrain(max(ps.punchInFrame + 1u, ctx.endFrame), 1u, (uint32_t)MAX_REC_FRAMES);
  ps.loopEnabled = true;
  storageSyncLoopState();
  clampInfoCellSelection();
  saveSessionWhenIdle();

  discardClipboard();
  tePhase = TrackEditPhase::Off;
  if (ctx.page == Page::Midi) {
    midiRow = TRANSPORT_ROW;
    midiCell = TR_CELL_LOOP;
  } else {
    audioRow = TRANSPORT_ROW;
    audioCell = TR_CELL_LOOP;
  }
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void opExecQuantize(const TrackEditContext &ctx) {
  if (ctx.page != Page::Midi) return;
  int qIdx = constrain(teQuantCursor, 0, QUANT_OPT_COUNT - 1);
  if (ctx.endFrame > ctx.startFrame) {
    midiSeqQuantizeRegion(ctx.track, ctx.startFrame, ctx.endFrame, QUANT_GRID_TICKS[qIdx], bpm);
  }

  teMenuCursor = TE_OP_QUANTIZE;
  revealReturnCursor = TE_OP_QUANTIZE;
  tePhase = TrackEditPhase::DuplReveal;
  duplRevealUntilMs = millis() + QUANT_REVEAL_MS;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void startCopyOrCut(const TrackEditContext &ctx, bool isCut) {
  bool ok = false;
  if (ctx.page == Page::Midi) {
    ok = midiSeqCopyRegionToClip(ctx.track, ctx.startFrame, ctx.endFrame, bpm);
  } else {
    ok = storageCopyAudioRegionToClip(ctx.track, ctx.startFrame, ctx.endFrame);
  }
  if (!ok) {
    uiShowToast("CLIPBOARD ERROR");
    tePhase = TrackEditPhase::Off;
    placeCursorOnTrackTimeline(ctx.page, ctx.track);
    inputSwallowModalKeys();
    uiForceRedraw();
    return;
  }

  clipValid = true;
  clipIsCut = isCut;
  clipPage = ctx.page;
  clipSourceTrack = ctx.track;
  clipStartFrame = ctx.startFrame;
  clipEndFrame = ctx.endFrame;

  tePhase = TrackEditPhase::PickPasteTrack;
  placeCursorOnTrackTimeline(ctx.page, ctx.track);
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void opExecCopy(const TrackEditContext &ctx) {
  startCopyOrCut(ctx, false);
}

FLASHMEM static void opExecCut(const TrackEditContext &ctx) {
  if (clipValid) return;
  startCopyOrCut(ctx, true);
}

FLASHMEM static void opExecPaste(const TrackEditContext &ctx) {
  if (!clipValid) return;
  uint32_t clipFrames = (clipEndFrame > clipStartFrame) ? (clipEndFrame - clipStartFrame) : 0u;
  uint32_t pasteFrame = min(ctx.playheadFrame, (uint32_t)MAX_REC_FRAMES - 1u);

  if (clipFrames > 0) {
    if (clipPage == Page::Midi) {
      if (clipIsCut) {
        midiSeqDeleteRegion(clipSourceTrack, clipStartFrame, clipEndFrame, bpm);
      }
      midiSeqPasteClipToTrack(ctx.track, pasteFrame, clipFrames, bpm);
    } else {
      if (clipIsCut) {
        storageDeleteAudioRegion(clipSourceTrack, clipStartFrame, clipEndFrame);
      }
      storagePasteAudioClipToTrack(ctx.track, pasteFrame, clipFrames);
    }
  }

  discardClipboard();
  tePhase = TrackEditPhase::Off;
  placeCursorOnTrackTimeline(ctx.page, ctx.track);
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void opExecDupl(const TrackEditContext &ctx) {
  uint32_t lenFrames = (ctx.endFrame > ctx.startFrame) ? (ctx.endFrame - ctx.startFrame) : 0u;
  if (lenFrames == 0 || ctx.endFrame >= MAX_REC_FRAMES) {
    return;
  }

  uint32_t destStart = ctx.endFrame;
  uint32_t destEnd = min((uint32_t)MAX_REC_FRAMES, destStart + lenFrames);
  uint32_t actualFrames = destEnd - destStart;
  bool ok = false;

  if (ctx.page == Page::Midi) {
    if (midiSeqCopyRegionToClip(ctx.track, ctx.startFrame, ctx.endFrame, bpm)) {
      ok = midiSeqPasteClipToTrack(ctx.track, destStart, actualFrames, bpm);
    }
  } else {
    if (storageCopyAudioRegionToClip(ctx.track, ctx.startFrame, ctx.endFrame)) {
      ok = storagePasteAudioClipToTrack(ctx.track, destStart, actualFrames);
    }
  }
  storageRemoveClipboardFiles();

  if (ok) {
    hlStartFrame = destStart;
    hlEndFrame = destEnd;
    appSeekTape(destStart);
  }

  teMenuCursor = TE_OP_DUPL;
  revealReturnCursor = TE_OP_DUPL;
  tePhase = TrackEditPhase::DuplReveal;
  duplRevealUntilMs = millis() + DUPL_REVEAL_MS;
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static bool opAudioDelEnabled(const TrackEditContext &ctx) {
  return (ctx.page == Page::Audio) &&
         (tePhase == TrackEditPhase::ActionMenu || tePhase == TrackEditPhase::DuplReveal);
}

FLASHMEM static bool opMidiClearEnabled(const TrackEditContext &ctx) {
  return (ctx.page == Page::Midi) &&
         (tePhase == TrackEditPhase::ActionMenu || tePhase == TrackEditPhase::DuplReveal);
}

FLASHMEM static void opExecDel(const TrackEditContext &ctx) {
  if (ctx.endFrame > ctx.startFrame) {
    if (ctx.page == Page::Midi) {
      midiSeqDeleteRegion(ctx.track, ctx.startFrame, ctx.endFrame, bpm, MidiClearMode::All);
    } else {
      storageDeleteAudioRegion(ctx.track, ctx.startFrame, ctx.endFrame);
    }
  }

  discardClipboard();
  tePhase = TrackEditPhase::Off;
  placeCursorOnTrackTimeline(ctx.page, ctx.track);
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void execMidiClearRegion(const TrackEditContext &ctx, MidiClearMode mode) {
  if (ctx.page == Page::Midi && ctx.endFrame > ctx.startFrame) {
    midiSeqDeleteRegion(ctx.track, ctx.startFrame, ctx.endFrame, bpm, mode);
  }
  discardClipboard();
  tePhase = TrackEditPhase::Off;
  placeCursorOnTrackTimeline(ctx.page, ctx.track);
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void opExecClrNotes(const TrackEditContext &ctx) {
  execMidiClearRegion(ctx, MidiClearMode::Notes);
}

FLASHMEM static void opExecClrCcPb(const TrackEditContext &ctx) {
  execMidiClearRegion(ctx, MidiClearMode::CcPb);
}

FLASHMEM static void opExecClrAll(const TrackEditContext &ctx) {
  execMidiClearRegion(ctx, MidiClearMode::All);
}

FLASHMEM static void opExecExportClip(const TrackEditContext &ctx) {
  bool ok = false;
  if (ctx.page == Page::Midi && ctx.endFrame > ctx.startFrame) {
    ok = midiSeqExportClipMid(ctx.track, ctx.startFrame, ctx.endFrame, bpm);
  }
  discardClipboard();
  tePhase = TrackEditPhase::Off;
  placeCursorOnTrackTimeline(ctx.page, ctx.track);
  uiShowToast(ok ? "EXPORTED MIDI CLIP" : "CLIP EXPORT FAILED");
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM static void opExecLoadClip(const TrackEditContext &ctx) {
  if (ctx.page == Page::Midi) {
    openMidiClipBrowser(ctx.track, ctx.startFrame);
  } else if (ctx.page == Page::Audio) {
    openAudioClipBrowser(ctx.track, ctx.startFrame);
  } else {
    return;
  }
  invalidateMidiClipBrowserCache();
  inputSwallowModalKeys();
  uiForceRedraw();
}

PROGMEM static const TrackEditOpDesc TRACK_EDIT_OPS[TE_OP_COUNT] = {
  {TE_OP_SET_PUNCH,   "SET PUNCH/LOOP REGION", opEnabledInActionMenu, opExecSetPunch},
  {TE_OP_QUANTIZE,    "QUANTIZE",            opQuantizeEnabled,     opExecQuantize},
  {TE_OP_COPY,        "COPY",                opEnabledInActionMenu, opExecCopy},
  {TE_OP_CUT,         "CUT",                 opCutEnabled,          opExecCut},
  {TE_OP_PASTE,       "PASTE",               opPasteEnabled,        opExecPaste},
  {TE_OP_DUPL,        "DUPL",                opEnabledInActionMenu, opExecDupl},
  {TE_OP_DEL,         "CLR",                 opAudioDelEnabled,     opExecDel},
  {TE_OP_CLR_NOTES,   "CLR NOTES",           opMidiClearEnabled,    opExecClrNotes},
  {TE_OP_CLR_CC_PB,   "CLR CC/PB",           opMidiClearEnabled,    opExecClrCcPb},
  {TE_OP_CLR_ALL,     "CLR ALL",             opMidiClearEnabled,    opExecClrAll},
  {TE_OP_EXPORT_CLIP, "EXPORT AS MIDI CLIP", opMidiClearEnabled,    opExecExportClip},
  {TE_OP_LOAD_CLIP,   "LOAD MIDI CLIP",      opEnabledInActionMenu, opExecLoadClip}
};

FLASHMEM static TrackEditContext buildContext() {
  return TrackEditContext{
    tePage,
    teTrack,
    hlStartFrame,
    hlEndFrame,
    transport.tapeFrame
  };
}

// ============================================================================
// Public State Queries & Transitions
// ============================================================================
FLASHMEM TrackEditPhase trackEditPhase() {
  return tePhase;
}

FLASHMEM bool trackEditIsActive() {
  return tePhase != TrackEditPhase::Off;
}

FLASHMEM bool trackEditIsModalOpen() {
  return tePhase == TrackEditPhase::ActionMenu || tePhase == TrackEditPhase::PasteMenu;
}

FLASHMEM Page trackEditPage() {
  return tePage;
}

FLASHMEM const char *trackEditHeaderTitle(Page page) {
  if (tePhase == TrackEditPhase::Off || page != tePage) return nullptr;
  static char titleBuf[12];
  char prefix = (page == Page::Midi) ? 'M' : 'A';
  int trkNum = constrain(teTrack + 1, 1, 8);
  switch (tePhase) {
    case TrackEditPhase::SelectRegion:
      snprintf(titleBuf, sizeof(titleBuf), "%c%d:%s", prefix, trkNum, editingEnd ? "END" : "STRT");
      return titleBuf;
    case TrackEditPhase::PickPasteTrack:
      snprintf(titleBuf, sizeof(titleBuf), "%s:TRK", clipIsCut ? "CUT" : "CPY");
      return titleBuf;
    case TrackEditPhase::PickPastePos:
    case TrackEditPhase::PasteMenu:
      snprintf(titleBuf, sizeof(titleBuf), "%c%d:PAST", prefix, trkNum);
      return titleBuf;
    case TrackEditPhase::ActionMenu:
    case TrackEditPhase::DuplReveal:
      snprintf(titleBuf, sizeof(titleBuf), "%c%d:EDIT", prefix, trkNum);
      return titleBuf;
    default:
      return nullptr;
  }
}

FLASHMEM bool trackEditGetHighlight(Page page, int track,
                                    uint32_t &outStartFrame, uint32_t &outEndFrame,
                                    bool &outEditingEnd) {
  if (tePhase == TrackEditPhase::Off || page != tePage) return false;

  if (tePhase == TrackEditPhase::SelectRegion ||
      tePhase == TrackEditPhase::ActionMenu ||
      tePhase == TrackEditPhase::DuplReveal) {
    if (track != teTrack) return false;
    outStartFrame = hlStartFrame;
    outEndFrame = hlEndFrame;
    outEditingEnd = editingEnd;
    return true;
  }

  if (tePhase == TrackEditPhase::PickPasteTrack) {
    if (!clipValid || page != clipPage || track != clipSourceTrack) return false;
    outStartFrame = clipStartFrame;
    outEndFrame = clipEndFrame;
    outEditingEnd = false;
    return true;
  }

  if (tePhase == TrackEditPhase::PickPastePos || tePhase == TrackEditPhase::PasteMenu) {
    if (!clipValid || track != teTrack) return false;
    uint32_t dur = (clipEndFrame > clipStartFrame) ? (clipEndFrame - clipStartFrame) : 1u;
    outStartFrame = min((uint32_t)transport.tapeFrame, (uint32_t)MAX_REC_FRAMES - 1u);
    outEndFrame = min((uint32_t)MAX_REC_FRAMES, outStartFrame + dur);
    outEditingEnd = false;
    return true;
  }

  return false;
}

FLASHMEM void trackEditEnter(Page page, int track) {
  if (page != Page::Audio && page != Page::Midi) return;
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
  }
  discardClipboard();

  tePage = page;
  teTrack = constrain(track, 0, TRACK_ROWS - 1);
  hlStartFrame = min((uint32_t)transport.tapeFrame, (uint32_t)MAX_REC_FRAMES - 1u);
  bool onGrid = isFrameOn32ndGrid(hlStartFrame) || (headerCell >= HDR_CELL_BAR);
  hlEndFrame = twoBarsAfterFrame(hlStartFrame, onGrid);
  editingEnd = false;
  tePhase = TrackEditPhase::SelectRegion;

  placeCursorInHeader(page);
  uiShowToast("", 0); // ensure header bar time/bar.beats are visible immediately
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void trackEditToggleEndpoint() {
  if (tePhase != TrackEditPhase::SelectRegion) return;
  editingEnd = !editingEnd;
  appSeekTape(editingEnd ? hlEndFrame : hlStartFrame);
  uiForceRedraw();
}

FLASHMEM void trackEditOnPlayheadAdjusted() {
  if (tePhase != TrackEditPhase::SelectRegion) return;
  bool barGrid = (headerCell >= HDR_CELL_BAR);
  uint32_t tf = transport.tapeFrame;

  if (!editingEnd) {
    uint32_t nextStart = min(tf, (uint32_t)MAX_REC_FRAMES - 1u);
    hlStartFrame = nextStart;
    if (hlStartFrame >= hlEndFrame) {
      hlEndFrame = twoBarsAfterFrame(hlStartFrame, barGrid);
    }
  } else {
    uint32_t nextEnd = constrain(tf, 1u, (uint32_t)MAX_REC_FRAMES);
    hlEndFrame = nextEnd;
    if (hlEndFrame <= hlStartFrame) {
      hlStartFrame = twoBarsBeforeFrame(hlEndFrame, barGrid);
    }
  }
  uiForceRedraw();
}

FLASHMEM void trackEditOpenActionMenu() {
  if (tePhase != TrackEditPhase::SelectRegion && tePhase != TrackEditPhase::DuplReveal) return;
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
  }
  tePhase = TrackEditPhase::ActionMenu;
  TrackEditContext ctx = buildContext();
  if (teMenuCursor < 0 || teMenuCursor >= TE_OP_COUNT || !TRACK_EDIT_OPS[teMenuCursor].isEnabled(ctx)) {
    teMenuCursor = TE_OP_SET_PUNCH;
  }
  invalidateTrackEditOverlayCache();
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void trackEditBeginPastePositioning(int targetTrack) {
  if (tePhase != TrackEditPhase::PickPasteTrack || !clipValid) return;
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
  }
  teTrack = constrain(targetTrack, 0, TRACK_ROWS - 1);
  tePhase = TrackEditPhase::PickPastePos;
  appSeekTape(clipStartFrame);
  placeCursorInHeader(tePage);
  uiShowToast("", 0);
  uiForceRedraw();
}

FLASHMEM void trackEditOpenPasteMenu() {
  if (tePhase != TrackEditPhase::PickPastePos || !clipValid) return;
  if (transport.mode != TransportMode::Stopped) {
    appStopTransport();
  }
  tePhase = TrackEditPhase::PasteMenu;
  teMenuCursor = TE_OP_PASTE;
  invalidateTrackEditOverlayCache();
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void trackEditCancel() {
  closeMidiClipBrowser();
  if (tePhase == TrackEditPhase::Off) return;
  Page pg = tePage;
  int trk = teTrack;
  discardClipboard();
  tePhase = TrackEditPhase::Off;
  placeCursorOnTrackTimeline(pg, trk);
  inputSwallowModalKeys();
  uiForceRedraw();
}

FLASHMEM void trackEditReset() {
  closeMidiClipBrowser();
  discardClipboard();
  tePhase = TrackEditPhase::Off;
  invalidateTrackEditOverlayCache();
}

FLASHMEM void trackEditTick() {
  if (tePhase == TrackEditPhase::DuplReveal) {
    if ((int32_t)(millis() - duplRevealUntilMs) >= 0) {
      tePhase = TrackEditPhase::ActionMenu;
      teMenuCursor = revealReturnCursor;
      invalidateTrackEditOverlayCache();
      uiForceRedraw();
    }
  }
}

FLASHMEM void trackEditModalDir(uint8_t dir) {
  if (!trackEditIsModalOpen()) return;
  TrackEditContext ctx = buildContext();
  int cur = constrain(teMenuCursor, 0, TE_OP_COUNT - 1);

  if (tePhase == TrackEditPhase::PasteMenu) {
    teMenuCursor = TE_OP_PASTE;
    return;
  }

  const bool isMidi = (tePage == Page::Midi);
  const int dx = ((dir & BTN_RIGHT) ? 1 : 0) - ((dir & BTN_LEFT) ? 1 : 0);
  const int dy = ((dir & BTN_DOWN) ? 1 : 0) - ((dir & BTN_UP) ? 1 : 0);

  const int maxRow2Op = isMidi ? (int)TE_OP_DUPL : (int)TE_OP_DEL;

  auto pickEnabledBottomOp = [&](int preferred) -> int {
    int p = constrain(preferred, (int)TE_OP_COPY, maxRow2Op);
    if (TRACK_EDIT_OPS[p].isEnabled(ctx)) return p;
    for (int op = TE_OP_COPY; op <= maxRow2Op; op++) {
      if (TRACK_EDIT_OPS[op].isEnabled(ctx)) return op;
    }
    return TE_OP_COPY;
  };

  // Vertical navigation between rows:
  //   Row 0: SET PUNCH REGION
  //   Row 1 (MIDI only): QUANT (4th / 8th / 16th / 32nd)
  //   Row 2 (or Row 1 on Audio): COPY / CUT / PASTE / DUPL (+ CLR on Audio)
  //   Row 3 (MIDI only): CLR NOTES / CLR CC/PB / CLR ALL
  //   Row 4 (MIDI only): EXPORT AS MIDI CLIP
  //   Row 5 (or Row 2 on Audio): LOAD MIDI CLIP / LOAD AUDIO CLIP
  if (dy != 0) {
    const int numRows = isMidi ? 6 : 3;
    int curRow = 0;
    if (cur == TE_OP_SET_PUNCH) {
      curRow = 0;
    } else if (isMidi && cur == TE_OP_QUANTIZE) {
      curRow = 1;
    } else if (isMidi && cur >= TE_OP_CLR_NOTES && cur <= TE_OP_CLR_ALL) {
      curRow = 3;
    } else if (isMidi && cur == TE_OP_EXPORT_CLIP) {
      curRow = 4;
    } else if (cur == TE_OP_LOAD_CLIP) {
      curRow = isMidi ? 5 : 2;
    } else {
      curRow = isMidi ? 2 : 1;
    }
    int nextRow = (curRow + dy + numRows) % numRows;
    int nextCur = cur;
    if (nextRow == 0) {
      nextCur = TE_OP_SET_PUNCH;
    } else if (isMidi && nextRow == 1) {
      nextCur = TE_OP_QUANTIZE;
    } else if (isMidi && nextRow == 3) {
      nextCur = constrain(teClearCursor, (int)TE_OP_CLR_NOTES, (int)TE_OP_CLR_ALL);
      teClearCursor = nextCur;
    } else if (isMidi && nextRow == 4) {
      nextCur = TE_OP_EXPORT_CLIP;
    } else if ((isMidi && nextRow == 5) || (!isMidi && nextRow == 2)) {
      nextCur = TE_OP_LOAD_CLIP;
    } else {
      nextCur = pickEnabledBottomOp(teBottomCursor);
      teBottomCursor = nextCur;
    }
    if (nextCur != teMenuCursor) {
      teMenuCursor = nextCur;
      uiForceRedraw();
    }
    return;
  }

  // Horizontal navigation within the active row
  if (dx != 0) {
    if (cur == TE_OP_QUANTIZE && TRACK_EDIT_OPS[TE_OP_QUANTIZE].isEnabled(ctx)) {
      int nextQ = constrain(teQuantCursor + dx, 0, QUANT_OPT_COUNT - 1);
      if (nextQ != teQuantCursor) {
        teQuantCursor = nextQ;
        uiForceRedraw();
      }
      return;
    }
    if (cur >= TE_OP_COPY && cur <= maxRow2Op) {
      int cand = cur + dx;
      while (cand >= TE_OP_COPY && cand <= maxRow2Op) {
        if (TRACK_EDIT_OPS[cand].isEnabled(ctx)) {
          teMenuCursor = cand;
          teBottomCursor = cand;
          uiForceRedraw();
          return;
        }
        cand += dx;
      }
      return;
    }
    if (isMidi && cur >= TE_OP_CLR_NOTES && cur <= TE_OP_CLR_ALL) {
      int nextClr = constrain(cur + dx, (int)TE_OP_CLR_NOTES, (int)TE_OP_CLR_ALL);
      if (nextClr != teMenuCursor && TRACK_EDIT_OPS[nextClr].isEnabled(ctx)) {
        teMenuCursor = nextClr;
        teClearCursor = nextClr;
        uiForceRedraw();
      }
      return;
    }
  }
}

FLASHMEM void trackEditModalEditPress() {
  if (tePhase == TrackEditPhase::DuplReveal) {
    if (revealReturnCursor == TE_OP_DUPL) {
      TrackEditContext ctx = buildContext();
      opExecDupl(ctx);
    } else {
      tePhase = TrackEditPhase::ActionMenu;
      teMenuCursor = revealReturnCursor;
      invalidateTrackEditOverlayCache();
      inputSwallowModalKeys();
      uiForceRedraw();
    }
    return;
  }
  if (!trackEditIsModalOpen()) return;
  TrackEditContext ctx = buildContext();
  int cur = constrain(teMenuCursor, 0, TE_OP_COUNT - 1);
  if (cur >= TE_OP_COPY && cur <= TE_OP_DEL) {
    teBottomCursor = cur;
  } else if (cur >= TE_OP_CLR_NOTES && cur <= TE_OP_CLR_ALL) {
    teClearCursor = cur;
  }
  const TrackEditOpDesc &op = TRACK_EDIT_OPS[cur];
  if (op.isEnabled(ctx) && op.execute) {
    op.execute(ctx);
  }
}

// ============================================================================
// Track Edit Overlay Rendering (Flicker-Free Surface Strips)
// ============================================================================
FLASHMEM void drawTrackEditOverlay(bool full) {
  if (!trackEditIsModalOpen()) return;

  const bool isMidi = (tePage == Page::Midi);
  const uint16_t pageAccent = isMidi ? COLOR_MIDI : ACCENT;
  const int ovX = SX(32);
  const int ovW = SX(256);
  const int hdrH = isMidi ? SY(20) : SY(22);
  const int rowH = isMidi ? SY(22) : SY(24);
  const int numVisRows = isMidi ? 6 : 3;
  const int totalH = hdrH + numVisRows * rowH;
  const int hdrY = SY(26) + (SY(162) - totalH) / 2;

  TrackEditContext ctx = buildContext();

  // 1. Header Banner Strip
  {
    char hdrTitle[28];
    char trkPrefix = isMidi ? 'M' : 'A';
    int trkNum = constrain(teTrack + 1, 1, 8);
    if (tePhase == TrackEditPhase::PasteMenu) {
      snprintf(hdrTitle, sizeof(hdrTitle), "PASTE -> %s %c%d",
               isMidi ? "MIDI" : "AUDIO", trkPrefix, trkNum);
    } else {
      snprintf(hdrTitle, sizeof(hdrTitle), "%s %c%d TRACK EDIT",
               isMidi ? "MIDI" : "AUDIO", trkPrefix, trkNum);
    }

    uint64_t bk = hashStr(hashInit(), hdrTitle);
    bk = hashMix(bk, (uint64_t)pageAccent | ((uint64_t)tePhase << 16));
    if (full || bk != prevTeBannerKey) {
      prevTeBannerKey = bk;
      Surface s(ovW, hdrH);
      s.fillRect(0, 0, ovW, hdrH, DARK);
      s.fillRect(0, 0, ovW, PX, pageAccent);
      s.fillRect(0, 0, PX, hdrH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, hdrH, pageAccent);
      s.fillRect(PX, hdrH - 1, ovW - 2 * PX, 1, MID);
      int tw = textWidth(hdrTitle);
      s.drawText(max(SX(6), (ovW - tw) / 2), TY(hdrH), hdrTitle, pageAccent);
      s.blit(ovX, hdrY);
    }
  }

  const int pw = ovW - SX(20);
  const int ph = rowH - SY(4);
  const int px = SX(10);
  const int gap = SX(4);
  const int colW = (pw - 4 * gap) / 5; // 5 equal column slots across the row

  // 2. Row 0: [ SET PUNCH REGION ]
  {
    const TrackEditOpDesc &op = TRACK_EDIT_OPS[TE_OP_SET_PUNCH];
    const bool enabled = op.isEnabled(ctx);
    const bool sel = enabled && (teMenuCursor == TE_OP_SET_PUNCH);
    const int ry = hdrY + hdrH;

    uint16_t pillBg = !enabled ? BG : (sel ? pageAccent : PANEL);
    uint16_t pillFg = !enabled ? MID : (sel ? BG : INK);
    uint16_t pillBd = !enabled ? DARK : (sel ? INK : MID);

    uint64_t rk = hashStr(hashInit(), op.label);
    rk = hashMix(rk, (uint64_t)pillBg | ((uint64_t)pillFg << 16) |
                     ((uint64_t)pillBd << 32) | ((uint64_t)enabled << 48) |
                     ((uint64_t)sel << 49));
    if (full || rk != prevTeRowKey[0]) {
      prevTeRowKey[0] = rk;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);
      const int py = max(1, (rowH - ph) / 2);
      s.drawPill(px, py, pw, ph, op.label, pillFg, pillBg, pillBd);
      s.blit(ovX, ry);
    }
  }

  // 3. Row 1 (MIDI only): QUANT  [ 4th ] [ 8th ] [ 16th ] [ 32nd ]
  if (isMidi) {
    const TrackEditOpDesc &op = TRACK_EDIT_OPS[TE_OP_QUANTIZE];
    const bool enabled = op.isEnabled(ctx);
    const bool sel = enabled && (teMenuCursor == TE_OP_QUANTIZE);
    const int qCur = constrain(teQuantCursor, 0, QUANT_OPT_COUNT - 1);
    const int ry = hdrY + hdrH + rowH;

    uint64_t rk = hashStr(hashInit(), "QUANT");
    rk = hashMix(rk, (uint64_t)pageAccent | ((uint64_t)enabled << 16) |
                     ((uint64_t)sel << 17) | ((uint64_t)qCur << 20));
    if (full || rk != prevTeRowKey[1]) {
      prevTeRowKey[1] = rk;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);

      const int py = max(1, (rowH - ph) / 2);
      uint16_t lblFg = !enabled ? MID : (sel ? pageAccent : INK);
      s.drawText(px + SX(2), TY(rowH), "QUANT", lblFg);

      for (int q = 0; q < QUANT_OPT_COUNT; q++) {
        const bool qSel = sel && (q == qCur);
        const uint16_t qBg = !enabled ? BG : (qSel ? pageAccent : PANEL);
        const uint16_t qFg = !enabled ? MID : (qSel ? BG : INK);
        const uint16_t qBd = !enabled ? DARK : (qSel ? INK : MID);
        const int bx = px + (q + 1) * (colW + gap);
        const int bw = (q == QUANT_OPT_COUNT - 1) ? (px + pw - bx) : colW;
        s.drawPill(bx, py, bw, ph, QUANT_LABELS[q], qFg, qBg, qBd);
      }
      s.blit(ovX, ry);
    }
  }

  // 4. Row 2 (MIDI: COPY / CUT / PASTE / DUPL; Audio: COPY / CUT / PASTE / DUPL / CLR)
  {
    const int visRow = isMidi ? 2 : 1;
    const int ry = hdrY + hdrH + visRow * rowH;
    const int numCols = isMidi ? 4 : 5;
    const int maxOp = TE_OP_COPY + numCols - 1;
    const int rowColW = (pw - (numCols - 1) * gap) / numCols;

    uint64_t rk = hashInit();
    rk = hashMix(rk, (uint64_t)pageAccent | ((uint64_t)teMenuCursor << 16) | ((uint64_t)tePhase << 24));
    for (int opIdx = TE_OP_COPY; opIdx <= maxOp; opIdx++) {
      bool opEn = TRACK_EDIT_OPS[opIdx].isEnabled(ctx);
      bool opSel = opEn && (teMenuCursor == opIdx);
      rk = hashMix(rk, (uint64_t)opIdx | ((uint64_t)opEn << 8) | ((uint64_t)opSel << 9));
    }

    if (full || rk != prevTeRowKey[2]) {
      prevTeRowKey[2] = rk;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);

      const int py = max(1, (rowH - ph) / 2);
      for (int col = 0; col < numCols; col++) {
        const int opIdx = TE_OP_COPY + col;
        const TrackEditOpDesc &op = TRACK_EDIT_OPS[opIdx];
        const bool opEn = op.isEnabled(ctx);
        const bool opSel = opEn && (teMenuCursor == opIdx);

        uint16_t pBg = !opEn ? BG : (opSel ? pageAccent : PANEL);
        uint16_t pFg = !opEn ? MID : (opSel ? BG : INK);
        uint16_t pBd = !opEn ? DARK : (opSel ? INK : MID);
        if (opSel && (opIdx == TE_OP_DEL || opIdx == TE_OP_CUT)) {
          pBg = COLOR_WARN;
        }

        const int bx = px + col * (rowColW + gap);
        const int bw = (col == numCols - 1) ? (px + pw - bx) : rowColW;
        s.drawPill(bx, py, bw, ph, op.label, pFg, pBg, pBd);
      }
      s.blit(ovX, ry);
    }
  }

  // 5. Row 3 (MIDI only): [ CLR NOTES ] [ CLR CC/PB ] [ CLR ALL ]
  if (isMidi) {
    const int ry = hdrY + hdrH + 3 * rowH;
    const int numCols = 3;
    const int rowColW = (pw - (numCols - 1) * gap) / numCols;

    uint64_t rk = hashInit();
    rk = hashMix(rk, (uint64_t)pageAccent | ((uint64_t)teMenuCursor << 16) | ((uint64_t)tePhase << 24));
    for (int opIdx = TE_OP_CLR_NOTES; opIdx <= TE_OP_CLR_ALL; opIdx++) {
      bool opEn = TRACK_EDIT_OPS[opIdx].isEnabled(ctx);
      bool opSel = opEn && (teMenuCursor == opIdx);
      rk = hashMix(rk, (uint64_t)opIdx | ((uint64_t)opEn << 8) | ((uint64_t)opSel << 9));
    }

    if (full || rk != prevTeRowKey[3]) {
      prevTeRowKey[3] = rk;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);

      const int py = max(1, (rowH - ph) / 2);
      for (int col = 0; col < numCols; col++) {
        const int opIdx = TE_OP_CLR_NOTES + col;
        const TrackEditOpDesc &op = TRACK_EDIT_OPS[opIdx];
        const bool opEn = op.isEnabled(ctx);
        const bool opSel = opEn && (teMenuCursor == opIdx);

        uint16_t pBg = !opEn ? BG : (opSel ? COLOR_WARN : PANEL);
        uint16_t pFg = !opEn ? MID : (opSel ? BG : INK);
        uint16_t pBd = !opEn ? DARK : (opSel ? INK : MID);

        const int bx = px + col * (rowColW + gap);
        const int bw = (col == numCols - 1) ? (px + pw - bx) : rowColW;
        s.drawPill(bx, py, bw, ph, op.label, pFg, pBg, pBd);
      }
      s.blit(ovX, ry);
    }

    // 6. Row 4 (MIDI only): [ EXPORT AS MIDI CLIP ]
    const TrackEditOpDesc &expOp = TRACK_EDIT_OPS[TE_OP_EXPORT_CLIP];
    const bool expEn = expOp.isEnabled(ctx);
    const bool expSel = expEn && (teMenuCursor == TE_OP_EXPORT_CLIP);
    const int ryExp = hdrY + hdrH + 4 * rowH;

    uint16_t expBg = !expEn ? BG : (expSel ? pageAccent : PANEL);
    uint16_t expFg = !expEn ? MID : (expSel ? BG : INK);
    uint16_t expBd = !expEn ? DARK : (expSel ? INK : MID);

    uint64_t expKey = hashStr(hashInit(), expOp.label);
    expKey = hashMix(expKey, (uint64_t)expBg | ((uint64_t)expFg << 16) |
                             ((uint64_t)expBd << 32) | ((uint64_t)expEn << 48) |
                             ((uint64_t)expSel << 49));
    if (full || expKey != prevTeRowKey[4]) {
      prevTeRowKey[4] = expKey;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);

      const int py = max(1, (rowH - ph) / 2);
      s.drawPill(px, py, pw, ph, expOp.label, expFg, expBg, expBd);
      s.blit(ovX, ryExp);
    }
  }

  // 7. Bottom Row: [ LOAD MIDI CLIP ] (on MIDI) or [ LOAD AUDIO CLIP ] (on Audio)
  {
    const TrackEditOpDesc &loadOp = TRACK_EDIT_OPS[TE_OP_LOAD_CLIP];
    const char *loadLbl = isMidi ? "LOAD MIDI CLIP" : "LOAD AUDIO CLIP";
    const bool loadEn = loadOp.isEnabled(ctx);
    const bool loadSel = loadEn && (teMenuCursor == TE_OP_LOAD_CLIP);
    const int ryLoad = hdrY + hdrH + (isMidi ? 5 : 2) * rowH;

    uint16_t loadBg = !loadEn ? BG : (loadSel ? pageAccent : PANEL);
    uint16_t loadFg = !loadEn ? MID : (loadSel ? BG : INK);
    uint16_t loadBd = !loadEn ? DARK : (loadSel ? INK : MID);

    uint64_t loadKey = hashStr(hashInit(), loadLbl);
    loadKey = hashMix(loadKey, (uint64_t)loadBg | ((uint64_t)loadFg << 16) |
                               ((uint64_t)loadBd << 32) | ((uint64_t)loadEn << 48) |
                               ((uint64_t)loadSel << 49));
    if (full || loadKey != prevTeRowKey[5]) {
      prevTeRowKey[5] = loadKey;
      Surface s(ovW, rowH);
      s.fillRect(0, 0, ovW, rowH, PANEL);
      s.fillRect(0, 0, PX, rowH, pageAccent);
      s.fillRect(ovW - PX, 0, PX, rowH, pageAccent);
      s.fillRect(0, rowH - PX, ovW, PX, pageAccent);

      const int py = max(1, (rowH - ph) / 2 - (PX / 2));
      s.drawPill(px, py, pw, ph, loadLbl, loadFg, loadBg, loadBd);
      s.blit(ovX, ryLoad);
    }
  }
}

FLASHMEM void drawMidiClipBrowserModal(bool full) {
  if (!isMidiClipBrowserOpen()) return;

  const bool isAudio = isAudioClipBrowserMode();
  const uint16_t accentCol = isAudio ? ACCENT : COLOR_MIDI;
  const int trk = constrain(midiClipBrowserTargetTrack() + 1, 1, 8);
  const char *curDir = midiClipBrowserCurrentDir();

  // 1. Header Banner: [M1 LOAD:] / [A1 LOAD:] and current directory path
  {
    char leftTxt[20];
    snprintf(leftTxt, sizeof(leftTxt), "%c%d LOAD:", isAudio ? 'A' : 'M', trk);

    const char *dirDisp = curDir;
    int maxDirW = SX(308) - SX(16) - textWidth(leftTxt);
    while (*dirDisp && textWidth(dirDisp) > maxDirW) {
      dirDisp++;
    }

    uint64_t bk = hashStr(hashInit(), leftTxt);
    bk = hashStr(bk, dirDisp);
    if (full || bk != prevMidiBrowserBannerKey) {
      prevMidiBrowserBannerKey = bk;
      const int bw = SX(308), bh = SY(22);
      Surface s(bw, bh);
      s.drawBox(DARK, accentCol);
      s.drawText(SX(6), TY(bh), leftTxt, accentCol);
      int dw = textWidth(dirDisp);
      s.drawText(max(SX(68), bw - SX(6) - dw), TY(bh), dirDisp, LIGHT);
      s.blit(SX(6), SY(26));
    }
  }

  // 2. Scrollable 7-Row Directory & .MID/.WAV File List
  const int total = midiClipBrowserEntryCount();
  const int cur = midiClipBrowserCursor();
  const int scroll = midiClipBrowserScroll();

  for (int r = 0; r < MIDI_BROWSER_VISIBLE_ROWS; r++) {
    const int y = SY(50) + r * SY(26);
    const int rw = SX(308), rh = SY(24);
    const int idx = scroll + r;
    const MidiBrowserEntry *ent = midiClipBrowserEntryAt(idx);
    const bool sel = (ent != nullptr) && (idx == cur);

    uint16_t bg = sel ? DARK : PANEL;
    uint16_t border = sel ? accentCol : MID;
    uint16_t barCol = !ent ? DIM : (sel ? accentCol : (ent->isDir ? ACCENT : COLOR_PLAY));

    uint64_t rk = hashStr(hashInit(), ent ? ent->name : "");
    rk = hashMix(rk, (uint64_t)bg | ((uint64_t)border << 16) | ((uint64_t)barCol << 32) |
                     ((uint64_t)sel << 48) | ((uint64_t)r << 52) |
                     ((uint64_t)(ent ? ent->isDir : 2) << 56) | ((uint64_t)(total <= 1) << 58) |
                     ((uint64_t)isAudio << 60));
    if (full || rk != prevMidiBrowserRowKey[r]) {
      prevMidiBrowserRowKey[r] = rk;
      char rowTxt[68] = "";
      const char *pillLbl = nullptr;
      if (ent) {
        if (ent->isDir) {
          if (strcmp(ent->name, "..") == 0) {
            snprintf(rowTxt, sizeof(rowTxt), "[..]  PARENT FOLDER");
          } else {
            snprintf(rowTxt, sizeof(rowTxt), "[DIR] %.56s", ent->name);
          }
          pillLbl = "OPEN";
        } else {
          snprintf(rowTxt, sizeof(rowTxt), "%.62s", ent->name);
          pillLbl = "LOAD";
        }
      } else if (total <= 1 && r == 1) {
        snprintf(rowTxt, sizeof(rowTxt), isAudio ? "NO .WAV FILES IN FOLDER" : "NO .MID FILES IN FOLDER");
      }
      int maxRowTxtW = rw - (sel ? SX(64) : SX(16));
      while (textWidth(rowTxt) > maxRowTxtW && strlen(rowTxt) > 4) {
        rowTxt[strlen(rowTxt) - 1] = '\0';
      }

      Surface s(rw, rh);
      s.drawBox(bg, border);
      s.fillRect(PX, PX, SX(3), rh - 2 * PX, barCol);

      if (rowTxt[0] != '\0') {
        uint16_t fg = !ent ? DIM : (sel ? INK : (ent->isDir ? ACCENT : LIGHT));
        s.drawText(SX(10), TY(rh), rowTxt, fg);
      }

      if (sel && pillLbl) {
        const int pw = SX(48), ph = SY(16);
        const int px = rw - SX(6) - pw, py = (rh - ph) / 2;
        s.drawPill(px, py, pw, ph, pillLbl, BG, accentCol, INK);
      }
      s.blit(SX(6), y);
    }
    keepStreaming();
  }
}

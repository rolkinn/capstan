// page_song_settings.cpp — Song Settings Page UI & Input
#include "page_song_settings.h"
#include "page_system_settings.h"
#include "midi_in.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>

using namespace m8;
using namespace looper;

static constexpr int PSET_VISIBLE_ROWS = 7;
static uint64_t prevSongSettingsKey[PSET_VISIBLE_ROWS] = {};
static int songSetCol = 0;
static int songSetScroll = 0;

FLASHMEM static int songSettingColCount(int row) {
  if (row == PSET_REC_SRC) return 3;
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

FLASHMEM static const char *songSettingsHeaderTitle() {
  return "SONG";
}

FLASHMEM static void songSettingsInvalidateCache() {
  memset(prevSongSettingsKey, 0, sizeof(prevSongSettingsKey));
  invalidateSysPresetsModalCache();
}

FLASHMEM static void songSettingsDrawPage(bool full) {
  if (isSysPresetsModalOpen()) {
    drawSysPresetsModal(full);
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
  PROGMEM static const char *const LABELS[PSET_COUNT] = {
    "REC LINE/USB/INSTR",
    "MONITOR LINE/USB",
    "REC TRIG THRESH",
    "MIDI CLOCK OUT",
    "MIDI CLOCK SOURCE",
    "MIDI TRANSPORT IN",
    "MIDI THRU",
    "SONG INSTRUMENTS",
    "EXPORT MIDI TRACKS",
    "EXPORT MASTER OUT"
  };

  for (int vis = 0; vis < PSET_VISIBLE_ROWS; vis++) {
    int i = songSetScroll + vis;
    int y = SY(28) + vis * SY(26);
    bool sel = (songSetRow == i);

    if (i == PSET_REC_SRC) {
      const char *vals[3] = {
        ps.recLineIn ? "ON" : "OFF",
        ps.recUsbIn ? "ON" : "OFF",
        ps.recInternalInstr ? "ON" : "OFF"
      };
      const bool offs[3] = {
        !ps.recLineIn,
        !ps.recUsbIn,
        !ps.recInternalInstr
      };
      drawMultiToggleSettingsRow(y, (uint32_t)i, LABELS[i], 3, vals, offs,
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
      drawMultiToggleSettingsRow(y, (uint32_t)i, LABELS[i], 2, vals, offs,
                                 sel, songSetCol, prevSongSettingsKey[vis], full);
      keepStreaming();
      continue;
    }

    char val[36];
    val[0] = '\0';
    bool isOff = false;
    bool isPill = false;

    switch (i) {
      case PSET_DEF_THRESH:
        snprintf(val, sizeof(val), "%d DB", (int)ps.defaultThreshDb);
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

    drawSettingsTableRow(y, (uint32_t)i, LABELS[i], val, sel, isOff, isPill,
                         prevSongSettingsKey[vis], full);
    keepStreaming();
  }
}

FLASHMEM static void songSettingsOnShiftNavigate(uint8_t dir) {
  if (dir & BTN_LEFT) setPage(Page::SystemSettings);
  else if (dir & BTN_RIGHT) setPage(Page::Main);
  else if (dir & BTN_DOWN) setPage(lastRow2Page());
}

FLASHMEM static void songSettingsOnOptAdjust(int dx, int dy, int delta) {
  (void)dx;
  (void)dy;
  SongSettings &ps = songSettings();
  switch (songSetRow) {
    case PSET_REC_SRC: {
      int col = constrain(songSetCol, 0, 2);
      if (col == 0) ps.recLineIn = !ps.recLineIn;
      else if (col == 1) ps.recUsbIn = !ps.recUsbIn;
      else ps.recInternalInstr = !ps.recInternalInstr;
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
    case PSET_SAVED_INST:
    case PSET_EXPORT_MIDI:
    case PSET_EXPORT_AUDIO:
      return;
  }
  uiForceRedraw();
}

FLASHMEM static void songSettingsOnCursorMove(int dx, int dy, uint8_t dir) {
  (void)dir;
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
  (void)shiftHeld;
  if (songSetRow == PSET_SAVED_INST) {
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
  if (songSetRow == PSET_SAVED_INST || songSetRow == PSET_EXPORT_MIDI || songSetRow == PSET_EXPORT_AUDIO) return;
  SongSettings &ps = songSettings();
  switch (songSetRow) {
    case PSET_REC_SRC: {
      int col = constrain(songSetCol, 0, 2);
      if (col == 0) ps.recLineIn = true;
      else if (col == 1) ps.recUsbIn = false;
      else ps.recInternalInstr = true;
      applySongSettings();
      break;
    }
    case PSET_MONITOR: {
      int col = constrain(songSetCol, 0, 1);
      if (col == 0) ps.lineMonitor = MonitorMode::On;
      else ps.usbMonitor = MonitorMode::Off;
      applySongSettings();
      break;
    }
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

// midi_map.cpp — Configurable Global & Per-Instrument MIDI CC Mappings & MIDI Learn
#include "midi_map.h"
#include "app.h"
#include "ui.h"
#include "input.h"
#include "master_fx.h"
#include "midi_in.h"
#include "track_edit.h"
#include "instruments.h"
#include "inst_fm2op.h"
#include "inst_phasedist.h"
#include "inst_sampler.h"
#include "inst_noisedrone.h"
#include <Arduino.h>
#include <SdFat.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace looper {

// Global MIDI mapping table (persisted in /Capstan/SYSTEM.DAT)
static MidiCcMapEntry globalMap[MIDI_TARGET_COUNT] = {};

// Per-Instrument MIDI mapping table (persisted in song INST.DAT & .FMI preset files)
static MidiCcMapEntry instMap[NUM_INSTRUMENTS][INST_MIDI_MAP_TOTAL_SLOTS] = {};
static MidiCcMapEntry clipboardInstMap[INST_MIDI_MAP_TOTAL_SLOTS] = {};
static uint16_t instMappedCcChMask[128] = {};
static uint32_t instMappedCcMask[NUM_INSTRUMENTS][4] = {};
static volatile bool instMapParamDirty = false;

// O(1) ISR-safe lookup bitmask: bit (ch - 1) set when (ch, cc) is mapped to any target
static uint16_t mappedCcChMask[128] = {};
static bool midiMapDirty = false;
static uint32_t midiMapDirtyAtMs = 0;

// Per-target last seen CC value (0xFF = none seen yet) and last trigger timestamp (ms)
// for clean momentary/trigger edge detection on button targets
static uint8_t lastSeenVal[MIDI_TARGET_COUNT] = {};
static uint32_t lastTrigMs[MIDI_TARGET_COUNT] = {};

// MIDI Learn state & 1-second post-learn recognition delay
static volatile bool learningActive = false;
static int learningTargetId = 0;
static volatile bool instLearningActive = false;
static int instLearningInstIdx = 0;
static int instLearningSlotIdx = 0;
static bool learnIgnoreActive = false;
static uint32_t learnIgnoreUntilMs = 0;

// Held REW / FF scrub & double-tap state (active when Punch/Loop are disabled)
static constexpr uint32_t MIDI_REW_FF_TAP_HOLD_MS = 300u;
static int midiScrubDir = 0; // -1 = REW, +1 = FF, 0 = idle
static uint32_t midiScrubStartMs = 0;
static uint32_t midiScrubLastTickMs = 0;
static uint32_t midiScrubOriginFrame = 0;
static bool midiScrubWasDoubleTap = false;
static float midiScrubFracFrames = 0.0f;
static int lastRewFfTapDir = 0;
static uint32_t lastRewFfTapMs = 0;
static uint32_t lastRewFfTapFrame = 0;

// Held Navigation key repeat (>= 300 ms), single tap (< 50 ms), & double-tap (< 200 ms) state
static constexpr uint32_t MIDI_NAV_TAP_MAX_MS    = 50u;
static constexpr uint32_t MIDI_NAV_DOUBLE_TAP_MS = 200u;
static constexpr uint32_t MIDI_NAV_HOLD_START_MS = 300u;
static uint8_t midiNavDirBit = 0;
static uint32_t midiNavStartMs = 0;
static uint32_t midiNavNextRepeatMs = 0;
static bool midiNavWasDoubleTap = false;
static uint8_t lastNavTapDirBit = 0;
static uint32_t lastNavTapMs = 0;

// Mapped EDIT key state (single tap = EDIT, double-tap < 200 ms = OPT+EDIT, held >= 64 = held EDIT)
static bool midiEditIsDown = false;
static uint32_t midiEditPressMs = 0;
static bool midiEditPendingSingleTap = false;
static uint32_t midiEditPendingTapAtMs = 0;
static bool midiEditHoldStarted = false;

PROGMEM static const uint8_t NAV_DIR_BITS[4] = {
  m8::BTN_UP,
  m8::BTN_DOWN,
  m8::BTN_LEFT,
  m8::BTN_RIGHT
};

static inline void stopMidiScrubIfTarget(int targetId) {
  if ((targetId == MIDI_TGT_REW && midiScrubDir < 0) ||
      (targetId == MIDI_TGT_FF && midiScrubDir > 0)) {
    midiScrubDir = 0;
    midiScrubFracFrames = 0.0f;
  }
  if ((targetId == MIDI_TGT_REW && lastRewFfTapDir < 0) ||
      (targetId == MIDI_TGT_FF && lastRewFfTapDir > 0)) {
    lastRewFfTapDir = 0;
  }
  if (targetId >= MIDI_TGT_NAV_UP && targetId <= MIDI_TGT_NAV_RIGHT) {
    uint8_t dirBit = NAV_DIR_BITS[targetId - MIDI_TGT_NAV_BASE];
    if (midiNavDirBit == dirBit) midiNavDirBit = 0;
    if (lastNavTapDirBit == dirBit) lastNavTapDirBit = 0;
  } else if (targetId == MIDI_TGT_NAV_EDIT) {
    midiEditIsDown = false;
    midiEditPendingSingleTap = false;
    midiEditHoldStarted = false;
    inputFinishMidiEditHold();
  }
}

PROGMEM static const char *const TRANSPORT_TARGET_NAMES[11] = {
  "PLAY / STOP",
  "PLAY",
  "STOP",
  "REC / OVERDUB",
  "DISARM ALL",
  "PUNCH ENABLE",
  "LOOP ENABLE",
  "RETURN TO ZERO",
  "REWIND",
  "FAST FORWARD",
  "PLAYBACK SPEED"
};

PROGMEM static const char *const AUD_OFFSET_NAMES[MIDI_TGT_AUD_PER_TRK] = {
  "ARM",
  "MUTE",
  "SOLO",
  "VOL",
  "BAL"
};

PROGMEM static const char *const MID_OFFSET_NAMES[MIDI_TGT_MID_PER_TRK] = {
  "ARM",
  "MUTE",
  "SOLO"
};

PROGMEM static const char *const MASTER_TARGET_NAMES[9] = {
  "MASTER VOLUME",
  "MASTER CHORUS",
  "MASTER DELAY",
  "MASTER D.FB",
  "MASTER REVERB",
  "MASTER TAPE",
  "MASTER COMP",
  "SONG TEMPO BPM",
  "METRONOME MODE"
};

PROGMEM static const char *const NAV_TARGET_NAMES[5] = {
  "UP",
  "DOWN",
  "LEFT",
  "RIGHT",
  "EDIT"
};

PROGMEM static const char *const CATEGORY_LABELS[MIDI_CAT_COUNT] = {
  "CAT: TRANSPORT",
  "CAT: AUDIO TRK",
  "CAT: MIDI TRK",
  "CAT: MASTER/BPM",
  "CAT: NAVIGATION"
};

FLASHMEM void midiMapRebuildMask() {
  uint16_t mask[128] = {};
  for (int i = 0; i < MIDI_TARGET_COUNT; i++) {
    uint8_t ch = globalMap[i].channel;
    uint8_t cc = globalMap[i].cc & 0x7F;
    if (ch != MIDI_MAP_CH_OFF) {
      mask[cc] |= (ch == MIDI_MAP_CH_ALL) ? 0xFFFFu : (uint16_t)(1u << ((ch - 1) & 15));
    }
  }
  uint32_t irq = saveIrq();
  memcpy(mappedCcChMask, mask, sizeof(mappedCcChMask));
  restoreIrq(irq);
}

FLASHMEM void midiMapFlushSaveIfDirty() {
  if (!midiMapDirty) return;
  midiMapDirty = false;
  saveSystemSettings();
}

FLASHMEM static void markMidiMapDirty() {
  midiMapDirty = true;
  midiMapDirtyAtMs = millis();
}

FLASHMEM void midiMapResetGlobal() {
  memset(globalMap, 0, sizeof(globalMap));
  memset(lastSeenVal, 0xFF, sizeof(lastSeenVal));
  midiMapRebuildMask();
  midiScrubDir = 0;
  midiScrubFracFrames = 0.0f;
  lastRewFfTapDir = 0;
  midiNavDirBit = 0;
  lastNavTapDirBit = 0;
  midiEditIsDown = false;
  midiEditPendingSingleTap = false;
  midiEditHoldStarted = false;
  inputFinishMidiEditHold();
}

FLASHMEM void midiMapInit() {
  midiMapResetGlobal();
  instMidiMapResetAll();
  midiMapDirty = false;
  learningActive = false;
  instLearningActive = false;
  learnIgnoreActive = false;
  learnIgnoreUntilMs = 0;
}

const MidiCcMapEntry *midiMapGlobalTable() { return globalMap; }
MidiCcMapEntry *midiMapGlobalTableMut() { return globalMap; }

MidiCcMapEntry midiMapEffective(int targetId) {
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT) {
    return MidiCcMapEntry{MIDI_MAP_CH_OFF, 0};
  }
  return globalMap[targetId];
}

FLASHMEM void midiMapSetEntry(int targetId, uint8_t channel, uint8_t cc) {
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT) return;
  cc &= 0x7F;
  if (channel > MIDI_MAP_CH_ALL) channel = MIDI_MAP_CH_OFF;
  globalMap[targetId].channel = channel;
  globalMap[targetId].cc = cc;
  lastSeenVal[targetId] = 0xFF;
  stopMidiScrubIfTarget(targetId);
  midiMapRebuildMask();
  markMidiMapDirty();
}

FLASHMEM void midiMapClearEntry(int targetId) {
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT) return;
  globalMap[targetId].channel = MIDI_MAP_CH_OFF;
  globalMap[targetId].cc = 0;
  lastSeenVal[targetId] = 0xFF;
  stopMidiScrubIfTarget(targetId);
  midiMapRebuildMask();
  markMidiMapDirty();
}

FLASHMEM void midiMapClearCategory(MidiMapCategory cat) {
  int first = 0, count = 0;
  midiMapCategoryBounds(cat, first, count);
  for (int i = 0; i < count; i++) {
    int tid = first + i;
    if (tid >= 0 && tid < MIDI_TARGET_COUNT) {
      globalMap[tid].channel = MIDI_MAP_CH_OFF;
      globalMap[tid].cc = 0;
      lastSeenVal[tid] = 0xFF;
      stopMidiScrubIfTarget(tid);
    }
  }
  midiMapRebuildMask();
  midiMapDirty = false;
  saveSystemSettings();
}

FLASHMEM void midiMapClearAll() {
  midiMapResetGlobal();
  midiMapDirty = false;
  saveSystemSettings();
}

// UI channel order (18 steps): 0 = OFF (0), 1 = ALL (17), 2..17 = C01..C16 (1..16)
FLASHMEM static int channelToUiIdx(uint8_t ch) {
  if (ch == MIDI_MAP_CH_OFF) return 0;
  if (ch == MIDI_MAP_CH_ALL) return 1;
  return 1 + constrain((int)ch, 1, 16);
}

FLASHMEM static uint8_t uiIdxToChannel(int idx) {
  idx = constrain(idx, 0, 17);
  if (idx == 0) return MIDI_MAP_CH_OFF;
  if (idx == 1) return MIDI_MAP_CH_ALL;
  return (uint8_t)(idx - 1);
}

FLASHMEM void midiMapStepChannel(int targetId, int delta) {
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT || delta == 0) return;
  MidiCcMapEntry &ent = globalMap[targetId];
  uint8_t prevCh = ent.channel;
  int curIdx = channelToUiIdx(ent.channel);
  int nextIdx = constrain(curIdx + delta, 0, 17);
  uint8_t nextCh = uiIdxToChannel(nextIdx);
  if (nextCh == prevCh) return;
  midiMapSetEntry(targetId, nextCh, ent.cc);
}

FLASHMEM void midiMapStepCc(int targetId, int delta) {
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT || delta == 0) return;
  MidiCcMapEntry &ent = globalMap[targetId];
  uint8_t ch = (ent.channel == MIDI_MAP_CH_OFF) ? MIDI_MAP_CH_ALL : ent.channel;
  int nextCc = constrain((int)ent.cc + delta, 0, 127);
  midiMapSetEntry(targetId, ch, (uint8_t)nextCc);
}

FLASHMEM const char *midiMapCategoryLabel(MidiMapCategory cat) {
  int idx = constrain((int)cat, 0, (int)MIDI_CAT_COUNT - 1);
  return CATEGORY_LABELS[idx];
}

FLASHMEM void midiMapCategoryBounds(MidiMapCategory cat, int &outFirstTarget, int &outCount) {
  switch (cat) {
    case MIDI_CAT_TRANSPORT:
      outFirstTarget = 0;
      outCount = MIDI_TGT_AUD_BASE; // 11
      break;
    case MIDI_CAT_AUDIO:
      outFirstTarget = MIDI_TGT_AUD_BASE;
      outCount = NUM_AUDIO_TRACKS * MIDI_TGT_AUD_PER_TRK; // 40
      break;
    case MIDI_CAT_MIDI:
      outFirstTarget = MIDI_TGT_MID_BASE;
      outCount = NUM_MIDI_TRACKS * MIDI_TGT_MID_PER_TRK; // 24
      break;
    case MIDI_CAT_MASTER:
      outFirstTarget = MIDI_TGT_MASTER_BASE;
      outCount = MIDI_TGT_NAV_BASE - MIDI_TGT_MASTER_BASE; // 9
      break;
    case MIDI_CAT_NAVIGATION:
    default:
      outFirstTarget = MIDI_TGT_NAV_BASE;
      outCount = MIDI_TARGET_COUNT - MIDI_TGT_NAV_BASE; // 4
      break;
  }
}

FLASHMEM void midiMapFormatTargetName(int targetId, char *out, int outSz) {
  if (!out || outSz <= 0) return;
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT) {
    out[0] = '\0';
    return;
  }
  if (targetId < MIDI_TGT_AUD_BASE) {
    snprintf(out, (size_t)outSz, "%s", TRANSPORT_TARGET_NAMES[targetId]);
    return;
  }
  if (targetId < MIDI_TGT_MID_BASE) {
    int rel = targetId - MIDI_TGT_AUD_BASE;
    int trk = rel / MIDI_TGT_AUD_PER_TRK;
    int off = rel % MIDI_TGT_AUD_PER_TRK;
    if (off == MIDI_AUD_OFF_PAN && isAudioTrackMono(trk)) {
      snprintf(out, (size_t)outSz, "AUD %02d PAN", trk + 1);
    } else {
      snprintf(out, (size_t)outSz, "AUD %02d %s", trk + 1, AUD_OFFSET_NAMES[off]);
    }
    return;
  }
  if (targetId < MIDI_TGT_MASTER_BASE) {
    int rel = targetId - MIDI_TGT_MID_BASE;
    int trk = rel / MIDI_TGT_MID_PER_TRK;
    int off = rel % MIDI_TGT_MID_PER_TRK;
    snprintf(out, (size_t)outSz, "MIDI %02d %s", trk + 1, MID_OFFSET_NAMES[off]);
    return;
  }
  if (targetId < MIDI_TGT_NAV_BASE) {
    int mIdx = targetId - MIDI_TGT_MASTER_BASE;
    snprintf(out, (size_t)outSz, "%s", MASTER_TARGET_NAMES[mIdx]);
    return;
  }
  int nIdx = targetId - MIDI_TGT_NAV_BASE;
  snprintf(out, (size_t)outSz, "%s", NAV_TARGET_NAMES[nIdx]);
}

FLASHMEM bool midiMapIsTriggerTarget(int targetId) {
  if (targetId < MIDI_TGT_AUD_BASE) {
    return targetId != MIDI_TGT_PLAY_SPEED;
  }
  if (targetId < MIDI_TGT_MID_BASE) {
    int off = (targetId - MIDI_TGT_AUD_BASE) % MIDI_TGT_AUD_PER_TRK;
    return (off == MIDI_AUD_OFF_ARM || off == MIDI_AUD_OFF_MUTE || off == MIDI_AUD_OFF_SOLO);
  }
  if (targetId < MIDI_TGT_MASTER_BASE) {
    return true; // ARM, MUTE, SOLO on MIDI tracks
  }
  if (targetId < MIDI_TGT_NAV_BASE) {
    return targetId == MIDI_TGT_METRO_MODE;
  }
  return true;
}

FLASHMEM void midiMapStartLearn(int targetId) {
  if (targetId < 0 || targetId >= MIDI_TARGET_COUNT) return;
  instLearningActive = false;
  learningTargetId = targetId;
  learningActive = true;
}

FLASHMEM void midiMapCancelLearn() {
  learningActive = false;
}

bool midiMapIsLearning() {
  return learningActive;
}

FLASHMEM int midiMapLearningTarget() {
  return learningTargetId;
}

// Runs in RAM1 (called from midiSeqInputEvent ISR/poll) so mapped CCs do not trigger MIDI threshold start
bool midiMapIsCcIntercepted(uint8_t inCh1To16, uint8_t cc) {
  if (learningActive || instLearningActive) return true;
  if (inCh1To16 < 1 || inCh1To16 > 16) return false;
  return (mappedCcChMask[cc & 0x7F] & (uint16_t)(1u << (inCh1To16 - 1))) != 0;
}

FLASHMEM static void executeTargetAction(int targetId, uint8_t val) {
  SongSettings &ps = songSettings();

  if (targetId < MIDI_TGT_AUD_BASE) {
    switch (targetId) {
      case MIDI_TGT_PLAY_TOGGLE:
        appToggleTransport();
        break;
      case MIDI_TGT_PLAY:
        if (transport.mode == TransportMode::Stopped) {
          appStartTransport(true);
        }
        break;
      case MIDI_TGT_STOP:
        if (transport.mode != TransportMode::Stopped) {
          appStopTransport();
        } else {
          appSeekTape(0);
        }
        break;
      case MIDI_TGT_REC_ARM:
        toggleCursorOrLastArmedTrack();
        break;
      case MIDI_TGT_DISARM_ALL:
        for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
          if (tracks[i].armMode != ArmMode::Off) armAudioTrack(i, ArmMode::Off);
        }
        for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
          if (midiTracks[i].armMode != ArmMode::Off) armMidiTrack(i, ArmMode::Off);
        }
        break;
      case MIDI_TGT_PUNCH_TOGGLE:
        ps.punchEnabled = !ps.punchEnabled;
        clampInfoCellSelection();
        storageSyncLoopState();
        saveSessionWhenIdle();
        break;
      case MIDI_TGT_LOOP_TOGGLE:
        ps.loopEnabled = !ps.loopEnabled;
        clampInfoCellSelection();
        storageSyncLoopState();
        saveSessionWhenIdle();
        break;
      case MIDI_TGT_RTZ:
        if (transport.tapeFrame == 0 && (ps.punchEnabled || ps.loopEnabled)) {
          appSeekTape(ps.punchInFrame);
        } else {
          appSeekTape(0);
        }
        break;
      case MIDI_TGT_REW:
        if (ps.punchEnabled || ps.loopEnabled) {
          appSeekTape(ps.punchInFrame);
          trackEditOnPlayheadAdjusted();
        }
        break;
      case MIDI_TGT_FF:
        if (ps.punchEnabled || ps.loopEnabled) {
          appSeekTape(ps.punchOutFrame);
          trackEditOnPlayheadAdjusted();
        }
        break;
      case MIDI_TGT_PLAY_SPEED: {
        int nextSq = DEFAULT_SPEED_QUARTER;
        if (transport.mode != TransportMode::Stopped && anyTrackArmed()) {
          nextSq = (val < 64) ? REVERSE_1X_SPEED_QUARTER : DEFAULT_SPEED_QUARTER;
        } else if (val >= 62 && val <= 65) {
          nextSq = DEFAULT_SPEED_QUARTER; // Center detent snaps to +1.00x
        } else {
          nextSq = ((int)val * (MAX_SPEED_QUARTER - MIN_SPEED_QUARTER) + 63) / 127 + MIN_SPEED_QUARTER;
        }
        appSetPlaybackSpeedQuarter(nextSq);
        break;
      }
    }
    uiForceRedraw();
    return;
  }

  if (targetId < MIDI_TGT_MID_BASE) {
    int rel = targetId - MIDI_TGT_AUD_BASE;
    int t = constrain(rel / MIDI_TGT_AUD_PER_TRK, 0, NUM_AUDIO_TRACKS - 1);
    int off = rel % MIDI_TGT_AUD_PER_TRK;
    Track &tr = tracks[t];

    switch (off) {
      case MIDI_AUD_OFF_ARM:
        stepAudioTrackArm(t, 1);
        break;
      case MIDI_AUD_OFF_MUTE:
        if (!(recording.mode != Recording::Idle && recording.track == t && recording.resample)) {
          if (tr.state == TrackState::Muted) {
            tr.state = (tr.recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
          } else {
            tr.state = TrackState::Muted;
          }
          saveSessionWhenIdle();
        }
        break;
      case MIDI_AUD_OFF_SOLO:
        tr.solo = !tr.solo;
        midiSeqSilenceMutedTracks();
        saveSessionWhenIdle();
        break;
      case MIDI_AUD_OFF_VOL: {
        int v = ((int)val * VOLUME_MAX + 63) / 127;
        if (tr.volume != (int8_t)v) {
          setTrackMix(t, v, tr.balance);
          saveSessionWhenIdle();
        }
        break;
      }
      case MIDI_AUD_OFF_PAN: {
        int bal = (val == 64) ? 0
                : (val < 64)  ? -(((64 - (int)val) * PAN_MAX + 32) / 64)
                              : (((int)val - 64) * PAN_MAX + 31) / 63;
        bal = constrain(bal, -PAN_MAX, PAN_MAX);
        if (tr.balance != (int8_t)bal) {
          setTrackMix(t, tr.volume, bal);
          saveSessionWhenIdle();
        }
        break;
      }
    }
    uiForceRedraw();
    return;
  }

  if (targetId < MIDI_TGT_MASTER_BASE) {
    int rel = targetId - MIDI_TGT_MID_BASE;
    int t = constrain(rel / MIDI_TGT_MID_PER_TRK, 0, NUM_MIDI_TRACKS - 1);
    int off = rel % MIDI_TGT_MID_PER_TRK;
    MidiTrack &mt = midiTracks[t];

    switch (off) {
      case MIDI_MID_OFF_ARM: {
        int next = ((int)mt.armMode + 1) % 3;
        armMidiTrack(t, (ArmMode)next);
        break;
      }
      case MIDI_MID_OFF_MUTE:
        mt.muted = !mt.muted;
        if (mt.muted) {
          midiSeqSilenceMutedTracks();
        }
        saveSessionWhenIdle();
        break;
      case MIDI_MID_OFF_SOLO:
        mt.solo = !mt.solo;
        midiSeqSilenceMutedTracks();
        saveSessionWhenIdle();
        break;
    }
    uiForceRedraw();
    return;
  }

  // Master Bus & Tempo
  uint8_t pct = (uint8_t)(((int)val * 100 + 63) / 127);
  switch (targetId) {
    case MIDI_TGT_MASTER_VOL: {
      uint8_t v = (uint8_t)(((int)val * MASTER_VOLUME_MAX + 63) / 127);
      if (ps.masterVolume != v) {
        ps.masterVolume = v;
        setMasterVolume(v);
        saveSessionWhenIdle();
      }
      break;
    }
    case MIDI_TGT_MASTER_CHOR:
      if (ps.masterChorus != pct) {
        ps.masterChorus = pct;
        setMasterChorus(pct);
        saveSessionWhenIdle();
      }
      break;
    case MIDI_TGT_MASTER_DLAY:
      if (ps.masterDelay != pct) {
        ps.masterDelay = pct;
        setMasterDelay(pct);
        saveSessionWhenIdle();
      }
      break;
    case MIDI_TGT_MASTER_DFB:
      if (ps.masterDelayFeedback != pct) {
        ps.masterDelayFeedback = pct;
        setMasterDelayFeedback(pct);
        saveSessionWhenIdle();
      }
      break;
    case MIDI_TGT_MASTER_RVRB:
      if (ps.masterReverb != pct) {
        ps.masterReverb = pct;
        setMasterReverb(pct);
        saveSessionWhenIdle();
      }
      break;
    case MIDI_TGT_MASTER_TAPE:
      if (ps.masterTape != pct) {
        ps.masterTape = pct;
        setMasterTape(pct);
        saveSessionWhenIdle();
      }
      break;
    case MIDI_TGT_MASTER_COMP:
      if (ps.masterComp != pct) {
        ps.masterComp = pct;
        setMasterComp(pct);
        saveSessionWhenIdle();
      }
      break;
    case MIDI_TGT_TEMPO_BPM:
      if (!midiClockFollowing()) {
        float targetBpm = roundf(MIN_BPM + ((float)val / 127.0f) * (MAX_BPM - MIN_BPM));
        setTempo(targetBpm);
      }
      break;
    case MIDI_TGT_METRO_MODE: {
      int m = ((int)ps.metroMode + 1) % 3;
      ps.metroMode = (MetroMode)m;
      updateMetronomeState();
      saveSessionWhenIdle();
      break;
    }
  }
  uiForceRedraw();
}

FLASHMEM static void instMidiMapHandleExternalCc(uint8_t inCh1To16, uint8_t cc, uint8_t val);

FLASHMEM bool midiMapHandleCc(uint8_t inCh1To16, uint8_t cc, uint8_t val) {
  cc &= 0x7F;
  val &= 0x7F;

  // 1. If MIDI Learn is currently listening inside the System or Instrument MIDI Map, capture (Channel, CC)
  //    and arm a 1-second delay before recognizing mapped CCs so the target value is not immediately changed.
  if (learningActive) {
    learningActive = false;
    midiMapSetEntry(learningTargetId, inCh1To16, cc);
    midiMapFlushSaveIfDirty();
    lastSeenVal[learningTargetId] = val;
    learnIgnoreUntilMs = millis() + 1000u;
    learnIgnoreActive = true;
    char msg[32];
    snprintf(msg, sizeof(msg), "MAPPED: C%02u CC%03u", (unsigned)inCh1To16, (unsigned)cc);
    uiShowToast(msg);
    uiForceRedraw();
    return true;
  }
  if (instLearningActive) {
    instLearningActive = false;
    instMidiMapSetSlot(instLearningInstIdx, instLearningSlotIdx, inCh1To16, cc);
    saveSessionWhenIdle();
    learnIgnoreUntilMs = millis() + 1000u;
    learnIgnoreActive = true;
    char msg[32];
    snprintf(msg, sizeof(msg), "MAPPED: C%02u CC%03u", (unsigned)inCh1To16, (unsigned)cc);
    uiShowToast(msg);
    uiForceRedraw();
    return true;
  }

  if (inCh1To16 < 1 || inCh1To16 > 16) {
    return false;
  }

  uint32_t nowMs = millis();
  bool inLearnCooldown = false;
  if (learnIgnoreActive) {
    if ((int32_t)(nowMs - learnIgnoreUntilMs) < 0) {
      inLearnCooldown = true;
    } else {
      learnIgnoreActive = false;
    }
  }

  const uint16_t chBit = (uint16_t)(1u << (inCh1To16 - 1));
  if (!inLearnCooldown && (instMappedCcChMask[cc] & chBit) != 0) {
    instMidiMapHandleExternalCc(inCh1To16, cc, val);
  }

  // O(1) fast-path exit when (inCh1To16, cc) is not mapped to any global system target
  if ((mappedCcChMask[cc] & chBit) == 0) {
    return false;
  }

  // 2. Match against all active global target mappings
  bool matched = false;

  for (int i = 0; i < MIDI_TARGET_COUNT; i++) {
    const MidiCcMapEntry &eff = globalMap[i];
    if (eff.channel == MIDI_MAP_CH_OFF || eff.cc != cc) continue;
    if (eff.channel != MIDI_MAP_CH_ALL && eff.channel != inCh1To16) continue;

    matched = true;
    uint8_t prev = lastSeenVal[i];
    lastSeenVal[i] = val;

    if (inLearnCooldown) continue;

    if (i == MIDI_TGT_REW || i == MIDI_TGT_FF) {
      const int dir = (i == MIDI_TGT_REW) ? -1 : 1;
      const SongSettings &ps = songSettings();
      if (val < 64) {
        if (midiScrubDir == dir) {
          if (!midiScrubWasDoubleTap &&
              (uint32_t)(nowMs - midiScrubStartMs) < MIDI_REW_FF_TAP_HOLD_MS) {
            lastRewFfTapDir = dir;
            lastRewFfTapMs = nowMs;
            lastRewFfTapFrame = midiScrubOriginFrame;
          }
          midiScrubDir = 0;
          midiScrubFracFrames = 0.0f;
        }
      } else if (ps.punchEnabled || ps.loopEnabled) {
        midiScrubDir = 0;
        midiScrubFracFrames = 0.0f;
        lastRewFfTapDir = 0;
        bool risingEdge = (prev == 0xFF || prev < 64);
        bool singleShotTrigger = (val == 127) && ((uint32_t)(nowMs - lastTrigMs[i]) >= 60u);
        if (risingEdge || singleShotTrigger) {
          lastTrigMs[i] = nowMs;
          executeTargetAction(i, val);
        }
      } else {
        bool risingEdge = (prev == 0xFF || prev < 64);
        bool singleShotTrigger = (midiScrubDir != dir) && (val == 127) && ((uint32_t)(nowMs - lastTrigMs[i]) >= 60u);
        if (risingEdge || singleShotTrigger) {
          lastTrigMs[i] = nowMs;
          if (lastRewFfTapDir == dir &&
              (uint32_t)(nowMs - lastRewFfTapMs) < MIDI_REW_FF_TAP_HOLD_MS) {
            lastRewFfTapDir = 0;
            midiScrubWasDoubleTap = true;
            appStepTapeBeat(dir, lastRewFfTapFrame);
          } else {
            lastRewFfTapDir = 0;
            midiScrubWasDoubleTap = false;
            midiScrubOriginFrame = transport.tapeFrame;
            appStepTapeMs(dir);
          }
          trackEditOnPlayheadAdjusted();
          midiScrubDir = dir;
          midiScrubStartMs = nowMs;
          midiScrubLastTickMs = nowMs;
          midiScrubFracFrames = 0.0f;
          uiForceRedraw();
        }
      }
      continue;
    }

    if (i >= MIDI_TGT_NAV_UP && i <= MIDI_TGT_NAV_RIGHT) {
      const uint8_t dirBit = NAV_DIR_BITS[i - MIDI_TGT_NAV_BASE];
      if (val < 64) {
        if (midiNavDirBit == dirBit) {
          uint32_t heldMs = (uint32_t)(nowMs - midiNavStartMs);
          if (!midiNavWasDoubleTap && !inputEditHeld() &&
              heldMs < MIDI_NAV_DOUBLE_TAP_MS) {
            lastNavTapDirBit = dirBit;
            lastNavTapMs = (heldMs < MIDI_NAV_TAP_MAX_MS) ? nowMs : midiNavStartMs;
          }
          midiNavDirBit = 0;
        }
      } else {
        bool risingEdge = (prev == 0xFF || prev < 64);
        bool singleShotTrigger = (midiNavDirBit != dirBit) && (val == 127) && ((uint32_t)(nowMs - lastTrigMs[i]) >= 60u);
        if (risingEdge || singleShotTrigger) {
          lastTrigMs[i] = nowMs;
          if (!inputEditHeld() && lastNavTapDirBit == dirBit &&
              (uint32_t)(nowMs - lastNavTapMs) < MIDI_NAV_DOUBLE_TAP_MS) {
            lastNavTapDirBit = 0;
            midiNavWasDoubleTap = true;
            inputRestoreNavSnapshot(dirBit);
            inputTriggerNav(dirBit, true);
          } else {
            lastNavTapDirBit = 0;
            midiNavWasDoubleTap = false;
            if (!inputEditHeld()) {
              inputSaveNavSnapshot();
            }
            inputTriggerNav(dirBit, false);
          }
          midiNavDirBit = dirBit;
          midiNavStartMs = nowMs;
          midiNavNextRepeatMs = nowMs + MIDI_NAV_HOLD_START_MS;
        }
      }
      continue;
    }

    if (i == MIDI_TGT_NAV_EDIT) {
      if (val < 64) {
        if (midiEditIsDown) {
          midiEditIsDown = false;
          bool wasUsedAsMod = inputFinishMidiEditHold();
          if (!wasUsedAsMod && !midiEditHoldStarted) {
            uint32_t heldMs = (uint32_t)(nowMs - midiEditPressMs);
            if (heldMs < MIDI_NAV_DOUBLE_TAP_MS) {
              midiEditPendingSingleTap = true;
              midiEditPendingTapAtMs = (heldMs < MIDI_NAV_TAP_MAX_MS)
                                           ? (nowMs + MIDI_NAV_DOUBLE_TAP_MS)
                                           : (midiEditPressMs + MIDI_NAV_DOUBLE_TAP_MS);
            } else {
              midiEditPendingSingleTap = false;
              inputTriggerMidiEditTap();
            }
          }
        }
      } else {
        bool risingEdge = (prev == 0xFF || prev < 64);
        bool singleShotTrigger = (!midiEditIsDown) && (val == 127) && ((uint32_t)(nowMs - lastTrigMs[i]) >= 60u);
        if (risingEdge || singleShotTrigger) {
          lastTrigMs[i] = nowMs;
          if (midiEditPendingSingleTap &&
              (int32_t)(nowMs - midiEditPendingTapAtMs) < 0) {
            midiEditPendingSingleTap = false;
            midiEditIsDown = true;
            midiEditHoldStarted = true;
            inputSetMidiEditHeld(true, true);
            inputTriggerMidiOptEdit();
          } else {
            if (midiEditPendingSingleTap) {
              midiEditPendingSingleTap = false;
              inputTriggerMidiEditTap();
            }
            midiEditIsDown = true;
            midiEditPressMs = nowMs;
            midiEditHoldStarted = false;
            inputSetMidiEditHeld(true, false);
          }
        }
      }
      continue;
    }

    if (midiMapIsTriggerTarget(i)) {
      // Trigger on button press (val >= 64) when transitioning from < 64 (momentary)
      // or when receiving a dedicated 127 trigger message (with 60 ms debounce)
      bool risingEdge = (val >= 64) && (prev == 0xFF || prev < 64);
      bool singleShotTrigger = (val == 127) && ((uint32_t)(nowMs - lastTrigMs[i]) >= 60u);
      if (risingEdge || singleShotTrigger) {
        lastTrigMs[i] = nowMs;
        executeTargetAction(i, val);
      }
    } else {
      executeTargetAction(i, val);
    }
  }

  return matched;
}

FLASHMEM void midiMapService() {
  uint32_t ms = millis();

  if (instMapParamDirty) {
    instMapParamDirty = false;
    saveSessionWhenIdle();
    uiForceRedraw();
  }

  if (midiMapDirty && (int32_t)(ms - midiMapDirtyAtMs) >= 500) {
    midiMapFlushSaveIfDirty();
  }

  if (midiEditIsDown) {
    if (inputIsMidiEditUsedAsModifier()) {
      midiEditPendingSingleTap = false;
    } else if (!midiEditHoldStarted &&
               (int32_t)(ms - midiEditPressMs) >= (int32_t)MIDI_NAV_HOLD_START_MS) {
      if (inputStartMidiEditHoldAction()) {
        midiEditHoldStarted = true;
        midiEditPendingSingleTap = false;
      }
    }
  } else if (midiEditPendingSingleTap) {
    if ((int32_t)(ms - midiEditPendingTapAtMs) >= 0) {
      midiEditPendingSingleTap = false;
      inputTriggerMidiEditTap();
    }
  }

  if (midiNavDirBit != 0 && !midiNavWasDoubleTap) {
    if ((int32_t)(ms - midiNavStartMs) >= (int32_t)MIDI_NAV_HOLD_START_MS &&
        (int32_t)(ms - midiNavNextRepeatMs) >= 0) {
      inputTriggerNav(midiNavDirBit, false);
      midiNavNextRepeatMs = ms + inputCurrentRepeatIntervalMs();
    }
  }

  if (midiScrubDir == 0) return;
  const SongSettings &ps = songSettings();
  if (ps.punchEnabled || ps.loopEnabled) {
    midiScrubDir = 0;
    midiScrubFracFrames = 0.0f;
    return;
  }

  if ((int32_t)(ms - midiScrubStartMs) >= (int32_t)MIDI_REW_FF_TAP_HOLD_MS) {
    uint32_t dtMs = ms - midiScrubLastTickMs;
    if (dtMs > 0) {
      midiScrubLastTickMs = ms;
      float holdSec = (float)(ms - midiScrubStartMs - MIDI_REW_FF_TAP_HOLD_MS) / 1000.0f;
      float speed = constrain(0.5f + holdSec * 2.25f, 0.5f, 5.0f);
      midiScrubFracFrames += ((float)dtMs / 1000.0f) * speed * (float)SAMPLE_RATE_HZ;
      int32_t stepFrames = (int32_t)midiScrubFracFrames;
      if (stepFrames > 0) {
        midiScrubFracFrames -= (float)stepFrames;
        appStepTape(midiScrubDir * stepFrames);
        trackEditOnPlayheadAdjusted();
        uiForceRedraw();
      }
    }
  } else {
    midiScrubLastTickMs = ms;
  }
}

// ============================================================================
// Per-Instrument MIDI CC Parameter Mapping (Persisted in INST.DAT & .FMI)
// ============================================================================
PROGMEM static const char *const INST_MAP_FM_LABELS[9] = {
  "ALGORITHM",
  "OP A WAVE",
  "OP A RATIO",
  "OP A LEVEL",
  "OP A FDBK",
  "OP B WAVE",
  "OP B RATIO",
  "OP B LEVEL",
  "OP B FDBK"
};

PROGMEM static const char *const INST_MAP_PD_LABELS[15] = {
  "L1 WAVE1",
  "L1 WAVE2",
  "L1 DCW",
  "L1 ENV",
  "L1 LVL",
  "L1 BAL",
  "L2 WAVE1",
  "L2 WAVE2",
  "L2 DCW",
  "L2 ENV",
  "L2 LVL",
  "L2 BAL",
  "L2>1 TUNE",
  "L2>1 MOD",
  "L2>1 AMT"
};

PROGMEM static const char *const INST_MAP_SMP_LABELS[1] = {
  "PRESS MODE"
};

PROGMEM static const char *const INST_MAP_ND_LABELS[16] = {
  "OSC WAVE",
  "OSC SHAPE",
  "OSC TUNE",
  "OSC SUB",
  "OSC LVL",
  "BED TYPE",
  "BED FREQ",
  "BED RESO",
  "BED KTRK",
  "BED LVL",
  "CRKL TYPE",
  "CRKL DENS",
  "CRKL POP",
  "MOD DRIFT",
  "VOICE PAN",
  "TRIG MODE"
};

PROGMEM static const char *const INST_MAP_SHARED_LABELS[INST_MIDI_MAP_SHARED_SLOTS] = {
  "AMP AMT",
  "AMP ATK",
  "AMP DEC",
  "AMP SUS",
  "AMP REL",
  "FLT AMT",
  "FLT ATK",
  "FLT DEC",
  "FLT SUS",
  "FLT REL",
  "FX DRIVE",
  "FX CHORUS",
  "FX DELAY",
  "FX D.FB",
  "FX REVERB"
};

static inline int instEngineParamCount(InstType type) {
  switch (type) {
    case InstType::Fm2Op:      return 9;
    case InstType::PhaseDist:  return 15;
    case InstType::Sampler:    return 1;
    case InstType::NoiseDrone: return 16;
    default:                   return 0;
  }
}

FLASHMEM static void instMidiMapRebuildMask() {
  uint16_t chMask[128] = {};
  uint32_t instMask[NUM_INSTRUMENTS][4] = {};

  for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
    for (int s = 0; s < INST_MIDI_MAP_TOTAL_SLOTS; s++) {
      uint8_t ch = instMap[inst][s].channel;
      uint8_t cc = instMap[inst][s].cc & 0x7Fu;
      if (ch != MIDI_MAP_CH_OFF) {
        chMask[cc] |= (ch == MIDI_MAP_CH_ALL) ? 0xFFFFu : (uint16_t)(1u << ((ch - 1) & 15));
        instMask[inst][cc >> 5] |= (1u << (cc & 31));
      }
    }
  }

  uint32_t irq = saveIrq();
  memcpy(instMappedCcChMask, chMask, sizeof(instMappedCcChMask));
  memcpy(instMappedCcMask, instMask, sizeof(instMappedCcMask));
  restoreIrq(irq);
}

FLASHMEM void instMidiMapResetAll() {
  instLearningActive = false;
  memset(instMap, 0, sizeof(instMap));
  memset(clipboardInstMap, 0, sizeof(clipboardInstMap));
  instMidiMapRebuildMask();
}

FLASHMEM void instMidiMapClearInst(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if (instLearningActive && instLearningInstIdx == instIdx) {
    instLearningActive = false;
  }
  memset(instMap[instIdx], 0, sizeof(instMap[instIdx]));
  instMidiMapRebuildMask();
}

FLASHMEM void instMidiMapCopyInst(int srcInstIdx) {
  if (srcInstIdx < 0 || srcInstIdx >= NUM_INSTRUMENTS) return;
  memcpy(clipboardInstMap, instMap[srcInstIdx], sizeof(clipboardInstMap));
}

FLASHMEM void instMidiMapPasteInst(int dstInstIdx) {
  if (dstInstIdx < 0 || dstInstIdx >= NUM_INSTRUMENTS) return;
  memcpy(instMap[dstInstIdx], clipboardInstMap, sizeof(instMap[dstInstIdx]));
  memset(clipboardInstMap, 0, sizeof(clipboardInstMap));
  instMidiMapRebuildMask();
}

FLASHMEM int instMidiMapRowCount(InstType type) {
  int eng = instEngineParamCount(type);
  if (eng <= 0) return INST_MIDI_MAP_SHARED_SLOTS;
  return eng + INST_MIDI_MAP_SHARED_SLOTS;
}

FLASHMEM int instMidiMapRowToSlot(InstType type, int rowIdx) {
  int eng = instEngineParamCount(type);
  if (rowIdx < 0) return 0;
  if (rowIdx < eng) return rowIdx;
  int sharedIdx = constrain(rowIdx - eng, 0, INST_MIDI_MAP_SHARED_SLOTS - 1);
  return INST_MIDI_MAP_ENGINE_SLOTS + sharedIdx;
}

FLASHMEM const char *instMidiMapRowLabel(InstType type, int rowIdx) {
  int eng = instEngineParamCount(type);
  if (rowIdx >= 0 && rowIdx < eng) {
    switch (type) {
      case InstType::Fm2Op:      return INST_MAP_FM_LABELS[rowIdx];
      case InstType::PhaseDist:  return INST_MAP_PD_LABELS[rowIdx];
      case InstType::Sampler:    return INST_MAP_SMP_LABELS[rowIdx];
      case InstType::NoiseDrone: return INST_MAP_ND_LABELS[rowIdx];
      default:                   break;
    }
  }
  int sharedIdx = constrain(rowIdx - eng, 0, INST_MIDI_MAP_SHARED_SLOTS - 1);
  return INST_MAP_SHARED_LABELS[sharedIdx];
}

MidiCcMapEntry instMidiMapGetSlot(int instIdx, int slot) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS ||
      slot < 0 || slot >= INST_MIDI_MAP_TOTAL_SLOTS) {
    return MidiCcMapEntry{MIDI_MAP_CH_OFF, 0};
  }
  return instMap[instIdx][slot];
}

FLASHMEM void instMidiMapSetSlot(int instIdx, int slot, uint8_t channel, uint8_t cc) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS ||
      slot < 0 || slot >= INST_MIDI_MAP_TOTAL_SLOTS) {
    return;
  }
  cc &= 0x7Fu;
  if (channel > MIDI_MAP_CH_ALL) channel = MIDI_MAP_CH_OFF;
  instMap[instIdx][slot].channel = channel;
  instMap[instIdx][slot].cc = cc;
  instMidiMapRebuildMask();
}

FLASHMEM void instMidiMapClearSlot(int instIdx, int slot) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS ||
      slot < 0 || slot >= INST_MIDI_MAP_TOTAL_SLOTS) {
    return;
  }
  instMap[instIdx][slot].channel = MIDI_MAP_CH_OFF;
  instMap[instIdx][slot].cc = 0;
  instMidiMapRebuildMask();
}

FLASHMEM void instMidiMapStepChannel(int instIdx, int slot, int delta) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS ||
      slot < 0 || slot >= INST_MIDI_MAP_TOTAL_SLOTS || delta == 0) {
    return;
  }
  MidiCcMapEntry &ent = instMap[instIdx][slot];
  uint8_t prevCh = ent.channel;
  int curIdx = channelToUiIdx(ent.channel);
  int nextIdx = constrain(curIdx + delta, 0, 17);
  uint8_t nextCh = uiIdxToChannel(nextIdx);
  if (nextCh == prevCh) return;
  instMidiMapSetSlot(instIdx, slot, nextCh, ent.cc);
}

FLASHMEM void instMidiMapStepCc(int instIdx, int slot, int delta) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS ||
      slot < 0 || slot >= INST_MIDI_MAP_TOTAL_SLOTS) {
    return;
  }
  MidiCcMapEntry &ent = instMap[instIdx][slot];
  if (delta == 0 && ent.channel != MIDI_MAP_CH_OFF) return;
  uint8_t ch = (ent.channel == MIDI_MAP_CH_OFF) ? MIDI_MAP_CH_ALL : ent.channel;
  int nextCc = constrain((int)ent.cc + delta, 0, 127);
  instMidiMapSetSlot(instIdx, slot, ch, (uint8_t)nextCc);
}

FLASHMEM void instMidiMapStartLearn(int instIdx, int slot) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS ||
      slot < 0 || slot >= INST_MIDI_MAP_TOTAL_SLOTS) {
    return;
  }
  learningActive = false;
  instLearningInstIdx = instIdx;
  instLearningSlotIdx = slot;
  instLearningActive = true;
}

FLASHMEM void instMidiMapCancelLearn() {
  instLearningActive = false;
}

bool instMidiMapIsLearning() {
  return instLearningActive;
}

FLASHMEM int instMidiMapLearningInst() {
  return instLearningInstIdx;
}

FLASHMEM int instMidiMapLearningSlot() {
  return instLearningSlotIdx;
}

static inline uint8_t scaleCcTo100(uint8_t val) {
  return (uint8_t)(((int)val * 100 + 63) / 127);
}

static inline uint8_t scaleCcToEnum(uint8_t val, int count) {
  return (uint8_t)constrain(((int)val * count) / 128, 0, count - 1);
}

static inline uint8_t scaleCcToBipolar100(uint8_t val) {
  int bal = (val == 64) ? 0
          : (val < 64)  ? -(((64 - (int)val) * 100 + 32) / 64)
                        : (((int)val - 64) * 100 + 31) / 63;
  return (uint8_t)(int8_t)constrain(bal, -100, 100);
}

static inline uint16_t scaleCcToTune4800(uint8_t val) {
  int tune = (val == 64) ? 2400
           : (val < 64)  ? (((int)val * 2400 + 32) / 64)
                         : (2400 + (((int)val - 64) * 2400 + 31) / 63);
  return (uint16_t)constrain(tune, 0, 4800);
}

static inline bool assignU8(uint8_t &dst, uint8_t v) {
  if (dst == v) return false;
  dst = v;
  return true;
}

static inline bool assignU16(uint16_t &dst, uint16_t v) {
  if (dst == v) return false;
  dst = v;
  return true;
}

FLASHMEM static bool applyInstPatchSlotCc(FmPatch &p, InstType iType, int slot, uint8_t val) {
  val &= 0x7Fu;
  if (slot >= INST_MIDI_MAP_ENGINE_SLOTS && slot < INST_MIDI_MAP_TOTAL_SLOTS) {
    switch (slot - INST_MIDI_MAP_ENGINE_SLOTS) {
      case 0:  return assignU8(p.amp.amt,           val);
      case 1:  return assignU8(p.amp.attack,        val);
      case 2:  return assignU8(p.amp.decay,         val);
      case 3:  return assignU8(p.amp.sustain,       val);
      case 4:  return assignU8(p.amp.release,       val);
      case 5:  return assignU8(p.flt.amt,           val);
      case 6:  return assignU8(p.flt.attack,        val);
      case 7:  return assignU8(p.flt.decay,         val);
      case 8:  return assignU8(p.flt.sustain,       val);
      case 9:  return assignU8(p.flt.release,       val);
      case 10: return assignU8(p.fx.drive,          scaleCcTo100(val));
      case 11: return assignU8(p.fx.chorus,         scaleCcTo100(val));
      case 12: return assignU8(p.fx.delay,          scaleCcTo100(val));
      case 13: return assignU8(p.fx.delayFeedback,  scaleCcTo100(val));
      case 14: return assignU8(p.fx.reverb,         scaleCcTo100(val));
      default: return false;
    }
  }

  switch (iType) {
    case InstType::Fm2Op: {
      if (slot == 0) {
        return assignU8(p.algorithm, scaleCcToEnum(val, NUM_FM_ALGOS));
      }
      if (slot >= 1 && slot <= 8) {
        int opIdx = (slot - 1) >> 2;
        int sub = (slot - 1) & 3;
        FmOpParams &op = p.op[opIdx];
        if (sub == 0) return assignU8(op.wave, scaleCcToEnum(val, NUM_FM_WAVES));
        if (sub == 1) return assignU16(op.ratioX100, (uint16_t)(((int)val * (int)FM_RATIO_MAX + 63) / 127));
        if (sub == 2) return assignU8(op.level, scaleCcTo100(val));
        return assignU8(op.feedback, scaleCcTo100(val));
      }
      return false;
    }

    case InstType::PhaseDist: {
      if (slot >= 0 && slot <= 11) {
        int opIdx = (slot < 6) ? 0 : 1;
        int sub = (slot < 6) ? slot : (slot - 6);
        FmOpParams &op = p.op[opIdx];
        if (sub == 0) return assignU8(op.wave, scaleCcToEnum(val, NUM_PD_WAVES));
        if (sub == 1) return assignU8(op.reserved[0], scaleCcToEnum(val, NUM_PD_WAVE2_OPTS));
        if (sub == 2) return assignU8(op.reserved[1], scaleCcTo100(val));
        if (sub == 3) return assignU8(op.feedback, scaleCcTo100(val));
        if (sub == 4) return assignU8(op.level, scaleCcTo100(val));
        return assignU8(op.reserved[2], scaleCcToBipolar100(val));
      }
      if (slot == 12) return assignU16(p.op[1].ratioX100, scaleCcToTune4800(val));
      if (slot == 13) return assignU8(p.algorithm, scaleCcToEnum(val, NUM_PD_MOD_MODES));
      if (slot == 14) return assignU16(p.op[0].ratioX100, (uint16_t)scaleCcTo100(val));
      return false;
    }

    case InstType::Sampler:
      if (slot == 0) {
        return assignU8(p.algorithm, scaleCcToEnum(val, NUM_SAMPLER_PRESS_MODES));
      }
      return false;

    case InstType::NoiseDrone: {
      switch (slot) {
        case 0:  return assignU8(p.op[0].wave, scaleCcToEnum(val, NUM_ND_WAVES));
        case 1:  return assignU8(p.op[0].feedback, scaleCcTo100(val));
        case 2:  return assignU16(p.op[0].ratioX100, scaleCcToTune4800(val));
        case 3:  return assignU8(p.op[0].reserved[0], scaleCcTo100(val));
        case 4:  return assignU8(p.op[0].level, scaleCcTo100(val));
        case 5:  return assignU8(p.op[1].wave, scaleCcToEnum(val, NUM_ND_NOISE_TYPES));
        case 6:  return assignU8(p.op[1].reserved[0], scaleCcTo100(val));
        case 7:  return assignU8(p.op[1].feedback, scaleCcTo100(val));
        case 8:  return assignU8(p.op[1].reserved[1], scaleCcTo100(val));
        case 9:  return assignU8(p.op[1].level, scaleCcTo100(val));
        case 10: return assignU8(p.op[0].reserved[1], scaleCcToEnum(val, NUM_ND_CRACKLE_TYPES));
        case 11: return assignU8(p.op[0].reserved[2], scaleCcTo100(val));
        case 12: {
          uint16_t next = (p.op[1].ratioX100 & 0xFF00u) | (uint16_t)scaleCcTo100(val);
          return assignU16(p.op[1].ratioX100, next);
        }
        case 13: {
          uint16_t next = (p.op[1].ratioX100 & 0x00FFu) | ((uint16_t)scaleCcTo100(val) << 8);
          return assignU16(p.op[1].ratioX100, next);
        }
        case 14: return assignU8(p.op[1].reserved[2], scaleCcToBipolar100(val));
        case 15: return assignU8(p.algorithm, scaleCcToEnum(val, NUM_ND_TRIG_MODES));
        default: return false;
      }
    }

    default:
      return false;
  }
}

FLASHMEM static void instMidiMapHandleExternalCc(uint8_t inCh1To16, uint8_t cc, uint8_t val) {
  bool anyChanged = false;
  for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
    if ((instMappedCcMask[inst][cc >> 5] & (1u << (cc & 31))) == 0) continue;
    FmPatch &p = fmSynthPatch(inst);
    InstType iType = (InstType)p.type;
    int engCount = instEngineParamCount(iType);
    if (engCount <= 0) continue;

    for (int s = 0; s < INST_MIDI_MAP_TOTAL_SLOTS; s++) {
      if (s >= engCount && s < INST_MIDI_MAP_ENGINE_SLOTS) continue;
      const MidiCcMapEntry &ent = instMap[inst][s];
      if (ent.channel == MIDI_MAP_CH_OFF || ent.cc != cc) continue;
      if (ent.channel != MIDI_MAP_CH_ALL && ent.channel != inCh1To16) continue;
      if (applyInstPatchSlotCc(p, iType, s, val)) {
        anyChanged = true;
      }
    }
  }
  if (anyChanged) {
    instMapParamDirty = true;
  }
}

FLASHMEM static void instMidiMapApplyRoutedCcSlow(int instIdx, uint8_t cc, uint8_t val) {
  if (learnIgnoreActive) {
    if ((int32_t)(millis() - learnIgnoreUntilMs) < 0) {
      return;
    }
    learnIgnoreActive = false;
  }
  FmPatch &p = fmSynthPatch(instIdx);
  InstType iType = (InstType)p.type;
  int engCount = instEngineParamCount(iType);
  if (engCount <= 0) return;

  bool anyChanged = false;
  for (int s = 0; s < INST_MIDI_MAP_TOTAL_SLOTS; s++) {
    if (s >= engCount && s < INST_MIDI_MAP_ENGINE_SLOTS) continue;
    const MidiCcMapEntry &ent = instMap[instIdx][s];
    if (ent.channel == MIDI_MAP_CH_OFF || ent.cc != cc) continue;
    if (applyInstPatchSlotCc(p, iType, s, val)) {
      anyChanged = true;
    }
  }
  if (anyChanged) {
    instMapParamDirty = true;
  }
}

void instMidiMapApplyRoutedCc(int instIdx, uint8_t cc, uint8_t val) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if (learningActive || instLearningActive) return;
  cc &= 0x7Fu;
  if ((instMappedCcMask[instIdx][cc >> 5] & (1u << (cc & 31))) == 0) return;
  instMidiMapApplyRoutedCcSlow(instIdx, cc, val & 0x7Fu);
}

// ============================================================================
// SD Card Persistence (`INST.DAT` & `.FMI` via RAM1 Stack Buffers)
// ============================================================================
struct DiskInstMidiMapChunk {
  char magic[4];     // "IMAP"
  uint16_t version;  // 1
  uint16_t numSlots; // INST_MIDI_MAP_TOTAL_SLOTS (31)
  MidiCcMapEntry entries[NUM_INSTRUMENTS][INST_MIDI_MAP_TOTAL_SLOTS];
};
static_assert(sizeof(DiskInstMidiMapChunk) <= 512, "DiskInstMidiMapChunk must fit in one 512-byte sector");

struct DiskSingleInstMidiMapChunk {
  char magic[4];     // "IM1P"
  uint16_t version;  // 1
  uint16_t numSlots; // INST_MIDI_MAP_TOTAL_SLOTS (31)
  MidiCcMapEntry entries[INST_MIDI_MAP_TOTAL_SLOTS];
};
static_assert(sizeof(DiskSingleInstMidiMapChunk) <= 512, "DiskSingleInstMidiMapChunk must fit in one 512-byte sector");

FLASHMEM static void sanitizeLoadedMapEntry(MidiCcMapEntry &ent) {
  ent.cc &= 0x7Fu;
  if (ent.channel > MIDI_MAP_CH_ALL) {
    ent.channel = MIDI_MAP_CH_OFF;
    ent.cc = 0;
  }
}

FLASHMEM bool instMidiMapSaveSongData(FsFile &f) {
  if (!f.isOpen()) return false;
  DiskInstMidiMapChunk chunk{};
  memcpy(chunk.magic, "IMAP", 4);
  chunk.version = 1;
  chunk.numSlots = INST_MIDI_MAP_TOTAL_SLOTS;
  memcpy(chunk.entries, instMap, sizeof(chunk.entries));
  return f.write(&chunk, sizeof(chunk)) == (int)sizeof(chunk);
}

FLASHMEM bool instMidiMapLoadSongData(FsFile &f) {
  memset(instMap, 0, sizeof(instMap));
  instMidiMapRebuildMask();
  if (!f.isOpen()) return false;

  DiskInstMidiMapChunk chunk{};
  int rd = f.read(&chunk, sizeof(chunk));
  if (rd < 8 || memcmp(chunk.magic, "IMAP", 4) != 0 || chunk.version != 1) {
    return true; // Songs saved before Instrument MIDI Map existed remain valid
  }

  int slots = min((int)chunk.numSlots, INST_MIDI_MAP_TOTAL_SLOTS);
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    for (int s = 0; s < slots; s++) {
      instMap[i][s] = chunk.entries[i][s];
      sanitizeLoadedMapEntry(instMap[i][s]);
    }
  }
  instMidiMapRebuildMask();
  return true;
}

FLASHMEM bool instMidiMapSaveSingleInstData(FsFile &f, int instIdx) {
  if (!f.isOpen() || instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  DiskSingleInstMidiMapChunk chunk{};
  memcpy(chunk.magic, "IM1P", 4);
  chunk.version = 1;
  chunk.numSlots = INST_MIDI_MAP_TOTAL_SLOTS;
  memcpy(chunk.entries, instMap[instIdx], sizeof(chunk.entries));
  return f.write(&chunk, sizeof(chunk)) == (int)sizeof(chunk);
}

FLASHMEM bool instMidiMapLoadSingleInstData(FsFile &f, int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  memset(instMap[instIdx], 0, sizeof(instMap[instIdx]));
  instMidiMapRebuildMask();
  if (!f.isOpen()) return false;

  DiskSingleInstMidiMapChunk chunk{};
  int rd = f.read(&chunk, sizeof(chunk));
  if (rd < 8 || memcmp(chunk.magic, "IM1P", 4) != 0 || chunk.version != 1) {
    return true; // Presets saved before Instrument MIDI Map existed remain valid
  }

  int slots = min((int)chunk.numSlots, INST_MIDI_MAP_TOTAL_SLOTS);
  for (int s = 0; s < slots; s++) {
    instMap[instIdx][s] = chunk.entries[s];
    sanitizeLoadedMapEntry(instMap[instIdx][s]);
  }
  instMidiMapRebuildMask();
  return true;
}

} // namespace looper

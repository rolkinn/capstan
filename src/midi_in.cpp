// midi_in.cpp — Serial & USB MIDI input parser + External MIDI Clock follower
#include "midi_in.h"
#include "engine.h"
#include "midi_seq.h"
#include "app.h"
#include "ui.h"
#include <Arduino.h>
#include <math.h>

using namespace looper;

static constexpr uint32_t FOLLOW_TIMEOUT_MS = 600;
static HardwareSerial *port = nullptr;
static volatile bool suspended = false;
static volatile uint8_t source = MIDI_SERIAL;
static volatile uint32_t sourceUs = 0;
static volatile bool sourceKnown = false;

// Clock tick ring buffer from ISR to loop()
static constexpr uint32_t TICKS = 128;
static volatile uint32_t tickUs[TICKS];
static volatile uint32_t tickCount = 0;

struct ClockEvent {
  uint8_t type;
  uint32_t param;
};
static constexpr uint32_t EVENTS = 16;
static volatile ClockEvent events[EVENTS];
static volatile uint32_t eventHead = 0;

// Internal event type tags queued from ISR/poll to midiInService() in loop()
enum MidiTransportEvt : uint8_t {
  EVT_MMC_STOP       = 0x01,
  EVT_MMC_PLAY       = 0x02,
  EVT_MMC_FF         = 0x04,
  EVT_MMC_REW        = 0x05,
  EVT_MMC_REC_STROBE = 0x06,
  EVT_MMC_REC_EXIT   = 0x07,
  EVT_MMC_RESET      = 0x0D,
  EVT_MMC_LOCATE     = 0x44
};

// Serial MIDI parser state (handles Running Status + System Common + SysEx + Channel Voice)
static uint8_t runningStatus = 0;
static uint8_t dataBytes[2] = {0, 0};
static uint8_t dataCount = 0;
static uint8_t expectedDataBytes = 0;
static bool inSysex = false;
static uint8_t sysexBuf[16] = {};
static uint8_t sysexLen = 0;

static uint8_t msgDataLen(uint8_t status) {
  if (status >= 0x80 && status <= 0xEF) {
    uint8_t hi = status & 0xF0;
    if (hi == 0xC0 || hi == 0xD0) return 1; // Program Change, Channel Pressure
    return 2;                               // Note Off/On, Poly AT, CC, Pitch Bend
  }
  if (status == 0xF2) return 2;             // Song Position Pointer
  if (status == 0xF1 || status == 0xF3) return 1;
  return 0;
}

static void pushClockEvent(uint8_t type, uint32_t param = 0) {
  volatile ClockEvent &e = events[eventHead % EVENTS];
  e.type = type;
  e.param = param;
  eventHead++;
}

static bool clockSourceAllows(MidiSource from) {
  ClockSource cs = songSettings().clockSource;
  if (cs == ClockSource::Internal) return false;
  if (cs == ClockSource::ExternalUsb) return from == MIDI_USB;
  if (cs == ClockSource::ExternalTrs) return from == MIDI_SERIAL;
  return true; // ClockSource::ExternalAuto
}

static bool transportSourceAllows(MidiSource from) {
  MidiTransportIn ti = songSettings().midiTransportIn;
  if (ti == MidiTransportIn::Off) return false;
  if (ti == MidiTransportIn::Usb) return from == MIDI_USB;
  if (ti == MidiTransportIn::Trs) return from == MIDI_SERIAL;
  return true; // MidiTransportIn::Auto
}

FLASHMEM void midiInSuspend(bool off) { suspended = off; }

FLASHMEM void midiInSongPosition(uint16_t pos16, uint32_t us, MidiSource from) {
  (void)us;
  if (suspended || !transportSourceAllows(from)) return;
  pushClockEvent(0xF2, pos16);
}

FLASHMEM void midiInSysEx(const uint8_t *data, uint16_t len, uint32_t us, MidiSource from) {
  (void)us;
  if (suspended || !data || len < 6 || !transportSourceAllows(from)) return;
  // Universal Real-Time SysEx (0xF0 0x7F <devId> 0x06 <subId> ... 0xF7) — MIDI Machine Control (MMC)
  if (data[0] != 0xF0 || data[1] != 0x7F || data[3] != 0x06 || data[len - 1] != 0xF7) return;
  uint8_t cmd = data[4];
  switch (cmd) {
    case 0x01: // Stop
    case 0x09: // Pause
      pushClockEvent(EVT_MMC_STOP);
      break;
    case 0x02: // Play
    case 0x03: // Deferred Play
      pushClockEvent(EVT_MMC_PLAY);
      break;
    case 0x04: // Fast Forward
      pushClockEvent(EVT_MMC_FF);
      break;
    case 0x05: // Rewind
      pushClockEvent(EVT_MMC_REW);
      break;
    case 0x06: // Record Strobe (Punch In / Toggle Rec)
    case 0x08: // Record Pause
      pushClockEvent(EVT_MMC_REC_STROBE);
      break;
    case 0x07: // Record Exit (Punch Out / Disarm)
      pushClockEvent(EVT_MMC_REC_EXIT);
      break;
    case 0x0D: // MMC Reset
      pushClockEvent(EVT_MMC_RESET);
      break;
    case 0x44: // MMC Locate: F0 7F <dev> 06 44 06 01 <hr> <mn> <sc> <fr> <ff> F7
      if (len >= 12 && data[5] == 0x06 && data[6] == 0x01) {
        uint32_t hr = data[7] & 0x1F;
        uint32_t mn = data[8] & 0x7F;
        uint32_t sc = data[9] & 0x7F;
        uint32_t fr = data[10] & 0x7F;
        uint32_t totalMs = (hr * 3600u + mn * 60u + sc) * 1000u + (fr * 1000u) / 30u;
        uint64_t targetFrame = ((uint64_t)totalMs * SAMPLE_RATE_HZ + 500ULL) / 1000ULL;
        if (targetFrame > MAX_REC_FRAMES) targetFrame = MAX_REC_FRAMES;
        pushClockEvent(EVT_MMC_LOCATE, (uint32_t)targetFrame);
      }
      break;
  }
}

void midiInByte(uint8_t b, uint32_t us, MidiSource from) {
  if (suspended) return;

  // 1. System Real-Time messages (0xF8..0xFF) can appear anywhere, even mid-message or mid-SysEx
  if (b >= 0xF8) {
    if (b == 0xF8) {
      if (!clockSourceAllows(from)) return;
      bool active = sourceKnown && (int32_t)(us - sourceUs) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);
      if (active && from != source && songSettings().clockSource == ClockSource::ExternalAuto) return;
      source = from;
      sourceUs = us;
      sourceKnown = true;
      tickUs[tickCount % TICKS] = us;
      tickCount++;
    } else if (b == 0xFA || b == 0xFB || b == 0xFC) {
      if (clockSourceAllows(from)) {
        bool active = sourceKnown && (int32_t)(us - sourceUs) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);
        if (!active || from == source || songSettings().clockSource != ClockSource::ExternalAuto) {
          source = from;
          sourceUs = us;
          sourceKnown = true;
        }
      }
      if (transportSourceAllows(from)) {
        pushClockEvent(b);
      }
    }
    return;
  }

  // 2. System Exclusive (0xF0..0xF7)
  if (b == 0xF0) {
    runningStatus = 0;
    dataCount = 0;
    expectedDataBytes = 0;
    inSysex = true;
    sysexBuf[0] = 0xF0;
    sysexLen = 1;
    return;
  }
  if (inSysex) {
    if (b == 0xF7) {
      if (sysexLen < sizeof(sysexBuf)) {
        sysexBuf[sysexLen++] = 0xF7;
        midiInSysEx(sysexBuf, sysexLen, us, from);
      }
      inSysex = false;
      sysexLen = 0;
      return;
    }
    if (b < 0x80) {
      if (sysexLen < sizeof(sysexBuf)) {
        sysexBuf[sysexLen++] = b;
      } else {
        sysexLen = 0xFF; // mark overflow
      }
      return;
    }
    // Any other non-realtime status byte (0x80..0xF6) terminates incomplete SysEx
    inSysex = false;
    sysexLen = 0;
  }

  // 3. Status bytes (0x80..0xF7)
  if (b >= 0x80) {
    if (b <= 0xEF) {
      runningStatus = b;
      dataCount = 0;
      expectedDataBytes = msgDataLen(b);
    } else {
      // System Common (0xF0..0xF7) cancels running status
      runningStatus = (b == 0xF2) ? b : 0;
      dataCount = 0;
      expectedDataBytes = msgDataLen(b);
    }
    return;
  }

  // 4. Data bytes (0x00..0x7F)
  if (!runningStatus || expectedDataBytes == 0) return;
  dataBytes[dataCount++] = b;
  if (dataCount >= expectedDataBytes) {
    dataCount = 0; // keep runningStatus for subsequent channel messages
    if (runningStatus == 0xF2) {
      midiInSongPosition(dataBytes[0] | (dataBytes[1] << 7), us, from);
      runningStatus = 0;
    } else if (runningStatus >= 0x80 && runningStatus <= 0xEF) {
      midiSeqInputEvent(runningStatus, dataBytes[0], expectedDataBytes == 2 ? dataBytes[1] : 0, from);
    }
  }
}

static void poll() {
  while (port && port->available() > 0) {
    midiInByte((uint8_t)port->read(), micros(), MIDI_SERIAL);
  }
}

FLASHMEM void midiInBegin(HardwareSerial *p) {
  port = p;
  midiInPollHook = poll;
}

static bool theirsRunning = false;
static uint32_t startedUs = 0;
static uint32_t seenTicks = 0, seenEvents = 0;
static uint32_t lastTickUs = 0;
static uint32_t validTickStreak = 0;
static bool tempoLocked = false;
static float smoothedTickUs = 20833.3f; // ~120 BPM (500,000 us / 24)
static uint32_t theirSpb = 22050;
static float measuredBpm = 120.0f;

FLASHMEM static bool alive() {
  uint32_t irq = saveIrq();
  uint32_t n = tickCount, latest = n ? tickUs[(n - 1) % TICKS] : startedUs;
  restoreIrq(irq);
  if (theirsRunning && (int32_t)(startedUs - latest) > 0) latest = startedUs;
  return (n || theirsRunning) && (int32_t)(micros() - latest) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);
}

FLASHMEM bool midiClockFollowing() {
  return songSettings().clockSource != ClockSource::Internal &&
         sourceKnown &&
         clockSourceAllows((MidiSource)source) &&
         alive();
}

FLASHMEM int midiFollowSource() { return source; }

FLASHMEM void midiInResetClockState() {
  uint32_t irq = saveIrq();
  sourceKnown = false;
  seenTicks = tickCount;
  seenEvents = eventHead;
  restoreIrq(irq);
  theirsRunning = false;
  lastTickUs = 0;
  validTickStreak = 0;
  tempoLocked = false;
}

FLASHMEM void midiInService() {
  uint32_t irq = saveIrq();
  uint32_t n = tickCount, ne = eventHead;
  restoreIrq(irq);

  bool extEnabled = (songSettings().clockSource != ClockSource::Internal);
  bool transportEnabled = (songSettings().midiTransportIn != MidiTransportIn::Off);

  if (ne - seenEvents > EVENTS) seenEvents = ne - EVENTS;
  for (; seenEvents < ne; seenEvents++) {
    ClockEvent e;
    irq = saveIrq();
    e.type = events[seenEvents % EVENTS].type;
    e.param = events[seenEvents % EVENTS].param;
    restoreIrq(irq);

    noteUserActivity();
    if (!transportEnabled) continue;

    switch (e.type) {
      case 0xFA: // MIDI Start
        if (extEnabled) {
          theirsRunning = true;
          startedUs = micros();
        }
        appSeekTape(0);
        appStartTransport(!extEnabled);
        uiForceRedraw();
        break;
      case 0xFB: // MIDI Continue
        if (extEnabled) {
          theirsRunning = true;
          startedUs = micros();
        }
        appStartTransport(!extEnabled);
        uiForceRedraw();
        break;
      case 0xFC: // MIDI Stop
        if (extEnabled) {
          theirsRunning = false;
        }
        appStopTransport();
        uiForceRedraw();
        break;
      case 0xF2: // Song Position Pointer (in 16th notes = 1/4 beat)
        {
          double beats = e.param * 0.25;
          uint32_t frame = beatsToFrames(beats, bpm);
          appSeekTape(frame);
          uiForceRedraw();
        }
        break;
      case EVT_MMC_PLAY:
        appStartTransport(!extEnabled);
        uiForceRedraw();
        break;
      case EVT_MMC_STOP:
        if (transport.mode != TransportMode::Stopped) {
          appStopTransport();
        } else {
          appSeekTape(0);
        }
        uiForceRedraw();
        break;
      case EVT_MMC_FF: {
        const SongSettings &ps = songSettings();
        if (ps.punchEnabled || ps.loopEnabled) {
          appSeekTape(ps.punchOutFrame);
        } else {
          appStepTapeBy32nds(8);
        }
        uiForceRedraw();
        break;
      }
      case EVT_MMC_REW: {
        const SongSettings &ps = songSettings();
        if (ps.punchEnabled || ps.loopEnabled) {
          appSeekTape(ps.punchInFrame);
        } else {
          appStepTapeBy32nds(-8);
        }
        uiForceRedraw();
        break;
      }
      case EVT_MMC_REC_STROBE:
        toggleCursorOrLastArmedTrack();
        uiForceRedraw();
        break;
      case EVT_MMC_REC_EXIT:
        for (int i = 0; i < NUM_AUDIO_TRACKS; i++) {
          if (tracks[i].armMode != ArmMode::Off) armAudioTrack(i, ArmMode::Off);
        }
        for (int i = 0; i < NUM_MIDI_TRACKS; i++) {
          if (midiTracks[i].armMode != ArmMode::Off) armMidiTrack(i, ArmMode::Off);
        }
        uiForceRedraw();
        break;
      case EVT_MMC_RESET:
        appStopTransport();
        appSeekTape(0);
        uiForceRedraw();
        break;
      case EVT_MMC_LOCATE:
        appSeekTape(e.param);
        uiForceRedraw();
        break;
    }
  }

  if (n - seenTicks > TICKS) {
    seenTicks = n - TICKS;
    lastTickUs = 0;
    validTickStreak = 0;
    tempoLocked = false;
  }
  for (; seenTicks < n; seenTicks++) {
    uint32_t us = tickUs[seenTicks % TICKS];
    if (lastTickUs != 0) {
      uint32_t dt = us - lastTickUs;
      if (dt > 150000) {
        // Clock paused and resumed; start a fresh measurement window
        validTickStreak = 0;
        tempoLocked = false;
      } else if (dt >= 6000 && dt <= 90000) {
        // Valid MIDI clock interval for 30..250 BPM (with transport framing jitter margin)
        if (validTickStreak < TICKS - 1) {
          validTickStreak++;
        }

        // Detect deliberate master tempo jumps (> 4% change over a 6-tick / quarter-beat span)
        if (validTickStreak >= 6) {
          uint32_t shortSpanUs = us - tickUs[(seenTicks - 6) % TICKS];
          float shortAvgUs = (float)shortSpanUs * (1.0f / 6.0f);
          if (fabsf(shortAvgUs - smoothedTickUs) > smoothedTickUs * 0.04f) {
            validTickStreak = 6;
            smoothedTickUs = shortAvgUs;
          }
        }

        // Compute telescoping average tick duration across up to 48 ticks (2 full beats)
        uint32_t spanTicks = (validTickStreak >= 48) ? 48u
                           : (validTickStreak >= 24) ? 24u
                           : (validTickStreak >= 6)  ? validTickStreak : 1u;
        float windowTickUs = (spanTicks > 1u)
                                 ? ((float)(us - tickUs[(seenTicks - spanTicks) % TICKS]) / (float)spanTicks)
                                 : (float)dt;

        if (validTickStreak == 1) {
          smoothedTickUs = windowTickUs;
        } else {
          float alpha = (validTickStreak < 24) ? 0.25f : 0.06f;
          smoothedTickUs += alpha * (windowTickUs - smoothedTickUs);
        }

        float rawBpm = 60000000.0f / (smoothedTickUs * 24.0f);
        rawBpm = constrain(rawBpm, MIN_BPM, MAX_BPM);

        // Schmitt-trigger hysteresis + whole-integer BPM snap to prevent 0.1 BPM chatter
        float nearestInt = roundf(rawBpm);
        float candidateBpm = (fabsf(rawBpm - nearestInt) <= 0.06f)
                                 ? nearestInt
                                 : (roundf(rawBpm * 10.0f) / 10.0f);

        bool updateTempo = false;
        if (!tempoLocked && validTickStreak >= 6) {
          measuredBpm = candidateBpm;
          tempoLocked = true;
          updateTempo = true;
        } else if (tempoLocked && candidateBpm != measuredBpm) {
          bool onWholeBpm = fabsf(measuredBpm - roundf(measuredBpm)) < 0.01f;
          float deadband = onWholeBpm ? 0.15f : 0.09f;
          if (fabsf(rawBpm - measuredBpm) >= deadband ||
              (candidateBpm == nearestInt && fabsf(rawBpm - nearestInt) <= 0.04f)) {
            measuredBpm = candidateBpm;
            updateTempo = true;
          }
        }

        if (updateTempo || (extEnabled && tempoLocked && bpm != measuredBpm)) {
          theirSpb = spbForBpm(measuredBpm);
          if (extEnabled) {
            followTempo(theirSpb, measuredBpm);
          }
        }
      }
    }
    lastTickUs = us;
  }

  if (!alive()) {
    theirsRunning = false;
    if (tempoLocked && lastTickUs != 0 && (int32_t)(micros() - lastTickUs) >= (int32_t)(FOLLOW_TIMEOUT_MS * 1000)) {
      lastTickUs = 0;
      validTickStreak = 0;
      tempoLocked = false;
    }
  }
}

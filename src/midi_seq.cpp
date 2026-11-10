// midi_seq.cpp — 8-Track Linear MIDI Recorder & Sequencer
#include "midi_seq.h"
#include "midi_map.h"
#include "instruments.h"
#include "storage.h"
#include "ui.h"
#include <Arduino.h>
#include <string.h>

namespace looper {

// 8 tracks x 2048 events x 8 bytes = 128 KB in RAM2 (DMAMEM)
DMAMEM static MidiEvent eventPool[NUM_MIDI_TRACKS][MAX_MIDI_EVENTS_PER_TRACK];
MidiTrack midiTracks[NUM_MIDI_TRACKS];

// Lock-free ring buffer from MIDI input (ISR / poll) to loop()
struct RawMidiMsg {
  uint8_t status;
  uint8_t d1;
  uint8_t d2;
  uint8_t source;       // 0 = MIDI_SERIAL (TRS), 1 = MIDI_USB
  uint8_t sampleOffset; // 0..127 sub-block sample offset at arrival
  uint32_t thruSentMask;// bitmask of output destinations already dispatched in ISR
  uint32_t tapeFrame;
  TransportMode tMode;
};
static constexpr uint32_t IN_RING_SIZE = 128;
static volatile RawMidiMsg inRing[IN_RING_SIZE];
static volatile uint32_t inHead = 0, inTail = 0;
static volatile bool midiActivityPending = false;
static volatile uint32_t seqEditDepth = 0;

struct SeqEditGuard {
  SeqEditGuard() {
    uint32_t irq = saveIrq();
    seqEditDepth++;
    restoreIrq(irq);
  }
  ~SeqEditGuard() {
    uint32_t irq = saveIrq();
    seqEditDepth--;
    restoreIrq(irq);
  }
};

// Track active playing notes per output channel (0..15) so we can send clean Note Offs
static uint8_t activeNotes[16][16] = {};   // 16 channels x 128 notes (1 bit per note)

static inline void markNoteActive(uint8_t ch0, uint8_t note, bool on) {
  if (ch0 >= 16 || note >= 128) return;
  uint32_t irq = saveIrq();
  if (on) activeNotes[ch0][note >> 3] |= (1 << (note & 7));
  else    activeNotes[ch0][note >> 3] &= ~(1 << (note & 7));
  restoreIrq(irq);
}

static inline bool isNoteActive(uint8_t ch0, uint8_t note) {
  if (ch0 >= 16 || note >= 128) return false;
  return (activeNotes[ch0][note >> 3] & (1 << (note & 7))) != 0;
}

static inline void setTrackNoteActive(int t, uint8_t note, bool on) {
  if (t < 0 || t >= NUM_MIDI_TRACKS || note >= 128) return;
  uint32_t bit = 1u << (note & 31);
  uint32_t irq = saveIrq();
  if (on) midiTracks[t].activeNoteMask[note >> 5] |= bit;
  else    midiTracks[t].activeNoteMask[note >> 5] &= ~bit;
  restoreIrq(irq);
}

static inline void clearTrackActiveNotes(int t) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  uint32_t irq = saveIrq();
  memset(midiTracks[t].activeNoteMask, 0, sizeof(midiTracks[t].activeNoteMask));
  restoreIrq(irq);
}

static inline void clearAllTrackActiveNotes() {
  uint32_t irq = saveIrq();
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    memset(midiTracks[t].activeNoteMask, 0, sizeof(midiTracks[t].activeNoteMask));
  }
  restoreIrq(irq);
}

// Track notes currently opened in eventPool[t] by the active live recording pass
static uint32_t recNoteOpenMask[NUM_MIDI_TRACKS][4] = {};

static inline bool isRecNoteOpen(int t, uint8_t note) {
  if (t < 0 || t >= NUM_MIDI_TRACKS || note >= 128) return false;
  return (recNoteOpenMask[t][note >> 5] & (1u << (note & 31))) != 0;
}

static inline void setRecNoteOpen(int t, uint8_t note, bool open) {
  if (t < 0 || t >= NUM_MIDI_TRACKS || note >= 128) return;
  uint32_t bit = 1u << (note & 31);
  if (open) recNoteOpenMask[t][note >> 5] |= bit;
  else      recNoteOpenMask[t][note >> 5] &= ~bit;
}

static inline bool hasAnyRecNoteOpen(int t) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return false;
  return (recNoteOpenMask[t][0] | recNoteOpenMask[t][1] |
          recNoteOpenMask[t][2] | recNoteOpenMask[t][3]) != 0;
}

static inline void clearRecNoteOpenMask(int t) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  memset(recNoteOpenMask[t], 0, sizeof(recNoteOpenMask[t]));
}

int midiSeqFocusedTrack() {
  Page pg = currentPage();
  if (pg == Page::Midi) {
    return constrain(midiRow, 0, TRACK_ROWS - 1);
  }
  if (pg == Page::Master && isMasterMidiMode() &&
      masterRow >= MASTER_ROW_TRK_PARAM1 && masterRow <= MASTER_ROW_TRK_MS) {
    return (masterRow == MASTER_ROW_TRK_MS)
               ? constrain(masterCell / 2, 0, TRACK_ROWS - 1)
               : constrain(masterCell, 0, TRACK_ROWS - 1);
  }
  return -1;
}

// Send a channel voice message to either External MIDI (1..16) or Internal Synth (17..24 = I01..I08)
// Returns false only when allowSdIo == false (ISR context) and an uncached Sampler key requires SD I/O.
static bool sendMidiOut(uint8_t typeHi, uint8_t outDest, uint8_t d1, uint8_t d2,
                        uint8_t portMask = MIDI_PORT_BOTH, uint8_t sampleOffset = 0, bool allowSdIo = true) {
  if (outDest == 0) return true;
  uint8_t hi = typeHi & 0xF0;
  d1 &= 0x7F;
  d2 &= 0x7F;

  if (outDest >= 17 && outDest <= MAX_MIDI_OUT_CHAN) {
    int instIdx = (int)outDest - 17;
    if (hi == 0x90) {
      if (d2 > 0) return fmSynthNoteOn(instIdx, d1, d2, sampleOffset, allowSdIo);
      fmSynthNoteOff(instIdx, d1, sampleOffset);
    } else if (hi == 0x80) {
      fmSynthNoteOff(instIdx, d1, sampleOffset);
    } else if (hi == 0xB0) {
      fmSynthCC(instIdx, d1, d2);
    } else if (hi == 0xE0) {
      fmSynthPitchBend(instIdx, d1, d2);
    }
    return true;
  }

  if (!portMask) return true;
  uint8_t ch0 = (uint8_t)constrain((int)outDest - 1, 0, 15);
  uint8_t status = hi | ch0;
  midiSendMsg(status, d1, d2, 3, portMask);

  if (hi == 0x90 && d2 > 0) {
    markNoteActive(ch0, d1, true);
  } else if (hi == 0x80 || (hi == 0x90 && d2 == 0)) {
    markNoteActive(ch0, d1, false);
  }
  return true;
}

FLASHMEM void midiSeqAllNotesOff() {
  clearAllTrackActiveNotes();
  fmSynthAllNotesOff();
  for (uint8_t ch = 0; ch < 16; ch++) {
    bool anyOn = false;
    for (uint8_t n = 0; n < 128; n++) {
      if (isNoteActive(ch, n)) {
        midiSendMsg(0x80 | ch, n, 0, 3);
        markNoteActive(ch, n, false);
        anyOn = true;
      }
    }
    if (anyOn) {
      // Also send CC 123 (All Notes Off) and Reset Pitch Bend to center (8192 = 0x00, 0x40)
      midiSendMsg(0xB0 | ch, 123, 0, 3);
      midiSendMsg(0xE0 | ch, 0x00, 0x40, 3);
    }
  }
}

FLASHMEM static void midiSeqSilenceDest(uint8_t outDest) {
  if (outDest >= 17 && outDest <= MAX_MIDI_OUT_CHAN) {
    fmSynthSilenceInst((int)outDest - 17);
    return;
  }
  if (outDest >= 1 && outDest <= 16) {
    uint8_t ch = outDest - 1;
    bool anyOn = false;
    for (uint8_t n = 0; n < 128; n++) {
      if (isNoteActive(ch, n)) {
        midiSendMsg(0x80 | ch, n, 0, 3);
        markNoteActive(ch, n, false);
        anyOn = true;
      }
    }
    if (anyOn) {
      midiSendMsg(0xB0 | ch, 123, 0, 3);
      midiSendMsg(0xE0 | ch, 0x00, 0x40, 3);
    }
  }
}

FLASHMEM static void midiSeqSilenceDestIfUnused(uint8_t dest) {
  if (dest == 0) return;
  const bool anySolo = anyTrackSolo();
  for (int j = 0; j < NUM_MIDI_TRACKS; j++) {
    if (midiTracks[j].outChannel == dest &&
        !midiTracks[j].muted && (!anySolo || midiTracks[j].solo)) {
      return;
    }
  }
  midiSeqSilenceDest(dest);
}

FLASHMEM void midiSeqSilenceMutedTracks() {
  const bool anySolo = anyTrackSolo();
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    bool audible = !midiTracks[t].muted && (!anySolo || midiTracks[t].solo);
    if (!audible) {
      clearTrackActiveNotes(t);
      midiSeqSilenceDestIfUnused(midiTracks[t].outChannel);
    }
  }
}

// UI order for MIDI IN: 0 = OFF (MIDI_IN_OFF), 1 = ALL (0), 2..17 = 01..16 (1..16)
FLASHMEM void midiSeqStepInChannel(int t, int delta) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  MidiTrack &mt = midiTracks[t];
  int idx = (mt.inChannel == MIDI_IN_OFF) ? 0 : ((int)mt.inChannel + 1);
  idx = constrain(idx + delta, 0, 17);
  mt.inChannel = (idx == 0) ? MIDI_IN_OFF : (uint8_t)(idx - 1);
}

FLASHMEM void midiSeqCycleInChannel(int t) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  MidiTrack &mt = midiTracks[t];
  int idx = (mt.inChannel == MIDI_IN_OFF) ? 0 : ((int)mt.inChannel + 1);
  idx = (idx + 1) % 18;
  mt.inChannel = (idx == 0) ? MIDI_IN_OFF : (uint8_t)(idx - 1);
}

// UI order for MIDI OUT: 0 = OFF, 1..16 = C01..C16, 17..24 = I01..I08
FLASHMEM void midiSeqSetOutChannel(int t, uint8_t dest) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  MidiTrack &mt = midiTracks[t];
  uint8_t prevOut = mt.outChannel;
  mt.outChannel = constrain(dest, (uint8_t)0, (uint8_t)MAX_MIDI_OUT_CHAN);
  if (prevOut != mt.outChannel) {
    if (mt.outChannel == 0) {
      clearTrackActiveNotes(t);
    }
    midiSeqSilenceDestIfUnused(prevOut);
  }
}

FLASHMEM void midiSeqStepOutChannel(int t, int dy, int delta) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  uint8_t cur = midiTracks[t].outChannel;
  uint8_t next = cur;
  if (dy > 0 && cur == 0) {
    next = (uint8_t)(t + 1);
  } else if (dy > 0 && cur <= 16) {
    next = 17;
  } else if (dy < 0 && cur >= 17) {
    next = (uint8_t)(t + 1);
  } else if (dy < 0 && cur >= 1 && cur <= 16) {
    next = 0;
  } else {
    next = (uint8_t)constrain((int)cur + delta, 0, MAX_MIDI_OUT_CHAN);
  }
  midiSeqSetOutChannel(t, next);
}

FLASHMEM void midiSeqCycleOutChannel(int t) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  uint8_t next = (uint8_t)((midiTracks[t].outChannel + 1) % (MAX_MIDI_OUT_CHAN + 1));
  midiSeqSetOutChannel(t, next);
}

FLASHMEM void midiSeqInit() {
  SeqEditGuard guard;
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    MidiTrack &mt = midiTracks[t];
    mt.armMode = ArmMode::Off;
    mt.trigMode = TrigMode::Manual;
    mt.muted = false;
    mt.solo = false;
    mt.inChannel = 0;                   // OMNI by default
    mt.outChannel = (uint8_t)(17 + t);  // Internal Inst I01..I08 by default
    mt.eventCount = 0;
    mt.playIndex = 0;
    mt.recWindowActive = false;
    mt.recEraseCursorTick = 0;
    mt.overviewVersion++;
    mt.lastStatus = 0;
    mt.lastData1 = 0;
    mt.lastData2 = 0;
    mt.lastActivityMs = 0;
    clearTrackActiveNotes(t);
    clearRecNoteOpenMask(t);
  }
  memset(activeNotes, 0, sizeof(activeNotes));
}

FLASHMEM void midiSeqClearTrack(int t) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  SeqEditGuard guard;
  MidiTrack &mt = midiTracks[t];
  mt.muted = false;
  mt.solo = false;
  mt.eventCount = 0;
  mt.playIndex = 0;
  mt.recWindowActive = false;
  mt.recEraseCursorTick = 0;
  mt.overviewVersion++;
  clearTrackActiveNotes(t);
  clearRecNoteOpenMask(t);
}

FLASHMEM void midiSeqClearTrackMode(int t, MidiClearMode mode) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) return;
  if (mode == MidiClearMode::All) {
    midiSeqClearTrack(t);
    midiSeqAllNotesOff();
    return;
  }

  SeqEditGuard guard;
  MidiTrack &mt = midiTracks[t];
  MidiEvent *ev = eventPool[t];
  uint32_t n = mt.eventCount;
  uint32_t writeIdx = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t hi = ev[i].status & 0xF0;
    bool isNote = (hi == 0x80 || hi == 0x90);
    bool drop = (mode == MidiClearMode::Notes) ? isNote : !isNote;
    if (!drop) {
      if (writeIdx != i) ev[writeIdx] = ev[i];
      writeIdx++;
    }
  }
  mt.eventCount = writeIdx;
  if (mt.eventCount == 0) {
    mt.muted = false;
    mt.solo = false;
  }
  mt.playIndex = 0;
  mt.recWindowActive = false;
  mt.recEraseCursorTick = 0;
  mt.overviewVersion++;
  if (mode == MidiClearMode::Notes) {
    clearRecNoteOpenMask(t);
  }
  midiSeqAllNotesOff();
}

FLASHMEM void midiSeqClearAll() {
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    midiSeqClearTrack(t);
    midiTracks[t].armMode = ArmMode::Off;
    midiTracks[t].trigMode = TrigMode::Manual;
    midiTracks[t].muted = false;
    midiTracks[t].solo = false;
    midiTracks[t].inChannel = 0;
    midiTracks[t].outChannel = (uint8_t)(17 + t);
  }
  midiSeqAllNotesOff();
}

void midiSeqInputEvent(uint8_t status, uint8_t d1, uint8_t d2, uint8_t fromSource) {
  uint8_t type = status & 0xF0;
  if (type >= 0x80 && type <= 0xE0) {
    midiActivityPending = true;
  }
  // Only accept Note Off (0x80), Note On (0x90), Control Change (0xB0), and Pitch Bend (0xE0)
  if (type != 0x80 && type != 0x90 && type != 0xB0 && type != 0xE0) return;
  d1 &= 0x7F;
  d2 &= 0x7F;
  if (type == 0x90 && d2 == 0) {
    status = (uint8_t)(0x80 | (status & 0x0F));
    type = 0x80;
  }

  uint8_t inCh1To16 = (status & 0x0F) + 1;
  uint32_t irq = saveIrq();
  TransportMode tm = transport.mode;
  uint32_t tf = transport.tapeFrame;
  uint8_t sampleOffset = (uint8_t)(transport.now & (AUDIO_BLOCK_SAMPLES - 1));

  // Check if transport is waiting for MIDI Input Threshold trigger
  bool triggeredStart = false;
  bool ccIntercepted = (type == 0xB0) && midiMapIsCcIntercepted(inCh1To16, d1);
  if (tm == TransportMode::WaitThreshold && transport.waitMidiThresh) {
    // Note Off and mapped/learning CCs do not trigger start; Note On (vel > 0), unmapped CC, or Pitch Bend does
    bool isTriggerMsg = (type == 0x90 && d2 > 0) ||
                        (type == 0xB0 && !ccIntercepted) ||
                        (type == 0xE0);
    if (isTriggerMsg) {
      for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
        const MidiTrack &mt = midiTracks[t];
        if (mt.armMode != ArmMode::Off && mt.trigMode == TrigMode::Threshold) {
          if (mt.inChannel != MIDI_IN_OFF && (mt.inChannel == 0 || mt.inChannel == inCh1To16)) {
            transport.mode = TransportMode::Running;
            transport.running = true;
            transport.sampleInBeat = 0;
            tm = TransportMode::Running;
            triggeredStart = true;
            break;
          }
        }
      }
    }
  }
  restoreIrq(irq);

  if (triggeredStart) {
    midiSendClockNow(0xFA);
  }

  // Immediate 250 us ISR Live MIDI Thru & Internal Synth dispatch for non-intercepted messages
  uint32_t thruSentMask = 0;
  if (!ccIntercepted) {
    const bool running = (tm == TransportMode::Running);
    const int selectedMidiTrack = midiSeqFocusedTrack();
    const Page pg = currentPage();
    const bool onInstPage = (pg == Page::Instruments || pg == Page::InstrumentEdit || pg == Page::InstrumentType);
    const uint8_t thruPortMask = (fromSource == 1) ? MIDI_PORT_TRS : MIDI_PORT_BOTH;

    for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
      const MidiTrack &mt = midiTracks[t];
      bool chMatch = (mt.inChannel != MIDI_IN_OFF) && (mt.inChannel == 0 || mt.inChannel == inCh1To16);
      if (!chMatch) continue;

      bool isArmed = (mt.armMode != ArmMode::Off);
      bool isSelected = (t == selectedMidiTrack);
      if (onInstPage && !running) {
        isArmed = false;
        isSelected = false;
      }

      if ((isArmed || isSelected) && mt.outChannel > 0) {
        uint8_t outCh = min(mt.outChannel, (uint8_t)MAX_MIDI_OUT_CHAN);
        uint32_t bit = 1u << (outCh - 1);
        bool allowLiveOut = (outCh >= 17) || songSettings().midiThru;
        if (allowLiveOut && !(thruSentMask & bit)) {
          if (sendMidiOut(type, outCh, d1, d2, thruPortMask, sampleOffset, false)) {
            thruSentMask |= bit;
          }
        }
      }
    }

    if (onInstPage) {
      uint8_t instDest = (uint8_t)(17 + constrain(instListCursor, 0, NUM_INSTRUMENTS - 1));
      uint32_t bit = 1u << (instDest - 1);
      if (!(thruSentMask & bit)) {
        if (sendMidiOut(type, instDest, d1, d2, MIDI_PORT_BOTH, sampleOffset, false)) {
          thruSentMask |= bit;
        }
      }
    }
  }

  irq = saveIrq();
  uint32_t nextH = inHead + 1;
  if (nextH - inTail <= IN_RING_SIZE) {
    volatile RawMidiMsg &m = inRing[inHead % IN_RING_SIZE];
    m.status = status;
    m.d1 = d1;
    m.d2 = d2;
    m.source = fromSource;
    m.sampleOffset = sampleOffset;
    m.thruSentMask = thruSentMask;
    m.tapeFrame = tf;
    m.tMode = tm;
    inHead = nextH;
  }
  restoreIrq(irq);
}

// Erase events on track t in the tick interval [fromTick, toTick]
FLASHMEM static void eraseEventsInRange(int t, uint32_t fromTick, uint32_t toTick) {
  if (fromTick > toTick) return;
  SeqEditGuard guard;
  MidiTrack &mt = midiTracks[t];
  MidiEvent *ev = eventPool[t];
  uint32_t n = mt.eventCount;
  if (n == 0) return;

  uint32_t writeIdx = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (ev[i].tick < fromTick || ev[i].tick > toTick) {
      if (writeIdx != i) ev[writeIdx] = ev[i];
      writeIdx++;
    } else if ((ev[i].status & 0xF0) == 0x80) {
      uint8_t note = ev[i].data1 & 0x7F;
      if (!isRecNoteOpen(t, note) && (mt.activeNoteMask[note >> 5] & (1u << (note & 31)))) {
        sendMidiOut(0x80, mt.outChannel, note, 0);
        setTrackNoteActive(t, note, false);
      }
    }
  }
  if (writeIdx != n) {
    mt.eventCount = writeIdx;
    uint32_t p = 0;
    if (transport.speedQuarter < 0) {
      while (p < mt.eventCount && ev[p].tick < fromTick) p++;
    } else {
      // Update playIndex so it points to the first event > toTick
      while (p < mt.eventCount && ev[p].tick <= toTick) p++;
    }
    mt.playIndex = p;
    mt.overviewVersion++;
  }
}

static inline bool isNoteOffEv(const MidiEvent &e) {
  return (e.status & 0xF0) == 0x80;
}

// Insert an event in sorted tick order into track t (at identical ticks, Note-Offs precede Note-Ons)
FLASHMEM static void insertEventSorted(int t, const MidiEvent &e, bool fromRecording = false) {
  SeqEditGuard guard;
  MidiTrack &mt = midiTracks[t];
  if (mt.eventCount >= MAX_MIDI_EVENTS_PER_TRACK) return;
  MidiEvent *ev = eventPool[t];
  const bool eIsOff = isNoteOffEv(e);

  // Fast path: append at end if tick >= last event's tick
  uint32_t pos = mt.eventCount;
  while (pos > 0) {
    const MidiEvent &prev = ev[pos - 1];
    if (prev.tick > e.tick) {
      ev[pos] = prev;
      pos--;
      continue;
    }
    if (prev.tick == e.tick && eIsOff && !isNoteOffEv(prev)) {
      if (!(fromRecording && (prev.status & 0xF0) == 0x90 && prev.data1 == e.data1)) {
        ev[pos] = prev;
        pos--;
        continue;
      }
    }
    break;
  }
  ev[pos] = e;
  mt.eventCount++;
  if (transport.speedQuarter < 0) {
    if (pos < mt.playIndex) mt.playIndex++;
    if (fromRecording && mt.playIndex > pos) mt.playIndex = pos;
  } else {
    if (pos <= mt.playIndex) mt.playIndex++;
    if (fromRecording && mt.playIndex <= pos) mt.playIndex = pos + 1;
  }
  mt.overviewVersion++;
}

// In Record (Replace) mode, erase existing events up to targetTick and advance recEraseCursorTick past targetTick
FLASHMEM static void advanceRecordReplaceErase(int t, uint32_t targetTick, bool rev) {
  MidiTrack &mt = midiTracks[t];
  if (!mt.recWindowActive || mt.armMode != ArmMode::Record) return;
  if (rev) {
    if (targetTick <= mt.recEraseCursorTick) {
      uint32_t beforeCount = mt.eventCount;
      eraseEventsInRange(t, targetTick, mt.recEraseCursorTick);
      if (mt.eventCount != beforeCount) {
        markSongDirty();
      }
      mt.recEraseCursorTick = (targetTick > 0) ? (targetTick - 1u) : 0u;
    }
  } else {
    if (targetTick >= mt.recEraseCursorTick) {
      uint32_t beforeCount = mt.eventCount;
      eraseEventsInRange(t, mt.recEraseCursorTick, targetTick);
      if (mt.eventCount != beforeCount) {
        markSongDirty();
      }
      mt.recEraseCursorTick = targetTick + 1u;
    }
  }
}

// Synthesize closing Note-Off (or Note-On in reverse) events for any notes still held when recording ends
FLASHMEM static void closeOpenRecordedNotes(int t, uint32_t endTick, bool rev, float curBpm) {
  if (t < 0 || t >= NUM_MIDI_TRACKS || !hasAnyRecNoteOpen(t)) return;
  MidiTrack &mt = midiTracks[t];
  MidiEvent *ev = eventPool[t];
  const uint32_t maxAllowedTick = framesToMidiTicks(MAX_REC_FRAMES, curBpm);

  for (uint8_t n = 0; n < 128; n++) {
    if (!isRecNoteOpen(t, n)) continue;
    setRecNoteOpen(t, n, false);

    if (rev) {
      uint32_t offTick = endTick;
      uint8_t vel = 100;
      int32_t foundIdx = -1;
      for (uint32_t k = 0; k < mt.eventCount; k++) {
        if (ev[k].data1 == n && (ev[k].status & 0xF0) == 0x80) {
          offTick = ev[k].tick;
          if (ev[k].data2 > 0) vel = ev[k].data2;
          foundIdx = (int32_t)k;
          break;
        }
      }
      if (foundIdx < 0) continue;
      uint32_t onTick = endTick;
      if (offTick == 0) {
        ev[foundIdx].tick = 1u;
        onTick = 0u;
      } else if (onTick >= offTick) {
        onTick = offTick - 1u;
      }
      advanceRecordReplaceErase(t, onTick, true);
      MidiEvent onEv = {onTick, 0x90, n, vel, 0};
      insertEventSorted(t, onEv, true);
      markSongDirty();
    } else {
      uint32_t onTick = endTick;
      int32_t foundIdx = -1;
      for (uint32_t k = mt.eventCount; k > 0; k--) {
        const MidiEvent &prev = ev[k - 1];
        if (prev.data1 == n && (prev.status & 0xF0) == 0x90) {
          onTick = prev.tick;
          foundIdx = (int32_t)(k - 1);
          break;
        }
      }
      if (foundIdx < 0) continue;
      uint32_t offTick = max(endTick, onTick + 1u);
      if (offTick > maxAllowedTick) offTick = maxAllowedTick;
      advanceRecordReplaceErase(t, offTick, false);
      MidiEvent offEv = {offTick, 0x80, n, 0, 0};
      insertEventSorted(t, offEv, true);
      markSongDirty();
    }
  }
}

FLASHMEM void midiSeqRebuildAllOverviews(float bpm, uint32_t displayMaxFrames) {
  (void)bpm;
  (void)displayMaxFrames;
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    midiTracks[t].overviewVersion++;
  }
}

FLASHMEM static void syncPlayIndexToTick(int t, uint32_t tick) {
  SeqEditGuard guard;
  MidiTrack &mt = midiTracks[t];
  const MidiEvent *ev = eventPool[t];
  uint32_t idx = 0;
  if (transport.speedQuarter < 0) {
    while (idx < mt.eventCount && ev[idx].tick <= tick) idx++;
  } else {
    while (idx < mt.eventCount && ev[idx].tick < tick) idx++;
  }
  mt.playIndex = idx;
}

static uint32_t lastSeenLoopWrapCount = 0;
static bool wasRunning = false;

FLASHMEM static void drainMidiInputRing(float curBpm, int selectedMidiTrack, bool midiThruEnabled, bool allowMappedCc);

FLASHMEM void midiSeqOnTransportStart(uint32_t tapeFrame, float bpm) {
  lastSeenLoopWrapCount = transport.loopWrapCount;
  uint32_t startTick = framesToMidiTicks(tapeFrame, bpm);
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    syncPlayIndexToTick(t, startTick);
    midiTracks[t].recWindowActive = false;
    midiTracks[t].recEraseCursorTick = startTick;
    clearRecNoteOpenMask(t);
  }
}

FLASHMEM void midiSeqOnTransportStop() {
  // Drain any MIDI input messages that arrived before transport stopped
  drainMidiInputRing(bpm, midiSeqFocusedTrack(), songSettings().midiThru, true);

  const bool rev = (transport.speedQuarter < 0);
  const uint32_t stopTick = framesToMidiTicks(transport.tapeFrame, bpm);
  bool anyStoppedRec = false;

  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    MidiTrack &mt = midiTracks[t];
    if (mt.recWindowActive || hasAnyRecNoteOpen(t)) {
      advanceRecordReplaceErase(t, stopTick, rev);
      closeOpenRecordedNotes(t, stopTick, rev, bpm);
      mt.recWindowActive = false;
      mt.armMode = ArmMode::Off;
      syncPlayIndexToTick(t, stopTick);
      mt.overviewVersion++;
      anyStoppedRec = true;
    }
  }
  wasRunning = false;
  midiSeqAllNotesOff();
  if (anyStoppedRec) {
    uiForceRedraw();
  }
}

FLASHMEM void midiSeqOnSeek(uint32_t tapeFrame, float bpm) {
  lastSeenLoopWrapCount = transport.loopWrapCount;
  const bool rev = (transport.speedQuarter < 0);
  uint32_t tick = framesToMidiTicks(tapeFrame, bpm);
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    if (hasAnyRecNoteOpen(t)) {
      closeOpenRecordedNotes(t, midiTracks[t].recEraseCursorTick, rev, bpm);
    }
    syncPlayIndexToTick(t, tick);
    midiTracks[t].recEraseCursorTick = tick;
  }
  midiSeqAllNotesOff();
}

static inline uint8_t computePlaybackSampleOffset(uint32_t evTick, uint32_t blockStartFrame, float curBpm, bool rev) {
  if (rev) return 0;
  uint32_t evFrame = midiTicksToFrames(evTick, curBpm);
  if (evFrame <= blockStartFrame) return 0;
  uint32_t diff = evFrame - blockStartFrame;
  return (diff < AUDIO_BLOCK_SAMPLES) ? (uint8_t)diff : (uint8_t)(AUDIO_BLOCK_SAMPLES - 1);
}

// Dispatch due playback events across all unmuted MIDI tracks up to curTick.
// Safe to call from both 1 kHz usbTimer ISR (fromIsr == true) and loop() / keepStreaming() (fromIsr == false).
static void midiSeqDispatchPlayback(float curBpm, bool fromIsr) {
  if (fromIsr && seqEditDepth != 0) return;

  uint32_t irq = saveIrq();
  uint32_t tf = transport.tapeFrame;
  TransportMode tm = transport.mode;
  int8_t speedQ = transport.speedQuarter;
  uint32_t wrapNow = transport.loopWrapCount;
  restoreIrq(irq);

  if (tm != TransportMode::Running || speedQ == 0) return;
  if (fromIsr && wrapNow != lastSeenLoopWrapCount) return;

  const bool rev = (speedQ < 0);
  const uint32_t curTick = framesToMidiTicks(tf, curBpm);
  const uint32_t blockStartFrame = tf & ~(uint32_t)(AUDIO_BLOCK_SAMPLES - 1);
  const bool anyMidiSolo = anyTrackSolo();
  const uint32_t nowMs = millis();
  const bool allowSdIo = !fromIsr;

  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    MidiTrack &mt = midiTracks[t];
    if (fromIsr && mt.recWindowActive && mt.armMode == ArmMode::Record) {
      // Wait for loop() to erase replaced events first
      continue;
    }
    const MidiEvent *ev = eventPool[t];
    bool audible = !mt.muted && (!anyMidiSolo || mt.solo) && (mt.outChannel != 0);
    if (!audible) {
      clearTrackActiveNotes(t);
    }
    if (rev) {
      while (mt.playIndex > 0 && ev[mt.playIndex - 1].tick >= curTick) {
        const MidiEvent &e = ev[mt.playIndex - 1];
        if (audible) {
          uint8_t hi = e.status & 0xF0;
          uint8_t outStatus = e.status;
          uint8_t outD2 = e.data2;
          if (isNoteOffEv(e)) {
            outStatus = 0x90;
            outD2 = (hi == 0x80 && e.data2 > 0) ? e.data2 : 100;
            bool matchedVelZero = false;
            for (uint32_t k = mt.playIndex - 1; k > 0; k--) {
              const MidiEvent &prev = ev[k - 1];
              if (prev.data1 == e.data1 && (prev.status & 0xF0) == 0x90) {
                outD2 = prev.data2;
                matchedVelZero = (prev.data2 == 0);
                break;
              }
            }
            if (matchedVelZero) {
              mt.playIndex--;
              continue;
            }
          } else if (hi == 0x90) {
            outStatus = 0x80;
            outD2 = 0;
          }
          if (!sendMidiOut(outStatus, mt.outChannel, e.data1, outD2, MIDI_PORT_BOTH, 0, allowSdIo)) {
            // Uncached Sampler note in ISR: leave playIndex intact for keepStreaming() / loop()
            break;
          }
          mt.playIndex--;
          uint8_t outHi = outStatus & 0xF0;
          if (outHi == 0x90 && outD2 > 0) {
            setTrackNoteActive(t, e.data1, true);
          } else if (outHi == 0x80 || (outHi == 0x90 && outD2 == 0)) {
            setTrackNoteActive(t, e.data1, false);
          }
          mt.lastStatus = outStatus;
          mt.lastData1 = e.data1;
          mt.lastData2 = outD2;
          mt.lastActivityMs = nowMs;
        } else {
          mt.playIndex--;
        }
      }
    } else {
      while (mt.playIndex < mt.eventCount && ev[mt.playIndex].tick <= curTick) {
        const MidiEvent &e = ev[mt.playIndex];
        if (audible) {
          uint8_t hi = e.status & 0xF0;
          if (hi == 0x90 && e.data2 == 0) {
            mt.playIndex++;
            continue;
          }
          uint8_t sampleOff = computePlaybackSampleOffset(e.tick, blockStartFrame, curBpm, false);
          if (!sendMidiOut(e.status, mt.outChannel, e.data1, e.data2, MIDI_PORT_BOTH, sampleOff, allowSdIo)) {
            // Uncached Sampler note in ISR: leave playIndex intact for keepStreaming() / loop()
            break;
          }
          mt.playIndex++;
          if (hi == 0x90 && e.data2 > 0) {
            setTrackNoteActive(t, e.data1, true);
          } else if (hi == 0x80) {
            setTrackNoteActive(t, e.data1, false);
          }
          mt.lastStatus = e.status;
          mt.lastData1 = e.data1;
          mt.lastData2 = e.data2;
          mt.lastActivityMs = nowMs;
        } else {
          mt.playIndex++;
        }
      }
    }
  }
}

FLASHMEM static void drainMidiInputRing(float curBpm, int selectedMidiTrack, bool midiThruEnabled, bool allowMappedCc) {
  if (midiActivityPending || inTail != inHead) {
    midiActivityPending = false;
    noteUserActivity();
  }
  const bool running = (transport.mode == TransportMode::Running);
  const int8_t speedQ = transport.speedQuarter;
  const bool rev = (speedQ < 0);
  const bool canRecordSpeed = (speedQ == DEFAULT_SPEED_QUARTER || speedQ == REVERSE_1X_SPEED_QUARTER);
  const uint32_t punchInTick = framesToMidiTicks(transport.punchInFrame, curBpm);
  const uint32_t punchOutTick = framesToMidiTicks(transport.punchOutFrame, curBpm);
  const uint32_t maxAllowedTick = framesToMidiTicks(MAX_REC_FRAMES, curBpm);
  const bool loopOn = transport.loopEnabled && (punchOutTick > punchInTick);

  while (inTail != inHead) {
    RawMidiMsg m;
    uint32_t irq = saveIrq();
    const volatile RawMidiMsg &slot = inRing[inTail % IN_RING_SIZE];
    m.status = slot.status;
    m.d1 = slot.d1;
    m.d2 = slot.d2;
    m.source = slot.source;
    m.sampleOffset = slot.sampleOffset;
    m.thruSentMask = slot.thruSentMask;
    m.tapeFrame = slot.tapeFrame;
    m.tMode = slot.tMode;
    uint8_t typeHi = m.status & 0xF0;
    uint8_t inCh1To16 = (m.status & 0x0F) + 1;
    if (!allowMappedCc && typeHi == 0xB0 && midiMapIsCcIntercepted(inCh1To16, m.d1)) {
      restoreIrq(irq);
      break;
    }
    inTail++;
    restoreIrq(irq);

    if (typeHi == 0xB0 && allowMappedCc && midiMapHandleCc(inCh1To16, m.d1, m.d2)) {
      continue;
    }
    uint32_t msgTick = framesToMidiTicks(m.tapeFrame, curBpm);
    if (msgTick > maxAllowedTick) msgTick = maxAllowedTick;
    uint32_t nowMs = millis();
    uint8_t thruPortMask = (m.source == 1) ? MIDI_PORT_TRS : MIDI_PORT_BOTH;

    const Page pg = currentPage();
    const bool onInstPage = (pg == Page::Instruments || pg == Page::InstrumentEdit || pg == Page::InstrumentType);
    const bool msgRunning = running || (m.tMode == TransportMode::Running);
    uint32_t thruSentMask = m.thruSentMask; // destinations already dispatched in 250 us ISR

    for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
      MidiTrack &mt = midiTracks[t];
      bool chMatch = (mt.inChannel != MIDI_IN_OFF) && (mt.inChannel == 0 || mt.inChannel == inCh1To16);
      if (!chMatch) continue;

      bool isArmed = (mt.armMode != ArmMode::Off);
      bool isSelected = (t == selectedMidiTrack);
      if (onInstPage && !msgRunning) {
        isArmed = false;
        isSelected = false;
      }

      if (isArmed || isSelected) {
        mt.lastStatus = typeHi;
        mt.lastData1 = m.d1;
        mt.lastData2 = m.d2;
        mt.lastActivityMs = nowMs;
        if (typeHi == 0x90 && m.d2 > 0) {
          setTrackNoteActive(t, m.d1, true);
        } else if (typeHi == 0x80 || (typeHi == 0x90 && m.d2 == 0)) {
          setTrackNoteActive(t, m.d1, false);
        }

        // Fallback trigger if not already dispatched in 250 us ISR (e.g. uncached 5th+ Sampler key)
        if (mt.outChannel > 0) {
          uint8_t outCh = min(mt.outChannel, (uint8_t)MAX_MIDI_OUT_CHAN);
          uint32_t bit = 1u << (outCh - 1);
          bool allowLiveOut = (outCh >= 17) || midiThruEnabled;
          if (allowLiveOut && !(thruSentMask & bit)) {
            sendMidiOut(typeHi, outCh, m.d1, m.d2, thruPortMask, m.sampleOffset, true);
            thruSentMask |= bit;
          }
        }

        // Record into track if currently inside active recording window
        if (isArmed && canRecordSpeed && (mt.recWindowActive || m.tMode == TransportMode::Running)) {
          bool insidePunch = !transport.punchEnabled ||
                             (rev ? (msgTick > punchInTick && msgTick <= punchOutTick)
                                  : (msgTick >= punchInTick && msgTick < punchOutTick));
          if (loopOn && (rev ? (msgTick < punchInTick || msgTick > punchOutTick) : (msgTick >= punchOutTick))) {
            insidePunch = false;
          }
          if (insidePunch) {
            if (!mt.recWindowActive) {
              mt.recWindowActive = true;
              if (transport.punchEnabled) {
                if (rev && mt.recEraseCursorTick > punchOutTick) mt.recEraseCursorTick = punchOutTick;
                else if (!rev && mt.recEraseCursorTick < punchInTick) mt.recEraseCursorTick = punchInTick;
              }
              if (rev && mt.recEraseCursorTick < msgTick) mt.recEraseCursorTick = msgTick;
              else if (!rev && mt.recEraseCursorTick > msgTick) mt.recEraseCursorTick = msgTick;
              clearRecNoteOpenMask(t);
            }

            MidiEvent *evPool = eventPool[t];
            if (rev) {
              if (typeHi == 0x90 && m.d2 > 0) {
                // Reverse Note-On: marks the higher-tick end (0x80) of the note in tape coordinates
                advanceRecordReplaceErase(t, msgTick, true);
                if (isRecNoteOpen(t, m.d1)) {
                  closeOpenRecordedNotes(t, msgTick, true, curBpm);
                }
                MidiEvent ev = {msgTick, 0x80, m.d1, m.d2, 0};
                insertEventSorted(t, ev, true);
                setRecNoteOpen(t, m.d1, true);
                markSongDirty();
              } else if (typeHi == 0x80 || (typeHi == 0x90 && m.d2 == 0)) {
                // Reverse Note-Off: marks the lower-tick start (0x90) of the note in tape coordinates
                if (isRecNoteOpen(t, m.d1)) {
                  setRecNoteOpen(t, m.d1, false);
                  uint32_t offTick = msgTick;
                  uint8_t vel = 100;
                  int32_t foundIdx = -1;
                  for (uint32_t k = 0; k < mt.eventCount; k++) {
                    if (evPool[k].data1 == m.d1 && (evPool[k].status & 0xF0) == 0x80) {
                      offTick = evPool[k].tick;
                      if (evPool[k].data2 > 0) vel = evPool[k].data2;
                      foundIdx = (int32_t)k;
                      break;
                    }
                  }
                  uint32_t onTick = msgTick;
                  if (foundIdx >= 0) {
                    if (offTick == 0) {
                      evPool[foundIdx].tick = 1u;
                      onTick = 0u;
                    } else if (onTick >= offTick) {
                      onTick = offTick - 1u;
                    }
                  }
                  advanceRecordReplaceErase(t, onTick, true);
                  MidiEvent ev = {onTick, 0x90, m.d1, vel, 0};
                  insertEventSorted(t, ev, true);
                  markSongDirty();
                } else {
                  advanceRecordReplaceErase(t, msgTick, true);
                }
              } else {
                advanceRecordReplaceErase(t, msgTick, true);
                MidiEvent ev = {msgTick, typeHi, m.d1, m.d2, 0};
                insertEventSorted(t, ev, true);
                markSongDirty();
              }
            } else {
              // Forward recording
              if (typeHi == 0x90 && m.d2 > 0) {
                advanceRecordReplaceErase(t, msgTick, false);
                if (isRecNoteOpen(t, m.d1)) {
                  uint32_t prevOnTick = msgTick;
                  for (uint32_t k = mt.eventCount; k > 0; k--) {
                    const MidiEvent &prev = evPool[k - 1];
                    if (prev.data1 == m.d1 && (prev.status & 0xF0) == 0x90) {
                      prevOnTick = prev.tick;
                      break;
                    }
                  }
                  uint32_t closeTick = min(maxAllowedTick, max(msgTick, prevOnTick + 1u));
                  advanceRecordReplaceErase(t, closeTick, false);
                  MidiEvent closeEv = {closeTick, 0x80, m.d1, 0, 0};
                  insertEventSorted(t, closeEv, true);
                }
                MidiEvent ev = {msgTick, 0x90, m.d1, m.d2, 0};
                insertEventSorted(t, ev, true);
                setRecNoteOpen(t, m.d1, true);
                markSongDirty();
              } else if (typeHi == 0x80 || (typeHi == 0x90 && m.d2 == 0)) {
                if (isRecNoteOpen(t, m.d1)) {
                  setRecNoteOpen(t, m.d1, false);
                  uint32_t onTick = msgTick;
                  for (uint32_t k = mt.eventCount; k > 0; k--) {
                    const MidiEvent &prev = evPool[k - 1];
                    if (prev.data1 == m.d1 && (prev.status & 0xF0) == 0x90) {
                      onTick = prev.tick;
                      break;
                    }
                  }
                  uint32_t offTick = min(maxAllowedTick, max(msgTick, onTick + 1u));
                  advanceRecordReplaceErase(t, offTick, false);
                  MidiEvent ev = {offTick, 0x80, m.d1, 0, 0};
                  insertEventSorted(t, ev, true);
                  markSongDirty();
                } else if (mt.armMode == ArmMode::Overdub) {
                  bool hasOpenOnTrack = false;
                  uint32_t onTick = msgTick;
                  for (uint32_t k = mt.eventCount; k > 0; k--) {
                    const MidiEvent &prev = evPool[k - 1];
                    if (prev.tick > msgTick) continue;
                    if (prev.data1 == m.d1) {
                      uint8_t phi = prev.status & 0xF0;
                      if (phi == 0x90) {
                        hasOpenOnTrack = true;
                        onTick = prev.tick;
                        break;
                      } else if (phi == 0x80) {
                        break;
                      }
                    }
                  }
                  if (hasOpenOnTrack) {
                    uint32_t offTick = min(maxAllowedTick, max(msgTick, onTick + 1u));
                    MidiEvent ev = {offTick, 0x80, m.d1, 0, 0};
                    insertEventSorted(t, ev, true);
                    markSongDirty();
                  }
                } else {
                  advanceRecordReplaceErase(t, msgTick, false);
                }
              } else {
                advanceRecordReplaceErase(t, msgTick, false);
                MidiEvent ev = {msgTick, typeHi, m.d1, m.d2, 0};
                insertEventSorted(t, ev, true);
                markSongDirty();
              }
            }
          }
        }
      } else if (typeHi == 0x80 || (typeHi == 0x90 && m.d2 == 0)) {
        setTrackNoteActive(t, m.d1, false);
      }
    }

    if (onInstPage) {
      uint8_t instDest = (uint8_t)(17 + constrain(instListCursor, 0, NUM_INSTRUMENTS - 1));
      uint32_t bit = 1u << (instDest - 1);
      if (!(thruSentMask & bit)) {
        sendMidiOut(typeHi, instDest, m.d1, m.d2, MIDI_PORT_BOTH, m.sampleOffset, true);
        thruSentMask |= bit;
      }
    }
  }
}

FLASHMEM void midiSeqFastService() {
  if (seqEditDepth != 0) return;
  fmSynthService();
  if (transport.loopWrapCount == lastSeenLoopWrapCount) {
    midiSeqDispatchPlayback(bpm, false);
  }
}

void midiSeqIsrTick() {
  midiSeqDispatchPlayback(bpm, true);
}

FLASHMEM void midiSeqService(float curBpm, int selectedMidiTrack, bool midiThruEnabled) {
  fmSynthService();
  uint32_t tf = transport.tapeFrame;
  bool running = (transport.mode == TransportMode::Running);
  if (wasRunning && !running) {
    midiSeqOnTransportStop();
  }
  wasRunning = running;

  const int8_t speedQ = transport.speedQuarter;
  const bool rev = (speedQ < 0);
  const bool canRecordSpeed = (speedQ == DEFAULT_SPEED_QUARTER || speedQ == REVERSE_1X_SPEED_QUARTER);
  uint32_t curTick = framesToMidiTicks(tf, curBpm);
  uint32_t punchInTick = framesToMidiTicks(transport.punchInFrame, curBpm);
  uint32_t punchOutTick = framesToMidiTicks(transport.punchOutFrame, curBpm);
  const bool loopOn = transport.loopEnabled && (punchOutTick > punchInTick);
  uint32_t wrapNow = transport.loopWrapCount;
  const bool loopWrapped = running && (wrapNow != lastSeenLoopWrapCount);
  if (loopWrapped) {
    lastSeenLoopWrapCount = wrapNow;
  }

  bool wantRecTrack[NUM_MIDI_TRACKS] = {};

  // 1. Update recording window entry & Record-Replace erasure up to curTick
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    MidiTrack &mt = midiTracks[t];
    if (loopWrapped) {
      if (mt.recWindowActive && mt.armMode == ArmMode::Record) {
        if (rev) {
          advanceRecordReplaceErase(t, punchInTick, true);
        } else if (punchOutTick > 0) {
          advanceRecordReplaceErase(t, punchOutTick - 1u, false);
        }
      }
      continue;
    }

    bool wantRec = false;
    if (running && canRecordSpeed && mt.armMode != ArmMode::Off) {
      if (transport.punchEnabled) {
        wantRec = rev ? (curTick > punchInTick && curTick <= punchOutTick)
                      : (curTick >= punchInTick && curTick < punchOutTick);
      } else if (loopOn) {
        wantRec = rev ? (curTick >= punchInTick && curTick <= punchOutTick)
                      : (curTick < punchOutTick);
      } else {
        wantRec = true;
      }
    }
    wantRecTrack[t] = wantRec;

    if (wantRec && !mt.recWindowActive) {
      mt.recWindowActive = true;
      if (transport.punchEnabled) {
        if (rev && mt.recEraseCursorTick > punchOutTick) mt.recEraseCursorTick = punchOutTick;
        else if (!rev && mt.recEraseCursorTick < punchInTick) mt.recEraseCursorTick = punchInTick;
      }
      if (rev && mt.recEraseCursorTick < curTick) mt.recEraseCursorTick = curTick;
      else if (!rev && mt.recEraseCursorTick > curTick) mt.recEraseCursorTick = curTick;
      clearRecNoteOpenMask(t);
    }

    if (wantRec && mt.recWindowActive && mt.armMode == ArmMode::Record) {
      advanceRecordReplaceErase(t, curTick, rev);
    }
  }

  // 2. Process incoming MIDI events from the input ring (Fallback Thru + Recording + Mapped CCs)
  drainMidiInputRing(curBpm, selectedMidiTrack, midiThruEnabled, true);
  midiMapService();

  // 2b. Finalize any tracks exiting their recording window (Punch-Out, Manual Disarm, or Loop Wrap)
  if (loopWrapped) {
    uint32_t endTick = rev ? punchInTick : punchOutTick;
    for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
      MidiTrack &mt = midiTracks[t];
      if (mt.recWindowActive || hasAnyRecNoteOpen(t) || mt.armMode != ArmMode::Off) {
        if (mt.recWindowActive && mt.armMode == ArmMode::Record) {
          if (rev) advanceRecordReplaceErase(t, punchInTick, true);
          else if (punchOutTick > 0) advanceRecordReplaceErase(t, punchOutTick - 1u, false);
        }
        closeOpenRecordedNotes(t, endTick, rev, curBpm);
        mt.recWindowActive = false;
        mt.armMode = ArmMode::Off;
        mt.overviewVersion++;
        uiForceRedraw();
      }
    }
    midiSeqAllNotesOff();
    uint32_t wrapTick = rev ? punchOutTick : punchInTick;
    for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
      syncPlayIndexToTick(t, wrapTick);
      midiTracks[t].recEraseCursorTick = wrapTick;
    }
  } else {
    for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
      MidiTrack &mt = midiTracks[t];
      if (!wantRecTrack[t] && (mt.recWindowActive || hasAnyRecNoteOpen(t))) {
        uint32_t endTick = curTick;
        if (transport.punchEnabled || loopOn) {
          if (rev && curTick <= punchInTick) endTick = punchInTick;
          else if (!rev && curTick >= punchOutTick) endTick = punchOutTick;
        }
        if (mt.recWindowActive && mt.armMode == ArmMode::Record) {
          if (rev) advanceRecordReplaceErase(t, endTick, true);
          else if (endTick > 0) advanceRecordReplaceErase(t, (!rev && endTick == punchOutTick) ? (endTick - 1u) : endTick, false);
        }
        closeOpenRecordedNotes(t, endTick, rev, curBpm);
        mt.recWindowActive = false;
        mt.armMode = ArmMode::Off;
        syncPlayIndexToTick(t, curTick);
        mt.overviewVersion++;
        uiForceRedraw();
      }
    }
  }

  // 3. Play back recorded MIDI events on all unmuted tracks up to curTick
  midiSeqDispatchPlayback(curBpm, false);
}

FLASHMEM const MidiEvent *midiSeqEvents(int t, uint32_t &count) {
  if (t < 0 || t >= NUM_MIDI_TRACKS) { count = 0; return nullptr; }
  count = midiTracks[t].eventCount;
  return eventPool[t];
}

// ---- SD Card Persistence (`MIDI.DAT`)
struct MidiFileHeader {
  char magic[4];       // "M8MD"
  uint16_t version;    // 2
  uint16_t numTracks;  // 8 (NUM_MIDI_TRACKS)
  struct TrackMeta {
    uint8_t inChannel;
    uint8_t outChannel;
    uint8_t muted;
    uint8_t trigMode;
    uint8_t armMode;
    uint8_t solo;
    uint8_t reserved[2];
    uint32_t eventCount;
  } tracks[NUM_MIDI_TRACKS];
};

static_assert(sizeof(MidiEvent) == 8, "MidiEvent must be 8 bytes");
static_assert(sizeof(MidiFileHeader) <= 512, "MidiFileHeader must fit in one 512-byte sector");

// Stage DMAMEM eventPool reads/writes through RAM1 stack batches so SdFat's
// DMA_SDIO never DMAs directly to/from cached RAM2 (DMAMEM) without cache maintenance.
static constexpr uint32_t MIDI_IO_BATCH = 32;

FLASHMEM bool midiSeqSave(FsFile &f) {
  MidiFileHeader hdr{};
  memcpy(hdr.magic, "M8MD", 4);
  hdr.version = 2;
  hdr.numTracks = NUM_MIDI_TRACKS;
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    hdr.tracks[t].inChannel = midiTracks[t].inChannel;
    hdr.tracks[t].outChannel = midiTracks[t].outChannel;
    hdr.tracks[t].muted = midiTracks[t].muted ? 1 : 0;
    hdr.tracks[t].trigMode = (uint8_t)midiTracks[t].trigMode;
    hdr.tracks[t].armMode = (uint8_t)midiTracks[t].armMode;
    hdr.tracks[t].solo = midiTracks[t].solo ? 1 : 0;
    hdr.tracks[t].eventCount = min(midiTracks[t].eventCount, MAX_MIDI_EVENTS_PER_TRACK);
  }
  if (!f.seekSet(0)) return false;
  if (f.write(&hdr, sizeof(hdr)) != (int)sizeof(hdr)) return false;

  MidiEvent batch[MIDI_IO_BATCH];
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    uint32_t n = hdr.tracks[t].eventCount;
    uint32_t offset = 0;
    while (offset < n) {
      uint32_t chunkCount = min(MIDI_IO_BATCH, n - offset);
      memcpy(batch, &eventPool[t][offset], chunkCount * sizeof(MidiEvent));
      size_t bytes = chunkCount * sizeof(MidiEvent);
      if (f.write(batch, bytes) != bytes) return false;
      offset += chunkCount;
    }
  }
  f.truncate();
  f.sync();
  return true;
}

FLASHMEM bool midiSeqLoad(FsFile &f, float bpm, uint32_t displayMaxFrames) {
  SeqEditGuard guard;
  midiSeqClearAll();
  if (!f.seekSet(0)) return false;

  char magic[4];
  uint16_t version = 0;
  uint16_t numTracks = 0;
  if (f.read(magic, 4) != 4 || memcmp(magic, "M8MD", 4) != 0) return false;
  if (f.read(&version, 2) != 2 || f.read(&numTracks, 2) != 2) return false;
  if (version != 2 || numTracks == 0 || numTracks > 16) return false;

  uint32_t trackCounts[NUM_MIDI_TRACKS] = {};
  int count = min((int)numTracks, NUM_MIDI_TRACKS);

  for (int t = 0; t < (int)numTracks; t++) {
    MidiFileHeader::TrackMeta meta{};
    if (f.read(&meta, sizeof(meta)) != (int)sizeof(meta)) return false;
    if (t < count) {
      MidiTrack &mt = midiTracks[t];
      mt.inChannel = constrain(meta.inChannel, (uint8_t)0, (uint8_t)MIDI_IN_OFF);
      mt.outChannel = constrain(meta.outChannel, (uint8_t)0, (uint8_t)MAX_MIDI_OUT_CHAN);
      mt.muted = meta.muted != 0;
      mt.solo = meta.solo != 0;
      mt.trigMode = (meta.trigMode == 1) ? TrigMode::Threshold : TrigMode::Manual;
      uint8_t arm = meta.armMode;
      mt.armMode = (arm == (uint8_t)ArmMode::Record)  ? ArmMode::Record :
                   (arm == (uint8_t)ArmMode::Overdub) ? ArmMode::Overdub : ArmMode::Off;
      trackCounts[t] = min(meta.eventCount, MAX_MIDI_EVENTS_PER_TRACK);
    }
  }

  MidiEvent batch[MIDI_IO_BATCH];
  for (int t = 0; t < count; t++) {
    MidiTrack &mt = midiTracks[t];
    uint32_t n = trackCounts[t];
    uint32_t offset = 0;
    while (offset < n) {
      uint32_t chunkCount = min(MIDI_IO_BATCH, n - offset);
      size_t bytes = chunkCount * sizeof(MidiEvent);
      if (f.read(batch, bytes) != (int)bytes) {
        mt.eventCount = 0;
        return false;
      }
      memcpy(&eventPool[t][offset], batch, bytes);
      offset += chunkCount;
    }
    mt.eventCount = n;
    uint32_t openMask[4] = {};
    for (uint32_t i = 0; i < n; i++) {
      MidiEvent &e = eventPool[t][i];
      uint8_t hi = e.status & 0xF0;
      uint8_t note = e.data1 & 0x7F;
      uint32_t bit = 1u << (note & 31);
      if (hi == 0x90) {
        if (e.data2 == 0 && (openMask[note >> 5] & bit)) {
          e.status = 0x80;
          openMask[note >> 5] &= ~bit;
        } else {
          openMask[note >> 5] |= bit;
        }
      } else if (hi == 0x80) {
        openMask[note >> 5] &= ~bit;
      }
    }
  }
  midiSeqRebuildAllOverviews(bpm, displayMaxFrames);
  return true;
}

static constexpr const char *MIDI_CLIP_FILE_PATH = "/Capstan/_CLIPMID.TMP";

FLASHMEM bool midiSeqCopyRegionToClip(int srcTrack, uint32_t startFrame, uint32_t endFrame, float bpm) {
  if (srcTrack < 0 || srcTrack >= NUM_MIDI_TRACKS || startFrame >= endFrame) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  uint32_t startTick = framesToMidiTicks(startFrame, bpm);
  uint32_t endTick = max(startTick + 1u, framesToMidiTicks(endFrame, bpm));
  uint32_t regionTicks = endTick - startTick;

  FsFile clipFile = SD.sdfs.open(MIDI_CLIP_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!clipFile.isOpen()) return false;

  const MidiTrack &mt = midiTracks[srcTrack];
  const MidiEvent *ev = eventPool[srcTrack];
  MidiEvent batch[MIDI_IO_BATCH];
  uint32_t batchCount = 0;
  uint32_t openNotes[4] = {}; // 128-bit mask of notes turned on within [startTick, endTick)
  bool ok = true;

  auto flushBatch = [&]() -> bool {
    if (batchCount == 0) return true;
    size_t bytes = batchCount * sizeof(MidiEvent);
    bool wrote = (clipFile.write(batch, bytes) == bytes);
    batchCount = 0;
    return wrote;
  };

  for (uint32_t i = 0; i < mt.eventCount; i++) {
    const MidiEvent &e = ev[i];
    if (e.tick < startTick) continue;
    if (e.tick > endTick) break;

    uint8_t hi = e.status & 0xF0;
    uint8_t note = e.data1 & 0x7F;
    bool isNoteOn = (hi == 0x90);
    bool isNoteOff = (hi == 0x80);

    if (e.tick == endTick) {
      // At exact endTick boundary, only include NoteOffs for notes opened inside the region
      if (!isNoteOff || !(openNotes[note >> 5] & (1u << (note & 31)))) {
        continue;
      }
    }

    if (isNoteOn) {
      openNotes[note >> 5] |= (1u << (note & 31));
    } else if (isNoteOff) {
      openNotes[note >> 5] &= ~(1u << (note & 31));
    }

    MidiEvent rel = e;
    rel.tick = min(e.tick - startTick, regionTicks - 1u);
    batch[batchCount++] = rel;
    if (batchCount == MIDI_IO_BATCH && !flushBatch()) {
      ok = false;
      break;
    }
  }

  // Synthesize NoteOffs at end of region for any notes still open so pasted clips never stick
  if (ok) {
    uint32_t endRelTick = (regionTicks > 0) ? (regionTicks - 1u) : 0u;
    for (uint8_t n = 0; n < 128; n++) {
      if (openNotes[n >> 5] & (1u << (n & 31))) {
        MidiEvent offEv = {endRelTick, 0x80, n, 0, 0};
        batch[batchCount++] = offEv;
        if (batchCount == MIDI_IO_BATCH && !flushBatch()) {
          ok = false;
          break;
        }
      }
    }
  }

  if (ok && !flushBatch()) {
    ok = false;
  }

  clipFile.truncate();
  clipFile.sync();
  clipFile.close();
  return ok;
}

FLASHMEM bool midiSeqPasteClipToTrack(int dstTrack, uint32_t destStartFrame, uint32_t regionFrames, float bpm) {
  if (dstTrack < 0 || dstTrack >= NUM_MIDI_TRACKS || regionFrames == 0 || destStartFrame >= MAX_REC_FRAMES) {
    return false;
  }
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  FsFile clipFile = SD.sdfs.open(MIDI_CLIP_FILE_PATH, O_RDONLY);
  if (!clipFile.isOpen()) return false;

  uint32_t destStartTick = framesToMidiTicks(destStartFrame, bpm);
  uint32_t destEndFrame = min((uint32_t)MAX_REC_FRAMES, destStartFrame + regionFrames);
  uint32_t destEndTick = max(destStartTick + 1u, framesToMidiTicks(destEndFrame, bpm));
  uint32_t maxAllowedTick = framesToMidiTicks(MAX_REC_FRAMES, bpm);

  // Replace existing events in destination region [destStartTick, destEndTick - 1]
  eraseEventsInRange(dstTrack, destStartTick, destEndTick - 1u);

  MidiEvent batch[MIDI_IO_BATCH];
  uint32_t totalEvents = (uint32_t)(clipFile.fileSize() / sizeof(MidiEvent));
  uint32_t offset = 0;

  while (offset < totalEvents) {
    uint32_t chunkCount = min(MIDI_IO_BATCH, totalEvents - offset);
    size_t bytes = chunkCount * sizeof(MidiEvent);
    int rd = clipFile.read(batch, bytes);
    if (rd <= 0) break;
    uint32_t got = (uint32_t)rd / sizeof(MidiEvent);
    for (uint32_t i = 0; i < got; i++) {
      MidiEvent ev = batch[i];
      uint64_t absTick = (uint64_t)destStartTick + (uint64_t)ev.tick;
      if (absTick > maxAllowedTick) absTick = maxAllowedTick;
      ev.tick = (uint32_t)absTick;
      insertEventSorted(dstTrack, ev);
    }
    offset += got;
  }

  clipFile.close();
  syncPlayIndexToTick(dstTrack, framesToMidiTicks(transport.tapeFrame, bpm));
  midiSeqRebuildAllOverviews(bpm, songMaxFrames());
  markSongDirty();
  return true;
}

FLASHMEM bool midiSeqDeleteRegion(int track, uint32_t startFrame, uint32_t endFrame, float bpm,
                                  MidiClearMode mode) {
  if (track < 0 || track >= NUM_MIDI_TRACKS || startFrame >= endFrame) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  uint32_t startTick = framesToMidiTicks(startFrame, bpm);
  uint32_t endTick = max(startTick + 1u, framesToMidiTicks(endFrame, bpm));
  if (mode == MidiClearMode::All) {
    eraseEventsInRange(track, startTick, endTick - 1u);
  } else {
    SeqEditGuard guard;
    MidiTrack &mt = midiTracks[track];
    MidiEvent *ev = eventPool[track];
    uint32_t n = mt.eventCount;
    uint32_t writeIdx = 0;
    uint32_t openBefore[4] = {};
    uint32_t openInside[4] = {};
    bool needSort = false;

    for (uint32_t i = 0; i < n; i++) {
      MidiEvent e = ev[i];
      uint8_t hi = e.status & 0xF0;
      bool isNote = (hi == 0x80 || hi == 0x90);
      bool isNoteOn = (hi == 0x90);
      bool isNoteOff = (hi == 0x80);
      uint8_t note = e.data1 & 0x7F;
      uint32_t bit = 1u << (note & 31);

      if (e.tick < startTick) {
        if (isNoteOn) openBefore[note >> 5] |= bit;
        else if (isNoteOff) openBefore[note >> 5] &= ~bit;
        if (writeIdx != i) ev[writeIdx] = e;
        writeIdx++;
      } else if (e.tick < endTick) {
        if (mode == MidiClearMode::CcPb) {
          if (isNote) {
            if (writeIdx != i) ev[writeIdx] = e;
            writeIdx++;
          }
        } else {
          // MidiClearMode::Notes
          if (!isNote) {
            if (writeIdx != i) ev[writeIdx] = e;
            writeIdx++;
          } else if (isNoteOn) {
            if (openBefore[note >> 5] & bit) {
              openBefore[note >> 5] &= ~bit;
              MidiEvent offEv = {startTick, 0x80, note, 0, 0};
              ev[writeIdx++] = offEv;
              needSort = true;
            }
            openInside[note >> 5] |= bit;
          } else if (isNoteOff) {
            if (openBefore[note >> 5] & bit) {
              openBefore[note >> 5] &= ~bit;
              e.tick = startTick;
              ev[writeIdx++] = e;
              needSort = true;
            } else {
              openInside[note >> 5] &= ~bit;
            }
          }
        }
      } else {
        if (mode == MidiClearMode::Notes && isNote) {
          if (isNoteOff && (openInside[note >> 5] & bit)) {
            openInside[note >> 5] &= ~bit;
            continue;
          }
          if (isNoteOn) {
            openInside[note >> 5] &= ~bit;
          }
        }
        if (writeIdx != i) ev[writeIdx] = e;
        writeIdx++;
      }
    }
    mt.eventCount = writeIdx;
    if (needSort && writeIdx > 1) {
      for (uint32_t i = 1; i < writeIdx; i++) {
        MidiEvent key = ev[i];
        bool keyIsOff = isNoteOffEv(key);
        uint32_t j = i;
        while (j > 0 && (ev[j - 1].tick > key.tick ||
                         (ev[j - 1].tick == key.tick && keyIsOff && !isNoteOffEv(ev[j - 1])))) {
          ev[j] = ev[j - 1];
          j--;
        }
        ev[j] = key;
      }
    }
    mt.overviewVersion++;
  }
  if (midiTracks[track].eventCount == 0) {
    midiTracks[track].muted = false;
  }
  midiSeqAllNotesOff();
  syncPlayIndexToTick(track, framesToMidiTicks(transport.tapeFrame, bpm));
  midiSeqRebuildAllOverviews(bpm, songMaxFrames());
  markSongDirty();
  return true;
}

FLASHMEM bool midiSeqQuantizeRegion(int track, uint32_t startFrame, uint32_t endFrame, uint32_t gridTicks, float bpm) {
  if (track < 0 || track >= NUM_MIDI_TRACKS || startFrame >= endFrame || gridTicks == 0) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  MidiTrack &mt = midiTracks[track];
  uint32_t n = mt.eventCount;
  if (n == 0) return false;
  SeqEditGuard guard;

  MidiEvent *ev = eventPool[track];
  uint32_t startTick = framesToMidiTicks(startFrame, bpm);
  uint32_t endTick = max(startTick + 1u, framesToMidiTicks(endFrame, bpm));
  uint32_t maxAllowedTick = framesToMidiTicks(MAX_REC_FRAMES, bpm);

  uint32_t firstGrid = ((startTick + gridTicks - 1u) / gridTicks) * gridTicks;
  uint32_t lastGrid = ((endTick - 1u) / gridTicks) * gridTicks;
  bool hasRegionGrid = (firstGrid <= lastGrid);

  auto quantizeTick = [&](uint32_t t) -> uint32_t {
    // If already on a grid point of the selected (or larger) granularity, do not shift
    if ((t % gridTicks) == 0) return t;
    uint64_t q = ((uint64_t)t + (uint64_t)(gridTicks / 2u)) / (uint64_t)gridTicks * (uint64_t)gridTicks;
    if (hasRegionGrid) {
      if (q < (uint64_t)firstGrid) q = firstGrid;
      if (q > (uint64_t)lastGrid) q = lastGrid;
    }
    if (q > (uint64_t)maxAllowedTick) q = maxAllowedTick;
    return (uint32_t)q;
  };

  int16_t openIdx[128];
  uint32_t origOnTick[128];
  for (int i = 0; i < 128; i++) {
    openIdx[i] = -1;
    origOnTick[i] = 0;
  }

  bool changed = false;
  for (uint32_t i = 0; i < n; i++) {
    ev[i].reserved = 0;
    uint8_t hi = ev[i].status & 0xF0;
    if (hi == 0x90 || hi == 0x80) {
      uint8_t note = ev[i].data1 & 0x7F;
      bool isOn = (hi == 0x90);
      if (isOn) {
        if (ev[i].tick >= startTick && ev[i].tick < endTick) {
          openIdx[note] = (int16_t)i;
          origOnTick[note] = ev[i].tick;
          uint32_t qOn = quantizeTick(ev[i].tick);
          if (qOn != ev[i].tick) {
            ev[i].tick = qOn;
            changed = true;
          }
        } else {
          openIdx[note] = -1;
        }
      } else {
        if (openIdx[note] >= 0) {
          int16_t onI = openIdx[note];
          uint32_t origOn = origOnTick[note];
          uint32_t origDur = (ev[i].tick > origOn) ? (ev[i].tick - origOn) : 1u;
          uint32_t newOn = ev[onI].tick;
          uint64_t newOff = (uint64_t)newOn + (uint64_t)origDur;
          if (newOff > (uint64_t)maxAllowedTick) newOff = maxAllowedTick;
          if (newOff <= (uint64_t)newOn && newOn < maxAllowedTick) newOff = newOn + 1u;
          if ((uint32_t)newOff != ev[i].tick) {
            ev[i].tick = (uint32_t)newOff;
            changed = true;
          }
          openIdx[note] = -1;
        }
      }
    } else {
      if (ev[i].tick >= startTick && ev[i].tick < endTick) {
        uint32_t qTick = quantizeTick(ev[i].tick);
        if (qTick != ev[i].tick) {
          ev[i].tick = qTick;
          changed = true;
        }
      }
    }
  }

  if (!changed) return true;

  // Re-sort events by tick (at identical ticks, Note-Offs precede Note-Ons)
  for (uint32_t i = 1; i < n; i++) {
    MidiEvent key = ev[i];
    bool keyIsOff = isNoteOffEv(key);
    uint32_t j = i;
    while (j > 0 && (ev[j - 1].tick > key.tick ||
                     (ev[j - 1].tick == key.tick && keyIsOff && !isNoteOffEv(ev[j - 1])))) {
      ev[j] = ev[j - 1];
      j--;
    }
    ev[j] = key;
  }

  // Resolve any same-pitch notes that snapped to the same tick or now overlap
  int16_t lastOnIdx[128];
  int16_t lastOffIdx[128];
  for (int i = 0; i < 128; i++) {
    openIdx[i] = -1;
    lastOnIdx[i] = -1;
    lastOffIdx[i] = -1;
  }
  bool removedAny = false;
  bool offTickAdjusted = false;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t hi = ev[i].status & 0xF0;
    if (hi != 0x90 && hi != 0x80) continue;
    uint8_t note = ev[i].data1 & 0x7F;
    bool isOn = (hi == 0x90);
    if (isOn) {
      openIdx[note] = (int16_t)i;
    } else if (openIdx[note] >= 0) {
      int16_t curOn = openIdx[note];
      int16_t curOff = (int16_t)i;
      openIdx[note] = -1;

      int16_t prevOn = lastOnIdx[note];
      int16_t prevOff = lastOffIdx[note];
      if (prevOn >= 0 && prevOff >= 0) {
        if (ev[curOn].tick == ev[prevOn].tick) {
          if (ev[curOn].data2 > ev[prevOn].data2) {
            ev[prevOn].data2 = ev[curOn].data2;
          }
          if (ev[curOff].tick > ev[prevOff].tick) {
            ev[prevOff].tick = ev[curOff].tick;
            offTickAdjusted = true;
          }
          ev[curOn].reserved = 1;
          ev[curOff].reserved = 1;
          removedAny = true;
          continue;
        } else if (ev[prevOff].tick > ev[curOn].tick) {
          ev[prevOff].tick = ev[curOn].tick;
          offTickAdjusted = true;
        }
      }
      lastOnIdx[note] = curOn;
      lastOffIdx[note] = curOff;
    }
  }

  if (removedAny) {
    uint32_t writeIdx = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ev[i].reserved == 0) {
        if (writeIdx != i) ev[writeIdx] = ev[i];
        writeIdx++;
      }
    }
    mt.eventCount = writeIdx;
    n = writeIdx;
  }

  if (offTickAdjusted) {
    for (uint32_t i = 1; i < n; i++) {
      MidiEvent key = ev[i];
      bool keyIsOff = isNoteOffEv(key);
      uint32_t j = i;
      while (j > 0 && (ev[j - 1].tick > key.tick ||
                       (ev[j - 1].tick == key.tick && keyIsOff && !isNoteOffEv(ev[j - 1])))) {
        ev[j] = ev[j - 1];
        j--;
      }
      ev[j] = key;
    }
  }

  midiSeqAllNotesOff();
  syncPlayIndexToTick(track, framesToMidiTicks(transport.tapeFrame, bpm));
  midiSeqRebuildAllOverviews(bpm, songMaxFrames());
  markSongDirty();
  return true;
}

// Remove any existing note on `track` with pitch `note` whose interval [t0, max(t0+1, t1))
// overlaps [startTick, endTick). Returns true if any note was removed.
FLASHMEM static bool removeOverlappingPitchNotes(int track, uint8_t note,
                                                 uint32_t startTick, uint32_t endTick,
                                                 bool stopAfterFirst) {
  if (track < 0 || track >= NUM_MIDI_TRACKS || endTick <= startTick) return false;
  SeqEditGuard guard;
  MidiTrack &mt = midiTracks[track];
  MidiEvent *ev = eventPool[track];
  uint32_t n = mt.eventCount;
  if (n == 0) return false;

  note &= 0x7F;
  for (uint32_t i = 0; i < n; i++) {
    ev[i].reserved = 0;
  }

  bool open = false;
  uint32_t openIdx = 0;
  uint32_t openTick = 0;
  bool removedAny = false;

  for (uint32_t i = 0; i < n; i++) {
    uint8_t hi = ev[i].status & 0xF0;
    if (hi != 0x80 && hi != 0x90) continue;
    if ((ev[i].data1 & 0x7F) != note) continue;

    bool isOn = (hi == 0x90);
    if (isOn) {
      if (open) {
        uint32_t t1 = max(openTick + 1u, ev[i].tick);
        if (openTick < endTick && t1 > startTick) {
          ev[openIdx].reserved = 1;
          removedAny = true;
          if (stopAfterFirst) {
            open = false;
            break;
          }
        }
      }
      open = true;
      openIdx = i;
      openTick = ev[i].tick;
    } else {
      if (open) {
        uint32_t t1 = max(openTick + 1u, ev[i].tick);
        open = false;
        if (openTick < endTick && t1 > startTick) {
          ev[openIdx].reserved = 1;
          ev[i].reserved = 1;
          removedAny = true;
          if (stopAfterFirst) break;
        }
      }
    }
  }

  if (open && !(stopAfterFirst && removedAny)) {
    if (openTick < endTick && (openTick + 1u) > startTick) {
      ev[openIdx].reserved = 1;
      removedAny = true;
    }
  }

  if (!removedAny) return false;

  uint32_t writeIdx = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (ev[i].reserved == 0) {
      if (writeIdx != i) ev[writeIdx] = ev[i];
      writeIdx++;
    }
  }
  mt.eventCount = writeIdx;
  mt.overviewVersion++;
  return true;
}

FLASHMEM bool midiSeqFindNoteAt(int track, uint8_t note, uint32_t tick,
                                uint32_t &outStartTick, uint32_t &outEndTick, uint8_t &outVelocity) {
  if (track < 0 || track >= NUM_MIDI_TRACKS) return false;
  const MidiTrack &mt = midiTracks[track];
  const MidiEvent *ev = eventPool[track];
  uint32_t n = mt.eventCount;
  if (n == 0) return false;

  const uint32_t stepTicks = MIDI_PPQN / 8; // 1/32nd note = 60 ticks
  const uint32_t startTick = tick;
  const uint32_t endTick = tick + stepTicks;
  note &= 0x7F;

  bool open = false;
  uint32_t openTick = 0;
  uint8_t openVel = 100;

  for (uint32_t i = 0; i < n; i++) {
    uint8_t hi = ev[i].status & 0xF0;
    if (hi != 0x80 && hi != 0x90) continue;
    if ((ev[i].data1 & 0x7F) != note) continue;

    if (hi == 0x90) {
      if (open) {
        uint32_t t1 = max(openTick + 1u, ev[i].tick);
        if (openTick < endTick && t1 > startTick) {
          outStartTick = openTick;
          outEndTick = t1;
          outVelocity = openVel;
          return true;
        }
      }
      open = true;
      openTick = ev[i].tick;
      openVel = ev[i].data2 & 0x7F;
    } else if (open) {
      uint32_t t1 = max(openTick + 1u, ev[i].tick);
      open = false;
      if (openTick < endTick && t1 > startTick) {
        outStartTick = openTick;
        outEndTick = t1;
        outVelocity = openVel;
        return true;
      }
    }
  }

  if (open) {
    uint32_t t1 = openTick + stepTicks;
    if (openTick < endTick && t1 > startTick) {
      outStartTick = openTick;
      outEndTick = t1;
      outVelocity = openVel;
      return true;
    }
  }
  return false;
}

FLASHMEM bool midiSeqInsertNote(int track, uint8_t note, uint32_t startTick, uint32_t endTick, uint8_t velocity, float bpm) {
  if (track < 0 || track >= NUM_MIDI_TRACKS) return false;
  if (endTick <= startTick) endTick = startTick + 1u;
  uint32_t maxAllowedTick = framesToMidiTicks(MAX_REC_FRAMES, bpm);
  if (startTick >= maxAllowedTick) return false;
  if (endTick > maxAllowedTick) endTick = maxAllowedTick;

  note &= 0x7F;
  uint8_t vel = (uint8_t)constrain((int)velocity, 0, 127);

  removeOverlappingPitchNotes(track, note, startTick, endTick, false);

  MidiTrack &mt = midiTracks[track];
  if (mt.eventCount + 2 > MAX_MIDI_EVENTS_PER_TRACK) {
    return false;
  }

  MidiEvent onEv = {startTick, 0x90, note, vel, 0};
  MidiEvent offEv = {endTick, 0x80, note, 0, 0};
  insertEventSorted(track, onEv);
  insertEventSorted(track, offEv);

  syncPlayIndexToTick(track, framesToMidiTicks(transport.tapeFrame, bpm));
  midiSeqRebuildAllOverviews(bpm, songMaxFrames());
  markSongDirty();
  return true;
}

FLASHMEM bool midiSeqDeleteNoteAt(int track, uint8_t note, uint32_t tick, float bpm) {
  if (track < 0 || track >= NUM_MIDI_TRACKS) return false;
  uint32_t stepTicks = MIDI_PPQN / 8; // 1/32nd note = 60 ticks
  if (!removeOverlappingPitchNotes(track, note, tick, tick + stepTicks, true)) {
    return false;
  }
  syncPlayIndexToTick(track, framesToMidiTicks(transport.tapeFrame, bpm));
  midiSeqRebuildAllOverviews(bpm, songMaxFrames());
  markSongDirty();
  return true;
}

// ============================================================================
// Standard MIDI File (.mid, SMF Format 0) Export to /Capstan/SONGS/<Song>/MIDI/
// ============================================================================
FLASHMEM static void formatGridPointForMid(uint32_t frames, float curBpm, char *out, size_t sz) {
  double totalBeats = framesToBeats(frames, curBpm);
  int64_t total32nds = max((int64_t)0, (int64_t)(totalBeats * 8.0 + 1e-6));
  constexpr int32_t SUB32_PER_BAR = BEATS_PER_BAR * 8;
  int32_t bar = (int32_t)(total32nds / SUB32_PER_BAR) + 1;
  int32_t sub32InBar = (int32_t)(total32nds % SUB32_PER_BAR);
  int32_t beatInBar = (sub32InBar / 8) + 1;
  int32_t eighthInBar = (sub32InBar / 4) + 1;
  int32_t sixteenthInBar = (sub32InBar / 2) + 1;
  int32_t thirtySecondInBar = sub32InBar + 1;
  snprintf(out, sz, "%03ld.%ld.%ld.%ld.%ld",
           (long)bar, (long)beatInBar, (long)eighthInBar, (long)sixteenthInBar, (long)thirtySecondInBar);
}

FLASHMEM static bool ensureSongMidiDir(char *outDir, size_t outDirSz) {
  const char *song = currentSongName();
  if (!song || !song[0]) return false;

  if (!SD.sdfs.exists(ROOT_DIR)) {
    if (!SD.sdfs.mkdir(ROOT_DIR)) return false;
  }
  if (!SD.sdfs.exists(SONGS_DIR)) {
    if (!SD.sdfs.mkdir(SONGS_DIR)) return false;
  }
  char songDir[64];
  snprintf(songDir, sizeof(songDir), "%s/%s", SONGS_DIR, song);
  if (!SD.sdfs.exists(songDir)) {
    if (!SD.sdfs.mkdir(songDir)) return false;
  }
  snprintf(outDir, outDirSz, "%s/MIDI", songDir);
  if (!SD.sdfs.exists(outDir)) {
    if (!SD.sdfs.mkdir(outDir)) return false;
  }
  return true;
}

FLASHMEM static bool writeStandardMidiFile(const char *filePath, int track,
                                           uint32_t startTick, uint32_t endTick,
                                           bool isClip, float curBpm) {
  if (track < 0 || track >= NUM_MIDI_TRACKS || !filePath || !filePath[0]) return false;

  uint8_t *buf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  constexpr size_t bufCap = CHUNK_FRAMES * sizeof(uint32_t);
  if (!buf) return false;

  FsFile f = SD.sdfs.open(filePath, O_RDWR | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return false;

  // Reserve 22 bytes at the start of the file for MThd (14 bytes) + MTrk header (8 bytes)
  constexpr size_t SMF_HDR_SIZE = 22;
  memset(buf, 0, SMF_HDR_SIZE);
  size_t bufPos = SMF_HDR_SIZE;
  uint32_t trackBytes = 0;
  bool ok = true;

  auto flushBuf = [&]() __attribute__((always_inline)) -> bool {
    if (bufPos == 0) return true;
    bool wrote = (f.write(buf, bufPos) == bufPos);
    bufPos = 0;
    return wrote;
  };

  auto emitByte = [&](uint8_t b) __attribute__((always_inline)) -> bool {
    if (bufPos >= bufCap && !flushBuf()) return false;
    buf[bufPos++] = b;
    trackBytes++;
    return true;
  };

  auto emitVlq = [&](uint32_t val) __attribute__((always_inline)) -> bool {
    uint8_t vlq[4];
    int len = 1;
    vlq[3] = (uint8_t)(val & 0x7F);
    while ((val >>= 7) > 0 && len < 4) {
      vlq[3 - len] = (uint8_t)((val & 0x7F) | 0x80);
      len++;
    }
    for (int i = 4 - len; i < 4; i++) {
      if (!emitByte(vlq[i])) return false;
    }
    return true;
  };

  // 1. Track Name Meta Event (0xFF 0x03 len "M1".."M8") at delta 0
  char trkName[4];
  snprintf(trkName, sizeof(trkName), "M%d", track + 1);
  uint8_t nameLen = (uint8_t)strlen(trkName);
  if (!emitByte(0x00) || !emitByte(0xFF) || !emitByte(0x03) || !emitByte(nameLen)) ok = false;
  for (uint8_t i = 0; i < nameLen && ok; i++) {
    if (!emitByte((uint8_t)trkName[i])) ok = false;
  }

  // 2. Tempo Meta Event (0xFF 0x51 0x03 tt tt tt) at delta 0
  float safeBpm = constrain(curBpm, MIN_BPM, MAX_BPM);
  uint32_t usPerQn = (uint32_t)constrain((int)lroundf(60000000.0f / safeBpm), 1, 0xFFFFFF);
  if (ok) {
    if (!emitByte(0x00) || !emitByte(0xFF) || !emitByte(0x51) || !emitByte(0x03) ||
        !emitByte((uint8_t)((usPerQn >> 16) & 0xFF)) ||
        !emitByte((uint8_t)((usPerQn >> 8) & 0xFF)) ||
        !emitByte((uint8_t)(usPerQn & 0xFF))) {
      ok = false;
    }
  }

  // 3. MIDI Channel Voice Events
  const MidiTrack &mt = midiTracks[track];
  const MidiEvent *ev = eventPool[track];
  const uint8_t ch = (mt.outChannel >= 1 && mt.outChannel <= 16)
                         ? (uint8_t)((mt.outChannel - 1) & 0x0F)
                         : (uint8_t)(track & 0x0F);

  uint32_t prevTick = 0;
  uint32_t openNotes[4] = {};
  const uint32_t clipSpanTicks = (isClip && endTick > startTick) ? (endTick - startTick) : 0u;

  for (uint32_t i = 0; i < mt.eventCount && ok; i++) {
    const MidiEvent &e = ev[i];
    if (isClip) {
      if (e.tick < startTick) continue;
      if (e.tick > endTick) break;
    }

    uint8_t hi = e.status & 0xF0;
    if (hi != 0x80 && hi != 0x90 && hi != 0xB0 && hi != 0xE0) continue;

    uint8_t d1 = e.data1 & 0x7F;
    uint8_t d2 = e.data2 & 0x7F;
    bool isNoteOn = (hi == 0x90 && d2 > 0);
    bool isNoteOff = (hi == 0x80 || (hi == 0x90 && d2 == 0));

    if (isClip && e.tick == endTick) {
      if (!isNoteOff || !(openNotes[d1 >> 5] & (1u << (d1 & 31)))) {
        continue;
      }
    }

    if (isNoteOn) {
      openNotes[d1 >> 5] |= (1u << (d1 & 31));
    } else if (isNoteOff) {
      openNotes[d1 >> 5] &= ~(1u << (d1 & 31));
    }

    uint32_t relTick = isClip ? min(e.tick - startTick, clipSpanTicks) : e.tick;
    uint32_t delta = (relTick >= prevTick) ? (relTick - prevTick) : 0u;
    prevTick = relTick;

    if (!emitVlq(delta) ||
        !emitByte((uint8_t)(hi | ch)) ||
        !emitByte(d1) ||
        !emitByte(d2)) {
      ok = false;
      break;
    }
  }

  // 4. Close any notes still open at the end of the track/clip
  if (ok) {
    uint32_t closeTick = isClip ? max(prevTick, clipSpanTicks) : prevTick;
    for (uint8_t n = 0; n < 128 && ok; n++) {
      if (openNotes[n >> 5] & (1u << (n & 31))) {
        uint32_t delta = (closeTick >= prevTick) ? (closeTick - prevTick) : 0u;
        prevTick = closeTick;
        if (!emitVlq(delta) ||
            !emitByte((uint8_t)(0x80 | ch)) ||
            !emitByte(n) ||
            !emitByte(0)) {
          ok = false;
          break;
        }
      }
    }
  }

  // 5. End of Track Meta Event (0xFF 0x2F 0x00)
  if (ok) {
    uint32_t eotDelta = (isClip && clipSpanTicks > prevTick) ? (clipSpanTicks - prevTick) : 0u;
    if (!emitVlq(eotDelta) || !emitByte(0xFF) || !emitByte(0x2F) || !emitByte(0x00)) {
      ok = false;
    }
  }

  if (ok && !flushBuf()) {
    ok = false;
  }

  // 6. Write Standard MIDI File MThd + MTrk header at offset 0
  if (ok) {
    // MThd chunk (14 bytes): Format 0, 1 track, MIDI_PPQN (480) ticks/quarter
    buf[0]  = 'M'; buf[1]  = 'T'; buf[2]  = 'h'; buf[3]  = 'd';
    buf[4]  = 0x00; buf[5]  = 0x00; buf[6]  = 0x00; buf[7]  = 0x06;
    buf[8]  = 0x00; buf[9]  = 0x00; // Format 0
    buf[10] = 0x00; buf[11] = 0x01; // 1 track
    buf[12] = (uint8_t)((MIDI_PPQN >> 8) & 0xFF);
    buf[13] = (uint8_t)(MIDI_PPQN & 0xFF);
    // MTrk chunk header (8 bytes)
    buf[14] = 'M'; buf[15] = 'T'; buf[16] = 'r'; buf[17] = 'k';
    buf[18] = (uint8_t)((trackBytes >> 24) & 0xFF);
    buf[19] = (uint8_t)((trackBytes >> 16) & 0xFF);
    buf[20] = (uint8_t)((trackBytes >> 8) & 0xFF);
    buf[21] = (uint8_t)(trackBytes & 0xFF);

    if (!f.seekSet(0) || f.write(buf, SMF_HDR_SIZE) != SMF_HDR_SIZE) {
      ok = false;
    }
  }

  f.sync();
  f.close();
  return ok;
}

FLASHMEM bool midiSeqExportAllTracksMid(float curBpm) {
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }
  char midiDir[80];
  if (!ensureSongMidiDir(midiDir, sizeof(midiDir))) return false;

  bool allOk = true;
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    char filePath[128];
    snprintf(filePath, sizeof(filePath), "%s/%s - M%d.mid",
             midiDir, currentSongName(), t + 1);
    if (!writeStandardMidiFile(filePath, t, 0, 0, false, curBpm)) {
      allOk = false;
    }
  }
  if (SD.sdfs.card()) {
    SD.sdfs.card()->syncDevice();
  }
  return allOk;
}

FLASHMEM bool midiSeqExportClipMid(int track, uint32_t startFrame, uint32_t endFrame, float curBpm) {
  if (track < 0 || track >= NUM_MIDI_TRACKS || startFrame >= endFrame) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }
  char midiDir[80];
  if (!ensureSongMidiDir(midiDir, sizeof(midiDir))) return false;

  char startGrid[32], endGrid[32];
  formatGridPointForMid(startFrame, curBpm, startGrid, sizeof(startGrid));
  formatGridPointForMid(endFrame, curBpm, endGrid, sizeof(endGrid));

  char filePath[160];
  snprintf(filePath, sizeof(filePath), "%s/%s - M%d - %s_%s.mid",
           midiDir, currentSongName(), track + 1, startGrid, endGrid);

  uint32_t startTick = framesToMidiTicks(startFrame, curBpm);
  uint32_t endTick = max(startTick + 1u, framesToMidiTicks(endFrame, curBpm));
  bool ok = writeStandardMidiFile(filePath, track, startTick, endTick, true, curBpm);
  if (SD.sdfs.card()) {
    SD.sdfs.card()->syncDevice();
  }
  return ok;
}

// ============================================================================
// Standard MIDI File (.mid) Parser & Loader + SD Card MIDI Clip Browser
// ============================================================================
struct SmfBufferedReader {
  FsFile *file;
  uint8_t *buf;
  uint32_t fileLen;
  uint32_t bufStart;
  uint32_t bufEnd;
};

FLASHMEM static bool smfReadByteAt(SmfBufferedReader &r, uint32_t pos, uint8_t &out) {
  if (pos >= r.fileLen) return false;
  if (pos < r.bufStart || pos >= r.bufEnd) {
    if (r.file->curPosition() != pos) {
      if (!r.file->seekSet(pos)) return false;
    }
    constexpr uint32_t bufCap = (uint32_t)(CHUNK_FRAMES * sizeof(uint32_t));
    size_t toRead = (size_t)min(bufCap, r.fileLen - pos);
    int rd = r.file->read(r.buf, toRead);
    if (rd <= 0) return false;
    r.bufStart = pos;
    r.bufEnd = pos + (uint32_t)rd;
  }
  out = r.buf[pos - r.bufStart];
  return true;
}

FLASHMEM static bool smfReadU16BE(SmfBufferedReader &r, uint32_t pos, uint16_t &out) {
  uint8_t b0 = 0, b1 = 0;
  if (!smfReadByteAt(r, pos, b0) || !smfReadByteAt(r, pos + 1, b1)) return false;
  out = (uint16_t)(((uint16_t)b0 << 8) | (uint16_t)b1);
  return true;
}

FLASHMEM static bool smfReadU32BE(SmfBufferedReader &r, uint32_t pos, uint32_t &out) {
  uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  if (!smfReadByteAt(r, pos, b0) || !smfReadByteAt(r, pos + 1, b1) ||
      !smfReadByteAt(r, pos + 2, b2) || !smfReadByteAt(r, pos + 3, b3)) {
    return false;
  }
  out = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
  return true;
}

FLASHMEM static bool smfReadVlq(SmfBufferedReader &r, uint32_t &pos, uint32_t endPos, uint32_t &outVal) {
  outVal = 0;
  for (int i = 0; i < 4; i++) {
    if (pos >= endPos) return false;
    uint8_t b = 0;
    if (!smfReadByteAt(r, pos++, b)) return false;
    outVal = (outVal << 7) | (uint32_t)(b & 0x7Fu);
    if ((b & 0x80u) == 0) return true;
  }
  return true;
}

FLASHMEM bool midiSeqLoadMidFileToTrack(int dstTrack, uint32_t destStartFrame,
                                        const char *filePath, float curBpm) {
  if (dstTrack < 0 || dstTrack >= NUM_MIDI_TRACKS || !filePath || !filePath[0]) return false;
  if (destStartFrame >= MAX_REC_FRAMES) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  FsFile f = SD.sdfs.open(filePath, O_RDONLY);
  if (!f.isOpen()) return false;

  uint8_t *buf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  const uint32_t fileLen = (uint32_t)min((uint64_t)0xFFFFFFFFu, f.fileSize());
  if (!buf || fileLen < 14) {
    f.close();
    return false;
  }

  SmfBufferedReader rdr{&f, buf, fileLen, 0, 0};

  // Verify MThd header
  uint8_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
  uint32_t hdrLen = 0;
  uint16_t fmt = 0, numTrks = 0, division = 0;
  if (!smfReadByteAt(rdr, 0, m0) || !smfReadByteAt(rdr, 1, m1) ||
      !smfReadByteAt(rdr, 2, m2) || !smfReadByteAt(rdr, 3, m3) ||
      m0 != 'M' || m1 != 'T' || m2 != 'h' || m3 != 'd' ||
      !smfReadU32BE(rdr, 4, hdrLen) || hdrLen < 6 ||
      !smfReadU16BE(rdr, 8, fmt) || !smfReadU16BE(rdr, 10, numTrks) || !smfReadU16BE(rdr, 12, division)) {
    f.close();
    return false;
  }

  uint32_t srcPpqn = MIDI_PPQN;
  if ((division & 0x8000u) == 0 && (division & 0x7FFFu) > 0) {
    srcPpqn = (uint32_t)(division & 0x7FFFu);
  }

  const uint32_t destStartTick = framesToMidiTicks(destStartFrame, curBpm);
  const uint32_t maxAllowedTick = framesToMidiTicks(MAX_REC_FRAMES, curBpm);
  const uint32_t firstChunkPos = 8u + hdrLen;

  auto scaleTick = [&](uint64_t srcTick) __attribute__((always_inline)) -> uint32_t {
    uint64_t scaled = (srcTick * (uint64_t)MIDI_PPQN + (uint64_t)(srcPpqn / 2u)) / (uint64_t)srcPpqn;
    if (scaled > (uint64_t)maxAllowedTick) scaled = maxAllowedTick;
    return (uint32_t)scaled;
  };

  // Pass 0 = scan clip tick span, Pass 1 = insert Note / CC / PitchBend events
  uint32_t maxRelTick = 0;
  bool foundAnyTrack = false;

  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      if (!foundAnyTrack) {
        f.close();
        return false;
      }
      if (maxRelTick > 0) {
        uint64_t endT = min((uint64_t)maxAllowedTick, (uint64_t)destStartTick + (uint64_t)maxRelTick);
        if (endT > (uint64_t)destStartTick) {
          eraseEventsInRange(dstTrack, destStartTick, (uint32_t)(endT - 1u));
        }
      }
    }

    uint32_t chunkPos = firstChunkPos;
    uint16_t trksSeen = 0;
    while (chunkPos + 8u <= fileLen && (numTrks == 0 || trksSeen < numTrks)) {
      uint8_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
      uint32_t chunkLen = 0;
      if (!smfReadByteAt(rdr, chunkPos, c0) || !smfReadByteAt(rdr, chunkPos + 1, c1) ||
          !smfReadByteAt(rdr, chunkPos + 2, c2) || !smfReadByteAt(rdr, chunkPos + 3, c3) ||
          !smfReadU32BE(rdr, chunkPos + 4, chunkLen)) {
        break;
      }
      uint32_t dataPos = chunkPos + 8u;
      uint32_t dataEnd = (chunkLen <= fileLen - dataPos) ? (dataPos + chunkLen) : fileLen;
      chunkPos = dataEnd;

      if (c0 != 'M' || c1 != 'T' || c2 != 'r' || c3 != 'k') {
        continue;
      }
      trksSeen++;
      foundAnyTrack = true;

      uint32_t pos = dataPos;
      uint64_t absSrcTick = 0;
      uint8_t runningStatus = 0;
      uint32_t openNotes[4] = {};

      while (pos < dataEnd) {
        uint32_t delta = 0;
        if (!smfReadVlq(rdr, pos, dataEnd, delta)) break;
        absSrcTick += (uint64_t)delta;
        uint32_t relTick = scaleTick(absSrcTick);
        if (pass == 0 && relTick > maxRelTick) {
          maxRelTick = relTick;
        }

        if (pos >= dataEnd) break;
        uint8_t b = 0;
        if (!smfReadByteAt(rdr, pos++, b)) break;

        if (b == 0xFFu) {
          // Meta Event: 0xFF <type> <vlq len> <data>
          runningStatus = 0;
          if (pos >= dataEnd) break;
          uint8_t metaType = 0;
          if (!smfReadByteAt(rdr, pos++, metaType)) break;
          uint32_t metaLen = 0;
          if (!smfReadVlq(rdr, pos, dataEnd, metaLen)) break;
          if (metaLen > dataEnd - pos) break;
          pos += metaLen;
          if (metaType == 0x2Fu) {
            break; // End of Track
          }
          continue;
        }

        if (b == 0xF0u || b == 0xF7u) {
          // SysEx Event: 0xF0/0xF7 <vlq len> <data>
          runningStatus = 0;
          uint32_t sysexLen = 0;
          if (!smfReadVlq(rdr, pos, dataEnd, sysexLen)) break;
          if (sysexLen > dataEnd - pos) break;
          pos += sysexLen;
          continue;
        }

        if (b >= 0xF8u) {
          // Real-time system byte inside stream (rare): 0 data bytes
          continue;
        }
        if (b > 0xF0u) {
          // System Common message
          runningStatus = 0;
          if (b == 0xF2u) pos = min(dataEnd, pos + 2u);
          else if (b == 0xF1u || b == 0xF3u) pos = min(dataEnd, pos + 1u);
          continue;
        }

        uint8_t st = 0;
        uint8_t d1 = 0;
        if ((b & 0x80u) != 0) {
          st = b;
          runningStatus = st;
          if (pos >= dataEnd || !smfReadByteAt(rdr, pos++, d1)) break;
          d1 &= 0x7Fu;
        } else {
          if (runningStatus == 0) break;
          st = runningStatus;
          d1 = b & 0x7Fu;
        }

        uint8_t hi = st & 0xF0u;
        if (hi == 0xC0u || hi == 0xD0u) {
          // 1-data-byte channel messages (Program Change, Channel Aftertouch): ignore
          continue;
        }

        // 2-data-byte channel messages
        if (pos >= dataEnd) break;
        uint8_t d2 = 0;
        if (!smfReadByteAt(rdr, pos++, d2)) break;
        d2 &= 0x7Fu;

        // Keep only Note On/Off (0x90/0x80), CC (0xB0), and Pitch Bend (0xE0)
        if (hi != 0x80u && hi != 0x90u && hi != 0xB0u && hi != 0xE0u) {
          continue;
        }

        if (pass == 1) {
          uint8_t evStatus = hi;
          uint8_t evD1 = d1;
          uint8_t evD2 = d2;
          if (hi == 0x90u && d2 == 0) {
            evStatus = 0x80u;
          }
          if (evStatus == 0x90u) {
            openNotes[evD1 >> 5] |= (1u << (evD1 & 31));
          } else if (evStatus == 0x80u) {
            openNotes[evD1 >> 5] &= ~(1u << (evD1 & 31));
          }

          uint64_t absTick = min((uint64_t)maxAllowedTick, (uint64_t)destStartTick + (uint64_t)relTick);
          MidiEvent ev = {(uint32_t)absTick, evStatus, evD1, evD2, 0};
          insertEventSorted(dstTrack, ev);
        }
      }

      // Close any notes left open at the end of this MTrk
      if (pass == 1) {
        uint64_t closeTick = min((uint64_t)maxAllowedTick,
                                 (uint64_t)destStartTick + (uint64_t)max(scaleTick(absSrcTick), maxRelTick));
        for (uint8_t n = 0; n < 128; n++) {
          if (openNotes[n >> 5] & (1u << (n & 31))) {
            MidiEvent offEv = {(uint32_t)closeTick, 0x80u, n, 0, 0};
            insertEventSorted(dstTrack, offEv);
          }
        }
      }
    }
  }

  f.close();
  syncPlayIndexToTick(dstTrack, framesToMidiTicks(transport.tapeFrame, curBpm));
  midiSeqRebuildAllOverviews(curBpm, songMaxFrames());
  markSongDirty();
  return true;
}

static constexpr int MAX_MIDI_BROWSER_ENTRIES = 32;
static constexpr int MIDI_BROWSER_DIR_MAX = 96;
static MidiBrowserEntry *const midiBrowserEntries = reinterpret_cast<MidiBrowserEntry *>(sharedBrowserPoolBuf());
static int midiBrowserCount = 0;
static int midiBrowserWinStart = 0;
static int midiBrowserWinCount = 0;
static int midiBrowserCur = 0;
static int midiBrowserScr = 0;
static int midiBrowserTrack = 0;
static uint32_t midiBrowserStart = 0;
static bool midiBrowserOpen = false;
static bool midiBrowserAudioMode = false;
static char *const midiBrowserDir = sharedBrowserDirBuf();

FLASHMEM static bool hasMidExtension(const char *name) {
  if (!name) return false;
  size_t len = strlen(name);
  if (len >= 5 && strcasecmp(name + len - 4, ".mid") == 0) return true;
  if (len >= 6 && strcasecmp(name + len - 5, ".midi") == 0) return true;
  return false;
}

FLASHMEM static bool hasWavClipExtension(const char *name) {
  if (!name) return false;
  size_t len = strlen(name);
  return (len >= 5 && strcasecmp(name + len - 4, ".wav") == 0);
}

static inline bool midiBrowserEntryComesBefore(const MidiBrowserEntry &a, const MidiBrowserEntry &b) {
  bool aParent = (a.isDir != 0) && (strcmp(a.name, "..") == 0);
  bool bParent = (b.isDir != 0) && (strcmp(b.name, "..") == 0);
  if (aParent != bParent) return aParent;
  if ((a.isDir != 0) != (b.isDir != 0)) return a.isDir != 0;
  int cmp = strcasecmp(a.name, b.name);
  if (cmp != 0) return cmp < 0;
  return strcmp(a.name, b.name) < 0;
}

FLASHMEM static void insertMidiSmallest(MidiBrowserEntry *arr, int &count, int cap,
                                        const MidiBrowserEntry &item) {
  if (cap <= 0) return;
  if (count >= cap && !midiBrowserEntryComesBefore(item, arr[cap - 1])) return;
  int ins = (count < cap) ? count++ : (cap - 1);
  while (ins > 0 && midiBrowserEntryComesBefore(item, arr[ins - 1])) {
    arr[ins] = arr[ins - 1];
    ins--;
  }
  arr[ins] = item;
}

FLASHMEM static void insertMidiLargest(MidiBrowserEntry *arr, int &count, int cap,
                                       const MidiBrowserEntry &item) {
  if (cap <= 0) return;
  if (count >= cap && !midiBrowserEntryComesBefore(arr[0], item)) return;
  if (count < cap) {
    int ins = count++;
    while (ins > 0 && midiBrowserEntryComesBefore(item, arr[ins - 1])) {
      arr[ins] = arr[ins - 1];
      ins--;
    }
    arr[ins] = item;
  } else {
    int ins = 0;
    while (ins + 1 < cap && midiBrowserEntryComesBefore(arr[ins + 1], item)) {
      arr[ins] = arr[ins + 1];
      ins++;
    }
    arr[ins] = item;
  }
}

FLASHMEM static void scanMidiBrowserDirectoryWindow(const MidiBrowserEntry *pivot, int pivotIdx, int wantStart) {
  MidiBrowserEntry pivotCopy{};
  const bool usePivot = (pivot != nullptr);
  int pOff = 0;
  int leftCap = 0;
  int rightCap = MAX_MIDI_BROWSER_ENTRIES;
  int leftCount = 0;
  int rightCount = 0;
  int totalCount = 0;

  if (usePivot) {
    pivotCopy = *pivot;
    pOff = constrain(pivotIdx - wantStart, 0, MAX_MIDI_BROWSER_ENTRIES - 1);
    leftCap = pOff;
    rightCap = (MAX_MIDI_BROWSER_ENTRIES - 1) - pOff;
  }

  auto considerItem = [&](const MidiBrowserEntry &item) {
    totalCount++;
    if (!usePivot) {
      insertMidiSmallest(midiBrowserEntries, rightCount, rightCap, item);
    } else if (midiBrowserEntryComesBefore(item, pivotCopy)) {
      insertMidiLargest(midiBrowserEntries, leftCount, leftCap, item);
    } else if (midiBrowserEntryComesBefore(pivotCopy, item)) {
      insertMidiSmallest(&midiBrowserEntries[pOff + 1], rightCount, rightCap, item);
    }
  };

  if (strcmp(midiBrowserDir, "/") != 0) {
    MidiBrowserEntry parentItem{};
    snprintf(parentItem.name, sizeof(parentItem.name), "..");
    parentItem.isDir = 1;
    considerItem(parentItem);
  }

  FsFile dir = SD.sdfs.open(midiBrowserDir, O_RDONLY);
  if (dir.isOpen() && dir.isDir()) {
    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
      char nameBuf[64] = {};
      entry.getName(nameBuf, sizeof(nameBuf));
      bool isSubDir = entry.isDir();
      bool isHidden = entry.isHidden() || (nameBuf[0] == '.') ||
                      (strcasecmp(nameBuf, "System Volume Information") == 0);
      entry.close();

      if (isHidden || nameBuf[0] == '\0') continue;
      if (!isSubDir && !(midiBrowserAudioMode ? hasWavClipExtension(nameBuf) : hasMidExtension(nameBuf))) continue;
      if (strlen(nameBuf) >= sizeof(midiBrowserEntries[0].name)) continue;

      MidiBrowserEntry item{};
      snprintf(item.name, sizeof(item.name), "%s", nameBuf);
      item.isDir = isSubDir ? 1 : 0;
      considerItem(item);
      keepStreaming();
    }
  }
  if (dir.isOpen()) dir.close();

  midiBrowserCount = totalCount;
  if (!usePivot) {
    midiBrowserWinStart = 0;
    midiBrowserWinCount = rightCount;
  } else {
    midiBrowserEntries[leftCount] = pivotCopy;
    if (leftCount < pOff && rightCount > 0) {
      memmove(&midiBrowserEntries[leftCount + 1], &midiBrowserEntries[pOff + 1],
              (size_t)rightCount * sizeof(MidiBrowserEntry));
    }
    midiBrowserWinStart = max(0, pivotIdx - leftCount);
    midiBrowserWinCount = leftCount + 1 + rightCount;
  }
}

FLASHMEM static void scanMidiBrowserDirectory() {
  midiBrowserCur = 0;
  midiBrowserScr = 0;
  scanMidiBrowserDirectoryWindow(nullptr, 0, 0);
}

FLASHMEM static void syncMidiBrowserScroll() {
  if (midiBrowserCount <= 0) {
    midiBrowserCur = 0;
    midiBrowserScr = 0;
    midiBrowserWinStart = 0;
    midiBrowserWinCount = 0;
    return;
  }
  midiBrowserCur = constrain(midiBrowserCur, 0, midiBrowserCount - 1);
  if (midiBrowserCur < midiBrowserScr) {
    midiBrowserScr = midiBrowserCur;
  } else if (midiBrowserCur >= midiBrowserScr + MIDI_BROWSER_VISIBLE_ROWS) {
    midiBrowserScr = midiBrowserCur - MIDI_BROWSER_VISIBLE_ROWS + 1;
  }
  int maxScr = max(0, midiBrowserCount - MIDI_BROWSER_VISIBLE_ROWS);
  midiBrowserScr = constrain(midiBrowserScr, 0, maxScr);

  int visEnd = min(midiBrowserCount - 1, midiBrowserScr + MIDI_BROWSER_VISIBLE_ROWS - 1);
  if (midiBrowserScr < midiBrowserWinStart || visEnd >= midiBrowserWinStart + midiBrowserWinCount) {
    int maxWinStart = max(0, midiBrowserCount - MAX_MIDI_BROWSER_ENTRIES);
    int wantStart = constrain(midiBrowserScr - 12, 0, maxWinStart);
    if (wantStart == 0) {
      scanMidiBrowserDirectoryWindow(nullptr, 0, 0);
    } else {
      int overlapLo = max(midiBrowserWinStart, wantStart);
      int overlapHi = min(midiBrowserWinStart + midiBrowserWinCount - 1, wantStart + MAX_MIDI_BROWSER_ENTRIES - 1);
      if (midiBrowserWinCount > 0 && overlapLo <= overlapHi) {
        int pivotIdx = (wantStart < midiBrowserWinStart) ? overlapHi : overlapLo;
        MidiBrowserEntry pivot = midiBrowserEntries[pivotIdx - midiBrowserWinStart];
        scanMidiBrowserDirectoryWindow(&pivot, pivotIdx, wantStart);
      } else {
        scanMidiBrowserDirectoryWindow(nullptr, 0, 0);
      }
    }
  }
}

FLASHMEM bool isMidiClipBrowserOpen() {
  return midiBrowserOpen;
}

FLASHMEM bool isAudioClipBrowserMode() {
  return midiBrowserAudioMode;
}

FLASHMEM int midiClipBrowserTargetTrack() {
  return midiBrowserTrack;
}

FLASHMEM const char *midiClipBrowserCurrentDir() {
  return midiBrowserDir;
}

FLASHMEM int midiClipBrowserEntryCount() {
  return midiBrowserCount;
}

FLASHMEM int midiClipBrowserCursor() {
  return midiBrowserCur;
}

FLASHMEM int midiClipBrowserScroll() {
  return midiBrowserScr;
}

FLASHMEM const MidiBrowserEntry *midiClipBrowserEntryAt(int idx) {
  if (idx < midiBrowserWinStart || idx >= midiBrowserWinStart + midiBrowserWinCount) return nullptr;
  return &midiBrowserEntries[idx - midiBrowserWinStart];
}

FLASHMEM void openMidiClipBrowser(int track, uint32_t startFrame) {
  midiBrowserAudioMode = false;
  midiBrowserTrack = constrain(track, 0, NUM_MIDI_TRACKS - 1);
  midiBrowserStart = min(startFrame, (uint32_t)MAX_REC_FRAMES - 1u);

  const char *song = currentSongName();
  char songDir[64] = {};
  if (song && song[0]) {
    snprintf(songDir, sizeof(songDir), "%s/%s", SONGS_DIR, song);
  }
  char songMidiDir[80];
  if (songDir[0] && SD.sdfs.exists(songDir) && ensureSongMidiDir(songMidiDir, sizeof(songMidiDir))) {
    snprintf(midiBrowserDir, MIDI_BROWSER_DIR_MAX, "%s", songMidiDir);
  } else {
    if (!SD.sdfs.exists(ROOT_DIR)) {
      SD.sdfs.mkdir(ROOT_DIR);
    }
    snprintf(midiBrowserDir, MIDI_BROWSER_DIR_MAX, "%s", ROOT_DIR);
  }

  scanMidiBrowserDirectory();
  if (midiBrowserCount > 1 && midiBrowserWinCount > 0 && strcmp(midiBrowserEntries[0].name, "..") == 0) {
    midiBrowserCur = 1;
  }
  syncMidiBrowserScroll();
  midiBrowserOpen = true;
}

FLASHMEM void openAudioClipBrowser(int track, uint32_t startFrame) {
  midiBrowserAudioMode = true;
  midiBrowserTrack = constrain(track, 0, NUM_AUDIO_TRACKS - 1);
  midiBrowserStart = min(startFrame, (uint32_t)MAX_REC_FRAMES - 1u);

  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  snprintf(midiBrowserDir, MIDI_BROWSER_DIR_MAX, "%s", ROOT_DIR);
  scanMidiBrowserDirectory();

  if (midiBrowserCount > 1 && midiBrowserWinCount > 0 && strcmp(midiBrowserEntries[0].name, "..") == 0) {
    midiBrowserCur = 1;
  }
  syncMidiBrowserScroll();
  midiBrowserOpen = true;
}

FLASHMEM void closeMidiClipBrowser() {
  if (midiBrowserOpen) {
    midiBrowserDir[0] = '\0';
  }
  midiBrowserOpen = false;
  midiBrowserAudioMode = false;
}

FLASHMEM void midiClipBrowserMove(int dy, int pageDelta) {
  if (!midiBrowserOpen || midiBrowserCount <= 0) return;
  int step = dy + pageDelta * MIDI_BROWSER_VISIBLE_ROWS;
  if (step == 0) return;
  midiBrowserCur = constrain(midiBrowserCur + step, 0, midiBrowserCount - 1);
  syncMidiBrowserScroll();
}

FLASHMEM bool midiClipBrowserConfirm(float curBpm) {
  if (!midiBrowserOpen || midiBrowserCount <= 0) return false;
  const int idx = constrain(midiBrowserCur, 0, midiBrowserCount - 1);
  const MidiBrowserEntry *ent = midiClipBrowserEntryAt(idx);
  if (!ent) return false;
  char entName[sizeof(midiBrowserEntries[0].name)];
  const bool isDir = (ent->isDir != 0);
  memcpy(entName, ent->name, sizeof(entName));
  entName[sizeof(entName) - 1] = '\0';

  if (isDir) {
    if (strcmp(entName, "..") == 0) {
      char *lastSlash = strrchr(midiBrowserDir, '/');
      if (!lastSlash || lastSlash == midiBrowserDir) {
        snprintf(midiBrowserDir, MIDI_BROWSER_DIR_MAX, "/");
      } else {
        *lastSlash = '\0';
      }
    } else {
      char nextDir[MIDI_BROWSER_DIR_MAX];
      if (strcmp(midiBrowserDir, "/") == 0) {
        snprintf(nextDir, sizeof(nextDir), "/%.62s", entName);
      } else {
        snprintf(nextDir, sizeof(nextDir), "%.31s/%.62s", midiBrowserDir, entName);
      }
      snprintf(midiBrowserDir, MIDI_BROWSER_DIR_MAX, "%s", nextDir);
    }
    scanMidiBrowserDirectory();
    if (midiBrowserCount > 1 && midiBrowserWinCount > 0 && strcmp(midiBrowserEntries[0].name, "..") == 0) {
      midiBrowserCur = 1;
    }
    syncMidiBrowserScroll();
    return false;
  }

  char fullPath[168];
  if (strcmp(midiBrowserDir, "/") == 0) {
    snprintf(fullPath, sizeof(fullPath), "/%.62s", entName);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%.95s/%.62s", midiBrowserDir, entName);
  }
  bool ok = midiBrowserAudioMode
                ? storageLoadWavFileToTrack(midiBrowserTrack, midiBrowserStart, fullPath)
                : midiSeqLoadMidFileToTrack(midiBrowserTrack, midiBrowserStart, fullPath, curBpm);
  if (!ok) {
    return false;
  }
  midiBrowserDir[0] = '\0';
  midiBrowserOpen = false;
  midiBrowserAudioMode = false;
  return true;
}

}  // namespace looper

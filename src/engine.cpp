// engine.cpp — 8-Track Audio Tape Engine (4 Stereo + 4 Mono) + Transport & Metronome Clock
#include "engine.h"
#include "instruments.h"
#include "app.h"
#include "midi_in.h"
#include <math.h>

namespace looper {

// 4 stereo tracks (4 x RING_FRAMES words) + 4 mono tracks (4 x RING_FRAMES/2 words) = 252 KB in RAM2 (DMAMEM)
constexpr uint32_t TOTAL_RING_WORDS =
    (NUM_STEREO_AUDIO_TRACKS * RING_FRAMES) + (NUM_MONO_AUDIO_TRACKS * (RING_FRAMES / 2));
DMAMEM static uint32_t rings[TOTAL_RING_WORDS];
uint32_t inputRing[INPUT_FRAMES];

Track tracks[NUM_AUDIO_TRACKS];
Transport transport;
Recording recording;

volatile MonitorMode lineMonitorMode = MonitorMode::On;
volatile MonitorMode usbMonitorMode = MonitorMode::On;
volatile bool recLineInEnabled = true;
volatile bool recUsbInEnabled = true;
volatile bool recInstrEnabled = true;
volatile AudioInputSource audioInputSource = AudioInputSource::All;
volatile bool metronomeOn = false;
volatile bool masterExportActive = false;
volatile bool samplerRecActive = false;
volatile float inputMeterL = 0.0f, inputMeterR = 0.0f;
volatile float outputMeterL = 0.0f, outputMeterR = 0.0f;

bool (*usbAudioRead)(uint32_t *frames) = nullptr;
int16_t *(*usbAudioWrite)() = nullptr;
void (*usbAudioWritten)() = nullptr;

static struct RingInit {
  RingInit() {
    uint32_t *ptr = rings;
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      tracks[t].ring = ptr;
      ptr += isAudioTrackMono(t) ? (RING_FRAMES / 2) : RING_FRAMES;
    }
  }
} ringInit;

uint32_t spbForBpm(float bpm) {
  float b = constrain(bpm, MIN_BPM, MAX_BPM);
  return (uint32_t)(AUDIO_SAMPLE_RATE_EXACT * 60.0f / b + 0.5f);
}

FLASHMEM double framesToBeats(uint32_t frames, float bpm) {
  double sec = (double)frames / (double)SAMPLE_RATE_HZ;
  return sec * (bpm / 60.0);
}

FLASHMEM uint32_t beatsToFrames(double beats, float bpm) {
  if (beats <= 0.0) return 0;
  double sec = beats * (60.0 / max(1.0f, bpm));
  uint64_t f = (uint64_t)(sec * SAMPLE_RATE_HZ + 0.5);
  return (uint32_t)min(f, (uint64_t)MAX_REC_FRAMES);
}

uint32_t framesToMidiTicks(uint32_t frames, float bpm) {
  uint32_t bpmTenths = (uint32_t)constrain((int)(bpm * 10.0f + 0.5f), (int)(MIN_BPM * 10.0f), (int)(MAX_BPM * 10.0f));
  // ticks = round(frames * (bpmTenths / 10) * 480 / (44100 * 60)) = round(frames * bpmTenths * 4 / 220500)
  uint64_t num = (uint64_t)frames * (uint64_t)bpmTenths * 4ULL + 110250ULL;
  return (uint32_t)(num / 220500ULL);
}

uint32_t midiTicksToFrames(uint32_t ticks, float bpm) {
  uint32_t bpmTenths = (uint32_t)constrain((int)(bpm * 10.0f + 0.5f), (int)(MIN_BPM * 10.0f), (int)(MAX_BPM * 10.0f));
  uint64_t den = (uint64_t)bpmTenths * 4ULL;
  uint64_t f = ((uint64_t)ticks * 220500ULL + (den >> 1)) / den;
  return (uint32_t)min(f, (uint64_t)MAX_REC_FRAMES);
}

// ---- Track Volume and Panning (Balance)
static float trackVolumeGain(int volStep) {
  if (volStep <= 0) return 0.0f;
  float db = (volStep - VOLUME_UNITY) * VOLUME_STEP_DB;
  return powf(10.0f, db / 20.0f);
}

static inline void computeStereoBalanceMix(float gain, int balance, int maxBal,
                                           float &ll, float &lr, float &rl, float &rr) {
  float toL = balance < 0 ? -(float)balance / (float)maxBal : 0.0f;
  float toR = balance > 0 ? (float)balance / (float)maxBal : 0.0f;
  ll = gain * (1.0f - toR);
  lr = gain * toL;
  rl = gain * toR;
  rr = gain * (1.0f - toL);
}

FLASHMEM void setTrackMix(int t, int volume, int balance) {
  if (t < 0 || t >= NUM_AUDIO_TRACKS) return;
  Track &tr = tracks[t];
  volume = constrain(volume, 0, VOLUME_MAX);
  balance = constrain(balance, -PAN_MAX, PAN_MAX);
  float g = trackVolumeGain(volume);
  float ll, lr, rl, rr;
  if (isAudioTrackMono(t)) {
    float toL = balance < 0 ? -(float)balance / (float)PAN_MAX : 0.0f;
    float toR = balance > 0 ? (float)balance / (float)PAN_MAX : 0.0f;
    ll = g * (1.0f - toR);
    lr = 0.0f;
    rl = 0.0f;
    rr = g * (1.0f - toL);
  } else {
    computeStereoBalanceMix(g, balance, PAN_MAX, ll, lr, rl, rr);
  }
  noInterrupts();
  tr.volume = (int8_t)volume;
  tr.balance = (int8_t)balance;
  tr.mixLL = ll;
  tr.mixLR = lr;
  tr.mixRL = rl;
  tr.mixRR = rr;
  interrupts();
}

FLASHMEM void takeTrackPeaks(int t, float &left, float &right) {
  if (t < 0 || t >= NUM_AUDIO_TRACKS) { left = right = 0.0f; return; }
  Track &tr = tracks[t];
  noInterrupts();
  left = tr.peakL;
  right = tr.peakR;
  tr.peakL = 0.0f;
  tr.peakR = 0.0f;
  interrupts();
}

FLASHMEM void takeInputPeaks(float &left, float &right) {
  noInterrupts();
  left = inputMeterL;
  right = inputMeterR;
  inputMeterL = 0.0f;
  inputMeterR = 0.0f;
  interrupts();
}

FLASHMEM void takeOutputPeaks(float &left, float &right) {
  noInterrupts();
  left = outputMeterL;
  right = outputMeterR;
  outputMeterL = 0.0f;
  outputMeterR = 0.0f;
  interrupts();
}

// ---- Master Volume & Metronome Volume
static int masterStep = MASTER_VOLUME_DEFAULT;
static volatile float outputGain = 0.0f;
static volatile float metroGain = 1.0f;

FLASHMEM void setMasterVolume(int step) {
  masterStep = constrain(step, 0, MASTER_VOLUME_MAX);
  float g = masterStep >= MASTER_VOLUME_MAX ? MASTER_TOP_GAIN
          : masterStep ? powf(10.0f, masterStepDb(masterStep) / 20.0f) : 0.0f;
  noInterrupts();
  outputGain = g;
  interrupts();
}

FLASHMEM void setMetronomeVolume(int step) {
  step = constrain(step, 0, VOLUME_MAX);
  float g = trackVolumeGain(step);
  noInterrupts();
  metroGain = g;
  interrupts();
}

// ---- Metronome Woodblock Synthesis
static constexpr float CLICK_BAR_HZ = 1600.0f, CLICK_BEAT_HZ = 1150.0f;
static constexpr float REF_FALL_DB = 6.0f;
static const float REF_FALL = powf(10.0f, -REF_FALL_DB / 20.0f * AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT);
static constexpr float CLICK_OVER_DB = 2.0f, CLICK_FLOOR_DB = -14.0f;
static const float CLICK_OVER = powf(10.0f, CLICK_OVER_DB / 20.0f);
static const float CLICK_FLOOR = powf(10.0f, CLICK_FLOOR_DB / 20.0f);
static float refPeak = 0.0f;

static float clickLevel() {
  float m = max(CLICK_FLOOR * 32767.0f, refPeak * CLICK_OVER);
  return min(m, 32767.0f) / 1.4f;
}

static const float CLICK_DECAY = expf(-1.0f / (0.012f * AUDIO_SAMPLE_RATE_EXACT));
static const float CLICK_DECAY_HI = expf(-1.0f / (0.004f * AUDIO_SAMPLE_RATE_EXACT));
static float clickAmp = 0.0f, clickAmpHi = 0.0f;
static uint32_t clickPhase32 = 0, clickPhaseHi32 = 0, clickStep32 = 0, clickStepHi32 = 0;

static void startClick(bool bar) {
  float hz = bar ? CLICK_BAR_HZ : CLICK_BEAT_HZ;
  constexpr float phaseScale = 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT;
  clickStep32 = (uint32_t)(hz * phaseScale + 0.5f);
  clickStepHi32 = (uint32_t)(hz * 2.76f * phaseScale + 0.5f);
  clickPhase32 = 0;
  clickPhaseHi32 = 0;
  clickAmp = clickLevel() * (bar ? 1.0f : 0.75f);
  clickAmpHi = clickAmp * 0.5f;
}

static float clickSample() {
  if (clickAmp < 1.0f) return 0.0f;
  float s = clickAmp * instSinLutAt(clickPhase32 >> (32 - INST_LUT_BITS))
          + clickAmpHi * instSinLutAt(clickPhaseHi32 >> (32 - INST_LUT_BITS));
  clickPhase32 += clickStep32;
  clickPhaseHi32 += clickStepHi32;
  clickAmp *= CLICK_DECAY;
  clickAmpHi *= CLICK_DECAY_HI;
  return s;
}

// ---- CPU Load Measurement
static volatile uint32_t loadCycles = 0, loadMost = 0, loadBlocks = 0;
FLASHMEM void takeEngineLoad(float &avg, float &most) {
#if defined(ARM_DWT_CYCCNT)
  noInterrupts();
  uint32_t c = loadCycles, m = loadMost, n = loadBlocks;
  loadCycles = loadMost = loadBlocks = 0;
  interrupts();
  const float blockCycles = (float)F_CPU_ACTUAL * AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT;
  avg = n ? c / (float)n / blockCycles : 0.0f;
  most = m / blockCycles;
#else
  avg = most = 0.0f;
#endif
}

// ---- MIDI Out Queues (Dedicated TRS Byte Queue + Lock-Free USB Packet Queue)
static HardwareSerial *midiPort = nullptr;
#if !defined(MIDI_INTERFACE)
static IntervalTimer midiTimer;
#endif

static constexpr uint32_t TICK_QUEUE = 64;
static volatile uint32_t trsTickDue[TICK_QUEUE];
static volatile uint32_t trsTickHead = 0, trsTickTail = 0;
static volatile uint32_t usbTickDue[TICK_QUEUE];
static volatile uint32_t usbTickHead = 0, usbTickTail = 0;

static constexpr uint32_t TRS_MSG_QUEUE = 512;
static volatile uint8_t trsMsgQueue[TRS_MSG_QUEUE];
static volatile uint32_t trsMsgHead = 0, trsMsgTail = 0;

static constexpr uint32_t USB_PKT_QUEUE = 256;
static volatile MidiPacket usbPktQueue[USB_PKT_QUEUE];
static volatile uint32_t usbPktHead = 0, usbPktTail = 0;
static volatile bool usbOut = false;

void (*midiInPollHook)() = nullptr;

uint8_t midiActiveClockOutPorts() {
  uint8_t mask = 0;
  switch (songSettings().midiClockOut) {
    case MidiClockOut::Off:  mask = MIDI_PORT_NONE; break;
    case MidiClockOut::Usb:  mask = MIDI_PORT_USB;  break;
    case MidiClockOut::Trs:  mask = MIDI_PORT_TRS;  break;
    case MidiClockOut::Both: mask = MIDI_PORT_BOTH; break;
  }
  ClockSource cs = songSettings().clockSource;
  if (cs == ClockSource::ExternalUsb) {
    mask &= ~MIDI_PORT_USB;
  } else if (cs == ClockSource::ExternalTrs) {
    mask &= ~MIDI_PORT_TRS;
  } else if (cs == ClockSource::ExternalAuto) {
    if (midiClockFollowing()) {
      if (midiFollowSource() == MIDI_USB) mask &= ~MIDI_PORT_USB;
      else                                mask &= ~MIDI_PORT_TRS;
    } else {
      mask = MIDI_PORT_NONE;
    }
  }
  return mask;
}

void midiServiceTrsInOut() {
  if (midiInPollHook) midiInPollHook();
  if (!midiPort) return;
  while (trsMsgTail != trsMsgHead) {
    if (midiPort->availableForWrite() <= 0) return;
    midiPort->write(trsMsgQueue[trsMsgTail++ % TRS_MSG_QUEUE]);
  }
  uint32_t now = micros();
  while (trsTickTail != trsTickHead && (int32_t)(now - trsTickDue[trsTickTail % TICK_QUEUE]) >= 0) {
    if (midiPort->availableForWrite() <= 0) return;
    midiPort->write(0xF8);
    trsTickTail++;
  }
}

FLASHMEM void midiBegin(HardwareSerial *port) {
  if (!MIDI_OUT_ENABLED) return;
  midiPort = port;
#if !defined(MIDI_INTERFACE)
  midiTimer.begin(midiServiceTrsInOut, 250);
#endif
}

FLASHMEM void midiEnd() {
#if !defined(MIDI_INTERFACE)
  midiTimer.end();
#endif
  midiPort = nullptr;
  midiInPollHook = nullptr;
}

void midiSendNow(uint8_t status, uint8_t portMask) {
  if (status >= 0xF8) {
    portMask &= midiActiveClockOutPorts();
  }
  if (!portMask) return;
  uint32_t irq = saveIrq();
  if ((portMask & MIDI_PORT_TRS) && midiPort && (trsMsgHead - trsMsgTail < TRS_MSG_QUEUE)) {
    trsMsgQueue[trsMsgHead++ % TRS_MSG_QUEUE] = status;
  }
  if ((portMask & MIDI_PORT_USB) && usbOut && (usbPktHead - usbPktTail < USB_PKT_QUEUE)) {
    uint32_t idx = usbPktHead % USB_PKT_QUEUE;
    usbPktQueue[idx].status = status;
    usbPktQueue[idx].d1 = 0;
    usbPktQueue[idx].d2 = 0;
    usbPktQueue[idx].len = 1;
    usbPktHead++;
  }
  restoreIrq(irq);
}

void midiSendClockNow(uint8_t status) {
  midiSendNow(status, midiActiveClockOutPorts());
}

void midiSendMsg(uint8_t status, uint8_t d1, uint8_t d2, int len, uint8_t portMask) {
  if (!portMask || len <= 0) return;
  uint32_t irq = saveIrq();
  if ((portMask & MIDI_PORT_TRS) && midiPort) {
    uint32_t availTrs = TRS_MSG_QUEUE - (trsMsgHead - trsMsgTail);
    if (availTrs >= (uint32_t)len) {
      trsMsgQueue[trsMsgHead++ % TRS_MSG_QUEUE] = status;
      if (len >= 2) trsMsgQueue[trsMsgHead++ % TRS_MSG_QUEUE] = d1 & 0x7F;
      if (len >= 3) trsMsgQueue[trsMsgHead++ % TRS_MSG_QUEUE] = d2 & 0x7F;
    }
  }
  if ((portMask & MIDI_PORT_USB) && usbOut) {
    if (usbPktHead - usbPktTail < USB_PKT_QUEUE) {
      uint32_t idx = usbPktHead % USB_PKT_QUEUE;
      usbPktQueue[idx].status = status;
      usbPktQueue[idx].d1 = d1 & 0x7F;
      usbPktQueue[idx].d2 = d2 & 0x7F;
      usbPktQueue[idx].len = (uint8_t)len;
      usbPktHead++;
    }
  }
  restoreIrq(irq);
}

static void queueTick(uint32_t due) {
  uint8_t clkMask = midiActiveClockOutPorts();
  if (!clkMask) return;
  uint32_t irq = saveIrq();
  if ((clkMask & MIDI_PORT_TRS) && midiPort && (trsTickHead - trsTickTail < TICK_QUEUE)) {
    trsTickDue[trsTickHead++ % TICK_QUEUE] = due;
  }
  if ((clkMask & MIDI_PORT_USB) && usbOut && (usbTickHead - usbTickTail < TICK_QUEUE)) {
    usbTickDue[usbTickHead++ % TICK_QUEUE] = due;
  }
  restoreIrq(irq);
}

FLASHMEM void midiOutAttachUsb() {
  uint32_t irq = saveIrq();
  usbTickTail = usbTickHead;
  usbPktTail = usbPktHead;
  usbOut = true;
  restoreIrq(irq);
}

int midiOutTakeUsbPackets(MidiPacket *out, int max) {
  int n = 0;
  uint32_t irq = saveIrq();
  while (n < max && usbPktTail != usbPktHead) {
    uint32_t idx = usbPktTail % USB_PKT_QUEUE;
    out[n].status = usbPktQueue[idx].status;
    out[n].d1     = usbPktQueue[idx].d1;
    out[n].d2     = usbPktQueue[idx].d2;
    out[n].len    = usbPktQueue[idx].len;
    usbPktTail++;
    n++;
  }
  restoreIrq(irq);
  return n;
}

int midiOutTakeUsbTicks(uint32_t now, int max) {
  int n = 0;
  uint32_t irq = saveIrq();
  while (n < max && usbTickTail != usbTickHead && (int32_t)(now - usbTickDue[usbTickTail % TICK_QUEUE]) >= 0) {
    usbTickTail++;
    n++;
  }
  restoreIrq(irq);
  return n;
}

// ---- Master Limiter & Smoothing Constants
static const float GLIDE = 1.0f - expf(-1000.0f / (GAIN_GLIDE_MS * AUDIO_SAMPLE_RATE_EXACT));
static const float MASTER_GLIDE = 1.0f - expf(-1000.0f / (MASTER_GLIDE_MS * AUDIO_SAMPLE_RATE_EXACT));
static const float LIMIT_RELEASE = 1.0f - expf(-1000.0f / (LIMIT_RELEASE_MS * AUDIO_SAMPLE_RATE_EXACT));

struct Limiter {
  float gain = 1.0f;
  void run(float &l, float &r, float ceiling) {
    float a = max(fabsf(l), fabsf(r));
    if (a * gain > ceiling) {
      gain = ceiling / a;
    }
    if (gain == 1.0f) return;
    l *= gain;
    r *= gain;
    gain += (1.0f - gain) * LIMIT_RELEASE;
    if (gain > 0.9995f) gain = 1.0f;
  }
};

static Limiter mixLimiter;

void LooperEngine::onBeat() {
  int32_t beat = transport.beat;
  tickInBeat_ = 0;
  nextTickAt_ = 0;
  if (metronomeOn) {
    startClick((beat % BEATS_PER_BAR) == 0);
  }
}

void LooperEngine::update() {
#if defined(ARM_DWT_CYCCNT)
  const uint32_t cyclesAt = ARM_DWT_CYCCNT;
#endif
  audio_block_t *inL = receiveReadOnly(0), *inR = receiveReadOnly(1);
  audio_block_t *outL = allocate(), *outR = allocate();
  if (!outL || !outR) {
    if (outL) release(outL);
    if (outR) release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
    return;
  }

  static uint32_t usbFrames[AUDIO_BLOCK_SAMPLES];
  const bool usbIn = usbAudioRead && usbAudioRead(usbFrames);
  int16_t *usbOutBuf = usbAudioWrite ? usbAudioWrite() : nullptr;

  int targetTrk = recording.track;
  AudioRecSrc targetSrc = (targetTrk >= 0 && targetTrk < NUM_AUDIO_TRACKS)
                              ? recording.src
                              : AudioRecSrc::Off;
  MonoInput targetMonoIn = recording.monoInput;
  if (targetTrk < 0 || targetTrk >= NUM_AUDIO_TRACKS) {
    targetTrk = -1;
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].armMode != ArmMode::Off && tracks[t].recSrc != AudioRecSrc::Off) {
        targetTrk = t;
        targetSrc = tracks[t].recSrc;
        targetMonoIn = tracks[t].monoInput;
        break;
      }
    }
  }
  const int capMidiTrk = (targetSrc >= AudioRecSrc::Midi1 && targetSrc <= AudioRecSrc::Midi8)
                             ? ((int)targetSrc - (int)AudioRecSrc::Midi1)
                             : -1;
  int32_t fmL[AUDIO_BLOCK_SAMPLES];
  int32_t fmR[AUDIO_BLOCK_SAMPLES];
  int32_t midiCapL[AUDIO_BLOCK_SAMPLES];
  int32_t midiCapR[AUDIO_BLOCK_SAMPLES];
  const bool hasFm = fmSynthRenderBlock(fmL, fmR, AUDIO_BLOCK_SAMPLES,
                                        capMidiTrk,
                                        (capMidiTrk >= 0) ? midiCapL : nullptr,
                                        (capMidiTrk >= 0) ? midiCapR : nullptr);

  const AudioInputSource src = audioInputSource;
  const bool smpRec   = samplerRecActive;
  const bool micMode  = (src == AudioInputSource::MicAndSynth || src == AudioInputSource::MicOnly);
  const bool useLine  = micMode ? true : (recLineInEnabled || (smpRec && !recUsbInEnabled));
  const bool useUsb   = micMode ? false : recUsbInEnabled;
  const bool useSynth = (src == AudioInputSource::MicOnly || smpRec) ? false : recInstrEnabled;
  bool audioArmed = (recording.track >= 0) || smpRec;
  if (!audioArmed) {
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].armMode != ArmMode::Off) {
        audioArmed = true;
        break;
      }
    }
  }
  const MonitorMode lMon = lineMonitorMode;
  const MonitorMode uMon = usbMonitorMode;
  const bool monLine  = (lMon == MonitorMode::On) || (lMon == MonitorMode::Arm && audioArmed);
  const bool monUsb   = ((uMon == MonitorMode::On) || (uMon == MonitorMode::Arm && audioArmed)) && usbIn;

  int16_t recSampleL[AUDIO_BLOCK_SAMPLES];
  int16_t recSampleR[AUDIO_BLOCK_SAMPLES];
  int16_t inPeakL = 0, inPeakR = 0, inPeakSum = 0;
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int16_t la = inL ? inL->data[i] : 0;
    int16_t lb = inR ? inR->data[i] : 0;

    int32_t recL = (useLine ? (int32_t)la : 0)
                 + ((useUsb && usbIn) ? (int32_t)(int16_t)(usbFrames[i] & 0xFFFF) : 0)
                 + ((useSynth && hasFm) ? fmL[i] : 0);
    int32_t recR = (useLine ? (int32_t)lb : 0)
                 + ((useUsb && usbIn) ? (int32_t)(int16_t)(usbFrames[i] >> 16) : 0)
                 + ((useSynth && hasFm) ? fmR[i] : 0);
    int16_t clL = (int16_t)constrain(recL, -32768, 32767);
    int16_t clR = (int16_t)constrain(recR, -32768, 32767);
    recSampleL[i] = clL;
    recSampleR[i] = clR;
    int16_t a = (int16_t)min((int32_t)32767, abs(recL));
    int16_t b = (int16_t)min((int32_t)32767, abs(recR));
    int16_t cSum = (int16_t)(((int32_t)clL + (int32_t)clR) >> 1);
    int16_t sAbs = (int16_t)min((int32_t)32767, abs((int32_t)cSum));
    if (a > inPeakL) inPeakL = a;
    if (b > inPeakR) inPeakR = b;
    if (sAbs > inPeakSum) inPeakSum = sAbs;
  }
  if (inPeakL / 32768.0f > inputMeterL) inputMeterL = inPeakL / 32768.0f;
  if (inPeakR / 32768.0f > inputMeterR) inputMeterR = inPeakR / 32768.0f;

  uint32_t blockMicros = micros();
  const float usPerSample = 1e6f / AUDIO_SAMPLE_RATE_EXACT;

  // Apply pending tempo changes immediately
  if (transport.pendingSpb) {
    uint32_t spb = transport.pendingSpb;
    transport.sampleInBeat = (uint64_t)transport.sampleInBeat * spb / max(1u, transport.spb);
    transport.spb = spb;
    transport.pendingSpb = 0;
    nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
  }

  uint32_t spb = transport.spb;
  TransportMode tMode = transport.mode;

  // Check per-track recording source threshold trigger when waiting
  if (tMode == TransportMode::WaitThreshold && transport.waitAudioThresh && targetTrk >= 0) {
    float trigPeak = 0.0f;
    const bool tgtMono = isAudioTrackMono(targetTrk);
    int16_t pkL = 0, pkR = 0, pkSum = 0;
    if (targetSrc == AudioRecSrc::Line || targetSrc == AudioRecSrc::Usb || capMidiTrk >= 0) {
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        int32_t sL = 0, sR = 0;
        if (targetSrc == AudioRecSrc::Line) {
          sL = inL ? inL->data[i] : 0;
          sR = inR ? inR->data[i] : 0;
        } else if (targetSrc == AudioRecSrc::Usb) {
          sL = usbIn ? (int16_t)(usbFrames[i] & 0xFFFF) : 0;
          sR = usbIn ? (int16_t)(usbFrames[i] >> 16) : 0;
        } else {
          sL = constrain(midiCapL[i], -32768, 32767);
          sR = constrain(midiCapR[i], -32768, 32767);
        }
        int16_t a = (int16_t)min((int32_t)32767, abs(sL));
        int16_t b = (int16_t)min((int32_t)32767, abs(sR));
        int16_t cSum = (int16_t)((sL + sR) >> 1);
        int16_t sAbs = (int16_t)min((int32_t)32767, abs((int32_t)cSum));
        if (a > pkL) pkL = a;
        if (b > pkR) pkR = b;
        if (sAbs > pkSum) pkSum = sAbs;
      }
      if (tgtMono) {
        int16_t mPk = (targetMonoIn == MonoInput::Left)  ? pkL
                    : (targetMonoIn == MonoInput::Right) ? pkR
                                                         : pkSum;
        trigPeak = mPk / 32768.0f;
      } else {
        trigPeak = max(pkL, pkR) / 32768.0f;
      }
    }
    if (trigPeak >= transport.audioThreshLinear) {
      tMode = TransportMode::Running;
      transport.mode = TransportMode::Running;
      transport.running = true;
      transport.sampleInBeat = 0;
      midiSendNow(0xFA); // MIDI Start
    }
  }

  const bool anyAudioSolo = anyTrackSolo();

  float mix[NUM_AUDIO_TRACKS][4];
  float peakL[NUM_AUDIO_TRACKS] = {}, peakR[NUM_AUDIO_TRACKS] = {};
  uint8_t activeTracks[NUM_AUDIO_TRACKS];
  int numActiveTracks = 0;
  const int recTrkBlock = recording.track;
  const int srcAudioTrkBlock = (targetSrc >= AudioRecSrc::Audio1 && targetSrc <= AudioRecSrc::Audio8)
                                   ? ((int)targetSrc - (int)AudioRecSrc::Audio1)
                                   : -1;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    const Track &tr = tracks[t];
    bool audible = (tr.state == TrackState::Playing) && (!anyAudioSolo || tr.solo);
    float g = audible ? 1.0f : 0.0f;
    mix[t][0] = g * tr.mixLL;
    mix[t][1] = g * tr.mixLR;
    mix[t][2] = g * tr.mixRL;
    mix[t][3] = g * tr.mixRR;
    if (tr.live || t == recTrkBlock || t == srcAudioTrkBlock ||
        (mix[t][0] + mix[t][1] + mix[t][2] + mix[t][3] +
         tr.curMix[0] + tr.curMix[1] + tr.curMix[2] + tr.curMix[3]) > 1e-6f) {
      activeTracks[numActiveTracks++] = (uint8_t)t;
    }
  }

  const float outGain = outputGain;
  const float clickGain = outGain * metroGain;
  const float limitCeiling = masterExportActive ? LIMIT_CEILING : (LIMIT_CEILING / max(1.0f, outGain));
  const bool recTrackIsMono = (targetTrk >= 0 && isAudioTrackMono(targetTrk));
  const MonoInput recMonoIn = targetMonoIn;
  if (usbOutBuf) {
    memset(usbOutBuf, 0, AUDIO_BLOCK_SAMPLES * USB_OUT_CHANNELS * sizeof(*usbOutBuf));
  }
  static float outGainNow = 0.0f;
  float blockPeak = 0.0f;
  float outPeakL = 0.0f, outPeakR = 0.0f;

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int32_t lineL = inL ? inL->data[i] : 0;
    int32_t lineR = inR ? inR->data[i] : 0;
    int32_t uL = usbIn ? (int16_t)(usbFrames[i] & 0xFFFF) : 0;
    int32_t uR = usbIn ? (int16_t)(usbFrames[i] >> 16) : 0;
    int32_t synL = hasFm ? fmL[i] : 0;
    int32_t synR = hasFm ? fmR[i] : 0;

    const uint32_t at = transport.now;
    const bool autoMuteRec = recording.autoMute;
    const bool expOut = masterExportActive;
    bool recActiveHere = false;
    float recTrackSl = 0.0f, recTrackSr = 0.0f;
    float srcAudioL = 0.0f, srcAudioR = 0.0f;

    // Monitor bus for Master Out (excludes USB Audio In from USB Master Out to prevent DAW feedback loops)
    const float monL = (float)constrain(((monLine && !expOut) ? lineL : 0) + synL, -32768, 32767);
    const float monR = (float)constrain(((monLine && !expOut) ? lineR : 0) + synR, -32768, 32767);
    float mixL = 0.0f, mixR = 0.0f;
    int16_t *usbFrame = usbOutBuf ? usbOutBuf + i * USB_OUT_CHANNELS : nullptr;

    // Handle Count-In mode: click metronome for countInRemainingBeats before starting tape
    if (tMode == TransportMode::CountIn) {
      if (transport.sampleInBeat == 0) {
        if (transport.countInRemainingBeats <= 0) {
          tMode = TransportMode::Running;
          transport.mode = TransportMode::Running;
          transport.running = true;
          midiSendNow(0xFA);
        } else {
          startClick(((transport.countInTotalBeats - transport.countInRemainingBeats) % BEATS_PER_BAR) == 0);
          transport.countInRemainingBeats = transport.countInRemainingBeats - 1;
        }
      }
      if (tMode == TransportMode::CountIn) {
        if (++transport.sampleInBeat >= spb) {
          transport.sampleInBeat = 0;
        }
      }
    }

    uint32_t runningTf = 0;
    uint32_t runningStepFrames = 0;
    bool runningStep = false;
    bool runningLoopOn = false;
    bool runningRev = false;

    if (tMode == TransportMode::Running) {
      const int8_t sq = transport.speedQuarter;
      const bool rev = (sq < 0);
      const uint32_t absQ = (uint32_t)(rev ? -sq : sq);
      uint32_t tf = transport.tapeFrame;
      const bool loopOn = transport.loopEnabled && (transport.punchOutFrame > transport.punchInFrame);
      if (loopOn) {
        if (!rev && tf >= transport.punchOutFrame) {
          tf = transport.punchInFrame;
          transport.tapeFrame = tf;
        } else if (rev && (tf < transport.punchInFrame || tf >= transport.punchOutFrame)) {
          tf = (transport.punchOutFrame > 0) ? (transport.punchOutFrame - 1u) : 0u;
          transport.tapeFrame = tf;
        }
      }
      if (!rev && tf >= MAX_REC_FRAMES) {
        // End of 666-second tape reached
        tMode = TransportMode::Stopped;
        transport.mode = TransportMode::Stopped;
        transport.running = false;
        if (recording.activeInWindow) {
          recording.activeInWindow = false;
          recording.mode = Recording::Finishing;
          recording.endNow = at;
          recording.windowExited = true;
        }
        midiSendNow(0xFC);
      } else {
        runningTf = tf;
        runningStep = (absQ > 0);
        runningLoopOn = loopOn;
        runningRev = rev;

        uint32_t stepFrames = 0;
        if (absQ > 0) {
          uint32_t sub = (uint32_t)transport.subFrame + absQ;
          stepFrames = sub >> 2;
          transport.subFrame = (uint8_t)(sub & 3u);
          if (!rev) {
            uint32_t maxStep = (loopOn && tf < transport.punchOutFrame)
                                   ? (transport.punchOutFrame - tf)
                                   : (MAX_REC_FRAMES - tf);
            if (stepFrames > maxStep) stepFrames = maxStep;
          } else {
            uint32_t maxStep = (loopOn && tf >= transport.punchInFrame)
                                   ? ((tf - transport.punchInFrame) + 1u)
                                   : (tf + 1u);
            if (stepFrames > maxStep) stepFrames = maxStep;
          }
        } else {
          transport.subFrame = 0;
        }
        runningStepFrames = stepFrames;

        if (stepFrames > 0) {
          if (transport.sampleInBeat == 0) {
            onBeat();
          }
          while (tickInBeat_ < 24 && transport.sampleInBeat >= nextTickAt_) {
            queueTick(blockMicros + (uint32_t)((i + AUDIO_BLOCK_SAMPLES) * usPerSample));
            tickInBeat_++;
            nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
          }
        }

        // Determine whether audio recording is active at this exact tapeFrame (allowed at +1.00x or -1.00x)
        int recTrk = recording.track;
        bool wantRecHere = false;
        if (recTrk >= 0 && recTrk < NUM_AUDIO_TRACKS &&
            (sq == DEFAULT_SPEED_QUARTER || sq == REVERSE_1X_SPEED_QUARTER) &&
            (recording.mode == Recording::Armed || recording.mode == Recording::Active)) {
          if (transport.punchEnabled) {
            wantRecHere = (tf >= transport.punchInFrame && tf < transport.punchOutFrame);
          } else if (loopOn) {
            wantRecHere = !rev ? (tf >= recording.startTapeFrame && tf < transport.punchOutFrame)
                               : (tf <= recording.startTapeFrame && tf >= transport.punchInFrame);
          } else {
            wantRecHere = !rev ? (tf >= recording.startTapeFrame)
                               : (tf <= recording.startTapeFrame);
          }
        }

        if (wantRecHere && !recording.activeInWindow) {
          recording.activeInWindow = true;
          recording.mode = Recording::Active;
          recording.reverse = rev;
          recording.startNow = at;
          recording.startTapeFrame = tf;
          recording.windowEntered = true;
        } else if (!wantRecHere && recording.activeInWindow) {
          recording.activeInWindow = false;
          recording.mode = Recording::Finishing;
          recording.endNow = at;
          recording.windowExited = true;
        }
        recActiveHere = (recTrk >= 0 && recording.activeInWindow);

        // Process active audio tracks (0..3 Stereo, 4..7 Mono)
        for (int aIdx = 0; aIdx < numActiveTracks; aIdx++) {
          const int t = activeTracks[aIdx];
          Track &tr = tracks[t];
          bool isRecTrackHere = (t == recTrk && recording.activeInWindow);
          float sl = 0.0f, sr = 0.0f;

          if (tr.live && absQ > 0 && tf < tr.recordedFrames) {
            uint32_t tail = tr.tail;
            int32_t avail = (int32_t)(tr.head - tail);
            if (avail >= 1) {
              if (isAudioTrackMono(t)) {
                int16_t sm = reinterpret_cast<const int16_t *>(tr.ring)[tail % RING_FRAMES];
                sl = (float)sm;
                sr = (float)sm;
              } else {
                uint32_t packed = tr.ring[tail % RING_FRAMES];
                sl = (int16_t)(packed & 0xFFFF);
                sr = (int16_t)(packed >> 16);
              }
              if (stepFrames > 0) {
                uint32_t ringStep = (absQ > 8u) ? min(stepFrames, 2u) : stepFrames;
                uint32_t adv = min((uint32_t)avail, ringStep);
                tr.tail = tail + adv;
              }
            }
          } else if (tr.live && stepFrames > 0) {
            // Keep ring tail advancing if pre-filled with silence
            uint32_t tail = tr.tail;
            int32_t avail = (int32_t)(tr.head - tail);
            if (avail >= 1) {
              uint32_t ringStep = (absQ > 8u) ? min(stepFrames, 2u) : stepFrames;
              uint32_t adv = min((uint32_t)avail, ringStep);
              tr.tail = tail + adv;
            }
          }

          if (isRecTrackHere) {
            recTrackSl = sl;
            recTrackSr = sr;
            if (!recording.overdub || autoMuteRec) {
              // Replace mode or auto-muted source (M01..PST): silence recording track audio from master mix
              sl = 0.0f;
              sr = 0.0f;
            }
          } else if (t == recTrk && autoMuteRec) {
            // Keep auto-muted track silent while armed/recording
            sl = 0.0f;
            sr = 0.0f;
          }

          const float *m = mix[t];
          float *c = tr.curMix;
          for (int k = 0; k < 4; k++) c[k] += (m[k] - c[k]) * GLIDE;
          float ol = c[0] * sl + c[1] * sr;
          float orr = c[2] * sl + c[3] * sr;
          peakL[t] = max(peakL[t], fabsf(ol));
          peakR[t] = max(peakR[t], fabsf(orr));
          if (t == srcAudioTrkBlock) {
            srcAudioL = ol;
            srcAudioR = orr;
          }
          ol *= masterGain;
          orr *= masterGain;
          mixL += ol;
          mixR += orr;

          if (usbFrame) {
            int16_t *st = usbFrame + usbTrackChannel(t);
            if (isAudioTrackMono(t)) {
              float om = (c[0] >= c[3]) ? ol : orr;
              st[0] = (int16_t)constrain(om, -32767.0f, 32767.0f);
            } else {
              st[0] = (int16_t)constrain(ol, -32767.0f, 32767.0f);
              st[1] = (int16_t)constrain(orr, -32767.0f, 32767.0f);
            }
          }
        }
      }
    }

    mixL += monL;
    mixR += monR;
    const float preFxL = mixL;
    const float preFxR = mixR;
    processMasterFx(mixL, mixR);
    mixLimiter.run(mixL, mixR, limitCeiling);
    const float postFxL = mixL;
    const float postFxR = mixR;

    if (tMode == TransportMode::WaitThreshold && transport.waitAudioThresh &&
        (targetSrc == AudioRecSrc::ResamplePre || targetSrc == AudioRecSrc::ResamplePost)) {
      float rL = (targetSrc == AudioRecSrc::ResamplePost) ? postFxL : preFxL;
      float rR = (targetSrc == AudioRecSrc::ResamplePost) ? postFxR : preFxR;
      float rPk = recTrackIsMono
                      ? ((recMonoIn == MonoInput::Left)  ? fabsf(rL)
                       : (recMonoIn == MonoInput::Right) ? fabsf(rR)
                                                         : fabsf(0.5f * (rL + rR)))
                      : max(fabsf(rL), fabsf(rR));
      if (rPk >= transport.audioThreshLinear * 32768.0f) {
        tMode = TransportMode::Running;
        transport.mode = TransportMode::Running;
        transport.running = true;
        transport.sampleInBeat = 0;
        midiSendNow(0xFA);
      }
    }

    // Capture per-track selected audio recording source (or Sampler audio-in) into inputRing
    int16_t capL = 0;
    int16_t capR = 0;
    if (smpRec) {
      capL = recSampleL[i];
      capR = recSampleR[i];
    } else {
      int32_t rawSrcL = 0;
      int32_t rawSrcR = 0;
      switch (targetSrc) {
        case AudioRecSrc::Off:
        default:
          break;
        case AudioRecSrc::Line:
          rawSrcL = lineL;
          rawSrcR = lineR;
          break;
        case AudioRecSrc::Usb:
          rawSrcL = uL;
          rawSrcR = uR;
          break;
        case AudioRecSrc::Midi1:
        case AudioRecSrc::Midi2:
        case AudioRecSrc::Midi3:
        case AudioRecSrc::Midi4:
        case AudioRecSrc::Midi5:
        case AudioRecSrc::Midi6:
        case AudioRecSrc::Midi7:
        case AudioRecSrc::Midi8:
          rawSrcL = midiCapL[i];
          rawSrcR = midiCapR[i];
          break;
        case AudioRecSrc::Audio1:
        case AudioRecSrc::Audio2:
        case AudioRecSrc::Audio3:
        case AudioRecSrc::Audio4:
        case AudioRecSrc::Audio5:
        case AudioRecSrc::Audio6:
        case AudioRecSrc::Audio7:
        case AudioRecSrc::Audio8:
          rawSrcL = (int32_t)lrintf(srcAudioL);
          rawSrcR = (int32_t)lrintf(srcAudioR);
          break;
        case AudioRecSrc::ResamplePre:
          rawSrcL = (int32_t)lrintf(preFxL);
          rawSrcR = (int32_t)lrintf(preFxR);
          break;
        case AudioRecSrc::ResamplePost:
          rawSrcL = (int32_t)lrintf(postFxL);
          rawSrcR = (int32_t)lrintf(postFxR);
          break;
      }
      int16_t sL = (int16_t)constrain(rawSrcL, -32768, 32767);
      int16_t sR = (int16_t)constrain(rawSrcR, -32768, 32767);
      if (recTrackIsMono) {
        int16_t mIn = (recMonoIn == MonoInput::Left)  ? sL
                    : (recMonoIn == MonoInput::Right) ? sR
                                                      : (int16_t)(((int32_t)sL + (int32_t)sR) >> 1);
        capL = mIn;
        capR = mIn;
      } else {
        capL = sL;
        capR = sR;
      }
    }
    if (recActiveHere && recording.overdub) {
      int32_t sumL = (int32_t)capL + (int32_t)recTrackSl;
      int32_t sumR = (int32_t)capR + (int32_t)recTrackSr;
      capL = (int16_t)constrain(sumL, -32768, 32767);
      capR = (int16_t)constrain(sumR, -32768, 32767);
    }
    inputRing[at % INPUT_FRAMES] = (uint16_t)capL | ((uint32_t)(uint16_t)capR << 16);

    if (runningStep && runningStepFrames > 0) {
      if (!runningRev) {
        uint32_t nextTf = runningTf + runningStepFrames;
        if (runningLoopOn && nextTf >= transport.punchOutFrame) {
          if (recording.activeInWindow) {
            recording.activeInWindow = false;
            recording.mode = Recording::Finishing;
            recording.endNow = at + 1;
            recording.windowExited = true;
          } else if (recording.mode == Recording::Armed) {
            recording.mode = Recording::Finishing;
            recording.endNow = at + 1;
            recording.windowExited = true;
          }
          nextTf = transport.punchInFrame;
          transport.tapeFrame = nextTf;
          transport.loopWrapCount = transport.loopWrapCount + 1;
          uint32_t safeSpb = max(1u, spb);
          transport.beat = (int32_t)(nextTf / safeSpb);
          transport.sampleInBeat = nextTf % safeSpb;
          tickInBeat_ = (int)((uint64_t)transport.sampleInBeat * 24 / safeSpb);
          nextTickAt_ = (uint64_t)tickInBeat_ * safeSpb / 24;
        } else {
          transport.tapeFrame = nextTf;
          uint32_t nextSib = transport.sampleInBeat + runningStepFrames;
          if (nextSib >= spb) {
            uint32_t safeSpb = max(1u, spb);
            transport.beat = transport.beat + (int32_t)(nextSib / safeSpb);
            transport.sampleInBeat = nextSib % safeSpb;
            if (transport.sampleInBeat > 0) {
              onBeat();
            }
          } else {
            transport.sampleInBeat = nextSib;
          }
        }
      } else {
        bool wrapLoop = runningLoopOn && (runningTf >= transport.punchInFrame) &&
                        (runningTf - transport.punchInFrame < runningStepFrames);
        bool hitZero = !runningLoopOn && (runningTf < runningStepFrames);
        if (wrapLoop) {
          if (recording.activeInWindow) {
            recording.activeInWindow = false;
            recording.mode = Recording::Finishing;
            recording.endNow = at + 1;
            recording.windowExited = true;
          } else if (recording.mode == Recording::Armed) {
            recording.mode = Recording::Finishing;
            recording.endNow = at + 1;
            recording.windowExited = true;
          }
          uint32_t nextTf = (transport.punchOutFrame > 0) ? (transport.punchOutFrame - 1u) : 0u;
          transport.tapeFrame = nextTf;
          transport.loopWrapCount = transport.loopWrapCount + 1;
          uint32_t safeSpb = max(1u, spb);
          transport.beat = (int32_t)(nextTf / safeSpb);
          transport.sampleInBeat = nextTf % safeSpb;
          tickInBeat_ = (int)((uint64_t)transport.sampleInBeat * 24 / safeSpb);
          nextTickAt_ = (uint64_t)tickInBeat_ * safeSpb / 24;
        } else if (hitZero) {
          // Beginning of tape (0:00.000) reached in reverse
          transport.tapeFrame = 0;
          tMode = TransportMode::Stopped;
          transport.mode = TransportMode::Stopped;
          transport.running = false;
          if (recording.activeInWindow) {
            recording.activeInWindow = false;
            recording.mode = Recording::Finishing;
            recording.endNow = at + 1;
            recording.windowExited = true;
          }
          midiSendNow(0xFC);
        } else {
          uint32_t nextTf = runningTf - runningStepFrames;
          transport.tapeFrame = nextTf;
          uint32_t nextSib = transport.sampleInBeat + runningStepFrames;
          if (nextSib >= spb) {
            uint32_t safeSpb = max(1u, spb);
            transport.beat = max((int32_t)0, transport.beat - (int32_t)(nextSib / safeSpb));
            transport.sampleInBeat = nextSib % safeSpb;
            if (transport.sampleInBeat > 0) {
              onBeat();
            }
          } else {
            transport.sampleInBeat = nextSib;
          }
        }
      }
    }

    float click = clickSample() * clickGain;
    outGainNow += (outGain - outGainNow) * MASTER_GLIDE;
    blockPeak = max(blockPeak, max(fabsf(mixL), fabsf(mixR)));
    if (expOut) {
      int16_t expL = (int16_t)constrain((int32_t)lrintf(postFxL), -32768, 32767);
      int16_t expR = (int16_t)constrain((int32_t)lrintf(postFxR), -32768, 32767);
      inputRing[at % INPUT_FRAMES] = (uint16_t)expL | ((uint32_t)(uint16_t)expR << 16);
    }
    if (usbFrame) {
      usbFrame[0] = (int16_t)constrain(postFxL, -32767.0f, 32767.0f);
      usbFrame[1] = (int16_t)constrain(postFxR, -32767.0f, 32767.0f);
    }
    mixL *= outGainNow;
    mixR *= outGainNow;
    transport.now = at + 1;
    if (monUsb) {
      mixL += (float)uL * outGainNow;
      mixR += (float)uR * outGainNow;
    }
    float outAbsL = min(32767.0f, fabsf(mixL));
    float outAbsR = min(32767.0f, fabsf(mixR));
    if (outAbsL > outPeakL) outPeakL = outAbsL;
    if (outAbsR > outPeakR) outPeakR = outAbsR;
    mixL += click;
    mixR += click;
    outL->data[i] = (int16_t)constrain(mixL, -32767.0f, 32767.0f);
    outR->data[i] = (int16_t)constrain(mixR, -32767.0f, 32767.0f);
  }

  refPeak = max(blockPeak, refPeak * REF_FALL);
  if (outPeakL / 32768.0f > outputMeterL) outputMeterL = outPeakL / 32768.0f;
  if (outPeakR / 32768.0f > outputMeterR) outputMeterR = outPeakR / 32768.0f;

  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    Track &tr = tracks[t];
    if (peakL[t] > tr.peakL * 32768.0f) tr.peakL = peakL[t] / 32768.0f;
    if (peakR[t] > tr.peakR * 32768.0f) tr.peakR = peakR[t] / 32768.0f;
  }

  transmit(outL, 0);
  transmit(outR, 1);
  release(outL);
  release(outR);
  if (usbOutBuf) usbAudioWritten();
  if (inL) release(inL);
  if (inR) release(inR);

#if defined(ARM_DWT_CYCCNT)
  uint32_t used = ARM_DWT_CYCCNT - cyclesAt;
  loadCycles = loadCycles + used;
  loadBlocks = loadBlocks + 1;
  if (used > loadMost) loadMost = used;
#endif
}

}  // namespace looper

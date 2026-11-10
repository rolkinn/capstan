// engine.h — 8-Track Audio Tape Engine (4 Stereo + 4 Mono) + Transport & Metronome Clock
#pragma once
#include <Arduino.h>
#include <AudioStream.h>
#include <control_sgtl5000.h>
#include <SD.h>
#include "config.h"
#include "master_fx.h"

namespace looper {

enum class TrackState : uint8_t { Empty, Playing, Muted };
enum class ArmMode : uint8_t { Off = 0, Record = 1, Overdub = 2 };
enum class MonoInput : uint8_t { Left = 0, Right = 1, SumLR = 2 };
enum class TrigMode : uint8_t { Manual = 0, Threshold = 1, Resample = 2 };
enum class TransportMode : uint8_t { Stopped = 0, WaitThreshold = 1, CountIn = 2, Running = 3 };
enum class ClockSource : uint8_t {
  Internal     = 0,
  ExternalAuto = 1,
  ExternalUsb  = 2,
  ExternalTrs  = 3
};
enum class MidiClockOut : uint8_t {
  Off  = 0,
  Usb  = 1,
  Trs  = 2,
  Both = 3
};
enum class MidiTransportIn : uint8_t {
  Auto = 0,
  Usb  = 1,
  Trs  = 2,
  Off  = 3
};
enum MidiPortMask : uint8_t {
  MIDI_PORT_NONE = 0,
  MIDI_PORT_TRS  = 1 << 0,
  MIDI_PORT_USB  = 1 << 1,
  MIDI_PORT_BOTH = MIDI_PORT_TRS | MIDI_PORT_USB
};
enum class AudioInputSource : uint8_t {
  LineAndSynth = 0, // Analog Line-In + Internal Synth (default)
  UsbAndSynth  = 1, // USB Audio In + Internal Synth
  All          = 2, // Analog Line-In + USB Audio In + Internal Synth
  LineOnly     = 3, // Analog Line-In only
  UsbOnly      = 4, // USB Audio In only
  SynthOnly    = 5, // Internal Synth only
  MicAndSynth  = 6, // Internal Microphone + Internal Synth (Model:02 backend, hidden from UI)
  MicOnly      = 7, // Internal Microphone only (Model:02 backend, hidden from UI)
  Count        = 8
};
constexpr int SELECTABLE_AUDIO_INPUT_SOURCES = 6; // Excludes MicAndSynth and MicOnly from UI selection
enum class MetroMode : uint8_t { Off = 0, RecOnly = 1, AlwaysOn = 2 };
enum class MonitorMode : uint8_t { Off = 0, On = 1, Arm = 2 };

struct Track {
  volatile TrackState state = TrackState::Empty;
  volatile ArmMode armMode = ArmMode::Off;
  volatile MonoInput monoInput = MonoInput::Left;
  volatile bool solo = false;
  TrigMode trigMode = TrigMode::Manual;
  int8_t threshDb = DEFAULT_THRESH_DB;             // -60 .. -6 dB

  uint32_t recordedFrames = 0;                     // valid stereo frames on tape (0 .. MAX_REC_FRAMES)
  int8_t volume = VOLUME_UNITY;                    // 0 (off) .. VOLUME_MAX (100), default 80
  int8_t balance = 0;                              // -PAN_MAX (-100 L) .. 0 (C) .. +PAN_MAX (+100 R)
  float mixLL = 1.0f, mixLR = 0.0f, mixRL = 0.0f, mixRR = 1.0f;
  volatile float peakL = 0.0f, peakR = 0.0f;

  // Waveform peak bins across the track's recorded length (or song length)
  uint8_t wavePeaks[WAVE_PEAK_BYTES] = {};
  uint8_t overviewVersion = 0;

  inline uint8_t getWavePeak(int bin) const {
    if (bin < 0 || bin >= WAVE_PEAK_BINS) return 0;
    uint8_t byteVal = wavePeaks[bin >> 1];
    uint8_t nib = (bin & 1) ? (byteVal >> 4) : (byteVal & 0x0F);
    return (uint8_t)(nib * 17);
  }

  inline void setWavePeak(int bin, uint8_t val255) {
    if (bin < 0 || bin >= WAVE_PEAK_BINS) return;
    uint8_t nib = (uint8_t)((val255 + 8) / 17);
    if (nib > 15) nib = 15;
    uint8_t &b = wavePeaks[bin >> 1];
    if (bin & 1) b = (uint8_t)((b & 0x0F) | (nib << 4));
    else         b = (uint8_t)((b & 0xF0) | nib);
  }

  // Ring buffer for streaming playback (in DMAMEM)
  uint32_t *ring = nullptr;
  volatile uint32_t head = 0, tail = 0;
  volatile bool live = false;
  float curMix[4] = {};

  // Producer side (loop() only)
  FsFile file;
  uint32_t fileFrame = 0;                          // tape frame that stream frame `head` corresponds to
};

struct Transport {
  volatile TransportMode mode = TransportMode::Stopped;
  volatile bool running = false;                   // true when mode == Running
  volatile int8_t speedQuarter = DEFAULT_SPEED_QUARTER; // -20 (-5.00x) .. 0 (0.00x) .. +4 (+1.00x) .. +20 (+5.00x)
  volatile uint8_t subFrame = 0;                   // 0..3 quarter-frame accumulator
  volatile uint32_t tapeFrame = 0;                 // linear playhead: 0 .. MAX_REC_FRAMES (666 s)
  volatile uint32_t spb = 22050;                   // samples per quarter-note beat (~120 BPM)
  volatile uint32_t pendingSpb = 0;
  volatile int32_t beat = 0;                       // beat counter
  volatile uint32_t sampleInBeat = 0;
  volatile uint32_t now = 0;                       // monotonic sample counter since boot

  // Count-in state
  volatile int32_t countInRemainingBeats = 0;
  volatile int32_t countInTotalBeats = 4;

  // Shared Punch-In / Punch-Out & Loop window (in stereo frames, 44.1 kHz)
  volatile bool punchEnabled = false;
  volatile bool loopEnabled = false;
  volatile uint32_t punchInFrame = 0;
  volatile uint32_t punchOutFrame = 4 * 22050;     // default 1 bar (2.0 s at 120 BPM)
  volatile uint32_t loopWrapCount = 0;             // incremented by ISR each time loop wraps OUT -> IN

  // Threshold trigger state
  volatile bool waitAudioThresh = false;
  volatile bool waitMidiThresh = false;
  volatile float audioThreshLinear = 0.0316f;      // linear amplitude threshold (0..1)
};

struct Recording {
  enum Mode : uint8_t { Idle = 0, Armed = 1, Active = 2, Finishing = 3 };
  volatile Mode mode = Idle;
  volatile int track = -1;                         // 0..7, or -1 if no audio track armed
  volatile bool overdub = false;                   // true = Sound-on-Sound mix; false = Replace
  volatile bool resample = false;                  // true = record Master Audio Out instead of input source
  volatile bool resamplePostFx = true;             // true = Post-Master FX, false = Pre-Master FX
  volatile bool reverse = false;                   // true = recording in reverse (-1.00x)
  volatile MonoInput monoInput = MonoInput::Left;  // for mono tracks (4..7): Left (T), Right (R), or SumLR
  volatile bool activeInWindow = false;            // ISR: currently inside record/punch window
  volatile bool windowEntered = false;             // ISR -> loop(): entered record window
  volatile bool windowExited = false;              // ISR -> loop(): exited record window (punch-out)
  volatile uint32_t startNow = 0;                  // `transport.now` when active recording started
  volatile uint32_t endNow = 0;                    // `transport.now` when active recording ended
  volatile uint32_t startTapeFrame = 0;            // `transport.tapeFrame` where writing starts
  uint32_t readNow = 0;                            // `loop()` consumer cursor in `inputRing`
  uint32_t writeTapeFrame = 0;                     // `loop()` SD write cursor in tape frames
  uint32_t writtenFrames = 0;
  int16_t lastOverviewCol = -1;                    // last overview column cleared in Replace mode
};

extern Track tracks[NUM_AUDIO_TRACKS];
extern Transport transport;
extern Recording recording;
extern volatile MonitorMode lineMonitorMode;
extern volatile MonitorMode usbMonitorMode;
extern volatile bool recLineInEnabled;
extern volatile bool recUsbInEnabled;
extern volatile bool recInstrEnabled;
extern volatile AudioInputSource audioInputSource;
extern uint32_t inputRing[INPUT_FRAMES];

// Input/Output levels & USB audio
extern volatile float inputMeterL, inputMeterR;
extern volatile float outputMeterL, outputMeterR;
constexpr int USB_OUT_CHANNELS = 2 + 2 * NUM_STEREO_AUDIO_TRACKS + NUM_MONO_AUDIO_TRACKS;
constexpr int usbTrackChannel(int t) {
  return (t < NUM_STEREO_AUDIO_TRACKS)
             ? (2 + 2 * t)
             : (2 + 2 * NUM_STEREO_AUDIO_TRACKS + (t - NUM_STEREO_AUDIO_TRACKS));
}
extern bool (*usbAudioRead)(uint32_t *frames);
extern int16_t *(*usbAudioWrite)();
extern void (*usbAudioWritten)();

// Clock & Tempo helpers
uint32_t spbForBpm(float bpm);
double framesToBeats(uint32_t frames, float bpm);
uint32_t beatsToFrames(double beats, float bpm);
uint32_t framesToMidiTicks(uint32_t frames, float bpm);
uint32_t midiTicksToFrames(uint32_t ticks, float bpm);

// Mixer & Metronome controls
void setTrackMix(int t, int volume, int balance);
void takeTrackPeaks(int t, float &left, float &right);
void takeInputPeaks(float &left, float &right);
void takeOutputPeaks(float &left, float &right);
void setMasterVolume(int step);
void setMetronomeVolume(int step);
void takeEngineLoad(float &avg, float &most);

extern volatile bool metronomeOn;
extern volatile bool masterExportActive;
extern volatile bool samplerRecActive;

// Interrupt-safe critical section helpers (preserves nested ISR PRIMASK state)
static inline uint32_t saveIrq() {
  uint32_t primask = 0;
#if defined(__ARM_ARCH_7EM__) || defined(__IMXRT1062__)
  __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");
#else
  noInterrupts();
#endif
  return primask;
}

static inline void restoreIrq(uint32_t primask) {
#if defined(__ARM_ARCH_7EM__) || defined(__IMXRT1062__)
  if ((primask & 1) == 0) {
    __asm__ volatile("cpsie i" ::: "memory");
  }
#else
  if (!primask) interrupts();
#endif
}

// MIDI hardware & USB output packet/clock queues
struct MidiPacket {
  uint8_t status;
  uint8_t d1;
  uint8_t d2;
  uint8_t len;
};

extern void (*midiInPollHook)();
void midiBegin(HardwareSerial *port);
void midiEnd();
void midiServiceTrsInOut();
uint8_t midiActiveClockOutPorts();
void midiSendNow(uint8_t status, uint8_t portMask = MIDI_PORT_BOTH);
void midiSendClockNow(uint8_t status);
void midiSendMsg(uint8_t status, uint8_t d1, uint8_t d2, int len, uint8_t portMask = MIDI_PORT_BOTH);
void midiOutAttachUsb();
int midiOutTakeUsbPackets(MidiPacket *out, int max);
int midiOutTakeUsbTicks(uint32_t now, int max);

class LooperEngine : public AudioStream {
public:
  LooperEngine() : AudioStream(2, inputQueue_) {}
  float masterGain = 0.70794578f;                  // -3.0 dB mix bus headroom
  void update() override;

private:
  audio_block_t *inputQueue_[2];
  int tickInBeat_ = 0;
  uint32_t nextTickAt_ = 0;
  void onBeat();
};

}  // namespace looper

// Capstan.ino — Capstan: 8-Track Audio + 8-Track MIDI Tape Recorder for M8 (Model:01 & Model:02)
//
// Architecture:
//   * engine.cpp: 8-track 44.1 kHz linear tape engine (4 Stereo + 4 Mono), punch-in/out window,
//                 sound-on-sound overdub, replace & master resampling recording, per-track Vol/Bal/Pan/Solo,
//                 audio input threshold trigger, count-in & woodblock metronome
//   * midi_seq.cpp: 8-track MIDI event recorder & player (Note On/Off, CC, Pitch Bend),
//                   Omni or 1-16 input channel, C01-C16 or I01-I08 output routing, MIDI threshold trigger,
//                   record replace vs overdub merge, live MIDI thru, Piano Roll editor
//   * instruments.cpp: 8-slot internal synthesizer/sampler host (2-Op FM, Phase Distortion,
//                      4-Octave SD WAV Sampler, Noise Texture Drone) with 32-voice global polyphony
//   * master_fx.cpp: 5-slot Master Effects Bus (Chorus -> Delay -> Reverb -> Tape -> Comp)
//   * storage.cpp: SD streaming and in-place record/overdub for 8 WAV tracks (up to 666s)
//                  plus song, project, MIDI, instrument, & MIDI map persistence in /Capstan
//   * ui.cpp: 9-page UI (Audio, Midi, SongSettings, Main, SystemSettings,
//             Instruments, InstrumentType, InstrumentEdit, Master)
//   * input.cpp: SHIFT + Arrows page navigation, tape transport, OPT + Arrows adjustments

#include <AudioStream.h>
#include <input_i2s.h>
#include <output_i2s.h>
#include <control_sgtl5000.h>
#include <SD.h>
#include "src/app.h"
#include "src/ui.h"
#include "src/page_system_settings.h"
#include "src/usb_drive.h"
#include "src/input.h"
#include "src/midi_in.h"
#include "src/midi_usb.h"
#include "src/midi_seq.h"

AudioInputI2S        i2sIn;
LooperEngine         engine;
AudioOutputI2S       i2sOut;
AudioConnection      c1(i2sIn, 0, engine, 0), c2(i2sIn, 1, engine, 1);
AudioConnection      c3(engine, 0, i2sOut, 0), c4(engine, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

// ---- Freeze detector (enabled when CAPSTAN_DEBUG != 0)
static volatile uint32_t loopCount = 0;
#if CAPSTAN_DEBUG
static IntervalTimer freezeTimer;

FLASHMEM static void freezeCheck() {
  static uint32_t lastLoop = 0, lastNow = 0, loopStill = 0, audioStill = 0;
  uint32_t lc = loopCount, now = transport.now;
  if (lc == lastLoop) {
    if (++loopStill == 15) Serial.printf("FREEZE: loop() stuck for 1.5 s in: %s\n", (const char *)debugStage);
  } else {
    if (loopStill >= 15) Serial.printf("loop() resumed after %lu ms\n", loopStill * 100);
    loopStill = 0;
    lastLoop = lc;
  }
  if (now == lastNow) {
    if (++audioStill == 15) Serial.printf("FREEZE: audio interrupt stopped (loop in: %s)\n", (const char *)debugStage);
  } else {
    if (audioStill >= 15) Serial.printf("audio resumed after %lu ms\n", audioStill * 100);
    audioStill = 0;
    lastNow = now;
  }
}
#endif

// ---- Power button handler
static bool powerBtnHeldState = false;
static uint32_t powerBtnPressMs = 0;

FLASHMEM static void handlePowerButton(bool allowProjectPlayLock = false) {
  if (!powerBtnHeldState) {
    if (!board.powerButtonPressed()) return;
    powerBtnHeldState = true;
    powerBtnPressMs = millis();
  }
  if (board.powerButtonHeld()) {
    if (millis() - powerBtnPressMs >= 1000) {
      board.setBacklight(0);
      board.setSpeaker(false);
      codec.volume(0.0f);
      codec.dacVolume(0.0f);
      codec.muteHeadphone();
      codec.muteLineout();
      midiEnd();
      midiUsbEnd();
#if CAPSTAN_DEBUG
      freezeTimer.end();
#endif
      flushHeaders();
      board.powerOff();
    }
    return;
  }
  powerBtnHeldState = false;
  board.powerButtonPressed();
  bool wasBlanked = isScreenBlanked();
  noteUserActivity();
  if (allowProjectPlayLock && !wasBlanked) {
    toggleSysProjectPlayLock();
  }
}

FLASHMEM static void fatal(const char *msg) {
  drawText(4, SY(160) + 16, msg, TEXT_ERROR, BG);
  for (;;) handlePowerButton();
}

FLASHMEM static void waitForCard() {
  auto &t = board.tft();
  t.fillScreen(BG);
  const char *msg = "INSERT AN SD CARD";
  drawText((SCREEN_W - textWidth(msg)) / 2, (SCREEN_H - FONT_H) / 2, msg, LIGHT, BG);
#if CAPSTAN_DEBUG
  Serial.println("SD card: none, waiting for one");
#endif
  while (!cardRestart()) {
    for (uint32_t at = millis(); millis() - at < 300;) {
      loopCount++;
      handlePowerButton();
    }
  }
#if CAPSTAN_DEBUG
  Serial.println("SD card: in");
#endif
  t.fillScreen(BG);
  uiGeneration++;
}

FLASHMEM static void reloadFromCard() {
  for (;;) {
    storageForget();
    if (storageBegin([](const char *msg) {
#if CAPSTAN_DEBUG
      Serial.println(msg);
#else
      (void)msg;
#endif
    })) break;
    if (cardResponds()) fatal("SD card error: check free space in /Capstan.");
    waitForCard();
  }
  beginSession();
  transport.spb = spbForBpm(bpm);
  uiForceRedraw();
}

FLASHMEM static void updateSpeakerFromHeadphones() {
  static bool phonesIn = false, speakerOn = false, first = true;
  bool phones;
  if (board.headphonesChanged(phones)) {
    phonesIn = phones;
  }
  if (first || speakerOn != !phonesIn) {
    first = false;
    speakerOn = !phonesIn;
    board.setSpeaker(speakerOn);
  }
}

FLASHMEM void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  AudioMemory(12);
#if defined(AUDIO_UAC2)
  usbAudioRead = [](uint32_t *frames) { return usb_audio_rx_block(frames) != 0; };
  usbAudioWrite = usb_audio_tx_block;
  usbAudioWritten = usb_audio_tx_written;
#endif
  board.begin();
  board.powerButtonPressed();
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.muteLineout();
  codec.dacVolume(0.0f);
  codec.audioProcessorDisable();
  codec.autoVolumeDisable();
  codec.adcHighPassFilterEnable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.micGain(0);
  codec.lineInLevel(8);
  codec.lineOutLevel(13);
  codec.unmuteLineout();
  uint32_t codecReadyAt = millis();

  showSplash();
  splashIntro();
  auto progress = [](const char *msg) {
#if CAPSTAN_DEBUG
    if (msg && msg[0]) Serial.println(msg);
#endif
    splashProgress(msg);
  };
  while (!storageBegin(progress)) {
    if (cardResponds()) fatal("SD card error: check free space in /Capstan.");
    storageForget();
    waitForCard();
    showSplash();
  }
  progress("");
  splashTick();
  beginSession();
  splashTick();
  transport.spb = spbForBpm(bpm);

  while (millis() - codecReadyAt < 150) delay(1);
  codec.dacVolume(1.0f);
  codec.volume(0.45f);
  updateSpeakerFromHeadphones();

  if (MIDI_OUT_ENABLED) midiBegin(board.midiOutBegin());
  if (MIDI_OUT_ENABLED && MIDI_IN_ENABLED) midiInBegin(board.midiInBegin());
  if (MIDI_OUT_ENABLED && MIDI_USB_ENABLED) midiUsbBegin();

  usbDriveTick = [] { loopCount++; handlePowerButton(); };
  usbDriveReload = reloadFromCard;
  usbDriveBegin();

  hideSplash();
#if CAPSTAN_DEBUG
  freezeTimer.priority(255);
  freezeTimer.begin(freezeCheck, 100000);
#endif

  board.tft().fillScreen(BG);
  uiForceRedraw();
}

void loop() {
  board.chargeGuard();
  loopCount++;
#if CAPSTAN_DEBUG
  debugStage = "power button";
#endif
  handlePowerButton(true);

  // Hot-swap SD card check when transport is stopped
  static uint32_t cardCheckAt = 0;
  if (transport.mode == TransportMode::Stopped && recording.mode == Recording::Idle && millis() - cardCheckAt >= 500) {
    cardCheckAt = millis();
#if CAPSTAN_DEBUG
    debugStage = "SD card check";
#endif
    if (!cardResponds()) {
#if CAPSTAN_DEBUG
      Serial.println("SD card: removed, waiting for card");
#endif
      reloadFromCard();
    }
  }

#if CAPSTAN_DEBUG
  debugStage = "storage";
#endif
  serviceStorage();

  // Speaker off while headphones are plugged in
  updateSpeakerFromHeadphones();

#if CAPSTAN_DEBUG
  debugStage = "MIDI in & seq";
#endif
  midiInService();
  midiSeqService(bpm, midiSeqFocusedTrack(), songSettings().midiThru);

  if (LOG_ENGINE_LOAD) {
    static uint32_t loadAt = 0;
    if (millis() - loadAt >= 5000) {
      loadAt = millis();
      float avg, most;
      takeEngineLoad(avg, most);
      int playing = 0;
      for (const auto &tr : tracks) if (tr.live) playing++;
      Serial.printf("engine: %.1f%% avg, %.1f%% max (%d audio tracks active)\n",
                    avg * 100, most * 100, playing);
    }
  }

#if CAPSTAN_DEBUG
  debugStage = "keys";
#endif
  pollKeys();
  updateMetronomeState();

#if CAPSTAN_DEBUG
  debugStage = "screen";
#endif
  uiRefresh();
}

// midi_usb.cpp — USB MIDI Clock & Channel Voice I/O + Clean USB Shutdown Detach
#include <Arduino.h>
#include <string.h>
#include "usb_dev.h"
#if defined(USB_SERIAL_MSC)
#include "usb_msc.h"
#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)
#include "usb_audio_l7.h"
#endif
#endif
#include "midi_usb.h"
#include "engine.h"
#include "midi_in.h"
#include "midi_seq.h"

using namespace looper;

#if defined(MIDI_INTERFACE)
namespace {

static inline bool usbMidiEndpointActive() {
#if defined(USB_SERIAL_MSC)
  return usb_configuration && !usb_msc_is_active();
#else
  return usb_configuration != 0;
#endif
}

} // namespace

static IntervalTimer usbTimer;
static constexpr uint32_t USB_MIDI_TX_EP_MASK = 1u << (MIDI_TX_ENDPOINT + 16);
static constexpr uint32_t USB_MIDI_TX_NUM = 4;
static constexpr uint32_t USB_MIDI_TX_WORDS = 16; // 64 bytes in uncached RAM1 (DTCM)
static constexpr uint32_t USB_MIDI_RX_BUDGET_PER_TICK = 64; // up to 64 32-bit USB-MIDI packets / 250 us (1 MB/s)
static transfer_t usbMidiTxXfer[USB_MIDI_TX_NUM] __attribute__((used, aligned(32)));
static uint32_t usbMidiTxBuf[USB_MIDI_TX_NUM][USB_MIDI_TX_WORDS] __attribute__((aligned(32)));
static uint8_t usbMidiTxHead = 0;
static uint32_t usbMidiSeenEpoch = 0;
static uint32_t usbTxBusySinceUs = 0;
static uint8_t seqTickDiv = 0;

static inline uint32_t packUsbMidiPacket(const MidiPacket &p) {
  const uint8_t st = p.status;
  if (st >= 0xF8 || st == 0xF6) {
    return 0x0Fu | ((uint32_t)st << 8);
  }
  if (st == 0xF2) {
    return 0x03u | (0xF2u << 8) | ((uint32_t)(p.d1 & 0x7F) << 16) | ((uint32_t)(p.d2 & 0x7F) << 24);
  }
  if (st == 0xF1 || st == 0xF3) {
    return 0x02u | ((uint32_t)st << 8) | ((uint32_t)(p.d1 & 0x7F) << 16);
  }
  if (st >= 0x80 && st <= 0xEF) {
    const uint8_t cin = (st >> 4) & 0x0F;
    const uint8_t d2 = (cin == 0x0C || cin == 0x0D) ? 0u : (p.d2 & 0x7F);
    return (uint32_t)cin | ((uint32_t)st << 8) | ((uint32_t)(p.d1 & 0x7F) << 16) | ((uint32_t)d2 << 24);
  }
  return 0;
}

static void poll() {
  // 0. Service TRS Serial MIDI In/Out + Clock on the same 4 kHz (250 us) timer
  midiServiceTrsInOut();

  // 1. Receive incoming USB MIDI packets (Live Thru & Synth dispatch happen inside midiSeqInputEvent)
  // Loop on lock-free usb_midi_available() rather than usbMIDI.read() so multi-packet SysEx chunks
  // (CIN 0x04 / 0x0F) and ignored USB-MIDI packets drain immediately without stalling EP4 OUT.
  uint32_t rxBudget = USB_MIDI_RX_BUDGET_PER_TICK;
  while (rxBudget-- > 0 && usbMidiEndpointActive() && usb_midi_available()) {
    if (!usbMIDI.read()) continue;
    uint8_t type = usbMIDI.getType();
    uint32_t us = micros();
    if (type >= 0xF8) {
      midiInByte(type, us, MIDI_USB);
    } else if (type == 0xF0) {
      midiInSysEx(usbMIDI.getSysExArray(), usbMIDI.getSysExArrayLength(), us, MIDI_USB);
    } else if (type == 0xF2) {
      midiInSongPosition(usbMIDI.getData1() | (usbMIDI.getData2() << 7), us, MIDI_USB);
    } else if (type >= 0x80 && type <= 0xEF) {
      uint8_t ch0 = (usbMIDI.getChannel() - 1) & 0x0F;
      midiSeqInputEvent((type & 0xF0) | ch0, usbMIDI.getData1(), usbMIDI.getData2(), MIDI_USB);
    }
  }

  // 2. 1 kHz (1.0 ms) MIDI Sequencer playback tick (dispatches due external MIDI & internal synth events)
  if ((++seqTickDiv & 3u) == 0u) {
    midiSeqIsrTick();
  }

  // 3. Transmit queued outgoing USB MIDI clock ticks and packets (lock-free packet queue)
  // Uses a dedicated 4-descriptor DTCM DMA ring on MIDI_TX_ENDPOINT (bypassing usb_midi_write_packed,
  // its 40 ms blocking yield() loop, and its 8 kHz SOF auto-flush interrupt) with minimal IRQ_USB1 masking.
  const uint32_t nowUs = micros();
  if (!usbMidiEndpointActive()) {
    usbTxBusySinceUs = 0;
    midiOutTakeUsbTicks(nowUs, 16);
    MidiPacket dropPkts[16];
    midiOutTakeUsbPackets(dropPkts, 16);
  } else if (!midiOutHasUsbPending(nowUs)) {
    usbTxBusySinceUs = 0;
  } else if (((USB1_ENDPTPRIME | USB1_ENDPTSTATUS) & USB_MIDI_TX_EP_MASK) != 0) {
    if (usbTxBusySinceUs == 0) {
      usbTxBusySinceUs = nowUs ? nowUs : 1u;
    } else if ((uint32_t)(nowUs - usbTxBusySinceUs) >= 4000u) {
      // Host is not polling USB MIDI IN; drain queues so they do not back up
      midiOutTakeUsbTicks(nowUs, 16);
      MidiPacket dropPkts[16];
      midiOutTakeUsbPackets(dropPkts, 16);
    }
  } else {
    uint32_t localWords[USB_MIDI_TX_WORDS];
    uint32_t wordCount = 0;

    int ticks = midiOutTakeUsbTicks(nowUs, 7);
    for (int i = 0; i < ticks; i++) {
      localWords[wordCount++] = 0x0000F80Fu;
    }

    MidiPacket pkts[8];
    int n = midiOutTakeUsbPackets(pkts, 8);
    for (int i = 0; i < n; i++) {
      uint32_t w = packUsbMidiPacket(pkts[i]);
      if (w != 0 && wordCount < USB_MIDI_TX_WORDS) {
        localWords[wordCount++] = w;
      }
    }

    usbTxBusySinceUs = 0;
    if (wordCount > 0) {
      NVIC_DISABLE_IRQ(IRQ_USB1);
      __asm__ volatile("dsb\n\tisb" ::: "memory");
      if (usbMidiEndpointActive() && (((USB1_ENDPTPRIME | USB1_ENDPTSTATUS) & USB_MIDI_TX_EP_MASK) == 0)) {
#if defined(USB_SERIAL_MSC)
        const uint32_t ep = usb_midi_reset_epoch;
        if (usbMidiSeenEpoch != ep) {
          usbMidiSeenEpoch = ep;
          memset(usbMidiTxXfer, 0, sizeof(usbMidiTxXfer));
          usbMidiTxHead = 0;
        }
#endif
        const uint8_t slot = usbMidiTxHead;
        transfer_t *const xfer = &usbMidiTxXfer[slot];
        if ((xfer->status & 0x80u) == 0) {
          uint32_t *const buf = usbMidiTxBuf[slot];
          memcpy(buf, localWords, wordCount * sizeof(uint32_t));
          usbMidiTxHead = (slot + 1) & (USB_MIDI_TX_NUM - 1);
          usb_prepare_transfer(xfer, buf, wordCount * sizeof(uint32_t), 0);
          usb_transmit(MIDI_TX_ENDPOINT, xfer);
        }
      }
      NVIC_ENABLE_IRQ(IRQ_USB1);
    }
  }
}

FLASHMEM void midiUsbBegin() {
  midiOutAttachUsb();
  usbTimer.priority(224);
  usbTimer.begin(poll, 250);
}

FLASHMEM void midiUsbEnd() {
  usbTimer.end();
}

FLASHMEM void usbShutdownDetach() {
  NVIC_DISABLE_IRQ(IRQ_USB1);
  __asm__ volatile("dsb\n\tisb" ::: "memory");
  USB1_USBCMD &= ~USB_USBCMD_RS;
  usb_configuration = 0;
  USB1_ENDPTSETUPSTAT = USB1_ENDPTSETUPSTAT;
  USB1_ENDPTCOMPLETE = USB1_ENDPTCOMPLETE;
  while (USB1_ENDPTPRIME != 0) {}
  USB1_ENDPTFLUSH = 0xFFFFFFFFu;
  while (USB1_ENDPTFLUSH != 0) {}
#if defined(USB_SERIAL_MSC)
  usb_midi_reset();
#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)
  usb_audio_reset();
#endif
#endif
}

FLASHMEM bool usbIsHostConnected() {
  return (usb_configuration != 0) || ((USB1_PORTSC1 & 1u) != 0);
}

FLASHMEM void usbSetChargeDescriptorFlag(bool chargeWhileOn) {
#if defined(USB_SERIAL_MSC)
  usb_set_self_powered_no_charge(chargeWhileOn ? 0 : 1, 0, nullptr);
#else
  (void)chargeWhileOn;
#endif
}

FLASHMEM void usbReenumerateChargeDescriptor(bool chargeWhileOn, void (*tickFn)()) {
#if defined(USB_SERIAL_MSC)
  usb_set_self_powered_no_charge(chargeWhileOn ? 0 : 1, 1, tickFn);
#else
  (void)chargeWhileOn;
  (void)tickFn;
#endif
}
#else
void midiUsbBegin() {}
void midiUsbEnd() {}
void usbShutdownDetach() {}
bool usbIsHostConnected() { return false; }
void usbSetChargeDescriptorFlag(bool) {}
void usbReenumerateChargeDescriptor(bool, void (*)()) {}
#endif

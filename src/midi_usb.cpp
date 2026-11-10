// midi_usb.cpp — USB MIDI Clock & Channel Voice I/O
#include <Arduino.h>
#include <string.h>
#include "usb_dev.h"
#include "midi_usb.h"
#include "engine.h"
#include "midi_in.h"
#include "midi_seq.h"

using namespace looper;

#if defined(MIDI_INTERFACE)
namespace {

struct UsbEndpointQueueHead {
  uint32_t config;
  uint32_t current;
  uint32_t next;
  uint32_t status;
  uint32_t pointer0;
  uint32_t pointer1;
  uint32_t pointer2;
  uint32_t pointer3;
  uint32_t pointer4;
  uint32_t reserved;
  uint32_t setup0;
  uint32_t setup1;
  transfer_t *first_transfer;
  transfer_t *last_transfer;
  void (*callback_function)(transfer_t *completed_transfer);
  uint32_t unused1;
};

} // namespace

extern "C" UsbEndpointQueueHead endpoint_queue_head[];

static IntervalTimer usbTimer;
static constexpr uint32_t USB_MIDI_TX_EP_MASK = 1u << (MIDI_TX_ENDPOINT + 16);
static constexpr uint32_t USB_MIDI_TX_NUM = 4;
static constexpr uint32_t USB_MIDI_TX_WORDS = 16; // 64 bytes (2 cache lines, fits <= 64B Full-Speed & 512B High-Speed)
static transfer_t usbMidiTxXfer[USB_MIDI_TX_NUM] __attribute__((used, aligned(32)));
DMAMEM static uint32_t usbMidiTxBuf[USB_MIDI_TX_NUM][USB_MIDI_TX_WORDS] __attribute__((aligned(32)));
static uint8_t usbMidiTxHead = 0;
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
  while (usb_configuration && usbMIDI.read()) {
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
  // Uses a dedicated 4-descriptor DMA ring on MIDI_TX_ENDPOINT (bypassing usb_midi_write_packed,
  // its 40 ms blocking yield() loop, and its 8 kHz SOF auto-flush interrupt) guarded by NVIC_DISABLE_IRQ(IRQ_USB1).
  const uint32_t nowUs = micros();
  if (!usb_configuration) {
    usbTxBusySinceUs = 0;
    midiOutTakeUsbTicks(nowUs, 16);
    MidiPacket dropPkts[16];
    midiOutTakeUsbPackets(dropPkts, 16);
  } else {
    NVIC_DISABLE_IRQ(IRQ_USB1);
    UsbEndpointQueueHead *const txEp = &endpoint_queue_head[MIDI_TX_ENDPOINT * 2 + 1];
    if (txEp->last_transfer == nullptr) {
      memset(usbMidiTxXfer, 0, sizeof(usbMidiTxXfer));
      usbMidiTxHead = 0;
    }
    transfer_t *const xfer = &usbMidiTxXfer[usbMidiTxHead];
    const bool epBusy = (((USB1_ENDPTPRIME | USB1_ENDPTSTATUS) & USB_MIDI_TX_EP_MASK) != 0) ||
                        ((usb_transfer_status(xfer) & 0x80u) != 0);
    if (!epBusy) {
      usbTxBusySinceUs = 0;
      uint32_t *const buf = usbMidiTxBuf[usbMidiTxHead];
      uint32_t wordCount = 0;

      int ticks = midiOutTakeUsbTicks(nowUs, 7);
      for (int i = 0; i < ticks; i++) {
        buf[wordCount++] = 0x0000F80Fu;
      }

      MidiPacket pkts[8];
      int n = midiOutTakeUsbPackets(pkts, 8);
      for (int i = 0; i < n; i++) {
        uint32_t w = packUsbMidiPacket(pkts[i]);
        if (w != 0 && wordCount < USB_MIDI_TX_WORDS) {
          buf[wordCount++] = w;
        }
      }

      if (wordCount > 0) {
        usbMidiTxHead = (usbMidiTxHead + 1) & (USB_MIDI_TX_NUM - 1);
        arm_dcache_flush_delete(buf, sizeof(usbMidiTxBuf[0]));
        usb_prepare_transfer(xfer, buf, wordCount * sizeof(uint32_t), 0);
        usb_transmit(MIDI_TX_ENDPOINT, xfer);
      }
      NVIC_ENABLE_IRQ(IRQ_USB1);
    } else {
      NVIC_ENABLE_IRQ(IRQ_USB1);
      if (usbTxBusySinceUs == 0) {
        usbTxBusySinceUs = nowUs ? nowUs : 1u;
      } else if ((uint32_t)(nowUs - usbTxBusySinceUs) >= 4000u) {
        // Host is not polling USB MIDI IN; drain queues so they do not back up
        midiOutTakeUsbTicks(nowUs, 16);
        MidiPacket dropPkts[16];
        midiOutTakeUsbPackets(dropPkts, 16);
      }
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
#else
void midiUsbBegin() {}
void midiUsbEnd() {}
#endif

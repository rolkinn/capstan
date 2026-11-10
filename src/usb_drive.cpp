// usb_drive.cpp — USB Mass Storage mode for mounting the M8 SD card on a computer
#include "usb_drive.h"
#include "pages.h"
#include "storage.h"
#include "midi_in.h"

#if defined(USB_SERIAL_MSC)
#include "usb_desc.h"
#include "usb_dev.h"
#include "usb_msc.h"
#include "core_pins.h"
#include <string.h>

extern "C" {

extern volatile uint8_t usb_high_speed;
extern volatile uint8_t usb_configuration;

#define SECTOR 512
#define DATA_BYTES 16384

// Share the 16 KB (32-byte aligned) RAM1 SD streaming buffer from storage.cpp
// instead of allocating a second 16 KB static buffer in RAM1 .bss.
static inline uint8_t *msc_data_buf() {
  return reinterpret_cast<uint8_t *>(looper::storageSharedChunkBuf());
}

static uint8_t cbw_buf[512] __attribute__ ((aligned(32)));
static uint8_t csw_buf[32] __attribute__ ((aligned(32)));
static uint8_t small_buf[64] __attribute__ ((aligned(32)));
#define RXN 2
#define TXN 4
static transfer_t rx_xfer[RXN] __attribute__ ((used, aligned(32)));
static transfer_t tx_xfer[TXN] __attribute__ ((used, aligned(32)));
static uint8_t rx_head = 0, tx_head = 0;
static transfer_t *cbw_xfer = NULL;
static transfer_t *async_xfer[3];
static uint8_t async_used = 0;
static uint8_t async_mode = 0;
static volatile uint8_t need_init = 1;
static uint32_t async_since = 0;

static const usb_msc_media_t *media = NULL;
static uint16_t pkt = 64;
static volatile uint8_t reset_requested = 0;
static uint8_t cbw_queued = 0, host_seen = 0, ejected = 0;
static uint32_t n_read = 0, n_written = 0;
static uint8_t sense_key = 0, sense_asc = 0, sense_ascq = 0;

volatile uint8_t usb_msc_last_op = 0;
volatile uint32_t usb_msc_commands = 0;
volatile uint32_t usb_msc_failed = 0, usb_msc_last_sense = 0, usb_msc_broken = 0, usb_msc_restarts = 0;
volatile uint32_t usb_msc_reads = 0, usb_msc_writes = 0;
volatile uint8_t usb_msc_failed_op = 0;

static volatile uint8_t attention = 0;

FLASHMEM void usb_msc_set_media(const usb_msc_media_t *m) {
  media = m;
  attention = m != NULL;
}
FLASHMEM void usb_msc_clear_host_state(void) { host_seen = 0; ejected = 0; n_read = 0; n_written = 0; }
FLASHMEM int usb_msc_host_seen(void) { return host_seen; }
FLASHMEM int usb_msc_ejected(void) { return ejected; }
FLASHMEM uint32_t usb_msc_sectors_read(void) { return n_read; }
FLASHMEM uint32_t usb_msc_sectors_written(void) { return n_written; }
void usb_msc_reset_request(void) { need_init = 1; }

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static inline void stall_in(void)  { *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) |= USB_ENDPTCTRL_TXS; }
static inline void stall_out(void) { *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_RX_ENDPOINT) |= USB_ENDPTCTRL_RXS; }

FLASHMEM static void queue_cbw(void) {
  if (cbw_queued) return;
  cbw_xfer = &rx_xfer[rx_head];
  rx_head = (rx_head + 1) % RXN;
  NVIC_DISABLE_IRQ(IRQ_USB1);
  arm_dcache_delete(cbw_buf, sizeof cbw_buf);
  usb_prepare_transfer(cbw_xfer, cbw_buf, sizeof cbw_buf, 0);
  usb_receive(MSC_RX_ENDPOINT, cbw_xfer);
  NVIC_ENABLE_IRQ(IRQ_USB1);
  cbw_queued = 1;
}

FLASHMEM void usb_msc_configure(void) {
  pkt = usb_high_speed ? MSC_RX_SIZE_480 : MSC_RX_SIZE_12;
  usb_config_tx(MSC_TX_ENDPOINT, pkt, 0, NULL);
  usb_config_rx(MSC_RX_ENDPOINT, pkt, 0, NULL);
  attention = 0;
  need_init = 1;
}

FLASHMEM static void init_protocol(void) {
  usb_msc_restarts++;
  need_init = 0;
  reset_requested = 0;
  NVIC_DISABLE_IRQ(IRQ_USB1);
  uint32_t mask = (1u << MSC_RX_ENDPOINT) | (1u << (MSC_TX_ENDPOINT + 16));
  do {
    USB1_ENDPTFLUSH = mask;
    while (USB1_ENDPTFLUSH & mask) {}
  } while (USB1_ENDPTSTATUS & mask);
  *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_RX_ENDPOINT) &= ~USB_ENDPTCTRL_RXS;
  *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) &= ~USB_ENDPTCTRL_TXS;
  *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_RX_ENDPOINT) |= USB_ENDPTCTRL_RXR;
  *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) |= USB_ENDPTCTRL_TXR;
  usb_config_tx(MSC_TX_ENDPOINT, pkt, 0, NULL);
  usb_config_rx(MSC_RX_ENDPOINT, pkt, 0, NULL);
  NVIC_ENABLE_IRQ(IRQ_USB1);
  memset(rx_xfer, 0, sizeof rx_xfer);
  memset(tx_xfer, 0, sizeof tx_xfer);
  rx_head = 0;
  tx_head = 0;
  async_used = 0;
  async_mode = 0;
  cbw_queued = 0;
  host_seen = 0;
  ejected = 0;
  sense_key = sense_asc = sense_ascq = 0;
  queue_cbw();
}

FLASHMEM static uint32_t wait_done(transfer_t *x, uint32_t timeout_ms) {
  uint32_t t0 = millis();
  while (1) {
    if (!usb_configuration || reset_requested || need_init) return 0x40;
    uint32_t st = usb_transfer_status(x);
    if (!(st & 0x80)) return st;
    if (millis() - t0 > timeout_ms) return 0x40;
  }
}

FLASHMEM static int send_in(const void *buf, uint32_t len) {
  if (async_mode && async_used >= 3) return 0;
  transfer_t *x = &tx_xfer[tx_head];
  tx_head = (tx_head + 1) % TXN;
  NVIC_DISABLE_IRQ(IRQ_USB1);
  if (len) arm_dcache_flush_delete((void *)buf, (len + 31) & ~31u);
  usb_prepare_transfer(x, buf, len, 0);
  usb_transmit(MSC_TX_ENDPOINT, x);
  NVIC_ENABLE_IRQ(IRQ_USB1);
  if (async_mode) {
    async_xfer[async_used++] = x;
    return 1;
  }
  return !(wait_done(x, 8000) & 0x68);
}

FLASHMEM static int recv_out(void *buf, uint32_t len, uint32_t *got) {
  transfer_t *x = &rx_xfer[rx_head];
  rx_head = (rx_head + 1) % RXN;
  NVIC_DISABLE_IRQ(IRQ_USB1);
  arm_dcache_delete(buf, (len + 31) & ~31u);
  usb_prepare_transfer(x, buf, len, 0);
  usb_receive(MSC_RX_ENDPOINT, x);
  NVIC_ENABLE_IRQ(IRQ_USB1);
  uint32_t st = wait_done(x, 8000);
  if (st & 0x68) return 0;
  *got = len - ((st >> 16) & 0x7FFF);
  return 1;
}

typedef struct {
  uint32_t tag, dlen, residue;
  uint8_t in, status;
} cmd_t;

FLASHMEM static void fail(cmd_t *c, uint8_t key, uint8_t asc, uint8_t ascq) {
  sense_key = key;
  sense_asc = asc;
  sense_ascq = ascq;
  c->status = 1;
  if (c->dlen) {
    if (c->in) stall_in(); else stall_out();
  }
}

FLASHMEM static void data_in(cmd_t *c, const void *buf, uint32_t len) {
  if (len > c->dlen) len = c->dlen;
  if (!len) {
    if (c->dlen) fail(c, 5, 0x24, 0);
    return;
  }
  if (!send_in(buf, len)) { c->status = 2; return; }
  if (len < c->dlen && len % pkt == 0) send_in(buf, 0);
  c->residue = c->dlen - len;
}

FLASHMEM static uint32_t sector_count(void) { return media ? media->sector_count() : 0; }

FLASHMEM static void do_read(cmd_t *c, uint32_t lba, uint32_t count) {
  if ((uint64_t)lba + count > sector_count()) { fail(c, 5, 0x21, 0); return; }
  uint8_t *const data_buf = msc_data_buf();
  uint32_t sent = 0;
  while (count) {
    uint32_t n = count > DATA_BYTES / SECTOR ? DATA_BYTES / SECTOR : count;
    if (!media->read(lba, data_buf, n)) {
      sense_key = 3; sense_asc = 0x11; sense_ascq = 0;
      c->status = 1;
      stall_in();
      c->residue = c->dlen > sent ? c->dlen - sent : 0;
      return;
    }
    if (!send_in(data_buf, n * SECTOR)) { c->status = 2; return; }
    sent += n * SECTOR;
    n_read += n;
    lba += n;
    count -= n;
  }
  if (sent < c->dlen && sent % pkt == 0) send_in(data_buf, 0);
  c->residue = c->dlen > sent ? c->dlen - sent : 0;
}

FLASHMEM static void do_write(cmd_t *c, uint32_t lba, uint32_t count) {
  if ((uint64_t)lba + count > sector_count()) { fail(c, 5, 0x21, 0); return; }
  uint8_t *const data_buf = msc_data_buf();
  uint32_t got_total = 0;
  while (count) {
    uint32_t n = count > DATA_BYTES / SECTOR ? DATA_BYTES / SECTOR : count, got = 0;
    if (!recv_out(data_buf, n * SECTOR, &got)) { c->status = 2; return; }
    if (got != n * SECTOR) { c->status = 2; return; }
    if (!media->write(lba, data_buf, n)) {
      sense_key = 3; sense_asc = 0x0C; sense_ascq = 0;
      c->status = 1;
      stall_out();
      c->residue = c->dlen > got_total ? c->dlen - got_total : 0;
      return;
    }
    got_total += got;
    n_written += n;
    lba += n;
    count -= n;
  }
  c->residue = c->dlen > got_total ? c->dlen - got_total : 0;
}

FLASHMEM static void handle_command(cmd_t *c, const uint8_t *cb) {
  if (cb[0] != 0x12 && cb[0] != 0x03 && cb[0] != 0x1E) {
    if (!media) { fail(c, 2, 0x3A, 0); return; }
    if (attention) {
      attention = 0;
      if (cb[0] == 0x00) { fail(c, 6, 0x28, 0); return; }
    }
  }
  switch (cb[0]) {
  case 0x00:
    break;
  case 0x03: {
    memset(small_buf, 0, 18);
    small_buf[0] = 0x70;
    small_buf[2] = sense_key;
    small_buf[7] = 10;
    small_buf[12] = sense_asc;
    small_buf[13] = sense_ascq;
    sense_key = sense_asc = sense_ascq = 0;
    data_in(c, small_buf, cb[4] < 18 ? cb[4] : 18);
    break;
  }
  case 0x12: {
    if (cb[1] & 1) { fail(c, 5, 0x24, 0); break; }
    memset(small_buf, 0, 36);
    small_buf[1] = 0x80;
    small_buf[2] = 0x04;
    small_buf[3] = 0x02;
    small_buf[4] = 31;
    memcpy(small_buf + 8, "Capstan ", 8);
    memcpy(small_buf + 16, "SD CARD         ", 16);
    memcpy(small_buf + 32, "1.0 ", 4);
    data_in(c, small_buf, cb[4] < 36 ? cb[4] : 36);
    break;
  }
  case 0x1A:
    memset(small_buf, 0, 4);
    small_buf[0] = 3;
    data_in(c, small_buf, 4);
    break;
  case 0x1B:
    if ((cb[4] & 2) && !(cb[4] & 1)) {
      if (media->sync) media->sync();
      ejected = 1;
    }
    break;
  case 0x1E:
    break;
  case 0x23:
    memset(small_buf, 0, 12);
    small_buf[3] = 8;
    put_be32(small_buf + 4, sector_count());
    small_buf[8] = 2;
    small_buf[10] = SECTOR >> 8;
    data_in(c, small_buf, 12);
    break;
  case 0x25:
    put_be32(small_buf, sector_count() - 1);
    put_be32(small_buf + 4, SECTOR);
    data_in(c, small_buf, 8);
    break;
  case 0x28:
    do_read(c, be32(cb + 2), cb[7] << 8 | cb[8]);
    break;
  case 0x2A:
    do_write(c, be32(cb + 2), cb[7] << 8 | cb[8]);
    break;
  case 0x2F:
    break;
  case 0x35:
    if (media->sync) media->sync();
    break;
  default:
    fail(c, 5, 0x20, 0);
    break;
  }
}

FLASHMEM static void run_command(void) {
  uint32_t st = usb_transfer_status(cbw_xfer);
  cbw_queued = 0;
  uint32_t got = sizeof cbw_buf - ((st >> 16) & 0x7FFF);
  host_seen = 1;
  if ((st & 0x68) || got != 31 || le32(cbw_buf) != 0x43425355) {
    stall_in();
    stall_out();
    queue_cbw();
    return;
  }
  cmd_t c;
  c.tag = le32(cbw_buf + 4);
  c.dlen = le32(cbw_buf + 8);
  c.in = cbw_buf[12] & 0x80;
  c.residue = c.dlen;
  c.status = 0;
  usb_msc_last_op = cbw_buf[15];
  usb_msc_commands++;
  handle_command(&c, cbw_buf + 15);
  if (cbw_buf[15] == 0x28) usb_msc_reads++;
  if (cbw_buf[15] == 0x2A) usb_msc_writes++;
  if (c.status == 1 && cbw_buf[15] != 0x03) {
    usb_msc_failed++;
    usb_msc_failed_op = cbw_buf[15];
    usb_msc_last_sense = (uint32_t)sense_key << 16 | (uint32_t)sense_asc << 8 | sense_ascq;
  }
  if (c.status == 2) usb_msc_broken++;
  if (reset_requested || need_init) return;
  if (c.status != 2) {
    put_le32(csw_buf, 0x53425355);
    put_le32(csw_buf + 4, c.tag);
    put_le32(csw_buf + 8, c.residue);
    csw_buf[12] = c.status;
    send_in(csw_buf, 13);
  }
  if (!async_mode && !cbw_queued) queue_cbw();
}

FLASHMEM static void poll_async(void) {
  if (!usb_configuration) { cbw_queued = 0; async_used = 0; return; }
  if (need_init || reset_requested) { init_protocol(); async_since = millis(); return; }
  if (async_used) {
    int busy = 0;
    for (int k = 0; k < async_used; k++) busy |= (usb_transfer_status(async_xfer[k]) & 0x80) != 0;
    if (busy) {
      if (millis() - async_since > 3000) need_init = 1;
      return;
    }
    async_used = 0;
    queue_cbw();
    return;
  }
  if (!cbw_queued) queue_cbw();
  if (usb_transfer_status(cbw_xfer) & 0x80) return;
  async_mode = 1;
  async_used = 0;
  run_command();
  async_mode = 0;
  if (!async_used && !cbw_queued) queue_cbw();
  async_since = millis();
}

FLASHMEM static void poll_blocking(void) {
  if (!usb_configuration) { cbw_queued = 0; return; }
  if (need_init || reset_requested) { init_protocol(); return; }
  if (async_used) {
    poll_async();
    return;
  }
  if (!cbw_queued) queue_cbw();
  if (usb_transfer_status(cbw_xfer) & 0x80) return;
  run_command();
}

void usb_msc_poll(void) {
  if (media) poll_blocking(); else poll_async();
}

} // extern "C"
#endif

namespace looper {

void (*usbDriveTick)() = nullptr;
void (*usbDriveReload)() = nullptr;

PROGMEM static const char HDR_TITLE[] = "USB STORAGE MODE";
PROGMEM static const char HINT[] = "PRESS ANY KEY TO EXIT";

static constexpr int OV_X     = SX(40);
static constexpr int OV_W     = SX(240);
static constexpr int HDR_Y    = SY(68);
static constexpr int HDR_H    = SY(22);
static constexpr int ROW_H    = SY(24);
static constexpr int STATUS_Y = HDR_Y + HDR_H;
static constexpr int HINT_Y   = STATUS_Y + ROW_H;
static constexpr int TOTAL_H  = HDR_H + 2 * ROW_H;

FLASHMEM static void drawUsbOverlayHeader() {
  Surface s(OV_W, HDR_H);
  s.fillRect(0, 0, OV_W, HDR_H, DARK);
  s.fillRect(0, 0, OV_W, PX, ACCENT);
  s.fillRect(0, 0, PX, HDR_H, ACCENT);
  s.fillRect(OV_W - PX, 0, PX, HDR_H, ACCENT);
  s.fillRect(PX, HDR_H - 1, OV_W - 2 * PX, 1, MID);
  int tw = textWidth(HDR_TITLE);
  s.drawText(max(SX(6), (OV_W - tw) / 2), TY(HDR_H), HDR_TITLE, ACCENT);
  s.blit(OV_X, HDR_Y);
}

FLASHMEM static void drawUsbOverlayStatus(const char *text, uint16_t ink) {
  Surface s(OV_W, ROW_H);
  s.fillRect(0, 0, OV_W, ROW_H, PANEL);
  s.fillRect(0, 0, PX, ROW_H, ACCENT);
  s.fillRect(OV_W - PX, 0, PX, ROW_H, ACCENT);
  int tw = textWidth(text);
  s.drawText(max(SX(6), (OV_W - tw) / 2), TY(ROW_H), text, ink);
  s.blit(OV_X, STATUS_Y);
}

FLASHMEM static void drawUsbOverlayHint(const char *text = nullptr, uint16_t ink = DIM) {
  const char *lbl = text ? text : HINT;
  Surface s(OV_W, ROW_H);
  s.fillRect(0, 0, OV_W, ROW_H, PANEL);
  s.fillRect(0, 0, PX, ROW_H, ACCENT);
  s.fillRect(OV_W - PX, 0, PX, ROW_H, ACCENT);
  s.fillRect(0, ROW_H - PX, OV_W, PX, ACCENT);
  int tw = textWidth(lbl);
  s.drawText(max(SX(6), (OV_W - tw) / 2), TY(ROW_H) - (PX / 2), lbl, ink);
  s.blit(OV_X, HINT_Y);
}

#if defined(USB_SERIAL_MSC)

FLASHMEM static uint32_t cardSectors() { return SD.sdfs.card()->sectorCount(); }
FLASHMEM static int cardRead(uint32_t lba, uint8_t *dst, uint32_t n) { return SD.sdfs.card()->readSectors(lba, dst, n); }
FLASHMEM static int cardWrite(uint32_t lba, const uint8_t *src, uint32_t n) { return SD.sdfs.card()->writeSectors(lba, src, n); }
FLASHMEM static void cardSync() { SD.sdfs.card()->syncDevice(); }
PROGMEM static const usb_msc_media_t MEDIA = {cardSectors, cardRead, cardWrite, cardSync};

static IntervalTimer pollTimer;
FLASHMEM void usbDriveBegin() {
  pollTimer.priority(224);
  pollTimer.begin([] { usb_msc_poll(); }, 2000);
}

[[noreturn]] FLASHMEM static void restart(const char *why) {
#if CAPSTAN_DEBUG
  Serial.printf("storage mode: %s, restarting\n", why);
#endif
  cardSync();
  drawUsbOverlayStatus(why, COLOR_WARN);
  delay(300);
  SCB_AIRCR = 0x05FA0004;
  for (;;) {}
}

FLASHMEM static bool leave(const char *why, uint16_t ink = COLOR_WARN) {
#if CAPSTAN_DEBUG
  Serial.printf("storage mode: %s\n", why);
#endif
  cardSync();
  if (!usbDriveReload) restart(why);
  usb_msc_set_media(nullptr);
  usb_msc_clear_host_state();
  pollTimer.begin([] { usb_msc_poll(); }, 2000);
  drawUsbOverlayStatus(why, ink);
  uint32_t shownAt = millis();
  usbDriveReload();
  midiInSuspend(false);
  while (millis() - shownAt < 400 || board.keys()) {
    if (usbDriveTick) usbDriveTick();
  }
  noteUserActivity();
  inputSwallowModalKeys();
  board.tft().fillRect(OV_X, HDR_Y, OV_W, TOTAL_H, BG);
  invalidateAllPagesCache();
  uiForceRedraw();
  return true;
}

#if CAPSTAN_DEBUG
static uint32_t usbSeenCmds = 0;
static uint32_t usbStatsAt = 0;
static uint32_t usbShownStats = 0;
#endif

FLASHMEM bool usbDriveMode() {
  if (recording.mode != Recording::Idle && transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }
  midiInSuspend(true);
  flushHeaders();

  drawUsbOverlayHeader();
  drawUsbOverlayHint();

  pollTimer.end();
  bool card = cardRestart();
  usb_msc_set_media(card ? &MEDIA : nullptr);
  usb_msc_clear_host_state();
#if CAPSTAN_DEBUG
  Serial.printf("storage mode: started%s\n", card ? "" : " (no card)");
#endif
  uint32_t cardAt = millis();
  uint32_t askAt = millis();
  bool reconnected = false;

  int shown = -1;
  uint32_t battAt = 0;
  while (board.keys()) {
    if (usbDriveTick) usbDriveTick();
    usb_msc_poll();
  }
  for (;;) {
    if (usbDriveTick) usbDriveTick();
    usb_msc_poll();
#if CAPSTAN_DEBUG
    if (usb_msc_commands != usbSeenCmds) {
      usbSeenCmds = usb_msc_commands;
      if (usb_msc_last_op != 0x28 && usb_msc_last_op != 0x2A && usb_msc_last_op != 0x00)
        Serial.printf("storage mode: command %02X\n", usb_msc_last_op);
    }
    if (millis() - usbStatsAt >= 2000) {
      usbStatsAt = millis();
      uint32_t k = usb_msc_commands * 31 + usb_msc_failed * 7 + usb_msc_broken * 3 + usb_msc_restarts;
      if (k != usbShownStats) {
        usbShownStats = k;
        Serial.printf("storage mode: %lu commands, %lu reads, %lu writes; %lu failed (the last: %02X, sense %06lX), %lu broken, %lu restarts\n",
                      (unsigned long)usb_msc_commands, (unsigned long)usb_msc_reads, (unsigned long)usb_msc_writes,
                      (unsigned long)usb_msc_failed, usb_msc_failed_op, (unsigned long)usb_msc_last_sense,
                      (unsigned long)usb_msc_broken, (unsigned long)usb_msc_restarts);
      }
    }
#endif
    if (millis() - cardAt >= 500) {
      cardAt = millis();
      if (card && !cardResponds()) {
        card = false;
        usb_msc_set_media(nullptr);
#if CAPSTAN_DEBUG
        Serial.println("storage mode: the card taken out");
#endif
      } else if (!card && cardRestart()) {
        card = true;
        usb_msc_set_media(&MEDIA);
        usb_msc_clear_host_state();
        askAt = millis();
        reconnected = false;
#if CAPSTAN_DEBUG
        Serial.println("storage mode: a card put in");
#endif
      }
    }
    if (card && !reconnected && usb_configuration && !usb_msc_host_seen() && millis() - askAt >= 3000) {
      reconnected = true;
#if CAPSTAN_DEBUG
      Serial.println("storage mode: the computer isn't asking for the drive: reconnecting USB");
#endif
      USB1_USBCMD &= ~USB_USBCMD_RS;
      for (uint32_t at = millis(); millis() - at < 300;) {
        if (usbDriveTick) usbDriveTick();
      }
      USB1_USBCMD |= USB_USBCMD_RS;
      askAt = millis();
    }
    if (usb_msc_ejected()) return leave("EJECTED");
    int state = !card ? 3 : !usb_configuration ? 0 : usb_msc_host_seen() ? 2 : 1;
    if (millis() - battAt >= 40) {
      battAt = millis();
      drawBatteryIcon(false, LIGHT, PANEL);
    }
    if (state != shown) {
      shown = state;
      PROGMEM static const char *const TEXT[4] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED", "INSERT AN SD CARD"};
      uint16_t statusCol = (state == 2) ? INK : COLOR_WARN;
      drawUsbOverlayStatus(TEXT[state], statusCol);
#if CAPSTAN_DEBUG
      Serial.printf("storage mode: %s\n", TEXT[state]);
#endif
    }
    if (board.keys()) return leave("WAIT");
  }
}
#else
bool usbDriveMode() { return false; }
void usbDriveBegin() {}
#endif

}  // namespace looper

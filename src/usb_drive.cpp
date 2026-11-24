// usb_drive.cpp — Dedicated USB Mass Storage mode for mounting the M8 SD card on a computer
#include "usb_drive.h"
#include "pages.h"
#include "storage.h"
#include "midi_in.h"

#if defined(USB_SERIAL_MSC)
#include "usb_dev.h"
#include "usb_msc.h"
#endif

namespace looper {

void (*usbDriveTick)() = nullptr;
void (*usbDriveReload)() = nullptr;

PROGMEM static const char HDR_TITLE[] = "USB STORAGE MODE";
PROGMEM static const char HINT[] = "PRESS ANY KEY TO EXIT";
PROGMEM static const char ST_SWITCHING[] = "SWITCHING USB...";
PROGMEM static const char ST_CABLE[]     = "CONNECT A USB CABLE";
PROGMEM static const char ST_WAITING[]   = "WAITING FOR THE COMPUTER";
PROGMEM static const char ST_CONNECTED[] = "CONNECTED";
PROGMEM static const char ST_NOCARD[]    = "INSERT AN SD CARD";
PROGMEM static const char *const STATUS_TEXT[4] = {ST_CABLE, ST_WAITING, ST_CONNECTED, ST_NOCARD};

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

// Shares storageSharedChunkBuf() (16 KB 32B-aligned RAM1 buffer) while in USB Storage Mode.
static usb_msc_media_t mediaDesc = {cardSectors, cardRead, cardWrite, cardSync, nullptr};

[[noreturn]] FLASHMEM static void restart(const char *why) {
#if CAPSTAN_DEBUG
  Serial.printf("storage mode: %s, restarting\n", why);
#endif
  cardSync();
  drawUsbOverlayStatus(why, COLOR_WARN);
  usb_msc_set_media(nullptr);
  usb_msc_set_mode(0, usbDriveTick);
  SCB_AIRCR = 0x05FA0004;
  for (;;) {}
}

FLASHMEM static bool leave(const char *why, uint16_t ink = COLOR_WARN) {
#if CAPSTAN_DEBUG
  Serial.printf("storage mode: %s\n", why);
#endif
  cardSync();
  if (!usbDriveReload) restart(why);
  drawUsbOverlayStatus(why, ink);
  uint32_t shownAt = millis();
  usb_msc_set_media(nullptr);
  usb_msc_clear_host_state();
  // Detach Mass Storage (PID 0x04D5) and re-enumerate Normal Mode (Audio + MIDI + Serial, PID 0x048A)
  usb_msc_set_mode(0, usbDriveTick);
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
  drawBatteryIcon(true, LIGHT, PANEL);
  drawUsbOverlayStatus(ST_SWITCHING, COLOR_WARN);
  drawUsbOverlayHint();

  mediaDesc.work_buf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  bool card = cardRestart();
  usb_msc_set_media(card ? &mediaDesc : nullptr);
  usb_msc_clear_host_state();
  // Switch USB from Normal Mode (PID 0x048A) to dedicated Storage Mode (PID 0x04D5)
  usb_msc_set_mode(1, usbDriveTick);

#if CAPSTAN_DEBUG
  Serial.printf("storage mode: started%s\n", card ? "" : " (no card)");
#endif
  uint32_t cardAt = millis();
  uint32_t askAt = millis();
  bool reconnected = false;

  int shown = -1;
  uint32_t battAt = millis();
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
        usb_msc_set_media(&mediaDesc);
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
      usb_msc_set_mode(1, usbDriveTick);
      askAt = millis();
    }
    if (usb_msc_ejected()) return leave("EJECTED");
    int state = !card ? 3 : !usb_configuration ? 0 : usb_msc_host_seen() ? 2 : 1;
    if (millis() - battAt >= 1000) {
      battAt = millis();
      drawBatteryIcon(true, LIGHT, PANEL);
    }
    if (state != shown) {
      shown = state;
      uint16_t statusCol = (state == 2) ? INK : COLOR_WARN;
      drawUsbOverlayStatus(STATUS_TEXT[state], statusCol);
#if CAPSTAN_DEBUG
      Serial.printf("storage mode: %s\n", STATUS_TEXT[state]);
#endif
    }
    if (board.keys()) return leave("WAIT");
  }
}
#else
bool usbDriveMode() { return false; }
#endif

}  // namespace looper

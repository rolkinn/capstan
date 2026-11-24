/* usb_msc.h — Dynamic USB Mass Storage (Bulk-Only Transport, SCSI transparent)
 * for the Teensy 4 core, added by tools/teensy_msc/patch.py.
 *
 * Normal operation enumerates as Serial + MIDI + UAC2 Audio only (PID 0x048A,
 * no MSC interface) so host OS disk/indexing daemons never touch Capstan while
 * streaming audio. Calling usb_msc_set_mode(1, tick_fn) cleanly detaches USB and
 * re-enumerates as a dedicated Serial + Mass Storage device (PID 0x04D5) for SD
 * card access; usb_msc_set_mode(0, tick_fn) switches back on exit.
 */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MSC_CONFIG_DESC_SIZE 98

typedef struct {
	uint32_t (*sector_count)(void);                                      // 512-byte sectors
	int (*read)(uint32_t lba, uint8_t *dst, uint32_t count);              // 1: ok
	int (*write)(uint32_t lba, const uint8_t *src, uint32_t count);       // 1: ok
	void (*sync)(void);                                                  // commit to SD flash
	uint8_t *work_buf;                                                   // 16 KB 32B-aligned RAM1 staging buffer
} usb_msc_media_t;

void usb_msc_set_media(const usb_msc_media_t *media);

// Switch between Normal Mode (0: Serial + MIDI + UAC2 Audio) and Storage Mode (1: Serial + MSC).
void usb_msc_set_mode(uint8_t enable, void (*tick_fn)(void));
int usb_msc_is_active(void);

// Dynamic USB power-budget override for "USB CHARGE WHILE ON: OFF" mode:
//   0 (default) -> Bus-Powered 500 mA (bmAttributes = 0x80, bMaxPower = 250, GET_STATUS = 0x0000)
//   1           -> Self-Powered 0 mA  (bmAttributes = 0xC0, bMaxPower = 0,   GET_STATUS = 0x0001)
extern volatile uint8_t usb_self_powered_no_charge;
void usb_set_self_powered_no_charge(uint8_t enable, uint8_t reenumerate, void (*tick_fn)(void));

// Foreground protocol runner called from usbDriveMode().
void usb_msc_poll(void);

// Hooks called by the USB core (usb.c) inside usb_isr().
void usb_msc_configure(void);
void usb_msc_bus_reset(void);
void usb_msc_reset_request(void);
void usb_msc_clear_halt(uint32_t dir);

extern const uint8_t usb_msc_config_descriptor_480[];
extern const uint8_t usb_msc_config_descriptor_12[];

int usb_msc_host_seen(void);
void usb_msc_clear_host_state(void);
int usb_msc_ejected(void);

extern volatile uint8_t usb_msc_last_op;
extern volatile uint32_t usb_msc_commands;
extern volatile uint32_t usb_msc_failed, usb_msc_last_sense, usb_msc_broken, usb_msc_restarts, usb_msc_reads, usb_msc_writes;
extern volatile uint8_t usb_msc_failed_op;

#ifdef __cplusplus
}
#endif

#endif

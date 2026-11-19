/* usb_msc.c — Dynamic USB Mass Storage (Bulk-Only Transport, SCSI) for the
 * Teensy 4 core, added by tools/teensy_msc/patch.py.
 *
 * Implements:
 *   - Dedicated Storage Mode descriptors (CDC + MSC, PID 0x04D5, 500 mA bus-powered)
 *     completely isolated from Normal Mode (CDC + MIDI + UAC2 Audio, PID 0x048A).
 *   - Spec-compliant BOT §5.3.4 / §6.6.1 / §6.7.2 / §6.7.3 stall & CSW ordering:
 *     never primes a CSW on a stalled Bulk-In pipe; waits for CLEAR_FEATURE(HALT)
 *     to clear TXS, reset DATA0, and atomically prime the pending CSW inside usb_isr().
 *   - Synchronous 0xFF21 Bulk-Only Mass Storage Reset inside usb_isr().
 *   - macOS-friendly SCSI INQUIRY VPD pages (0x00, 0x80, 0x83) and MODE SENSE (10) (0x5A).
 *   - Zero duplicate 16 KB RAM1 allocation (uses caller's 16 KB RAM1 work_buf).
 */
#include "usb_desc.h"
#include "usb_dev.h"
#include "usb_serial.h"
#include "usb_midi.h"
#include "usb_msc.h"
#include "usb_audio_l7.h"
#include "core_pins.h"
#include "avr/pgmspace.h"
#include <string.h>

#ifdef MSC_INTERFACE

#ifndef LSB
#define LSB(n) ((n) & 255)
#define MSB(n) (((n) >> 8) & 255)
#endif

extern volatile uint8_t usb_high_speed;
extern volatile uint8_t usb_configuration;

PROGMEM const uint8_t usb_msc_config_descriptor_480[USB_MSC_CONFIG_DESC_SIZE] = {
	// Configuration descriptor (9 bytes): 3 interfaces (CDC 0..1, MSC 2), bus-powered 500 mA
	9, 2, LSB(USB_MSC_CONFIG_DESC_SIZE), MSB(USB_MSC_CONFIG_DESC_SIZE),
	3, 1, 0, 0x80, 250,
	// CDC Interface Association Descriptor (8 bytes)
	8, 11, CDC_STATUS_INTERFACE, 2, 0x02, 0x02, 0x01, 0,
	// CDC Control Interface 0 + Functional Descriptors + ACM Interrupt Endpoint (35 bytes)
	9, 4, CDC_STATUS_INTERFACE, 0, 1, 0x02, 0x02, 0x01, 0,
	5, 0x24, 0x00, 0x10, 0x01,
	5, 0x24, 0x01, 0x01, 1,
	4, 0x24, 0x02, 0x06,
	5, 0x24, 0x06, CDC_STATUS_INTERFACE, CDC_DATA_INTERFACE,
	7, 5, CDC_ACM_ENDPOINT | 0x80, 0x03, LSB(CDC_ACM_SIZE), MSB(CDC_ACM_SIZE), 5,
	// CDC Data Interface 1 + Bulk Endpoints (23 bytes)
	9, 4, CDC_DATA_INTERFACE, 0, 2, 0x0A, 0x00, 0x00, 0,
	7, 5, CDC_RX_ENDPOINT, 0x02, LSB(CDC_RX_SIZE_480), MSB(CDC_RX_SIZE_480), 0,
	7, 5, CDC_TX_ENDPOINT | 0x80, 0x02, LSB(CDC_TX_SIZE_480), MSB(CDC_TX_SIZE_480), 0,
	// Mass Storage Interface 2: SCSI transparent, Bulk-Only Transport (23 bytes)
	9, 4, MSC_INTERFACE, 0, 2, 0x08, 0x06, 0x50, 0,
	7, 5, MSC_TX_ENDPOINT | 0x80, 0x02, LSB(MSC_TX_SIZE_480), MSB(MSC_TX_SIZE_480), 0,
	7, 5, MSC_RX_ENDPOINT, 0x02, LSB(MSC_RX_SIZE_480), MSB(MSC_RX_SIZE_480), 0,
};

PROGMEM const uint8_t usb_msc_config_descriptor_12[USB_MSC_CONFIG_DESC_SIZE] = {
	// Configuration descriptor (9 bytes): 3 interfaces (CDC 0..1, MSC 2), bus-powered 500 mA
	9, 2, LSB(USB_MSC_CONFIG_DESC_SIZE), MSB(USB_MSC_CONFIG_DESC_SIZE),
	3, 1, 0, 0x80, 250,
	// CDC Interface Association Descriptor (8 bytes)
	8, 11, CDC_STATUS_INTERFACE, 2, 0x02, 0x02, 0x01, 0,
	// CDC Control Interface 0 + Functional Descriptors + ACM Interrupt Endpoint (35 bytes)
	9, 4, CDC_STATUS_INTERFACE, 0, 1, 0x02, 0x02, 0x01, 0,
	5, 0x24, 0x00, 0x10, 0x01,
	5, 0x24, 0x01, 0x01, 1,
	4, 0x24, 0x02, 0x06,
	5, 0x24, 0x06, CDC_STATUS_INTERFACE, CDC_DATA_INTERFACE,
	7, 5, CDC_ACM_ENDPOINT | 0x80, 0x03, LSB(CDC_ACM_SIZE), MSB(CDC_ACM_SIZE), 16,
	// CDC Data Interface 1 + Bulk Endpoints (23 bytes)
	9, 4, CDC_DATA_INTERFACE, 0, 2, 0x0A, 0x00, 0x00, 0,
	7, 5, CDC_RX_ENDPOINT, 0x02, LSB(CDC_RX_SIZE_12), MSB(CDC_RX_SIZE_12), 0,
	7, 5, CDC_TX_ENDPOINT | 0x80, 0x02, LSB(CDC_TX_SIZE_12), MSB(CDC_TX_SIZE_12), 0,
	// Mass Storage Interface 2: SCSI transparent, Bulk-Only Transport (23 bytes)
	9, 4, MSC_INTERFACE, 0, 2, 0x08, 0x06, 0x50, 0,
	7, 5, MSC_TX_ENDPOINT | 0x80, 0x02, LSB(MSC_TX_SIZE_12), MSB(MSC_TX_SIZE_12), 0,
	7, 5, MSC_RX_ENDPOINT, 0x02, LSB(MSC_RX_SIZE_12), MSB(MSC_RX_SIZE_12), 0,
};

_Static_assert(sizeof(usb_msc_config_descriptor_480) == USB_MSC_CONFIG_DESC_SIZE, "MSC HS config size mismatch");
_Static_assert(sizeof(usb_msc_config_descriptor_12) == USB_MSC_CONFIG_DESC_SIZE, "MSC FS config size mismatch");

#define SECTOR 512
#define DATA_BYTES 16384
#define RXN 2
#define TXN 4

static uint8_t cbw_buf[512] __attribute__ ((aligned(32)));
static uint8_t csw_buf[32] __attribute__ ((aligned(32)));
static uint8_t small_buf[64] __attribute__ ((aligned(32)));
static transfer_t rx_xfer[RXN] __attribute__ ((used, aligned(32)));
static transfer_t tx_xfer[TXN] __attribute__ ((used, aligned(32)));
static volatile uint8_t rx_head = 0, tx_head = 0;
static transfer_t * volatile cbw_xfer = NULL;
static transfer_t * volatile pending_csw_xfer = NULL;

static volatile uint8_t msc_active = 0;
static volatile uint8_t need_init = 1;
static volatile uint8_t reset_aborted = 0;
static volatile uint8_t cbw_queued = 0;
static volatile uint8_t cbw_invalid_wedge = 0;
static volatile uint8_t csw_pending_after_unjam = 0;

static const usb_msc_media_t * volatile media = NULL;
static volatile uint16_t pkt = 64;
static volatile uint8_t host_seen = 0, ejected = 0, attention = 0;
static volatile uint32_t n_read = 0, n_written = 0;
static uint8_t sense_key = 0, sense_asc = 0, sense_ascq = 0;

volatile uint8_t usb_msc_last_op = 0;
volatile uint32_t usb_msc_commands = 0;
volatile uint32_t usb_msc_failed = 0, usb_msc_last_sense = 0, usb_msc_broken = 0, usb_msc_restarts = 0;
volatile uint32_t usb_msc_reads = 0, usb_msc_writes = 0;
volatile uint8_t usb_msc_failed_op = 0;

static inline void disable_usb_irq(void)
{
	NVIC_DISABLE_IRQ(IRQ_USB1);
	__asm__ volatile("dsb\n\tisb" ::: "memory");
}

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static inline void stall_in(void)  { *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) |= USB_ENDPTCTRL_TXS; }
static inline void stall_out(void) { *((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_RX_ENDPOINT) |= USB_ENDPTCTRL_RXS; }

static inline int ep5_halted(void)
{
	return (*((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) & (USB_ENDPTCTRL_TXS | USB_ENDPTCTRL_RXS)) != 0;
}

int usb_msc_is_active(void)
{
	return msc_active;
}

FLASHMEM void usb_msc_set_media(const usb_msc_media_t *m)
{
	media = m;
	attention = (m != NULL);
}

FLASHMEM void usb_msc_clear_host_state(void)
{
	host_seen = 0;
	ejected = 0;
	n_read = 0;
	n_written = 0;
}

FLASHMEM int usb_msc_host_seen(void) { return host_seen; }
FLASHMEM int usb_msc_ejected(void) { return ejected; }
FLASHMEM uint32_t usb_msc_sectors_read(void) { return n_read; }
FLASHMEM uint32_t usb_msc_sectors_written(void) { return n_written; }

FLASHMEM static void prime_cbw_locked(void)
{
	if (cbw_queued || cbw_invalid_wedge || csw_pending_after_unjam) return;
	if (*((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_RX_ENDPOINT) & USB_ENDPTCTRL_RXS) return;
	transfer_t *x = &rx_xfer[rx_head];
	rx_head = (rx_head + 1) % RXN;
	cbw_xfer = x;
	arm_dcache_delete(cbw_buf, sizeof cbw_buf);
	usb_prepare_transfer(x, cbw_buf, sizeof cbw_buf, 0);
	usb_receive(MSC_RX_ENDPOINT, x);
	cbw_queued = 1;
}

FLASHMEM static void queue_cbw(void)
{
	disable_usb_irq();
	if (usb_configuration && msc_active && !need_init) {
		prime_cbw_locked();
	}
	NVIC_ENABLE_IRQ(IRQ_USB1);
}

// Called inside usb_isr() on USB Bus Reset (URI) or SET_CONFIGURATION(0).
FLASHMEM void usb_msc_bus_reset(void)
{
	reset_aborted = 1;
	need_init = 1;
	cbw_queued = 0;
	cbw_invalid_wedge = 0;
	csw_pending_after_unjam = 0;
	pending_csw_xfer = NULL;
}

// Called inside usb_isr() on SET_CONFIGURATION(1) when msc_active != 0.
FLASHMEM void usb_msc_configure(void)
{
	pkt = usb_high_speed ? MSC_RX_SIZE_480 : MSC_RX_SIZE_12;
	usb_config_tx(MSC_TX_ENDPOINT, pkt, 0, NULL);
	usb_config_rx(MSC_RX_ENDPOINT, pkt, 0, NULL);
	memset(rx_xfer, 0, sizeof rx_xfer);
	memset(tx_xfer, 0, sizeof tx_xfer);
	rx_head = 0;
	tx_head = 0;
	cbw_queued = 0;
	cbw_invalid_wedge = 0;
	csw_pending_after_unjam = 0;
	pending_csw_xfer = NULL;
	attention = 0;
	reset_aborted = 1;
	need_init = 0;
	prime_cbw_locked();
}

// Called inside usb_isr() on 0xFF21 (Bulk-Only Mass Storage Reset, BOT §5.3.4).
// Synchronously flushes EP5, clears wedge/pending CSW state, and primes CBW RX
// before EP0 ACKs so an immediate next CBW from the host is never missed.
FLASHMEM void usb_msc_reset_request(void)
{
	uint32_t mask = (1u << MSC_RX_ENDPOINT) | (1u << (MSC_TX_ENDPOINT + 16));
	do {
		USB1_ENDPTFLUSH = mask;
		while (USB1_ENDPTFLUSH & mask) {}
	} while (USB1_ENDPTSTATUS & mask);
	USB1_ENDPTCOMPLETE = mask;
	volatile uint32_t *ctrl = (volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT;
	*ctrl = (*ctrl & ~(USB_ENDPTCTRL_RXS | USB_ENDPTCTRL_TXS)) | USB_ENDPTCTRL_RXR | USB_ENDPTCTRL_TXR;
	pkt = usb_high_speed ? MSC_RX_SIZE_480 : MSC_RX_SIZE_12;
	usb_config_tx(MSC_TX_ENDPOINT, pkt, 0, NULL);
	usb_config_rx(MSC_RX_ENDPOINT, pkt, 0, NULL);
	memset(rx_xfer, 0, sizeof rx_xfer);
	memset(tx_xfer, 0, sizeof tx_xfer);
	rx_head = 0;
	tx_head = 0;
	cbw_queued = 0;
	cbw_invalid_wedge = 0;
	csw_pending_after_unjam = 0;
	pending_csw_xfer = NULL;
	reset_aborted = 1;
	need_init = 0;
	usb_msc_restarts++;
	prime_cbw_locked();
}

// Called inside usb_isr() on CLEAR_FEATURE(ENDPOINT_HALT) for EP5.
FLASHMEM void usb_msc_clear_halt(uint32_t dir)
{
	volatile uint32_t *ctrl = (volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT;
	if (cbw_invalid_wedge) {
		// BOT §6.6.1: invalid CBW keeps Bulk-In and Bulk-Out wedged until Reset Recovery
		*ctrl |= (dir ? USB_ENDPTCTRL_TXS : USB_ENDPTCTRL_RXS);
		return;
	}
	if (dir) {
		*ctrl = (*ctrl & ~USB_ENDPTCTRL_TXS) | USB_ENDPTCTRL_TXR;
		if (csw_pending_after_unjam && pending_csw_xfer != NULL) {
			transfer_t *x = pending_csw_xfer;
			pending_csw_xfer = NULL;
			csw_pending_after_unjam = 0;
			usb_transmit(MSC_TX_ENDPOINT, x);
		}
	} else {
		*ctrl = (*ctrl & ~USB_ENDPTCTRL_RXS) | USB_ENDPTCTRL_RXR;
	}
}

volatile uint8_t usb_self_powered_no_charge = 0;

FLASHMEM static void usb_soft_detach_and_reset(void)
{
	USB1_USBCMD &= ~USB_USBCMD_RS;
	usb_configuration = 0;
	USB1_ENDPTSETUPSTAT = USB1_ENDPTSETUPSTAT;
	USB1_ENDPTCOMPLETE = USB1_ENDPTCOMPLETE;
	while (USB1_ENDPTPRIME != 0) {}
	USB1_ENDPTFLUSH = 0xFFFFFFFFu;
	while (USB1_ENDPTFLUSH != 0) {}
	const uint32_t unused_ep = ENDPOINT_TRANSMIT_UNUSED + ENDPOINT_RECEIVE_UNUSED;
	USB1_ENDPTCTRL2 = unused_ep;
	USB1_ENDPTCTRL3 = unused_ep;
	USB1_ENDPTCTRL4 = unused_ep;
	USB1_ENDPTCTRL5 = unused_ep;
	USB1_ENDPTCTRL6 = unused_ep;
	USB1_ENDPTCTRL7 = unused_ep;
	usb_msc_bus_reset();
#if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
	usb_serial_reset();
#endif
#if defined(MIDI_INTERFACE)
	usb_midi_reset();
#endif
#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)
	usb_audio_reset();
#endif
}

FLASHMEM static void usb_soft_reattach_after_delay(void (*tick_fn)(void))
{
	uint32_t t0 = millis();
	while (millis() - t0 < 350u) {
		if (tick_fn) tick_fn();
	}

	disable_usb_irq();
	USB1_USBSTS = USB_USBSTS_URI | USB_USBSTS_SLI | USB_USBSTS_PCI;
	USB1_USBCMD |= USB_USBCMD_RS;
	NVIC_ENABLE_IRQ(IRQ_USB1);
}

FLASHMEM void usb_msc_set_mode(uint8_t enable, void (*tick_fn)(void))
{
	enable = enable ? 1 : 0;
	if (!enable && !msc_active) return;

	disable_usb_irq();
	usb_soft_detach_and_reset();
	msc_active = enable;
	NVIC_ENABLE_IRQ(IRQ_USB1);

	usb_soft_reattach_after_delay(tick_fn);
}

FLASHMEM void usb_set_self_powered_no_charge(uint8_t enable, uint8_t reenumerate, void (*tick_fn)(void))
{
	enable = enable ? 1 : 0;
	if (!reenumerate) {
		usb_self_powered_no_charge = enable;
		return;
	}

	disable_usb_irq();
	usb_soft_detach_and_reset();
	usb_self_powered_no_charge = enable;
	NVIC_ENABLE_IRQ(IRQ_USB1);

	usb_soft_reattach_after_delay(tick_fn);
}

FLASHMEM static void init_protocol(void)
{
	disable_usb_irq();
	if (!usb_configuration || !msc_active) {
		NVIC_ENABLE_IRQ(IRQ_USB1);
		return;
	}
	usb_msc_reset_request();
	reset_aborted = 0;
	NVIC_ENABLE_IRQ(IRQ_USB1);
	host_seen = 0;
	ejected = 0;
	sense_key = sense_asc = sense_ascq = 0;
}

FLASHMEM static uint32_t wait_done(transfer_t *x, uint32_t timeout_ms)
{
	uint32_t t0 = millis();
	while (1) {
		if (!usb_configuration || !msc_active || reset_aborted || need_init) return 0x40;
		uint32_t st = usb_transfer_status(x);
		if (!(st & 0x80u)) return st;
		if (millis() - t0 > timeout_ms) return 0x40;
	}
}

FLASHMEM static int send_in(const void *buf, uint32_t len)
{
	if (!usb_configuration || !msc_active || reset_aborted || need_init) return 0;
	transfer_t *x = &tx_xfer[tx_head];
	tx_head = (tx_head + 1) % TXN;
	disable_usb_irq();
	if (!usb_configuration || !msc_active || reset_aborted || need_init) {
		NVIC_ENABLE_IRQ(IRQ_USB1);
		return 0;
	}
	if (len) arm_dcache_flush_delete((void *)buf, (len + 31u) & ~31u);
	usb_prepare_transfer(x, buf, len, 0);
	usb_transmit(MSC_TX_ENDPOINT, x);
	NVIC_ENABLE_IRQ(IRQ_USB1);
	return !(wait_done(x, 8000) & 0x68u);
}

FLASHMEM static int recv_out(void *buf, uint32_t len, uint32_t *got)
{
	if (!usb_configuration || !msc_active || reset_aborted || need_init) return 0;
	transfer_t *x = &rx_xfer[rx_head];
	rx_head = (rx_head + 1) % RXN;
	disable_usb_irq();
	if (!usb_configuration || !msc_active || reset_aborted || need_init) {
		NVIC_ENABLE_IRQ(IRQ_USB1);
		return 0;
	}
	arm_dcache_delete(buf, (len + 31u) & ~31u);
	usb_prepare_transfer(x, buf, len, 0);
	usb_receive(MSC_RX_ENDPOINT, x);
	NVIC_ENABLE_IRQ(IRQ_USB1);
	uint32_t st = wait_done(x, 8000);
	if (st & 0x68u) return 0;
	*got = len - ((st >> 16) & 0x7FFFu);
	return 1;
}

typedef struct {
	uint32_t tag, dlen, residue;
	uint8_t in, status;
} cmd_t;

FLASHMEM static void fail(cmd_t *c, uint8_t key, uint8_t asc, uint8_t ascq)
{
	sense_key = key;
	sense_asc = asc;
	sense_ascq = ascq;
	c->status = 1;
	if (c->dlen) {
		if (c->in) stall_in(); else stall_out();
	}
}

FLASHMEM static void data_in(cmd_t *c, const void *buf, uint32_t len)
{
	if (len > c->dlen) len = c->dlen;
	if (!len) {
		if (c->dlen) stall_in();
		c->residue = c->dlen;
		return;
	}
	if (!send_in(buf, len)) { c->status = 2; return; }
	if (len < c->dlen && (len % pkt) == 0) {
		if (!send_in(buf, 0)) { c->status = 2; return; }
	}
	c->residue = c->dlen - len;
}

FLASHMEM static uint32_t sector_count(void)
{
	const usb_msc_media_t *m = media;
	return m ? m->sector_count() : 0;
}

FLASHMEM static void write_hex8(uint8_t *dst, uint32_t val)
{
	for (int i = 7; i >= 0; i--) {
		uint8_t nib = val & 0x0Fu;
		dst[i] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
		val >>= 4;
	}
}

FLASHMEM static void do_read(cmd_t *c, uint32_t lba, uint32_t count)
{
	const usb_msc_media_t *m = media;
	if (!m || !m->work_buf) { fail(c, 2, 0x3A, 0); return; }
	if ((uint64_t)lba + count > m->sector_count()) { fail(c, 5, 0x21, 0); return; }
	uint8_t *const data_buf = m->work_buf;
	uint32_t sent = 0;
	while (count) {
		uint32_t n = count > DATA_BYTES / SECTOR ? DATA_BYTES / SECTOR : count;
		if (!m->read(lba, data_buf, n)) {
			if (reset_aborted || need_init || !usb_configuration) { c->status = 2; return; }
			sense_key = 3; sense_asc = 0x11; sense_ascq = 0;
			c->status = 1;
			stall_in();
			c->residue = c->dlen > sent ? c->dlen - sent : 0;
			return;
		}
		if (reset_aborted || need_init || !usb_configuration) { c->status = 2; return; }
		if (!send_in(data_buf, n * SECTOR)) { c->status = 2; return; }
		sent += n * SECTOR;
		n_read += n;
		lba += n;
		count -= n;
	}
	if (sent < c->dlen && (sent % pkt) == 0) {
		if (!send_in(data_buf, 0)) { c->status = 2; return; }
	}
	c->residue = c->dlen > sent ? c->dlen - sent : 0;
}

FLASHMEM static void do_write(cmd_t *c, uint32_t lba, uint32_t count)
{
	const usb_msc_media_t *m = media;
	if (!m || !m->work_buf) { fail(c, 2, 0x3A, 0); return; }
	if ((uint64_t)lba + count > m->sector_count()) { fail(c, 5, 0x21, 0); return; }
	uint8_t *const data_buf = m->work_buf;
	uint32_t got_total = 0;
	while (count) {
		uint32_t n = count > DATA_BYTES / SECTOR ? DATA_BYTES / SECTOR : count, got = 0;
		if (!recv_out(data_buf, n * SECTOR, &got)) { c->status = 2; return; }
		if (got != n * SECTOR) { c->status = 2; return; }
		if (!m->write(lba, data_buf, n)) {
			if (reset_aborted || need_init || !usb_configuration) { c->status = 2; return; }
			sense_key = 3; sense_asc = 0x0C; sense_ascq = 0;
			c->status = 1;
			if ((got_total + got) < c->dlen) stall_out();
			c->residue = c->dlen > got_total ? c->dlen - got_total : 0;
			return;
		}
		if (reset_aborted || need_init || !usb_configuration) { c->status = 2; return; }
		got_total += got;
		n_written += n;
		lba += n;
		count -= n;
	}
	c->residue = c->dlen > got_total ? c->dlen - got_total : 0;
}

FLASHMEM static void handle_command(cmd_t *c, const uint8_t *cb)
{
	const usb_msc_media_t *m = media;
	if (cb[0] != 0x12 && cb[0] != 0x03 && cb[0] != 0x1E) {
		if (!m) { fail(c, 2, 0x3A, 0); return; }
		if (attention) {
			attention = 0;
			if (cb[0] == 0x00) { fail(c, 6, 0x28, 0); return; }
		}
	}
	switch (cb[0]) {
	case 0x00: // TEST UNIT READY
		break;
	case 0x03: { // REQUEST SENSE
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
	case 0x12: { // INQUIRY (standard + VPD pages 0x00, 0x80, 0x83 for macOS)
		uint32_t alloc = ((uint32_t)cb[3] << 8) | cb[4];
		if (cb[1] & 1) {
			memset(small_buf, 0, 32);
			if (cb[2] == 0x00) {
				// Supported VPD Pages
				small_buf[3] = 3;
				small_buf[4] = 0x00;
				small_buf[5] = 0x80;
				small_buf[6] = 0x83;
				data_in(c, small_buf, alloc < 7 ? alloc : 7);
			} else if (cb[2] == 0x80) {
				// Unit Serial Number
				small_buf[1] = 0x80;
				small_buf[3] = 8;
				write_hex8(small_buf + 4, HW_OCOTP_MAC0);
				data_in(c, small_buf, alloc < 12 ? alloc : 12);
			} else if (cb[2] == 0x83) {
				// Device Identification (T10 Vendor ID)
				small_buf[1] = 0x83;
				small_buf[3] = 20;
				small_buf[4] = 0x02;
				small_buf[5] = 0x01;
				small_buf[7] = 16;
				memcpy(small_buf + 8, "Capstan ", 8);
				write_hex8(small_buf + 16, HW_OCOTP_MAC0);
				data_in(c, small_buf, alloc < 24 ? alloc : 24);
			} else {
				fail(c, 5, 0x24, 0);
			}
			break;
		}
		if (cb[2] != 0) { fail(c, 5, 0x24, 0); break; }
		memset(small_buf, 0, 36);
		small_buf[1] = 0x80;
		small_buf[2] = 0x04;
		small_buf[3] = 0x02;
		small_buf[4] = 31;
		memcpy(small_buf + 8, "Capstan ", 8);
		memcpy(small_buf + 16, "SD CARD         ", 16);
		memcpy(small_buf + 32, "1.0 ", 4);
		data_in(c, small_buf, alloc < 36 ? alloc : 36);
		break;
	}
	case 0x1A: // MODE SENSE (6)
		memset(small_buf, 0, 4);
		small_buf[0] = 3;
		data_in(c, small_buf, cb[4] < 4 ? cb[4] : 4);
		break;
	case 0x5A: { // MODE SENSE (10)
		uint32_t alloc = ((uint32_t)cb[7] << 8) | cb[8];
		memset(small_buf, 0, 8);
		small_buf[1] = 6;
		data_in(c, small_buf, alloc < 8 ? alloc : 8);
		break;
	}
	case 0x1B: // START STOP UNIT (eject)
		if ((cb[4] & 2) && !(cb[4] & 1)) {
			if (m && m->sync) m->sync();
			ejected = 1;
		}
		break;
	case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
		break;
	case 0x23: { // READ FORMAT CAPACITIES
		uint32_t alloc = ((uint32_t)cb[7] << 8) | cb[8];
		memset(small_buf, 0, 12);
		small_buf[3] = 8;
		put_be32(small_buf + 4, sector_count());
		small_buf[8] = m ? 2 : 3;
		small_buf[10] = SECTOR >> 8;
		data_in(c, small_buf, alloc < 12 ? alloc : 12);
		break;
	}
	case 0x25: { // READ CAPACITY (10)
		uint32_t sc = sector_count();
		if (sc == 0) { fail(c, 2, 0x3A, 0); break; }
		put_be32(small_buf, sc - 1u);
		put_be32(small_buf + 4, SECTOR);
		data_in(c, small_buf, 8);
		break;
	}
	case 0x28: // READ (10)
		do_read(c, be32(cb + 2), ((uint32_t)cb[7] << 8) | cb[8]);
		break;
	case 0x2A: // WRITE (10)
		do_write(c, be32(cb + 2), ((uint32_t)cb[7] << 8) | cb[8]);
		break;
	case 0x2F: // VERIFY (10)
		c->residue = 0;
		break;
	case 0x35: // SYNCHRONIZE CACHE (10)
		if (m && m->sync) m->sync();
		break;
	default:
		fail(c, 5, 0x20, 0);
		break;
	}
}

FLASHMEM static void run_command(void)
{
	uint32_t st = usb_transfer_status(cbw_xfer);
	cbw_queued = 0;
	reset_aborted = 0;
	uint32_t got = sizeof cbw_buf - ((st >> 16) & 0x7FFFu);
	host_seen = 1;
	if ((st & 0x68u) || got != 31 || le32(cbw_buf) != 0x43425355u) {
		// BOT §6.6.1: invalid CBW wedges both Bulk-In and Bulk-Out until 0xFF21 Reset Recovery
		disable_usb_irq();
		cbw_invalid_wedge = 1;
		stall_in();
		stall_out();
		NVIC_ENABLE_IRQ(IRQ_USB1);
		usb_msc_broken++;
		return;
	}
	cmd_t c;
	c.tag = le32(cbw_buf + 4);
	c.dlen = le32(cbw_buf + 8);
	c.in = cbw_buf[12] & 0x80u;
	c.residue = (c.dlen == 0) ? 0 : c.dlen;
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
	if (c.status == 2) {
		usb_msc_broken++;
		return;
	}
	if (!usb_configuration || !msc_active || reset_aborted || need_init) return;

	put_le32(csw_buf, 0x53425355u);
	put_le32(csw_buf + 4, c.tag);
	put_le32(csw_buf + 8, c.residue);
	csw_buf[12] = c.status;

	transfer_t *x = &tx_xfer[tx_head];
	tx_head = (tx_head + 1) % TXN;
	disable_usb_irq();
	if (!usb_configuration || !msc_active || reset_aborted || need_init) {
		NVIC_ENABLE_IRQ(IRQ_USB1);
		return;
	}
	arm_dcache_flush_delete(csw_buf, 32);
	usb_prepare_transfer(x, csw_buf, 13, 0);
	if (*((volatile uint32_t *)&USB1_ENDPTCTRL0 + MSC_TX_ENDPOINT) & USB_ENDPTCTRL_TXS) {
		pending_csw_xfer = x;
		csw_pending_after_unjam = 1;
		NVIC_ENABLE_IRQ(IRQ_USB1);
	} else {
		pending_csw_xfer = NULL;
		csw_pending_after_unjam = 0;
		usb_transmit(MSC_TX_ENDPOINT, x);
		NVIC_ENABLE_IRQ(IRQ_USB1);
	}

	uint32_t t0 = millis();
	while (1) {
		if (!usb_configuration || !msc_active || reset_aborted || need_init) return;
		if (!csw_pending_after_unjam && !(usb_transfer_status(x) & 0x80u)) break;
		if (millis() - t0 > 8000u) { need_init = 1; return; }
	}
	if (!ep5_halted() && !cbw_queued) {
		queue_cbw();
	}
}

FLASHMEM void usb_msc_poll(void)
{
	if (!msc_active || !usb_configuration) {
		cbw_queued = 0;
		return;
	}
	if (need_init) {
		init_protocol();
		return;
	}
	if (cbw_invalid_wedge || csw_pending_after_unjam || ep5_halted()) return;
	if (!cbw_queued) {
		queue_cbw();
		if (!cbw_queued) return;
	}
	if (usb_transfer_status(cbw_xfer) & 0x80u) return;
	run_command();
}

#endif // MSC_INTERFACE

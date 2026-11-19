// usb_audio_l7.cpp — Capstan's USB Audio Class 2.0 (UAC2) implementation (see usb_audio_l7.h).
// tools/teensy_msc/patch.py copies this file into the Teensy 4 core (cores/teensy4).
// Include usb_dev.h / usb_desc.h before <Arduino.h> so GCC's -MMD dependency
// file tracks usb_desc.h rather than suppressing it behind Arduino.h.gch.
#include "usb_dev.h"
#include "usb_desc.h"
#include "usb_audio.h"
#include "usb_audio_l7.h"
#include <Arduino.h>
#include "AudioStream.h"
#include <string.h>

#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)

extern volatile uint8_t usb_high_speed;
extern volatile uint8_t usb_configuration;

volatile uint8_t usb_audio_receive_setting = 0;
volatile uint8_t usb_audio_transmit_setting = 0;

#define ROUND32(n) (((n) + 31u) & ~31u)

// Multi-buffered Isochronous DMA descriptor rings:
//   - 8 queued dTDs at High-Speed (8 * 125 us = 1.0 ms hardware DMA cushion)
//   - 2 queued dTDs at Full-Speed (2 * 1.0 ms = 2.0 ms hardware DMA cushion)
//   - 4 queued dTDs on the 1 ms Explicit Feedback IN endpoint (EP6 IN = 4.0 ms cushion)
// Packet DMA buffers (tx_packet, rx_packet, sync_value = 800 B total) reside in uncached
// RAM1 (DTCM) alongside endpoint_queue_head and transfer_t descriptors, eliminating 17,000/s
// D-cache flush/invalidate operations and OCRAM bus contention during SD/DSP activity.
static constexpr uint32_t ISO_N = 8;
static constexpr uint32_t ISO_FS_N = 2;
static constexpr uint32_t SYNC_N = 4;
static constexpr uint8_t USB_AUDIO_IRQ_PRIORITY = 64; // strictly above SDIO (96), DMA (128), Wire (144), Audio (208), MIDI (224)

static constexpr uint32_t TX_PKT_HS_STRIDE = ROUND32(AUDIO_TX_SIZE_480); // 32 B
static constexpr uint32_t TX_PKT_FS_STRIDE = ROUND32(AUDIO_TX_SIZE_12);  // 192 B
static constexpr uint32_t RX_PKT_HS_STRIDE = ROUND32(AUDIO_RX_SIZE_480); // 32 B
static constexpr uint32_t RX_PKT_FS_STRIDE = ROUND32(AUDIO_RX_SIZE_12);  // 192 B

union TxIsoPool {
	uint8_t hs[ISO_N][TX_PKT_HS_STRIDE];
	uint8_t fs[ISO_FS_N][TX_PKT_FS_STRIDE];
};
union RxIsoPool {
	uint8_t hs[ISO_N][RX_PKT_HS_STRIDE];
	uint8_t fs[ISO_FS_N][RX_PKT_FS_STRIDE];
};
static_assert((TX_PKT_HS_STRIDE % 32u) == 0 && (TX_PKT_FS_STRIDE % 32u) == 0, "TX slot must be 32B aligned");
static_assert((RX_PKT_HS_STRIDE % 32u) == 0 && (RX_PKT_FS_STRIDE % 32u) == 0, "RX slot must be 32B aligned");

static transfer_t tx_transfer[ISO_N] __attribute__ ((used, aligned(32)));
static transfer_t rx_transfer[ISO_N] __attribute__ ((used, aligned(32)));
static transfer_t sync_transfer[SYNC_N] __attribute__ ((used, aligned(32)));
static TxIsoPool tx_packet __attribute__ ((aligned(32)));
static RxIsoPool rx_packet __attribute__ ((aligned(32)));
static uint32_t sync_value[SYNC_N] __attribute__ ((aligned(32)));

// Hardware ARM_DWT_CYCCNT vs. USB1_FRINDEX clock domain estimator + ultra-gentle centering trim:
//   - Both ARM_DWT_CYCCNT (PLL1 = F_CPU_ACTUAL) and I2S1 (PLL4 = exact 44,100.000 Hz) derive
//     from the M8's 24 MHz crystal, so measuring elapsed CPU cycles per 8,000 USB microframes
//     inside usb_isr() gives exact M8 I2S samples per USB host second (< 0.02 Hz = 0.4 ppm)
//     with zero sensitivity to LooperEngine::update() DSP load.
//   - Both EP7 IN packet pacing (tx_rate) and EP6 IN explicit feedback (rx_rate) track
//     usb_host_rate plus an ultra-gentle (+/- 0.30 Hz = +/- 6.8 ppm max) heavily smoothed
//     centering trim so macOS coreaudiod sees one rock-steady AUDIO_CLOCK_ID = 9 domain.
static constexpr float HOST_RATE_MAX_DEV = 4.5f;       // +/- 4.5 Hz (~102 ppm quartz crystal ceiling)
static constexpr float RATE_KP = 0.001f;
static constexpr float RATE_KI_BLOCK = 0.000002f;
static constexpr float RATE_TRIM_MAX = 0.30f;          // +/- 0.30 Hz (~6.8 ppm fine ring centering)
static constexpr float FILL_SMOOTH = 1.0f / 64.0f;     // ~186 ms block/hub jitter filter
static constexpr uint32_t SOF_MIN_STEP_UFRAMES = 64u;  // 8.0 ms checkpoint (immune to Bulk MIDI & hub uframe jitter)

static volatile float usb_host_rate = AUDIO_SAMPLE_RATE_EXACT;
static bool sof_tracking = false;
static bool sof_rate_locked = false;
static uint32_t sof_prev_uframe = 0;
static uint32_t sof_prev_cyccnt = 0;
static uint32_t sof_accum_uframes = 0;
static uint32_t sof_accum_cycles = 0;
static volatile uint32_t sof_last_isr_cyccnt = 0;

// ---- To the computer (EP7 IN): 768-frame stereo ring (17.4 ms), centered at 384 frames.
static constexpr uint32_t TX_RING = 6 * AUDIO_BLOCK_SAMPLES;                    // 768 frames (3,072 B)
static constexpr uint32_t TX_MARK = 3 * AUDIO_BLOCK_SAMPLES;                    // 384 frames
static constexpr uint32_t TX_RECENTER_HIGH = TX_RING - 32;                      // 736 frames
static constexpr uint8_t TX_STARVE_MAX_UFRAMES = 64;                            // 8 ms before dropping stream lock
DMAMEM static int16_t tx_ring[TX_RING * AUDIO_TX_CHANNELS_480] __attribute__ ((aligned(32)));
static volatile uint32_t tx_head = 0;      // frames written (LooperEngine::update only)
static volatile uint32_t tx_tail = 0;      // frames sent (USB ISR only)
static volatile bool tx_started = false;   // USB ISR only
static volatile float tx_rate = AUDIO_SAMPLE_RATE_EXACT;
static float tx_acc = 0.0f;                // fractional frames owed (USB ISR only)
static int16_t tx_last_l = 0, tx_last_r = 0;
static uint8_t tx_starve_uframes = 0;
static float tx_fill = (float)TX_MARK;     // smoothed fill (LooperEngine::update only)
static float tx_integ = 0.0f;              // PI integral trim in Hz (LooperEngine::update only)
static bool tx_was_started = false;        // LooperEngine::update only
static volatile uint32_t tx_underruns = 0, tx_dropped = 0;

// ---- From the computer (EP6 OUT + EP6 IN explicit feedback): 1,024-frame stereo ring (23.2 ms),
// centered at 512 frames before a 128-frame block read.
// Inside [RX_SLIP_LOW, RX_SLIP_HIGH] ([256, 768]), playback is bit-exact 1:1 PCM (128 -> 128).
// Outside that window (e.g. if a host ignores explicit feedback when slaved to another audio
// device without host Drift Correction), blocks smoothly consume 127 or 129 frames into 128
// linearly-interpolated frames with phase-continuous endpoints.
static constexpr uint32_t RX_RING = 8 * AUDIO_BLOCK_SAMPLES;                    // 1,024 frames (4,096 B)
static constexpr uint32_t RX_MARK = 4 * AUDIO_BLOCK_SAMPLES;                    // 512 frames
static constexpr uint32_t RX_SLIP_LOW = 2 * AUDIO_BLOCK_SAMPLES;                // 256 frames
static constexpr uint32_t RX_SLIP_HIGH = 6 * AUDIO_BLOCK_SAMPLES;               // 768 frames
static constexpr uint32_t RX_RECENTER_HIGH = RX_RING - 32;                      // 992 frames
DMAMEM static uint32_t rx_ring[RX_RING] __attribute__ ((aligned(32)));
static volatile uint32_t rx_head = 0;      // frames received (USB ISR only)
static volatile uint32_t rx_tail = 0;      // frames read (LooperEngine::update only)
static volatile uint32_t rx_epoch = 0;     // incremented by USB ISR on reset / SET_INTERFACE
static uint32_t rx_seen_epoch = 0;         // LooperEngine::update only
static bool rx_started = false;            // LooperEngine::update only
static uint32_t rx_prev_frame = 0;         // last output stereo frame for phase-continuous slip
static volatile float rx_rate = AUDIO_SAMPLE_RATE_EXACT;
static float rx_fill = (float)RX_MARK;     // LooperEngine::update only
static float rx_integ = 0.0f;              // PI integral trim in Hz (LooperEngine::update only)
static volatile uint32_t rx_underruns = 0, rx_overruns = 0;

// Must be called with IRQ_USB1 disabled (or from inside usb_isr).
static void sample_usb_sof_clock(uint32_t uframe, uint32_t cyc)
{
	if (!usb_configuration) {
		sof_tracking = false;
		sof_rate_locked = false;
		sof_accum_uframes = 0;
		sof_accum_cycles = 0;
		usb_host_rate = AUDIO_SAMPLE_RATE_EXACT;
		return;
	}
	if (!sof_tracking) {
		sof_tracking = true;
		sof_prev_uframe = uframe;
		sof_prev_cyccnt = cyc;
		sof_accum_uframes = 0;
		sof_accum_cycles = 0;
		return;
	}
	const uint32_t duf = (uframe - sof_prev_uframe) & 0x3FFFu;
	if (duf < SOF_MIN_STEP_UFRAMES) return;
	const uint32_t dcyc = cyc - sof_prev_cyccnt;
	sof_prev_uframe = uframe;
	sof_prev_cyccnt = cyc;

	const uint32_t cpu_hz = F_CPU_ACTUAL ? F_CPU_ACTUAL : 600000000u;
	if (duf > 1024u) {
		sof_accum_uframes = 0;
		sof_accum_cycles = 0;
		return;
	}
	const uint32_t expected_cyc = duf * (cpu_hz / 8000u);
	// Reject bus suspend / hub reset gaps (> +/- 25% cycle anomaly over >= 8 ms checkpoint)
	if (dcyc < ((expected_cyc * 3u) >> 2) || dcyc > ((expected_cyc * 5u) >> 2)) {
		sof_accum_uframes = 0;
		sof_accum_cycles = 0;
		return;
	}

	sof_accum_uframes += duf;
	sof_accum_cycles += dcyc;
	const uint32_t win_uframes = sof_rate_locked ? 8000u : 2000u; // 0.25 s initial lock, 1.00 s steady-state
	if (sof_accum_uframes >= win_uframes) {
		const double num = (double)sof_accum_cycles * ((double)AUDIO_SAMPLE_RATE_EXACT * 8000.0);
		const double den = (double)sof_accum_uframes * (double)cpu_hz;
		const float raw = (float)(num / den);
		const float clamped = constrain(raw,
		                                AUDIO_SAMPLE_RATE_EXACT - HOST_RATE_MAX_DEV,
		                                AUDIO_SAMPLE_RATE_EXACT + HOST_RATE_MAX_DEV);
		if (!sof_rate_locked) {
			usb_host_rate = clamped;
			sof_rate_locked = true;
		} else {
			usb_host_rate = usb_host_rate + (clamped - usb_host_rate) * 0.25f;
		}
		sof_accum_uframes = 0;
		sof_accum_cycles = 0;
	}
}

static inline void sample_usb_sof_from_isr(void)
{
	const uint32_t uframe = usb_high_speed ? (USB1_FRINDEX & 0x3FFFu) : (USB1_FRINDEX & 0x3FF8u);
	const uint32_t cyc = ARM_DWT_CYCCNT;
	sof_last_isr_cyccnt = cyc;
	if (!sof_tracking || !usb_configuration ||
	    ((uframe - sof_prev_uframe) & 0x3FFFu) >= SOF_MIN_STEP_UFRAMES) {
		sample_usb_sof_clock(uframe, cyc);
	}
}

FLASHMEM __attribute__((noinline)) static void poll_usb_sof_idle(void)
{
	if (!usb_configuration) {
		NVIC_DISABLE_IRQ(IRQ_USB1);
		__asm__ volatile("dsb\n\tisb" ::: "memory");
		sample_usb_sof_clock(0, ARM_DWT_CYCCNT);
		NVIC_ENABLE_IRQ(IRQ_USB1);
		return;
	}
	NVIC_DISABLE_IRQ(IRQ_USB1);
	__asm__ volatile("dsb\n\tisb" ::: "memory");
	uint32_t primask;
	__asm__ volatile("mrs %0, PRIMASK\n\tcpsid i" : "=r"(primask) :: "memory");
	const uint32_t uframe = usb_high_speed ? (USB1_FRINDEX & 0x3FFFu) : (USB1_FRINDEX & 0x3FF8u);
	const uint32_t cyc = ARM_DWT_CYCCNT;
	if (!(primask & 1u)) __enable_irq();
	sample_usb_sof_clock(uframe, cyc);
	NVIC_ENABLE_IRQ(IRQ_USB1);
}

static inline void update_usb_host_rate_idle(void)
{
	// When ISO endpoints are active (Alt 1), usb_isr() samples USB1_FRINDEX directly on
	// microframe completion edges; only call the FLASHMEM idle poller when Alt 0 is idle.
	if (!usb_configuration || (uint32_t)(ARM_DWT_CYCCNT - sof_last_isr_cyccnt) >= (37500u * 128u)) {
		poll_usb_sof_idle();
	}
}

static void prime_tx_slot(uint32_t slot)
{
	if (!usb_audio_transmit_setting || !usb_configuration) return;
	const uint32_t hs = usb_high_speed;
	const uint32_t depth = hs ? ISO_N : ISO_FS_N;
	slot %= depth;
	const uint32_t channels = hs ? AUDIO_TX_CHANNELS_480 : AUDIO_TX_CHANNELS_12;
	const uint32_t maxFrames = (hs ? AUDIO_TX_SIZE_480 : AUDIO_TX_SIZE_12) / (2 * channels);

	tx_acc += tx_rate * (hs ? (1.0f / 8000.0f) : (1.0f / 1000.0f));
	uint32_t n = (uint32_t)tx_acc;
	tx_acc -= (float)n;
	if (n > maxFrames) n = maxFrames;

	uint32_t tail = tx_tail;
	const uint32_t head = tx_head;
	const uint32_t avail = head - tail;
	if (!tx_started) {
		if (avail >= TX_MARK) {
			tail = head - TX_MARK;
			tx_started = true;
			tx_starve_uframes = 0;
		}
	} else if (avail >= TX_RECENTER_HIGH) {
		// Host paused polling EP7 IN long enough to hit the ring ceiling; re-center cleanly
		tail = head - TX_MARK;
		tx_starve_uframes = 0;
	}

	uint8_t *const pkt = hs ? tx_packet.hs[slot] : tx_packet.fs[slot];
	int16_t *dst = (int16_t *)pkt;
	bool starved = false;
	for (uint32_t k = 0; k < n; k++, dst += channels) {
		if (tx_started) {
			if (tail != head) {
				const int16_t *src = tx_ring + (tail % TX_RING) * AUDIO_TX_CHANNELS_480;
				tx_last_l = src[0];
				tx_last_r = src[1];
				dst[0] = tx_last_l;
				dst[1] = tx_last_r;
				tail++;
				tx_starve_uframes = 0;
			} else {
				// Transient 1-packet underrun: hold last sample without snapping tx_started/tx_rate
				dst[0] = tx_last_l;
				dst[1] = tx_last_r;
				starved = true;
			}
		} else {
			dst[0] = 0;
			dst[1] = 0;
		}
	}
	if (starved) {
		tx_underruns = tx_underruns + 1;
		if (++tx_starve_uframes >= TX_STARVE_MAX_UFRAMES) {
			tx_started = false;
			tx_last_l = 0;
			tx_last_r = 0;
			tx_starve_uframes = 0;
		}
	}
	tx_tail = tail;
	const uint32_t len = n * channels * 2;
	usb_prepare_transfer(&tx_transfer[slot], pkt, len, slot);
	usb_transmit(AUDIO_TX_ENDPOINT, &tx_transfer[slot]);
}

static void tx_event(transfer_t *t)
{
	if (!usb_audio_transmit_setting || !usb_configuration) return;
	sample_usb_sof_from_isr();
	const uint32_t depth = usb_high_speed ? ISO_N : ISO_FS_N;
	prime_tx_slot(t ? (t->callback_param % depth) : 0);
}

static void prime_rx_slot(uint32_t slot, uint32_t size)
{
	const uint32_t hs = usb_high_speed;
	const uint32_t depth = hs ? ISO_N : ISO_FS_N;
	slot %= depth;
	uint8_t *const pkt = hs ? rx_packet.hs[slot] : rx_packet.fs[slot];
	usb_prepare_transfer(&rx_transfer[slot], pkt, size, slot);
	usb_receive(AUDIO_RX_ENDPOINT, &rx_transfer[slot]);
}

static void rx_event(transfer_t *t)
{
	if (!usb_audio_receive_setting || !usb_configuration) return;
	sample_usb_sof_from_isr();
	const uint32_t hs = usb_high_speed;
	const uint32_t depth = hs ? ISO_N : ISO_FS_N;
	const uint32_t size = hs ? AUDIO_RX_SIZE_480 : AUDIO_RX_SIZE_12;
	const uint32_t slot = t ? (t->callback_param % depth) : 0;
	if (t) {
		const uint32_t st = t->status;
		const uint32_t rem = (st >> 16) & 0x7FFFu;
		uint8_t *const pkt = hs ? rx_packet.hs[slot] : rx_packet.fs[slot];
		if (!(st & 0x68u) && rem <= size) {
			const uint32_t len = size - rem;
			const uint32_t *src = (const uint32_t *)pkt;
			const uint32_t frames = len / 4;
			uint32_t head = rx_head;
			const uint32_t tail = rx_tail;
			for (uint32_t k = 0; k < frames; k++) {
				if (head - tail >= RX_RING) {
					rx_overruns = rx_overruns + 1;
					break;
				}
				rx_ring[head % RX_RING] = src[k];
				head++;
			}
			__asm__ volatile("" ::: "memory");
			rx_head = head;
		}
	}
	prime_rx_slot(slot, size);
}

static void prime_sync_slot(uint32_t slot)
{
	if (!usb_audio_receive_setting || !usb_configuration) return;
	slot %= SYNC_N;
	// USB 2.0, 5.12.4.2: frames per microframe as 16.16 (high speed), per frame as 10.14 (full speed)
	const float rate = rx_rate;
	uint32_t nbytes;
	if (usb_high_speed) {
		sync_value[slot] = (uint32_t)(rate * (65536.0f / 8000.0f) + 0.5f);
		nbytes = 4;
	} else {
		sync_value[slot] = (uint32_t)(rate * (16384.0f / 1000.0f) + 0.5f);
		nbytes = 3;
	}
	usb_prepare_transfer(&sync_transfer[slot], &sync_value[slot], nbytes, slot);
	usb_transmit(AUDIO_SYNC_ENDPOINT, &sync_transfer[slot]);
}

static void sync_event(transfer_t *t)
{
	if (!usb_audio_receive_setting || !usb_configuration) return;
	sample_usb_sof_from_isr();
	prime_sync_slot(t ? (t->callback_param % SYNC_N) : 0);
}

FLASHMEM static void flush_ep_mask(uint32_t mask)
{
	do {
		USB1_ENDPTFLUSH = mask;
		while (USB1_ENDPTFLUSH & mask) {}
	} while (USB1_ENDPTSTATUS & mask);
	USB1_ENDPTCOMPLETE = mask;
}

FLASHMEM static void setup_tx_stream(bool prime)
{
	const uint32_t hs = usb_high_speed;
	flush_ep_mask(1u << (AUDIO_TX_ENDPOINT + 16));
	memset(tx_transfer, 0, sizeof(tx_transfer));
	usb_config_tx_iso(AUDIO_TX_ENDPOINT, hs ? AUDIO_TX_SIZE_480 : AUDIO_TX_SIZE_12, 1, tx_event);
	if (prime) {
		const uint32_t depth = hs ? ISO_N : ISO_FS_N;
		for (uint32_t i = 0; i < depth; i++) {
			prime_tx_slot(i);
		}
	}
}

FLASHMEM static void setup_rx_stream(bool prime)
{
	const uint32_t hs = usb_high_speed;
	const uint32_t rxSize = hs ? AUDIO_RX_SIZE_480 : AUDIO_RX_SIZE_12;
	flush_ep_mask((1u << AUDIO_RX_ENDPOINT) | (1u << (AUDIO_SYNC_ENDPOINT + 16)));
	memset(rx_transfer, 0, sizeof(rx_transfer));
	memset(sync_transfer, 0, sizeof(sync_transfer));
	usb_config_rx_iso(AUDIO_RX_ENDPOINT, rxSize, 1, rx_event);
	usb_config_tx_iso(AUDIO_SYNC_ENDPOINT, hs ? 4 : 3, 1, sync_event);
	if (prime) {
		const uint32_t depth = hs ? ISO_N : ISO_FS_N;
		for (uint32_t i = 0; i < depth; i++) {
			prime_rx_slot(i, rxSize);
		}
		for (uint32_t i = 0; i < SYNC_N; i++) {
			prime_sync_slot(i);
		}
	}
}

FLASHMEM void usb_audio_reset(void)
{
	usb_audio_transmit_setting = 0;
	usb_audio_receive_setting = 0;
	tx_tail = tx_head;
	tx_started = false;
	tx_acc = 0.0f;
	tx_last_l = 0;
	tx_last_r = 0;
	tx_starve_uframes = 0;
	rx_epoch = rx_epoch + 1;
	sof_tracking = false;
	sof_rate_locked = false;
	sof_accum_uframes = 0;
	sof_accum_cycles = 0;
	usb_host_rate = AUDIO_SAMPLE_RATE_EXACT;
	tx_rate = AUDIO_SAMPLE_RATE_EXACT;
	rx_rate = AUDIO_SAMPLE_RATE_EXACT;
}

FLASHMEM void usb_audio_configure(void)
{
	NVIC_SET_PRIORITY(IRQ_USB1, USB_AUDIO_IRQ_PRIORITY);
	usb_audio_transmit_setting = 0;
	usb_audio_receive_setting = 0;
	tx_tail = tx_head;
	tx_started = false;
	tx_acc = 0.0f;
	tx_last_l = 0;
	tx_last_r = 0;
	tx_starve_uframes = 0;
	rx_epoch = rx_epoch + 1;
	// Configure endpoint queue heads in Alt 0 (idle); Isochronous dTDs are primed only on Alt 1
	setup_rx_stream(false);
	setup_tx_stream(false);
}

FLASHMEM void usb_audio_set_interface(uint32_t which, uint32_t alt)
{
	if (which == 1) {
		usb_audio_transmit_setting = (uint8_t)alt;
		tx_tail = tx_head;
		tx_started = false;
		tx_acc = 0.0f;
		tx_last_l = 0;
		tx_last_r = 0;
		tx_starve_uframes = 0;
		tx_rate = usb_host_rate;
		setup_tx_stream(alt != 0);
	} else {
		usb_audio_receive_setting = (uint8_t)alt;
		rx_epoch = rx_epoch + 1;
		rx_rate = usb_host_rate;
		setup_rx_stream(alt != 0);
	}
}

static uint8_t reply[16];   // (DTCM: endpoint 0 sends it as it is)

static inline void put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

FLASHMEM int usb_audio_class_request(uint32_t requestAndType, uint32_t value, uint32_t index, const uint8_t **out, uint32_t *len)
{
	// Only the clock source has controls: its frequency (fixed 44.1 kHz) and whether it's valid.
	if ((index & 0xFF) != AUDIO_INTERFACE || (index >> 8) != AUDIO_CLOCK_ID) return 0;
	const uint32_t hz = 44100;
	if (requestAndType == 0x01A1 && value == 0x0100) {          // GET CUR, sampling frequency
		put32(reply, hz);
		*len = 4;
	} else if (requestAndType == 0x01A1 && value == 0x0200) {   // GET CUR, clock valid
		reply[0] = 1;
		*len = 1;
	} else if (requestAndType == 0x02A1 && value == 0x0100) {   // GET RANGE, sampling frequency: just 44.1 kHz
		reply[0] = 1;
		reply[1] = 0;
		put32(reply + 2, hz);
		put32(reply + 6, hz);
		put32(reply + 10, 0);
		*len = 14;
	} else {
		return 0;
	}
	*out = reply;
	return 1;
}

int16_t *usb_audio_tx_block(void)
{
	if (!usb_audio_transmit_setting || !usb_configuration) return NULL;
	if (tx_head - tx_tail + AUDIO_BLOCK_SAMPLES > TX_RING) {   // host not polling EP7 IN
		tx_dropped = tx_dropped + 1;
		return NULL;
	}
	return tx_ring + (tx_head % TX_RING) * AUDIO_TX_CHANNELS_480;
}

void usb_audio_tx_written(void)
{
	__asm__ volatile("" ::: "memory");
	const uint32_t head = tx_head + AUDIO_BLOCK_SAMPLES;
	tx_head = head;
	const float base_rate = usb_host_rate;
	if (!tx_started) {
		tx_was_started = false;
		tx_fill = (float)TX_MARK;
		tx_integ = 0.0f;
		tx_rate = base_rate;
		return;
	}
	const float curFill = (float)(head - tx_tail);
	if (!tx_was_started) {
		tx_was_started = true;
		tx_fill = curFill;
	} else {
		tx_fill += (curFill - tx_fill) * FILL_SMOOTH;
	}
	const float err = tx_fill - (float)TX_MARK;                // fuller than mark -> send slightly faster
	tx_integ = constrain(tx_integ + err * RATE_KI_BLOCK, -RATE_TRIM_MAX, RATE_TRIM_MAX);
	const float trim = constrain(tx_integ + err * RATE_KP, -RATE_TRIM_MAX, RATE_TRIM_MAX);
	tx_rate = base_rate + trim;
}

static inline uint32_t lerp_stereo_frame(uint32_t u0, uint32_t u1, int32_t w128)
{
	const int32_t l0 = (int16_t)(u0 & 0xFFFFu);
	const int32_t r0 = (int16_t)(u0 >> 16);
	const int32_t l1 = (int16_t)(u1 & 0xFFFFu);
	const int32_t r1 = (int16_t)(u1 >> 16);
	const int16_t l = (int16_t)((l0 * 128 + (l1 - l0) * w128 + 64) >> 7);
	const int16_t r = (int16_t)((r0 * 128 + (r1 - r0) * w128 + 64) >> 7);
	return (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
}

int usb_audio_rx_block(uint32_t *frames)
{
	// Keep the crystal-vs-USB1_FRINDEX baseline warm even while Alt 0 is idle
	update_usb_host_rate_idle();
	const float base_rate = usb_host_rate;

	const uint32_t ep = rx_epoch;
	if (rx_seen_epoch != ep || !usb_audio_receive_setting || !usb_configuration) {
		rx_seen_epoch = ep;
		rx_tail = rx_head;
		rx_started = false;
		rx_prev_frame = 0;
		rx_fill = (float)RX_MARK;
		rx_integ = 0.0f;
		rx_rate = base_rate;
		if (!usb_audio_receive_setting || !usb_configuration) return 0;
	}

	uint32_t tail = rx_tail;
	const uint32_t head = rx_head;
	uint32_t fill = head - tail;

	if (!rx_started) {
		rx_rate = base_rate;
		if (fill < RX_MARK) return 0;
		tail = head - RX_MARK;
		fill = RX_MARK;
		rx_started = true;
		rx_fill = (float)RX_MARK;
		rx_prev_frame = rx_ring[tail % RX_RING];
	} else if (fill < AUDIO_BLOCK_SAMPLES) {
		// Host stopped sending ISO packets; reset cleanly until prefill reaches RX_MARK again
		rx_started = false;
		rx_prev_frame = 0;
		rx_rate = base_rate;
		rx_underruns = rx_underruns + 1;
		return 0;
	} else {
		if (fill >= RX_RECENTER_HIGH) {
			// Burst overrun guard: re-center to RX_MARK instead of pinning against RX_RING
			tail = head - RX_MARK;
			fill = RX_MARK;
			rx_fill = (float)RX_MARK;
		} else {
			rx_fill += ((float)fill - rx_fill) * FILL_SMOOTH;
		}
		const float err = (float)RX_MARK - rx_fill;            // emptier than mark -> ask for slightly higher rate
		if (fill > RX_SLIP_HIGH || fill < RX_SLIP_LOW) {
			// Host is slaved to another clock source (e.g. Aggregate Device) and ignoring EP6 IN
			// explicit feedback; bleed rx_integ toward 0 so PI windup never accumulates.
			rx_integ *= 0.995f;
		} else {
			rx_integ = constrain(rx_integ + err * RATE_KI_BLOCK, -RATE_TRIM_MAX, RATE_TRIM_MAX);
		}
		const float trim = constrain(rx_integ + err * RATE_KP, -RATE_TRIM_MAX, RATE_TRIM_MAX);
		rx_rate = base_rate + trim;
	}

	uint32_t consumed = AUDIO_BLOCK_SAMPLES;
	if (fill > RX_SLIP_HIGH && fill >= AUDIO_BLOCK_SAMPLES + 1) {
		// Host clock faster than M8 and ignoring feedback: compress 129 -> 128 frames smoothly
		for (uint32_t k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
			const uint32_t u0 = rx_ring[(tail + k) % RX_RING];
			const uint32_t u1 = rx_ring[(tail + k + 1) % RX_RING];
			frames[k] = lerp_stereo_frame(u0, u1, (int32_t)(k + 1));
		}
		consumed = AUDIO_BLOCK_SAMPLES + 1;
	} else if (fill < RX_SLIP_LOW) {
		// Host clock slower than M8 and ignoring feedback: stretch 127 -> 128 frames smoothly
		uint32_t prev = rx_prev_frame;
		for (uint32_t k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
			const uint32_t cur = rx_ring[(tail + k) % RX_RING];
			frames[k] = lerp_stereo_frame(prev, cur, (int32_t)(127 - k));
			prev = cur;
		}
		consumed = AUDIO_BLOCK_SAMPLES - 1;
	} else {
		// Normal locked window [256, 768]: bit-exact 1:1 PCM copy
		for (uint32_t k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
			frames[k] = rx_ring[(tail + k) % RX_RING];
		}
	}

	rx_prev_frame = frames[AUDIO_BLOCK_SAMPLES - 1];
	__asm__ volatile("" ::: "memory");
	rx_tail = tail + consumed;
	return 1;
}

FLASHMEM void usb_audio_l7_get_stats(struct usb_audio_l7_stats *s)
{
	s->txUnderruns = tx_underruns;
	s->txDropped = tx_dropped;
	s->rxUnderruns = rx_underruns;
	s->rxOverruns = rx_overruns;
	s->txFill = tx_fill;
	s->rxFill = rx_fill;
	s->txRate = tx_rate;
	s->rxRate = rx_rate;
}

#endif

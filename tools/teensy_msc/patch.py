#!/usr/bin/env python3
"""Adds the Capstan composite USB type (-DUSB_SERIAL_MSC) to the Teensyduino 1.62 core
(Teensy 4.0 and Teensy MicroMod), enabling Capstan to:
  - Enumerate cleanly in Normal Mode as Serial + MIDI + UAC2 Audio only (PID 0x048A,
    interfaces 0..5 in ascending order, 500 mA bus-powered, zero Mass Storage interface)
    so macOS / Windows disk & indexing daemons never probe or reset the device during audio,
  - Dynamically re-enumerate into a dedicated Serial + Mass Storage device (PID 0x04D5,
    usb_msc.c) when entering USB STORAGE MODE (usbDriveMode()) and switch back on exit,
  - Send and receive USB MIDI events and clock (midi_usb.cpp), and
  - Stream USB Audio Class 2.0 (usb_audio_l7.cpp: 2 stereo channels to the host computer —
    Master L/R — and 2 stereo channels from the host computer, locked to one shared
    USB1_FRINDEX clock domain).

  python3 tools/teensy_msc/patch.py            # apply (safe to repeat)
  python3 tools/teensy_msc/patch.py --undo     # restore original core files

Everything added is gated behind -DUSB_SERIAL_MSC / MSC_INTERFACE / AUDIO_UAC2
(arduino-cli: teensy:avr:teensy40:usb=serialmsc or teensy:avr:teensyMM:usb=serialmsc);
other USB types and boards build unchanged. Modified core files are backed up as <name>.l7orig.
"""
import glob
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MARK = "L7_MSC"
MARK_MM = "# L7_MSC_MM: tools/teensy_msc/patch.py"
STAMP_NAME = ".l7_patch_stamp"
CORE_FILES = ("usb_msc.c", "usb_msc.h", "usb_audio_l7.cpp", "usb_audio_l7.h")
PATCHED_RELS = (
    "cores/teensy4/usb_desc.h",
    "cores/teensy4/usb_desc.c",
    "cores/teensy4/usb.c",
    "boards.txt",
    "cores/teensy4/usb_midi.h",
    "cores/teensy4/usb_midi.c",
    "cores/teensy4/usb_serial.c",
    "cores/teensy4/usb_audio.h",
    "cores/teensy4/usb_audio.cpp",
    "libraries/SdFat/src/common/FsStructs.h",
    "libraries/SdFat/src/common/FsName.cpp",
    "libraries/SdFat/src/SdCard/SdioTeensy.cpp",
)


def compute_patch_stamp():
    h = hashlib.sha256()
    for name in ("patch.py",) + CORE_FILES:
        h.update(open(os.path.join(HERE, name), "rb").read())
    return h.hexdigest()[:16]


def find_platform():
    patterns = [
        "~/Library/Arduino15/packages/teensy/hardware/avr/*/cores/teensy4",
        "~/AppData/Local/Arduino15/packages/teensy/hardware/avr/*/cores/teensy4",
        "~/.arduino15/packages/teensy/hardware/avr/*/cores/teensy4",
    ]
    if "LOCALAPPDATA" in os.environ:
        patterns.append(os.path.join(os.environ["LOCALAPPDATA"], "Arduino15/packages/teensy/hardware/avr/*/cores/teensy4"))
    hits = []
    for pat in patterns:
        hits.extend(glob.glob(os.path.expanduser(pat)))
    hits = sorted(set(hits))
    if not hits:
        sys.exit("no Teensyduino (teensy4 core) found under Arduino15")
    return os.path.dirname(os.path.dirname(hits[-1]))


def backup(path):
    if not os.path.exists(path + ".l7orig"):
        shutil.copy2(path, path + ".l7orig")


def edit(path, steps):
    s = open(path).read()
    if MARK in s:
        return False
    backup(path)
    for old, new in steps:
        if s.count(old) < 1:
            sys.exit("anchor not found in %s:\n%s" % (path, old))
        s = s.replace(old, new, 1)
    open(path, "w").write(s)
    return True


def edit_marked(path, marker, steps):
    s = open(path).read()
    if marker in s:
        return False
    backup(path)
    for old, new in steps:
        if s.count(old) < 1:
            sys.exit("anchor not found in %s:\n%s" % (path, old))
        s = s.replace(old, new, 1)
    open(path, "w").write(s)
    return True


MSC_TYPE = """#elif defined(USB_SERIAL_MSC) // L7_MSC L7_MIDI L7_UAC2: Serial + MIDI + UAC2 Audio, with dynamic Mass Storage mode (tools/teensy_msc)
  #define VENDOR_ID		0x16C0
  #define PRODUCT_ID		0x048A
  #define MSC_STORAGE_PID	0x04D5
  #if defined(ARDUINO_TEENSY_MICROMOD)
    #define BCD_DEVICE		0x0381
  #else
    #define BCD_DEVICE		0x0379
  #endif
  #define MANUFACTURER_NAME	{'C','a','p','s','t','a','n'}
  #define MANUFACTURER_NAME_LEN	7
  #define PRODUCT_NAME		{'C','a','p','s','t','a','n'}
  #define PRODUCT_NAME_LEN	7
  #define EP0_SIZE		64
  #define NUM_ENDPOINTS         7
  #define NUM_INTERFACE		6
  #define CDC_IAD_DESCRIPTOR	1
  #define CDC_STATUS_INTERFACE	0
  #define CDC_DATA_INTERFACE	1	// Serial
  #define CDC_ACM_ENDPOINT	2
  #define CDC_RX_ENDPOINT       3
  #define CDC_TX_ENDPOINT       3
  #define CDC_ACM_SIZE          16
  #define CDC_RX_SIZE_480       512
  #define CDC_TX_SIZE_480       512
  #define CDC_RX_SIZE_12        64
  #define CDC_TX_SIZE_12        64
  #define MIDI_INTERFACE        2	// MIDI (interface 2, ascending before UAC2 interfaces 3..5)
  #define MIDI_NUM_CABLES       1
  #define MIDI_TX_ENDPOINT      4
  #define MIDI_TX_SIZE_12       64
  #define MIDI_TX_SIZE_480      512
  #define MIDI_RX_ENDPOINT      4
  #define MIDI_RX_SIZE_12       64
  #define MIDI_RX_SIZE_480      512
  #define MSC_INTERFACE		2	// Mass Storage (interface 2 in dedicated USB Storage Mode, PID 0x04D5)
  #define MSC_TX_ENDPOINT	5
  #define MSC_RX_ENDPOINT	5
  #define MSC_TX_SIZE_12	64
  #define MSC_TX_SIZE_480	512
  #define MSC_RX_SIZE_12	64
  #define MSC_RX_SIZE_480	512
  #define ENDPOINT2_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define AUDIO_INTERFACE	3	// audio: USB Audio Class 2.0 (usb_audio_l7.cpp), interfaces 3, 4, 5
  #define AUDIO_UAC2		1
  #define AUDIO_CLOCK_ID	9
  #define AUDIO_TX_CHANNELS_480	2	// to the computer: Master L/R
  #define AUDIO_TX_CHANNELS_12	2	// (full speed: Master L/R)
  #define AUDIO_RX_CHANNELS	2	// from the computer (Left In, Right In)
  #define AUDIO_TX_ENDPOINT     7
  #define AUDIO_TX_SIZE_480     (7*2*AUDIO_TX_CHANNELS_480)	// up to 7 frames a microframe (44.1 kHz: 5.5)
  #define AUDIO_TX_SIZE_12      (47*2*AUDIO_TX_CHANNELS_12)	// up to 47 frames a frame
  #define AUDIO_RX_ENDPOINT     6
  #define AUDIO_RX_SIZE_480     (7*2*AUDIO_RX_CHANNELS)
  #define AUDIO_RX_SIZE_12      (47*2*AUDIO_RX_CHANNELS)
  #define AUDIO_SYNC_ENDPOINT	6
  #define AUDIO_CHANNEL_NAMES	0x10	// string descriptors 0x10-0x11: Master L, Master R
  #define ENDPOINT5_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT6_CONFIG	ENDPOINT_RECEIVE_ISOCHRONOUS + ENDPOINT_TRANSMIT_ISOCHRONOUS
  #define ENDPOINT7_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_ISOCHRONOUS

#elif defined(USB_AUDIO)
"""

CHANNEL_NAMES = ["Master L", "Master R"]


def uac2_descriptor(speed):
    """The audio function (USB Audio Class 2.0) for one speed: 2 stereo channels (Master L/R)
    to the computer, 2 stereo channels from it; one fixed 44.1 kHz clock; no controls."""
    hs = speed == "480"
    tx_ch = "AUDIO_TX_CHANNELS_%s" % speed
    tx_config = "0x03,0,0,0"                                 # (2: front left, right stereo)
    tx_names = "0"
    interval = "1" if hs else "1"                            # data: every microframe (high speed) / frame
    fb_interval = "4" if hs else "1"                         # feedback: every 1 ms
    fb_size = "4" if hs else "3"                             # USB 2.0 5.12.4.2 / Apple TN3190: 4 B (16.16) HS, 3 B (10.14) FS
    return """#if defined(AUDIO_UAC2) // L7_UAC2: tools/teensy_msc/patch.py
        // interface association: the audio function, UAC2 (protocol 0x20), iFunction = 2 (product name "Capstan")
        8, 11, AUDIO_INTERFACE, 3, 0x01, 0x00, 0x20, 2,
        // AudioControl interface (iInterface = 2)
        9, 4, AUDIO_INTERFACE, 0, 0, 0x01, 0x01, 0x20, 2,
        // class-specific AC header: UAC 2.00, category I/O box, wTotalLength 75 (the AC descriptors), no controls
        9, 0x24, 0x01, 0x00, 0x02, 0x08, 75, 0, 0x00,
        // clock source: internal, fixed; frequency and validity readable
        8, 0x24, 0x0A, AUDIO_CLOCK_ID, 0x01, 0x05, 0, 0,
        // input terminal 1 (line connector): what goes to the computer (iTerminal = 2)
        17, 0x24, 0x02, 1, 0x03, 0x06, 0, AUDIO_CLOCK_ID, %(tx_ch)s, %(tx_config)s, %(tx_names)s, 0x00, 0x00, 2,
        // output terminal 2 (USB streaming): to the computer (iTerminal = 2)
        12, 0x24, 0x03, 2, 0x01, 0x01, 0, 1, AUDIO_CLOCK_ID, 0x00, 0x00, 2,
        // input terminal 3 (USB streaming): from the computer (iTerminal = 2)
        17, 0x24, 0x02, 3, 0x01, 0x01, 0, AUDIO_CLOCK_ID, AUDIO_RX_CHANNELS, 0x03, 0, 0, 0, 0, 0x00, 0x00, 2,
        // output terminal 4 (line connector): into Capstan (iTerminal = 2)
        12, 0x24, 0x03, 4, 0x03, 0x06, 0, 3, AUDIO_CLOCK_ID, 0x00, 0x00, 2,
        // AudioStreaming interface: to the computer, alternate 0 (idle)
        9, 4, AUDIO_INTERFACE+1, 0, 0, 0x01, 0x02, 0x20, 2,
        // alternate 1: streaming
        9, 4, AUDIO_INTERFACE+1, 1, 1, 0x01, 0x02, 0x20, 2,
        // AS general: terminal 2, PCM
        16, 0x24, 0x01, 2, 0x00, 0x01, 0x01, 0, 0, 0, %(tx_ch)s, %(tx_config)s, %(tx_names)s,
        // format type I: 2-byte subslots, 16 bits
        6, 0x24, 0x02, 0x01, 2, 16,
        // endpoint: isochronous, asynchronous, data (IN)
        7, 5, AUDIO_TX_ENDPOINT | 0x80, 0x05, LSB(AUDIO_TX_SIZE_%(speed)s), MSB(AUDIO_TX_SIZE_%(speed)s), %(interval)s,
        // class-specific endpoint
        8, 0x25, 0x01, 0x00, 0x00, 0, 0, 0,
        // AudioStreaming interface: from the computer, alternate 0 (idle)
        9, 4, AUDIO_INTERFACE+2, 0, 0, 0x01, 0x02, 0x20, 2,
        // alternate 1: streaming, with the feedback endpoint
        9, 4, AUDIO_INTERFACE+2, 1, 2, 0x01, 0x02, 0x20, 2,
        // AS general: terminal 3, PCM, stereo
        16, 0x24, 0x01, 3, 0x00, 0x01, 0x01, 0, 0, 0, AUDIO_RX_CHANNELS, 0x03, 0, 0, 0, 0,
        // format type I: 2-byte subslots, 16 bits
        6, 0x24, 0x02, 0x01, 2, 16,
        // endpoint: isochronous, asynchronous, data (OUT)
        7, 5, AUDIO_RX_ENDPOINT, 0x05, LSB(AUDIO_RX_SIZE_%(speed)s), MSB(AUDIO_RX_SIZE_%(speed)s), %(interval)s,
        // class-specific endpoint
        8, 0x25, 0x01, 0x00, 0x00, 0, 0, 0,
        // feedback endpoint (IN): the rate Capstan wants
        7, 5, AUDIO_SYNC_ENDPOINT | 0x80, 0x11, %(fb_size)s, 0, %(fb_interval)s,
#else
""" % dict(tx_ch=tx_ch, tx_config=tx_config, tx_names=tx_names, interval=interval,
           fb_interval=fb_interval, fb_size=fb_size, speed=speed)


USB_C_URI_OLD = """\t\twhile (USB1_ENDPTPRIME != 0) ; // Wait for any endpoint priming
\t\tUSB1_ENDPTFLUSH = 0xFFFFFFFF;  // Cancel all endpoint primed status
\t\tif ((USB1_PORTSC1 & USB_PORTSC1_PR)) {
\t\t\t//printf("reset\\n");
\t\t} else {
\t\t\t// we took too long to respond :(
\t\t\t// TODO; is this ever really a problem?
\t\t\t//printf("reset too slow\\n");
\t\t}
\t\t#if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
\t\tusb_serial_reset();
\t\t#endif
\t\tendpointN_notify_mask = 0;"""

USB_C_URI_NEW = """\t\twhile (USB1_ENDPTPRIME != 0) ; // Wait for any endpoint priming
\t\tUSB1_ENDPTFLUSH = 0xFFFFFFFF;  // Cancel all endpoint primed status
\t\twhile (USB1_ENDPTFLUSH != 0) ; // L7_MSC: wait for hardware flush completion
\t\tusb_configuration = 0;
\t\tmemset(endpoint_queue_head + 2, 0, sizeof(endpoint_t) * NUM_ENDPOINTS * 2);
\t\tif ((USB1_PORTSC1 & USB_PORTSC1_PR)) {
\t\t\t//printf("reset\\n");
\t\t} else {
\t\t\t// we took too long to respond :(
\t\t\t// TODO; is this ever really a problem?
\t\t\t//printf("reset too slow\\n");
\t\t}
\t\t#if defined(CDC_STATUS_INTERFACE) && defined(CDC_DATA_INTERFACE)
\t\tusb_serial_reset();
\t\t#endif
\t\t#if defined(MIDI_INTERFACE)
\t\tusb_midi_reset();
\t\t#endif
\t\t#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2)
\t\tusb_audio_reset();
\t\t#endif
\t\t#if defined(MSC_INTERFACE)
\t\tusb_msc_bus_reset();
\t\t#endif
\t\tendpointN_notify_mask = 0;"""

USB_C_SET_CONFIG_OLD = """\t  case 0x0900: // SET_CONFIGURATION
\t\tusb_configuration = setup.wValue;
\t\t// configure all other endpoints"""

USB_C_SET_CONFIG_NEW = """\t  case 0x0900: // SET_CONFIGURATION
\t\t#if defined(USB_SERIAL_MSC) // L7_MSC: clean unconfigure & dynamic Audio/MSC endpoint gating
\t\tusb_configuration = 0;
\t\t{
\t\t\tconst uint32_t unused_ep = ENDPOINT_TRANSMIT_UNUSED + ENDPOINT_RECEIVE_UNUSED;
\t\t\tUSB1_ENDPTFLUSH = 0xFFFCFFFCu;
\t\t\twhile (USB1_ENDPTFLUSH != 0) ;
\t\t\tendpointN_notify_mask = 0;
\t\t\tmemset(endpoint_queue_head + 2, 0, sizeof(endpoint_t) * NUM_ENDPOINTS * 2);
\t\t\tif (setup.wValue == 0) {
\t\t\t\tUSB1_ENDPTCTRL2 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL3 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL4 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL5 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL6 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL7 = unused_ep;
\t\t\t\tusb_midi_reset();
\t\t\t\tusb_audio_reset();
\t\t\t\tusb_msc_bus_reset();
\t\t\t\tendpoint0_receive(NULL, 0, 0);
\t\t\t\treturn;
\t\t\t}
\t\t\tUSB1_ENDPTCTRL2 = ENDPOINT2_CONFIG;
\t\t\tUSB1_ENDPTCTRL3 = ENDPOINT3_CONFIG;
\t\t\tif (usb_msc_is_active()) {
\t\t\t\tUSB1_ENDPTCTRL4 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL5 = ENDPOINT5_CONFIG;
\t\t\t\tUSB1_ENDPTCTRL6 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL7 = unused_ep;
\t\t\t\tusb_serial_configure();
\t\t\t\tusb_midi_reset();
\t\t\t\tusb_audio_reset();
\t\t\t\tusb_msc_configure();
\t\t\t} else {
\t\t\t\tUSB1_ENDPTCTRL4 = ENDPOINT4_CONFIG;
\t\t\t\tUSB1_ENDPTCTRL5 = unused_ep;
\t\t\t\tUSB1_ENDPTCTRL6 = ENDPOINT6_CONFIG;
\t\t\t\tUSB1_ENDPTCTRL7 = ENDPOINT7_CONFIG;
\t\t\t\tusb_serial_configure();
\t\t\t\tusb_midi_configure();
\t\t\t\tusb_audio_configure();
\t\t\t}
\t\t\t__asm__ volatile("" ::: "memory");
\t\t\tusb_configuration = setup.wValue;
\t\t\tendpoint0_receive(NULL, 0, 0);
\t\t\treturn;
\t\t}
\t\t#endif
\t\tusb_configuration = setup.wValue;
\t\t// configure all other endpoints"""

USB_C_CLEAR_HALT_OLD = """\t  case 0x0102: // CLEAR_FEATURE (endpoint)
\t\tendpoint = setup.wIndex & 0x7F;
\t\tif (endpoint > 7) break;
\t\tdir = setup.wIndex & 0x80;"""

USB_C_CLEAR_HALT_NEW = """\t  case 0x0102: // CLEAR_FEATURE (endpoint)
\t\tendpoint = setup.wIndex & 0x7F;
\t\tif (endpoint > 7) break;
\t\tdir = setup.wIndex & 0x80;
#if defined(MSC_INTERFACE) // L7_MSC: BOT wedge & post-halt CSW state machine
\t\tif (usb_msc_is_active() && endpoint == MSC_TX_ENDPOINT) {
\t\t\tusb_msc_clear_halt(dir);
\t\t\tendpoint0_receive(NULL, 0, 0);
\t\t\treturn;
\t\t}
#endif"""

USB_C_GET_DESC_OLD = """\t\t\t\t} else {
\t\t\t\t\tdatalen = list->length;
\t\t\t\t}
\t\t\t\tif (datalen > setup.wLength) datalen = setup.wLength;

\t\t\t\t// copy the descriptor, from PROGMEM to DMAMEM
\t\t\t\tif (setup.wValue == 0x200) {
\t\t\t\t\t// config descriptor needs to adapt to speed
\t\t\t\t\tconst uint8_t *src = usb_config_descriptor_12;
\t\t\t\t\tif (usb_high_speed) src = usb_config_descriptor_480;
\t\t\t\t\tmemcpy(usb_descriptor_buffer, src, datalen);
\t\t\t\t} else if (setup.wValue == 0x700) {
\t\t\t\t\t// other speed config also needs to adapt
\t\t\t\t\tconst uint8_t *src = usb_config_descriptor_480;
\t\t\t\t\tif (usb_high_speed) src = usb_config_descriptor_12;
\t\t\t\t\tmemcpy(usb_descriptor_buffer, src, datalen);
\t\t\t\t\tusb_descriptor_buffer[1] = 7;
\t\t\t\t} else {
\t\t\t\t\tmemcpy(usb_descriptor_buffer, list->addr, datalen);
\t\t\t\t}"""

USB_C_GET_DESC_NEW = """\t\t\t\t} else {
\t\t\t\t\tdatalen = list->length;
#if defined(MSC_INTERFACE) // L7_MSC: dedicated Storage Mode descriptors
\t\t\t\t\tif (usb_msc_is_active() && (setup.wValue == 0x200 || setup.wValue == 0x700)) {
\t\t\t\t\t\tdatalen = USB_MSC_CONFIG_DESC_SIZE;
\t\t\t\t\t}
#endif
\t\t\t\t}
\t\t\t\tif (datalen > setup.wLength) datalen = setup.wLength;

\t\t\t\t// copy the descriptor, from PROGMEM to DMAMEM
\t\t\t\tif (setup.wValue == 0x200) {
\t\t\t\t\t// config descriptor needs to adapt to speed
\t\t\t\t\tconst uint8_t *src = usb_config_descriptor_12;
\t\t\t\t\tif (usb_high_speed) src = usb_config_descriptor_480;
#if defined(MSC_INTERFACE)
\t\t\t\t\tif (usb_msc_is_active()) {
\t\t\t\t\t\tsrc = usb_high_speed ? usb_msc_config_descriptor_480 : usb_msc_config_descriptor_12;
\t\t\t\t\t}
#endif
\t\t\t\t\tmemcpy(usb_descriptor_buffer, src, datalen);
#if defined(MSC_INTERFACE)
\t\t\t\t\tif (usb_self_powered_no_charge) {
\t\t\t\t\t\tif (datalen > 7) usb_descriptor_buffer[7] = 0xC0;
\t\t\t\t\t\tif (datalen > 8) usb_descriptor_buffer[8] = 0;
\t\t\t\t\t}
#endif
\t\t\t\t} else if (setup.wValue == 0x700) {
\t\t\t\t\t// other speed config also needs to adapt
\t\t\t\t\tconst uint8_t *src = usb_config_descriptor_480;
\t\t\t\t\tif (usb_high_speed) src = usb_config_descriptor_12;
#if defined(MSC_INTERFACE)
\t\t\t\t\tif (usb_msc_is_active()) {
\t\t\t\t\t\tsrc = usb_high_speed ? usb_msc_config_descriptor_12 : usb_msc_config_descriptor_480;
\t\t\t\t\t}
#endif
\t\t\t\t\tmemcpy(usb_descriptor_buffer, src, datalen);
\t\t\t\t\tusb_descriptor_buffer[1] = 7;
#if defined(MSC_INTERFACE)
\t\t\t\t\tif (usb_self_powered_no_charge) {
\t\t\t\t\t\tif (datalen > 7) usb_descriptor_buffer[7] = 0xC0;
\t\t\t\t\t\tif (datalen > 8) usb_descriptor_buffer[8] = 0;
\t\t\t\t\t}
#endif
\t\t\t\t} else {
\t\t\t\t\tmemcpy(usb_descriptor_buffer, list->addr, datalen);
#if defined(MSC_INTERFACE) && defined(MSC_STORAGE_PID)
\t\t\t\t\tif (usb_msc_is_active() && setup.wValue == 0x0100 && datalen >= 12) {
\t\t\t\t\t\tusb_descriptor_buffer[10] = MSC_STORAGE_PID & 255;
\t\t\t\t\t\tusb_descriptor_buffer[11] = (MSC_STORAGE_PID >> 8) & 255;
\t\t\t\t\t}
#endif
\t\t\t\t}"""

USB_C_REQUESTS = """#endif
#if defined(MSC_INTERFACE) // L7_MSC
	  case 0xFEA1: // GET MAX LUN (mass storage class): one logical unit
		if (usb_msc_is_active() && setup.wIndex == MSC_INTERFACE && setup.wLength >= 1) {
			endpoint0_buffer[0] = 0;
			endpoint0_transmit(endpoint0_buffer, 1, 0);
			return;
		}
		break;
	  case 0xFF21: // BULK-ONLY MASS STORAGE RESET
		if (usb_msc_is_active() && setup.wIndex == MSC_INTERFACE && setup.wValue == 0 && setup.wLength == 0) {
			usb_msc_reset_request();
			endpoint0_receive(NULL, 0, 0);
			return;
		}
		break;
#endif
	}
	printf("endpoint 0 stall\\n");"""

USB_C_UAC2 = """#if defined(AUDIO_INTERFACE) && defined(AUDIO_UAC2) // L7_UAC2: USB Audio Class 2.0 (usb_audio_l7.cpp)
	  case 0x0B01: // SET_INTERFACE (alternate setting)
		if (!usb_msc_is_active() && (setup.wIndex == AUDIO_INTERFACE+1 || setup.wIndex == AUDIO_INTERFACE+2)) {
			usb_audio_set_interface(setup.wIndex - AUDIO_INTERFACE, setup.wValue);
			endpoint0_receive(NULL, 0, 0);
			return;
		}
		break;
	  case 0x0A81: // GET_INTERFACE (alternate setting)
		if (!usb_msc_is_active() && (setup.wIndex == AUDIO_INTERFACE+1 || setup.wIndex == AUDIO_INTERFACE+2)) {
			endpoint0_buffer[0] = setup.wIndex == AUDIO_INTERFACE+1 ? usb_audio_transmit_setting : usb_audio_receive_setting;
			endpoint0_transmit(endpoint0_buffer, 1, 0);
			return;
		}
		break;
	  case 0x0121: // SET CUR / RANGE (to an interface or entity): taken, nothing to set (a fixed clock)
	  case 0x0221:
	  case 0x0122:
		if (!usb_msc_is_active() && setup.wLength <= sizeof(endpoint0_buffer)) {
			endpoint0_receive(endpoint0_buffer, setup.wLength, 0);
			return;
		}
		break;
	  case 0x01A1: // GET CUR
	  case 0x02A1: // GET RANGE
		if (!usb_msc_is_active()) {
			const uint8_t *reply;
			uint32_t len;
			if (usb_audio_class_request(setup.wRequestAndType, setup.wValue, setup.wIndex, &reply, &len)) {
				endpoint0_transmit(reply, len < setup.wLength ? len : setup.wLength, 0);
				return;
			}
		}
		break;
#elif defined(AUDIO_INTERFACE)
	  case 0x0B01: // SET_INTERFACE (alternate setting)"""


def channel_names():
    names = "#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2: the channels to the computer, by name\n"
    entries = "#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2\n"
    for k, name in enumerate(CHANNEL_NAMES):
        names += "PROGMEM const struct usb_string_descriptor_struct usb_string_channel%d = {%d, 3, {%s}};\n" % (
            k, 2 + 2 * len(name), ",".join("'%s'" % c for c in name))
        entries += "        {0x0300 + AUDIO_CHANNEL_NAMES + %d, 0x0409, (const uint8_t *)&usb_string_channel%d, 0},\n" % (k, k)
    return names + "#endif\n\n", entries + "#endif\n"


def uac2_desc(desc):
    s = open(desc).read()
    if "L7_UAC2" in s:
        orig = s
        names, entries = channel_names()
        i = s.index("#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2: the channels")
        j = s.index("#endif\n\n", i) + len("#endif\n\n")
        ei = s.index("#ifdef AUDIO_CHANNEL_NAMES // L7_UAC2\n")
        ej = s.index("#endif\n", ei) + len("#endif\n")
        s = s[:ei] + entries + s[ej:]
        s = s[:i] + names + s[j:]
        at = 0
        for speed in ("480", "12"):
            u_start = s.index("#if defined(AUDIO_UAC2) // L7_UAC2: tools/teensy_msc/patch.py\n", at)
            u_end = s.index("#else\n", u_start) + len("#else\n")
            new_blk = uac2_descriptor(speed)
            s = s[:u_start] + new_blk + s[u_end:]
            at = u_start + len(new_blk)
        if s == orig:
            return False
        open(desc, "w").write(s)
        return True
    backup(desc)
    size_old = "#define AUDIO_INTERFACE_DESC_SIZE\t8 + 9+10+12+9+12+10+9 + 9+9+7+11+9+7 + 9+9+7+11+9+7+9\n"
    if s.count(size_old) != 1:
        sys.exit("usb_desc.c: the audio size not as expected")
    s = s.replace(size_old, "#if defined(AUDIO_UAC2) // L7_UAC2\n#define AUDIO_INTERFACE_DESC_SIZE\t8+9+75 + 9+9+16+6+7+8 + 9+9+16+6+7+8+7\n#else\n"
                  + size_old + "#endif\n", 1)
    start, end = "#ifdef AUDIO_INTERFACE\n", "\t0,\t\t\t\t\t// bSynchAddress\n#endif\n\n#ifdef MULTITOUCH_INTERFACE"
    if s.count(start) != 2 or s.count(end) != 2:
        sys.exit("usb_desc.c: the audio descriptors not as expected")
    at = 0
    for speed in ("480", "12"):
        i = s.index(start, at) + len(start)
        s = s[:i] + uac2_descriptor(speed) + s[i:]
        j = s.index(end, i) + len("\t0,\t\t\t\t\t// bSynchAddress\n")
        s = s[:j] + "#endif // L7_UAC2\n" + s[j:]
        at = j
    names, entries = channel_names()
    list_head = "const usb_descriptor_list_t usb_descriptor_list[] = {\n"
    string0 = "        {0x0300, 0x0000, (const uint8_t *)&string0, 0},\n"
    if s.count(list_head) != 1 or s.count(string0) != 1:
        sys.exit("usb_desc.c: the descriptor list not as expected")
    s = s.replace(list_head, names + list_head, 1).replace(string0, entries + string0, 1)
    open(desc, "w").write(s)
    return True


def apply():
    plat = find_platform()
    core = os.path.join(plat, "cores", "teensy4")
    stamp_path = os.path.join(core, STAMP_NAME)
    want_stamp = compute_patch_stamp()
    cur_stamp = open(stamp_path).read().strip() if os.path.exists(stamp_path) else ""
    has_backups = any(os.path.exists(os.path.join(plat, rel) + ".l7orig") for rel in PATCHED_RELS)
    if has_backups and cur_stamp != want_stamp:
        undo()
    changed = []
    for name in CORE_FILES:
        src = os.path.join(HERE, name)
        dst = os.path.join(core, name)
        if not os.path.exists(dst) or open(src, "rb").read() != open(dst, "rb").read():
            shutil.copyfile(src, dst)
            os.utime(dst, None)
            changed.append(name)

    hdr = os.path.join(core, "usb_desc.h")
    if edit(hdr, [("#elif defined(USB_AUDIO)\n", MSC_TYPE)]):
        changed.append("usb_desc.h")
    else:
        h = open(hdr).read()
        i = h.index("#elif defined(USB_SERIAL_MSC)")
        j = h.index("#elif defined(USB_AUDIO)\n", i) + len("#elif defined(USB_AUDIO)\n")
        if h[i:j] != MSC_TYPE:
            open(hdr, "w").write(h[:i] + MSC_TYPE + h[j:])
            changed.append("usb_desc.h (updated)")

    desc = os.path.join(core, "usb_desc.c")
    s = open(desc).read()
    if MARK not in s:
        backup(desc)
        power_old = (
            "        0,                                      // iConfiguration\n"
            "        0xC0,                                   // bmAttributes\n"
            "        50,                                     // bMaxPower\n"
        )
        power_new = (
            "        0,                                      // iConfiguration\n"
            "#ifdef USB_SERIAL_MSC // L7_MSC: bus-powered 500 mA budget for M8 battery charger + audio\n"
            "        0x80,                                   // bmAttributes (bus-powered)\n"
            "        250,                                    // bMaxPower (500 mA)\n"
            "#else\n"
            "        0xC0,                                   // bmAttributes\n"
            "        50,                                     // bMaxPower\n"
            "#endif\n"
        )
        if s.count(power_old) != 2:
            sys.exit("usb_desc.c: power header anchor count != 2")
        s = s.replace(power_old, power_new)
        serial_init_old = "void usb_init_serialnumber(void)\n{"
        if s.count(serial_init_old) != 1:
            sys.exit("usb_desc.c: usb_init_serialnumber anchor not found")
        s = s.replace(serial_init_old, "FLASHMEM void usb_init_serialnumber(void)\n{", 1)
        open(desc, "w").write(s)
        changed.append("usb_desc.c")
    if uac2_desc(desc):
        changed.append("usb_desc.c (UAC2)")

    usbc = os.path.join(core, "usb.c")
    mtp_end = ("\t\tbreak;\n#endif\n\t}\n\tprintf(\"endpoint 0 stall\\n\");")
    if edit(usbc, [
        ('#include "usb_mtp.h"\n', '#include "usb_mtp.h"\n#include "usb_msc.h" // L7_MSC\n'),
        (USB_C_URI_OLD, USB_C_URI_NEW),
        (USB_C_SET_CONFIG_OLD, USB_C_SET_CONFIG_NEW),
        ("\t  case 0x0080: // GET_STATUS (device)\n\t\treply_buffer[0] = 0;",
         "\t  case 0x0080: // GET_STATUS (device)\n#if defined(MSC_INTERFACE)\n\t\treply_buffer[0] = usb_self_powered_no_charge ? 1 : 0;\n#else\n\t\treply_buffer[0] = 0;\n#endif"),
        (USB_C_CLEAR_HALT_OLD, USB_C_CLEAR_HALT_NEW),
        (USB_C_GET_DESC_OLD, USB_C_GET_DESC_NEW),
        (mtp_end, "\t\tbreak;\n" + USB_C_REQUESTS),
        ("uint32_t usb_transfer_status(const transfer_t *transfer)\n{\n#if defined(MTP_INTERFACE)",
         "FLASHMEM uint32_t usb_transfer_status(const transfer_t *transfer)\n{\n#if defined(MTP_INTERFACE) || defined(MSC_INTERFACE)"),
    ]):
        changed.append("usb.c")
    if edit_marked(usbc, "L7_UAC2", [
        ("#if defined(AUDIO_INTERFACE)\n\t  case 0x0B01: // SET_INTERFACE (alternate setting)", USB_C_UAC2),
        ("#ifdef AUDIO_INTERFACE\n\tif (setup.word1 == 0x02010121",
         "#if defined(AUDIO_INTERFACE) && !defined(AUDIO_UAC2) // L7_UAC2\n\tif (setup.word1 == 0x02010121"),
    ]):
        changed.append("usb.c (UAC2)")
    if edit_marked(usbc, "L7_IRQ_SAFE", [
        ("static void endpoint0_setup(uint64_t setupdata);",
         "FLASHMEM __attribute__((noinline)) static void endpoint0_setup(uint64_t setupdata);"),
        ("static void endpoint0_transmit(const void *data, uint32_t len, int notify);",
         "FLASHMEM static void endpoint0_transmit(const void *data, uint32_t len, int notify);"),
        ("static void endpoint0_receive(void *data, uint32_t len, int notify);",
         "FLASHMEM static void endpoint0_receive(void *data, uint32_t len, int notify);"),
        ("static void endpoint0_complete(void);",
         "FLASHMEM __attribute__((noinline)) static void endpoint0_complete(void);"),
        ("static void endpoint0_setup(uint64_t setupdata)\n{",
         "FLASHMEM __attribute__((noinline)) static void endpoint0_setup(uint64_t setupdata)\n{"),
        ("static void endpoint0_transmit(const void *data, uint32_t len, int notify)\n{",
         "FLASHMEM static void endpoint0_transmit(const void *data, uint32_t len, int notify)\n{"),
        ("static void endpoint0_receive(void *data, uint32_t len, int notify)\n{",
         "FLASHMEM static void endpoint0_receive(void *data, uint32_t len, int notify)\n{"),
        ("static void endpoint0_complete(void)\n{",
         "FLASHMEM __attribute__((noinline)) static void endpoint0_complete(void)\n{"),
        ("\tUSBPHY1_CTRL_CLR = USBPHY_CTRL_CLKGATE;\n\tUSBPHY1_PWD = 0;\n\t//printf(\"USBPHY1_PWD=%08lX\\n\", USBPHY1_PWD);\n\t//printf(\"USBPHY1_CTRL=%08lX\\n\", USBPHY1_CTRL);\n\n\tUSB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM;",
         "\tUSBPHY1_CTRL_CLR = USBPHY_CTRL_CLKGATE;\n\tUSBPHY1_CTRL_SET = USBPHY_CTRL_ENUTMILEVEL2 | USBPHY_CTRL_ENUTMILEVEL3;\n\tUSBPHY1_PWD = 0;\n\t//printf(\"USBPHY1_PWD=%08lX\\n\", USBPHY1_PWD);\n\t//printf(\"USBPHY1_CTRL=%08lX\\n\", USBPHY1_CTRL);\n\n\t// L7_IRQ_SAFE: USB_USBMODE_SDIS buffers full packets in hardware FIFO before D+/D- TX,\n\t// preventing mid-packet TX underruns (0xe00002ed XactErr) on USB-C hubs under MIDI + Audio load\n\tUSB1_USBMODE = USB_USBMODE_CM(2) | USB_USBMODE_SLOM | USB_USBMODE_SDIS;"),
        ("\tif (status & USB_USBSTS_UI) {",
         "\tif (status & (USB_USBSTS_UI | USB_USBSTS_UEI)) { // L7_IRQ_SAFE: service ENDPTCOMPLETE on hub XactErr"),
        ("void usb_start_sof_interrupts(int interface)\n{\n\t__disable_irq();\n\tsof_usage |= (1 << interface);\n\tuint32_t intr = USB1_USBINTR;\n\tif (!(intr & USB_USBINTR_SRE)) {\n\t\tUSB1_USBSTS = USB_USBSTS_SRI; // clear prior SOF before SOF IRQ enable\n\t\tUSB1_USBINTR = intr | USB_USBINTR_SRE;\n\t}\n\t__enable_irq();\n}\n\nvoid usb_stop_sof_interrupts(int interface)\n{",
         "void usb_start_sof_interrupts(int interface) // L7_IRQ_SAFE\n{\n\tuint32_t irq_state;\n\t__asm__ volatile(\"mrs %0, PRIMASK\\n\\tcpsid i\" : \"=r\"(irq_state) :: \"memory\");\n\tsof_usage |= (1 << interface);\n\tuint32_t intr = USB1_USBINTR;\n\tif (!(intr & USB_USBINTR_SRE)) {\n\t\tUSB1_USBSTS = USB_USBSTS_SRI; // clear prior SOF before SOF IRQ enable\n\t\tUSB1_USBINTR = intr | USB_USBINTR_SRE;\n\t}\n\tif (!(irq_state & 1)) __enable_irq();\n}\n\nFLASHMEM void usb_stop_sof_interrupts(int interface)\n{"),
        ("\t\tendpoint_queue_head[1].status = 0;\n\t\tUSB1_ENDPTPRIME |= (1<<16);\n\t\twhile (USB1_ENDPTPRIME) ;\n\t}\n\tendpoint0_transfer_ack.next = 1;\n\tendpoint0_transfer_ack.status = (1<<7) | (notify ? (1 << 15) : 0);\n\tendpoint0_transfer_ack.pointer0 = 0;\n\tendpoint_queue_head[0].next = (uint32_t)&endpoint0_transfer_ack;\n\tendpoint_queue_head[0].status = 0;\n\tUSB1_ENDPTCOMPLETE = (1<<0) | (1<<16);\n\tUSB1_ENDPTPRIME |= (1<<0);",
         "\t\tendpoint_queue_head[1].status = 0;\n\t\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\t\tUSB1_ENDPTPRIME = (1<<16);\n\t\twhile (USB1_ENDPTPRIME) ;\n\t}\n\tendpoint0_transfer_ack.next = 1;\n\tendpoint0_transfer_ack.status = (1<<7) | (notify ? (1 << 15) : 0);\n\tendpoint0_transfer_ack.pointer0 = 0;\n\tendpoint_queue_head[0].next = (uint32_t)&endpoint0_transfer_ack;\n\tendpoint_queue_head[0].status = 0;\n\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\tUSB1_ENDPTCOMPLETE = (1<<0) | (1<<16);\n\tUSB1_ENDPTPRIME = (1<<0);"),
        ("\t\tendpoint_queue_head[0].status = 0;\n\t\tUSB1_ENDPTPRIME |= (1<<0);\n\t\twhile (USB1_ENDPTPRIME) ;\n\t}\n\tendpoint0_transfer_ack.next = 1;\n\tendpoint0_transfer_ack.status = (1<<7) | (notify ? (1 << 15) : 0);\n\tendpoint0_transfer_ack.pointer0 = 0;\n\tendpoint_queue_head[1].next = (uint32_t)&endpoint0_transfer_ack;\n\tendpoint_queue_head[1].status = 0;\n\tUSB1_ENDPTCOMPLETE = (1<<0) | (1<<16);\n\tUSB1_ENDPTPRIME |= (1<<16);",
         "\t\tendpoint_queue_head[0].status = 0;\n\t\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\t\tUSB1_ENDPTPRIME = (1<<0);\n\t\twhile (USB1_ENDPTPRIME) ;\n\t}\n\tendpoint0_transfer_ack.next = 1;\n\tendpoint0_transfer_ack.status = (1<<7) | (notify ? (1 << 15) : 0);\n\tendpoint0_transfer_ack.pointer0 = 0;\n\tendpoint_queue_head[1].next = (uint32_t)&endpoint0_transfer_ack;\n\tendpoint_queue_head[1].status = 0;\n\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\tUSB1_ENDPTCOMPLETE = (1<<0) | (1<<16);\n\tUSB1_ENDPTPRIME = (1<<16);"),
        ("static void usb_endpoint_config(endpoint_t *qh, uint32_t config, void (*callback)(transfer_t *))",
         "FLASHMEM static void usb_endpoint_config(endpoint_t *qh, uint32_t config, void (*callback)(transfer_t *))"),
        ("void usb_config_rx(uint32_t ep, uint32_t packet_size, int do_zlp, void (*cb)(transfer_t *))",
         "FLASHMEM void usb_config_rx(uint32_t ep, uint32_t packet_size, int do_zlp, void (*cb)(transfer_t *))"),
        ("void usb_config_tx(uint32_t ep, uint32_t packet_size, int do_zlp, void (*cb)(transfer_t *))",
         "FLASHMEM void usb_config_tx(uint32_t ep, uint32_t packet_size, int do_zlp, void (*cb)(transfer_t *))"),
        ("void usb_config_rx_iso(uint32_t ep, uint32_t packet_size, int mult, void (*cb)(transfer_t *))",
         "FLASHMEM void usb_config_rx_iso(uint32_t ep, uint32_t packet_size, int mult, void (*cb)(transfer_t *))"),
        ("void usb_config_tx_iso(uint32_t ep, uint32_t packet_size, int mult, void (*cb)(transfer_t *))",
         "FLASHMEM void usb_config_tx_iso(uint32_t ep, uint32_t packet_size, int mult, void (*cb)(transfer_t *))"),
        ("\ttransfer->pointer4 = addr + 16384;\n\ttransfer->callback_param = param;\n}",
         "\ttransfer->pointer4 = addr + 16384;\n\ttransfer->callback_param = param;\n\t__asm__ volatile(\"dsb\" ::: \"memory\");\n}"),
        ("\t__disable_irq();\n\t//digitalWriteFast(1, HIGH);\n\t// Executing A Transfer Descriptor, page 2468 (RT1060 manual, Rev 1, 12/2018)\n\ttransfer_t *last = endpoint->last_transfer;\n\tif (last) {\n\t\tlast->next = (uint32_t)transfer;\n\t\tif (USB1_ENDPTPRIME & epmask) goto end;\n\t\t//digitalWriteFast(2, HIGH);\n\t\t//ret |= 0x01;\n\t\tuint32_t status, cyccnt=ARM_DWT_CYCCNT;\n\t\tdo {\n\t\t\tUSB1_USBCMD |= USB_USBCMD_ATDTW;\n\t\t\tstatus = USB1_ENDPTSTATUS;\n\t\t} while (!(USB1_USBCMD & USB_USBCMD_ATDTW) && (ARM_DWT_CYCCNT - cyccnt < 2400));\n\t\t//USB1_USBCMD &= ~USB_USBCMD_ATDTW;\n\t\tif (status & epmask) goto end;\n\t\t//ret |= 0x02;\n\t\tendpoint->next = (uint32_t)transfer;\n\t\tendpoint->status = 0;\n\t\tUSB1_ENDPTPRIME |= epmask;\n\t\tgoto end;\n\t}\n\t//digitalWriteFast(4, HIGH);\n\tendpoint->next = (uint32_t)transfer;\n\tendpoint->status = 0;\n\tUSB1_ENDPTPRIME |= epmask;\n\tendpoint->first_transfer = transfer;\nend:\n\tendpoint->last_transfer = transfer;\n\t__enable_irq();",
         "\tuint32_t irq_state; // L7_IRQ_SAFE\n\t__asm__ volatile(\"mrs %0, PRIMASK\\n\\tcpsid i\" : \"=r\"(irq_state) :: \"memory\");\n\t// Executing A Transfer Descriptor, RT1060 RM 55.5.4.3.2 (with ARM Cortex-M7 DSB barriers,\n\t// ATDTW clear, and first_active dTD recovery so a hub XactErr never orphans active dTDs)\n\ttransfer_t *last = endpoint->last_transfer;\n\tif (last && last != transfer) {\n\t\tlast->next = (uint32_t)transfer;\n\t\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\t\tif (USB1_ENDPTPRIME & epmask) goto end;\n\t\tuint32_t status, cyccnt=ARM_DWT_CYCCNT;\n\t\tdo {\n\t\t\tUSB1_USBCMD |= USB_USBCMD_ATDTW;\n\t\t\tstatus = USB1_ENDPTSTATUS;\n\t\t} while (!(USB1_USBCMD & USB_USBCMD_ATDTW) && (ARM_DWT_CYCCNT - cyccnt < 2400));\n\t\tUSB1_USBCMD &= ~USB_USBCMD_ATDTW;\n\t\tif (status & epmask) goto end;\n\t\ttransfer_t *first_active = endpoint->first_transfer;\n\t\tuint32_t guard = 32;\n\t\twhile (first_active && first_active != transfer && !(first_active->status & 0x80u) && --guard) {\n\t\t\tuint32_t nxt = first_active->next;\n\t\t\tfirst_active = (nxt & 1u) ? transfer : (transfer_t *)nxt;\n\t\t}\n\t\tif (!first_active || !guard) {\n\t\t\tfirst_active = transfer;\n\t\t\tendpoint->first_transfer = transfer;\n\t\t} else if (!endpoint->callback_function) {\n\t\t\tendpoint->first_transfer = first_active;\n\t\t}\n\t\tendpoint->next = (uint32_t)first_active;\n\t\tendpoint->status = 0;\n\t\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\t\tUSB1_ENDPTPRIME = epmask;\n\t\tgoto end;\n\t}\n\tendpoint->next = (uint32_t)transfer;\n\tendpoint->status = 0;\n\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\tUSB1_ENDPTPRIME = epmask;\n\tendpoint->first_transfer = transfer;\nend:\n\tendpoint->last_transfer = transfer;\n\tif (!(irq_state & 1)) __enable_irq();"),
        ("\t// do all the callbacks\n\twhile (count) {\n\t\ttransfer_t *next = (transfer_t *)first->next;\n\t\tep->callback_function(first);\n\t\tfirst = next;\n\t\tcount--;\n\t}\n}",
         "\t// do all the callbacks\n\twhile (count) {\n\t\ttransfer_t *next = (transfer_t *)first->next;\n\t\tep->callback_function(first); // L7_IRQ_SAFE\n\t\tfirst = next;\n\t\tcount--;\n\t}\n\tif (ep->first_transfer != NULL) {\n\t\tuint32_t idx = (uint32_t)(ep - endpoint_queue_head);\n\t\tuint32_t epnum = idx >> 1;\n\t\tuint32_t is_tx = idx & 1u;\n\t\tuint32_t epmask = 1u << (epnum + (is_tx ? 16u : 0u));\n\t\tif (!((USB1_ENDPTPRIME | USB1_ENDPTSTATUS) & epmask)) {\n\t\t\tuint32_t ctrl = *((volatile uint32_t *)&USB1_ENDPTCTRL0 + epnum);\n\t\t\tif (!(ctrl & (is_tx ? USB_ENDPTCTRL_TXS : USB_ENDPTCTRL_RXS))) {\n\t\t\t\ttransfer_t *fa = ep->first_transfer;\n\t\t\t\tif (fa->status & 0x80u) {\n\t\t\t\t\tep->next = (uint32_t)fa;\n\t\t\t\t\tep->status = 0;\n\t\t\t\t\t__asm__ volatile(\"dsb\" ::: \"memory\");\n\t\t\t\t\tUSB1_ENDPTPRIME = epmask;\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}"),
        ("\tuint32_t status, cmd;\n\t//int count=0;\n\tcmd = USB1_USBCMD;\n\twhile (1) {\n\t\t__disable_irq();\n\t\tUSB1_USBCMD = cmd | USB_USBCMD_ATDTW;\n\t\tstatus = transfer->status;\n\t\tcmd = USB1_USBCMD;\n\t\t__enable_irq();\n\t\tif (cmd & USB_USBCMD_ATDTW) return status;",
         "\tuint32_t status, cmd, irq_state; // L7_IRQ_SAFE\n\twhile (1) {\n\t\t__asm__ volatile(\"mrs %0, PRIMASK\\n\\tcpsid i\" : \"=r\"(irq_state) :: \"memory\");\n\t\tUSB1_USBCMD |= USB_USBCMD_ATDTW;\n\t\tstatus = transfer->status;\n\t\tcmd = USB1_USBCMD;\n\t\tUSB1_USBCMD &= ~USB_USBCMD_ATDTW;\n\t\tif (!(irq_state & 1)) __enable_irq();\n\t\tif (cmd & USB_USBCMD_ATDTW) return status;"),
    ]):
        changed.append("usb.c (IRQ safe)")

    if edit_marked(os.path.join(core, "usb_midi.h"), "L7_MIDI_SAFE", [
        ("void usb_midi_configure(void);\n",
         "extern volatile uint32_t usb_midi_reset_epoch; // L7_MIDI_SAFE\nvoid usb_midi_reset(void);\nvoid usb_midi_configure(void);\n"),
    ]):
        changed.append("usb_midi.h")

    if edit_marked(os.path.join(core, "usb_midi.c"), "L7_MIDI_SAFE", [
        ("static transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));\nDMAMEM static uint8_t txbuffer[TX_SIZE * TX_NUM] __attribute__ ((aligned(32)));",
         "#ifndef USB_SERIAL_MSC // L7_MIDI_SAFE: midi_usb.cpp drives EP4 IN via DTCM usbMidiTxBuf\nstatic transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));\nDMAMEM static uint8_t txbuffer[TX_SIZE * TX_NUM] __attribute__ ((aligned(32)));\n#endif\nvolatile uint32_t usb_midi_reset_epoch = 0;"),
        ("DMAMEM static uint8_t rx_buffer[RX_NUM * MIDI_RX_SIZE_480] __attribute__ ((aligned(32)));",
         "#ifdef USB_SERIAL_MSC // L7_MIDI_SAFE: uncached RAM1 (DTCM) RX DMA pool\nstatic uint8_t rx_buffer[RX_NUM * MIDI_RX_SIZE_480] __attribute__ ((aligned(32)));\n#else\nDMAMEM static uint8_t rx_buffer[RX_NUM * MIDI_RX_SIZE_480] __attribute__ ((aligned(32)));\n#endif"),
        ("void usb_midi_configure(void)\n{\n\tprintf(\"usb_midi_configure\\n\");\n\tif (usb_high_speed) {\n\t\ttx_packet_size = MIDI_TX_SIZE_480;\n\t\trx_packet_size = MIDI_RX_SIZE_480;\n\t} else {\n\t\ttx_packet_size = MIDI_TX_SIZE_12;\n\t\trx_packet_size = MIDI_RX_SIZE_12;\n\t}\n\tmemset(tx_transfer, 0, sizeof(tx_transfer));",
         "FLASHMEM void usb_midi_reset(void)\n{\n\trx_head = 0;\n\trx_tail = 0;\n\trx_available = 0;\n\tusb_midi_msg_sysex_len = 0;\n\ttx_head = 0;\n\ttx_available = 0;\n\tusb_midi_reset_epoch = usb_midi_reset_epoch + 1;\n}\n\nFLASHMEM void usb_midi_configure(void)\n{\n\tusb_midi_msg_sysex_len = 0;\n\tusb_midi_reset_epoch = usb_midi_reset_epoch + 1;\n\tprintf(\"usb_midi_configure\\n\");\n\tif (usb_high_speed) {\n\t\ttx_packet_size = MIDI_TX_SIZE_480;\n\t\trx_packet_size = MIDI_RX_SIZE_480;\n\t} else {\n\t\ttx_packet_size = MIDI_TX_SIZE_12;\n\t\trx_packet_size = MIDI_RX_SIZE_12;\n\t}\n#ifndef USB_SERIAL_MSC\n\tmemset(tx_transfer, 0, sizeof(tx_transfer));\n#endif"),
        ("void usb_midi_write_packed(uint32_t n)\n{\n\tprintf(\"usb_midi_write_packed\\n\");",
         "void usb_midi_write_packed(uint32_t n)\n{\n#ifdef USB_SERIAL_MSC\n\t(void)n;\n#else\n\tprintf(\"usb_midi_write_packed\\n\");"),
        ("\ttx_noautoflush = 0;\n}\n\nvoid usb_midi_flush_output(void)\n{",
         "\ttx_noautoflush = 0;\n#endif\n}\n\nvoid usb_midi_flush_output(void)\n{\n#ifndef USB_SERIAL_MSC"),
        ("\t\tusb_stop_sof_interrupts(MIDI_INTERFACE);\n\t}\n}",
         "\t\tusb_stop_sof_interrupts(MIDI_INTERFACE);\n\t}\n#endif\n}"),
        ("static void rx_queue_transfer(int i)\n{\n\tNVIC_DISABLE_IRQ(IRQ_USB1);\n\tvoid *buffer = rx_buffer + i * MIDI_RX_SIZE_480;\n\tusb_prepare_transfer(rx_transfer + i, buffer, rx_packet_size, i);\n\tarm_dcache_delete(buffer, rx_packet_size);\n\tusb_receive(MIDI_RX_ENDPOINT, rx_transfer + i);\n\tNVIC_ENABLE_IRQ(IRQ_USB1);\n}\n\n\n\n// called by USB interrupt when any packet is received\nstatic void rx_event(transfer_t *t)\n{\n\tint len = rx_packet_size - ((t->status >> 16) & 0x7FFF);\n\tlen &= 0xFFFC; // MIDI packets must be multiple of 4 bytes\n\tint i = t->callback_param;\n\tprintf(\"rx event, len=%d, i=%d\\n\", len, i);\n\tif (len > 0) {\n\t\tuint32_t head = rx_head;\n\t\trx_count[i] = len;\n\t\trx_index[i] = 0;\n\t\tif (++head > RX_NUM) head = 0;\n\t\trx_list[head] = i;\n\t\trx_head = head;\n\t\trx_available += len;\n\t} else {\n\t\t// received a zero length packet\n\t\trx_queue_transfer(i);\n\t}\n}\n\n\nuint32_t usb_midi_available(void)\n{\n\treturn rx_available / 4;\n}\n\nuint32_t usb_midi_read_message(void)\n{\n\tuint32_t n = 0;\n\tNVIC_DISABLE_IRQ(IRQ_USB1);\n\tuint32_t tail = rx_tail;\n\tif (tail != rx_head) {\n\t\tif (++tail > RX_NUM) tail = 0;\n\t\tuint32_t i = rx_list[tail];\n\t\t//uint32_t avail = (rx_count[i] - rx_index[i]) / 4;\n\t\tvoid *p = rx_buffer + i * MIDI_RX_SIZE_480 + rx_index[i];\n\t\tn = *(uint32_t *)p;\n\t\trx_available -= 4;\n\t\trx_index[i] += 4;\n\t\tif (rx_index[i] >= rx_count[i]) {\n\t\t\trx_tail = tail;\n\t\t\trx_queue_transfer(i);\n\t\t}\n\t}\n\tNVIC_ENABLE_IRQ(IRQ_USB1);\n\treturn n;\n}",
         "static void rx_queue_transfer(int i) // L7_MIDI_SAFE\n{\n\tvoid *buffer = rx_buffer + i * MIDI_RX_SIZE_480;\n\tusb_prepare_transfer(rx_transfer + i, buffer, rx_packet_size, i);\n#ifndef USB_SERIAL_MSC\n\tarm_dcache_delete(buffer, rx_packet_size);\n#endif\n\tusb_receive(MIDI_RX_ENDPOINT, rx_transfer + i);\n}\n\n// called by USB interrupt when any packet is received\nstatic void rx_event(transfer_t *t)\n{\n\tif (!usb_configuration || !t) return;\n\tconst uint32_t st = t->status;\n\tconst int i = t->callback_param;\n\tif (i < 0 || i >= RX_NUM) return;\n\tif (!(st & 0x68u)) {\n\t\tconst int rem = (st >> 16) & 0x7FFF;\n\t\tconst int len = (rem <= rx_packet_size) ? ((rx_packet_size - rem) & 0xFFFC) : 0;\n\t\tif (len > 0) {\n#ifndef USB_SERIAL_MSC\n\t\t\tarm_dcache_delete(rx_buffer + i * MIDI_RX_SIZE_480, rx_packet_size);\n#endif\n\t\t\tuint32_t head = rx_head;\n\t\t\trx_count[i] = len;\n\t\t\trx_index[i] = 0;\n\t\t\tif (++head > RX_NUM) head = 0;\n\t\t\trx_list[head] = i;\n\t\t\t__asm__ volatile(\"\" ::: \"memory\");\n\t\t\trx_head = head;\n\t\t\trx_available += len;\n\t\t\treturn;\n\t\t}\n\t}\n\tif (usb_configuration) rx_queue_transfer(i);\n}\n\nuint32_t usb_midi_available(void)\n{\n\tuint32_t n = rx_available >> 2;\n\tif (n == 0 && rx_tail != rx_head) return 1;\n\treturn n;\n}\n\nuint32_t usb_midi_read_message(void)\n{\n\tuint32_t n = 0;\n\tuint32_t irq_state;\n\t__asm__ volatile(\"mrs %0, PRIMASK\\n\\tcpsid i\" : \"=r\"(irq_state) :: \"memory\");\n\tuint32_t tail = rx_tail;\n\tif (tail != rx_head) {\n\t\tif (++tail > RX_NUM) tail = 0;\n\t\tuint32_t i = rx_list[tail];\n\t\tvoid *p = rx_buffer + i * MIDI_RX_SIZE_480 + rx_index[i];\n\t\tn = *(uint32_t *)p;\n\t\tif (rx_available >= 4) rx_available -= 4;\n\t\telse rx_available = 0;\n\t\trx_index[i] += 4;\n\t\tif (rx_index[i] >= rx_count[i]) {\n\t\t\trx_tail = tail;\n\t\t\tif (usb_configuration) rx_queue_transfer(i);\n\t\t}\n\t} else {\n\t\trx_available = 0;\n\t}\n\tif (!(irq_state & 1)) __enable_irq();\n\treturn n;\n}"),
    ]):
        changed.append("usb_midi.c")

    if edit_marked(os.path.join(core, "usb_serial.c"), "L7_SERIAL_FLASH", [
        ("static void usb_serial_flush_callback(void);\n\n#define TX_NUM   4\n#define TX_SIZE  2048 /* should be a multiple of CDC_TX_SIZE */\nstatic transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));\nDMAMEM static uint8_t txbuffer[TX_SIZE * TX_NUM] __attribute__ ((aligned(32)));\nstatic uint8_t tx_head=0;\nstatic uint16_t tx_available=0;\nstatic uint16_t tx_packet_size=0;\n\n#define RX_NUM  8",
         "FLASHMEM static void usb_serial_flush_callback(void);\n\n#ifdef USB_SERIAL_MSC // L7_SERIAL_FLASH: compact CDC buffers (frees 9 KB RAM2 + 192 B RAM1)\n#define TX_NUM   2\n#define TX_SIZE  512\n#else\n#define TX_NUM   4\n#define TX_SIZE  2048 /* should be a multiple of CDC_TX_SIZE */\n#endif\nstatic transfer_t tx_transfer[TX_NUM] __attribute__ ((used, aligned(32)));\nDMAMEM static uint8_t txbuffer[TX_SIZE * TX_NUM] __attribute__ ((aligned(32)));\nstatic uint8_t tx_head=0;\nstatic uint16_t tx_available=0;\nstatic uint16_t tx_packet_size=0;\n\n#ifdef USB_SERIAL_MSC\n#define RX_NUM  4\n#else\n#define RX_NUM  8\n#endif"),
        ("void usb_serial_reset(void)\n{\n\tprintf(\"usb_serial_reset\\n\");\n\t// deallocate all transfer descriptors\n}\n\nvoid usb_serial_configure(void)\n{",
         "FLASHMEM void usb_serial_reset(void)\n{\n\tprintf(\"usb_serial_reset\\n\");\n\tUSB1_GPTIMER0CTRL = 0;\n\trx_head = 0;\n\trx_tail = 0;\n\trx_available = 0;\n\ttx_head = 0;\n\ttx_available = 0;\n\ttx_noautoflush = 0;\n}\n\nFLASHMEM void usb_serial_configure(void)\n{"),
        ("static void rx_queue_transfer(int i)\n{\n\tNVIC_DISABLE_IRQ(IRQ_USB1);\n\tprintf(\"rx queue i=%d\\n\", i);\n\tvoid *buffer = rx_buffer + i * CDC_RX_SIZE_480;\n\tusb_prepare_transfer(rx_transfer + i, buffer, rx_packet_size, i);\n\tarm_dcache_delete(buffer, rx_packet_size);\n\tusb_receive(CDC_RX_ENDPOINT, rx_transfer + i);\n\tNVIC_ENABLE_IRQ(IRQ_USB1);\n}\n\n// called by USB interrupt when any packet is received\nstatic void rx_event(transfer_t *t)\n{\n\tint len = rx_packet_size - ((t->status >> 16) & 0x7FFF);\n\tint i = t->callback_param;\n\tprintf(\"rx event, len=%d, i=%d\\n\", len, i);\n\tif (len > 0) {\n\t\t// received a packet with data\n\t\tuint32_t head = rx_head;\n\t\tif (head != rx_tail) {\n\t\t\t// a previous packet is still buffered\n\t\t\tuint32_t ii = rx_list[head];\n\t\t\tuint32_t count = rx_count[ii];\n\t\t\tif (len <= CDC_RX_SIZE_480 - count) {\n\t\t\t\t// previous buffer has enough free space for this packet's data\n\t\t\t\tmemcpy(rx_buffer + ii * CDC_RX_SIZE_480 + count,\n\t\t\t\t\trx_buffer + i * CDC_RX_SIZE_480, len);\n\t\t\t\trx_count[ii] = count + len;\n\t\t\t\trx_available += len;\n\t\t\t\trx_queue_transfer(i);\n\t\t\t\t// TODO: trigger serialEvent\n\t\t\t\treturn;\n\t\t\t}\n\t\t}\n\t\t// add this packet to rx_list\n\t\trx_count[i] = len;\n\t\trx_index[i] = 0;\n\t\tif (++head > RX_NUM) head = 0;\n\t\trx_list[head] = i;\n\t\trx_head = head;\n\t\trx_available += len;\n\t\t// TODO: trigger serialEvent\n\t} else {\n\t\t// received a zero length packet\n\t\trx_queue_transfer(i);\n\t}\n}\n\n//static int maxtimes=0;\n\n// read a block of bytes to a buffer\nint usb_serial_read(void *buffer, uint32_t size)\n{\n\tuint8_t *p = (uint8_t *)buffer;\n\tuint32_t count=0;\n\n\tNVIC_DISABLE_IRQ(IRQ_USB1);\n\t//if (++maxtimes > 15) while (1) ;\n\tuint32_t tail = rx_tail;\n\t//printf(\"usb_serial_read, size=%d, tail=%d, head=%d\\n\", size, tail, rx_head);\n\twhile (count < size && tail != rx_head) {\n\t\tif (++tail > RX_NUM) tail = 0;\n\t\tuint32_t i = rx_list[tail];\n\t\tuint32_t len = size - count;\n\t\tuint32_t avail = rx_count[i] - rx_index[i];\n\t\t //printf(\"usb_serial_read, count=%d, size=%d, i=%d, index=%d, len=%d, avail=%d, c=%c\\n\",\n\t\t  //count, size, i, rx_index[i], len, avail, rx_buffer[i * CDC_RX_SIZE_480]);\n\t\tif (avail > len) {\n\t\t\t// partially consume this packet\n\t\t\tmemcpy(p, rx_buffer + i * CDC_RX_SIZE_480 + rx_index[i], len);\n\t\t\trx_available -= len;\n\t\t\trx_index[i] += len;\n\t\t\tcount += len;\n\t\t} else {\n\t\t\t// fully consume this packet\n\t\t\tmemcpy(p, rx_buffer + i * CDC_RX_SIZE_480 + rx_index[i], avail);\n\t\t\tp += avail;\n\t\t\trx_available -= avail;\n\t\t\tcount += avail;\n\t\t\trx_tail = tail;\n\t\t\trx_queue_transfer(i);\n\t\t}\n\t}\n\tNVIC_ENABLE_IRQ(IRQ_USB1);\n\treturn count;\n}\n\n// peek at the next character, or -1 if nothing received\nint usb_serial_peekchar(void)\n{\n\tuint32_t tail = rx_tail;\n\tif (tail == rx_head) return -1;\n\tif (++tail > RX_NUM) tail = 0;\n\tuint32_t i = rx_list[tail];\n\treturn rx_buffer[i * CDC_RX_SIZE_480 + rx_index[i]];\n}\n\n// number of bytes available in the receive buffer\nint usb_serial_available(void)\n{\n\tuint32_t n = rx_available;\n\tif (n == 0) yield();\n\treturn n;\n}\n\n// discard any buffered input\nvoid usb_serial_flush_input(void)\n{\n\tuint32_t tail = rx_tail;\n\twhile (tail != rx_head) {\n\t\tif (++tail > RX_NUM) tail = 0;\n\t\tuint32_t i = rx_list[tail];\n\t\trx_available -= rx_count[i] - rx_index[i];\n\t\trx_queue_transfer(i);\n\t\trx_tail = tail;\n\t}\n}\n\n\n// get the next character, or -1 if nothing received\nint usb_serial_getchar(void)",
         "static void rx_queue_transfer(int i)\n{\n\tvoid *buffer = rx_buffer + i * CDC_RX_SIZE_480;\n\tusb_prepare_transfer(rx_transfer + i, buffer, rx_packet_size, i);\n\tarm_dcache_delete(buffer, rx_packet_size);\n\tusb_receive(CDC_RX_ENDPOINT, rx_transfer + i);\n}\n\n// called by USB interrupt when any packet is received\nFLASHMEM static void rx_event(transfer_t *t)\n{\n\tif (!usb_configuration || !t) return;\n\tconst uint32_t st = t->status;\n\tconst int i = t->callback_param;\n\tif (i < 0 || i >= RX_NUM) return;\n\tif (!(st & 0x68u)) {\n\t\tconst int rem = (st >> 16) & 0x7FFF;\n\t\tconst int len = (rem <= rx_packet_size) ? (rx_packet_size - rem) : 0;\n\t\tif (len > 0) {\n\t\t\tarm_dcache_delete(rx_buffer + i * CDC_RX_SIZE_480, rx_packet_size);\n\t\t\tuint32_t head = rx_head;\n\t\t\tif (head != rx_tail) {\n\t\t\t\tuint32_t ii = rx_list[head];\n\t\t\t\tuint32_t count = rx_count[ii];\n\t\t\t\tif ((uint32_t)len <= CDC_RX_SIZE_480 - count) {\n\t\t\t\t\tmemcpy(rx_buffer + ii * CDC_RX_SIZE_480 + count,\n\t\t\t\t\t\trx_buffer + i * CDC_RX_SIZE_480, len);\n\t\t\t\t\trx_count[ii] = count + len;\n\t\t\t\t\trx_available += len;\n\t\t\t\t\tif (usb_configuration) rx_queue_transfer(i);\n\t\t\t\t\treturn;\n\t\t\t\t}\n\t\t\t}\n\t\t\trx_count[i] = len;\n\t\t\trx_index[i] = 0;\n\t\t\tif (++head > RX_NUM) head = 0;\n\t\t\trx_list[head] = i;\n\t\t\t__asm__ volatile(\"\" ::: \"memory\");\n\t\t\trx_head = head;\n\t\t\trx_available += len;\n\t\t\treturn;\n\t\t}\n\t}\n\tif (usb_configuration) rx_queue_transfer(i);\n}\n\nFLASHMEM int usb_serial_read(void *buffer, uint32_t size)\n{\n\tuint8_t *p = (uint8_t *)buffer;\n\tuint32_t count = 0;\n\tuint32_t irq_state;\n\t__asm__ volatile(\"mrs %0, PRIMASK\\n\\tcpsid i\" : \"=r\"(irq_state) :: \"memory\");\n\tuint32_t tail = rx_tail;\n\twhile (count < size && tail != rx_head) {\n\t\tif (++tail > RX_NUM) tail = 0;\n\t\tuint32_t i = rx_list[tail];\n\t\tuint32_t len = size - count;\n\t\tuint32_t avail = rx_count[i] - rx_index[i];\n\t\tif (avail > len) {\n\t\t\tmemcpy(p, rx_buffer + i * CDC_RX_SIZE_480 + rx_index[i], len);\n\t\t\trx_available -= len;\n\t\t\trx_index[i] += len;\n\t\t\tcount += len;\n\t\t} else {\n\t\t\tmemcpy(p, rx_buffer + i * CDC_RX_SIZE_480 + rx_index[i], avail);\n\t\t\tp += avail;\n\t\t\trx_available -= avail;\n\t\t\tcount += avail;\n\t\t\trx_tail = tail;\n\t\t\tif (usb_configuration) rx_queue_transfer(i);\n\t\t}\n\t}\n\tif (!(irq_state & 1)) __enable_irq();\n\treturn count;\n}\n\nFLASHMEM int usb_serial_peekchar(void)\n{\n\tuint32_t tail = rx_tail;\n\tif (tail == rx_head) return -1;\n\tif (++tail > RX_NUM) tail = 0;\n\tuint32_t i = rx_list[tail];\n\treturn rx_buffer[i * CDC_RX_SIZE_480 + rx_index[i]];\n}\n\nFLASHMEM int usb_serial_available(void)\n{\n\tuint32_t n = rx_available;\n\tif (n == 0) yield();\n\treturn n;\n}\n\nFLASHMEM void usb_serial_flush_input(void)\n{\n\tuint32_t irq_state;\n\t__asm__ volatile(\"mrs %0, PRIMASK\\n\\tcpsid i\" : \"=r\"(irq_state) :: \"memory\");\n\tuint32_t tail = rx_tail;\n\twhile (tail != rx_head) {\n\t\tif (++tail > RX_NUM) tail = 0;\n\t\tuint32_t i = rx_list[tail];\n\t\trx_available -= rx_count[i] - rx_index[i];\n\t\tif (usb_configuration) rx_queue_transfer(i);\n\t\trx_tail = tail;\n\t}\n\tif (!(irq_state & 1)) __enable_irq();\n}\n\nFLASHMEM int usb_serial_getchar(void)"),
        ("int usb_serial_putchar(uint8_t c)\n{",
         "FLASHMEM int usb_serial_putchar(uint8_t c)\n{"),
        ("int usb_serial_write(const void *buffer, uint32_t size)\n{",
         "FLASHMEM int usb_serial_write(const void *buffer, uint32_t size)\n{"),
        ("int usb_serial_write_buffer_free(void)\n{",
         "FLASHMEM int usb_serial_write_buffer_free(void)\n{"),
        ("void usb_serial_flush_output(void)\n{",
         "FLASHMEM void usb_serial_flush_output(void)\n{"),
        ("static void usb_serial_flush_callback(void)\n{",
         "FLASHMEM static void usb_serial_flush_callback(void)\n{"),
    ]):
        changed.append("usb_serial.c")

    if edit_marked(os.path.join(core, "usb_audio.h"), "L7_UAC2", [
        ('#include "usb_desc.h"\n#ifdef AUDIO_INTERFACE\n',
         '#include "usb_desc.h"\n#include "usb_audio_l7.h" // L7_UAC2\n#if defined(AUDIO_INTERFACE) && !defined(AUDIO_UAC2)\n'),
    ]):
        changed.append("usb_audio.h")
    if edit_marked(os.path.join(core, "usb_audio.cpp"), "L7_UAC2", [
        ('#ifdef AUDIO_INTERFACE\n', '#if defined(AUDIO_INTERFACE) && !defined(AUDIO_UAC2) // L7_UAC2: usb_audio_l7.cpp instead\n'),
    ]):
        changed.append("usb_audio.cpp")

    if edit(os.path.join(plat, "libraries", "SdFat", "src", "common", "FsStructs.h"),
            [("  return c < 0X20 || c == '\"'", "  return (c < 0X20 && c != 0X0D) /* L7_MSC: a CR is fine */ || c == '\"'")]):
        changed.append("FsStructs.h")
    if edit(os.path.join(plat, "libraries", "SdFat", "src", "common", "FsName.cpp"),
            [("    if (cp <= 0XFFFF) {\n      rtn = cp;", "    if (cp <= 0XFFFF) {  // L7_MSC: a CR is stored as macOS does, U+F00D\n      rtn = cp == 0X0D ? 0XF00D : cp;")]):
        changed.append("FsName.cpp")

    if edit(os.path.join(plat, "libraries", "SdFat", "src", "SdCard", "SdioTeensy.cpp"), [
        ("  SDHC_BLKATTR = SDHC_BLKATTR_BLKCNT(n) | SDHC_BLKATTR_BLKSIZE(512);\n"
         "  SDHC_IRQSIGEN = SDHC_IRQSIGEN_MASK;\n"
         "  if (!cardCommand(xfertyp, m_highCapacity ? sector : 512*sector)) {\n"
         "    return false;\n"
         "  }\n"
         "  return waitDmaStatus();\n",
         "  SDHC_BLKATTR = SDHC_BLKATTR_BLKCNT(n) | SDHC_BLKATTR_BLKSIZE(512);\n"
         "  // L7_MSC: the interrupt on only once the command is through (see tools/teensy_msc/patch.py)\n"
         "  if (!cardCommand(xfertyp, m_highCapacity ? sector : 512*sector)) {\n"
         "    return false;\n"
         "  }\n"
         "  noInterrupts();\n"
         "  if (m_irqstat & (SDHC_IRQSTAT_TC | SDHC_IRQSTAT_ERROR)) {  // (done already)\n"
         "#if defined(__IMXRT1062__)\n"
         "    SDHC_MIX_CTRL &= ~(SDHC_MIX_CTRL_AC23EN | SDHC_MIX_CTRL_DMAEN);\n"
         "#endif\n"
         "    m_dmaBusy = false;\n"
         "  } else {\n"
         "    SDHC_IRQSIGEN = SDHC_IRQSIGEN_MASK;\n"
         "  }\n"
         "  interrupts();\n"
         "  return waitDmaStatus();\n"),
    ]):
        changed.append("SdioTeensy.cpp")

    boards = os.path.join(plat, "boards.txt")
    b = open(boards).read()
    if MARK not in b:
        backup(boards)
        anchor = ("teensy40.menu.usb.serialmtp=Serial + MTP Disk (Experimental)\n"
                  "teensy40.menu.usb.serialmtp.build.usbtype=USB_MTPDISK_SERIAL\n"
                  "teensy40.menu.usb.serialmtp.upload_port.usbtype=USB_MTPDISK_SERIAL\n")
        if anchor not in b:
            sys.exit("boards.txt: anchor not found")
        b = b.replace(anchor, anchor +
                      "# L7_MSC: tools/teensy_msc/patch.py\n"
                      "teensy40.menu.usb.serialmsc=Serial + MIDI + Audio + Mass Storage (L7 patch)\n"
                      "teensy40.menu.usb.serialmsc.build.usbtype=USB_SERIAL_MSC\n"
                      "teensy40.menu.usb.serialmsc.upload_port.usbtype=USB_MIDI_AUDIO_SERIAL\n", 1)
        open(boards, "w").write(b)
        changed.append("boards.txt")
    b = open(boards).read()
    if MARK_MM not in b:
        if not os.path.exists(boards + ".l7orig"):
            backup(boards)
        anchor = ("teensyMM.menu.usb.serialmtp=Serial + MTP Disk (Experimental)\n"
                  "teensyMM.menu.usb.serialmtp.build.usbtype=USB_MTPDISK_SERIAL\n"
                  "teensyMM.menu.usb.serialmtp.upload_port.usbtype=USB_MTPDISK_SERIAL\n")
        if anchor not in b:
            sys.exit("boards.txt: teensyMM anchor not found")
        b = b.replace(anchor, anchor +
                      MARK_MM + "\n"
                      "teensyMM.menu.usb.serialmsc=Serial + MIDI + Audio + Mass Storage (L7 patch)\n"
                      "teensyMM.menu.usb.serialmsc.build.usbtype=USB_SERIAL_MSC\n"
                      "teensyMM.menu.usb.serialmsc.upload_port.usbtype=USB_MIDI_AUDIO_SERIAL\n", 1)
        open(boards, "w").write(b)
        changed.append("boards.txt (MicroMod)")

    if cur_stamp != want_stamp:
        open(stamp_path, "w").write(want_stamp + "\n")
    if changed:
        build_root = os.path.normpath(os.path.join(HERE, "..", "..", "build"))
        for pat in ("*/core/usb_*.o", "*/core/usb_*.d", "*/core/core.a"):
            for stale in glob.glob(os.path.join(build_root, pat)):
                try:
                    os.remove(stale)
                except OSError:
                    pass
    print("patched:", ", ".join(changed) if changed else "nothing to do (already patched)", "-", plat)


def undo():
    plat = find_platform()
    core = os.path.join(plat, "cores", "teensy4")
    for rel in PATCHED_RELS:
        path = os.path.join(plat, rel)
        if os.path.exists(path + ".l7orig"):
            shutil.move(path + ".l7orig", path)
            print("restored", rel)
    for name in CORE_FILES:
        if os.path.exists(os.path.join(core, name)):
            os.remove(os.path.join(core, name))
            print("removed", name)
    stamp_path = os.path.join(core, STAMP_NAME)
    if os.path.exists(stamp_path):
        os.remove(stamp_path)


if __name__ == "__main__":
    undo() if "--undo" in sys.argv else apply()

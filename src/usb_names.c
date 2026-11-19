// usb_names.c — USB manufacturer, product name ("Capstan"), and hardware-unique serial number
// Populates "CAPSTAN-01-<8HEX>" (Model:01) or "CAPSTAN-02-<8HEX>" (Model:02) from HW_OCOTP_MAC0
// inside startup_middle_hook() before usb_init() attaches to the USB bus.
#include "usb_names.h"
#include "imxrt.h"
#include "avr/pgmspace.h"

#define MANUFACTURER_NAME_CHARS {'C', 'a', 'p', 's', 't', 'a', 'n'}
#define MANUFACTURER_NAME_LEN   7

#define PRODUCT_NAME_CHARS      {'C', 'a', 'p', 's', 't', 'a', 'n'}
#define PRODUCT_NAME_LEN        7

#if defined(ARDUINO_TEENSY_MICROMOD)
#define SERIAL_MODEL_DIGIT      '2'
#else
#define SERIAL_MODEL_DIGIT      '1'
#endif

#define SERIAL_PREFIX_LEN       11
#define SERIAL_HWID_LEN         8
#define SERIAL_NUMBER_LEN       (SERIAL_PREFIX_LEN + SERIAL_HWID_LEN)

PROGMEM struct usb_string_descriptor_struct usb_string_manufacturer_name = {
  2 + MANUFACTURER_NAME_LEN * 2,
  3,
  MANUFACTURER_NAME_CHARS
};

PROGMEM struct usb_string_descriptor_struct usb_string_product_name = {
  2 + PRODUCT_NAME_LEN * 2,
  3,
  PRODUCT_NAME_CHARS
};

struct usb_string_descriptor_struct usb_string_serial_number = {
  2 + SERIAL_NUMBER_LEN * 2,
  3,
  {
    'C', 'A', 'P', 'S', 'T', 'A', 'N', '-', '0', SERIAL_MODEL_DIGIT, '-',
    '0', '0', '0', '0', '0', '0', '0', '0'
  }
};

FLASHMEM void startup_middle_hook(void) {
  uint32_t hwid = HW_OCOTP_MAC0;
  for (int i = SERIAL_HWID_LEN - 1; i >= 0; i--) {
    uint32_t nib = hwid & 0x0Fu;
    usb_string_serial_number.wString[SERIAL_PREFIX_LEN + i] =
        (uint16_t)(nib < 10u ? ('0' + nib) : ('A' + nib - 10u));
    hwid >>= 4;
  }
}

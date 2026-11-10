// usb_names.c — USB device manufacturer, product name ("Capstan"), and serial number
#include "usb_names.h"

#define MANUFACTURER_NAME_CHARS {'C', 'a', 'p', 's', 't', 'a', 'n'}
#define MANUFACTURER_NAME_LEN   7

#define PRODUCT_NAME_CHARS      {'C', 'a', 'p', 's', 't', 'a', 'n'}
#define PRODUCT_NAME_LEN        7

#if defined(ARDUINO_TEENSY_MICROMOD)
#define SERIAL_NUMBER_CHARS     {'M', '8', '-', 'C', '1', '4', '-', '0', '2'}
#else
#define SERIAL_NUMBER_CHARS     {'M', '8', '-', 'C', '1', '4', '-', '0', '1'}
#endif
#define SERIAL_NUMBER_LEN       9

struct usb_string_descriptor_struct usb_string_manufacturer_name = {
  2 + MANUFACTURER_NAME_LEN * 2,
  3,
  MANUFACTURER_NAME_CHARS
};

struct usb_string_descriptor_struct usb_string_product_name = {
  2 + PRODUCT_NAME_LEN * 2,
  3,
  PRODUCT_NAME_CHARS
};

struct usb_string_descriptor_struct usb_string_serial_number = {
  2 + SERIAL_NUMBER_LEN * 2,
  3,
  SERIAL_NUMBER_CHARS
};

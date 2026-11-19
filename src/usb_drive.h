// usb_drive.h — USB Mass Storage Drive mode: exposes the M8 SD card to a host
// computer over USB as a standard removable block device.
//
// Requires the composite USB type installed by tools/teensy_msc/patch.py
// (arduino-cli: teensy:avr:teensy40:usb=serialmsc / teensy:avr:teensyMM:usb=serialmsc).
// Normal operation enumerates as Serial + MIDI + UAC2 Audio only (PID 0x048A, zero MSC
// interface) so host OS disk/indexing daemons never touch Capstan during audio/MIDI.
//
// Entering usbDriveMode() stops transport, flushes open headers, dynamically re-enumerates
// USB into a dedicated Serial + Mass Storage device (PID 0x04D5) until the drive is ejected
// on the host computer or any hardware key is pressed, then re-enumerates back to Normal Mode
// (PID 0x048A) and reloads session state from disk via usbDriveReload().
#pragma once

namespace looper {

// Called while in storage mode to service hardware watchdogs and the power button.
extern void (*usbDriveTick)();

// Reloads system and song state from the SD card upon exiting USB storage mode.
extern void (*usbDriveReload)();

// Enters interactive USB Mass Storage mode; returns false if built without USB_SERIAL_MSC.
bool usbDriveMode();

}  // namespace looper

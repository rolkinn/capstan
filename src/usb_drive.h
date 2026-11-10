// usb_drive.h — USB Mass Storage Drive mode: exposes the M8 SD card to a host
// computer over USB as a standard removable block device.
//
// Requires the composite USB type installed by tools/teensy_msc/patch.py
// (arduino-cli: teensy:avr:teensy40:usb=serialmsc / teensy:avr:teensyMM:usb=serialmsc;
// Arduino IDE: Tools > USB Type > "Serial + MIDI + Audio + Mass Storage (Capstan)").
// Outside storage mode, background SCSI probes from the host OS are answered as an
// empty card reader from a 2 ms timer so the composite USB connection never resets.
//
// Entering usbDriveMode() stops transport, flushes open headers, and mounts the SD
// card until the drive is ejected on the host computer or any hardware key is pressed,
// then reloads the active session state from disk via usbDriveReload().
#pragma once

namespace looper {

// Called while in storage mode to service hardware watchdogs and the power button.
extern void (*usbDriveTick)();

// Reloads system and song state from the SD card upon exiting USB storage mode.
extern void (*usbDriveReload)();

// Starts the background empty-reader SCSI responder timer at boot.
void usbDriveBegin();

// Enters interactive USB Mass Storage mode; returns false if built without USB_SERIAL_MSC.
bool usbDriveMode();

}  // namespace looper

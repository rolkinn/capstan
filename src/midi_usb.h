// midi_usb.h — USB MIDI Clock & Channel Voice I/O + Clean USB Shutdown Detach
#pragma once

void midiUsbBegin();
void midiUsbEnd();
void usbShutdownDetach();
bool usbIsHostConnected();
void usbSetChargeDescriptorFlag(bool chargeWhileOn);
void usbReenumerateChargeDescriptor(bool chargeWhileOn, void (*tickFn)() = nullptr);

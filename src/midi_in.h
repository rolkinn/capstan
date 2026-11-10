// midi_in.h — Serial & USB MIDI input parser + External MIDI Clock follower
#pragma once
#include <stdint.h>

class HardwareSerial;

enum MidiSource : uint8_t { MIDI_SERIAL = 0, MIDI_USB = 1 };

void midiInBegin(HardwareSerial *port);
void midiInByte(uint8_t b, uint32_t us, MidiSource from = MIDI_SERIAL);
void midiInSysEx(const uint8_t *data, uint16_t len, uint32_t us, MidiSource from = MIDI_SERIAL);
void midiInSuspend(bool off);
void midiInSongPosition(uint16_t pos16, uint32_t us, MidiSource from);
void midiInService();
void midiInResetClockState();
bool midiClockFollowing();
int midiFollowSource();

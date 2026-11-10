// input.h — key handling for the 9-Page 8-Track Audio + 8-Track MIDI Tape Recorder
#pragma once
#include <stdint.h>

void pollKeys();
bool inputOptHeld();
bool inputEditHeld();
int inputOptHeaderHighlightCell();
void inputSetMidiEditHeld(bool held, bool usedAsMod);
bool inputIsMidiEditUsedAsModifier();
bool inputFinishMidiEditHold();
bool inputStartMidiEditHoldAction();
uint32_t inputCurrentRepeatIntervalMs();
void inputSaveNavSnapshot();
void inputRestoreNavSnapshot(uint8_t dirBit);
void inputTriggerNav(uint8_t dirBit, bool withShift);
void inputTriggerMidiEditTap();
void inputTriggerMidiOptEdit();

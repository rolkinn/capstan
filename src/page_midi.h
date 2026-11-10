// page_midi.h — MIDI Recording Page (8 MIDI Tracks + Transport + Piano Roll Note Editor) UI & Input
#pragma once
#include "pages.h"

const PageOps &pageMidiOps();
void midiSyncCursorFromAudio();
void midiRememberTrackCursor();
void midiRestoreLastTrackCursor();

// MIDI Note Sequence Editor (1.5-Octave Piano Roll + 1/32nd Timeline Grid Overlay)
bool isMidiNoteEditorOpen();
void openMidiNoteEditor(int track);
void closeMidiNoteEditor();
void invalidateMidiNoteEditorCache();
void drawMidiNoteEditorOverlay(bool full);
void midiNoteEditorDir(uint8_t dir, bool shiftHeld);
void midiNoteEditorEditPress(bool shiftHeld);
void midiNoteEditorResetPress();
void midiNoteEditorCommitIfPlacing();
void serviceMidiPendingWaveMute(uint8_t currentKeys, bool editUsedAsMod);

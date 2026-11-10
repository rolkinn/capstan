// page_audio.h — Audio Recording Page (8 Audio Tracks: 4 Stereo + 4 Mono + Transport) UI & Input
#pragma once
#include "pages.h"

const PageOps &pageAudioOps();
void audioSyncCursorFromMidi();
void audioRememberTrackCursor();
void audioRestoreLastTrackCursor();

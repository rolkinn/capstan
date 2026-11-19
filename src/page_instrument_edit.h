// page_instrument_edit.h — Synthesizer Parameter Editor Page UI & Input
#pragma once
#include "pages.h"

constexpr int INST_EDIT_VIEWPORT_TOP_Y    = SY(54);
constexpr int INST_EDIT_VIEWPORT_BOTTOM_Y = SCREEN_H - SY(2);
constexpr int INST_EDIT_MAX_VIS_BODY_ROWS = 5;

const PageOps &pageInstrumentEditOps();
void drawSamplerBrowserModal(bool full);
void drawSf2BrowserModal(bool full);
void drawInstBrowserModal(bool full);
void drawInstSaveToModal(bool full);
void invalidateInstBrowserCache();

bool isSamplerKeyModalOpen();
uint8_t samplerKeyModalState();
void openSamplerKeyModal(int instIdx, int keyIdx);
void closeSamplerKeyModal();
void samplerKeyModalMove(int delta);
void samplerKeyModalConfirm();
void drawSamplerKeyModal(bool full);

bool isInstMidiMapOpen();
void closeInstMidiMap();
bool cancelInstMidiMapLearnOrConfirm();

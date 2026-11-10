// page_instrument_edit.h — Synthesizer Parameter Editor Page UI & Input
#pragma once
#include "pages.h"

const PageOps &pageInstrumentEditOps();
void drawSamplerBrowserModal(bool full);
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

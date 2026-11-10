// page_system_settings.h — System Settings Page UI & Input
#pragma once
#include "pages.h"

const PageOps &pageSystemSettingsOps();
void invalidateSysPresetsModalCache();
void drawSysPresetsModal(bool full);
void drawSysPresetDeleteConfirmModal(bool full);

bool isUsbSavePromptModalOpen();
void openUsbSavePromptModal();
void closeUsbSavePromptModal();
void usbSavePromptModalMove(int delta);
void usbSavePromptModalConfirm();
void drawUsbSavePromptModal(bool full);

bool isSysProjectsModalOpen();
uint8_t sysProjectsModalState();
bool isSysProjectsConfirmOpen();
bool isSysProjectPlayLocked();
void toggleSysProjectPlayLock();
void closeSysProjectsModal();
void drawSysProjectsConfirmModal(bool full);
void handleSysProjectsModalInput(uint8_t pressed, uint8_t now, uint32_t ms);
void commitProjectKeyboard(const char *enteredName, bool isRename);
void sysProjectPlayService();

bool isSysMidiMapModalOpen();
void openSysMidiMapModal();
void closeSysMidiMapModal();
void handleSysMidiMapModalInput(uint8_t pressed, uint8_t now, uint32_t ms);

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

bool isSysPlaylistsModalOpen();
uint8_t sysPlaylistsModalState();
bool isSysPlaylistsConfirmOpen();
bool isSysPlaylistPlayLocked();
void toggleSysPlaylistPlayLock();
void closeSysPlaylistsModal();
void drawSysPlaylistsConfirmModal(bool full);
void handleSysPlaylistsModalInput(uint8_t pressed, uint8_t now, uint32_t ms);
void commitPlaylistKeyboard(const char *enteredName, bool isRename);
void sysPlaylistPlayService();
void sysPlaylistPlayOnOutputSwitched(bool phonesConnected);
void sysPlaylistPlayOnMasterVolChanged(uint8_t vol);
void sysPlaylistPlayOnSpeedChanged(int8_t speedQuarter);

bool isSysMidiMapModalOpen();
void openSysMidiMapModal();
void closeSysMidiMapModal();
void handleSysMidiMapModalInput(uint8_t pressed, uint8_t now, uint32_t ms);

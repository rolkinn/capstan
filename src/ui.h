// ui.h — 9-Page UI layout and rendering for the M8 8-Track Audio + 8-Track MIDI Tape Recorder
#pragma once
#include "app.h"

#if defined(ARDUINO_TEENSY_MICROMOD)
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 320;
constexpr int PX = 2;
constexpr int FONT_SCALE = 1;
constexpr int FONT_GAP = 3;
constexpr int FONT_ROWS = 13;
constexpr int FONT_H = 13;
#else
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int PX = 2;
constexpr int FONT_SCALE = 2;
constexpr int FONT_GAP = 2;
constexpr int FONT_ROWS = 5;
constexpr int FONT_H = 5 * PX;
#endif

constexpr int SX(int x) { return (x * SCREEN_W) / 320; }
constexpr int SY(int y) { return (y * SCREEN_H) / 240; }
constexpr int TY(int boxH) { return (boxH - FONT_H) / 2; }

// Font helpers (ui_font.cpp)
int textWidth(const char *text);
void drawText(int x, int y, const char *text, uint16_t ink, uint16_t bg, int width = 0);

// Splash screen helpers (ui_splash.cpp)
void showSplash();
void hideSplash();
void splashIntro();
void splashTick();
void splashProgress(const char *msg);
void drawTapeDeckGraphic(int x, int y, int w, int h, uint32_t ms, uint16_t logoCol, uint8_t fadeWeight = 255, const char *labelText = nullptr, bool showRecDot = false, bool reelsOnly = false);

// Battery icon & screen refresh (ui.cpp)
extern uint32_t uiGeneration;
void drawBatteryIcon(bool all, uint16_t ink, uint16_t ground);
void uiRefresh();
void uiForceRedraw();
void uiShowToast(const char *msg, uint32_t durationMs = 3000);
void noteUserActivity();
bool isScreenBlanked();

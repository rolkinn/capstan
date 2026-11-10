// colors.h — Cohesive High-Contrast Black, Gray & Pale Green Palette
#pragma once
#include <stdint.h>

// Blends two RGB565 colours: a at weight w (0-255), b at 255 - w.
constexpr uint16_t blend565(uint16_t a, uint16_t b, uint8_t w) {
  return (uint16_t)(((((a >> 11) & 31) * w + ((b >> 11) & 31) * (255 - w)) / 255) << 11 |
                    ((((a >> 5) & 63) * w + ((b >> 5) & 63) * (255 - w)) / 255) << 5 |
                    (((a & 31) * w + (b & 31) * (255 - w)) / 255));
}

// RGB565 from 8-bit red, green, blue.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
}

// Base blacks, slate-grays, and high-contrast ink
constexpr uint16_t GREY_0   = rgb565(0, 0, 0);
constexpr uint16_t GREY_12  = rgb565(20, 24, 22);
constexpr uint16_t GREY_17  = rgb565(36, 44, 40);
constexpr uint16_t GREY_30  = rgb565(74, 86, 80);
constexpr uint16_t GREY_44  = rgb565(118, 132, 124);
constexpr uint16_t GREY_81  = rgb565(196, 210, 202);
constexpr uint16_t GREY_100 = rgb565(246, 252, 248);

constexpr uint16_t BG    = GREY_0;
constexpr uint16_t PANEL = GREY_12;
constexpr uint16_t DARK  = GREY_17;
constexpr uint16_t MID   = GREY_30;
constexpr uint16_t DIM   = GREY_44;
constexpr uint16_t LIGHT = GREY_81;
constexpr uint16_t INK   = GREY_100;

// Cohesive pale-green primary accent
constexpr uint16_t ACCENT      = rgb565(168, 228, 188);
constexpr uint16_t ACCENT_DARK = blend565(ACCENT, BG, 128);

// Semantic transport & track state colours (cohesive pale greens, purples, grays & record states)
constexpr uint16_t COLOR_REC     = rgb565(236, 88, 88);
constexpr uint16_t COLOR_REC_DIM = rgb565(96, 30, 30);
constexpr uint16_t COLOR_OVERDUB = rgb565(244, 152, 64);
constexpr uint16_t COLOR_DUB_DIM = rgb565(96, 52, 18);
constexpr uint16_t COLOR_PLAY    = rgb565(144, 236, 174);
constexpr uint16_t COLOR_PUNCH   = rgb565(184, 224, 204);
constexpr uint16_t COLOR_MIDI    = rgb565(198, 168, 244);
constexpr uint16_t COLOR_WARN    = rgb565(212, 228, 160);

// Faint timeline ruler tick marks (top = green time ticks, bottom = purple beat/bar ticks)
constexpr uint16_t COLOR_TICK_TIME = blend565(ACCENT, BG, 105);
constexpr uint16_t COLOR_TICK_BEAT = blend565(COLOR_MIDI, BG, 105);

constexpr uint16_t LOGO_COLOR    = rgb565(168, 228, 188);
constexpr uint16_t TEXT_ERROR    = rgb565(236, 88, 88);

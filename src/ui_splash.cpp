// ui_splash.cpp — Animated Tape Deck startup screen for Capstan
#include "ui.h"
#include "ui_font.h"
#include "storage.h"
#include "usb_drive.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static constexpr int DECK_H   = SY(156);
static constexpr int DECK_W   = (DECK_H * 176) / 108;
static constexpr int DECK_X   = (SCREEN_W - DECK_W) / 2;
static constexpr int DECK_Y   = SY(16);

static constexpr int BOTTOM_W = SX(248);
static constexpr int BOTTOM_H = SY(30);
static constexpr int BOTTOM_X = (SCREEN_W - BOTTOM_W) / 2;
static constexpr int BOTTOM_Y = SY(188);

// Animation timing constants
static constexpr uint32_t FADE_IN_MS = 350;
static constexpr uint32_t HUE_MS     = 1200;
static constexpr uint32_t INTRO_MS   = 900;
static constexpr uint32_t OUT_MS     = 260;

static uint32_t shownAt = 0;
static uint32_t outAt = 0;
static uint32_t lastFrameAt = 0;
static bool readyForButton = false;

static char bootStageMsg[36] = "Starting...";
static int bootStagePct = 0;
static bool customStageSet = false;

// Width of a line of text, in glyph cells (1 cell = 1px at scale 1).
FLASHMEM static int textCols(const char *text) {
  const int gapCells = FONT_GAP / FONT_SCALE;
  int cols = 0;
  for (const char *p = text; *p; p++) {
    if (const Glyph *g = glyphFor(*p)) {
      cols += glyphWidth(*g) + (p[1] ? gapCells : 0);
    }
  }
  return cols;
}

// Draw scaled glyph text into an offscreen strip buffer representing rows [stripY0 .. stripY0 + stripH - 1]
FLASHMEM static void putScaledTextInStrip(uint16_t *buf, int bufW, int stripY0, int stripH,
                                 int x, int y, const char *text, int scalePx, uint16_t color) {
  const int gapCells = FONT_GAP / FONT_SCALE;
  int col = 0;
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    int gw = glyphWidth(*g);
    for (int r = 0; r < FONT_ROWS; r++) {
      GlyphRowBits rowBits = g->rows[r];
      if (!rowBits) continue;
      int py0 = y + r * scalePx;
      int py1 = py0 + scalePx;
      if (py1 <= stripY0 || py0 >= stripY0 + stripH) continue;
      for (int c = 0; c < gw; c++) {
        if ((rowBits >> c) & 1u) {
          int px0 = x + (col + c) * scalePx;
          int px1 = px0 + scalePx;
          int cy0 = max(stripY0, py0) - stripY0;
          int cy1 = min(stripY0 + stripH, py1) - stripY0;
          int cx0 = max(0, px0);
          int cx1 = min(bufW, px1);
          for (int sy = cy0; sy < cy1; sy++) {
            uint16_t *row = buf + sy * bufW;
            for (int sx = cx0; sx < cx1; sx++) {
              row[sx] = color;
            }
          }
        }
      }
    }
    col += gw + gapCells;
  }
}

static inline void fillStripRect(uint16_t *buf, int bufW, int stripY0, int stripH,
                                 int x, int y, int w, int h, uint16_t color) {
  int y0 = max(stripY0, y) - stripY0;
  int y1 = min(stripY0 + stripH, y + h) - stripY0;
  if (y0 >= y1) return;
  int x0 = max(0, x);
  int x1 = min(bufW, x + w);
  if (x0 >= x1) return;
  for (int sy = y0; sy < y1; sy++) {
    uint16_t *row = buf + sy * bufW;
    for (int sx = x0; sx < x1; sx++) {
      row[sx] = color;
    }
  }
}

// OKLCH (lightness, chroma, hue in degrees) to RGB565
FLASHMEM static uint16_t oklch565(float L, float C, float hDeg) {
  float h = hDeg * (float)M_PI / 180.0f, r = 0.0f, g = 0.0f, b = 0.0f;
  for (int i = 0; i < 12; i++, C *= 0.9f) {
    float a = C * cosf(h), bb = C * sinf(h);
    float l = L + 0.3963377774f * a + 0.2158037573f * bb;
    float m = L - 0.1055613458f * a - 0.0638541728f * bb;
    float s = L - 0.0894841775f * a - 1.2914855480f * bb;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    r =  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    if (r >= -0.001f && r <= 1.001f && g >= -0.001f && g <= 1.001f && b >= -0.001f && b <= 1.001f) break;
  }
  auto gamma = [](float c) {
    c = constrain(c, 0.0f, 1.0f);
    return (uint8_t)lroundf(255.0f * (c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f));
  };
  return rgb565(gamma(r), gamma(g), gamma(b));
}

// Signed distance from point (px, py) to line segment (ax, ay)-(bx, by)
FLASHMEM static inline float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
  float pax = px - ax, pay = py - ay;
  float bax = bx - ax, bay = by - ay;
  float denom = bax * bax + bay * bay;
  float h = (denom > 0.0f) ? constrain((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
  float dx = pax - bax * h;
  float dy = pay - bay * h;
  return sqrtf(dx * dx + dy * dy);
}

// Unsigned distance from point (px, py) to outline of rounded rectangle centered at (cx, cy) with half-size (hx, hy) and corner radius r
FLASHMEM static inline float sdRoundBoxOutline(float px, float py, float cx, float cy, float hx, float hy, float r) {
  float dx = fabsf(px - cx) - (hx - r);
  float dy = fabsf(py - cy) - (hy - r);
  float ox = fmaxf(dx, 0.0f), oy = fmaxf(dy, 0.0f);
  float cornerD = (ox > 0.0f && oy > 0.0f) ? sqrtf(ox * ox + oy * oy) : (ox + oy);
  float outD = cornerD + fminf(fmaxf(dx, dy), 0.0f) - r;
  return fabsf(outD);
}

// Evaluate distance to the neon wireframe cassette in canonical 176x108 coordinates
FLASHMEM static inline float evalCassetteDist(float x, float y, const float ux[3], const float uy[3]) {
  // 1. Outer cassette shell: center (88, 54), half-size (82.5, 49.5), corner radius 9.5
  float d = sdRoundBoxOutline(x, y, 88.0f, 54.0f, 82.5f, 49.5f, 9.5f);

  // 2. Central tape window (double outline matching neon cassette reference)
  if (x >= 26.0f && x <= 150.0f && y >= 26.0f && y <= 80.0f) {
    float dw1 = sdRoundBoxOutline(x, y, 88.0f, 53.0f, 55.6f, 20.5f, 4.0f);
    float dw2 = sdRoundBoxOutline(x, y, 88.0f, 53.0f, 54.4f, 19.3f, 3.0f);
    d = fminf(d, fminf(dw1, dw2));
  }

  // 3. Top-left ['87] badge: center (24.0, 20.2), half-size (10.5, 7.0), radius 0.6
  if (x <= 39.0f && y <= 32.0f) {
    float daBox = sdRoundBoxOutline(x, y, 24.0f, 20.2f, 10.5f, 7.0f, 0.6f);
    d = fminf(d, daBox);
    if (x >= 14.0f && x <= 34.0f && y >= 13.5f && y <= 27.0f) {
      float dTick = sdSegment(x, y, 17.0f, 16.2f, 16.5f, 19.0f);
      float d8Top = sdRoundBoxOutline(x, y, 22.0f, 18.2f, 2.0f, 1.95f, 1.6f);
      float d8Bot = sdRoundBoxOutline(x, y, 22.0f, 22.1f, 2.15f, 2.0f, 1.7f);
      float d7Top = sdSegment(x, y, 26.6f, 16.3f, 31.2f, 16.3f);
      float d7Stem = sdSegment(x, y, 31.2f, 16.3f, 28.0f, 24.1f);
      d = fminf(d, fminf(dTick, fminf(fminf(d8Top, d8Bot), fminf(d7Top, d7Stem))));
    }
  }

  // 4. Left & Right spinning 6-tooth sprocket reels: centers (54.8, 53.0) and (121.2, 53.0), r = 13.4
  if (y >= 34.0f && y <= 72.0f) {
    float dxL = x - 54.8f;
    float dxR = x - 121.2f;
    float dy  = y - 53.0f;
    float dx  = (fabsf(dxL) < fabsf(dxR)) ? dxL : dxR;
    if (fabsf(dx) <= 18.5f) {
      float r2 = dx * dx + dy * dy;
      if (r2 <= 18.5f * 18.5f) {
        float r = sqrtf(r2);
        d = fminf(d, fabsf(r - 13.4f));
        // 6 inward-pointing rectangular sprocket teeth (3 opposite pairs along ux[k], uy[k])
        if (r >= 7.5f && r <= 14.2f) {
          for (int k = 0; k < 3; k++) {
            float proj = fabsf(dx * ux[k] + dy * uy[k]);
            float perp = fabsf(dx * uy[k] - dy * ux[k]);
            if (proj >= 8.0f && perp <= 3.8f) {
              float dpProj = fmaxf(fmaxf(10.1f - proj, proj - 13.4f), 0.0f);
              float absPerp = fabsf(perp - 1.35f);
              float dSide  = (dpProj > 0.0f) ? sqrtf(dpProj * dpProj + absPerp * absPerp) : absPerp;
              float dpPerp = fmaxf(perp - 1.35f, 0.0f);
              float absTip = fabsf(proj - 10.1f);
              float dTip   = (dpPerp > 0.0f) ? sqrtf(absTip * absTip + dpPerp * dpPerp) : absTip;
              d = fminf(d, fminf(dSide, dTip));
            }
          }
        }
      }
    }
  }

  // 5. Bottom trapezoid head shield + 4 circular holes
  if (y >= 74.0f) {
    float dtTop1 = sdSegment(x, y, 30.2f, 79.5f, 145.8f, 79.5f);
    float dtTop2 = sdSegment(x, y, 31.1f, 80.5f, 144.9f, 80.5f);
    float dtL1   = sdSegment(x, y, 30.2f, 79.5f, 25.8f, 103.5f);
    float dtL2   = sdSegment(x, y, 31.1f, 80.5f, 26.8f, 103.5f);
    float dtR1   = sdSegment(x, y, 145.8f, 79.5f, 150.2f, 103.5f);
    float dtR2   = sdSegment(x, y, 144.9f, 80.5f, 149.2f, 103.5f);
    d = fminf(d, fminf(fminf(dtTop1, dtTop2), fminf(fminf(dtL1, dtL2), fminf(dtR1, dtR2))));

    float sx = 88.0f - fabsf(x - 88.0f);
    float dhOuter = fabsf(sqrtf((sx - 43.2f) * (sx - 43.2f) + (y - 92.6f) * (y - 92.6f)) - 5.0f);
    float dhInner = fabsf(sqrtf((sx - 60.9f) * (sx - 60.9f) + (y - 88.1f) * (y - 88.1f)) - 4.1f);
    d = fminf(d, fminf(dhOuter, dhInner));
  }

  return d;
}

// 1. Render the Animated Neon Wireframe Cassette in horizontal strips (or only the two spinning reels when reelsOnly = true)
FLASHMEM void drawTapeDeckGraphic(int x, int y, int w, int h, uint32_t ms, uint16_t logoCol, uint8_t fadeWeight, const char *labelText, bool showRecDot, bool reelsOnly) {
  if (w <= 0 || h <= 0) return;
  if (w > DECK_W) w = DECK_W;
  static constexpr int STRIP_H = 16;
  static constexpr float MAX_D = 4.2f;
  uint16_t strip[DECK_W * STRIP_H];

  // Smooth rotation of the 6-tooth sprocket hubs (60-degree / pi/3 symmetry)
  float baseAngle = (float)M_PI * 0.5f + (float)ms * 0.0024f;
  float ux[3], uy[3];
  for (int k = 0; k < 3; k++) {
    float a = baseAngle + (float)k * ((float)M_PI / 3.0f);
    ux[k] = cosf(a);
    uy[k] = sinf(a);
  }

  // Precompute 64-entry neon core + phosphor halo LUT for distance d in [0, MAX_D]
  uint16_t colorLut[64];
  uint16_t coreCol  = blend565(INK, logoCol, 155);
  uint16_t rimCol   = blend565(logoCol, BG, 165);
  uint16_t haloBase = blend565(logoCol, rgb565(0, 148, 52), 115);
  for (int i = 0; i < 64; i++) {
    float d = (float)i * (MAX_D / 63.0f);
    uint16_t c;
    if (d <= 0.38f) {
      uint8_t wt = (uint8_t)lroundf((1.0f - (d / 0.38f)) * 255.0f);
      c = blend565(coreCol, logoCol, wt);
    } else if (d <= 0.90f) {
      uint8_t wt = (uint8_t)lroundf((1.0f - ((d - 0.38f) / 0.52f)) * 255.0f);
      c = blend565(logoCol, rimCol, wt);
    } else {
      float t = (d - 0.90f) / (MAX_D - 0.90f);
      float glow = expf(-3.4f * t) * (1.0f - t);
      uint8_t wt = (uint8_t)constrain((int)lroundf(glow * 185.0f), 0, 255);
      c = blend565(haloBase, BG, wt);
    }
    colorLut[i] = (fadeWeight == 255) ? c : blend565(c, BG, fadeWeight);
  }

  const float invW = 176.0f / (float)w;
  const float invH = 108.0f / (float)h;
  const float distScale = (float)h / (float)SY(96);
  const float lutScale = (63.0f / MAX_D) * distScale;
  const float maxCanD = MAX_D / distScale;

  if (reelsOnly && !showRecDot) {
    const int yStart = max(0, (int)(34.5f * (float)h / 108.0f));
    const int yEnd   = min(h, (int)(71.5f * (float)h / 108.0f) + 1);
    const int x0[2]  = {
      max(0, (int)(36.5f * (float)w / 176.0f)),
      max(0, (int)(103.0f * (float)w / 176.0f))
    };
    const int x1[2]  = {
      min(w, (int)(73.0f * (float)w / 176.0f) + 1),
      min(w, (int)(139.5f * (float)w / 176.0f) + 1)
    };
    for (int y0 = yStart; y0 < yEnd; y0 += STRIP_H) {
      int sh = min(STRIP_H, yEnd - y0);
      for (int reel = 0; reel < 2; reel++) {
        int rx0 = x0[reel];
        int rw = x1[reel] - rx0;
        if (rw <= 0) continue;
        for (int sy = 0; sy < sh; sy++) {
          float ly = ((float)(y0 + sy) + 0.5f) * invH;
          uint16_t *row = strip + sy * rw;
          for (int sx = 0; sx < rw; sx++) {
            float lx = ((float)(rx0 + sx) + 0.5f) * invW;
            float d = evalCassetteDist(lx, ly, ux, uy);
            row[sx] = (d < maxCanD) ? colorLut[min(63, (int)(d * lutScale))] : BG;
          }
        }
        board.tft().writeRect(x + rx0, y + y0, rw, sh, strip);
      }
      looper::keepStreaming();
    }
    return;
  }

  int labelX = 0, labelY = 0;
  int labelScale = FONT_SCALE;
  int dotCx = 0, dotCy = 0, dotR = SY(4);
  uint16_t labelCol = coreCol;
  uint16_t shadowCol = BG;
  uint16_t dotCol = COLOR_REC;
  const bool hasLabel = (labelText != nullptr && labelText[0] != '\0');
  if (hasLabel) {
    int boxRightX = (int)lroundf(34.5f * (float)w / 176.0f);
    int deckRightX = (int)lroundf(170.5f * (float)w / 176.0f);
    int badgeCy = (int)lroundf(20.2f * (float)h / 108.0f);
    if (showRecDot) {
      labelScale = 2 * FONT_SCALE;
      int tw = textCols(labelText) * labelScale;
      int th = FONT_ROWS * labelScale;
      int dotSpace = SX(16);
      int totalW = dotSpace + tw;
      int startX = boxRightX + max(2, (deckRightX - boxRightX - totalW) / 2);
      dotCx = startX + SX(5);
      dotCy = badgeCy;
      labelX = startX + dotSpace;
      labelY = badgeCy - th / 2;
      labelCol = logoCol;
      shadowCol = blend565(logoCol, BG, 70);
      bool recPulse = ((ms / 400) & 1) == 0;
      dotCol = recPulse ? COLOR_REC : blend565(COLOR_REC, BG, 180);
      if (fadeWeight < 255) {
        labelCol = blend565(labelCol, BG, fadeWeight);
        shadowCol = blend565(shadowCol, BG, fadeWeight);
        dotCol = blend565(dotCol, BG, fadeWeight);
      }
    } else {
      int tw = textCols(labelText) * labelScale;
      int th = FONT_ROWS * labelScale;
      labelX = boxRightX + max(2, (deckRightX - boxRightX - tw) / 2);
      labelY = badgeCy - th / 2;
      if (fadeWeight < 255) labelCol = blend565(labelCol, BG, fadeWeight);
    }
  }

  for (int y0 = 0; y0 < h; y0 += STRIP_H) {
    int sh = min(STRIP_H, h - y0);
    for (int sy = 0; sy < sh; sy++) {
      float ly = ((float)(y0 + sy) + 0.5f) * invH;
      uint16_t *row = strip + sy * w;
      for (int sx = 0; sx < w; sx++) {
        float lx = ((float)sx + 0.5f) * invW;
        float d = evalCassetteDist(lx, ly, ux, uy);
        if (d < maxCanD) {
          int idx = min(63, (int)(d * lutScale));
          row[sx] = colorLut[idx];
        } else {
          row[sx] = BG;
        }
      }
    }
    if (hasLabel) {
      if (showRecDot) {
        for (int sy = 0; sy < sh; sy++) {
          int py = y0 + sy;
          int dy = py - dotCy;
          if (abs(dy) > dotR) continue;
          for (int px = dotCx - dotR - 1; px <= dotCx + dotR + 1; px++) {
            if (px < 0 || px >= w) continue;
            int dx = px - dotCx;
            if (dx * dx + dy * dy <= dotR * dotR) {
              strip[sy * w + px] = dotCol;
            }
          }
        }
        putScaledTextInStrip(strip, w, y0, sh, labelX + 2, labelY + 2, labelText, labelScale, shadowCol);
      }
      putScaledTextInStrip(strip, w, y0, sh, labelX, labelY, labelText, labelScale, labelCol);
    }
    board.tft().writeRect(x, y + y0, w, sh, strip);
    looper::keepStreaming();
  }
}

FLASHMEM static void renderTapeDeck(uint32_t ms, uint8_t fadeWeight, uint16_t logoCol) {
  drawTapeDeckGraphic(DECK_X, DECK_Y, DECK_W, DECK_H, ms, logoCol, fadeWeight, "CAPSTAN", true);
}

// 2. Render Bottom Status (Minimal loading bar during boot, then "Press any button..." when loaded)
FLASHMEM static void renderBottomPrompt(uint32_t ms, uint8_t fadeWeight, uint16_t logoCol) {
  static constexpr int STRIP_H = 15;
  uint16_t strip[BOTTOM_W * STRIP_H];

  auto fade = [&](uint16_t c) { return (fadeWeight == 255) ? c : blend565(c, BG, fadeWeight); };

  for (int y0 = 0; y0 < BOTTOM_H; y0 += STRIP_H) {
    int sh = min(STRIP_H, BOTTOM_H - y0);
    for (int i = 0; i < BOTTOM_W * sh; i++) strip[i] = BG;

    if (readyForButton) {
      const char *prompt = "Press any button...";
      int pw = textCols(prompt) * FONT_SCALE;
      int px = max(0, (BOTTOM_W - pw) / 2);
      // Smooth breathing brightness pulse while waiting for a button press
      float pulse = 0.65f + 0.35f * sinf((float)ms * 0.005f);
      uint8_t w = (uint8_t)constrain((int)(pulse * 255.0f), 110, 255);
      uint16_t promptCol = fade(blend565(INK, DIM, w));
      putScaledTextInStrip(strip, BOTTOM_W, y0, sh, px, SY(8), prompt, FONT_SCALE, promptCol);
    } else {
      int tw = textCols(bootStageMsg) * FONT_SCALE;
      int tx = max(0, (BOTTOM_W - tw) / 2);
      putScaledTextInStrip(strip, BOTTOM_W, y0, sh, tx, SY(2), bootStageMsg, FONT_SCALE, fade(DIM));

      // Minimal progress bar below the loading text
      const int barW = SX(160);
      const int barH = SY(4);
      const int barX = (BOTTOM_W - barW) / 2;
      const int barY = SY(18);
      fillStripRect(strip, BOTTOM_W, y0, sh, barX, barY, barW, barH, fade(DARK));
      int fillW = constrain((barW * constrain(bootStagePct, 0, 100)) / 100, 0, barW);
      if (fillW > 0) {
        fillStripRect(strip, BOTTOM_W, y0, sh, barX, barY, fillW, barH, fade(logoCol));
      }
    }

    board.tft().writeRect(BOTTOM_X, BOTTOM_Y + y0, BOTTOM_W, sh, strip);
  }
}

FLASHMEM void splashTick() {
  uint32_t now = millis();
  if (lastFrameAt != 0 && (now - lastFrameAt) < 16 && outAt == 0) {
    return;
  }
  lastFrameAt = now;

  uint32_t ms = now - shownAt;

  uint8_t fadeIn = (ms < FADE_IN_MS) ? (uint8_t)(255 * ms / FADE_IN_MS) : 255;
  uint8_t fadeOut = !outAt ? 255 : (uint8_t)(255 - (255 * min(now - outAt, OUT_MS) / OUT_MS));
  uint8_t fadeWeight = (uint8_t)(((uint16_t)fadeIn * (uint16_t)fadeOut) / 255);

  uint16_t logoCol;
  if (ms < FADE_IN_MS) {
    logoCol = LOGO_COLOR;
  } else {
    float t = min(1.0f, (float)(ms - FADE_IN_MS) / (float)HUE_MS);
    float e = t * t * (3.0f - 2.0f * t);
    logoCol = (t >= 1.0f) ? LOGO_COLOR : oklch565(0.8238f, 0.1641f, 164.76f + 360.0f * e);
  }

  if (!customStageSet && !readyForButton) {
    int introPct = constrain((int)(ms * 60 / INTRO_MS), 5, 60);
    if (introPct > bootStagePct) bootStagePct = introPct;
  }

  renderTapeDeck(ms, fadeWeight, logoCol);
  renderBottomPrompt(ms, fadeWeight, logoCol);
}

FLASHMEM void splashProgress(const char *msg) {
  if (!msg || !msg[0]) {
    readyForButton = true;
    bootStagePct = 100;
  } else {
    customStageSet = true;
    snprintf(bootStageMsg, sizeof(bootStageMsg), "%s", msg);
    if (strstr(msg, "Mount") != nullptr || strstr(msg, "SD") != nullptr) {
      bootStagePct = max(bootStagePct, 75);
    } else if (strstr(msg, "Load") != nullptr || strstr(msg, "song") != nullptr) {
      bootStagePct = max(bootStagePct, 90);
    } else {
      bootStagePct = min(95, max(bootStagePct + 10, 70));
    }
  }
  lastFrameAt = 0;
  splashTick();
}

FLASHMEM void showSplash() {
  board.tft().fillScreen(BG);
  snprintf(bootStageMsg, sizeof(bootStageMsg), "Starting...");
  bootStagePct = 5;
  customStageSet = false;
  readyForButton = false;
  shownAt = millis();
  outAt = 0;
  lastFrameAt = 0;
  splashTick();
}

FLASHMEM void splashIntro() {
  while (millis() - shownAt < INTRO_MS) {
    if (board.keys()) {
      break;
    }
    splashTick();
    delay(1);
  }
  lastFrameAt = 0;
  splashTick();
}

FLASHMEM void hideSplash() {
  readyForButton = true;
  bootStagePct = 100;
  lastFrameAt = 0;
  splashTick();

  // First ensure any button held during early boot is released
  while (board.keys() != 0) {
    if (usbDriveTick) usbDriveTick();
    splashTick();
    delay(5);
  }

  // Clear any pending key-change edge and wait until any button is pressed
  uint8_t k = 0;
  board.keysChanged(k);
  while (board.keys() == 0) {
    if (usbDriveTick) usbDriveTick();
    splashTick();
    delay(5);
  }

  // Smooth fade-out while consuming the button release so it doesn't trigger an action in loop()
  outAt = millis();
  while (millis() - outAt < OUT_MS || board.keys() != 0) {
    if (usbDriveTick) usbDriveTick();
    lastFrameAt = 0;
    splashTick();
    delay(10);
  }
  board.keysChanged(k);
  noteUserActivity();
  board.tft().fillScreen(BG);
  uiGeneration++;
}

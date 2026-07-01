#ifdef DISPLAY_LED_MATRIX
#include "LEDMatrix.h"

#include "fonts/tiny4x6.h"
#include <Fonts/FreeSansBold12pt7b.h>

// ---------------------------------------------------------------------------
// Emoji-System — RGBA-Icons aus LittleFS /icons/ (via main.cpp GetIcon()).
// Icons werden beim Boot in PSRAM geladen (LoadIcons).
// Zum Hinzufügen: PNG konvertieren (scripts/convert_icons.py), .rgba hochladen,
// dann neuen Eintrag in EMOJI_TABLE ergänzen.
// ---------------------------------------------------------------------------
static const uint8_t  ICON_W       = 20;   // Quellgröße = Zielgröße (2x_groesser GIFs)
static const uint8_t  ICON_H       = 20;
static const uint8_t  EMOJI_DRAW_W = 20;
static const uint8_t  EMOJI_DRAW_H = 20;
static const int16_t  EMOJI_Y0     = 5;    // Top-Y: Unterkante auf Textbaseline (y=24)
static const uint8_t  EMOJI_GAP    = 21;   // Vorschubbreite (EMOJI_DRAW_W + 1px)

extern const uint8_t* GetIcon(const char* name);  // definiert in main.cpp

struct EmojiDef {
  const char *utf8;
  const char *iconName;  // Dateiname in /icons/ ohne .rgba
};

static const EmojiDef EMOJI_TABLE[] = {
  { "\xE2\x9D\xA4",         "heart"        },  // ❤  U+2764
  { "\xE2\xAD\x90",         "star"         },  // ⭐  U+2B50
  { "\xF0\x9F\x8C\x9F",     "star_glow"    },  // 🌟  U+1F31F
  { "\xF0\x9F\x8C\x99",     "moon"         },  // 🌙  U+1F319
  { "\xF0\x9F\x94\xA5",     "fire"         },  // 🔥  U+1F525
  { "\xF0\x9F\x98\x8A",     "smile"        },  // 😊  U+1F60A
  { "\xF0\x9F\x98\x80",     "grin"         },  // 😀  U+1F600
  { "\xF0\x9F\x98\x8E",     "cool"         },  // 😎  U+1F60E
  { "\xF0\x9F\x98\xA1",     "angry"        },  // 😡  U+1F621
  { "\xF0\x9F\x98\xAD",     "cry"          },  // 😭  U+1F62D
  { "\xF0\x9F\x98\xB4",     "sleep"        },  // 😴  U+1F634
  { "\xF0\x9F\x99\x82",     "slight_smile" },  // 🙂  U+1F642
  { "\xF0\x9F\xA4\x94",     "think"        },  // 🤔  U+1F914
  { "\xF0\x9F\xA4\x96",     "robot"        },  // 🤖  U+1F916
  { "\xE2\x9C\x85",         "check"        },  // ✅  U+2705
  { "\xE2\x9D\x8C",         "cross"        },  // ❌  U+274C
  { "\xE2\x9D\x93",         "question"     },  // ❓  U+2753
  { "\xE2\x9D\x97",         "exclaim"      },  // ❗  U+2757
  { "\xF0\x9F\x91\x8D",     "thumbsup"     },  // 👍  U+1F44D
  { "\xF0\x9F\x91\x8E",     "thumbsdown"   },  // 👎  U+1F44E
  { "\xF0\x9F\x8D\x95",     "pizza"        },  // 🍕  U+1F355
  { "\xF0\x9F\x8D\xBA",     "beer"         },  // 🍺  U+1F37A
  { "\xF0\x9F\x8E\x81",     "gift"         },  // 🎁  U+1F381
  { "\xF0\x9F\x8E\x89",     "party"        },  // 🎉  U+1F389
  { "\xF0\x9F\x8E\xAE",     "gamepad"      },  // 🎮  U+1F3AE
  { "\xF0\x9F\x91\xBE",     "alien"        },  // 👾  U+1F47E
  { "\xF0\x9F\x92\xA1",     "bulb"         },  // 💡  U+1F4A1
  { "\xF0\x9F\x92\xB0",     "money"        },  // 💰  U+1F4B0
  { "\xF0\x9F\x93\x9D",     "memo"         },  // 📝  U+1F4DD
  { "\xF0\x9F\x93\xA2",     "speaker"      },  // 📢  U+1F4E2
  { "\xF0\x9F\x93\xB1",     "phone"        },  // 📱  U+1F4F1
  { "\xF0\x9F\x94\x94",     "bell"         },  // 🔔  U+1F514
  { "\xE2\x9A\xA0",         "warning"      },  // ⚠  U+26A0
};
static const uint8_t EMOJI_COUNT = sizeof(EMOJI_TABLE) / sizeof(EMOJI_TABLE[0]);

// Returns index into EMOJI_TABLE if *p matches a known emoji, -1 otherwise.
// Advances *p past the matched sequence (incl. optional U+FE0F variation selector).
static int8_t matchEmoji(const char **p) {
  for (uint8_t i = 0; i < EMOJI_COUNT; i++) {
    size_t len = strlen(EMOJI_TABLE[i].utf8);
    if (memcmp(*p, EMOJI_TABLE[i].utf8, len) == 0) {
      *p += len;
      if ((uint8_t)(*p)[0] == 0xEF && (uint8_t)(*p)[1] == 0xB8 && (uint8_t)(*p)[2] == 0x8F)
        *p += 3;
      return (int8_t)i;
    }
  }
  return -1;
}

// Rendert ein 16×16 RGBA-Icon (aus GetIcon) skaliert auf EMOJI_DRAW_W×EMOJI_DRAW_H
// in den RGB888-Buffer. Transparente Pixel (alpha < 32) werden übersprungen.
// shadow=true: RGB wird auf 1/4 gedunkelt (Drop-Shadow-Pass).
static void drawIconRGBAToBuffer(uint8_t *buf, const uint8_t *rgba,
                                  int16_t x, int16_t y0, bool shadow = false) {
  for (uint8_t oy = 0; oy < EMOJI_DRAW_H; oy++) {
    uint8_t sy = (uint8_t)(oy * ICON_H / EMOJI_DRAW_H);
    int16_t py = y0 + oy;
    if (py < 0 || py >= TOTAL_HEIGHT) continue;
    for (uint8_t ox = 0; ox < EMOJI_DRAW_W; ox++) {
      uint8_t sx = (uint8_t)(ox * ICON_W / EMOJI_DRAW_W);
      int16_t px = x + ox;
      if (px < 0 || px >= TOTAL_WIDTH) continue;
      const uint8_t *p = rgba + ((uint16_t)sy * ICON_W + sx) * 4;
      if (p[3] < 32) continue;  // transparent
      uint32_t idx = ((uint32_t)py * TOTAL_WIDTH + px) * 3;
      if (shadow) {
        buf[idx]     = p[0] >> 2;
        buf[idx + 1] = p[1] >> 2;
        buf[idx + 2] = p[2] >> 2;
      } else if (p[3] >= 240) {
        buf[idx]     = p[0];
        buf[idx + 1] = p[1];
        buf[idx + 2] = p[2];
      } else {
        uint8_t a = p[3], ia = 255 - a;
        buf[idx]     = (uint8_t)(((uint16_t)p[0] * a + (uint16_t)buf[idx]     * ia) >> 8);
        buf[idx + 1] = (uint8_t)(((uint16_t)p[1] * a + (uint16_t)buf[idx + 1] * ia) >> 8);
        buf[idx + 2] = (uint8_t)(((uint16_t)p[2] * a + (uint16_t)buf[idx + 2] * ia) >> 8);
      }
    }
  }
}

LedMatrix::LedMatrix() {
  int8_t colorPins1[3] = {R1_PIN, G1_PIN, B1_PIN};
  int8_t colorPins2[3] = {R2_PIN, G2_PIN, B2_PIN};
  const HUB75_I2S_CFG::i2s_pins pins = {colorPins1[rgbOrder[rgbMode * 3]],
                                        colorPins1[rgbOrder[rgbMode * 3 + 1]],
                                        colorPins1[rgbOrder[rgbMode * 3 + 2]],
                                        colorPins2[rgbOrder[rgbMode * 3]],
                                        colorPins2[rgbOrder[rgbMode * 3 + 1]],
                                        colorPins2[rgbOrder[rgbMode * 3 + 2]],
                                        A_PIN,
                                        B_PIN,
                                        C_PIN,
                                        D_PIN,
                                        E_PIN,
                                        LAT_PIN,
                                        OE_PIN,
                                        CLK_PIN};

  HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANELS_NUMBER, pins);
  // Without setting clkphase to false, HD panels seem to flicker.
  mxconfig.clkphase = (panelClkphase == 1);
  mxconfig.i2sspeed =
      panelI2sspeed == 20
          ? HUB75_I2S_CFG::clk_speed::HZ_20M
          : (panelI2sspeed == 16 ? HUB75_I2S_CFG::clk_speed::HZ_16M
                                 : HUB75_I2S_CFG::clk_speed::HZ_8M);
  mxconfig.latch_blanking = panelLatchBlanking;
  mxconfig.min_refresh_rate = panelMinRefreshRate;
  mxconfig.driver = (HUB75_I2S_CFG::shift_driver)panelDriver;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();

  // Pre-allocated canvas for flicker-free GFX text rendering.
  // Renders into SRAM pixel-by-pixel without touching the live DMA buffer.
  textCanvas = new GFXcanvas16(TOTAL_WIDTH, TOTAL_HEIGHT);
}

bool LedMatrix::HasScalingModes() {
  return false;  // This display does not support subpixel scaling
}

const char **LedMatrix::GetScalingModes() { return nullptr; }

uint8_t LedMatrix::GetScalingModeCount() { return 0; }

uint8_t LedMatrix::GetCurrentScalingMode() { return 0; }

void LedMatrix::SetCurrentScalingMode(uint8_t mode) {}

void LedMatrix::DrawPixel(uint16_t x, uint16_t y, uint8_t r, uint8_t g,
                          uint8_t b) {
  dma_display->drawPixelRGB888(x, y + yOffset, r, g, b);
}

void LedMatrix::DrawPixel(uint16_t x, uint16_t y, uint16_t color) {
  dma_display->drawPixel(x, y + yOffset, color);
}

void LedMatrix::ClearScreen() { dma_display->clearScreen(); }

void LedMatrix::SetBrightness(uint8_t level) {
  dma_display->setBrightness8(lumval[level]);
}

void LedMatrix::FillScreen(uint8_t r, uint8_t g, uint8_t b) {
  dma_display->fillScreenRGB888(r, g, b);
}

void LedMatrix::DisplayText(const char *text, uint16_t x, uint16_t y, uint8_t r,
                            uint8_t g, uint8_t b, bool transparent,
                            bool inverted) {
  for (uint8_t ti = 0; ti < strlen(text); ti++) {
    for (uint8_t tj = 0; tj <= 5; tj++) {
      uint8_t fourPixels = getFontLine(text[ti], tj);
      for (uint8_t pixel = 0; pixel < 4; pixel++) {
        bool p = (fourPixels >> (3 - pixel)) & 0x1;
        if (inverted) {
          p = !p;
        }
        if (transparent && !p) {
          continue;
        }
        DrawPixel(x + pixel + (ti * 4), y + tj, r * p, g * p, b * p);
      }
    }
  }
}

void LedMatrix::DisplayTextScaled(const char *text, uint16_t x, uint16_t y,
                                   uint8_t r, uint8_t g, uint8_t b, uint8_t scale) {
  if (scale <= 1) { DisplayText(text, x, y, r, g, b); return; }
  for (uint8_t ti = 0; ti < strlen(text); ti++) {
    for (uint8_t tj = 0; tj < 6; tj++) {
      uint8_t fourPixels = getFontLine(text[ti], tj);
      for (uint8_t pixel = 0; pixel < 4; pixel++) {
        if (!((fourPixels >> (3 - pixel)) & 0x1)) continue;
        for (uint8_t sy = 0; sy < scale; sy++)
          for (uint8_t sx = 0; sx < scale; sx++)
            DrawPixel(x + (pixel + ti * 4) * scale + sx,
                      y + tj * scale + sy, r, g, b);
      }
    }
  }
}

void IRAM_ATTR LedMatrix::FillZoneRaw(uint8_t idx, uint8_t *pBuffer) {
  const uint8_t zoneYOffset = (idx / ZONES_PER_ROW) * ZONE_HEIGHT;
  const uint8_t zoneXOffset = (idx % ZONES_PER_ROW) * ZONE_WIDTH;

  for (uint8_t y = 0; y < ZONE_HEIGHT; y++) {
    for (uint8_t x = 0; x < ZONE_WIDTH; x++) {
      uint16_t pos = (y * ZONE_WIDTH + x) * 3;

      DrawPixel(x + zoneXOffset, y + zoneYOffset, pBuffer[pos],
                pBuffer[pos + 1], pBuffer[pos + 2]);
    }
  }
}

void IRAM_ATTR LedMatrix::FillZoneRaw565(uint8_t idx, uint8_t *pBuffer) {
  const uint8_t zoneYOffset = (idx / ZONES_PER_ROW) * ZONE_HEIGHT;
  const uint8_t zoneXOffset = (idx % ZONES_PER_ROW) * ZONE_WIDTH;

  for (uint8_t y = 0; y < ZONE_HEIGHT; y++) {
    for (uint8_t x = 0; x < ZONE_WIDTH; x++) {
      uint16_t pos = (y * ZONE_WIDTH + x) * 2;
      DrawPixel(x + zoneXOffset, y + zoneYOffset,
                (((uint16_t)pBuffer[pos + 1]) << 8) + pBuffer[pos]);
    }
  }
}

void IRAM_ATTR LedMatrix::ClearZone(uint8_t idx) {
  const uint8_t zoneYOffset = (idx / ZONES_PER_ROW) * ZONE_HEIGHT;
  const uint8_t zoneXOffset = (idx % ZONES_PER_ROW) * ZONE_WIDTH;

  for (uint8_t y = 0; y < ZONE_HEIGHT; y++) {
    for (uint8_t x = 0; x < ZONE_WIDTH; x++) {
      DrawPixel(x + zoneXOffset, y + zoneYOffset, 0, 0, 0);
    }
  }
}

void IRAM_ATTR LedMatrix::FillPanelRaw(uint8_t *pBuffer) {
  uint16_t pos;

  for (uint16_t y = 0; y < TOTAL_HEIGHT; y++) {
    for (uint16_t x = 0; x < TOTAL_WIDTH; x++) {
      pos = (y * TOTAL_WIDTH + x) * 3;

      DrawPixel(x, y, pBuffer[pos], pBuffer[pos + 1], pBuffer[pos + 2]);
    }
  }
}

// Renders text with FreeSansBold12pt7b (~17px tall). x may be negative (clips correctly).
// Baseline y=24 leaves headroom for ascenders and descenders within 32px display height.
void LedMatrix::DisplayTextGFX(const char *text, int16_t x, uint8_t r,
                                uint8_t g, uint8_t b) {
  dma_display->setFont(&FreeSansBold12pt7b);
  // Black background fills each glyph cell — avoids ClearScreen() between frames
  dma_display->setTextColor(dma_display->color565(r, g, b), 0x0000);
  dma_display->setTextWrap(false);
  dma_display->setCursor(x, 24);
  dma_display->print(text);
  dma_display->setFont(nullptr);
}

void LedMatrix::EraseVLine(int16_t x) {
  if (x >= 0 && x < dma_display->width())
    dma_display->drawFastVLine(x, 0, dma_display->height(), 0);
}

// Returns pixel width of text rendered with FreeSansBold12pt7b + inline emojis.
uint16_t LedMatrix::GetTextGFXWidth(const char *text) {
  // Build a plain-text copy with emojis replaced by spaces for font measurement,
  // then add 11px per emoji (10px bitmap + 1px gap).
  char plain[128];
  uint8_t emojiCount = 0;
  const char *src = text;
  char *dst = plain;
  while (*src && dst < plain + sizeof(plain) - 1) {
    int8_t idx = matchEmoji(&src);
    if (idx >= 0) {
      emojiCount++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';

  uint16_t textW = 0;
  if (*plain) {
    textCanvas->setFont(&FreeSansBold12pt7b);
    int16_t x1, y1; uint16_t w, h;
    textCanvas->getTextBounds(plain, 0, 24, &x1, &y1, &w, &h);
    textCanvas->setFont(nullptr);
    textW = w;
  }
  return textW + (uint16_t)emojiCount * EMOJI_GAP;
}

// Renders text + inline emojis at position x into an RGB888 renderBuffer.
// Text is rendered in segments between emojis so each part lands at the correct
// x position — no overlap between text and emoji bitmaps.
void LedMatrix::RenderTextGFXToBuffer(uint8_t *buf, const char *text, int16_t x,
                                       uint8_t r, uint8_t g, uint8_t b) {
  textCanvas->fillScreen(0);
  uint16_t color565 = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
  textCanvas->setFont(&FreeSansBold12pt7b);
  textCanvas->setTextColor(color565);
  textCanvas->setTextWrap(false);

  struct EmojiPlacement { int16_t x; uint8_t idx; };
  EmojiPlacement placements[16];
  uint8_t numPlacements = 0;

  const char *src = text;
  int16_t curX = x;
  char seg[128];
  char *dst = seg;

  while (*src) {
    int8_t idx = matchEmoji(&src);
    if (idx >= 0) {
      // Render accumulated text segment at curX, then advance curX by its width
      if (dst > seg) {
        *dst = '\0';
        textCanvas->setCursor(curX, 24);
        textCanvas->print(seg);
        int16_t x1, y1; uint16_t w, h;
        textCanvas->getTextBounds(seg, 0, 24, &x1, &y1, &w, &h);
        curX += (int16_t)w;
        dst = seg;
      }
      if (numPlacements < 16)
        placements[numPlacements++] = { curX, (uint8_t)idx };
      curX += EMOJI_GAP;
    } else if (dst < seg + sizeof(seg) - 1) {
      *dst++ = *src++;
    } else {
      src++;  // buffer full — skip character
    }
  }
  // Flush remaining text segment
  if (dst > seg) {
    *dst = '\0';
    textCanvas->setCursor(curX, 24);
    textCanvas->print(seg);
  }
  textCanvas->setFont(nullptr);

  // Copy canvas → RGB888 buffer
  const uint16_t *pixels = textCanvas->getBuffer();
  const uint32_t n = TOTAL_WIDTH * TOTAL_HEIGHT;
  for (uint32_t i = 0; i < n; i++) {
    uint16_t c      = pixels[i];
    buf[i * 3]     = (c >> 8) & 0xF8;
    buf[i * 3 + 1] = (c >> 3) & 0xFC;
    buf[i * 3 + 2] = (c << 3) & 0xF8;
  }

  // Composite RGBA-Icons in buf — Shadow (+1/+1 gedunkelt), dann Icon obendrauf
  for (uint8_t i = 0; i < numPlacements; i++) {
    const uint8_t *rgba = GetIcon(EMOJI_TABLE[placements[i].idx].iconName);
    if (!rgba) continue;  // Icon nicht geladen — überspringen
    drawIconRGBAToBuffer(buf, rgba, placements[i].x + 1, EMOJI_Y0 + 1, true);
    drawIconRGBAToBuffer(buf, rgba, placements[i].x,     EMOJI_Y0,      false);
  }
}

LedMatrix::~LedMatrix() {
  delete textCanvas;
  delete dma_display;
}
#endif
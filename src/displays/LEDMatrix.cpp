#ifdef DISPLAY_LED_MATRIX
#include "LEDMatrix.h"

#include "fonts/tiny4x6.h"
#include <Fonts/FreeSansBold12pt7b.h>

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

// Returns pixel width of text rendered with FreeSansBold12pt7b (pure font math, no rendering).
uint16_t LedMatrix::GetTextGFXWidth(const char *text) {
  textCanvas->setFont(&FreeSansBold12pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  textCanvas->getTextBounds(text, 0, 24, &x1, &y1, &w, &h);
  textCanvas->setFont(nullptr);
  return w;
}

// Renders text at position x (may be negative for scroll) into an RGB888 renderBuffer.
// Uses an off-screen GFXcanvas16 — no DMA writes during rendering, zero flicker.
void LedMatrix::RenderTextGFXToBuffer(uint8_t *buf, const char *text, int16_t x,
                                       uint8_t r, uint8_t g, uint8_t b) {
  textCanvas->fillScreen(0);
  textCanvas->setFont(&FreeSansBold12pt7b);
  uint16_t color565 = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
  textCanvas->setTextColor(color565);
  textCanvas->setTextWrap(false);
  textCanvas->setCursor(x, 24);
  textCanvas->print(text);
  textCanvas->setFont(nullptr);

  const uint16_t *pixels = textCanvas->getBuffer();
  const uint32_t n = TOTAL_WIDTH * TOTAL_HEIGHT;
  for (uint32_t i = 0; i < n; i++) {
    uint16_t c  = pixels[i];
    buf[i * 3]     = (c >> 8) & 0xF8;
    buf[i * 3 + 1] = (c >> 3) & 0xFC;
    buf[i * 3 + 2] = (c << 3) & 0xF8;
  }
}

LedMatrix::~LedMatrix() {
  delete textCanvas;
  delete dma_display;
}
#endif
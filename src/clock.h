#ifndef CLOCK_H
#define CLOCK_H
#ifdef ZEDMD_WIFI

#include <Arduino.h>

// ── Clock State Variables ─────────────────────────────────────────────────────
// Readable from main.cpp (SaveClockColors, LoadClockColors, get_config, Endpoints)
// and from weather.cpp (weatherDisplayClock, weatherDisplayForecast)

extern uint8_t       clockR, clockG, clockB;   // Clock color   (Default: Cyan)
extern uint8_t       dateR,  dateG,  dateB;    // Date color    (Default: Gray)
extern volatile bool clockColorChanged;        // Forces redraw on color change
extern bool          ntpSynced;
extern String        ntpServer;
extern String        clockTimezone;

// ── Public API ────────────────────────────────────────────────────────────────

void clockInit();     // Initialize NTP (call after WiFi connect)
void clockDisplay();  // Draw clock on DMD (formerly DisplayClock)

// Drawing helpers — also used by weather.cpp
void DrawSegDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawSegDigitClassic(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawSegDigitClassic2(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawSegDigitModern(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawDigitAuto(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);  // glow-aware
void DrawColon(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void DrawColonAuto(int x, int y, uint8_t r, uint8_t g, uint8_t b);             // glow-aware
void DrawSegDigitShadow(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawColonShadow(int x, int y, uint8_t r, uint8_t g, uint8_t b);

extern volatile bool clockGlowEnabled;
extern volatile int  clockSegStyle;   // 0=Default 1=Classic 2=Modern 3=Classic2

#endif // ZEDMD_WIFI
#endif // CLOCK_H

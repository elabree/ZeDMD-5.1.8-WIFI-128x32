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
void DrawColon(int x, int y, uint8_t r, uint8_t g, uint8_t b);

#endif // ZEDMD_WIFI
#endif // CLOCK_H

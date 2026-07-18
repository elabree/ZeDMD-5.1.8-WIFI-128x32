#ifndef CLOCK_H
#define CLOCK_H
#ifdef ZEDMD_WIFI

#include <Arduino.h>

// ── Uhr-Zustandsvariablen ─────────────────────────────────────────────────────
// Lesbar von main.cpp (SaveClockColors, LoadClockColors, get_config, Endpoints)
// und von weather.cpp (weatherDisplayClock, weatherDisplayForecast)

extern uint8_t       clockR, clockG, clockB;   // Uhrzeitfarbe  (Default: Cyan)
extern uint8_t       dateR,  dateG,  dateB;    // Datumfarbe    (Default: Grau)
extern volatile bool clockColorChanged;        // Erzwingt Neuzeichnung bei Farbänderung
extern bool          ntpSynced;
extern String        ntpServer;
extern String        clockTimezone;

// ── Public API ────────────────────────────────────────────────────────────────

void clockInit();     // NTP initialisieren (nach WiFi-Connect aufrufen)
void clockDisplay();  // Uhr auf DMD zeichnen (= ehem. DisplayClock)

// Zeichenhilfsfunktionen — auch von weather.cpp genutzt
void DrawSegDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b);
void DrawColon(int x, int y, uint8_t r, uint8_t g, uint8_t b);

#endif // ZEDMD_WIFI
#endif // CLOCK_H

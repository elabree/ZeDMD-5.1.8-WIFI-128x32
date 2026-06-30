#pragma once
#ifdef ZEDMD_WIFI

#include <Arduino.h>

// ── Wetter-Zustandsvariablen ─────────────────────────────────────────────────
// Lesbar von main.cpp (MQTT-Callback, get_config, SaveWeatherConfig)

extern float    weatherTemp;
extern float    weatherWindSpeed;
extern uint8_t  weatherHumidity;
extern uint16_t weatherPressure;
extern uint16_t weatherCode;
extern bool     weatherIsDay;
extern volatile bool weatherAvailable;
extern uint32_t lastWeatherFetch;
extern uint32_t lastMqttWeather;
extern uint32_t weatherPhaseStart;
extern uint16_t forecastCode[3];
extern int8_t   forecastTempMax[3];
extern volatile bool forecastAvailable;
extern volatile bool weatherFetchRunning;
extern float    weatherLat;
extern float    weatherLon;

// ── Public API ────────────────────────────────────────────────────────────────

void weatherInit();             // Platzhalter — bei Bedarf erweitern (z.B. Mutex)
void weatherTrigger();          // Startet HTTP-Fetch in eigenem Task (PSRAM-Stack)
void weatherIconTest();         // TEST: alle Wetter-Icons nebeneinander (temporär)
void weatherDisplay();          // Wetter-Vollbild
void weatherDisplayClock();     // Uhr + Wetter kombiniert
void weatherDisplayForecast();  // 3-Tages-Vorhersage
bool weatherIsAvailable();      // true wenn Wetter-Daten vorhanden

#endif // ZEDMD_WIFI

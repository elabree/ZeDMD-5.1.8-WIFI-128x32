#ifndef WEATHER_H
#define WEATHER_H
#ifdef ZEDMD_WIFI

#include <Arduino.h>

// ── Weather state variables ───────────────────────────────────────────────────
// Readable from main.cpp (MQTT callback, get_config, SaveWeatherConfig)

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

void weatherInit();             // placeholder — extend as needed (e.g. mutex)
void weatherTrigger();          // starts HTTP fetch in its own task (PSRAM stack)
void weatherIconTest();          // TEST: all 16×16 icons (6+5 grid)
void weatherSmallIconTest();     // TEST: all 8×8 icons (1 row, forecast size)
void weatherDisplay();          // full-screen weather
void weatherDisplayClock();     // clock + weather combined
void weatherDisplayForecast();  // 3-day forecast
bool weatherIsAvailable();      // true if weather data is available

#endif // ZEDMD_WIFI
#endif // WEATHER_H

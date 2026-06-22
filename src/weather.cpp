#ifdef ZEDMD_WIFI

#include "weather.h"
#include "displayDriver.h"
#include "panel.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <time.h>

// ── Aus main.cpp (extern) ─────────────────────────────────────────────────────

extern DisplayDriver*    display;
extern uint8_t           screensaverBrightness;
extern bool              wifiActive;

extern void Render();
extern void logMsg(const char* fmt, ...);

#include "clock.h"  // clockR/G/B, dateR/G/B, clockColorChanged, DrawSegDigit, DrawColon

// ── Globals ───────────────────────────────────────────────────────────────────

float    weatherTemp        = 0.0f;
float    weatherWindSpeed   = 0.0f;
uint8_t  weatherHumidity    = 0;
uint16_t weatherPressure    = 0;
uint16_t weatherCode        = 0;
bool     weatherIsDay       = true;
volatile bool weatherAvailable   = false;
uint32_t lastWeatherFetch   = 0;
uint32_t lastMqttWeather    = 0;
uint32_t weatherPhaseStart  = 0;
uint16_t forecastCode[3]    = {0, 0, 0};
int8_t   forecastTempMax[3] = {0, 0, 0};
volatile bool forecastAvailable  = false;
volatile bool weatherFetchRunning = false;

float weatherLat = 52.3202f;
float weatherLon = 10.4011f;

// ── Interne Hilfstypen und Funktionen ─────────────────────────────────────────

struct WeatherInfo { const char* text; uint8_t icon; };

static WeatherInfo GetWeatherInfo(uint16_t code, bool isDay = true) {
  if (!isDay && (code == 0 || code == 1)) return {"Klar",        6};
  if (!isDay && code == 2)                return {"Teils bew.",  7};
  if (code == 0)                       return {"Sonnig",        0};
  if (code == 1)                       return {"Heiter",        0};
  if (code == 2)                       return {"Teils bew.",    1};
  if (code == 3)                       return {"Bedeckt",       2};
  if (code == 45 || code == 48)        return {"Nebel",         2};
  if (code >= 51 && code <= 57)        return {"Nieselregen",   3};
  if (code >= 61 && code <= 67)        return {"Regen",         3};
  if (code >= 71 && code <= 77)        return {"Schnee",        4};
  if (code >= 80 && code <= 82)        return {"Schauer",       3};
  if (code >= 85 && code <= 86)        return {"Schneeschr.",   4};
  if (code >= 95)                      return {"Gewitter",      5};
  return {"Unbekannt", 2};
}

static void DrawWeatherIcon(uint8_t idx, int x, int y, uint8_t scale = 2) {
  if (idx >= 8) idx = 2;
  static const uint8_t L1[8][8] = {
    {0b00100100, 0b00011000, 0b01111110, 0b11111111,
     0b11111111, 0b01111110, 0b00011000, 0b00100100},
    {0b01100000, 0b11100000, 0b01100000, 0b00000000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    {0b00000000, 0b00111000, 0b01111110, 0b11111111,
     0b11111111, 0b01111110, 0b00000000, 0b00000000},
    {0b00111100, 0b01111110, 0b11111111, 0b11111111,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    {0b00010000, 0b01010100, 0b00111000, 0b11111111,
     0b00111000, 0b01010100, 0b00010000, 0b00000000},
    {0b00111100, 0b01111110, 0b11111111, 0b11111111,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    {0b00111100, 0b01110000, 0b11100000, 0b11100000,
     0b11100000, 0b11100000, 0b01110000, 0b00111100},
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b00011100, 0b00111110, 0b01111111, 0b00000000},
  };
  static const uint8_t C1[8][3] = {
    {255, 210,   0}, {255, 210,   0}, {155, 155, 165},
    {155, 155, 165}, {200, 230, 255}, {155, 155, 165},
    {200, 220, 255}, {155, 155, 165},
  };
  static const uint8_t L2[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0b00000000, 0b00000000, 0b00001100, 0b00111110,
     0b01111111, 0b01111111, 0b00111110, 0b00000000},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b01000100, 0b00100010, 0b01000100, 0b00100010},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b00011100, 0b00111000, 0b01110000, 0b00000000},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0b01110000, 0b11000000, 0b11000000, 0b01110000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
  };
  static const uint8_t C2[8][3] = {
    {  0,   0,   0}, {170, 170, 175}, {  0,   0,   0},
    {100, 160, 255}, {  0,   0,   0}, {255, 210,   0},
    {  0,   0,   0}, {200, 220, 255},
  };

  auto drawLayer = [&](const uint8_t bmp[8], uint8_t r, uint8_t g, uint8_t b) {
    for (int row = 0; row < 8; row++) {
      uint8_t mask = bmp[row];
      if (!mask) continue;
      for (int col = 0; col < 8; col++) {
        if (mask & (0x80 >> col)) {
          int px = x + col * scale, py = y + row * scale;
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++)
              display->DrawPixel(px+dx, py+dy, r, g, b);
        }
      }
    }
  };
  drawLayer(L1[idx], C1[idx][0], C1[idx][1], C1[idx][2]);
  drawLayer(L2[idx], C2[idx][0], C2[idx][1], C2[idx][2]);
}

// ── HTTP-Fetch ────────────────────────────────────────────────────────────────

static void fetchWeather() {
  if (!wifiActive || WiFi.status() != WL_CONNECTED) return;
  lastWeatherFetch = millis();
  logMsg("Wetter: Fetch-Start, Heap frei=%u intern-max=%u",
         esp_get_free_heap_size(),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // Response-Body in PSRAM — verhindert 20-30 KB Allokation im internen SRAM-Heap
  const size_t WX_BUF = 32768;
  char* body = (char*)heap_caps_malloc(WX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!body) { logMsg("Wetter: PSRAM-Puffer fehlgeschlagen"); return; }

  WiFiClient client;
  HTTPClient http;
  // HTTP (kein TLS) — Open-Meteo unterstuetzt plain HTTP fuer Embedded-Geraete;
  // interner RAM reicht nicht fuer mbedTLS-Handshake wenn Audio-Codec aktiv ist
  char urlBuf[320];
  snprintf(urlBuf, sizeof(urlBuf),
    "http://api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,relative_humidity_2m,weather_code,pressure_msl,wind_speed_10m,is_day"
    "&daily=temperature_2m_max&hourly=weather_code"
    "&timezone=Europe%%2FBerlin&forecast_days=4",
    weatherLat, weatherLon);

  http.begin(client, urlBuf);
  http.setTimeout(8000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int bodyLen = http.getStream().readBytes(body, (int)WX_BUF - 1);
    body[bodyLen] = '\0';

    auto extractFloat = [&](const char* key, const char* searchFrom) -> float {
      const char* p = strstr(searchFrom, key);
      if (!p) return -999.0f;
      p = strchr(p, ':');
      if (!p) return -999.0f;
      p++;
      while (*p == ' ') p++;
      return atof(p);
    };
    auto extractInt = [&](const char* key, const char* searchFrom) -> int {
      const char* p = strstr(searchFrom, key);
      if (!p) return -1;
      p = strchr(p, ':');
      if (!p) return -1;
      p++;
      while (*p == ' ') p++;
      return atoi(p);
    };

    const char* curStart = strstr(body, "\"current\":{");
    if (!curStart) curStart = strstr(body, "\"current\": {");
    if (curStart) {
      int isDay = extractInt("\"is_day\"", curStart);
      if (isDay >= 0) weatherIsDay = (isDay == 1);
      int code = extractInt("\"weather_code\"", curStart);
      if (code >= 0) {
        weatherCode      = (uint16_t)code;
        lastWeatherFetch = millis();
        weatherAvailable = true;
        if (millis() - lastMqttWeather > 10UL * 60UL * 1000UL) {
          float temp = extractFloat("\"temperature_2m\"", curStart);
          if (temp > -900.0f) {
            weatherTemp      = temp;
            weatherHumidity  = (uint8_t)max(0, extractInt("\"relative_humidity_2m\"", curStart));
            float pres       = extractFloat("\"pressure_msl\"", curStart);
            weatherPressure  = (pres > 0) ? (uint16_t)pres : 0;
            float wind       = extractFloat("\"wind_speed_10m\"", curStart);
            weatherWindSpeed = (wind > 0) ? wind : 0.0f;
          }
        }
      }
    }

    const char* hourlyStart = strstr(body, "\"hourly\":{");
    if (!hourlyStart) hourlyStart = strstr(body, "\"hourly\": {");
    const char* dailyStart  = strstr(body, "\"daily\":{");
    if (!dailyStart)  dailyStart  = strstr(body, "\"daily\": {");

    if (hourlyStart && dailyStart) {
      auto extractArrayVal = [&](const char* key, const char* searchFrom, int idx) -> float {
        const char* p = strstr(searchFrom, key);
        if (!p) return -999.0f;
        p = strchr(p, '[');
        if (!p) return -999.0f;
        p++;
        for (int i = 0; i < idx; i++) {
          p = strchr(p, ',');
          if (!p) return -999.0f;
          p++;
        }
        while (*p == ' ') p++;
        return atof(p);
      };
      bool ok = true;
      const int noonIdx[3] = {36, 60, 84};
      for (int i = 0; i < 3; i++) {
        float code = extractArrayVal("\"weather_code\"",       hourlyStart, noonIdx[i]);
        float tmax = extractArrayVal("\"temperature_2m_max\"", dailyStart,  i + 1);
        if (code < -900.0f || tmax < -900.0f) { ok = false; break; }
        forecastCode[i]    = (uint16_t)roundf(code);
        forecastTempMax[i] = (int8_t)roundf(tmax);
      }
      if (ok) {
        __sync_synchronize();
        forecastAvailable = true;
        logMsg("Wetter: Vorhersage OK (%d/%d/%d)", forecastCode[0], forecastCode[1], forecastCode[2]);
      } else {
        logMsg("Wetter: Vorhersage Parsing fehlgeschlagen");
      }
    } else {
      logMsg("Wetter: Kein hourly/daily-Block in Antwort");
    }
  } else {
    logMsg("Wetter: HTTP Fehler %d", httpCode);
  }
  http.end();
  free(body);
}

static TaskHandle_t wxTaskHandle = NULL;

// Läuft einmal, suspendiert sich danach — wird per vTaskResume reaktiviert statt neu erzeugt,
// damit wxTaskBuf (StaticTask_t) nie wiederverwendet wird bevor der Idle-Task ihn aufgeräumt hat.
static void weatherFetchTask(void* pvParams) {
  while (true) {
    fetchWeather();
    weatherFetchRunning = false;
    vTaskSuspend(NULL);
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

void weatherInit() {
  // Platzhalter — zukünftige Initialisierung (z.B. Mutex für Wetterdaten) hier
}

void weatherTrigger() {
  if (weatherFetchRunning) return;
  weatherFetchRunning = true;
  if (!wxTaskHandle) {
    // Erster Aufruf: Task einmalig erzeugen (Stack bleibt für die gesamte Laufzeit)
    static StaticTask_t wxTaskBuf;
    static StackType_t* wxStack = nullptr;
    if (!wxStack) {
      wxStack = (StackType_t*)heap_caps_malloc(20480, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!wxStack) {
      weatherFetchRunning = false;
      logMsg("Wetter: Stack-Allokierung fehlgeschlagen");
      return;
    }
    wxTaskHandle = xTaskCreateStatic(weatherFetchTask, "wxFetch", 20480 / sizeof(StackType_t),
                                     NULL, 1, wxStack, &wxTaskBuf);
    if (!wxTaskHandle) {
      weatherFetchRunning = false;
      logMsg("Wetter: Task-Start fehlgeschlagen");
    }
  } else {
    vTaskResume(wxTaskHandle);
  }
}

bool weatherIsAvailable() {
  return weatherAvailable;
}

void weatherDisplay() {
  display->SetBrightness(screensaverBrightness);
  display->ClearScreen();

  WeatherInfo wi = GetWeatherInfo(weatherCode, weatherIsDay);

  DrawWeatherIcon(wi.icon, 1, 8);

  char tempStr[12];
  snprintf(tempStr, sizeof(tempStr), "%.1f C", weatherTemp);
  display->DisplayText(tempStr, 0, 2, clockR, clockG, clockB, 1);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    const char* days[] = {"So","Mo","Di","Mi","Do","Fr","Sa"};
    char dateStr[16];
    snprintf(dateStr, sizeof(dateStr), "%s %02d.%02d.%02d",
      days[timeinfo.tm_wday], timeinfo.tm_mday,
      timeinfo.tm_mon + 1, timeinfo.tm_year % 100);
    int dateX = TOTAL_WIDTH - (int)(strlen(dateStr) * 4) - 1;
    display->DisplayText(dateStr, dateX, 2, dateR, dateG, dateB, 1);
  }

  display->DisplayText(wi.text, 19, 12, dateR, dateG, dateB, 1);

  char humStr[18];
  snprintf(humStr, sizeof(humStr), "%u%%  %uhPa", weatherHumidity, weatherPressure);
  display->DisplayText(humStr, 0, 24, dateR, dateG, dateB, 1);

  Render();
}

void weatherDisplayClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  static int  lastCWMin         = -1;
  static int  lastCWHour        = -1;
  static bool lastWeatherAvail  = false;
  if (!clockColorChanged &&
      timeinfo.tm_min  == lastCWMin  &&
      timeinfo.tm_hour == lastCWHour &&
      lastWeatherAvail == weatherAvailable) {
    for (int y = 0; y < TOTAL_HEIGHT; y++)
      display->DrawPixel(53, y, 100, 100, 100);
    return;
  }
  clockColorChanged = false;
  lastCWMin         = timeinfo.tm_min;
  lastCWHour        = timeinfo.tm_hour;
  lastWeatherAvail  = weatherAvailable;

  display->SetBrightness(screensaverBrightness);
  display->ClearScreen();

  const char* days[] = {"So","Mo","Di","Mi","Do","Fr","Sa"};
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%s %02d.%02d.%02d",
    days[timeinfo.tm_wday], timeinfo.tm_mday,
    timeinfo.tm_mon + 1, timeinfo.tm_year % 100);

  int h1 = timeinfo.tm_hour / 10;
  int h2 = timeinfo.tm_hour % 10;
  int m1 = timeinfo.tm_min  / 10;
  int m2 = timeinfo.tm_min  % 10;

  int startX = 0, startY = 3;
  if (h1 > 0) DrawSegDigit(startX,      startY, h1, clockR, clockG, clockB);
  DrawSegDigit(startX + 11, startY, h2, clockR, clockG, clockB);
  DrawColon(   startX + 23, startY,     clockR, clockG, clockB);
  DrawSegDigit(startX + 27, startY, m1, clockR, clockG, clockB);
  DrawSegDigit(startX + 38, startY, m2, clockR, clockG, clockB);

  display->DisplayText(dateStr, 2, 25, dateR, dateG, dateB, 1);

  for (int y = 0; y < TOTAL_HEIGHT; y++)
    display->DrawPixel(53, y, 100, 100, 100);

  bool mqttStale = (lastMqttWeather > 0 &&
                    millis() - lastMqttWeather > 30UL * 60UL * 1000UL);
  if (weatherAvailable && !mqttStale) {
    WeatherInfo wi = GetWeatherInfo(weatherCode, weatherIsDay);

    DrawWeatherIcon(wi.icon, 55, 3, 2);

    int tempInt = (int)roundf(weatherTemp);
    uint8_t tR, tG, tB;
    if      (tempInt < -10) { tR=0;   tG=0;   tB=120; }
    else if (tempInt <= 0)  { tR=0;   tG=0;   tB=220; }
    else if (tempInt <= 10) { tR=0;   tG=180; tB=255; }
    else if (tempInt <= 15) { tR=255; tG=210; tB=0;   }
    else if (tempInt <= 25) { tR=255; tG=120; tB=0;   }
    else if (tempInt <= 30) { tR=255; tG=0;   tB=0;   }
    else                    { tR=180; tG=0;   tB=0;   }

    int tx = 72;
    if (weatherTemp < -0.5f) {
      for (int dx = 0; dx < 5; dx++)
        for (int dy = 0; dy < 2; dy++)
          display->DrawPixel(tx + dx, 12 + dy, tR, tG, tB);
      tx += 6;
      tempInt = -tempInt;
    }
    if (tempInt >= 10) { DrawSegDigit(tx, 3, tempInt / 10, tR, tG, tB); tx += 11; }
    DrawSegDigit(tx, 3, tempInt % 10, tR, tG, tB);
    tx += 10;
    display->DisplayText("C", tx + 1, 5, tR, tG, tB, 1);

    display->DisplayText(wi.text, 55, 25, dateR, dateG, dateB, 1);

    char humStr[8];
    snprintf(humStr, sizeof(humStr), "H:%u%%", weatherHumidity);
    display->DisplayText(humStr, 101, 2, dateR, dateG, dateB, 1);
    char windStr[9];
    snprintf(windStr, sizeof(windStr), "W:%dkm", (int)roundf(weatherWindSpeed));
    display->DisplayText(windStr, 101, 11, dateR, dateG, dateB, 1);
    char presStr[10];
    snprintf(presStr, sizeof(presStr), "P:%u", weatherPressure);
    display->DisplayText(presStr, 101, 20, dateR, dateG, dateB, 1);
  } else if (mqttStale) {
    display->DisplayText("MQTT", 66, 10, 255, 80, 0, 1);
    display->DisplayText("fehlt", 64, 18, 255, 80, 0, 1);
  } else {
    display->DisplayText("Kein", 64, 10, dateR, dateG, dateB, 1);
    display->DisplayText("Wetter", 61, 18, dateR, dateG, dateB, 1);
  }

  Render();
}

void weatherDisplayForecast() {
  if (!forecastAvailable) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  static uint16_t lastFCode[3] = {0xFFFF, 0xFFFF, 0xFFFF};
  static int8_t   lastFTmax[3] = {-99, -99, -99};
  bool changed = clockColorChanged;
  for (int i = 0; i < 3; i++)
    if (lastFCode[i] != forecastCode[i] || lastFTmax[i] != forecastTempMax[i]) { changed = true; break; }
  if (!changed) {
    for (int y = 0; y < TOTAL_HEIGHT; y++) {
      display->DrawPixel(42, y, 100, 100, 100);
      display->DrawPixel(85, y, 100, 100, 100);
    }
    return;
  }
  clockColorChanged = false;
  for (int i = 0; i < 3; i++) { lastFCode[i] = forecastCode[i]; lastFTmax[i] = forecastTempMax[i]; }

  display->SetBrightness(screensaverBrightness);
  display->ClearScreen();

  const char* dayNames[] = {"So","Mo","Di","Mi","Do","Fr","Sa"};
  const int cellX[3] = {0, 43, 86};

  for (int y = 0; y < TOTAL_HEIGHT; y++) {
    display->DrawPixel(42, y, 100, 100, 100);
    display->DrawPixel(85, y, 100, 100, 100);
  }

  for (int i = 0; i < 3; i++) {
    int cx = cellX[i];
    int dayOfWeek = (timeinfo.tm_wday + 1 + i) % 7;
    WeatherInfo wi = GetWeatherInfo(forecastCode[i]);
    int tempInt = (int)forecastTempMax[i];

    uint8_t tR, tG, tB;
    if      (tempInt < -10) { tR=0;   tG=0;   tB=120; }
    else if (tempInt <= 0)  { tR=0;   tG=0;   tB=220; }
    else if (tempInt <= 10) { tR=0;   tG=180; tB=255; }
    else if (tempInt <= 15) { tR=255; tG=210; tB=0;   }
    else if (tempInt <= 25) { tR=255; tG=120; tB=0;   }
    else if (tempInt <= 30) { tR=255; tG=0;   tB=0;   }
    else                    { tR=180; tG=0;   tB=0;   }

    const char* dn = dayNames[dayOfWeek];
    display->DisplayText(dn, cx + (42 - (int)strlen(dn) * 4) / 2, 0, dateR, dateG, dateB, 1);

    DrawWeatherIcon(wi.icon, cx + 2, 5, 1);

    int tx = cx + 11;
    if (forecastTempMax[i] < 0) {
      for (int dx = 0; dx < 4; dx++)
        display->DrawPixel(tx + dx, 14, tR, tG, tB);
      tx += 5;
      tempInt = -tempInt;
    }
    if (tempInt >= 10) { DrawSegDigit(tx, 5, tempInt / 10, tR, tG, tB); tx += 11; }
    DrawSegDigit(tx, 5, tempInt % 10, tR, tG, tB);
    tx += 10;
    display->DisplayText("C", tx + 1, 7, tR, tG, tB, 1);

    int descWidth = (int)strlen(wi.text) * 4;
    display->DisplayText(wi.text, cx + max(0, (42 - descWidth) / 2), 26, dateR, dateG, dateB, 1);
  }

  Render();
}

#endif // ZEDMD_WIFI

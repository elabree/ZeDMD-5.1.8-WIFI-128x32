#ifdef ZEDMD_WIFI

#include "weather.h"
#include "displayDriver.h"
#include "panel.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <math.h>
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

// 8×8 Wetter-Icons, zwei Layer, scale=Pixelgröße (1=8px, 2=16px)
static void DrawWeatherIcon(uint8_t idx, int x, int y, uint8_t scale = 2) {
  if (idx >= 8) idx = 2;

  // Layer 1 — Hauptform
  static const uint8_t L1[8][8] = {
    // 0 Sonne: kompakter Kern + Strahlen in alle 8 Richtungen
    {0b00011000, 0b01000010, 0b00111100, 0b10111101,
     0b10111101, 0b00111100, 0b01000010, 0b00011000},
    // 1 Teils bew.: kleines Sonnen-Icon (symmetrisch um col 1.5)
    {0b01100000, 0b11110000, 0b01100000, 0b00000000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    // 2 Bedeckt: Wolke (Original 9301b21)
    {0b00000000, 0b00111000, 0b01111110, 0b11111111,
     0b11111111, 0b01111110, 0b00000000, 0b00000000},
    // 3 Regen (unverändert)
    {0b00111100, 0b01111110, 0b11111111, 0b11111111,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    // 4 Schnee: 8px breit, symmetrisch um col 3.5 und row 3.5
    {0b00011000, 0b01000010, 0b00111100, 0b11100111,
     0b11100111, 0b00111100, 0b01000010, 0b00011000},
    // 5 Gewitter (unverändert)
    {0b00111100, 0b01111110, 0b11111111, 0b11111111,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    // 6 Mond (unverändert)
    {0b00111100, 0b01110000, 0b11100000, 0b11100000,
     0b11100000, 0b11100000, 0b01110000, 0b00111100},
    // 7 Teils bewölkt (unverändert)
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b00011100, 0b00111110, 0b01111111, 0b00000000},
  };

  // Layer 1 Farben
  static const uint8_t C1[8][3] = {
    {255, 210,   0},  // 0 Sonne — gelb
    {255, 210,   0},  // 1 Mond
    {155, 155, 165},  // 2 Bedeckt — grau
    {155, 155, 165},  // 3 Regen
    {200, 230, 255},  // 4 Schnee
    {155, 155, 165},  // 5 Gewitter
    {200, 220, 255},  // 6 Mond — blaugrau
    {155, 155, 165},  // 7 Bewölkt
  };

  // Layer 2 — Details (Regen, Blitz, Mondschatten …)
  static const uint8_t L2[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},  // 0 Sonne — kein zweiter Layer
    {0b00000000, 0b00000000, 0b00001100, 0b00111110,
     0b01111111, 0b01111111, 0b00111110, 0b00000000},  // 1 Mond
    {0, 0, 0, 0, 0, 0, 0, 0},  // 2 Bedeckt — kein zweiter Layer
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b01000100, 0b00100010, 0b01000100, 0b00100010},  // 3 Regen — Tropfen
    {0, 0, 0, 0, 0, 0, 0, 0},  // 4 Schnee
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b00000100, 0b00010000, 0b00000100, 0b00010000},  // 5 Gewitter — Blitz (1px, scharfer zig-zag)
    {0, 0, 0, 0, 0, 0, 0, 0},  // 6 Mond
    {0b01110000, 0b11000000, 0b11000000, 0b01110000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},  // 7 Bewölkt
  };

  // Layer 2 Farben
  static const uint8_t C2[8][3] = {
    {  0,   0,   0},  // 0
    {170, 170, 175},  // 1 Mond
    {  0,   0,   0},  // 2
    {100, 160, 255},  // 3 Regen — blau
    {  0,   0,   0},  // 4
    {255, 210,   0},  // 5 Gewitter — gelber Blitz
    {  0,   0,   0},  // 6
    {200, 220, 255},  // 7
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
  // Sonne: per-Pixel-Gradient (weißer Kern → Gelb → Orange an den Strahlenspitzen)
  if (idx == 0) {
    #define _O {  0,  0,  0}   // aus
    #define _T {255,150,  0}   // Strahlenspitzen — Orange
    #define _R {255,190,  0}   // Diagonalstrahlen — Goldgelb
    #define _I {255,230, 60}   // innerer Ring — Gelb
    #define _C {255,255,160}   // Kern — weißlich-gelb
    static const uint8_t SUN[8][8][3] = {
      {_O, _O, _O, _T, _T, _O, _O, _O},
      {_O, _R, _O, _O, _O, _O, _R, _O},
      {_O, _O, _I, _I, _I, _I, _O, _O},
      {_T, _O, _I, _C, _C, _I, _O, _T},
      {_T, _O, _I, _C, _C, _I, _O, _T},
      {_O, _O, _I, _I, _I, _I, _O, _O},
      {_O, _R, _O, _O, _O, _O, _R, _O},
      {_O, _O, _O, _T, _T, _O, _O, _O},
    };
    #undef _O
    #undef _T
    #undef _R
    #undef _I
    #undef _C
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        uint8_t r = SUN[row][col][0], g = SUN[row][col][1], b = SUN[row][col][2];
        if (!r && !g && !b) continue;
        int px = x + col * scale, py = y + row * scale;
        for (int dy = 0; dy < scale; dy++)
          for (int dx = 0; dx < scale; dx++)
            display->DrawPixel(px + dx, py + dy, r, g, b);
      }
    }
    return;
  }

  // Helfer: Layer mit per-Zeile unterschiedlicher Farbe zeichnen
  auto drawLayerGrad = [&](const uint8_t bmp[8], const uint8_t rc[8][3]) {
    for (int row = 0; row < 8; row++) {
      uint8_t mask = bmp[row];
      if (!mask) continue;
      uint8_t r = rc[row][0], g = rc[row][1], b = rc[row][2];
      if (!r && !g && !b) continue;
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

  if (idx == 1) {  // Teils bewölkt: kleine Sonne + Wolke
    static const uint8_t G1[8][3] = {
      {255,200, 40},{255,180, 10},{255,200, 40},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t G2[8][3] = {
      {  0,  0,  0},{  0,  0,  0},
      {200,205,215},{182,187,198},{165,170,182},{150,155,167},{140,145,157},{  0,  0,  0}};
    drawLayerGrad(L1[1], G1);
    drawLayerGrad(L2[1], G2);
    return;
  }
  if (idx == 2) {  // Wolke
    static const uint8_t G1[8][3] = {
      {  0,  0,  0},
      {212,217,227},{192,197,208},{172,177,188},{160,165,177},{148,153,165},
      {  0,  0,  0},{  0,  0,  0}};
    drawLayerGrad(L1[2], G1);
    return;
  }
  if (idx == 3) {  // Regen: dunkle Wolke + blaue Tropfen
    static const uint8_t G1[8][3] = {
      {142,147,160},{130,135,148},{118,123,136},{108,113,125},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t G2[8][3] = {
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
      { 80,165,255},{ 65,145,240},{ 80,165,255},{ 65,145,240}};
    drawLayerGrad(L1[3], G1);
    drawLayerGrad(L2[3], G2);
    return;
  }
  if (idx == 4) {  // Schnee: blauweißer Eiskristall, Mitte heller
    static const uint8_t G1[8][3] = {
      {170,200,255},{155,188,255},{180,212,255},{200,228,255},
      {200,228,255},{180,212,255},{155,188,255},{170,200,255}};
    drawLayerGrad(L1[4], G1);
    return;
  }
  if (idx == 5) {  // Gewitter: sehr dunkle Wolke + heller Blitz
    static const uint8_t G1[8][3] = {
      {118,123,136},{108,113,125},{ 98,103,115},{ 90, 95,108},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t G2[8][3] = {
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
      {255,255,220},{255,240,100},{255,210, 30},{255,170,  0}};
    drawLayerGrad(L1[5], G1);
    drawLayerGrad(L2[5], G2);
    return;
  }
  if (idx == 6) {  // Mond: warm-cremefarbige Sichel, Innenkante heller
    #define _OO {  0,  0,  0}
    #define _DI {220,215,182}
    #define _MD {242,237,202}
    #define _BR {255,252,225}
    static const uint8_t MOON[8][8][3] = {
      {_OO,_OO,_MD,_MD,_DI,_DI,_OO,_OO},
      {_OO,_MD,_BR,_MD,_OO,_OO,_OO,_OO},
      {_DI,_MD,_BR,_OO,_OO,_OO,_OO,_OO},
      {_DI,_MD,_BR,_OO,_OO,_OO,_OO,_OO},
      {_DI,_MD,_BR,_OO,_OO,_OO,_OO,_OO},
      {_DI,_MD,_BR,_OO,_OO,_OO,_OO,_OO},
      {_OO,_MD,_BR,_MD,_OO,_OO,_OO,_OO},
      {_OO,_OO,_MD,_MD,_DI,_DI,_OO,_OO},
    };
    #undef _OO
    #undef _DI
    #undef _MD
    #undef _BR
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        uint8_t r = MOON[row][col][0], g = MOON[row][col][1], b = MOON[row][col][2];
        if (!r && !g && !b) continue;
        int px = x + col * scale, py = y + row * scale;
        for (int dy = 0; dy < scale; dy++)
          for (int dx = 0; dx < scale; dx++)
            display->DrawPixel(px+dx, py+dy, r, g, b);
      }
    return;
  }
  if (idx == 7) {  // Teils bewölkt: Wolke + kleines Sonneneck
    static const uint8_t G1[8][3] = {
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
      {205,210,220},{182,187,198},{160,165,177},{  0,  0,  0}};
    static const uint8_t G2[8][3] = {
      {255,210, 50},{255,155, 10},{255,155, 10},{255,210, 50},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    drawLayerGrad(L1[7], G1);
    drawLayerGrad(L2[7], G2);
    return;
  }

  drawLayer(L1[idx], C1[idx][0], C1[idx][1], C1[idx][2]);
  if (C2[idx][0] || C2[idx][1] || C2[idx][2])
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

// ── 16×16 Wetter-Icons (direkt gezeichnet, keine Skalierung) ─────────────────
static void DrawWeatherIcon16(uint8_t idx, int x, int y) {
  if (idx >= 8) idx = 2;

  auto px = [&](int dx, int dy, uint8_t r, uint8_t g, uint8_t b) {
    if (dx < 0 || dx > 15 || dy < 0 || dy > 15) return;
    display->DrawPixel(x + dx, y + dy, r, g, b);
  };

  // Wolken-Bitmap: 16 Spalten × 9 Zeilen, bit15=col0, bit0=col15
  static const uint16_t CLD[9] = {
    0x07E0, // row 0: cols 5-10
    0x1FF8, // row 1: cols 3-12
    0x3FFC, // row 2: cols 2-13
    0x7FFE, // row 3: cols 1-14
    0xFFFF, // row 4: alle 16
    0xFFFF, // row 5: alle 16
    0xFFFF, // row 6: alle 16
    0x7FFE, // row 7: cols 1-14
    0x3FFC, // row 8: cols 2-13
  };
  // Farbverlauf: oben hell (Highlight) → unten dunkel (Schatten)
  static const uint8_t LC[9][3] = {  // normale Wolke
    {215,220,232},{200,205,218},{185,190,205},
    {172,177,192},{160,165,180},{150,155,170},
    {140,145,160},{130,135,150},{118,123,138}
  };
  static const uint8_t DC[9][3] = {  // Gewitterwolke (dunkler)
    {128,131,145},{115,118,132},{103,106,120},
    { 93, 96,110},{ 84, 87,101},{ 76, 79, 93},
    { 68, 71, 85},{ 60, 63, 77},{ 52, 55, 69}
  };

  auto drawCloud = [&](int dy0, bool dark) {
    const uint8_t (*c)[3] = dark ? DC : LC;
    for (int r = 0; r < 9; r++) {
      uint16_t m = CLD[r];
      for (int col = 0; col < 16; col++)
        if (m & (0x8000 >> col))
          px(col, dy0 + r, c[r][0], c[r][1], c[r][2]);
    }
  };

  switch (idx) {

  case 0: { // ☀️ Sonne — Kreis mit 8 Strahlen
    for (int dy = 0; dy < 16; dy++) {
      for (int dx = 0; dx < 16; dx++) {
        float fx = dx - 7.5f, fy = dy - 7.5f;
        float d = sqrtf(fx * fx + fy * fy);
        if      (d <= 2.0f) px(dx, dy, 255, 255, 200);
        else if (d <= 3.0f) px(dx, dy, 255, 245, 140);
        else if (d <= 4.0f) px(dx, dy, 255, 220,  55);
        else if (d <= 4.5f) px(dx, dy, 255, 188,   5);
      }
    }
    // N/S-Strahlen (2px breit, 2px lang)
    for (int i = 0; i < 2; i++) {
      px(7+i, 0, 255, 155, 0); px(7+i, 1, 255, 170, 0);
      px(7+i,14, 255, 155, 0); px(7+i,15, 255, 155, 0);
    }
    // W/E-Strahlen
    for (int i = 0; i < 2; i++) {
      px( 0, 7+i, 255, 155, 0); px( 1, 7+i, 255, 170, 0);
      px(14, 7+i, 255, 155, 0); px(15, 7+i, 255, 155, 0);
    }
    // Diagonale Strahlen (1px pro Ecke)
    px( 2,  2, 255, 162, 0); px( 1,  1, 255, 148, 0);
    px(13,  2, 255, 162, 0); px(14,  1, 255, 148, 0);
    px( 2, 13, 255, 162, 0); px( 1, 14, 255, 148, 0);
    px(13, 13, 255, 162, 0); px(14, 14, 255, 148, 0);
    break;
  }

  case 1: { // ⛅ Teils bewölkt: kleine Sonne oben-links + Wolke unten-rechts
    // Kleine Sonne (r=3) zentriert bei (3,3)
    for (int dy = 0; dy < 8; dy++) {
      for (int dx = 0; dx < 8; dx++) {
        float fx = dx - 3.0f, fy = dy - 3.0f;
        float d  = sqrtf(fx * fx + fy * fy);
        if      (d <= 1.2f) px(dx, dy, 255, 255, 185);
        else if (d <= 2.2f) px(dx, dy, 255, 225,  55);
        else if (d <= 3.0f) px(dx, dy, 255, 178,   5);
      }
    }
    px(3,0,255,148,0); px(0,3,255,148,0);
    px(6,3,255,148,0); px(3,6,255,148,0);
    px(1,1,255,152,0); px(5,1,255,152,0);
    px(1,5,255,152,0); px(5,5,255,152,0);
    // Kleine Wolke rechts unten (cols 6-15, rows 9-14)
    static const uint16_t SC[6] = {
      0x003C, // row 9:  cols 10-13
      0x00FF, // row 10: cols  8-15
      0x01FF, // row 11: cols  7-15
      0x03FF, // row 12: cols  6-15
      0x03FF, // row 13: cols  6-15
      0x01FE, // row 14: cols  7-14
    };
    static const uint8_t SG[6][3] = {
      {210,215,228},{193,198,213},{178,183,198},
      {163,168,183},{150,155,170},{138,143,158}
    };
    for (int r = 0; r < 6; r++) {
      uint16_t m = SC[r];
      for (int col = 0; col < 16; col++)
        if (m & (0x8000 >> col)) px(col, 9+r, SG[r][0], SG[r][1], SG[r][2]);
    }
    break;
  }

  case 2: { // ☁️ Wolke — vertikal zentriert
    drawCloud(3, false);
    break;
  }

  case 3: { // 🌧️ Regen — Wolke + diagonale Tropfen
    drawCloud(0, false);
    // 4 Tropfen à 3px diagonal, direkt ab Wolkenunterkante (row 9)
    static const int8_t DRX[] = { 3, 6,  9, 12};
    for (int d = 0; d < 4; d++) {
      px(DRX[d],   9,  95, 185, 255);
      px(DRX[d]-1,10,  70, 160, 245);
      px(DRX[d]-2,11,  50, 138, 230);
    }
    break;
  }

  case 4: { // ❄️ Schneeflocke — Kreuz mit Ästen
    // Horizontaler Arm (2px hoch), volle Breite
    for (int c = 0; c < 16; c++) {
      uint8_t v = (c>=6&&c<=9) ? 225 : (c>=4&&c<=11 ? 195 : 165);
      px(c, 7, v, v+12, 255);
      px(c, 8, v, v+12, 255);
    }
    // Vertikaler Arm (2px breit), volle Höhe
    for (int r = 0; r < 16; r++) {
      uint8_t v = (r>=6&&r<=9) ? 225 : (r>=4&&r<=11 ? 195 : 165);
      px(7, r, v, v+12, 255);
      px(8, r, v, v+12, 255);
    }
    // Äste senkrecht auf jedem Arm (symmetrisch bei pos 3 und 12)
    static const int8_t AP[] = {3, 12};
    for (int i = 0; i < 2; i++) {
      int a = AP[i];
      px(a, 5, 175,188,255); px(a, 6, 182,195,255);  // über horizontalem Arm
      px(a, 9, 182,195,255); px(a,10, 175,188,255);  // unter horizontalem Arm
      px(5, a, 175,188,255); px(6, a, 182,195,255);  // links vom vertikalen Arm
      px(9, a, 182,195,255); px(10,a, 175,188,255);  // rechts vom vertikalen Arm
    }
    // Kern extra hell
    px(7,7,242,250,255); px(8,7,242,250,255);
    px(7,8,242,250,255); px(8,8,242,250,255);
    break;
  }

  case 5: { // ⛈️ Gewitter — sehr dunkle Wolke + Blitz (2px, Z mit horizontalem Kink)
    drawCloud(0, true);
    // Oberer Arm: 2px breit, diagonal nach links-unten (3px weiter links)
    px( 6, 9, 255,255,195); px( 7, 9, 255,255,195);
    px( 5,10, 255,248,165); px( 6,10, 255,248,165);
    // Horizontaler Kink (5px, der hellste Punkt — charakteristisches Blitz-Knick)
    for (int c = 5; c <= 9; c++) px(c, 11, 255, 232, 95);
    // Unterer Arm: 2px breit, diagonal nach links-unten (SW)
    px( 8,12, 255,210, 40); px( 9,12, 255,210, 40);
    px( 7,13, 255,185, 12); px( 8,13, 255,185, 12);
    px( 6,14, 255,155,  0); px( 7,14, 255,155,  0);
    px( 6,15, 240,125,  0);
    break;
  }

  case 6: { // 🌙 Mond — Sichel per Kreis-Subtraktion
    for (int dy = 0; dy < 16; dy++) {
      for (int dx = 0; dx < 16; dx++) {
        float fx   = dx - 7.5f, fy = dy - 7.5f;
        float dOut = sqrtf(fx * fx + fy * fy);
        float dIn  = sqrtf((dx - 10.5f) * (dx - 10.5f) + fy * fy);
        if (dOut <= 7.0f && dIn > 6.0f) {
          float t = dOut / 7.0f;
          px(dx, dy,
             (uint8_t)(255 - t * 30),
             (uint8_t)(250 - t * 38),
             (uint8_t)(205 - t * 50));
        }
      }
    }
    break;
  }

  case 7: { // Teils bewölkt: Sonnenecke oben-links + Wolke
    // Kleines Sonneneck (r=2.5) bei (2,2)
    for (int dy = 0; dy < 6; dy++) {
      for (int dx = 0; dx < 6; dx++) {
        float d = sqrtf((dx-2.5f)*(dx-2.5f) + (dy-2.5f)*(dy-2.5f));
        if      (d <= 1.2f) px(dx, dy, 255, 255, 185);
        else if (d <= 2.0f) px(dx, dy, 255, 215,  40);
        else if (d <= 2.8f) px(dx, dy, 255, 170,   0);
      }
    }
    px(2,0,255,145,0); px(0,2,255,145,0);
    px(4,0,255,145,0); px(0,4,255,145,0);
    drawCloud(4, false);
    break;
  }

  } // switch
}

void weatherIconTest() {
  // Zeigt alle 8 Wetter-Icons in 2 Reihen à 4 (füllt 128×32 komplett)
  display->SetBrightness(screensaverBrightness);
  display->ClearScreen();
  for (uint8_t i = 0; i < 8; i++) {
    DrawWeatherIcon16(i, (i % 4) * 32 + 8, (i < 4) ? 0 : 16);
  }
  Render();
}

void weatherDisplay() {
  display->SetBrightness(screensaverBrightness);
  display->ClearScreen();

  WeatherInfo wi = GetWeatherInfo(weatherCode, weatherIsDay);

  DrawWeatherIcon16(wi.icon, 1, 8);

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

    DrawWeatherIcon16(wi.icon, 55, 3);

    int tempInt = (int)roundf(weatherTemp);
    uint8_t tR, tG, tB;
    if      (tempInt < -10) { tR=0;   tG=0;   tB=120; }
    else if (tempInt <= 0)  { tR=0;   tG=0;   tB=220; }
    else if (tempInt <= 10) { tR=0;   tG=180; tB=255; }
    else if (tempInt <= 15) { tR=255; tG=210; tB=0;   }
    else if (tempInt <= 25) { tR=255; tG=120; tB=0;   }
    else if (tempInt <= 30) { tR=255; tG=0;   tB=0;   }
    else                    { tR=180; tG=0;   tB=0;   }

    // Zentriert in Zone x=72..96 (zwischen Icon-Ende und Nebendaten)
    bool negative = (weatherTemp < -0.5f);
    int absTemp = negative ? -tempInt : tempInt;
    int w = 0;
    if (negative)      w += 6;   // Minuszeichen
    if (absTemp >= 10) w += 11;  // Zehner
    w += 10;                     // Einer
    w += 5;                      // Lücke + "C"
    int tx = 72 + (25 - w) / 2;
    if (tx < 72) tx = 72;
    if (negative) {
      for (int dx = 0; dx < 5; dx++)
        for (int dy = 0; dy < 2; dy++)
          display->DrawPixel(tx + dx, 12 + dy, tR, tG, tB);
      tx += 6;
    }
    if (absTemp >= 10) { DrawSegDigit(tx, 3, absTemp / 10, tR, tG, tB); tx += 11; }
    DrawSegDigit(tx, 3, absTemp % 10, tR, tG, tB);
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

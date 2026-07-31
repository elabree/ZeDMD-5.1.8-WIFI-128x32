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

// ── Externals from main.cpp ───────────────────────────────────────────────────

extern DisplayDriver*    display;
extern uint8_t           screensaverBrightness;
extern bool              wifiActive;

extern void Render();
extern void logMsg(const char* fmt, ...);
extern void ApplyBrightness(uint8_t base);
extern const uint8_t* GetIcon(const char* name);

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

// ── Internal types and helpers ────────────────────────────────────────────────

struct WeatherInfo { const char* text; uint8_t icon; };

static WeatherInfo GetWeatherInfo(uint16_t code, bool isDay = true) {
  if (!isDay && (code == 0 || code == 1)) return {"Klar",        6};
  if (!isDay && code == 2)                return {"Teils bew.",  7};
  if (code == 0)                       return {"Sonnig",        0};
  if (code == 1)                       return {"Heiter",        0};
  if (code == 2)                       return {"Teils bew.",    1};
  if (code == 3)                       return {"Bedeckt",       2};
  if (code == 45 || code == 48)        return {"Nebel",        10};
  if (code >= 51 && code <= 57)        return {"Nieselregen",   3};
  if (code >= 61 && code <= 67)        return {"Regen",         8};
  if (code >= 71 && code <= 77)        return {"Schnee",        4};
  if (code >= 80 && code <= 82)        return {"Schauer",       9};
  if (code >= 85 && code <= 86)        return {"Schneeschr.",   4};
  if (code >= 95)                      return {"Gewitter",      5};
  return {"?", 255};
}

static void getTempColor(int temp, uint8_t &r, uint8_t &g, uint8_t &b) {
  if      (temp < -10) { r=0;   g=0;   b=120; }
  else if (temp <=  0) { r=0;   g=0;   b=220; }
  else if (temp <= 10) { r=0;   g=180; b=255; }
  else if (temp <= 15) { r=255; g=210; b=0;   }
  else if (temp <= 25) { r=255; g=120; b=0;   }
  else if (temp <= 30) { r=255; g=0;   b=0;   }
  else                 { r=180; g=0;   b=0;   }
}

// 8x8 weather icons, two layers, scale = pixel size (1=8px, 2=16px)
static void DrawWeatherIcon(uint8_t idx, int x, int y, uint8_t scale = 2) {
  if (idx == 255) {
    display->DisplayText("?", x + scale, y + scale, 128, 128, 128, scale);
    return;
  }
  if (idx >= 11) idx = 2;

  // Layer 1 — main shape
  static const uint8_t L1[8][8] = {
    // 0 Sun: compact core + rays in all 8 directions
    {0b00011000, 0b01000010, 0b00111100, 0b10111101,
     0b10111101, 0b00111100, 0b01000010, 0b00011000},
    // 1 Partly cloudy: small sun icon (symmetric around col 1.5)
    {0b01100000, 0b11110000, 0b01100000, 0b00000000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    // 2 Overcast: cloud (original 9301b21)
    {0b00000000, 0b00111000, 0b01111110, 0b11111111,
     0b11111111, 0b01111110, 0b00000000, 0b00000000},
    // 3 Rain (unchanged)
    {0b00111100, 0b01111110, 0b11111111, 0b11111111,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    // 4 Snow: 8px wide, symmetric around col 3.5 and row 3.5
    {0b00011000, 0b01000010, 0b00111100, 0b11100111,
     0b11100111, 0b00111100, 0b01000010, 0b00011000},
    // 5 Thunderstorm (unchanged)
    {0b00111100, 0b01111110, 0b11111111, 0b11111111,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},
    // 6 Moon (unchanged)
    {0b00111100, 0b01110000, 0b11100000, 0b11100000,
     0b11100000, 0b11100000, 0b01110000, 0b00111100},
    // 7 Partly cloudy (unchanged)
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b00011100, 0b00111110, 0b01111111, 0b00000000},
  };

  // Layer 1 colors
  static const uint8_t C1[8][3] = {
    {255, 210,   0},  // 0 Sun — yellow
    {255, 210,   0},  // 1 Moon
    {155, 155, 165},  // 2 Overcast — grey
    {155, 155, 165},  // 3 Rain
    {200, 230, 255},  // 4 Snow
    {155, 155, 165},  // 5 Thunderstorm
    {200, 220, 255},  // 6 Moon — blue-grey
    {155, 155, 165},  // 7 Cloudy
  };

  // Layer 2 — details (rain, lightning, moon shadow …)
  static const uint8_t L2[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},  // 0 Sun — no second layer
    {0b00000000, 0b00000000, 0b00001100, 0b00111110,
     0b01111111, 0b01111111, 0b00111110, 0b00000000},  // 1 Moon
    {0, 0, 0, 0, 0, 0, 0, 0},  // 2 Overcast — no second layer
    {0b00000000, 0b00000000, 0b00000000, 0b00000000,
     0b01000100, 0b00100010, 0b01000100, 0b00100010},  // 3 Rain — drops
    {0, 0, 0, 0, 0, 0, 0, 0},  // 4 Snow
    {0b00001000, 0b00010000, 0b00111100, 0b00001000,
     0b00010000, 0b00100000, 0b01000000, 0b00000000},  // 5 Thunderstorm — lightning bolt
    {0, 0, 0, 0, 0, 0, 0, 0},  // 6 Moon
    {0b01110000, 0b11000000, 0b11000000, 0b01110000,
     0b00000000, 0b00000000, 0b00000000, 0b00000000},  // 7 Cloudy
  };

  // Layer 2 colors
  static const uint8_t C2[8][3] = {
    {  0,   0,   0},  // 0
    {170, 170, 175},  // 1 Moon
    {  0,   0,   0},  // 2
    {100, 160, 255},  // 3 Rain — blue
    {  0,   0,   0},  // 4
    {255, 210,   0},  // 5 Thunderstorm — yellow lightning
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
  // Sun: per-pixel gradient (white core → yellow → orange at ray tips)
  if (idx == 0) {
    #define _O {  0,  0,  0}   // off
    #define _T {255,150,  0}   // ray tips — orange
    #define _R {255,190,  0}   // diagonal rays — gold yellow
    #define _I {255,230, 60}   // inner ring — yellow
    #define KK {255,255,160}   // core — whitish yellow
    static const uint8_t SUN[8][8][3] = {
      {_O, _O, _O, _T, _T, _O, _O, _O},
      {_O, _R, _O, _O, _O, _O, _R, _O},
      {_O, _O, _I, _I, _I, _I, _O, _O},
      {_T, _O, _I, KK, KK, _I, _O, _T},
      {_T, _O, _I, KK, KK, _I, _O, _T},
      {_O, _O, _I, _I, _I, _I, _O, _O},
      {_O, _R, _O, _O, _O, _O, _R, _O},
      {_O, _O, _O, _T, _T, _O, _O, _O},
    };
    #undef _O
    #undef _T
    #undef _R
    #undef _I
    #undef KK
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

  // helper: draw layer with per-row color gradient
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

  if (idx == 1) {  // partly cloudy: small sun + cloud
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
  if (idx == 2) {  // cloud
    static const uint8_t G1[8][3] = {
      {  0,  0,  0},
      {212,217,227},{192,197,208},{172,177,188},{160,165,177},{148,153,165},
      {  0,  0,  0},{  0,  0,  0}};
    drawLayerGrad(L1[2], G1);
    return;
  }
  if (idx == 3) {  // drizzle — 2 vertical drops (cols 3+6, rows 5–6)
    static const uint8_t G1[8][3] = {
      {120,125,138},{108,113,126},{ 96,101,114},{ 86, 91,104},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t DRIZZLE[8] = {
      0,0,0,0, 0,0b00100100,0,0b00100100};
    drawLayerGrad(L1[3], G1);
    drawLayer(DRIZZLE, 90, 175, 255);
    return;
  }
  if (idx == 4) {  // snow: blue-white ice crystal, brighter centre
    static const uint8_t G1[8][3] = {
      {170,200,255},{155,188,255},{180,212,255},{200,228,255},
      {200,228,255},{180,212,255},{155,188,255},{170,200,255}};
    drawLayerGrad(L1[4], G1);
    return;
  }
  if (idx == 5) {  // thunderstorm: very dark cloud + bright lightning
    static const uint8_t G1[8][3] = {
      {  0,  0,  0},{ 96,101,114},{ 86, 91,104},{ 76, 81, 94},
      { 68, 73, 86},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t G2[8][3] = {
      {255,230,  0},{255,230,  0},{255,230,  0},{255,230,  0},
      {255,230,  0},{255,230,  0},{255,230,  0},{  0,  0,  0}};
    // cloud shifted 1 row down (rows 1–4), lightning pixels cut out
    static const uint8_t CLOUD5[8] = {
      0, 0b00101100, 0b01000010, 0b11110111,
      0b11101111,0,0,0};
    drawLayerGrad(CLOUD5, G1);
    drawLayerGrad(L2[5], G2);
    return;
  }
  if (idx == 6) {  // moon: warm cream crescent, brighter inner edge
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
  if (idx == 7) {  // partly cloudy night: crescent moon (cream white) + cloud like icon 1
    static const uint8_t GC[8][3] = {  // cloud (L2[1] shape)
      {  0,  0,  0},{  0,  0,  0},
      {200,205,215},{182,187,198},{165,170,182},{150,155,167},{140,145,157},{  0,  0,  0}};
    static const uint8_t GM[8][3] = {  // crescent moon — cream white instead of yellow
      {242,237,202},{220,215,182},{220,215,182},{242,237,202},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    drawLayerGrad(L2[1], GC);
    drawLayerGrad(L2[7], GM);
    return;
  }
  if (idx == 8) {  // rain — vertical drops
    static const uint8_t G1[8][3] = {
      {108,113,126},{ 96,101,114},{ 86, 91,104},{ 78, 83, 96},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t DROPS[8] = {  // diagonal \ over 4 rows
      0,0,0,0, 0b00100100,0b00010010,0b00100100,0b00010010};
    drawLayerGrad(L1[3], G1);
    drawLayer(DROPS, 90, 175, 255);
    return;
  }
  if (idx == 9) {  // showers — sun top-left + cloud (1px higher) + diagonal drops
    static const uint8_t GS[8][3] = {  // sun (same as icon 1)
      {255,200, 40},{255,180, 10},{255,200, 40},
      {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t CLOUD9[8] = {  // cloud 1px higher than L2[1], extended left
      0b00000000, 0b00001100, 0b00111110, 0b11111111,
      0b11111111, 0b01111110, 0b00000000, 0b00000000};
    static const uint8_t GC[8][3] = {  // cloud gradient like icon 8 (darker)
      {  0,  0,  0},{108,113,126},{ 96,101,114},{ 86, 91,104},
      { 78, 83, 96},{ 68, 73, 86},{  0,  0,  0},{  0,  0,  0}};
    static const uint8_t DRPS[8] = {  // diagonale Tropfen Zeilen 6–7
      0,0,0,0, 0,0, 0b00100010, 0b00010001};
    drawLayerGrad(L1[1], GS);
    drawLayerGrad(CLOUD9, GC);
    drawLayer(DRPS, 90, 175, 255);
    return;
  }
  if (idx == 10) {  // fog — horizontal stripes, no cloud block
    static const uint8_t FOG[8] = {
      0b00000000, 0b11111100, 0b00000000, 0b00111111,
      0b00000000, 0b11111100, 0b00000000, 0b00000000};
    drawLayer(FOG, 185, 192, 205);
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
  logMsg("Weather: fetch start, heap free=%u internal-max=%u",
         esp_get_free_heap_size(),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // response body in PSRAM — prevents 20-30 KB allocation in internal SRAM heap
  const size_t WX_BUF = 32768;
  char* body = (char*)heap_caps_malloc(WX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!body) { logMsg("Weather: PSRAM buffer alloc failed"); return; }

  WiFiClient client;
  HTTPClient http;
  // HTTP (no TLS) — Open-Meteo supports plain HTTP for embedded devices;
  // internal RAM is insufficient for mbedTLS handshake when audio codec is active
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
        logMsg("Weather: code=%d isDay=%d (1=day)", weatherCode, extractInt("\"is_day\"", curStart));
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
        logMsg("Weather: forecast OK (codes %d/%d/%d)", forecastCode[0], forecastCode[1], forecastCode[2]);
      } else {
        logMsg("Weather: forecast parsing failed");
      }
    } else {
      logMsg("Weather: no hourly/daily block in response");
    }
  } else {
    logMsg("Weather: HTTP error %d", httpCode);
  }
  http.end();
  free(body);
}

static TaskHandle_t wxTaskHandle = NULL;

// Runs once, then suspends — reactivated via vTaskResume instead of being recreated,
// so wxTaskBuf (StaticTask_t) is never reused before the idle task has cleaned it up.
static void weatherFetchTask(void* pvParams) {
  while (true) {
    fetchWeather();
    weatherFetchRunning = false;
    vTaskSuspend(NULL);
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

void weatherInit() {
  // placeholder — future initialization (e.g. mutex for weather data) goes here
}

void weatherTrigger() {
  if (weatherFetchRunning) return;
  weatherFetchRunning = true;
  if (!wxTaskHandle) {
    // first call: create task once (stack persists for the entire runtime)
    static StaticTask_t wxTaskBuf;
    static StackType_t* wxStack = nullptr;
    if (!wxStack) {
      wxStack = (StackType_t*)heap_caps_malloc(20480, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!wxStack) {
      weatherFetchRunning = false;
      logMsg("Weather: stack alloc failed");
      return;
    }
    wxTaskHandle = xTaskCreateStatic(weatherFetchTask, "wxFetch", 20480 / sizeof(StackType_t),
                                     NULL, 1, wxStack, &wxTaskBuf);
    if (!wxTaskHandle) {
      weatherFetchRunning = false;
      logMsg("Weather: task start failed");
    }
  } else {
    vTaskResume(wxTaskHandle);
  }
}

bool weatherIsAvailable() {
  return weatherAvailable;
}

// ── 16×16 weather icons (drawn directly, no scaling) ─────────────────────────
static void DrawWeatherIcon16(uint8_t idx, int x, int y) {
  if (idx == 255) {
    const uint8_t* q = GetIcon("question");
    if (q) {
      // question.rgba is 20x20 — draw 16x16 centered crop (offset 2,2)
      for (int dy = 0; dy < 16; dy++) {
        for (int dx = 0; dx < 16; dx++) {
          const uint8_t* p = q + ((dy + 2) * 20 + (dx + 2)) * 4;
          if (p[3] < 32) continue;
          display->DrawPixel(x + dx, y + dy, p[0], p[1], p[2]);
        }
      }
    } else {
      display->DisplayText("?", x + 4, y + 4, 128, 128, 128, 2);
    }
    return;
  }
  if (idx >= 11) idx = 2;

  auto px = [&](int dx, int dy, uint8_t r, uint8_t g, uint8_t b) {
    if (dx < 0 || dx > 15 || dy < 0 || dy > 15) return;
    display->DrawPixel(x + dx, y + dy, r, g, b);
  };

  // cloud bitmap: 16 cols x 9 rows, bit15=col0, bit0=col15
  static const uint16_t CLD[9] = {
    0x0F00, // row 0: cols 4-7 (left bump, higher)
    0x1FB8, // row 1: cols 3-8 + cols 10-12 (Spalt bei col 9)
    0x3FFC, // row 2: cols 2-13
    0x7FFE, // row 3: cols 1-14
    0xFFFF, // row 4: alle 16
    0xFFFF, // row 5: alle 16
    0xFFFF, // row 6: alle 16
    0x7FFE, // row 7: cols 1-14
    0x3FFC, // row 8: cols 2-13
  };
  // gradient: bright top (highlight) → dark bottom (shadow)
  static const uint8_t LC[9][3] = {  // normal cloud (+20 brighter)
    {235,240,252},{220,225,238},{205,210,225},
    {192,197,212},{180,185,200},{170,175,190},
    {160,165,180},{150,155,170},{138,143,158}
  };
  static const uint8_t DC[9][3] = {  // storm cloud (-20 darker)
    {108,111,125},{ 95, 98,112},{ 83, 86,100},
    { 73, 76, 90},{ 64, 67, 81},{ 56, 59, 73},
    { 48, 51, 65},{ 40, 43, 57},{ 32, 35, 49}
  };
  static const uint8_t RC[9][3] = {  // rain cloud (65% toward DC)
    {152,156,169},{139,142,156},{126,129,144},
    {115,118,133},{105,108,123},{ 96,100,114},
    { 87, 91,105},{ 79, 82, 97},{ 69, 73, 87}
  };

  // style: 0=normal (LC), 1=rain (RC), 2=storm (DC)
  auto drawCloud = [&](int dy0, uint8_t style) {
    const uint8_t (*c)[3] = (style == 2) ? DC : (style == 1) ? RC : LC;
    for (int r = 0; r < 9; r++) {
      uint16_t m = CLD[r];
      for (int col = 0; col < 16; col++)
        if (m & (0x8000 >> col))
          px(col, dy0 + r, c[r][0], c[r][1], c[r][2]);
    }
  };

  switch (idx) {

  case 0: { // ☀️ sun — circle with 8 rays
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
    // N/S rays (2px wide, 2px long)
    for (int i = 0; i < 2; i++) {
      px(7+i, 0, 255, 155, 0); px(7+i, 1, 255, 170, 0);
      px(7+i,14, 255, 155, 0); px(7+i,15, 255, 155, 0);
    }
    // W/E rays
    for (int i = 0; i < 2; i++) {
      px( 0, 7+i, 255, 155, 0); px( 1, 7+i, 255, 170, 0);
      px(14, 7+i, 255, 155, 0); px(15, 7+i, 255, 155, 0);
    }
    // diagonal rays (1px per corner)
    px( 2,  2, 255, 162, 0); px( 1,  1, 255, 148, 0);
    px(13,  2, 255, 162, 0); px(14,  1, 255, 148, 0);
    px( 2, 13, 255, 162, 0); px( 1, 14, 255, 148, 0);
    px(13, 13, 255, 162, 0); px(14, 14, 255, 148, 0);
    break;
  }

  case 1: { // ⛅ partly cloudy: small sun top-left + large cloud below
    // small sun (r=3) centered at (3,3)
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
    drawCloud(4, 0);
    break;
  }

  case 2: { // ☁️ cloud — vertically centered
    drawCloud(3, 0);
    break;
  }

  case 3: { // 🌦️ drizzle — cloud + 6 individual drop dots
    drawCloud(0, 1);
    static const int8_t DX[] = {4, 8, 12};
    for (int d = 0; d < 3; d++) {
      px(DX[d], 10, 90, 175, 255);
      px(DX[d], 13, 90, 175, 255);
    }
    break;
  }

  case 4: { // ❄️ snow — two 5×5 snowflakes (cross + diagonal tips)
    drawCloud(0, 0);
    auto flake = [&](int cx, int cy) {
      px(cx-2,cy-2,255,255,255); px(cx,cy-2,255,255,255); px(cx+2,cy-2,255,255,255);
      px(cx-1,cy-1,255,255,255); px(cx,cy-1,255,255,255); px(cx+1,cy-1,255,255,255);
      for (int dx = -2; dx <= 2; dx++) px(cx+dx, cy, 255,255,255);
      px(cx-1,cy+1,255,255,255); px(cx,cy+1,255,255,255); px(cx+1,cy+1,255,255,255);
      px(cx-2,cy+2,255,255,255); px(cx,cy+2,255,255,255); px(cx+2,cy+2,255,255,255);
    };
    flake( 3, 10);
    flake(11, 13);
    break;
  }

  case 5: { // ⛈️ thunderstorm — dark cloud + lightning (3px wide, Z-shape)
    drawCloud(0, 2);
    static const uint8_t TC[3][3] = {{255,255,195},{255,248,165},{255,238,125}};
    static const uint8_t BC[3][3] = {{255,205, 45},{255,178, 10},{240,148,  0}};
    // upper arm: diagonal down-left, 3px wide
    for (int i = 0; i < 3; i++)
      for (int dx = 0; dx < 3; dx++)
        px(7-i+dx, 9+i, TC[i][0], TC[i][1], TC[i][2]);
    // horizontal kink (6px)
    for (int c = 4; c <= 9; c++) px(c, 12, 255, 225, 85);
    // lower arm: diagonal down-left, 3px wide
    for (int i = 0; i < 3; i++)
      for (int dx = 0; dx < 3; dx++)
        px(6-i+dx, 13+i, BC[i][0], BC[i][1], BC[i][2]);
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

  case 7: { // 🌥️ partly cloudy night: crescent moon top-left + cloud below
    for (int dy = 0; dy < 8; dy++) {
      for (int dx = 0; dx < 8; dx++) {
        float dOut = sqrtf((dx-3.0f)*(dx-3.0f) + (dy-3.0f)*(dy-3.0f));
        float dIn  = sqrtf((dx-5.5f)*(dx-5.5f) + (dy-3.0f)*(dy-3.0f));
        if (dOut <= 3.5f && dIn > 3.0f) {
          float t = dOut / 3.5f;
          px(dx, dy,
            (uint8_t)(255 - t * 20),
            (uint8_t)(250 - t * 30),
            (uint8_t)(200 - t * 40));
        }
      }
    }
    drawCloud(4, 0);
    break;
  }

  case 8: { // 🌧️ rain — cloud + 3 diagonal \ stripes, 3px each, 2 groups
    drawCloud(0, 1);
    static const int8_t SX8[] = {12, 8, 4};
    for (int d = 0; d < 3; d++) {
      px(SX8[d]-0,  9, 90, 175, 255);
      px(SX8[d]-1, 10, 90, 175, 255);
      px(SX8[d]-2, 11, 55, 130, 225);
      px(SX8[d]-0, 12, 90, 175, 255);
      px(SX8[d]-1, 13, 90, 175, 255);
      px(SX8[d]-2, 14, 55, 130, 225);
    }
    break;
  }

  case 9: { // 🌦️ rain showers — sun top-left + cloud lower + diagonal drops
    // sun (same shape as icon 1)
    for (int dy = 0; dy < 8; dy++) {
      for (int dx = 0; dx < 8; dx++) {
        float fx = dx - 3.0f, fy = dy - 3.0f;
        float d = sqrtf(fx*fx + fy*fy);
        if      (d <= 1.2f) px(dx, dy, 255, 255, 185);
        else if (d <= 2.2f) px(dx, dy, 255, 225,  55);
        else if (d <= 3.0f) px(dx, dy, 255, 178,   5);
      }
    }
    px(3, 0, 255, 148, 0); px(0, 3, 255, 148, 0);
    px(6, 3, 255, 148, 0); px(3, 6, 255, 148, 0);
    px(1, 1, 255, 152, 0); px(5, 1, 255, 152, 0);
    px(1, 5, 255, 152, 0); px(5, 5, 255, 152, 0);
    // cloud from row 3 (covers sun lower half)
    drawCloud(3, 1);
    // diagonal drops below the cloud
    static const int8_t SX9[] = {12, 8, 4};
    for (int d = 0; d < 3; d++) {
      px(SX9[d]-0, 12, 90, 175, 255);
      px(SX9[d]-1, 13, 90, 175, 255);
      px(SX9[d]-2, 14, 55, 130, 225);
    }
    break;
  }

  case 10: { // 🌫️ Nebel — 5 horizontale Streifen, alternierend versetzt
    auto fogLine = [&](int dy, int x0, int x1, uint8_t v) {
      for (int c = x0; c <= x1; c++) px(c, dy, v, (uint8_t)(v + 8), (uint8_t)(v + 18));
    };
    fogLine( 3,  2, 11, 200);
    fogLine( 5,  4, 13, 185);
    fogLine( 7,  2, 11, 170);
    fogLine( 9,  4, 13, 155);
    fogLine(11,  2, 11, 140);
    break;
  }

  } // switch
}

void weatherIconTest() {
  // 11 Icons: Reihe 0 → Icons 0-5 (6×, Abstand 21px), Reihe 1 → Icons 6-10 (5×, Abstand 24px)
  ApplyBrightness(screensaverBrightness);
  display->ClearScreen();
  for (uint8_t i = 0; i < 6; i++) {
    DrawWeatherIcon16(i, i * 21 + 2, 0);
  }
  for (uint8_t i = 6; i < 11; i++) {
    DrawWeatherIcon16(i, (i - 6) * 24 + 4, 16);
  }
  Render();
}

void weatherSmallIconTest() {
  // 11 Icons (scale=1, 8×8px) in einer Zeile: x=0,12,24,...120 — y zentriert (12)
  ApplyBrightness(screensaverBrightness);
  display->ClearScreen();
  for (uint8_t i = 0; i < 11; i++) {
    DrawWeatherIcon(i, i * 12, 12, 1);
  }
  Render();
}

void weatherDisplay() {
  ApplyBrightness(screensaverBrightness);
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

  ApplyBrightness(screensaverBrightness);
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

  bool mqttFallback = (lastMqttWeather > 0 &&
                       millis() - lastMqttWeather > 10UL * 60UL * 1000UL);
  bool mqttStale    = (lastMqttWeather > 0 &&
                       millis() - lastMqttWeather > 30UL * 60UL * 1000UL);
  if (weatherAvailable && !mqttStale) {
    WeatherInfo wi = GetWeatherInfo(weatherCode, weatherIsDay);

    DrawWeatherIcon16(wi.icon, 55, 3);

    int tempInt = (int)roundf(weatherTemp);
    uint8_t tR, tG, tB;
    getTempColor(tempInt, tR, tG, tB);
    // gray out all values when falling back to OpenMeteo (MQTT silent >10 min)
    uint8_t wR, wG, wB;
    if (mqttFallback) { tR = tG = tB = wR = wG = wB = 100; }
    else              { wR = dateR; wG = dateG; wB = dateB; }

    // centered in zone x=72..96 (between icon end and side data)
    bool negative = (weatherTemp < -0.5f);
    int absTemp = negative ? -tempInt : tempInt;
    int w = 0;
    if (negative)      w += 6;   // Minuszeichen
    if (absTemp >= 10) w += 11;  // Zehner
    w += 10;                     // Einer
    w += 5;                      // gap + "C"
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

    display->DisplayText(wi.text, 55, 25, wR, wG, wB, 1);

    char humStr[8];
    snprintf(humStr, sizeof(humStr), "H:%u%%", weatherHumidity);
    display->DisplayText(humStr, 101, 2, wR, wG, wB, 1);
    char windStr[9];
    snprintf(windStr, sizeof(windStr), "W:%dkm", (int)roundf(weatherWindSpeed));
    display->DisplayText(windStr, 101, 11, wR, wG, wB, 1);
    char presStr[10];
    snprintf(presStr, sizeof(presStr), "P:%u", weatherPressure);
    display->DisplayText(presStr, 101, 20, wR, wG, wB, 1);
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

  ApplyBrightness(screensaverBrightness);
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
    getTempColor(tempInt, tR, tG, tB);

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

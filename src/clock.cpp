#ifdef ZEDMD_WIFI

#include "clock.h"
#include "displayDriver.h"
#include "panel.h"
#include <Arduino.h>
#include <time.h>

// ── Aus main.cpp (extern) ─────────────────────────────────────────────────────

extern DisplayDriver* display;
extern uint8_t        screensaverBrightness;
extern void Render();
extern void logMsg(const char* fmt, ...);
extern void ApplyBrightness(uint8_t base);

// ── Globals (definiert hier, extern in clock.h) ───────────────────────────────

uint8_t       clockR = 0,   clockG = 255, clockB = 200;
uint8_t       dateR  = 180, dateG  = 180, dateB  = 180;
volatile bool clockColorChanged = true;
bool          ntpSynced = false;
String        ntpServer = "pool.ntp.org";
String        clockTimezone = "CET-1CEST,M3.5.0,M10.5.0/3";

// ── NTP ───────────────────────────────────────────────────────────────────────

void clockInit() {
  configTzTime(clockTimezone.c_str(), ntpServer.c_str());
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    ntpSynced = true;
    logMsg("NTP sync OK");
  } else {
    logMsg("NTP sync FAILED");
  }
}

// ── 7-Segment Ziffer ─────────────────────────────────────────────────────────
// Jede Ziffer ist 10px breit, 20px hoch, 2px Segmentdicke
// Segmente: a=oben, b=oben-rechts, c=unten-rechts, d=unten,
//           e=unten-links, f=oben-links, g=mitte

void DrawSegDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t segs[10] = {
    0b0111111,  // 0: a,b,c,d,e,f
    0b0000110,  // 1: b,c
    0b1011011,  // 2: a,b,d,e,g
    0b1001111,  // 3: a,b,c,d,g
    0b1100110,  // 4: b,c,f,g
    0b1101101,  // 5: a,c,d,f,g
    0b1111101,  // 6: a,c,d,e,f,g
    0b0000111,  // 7: a,b,c
    0b1111111,  // 8: alle
    0b1101111,  // 9: a,b,c,d,f,g
  };
  if (digit < 0 || digit > 9) return;
  uint8_t s = segs[digit];
  int w = 10;
  int h = 20;
  int t = 2;

  auto drawH = [&](int sx, int sy, int sw) {
    for (int i = 0; i < sw; i++)
      for (int j = 0; j < t; j++)
        display->DrawPixel(sx + i, sy + j, r, g, b);
  };
  auto drawV = [&](int sx, int sy, int sh) {
    for (int i = 0; i < t; i++)
      for (int j = 0; j < sh; j++)
        display->DrawPixel(sx + i, sy + j, r, g, b);
  };

  if (s & 0b0000001) drawH(x+1, y,        w-2);
  if (s & 0b0000010) drawV(x+w-t, y+1,    h/2-1);
  if (s & 0b0000100) drawV(x+w-t, y+h/2,  h/2-1);
  if (s & 0b0001000) drawH(x+1, y+h-t,    w-2);
  if (s & 0b0010000) drawV(x,    y+h/2,   h/2-1);
  if (s & 0b0100000) drawV(x,    y+1,     h/2-1);
  if (s & 0b1000000) drawH(x+1, y+h/2-1, w-2);
}


// ── Doppelpunkt ───────────────────────────────────────────────────────────────

void DrawColon(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      display->DrawPixel(x+i, y+5+j,  r, g, b);
      display->DrawPixel(x+i, y+13+j, r, g, b);
    }
}

// ── Uhr-Anzeige ───────────────────────────────────────────────────────────────

void clockDisplay() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  static int lastClockMin  = -1;
  static int lastClockHour = -1;

  if (!clockColorChanged &&
      timeinfo.tm_min == lastClockMin && timeinfo.tm_hour == lastClockHour) return;

  clockColorChanged = false;
  lastClockMin  = timeinfo.tm_min;
  lastClockHour = timeinfo.tm_hour;

  ApplyBrightness(screensaverBrightness);
  display->ClearScreen();

  const char* days[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%s %02d.%02d.%02d",
    days[timeinfo.tm_wday],
    timeinfo.tm_mday,
    timeinfo.tm_mon + 1,
    timeinfo.tm_year % 100);

  int h1 = timeinfo.tm_hour / 10;
  int h2 = timeinfo.tm_hour % 10;
  int m1 = timeinfo.tm_min / 10;
  int m2 = timeinfo.tm_min % 10;

  int startX = 4;
  int startY = 5;

  if (h1 > 0) DrawSegDigit(startX,      startY, h1, clockR, clockG, clockB);
  DrawSegDigit(startX + 11, startY, h2, clockR, clockG, clockB);
  DrawColon(   startX + 23, startY,     clockR, clockG, clockB);
  DrawSegDigit(startX + 27, startY, m1, clockR, clockG, clockB);
  DrawSegDigit(startX + 38, startY, m2, clockR, clockG, clockB);

  int dateX = TOTAL_WIDTH - (int)(strlen(dateStr) * 4) - 1;
  display->DisplayText(dateStr, dateX, TOTAL_HEIGHT - 6, dateR, dateG, dateB, 1);

  Render();
}

#endif // ZEDMD_WIFI

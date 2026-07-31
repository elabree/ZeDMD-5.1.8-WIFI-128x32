#ifdef ZEDMD_WIFI

#include "clock.h"
#include "displayDriver.h"
#include "panel.h"
#include <Arduino.h>
#include <time.h>

// ── Externals from main.cpp ───────────────────────────────────────────────────

extern DisplayDriver* display;
extern uint8_t        screensaverBrightness;
extern void Render();
extern void logMsg(const char* fmt, ...);
extern void ApplyBrightness(uint8_t base);

// ── Globals (defined here, extern in clock.h) ────────────────────────────────

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

// ── 7-Segment Digit ──────────────────────────────────────────────────────────
// 10px wide, 20px tall, 2px segment thickness
// Bits: a=0(top) b=1(t-r) c=2(b-r) d=3(bottom) e=4(b-l) f=5(t-l) g=6(mid)

static inline bool SegXor(uint8_t d, int s1, int s2) {
  return ((d >> s1) ^ (d >> s2)) & 1;
}

void DrawSegDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  constexpr uint8_t segs[10] = {
    0b0111111,  // 0: a,b,c,d,e,f
    0b0000110,  // 1: b,c
    0b1011011,  // 2: a,b,d,e,g
    0b1001111,  // 3: a,b,c,d,g
    0b1100110,  // 4: b,c,f,g
    0b1101101,  // 5: a,c,d,f,g
    0b1111101,  // 6: a,c,d,e,f,g
    0b0000111,  // 7: a,b,c
    0b1111111,  // 8: all
    0b1101111,  // 9: a,b,c,d,f,g
  };
  if (digit < 0 || digit > 9) return;
  const uint8_t s = segs[digit];
  constexpr int w = 10, h = 20, t = 2;

  // Segments 1px shorter at each end — corners are set separately
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

  if (s & 0b0000001) drawH(x+1,   y,       w-2);   // a: top
  if (s & 0b0000010) drawV(x+w-t, y+1,     h/2-1); // b: top-right
  if (s & 0b0000100) drawV(x+w-t, y+h/2,   h/2-1); // c: bottom-right
  if (s & 0b0001000) drawH(x+1,   y+h-t,   w-2);   // d: bottom
  if (s & 0b0010000) drawV(x,     y+h/2,   h/2-1); // e: bottom-left
  if (s & 0b0100000) drawV(x,     y+1,     h/2-1); // f: top-left
  if (s & 0b1000000) drawH(x+1,   y+h/2-1, w-2);   // g: middle

  // Outer corners: set pixel when exactly one adjacent segment is active (XOR = chamfer)
  if (SegXor(s,5,0)) display->DrawPixel(x,     y,     r,g,b); // f^a: top-left
  if (SegXor(s,0,1)) display->DrawPixel(x+w-1, y,     r,g,b); // a^b: top-right
  if (SegXor(s,3,4)) display->DrawPixel(x,     y+h-1, r,g,b); // d^e: bottom-left
  if (SegXor(s,2,3)) display->DrawPixel(x+w-1, y+h-1, r,g,b); // c^d: bottom-right

  // Middle corners: clear pixel where f and g (left) or b and g (right) meet
  if ((s & 0b1100000) == 0b1100000) display->DrawPixel(x,     y+h/2-1, 0,0,0); // f+g: left
  if ((s & 0b1000010) == 0b1000010) display->DrawPixel(x+w-1, y+h/2-1, 0,0,0); // b+g: right
}


// ── Colon ─────────────────────────────────────────────────────────────────────

void DrawColon(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      display->DrawPixel(x+i, y+5+j,  r, g, b);
      display->DrawPixel(x+i, y+13+j, r, g, b);
    }
}

// ── Clock Display ─────────────────────────────────────────────────────────────

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

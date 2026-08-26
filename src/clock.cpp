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

// ── Glow / style toggles ─────────────────────────────────────────────────────
volatile bool clockGlowEnabled = false;
volatile int  clockSegStyle    = 0;       // 0=Default 1=Classic 2=Modern 3=Classic2

// ── 7-Segment Digit ──────────────────────────────────────────────────────────
// 10px wide, 20px tall, 2px segment thickness
// Bits: a=0(top) b=1(t-r) c=2(b-r) d=3(bottom) e=4(b-l) f=5(t-l) g=6(mid)

static constexpr uint8_t kSegs[10] = {
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

static inline bool SegXor(uint8_t d, int s1, int s2) {
  return ((d >> s1) ^ (d >> s2)) & 1;
}

// Chamfered style: filled outer corners (XOR), cleared middle junctions
void DrawSegDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  if (digit < 0 || digit > 9) return;
  const uint8_t s = kSegs[digit];
  constexpr int w = 10, h = 20, t = 2;

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

  if (SegXor(s,5,0)) display->DrawPixel(x,     y,     r,g,b); // f^a: top-left
  if (SegXor(s,0,1)) display->DrawPixel(x+w-1, y,     r,g,b); // a^b: top-right
  if (SegXor(s,3,4)) display->DrawPixel(x,     y+h-1, r,g,b); // d^e: bottom-left
  if (SegXor(s,2,3)) display->DrawPixel(x+w-1, y+h-1, r,g,b); // c^d: bottom-right

  if ((s & 0b1100000) == 0b1100000) display->DrawPixel(x,     y+h/2-1, 0,0,0); // f+g: left
  if ((s & 0b1000010) == 0b1000010) display->DrawPixel(x+w-1, y+h/2-1, 0,0,0); // b+g: right
}

// Classic style: Default + extra notch at the straight vertical junction
// where b+c (or e+f) meet without g — makes "1" and "7" look like two
// distinct chamfered bars.  "0" is exempt (closed frame: both a and d present).
void DrawSegDigitClassic(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  if (digit < 0 || digit > 9) return;
  const uint8_t s = kSegs[digit];
  constexpr int w = 10, h = 20, t = 2;

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

  // Same base segments as Default
  if (s & 0b0000001) drawH(x+1,   y,       w-2);
  if (s & 0b0000010) drawV(x+w-t, y+1,     h/2-1);
  if (s & 0b0000100) drawV(x+w-t, y+h/2,   h/2-1);
  if (s & 0b0001000) drawH(x+1,   y+h-t,   w-2);
  if (s & 0b0010000) drawV(x,     y+h/2,   h/2-1);
  if (s & 0b0100000) drawV(x,     y+1,     h/2-1);
  if (s & 0b1000000) drawH(x+1,   y+h/2-1, w-2);

  // Same XOR outer corners as Default
  if (SegXor(s,5,0)) display->DrawPixel(x,     y,     r,g,b);
  if (SegXor(s,0,1)) display->DrawPixel(x+w-1, y,     r,g,b);
  if (SegXor(s,3,4)) display->DrawPixel(x,     y+h-1, r,g,b);
  if (SegXor(s,2,3)) display->DrawPixel(x+w-1, y+h-1, r,g,b);

  // L-junction clearing — same as Default (outer pixel, matches b+g / f+g look)
  if ((s & 0b1100000) == 0b1100000) display->DrawPixel(x,     y+h/2-1, 0,0,0);  // f+g: x+0
  if ((s & 0b1000010) == 0b1000010) display->DrawPixel(x+w-1, y+h/2-1, 0,0,0);  // b+g: x+9

  // Inner-side chamfer at all straight vertical junctions when g absent.
  // Outer column stays solid; inner column gets a 2px notch at the midpoint —
  // matches how a real 7-seg display shows the b+c and e+f segment boundary.
  if (!(s & 0b1000000)) {
    if ((s & 0b0000110) == 0b0000110) {   // b AND c present — right side
      display->DrawPixel(x+w-t, y+h/2-1, 0,0,0);   // inner bottom of b
      display->DrawPixel(x+w-t, y+h/2,   0,0,0);   // inner top of c
    }
    if ((s & 0b0110000) == 0b0110000) {   // e AND f present — left side
      display->DrawPixel(x+t-1, y+h/2-1, 0,0,0);   // inner bottom of f
      display->DrawPixel(x+t-1, y+h/2,   0,0,0);   // inner top of e
    }
  }
}

// Classic2 style: inner-pixel L-junction clearing + inner chamfer at b+c/e+f,
// but "0" (closed frame: a+d both set) is exempt from the mid-junction chamfer.
// This is the ecb831d build — inner edge chamfered, outer edge always solid.
void DrawSegDigitClassic2(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  if (digit < 0 || digit > 9) return;
  const uint8_t s = kSegs[digit];
  constexpr int w = 10, h = 20, t = 2;

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

  if (s & 0b0000001) drawH(x+1,   y,       w-2);
  if (s & 0b0000010) drawV(x+w-t, y+1,     h/2-1);
  if (s & 0b0000100) drawV(x+w-t, y+h/2,   h/2-1);
  if (s & 0b0001000) drawH(x+1,   y+h-t,   w-2);
  if (s & 0b0010000) drawV(x,     y+h/2,   h/2-1);
  if (s & 0b0100000) drawV(x,     y+1,     h/2-1);
  if (s & 0b1000000) drawH(x+1,   y+h/2-1, w-2);

  if (SegXor(s,5,0)) display->DrawPixel(x,     y,     r,g,b);
  if (SegXor(s,0,1)) display->DrawPixel(x+w-1, y,     r,g,b);
  if (SegXor(s,3,4)) display->DrawPixel(x,     y+h-1, r,g,b);
  if (SegXor(s,2,3)) display->DrawPixel(x+w-1, y+h-1, r,g,b);

  // Inner-pixel L-junction clearing (x+1 left, x+8 right)
  if ((s & 0b1100000) == 0b1100000) display->DrawPixel(x+t-1, y+h/2-1, 0,0,0);  // f+g
  if ((s & 0b1000010) == 0b1000010) display->DrawPixel(x+w-t, y+h/2-1, 0,0,0);  // b+g

  // Inner chamfer at b+c / e+f junction when g absent — "0" exempt (a+d both set)
  if (!(s & 0b1000000) && ((s & 0b0001001) != 0b0001001)) {
    if ((s & 0b0000110) == 0b0000110) {
      display->DrawPixel(x+w-t, y+h/2-1, 0,0,0);
      display->DrawPixel(x+w-t, y+h/2,   0,0,0);
    }
    if ((s & 0b0110000) == 0b0110000) {
      display->DrawPixel(x+t-1, y+h/2-1, 0,0,0);
      display->DrawPixel(x+t-1, y+h/2,   0,0,0);
    }
  }
}

// Modern style: plain segments, no chamfering — original v1.1 look
void DrawSegDigitModern(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  if (digit < 0 || digit > 9) return;
  const uint8_t s = kSegs[digit];
  constexpr int w = 10, h = 20, t = 2;

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

  if (s & 0b0000001) drawH(x+1,   y,       w-2);
  if (s & 0b0000010) drawV(x+w-t, y+1,     h/2-1);
  if (s & 0b0000100) drawV(x+w-t, y+h/2,   h/2-1);
  if (s & 0b0001000) drawH(x+1,   y+h-t,   w-2);
  if (s & 0b0010000) drawV(x,     y+h/2,   h/2-1);
  if (s & 0b0100000) drawV(x,     y+1,     h/2-1);
  if (s & 0b1000000) drawH(x+1,   y+h/2-1, w-2);
}

// Dispatcher — 0=Default, 1=Classic, 2=Modern, 3=Classic2
void DrawDigit(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  switch (clockSegStyle) {
    case 1:  DrawSegDigitClassic(x, y, digit, r, g, b);  break;
    case 2:  DrawSegDigitModern(x, y, digit, r, g, b);   break;
    case 3:  DrawSegDigitClassic2(x, y, digit, r, g, b); break;
    default: DrawSegDigit(x, y, digit, r, g, b);         break;
  }
}


// ── Shadow variants ───────────────────────────────────────────────────────────
// Draw the digit/colon offset by (SHADOW_DX, SHADOW_DY) at reduced brightness
// first, then the original on top — keeps edges crisp while adding depth.

static constexpr int   SHADOW_DX = 2;
static constexpr int   SHADOW_DY = 2;
static constexpr float SHADOW_DIM = 0.30f;  // 30% of main color

void DrawSegDigitShadow(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  DrawDigit(x + SHADOW_DX, y + SHADOW_DY, digit,
            (uint8_t)(r * SHADOW_DIM), (uint8_t)(g * SHADOW_DIM), (uint8_t)(b * SHADOW_DIM));
  DrawDigit(x, y, digit, r, g, b);
}

void DrawColonShadow(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  DrawColon(x + SHADOW_DX, y + SHADOW_DY,
            (uint8_t)(r * SHADOW_DIM), (uint8_t)(g * SHADOW_DIM), (uint8_t)(b * SHADOW_DIM));
  DrawColon(x, y, r, g, b);
}

// ── Glow-aware wrappers — use these everywhere instead of raw DrawDigit/DrawColon ──
// Checks clockGlowEnabled internally so callers don't need to duplicate the if/else.

void DrawDigitAuto(int x, int y, int digit, uint8_t r, uint8_t g, uint8_t b) {
  if (clockGlowEnabled) DrawSegDigitShadow(x, y, digit, r, g, b);
  else                  DrawDigit(x, y, digit, r, g, b);
}

void DrawColonAuto(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (clockGlowEnabled) DrawColonShadow(x, y, r, g, b);
  else                  DrawColon(x, y, r, g, b);
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
  int startY = clockGlowEnabled ? 4 : 5;  // shift up 1px to keep shadow clear of date

  if (h1 > 0) DrawDigitAuto(startX,      startY, h1, clockR, clockG, clockB);
  DrawDigitAuto(startX + 11, startY, h2, clockR, clockG, clockB);
  DrawColonAuto(startX + 23, startY,     clockR, clockG, clockB);
  DrawDigitAuto(startX + 27, startY, m1, clockR, clockG, clockB);
  DrawDigitAuto(startX + 38, startY, m2, clockR, clockG, clockB);

  int dateX = TOTAL_WIDTH - (int)(strlen(dateStr) * 4) - 1;
  display->DisplayText(dateStr, dateX, TOTAL_HEIGHT - 6, dateR, dateG, dateB, 1);

  Render();
}

#endif // ZEDMD_WIFI

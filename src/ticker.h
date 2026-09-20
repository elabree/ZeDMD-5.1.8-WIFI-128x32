#pragma once
#ifdef ZEDMD_WIFI

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#define MAX_TICKER_SYMBOLS  10
#define TICKER_HISTORY_LEN  40   // hourly data points for the sparkline

struct TickerEntry {
  char  symbol[16];
  char  resolved[16];                 // cached Yahoo symbol after WKN/ISIN resolution, empty if not yet resolved
  char  shortName[32];                // human-readable name, e.g. "Commerzbank AG"
  char  currency[8];                  // "USD", "EUR", "GBP", ...
  float price;
  float changePct;
  bool  valid;
  float history[TICKER_HISTORY_LEN];  // close prices, oldest first
  int   historyLen;                   // actual number of points (<= TICKER_HISTORY_LEN)
};

extern TickerEntry tickerData[MAX_TICKER_SYMBOLS];
extern int         tickerCount;
extern char        tickerSymbolsStr[160];
extern uint32_t    tickerIntervalSec;
extern char        tickerRange[4];      // "1d" or "5d"
extern volatile bool tickerFetching;
extern uint32_t    lastTickerFetch;
extern bool        tickerEnabled;

// carousel display state (read in main.cpp loop)
extern int      tickerCurrentIndex;
extern uint32_t tickerPhaseStart;

void tickerInit();
void tickerTrigger();
void tickerRegisterRoutes(AsyncWebServer* server);

#endif // ZEDMD_WIFI

#ifdef ZEDMD_WIFI

#include "ticker.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

extern void logMsg(const char* fmt, ...);

// ── storage ──────────────────────────────────────────────────────────────────

TickerEntry  tickerData[MAX_TICKER_SYMBOLS];
int          tickerCount         = 0;
char         tickerSymbolsStr[160] = "";
uint32_t     tickerIntervalSec   = 900;
char         tickerRange[4]      = "5d";
volatile bool tickerFetching     = false;
uint32_t     lastTickerFetch     = 0;
bool         tickerEnabled       = true;

int      tickerCurrentIndex = 0;
uint32_t tickerPhaseStart   = 0;

static SemaphoreHandle_t tickerMutex  = nullptr;
static TaskHandle_t      tickerHandle = nullptr;
static StackType_t*      tickerStack  = nullptr;
static StaticTask_t      tickerTaskBuf;

static const size_t TICKER_BUF = 16 * 1024;

// ── JSON field extraction ─────────────────────────────────────────────────────

// Reads a JSON string value after "\"key\":\"". dst is NUL-terminated on return.
static void extractString(const char* src, const char* key, char* dst, size_t dstLen) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char* p = strstr(src, search);
  if (!p) { dst[0] = '\0'; return; }
  p += strlen(search);
  const char* e = strchr(p, '"');
  if (!e) { dst[0] = '\0'; return; }
  size_t len = (size_t)(e - p);
  if (len >= dstLen) len = dstLen - 1;
  strncpy(dst, p, len);
  dst[len] = '\0';
  // trim trailing whitespace
  for (int i = (int)len - 1; i >= 0 && (dst[i] == ' ' || dst[i] == '\t'); i--)
    dst[i] = '\0';
}

// Reads the numeric value after "\"key\":". Returns true on success.
static bool extractFloat(const char* src, const char* key, float* out) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char* p = strstr(src, search);
  if (!p) return false;
  p += strlen(search);
  while (*p == ' ') p++;
  *out = strtof(p, nullptr);
  return true;
}

// ── fetch task ────────────────────────────────────────────────────────────────

// Returns true if sym is a WKN (6 alphanumeric) or ISIN (2 letters + 10 alphanumeric).
static bool isWknOrIsin(const char* sym) {
  size_t len = strlen(sym);
  if (len == 6) {
    for (size_t i = 0; i < 6; i++) if (!isalnum((unsigned char)sym[i])) return false;
    return true;
  }
  if (len == 12) {
    if (!isupper((unsigned char)sym[0]) || !isupper((unsigned char)sym[1])) return false;
    for (size_t i = 2; i < 12; i++) if (!isalnum((unsigned char)sym[i])) return false;
    return true;
  }
  return false;
}

// Resolves a WKN/ISIN to a Yahoo Finance ticker symbol via search API.
static bool resolveToYahooSymbol(const char* query, char* buf, size_t bufLen,
                                  char* out, size_t outLen,
                                  WiFiClientSecure& secure, HTTPClient& http) {
  char url[192];
  snprintf(url, sizeof(url),
           "https://query1.finance.yahoo.com/v1/finance/search?q=%s&quotesCount=1&newsCount=0",
           query);
  http.begin(secure, url);
  http.setTimeout(8000);
  http.setUserAgent("ZeDMD/1.8");
  bool ok = false;
  if (http.GET() == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    int len = 0;
    uint32_t lastData = millis();
    while (len < (int)bufLen - 1 && millis() - lastData < 2000) {
      int avail = stream->available();
      if (avail > 0) {
        int chunk = stream->readBytes(buf + len, min(avail, (int)bufLen - 1 - len));
        if (chunk > 0) { len += chunk; lastData = millis(); }
      } else if (!stream->connected()) break;
      else vTaskDelay(pdMS_TO_TICKS(10));
    }
    buf[len] = '\0';
    // Extract first "symbol":"..." from the quotes array
    const char* p = strstr(buf, "\"symbol\":\"");
    if (p) {
      p += 10;
      const char* e = strchr(p, '"');
      if (e && (size_t)(e - p) < outLen) {
        strncpy(out, p, e - p);
        out[e - p] = '\0';
        ok = (out[0] != '\0');
      }
    }
  }
  http.end();
  return ok;
}

// Fetches price, changePct and history for one symbol; resolves WKN/ISIN on first call.
static bool fetchSymbol(TickerEntry* entry, char* body,
                        WiFiClientSecure& secure, HTTPClient& http) {
  const char* querySymbol = entry->symbol;
  if (isWknOrIsin(entry->symbol)) {
    if (entry->resolved[0] == '\0') {
      // first time: resolve and cache
      if (resolveToYahooSymbol(entry->symbol, body, TICKER_BUF,
                               entry->resolved, sizeof(entry->resolved),
                               secure, http)) {
        logMsg("Ticker: resolved %s -> %s", entry->symbol, entry->resolved);
      } else {
        logMsg("Ticker: could not resolve %s, skipping", entry->symbol);
        return false;
      }
    }
    querySymbol = entry->resolved;
  }

  char url[256];
  snprintf(url, sizeof(url),
           "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1h&range=%s",
           querySymbol, tickerRange);

  http.begin(secure, url);
  http.setTimeout(10000);
  http.setUserAgent("ZeDMD/1.8");

  bool ok = false;
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    int contentLen = http.getSize();  // -1 if chunked/unknown
    int len = 0;
    uint32_t deadline = millis() + 8000;
    uint32_t lastData = millis();
    while (len < (int)TICKER_BUF - 1 && millis() < deadline) {
      int avail = stream->available();
      if (avail > 0) {
        int chunk = stream->readBytes(body + len, min(avail, (int)TICKER_BUF - 1 - len));
        if (chunk > 0) { len += chunk; lastData = millis(); }
        // stop early when Content-Length is known
        if (contentLen > 0 && len >= contentLen) break;
      } else if (!stream->connected()) {
        break;
      } else if (millis() - lastData > 2000) {
        // no new data for 2s — response complete (keepalive doesn't close stream)
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    body[len] = '\0';

    extractString(body, "shortName", entry->shortName, sizeof(entry->shortName));
    extractString(body, "currency", entry->currency, sizeof(entry->currency));

    float price = 0.0f, chg = 0.0f;
    if (extractFloat(body, "regularMarketPrice", &price) &&
        extractFloat(body, "regularMarketChangePercent", &chg)) {
      entry->price     = price;
      entry->changePct = chg;
      entry->valid     = true;
      ok = true;
    }

    entry->historyLen = 0;
    const char* p = strstr(body, "\"close\":[");
    if (p) {
      p += 9;
      while (*p && *p != ']' && entry->historyLen < TICKER_HISTORY_LEN) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        if (strncmp(p, "null", 4) == 0) { p += 4; continue; }
        char* end;
        float v = strtof(p, &end);
        if (end == p) break;
        if (v > 0.0f) entry->history[entry->historyLen++] = v;
        p = end;
      }
    }
    logMsg("Ticker: %s price=%.2f chg=%.2f%% hist=%d",
           entry->symbol, entry->price, entry->changePct, entry->historyLen);
  } else {
    logMsg("Ticker: %s HTTP %d", entry->symbol, httpCode);
  }
  http.end();
  return ok;
}

static void tickerFetchTask(void* /*pvParams*/) {
  // Self-suspend first to avoid racing xTaskCreateStatic on the other core.
  vTaskSuspend(nullptr);

  while (true) {
    if (tickerSymbolsStr[0] == '\0') { tickerFetching = false; vTaskSuspend(nullptr); continue; }

    logMsg("Ticker: fetch start, symbols=%s", tickerSymbolsStr);

    char* body = (char*)heap_caps_malloc(TICKER_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { tickerFetching = false; vTaskSuspend(nullptr); continue; }

    TickerEntry tmp[MAX_TICKER_SYMBOLS];
    int n = 0;

    char symBuf[sizeof(tickerSymbolsStr)];
    strncpy(symBuf, tickerSymbolsStr, sizeof(symBuf) - 1);
    symBuf[sizeof(symBuf) - 1] = '\0';

    // One shared TLS connection for all symbol fetches — http.end() between symbols
    // keeps the TCP connection alive (keepalive), next http.begin() reuses it.
    WiFiClientSecure secure;
    secure.setInsecure();
    HTTPClient http;
    http.setReuse(true);

    char* tok = strtok(symBuf, ",");
    while (tok && n < MAX_TICKER_SYMBOLS) {
      while (*tok == ' ') tok++;
      if (*tok) {
        memset(&tmp[n], 0, sizeof(TickerEntry));
        strncpy(tmp[n].symbol, tok, sizeof(tmp[n].symbol) - 1);
        if (fetchSymbol(&tmp[n], body, secure, http)) n++;
      }
      tok = strtok(nullptr, ",");
    }

    http.end();  // close the shared connection after all fetches
    logMsg("Ticker: fetch done, %d valid", n);

    if (n > 0) {
      if (xSemaphoreTake(tickerMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        memcpy(tickerData, tmp, sizeof(TickerEntry) * n);
        tickerCount        = n;
        tickerCurrentIndex = 0;
        tickerPhaseStart   = 1;  // sentinel: first draw pending, don't increment index
        xSemaphoreGive(tickerMutex);
      }
      lastTickerFetch = millis();
    }

    heap_caps_free(body);
    tickerFetching = false;
    vTaskSuspend(nullptr);
  }
}

// ── public API ────────────────────────────────────────────────────────────────

void tickerInit() {
  tickerMutex = xSemaphoreCreateMutex();
  memset(tickerData, 0, sizeof(tickerData));

  File f = LittleFS.open("/ticker_symbols.val", "r");
  if (f) {
    size_t n = f.readBytes(tickerSymbolsStr, sizeof(tickerSymbolsStr) - 1);
    tickerSymbolsStr[n] = '\0';
    for (int i = (int)n - 1;
         i >= 0 && (tickerSymbolsStr[i] == '\n' || tickerSymbolsStr[i] == '\r' ||
                    tickerSymbolsStr[i] == ' ');
         i--)
      tickerSymbolsStr[i] = '\0';
    f.close();
  }

  File fi = LittleFS.open("/ticker_interval.val", "r");
  if (fi) {
    tickerIntervalSec = (uint32_t)fi.readStringUntil('\n').toInt();
    if (tickerIntervalSec < 60) tickerIntervalSec = 900;
    fi.close();
  }

  File fr = LittleFS.open("/ticker_range.val", "r");
  if (fr) {
    size_t n = fr.readBytes(tickerRange, sizeof(tickerRange) - 1);
    tickerRange[n] = '\0';
    fr.close();
  }
  if (tickerRange[0] != '1' && tickerRange[0] != '5') strncpy(tickerRange, "5d", sizeof(tickerRange));

  File fe = LittleFS.open("/ticker_enabled.val", "r");
  if (fe) {
    int val = fe.read();
    if (val >= 0) tickerEnabled = (val != 0);
    fe.close();
  }

  tickerStack = (StackType_t*)heap_caps_malloc(20480, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  tickerHandle = xTaskCreateStatic(tickerFetchTask, "tkrFetch",
                                   20480 / sizeof(StackType_t), nullptr,
                                   1, tickerStack, &tickerTaskBuf);
  vTaskSuspend(tickerHandle);
}

void tickerTrigger() {
  if (!tickerEnabled) return;
  if (tickerFetching || !tickerHandle) return;
  if (WiFi.status() != WL_CONNECTED) return;  // don't fetch before WiFi is ready
  tickerFetching = true;
  vTaskResume(tickerHandle);
}

void tickerRegisterRoutes(AsyncWebServer* server) {
  // POST /ticker_symbols  — comma-separated symbols
  server->on("/ticker_symbols", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("symbols", true)) { request->send(400, "text/plain", "Missing symbols"); return; }
    String syms = request->getParam("symbols", true)->value();
    syms.trim();
    // uppercase the symbols string
    syms.toUpperCase();
    strncpy(tickerSymbolsStr, syms.c_str(), sizeof(tickerSymbolsStr) - 1);
    tickerSymbolsStr[sizeof(tickerSymbolsStr) - 1] = '\0';

    File f = LittleFS.open("/ticker_symbols.val", "w");
    if (f) { f.print(syms); f.close(); }

    // clear resolve cache so changed WKN/ISIN symbols are re-resolved
    for (int i = 0; i < MAX_TICKER_SYMBOLS; i++) tickerData[i].resolved[0] = '\0';
    lastTickerFetch  = 0;
    tickerPhaseStart = 0;
    tickerTrigger();
    request->send(200, "text/plain", "OK");
  });

  // POST /ticker_interval  — refresh interval in seconds (60–3600)
  server->on("/ticker_interval", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("interval", true)) { request->send(400, "text/plain", "Missing interval"); return; }
    uint32_t iv = (uint32_t)request->getParam("interval", true)->value().toInt();
    if (iv < 60) iv = 900;
    if (iv > 3600) iv = 3600;
    tickerIntervalSec = iv;

    File f = LittleFS.open("/ticker_interval.val", "w");
    if (f) { f.print(iv); f.close(); }

    request->send(200, "text/plain", "OK");
  });

  // POST /ticker_range  — "1d" or "5d"
  server->on("/ticker_range", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("range", true)) { request->send(400, "text/plain", "Missing range"); return; }
    String r = request->getParam("range", true)->value();
    r.trim();
    if (r != "1d" && r != "5d") { request->send(400, "text/plain", "range must be 1d or 5d"); return; }
    strncpy(tickerRange, r.c_str(), sizeof(tickerRange) - 1);
    tickerRange[sizeof(tickerRange) - 1] = '\0';
    File f = LittleFS.open("/ticker_range.val", "w");
    if (f) { f.print(r); f.close(); }
    lastTickerFetch = 0;
    tickerTrigger();
    request->send(200, "text/plain", "OK");
  });

  // POST /ticker_enabled  — enable or disable ticker display (enabled=1|0)
  server->on("/ticker_enabled", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("enabled", true)) { request->send(400, "text/plain", "Missing enabled"); return; }
    tickerEnabled = request->getParam("enabled", true)->value().toInt() != 0;
    File f = LittleFS.open("/ticker_enabled.val", "w");
    if (f) { f.write(tickerEnabled ? 1 : 0); f.close(); }
    request->send(200, "text/plain", "OK");
  });

  // GET /ticker_status
  server->on("/ticker_status", HTTP_GET, [](AsyncWebServerRequest* request) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"symbols\":\"%s\",\"count\":%d,\"intervalSec\":%lu,"
             "\"range\":\"%s\",\"lastFetch\":%lu,\"fetching\":%s,\"enabled\":%s}",
             tickerSymbolsStr, tickerCount,
             (unsigned long)tickerIntervalSec, tickerRange,
             (unsigned long)lastTickerFetch,
             tickerFetching ? "true" : "false",
             tickerEnabled   ? "true" : "false");
    request->send(200, "application/json", buf);
  });
}

#endif // ZEDMD_WIFI

#ifdef ZEDMD_WIFI

#include "rss.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

extern void logMsg(const char* fmt, ...);

// ── storage ──────────────────────────────────────────────────────────────────

char            rssUrl[256]     = "";
volatile bool   rssFetching     = false;
uint32_t        lastRssFetch    = 0;

static char*            rssHeadlines    = nullptr;  // PSRAM buffer
static SemaphoreHandle_t rssMutex       = nullptr;
static TaskHandle_t      rssFetchHandle = nullptr;
static StackType_t*      rssStack       = nullptr;
static StaticTask_t      rssTaskBuf;

static const size_t RSS_BUF    = 24 * 1024;  // HTTP response
static const size_t RSS_OUT    = 4  * 1024;  // assembled headline string
static const int    RSS_MAX_ITEMS = 10;

// ── helpers ───────────────────────────────────────────────────────────────────

static void extractTag(const char* src, const char* open, const char* close,
                        char* dst, size_t dstLen) {
  const char* s = strstr(src, open);
  if (!s) { dst[0] = '\0'; return; }
  s += strlen(open);
  // handle CDATA — closing delimiter shifts from </tag> to ]]>
  bool isCdata = (strncmp(s, "<![CDATA[", 9) == 0);
  if (isCdata) s += 9;
  const char* e = isCdata ? strstr(s, "]]>") : strstr(s, close);
  if (!e) { dst[0] = '\0'; return; }
  // trim leading whitespace
  while (s < e && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
  size_t len = (size_t)(e - s);
  if (len >= dstLen) len = dstLen - 1;
  strncpy(dst, s, len);
  dst[len] = '\0';
}

// ── fetch task ────────────────────────────────────────────────────────────────

static void rssFetchTask(void* /*pvParams*/) {
  vTaskSuspend(nullptr);  // self-suspend first — avoid race with xTaskCreateStatic on other core

  while (true) {
    if (rssUrl[0] == '\0' || WiFi.status() != WL_CONNECTED) {
      rssFetching = false;
      vTaskSuspend(nullptr);
      continue;
    }

    char* body = (char*)heap_caps_malloc(RSS_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { rssFetching = false; vTaskSuspend(nullptr); continue; }

    WiFiClient       plain;
    WiFiClientSecure secure;
    HTTPClient       http;

    if (strncmp(rssUrl, "https://", 8) == 0) {
      secure.setInsecure();
      http.begin(secure, rssUrl);
    } else {
      http.begin(plain, rssUrl);
    }
    http.setTimeout(10000);
    http.setUserAgent("ZeDMD/1.8");
    int httpCode = http.GET();

    int len = 0;
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient* stream = http.getStreamPtr();
      int contentLen = http.getSize();
      uint32_t lastData = millis();
      while (len < (int)RSS_BUF - 1) {
        int avail = stream->available();
        if (avail > 0) {
          int chunk = stream->readBytes(body + len, min(avail, (int)RSS_BUF - 1 - len));
          if (chunk > 0) { len += chunk; lastData = millis(); }
          if (contentLen > 0 && len >= contentLen) break;
        } else if (!stream->connected()) {
          break;
        } else if (millis() - lastData > 2000) {
          break;
        } else {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }
      body[len] = '\0';
      logMsg("RSS: fetch OK, %d bytes", len);
    } else {
      logMsg("RSS: fetch HTTP %d", httpCode);
    }
    http.end();

    if (body[0]) {
      char title[256];
      char assembled[RSS_OUT];
      assembled[0] = '\0';
      int  count   = 0;
      const char* pos = body;

      // skip the feed-level <title> by starting after first <item>
      const char* item = strstr(pos, "<item>");
      if (!item) item = pos;  // fallback: no <item> tags, parse from start

      while (count < RSS_MAX_ITEMS && item) {
        const char* nextItem = strstr(item + 1, "<item>");
        // search for <title> within this item block
        const char* searchEnd = nextItem ? nextItem : (body + strlen(body));
        // make a temporary null-terminated excerpt
        char excerpt[1024];
        size_t excerptLen = (size_t)(searchEnd - item);
        if (excerptLen >= sizeof(excerpt)) excerptLen = sizeof(excerpt) - 1;
        strncpy(excerpt, item, excerptLen);
        excerpt[excerptLen] = '\0';

        extractTag(excerpt, "<title>", "</title>", title, sizeof(title));

        if (title[0]) {
          if (count > 0) strncat(assembled, " \xB7 ", sizeof(assembled) - strlen(assembled) - 1);
          strncat(assembled, title, sizeof(assembled) - strlen(assembled) - 1);
          count++;
        }
        item = nextItem;
      }

      logMsg("RSS: %d headlines assembled", count);
      if (xSemaphoreTake(rssMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        strncpy(rssHeadlines, assembled, RSS_OUT - 1);
        rssHeadlines[RSS_OUT - 1] = '\0';
        xSemaphoreGive(rssMutex);
      }
      lastRssFetch = millis();
    }

    heap_caps_free(body);
    rssFetching = false;
    vTaskSuspend(nullptr);
  }
}

// ── public API ────────────────────────────────────────────────────────────────

void rssInit() {
  rssMutex = xSemaphoreCreateMutex();
  rssHeadlines = (char*)heap_caps_calloc(RSS_OUT, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  File f = LittleFS.open("/rss_url.val", "r");
  if (f) {
    size_t n = f.readBytes(rssUrl, sizeof(rssUrl) - 1);
    rssUrl[n] = '\0';
    // strip trailing whitespace/newline
    for (int i = (int)n - 1; i >= 0 && (rssUrl[i] == '\n' || rssUrl[i] == '\r' || rssUrl[i] == ' '); i--)
      rssUrl[i] = '\0';
    f.close();
  }

  rssStack = (StackType_t*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rssFetchHandle = xTaskCreateStatic(rssFetchTask, "rssFetch",
                                     16384 / sizeof(StackType_t), nullptr,
                                     1, rssStack, &rssTaskBuf);
  // task self-suspends as its first instruction; this call is a safe redundant guard
  vTaskSuspend(rssFetchHandle);
}

void rssTrigger() {
  if (rssFetching || !rssFetchHandle) return;
  rssFetching = true;
  vTaskResume(rssFetchHandle);
}

const char* rssGetHeadlines() {
  return rssHeadlines ? rssHeadlines : "";
}

void rssRegisterRoutes(AsyncWebServer* server) {
  // POST /rss_url  — set feed URL and trigger immediate fetch
  server->on("/rss_url", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("url", true)) { request->send(400, "text/plain", "Missing url"); return; }
    String url = request->getParam("url", true)->value();
    url.trim();
    strncpy(rssUrl, url.c_str(), sizeof(rssUrl) - 1);
    rssUrl[sizeof(rssUrl) - 1] = '\0';

    File f = LittleFS.open("/rss_url.val", "w");
    if (f) { f.print(url); f.close(); }

    lastRssFetch = 0;  // force refresh
    rssTrigger();
    request->send(200, "text/plain", "OK");
  });

  // GET /rss_status
  server->on("/rss_status", HTTP_GET, [](AsyncWebServerRequest* request) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"url\":\"%s\",\"lastFetch\":%lu,\"fetching\":%s}",
             rssUrl, (unsigned long)lastRssFetch, rssFetching ? "true" : "false");
    request->send(200, "application/json", buf);
  });
}

#endif // ZEDMD_WIFI

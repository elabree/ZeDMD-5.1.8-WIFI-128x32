#if defined(WEBRADIO_ENABLED)

#include "radio.h"
#include <Audio.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include "sd_interface.h"

extern void radioIconSlugsLoad();

static constexpr uint32_t RADIO_DISP_TRACK_MS   = 15000;
static constexpr uint32_t RADIO_SWITCH_GRACE_MS  =  6000;
static constexpr uint32_t RADIO_DISP_CONNECT_MS  = 30000;
static constexpr uint32_t RADIO_DISP_PLAY_MS     = 20000;
static constexpr uint32_t RADIO_DISP_MANUAL_MS   = 10000;

static Audio audio;

volatile bool    radioIsPlaying     = false;
volatile bool    radioUserActive    = false;   // true as long as user wanted radio (even after EOF)
volatile bool    radioDisplayActive = false;
uint32_t         radioDisplayUntil  = 0;   // millis() timestamp; 0 = no auto-off
char             radioStationName[64]  = "";
char             radioTrackTitle[128]  = "";
SemaphoreHandle_t radioStringMutex    = nullptr;
uint8_t       radioVolume           = RADIO_DEFAULT_VOLUME;
static int8_t radioEqBass           = 0;
static int8_t radioEqMid            = 0;
static int8_t radioEqTreble         = 0;
static bool   radioMono             = false;
static bool   radioSwapChannels     = false;
RadioPreset   radioPresets[MAX_RADIO_PRESETS];
int           radioPresetCount   = 0;
volatile int  radioCurrentPreset = -1;
int           radioLastPreset    = -1;

static char          pendingUrl[256]     = "";
static volatile bool connectPending      = false;
static volatile bool stopPending         = false;
static volatile bool switchingStation    = false;
static char          gifAudioPath[256]   = "";
static volatile bool gifAudioPending     = false;
static volatile bool gifAudioStopPending = false;
volatile bool        gifAudioPlaying     = false;
// Grace timer: ignore EOF callbacks during connection setup
// (playlist resolution and redirect fire EOF before the stream starts)
static volatile uint32_t switchGraceUntil = 0;

extern void logMsg(const char* fmt, ...);

// ── Callbacks from ESP32-audioI2S ─────────────────────────────────────────────

void audio_info(const char* info) {
  if (!info) return;
  logMsg("[audio] %s", info);
  // audio_showstationname() does not fire for all streams — read ICY name directly from audio_info
  if (strncmp(info, "icy-name: ", 10) == 0 && info[10] && radioStringMutex) {
    if (xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      strlcpy(radioStationName, info + 10, sizeof(radioStationName));
      xSemaphoreGive(radioStringMutex);
    }
  }
}

void audio_showstationname(const char* info) {
  if (!info || !radioStringMutex) return;
  if (xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    strlcpy(radioStationName, info, sizeof(radioStationName));
    xSemaphoreGive(radioStringMutex);
  }
}

void audio_showstreamtitle(const char* info) {
  if (!info || !radioStringMutex) return;
  if (xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    strlcpy(radioTrackTitle, info, sizeof(radioTrackTitle));
    xSemaphoreGive(radioStringMutex);
  }
  if (radioIsPlaying) {
    radioDisplayActive = true;
    radioDisplayUntil  = millis() + RADIO_DISP_TRACK_MS;
  }
}

void audio_eof_stream(const char* info) {
  logMsg("[radio] eof_stream: \"%s\" grace=%ld", info ? info : "",
         (long)((int32_t)(switchGraceUntil - millis())));
  if (gifAudioPlaying) {
    gifAudioPlaying = false;
  } else if ((int32_t)(switchGraceUntil - millis()) <= 0) {
    radioIsPlaying = false;
    logMsg("[radio] eof_stream: radioIsPlaying -> false");
  } else {
    logMsg("[radio] eof_stream: ignoriert (Grace aktiv)");
  }
}

// ── Last-preset persistence (before public API — used by radioInit/radioPlay) ─────

static void saveLastPreset() {
  File f = LittleFS.open("/radio_last.val", "w");
  if (f) { f.println(radioLastPreset); f.close(); }
}

static void loadLastPreset() {
  File f = LittleFS.open("/radio_last.val", "r");
  if (f) { radioLastPreset = f.readStringUntil('\n').toInt(); f.close(); }
}

static void saveRadioVolume() {
  File f = LittleFS.open("/radio_volume.val", "w");
  if (f) { f.println((int)radioVolume); f.close(); }
}

static void loadRadioVolume() {
  File f = LittleFS.open("/radio_volume.val", "r");
  if (f) {
    int v = f.readStringUntil('\n').toInt();
    f.close();
    radioVolume = (uint8_t)constrain(v, 0, 21);
  }
}

static void saveRadioEq() {
  File f = LittleFS.open("/radio_eq.val", "w");
  if (f) {
    f.println((int)radioEqBass);
    f.println((int)radioEqMid);
    f.println((int)radioEqTreble);
    f.close();
  }
}

static void loadRadioEq() {
  File f = LittleFS.open("/radio_eq.val", "r");
  if (f) {
    radioEqBass   = (int8_t)constrain(f.readStringUntil('\n').toInt(), -40, 12);
    radioEqMid    = (int8_t)constrain(f.readStringUntil('\n').toInt(), -40, 12);
    radioEqTreble = (int8_t)constrain(f.readStringUntil('\n').toInt(), -40, 12);
    f.close();
  }
}

static void saveRadioMono() {
  File f = LittleFS.open("/radio_mono.val", "w");
  if (f) { f.println(radioMono ? 1 : 0); f.close(); }
}

static void loadRadioMono() {
  File f = LittleFS.open("/radio_mono.val", "r");
  if (f) { radioMono = f.readStringUntil('\n').toInt() != 0; f.close(); }
}

static void saveRadioSwap() {
  File f = LittleFS.open("/radio_swap.val", "w");
  if (f) { f.println(radioSwapChannels ? 1 : 0); f.close(); }
}

static void loadRadioSwap() {
  File f = LittleFS.open("/radio_swap.val", "r");
  if (f) { radioSwapChannels = f.readStringUntil('\n').toInt() != 0; f.close(); }
}

// ── HTTP redirect resolution ──────────────────────────────────────────────────
// Follows exactly one HTTP 3xx redirect and returns the Location URL.
// HTTPS redirects are ignored (TLS OOM on ESP32 with running decoder).
// For HTTP URLs only — HTTPS inputs are not handled.

static size_t readHttpLine(WiFiClient& wc, char* buf, size_t maxLen, uint32_t deadline) {
  size_t n = 0;
  while (millis() < deadline && n < maxLen - 1) {
    if (!wc.available()) {
      if (!wc.connected()) break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    char c = (char)wc.read();
    if (c == '\n') break;
    if (c != '\r') buf[n++] = c;
  }
  buf[n] = '\0';
  return n;
}

static bool resolveOneRedirect(const char* url, char* out, size_t outLen) {
  if (strncmp(url, "http://", 7) != 0) return false;

  // Parse host[:port] and path
  const char* hostStart = url + 7;
  const char* slash = strchr(hostStart, '/');
  if (!slash) return false;

  char host[128] = {};
  size_t hlen = (size_t)(slash - hostStart);
  if (hlen >= sizeof(host)) return false;
  memcpy(host, hostStart, hlen);

  int port = 80;
  char* colon = strchr(host, ':');
  if (colon) { port = atoi(colon + 1); *colon = '\0'; }

  WiFiClient wc;
  wc.setTimeout(3000);
  if (!wc.connect(host, port)) return false;

  wc.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\nConnection: close\r\n\r\n",
            slash, host);

  char buf[512];
  uint32_t deadline = millis() + 3000;

  // Read status line: "HTTP/1.x NNN ..."
  readHttpLine(wc, buf, sizeof(buf), deadline);
  int code = 0;
  if (strncmp(buf, "HTTP/", 5) == 0) code = atoi(buf + 9);

  bool found = false;
  if (code == 301 || code == 302 || code == 307 || code == 308) {
    while (millis() < deadline) {
      size_t n = readHttpLine(wc, buf, sizeof(buf), deadline);
      if (n == 0) break; // empty line = end of headers
      if (strncasecmp(buf, "Location:", 9) == 0) {
        const char* loc = buf + 9;
        while (*loc == ' ') loc++;
        if (strncmp(loc, "https://", 8) == 0) break; // ignore HTTPS redirect
        strlcpy(out, loc, outLen);
        found = true;
        break;
      }
    }
  }

  wc.stop();
  return found;
}

// ── FreeRTOS task (Core 0) ────────────────────────────────────────────────────

static void radioTask(void* params) {
  for (;;) {
    if (stopPending) {
      stopPending      = false;
      radioIsPlaying   = false;
      radioUserActive  = false;
      gifAudioPlaying  = false;
      switchGraceUntil = 0;
      audio.setVolume(radioVolume);
      audio.stopSong();
    } else if (connectPending) {
      char localUrl[256];
      strlcpy(localUrl, pendingUrl, sizeof(localUrl));
      logMsg("[radio] task: connect -> \"%s\" preset=%d", localUrl, (int)radioCurrentPreset);
      switchingStation = true;
      connectPending   = false;
      radioIsPlaying   = false;
      gifAudioPlaying  = false;
      audio.setVolume(0);       // Mute: suppress PSRAM residual data from the previous station
      audio.stopSong();
      vTaskDelay(pdMS_TO_TICKS(500));
      switchGraceUntil = millis() + RADIO_SWITCH_GRACE_MS;
      // Pre-resolve redirect: some CDN servers (e.g. NDR icecast → rndfnk.com)
      // respond with HTTP 302 which the audio library does not reliably follow.
      char redirectedUrl[256] = {};
      if (resolveOneRedirect(localUrl, redirectedUrl, sizeof(redirectedUrl))) {
        logMsg("[radio] redirect: %s → %s", localUrl, redirectedUrl);
        strlcpy(localUrl, redirectedUrl, sizeof(localUrl));
      }
      bool ok = audio.connecttohost(localUrl);
      logMsg("[radio] task: connecttohost=%d grace=%lu", (int)ok, switchGraceUntil);
      if (ok) {
        radioIsPlaying   = true;
        switchingStation = false;
        radioDisplayActive = true;
        radioDisplayUntil  = millis() + RADIO_DISP_CONNECT_MS;
        audio.setVolume(radioVolume);
        audio.setTone(radioEqBass, radioEqMid, radioEqTreble);
        audio.forceMono(radioMono);
        audio.swapChannels(radioSwapChannels);
      } else {
        radioIsPlaying   = false;
        switchingStation = false;
        switchGraceUntil = 0;
        audio.setVolume(radioVolume);
      }
    } else if (gifAudioStopPending) {
      gifAudioStopPending = false;
      if (gifAudioPlaying) {
        gifAudioPlaying = false;
        audio.stopSong();
      }
    } else if (gifAudioPending) {
      gifAudioPending = false;
      if (!radioIsPlaying && !radioUserActive) {
        audio.connecttoFS(SD, gifAudioPath);
        gifAudioPlaying = true;
      }
    }

    if (radioIsPlaying || gifAudioPlaying) {
      audio.loop();
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ── Public API ─────────────────────────────────────────────────────────────────

void radioInit() {
  radioStringMutex = xSemaphoreCreateMutex();
  audio.setPinout(RADIO_I2S_BCLK, RADIO_I2S_LRC, RADIO_I2S_DOUT);
  logMsg("radioInit: BCLK=%d LRC=%d DOUT=%d", RADIO_I2S_BCLK, RADIO_I2S_LRC, RADIO_I2S_DOUT);
  loadRadioVolume();
  audio.setVolume(radioVolume);
  loadRadioEq();
  audio.setTone(radioEqBass, radioEqMid, radioEqTreble);
  loadRadioMono();
  audio.forceMono(radioMono);
  loadRadioSwap();
  audio.swapChannels(radioSwapChannels);
  radioLoadPresets();
  radioIconSlugsLoad();  // only icons for preset stations — no directory scan of all files
  loadLastPreset();

  static StaticTask_t radioTaskBuf;
  static StackType_t* radioStack = nullptr;
  if (!radioStack) {
    radioStack = (StackType_t*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (radioStack) {
    xTaskCreateStaticPinnedToCore(radioTask, "radioTask", 16384 / sizeof(StackType_t),
                                  NULL, 11, radioStack, &radioTaskBuf, 0);
    logMsg("radioInit: stack=16KB PSRAM Core0");
  } else {
    xTaskCreatePinnedToCore(radioTask, "radioTask", 16384, NULL, 11, NULL, 0);
    logMsg("radioInit: stack=16KB SRAM Core0 (PSRAM alloc failed)");
  }
}

void radioPlay(const char* url, int presetIndex) {
  logMsg("[radio] radioPlay: url=\"%s\" preset=%d playing=%d", url, presetIndex, (int)radioIsPlaying);
  radioUserActive    = true;
  switchingStation   = true;
  radioCurrentPreset = presetIndex;
  if (presetIndex >= 0) { radioLastPreset = presetIndex; saveLastPreset(); }
  if (radioStringMutex && xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    // Pre-fill with preset name so display shows station immediately while connecting
    if (presetIndex >= 0 && presetIndex < radioPresetCount)
      strlcpy(radioStationName, radioPresets[presetIndex].name, sizeof(radioStationName));
    else
      radioStationName[0] = '\0';
    radioTrackTitle[0] = '\0';
    xSemaphoreGive(radioStringMutex);
  }
  radioDisplayActive = true;
  radioDisplayUntil  = millis() + RADIO_DISP_PLAY_MS;
  // TLS handshake occupies 30-40 KB of internal SRAM — OOM with running MP3 codec
  if (strncmp(url, "https://", 8) == 0) {
    snprintf(pendingUrl, sizeof(pendingUrl), "http://%s", url + 8);
    logMsg("[radio] HTTPS → HTTP: \"%s\"", pendingUrl);
  } else {
    strlcpy(pendingUrl, url, sizeof(pendingUrl));
  }
  __sync_synchronize();
  connectPending = true;
}

void radioStop() {
  radioUserActive     = false;
  stopPending         = true;
  radioIsPlaying      = false;
  radioDisplayActive  = false;
  radioDisplayUntil   = 0;
  radioStationName[0] = '\0';
  radioTrackTitle[0]  = '\0';
  radioCurrentPreset  = -1;
}

void radioSetVolume(uint8_t vol) {
  if (vol > 21) vol = 21;
  radioVolume = vol;
  audio.setVolume(vol);
  saveRadioVolume();
}

void radioSetEq(int8_t bass, int8_t mid, int8_t treble) {
  radioEqBass   = (int8_t)constrain(bass,   -40, 12);
  radioEqMid    = (int8_t)constrain(mid,    -40, 12);
  radioEqTreble = (int8_t)constrain(treble, -40, 12);
  audio.setTone(radioEqBass, radioEqMid, radioEqTreble);
}

void radioSetSwapChannels(bool swap) {
  radioSwapChannels = swap;
  audio.swapChannels(swap);
  saveRadioSwap();
}

void radioPlayLocalFile(const char* sdPath) {
  if (radioIsPlaying) return;  // stream takes priority
  strlcpy(gifAudioPath, sdPath, sizeof(gifAudioPath));
  __sync_synchronize();
  gifAudioPending = true;
}

void radioStopLocalFile() {
  gifAudioStopPending = true;
}

// ── Preset persistence ────────────────────────────────────────────────────────

void radioLoadPresets() {
  File f = LittleFS.open("/radio_presets.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    radioPresetCount = 0;
    for (JsonObject p : doc.as<JsonArray>()) {
      if (radioPresetCount >= MAX_RADIO_PRESETS) break;
      strlcpy(radioPresets[radioPresetCount].name,     p["name"] | "",  sizeof(RadioPreset::name));
      strlcpy(radioPresets[radioPresetCount].url,      p["url"]  | "",  sizeof(RadioPreset::url));
      strlcpy(radioPresets[radioPresetCount].icon_url, p["icon"] | "",  sizeof(RadioPreset::icon_url));
      radioPresetCount++;
    }
  }
  f.close();
}

void radioSavePresets() {
  const size_t bufSize = 14000;
  char* buf = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) return;
  size_t pos = 0;
  buf[pos++] = '[';
  for (int i = 0; i < radioPresetCount; i++) {
    if (i > 0) buf[pos++] = ',';
    int n = snprintf(buf + pos, bufSize - pos,
                     "{\"name\":\"%s\",\"url\":\"%s\",\"icon\":\"%s\"}",
                     radioPresets[i].name, radioPresets[i].url, radioPresets[i].icon_url);
    if (n < 0 || (size_t)n >= bufSize - pos) break;
    pos += (size_t)n;
  }
  buf[pos++] = ']';
  buf[pos]   = '\0';
  File f = LittleFS.open("/radio_presets.json", "w");
  if (f) { f.write((uint8_t*)buf, pos); f.close(); }
  heap_caps_free(buf);
  radioIconSlugsLoad();  // refresh icon cache after preset change
}

// ── Webserver routes ──────────────────────────────────────────────────────────

extern void sendLittleFSHtml(AsyncWebServerRequest *request, const char* path);

void radioRegisterRoutes(AsyncWebServer* server) {
  server->on("/radio.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendLittleFSHtml(request, "/radio.html");
  });

  server->on("/radio_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Snapshots under mutex — Core 0 may write radioStationName/radioTrackTitle at any time
    char stSnap[64]    = "";
    char titleSnap[128] = "";
    if (radioStringMutex && xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      strlcpy(stSnap,    radioStationName, sizeof(stSnap));
      strlcpy(titleSnap, radioTrackTitle,  sizeof(titleSnap));
      xSemaphoreGive(radioStringMutex);
    }
    // Read preset index once (volatile), then use consistently
    int preset = radioCurrentPreset;
    if (!stSnap[0] && preset >= 0 && preset < radioPresetCount)
      strlcpy(stSnap, radioPresets[preset].name, sizeof(stSnap));
    char iconSnap[256] = "";
    if (preset >= 0 && preset < radioPresetCount)
      strlcpy(iconSnap, radioPresets[preset].icon_url, sizeof(iconSnap));

    char json[1024];
    snprintf(json, sizeof(json),
      "{\"playing\":%s,\"station\":\"%s\",\"title\":\"%s\",\"icon\":\"%s\","
      "\"volume\":%d,\"preset\":%d,\"lastPreset\":%d,\"displayActive\":%s,"
      "\"eqBass\":%d,\"eqMid\":%d,\"eqTreble\":%d,\"mono\":%s,\"swapChannels\":%s}",
      (radioIsPlaying || switchingStation) ? "true" : "false",
      stSnap, titleSnap, iconSnap,
      (int)radioVolume, preset, (int)radioLastPreset,
      radioDisplayActive ? "true" : "false",
      (int)radioEqBass, (int)radioEqMid, (int)radioEqTreble,
      radioMono ? "true" : "false",
      radioSwapChannels ? "true" : "false");
    AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", json);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
  });

  server->on("/radio_play", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("url", true)) {
      request->send(400, "text/plain", "Missing url");
      return;
    }
    String url    = request->getParam("url",    true)->value();
    int    preset = request->hasParam("preset", true)
                    ? request->getParam("preset", true)->value().toInt()
                    : -1;
    radioPlay(url.c_str(), preset);
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    radioStop();
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_volume", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("vol", true)) {
      request->send(400, "text/plain", "Missing vol");
      return;
    }
    radioSetVolume((uint8_t)request->getParam("vol", true)->value().toInt());
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_eq", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("bass", true) ||
        !request->hasParam("mid",  true) ||
        !request->hasParam("treble", true)) {
      request->send(400, "text/plain", "Missing bass/mid/treble");
      return;
    }
    radioSetEq(
      (int8_t)request->getParam("bass",   true)->value().toInt(),
      (int8_t)request->getParam("mid",    true)->value().toInt(),
      (int8_t)request->getParam("treble", true)->value().toInt()
    );
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_eq_save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("bass", true) &&
        request->hasParam("mid",  true) &&
        request->hasParam("treble", true)) {
      radioSetEq(
        (int8_t)request->getParam("bass",   true)->value().toInt(),
        (int8_t)request->getParam("mid",    true)->value().toInt(),
        (int8_t)request->getParam("treble", true)->value().toInt()
      );
    }
    saveRadioEq();
    request->send(200, "text/plain", "OK");
  });

  server->on("/gif_audio_play", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("path", true)) {
      request->send(400, "text/plain", "Missing path");
      return;
    }
    String path = request->getParam("path", true)->value();
    if (path.startsWith("SD:")) path = path.substring(3);
    if (!path.startsWith("/") || (!path.endsWith(".mp3") && !path.endsWith(".MP3"))) {
      request->send(400, "text/plain", "Ungültiger Pfad (erwartet /GifAudio/file.mp3)");
      return;
    }
    if (!SD.exists(path.c_str())) {
      request->send(404, "text/plain", "Datei nicht gefunden");
      return;
    }
    if (radioIsPlaying) {
      request->send(409, "text/plain", "Radio läuft — GIF-Audio hat keinen Vorrang");
      return;
    }
    radioPlayLocalFile(path.c_str());
    request->send(200, "text/plain", "OK");
  });

  server->on("/gif_audio_stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    radioStopLocalFile();
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_mono", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("mono", true)) {
      request->send(400, "text/plain", "Missing mono");
      return;
    }
    radioMono = request->getParam("mono", true)->value().toInt() != 0;
    audio.forceMono(radioMono);
    saveRadioMono();
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_swap", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("swap", true)) {
      request->send(400, "text/plain", "Missing swap");
      return;
    }
    radioSetSwapChannels(request->getParam("swap", true)->value().toInt() != 0);
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_display", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Button press: show display for 10 seconds, then auto-off
    radioDisplayActive = true;
    radioDisplayUntil  = millis() + RADIO_DISP_MANUAL_MS;
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_presets", HTTP_GET, [](AsyncWebServerRequest *request) {
    const size_t bufSize = 14000;
    char* buf = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { request->send(503, "text/plain", "OOM"); return; }
    size_t pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < radioPresetCount; i++) {
      if (i > 0) buf[pos++] = ',';
      int n = snprintf(buf + pos, bufSize - pos,
                       "{\"name\":\"%s\",\"url\":\"%s\",\"icon\":\"%s\"}",
                       radioPresets[i].name, radioPresets[i].url, radioPresets[i].icon_url);
      if (n < 0 || (size_t)n >= bufSize - pos) break;
      pos += (size_t)n;
    }
    buf[pos++] = ']';
    buf[pos]   = '\0';
    request->send(200, "application/json", buf);
    heap_caps_free(buf);
  });

  server->on("/radio_save_preset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name", true) || !request->hasParam("url", true)) {
      request->send(400, "text/plain", "Missing name or url");
      return;
    }
    if (radioPresetCount >= MAX_RADIO_PRESETS) {
      request->send(409, "text/plain", "Max presets reached");
      return;
    }
    String name = request->getParam("name", true)->value();
    String url  = request->getParam("url",  true)->value();
    String icon = request->hasParam("icon", true) ? request->getParam("icon", true)->value() : "";
    strlcpy(radioPresets[radioPresetCount].name,     name.c_str(), sizeof(RadioPreset::name));
    strlcpy(radioPresets[radioPresetCount].url,      url.c_str(),  sizeof(RadioPreset::url));
    strlcpy(radioPresets[radioPresetCount].icon_url, icon.c_str(), sizeof(RadioPreset::icon_url));
    radioPresetCount++;
    radioSavePresets();
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_delete_preset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("index", true)) {
      request->send(400, "text/plain", "Missing index");
      return;
    }
    int idx = request->getParam("index", true)->value().toInt();
    if (idx < 0 || idx >= radioPresetCount) {
      request->send(400, "text/plain", "Invalid index");
      return;
    }
    if (radioCurrentPreset == idx) radioStop();
    else if (radioCurrentPreset > idx) radioCurrentPreset--;
    for (int i = idx; i < radioPresetCount - 1; i++)
      radioPresets[i] = radioPresets[i + 1];
    radioPresetCount--;
    radioSavePresets();
    request->send(200, "text/plain", "OK");
  });
}

#endif // WEBRADIO_ENABLED

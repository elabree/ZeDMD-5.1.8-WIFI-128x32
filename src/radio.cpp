#if defined(WEBRADIO_ENABLED)

#include "radio.h"
#include <Audio.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "sd_interface.h"

static Audio audio;

volatile bool    radioIsPlaying     = false;
volatile bool    radioUserActive    = false;   // true solange User Radio wollte (auch nach EOF)
volatile bool    radioDisplayActive = false;
uint32_t         radioDisplayUntil  = 0;   // millis()-Zeitstempel; 0 = kein Auto-Aus
char             radioStationName[64]  = "";
char             radioTrackTitle[128]  = "";
SemaphoreHandle_t radioStringMutex    = nullptr;
uint8_t       radioVolume           = RADIO_DEFAULT_VOLUME;
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
// Grace-Timer: EOF-Callbacks während Verbindungsaufbau ignorieren
// (Playlist-Auflösung und Redirect feuern EOF bevor der Stream startet)
static volatile uint32_t switchGraceUntil = 0;

extern void logMsg(const char* fmt, ...);

// ── Callbacks von ESP32-audioI2S ──────────────────────────────────────────────

void audio_info(const char* info) {
  if (info) logMsg("[audio] %s", info);
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

// ── Last-Preset-Persistenz (vor Public API — wird von radioInit/radioPlay genutzt) ──

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

// ── FreeRTOS Task (Core 0) ────────────────────────────────────────────────────

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
      audio.setVolume(0);       // Mute: PSRAM-Altdaten vom vorherigen Sender unterdrücken
      audio.stopSong();
      vTaskDelay(pdMS_TO_TICKS(500));
      switchGraceUntil = millis() + 6000;  // 6s Grace für Playlist-Auflösung
      bool ok = audio.connecttohost(localUrl);
      logMsg("[radio] task: connecttohost=%d grace=%lu", (int)ok, switchGraceUntil);
      if (ok) {
        radioIsPlaying   = true;
        switchingStation = false;
        audio.setVolume(radioVolume);
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

// ── Public API ────────────────────────────────────────────────────────────────

void radioInit() {
  radioStringMutex = xSemaphoreCreateMutex();
  audio.setPinout(RADIO_I2S_BCLK, RADIO_I2S_LRC, RADIO_I2S_DOUT);
  loadRadioVolume();
  audio.setVolume(radioVolume);
  radioLoadPresets();
  loadLastPreset();
  xTaskCreatePinnedToCore(radioTask, "radioTask", 16384, NULL, 2, NULL, 0);
}

void radioPlay(const char* url, int presetIndex) {
  logMsg("[radio] radioPlay: url=\"%s\" preset=%d playing=%d", url, presetIndex, (int)radioIsPlaying);
  radioUserActive    = true;
  switchingStation   = true;
  radioCurrentPreset = presetIndex;
  if (presetIndex >= 0) { radioLastPreset = presetIndex; saveLastPreset(); }
  if (radioStringMutex && xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    radioStationName[0] = '\0';
    radioTrackTitle[0]  = '\0';
    xSemaphoreGive(radioStringMutex);
  }
  radioDisplayActive = true;
  radioDisplayUntil  = millis() + 5000;
  strlcpy(pendingUrl, url, sizeof(pendingUrl));
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

void radioPlayLocalFile(const char* sdPath) {
  if (radioIsPlaying) return;  // Stream hat Vorrang
  strlcpy(gifAudioPath, sdPath, sizeof(gifAudioPath));
  __sync_synchronize();
  gifAudioPending = true;
}

void radioStopLocalFile() {
  gifAudioStopPending = true;
}

// ── Preset-Persistenz ─────────────────────────────────────────────────────────

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
  DynamicJsonDocument doc(6144);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < radioPresetCount; i++) {
    JsonObject p = arr.createNestedObject();
    p["name"] = radioPresets[i].name;
    p["url"]  = radioPresets[i].url;
    p["icon"] = radioPresets[i].icon_url;
  }
  File f = LittleFS.open("/radio_presets.json", "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

// ── Webserver-Routen ──────────────────────────────────────────────────────────

extern void sendLittleFSHtml(AsyncWebServerRequest *request, const char* path);

void radioRegisterRoutes(AsyncWebServer* server) {
  server->on("/radio.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendLittleFSHtml(request, "/radio.html");
  });

  server->on("/radio_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Snapshots unter Mutex — Core 0 kann radioStationName/radioTrackTitle jederzeit schreiben
    char stSnap[64]    = "";
    char titleSnap[128] = "";
    if (radioStringMutex && xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      strlcpy(stSnap,    radioStationName, sizeof(stSnap));
      strlcpy(titleSnap, radioTrackTitle,  sizeof(titleSnap));
      xSemaphoreGive(radioStringMutex);
    }
    // Preset-Index einmal lesen (volatile), dann konsistent verwenden
    int preset = radioCurrentPreset;
    if (!stSnap[0] && preset >= 0 && preset < radioPresetCount)
      strlcpy(stSnap, radioPresets[preset].name, sizeof(stSnap));
    char iconSnap[256] = "";
    if (preset >= 0 && preset < radioPresetCount)
      strlcpy(iconSnap, radioPresets[preset].icon_url, sizeof(iconSnap));

    String json = "{";
    json += "\"playing\":"       + String((radioIsPlaying || switchingStation) ? "true" : "false") + ",";
    json += "\"station\":\""     + String(stSnap)            + "\",";
    json += "\"title\":\""       + String(titleSnap)         + "\",";
    json += "\"icon\":\""        + String(iconSnap)          + "\",";
    json += "\"volume\":"        + String(radioVolume)        + ",";
    json += "\"preset\":"        + String(preset)             + ",";
    json += "\"lastPreset\":"    + String(radioLastPreset)    + ",";
    json += "\"displayActive\":" + String(radioDisplayActive  ? "true" : "false");
    json += "}";
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

  server->on("/radio_display", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Knopfdruck: Display 10 Sekunden anzeigen, dann automatisch aus
    radioDisplayActive = true;
    radioDisplayUntil  = millis() + 10000;
    request->send(200, "text/plain", "OK");
  });

  server->on("/radio_presets", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    for (int i = 0; i < radioPresetCount; i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + String(radioPresets[i].name)     + "\","
               "\"url\":\""  + String(radioPresets[i].url)      + "\","
               "\"icon\":\"" + String(radioPresets[i].icon_url) + "\"}";
    }
    json += "]";
    request->send(200, "application/json", json);
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

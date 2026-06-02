#if defined(WEBRADIO_ENABLED)

#include "radio.h"
#include <Audio.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SD.h>

static Audio audio;

volatile bool    radioIsPlaying     = false;
volatile bool    radioDisplayActive = true;
char             radioStationName[64]  = "";
char             radioTrackTitle[128]  = "";
SemaphoreHandle_t radioStringMutex    = nullptr;
uint8_t       radioVolume           = RADIO_DEFAULT_VOLUME;
RadioPreset   radioPresets[MAX_RADIO_PRESETS];
int           radioPresetCount   = 0;
int           radioCurrentPreset = -1;
int           radioLastPreset    = -1;

static char          pendingUrl[256]     = "";
static volatile bool connectPending      = false;
static volatile bool stopPending         = false;
static volatile bool switchingStation    = false;
static char          gifAudioPath[256]   = "";
static volatile bool gifAudioPending     = false;
static volatile bool gifAudioStopPending = false;
volatile bool        gifAudioPlaying     = false;

// ── Callbacks von ESP32-audioI2S ──────────────────────────────────────────────

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
  if (gifAudioPlaying)       gifAudioPlaying = false;
  else if (!switchingStation) radioIsPlaying  = false;
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

// ── FreeRTOS Task (Core 0) ────────────────────────────────────────────────────

static void radioTask(void* params) {
  for (;;) {
    if (stopPending) {
      stopPending    = false;
      radioIsPlaying = false;
      gifAudioPlaying = false;
      audio.stopSong();
    } else if (connectPending) {
      switchingStation = true;
      connectPending  = false;
      radioIsPlaying  = false;
      gifAudioPlaying = false;
      audio.stopSong();
      vTaskDelay(pdMS_TO_TICKS(200));
      audio.connecttohost(pendingUrl);
      radioIsPlaying   = true;
      switchingStation = false;
    } else if (gifAudioStopPending) {
      gifAudioStopPending = false;
      if (gifAudioPlaying) {
        gifAudioPlaying = false;
        audio.stopSong();
      }
    } else if (gifAudioPending) {
      gifAudioPending = false;
      if (!radioIsPlaying) {
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
  audio.setVolume(RADIO_DEFAULT_VOLUME);
  radioLoadPresets();
  loadLastPreset();
  xTaskCreatePinnedToCore(radioTask, "radioTask", 16384, NULL, 2, NULL, 0);
}

void radioPlay(const char* url, int presetIndex) {
  switchingStation   = true;
  radioCurrentPreset = presetIndex;
  if (presetIndex >= 0) { radioLastPreset = presetIndex; saveLastPreset(); }
  radioTrackTitle[0] = '\0';
  strlcpy(pendingUrl, url, sizeof(pendingUrl));
  __sync_synchronize();  // pendingUrl muss vollständig geschrieben sein bevor Core 0 die Flag sieht
  connectPending = true;
}

void radioStop() {
  stopPending         = true;
  radioIsPlaying      = false;
  radioStationName[0] = '\0';
  radioTrackTitle[0]  = '\0';
  radioCurrentPreset  = -1;
}

void radioSetVolume(uint8_t vol) {
  if (vol > 21) vol = 21;
  radioVolume = vol;
  audio.setVolume(vol);
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

void radioRegisterRoutes(AsyncWebServer* server) {
  server->on("/radio.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/radio.html", "text/html");
  });

  server->on("/radio_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"playing\":"       + String((radioIsPlaying || switchingStation) ? "true" : "false") + ",";
    json += "\"station\":\""     + String(radioStationName)   + "\",";
    json += "\"title\":\""       + String(radioTrackTitle)    + "\",";
    json += "\"volume\":"        + String(radioVolume)        + ",";
    json += "\"preset\":"        + String(radioCurrentPreset) + ",";
    json += "\"lastPreset\":"    + String(radioLastPreset)    + ",";
    json += "\"displayActive\":" + String(radioDisplayActive  ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
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

  server->on("/radio_display", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("active", true))
      radioDisplayActive = request->getParam("active", true)->value() == "1";
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

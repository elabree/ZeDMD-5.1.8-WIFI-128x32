
#include <Arduino.h>
#include <algorithm>
#include <memory>
#include <AsyncUDP.h>
#include <Bounce2.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#ifdef ZEDMD_WIFI
#include <PubSubClient.h>
#include <Update.h>
#endif

#include <cstring>
#include <AnimatedGIF.h>
#ifdef WEBRADIO_ENABLED
#include "radio.h"
#endif
#ifdef ZEDMD_WIFI
#include "weather.h"
#include "clock.h"
#endif
#include "sd_interface.h"
#ifdef SD_MMC_BUILD
  #define SD_MMC_CLK_PIN  39
  #define SD_MMC_CMD_PIN  38
  #define SD_MMC_DATA_PIN 40
#else
  #include <SPI.h>
  // SD Karte SPI Pins
  #ifdef CONFIG_IDF_TARGET_ESP32S3
    // ESP32-S3 Pins (HUB75 belegt andere Pins) — überschreibbar via build_flags
    #ifndef SD_MOSI
    #define SD_MOSI 11
    #endif
    #ifndef SD_MISO
    #define SD_MISO 13
    #endif
    #ifndef SD_SCK
    #define SD_SCK  12
    #endif
    #ifndef SD_CS
    #define SD_CS   10
    #endif
  #elif defined(ZEDMD_WIFI)
    // Standard ESP32 WiFi Build
    #define SD_MOSI 18
    #define SD_MISO 21
    #define SD_SCK  2
    #define SD_CS   33
  #else
    // Standard ESP32 USB Build — keine SD Karte
    #define SD_MOSI 18
    #define SD_MISO 21
    #define SD_SCK  2
    #define SD_CS   33
  #endif
#endif

// Specific improvements and #define for the ESP32 S3 series
#if defined(ARDUINO_ESP32_S3_N16R8) || defined(DISPLAY_RM67162_AMOLED)
#include "S3Specific.h"
#endif
#include "displayDriver.h"  // Base class for all display drivers
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "miniz/miniz.h"
#include "panel.h"
#include "version.h"
#include <time.h>  // NTP / Uhrzeit

// To save RAM only include the driver we want to use.
#ifdef DISPLAY_RM67162_AMOLED
#include "displays/Rm67162Amoled.h"
#else
#include "displays/LEDMatrix.h"
#endif

#define N_FRAME_CHARS 5
#define N_CTRL_CHARS 5
#define N_ACK_CHARS (N_CTRL_CHARS + 1)
#define N_INTERMEDIATE_CTR_CHARS 4
#ifdef BOARD_HAS_PSRAM
#define NUM_BUFFERS 128  // Number of buffers
#ifdef DISPLAY_RM67162_AMOLED
// @fixme double buffering doesn't work on Lilygo Amoled
#define NUM_RENDER_BUFFERS 1
#else
#define NUM_RENDER_BUFFERS 2
#endif
#define BUFFER_SIZE 1152
#else
#define NUM_BUFFERS 12  // Number of buffers
#define NUM_RENDER_BUFFERS 1
#define BUFFER_SIZE 1152
#endif
#if defined(ARDUINO_ESP32_S3_N16R8) || defined(DISPLAY_RM67162_AMOLED)
// USB CDC
#define SERIAL_BAUD 115200
#define USB_PACKAGE_SIZE 512
#else
#define SERIAL_BAUD 921600
#define USB_PACKAGE_SIZE 32
#endif
#define SERIAL_TIMEOUT \
  8  // Time in milliseconds to wait for the next data chunk.

#define CONNECTION_TIMEOUT 5000

#ifdef ARDUINO_ESP32_S3_N16R8
#define UP_BUTTON_PIN 0
#define DOWN_BUTTON_PIN 45
#define FORWARD_BUTTON_PIN 48
#define BACKWARD_BUTTON_PIN 47
#elif defined(DISPLAY_RM67162_AMOLED)
#define UP_BUTTON_PIN 0
#define FORWARD_BUTTON_PIN 21
#else
#define UP_BUTTON_PIN 21
#define FORWARD_BUTTON_PIN 33
#endif

#define LED_CHECK_DELAY 1000  // ms per color

#define RC 0
#define GC 1
#define BC 2

enum {
  TRANSPORT_USB = 0,
  TRANSPORT_WIFI_UDP = 1,
  TRANSPORT_WIFI_TCP = 2,
  TRANSPORT_SPI = 3
};

const uint8_t FrameChars[5]
    __attribute__((aligned(4))) = {'F', 'R', 'A', 'M', 'E'};
const uint8_t CtrlChars[6]
    __attribute__((aligned(4))) = {'Z', 'e', 'D', 'M', 'D', 'A'};
uint8_t numCtrlCharsFound = 0;

AsyncWebServer *server;
AsyncServer *tcp;
AsyncUDP *udp;
DisplayDriver *display;

// Buffers for storing data
uint8_t *buffers[NUM_BUFFERS];
mz_ulong bufferSizes[NUM_BUFFERS] __attribute__((aligned(4))) = {0};
bool bufferCompressed[NUM_BUFFERS] __attribute__((aligned(4))) = {0};

// The uncompress buffer should be bug enough
uint8_t* uncompressBuffer = nullptr;  // alloziert in PSRAM (setup)
uint8_t *renderBuffer[NUM_RENDER_BUFFERS];
uint8_t currentRenderBuffer __attribute__((aligned(4)));
uint8_t lastRenderBuffer __attribute__((aligned(4)));
char tmpStringBuffer[33] __attribute__((aligned(4))) = {0};
bool payloadCompressed __attribute__((aligned(4)));
uint16_t payloadSize __attribute__((aligned(4)));
uint16_t payloadMissing __attribute__((aligned(4)));
uint8_t headerBytesReceived __attribute__((aligned(4)));
uint8_t command __attribute__((aligned(4)));
uint8_t currentBuffer __attribute__((aligned(4)));
uint8_t lastBuffer __attribute__((aligned(4)));
uint8_t processingBuffer __attribute__((aligned(4)));

// Init display on a low brightness to avoid power issues, but bright enough to
// see something.
#ifdef DISPLAY_RM67162_AMOLED
uint8_t brightness = 5;
#else
uint8_t brightness = 2;
int8_t rgbMode = 0;
uint8_t rgbModeLoaded = 0;
int8_t yOffset = 0;
#ifdef DISPLAY_LED_MATRIX
uint8_t panelClkphase = 0;
uint8_t panelDriver = 0;
uint8_t panelI2sspeed = 8;
uint8_t panelLatchBlanking = 2;
uint8_t panelMinRefreshRate = 30;
#endif

// I needed to change these from RGB to RC (Red Color), BC, GC to prevent
// conflicting with the TFT_SPI Library.
const uint8_t rgbOrder[3 * 6] = {
    RC, GC, BC,  // rgbMode 0
    BC, RC, GC,  // rgbMode 1
    GC, BC, RC,  // rgbMode 2
    RC, BC, GC,  // rgbMode 3
    GC, RC, BC,  // rgbMode 4
    BC, GC, RC   // rgbMode 5
};

#endif
uint8_t usbPackageSizeMultiplier = USB_PACKAGE_SIZE / 32;
uint8_t settingsMenu = 0;
uint8_t debug = 0;
uint8_t udpDelay = 5;

String ssid;
String pwd;
uint16_t port = 3333;
uint8_t ssid_length;
uint8_t pwd_length;
bool wifiActive;
#ifdef ZEDMD_WIFI
int8_t transport = TRANSPORT_WIFI_UDP;
#else
int8_t transport = TRANSPORT_USB;
#endif
// ── Log Ring-Buffer ───────────────────────────────────────────────────────────
#define LOG_LINES        80
#define LOG_LINE_LEN     120
#define RTC_LOG_LINES    40
#define RTC_LINE_LEN     100
#define CRASH_LOG_SLOTS  10

static char (*logBuffer)[LOG_LINE_LEN] = nullptr;  // alloziert in PSRAM (setup)
static uint8_t logHead = 0;
static uint8_t logCount = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// RTC-Speicher: überlebt Watchdog/Exception-Resets
RTC_DATA_ATTR static char rtcLog[RTC_LOG_LINES][RTC_LINE_LEN];
RTC_DATA_ATTR static uint8_t rtcLogHead = 0;
RTC_DATA_ATTR static uint8_t rtcLogCount = 0;
RTC_DATA_ATTR static bool     rtcLogValid   = false;
RTC_DATA_ATTR static uint32_t rtcLastUptime = 0;   // Sekunden letzter stabiler Lauf; 0 nach POWERON

void logMsg(const char* fmt, ...) {
  char tmp[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
#ifdef WEBRADIO_ENABLED
  if (!radioIsPlaying) Serial.println(tmp);
#else
  Serial.println(tmp);
#endif
  uint32_t ms = millis();
  char line[LOG_LINE_LEN];
  snprintf(line, sizeof(line), "[%lu.%03lu] %s", ms / 1000, ms % 1000, tmp);
  portENTER_CRITICAL(&logMux);
  if (logBuffer) {
    strncpy(logBuffer[logHead], line, LOG_LINE_LEN - 1);
    logHead = (logHead + 1) % LOG_LINES;
    if (logCount < LOG_LINES) logCount++;
  }
  // Auch in RTC-Speicher schreiben
  strncpy(rtcLog[rtcLogHead], line, RTC_LINE_LEN - 1);
  rtcLog[rtcLogHead][RTC_LINE_LEN - 1] = '\0';
  rtcLogHead = (rtcLogHead + 1) % RTC_LOG_LINES;
  if (rtcLogCount < RTC_LOG_LINES) rtcLogCount++;
  rtcLogValid = true;
  portEXIT_CRITICAL(&logMux);
}
// ── Crash-Diagnose (LittleFS, konsistent mit Settings-Pattern) ───────────────

// File-scope: kein C++-Guard nötig, sicher nach PANIC-Reset
static constexpr int kRsnN = 9;
static const struct { esp_reset_reason_t r; const char* name; } kRsn[kRsnN] = {
  { ESP_RST_POWERON,    "POWERON"    },
  { ESP_RST_SW,         "SW"         },
  { ESP_RST_PANIC,      "PANIC"      },
  { ESP_RST_INT_WDT,    "INT_WDT"    },
  { ESP_RST_TASK_WDT,   "TASK_WDT"   },
  { ESP_RST_WDT,        "WDT"        },
  { ESP_RST_BROWNOUT,   "BROWNOUT"   },
  { ESP_RST_PWR_GLITCH, "PWR_GLITCH" },
  { ESP_RST_CPU_LOCKUP, "CPU_LOCKUP" },
};

static uint32_t diagBootCount = 0;

static uint32_t diagReadUInt(const char* json, const char* key, uint32_t def) {
  char needle[32];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  const char* p = strstr(json, needle);
  if (!p) return def;
  p += strlen(needle);
  while (*p == ' ') p++;
  return (uint32_t)strtoul(p, nullptr, 10);
}

// Direkt nach LittleFS.begin() aufrufen — vor LoadSettingsMenu() und dem
// esp_reset_reason()-Switch in setup(). Beim Dump enthält der RTC-Buffer
// genau 1 neues "=== ZeDMD booting ==="-Entry; der Rest ist Pre-Crash-Log.
static void diagBoot() {
  // static: kein Stack-Druck nach PANIC (Stack könnte teilbeschädigt sein)
  static char jsonBuf[600];
  strcpy(jsonBuf, "{}");
  {
    File rf = LittleFS.open("/diag.json", "r");
    if (rf) {
      size_t n = rf.readBytes(jsonBuf, sizeof(jsonBuf) - 1);
      jsonBuf[n] = '\0';
      rf.close();
    }
  }

  // ── Pre-Crash-State sichern VOR logMsg() — Dump-Bedingung basiert darauf ──
  bool    preLogValid = rtcLogValid;
  uint8_t preLogCount = rtcLogCount;
  uint8_t preLogHead  = rtcLogHead;
  // rtcLastUptime sofort nullen — Bootloop-Schutz: nächster Reboot sieht 0
  uint32_t lastUp     = rtcLastUptime;
  rtcLastUptime       = 0;
  logMsg("diagBoot: preLog snapped valid=%d count=%d lastUp=%us",
         (int)preLogValid, (int)preLogCount, (unsigned)lastUp);

  diagBootCount        = diagReadUInt(jsonBuf, "boots",      0) + 1;
  uint8_t  crashNext   = (uint8_t)(diagReadUInt(jsonBuf, "crashNext", 0) % CRASH_LOG_SLOTS);
  uint32_t otherCount  = diagReadUInt(jsonBuf, "boots_OTHER", 0);
  uint32_t counts[kRsnN];

  esp_reset_reason_t reason     = esp_reset_reason();
  const char*        reasonName = "OTHER";
  for (int i = 0; i < kRsnN; i++) {
    char key[20];
    snprintf(key, sizeof(key), "boots_%s", kRsn[i].name);
    counts[i] = diagReadUInt(jsonBuf, key, 0);
    if (kRsn[i].r == reason) { counts[i]++; reasonName = kRsn[i].name; }
  }
  if (strcmp(reasonName, "OTHER") == 0) otherCount++;
  logMsg("diagBoot: reason=%s boots=%u", reasonName, (unsigned)diagBootCount);

  logMsg("Boot #%u | Reset: %s | Heap: %u | lastUp: %us",
         (unsigned)diagBootCount, reasonName,
         (unsigned)esp_get_free_heap_size(), (unsigned)lastUp);

  // Nur dumpen wenn: valider Pre-Crash-Log, kein sauberer Reboot.
  // Bootloop-Schutz (lastUp > 60) nur für BROWNOUT/PWR_GLITCH — echte Crashes
  // (PANIC, WDT) immer dumpen, auch wenn der vorherige Boot kurz war.
  bool isCleanReset = (reason == ESP_RST_POWERON ||
                       reason == ESP_RST_SW       ||
                       reason == ESP_RST_DEEPSLEEP);
  bool isBrownout   = (reason == ESP_RST_BROWNOUT ||
                       reason == ESP_RST_PWR_GLITCH);
  bool isPanicOrWdt = !isCleanReset && !isBrownout;
  // PANIC/WDT: immer dumpen (auch bei nur 1 Log-Zeile — früher Crash)
  // BROWNOUT/andere: nur dumpen wenn genug Log vorhanden und kein Kurzboot
  bool shouldDump   = preLogValid && !isCleanReset && (
                        isPanicOrWdt ||
                        (preLogCount > 1 && (!isBrownout || lastUp > 60)));

  logMsg("diagBoot: valid=%d count=%d dump=%d reason=%s",
         (int)preLogValid, (int)preLogCount, (int)shouldDump, reasonName);

  if (shouldDump) {
    if (!LittleFS.exists("/crashlogs")) LittleFS.mkdir("/crashlogs");
    char fname[32];
    snprintf(fname, sizeof(fname), "/crashlogs/crash-%u.txt", (unsigned)crashNext);
    File f = LittleFS.open(fname, "w");
    if (f) {
      char hdr[80];
      snprintf(hdr, sizeof(hdr), "=== Crash %u | Reset: %s | Boot#%u | uptime: %us ===\n",
               (unsigned)crashNext, reasonName, (unsigned)diagBootCount, (unsigned)lastUp);
      f.print(hdr);
      uint8_t start = (preLogCount < RTC_LOG_LINES) ? 0 : preLogHead;
      for (uint8_t i = 0; i < preLogCount; i++)
        f.println(rtcLog[(start + i) % RTC_LOG_LINES]);
      f.close();
      crashNext   = (uint8_t)((crashNext + 1) % CRASH_LOG_SLOTS);
      rtcLogValid = false;
      rtcLogCount = 0;
      rtcLogHead  = 0;
      logMsg("Crash-Dump: %s (uptime war %us)", fname, (unsigned)lastUp);
    } else {
      logMsg("diagBoot: FEHLER - %s konnte nicht geoeffnet werden", fname);
    }
  }

  File wf = LittleFS.open("/diag.json.tmp", "w");
  if (wf) {
    wf.printf("{\n  \"boots\": %u,\n  \"lastReset\": \"%s\",\n  \"crashNext\": %u",
              (unsigned)diagBootCount, reasonName, (unsigned)crashNext);
    for (int i = 0; i < kRsnN; i++)
      wf.printf(",\n  \"boots_%s\": %u", kRsn[i].name, (unsigned)counts[i]);
    wf.printf(",\n  \"boots_OTHER\": %u\n}\n", (unsigned)otherCount);
    wf.close();
    LittleFS.rename("/diag.json.tmp", "/diag.json");
  }
}

// ─────────────────────────────────────────────────────────────────────────────

bool logoActive;
volatile bool transportActive;  // volatile: wird von Task_ReadSerial (Core 1) gesetzt!
uint8_t transportWaitCounter;
uint16_t logoWaitCounter;
uint32_t lastDataReceived;
bool serverRunning;
uint8_t throbberColors[6] __attribute__((aligned(4))) = {0};
mz_ulong uncompressedBufferSize = 2048;
uint16_t shortId;
// Screensaver — Pfade dynamisch im PSRAM, kein festes Limit
SemaphoreHandle_t screensaverFilesMutex = nullptr;
char (*screensaverFiles)[128] = nullptr;
uint16_t screensaverFilesCapacity = 0;
uint16_t screensaverCount = 0;
uint16_t screensaverIndex = 0;
uint32_t screensaverRAWShowStart = 0;
uint8_t screensaverBrightness = 3;
uint8_t screensaverDuration = 10;  // Anzeigedauer in Sekunden, Default 10
bool screensaverShuffle = false;
bool screensaverStrictTimer = true;
volatile bool sdCardAvailable = false;
bool sdCardWarningPending = false;
uint64_t sdTotalBytes = 0;
uint64_t sdUsedBytes = 0;
volatile bool screensaverReloadNeeded  = false;
volatile bool screensaverLoadRunning   = false;  // Background-Task läuft
volatile bool sdRefreshNeeded = false;
// Cache-Puffer in PSRAM — dynamisch allokiert, atomarer Swap verhindert Null-Fenster
#define CACHE_SD_FOLDER_SIZE     256
static char* cachedSDFolders     = nullptr;
static char* cachedGifAudioFiles  = nullptr;
static char* cachedSdFiles        = nullptr;
static char  cachedSdFilesFolder[CACHE_SD_FOLDER_SIZE] = "";
volatile bool gifAudioRefreshNeeded       = false;
volatile bool weatherIconTestActive       = false;
volatile bool iconsReloadNeeded           = false;
volatile bool sdFilesRefreshNeeded        = false;
volatile bool sdFoldersInvalidateNeeded   = false;
volatile bool sdFoldersRefreshNeeded      = false;
volatile bool sdFilesInvalidateNeeded     = false;
bool gifAudioEnabled = true;
bool screensaverPaused = false;  // Screensaver Pause/Play
static volatile bool displayTextActive     = false;
static char   displayTextContent[128]      = "";
static uint8_t displayTextR = 255, displayTextG = 255, displayTextB = 255;
static bool   displayTextScroll            = false;
static uint32_t displayTextEnd             = 0;
static int16_t  displayTextScrollX         = 128;
static bool     displayTextNeedsClear      = true;  // ClearScreen only on first frame and wrap
uint8_t screensaverMode = 0;     // 0=Screensaver only, 1=Clock only, 2=Clock+Screensaver
// ntpSynced, ntpServer, clockR/G/B, dateR/G/B, clockColorChanged → clock.cpp
// Wetter (Modus 3) — Globals jetzt in weather.cpp

#ifdef ZEDMD_WIFI
String   mqttServer    = "";
uint16_t mqttPort      = 1883;
#endif
#ifdef ZEDMD_WIFI
WiFiClient   mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
uint32_t     lastMqttReconnect = 0;

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (length == 0) return;
  static char buf[2048];  // static: kein Stack-Druck auf mqttTask (8KB Stack)
  if (length >= sizeof(buf)) return;
  memcpy(buf, payload, length);
  buf[length] = '\0';

  auto val = [&](const char* key) -> float {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(buf, search);
    if (!p) return 0.0f;
    const char* v = p + strlen(search);
    while (*v == ' ') v++;
    if (*v == '"') v++;
    return atof(v);
  };

  weatherTemp      = val("outTemp_C");
  weatherHumidity  = (uint8_t)roundf(val("outHumidity"));
  weatherWindSpeed = val("windSpeed_kph");
  weatherPressure  = (uint16_t)roundf(val("barometer_mbar"));
  weatherAvailable = true;
  lastMqttWeather  = millis();
  __sync_synchronize();  // alle Wetterwerte vor clockColorChanged auf Core 1 sichtbar
  clockColorChanged = true;
}

void mqttConnect() {
  if (mqttServer.length() == 0) return;
  if (!wifiActive || WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connect("ZeDMD_wx")) {
    mqttClient.subscribe("weather/loop");
    logMsg("MQTT: verbunden");
  } else {
    logMsg("MQTT: Verbindung fehlgeschlagen (rc=%d)", mqttClient.state());
  }
}

void mqttTask(void* pvParameters) {
  for (;;) {
    if (wifiActive && WiFi.status() == WL_CONNECTED) {
      if (mqttClient.connected()) {
        mqttClient.loop();
      } else {
        uint32_t now = millis();
        if (now - lastMqttReconnect > 5000) {
          lastMqttReconnect = now;
          mqttConnect();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
#endif
uint8_t weatherPage = 0;  // 0=Uhr+Wetter, 1=Vorhersage, 2=Screensaver
#ifndef SD_MMC_BUILD
SPIClass spiSD(HSPI);  // Global — darf nicht lokal sein! (SPI-SD-Builds only)
#endif
String screensaverPaths = "";      // Kommagetrennte Liste gewählter Pfade (leer = LittleFS)
String screensaverFavorites = "";  // Newline-getrennte Favoriten-Pfade
volatile bool forcePlayPending = false;
String forcePlayFile = "";
String currentlyPlayingFile = "";  // aktuell gespieltes GIF (auch Force-Play)
String screensaverIgnore = "";     // Newline-getrennte Ignore-Pfade
volatile bool cancelSdScan = false;
String lastUploadFolder = "";      // Zielordner des letzten SD-Uploads (für gezielten Cache-Invalidierung)

// AnimatedGIF
AnimatedGIF gif;
File gifFile;

// Forward Declarations
void LoadIcons();
const uint8_t* GetSmallIcon(const char* name);
void LoadScreensaverFiles();
void InitSDCard();
void SaveScreensaverPaths();
void LoadScreensaverPaths();
void addScreensaverFile(const String& path);
void SaveScreensaverCache();
bool TryLoadScreensaverCache();
void InvalidateAllFolderCaches();
void InvalidateFolderCache(const String& path);
static void psramCacheSet(char** ptr, const String& json);
void SaveGifAudioCache();
bool TryLoadGifAudioCache();
void InvalidateGifAudioCache();
String folderCacheKey(const String& path);
uint16_t TryLoadFolderCache(const String& sdPath);
void SaveFolderCache(const String& sdPath, uint16_t fromIndex, uint16_t count);
void sortScreensaverFiles();
void shuffleScreensaverFiles();
bool isFavorite(const char* path);
void toggleFavorite(const char* path);
void LoadFavorites();
bool isIgnored(const char* path);
void toggleIgnore(const char* path);
void LoadIgnore();
String GetSDFolders();
void SaveScreensaverLum();
void SaveScreensaverDuration();
void SaveScreensaverMode();
void SaveScreensaverShuffle();
void SaveScreensaverStrictTimer();
void SaveGifAudioEnabled();
void LoadGifAudioEnabled();
void sendLittleFSHtml(AsyncWebServerRequest *request, const char* path);
void SaveWeatherConfig();
void LoadWeatherConfig();
void SaveTimezoneConfig();
void LoadTimezoneConfig();
#ifdef ZEDMD_WIFI
void SaveMqttConfig();
void LoadMqttConfig();
#endif
void LoadScreensaverMode();
void SaveClockColors();   // bleibt in main.cpp
void LoadClockColors();   // bleibt in main.cpp
void DisplayText(const char* text, bool scroll, uint8_t r, uint8_t g, uint8_t b, uint32_t durationMs);
void GIFDraw(GIFDRAW *pDraw);
#ifdef WEBRADIO_ENABLED
void DisplayRadio();
#endif

void DoRestart(int sec) {
  if (wifiActive) {
    MDNS.end();
    WiFi.disconnect(true);
  }
  display->ClearScreen();
  display->DisplayText("Restarting ...", 0, 0, 255, 0, 0);
  vTaskDelay(pdMS_TO_TICKS(sec * 1000));
  display->ClearScreen();
  delay(20);

  // Note: ESP.restart() or esp_restart() will keep the state of global and
  // static variables. And not all sub-systems get resetted.
#if (defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 1)
  esp_sleep_enable_timer_wakeup(1000);  // Wake up after 1ms
  esp_deep_sleep_start();  // Enter deep sleep (ESP32 reboots on wake)
#else
  esp_restart();
#endif
}

void Restart() { DoRestart(1); }

void RestartAfterError() { DoRestart(30); }

void DisplayNumber(uint32_t chf, uint8_t nc, uint16_t x, uint16_t y, uint8_t r,
                   uint8_t g, uint8_t b, bool transparent = false) {
  char text[16];
  sprintf(text, "%d", chf);

  uint8_t i = 0;
  if (strlen(text) < nc) {
    for (; i < (nc - strlen(text)); i++) {
      display->DisplayText(" ", x + (4 * i), y, r, g, b, transparent);
    }
  }

  display->DisplayText(text, x + (4 * i), y, r, g, b, transparent);
}

void DisplayVersion(bool logo = false) {
  // display the version number to the lower right
  char version[16];
  snprintf(version, sizeof(version), "%d.%d.%d%s", ZEDMD_VERSION_MAJOR, ZEDMD_VERSION_MINOR,
           ZEDMD_VERSION_PATCH, ZEDMD_VERSION_SUFFIX);
  display->DisplayText(version, TOTAL_WIDTH - (strlen(version) * 4) - 5,
                       TOTAL_HEIGHT - 5, 255 * !logo, 255 * !logo, 255 * !logo,
                       logo);
}

void DisplayLum(uint8_t r = 128, uint8_t g = 128, uint8_t b = 128) {
  display->DisplayText(" ", (TOTAL_WIDTH / 2) - 26 - 1, TOTAL_HEIGHT - 6, r, g,
                       b);
  display->DisplayText("Brightness:", (TOTAL_WIDTH / 2) - 26, TOTAL_HEIGHT - 6,
                       r, g, b);
  DisplayNumber(brightness, 2, (TOTAL_WIDTH / 2) + 18, TOTAL_HEIGHT - 6, 255,
                191, 0);
}

void DisplayRGB(uint8_t r = 128, uint8_t g = 128, uint8_t b = 128) {
#ifndef DISPLAY_RM67162_AMOLED
  display->DisplayText("red", 0, 0, 0, 0, 0, true, true);
  for (uint8_t i = 0; i < 6; i++) {
    display->DrawPixel(TOTAL_WIDTH - (4 * 4) - 1, i, 0, 0, 0);
    display->DrawPixel((TOTAL_WIDTH / 2) - (6 * 4) - 1, i, 0, 0, 0);
  }
  display->DisplayText("blue", TOTAL_WIDTH - (4 * 4), 0, 0, 0, 0, true, true);
  display->DisplayText("green", 0, TOTAL_HEIGHT - 6, 0, 0, 0, true, true);
  display->DisplayText("RGB Order:", (TOTAL_WIDTH / 2) - (6 * 4), 0, r, g, b);
  DisplayNumber(rgbMode, 2, (TOTAL_WIDTH / 2) + (4 * 4), 0, 255, 191, 0);
#endif
}

/// @brief Get DisplayDriver object, required for webserver
DisplayDriver *GetDisplayObject() { return display; }

void SaveSettingsMenu() {
  File f = LittleFS.open("/settings_menu.val", "w");
  if (!f) return;
  f.write(settingsMenu);
  f.close();
}

void LoadSettingsMenu() {
  File f = LittleFS.open("/settings_menu.val", "r");
  if (!f) {
#if !defined(DISPLAY_RM67162_AMOLED) && !defined(ZEDMD_WIFI)
    // Show settings menu on freshly installed device (not for WiFi builds)
    settingsMenu = 1;
#endif
    SaveSettingsMenu();
    return;
  }
  settingsMenu = f.read();
  f.close();
}

void SaveTransport() {
  File f = LittleFS.open("/transport.val", "w");
  if (!f) return;
  f.write(transport);
  f.close();
}

void LoadTransport() {
  File f = LittleFS.open("/transport.val", "r");
  if (!f) {
    SaveTransport();
    return;
  }
  transport = f.read();
  f.close();
}

#ifdef DISPLAY_LED_MATRIX
void SaveRgbOrder() {
  File f = LittleFS.open("/rgb_order.val", "w");
  if (!f) return;
  f.write(rgbMode);
  f.close();
}

void LoadRgbOrder() {
  File f = LittleFS.open("/rgb_order.val", "r");
  if (!f) {
    SaveRgbOrder();
    return;
  }
  rgbMode = rgbModeLoaded = f.read();
  f.close();
}

void SavePanelSettings() {
  File f = LittleFS.open("/panel_clkphase.val", "w");
  if (f) { f.write(panelClkphase); f.close(); }
  f = LittleFS.open("/panel_driver.val", "w");
  if (f) { f.write(panelDriver); f.close(); }
  f = LittleFS.open("/panel_i2sspeed.val", "w");
  if (f) { f.write(panelI2sspeed); f.close(); }
  f = LittleFS.open("/panel_latch_blanking.val", "w");
  if (f) { f.write(panelLatchBlanking); f.close(); }
  f = LittleFS.open("/panel_min_refresh_rate.val", "w");
  if (f) { f.write(panelMinRefreshRate); f.close(); }
}

void LoadPanelSettings() {
  File f = LittleFS.open("/panel_clkphase.val", "r");
  if (!f) {
    SavePanelSettings();
    return;
  }
  panelClkphase = f.read();
  f.close();
  f = LittleFS.open("/panel_driver.val", "r");
  if (!f) { return; }
  panelDriver = f.read();
  f.close();
  f = LittleFS.open("/panel_i2sspeed.val", "r");
  if (!f) { return; }
  panelI2sspeed = f.read();
  f.close();
  f = LittleFS.open("/panel_latch_blanking.val", "r");
  if (!f) { return; }
  panelLatchBlanking = f.read();
  f.close();
  f = LittleFS.open("/panel_min_refresh_rate.val", "r");
  if (!f) { return; }
  panelMinRefreshRate = f.read();
  f.close();
}

#endif

void SaveLum() {
  File f = LittleFS.open("/lum.val", "w");
  if (!f) return;
  f.write(brightness);
  f.close();
}

void LoadLum() {
  File f = LittleFS.open("/lum.val", "r");
  if (!f) {
    SaveLum();
    return;
  }
  brightness = f.read();
  f.close();
}

void SaveDebug() {
  File f = LittleFS.open("/debug.val", "w");
  if (!f) return;
  f.write(debug);
  f.close();
}

void LoadDebug() {
  File f = LittleFS.open("/debug.val", "r");
  if (!f) {
    SaveDebug();
    return;
  }
  debug = f.read();
  f.close();
}

void SaveUsbPackageSizeMultiplier() {
  File f = LittleFS.open("/usb_size.val", "w");
  if (!f) return;
  f.write(usbPackageSizeMultiplier);
  f.close();
}

void LoadUsbPackageSizeMultiplier() {
  File f = LittleFS.open("/usb_size.val", "r");
  if (!f) {
    SaveUsbPackageSizeMultiplier();
    return;
  }
  usbPackageSizeMultiplier = f.read();
  f.close();
}

void SaveUdpDelay() {
  File f = LittleFS.open("/udp_delay.val", "w");
  if (!f) return;
  f.write(udpDelay);
  f.close();
}

void LoadUdpDelay() {
  File f = LittleFS.open("/udp_delay.val", "r");
  if (!f) {
    SaveUdpDelay();
    return;
  }
  udpDelay = f.read();
  f.close();
}

#ifdef ZEDMD_HD_HALF
void SaveYOffset() {
  File f = LittleFS.open("/y_offset.val", "w");
  if (!f) return;
  f.write(yOffset);
  f.close();
}

void LoadYOffset() {
  File f = LittleFS.open("/y_offset.val", "r");
  if (!f) {
    SaveYOffset();
    return;
  }
  yOffset = f.read();
  f.close();
}
#endif

void SaveScale() {
  File f = LittleFS.open("/scale.val", "w");
  if (!f) return;
  f.write(display->GetCurrentScalingMode());
  f.close();
}

void LoadScale() {
  File f = LittleFS.open("/scale.val", "r");
  if (!f) {
    SaveScale();
    return;
  }
  display->SetCurrentScalingMode(f.read());
  f.close();
}

bool LoadWiFiConfig() {
  File wifiConfig = LittleFS.open("/wifi_config.txt", "r");
  if (!wifiConfig) return false;

  while (wifiConfig.available()) {
    ssid = wifiConfig.readStringUntil('\n');
    ssid_length = wifiConfig.readStringUntil('\n').toInt();
    pwd = wifiConfig.readStringUntil('\n');
    pwd_length = wifiConfig.readStringUntil('\n').toInt();
    port = wifiConfig.readStringUntil('\n').toInt();
  }
  wifiConfig.close();
  return true;
}

bool SaveWiFiConfig() {
  File wifiConfig = LittleFS.open("/wifi_config.txt", "w");
  if (!wifiConfig) return false;

  wifiConfig.println(ssid);
  wifiConfig.println(String(ssid_length));
  wifiConfig.println(pwd);
  wifiConfig.println(String(pwd_length));
  wifiConfig.println(String(port));
  wifiConfig.close();
  return true;
}

void LedTester(void) {
  display->FillScreen(255, 0, 0);
  delay(LED_CHECK_DELAY);

  display->FillScreen(0, 255, 0);
  delay(LED_CHECK_DELAY);

  display->FillScreen(0, 0, 255);
  delay(LED_CHECK_DELAY);

  display->ClearScreen();
}

void AcquireNextBuffer() {
  // currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
  // return;
  while (1) {
    if (currentBuffer == lastBuffer &&
        ((currentBuffer + 1) % NUM_BUFFERS) != processingBuffer) {
      currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
      return;
    }
    // Avoid busy-waiting
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void CheckMenuButton() {
#ifndef DISPLAY_RM67162_AMOLED
  if (!digitalRead(FORWARD_BUTTON_PIN)) {
    settingsMenu = true;
    SaveSettingsMenu();
    delay(20);
    Restart();
  }
#endif
}

void MarkCurrentBufferDone() { lastBuffer = currentBuffer; }

bool AcquireNextProcessingBuffer() {
  if (processingBuffer != currentBuffer &&
      (((processingBuffer + 1) % NUM_BUFFERS) != currentBuffer ||
       currentBuffer == lastBuffer)) {
    processingBuffer = (processingBuffer + 1) % NUM_BUFFERS;
    return true;
  }
  return false;
}

void Render() {
  if (NUM_RENDER_BUFFERS == 1) {
    display->FillPanelRaw(renderBuffer[currentRenderBuffer]);
  } else if (currentRenderBuffer != lastRenderBuffer) {
    uint16_t pos;

    for (uint16_t y = 0; y < TOTAL_HEIGHT; y++) {
      for (uint16_t x = 0; x < TOTAL_WIDTH; x++) {
        pos = (y * TOTAL_WIDTH + x) * 3;
        if (!(0 == memcmp(&renderBuffer[currentRenderBuffer][pos],
                          &renderBuffer[lastRenderBuffer][pos], 3))) {
          display->DrawPixel(x, y, renderBuffer[currentRenderBuffer][pos],
                             renderBuffer[currentRenderBuffer][pos + 1],
                             renderBuffer[currentRenderBuffer][pos + 2]);
        }
      }
    }

    lastRenderBuffer = currentRenderBuffer;
    currentRenderBuffer = (currentRenderBuffer + 1) % NUM_RENDER_BUFFERS;
    memcpy(renderBuffer[currentRenderBuffer], renderBuffer[lastRenderBuffer],
           TOTAL_BYTES);
  }
}

void ClearScreen() {
  display->ClearScreen();
  memset(renderBuffer[currentRenderBuffer], 0, TOTAL_BYTES);

  if (NUM_RENDER_BUFFERS > 1) {
    lastRenderBuffer = currentRenderBuffer;
    currentRenderBuffer = (currentRenderBuffer + 1) % NUM_RENDER_BUFFERS;
  }
}

void DisplayLogo(void) {
  File f;

  if (TOTAL_HEIGHT == 64) {
    f = LittleFS.open("/logoHD.raw", "r");
  } else {
    f = LittleFS.open("/logo.raw", "r");
  }

  if (!f) {
    display->DisplayText("Logo is missing", 0, 0, 255, 0, 0);
    return;
  }
#ifndef DISPLAY_RM67162_AMOLED
  uint8_t px[3];
  for (uint16_t tj = 0; tj < TOTAL_BYTES; tj += 3) {
    f.read(px, 3);
    if (rgbMode == rgbModeLoaded) {
      renderBuffer[currentRenderBuffer][tj]     = px[0];
      renderBuffer[currentRenderBuffer][tj + 1] = px[1];
      renderBuffer[currentRenderBuffer][tj + 2] = px[2];
    } else {
      renderBuffer[currentRenderBuffer][tj + rgbOrder[rgbMode * 3]]     = px[0];
      renderBuffer[currentRenderBuffer][tj + rgbOrder[rgbMode * 3 + 1]] = px[1];
      renderBuffer[currentRenderBuffer][tj + rgbOrder[rgbMode * 3 + 2]] = px[2];
    }
  }
#else
  f.read(renderBuffer[currentRenderBuffer], TOTAL_BYTES);
#endif
  f.close();

  Render();
  // DisplayVersion(true);

  throbberColors[0] = 0;
  throbberColors[1] = 0;
  throbberColors[2] = 0;
  throbberColors[3] = 255;
  throbberColors[4] = 255;
  throbberColors[5] = 255;

  logoActive = true;
  logoWaitCounter = 0;
}

void DisplayId() {
  char id[5];
  sprintf(id, "%04X", shortId);
  display->DisplayText(id, TOTAL_WIDTH - 16, 0, 0, 0, 0, 1);
}

void DisplayUpdate() {
  File f;

  if (TOTAL_HEIGHT == 64) {
    f = LittleFS.open("/ppucHD.raw", "r");
  } else {
    f = LittleFS.open("/ppuc.raw", "r");
  }

  if (!f) {
    return;
  }

  // Bulk-Read statt byte-by-byte
  f.read(renderBuffer[currentRenderBuffer], TOTAL_BYTES);
  f.close();

  Render();

  // DisplayId();

  throbberColors[0] = 0;
  throbberColors[1] = 0;
  throbberColors[2] = 0;
  throbberColors[3] = 255;
  throbberColors[4] = 255;
  throbberColors[5] = 0;
}

// ─────────────────────────────────────────────
// AnimatedGIF Callbacks
// ─────────────────────────────────────────────

#define GIF_READ_AHEAD_SIZE 4096
#define GIF_AUDIO_DIR "/GifAudio"  // SD-Ordner für GIF-Begleit-MP3s
static uint8_t* gifReadAheadBuf = nullptr;  // alloziert in PSRAM (lazy, GIFOpenFile)
static int32_t  gifReadAheadStart = 0;
static int32_t  gifReadAheadLen   = 0;
static bool     gifIsSD           = false;

void * GIFOpenFile(const char *fname, int32_t *pSize) {
  String path = String(fname);
  if (path.startsWith("SD:")) {
    gifFile = SD.open(path.substring(3), "r");
    gifIsSD = true;
  } else if (path.startsWith("FS:")) {
    gifFile = LittleFS.open(path.substring(3), "r");
    gifIsSD = false;
  } else {
    gifFile = LittleFS.open(fname, "r");
    gifIsSD = false;
  }
  if (!gifReadAheadBuf) {
    gifReadAheadBuf = (uint8_t*)heap_caps_malloc(GIF_READ_AHEAD_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  gifReadAheadStart = 0;
  gifReadAheadLen   = 0;
  if (gifFile) {
    *pSize = gifFile.size();
    return (void *)&gifFile;
  }
  return NULL;
}

void GIFCloseFile(void *pHandle) {
  File *f = static_cast<File *>(pHandle);
  if (f) f->close();
}

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = static_cast<File *>(pFile->fHandle);
  if (!gifIsSD) {
    int32_t iBytesRead = f->read(pBuf, iLen);
    pFile->iPos = f->position();
    return iBytesRead;
  }
  int32_t bytesServed = 0;
  while (bytesServed < iLen) {
    int32_t curPos = pFile->iPos + bytesServed;
    if (gifReadAheadLen > 0 &&
        curPos >= gifReadAheadStart &&
        curPos < gifReadAheadStart + gifReadAheadLen) {
      int32_t bufOffset = curPos - gifReadAheadStart;
      int32_t available = gifReadAheadLen - bufOffset;
      int32_t toCopy    = min((int32_t)(iLen - bytesServed), available);
      memcpy(pBuf + bytesServed, gifReadAheadBuf + bufOffset, toCopy);
      bytesServed += toCopy;
    } else {
      f->seek(curPos);
      gifReadAheadStart = curPos;
      gifReadAheadLen   = f->read(gifReadAheadBuf, GIF_READ_AHEAD_SIZE);
      if (gifReadAheadLen <= 0) break;
    }
  }
  pFile->iPos += bytesServed;
  return bytesServed;
}

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  File *f = static_cast<File *>(pFile->fHandle);
  if (gifIsSD &&
      gifReadAheadLen > 0 &&
      iPosition >= gifReadAheadStart &&
      iPosition < gifReadAheadStart + gifReadAheadLen) {
    pFile->iPos = iPosition;
    return iPosition;
  }
  f->seek(iPosition);
  pFile->iPos = f->position();
  gifReadAheadLen = 0;
  return pFile->iPos;
}

// GIFDraw Callback — schreibt jeden Frame in den renderBuffer
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *usPalette = pDraw->pPalette;
  int y = pDraw->iY + pDraw->y;

  if (y >= TOTAL_HEIGHT) return;

  s = pDraw->pPixels;

  if (pDraw->ucDisposalMethod == 2) {
    // Hintergrundfarbe wiederherstellen — ucBackground ist ein Palette-Index, kein RGB565
    uint16_t bgColor = usPalette[pDraw->ucBackground];
    for (int x = 0; x < pDraw->iWidth; x++) {
      uint32_t pos = (y * TOTAL_WIDTH + pDraw->iX + x) * 3;
      if (pos + 2 < TOTAL_BYTES) {
        renderBuffer[currentRenderBuffer][pos]     = (bgColor >> 8) & 0xf8;
        renderBuffer[currentRenderBuffer][pos + 1] = (bgColor >> 3) & 0xfc;
        renderBuffer[currentRenderBuffer][pos + 2] = (bgColor << 3);
      }
    }
    s = pDraw->pPixels;
  }

  // Transparenz prüfen
  if (pDraw->ucHasTransparency) {
    uint8_t ucTransparent = pDraw->ucTransparent;
    for (int x = 0; x < pDraw->iWidth; x++) {
      uint8_t c = *s++;
      if (c == ucTransparent) continue;
      uint16_t color565 = usPalette[c];
      uint32_t pos = (y * TOTAL_WIDTH + pDraw->iX + x) * 3;
      if (pos + 2 < TOTAL_BYTES) {
        renderBuffer[currentRenderBuffer][pos]     = (color565 >> 8) & 0xf8;
        renderBuffer[currentRenderBuffer][pos + 1] = (color565 >> 3) & 0xfc;
        renderBuffer[currentRenderBuffer][pos + 2] = (color565 << 3);
      }
    }
  } else {
    for (int x = 0; x < pDraw->iWidth; x++) {
      uint16_t color565 = usPalette[*s++];
      uint32_t pos = (y * TOTAL_WIDTH + pDraw->iX + x) * 3;
      if (pos + 2 < TOTAL_BYTES) {
        renderBuffer[currentRenderBuffer][pos]     = (color565 >> 8) & 0xf8;
        renderBuffer[currentRenderBuffer][pos + 1] = (color565 >> 3) & 0xfc;
        renderBuffer[currentRenderBuffer][pos + 2] = (color565 << 3);
      }
    }
  }

  // Nach letzter Zeile rendern
  if (pDraw->y == pDraw->iHeight - 1) {
    Render();
  }
}

// GIF abspielen — loopt intern bis endTime, um Freeze zwischen den Loops zu vermeiden
bool PlayGIF(const String &path, uint32_t endTime = 0, bool clearFirst = true, bool loopUntilEnd = true) {
  if (!path.endsWith(".gif") && !path.endsWith(".GIF")) return false;

#ifdef WEBRADIO_ENABLED
  bool gifAudioActive = false;
  if (gifAudioEnabled && path.startsWith("SD:") && !radioIsPlaying && !radioUserActive) {
    // Dateiname aus GIF-Pfad extrahieren, MP3 in /GifAudio/ suchen:
    // 1) Exakter Match (base.mp3 / base.MP3)
    // 2) Fuzzy: erstes MP3 dessen Stem im GIF-Namen enthalten ist (case-insensitiv)
    String gifName = path.substring(path.lastIndexOf('/') + 1);
    int dotIdx = gifName.lastIndexOf('.');
    if (dotIdx >= 0) {
      String base     = gifName.substring(0, dotIdx);
      String mp3Path  = "";
      String exact    = String(GIF_AUDIO_DIR) + "/" + base + ".mp3";
      if      (SD.exists(exact.c_str()))                                 mp3Path = exact;
      else { exact = String(GIF_AUDIO_DIR) + "/" + base + ".MP3";
             if (SD.exists(exact.c_str()))                               mp3Path = exact; }
      if (mp3Path.isEmpty() && cachedGifAudioFiles && strlen(cachedGifAudioFiles) > 2) {
        // Fuzzy in RAM: cachedGifAudioFiles JSON nach passendem MP3-Stem durchsuchen
        String baseLow = base; baseLow.toLowerCase();
        const char* p = cachedGifAudioFiles;
        while ((p = strstr(p, "\"name\":\"")) != nullptr) {
          p += 8;
          const char* endQ = strchr(p, '"');
          if (!endQ) break;
          int flen = endQ - p;
          char fnameBuf[256];
          if (flen >= (int)sizeof(fnameBuf)) { p = endQ; continue; }
          strncpy(fnameBuf, p, flen); fnameBuf[flen] = '\0';
          String fname = fnameBuf;
          String fLow  = fname; fLow.toLowerCase();
          if (fLow.endsWith(".mp3")) {
            String stemLow = fLow.substring(0, fLow.lastIndexOf('.'));
            if (stemLow.length() > 0 && baseLow.indexOf(stemLow) >= 0) {
              mp3Path = String(GIF_AUDIO_DIR) + "/" + fname; break;
            }
          }
          p = endQ;
        }
      }
      if (!mp3Path.isEmpty()) {
        radioPlayLocalFile(mp3Path.c_str());
        gifAudioActive = true;
      }
    }
  }
#endif

  gif.begin(LITTLE_ENDIAN_PIXELS);
#ifdef BOARD_HAS_PSRAM
  gif.setDrawType(GIF_DRAW_COOKED);
#endif
  if (!gif.open(path.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
#ifdef WEBRADIO_ENABLED
    if (gifAudioActive) { radioStopLocalFile(); gifAudioActive = false; }
#endif
    return false;
  }

  if (clearFirst) {
    display->ClearScreen();
    memset(renderBuffer[currentRenderBuffer], 0, TOTAL_BYTES);
    if (NUM_RENDER_BUFFERS > 1)
      memset(renderBuffer[lastRenderBuffer], 0, TOTAL_BYTES);
  }

  bool firstLoop = true;
  do {
    // gif.reset() statt close()+open(): nur Seek auf 0 + Header (~2 ms statt 15–50 ms SD-open)
    if (!firstLoop) gif.reset();
    firstLoop = false;

    int frameDelay = 0;
    uint32_t frameStart = millis();
    while (gif.playFrame(false, &frameDelay) && !transportActive && !screensaverReloadNeeded
           && !forcePlayPending && !displayTextActive
           && (endTime == 0 || millis() < endTime)) {
      if (frameDelay > 0) {
        uint32_t elapsed   = millis() - frameStart;
        uint32_t remaining = (frameDelay > (int)elapsed) ? ((uint32_t)frameDelay - elapsed) : 0;
        uint32_t waitUntil = millis() + remaining;
        while (millis() < waitUntil && !transportActive && !forcePlayPending && !displayTextActive && (endTime == 0 || millis() < endTime)) {
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      }
      frameStart = millis();
      yield();
      esp_task_wdt_reset();
    }
    // Letzter Frame: volle Anzeigedauer abwarten bevor dem nächsten Loop
    if (frameDelay > 0 && !transportActive && !screensaverReloadNeeded && !forcePlayPending && !displayTextActive && (endTime == 0 || millis() < endTime)) {
      uint32_t elapsed   = millis() - frameStart;
      uint32_t remaining = (frameDelay > (int)elapsed) ? ((uint32_t)frameDelay - elapsed) : 0;
      uint32_t waitUntil = millis() + remaining;
      while (millis() < waitUntil && !transportActive && !forcePlayPending && !displayTextActive && (endTime == 0 || millis() < endTime)) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
  } while (loopUntilEnd && !transportActive && !screensaverReloadNeeded && !forcePlayPending && !displayTextActive && (endTime == 0 || millis() < endTime));

  gif.close();

#ifdef WEBRADIO_ENABLED
  if (gifAudioActive) radioStopLocalFile();
#endif

  return true;
}

// ─────────────────────────────────────────────

void ScreenSaver() {
  display->SetBrightness(screensaverBrightness);

  if (screensaverCount > 0) {
    String path = String(screensaverFiles[screensaverIndex]);

    // Nur RAW laden — GIF wird in loop() via PlayGIF abgespielt
    if (!path.endsWith(".gif") && !path.endsWith(".GIF")) {
      display->ClearScreen();  // Reste vom vorherigen Bild löschen
      File f;
      if (path.startsWith("SD:")) {
        f = SD.open(path.substring(3), "r");
      } else if (path.startsWith("FS:")) {
        f = LittleFS.open(path.substring(3), "r");
      } else {
        f = LittleFS.open(path, "r");
      }
      if (f) {
        f.read(renderBuffer[currentRenderBuffer], TOTAL_BYTES);
        f.close();
        Render();
      } else {
        ClearScreen();
      }
    }
    // GIF → loop() kümmert sich darum
  } else {
    // Fallback → logo.raw
    File f;
    if (TOTAL_HEIGHT == 64) {
      f = LittleFS.open("/logoHD.raw", "r");
    } else {
      f = LittleFS.open("/logo.raw", "r");
    }
    if (f) {
      // Bulk-Read statt byte-by-byte
      f.read(renderBuffer[currentRenderBuffer], TOTAL_BYTES);
      f.close();
      Render();
    } else {
      ClearScreen();
    }
  }

  throbberColors[0] = 48;
  throbberColors[1] = 0;
  throbberColors[2] = 0;
  throbberColors[3] = 0;
  throbberColors[4] = 0;
  throbberColors[5] = 0;
}

void RefreshSetupScreen() {
  DisplayLogo();
  for (uint16_t y = (TOTAL_HEIGHT / 32 * 5);
       y < TOTAL_HEIGHT - (TOTAL_HEIGHT / 32 * 5); y++) {
    for (uint16_t x = (TOTAL_WIDTH / 128 * 5);
         x < TOTAL_WIDTH - (TOTAL_WIDTH / 128 * 5); x++) {
      display->DrawPixel(x, y, 0, 0, 0);
    }
  }
  DisplayRGB();
  DisplayLum();
  display->DisplayText(
      transport == TRANSPORT_USB
          ? "USB "
          : (transport == TRANSPORT_WIFI_UDP
                 ? "WiFi UDP"
                 : (transport == TRANSPORT_WIFI_TCP ? "WiFi TCP" : "SPI ")),
      7 * (TOTAL_WIDTH / 128), (TOTAL_HEIGHT / 2) - 3, 128, 128, 128);
  display->DisplayText("Debug:", 7 * (TOTAL_WIDTH / 128),
                       (TOTAL_HEIGHT / 2) - 10, 128, 128, 128);
  DisplayNumber(debug, 1, 7 * (TOTAL_WIDTH / 128) + (6 * 4),
                (TOTAL_HEIGHT / 2) - 10, 255, 191, 0);
  display->DisplayText("USB Packet Size:", 7 * (TOTAL_WIDTH / 128),
                       (TOTAL_HEIGHT / 2) + 4, 128, 128, 128);
  DisplayNumber(usbPackageSizeMultiplier * 32, 4,
                7 * (TOTAL_WIDTH / 128) + (16 * 4), (TOTAL_HEIGHT / 2) + 4, 255,
                191, 0);
  display->DisplayText(
      "UDP Delay:", TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - (11 * 4),
      (TOTAL_HEIGHT / 2) - 3, 128, 128, 128);
  DisplayNumber(udpDelay, 1, TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 4,
                (TOTAL_HEIGHT / 2) - 3, 255, 191, 0);

#ifdef ZEDMD_HD_HALF
  display->DisplayText("Y-Offset", TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 32,
                       (TOTAL_HEIGHT / 2) - 10, 128, 128, 128);
#endif
  display->DisplayText("Exit", TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 16,
                       (TOTAL_HEIGHT / 2) + 4, 128, 128, 128);
}

static uint8_t IRAM_ATTR HandleData(uint8_t *pData, size_t len) {
  uint16_t pos = 0;
  bool headerCompleted = false;

  while (pos < len ||
         (headerCompleted && command != 5 && command != 22 && command != 23 &&
          command != 27 && command != 28 && command != 29 && command != 40 &&
          command != 41 && command != 42 && command != 43 && command != 44 &&
          command != 45 && command != 46 && command != 47 && command != 48)) {
    headerCompleted = false;
    if (numCtrlCharsFound < N_CTRL_CHARS) {
      // Detect 5 consecutive start bits
      if (pData[pos++] == CtrlChars[numCtrlCharsFound]) {
        numCtrlCharsFound++;
      } else {
        numCtrlCharsFound = 0;
      }
    } else if (numCtrlCharsFound == N_CTRL_CHARS) {
      if (headerBytesReceived == 0) {
        command = pData[pos++];
        ++headerBytesReceived;
        continue;
      } else if (headerBytesReceived == 1) {
        payloadSize = pData[pos++] << 8;
        ++headerBytesReceived;
        continue;
      } else if (headerBytesReceived == 2) {
        payloadSize |= pData[pos++];
        payloadMissing = payloadSize;
        ++headerBytesReceived;
        continue;
      } else if (headerBytesReceived == 3) {
        payloadCompressed = (bool)pData[pos++];
        ++headerBytesReceived;
        headerCompleted = true;
        continue;
      } else if (headerBytesReceived == 4) {
        esp_task_wdt_reset();
        if (payloadSize > BUFFER_SIZE) {
          if (debug) {
            display->DisplayText("Error, payloadSize > BUFFER_SIZE", 0, 0, 255,
                                 0, 0);
            DisplayNumber(payloadSize, 5, 0, 19, 255, 0, 0);
            DisplayNumber(BUFFER_SIZE, 5, 0, 25, 255, 0, 0);
            while (1);
          }
          headerBytesReceived = 0;
          numCtrlCharsFound = 0;
          return 2;
        }

        if (debug) {
          display->DisplayText("Command:", 7 * (TOTAL_WIDTH / 128),
                               (TOTAL_HEIGHT / 2) - 10, 128, 128, 128);
          DisplayNumber(command, 2, 7 * (TOTAL_WIDTH / 128) + (8 * 4),
                        (TOTAL_HEIGHT / 2) - 10, 255, 191, 0);
          display->DisplayText("Payload:", 7 * (TOTAL_WIDTH / 128),
                               (TOTAL_HEIGHT / 2) - 4, 128, 128, 128);
          DisplayNumber(payloadSize, 2, 7 * (TOTAL_WIDTH / 128) + (8 * 4),
                        (TOTAL_HEIGHT / 2) - 4, 255, 191, 0);
        }

        switch (command) {
          case 12:  // handshake
          {
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;

            // Including the ACK, the response will be 64 bytes long. That
            // leaves some space for future features.
            uint8_t response[64 - N_ACK_CHARS] = {};  // Stack reicht für 59 Bytes
            memcpy(response, CtrlChars, N_INTERMEDIATE_CTR_CHARS);
            response[N_INTERMEDIATE_CTR_CHARS] = TOTAL_WIDTH & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 1] = (TOTAL_WIDTH >> 8) & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 2] = TOTAL_HEIGHT & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 3] = (TOTAL_HEIGHT >> 8) & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 4] = ZEDMD_VERSION_MAJOR;
            response[N_INTERMEDIATE_CTR_CHARS + 5] = ZEDMD_VERSION_MINOR;
            response[N_INTERMEDIATE_CTR_CHARS + 6] = ZEDMD_VERSION_PATCH;
            response[N_INTERMEDIATE_CTR_CHARS + 7] =
                (usbPackageSizeMultiplier * 32) & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 8] =
                ((usbPackageSizeMultiplier * 32) >> 8) & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 9] = brightness;
#ifndef DISPLAY_RM67162_AMOLED
            response[N_INTERMEDIATE_CTR_CHARS + 10] = rgbMode;
            response[N_INTERMEDIATE_CTR_CHARS + 11] = yOffset;
            response[N_INTERMEDIATE_CTR_CHARS + 12] = panelClkphase;
            response[N_INTERMEDIATE_CTR_CHARS + 13] = panelDriver;
            response[N_INTERMEDIATE_CTR_CHARS + 14] = panelI2sspeed;
            response[N_INTERMEDIATE_CTR_CHARS + 15] = panelLatchBlanking;
            response[N_INTERMEDIATE_CTR_CHARS + 16] = panelMinRefreshRate;
#endif
            response[N_INTERMEDIATE_CTR_CHARS + 17] = udpDelay;
#ifdef ZEDMD_HD_HALF
            response[N_INTERMEDIATE_CTR_CHARS + 18] = 1;
#else
            response[N_INTERMEDIATE_CTR_CHARS + 18] = 0;
#endif
#if defined(ARDUINO_ESP32_S3_N16R8) || defined(DISPLAY_RM67162_AMOLED)
            response[N_INTERMEDIATE_CTR_CHARS + 18] += 0b00000010;
#endif
            response[N_INTERMEDIATE_CTR_CHARS + 19] = shortId & 0xff;
            response[N_INTERMEDIATE_CTR_CHARS + 20] = (shortId >> 8) & 0xff;
            response[63 - N_ACK_CHARS] = 'R';
            Serial.write(response, 64 - N_ACK_CHARS);
            // This flush is required for USB CDC on Windows.
            Serial.flush();
            return 1;
          }

          case 22:  // set brightness
          {
            brightness = pData[pos++];
            display->SetBrightness(brightness);
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }
#ifndef DISPLAY_RM67162_AMOLED
          case 23:  // set RGB order
          {
            rgbMode = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }
#endif
          case 27:  // set SSID
          {
            if (payloadSize > 32) {
              headerBytesReceived = 0;
              numCtrlCharsFound = 0;
              if (wifiActive) break;
              return 1;
            }
            if (payloadMissing == payloadSize) {
              memset(tmpStringBuffer, 0, 33);
              if (payloadMissing > (len - pos)) {
                memcpy(tmpStringBuffer, &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(tmpStringBuffer, &pData[pos], payloadSize);
                ssid = String(tmpStringBuffer);
                ssid_length = payloadSize;
                pos += payloadSize;
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            } else {
              if (payloadMissing > (len - pos)) {
                memcpy(&tmpStringBuffer[payloadSize - payloadMissing],
                       &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(&tmpStringBuffer[payloadSize - payloadMissing],
                       &pData[pos], payloadMissing);
                ssid = String(tmpStringBuffer);
                ssid_length = payloadSize;
                pos += payloadMissing;
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            }
            if (wifiActive) break;
            return 1;
          }

          case 28:  // set password
          {
            if (payloadSize > 32) {
              headerBytesReceived = 0;
              numCtrlCharsFound = 0;
              if (wifiActive) break;
              return 1;
            }
            if (payloadMissing == payloadSize) {
              memset(tmpStringBuffer, 0, 33);
              if (payloadMissing > (len - pos)) {
                memcpy(tmpStringBuffer, &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(tmpStringBuffer, &pData[pos], payloadSize);
                pwd = String(tmpStringBuffer);
                pwd_length = payloadSize;
                pos += payloadSize;
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            } else {
              if (payloadMissing > (len - pos)) {
                memcpy(&tmpStringBuffer[payloadSize - payloadMissing],
                       &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(&tmpStringBuffer[payloadSize - payloadMissing],
                       &pData[pos], payloadMissing);
                pwd = String(tmpStringBuffer);
                pwd_length = payloadSize;
                pos += payloadMissing;
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            }
            if (wifiActive) break;
            return 1;
          }

          case 29:  // set port
          {
            if (payloadSize > 32) {
              headerBytesReceived = 0;
              numCtrlCharsFound = 0;
              if (wifiActive) break;
              return 1;
            }
            if (payloadMissing == payloadSize) {
              memset(tmpStringBuffer, 0, 33);
              if (payloadMissing > (len - pos)) {
                memcpy(tmpStringBuffer, &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(tmpStringBuffer, &pData[pos], payloadSize);
                port = tmpStringBuffer[0] << 8;
                port |= tmpStringBuffer[1];
                pos += payloadSize;
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            } else {
              if (payloadMissing > (len - pos)) {
                memcpy(&tmpStringBuffer[payloadSize - payloadMissing],
                       &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(&tmpStringBuffer[payloadSize - payloadMissing],
                       &pData[pos], payloadMissing);
                port = tmpStringBuffer[0] << 8;
                port |= tmpStringBuffer[1];
                pos += payloadMissing;
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            }
            if (wifiActive) break;
            return 1;
          }

          case 30:  // save settings 0x1e
          {
            if (!wifiActive) {
              // send fast ack
              Serial.write(CtrlChars, N_ACK_CHARS);
              Serial.flush();
            }
            display->DisplayText("Saving settings ...", 0, 0, 255, 0, 0);
            SaveLum();
            SaveDebug();
            SaveTransport();
            SaveUsbPackageSizeMultiplier();
            SaveUdpDelay();
            SaveWiFiConfig();
#ifdef DISPLAY_LED_MATRIX
            SaveRgbOrder();
            SavePanelSettings();
#endif
#ifdef ZEDMD_HD_HALF
            SaveYOffset();
#endif
            display->DisplayText("Saving settings ... done", 0, 0, 255, 0, 0);
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 3;
          }

          case 31:  // reset 0x1f
          {
            if (!wifiActive) {
              Serial.write(CtrlChars, N_ACK_CHARS);
              Serial.flush();
            }
            Restart();
          }
#ifndef DISPLAY_RM67162_AMOLED
          case 40:  // set panelClkphase
          {
            panelClkphase = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 41:  // set panelI2sspeed
          {
            panelI2sspeed = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 42:  // set panelLatchBlanking
          {
            panelLatchBlanking = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 43:  // set panelMinRefreshRate
          {
            panelMinRefreshRate = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 44:  // set panelDriver
          {
            panelDriver = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }
#endif
          case 45:  // set transport
          {
            transport = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 46:  // set udpDelay
          {
            udpDelay = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 47:  // set usbPackageSizeMultiplier
          {
            usbPackageSizeMultiplier = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }
#ifndef DISPLAY_RM67162_AMOLED
          case 48:  // set yOffset
          {
            yOffset = pData[pos++];
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }
#endif
          case 16: {
            if (!wifiActive) {
              Serial.write(CtrlChars, N_ACK_CHARS);
              Serial.flush();
            }
            LedTester();
            Restart();
          }

          case 10: {  // Clear screen
            AcquireNextBuffer();
            bufferCompressed[currentBuffer] = false;
            bufferSizes[currentBuffer] = 2;
            buffers[currentBuffer][0] = 0;
            buffers[currentBuffer][1] = 0;
            MarkCurrentBufferDone();
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 11:  // KeepAlive
          {
            if (debug) {
              display->DisplayText("KEEP ALIVE RECEIVED",
                                   7 * (TOTAL_WIDTH / 128),
                                   (TOTAL_HEIGHT / 2) - 10, 128, 128, 128);
            }
            lastDataReceived = millis();
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 98:  // disable debug mode
          {
            debug = 0;
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 99:  // enable debug mode
          {
            debug = 1;
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          case 5: {  // RGB565 Zones Stream
            if (payloadMissing == payloadSize) {
              AcquireNextBuffer();
              bufferCompressed[currentBuffer] = payloadCompressed;
              bufferSizes[currentBuffer] = payloadSize;
              if (payloadMissing > (len - pos)) {
                memcpy(&buffers[currentBuffer][0], &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(&buffers[currentBuffer][0], &pData[pos], payloadSize);
                pos += payloadSize;
                MarkCurrentBufferDone();
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            } else {
              if (payloadMissing > (len - pos)) {
                memcpy(&buffers[currentBuffer][payloadSize - payloadMissing],
                       &pData[pos], len - pos);
                payloadMissing -= len - pos;
                pos += len - pos;
                break;
              } else {
                memcpy(&buffers[currentBuffer][payloadSize - payloadMissing],
                       &pData[pos], payloadMissing);
                pos += payloadMissing;
                MarkCurrentBufferDone();
                payloadMissing = 0;
                headerBytesReceived = 0;
                numCtrlCharsFound = 0;
              }
            }
            break;
          }

          case 6: {  // Render
#if defined(BOARD_HAS_PSRAM) && (NUM_RENDER_BUFFERS > 1)
            AcquireNextBuffer();
            bufferCompressed[currentBuffer] = false;
            bufferSizes[currentBuffer] = 2;
            buffers[currentBuffer][0] = 255;
            buffers[currentBuffer][1] = 255;
            MarkCurrentBufferDone();
#endif
            lastDataReceived = millis();
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }

          default: {
            headerBytesReceived = 0;
            numCtrlCharsFound = 0;
            if (wifiActive) break;
            return 1;
          }
        }
      }
    }
  }

  return 0;
}

void Task_ReadSerial(void *pvParameters) {
  const uint16_t usbPackageSize = usbPackageSizeMultiplier * 32;
  bool connected = false;

  Serial.setRxBufferSize(usbPackageSize + 128);
  Serial.setTxBufferSize(64);
#if (defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 1)
  // S3 USB CDC. The actual baud rate doesn't matter.
  Serial.begin(115200);
  while (!Serial) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // display->DisplayText("USB CDC", 0, 0, 0, 0, 0, 1);
#else
  Serial.setTimeout(SERIAL_TIMEOUT);
  Serial.begin(SERIAL_BAUD);
  while (!Serial) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (1 == debug) {
    DisplayNumber(SERIAL_BAUD, (SERIAL_BAUD >= 1000000 ? 7 : 6), 0, 0, 0, 0, 0,
                  1);
  } else {
    // display->DisplayText("USB UART", 0, 0, 0, 0, 0, 1);
  }
#endif

#ifdef BOARD_HAS_PSRAM
  uint8_t *pUsbBuffer = (uint8_t *)heap_caps_malloc(
      usbPackageSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
#else
  uint8_t *pUsbBuffer = (uint8_t *)malloc(usbPackageSize);
#endif

  if (nullptr == pUsbBuffer) {
    display->DisplayText("out of memory", 0, 0, 255, 0, 0);
    while (1);
  }

  payloadMissing = 0;
  headerBytesReceived = 0;
  numCtrlCharsFound = 0;

  int16_t received = 0;
  int16_t expected = 0;
  uint16_t noDataMs = 0;
  uint8_t numFrameCharsFound = 0;
  uint8_t result = 0;

  while (1) {
    noDataMs = 0;
    numFrameCharsFound = 0;
    // Wait for FRAME header
    while (numFrameCharsFound < N_FRAME_CHARS) {
      if (Serial.available()) {
        if (Serial.read() == FrameChars[numFrameCharsFound]) {
          numFrameCharsFound++;
        } else {
          numFrameCharsFound = 0;
        }
      } else {
        if (++noDataMs > 5000) {
          transportActive = false;
          noDataMs = 0;
        }
        // Avoid busy-waiting
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }

    expected = usbPackageSize - N_FRAME_CHARS;
    transportActive = true;
    noDataMs = 0;
    result = 0;

    while (1) {
      // Wait for data to be ready
      if (Serial.available() >= expected ||
          (!connected && Serial.available() >= (N_CTRL_CHARS + 4))) {
        memset(pUsbBuffer, 0, usbPackageSize);
        received = Serial.readBytes(pUsbBuffer, expected);
        result = HandleData(pUsbBuffer, received);
        expected = usbPackageSize;
        if (2 == result) {  // Error
          Serial.write(CtrlChars, N_CTRL_CHARS);
          Serial.write('F');
          Serial.flush();
          vTaskDelay(pdMS_TO_TICKS(2));
          Serial.end();
          vTaskDelay(pdMS_TO_TICKS(2));
          Serial.begin(SERIAL_BAUD);
          while (!Serial) {
            vTaskDelay(pdMS_TO_TICKS(1));
          }
          break;  // Wait for the next FRAME header
        }
        connected = true;
        if (3 == result) {
          break;  // fast ack has been sent, wait for the next FRAME header
        }
        Serial.write(CtrlChars, N_ACK_CHARS);
        Serial.flush();
        if (1 == result) break;  // Wait for the next FRAME header
        noDataMs = 0;
      } else {
        if (++noDataMs > 5000) {
          transportActive = false;
          noDataMs = 0;
          break;  // Wait for the next FRAME header
        }
        // Avoid busy-waiting
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
  }
}

static void HandleUdpPacket(AsyncUDPPacket packet) {
  static bool isProcessing = false;
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

  portENTER_CRITICAL(&mux);
  bool alreadyProcessing = isProcessing;
  if (!alreadyProcessing) isProcessing = true;
  portEXIT_CRITICAL(&mux);

  if (!alreadyProcessing) {
    transportActive = true;
    HandleData(packet.data(), packet.length());
    yield();
    portENTER_CRITICAL(&mux);
    isProcessing = false;
    portEXIT_CRITICAL(&mux);
  }
}

static void HandleTcpData(void *arg, AsyncClient *client, void *data,
                          size_t len) {
  HandleData((uint8_t *)data, len);
  client->ack(len);
}

static void HandleTcpDisconnect(void *arg, AsyncClient *client) {
  delete client;
  MarkCurrentBufferDone();
  AcquireNextBuffer();
  bufferSizes[currentBuffer] = 2;
  buffers[currentBuffer][0] = 0;
  buffers[currentBuffer][1] = 0;
  MarkCurrentBufferDone();
  ClearScreen();
  payloadMissing = 0;
  headerBytesReceived = 0;
  numCtrlCharsFound = 0;
  transportActive = false;
}

static void NewTcpClient(void *arg, AsyncClient *client) {
  if (transportActive) {
    client->stop();
    delete client;
    return;
  }
  payloadMissing = 0;
  headerBytesReceived = 0;
  numCtrlCharsFound = 0;
  transportActive = true;
  client->setNoDelay(true);
  client->setAckTimeout(2);
  client->onData(&HandleTcpData, NULL);
  client->onDisconnect(&HandleTcpDisconnect, NULL);
}

void sendLittleFSHtml(AsyncWebServerRequest *request, const char* path) {
  if (ESP.getMaxAllocHeap() < 10240) {
    request->send(503, "text/plain", "Low memory");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) { request->send(404); return; }
  String etag = "\"" + String(f.size()) + "\"";
  f.close();
  if (request->hasHeader("If-None-Match") &&
      request->getHeader("If-None-Match")->value() == etag) {
    request->send(304);
    return;
  }
  AsyncWebServerResponse *resp = request->beginResponse(LittleFS, path, "text/html");
  if (!resp) { request->send(503, "text/plain", "Low memory"); return; }
  resp->addHeader("ETag", etag);
  resp->addHeader("Cache-Control", "no-cache");
  request->send(resp);
}

void StartServer() {
  server = new AsyncWebServer(80);

  // Serve index.html
  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendLittleFSHtml(request, "/index.html");
  });

  // Handle AJAX request to save WiFi configuration
  server->on("/save_wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ssid", true) &&
        request->hasParam("password", true) &&
        request->hasParam("port", true)) {
      ssid = request->getParam("ssid", true)->value();
      pwd = request->getParam("password", true)->value();
      port = request->getParam("port", true)->value().toInt();
      ssid_length = ssid.length();
      pwd_length = pwd.length();

      bool success = SaveWiFiConfig();
      if (success) {
        request->send(200, "text/plain", "Config saved successfully!");
        Restart();
      } else {
        request->send(500, "text/plain", "Failed to save config!");
      }
    } else {
      request->send(400, "text/plain", "Missing parameters!");
    }
  });

  server->on("/wifi_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String jsonResponse;
    if (WiFi.status() == WL_CONNECTED) {
      int rssi = WiFi.RSSI();
      IPAddress ip = WiFi.localIP();  // Get the local IP address

      jsonResponse = "{\"connected\":true,\"ssid\":\"" + WiFi.SSID() +
                     "\",\"signal\":" + String(rssi) + "," + "\"ip\":\"" +
                     ip.toString() + "\"," + "\"port\":" + String(port) + "}";
    } else {
      jsonResponse = "{\"connected\":false}";
    }

    request->send(200, "application/json", jsonResponse);
  });

#ifndef DISPLAY_RM67162_AMOLED
  // Route to save RGB order
  server->on("/save_rgb_order", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("rgbOrder", true)) {
      if (rgbModeLoaded != 0) {
        request->send(200, "text/plain",
                      "ZeDMD needs to reboot first before the RGB order can be "
                      "adjusted. Try again in a few seconds.");

        rgbMode = 0;
        SaveRgbOrder();
        Restart();
      }

      String rgbOrderValue = request->getParam("rgbOrder", true)->value();
      rgbMode =
          rgbOrderValue.toInt();  // Convert to integer and set the RGB mode
      SaveRgbOrder();
      RefreshSetupScreen();
      request->send(200, "text/plain", "RGB order updated successfully");
    } else {
      request->send(400, "text/plain", "Missing RGB order parameter");
    }
  });
#endif

  // Route to save brightness
  server->on("/save_brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("brightness", true)) {
      String brightnessValue = request->getParam("brightness", true)->value();
      brightness = brightnessValue.toInt();
      GetDisplayObject()->SetBrightness(brightness);
      SaveLum();
      RefreshSetupScreen();
      request->send(200, "text/plain", "Brightness updated successfully");
    } else {
      request->send(400, "text/plain", "Missing brightness parameter");
    }
  });

  server->on("/get_version", HTTP_GET, [](AsyncWebServerRequest *request) {
    String version = String(ZEDMD_VERSION_MAJOR) + "." +
                     String(ZEDMD_VERSION_MINOR) + "." +
                     String(ZEDMD_VERSION_PATCH) + ZEDMD_VERSION_SUFFIX +
                     " (" __DATE__ " " __TIME__ ")";
#ifdef GIT_HASH
    version += " [" GIT_HASH
  #ifdef GIT_BRANCH
    "@" GIT_BRANCH
  #endif
    "]";
#endif
    request->send(200, "text/plain", version);
  });

  server->on("/get_height", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(TOTAL_HEIGHT));
  });

  server->on("/get_width", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(TOTAL_WIDTH));
  });
#ifndef DISPLAY_RM67162_AMOLED
  server->on("/get_rgb_order", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(rgbMode));
  });

  server->on("/get_panel_clkphase", HTTP_GET,
             [](AsyncWebServerRequest *request) {
               request->send(200, "text/plain", String(panelClkphase));
             });

  server->on("/get_panel_driver", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(panelDriver));
  });

  server->on("/get_panel_i2sspeed", HTTP_GET,
             [](AsyncWebServerRequest *request) {
               request->send(200, "text/plain", String(panelI2sspeed));
             });

  server->on("/get_panel_latchblanking", HTTP_GET,
             [](AsyncWebServerRequest *request) {
               request->send(200, "text/plain", String(panelLatchBlanking));
             });

  server->on("/get_panel_minrefreshrate", HTTP_GET,
             [](AsyncWebServerRequest *request) {
               request->send(200, "text/plain", String(panelMinRefreshRate));
             });

  server->on("/get_y_offset", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(yOffset));
  });
#endif
  server->on("/get_udp_delay", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(udpDelay));
  });

  server->on("/get_brightness", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(brightness));
  });

  server->on("/get_protocol", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (TRANSPORT_WIFI_UDP == transport) {
      request->send(200, "text/plain", "UDP");
    } else {
      request->send(200, "text/plain", "TCP");
    }
  });

  server->on("/get_port", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(port));
  });

  server->on(
      "/get_usb_package_size", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", String(usbPackageSizeMultiplier * 32));
      });

  server->on("/get_ssid", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", ssid);
  });

  server->on("/get_s3", HTTP_GET, [](AsyncWebServerRequest *request) {
#if defined(ARDUINO_ESP32_S3_N16R8) || defined(DISPLAY_RM67162_AMOLED)
    request->send(200, "text/plain", String(1));
#else
    request->send(200, "text/plain", String(0));
#endif
  });

  server->on("/get_short_id", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(shortId));
  });

  server->on("/handshake", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(
        200, "text/plain",
        String(TOTAL_WIDTH) + "|" + String(TOTAL_HEIGHT) + "|" +
            String(ZEDMD_VERSION_MAJOR) + "." + String(ZEDMD_VERSION_MINOR) +
            "." + String(ZEDMD_VERSION_PATCH) + "|" +
#if defined(ARDUINO_ESP32_S3_N16R8) || defined(DISPLAY_RM67162_AMOLED)
            String(1)
#else
            String(0)
#endif
            + "|" + ((TRANSPORT_WIFI_UDP == transport) ? "UDP" : "TCP") + "|" +
            String(port) + "|" + String(udpDelay) + "|" +
            String(usbPackageSizeMultiplier * 32) + "|" + String(brightness) +
            "|" +
#ifndef DISPLAY_RM67162_AMOLED
            String(rgbMode) + "|" + String(panelClkphase) + "|" +
            String(panelDriver) + "|" + String(panelI2sspeed) + "|" +
            String(panelLatchBlanking) + "|" + String(panelMinRefreshRate) +
            "|" + String(yOffset)
#else
            "0|0|0|0|0|0|0"
#endif
            + "|" + ssid + "|" +
#ifdef ZEDMD_HD_HALF
            "1"
#else
            "0"
#endif
            + "|" + String(shortId));
  });

  server->on("/ppuc.png", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/ppuc.png", "image/png");
  });

  server->on("/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    LittleFS.remove("/wifi_config.txt");  // Remove Wi-Fi config
    request->send(200, "text/plain", "Wi-Fi reset successful.");
    Restart();  // Restart the device
  });

  // POST /restart — Neustart des ESP
  server->on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Restarting...");
    delay(500);
    Restart();
  });

  // POST /reset_diag — Diagnose-Zähler zurücksetzen
  server->on("/reset_diag", HTTP_POST, [](AsyncWebServerRequest *request) {
    LittleFS.remove("/diag.json");
    request->send(200, "text/plain", "OK");
  });

  // POST /delete_crashlogs — alle Crash-Dumps löschen
  server->on("/delete_crashlogs", HTTP_POST, [](AsyncWebServerRequest *request) {
    for (int i = 0; i < CRASH_LOG_SLOTS; i++) {
      char fname[32];
      snprintf(fname, sizeof(fname), "/crashlogs/crash-%d.txt", i);
      LittleFS.remove(fname);
    }
    request->send(200, "text/plain", "OK");
  });

  server->on("/apply", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Apply successful.");
    SaveScreensaverLum();  // Screensaver-Helligkeit speichern
    Restart();  // Restart the device
  });

  // Serve debug information
  server->on("/debug_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String debugInfo = "IP Address: " + WiFi.localIP().toString() + "\n";
    debugInfo += "SSID: " + WiFi.SSID() + "\n";
    debugInfo += "RSSI: " + String(WiFi.RSSI()) + "\n";
    debugInfo += "Heap Free: " + String(ESP.getFreeHeap()) + " bytes\n";
    debugInfo += "Heap Largest Block: " + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) + " bytes\n";
    debugInfo += "Heap Min Ever Free: " + String(ESP.getMinFreeHeap()) + " bytes\n";
    debugInfo += "PSRAM Free: " + String(ESP.getFreePsram()) + " bytes\n";
    debugInfo += "Uptime: " + String(millis() / 1000) + " seconds\n";
    // Add more here if you need it
    request->send(200, "text/plain", debugInfo);
  });

  server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "";
    portENTER_CRITICAL(&logMux);
    uint8_t total = logCount;
    uint8_t start = (logCount < LOG_LINES) ? 0 : logHead;
    for (uint8_t i = 0; i < total; i++) {
      out += logBuffer[(start + i) % LOG_LINES];
      out += "\n";
    }
    portEXIT_CRITICAL(&logMux);
    request->send(200, "text/plain; charset=utf-8", out);
  });

  // GET /diag — Boot-/Crash-Statistik aus /diag.json + live crashFiles-Liste
  server->on("/diag", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out;
    File f = LittleFS.open("/diag.json", "r");
    if (f) {
      out = f.readString();
      f.close();
      int lastBrace = out.lastIndexOf('}');
      if (lastBrace >= 0) out = out.substring(0, lastBrace);
    } else {
      out = "{\n  \"boots\": 0,\n  \"lastReset\": \"UNKNOWN\",\n  \"crashNext\": 0";
    }
    out += ",\n  \"crashFiles\": [";
    bool first = true;
    for (int i = 0; i < CRASH_LOG_SLOTS; i++) {
      char fname[32];
      snprintf(fname, sizeof(fname), "/crashlogs/crash-%d.txt", i);
      if (LittleFS.exists(fname)) {
        if (!first) out += ", ";
        out += String(i);
        first = false;
      }
    }
    out += "]\n}\n";
    AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", out);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
  });

  // GET /crashlog?n=0..4 — einzelnen RTC-Dump aus LittleFS lesen
  server->on("/crashlog", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = 0;
    if (request->hasParam("n")) n = request->getParam("n")->value().toInt();
    if (n < 0 || n >= CRASH_LOG_SLOTS) { request->send(400, "text/plain", "n muss 0-9 sein"); return; }
    char fname[32];
    snprintf(fname, sizeof(fname), "/crashlogs/crash-%d.txt", n);
    if (!LittleFS.exists(fname)) {
      request->send(404, "text/plain", "Kein Dump vorhanden");
      return;
    }
    request->send(LittleFS, fname, "text/plain");
  });

  // GET /coredump — lädt rohen ELF-Coredump aus Flash-Partition zum Dekodieren mit espcoredump.py
  server->on("/coredump", HTTP_GET, [](AsyncWebServerRequest *request) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
      request->send(404, "text/plain", "No coredump partition");
      return;
    }
    // Erased flash = 0xFFFFFFFF → kein Coredump vorhanden.
    // ESP-IDF schreibt keinen ELF-Header direkt; erster uint32 ist die Datenlänge.
    uint32_t firstWord = 0xFFFFFFFF;
    esp_partition_read(part, 0, &firstWord, 4);
    if (firstWord == 0xFFFFFFFF) {
      request->send(404, "text/plain", "No valid coredump (no crash recorded yet)");
      return;
    }
    const size_t cdSize = part->size;
    AsyncWebServerResponse *resp = request->beginChunkedResponse("application/octet-stream",
      [part, cdSize](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        if (index >= cdSize) return 0;
        size_t toRead = min(maxLen, cdSize - index);
        if (esp_partition_read(part, index, buf, toRead) != ESP_OK) return 0;
        return toRead;
      });
    resp->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
  });

  // POST /save_mqtt_config
#ifdef ZEDMD_WIFI
  server->on("/save_mqtt_config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("server", true))
      mqttServer = request->getParam("server", true)->value();
    if (request->hasParam("port", true))
      mqttPort = (uint16_t)request->getParam("port", true)->value().toInt();
    SaveMqttConfig();
    mqttClient.disconnect();
    mqttClient.setServer(mqttServer.c_str(), mqttPort);
    logMsg("MQTT: Server geaendert auf %s:%d", mqttServer.c_str(), mqttPort);
    request->send(200, "text/plain", "OK");
  });
#endif

  // POST /save_weather_config
  server->on("/save_weather_config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("lat", true))
      weatherLat = request->getParam("lat", true)->value().toFloat();
    if (request->hasParam("lon", true))
      weatherLon = request->getParam("lon", true)->value().toFloat();
    SaveWeatherConfig();
    forecastAvailable = false;
    lastWeatherFetch  = 0;
    logMsg("Wetter: Koordinaten geaendert %.4f / %.4f", weatherLat, weatherLon);
    request->send(200, "text/plain", "OK");
  });

  // POST /save_timezone
  server->on("/save_timezone", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("tz", true)) {
      String tz = request->getParam("tz", true)->value();
      tz.trim();
      if (tz.length() > 0) {
        clockTimezone = tz;
        SaveTimezoneConfig();
        configTzTime(clockTimezone.c_str(), ntpServer.c_str());
        logMsg("Timezone geaendert: %s", clockTimezone.c_str());
      }
    }
    request->send(200, "text/plain", "OK");
  });

  // Route to return the current settings as JSON
  // snprintf in statischen Puffer — kein Heap-Aufbau durch String-Konkatenation
  server->on("/get_config", HTTP_GET, [](AsyncWebServerRequest *request) {
    static char json[896];
    String trimmedSsid = ssid;
    trimmedSsid.trim();

    int n = snprintf(json, sizeof(json),
      "{\"ssid\":\"%s\",\"port\":%u"
#ifndef DISPLAY_RM67162_AMOLED
      ",\"rgbOrder\":%u"
#endif
      ",\"brightness\":%u,\"screensaverBrightness\":%u,\"screensaverDuration\":%u"
      ",\"screensaverShuffle\":%u,\"screensaverStrictTimer\":%u,\"gifAudioEnabled\":%u"
      ",\"localIP\":\"%s\",\"screensaverMode\":%u"
      ",\"clockR\":%u,\"clockG\":%u,\"clockB\":%u"
      ",\"dateR\":%u,\"dateG\":%u,\"dateB\":%u"
      ",\"scaleMode\":%u,\"transport\":%u,\"udpDelay\":%u,\"usbSize\":%u",
      trimmedSsid.c_str(), (unsigned)port,
#ifndef DISPLAY_RM67162_AMOLED
      (unsigned)rgbMode,
#endif
      (unsigned)brightness, (unsigned)screensaverBrightness, (unsigned)screensaverDuration,
      (unsigned)screensaverShuffle, (unsigned)screensaverStrictTimer, (unsigned)gifAudioEnabled,
      WiFi.localIP().toString().c_str(), (unsigned)screensaverMode,
      (unsigned)clockR, (unsigned)clockG, (unsigned)clockB,
      (unsigned)dateR,  (unsigned)dateG,  (unsigned)dateB,
      (unsigned)display->GetCurrentScalingMode(), (unsigned)transport,
      (unsigned)udpDelay, (unsigned)usbPackageSizeMultiplier);

#ifdef ZEDMD_WIFI
    n += snprintf(json + n, sizeof(json) - n,
      ",\"mqttServer\":\"%s\",\"mqttPort\":%u",
      mqttServer.c_str(), (unsigned)mqttPort);
#endif

    n += snprintf(json + n, sizeof(json) - n,
      ",\"weatherLat\":%.4f,\"weatherLon\":%.4f,\"timezone\":\"%s\"",
      weatherLat, weatherLon, clockTimezone.c_str());

#ifdef DISPLAY_LED_MATRIX
    n += snprintf(json + n, sizeof(json) - n,
      ",\"panelClkphase\":%u,\"panelI2sspeed\":%u"
      ",\"panelLatchBlanking\":%u,\"panelMinRefreshRate\":%u,\"panelDriver\":%u",
      (unsigned)panelClkphase, (unsigned)panelI2sspeed,
      (unsigned)panelLatchBlanking, (unsigned)panelMinRefreshRate, (unsigned)panelDriver);
#endif

    if (n < (int)sizeof(json) - 1) json[n++] = '}';
    json[n] = '\0';

    request->send(200, "application/json", json);
  });

  server->on(
      "/get_scaling_modes", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!display) {
          request->send(500, "application/json",
                        "{\"error\":\"Display object not initialized\"}");
          return;
        }

        String jsonResponse;
        if (display->HasScalingModes()) {
          jsonResponse = "{";
          jsonResponse += "\"hasScalingModes\":true,";

          // Fetch current scaling mode
          uint8_t currentMode = display->GetCurrentScalingMode();
          jsonResponse += "\"currentMode\":" + String(currentMode) + ",";

          // Add the list of available scaling modes
          jsonResponse += "\"modes\":[";
          const char **scalingModes = display->GetScalingModes();
          uint8_t modeCount = display->GetScalingModeCount();
          for (uint8_t i = 0; i < modeCount; i++) {
            jsonResponse += "\"" + String(scalingModes[i]) + "\"";
            if (i < modeCount - 1) {
              jsonResponse += ",";
            }
          }
          jsonResponse += "]";
          jsonResponse += "}";
        } else {
          jsonResponse = "{\"hasScalingModes\":false}";
        }

        request->send(200, "application/json", jsonResponse);
      });

  // POST request to save the selected scaling mode
  server->on(
      "/save_scaling_mode", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!display) {
          request->send(500, "text/plain", "Display object not initialized");
          return;
        }

        if (request->hasParam("scalingMode", true)) {
          String scalingModeValue =
              request->getParam("scalingMode", true)->value();
          uint8_t scalingMode = scalingModeValue.toInt();

          // Update the scaling mode using the global display object
          display->SetCurrentScalingMode(scalingMode);
          SaveScale();
          request->send(200, "text/plain", "Scaling mode updated successfully");
        } else {
          request->send(400, "text/plain", "Missing scaling mode parameter");
        }
      });

  // GET /screensaver_files?offset=0
  server->on("/screensaver_files", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    if (screensaverFilesMutex && xSemaphoreTake(screensaverFilesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      uint16_t offset = 0;
      if (request->hasParam("offset"))
        offset = (uint16_t)constrain(request->getParam("offset")->value().toInt(), 0, (int)screensaverCount);
      uint16_t end = min((uint16_t)(offset + 20), screensaverCount);
      for (uint16_t i = offset; screensaverFiles && i < end; i++) {
        if (i > offset) json += ",";
        json += "\"" + String(screensaverFiles[i]) + "\"";
      }
      xSemaphoreGive(screensaverFilesMutex);
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // GET /gif_preview?path=<SD:|FS:>/<pfad> — GIF-Datei für Browser-Vorschau streamen
  server->on("/gif_preview", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("path")) {
      request->send(400, "text/plain", "Missing path");
      return;
    }
    String path = request->getParam("path")->value();

    // LittleFS: direkt senden
    if (path.startsWith("FS:")) {
      String fsPath = path.substring(3);
      if (!LittleFS.exists(fsPath)) { request->send(404, "text/plain", "Not found"); return; }
      request->send(LittleFS, fsPath, "image/gif");
      return;
    }

    // SD: erst komplett in PSRAM laden, dann SD sofort schliessen, dann aus RAM streamen.
    // Verhindert SPI-Konflikt zwischen WebServer (Core 0) und GIF-Screensaver (Core 1).
    if (path.startsWith("SD:")) {
      if (!sdCardAvailable) { request->send(503, "text/plain", "SD not available"); return; }
      String sdPath = path.substring(3);
      File f = SD.open(sdPath, "r");
      if (!f) { request->send(404, "text/plain", "Not found"); return; }
      size_t fileSize = f.size();
      if (fileSize > 1024UL * 1024UL) {
        f.close();
        request->send(413, "text/plain", "File too large for preview");
        return;
      }
      uint8_t *rawBuf = (uint8_t *)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!rawBuf) {
        f.close();
        request->send(503, "text/plain", "No memory");
        return;
      }
      f.read(rawBuf, fileSize);
      f.close();  // SD sofort freigeben — kein concurrent access mehr

      // shared_ptr: free genau einmal — bei EOF und bei Verbindungsabbruch
      std::shared_ptr<uint8_t> gifBuf(rawBuf, [](uint8_t *p) { heap_caps_free(p); });
      AsyncWebServerResponse *response = request->beginChunkedResponse("image/gif",
        [gifBuf, fileSize](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
          if (index >= fileSize) return 0;
          size_t toSend = min(fileSize - index, maxLen);
          memcpy(buf, gifBuf.get() + index, toSend);
          return toSend;
        }
      );
      response->addHeader("Cache-Control", "max-age=30");
      request->send(response);
      return;
    }

    request->send(400, "text/plain", "Invalid path prefix");
  });

  // GET /screensaver_folder_count — geladene Dateianzahl direkt aus screensaverCount
  server->on("/screensaver_folder_count", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint16_t count = screensaverCount;
    uint16_t showing = min((uint16_t)20, count);
    request->send(200, "application/json",
      "{\"total\":" + String(count) + ",\"showing\":" + String(showing) + "}");
  });

  // GET /delete_screensaver?file=xxx
  server->on("/delete_screensaver", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String filename = "/screensaver/" + request->getParam("file")->value();
      if (LittleFS.exists(filename)) {
        LittleFS.remove(filename);
        screensaverReloadNeeded = true;
        request->send(200, "text/plain", "Deleted");
      } else {
        request->send(404, "text/plain", "File not found");
      }
    } else {
      request->send(400, "text/plain", "Missing file parameter");
    }
  });

  // POST /upload_screensaver
  server->on("/upload_screensaver", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      screensaverReloadNeeded = true;
      request->send(200, "text/plain", "Upload OK");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      static File uploadFile;
      static String targetPath;
      if (index == 0) {
        if (uploadFile) { uploadFile.close(); LittleFS.remove((targetPath + ".tmp").c_str()); }
        targetPath = "/screensaver/" + filename;
        uploadFile = LittleFS.open(targetPath + ".tmp", "w");
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        LittleFS.rename(targetPath + ".tmp", targetPath);
      }
    }
  );

  // POST /save_clock_colors
  server->on("/save_clock_colors", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("clockR", true)) clockR = request->getParam("clockR", true)->value().toInt();
    if (request->hasParam("clockG", true)) clockG = request->getParam("clockG", true)->value().toInt();
    if (request->hasParam("clockB", true)) clockB = request->getParam("clockB", true)->value().toInt();
    if (request->hasParam("dateR",  true)) dateR  = request->getParam("dateR",  true)->value().toInt();
    if (request->hasParam("dateG",  true)) dateG  = request->getParam("dateG",  true)->value().toInt();
    if (request->hasParam("dateB",  true)) dateB  = request->getParam("dateB",  true)->value().toInt();
    SaveClockColors();
    clockColorChanged = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /save_screensaver_mode
  server->on("/save_screensaver_mode", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("mode", true)) {
      screensaverMode = request->getParam("mode", true)->value().toInt();
      clockColorChanged = true;  // Sofortige Neuzeichnung beim Moduswechsel
      weatherPhaseStart = 0;     // Phase-Timer zurücksetzen
      weatherPage = 0;
      request->send(200, "text/plain", "OK");
      SaveScreensaverMode();
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /screensaver_pause — Pause/Play Toggle
  server->on("/screensaver_pause", HTTP_POST, [](AsyncWebServerRequest *request) {
    screensaverPaused = !screensaverPaused;
    request->send(200, "application/json",
      String("{\"paused\":") + (screensaverPaused ? "true" : "false") + "}");
  });

  // GET /screensaver_status
  server->on("/screensaver_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json",
      String("{\"paused\":") + (screensaverPaused ? "true" : "false") + "}");
  });

  // POST /save_screensaver_brightness — speichert direkt
  server->on("/save_screensaver_brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("screensaverBrightness", true)) {
      screensaverBrightness = request->getParam("screensaverBrightness", true)->value().toInt();
      GetDisplayObject()->SetBrightness(screensaverBrightness);
      clockColorChanged = true;
      SaveScreensaverLum();
      request->send(200, "text/plain", "Screensaver brightness saved");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /save_screensaver_duration
  server->on("/save_screensaver_duration", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("screensaverDuration", true)) {
      screensaverDuration = request->getParam("screensaverDuration", true)->value().toInt();
      SaveScreensaverDuration();
      request->send(200, "text/plain", "Screensaver duration saved");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /save_screensaver_shuffle
  server->on("/save_screensaver_shuffle", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("screensaverShuffle", true)) {
      screensaverShuffle = request->getParam("screensaverShuffle", true)->value().toInt() != 0;
      SaveScreensaverShuffle();
      if (screensaverCount > 1) {
        screensaverIndex = 0;
        if (screensaverShuffle) shuffleScreensaverFiles();
        else sortScreensaverFiles();
      }
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /screensaver_reshuffle — Reihenfolge neu würfeln ohne Einstellung zu ändern
  server->on("/screensaver_reshuffle", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (screensaverShuffle && screensaverCount > 1) {
      screensaverIndex = 0;
      shuffleScreensaverFiles();
    }
    request->send(200, "text/plain", "OK");
  });

  // POST /save_screensaver_strict_timer
  server->on("/save_screensaver_strict_timer", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("screensaverStrictTimer", true)) {
      screensaverStrictTimer = request->getParam("screensaverStrictTimer", true)->value().toInt() != 0;
      SaveScreensaverStrictTimer();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /save_gif_audio_enabled
  server->on("/save_gif_audio_enabled", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("gifAudioEnabled", true)) {
      gifAudioEnabled = request->getParam("gifAudioEnabled", true)->value().toInt() != 0;
      SaveGifAudioEnabled();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });


  // GET /screensaver_current — aktuell angezeigte Datei
  server->on("/screensaver_current", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Force-Play hat Vorrang vor normalem Screensaver-Index
    String path = currentlyPlayingFile.length() > 0 ? currentlyPlayingFile :
                  (screensaverCount > 0 ? String(screensaverFiles[screensaverIndex]) : "");
    int slash = path.lastIndexOf('/');
    String fname = (slash >= 0) ? path.substring(slash + 1) : path;
    bool fav = isFavorite(path.c_str());
    bool ign = isIgnored(path.c_str());
    String json = "{\"path\":\"" + path + "\",\"name\":\"" + fname +
                  "\",\"favorite\":" + String(fav ? "true" : "false") +
                  ",\"ignored\":"   + String(ign ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  // POST /toggle_favorite
  server->on("/toggle_favorite", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("path", true)) {
      String path = request->getParam("path", true)->value();
      toggleFavorite(path.c_str());
      bool fav = isFavorite(path.c_str());
      request->send(200, "application/json",
        "{\"favorite\":" + String(fav ? "true" : "false") + "}");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // GET /get_favorites — Liste aller Favoriten
  server->on("/get_favorites", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", screensaverFavorites);
  });

  // POST /play_file — GIF direkt abspielen
  server->on("/play_file", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("path", true)) {
      forcePlayFile = request->getParam("path", true)->value();
      __sync_synchronize();
      forcePlayPending = true;
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /toggle_ignore
  server->on("/toggle_ignore", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("path", true)) {
      String path = request->getParam("path", true)->value();
      toggleIgnore(path.c_str());
      bool ign = isIgnored(path.c_str());
      request->send(200, "application/json",
        "{\"ignored\":" + String(ign ? "true" : "false") + "}");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // GET /get_ignores — Liste aller ignorierten Dateien
  server->on("/get_ignores", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", screensaverIgnore);
  });

  // GET /fs_info — Filesystem Speicherinfo
  server->on("/fs_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"total\":" + String(LittleFS.totalBytes()) + ",";
    json += "\"used\":" + String(LittleFS.usedBytes()) + ",";
    json += "\"free\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes());
    json += "}";
    request->send(200, "application/json", json);
  });

  // GET /sd_info — SD-Karte Speicherinfo (gecacht vom Boot)
  server->on("/sd_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"available\":" + String(sdCardAvailable ? "true" : "false") + ",";
    json += "\"total\":" + String((uint32_t)(sdTotalBytes / 1024)) + ",";
    json += "\"used\":"  + String((uint32_t)(sdUsedBytes  / 1024)) + ",";
    json += "\"free\":"  + String((uint32_t)((sdTotalBytes - sdUsedBytes) / 1024));
    json += "}";
    request->send(200, "application/json", json);
  });

  // POST /display_text — Text auf Display anzeigen (statisch oder scrollend, mit Farbe + Dauer)
  server->on("/display_text", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("text", true)) { request->send(400, "text/plain", "missing text"); return; }
    String text = request->getParam("text", true)->value();
    text.trim();
    if (text.length() == 0) {
      displayTextActive = false;
      request->send(200, "text/plain", "OK");
      return;
    }
    text.substring(0, 127).toCharArray(displayTextContent, sizeof(displayTextContent));
    displayTextScroll = display->GetTextGFXWidth(displayTextContent) > TOTAL_WIDTH;
    uint32_t dur = request->hasParam("duration", true) ?
                   (uint32_t)request->getParam("duration", true)->value().toInt() : 10;
    if (dur < 1) dur = 1; if (dur > 300) dur = 300;
    String col = request->hasParam("color", true) ?
                 request->getParam("color", true)->value() : "ffffff";
    if (col.startsWith("#")) col = col.substring(1);
    displayTextR = (uint8_t)strtol(col.substring(0, 2).c_str(), nullptr, 16);
    displayTextG = (uint8_t)strtol(col.substring(2, 4).c_str(), nullptr, 16);
    displayTextB = (uint8_t)strtol(col.substring(4, 6).c_str(), nullptr, 16);
    displayTextEnd        = millis() + dur * 1000;
    displayTextScrollX    = TOTAL_WIDTH;
    displayTextNeedsClear = true;
    displayTextActive     = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /display_text_stop — Display-Text sofort beenden
  server->on("/display_text_stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    displayTextActive = false;
    request->send(200, "text/plain", "OK");
  });

  // POST /eject_sd — SD-Karte sicher aushängen
  server->on("/eject_sd", HTTP_POST, [](AsyncWebServerRequest *request) {
    SD.end();
    sdCardAvailable = false;
    sdTotalBytes = 0;
    sdUsedBytes  = 0;
    sdFoldersInvalidateNeeded = true;
    sdFilesInvalidateNeeded   = true;
    screensaverReloadNeeded   = true;  // SD-Pfade aus screensaverFiles entfernen (SD nicht verfügbar)
    request->send(200, "text/plain", "OK");
  });

  // POST /mount_sd — SD-Karte wieder einhängen
  server->on("/mount_sd", HTTP_POST, [](AsyncWebServerRequest *request) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
    const uint32_t spiSpeed = 8000000;
#else
    const uint32_t spiSpeed = 4000000;
#endif
    SD.end();
    bool ok = false;
    for (uint8_t i = 0; i < 3; i++) {
      if (SD.begin(SD_CS, spiSD, spiSpeed)) { ok = true; break; }
      SD.end();
      delay(500);
    }
    if (ok) {
      sdCardAvailable      = true;
      sdTotalBytes         = SD.cardSize();
      sdUsedBytes          = SD.usedBytes();
      gifAudioRefreshNeeded     = true;
      sdFoldersInvalidateNeeded  = true;
      screensaverReloadNeeded    = true;
      logMsg("SD remount OK");
      request->send(200, "text/plain", "OK");
    } else {
      sdCardAvailable = false;
      logMsg("SD remount FAILED");
      request->send(503, "text/plain", "Mount failed");
    }
  });

  // POST /test_weather_icons — Toggle: zeigt alle Wetter-Icons auf dem Display (temporär)
  server->on("/test_weather_icons", HTTP_POST, [](AsyncWebServerRequest *request) {
    weatherIconTestActive = !weatherIconTestActive;
    request->send(200, "text/plain", weatherIconTestActive ? "ON" : "OFF");
  });

  // GET /sd_folders — gibt gecachten Status zurück, kein SD Zugriff im Webserver Task!
  server->on("/sd_folders", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"available\":" + String(sdCardAvailable ? "true" : "false") + ",";
    json += "\"currentPaths\":\"" + screensaverPaths + "\",";
    json += "\"folders\":";
    if (cachedSDFolders) json += cachedSDFolders;
    json += "}";
    request->send(200, "application/json", json);
  });

  // POST /set_screensaver_paths
  server->on("/set_screensaver_paths", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("paths", true)) {
      screensaverPaths = request->getParam("paths", true)->value();
      screensaverPaths.trim();
      screensaverIndex = 0;
      screensaverReloadNeeded = true;
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing parameter");
    }
  });

  // POST /cancel_scan — SD-Scan abbrechen (Screensaver: setzt auch Pfade zurück)
  server->on("/cancel_scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    cancelSdScan = true;
    screensaverPaths = "";
    screensaverReloadNeeded = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /cancel_gif_audio_scan — GIF-Audio-Scan abbrechen
  server->on("/cancel_gif_audio_scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    cancelSdScan = true;
    request->send(200, "text/plain", "OK");
  });

  // ── GIF-Audio Verwaltung (/GifAudio/ auf SD) ─────────────────────────────────

  server->on("/gif_audio_files", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", cachedGifAudioFiles ? cachedGifAudioFiles : "[]");
  });

  server->on("/gif_audio_upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!sdCardAvailable) { request->send(503, "text/plain", "SD-Karte nicht verfügbar"); return; }
      InvalidateGifAudioCache();
      gifAudioRefreshNeeded = true;
      request->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      static File uploadFile;
      static String targetPath;
      if (!sdCardAvailable) return;
      if (index == 0) {
        if (uploadFile) { uploadFile.close(); SD.remove((targetPath + ".tmp").c_str()); }
        if (!SD.exists(GIF_AUDIO_DIR)) SD.mkdir(GIF_AUDIO_DIR);
        targetPath = String(GIF_AUDIO_DIR) + "/" + filename;
        uploadFile = SD.open((targetPath + ".tmp").c_str(), FILE_WRITE);
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        SD.rename(targetPath + ".tmp", targetPath);
      }
    }
  );

  server->on("/gif_audio_delete", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name", true)) {
      request->send(400, "text/plain", "Missing name");
      return;
    }
    if (!sdCardAvailable) { request->send(503, "text/plain", "SD not available"); return; }
    String path = String(GIF_AUDIO_DIR) + "/" + request->getParam("name", true)->value();
    SD.remove(path.c_str());
    InvalidateGifAudioCache();
    gifAudioRefreshNeeded = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /upload_sd — Upload auf SD Karte
  // Optionaler Query-Parameter ?folder=Name überschreibt screensaverPaths
  server->on("/upload_sd", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      InvalidateFolderCache(lastUploadFolder);
      screensaverReloadNeeded = true;
      sdFoldersRefreshNeeded = true;
      request->send(200, "text/plain", "Upload OK");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      static File uploadFile;
      static String targetPath;
      if (!sdCardAvailable) return;
      if (index == 0) {
        if (uploadFile) { uploadFile.close(); SD.remove((targetPath + ".tmp").c_str()); }
        String folder;
        if (request->hasParam("folder")) {
          folder = request->getParam("folder")->value();
          folder.trim();
          if (!folder.startsWith("/")) folder = "/" + folder;
        } else {
          int comma = screensaverPaths.indexOf(',');
          String firstPath = (comma >= 0) ? screensaverPaths.substring(0, comma) : screensaverPaths;
          firstPath.trim();
          folder = firstPath.length() > 0 ? firstPath : "/screensaver";
          if (!folder.startsWith("/")) folder = "/" + folder;
        }
        if (!SD.exists(folder)) SD.mkdir(folder);
        lastUploadFolder = folder;
        targetPath = folder + "/" + filename;
        uploadFile = SD.open(targetPath + ".tmp", FILE_WRITE);
        Serial.printf("SD Upload: %s\n", targetPath.c_str());
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        SD.rename(targetPath + ".tmp", targetPath);
        Serial.println("SD Upload: done");
      }
    }
  );

  // Start the web server
  // GET /admin — Passwortgeschützte Admin Seite (admin.html im LittleFS)
  server->on("/admin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate("admin", "zedmd1234")) {
      return request->requestAuthentication();
    }
    sendLittleFSHtml(request, "/admin.html");
  });

  // GET /sd_files — SD Dateien in einem Ordner (gecacht; loop() baut Cache)
  server->on("/sd_files", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("folder")) {
      String folder = "/" + request->getParam("folder")->value();
      if (strcmp(cachedSdFilesFolder, folder.c_str()) != 0) {
        strncpy(cachedSdFilesFolder, folder.c_str(), CACHE_SD_FOLDER_SIZE - 1);
        cachedSdFilesFolder[CACHE_SD_FOLDER_SIZE - 1] = '\0';
        sdFilesInvalidateNeeded = true;  // ungültig → loop() baut neu
      }
      sdFilesRefreshNeeded = true;
    }
    request->send(200, "application/json", cachedSdFiles ? cachedSdFiles : "[]");
  });

  // GET /delete_sd_file — SD Datei löschen
  server->on("/delete_sd_file", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (sdCardAvailable && request->hasParam("folder") && request->hasParam("file")) {
      String path = "/" + request->getParam("folder")->value() + "/" + request->getParam("file")->value();
      InvalidateFolderCache("/" + request->getParam("folder")->value());
      SD.remove(path) ?
        request->send(200, "text/plain", "OK") :
        request->send(500, "text/plain", "Delete failed");
    } else {
      request->send(400, "text/plain", "Missing parameters");
    }
  });

  // POST /save_transport
  server->on("/save_transport", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("transport", true)) {
      transport = request->getParam("transport", true)->value().toInt();
      File f = LittleFS.open("/transport.val", "w");
      if (f) { f.write(transport); f.close(); }
      request->send(200, "text/plain", "OK");
    } else request->send(400, "text/plain", "Missing parameter");
  });

  // POST /save_udp_delay
  server->on("/save_udp_delay", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("udpDelay", true)) {
      udpDelay = request->getParam("udpDelay", true)->value().toInt();
      File f = LittleFS.open("/udp_delay.val", "w");
      if (f) { f.write(udpDelay); f.close(); }
      request->send(200, "text/plain", "OK");
    } else request->send(400, "text/plain", "Missing parameter");
  });

  // POST /save_usb_size
  server->on("/save_usb_size", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("usbPackageSizeMultiplier", true)) {
      usbPackageSizeMultiplier = request->getParam("usbPackageSizeMultiplier", true)->value().toInt();
      File f = LittleFS.open("/usb_size.val", "w");
      if (f) { f.write(usbPackageSizeMultiplier); f.close(); }
      request->send(200, "text/plain", "OK");
    } else request->send(400, "text/plain", "Missing parameter");
  });

  // POST /save_panel_settings
#ifdef DISPLAY_LED_MATRIX
  server->on("/save_panel_settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("panelClkphase", true))
      panelClkphase = request->getParam("panelClkphase", true)->value().toInt();
    if (request->hasParam("panelI2sspeed", true))
      panelI2sspeed = request->getParam("panelI2sspeed", true)->value().toInt();
    if (request->hasParam("panelLatchBlanking", true))
      panelLatchBlanking = request->getParam("panelLatchBlanking", true)->value().toInt();
    if (request->hasParam("panelMinRefreshRate", true))
      panelMinRefreshRate = request->getParam("panelMinRefreshRate", true)->value().toInt();
    if (request->hasParam("panelDriver", true))
      panelDriver = request->getParam("panelDriver", true)->value().toInt();
    auto sv = [](const char* p, uint8_t v) {
      File f = LittleFS.open(p, "w");
      if (f) { f.write(v); f.close(); }
    };
    sv("/panel_clkphase.val", panelClkphase);
    sv("/panel_i2sspeed.val", panelI2sspeed);
    sv("/panel_latch_blanking.val", panelLatchBlanking);
    sv("/panel_min_refresh_rate.val", panelMinRefreshRate);
    sv("/panel_driver.val", panelDriver);
    request->send(200, "text/plain", "OK");
  });
#endif  // DISPLAY_LED_MATRIX

  // POST /upload_file — Upload HTML files to LittleFS root (admin only)
  server->on("/upload_file", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!request->authenticate("admin", "zedmd1234")) {
        return request->requestAuthentication();
      }
      request->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      static File uploadFile;
      static String targetPath;
      if (!filename.endsWith(".html") && !filename.endsWith(".htm")) return;
      if (index == 0) {
        if (uploadFile) { uploadFile.close(); LittleFS.remove((targetPath + ".tmp").c_str()); }
        targetPath = "/" + filename;
        uploadFile = LittleFS.open(targetPath + ".tmp", "w");
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        LittleFS.rename(targetPath + ".tmp", targetPath);
      }
    }
  );

  // POST /upload_icon — Upload 20×20 RGBA-Icons nach LittleFS /icons/
  server->on("/upload_icon", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!request->authenticate("admin", "zedmd1234")) return request->requestAuthentication();
      request->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      if (!request->authenticate("admin", "zedmd1234")) return;
      if (!filename.endsWith(".rgba")) return;
      static File uploadFile;
      if (index == 0) {
        if (!LittleFS.exists("/icons")) LittleFS.mkdir("/icons");
        if (uploadFile) { uploadFile.close(); LittleFS.remove(("/icons/" + filename + ".tmp").c_str()); }
        uploadFile = LittleFS.open("/icons/" + filename + ".tmp", "w");
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        String dst = "/icons/" + filename;
        LittleFS.remove(dst);
        LittleFS.rename("/icons/" + filename + ".tmp", dst);
        logMsg("Icon hochgeladen: %s", dst.c_str());
        iconsReloadNeeded = true;
      }
    }
  );

  // POST /upload_icon_small — Upload 10×10 RGBA-Icons nach LittleFS /icons_small/
  server->on("/upload_icon_small", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!request->authenticate("admin", "zedmd1234")) return request->requestAuthentication();
      request->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      if (!request->authenticate("admin", "zedmd1234")) return;
      if (!filename.endsWith(".rgba")) return;
      static File uploadFile;
      if (index == 0) {
        if (!LittleFS.exists("/icons_small")) LittleFS.mkdir("/icons_small");
        if (uploadFile) { uploadFile.close(); LittleFS.remove(("/icons_small/" + filename + ".tmp").c_str()); }
        uploadFile = LittleFS.open("/icons_small/" + filename + ".tmp", "w");
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        String dst = "/icons_small/" + filename;
        LittleFS.remove(dst);
        LittleFS.rename("/icons_small/" + filename + ".tmp", dst);
        logMsg("Small-Icon hochgeladen: %s", dst.c_str());
        iconsReloadNeeded = true;
      }
    }
  );

  // POST /ota — Firmware-Update über WiFi (admin only)
  server->on("/ota", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!request->authenticate("admin", "zedmd1234")) {
        return request->requestAuthentication();
      }
      bool success = !Update.hasError();
      AsyncWebServerResponse *response = request->beginResponse(
          200, "text/plain", success ? "OK — Reboot..." : Update.errorString());
      response->addHeader("Connection", "close");
      request->send(response);
      if (success) {
        delay(500);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index,
       uint8_t *data, size_t len, bool final) {
      static bool authOk = false;
      if (index == 0) {
        authOk = request->authenticate("admin", "zedmd1234");
        if (authOk) {
          logMsg("OTA: Start %s", filename.c_str());
          Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
        }
      }
      if (authOk && Update.isRunning()) Update.write(data, len);
      if (final && authOk) {
        if (Update.end(true)) {
          logMsg("OTA: Erfolgreich (%u Bytes)", index + len);
        } else {
          logMsg("OTA: Fehler — %s", Update.errorString());
        }
      }
    }
  );

  // ── Config Export / Import ────────────────────────────────────────────────

  server->on("/config_transfer.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendLittleFSHtml(request, "/config_transfer.html");
  });

  server->on("/export_config", HTTP_GET, [](AsyncWebServerRequest *request) {
    static const char* configFiles[] = {
      "/wifi_config.txt", "/lum.val", "/rgb_order.val", "/transport.val",
      "/screensaver_path.val", "/screensaver_mode.val", "/screensaver_lum.val",
      "/screensaver_duration.val", "/screensaver_shuffle.val",
      "/screensaver_strict_timer.val", "/gif_audio_enabled.val", "/screensaver_favorites.txt",
      "/screensaver_ignore.txt", "/clock_colors.val",
      "/mqtt_config.val", "/weather_config.val",
#ifdef WEBRADIO_ENABLED
      "/radio_presets.json",
#endif
      nullptr
    };
    String json = "{\"v\":1,\"files\":{";
    bool first = true;
    for (int i = 0; configFiles[i] != nullptr; i++) {
      File f = LittleFS.open(configFiles[i], "r");
      if (!f) continue;
      if (!first) json += ",";
      first = false;
      json += "\"";
      json += configFiles[i];
      json += "\":\"";
      while (f.available()) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)f.read());
        json += hex;
      }
      json += "\"";
      f.close();
    }
    json += "}}";
    request->send(200, "application/json", json);
  });

  server->on("/import_config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("data", true)) {
      request->send(400, "text/plain", "Missing data");
      return;
    }
    String json = request->getParam("data", true)->value();
    // Einfacher Parser: sucht "\"/<key>\":\"<hex>\""
    int pos = 0;
    int written = 0;
    while (true) {
      int keyStart = json.indexOf("\"/", pos);
      if (keyStart < 0) break;
      int keyEnd = json.indexOf("\":\"", keyStart);
      if (keyEnd < 0) break;
      int valStart = keyEnd + 3;
      int valEnd   = json.indexOf("\"", valStart);
      if (valEnd < 0) break;
      String path = json.substring(keyStart + 1, keyEnd);
      String hex  = json.substring(valStart, valEnd);
      File f = LittleFS.open(path, "w");
      if (f) {
        for (int i = 0; i + 1 < (int)hex.length(); i += 2) {
          char buf[3] = { hex[i], hex[i+1], 0 };
          f.write((uint8_t)strtol(buf, nullptr, 16));
        }
        f.close();
        written++;
      }
      pos = valEnd + 1;
    }
    request->send(200, "text/plain", String(written) + " Dateien importiert");
  });

#ifdef WEBRADIO_ENABLED
  radioRegisterRoutes(server);
#endif

  server->begin();
  serverRunning = true;
}

void StartWiFi() {
  char apSSID[17];
  snprintf(apSSID, sizeof(apSSID), "ZeDMD-WiFi-%04X", shortId);
  const char *apPassword = "zedmd1234";
  bool softAPFallback = false;
  IPAddress ip;

  if (ssid_length > 0) {
    WiFi.disconnect(true);
    WiFi.begin(ssid.substring(0, ssid_length).c_str(),
               pwd.substring(0, pwd_length).c_str());

    // Don't use WiFi.waitForConnectResult(10000) here, it blocks the menu
    // button.
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
      CheckMenuButton();
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(100));  // FreeRTOS delay, avoids blocking
    }

    if (WiFi.status() != WL_CONNECTED) {
      display->DisplayText("No WiFi connection, error ", 10,
                           TOTAL_HEIGHT / 2 - 9, 255, 0, 0);
      DisplayNumber(WiFi.status(), 2, 26 * 4 + 10, TOTAL_HEIGHT / 2 - 9, 255, 0,
                    0);
      display->DisplayText("Trying again ...", 10, TOTAL_HEIGHT / 2 - 3, 255, 0,
                           0);
      // second try
      startTime = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
        CheckMenuButton();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));  // FreeRTOS delay, avoids blocking
      }
      if (WiFi.status() != WL_CONNECTED) {
        softAPFallback = true;
      }
    }
  } else {
    // Don't use the fallback to skip the countdown.
    WiFi.softAP(apSSID, apPassword);
    ip = WiFi.softAPIP();
  }

  if (!softAPFallback && WiFi.getMode() == WIFI_STA) {
    ip = WiFi.localIP();
  }

  if (ip[0] == 0 || softAPFallback) {
    display->DisplayText("No WiFi connection, maybe     ", 10,
                         TOTAL_HEIGHT / 2 - 9, 255, 0, 0);
    display->DisplayText("the credentials are wrong.", 10, TOTAL_HEIGHT / 2 - 3,
                         255, 0, 0);
    display->DisplayText("Start AP in 20 seconds ...", 10, TOTAL_HEIGHT / 2 + 3,
                         255, 0, 0);
    for (uint8_t i = 19; i > 0; i--) {
      CheckMenuButton();
      vTaskDelay(pdMS_TO_TICKS(1000));
      DisplayNumber(i, 2, 58, TOTAL_HEIGHT / 2 + 3, 255, 0, 0);
    }
    WiFi.softAP(apSSID, apPassword);
    ip = WiFi.softAPIP();
    softAPFallback = true;
  }

  ClearScreen();
  DisplayLogo();
  // DisplayId();

  for (uint8_t i = 0; i < 4; i++) {
    if (i > 0) display->DrawPixel(i * 3 * 4 + i * 2 - 2, 4, 255, 255, 255);
    DisplayNumber(ip[i], 3, i * 3 * 4 + i * 2, 0, 255, 255, 255, 1);
  }

  WiFi.setSleep(false);  // WiFi speed improvement on ESP32 S3 and others.

  wifiActive = true;

  // Start the MDNS server for easy detection
  if (!MDNS.begin("zedmd-wifi")) {
    display->DisplayText("MDNS could not be started", 0, 0, 255, 0, 0);
    while (1);
  }

  // display->DisplayText("zedmd-wifi.local", 0, TOTAL_HEIGHT - 5, 0, 0, 0, 1);

  StartServer();

  if (TRANSPORT_WIFI_UDP == transport) {
    udp = new AsyncUDP();
    udp->onPacket(HandleUdpPacket);
    if (!udp->listen(ip, port)) {
      display->DisplayText("UDP server could not be started", 0, 0, 255, 0, 0);
      while (1);
    }
  } else {
    tcp = new AsyncServer(port);
    tcp->setNoDelay(true);
    tcp->onClient(&NewTcpClient, tcp);
    tcp->begin();
  }
}

// InitNTP() → clock.cpp (clockInit())

void SaveScreensaverMode() {
  File f = LittleFS.open("/screensaver_mode.val", "w");
  if (!f) return;
  f.write(screensaverMode);
  f.close();
}

void LoadScreensaverMode() {
  File f = LittleFS.open("/screensaver_mode.val", "r");
  if (!f) {
    SaveScreensaverMode();
    return;
  }
  screensaverMode = f.read();
  f.close();
}

// DrawSegDigit(), DrawColon(), clockDisplay(), clockInit() → clock.cpp

#ifdef WEBRADIO_ENABLED
void DisplayRadio() {
  static char     lastStation[64]  = "";
  static char     lastTitle[128]   = "";
  static uint32_t lastScroll       = 0;
  static int      scrollOffset     = 0;
  static int      lastScrollOffset = -1;

  static char stationSnap[64]  = "Radio";
  static char titleSnap[128]   = "Verbinde...";
  if (radioStringMutex && xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (radioStationName[0]) strlcpy(stationSnap, radioStationName, sizeof(stationSnap));
    else strlcpy(stationSnap, "Radio", sizeof(stationSnap));
    if (radioTrackTitle[0])  strlcpy(titleSnap,   radioTrackTitle,  sizeof(titleSnap));
    else strlcpy(titleSnap, "Verbinde...", sizeof(titleSnap));
    xSemaphoreGive(radioStringMutex);
  }
  const char* station = stationSnap;
  const char* title   = titleSnap;
  bool needRedraw = false;

  if (strcmp(lastStation, station) != 0) {
    strlcpy(lastStation, station, sizeof(lastStation));
    needRedraw = true;
  }
  if (strcmp(lastTitle, title) != 0) {
    strlcpy(lastTitle, title, sizeof(lastTitle));
    scrollOffset = 0; lastScrollOffset = -1;
    needRedraw = true;
  }
  int titleLen = strlen(title);
  if (titleLen > 32) {
    uint32_t now = millis();
    if (now - lastScroll > 220) {
      scrollOffset = (scrollOffset + 1) % (titleLen - 32 + 7);
      lastScroll   = now;
    }
    if (scrollOffset != lastScrollOffset) { lastScrollOffset = scrollOffset; needRedraw = true; }
  }
  if (!needRedraw) return;

  display->SetBrightness(screensaverBrightness);
  display->ClearScreen();

  char stBuf[33];
  strlcpy(stBuf, station, sizeof(stBuf));
  display->DisplayText(stBuf, max(0, (TOTAL_WIDTH - (int)strlen(stBuf) * 4) / 2), 2, 255, 200, 50);

  for (int x = 0; x < TOTAL_WIDTH; x++)
    display->DrawPixel(x, 11, 60, 60, 60);

  if (titleLen <= 32) {
    display->DisplayText(title, max(0, (TOTAL_WIDTH - titleLen * 4) / 2), 14, 200, 200, 200);
  } else {
    int startIdx = min(scrollOffset, titleLen);
    char scrollBuf[33] = {};
    strlcpy(scrollBuf, title + startIdx, (size_t)min(32, titleLen - startIdx) + 1);
    display->DisplayText(scrollBuf, 0, 14, 200, 200, 200);
  }

  display->DisplayText("LIVE", TOTAL_WIDTH - 16, 24, 220, 40, 40);
  Render();
}
#endif

// DrawColon(), clockDisplay() → clock.cpp

void SaveClockColors() {
  File f = LittleFS.open("/clock_colors.val", "w");
  if (f) {
    f.write(clockR); f.write(clockG); f.write(clockB);
    f.write(dateR);  f.write(dateG);  f.write(dateB);
    f.close();
  }
}

void LoadClockColors() {
  File f = LittleFS.open("/clock_colors.val", "r");
  if (f && f.size() >= 6) {
    clockR = f.read(); clockG = f.read(); clockB = f.read();
    dateR  = f.read(); dateG  = f.read(); dateB  = f.read();
    f.close();
  }
}

// ── MQTT + Wetter Konfiguration ───────────────────────────────────────────────

#ifdef ZEDMD_WIFI
void SaveMqttConfig() {
  File f = LittleFS.open("/mqtt_config.txt", "w");
  if (!f) return;
  f.println(mqttServer);
  f.println(mqttPort);
  f.close();
}
void LoadMqttConfig() {
  File f = LittleFS.open("/mqtt_config.txt", "r");
  if (!f) { SaveMqttConfig(); return; }
  mqttServer = f.readStringUntil('\n'); mqttServer.trim();
  mqttPort   = (uint16_t)f.readStringUntil('\n').toInt();
  f.close();
}
#endif

void SaveWeatherConfig() {
  File f = LittleFS.open("/weather_config.txt", "w");
  if (!f) return;
  f.println(weatherLat, 6);
  f.println(weatherLon, 6);
  f.close();
}
void LoadWeatherConfig() {
  File f = LittleFS.open("/weather_config.txt", "r");
  if (!f) { SaveWeatherConfig(); return; }
  weatherLat = f.readStringUntil('\n').toFloat();
  weatherLon = f.readStringUntil('\n').toFloat();
  f.close();
}

void SaveTimezoneConfig() {
  File f = LittleFS.open("/timezone.txt", "w");
  if (!f) return;
  f.println(clockTimezone);
  f.close();
}
void LoadTimezoneConfig() {
  File f = LittleFS.open("/timezone.txt", "r");
  if (!f) return;  // kein File = Defaultwert aus clock.cpp bleibt
  String tz = f.readStringUntil('\n');
  f.close();
  tz.trim();
  if (tz.length() > 0) clockTimezone = tz;
}

// ── Wetter-Feature (Modus 3) → weather.cpp ───────────────────────────────────
void SaveScreensaverLum() {
  File f = LittleFS.open("/screensaver_lum.val", "w");
  if (!f) return;
  f.write(screensaverBrightness);
  f.close();
}

void LoadScreensaverLum() {
  File f = LittleFS.open("/screensaver_lum.val", "r");
  if (!f) {
    SaveScreensaverLum();
    return;
  }
  screensaverBrightness = f.read();
  f.close();
}

void SaveScreensaverDuration() {
  File f = LittleFS.open("/screensaver_dur.val", "w");
  if (!f) return;
  f.write(screensaverDuration);
  f.close();
}

void LoadScreensaverDuration() {
  File f = LittleFS.open("/screensaver_dur.val", "r");
  if (!f) {
    SaveScreensaverDuration();
    return;
  }
  screensaverDuration = f.read();
  f.close();
}

void SaveScreensaverShuffle() {
  File f = LittleFS.open("/screensaver_shuffle.val", "w");
  if (!f) return;
  f.write((uint8_t)screensaverShuffle);
  f.close();
}

void LoadScreensaverShuffle() {
  File f = LittleFS.open("/screensaver_shuffle.val", "r");
  if (!f) { SaveScreensaverShuffle(); return; }
  screensaverShuffle = (bool)f.read();
  f.close();
}



void SaveScreensaverStrictTimer() {
  File f = LittleFS.open("/screensaver_strict_timer.val", "w");
  if (!f) return;
  f.write((uint8_t)screensaverStrictTimer);
  f.close();
}

void LoadScreensaverStrictTimer() {
  File f = LittleFS.open("/screensaver_strict_timer.val", "r");
  if (!f) { SaveScreensaverStrictTimer(); return; }
  screensaverStrictTimer = (bool)f.read();
  f.close();
}

void SaveGifAudioEnabled() {
  File f = LittleFS.open("/gif_audio_enabled.val", "w");
  if (!f) return;
  f.write((uint8_t)gifAudioEnabled);
  f.close();
}

void LoadGifAudioEnabled() {
  File f = LittleFS.open("/gif_audio_enabled.val", "r");
  if (!f) { SaveGifAudioEnabled(); return; }
  gifAudioEnabled = (bool)f.read();
  f.close();
}


void shuffleScreensaverFiles() {
  if (screensaverCount <= 1) return;
  for (uint16_t i = screensaverCount - 1; i > 0; i--) {
    uint16_t j = (uint16_t)(esp_random() % (i + 1));
    char tmp[128];
    memcpy(tmp, screensaverFiles[i], 128);
    memcpy(screensaverFiles[i], screensaverFiles[j], 128);
    memcpy(screensaverFiles[j], tmp, 128);
  }
}

void sortScreensaverFiles() {
  if (screensaverCount < 2) return;
  esp_task_wdt_reset();

  // Index-Array sortieren (2 Byte/Eintrag) statt 128-Byte-Blöcke verschieben
  uint16_t* idx = (uint16_t*)heap_caps_malloc(screensaverCount * sizeof(uint16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!idx) return;
  for (uint16_t i = 0; i < screensaverCount; i++) idx[i] = i;

  std::sort(idx, idx + screensaverCount, [](uint16_t a, uint16_t b) {
    const char* na = strrchr(screensaverFiles[a], '/');
    const char* nb = strrchr(screensaverFiles[b], '/');
    na = na ? na + 1 : screensaverFiles[a];
    nb = nb ? nb + 1 : screensaverFiles[b];
    return strcasecmp(na, nb) < 0;
  });

  // Sortierte Reihenfolge einmalig sequenziell umkopieren
#ifdef BOARD_HAS_PSRAM
  char (*tmp)[128] = (char (*)[128])heap_caps_malloc(screensaverCount * 128,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  char (*tmp)[128] = (char (*)[128])malloc(screensaverCount * 128);
#endif
  if (!tmp) { free(idx); return; }
  for (uint16_t i = 0; i < screensaverCount; i++) memcpy(tmp[i], screensaverFiles[idx[i]], 128);
  memcpy(screensaverFiles, tmp, (size_t)screensaverCount * 128);
  free(tmp);
  free(idx);
  esp_task_wdt_reset();
}

uint16_t nextScreensaverIndex() {
  if (screensaverCount <= 1) return 0;
  if (!screensaverShuffle) return (screensaverIndex + 1) % screensaverCount;
  uint16_t next = screensaverIndex + 1;
  if (next >= screensaverCount) {
    shuffleScreensaverFiles();
    next = 0;
  }
  return next;
}

// ── Icon-System ───────────────────────────────────────────────────────────────
// Icon-System: RGBA-Pixel-Art-Icons aus LittleFS in PSRAM.
// /icons/      → 20×20 px (Emoji-Lauftext + großes Wetter-Icon)
// /icons_small/→ 10×10 px (kleine Wetter-Forecast-Icons)

#define ICON_W        20
#define ICON_H        20
#define ICON_BYTES    (ICON_W * ICON_H * 4)
#define ICON_W_S      10
#define ICON_H_S      10
#define ICON_BYTES_S  (ICON_W_S * ICON_H_S * 4)
#define MAX_ICONS     48

struct IconEntry {
  char     name[24];
  uint8_t* data;
};

static IconEntry iconTable[MAX_ICONS];
static uint8_t   iconCount = 0;
static IconEntry iconTableSmall[MAX_ICONS];
static uint8_t   iconCountSmall = 0;

const uint8_t* GetIcon(const char* name) {
  for (uint8_t i = 0; i < iconCount; i++)
    if (strcmp(iconTable[i].name, name) == 0) return iconTable[i].data;
  return nullptr;
}

const uint8_t* GetSmallIcon(const char* name) {
  for (uint8_t i = 0; i < iconCountSmall; i++)
    if (strcmp(iconTableSmall[i].name, name) == 0) return iconTableSmall[i].data;
  return nullptr;
}

static void loadIconSet(const char* dir_path, IconEntry* table, uint8_t& count,
                        uint8_t max, uint32_t expected_bytes) {
  for (uint8_t i = 0; i < count; i++) {
    if (table[i].data) { heap_caps_free(table[i].data); table[i].data = nullptr; }
  }
  count = 0;
  if (!LittleFS.exists(dir_path)) { LittleFS.mkdir(dir_path); return; }
  File dir = LittleFS.open(dir_path);
  if (!dir || !dir.isDirectory()) { dir.close(); return; }
  File f = dir.openNextFile();
  while (f && count < max) {
    String fname = String(f.name());
    uint32_t sz = f.size();
    f.close();
    if (fname.endsWith(".rgba") && sz == expected_bytes) {
      String path = String(dir_path) + "/" + fname;
      File rf = LittleFS.open(path, "r");
      if (rf) {
        uint8_t* buf = (uint8_t*)heap_caps_malloc(expected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf && rf.read(buf, expected_bytes) == expected_bytes) {
          String n = fname.substring(0, fname.length() - 5);
          strncpy(table[count].name, n.c_str(), 23);
          table[count].name[23] = '\0';
          table[count].data = buf;
          count++;
        } else {
          if (buf) heap_caps_free(buf);
        }
        rf.close();
      }
    }
    f = dir.openNextFile();
  }
  dir.close();
}

void LoadIcons() {
  loadIconSet("/icons",       iconTable,      iconCount,      MAX_ICONS, ICON_BYTES);
  loadIconSet("/icons_small", iconTableSmall, iconCountSmall, MAX_ICONS, ICON_BYTES_S);
  logMsg("Icons: %d gross, %d klein", iconCount, iconCountSmall);
}

void InitSDCard() {
  bool mounted = false;

#ifdef SD_MMC_BUILD
  logMsg("InitSDCard: SDMMC CLK=%d CMD=%d DATA=%d", SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_DATA_PIN);
  SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_DATA_PIN);
  for (uint8_t i = 0; i < 4; i++) {
    if (SD_MMC.begin("/sdcard", true)) {  // true = 1-bit Modus
      mounted = true;
      break;
    }
    logMsg("SD: Mount-Versuch %d fehlgeschlagen...", i + 1);
    SD_MMC.end();
    delay(750);
  }
#else
  logMsg("InitSDCard: SCK=%d MISO=%d MOSI=%d CS=%d", SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // CS Pin explizit auf HIGH setzen vor SPI Init
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(SD_MISO, INPUT_PULLUP);

  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(800);  // SD-Karten brauchen nach Kaltstart mehr Anlaufzeit

  // SPI-Speed-Stufen: bei Fehlschlag schrittweise reduzieren
#ifdef CONFIG_IDF_TARGET_ESP32S3
  static const uint32_t spiSpeeds[] = {8000000, 4000000, 2000000};
#else
  static const uint32_t spiSpeeds[] = {4000000, 2000000, 1000000};
#endif

  for (uint8_t i = 0; i < 6; i++) {
    uint32_t spiSpeed = spiSpeeds[i < 3 ? i : 2];  // ab Versuch 3: niedrigste Speed
    if (SD.begin(SD_CS, spiSD, spiSpeed)) {
      logMsg("SD: Erfolgreich bei %lu MHz (Versuch %d)", spiSpeed / 1000000, i + 1);
      mounted = true;
      break;
    }
    logMsg("SD: Mount-Versuch %d fehlgeschlagen (%lu MHz)...", i + 1, spiSpeed / 1000000);
    SD.end();
    delay(500);
  }
#endif

  if (mounted) {
    sdCardAvailable      = true;
    gifAudioRefreshNeeded = true;  // Cache-Versuch in loop() — nach NTP/MQTT
    sdTotalBytes = SD.cardSize();
    sdUsedBytes  = SD.usedBytes();
    logMsg("SD Card OK! Size: %llu MB, Used: %llu MB",
           sdTotalBytes / (1024*1024), sdUsedBytes / (1024*1024));
  } else {
    sdCardAvailable = false;
    sdTotalBytes = 0;
    sdUsedBytes  = 0;
    logMsg("SD Card FAILED!");
  }
}

void SaveScreensaverPaths() {
  File f = LittleFS.open("/screensaver_path.val", "w");
  if (f) {
    f.print(screensaverPaths);
    f.close();
  }
}

void LoadScreensaverPaths() {
  File f = LittleFS.open("/screensaver_path.val", "r");
  if (f) {
    screensaverPaths = f.readString();
    screensaverPaths.trim();
    f.close();
  } else {
    screensaverPaths = "";
  }
}

// ── Favoriten ────────────────────────────────────────────────────────────────
bool isFavorite(const char* path) {
  String p = String(path) + "\n";
  return screensaverFavorites.indexOf(p) >= 0;
}

void saveFavorites() {
  File f = LittleFS.open("/screensaver_favorites.txt", "w");
  if (f) { f.print(screensaverFavorites); f.close(); }
}

void toggleFavorite(const char* path) {
  String p = String(path) + "\n";
  int idx = screensaverFavorites.indexOf(p);
  if (idx >= 0) screensaverFavorites.remove(idx, p.length());
  else          screensaverFavorites += p;
  saveFavorites();
}

void LoadFavorites() {
  File f = LittleFS.open("/screensaver_favorites.txt", "r");
  if (f) { screensaverFavorites = f.readString(); f.close(); }
}

// ── Ignore-Liste ──────────────────────────────────────────────────────────────
bool isIgnored(const char* path) {
  String p = String(path) + "\n";
  return screensaverIgnore.indexOf(p) >= 0;
}

void saveIgnore() {
  File f = LittleFS.open("/screensaver_ignore.txt", "w");
  if (f) { f.print(screensaverIgnore); f.close(); }
}

void toggleIgnore(const char* path) {
  String p = String(path) + "\n";
  int idx = screensaverIgnore.indexOf(p);
  if (idx >= 0) screensaverIgnore.remove(idx, p.length());
  else          screensaverIgnore += p;
  saveIgnore();
}

void LoadIgnore() {
  File f = LittleFS.open("/screensaver_ignore.txt", "r");
  if (f) { screensaverIgnore = f.readString(); f.close(); }
}
// ─────────────────────────────────────────────────────────────────────────────

// ── Dateilisten-Cache ─────────────────────────────────────────────────────────
void addScreensaverFile(const String& path) {
  if (isIgnored(path.c_str())) return;
  if (screensaverCount >= screensaverFilesCapacity) {
    uint16_t newCap = screensaverFilesCapacity == 0 ? 32 : screensaverFilesCapacity * 2;
    char (*newBuf)[128];
#ifdef BOARD_HAS_PSRAM
    newBuf = (char (*)[128])heap_caps_realloc(screensaverFiles, newCap * 128,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    newBuf = (char (*)[128])realloc(screensaverFiles, newCap * 128);
#endif
    if (!newBuf) return;
    screensaverFiles = newBuf;
    screensaverFilesCapacity = newCap;
  }
  strncpy(screensaverFiles[screensaverCount], path.c_str(), 127);
  screensaverFiles[screensaverCount][127] = '\0';
  screensaverCount++;
}

// Cache-Dateiname aus Ordnerpfad ableiten
String folderCacheKey(const String& path) {
  String key = path;
  key.replace("/", "_");
  key.replace(" ", "_");
  key.replace(":", "_");
  if (key.length() > 20) key = key.substring(key.length() - 20);
  return "/sc_" + key + ".bin";
}

// Einzelnen Ordner aus Cache laden — 0 = Cache-Miss
uint16_t TryLoadFolderCache(const String& sdPath) {
  String cacheFile = folderCacheKey(sdPath);
  File f = LittleFS.open(cacheFile, "r");
  if (!f) return 0;
  uint16_t countBefore = screensaverCount;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) addScreensaverFile(line);
  }
  f.close();
  uint16_t loaded = screensaverCount - countBefore;
  logMsg("Cache[%s]: %d Dateien geladen", sdPath.c_str(), loaded);
  return loaded;
}

// Einzelnen Ordner in Cache schreiben
void SaveFolderCache(const String& sdPath, uint16_t fromIndex, uint16_t count) {
  String cacheFile = folderCacheKey(sdPath);
  File f = LittleFS.open(cacheFile, "w");
  if (!f) { logMsg("Cache: Konnte %s nicht schreiben", cacheFile.c_str()); return; }
  for (uint16_t i = fromIndex; i < fromIndex + count; i++) f.println(screensaverFiles[i]);
  f.close();
  logMsg("Cache[%s]: %d Dateien gespeichert", sdPath.c_str(), count);
}

// Alle Ordner-Caches löschen
void InvalidateAllFolderCaches() {
  LittleFS.remove("/screensaver_cache.txt");  // alter globaler Cache
  File root = LittleFS.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (name.startsWith("sc_") && name.endsWith(".bin")) {
      f.close();
      LittleFS.remove("/" + name);
      logMsg("Cache: %s geloescht", name.c_str());
    } else {
      f.close();
    }
    f = root.openNextFile();
  }
  root.close();
}

void InvalidateFolderCache(const String& path) {
  String cacheFile = folderCacheKey(path);
  if (LittleFS.exists(cacheFile)) {
    LittleFS.remove(cacheFile);
    logMsg("Cache: %s invalidiert", cacheFile.c_str());
  }
}

// Stubs für alte Aufrufe
void SaveScreensaverCache() {}
bool TryLoadScreensaverCache() { return false; }

#define GIF_AUDIO_CACHE_FILE "/gif_audio_cache.bin"

// Setzt einen PSRAM-Cache-Pointer auf neuen Inhalt.
// Allokiert zuerst den neuen Buffer, tauscht den Pointer atomar, gibt dann erst den alten frei —
// damit ist kein Null-Fenster möglich wenn ein Webserver-Task gleichzeitig liest.
static void psramCacheSet(char** ptr, const String& json) {
  size_t len = json.length();
  char* newBuf = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!newBuf) return;
  memcpy(newBuf, json.c_str(), len + 1);
  char* old = *ptr;
  *ptr = newBuf;   // atomarer 32-Bit-Pointer-Swap auf Xtensa
  if (old) heap_caps_free(old);
}

void SaveGifAudioCache() {
  File f = LittleFS.open(GIF_AUDIO_CACHE_FILE, "w");
  if (!f) { logMsg("GifAudioCache: Schreiben fehlgeschlagen"); return; }
  // Format: name|size\n
  String tmp = cachedGifAudioFiles ? cachedGifAudioFiles : "[]";
  // Einfaches Parsen des JSON-Arrays ohne ArduinoJson
  tmp.replace("[", ""); tmp.replace("]", "");
  while (tmp.length() > 0) {
    tmp.trim();
    if (!tmp.startsWith("{")) break;
    int end = tmp.indexOf('}');
    if (end < 0) break;
    String obj = tmp.substring(1, end);
    tmp = tmp.substring(end + 1);
    if (tmp.startsWith(",")) tmp = tmp.substring(1);
    int ni = obj.indexOf("\"name\":\""); if (ni < 0) continue;
    int ns = ni + 8; int ne = obj.indexOf('"', ns); if (ne < 0) continue;
    String name = obj.substring(ns, ne);
    int si = obj.indexOf("\"size\":"); uint32_t sz = 0;
    if (si >= 0) sz = (uint32_t)obj.substring(si + 7).toInt();
    f.printf("%s|%u\n", name.c_str(), sz);
  }
  f.close();
  logMsg("GifAudioCache: gespeichert (%s)", GIF_AUDIO_CACHE_FILE);
}

bool TryLoadGifAudioCache() {
  File f = LittleFS.open(GIF_AUDIO_CACHE_FILE, "r");
  if (!f) return false;
  String json = "[";
  bool first = true;
  uint16_t count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int sep = line.indexOf('|');
    String name = (sep >= 0) ? line.substring(0, sep) : line;
    uint32_t sz  = (sep >= 0) ? (uint32_t)line.substring(sep + 1).toInt() : 0;
    if (!first) json += ",";
    json += "{\"name\":\"" + name + "\",\"size\":" + String(sz) + "}";
    first = false;
    count++;
  }
  f.close();
  json += "]";
  if (json == "[]") return false;
  psramCacheSet(&cachedGifAudioFiles, json);
  logMsg("Cache[%s]: %d Dateien geladen", GIF_AUDIO_DIR, count);
  return true;
}

void InvalidateGifAudioCache() {
  LittleFS.remove(GIF_AUDIO_CACHE_FILE);
  logMsg("GifAudioCache: invalidiert");
}
// ─────────────────────────────────────────────────────────────────────────────

void LoadScreensaverFiles() {
  // Alten Puffer atomar austauschen — Webserver-Callbacks lesen screensaverFiles unter Mutex
  if (screensaverFilesMutex) {
    xSemaphoreTake(screensaverFilesMutex, portMAX_DELAY);
    char (*oldBuf)[128] = screensaverFiles;
    screensaverFiles         = nullptr;
    screensaverCount         = 0;
    screensaverFilesCapacity = 0;
    xSemaphoreGive(screensaverFilesMutex);
    if (oldBuf) free(oldBuf);
  } else {
    free(screensaverFiles);
    screensaverFiles         = nullptr;
    screensaverCount         = 0;
    screensaverFilesCapacity = 0;
  }
  screensaverCount = 0;

  // Datei hinzufügen — wächst dynamisch (verdoppelt bei Bedarf)
  // Favoriten-Modus: Pfade direkt aus Favoritenliste laden
  if (screensaverPaths == "FAV") {
    String remaining = screensaverFavorites;
    while (remaining.length() > 0) {
      int nl = remaining.indexOf('\n');
      String p = (nl >= 0) ? remaining.substring(0, nl) : remaining;
      remaining = (nl >= 0) ? remaining.substring(nl + 1) : "";
      p.trim();
      if (p.length() == 0) continue;
      if (p.startsWith("SD:") && !sdCardAvailable) continue;  // SD nicht verfügbar → überspringen
      addScreensaverFile(p);
    }
    logMsg("LoadScreensaver: %d Favoriten geladen", screensaverCount);
    if (screensaverCount > 0) goto done;
    // Keine Favoriten verfügbar → LittleFS Fallback
    logMsg("LoadScreensaver: Keine Favoriten verfügbar, Fallback auf LittleFS");
  }

  // Alle gewählten Pfade (kommagetrennt); "FS:" = LittleFS /screensaver/
  if (screensaverPaths.length() > 0) {
    String remaining = screensaverPaths;
    while (remaining.length() > 0) {
      int comma = remaining.indexOf(',');
      String entry = (comma >= 0) ? remaining.substring(0, comma) : remaining;
      remaining = (comma >= 0) ? remaining.substring(comma + 1) : "";
      entry.trim();
      if (entry.length() == 0) continue;

      // LittleFS /screensaver/ einlesen
      if (entry == "FS:") {
        if (!LittleFS.exists("/screensaver")) LittleFS.mkdir("/screensaver");
        File fsDir = LittleFS.open("/screensaver");
        if (fsDir) {
          uint16_t countBefore = screensaverCount;
          File f = fsDir.openNextFile();
          while (f) {
            if (!f.isDirectory()) {
              const char* fname = f.name();
              if (fname && fname[0] != '.') {
                String name = String(fname);
                name.trim();
                if (name.endsWith(".raw") || name.endsWith(".gif") || name.endsWith(".GIF"))
                  addScreensaverFile("FS:/screensaver/" + name);
              }
            }
            f.close();
            f = fsDir.openNextFile();
          }
          fsDir.close();
          logMsg("LoadScreensaver: %d Dateien aus LittleFS geladen", screensaverCount - countBefore);
        }
        continue;
      }

      if (!sdCardAvailable) { logMsg("LoadScreensaver: SD nicht verfügbar, überspringe %s", entry.c_str()); continue; }
      String sdPath = entry;
      if (!sdPath.startsWith("/")) sdPath = "/" + sdPath;

      // FIX 11: Pro-Ordner-Cache versuchen — kein Scan nötig wenn Cache vorhanden
      uint16_t cached = TryLoadFolderCache(sdPath);
      if (cached > 0) continue;

      // Cache-Miss — SD scannen
      logMsg("LoadScreensaver: SD Pfad=%s (kein Cache, scanne...) exists=%d",
             sdPath.c_str(), (int)SD.exists(sdPath.c_str()));
      File dir = SD.open(sdPath.c_str());
      if (dir && dir.isDirectory()) {
        char pathBuf[128];
        uint16_t scanned = 0;
        uint16_t countBefore = screensaverCount;
        File f = dir.openNextFile();
        while (f) {
          if (cancelSdScan) { f.close(); dir.close(); cancelSdScan = false; goto done; }
          if (!f.isDirectory()) {
            const char* fname = f.name();
            if (fname && fname[0] != '.') {
              const char* dot = strrchr(fname, '.');
              if (dot) {
                char ext[8]; strncpy(ext, dot, 7); ext[7] = '\0';
                for (char* p = ext; *p; p++) *p = tolower((unsigned char)*p);
                if (strcmp(ext, ".gif") == 0 || strcmp(ext, ".raw") == 0) {
                  snprintf(pathBuf, sizeof(pathBuf), "SD:%s/%s", sdPath.c_str(), fname);
                  addScreensaverFile(pathBuf);
                }
              }
            }
          }
          f.close();
          esp_task_wdt_reset();
          if ((++scanned % 50) == 0) {
#ifdef WEBRADIO_ENABLED
            if (!radioIsPlaying) {
#endif
              char msg[24];
              snprintf(msg, sizeof(msg), "Lese SD %d", scanned);
              display->DisplayText(msg, 32, 13, 255, 180, 0);
              Render();
#ifdef WEBRADIO_ENABLED
            }
#endif
          }
          f = dir.openNextFile();
        }
        dir.close();
        uint16_t newFiles = screensaverCount - countBefore;
        logMsg("LoadScreensaver: %d Dateien aus %s geladen", newFiles, sdPath.c_str());
        // FIX 11: Ordner-Cache speichern für nächsten Boot
        if (newFiles > 0) SaveFolderCache(sdPath, countBefore, newFiles);
      } else {
        logMsg("LoadScreensaver: Ordner nicht gefunden: %s — wird aus Konfiguration entfernt", sdPath.c_str());
        // Pfad aus screensaverPaths entfernen und neu speichern
        {
          String newPaths, rebuild = screensaverPaths;
          while (rebuild.length() > 0) {
            int c = rebuild.indexOf(',');
            String e = (c >= 0) ? rebuild.substring(0, c) : rebuild;
            rebuild  = (c >= 0) ? rebuild.substring(c + 1) : "";
            e.trim();
            if (e.length() == 0 || e == entry) continue;
            if (newPaths.length() > 0) newPaths += ",";
            newPaths += e;
          }
          screensaverPaths = newPaths;
          File pf = LittleFS.open("/screensaver_path.val", "w");
          if (pf) { pf.print(screensaverPaths); pf.close(); }
        }
      }
    }
    logMsg("LoadScreensaver: %d Dateien gesamt geladen", screensaverCount);
  }

  // Fallback 1 → LittleFS /screensaver
  if (screensaverCount == 0) {
    logMsg("LoadScreensaver: Fallback auf LittleFS");
    if (!LittleFS.exists("/screensaver")) LittleFS.mkdir("/screensaver");
    File dir = LittleFS.open("/screensaver");
    if (dir) {
      File f = dir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          const char* fname = f.name();
          if (fname && fname[0] != '.') {
            String name = String(fname);
            name.trim();
            if (name.endsWith(".raw") || name.endsWith(".gif") || name.endsWith(".GIF")) {
              addScreensaverFile("FS:/screensaver/" + name);
            }
          }
        }
        f.close();
        f = dir.openNextFile();
      }
      dir.close();
    }
  }
  // Fallback 2 → logo.raw/logoHD.raw wird in ScreenSaver() direkt geladen
done:
  logMsg("LoadScreensaver: %d Dateien, Shuffle=%s", screensaverCount, screensaverShuffle ? "ja" : "nein");
  if (screensaverShuffle && screensaverCount > 1) shuffleScreensaverFiles();
  else if (screensaverCount > 1) sortScreensaverFiles();
  // Neuen Puffer unter Mutex sichtbar machen — ab hier kann der Webserver lesen
  if (screensaverFilesMutex) {
    xSemaphoreTake(screensaverFilesMutex, portMAX_DELAY);
    screensaverIndex = 0;
    xSemaphoreGive(screensaverFilesMutex);
  } else {
    screensaverIndex = 0;
  }
}

// Listet alle Ordner auf der SD Karte
String GetSDFolders() {
  String json = "[";
  if (!sdCardAvailable) return "[]";

  File root = SD.open("/");
  if (!root) return "[]";
  if (!root.isDirectory()) { root.close(); return "[]"; }

  bool first = true;
  int scanned = 0;
  File f = root.openNextFile();
  while (f && scanned < 500) {
    scanned++;
    if (f.isDirectory()) {
      String name = String(f.name());
      name.trim();
      if (name.length() > 0 && !name.startsWith(".") && !name.startsWith("System")) {
        if (!first) json += ",";
        json += "\"" + name + "\"";
        first = false;
      }
    }
    f.close();
    yield();
    esp_task_wdt_reset();
    f = root.openNextFile();
  }
  if (f) f.close();
  root.close();
  json += "]";
  return json;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  esp_task_wdt_reset();  // delay(2000) hat WDT-Budget aufgebraucht — vor dem langen setup() zurücksetzen
  // PSRAM-Puffer und Mutexe früh allozieren — vor Audio/WiFi/Codec-Initialisierungen
  logBuffer             = (char (*)[LOG_LINE_LEN])heap_caps_calloc(
                            LOG_LINES, LOG_LINE_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uncompressBuffer      = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  screensaverFilesMutex = xSemaphoreCreateMutex();
  // String-Caches in PSRAM — einmalig allokiert, stoppt Heap-Fragmentierung im Dauerbetrieb
  cachedSDFolders     = (char*)heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  cachedGifAudioFiles = (char*)heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  cachedSdFiles       = (char*)heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (cachedSDFolders)     strcpy(cachedSDFolders,     "[]");
  if (cachedGifAudioFiles) strcpy(cachedGifAudioFiles,  "[]");
  if (cachedSdFiles)       strcpy(cachedSdFiles,        "[]");
  logMsg("=== ZeDMD booting ===");
  esp_log_level_set("*", ESP_LOG_NONE);

  // (Re-)Initialize global state variables that might have survived a restart
  // and that don't get set by Load() functions below.
  currentRenderBuffer = 0;
  lastRenderBuffer = NUM_RENDER_BUFFERS - 1;
  payloadCompressed = false;
  payloadSize = 0;
  payloadMissing = 0;
  headerBytesReceived = 0;
  command = 0;
  currentBuffer = NUM_BUFFERS - 1;
  lastBuffer = currentBuffer;
  processingBuffer = NUM_BUFFERS - 1;
  wifiActive = false;
  logoActive = true;
  transportActive = false;
  transportWaitCounter = 0;
  logoWaitCounter = 0;
  lastDataReceived = 0;
  serverRunning = false;
  ssid_length = 0;
  pwd_length = 0;
  ssid = "";
  pwd = "";
  port = 3333;

  uint64_t chipId = ESP.getEfuseMac();
  shortId =
      (uint16_t)(chipId ^ (chipId >> 16) ^ (chipId >> 32) ^ (chipId >> 48));

  bool fileSystemOK;
  logMsg("LittleFS.begin...");
  if (fileSystemOK = LittleFS.begin()) {
    logMsg("LittleFS OK");
    diagBoot();
    LoadSettingsMenu();
#ifndef ZEDMD_WIFI
    LoadTransport();
#endif
    LoadWiFiConfig();
    LoadUsbPackageSizeMultiplier();
#ifdef DISPLAY_LED_MATRIX
    LoadRgbOrder();
    LoadPanelSettings();
#endif
    LoadLum();
    LoadDebug();
    LoadIcons();
    LoadScreensaverLum();
    LoadScreensaverDuration();
    LoadScreensaverShuffle();
    LoadScreensaverStrictTimer();
    LoadGifAudioEnabled();
    LoadScreensaverMode();
    LoadClockColors();
#ifdef ZEDMD_WIFI
    LoadMqttConfig();
#endif
    LoadWeatherConfig();
    LoadTimezoneConfig();
#ifdef ZEDMD_WIFI
    weatherInit();
#endif
    LoadFavorites();
    LoadIgnore();
    esp_task_wdt_reset();  // Load*()-Kette + LittleFS-I/O kann >2s dauern
    InitSDCard();
    if (!sdCardAvailable) sdCardWarningPending = true;
    { String _f = GetSDFolders(); psramCacheSet(&cachedSDFolders, _f); }  // Cache beim Boot befüllen
    esp_task_wdt_reset();  // InitSDCard + GetSDFolders können auf langsamen Karten >1s dauern
    LoadScreensaverPaths();
    screensaverReloadNeeded = true;  // Dateien im Hauptloop laden (nach Display+WiFi Init)
    LoadUdpDelay();
#ifdef ZEDMD_HD_HALF
    LoadYOffset();
#endif
  } else {
    logMsg("LittleFS FAILED — Panel-Defaults gelten");
  }

  logMsg("Display init...");
#ifdef DISPLAY_RM67162_AMOLED
  display = new Rm67162Amoled();  // For AMOLED display
#elif defined(DISPLAY_LED_MATRIX)
  display = new LedMatrix();  // For LED matrix display
#endif
  logMsg("Display OK");
  display->SetBrightness(brightness);

  if (!fileSystemOK) {
    display->DisplayText("Error reading file system!", 0, 0, 255, 0, 0);
    display->DisplayText("Try to flash the firmware again.", 0, 6, 255, 0, 0);
    while (true);
  }

  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP: {
      display->DisplayText("An unrecoverable error happend!", 0, 0, 255, 0, 0);
      display->DisplayText("Coredump has been written. See", 0, 6, 255, 0, 0);
      display->DisplayText("ppuc.org/ZeDMD how to download", 0, 12, 255, 0, 0);
      display->DisplayText("it. Error code: ", 0, 18, 255, 0, 0);
      DisplayNumber(esp_reset_reason(), 2, 16 * 4, 18, 255, 0, 0);
      if (debug) {
        display->DisplayText("Reboot in 30 seconds ...", 0, 24, 255, 0, 0);
        for (uint8_t i = 29; i > 0; i--) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          DisplayNumber(i, 2, 40, 24, 255, 0, 0);
        }
        Restart();
      }
      break;
    }

    case ESP_RST_PWR_GLITCH: {
      display->DisplayText("A power glitch caused a restart!", 0, 0, 255, 0, 0);
      display->DisplayText("Check your power supply and", 0, 6, 255, 0, 0);
      display->DisplayText("hardware.", 0, 12, 255, 0, 0);
      display->DisplayText("Reboot in 30 seconds ...", 0, 24, 255, 0, 0);
      for (uint8_t i = 29; i > 0; i--) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        DisplayNumber(i, 2, 40, 24, 255, 0, 0);
      }
      Restart();
      break;
    }

    default:
      break;
  }

  for (uint8_t i = 0; i < NUM_RENDER_BUFFERS; i++) {
#ifdef BOARD_HAS_PSRAM
    renderBuffer[i] = (uint8_t *)heap_caps_malloc(
        TOTAL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
#else
    renderBuffer[i] = (uint8_t *)malloc(TOTAL_BYTES);
#endif
    if (nullptr == renderBuffer[i]) {
      display->DisplayText("out of memory", 0, 0, 255, 0, 0);
      while (1);
    }
    memset(renderBuffer[i], 0, TOTAL_BYTES);
  }

  // SD card warning NACH renderBuffer-Allokation — Render() braucht valide Buffer!
  if (sdCardWarningPending) {
    display->DisplayText("SD card not found!", 0, 0, 255, 80, 0);
    bool hasLittleFSFiles = LittleFS.exists("/screensaver");
    if (hasLittleFSFiles) {
      File dir = LittleFS.open("/screensaver");
      hasLittleFSFiles = false;
      if (dir) {
        File f = dir.openNextFile();
        if (f) { hasLittleFSFiles = true; f.close(); }
        dir.close();
      }
    }
    if (hasLittleFSFiles)
      display->DisplayText("Fallback: LittleFS aktiv", 0, 8, 255, 80, 0);
    else
      display->DisplayText("Check SD card and restart.", 0, 8, 255, 80, 0);
    Render();
    delay(4000);
    display->ClearScreen();
    Render();
  }

#ifndef DISPLAY_RM67162_AMOLED
  if (settingsMenu) {
    // Turn off settings menu after restart here.
    // Previously, the value has been set when selecting exit.
    // But this way, people who can't access the buttons in their cab
    // can leave the menu with a power cycle.
    settingsMenu = false;
    SaveSettingsMenu();

    RefreshSetupScreen();
    display->DisplayText("Exit", TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 16,
                         (TOTAL_HEIGHT / 2) + 4, 255, 191, 0);

    Bounce2::Button *forwardButton = new Bounce2::Button();
    forwardButton->attach(FORWARD_BUTTON_PIN, INPUT_PULLUP);
    forwardButton->interval(100);
    forwardButton->setPressedState(LOW);

    Bounce2::Button *upButton = new Bounce2::Button();
    upButton->attach(UP_BUTTON_PIN, INPUT_PULLUP);
    upButton->interval(100);
    upButton->setPressedState(LOW);

#ifdef ARDUINO_ESP32_S3_N16R8
    Bounce2::Button *backwardButton = new Bounce2::Button();
    backwardButton->attach(BACKWARD_BUTTON_PIN, INPUT_PULLUP);
    backwardButton->interval(100);
    backwardButton->setPressedState(LOW);

    Bounce2::Button *downButton = new Bounce2::Button();
    downButton->attach(DOWN_BUTTON_PIN, INPUT_PULLUP);
    downButton->interval(100);
    downButton->setPressedState(LOW);
#endif

    uint8_t position = 1;
    while (1) {
      forwardButton->update();
      bool forward = forwardButton->pressed();
      bool backward = false;
#ifdef ARDUINO_ESP32_S3_N16R8
      backwardButton->update();
      backward = backwardButton->pressed();
#endif
      if (forward || backward) {
#ifdef ZEDMD_HD_HALF
        if (forward && ++position > 8)
          position = 1;
        else if (backward && --position < 1)
          position = 8;
#else
        if (forward && ++position > 7)
          position = 1;
        else if (backward && --position < 1)
          position = 7;
#endif
        switch (position) {
          case 1: {  // Exit
            RefreshSetupScreen();
            display->DisplayText("Exit",
                                 TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 16,
                                 (TOTAL_HEIGHT / 2) + 4, 255, 191, 0);
            break;
          }
          case 2: {  // Brightness
            RefreshSetupScreen();
            DisplayLum(255, 191, 0);
            break;
          }
          case 3: {  // USB Package Size
            RefreshSetupScreen();
            display->DisplayText("USB Packet Size:", 7 * (TOTAL_WIDTH / 128),
                                 (TOTAL_HEIGHT / 2) + 4, 255, 191, 0);
            break;
          }
          case 4: {  // Transport
            RefreshSetupScreen();
            display->DisplayText(
                transport == TRANSPORT_USB
                    ? "USB     "
                    : (transport == TRANSPORT_WIFI_UDP
                           ? "WiFi UDP"
                           : (transport == TRANSPORT_WIFI_TCP ? "WiFi TCP"
                                                              : "SPI     ")),
                7 * (TOTAL_WIDTH / 128), (TOTAL_HEIGHT / 2) - 3, 255, 191, 0);
            break;
          }
          case 5: {  // Debug
            RefreshSetupScreen();
            display->DisplayText("Debug:", 7 * (TOTAL_WIDTH / 128),
                                 (TOTAL_HEIGHT / 2) - 10, 255, 191, 0);
            break;
          }
          case 6: {  // RGB order
            RefreshSetupScreen();
            DisplayRGB(255, 191, 0);
            break;
          }
          case 7: {  // UDP Delay
            RefreshSetupScreen();
            display->DisplayText(
                "UDP Delay:",
                TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - (11 * 4),
                (TOTAL_HEIGHT / 2) - 3, 255, 191, 0);
            break;
          }
#ifdef ZEDMD_HD_HALF
          case 8: {  // Y Offset
            RefreshSetupScreen();
            display->DisplayText("Y-Offset",
                                 TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 32,
                                 (TOTAL_HEIGHT / 2) - 10, 255, 191, 0);
            break;
          }
#endif
        }
      }

      upButton->update();
      bool up = upButton->pressed();
      bool down = false;
#ifdef ARDUINO_ESP32_S3_N16R8
      downButton->update();
      down = downButton->pressed();
#endif
      if (up || down) {
        switch (position) {
          case 1: {  // Exit
            Restart();
            break;
          }
          case 2: {  // Brightness
            if (up && ++brightness > 15)
              brightness = 1;
            else if (down && --brightness < 1)
              brightness = 15;

            display->SetBrightness(brightness);
            DisplayLum(255, 191, 0);
            SaveLum();
            break;
          }
          case 3: {  // USB Package Size
            if (up && ++usbPackageSizeMultiplier > 60)
              usbPackageSizeMultiplier = 1;
            else if (down && --usbPackageSizeMultiplier < 1)
              usbPackageSizeMultiplier = 60;

            DisplayNumber(usbPackageSizeMultiplier * 32, 4,
                          7 * (TOTAL_WIDTH / 128) + (16 * 4),
                          (TOTAL_HEIGHT / 2) + 4, 255, 191, 0);
            SaveUsbPackageSizeMultiplier();
            break;
          }
          case 4: {  // Transport
            if (up && ++transport > TRANSPORT_SPI)
              transport = TRANSPORT_USB;
            else if (down && --transport < TRANSPORT_USB)
              transport = TRANSPORT_SPI;
            display->DisplayText(
                transport == TRANSPORT_USB
                    ? "USB     "
                    : (transport == TRANSPORT_WIFI_UDP
                           ? "WiFi UDP"
                           : (transport == TRANSPORT_WIFI_TCP ? "WiFi TCP"
                                                              : "SPI     ")),
                7 * (TOTAL_WIDTH / 128), (TOTAL_HEIGHT / 2) - 3, 255, 191, 0);
            SaveTransport();
            break;
          }
          case 5: {  // Debug
            if (++debug > 1) debug = 0;
            DisplayNumber(debug, 1, 7 * (TOTAL_WIDTH / 128) + (6 * 4),
                          (TOTAL_HEIGHT / 2) - 10, 255, 191, 0);
            SaveDebug();
            break;
          }
          case 6: {  // RGB order
            if (rgbModeLoaded != 0) {
              rgbMode = 0;
              SaveRgbOrder();
              delay(10);
              Restart();
            }
            if (up && ++rgbMode > 5)
              rgbMode = 0;
            else if (down && --rgbMode < 0)
              rgbMode = 5;
            RefreshSetupScreen();
            DisplayRGB(255, 191, 0);
            SaveRgbOrder();
            break;
          }
          case 7: {  // UDP Delay
            if (up && ++udpDelay > 9)
              udpDelay = 0;
            else if (down && udpDelay == 0)
              udpDelay = 9;
            else if (down)
              --udpDelay;

            DisplayNumber(udpDelay, 1,
                          TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 4,
                          (TOTAL_HEIGHT / 2) - 3, 255, 191, 0);
            SaveUdpDelay();
            break;
          }
#ifdef ZEDMD_HD_HALF
          case 8: {  // Y-Offset
            if (up && ++yOffset > 32)
              yOffset = 0;
            else if (down && --yOffset < 0)
              yOffset = 32;
            ClearScreen();
            RefreshSetupScreen();
            display->DisplayText("Y-Offset",
                                 TOTAL_WIDTH - (7 * (TOTAL_WIDTH / 128)) - 32,
                                 (TOTAL_HEIGHT / 2) - 10, 255, 191, 0);
            SaveYOffset();
            break;
          }
#endif
        }
      }

      delay(1);
    }
  }
#endif

  pinMode(FORWARD_BUTTON_PIN, INPUT_PULLUP);

  DisplayLogo();
  // DisplayId();

  // Create synchronization primitives
  for (uint8_t i = 0; i < NUM_BUFFERS; i++) {
#ifdef BOARD_HAS_PSRAM
    buffers[i] = (uint8_t *)heap_caps_malloc(
        BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
#else
    buffers[i] = (uint8_t *)malloc(BUFFER_SIZE);
#endif
    if (nullptr == buffers[i]) {
      display->DisplayText("out of memory", 0, 0, 255, 0, 0);
      while (1);
    }
  }

  switch (transport) {
    case TRANSPORT_USB: {
#ifdef BOARD_HAS_PSRAM
      xTaskCreatePinnedToCore(Task_ReadSerial, "Task_ReadSerial", 8192, NULL, 1,
                              NULL, 0);
#else
      xTaskCreatePinnedToCore(Task_ReadSerial, "Task_ReadSerial", 4096, NULL, 1,
                              NULL, 0);
#endif
      break;
    }

    case TRANSPORT_WIFI_UDP:
    case TRANSPORT_WIFI_TCP: {
      StartWiFi();
      clockInit();  // NTP nach WiFi Start
#ifdef WEBRADIO_ENABLED
      radioInit();
#endif
      mqttClient.setServer(mqttServer.c_str(), mqttPort);
      mqttClient.setCallback(onMqttMessage);
      mqttClient.setBufferSize(2048);
      vTaskDelay(pdMS_TO_TICKS(1000));  // WiFi-Stack nach Verbindungsaufbau stabilisieren
      mqttConnect();
      xTaskCreatePinnedToCore(mqttTask, "mqttTask", 8192, NULL, 1, NULL, 0);
      break;
    }

    case TRANSPORT_SPI: {
      display->DisplayText("SPI connection failure ...", 0, 0, 255, 0, 0);
      delay(5000);
      display->DisplayText("Is the SPI interface turned on?", 0, 6, 255, 0, 0);
      delay(5000);
      display->DisplayText("Your SPI cable might be too long", 0, 12, 255, 0,
                           0);
      delay(5000);
      display->DisplayText("No, your SPI cable is too short!", 0, 18, 255, 0,
                           0);
      delay(5000);
      display->DisplayText("SPI is not implemented yet!", 0, 24, 255, 191, 0);
      while (digitalRead(FORWARD_BUTTON_PIN));
      settingsMenu = true;
      SaveSettingsMenu();
      delay(20);
      Restart();
      break;
    }
  }
}

void loop() {
  esp_task_wdt_reset();

  // Uptime für Bootloop-Schutz in RTC halten — kein Flash-Write, µs-Overhead
  {
    static uint32_t lastUptimeUpdate = 0;
    uint32_t nowSec = millis() / 1000;
    if (nowSec - lastUptimeUpdate >= 5) {
      lastUptimeUpdate = nowSec;
      rtcLastUptime    = nowSec;
    }
  }

  CheckMenuButton();

  // Screensaver Dateien neu laden wenn Pfad geändert wurde
  // screensaverLoadRunning verhindert Re-Entrant-Aufruf bei schnellen Pfadwechseln
  if (screensaverReloadNeeded && !screensaverLoadRunning) {
    screensaverReloadNeeded = false;
    screensaverLoadRunning  = true;
    SaveScreensaverPaths();
    display->DisplayText("Lade...   ", 52, 13, 180, 180, 180);
    Render();
    LoadScreensaverFiles();
    screensaverLoadRunning = false;
    if (screensaverCount > 0 && sdCardAvailable) {
      display->DisplayText("SD OK     ", 40, 13, 0, 255, 100);
    } else {
      display->DisplayText("LittleFS  ", 40, 13, 180, 180, 180);
    }
    Render();
    vTaskDelay(pdMS_TO_TICKS(800));
  }

  // SD Karte refreshen und Cache aktualisieren
  if (sdRefreshNeeded) {
    sdRefreshNeeded = false;
#ifdef SD_MMC_BUILD
    SD_MMC.end();
#else
    SD.end();
    spiSD.end();
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
    InitSDCard();
    { String _f = GetSDFolders(); psramCacheSet(&cachedSDFolders, _f); }
  }

  // GIF-Audio-Dateiliste cachen (statt SD-I/O im Webserver-Callback)
  if (gifAudioRefreshNeeded && sdCardAvailable) {
    gifAudioRefreshNeeded = false;
    if (!TryLoadGifAudioCache()) {
      logMsg("GifAudio: scanne %s ...", GIF_AUDIO_DIR);
      String json = "[";
      if (!SD.exists(GIF_AUDIO_DIR)) SD.mkdir(GIF_AUDIO_DIR);
      File dir = SD.open(GIF_AUDIO_DIR);
      bool first = true;
      uint16_t gifAudioCount = 0;
      bool cancelled = false;
      while (File f = dir.openNextFile()) {
        if (cancelSdScan) {
          f.close();
          cancelled = true;
          cancelSdScan = false;
          break;
        }
        String name = String(f.name());
        uint32_t sz = f.size();
        f.close();
        if (!name.startsWith(".") && !name.endsWith(".tmp")) {
          if (!first) json += ",";
          json += "{\"name\":\"" + name + "\",\"size\":" + String(sz) + "}";
          first = false;
          gifAudioCount++;
          if ((gifAudioCount % 50) == 0) {
#ifdef WEBRADIO_ENABLED
            if (!radioIsPlaying) {
#endif
              char msg[24];
              snprintf(msg, sizeof(msg), "GifAudio %d", gifAudioCount);
              display->DisplayText(msg, 20, 13, 255, 180, 0);
              Render();
#ifdef WEBRADIO_ENABLED
            }
#endif
          }
        }
        esp_task_wdt_reset();
      }
      dir.close();
      if (cancelled) {
        logMsg("GifAudio: Scan abgebrochen nach %d Dateien", gifAudioCount);
      } else {
        json += "]";
        psramCacheSet(&cachedGifAudioFiles, json);
        logMsg("GifAudio: %d Dateien gefunden, Cache gespeichert", gifAudioCount);
        SaveGifAudioCache();
      }
    }
  }

  // Cache-Invalidierungen aus Webserver-Task (eject, Ordnerwechsel) — nur loop() darf psramCacheSet aufrufen
  if (sdFoldersInvalidateNeeded) {
    sdFoldersInvalidateNeeded = false;
    psramCacheSet(&cachedSDFolders, String("[]"));
    sdFoldersRefreshNeeded = true;
  }
  if (sdFoldersRefreshNeeded && sdCardAvailable) {
    sdFoldersRefreshNeeded = false;
    psramCacheSet(&cachedSDFolders, GetSDFolders());
  }
  if (sdFilesInvalidateNeeded) {
    sdFilesInvalidateNeeded = false;
    psramCacheSet(&cachedSdFiles, String("[]"));
  }

  if (iconsReloadNeeded) {
    iconsReloadNeeded = false;
    LoadIcons();
  }

  // SD-Dateiliste für gewählten Ordner cachen
  if (sdFilesRefreshNeeded && sdCardAvailable && cachedSdFilesFolder[0] != '\0') {
    sdFilesRefreshNeeded = false;
    String json = "[";
    File dir = SD.open(cachedSdFilesFolder);
    if (dir && dir.isDirectory()) {
      bool first = true;
      uint16_t fileCount = 0;
      File f = dir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          const char* fname = f.name();
          if (fname && fname[0] != '.') {
            if (!first) json += ",";
            json += "\"" + String(fname) + "\"";
            first = false;
          }
        }
        if ((++fileCount % 50) == 0) esp_task_wdt_reset();
        f.close();
        f = dir.openNextFile();
      }
      dir.close();
    }
    json += "]";
    psramCacheSet(&cachedSdFiles, json);
  }

  // WiFi Reconnect wenn Verbindung verloren — max. alle 5s, kein Delay/Return damit
  // Screensaver und GIF-Wiedergabe während des Reconnects weiterlaufen
  static uint32_t lastReconnectMs = 0;
  if (wifiActive && WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if (now - lastReconnectMs >= 5000) {
      lastReconnectMs = now;
      WiFi.reconnect();
    }
  }


  if (!transportActive) {
    static bool weatherIconTestRendered = false;
    if (weatherIconTestActive) {
      if (!weatherIconTestRendered) {
        weatherIconTest();
        weatherIconTestRendered = true;
      }
      return;
    }
    weatherIconTestRendered = false;

    if (wifiActive && !serverRunning) {
      // @see https://github.com/ESP32Async/ESPAsyncWebServer/issues/21
      // StartServer();
    }

    if (!logoActive) {
      logoActive = true;
      logoWaitCounter = 199;
    }

    ++logoWaitCounter;

#ifdef ZEDMD_WIFI
    if (25 == logoWaitCounter) {   // WiFi: 5s Logo
      DisplayUpdate();
    }
    if (50 == logoWaitCounter) {   // WiFi: 5s PPUC → Screensaver
#else
    if (125 == logoWaitCounter) {  // USB: 25s Logo
      DisplayUpdate();
    }
    if (250 == logoWaitCounter) {  // USB: 25s PPUC → Screensaver
#endif
      screensaverIndex = 0;
      screensaverRAWShowStart = 0;
      if (screensaverCount > 0) {
        String firstFile = String(screensaverFiles[0]);
        if (firstFile.endsWith(".gif") || firstFile.endsWith(".GIF")) {
          display->SetBrightness(screensaverBrightness);
          bool isMixedMode = (screensaverMode == 2 || screensaverMode == 4);
          uint32_t endTime = screensaverStrictTimer ? (millis() + (uint32_t)screensaverDuration * 1000) : 0;
          bool loopUntilEnd = screensaverStrictTimer || (screensaverPaused && !isMixedMode);
          PlayGIF(firstFile, endTime, true, loopUntilEnd);
          if (transportActive) return;
          if (screensaverReloadNeeded) return;
          screensaverIndex = nextScreensaverIndex();
          String nextFile = String(screensaverFiles[screensaverIndex]);
          if (!nextFile.endsWith(".gif") && !nextFile.endsWith(".GIF")) {
            ScreenSaver();
          }
        } else {
          ScreenSaver();
        }
      } else {
        ScreenSaver();
      }
    }

#ifdef ZEDMD_WIFI
    uint16_t ssThreshold = 50;
#else
    uint16_t ssThreshold = 250;
#endif


    // ── Screensaver / Clock Logik ─────────────────────────────────────────
    if (logoWaitCounter > ssThreshold) {

#ifdef WEBRADIO_ENABLED
      // Auto-Aus wenn Timer abgelaufen
      if (radioDisplayActive && radioDisplayUntil > 0 && millis() >= radioDisplayUntil) {
        radioDisplayActive = false;
        radioDisplayUntil  = 0;
      }
      if (radioIsPlaying && radioDisplayActive) {
        DisplayRadio();
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
      }
#endif

      // Direkt angewähltes GIF/RAW sofort abspielen (vor allen Modus-Checks)
      if (forcePlayPending && forcePlayFile.length() > 0) {
        forcePlayPending = false;
        String f = forcePlayFile;
        forcePlayFile = "";
        currentlyPlayingFile = f;  // für screensaver_current Endpoint
        for (uint16_t i = 0; i < screensaverCount; i++) {
          if (String(screensaverFiles[i]) == f) { screensaverIndex = i; break; }
        }
        display->SetBrightness(screensaverBrightness);
        if (f.endsWith(".gif") || f.endsWith(".GIF")) {
          PlayGIF(f, 0, true, false);
        } else {
          // RAW-Datei direkt anzeigen
          File rawF;
          if (f.startsWith("SD:"))       rawF = SD.open(f.substring(3), "r");
          else if (f.startsWith("FS:"))  rawF = LittleFS.open(f.substring(3), "r");
          else                           rawF = LittleFS.open(f, "r");
          if (rawF) {
            rawF.read(renderBuffer[currentRenderBuffer], TOTAL_BYTES);
            rawF.close();
            Render();
          }
        }
        if (transportActive) return;
        if (screensaverReloadNeeded) return;
      }

      // Display-Text (timed, optional scrollend) — unterbricht alle Modi
      if (displayTextActive) {
        if (millis() >= displayTextEnd) {
          displayTextActive = false;
          ClearScreen();
          Render();
        } else {
          // On first activation: zero both renderBuffers so the differential
          // Render() starts from a clean state (no GIF residue).
          if (displayTextNeedsClear) {
            display->ClearScreen();
            for (int i = 0; i < NUM_RENDER_BUFFERS; i++)
              memset(renderBuffer[i], 0, TOTAL_BYTES);
            displayTextNeedsClear = false;
          }

          if (displayTextScroll) {
            display->RenderTextGFXToBuffer(renderBuffer[currentRenderBuffer],
                                           displayTextContent, displayTextScrollX,
                                           displayTextR, displayTextG, displayTextB);
            Render();
            int16_t textW = (int16_t)display->GetTextGFXWidth(displayTextContent);
            if (--displayTextScrollX < -textW)
              displayTextScrollX = TOTAL_WIDTH;
          } else {
            uint16_t textW = display->GetTextGFXWidth(displayTextContent);
            int16_t xPos   = ((int16_t)TOTAL_WIDTH - (int16_t)textW) / 2;
            if (xPos < 0) xPos = 0;
            display->RenderTextGFXToBuffer(renderBuffer[currentRenderBuffer],
                                           displayTextContent, xPos,
                                           displayTextR, displayTextG, displayTextB);
            Render();
          }
          vTaskDelay(pdMS_TO_TICKS(displayTextScroll ? 20 : 200));
          return;
        }
      }

      // Modus 1: Clock only
      if (screensaverMode == 1) {
        clockDisplay();
        vTaskDelay(pdMS_TO_TICKS(radioIsPlaying ? 200 : 1000));
        return;
      }

      // Modus 2: Clock + Screensaver (im Wechsel, jeweils screensaverDuration Sekunden)
      static uint32_t phaseStartTime = 0;
      static bool showingClock = false;
      if (screensaverMode == 2) {
        uint32_t now = millis();
        if (phaseStartTime == 0) phaseStartTime = now;
        if ((now - phaseStartTime) >= (uint32_t)screensaverDuration * 1000) {
          showingClock = !showingClock;  // GIF ↔ Uhr
          phaseStartTime = now;
          if (showingClock) clockColorChanged = true;
        }
        if (showingClock) {
          clockDisplay();
          vTaskDelay(pdMS_TO_TICKS(radioIsPlaying ? 200 : 1000));
          return;
        }
      }

      // Modus 3: Uhr + Wetter gleichzeitig
      if (screensaverMode == 3) {
        uint32_t now = millis();
        uint32_t weatherInterval = forecastAvailable ? (15UL * 60UL * 1000UL) : (2UL * 60UL * 1000UL);
        if (lastWeatherFetch == 0 ? (now > 30000UL) : ((now - lastWeatherFetch) >= weatherInterval)) {
          weatherTrigger();
        }
        if (forecastAvailable) {
          if (weatherPhaseStart == 0) weatherPhaseStart = now;
          if ((now - weatherPhaseStart) >= 15000UL) {
            weatherPage = (weatherPage == 0) ? 1 : 0;
            weatherPhaseStart = now;
            clockColorChanged = true;
          }
        }
        if (weatherPage == 1) {
          weatherDisplayForecast();
        } else {
          weatherDisplayClock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
      }

      // Modus 4: Uhr+Wetter + Screensaver (im Wechsel)
      if (screensaverMode == 4) {
        uint32_t now = millis();
        uint32_t weatherInterval = forecastAvailable ? (15UL * 60UL * 1000UL) : (2UL * 60UL * 1000UL);
        if (lastWeatherFetch == 0 ? (now > 30000UL) : ((now - lastWeatherFetch) >= weatherInterval)) {
          weatherTrigger();
        }
        if (weatherPhaseStart == 0) weatherPhaseStart = now;
        if ((now - weatherPhaseStart) >= (uint32_t)screensaverDuration * 1000) {
          if (!forecastAvailable) {
            weatherPage = (weatherPage == 0) ? 2 : 0;
          } else {
            weatherPage = (weatherPage + 1) % 3;  // 0→1→2→0
          }
          weatherPhaseStart = now;
          clockColorChanged = true;
        }
        if (weatherPage == 0) {
          weatherDisplayClock();
          vTaskDelay(pdMS_TO_TICKS(1000));
          return;
        }
        if (weatherPage == 1) {
          weatherDisplayForecast();
          vTaskDelay(pdMS_TO_TICKS(1000));
          return;
        }
        // weatherPage == 2: Screensaver-Phase fällt durch zum GIF-Code unten
      }

      // Direkt angewähltes GIF/RAW sofort abspielen
      if (forcePlayPending && forcePlayFile.length() > 0) {
        forcePlayPending = false;
        String f = forcePlayFile;
        forcePlayFile = "";
        currentlyPlayingFile = f;  // für screensaver_current Endpoint
        for (uint16_t i = 0; i < screensaverCount; i++) {
          if (String(screensaverFiles[i]) == f) { screensaverIndex = i; break; }
        }
        display->SetBrightness(screensaverBrightness);
        if (f.endsWith(".gif") || f.endsWith(".GIF")) {
          PlayGIF(f, 0, true, false);
        } else {
          File rawF;
          if (f.startsWith("SD:"))       rawF = SD.open(f.substring(3), "r");
          else if (f.startsWith("FS:"))  rawF = LittleFS.open(f.substring(3), "r");
          else                           rawF = LittleFS.open(f, "r");
          if (rawF) {
            rawF.read(renderBuffer[currentRenderBuffer], TOTAL_BYTES);
            rawF.close();
            Render();
          }
        }
        if (transportActive) return;
        if (screensaverReloadNeeded) return;
      }

      // Modus 0, 2 oder 4 (Screensaver Teil): GIF/RAW abspielen
      if (screensaverCount > 0) {
        currentlyPlayingFile = "";  // normaler Screensaver übernimmt
        String currentFile = String(screensaverFiles[screensaverIndex]);
        if (currentFile.endsWith(".gif") || currentFile.endsWith(".GIF")) {
          display->SetBrightness(screensaverBrightness);
          bool isMixedMode = (screensaverMode == 2 || screensaverMode == 4);
          // Paused+nicht-gemischt: endlos loopen; sonst immer duration-Sekunden loopen
          // Strict: kann GIF mid-frame abschneiden; Non-strict: gleich, aber GIF läuft mind. einmal durch
          uint32_t endTime = (screensaverPaused && !isMixedMode) ? 0 : (millis() + (uint32_t)screensaverDuration * 1000);
          bool loopUntilEnd = true;
          PlayGIF(currentFile, endTime, true, loopUntilEnd);
          if (transportActive) return;
          if (screensaverReloadNeeded) return;
          if (screensaverMode == 4) {
            weatherPhaseStart = millis();
            weatherPage = 0;
            clockColorChanged = true;
          }
          if (!screensaverPaused) {
            screensaverRAWShowStart = 0;
            screensaverIndex = nextScreensaverIndex();
            String nextFile = String(screensaverFiles[screensaverIndex]);
            if (!nextFile.endsWith(".gif") && !nextFile.endsWith(".GIF")) {
              ScreenSaver();
            }
          }
        } else {
          if (screensaverRAWShowStart == 0) screensaverRAWShowStart = millis();
          if (!screensaverPaused &&
              (millis() - screensaverRAWShowStart) >= (uint32_t)screensaverDuration * 1000) {
            screensaverRAWShowStart = 0;
            screensaverIndex = nextScreensaverIndex();
            ScreenSaver();
            if (screensaverMode == 4) {
              weatherPhaseStart = millis();
              weatherPage = 0;
              clockColorChanged = true;
            }
          }
        }
      } else if (screensaverMode == 0) {
        // Kein Screensaver File → Fallback logo.raw läuft in ScreenSaver()
      }
    }
    // Throbber deaktiviert — Screensaver zeigt Bilder/GIFs
    // display->DrawPixel(TOTAL_WIDTH - 3, TOTAL_HEIGHT - 3, throbberColors[0],
    //                    throbberColors[1], throbberColors[2]);

    // switch (transportWaitCounter) {
    //   case 0: ... case 7: ...
    // }

    transportWaitCounter = (transportWaitCounter + 1) % 8;

    vTaskDelay(pdMS_TO_TICKS(200));
  } else {
    // if (wifiActive && serverRunning) {
    //  @see https://github.com/ESP32Async/ESPAsyncWebServer/issues/21
    //  server->end();
    //  delete server;
    //  server = nullptr;
    //  serverRunning = false;
    //}

    if (lastDataReceived > 0 &&
        (millis() - lastDataReceived) > CONNECTION_TIMEOUT) {
      transportActive = false;
      return;
    }

    if (logoActive) {
      display->SetBrightness(brightness);
      ClearScreen();
      logoActive = false;
    }

    if (AcquireNextProcessingBuffer()) {
      if (2 == bufferSizes[processingBuffer] &&
          255 == buffers[processingBuffer][0] &&
          255 == buffers[processingBuffer][1]) {
#if defined(BOARD_HAS_PSRAM) && (NUM_RENDER_BUFFERS > 1)
        Render();
#endif
      } else if (2 == bufferSizes[processingBuffer] &&
                 0 == buffers[processingBuffer][0] &&
                 0 == buffers[processingBuffer][1]) {
        ClearScreen();
      } else {
        if (bufferCompressed[processingBuffer]) {
          memset(uncompressBuffer, 0, 2048);
          uncompressedBufferSize = 2048;
          int minizStatus = mz_uncompress2(
              uncompressBuffer, &uncompressedBufferSize,
              buffers[processingBuffer], &bufferSizes[processingBuffer]);

          if (MZ_OK != minizStatus) {
            if (1 == debug) {
              display->DisplayText("miniz error: ", 0, 0, 255, 0, 0);
              DisplayNumber(minizStatus, 3, 13 * 4, 0, 255, 0, 0);
              display->DisplayText("free heap: ", 0, 6, 255, 0, 0);
              DisplayNumber(esp_get_free_heap_size(), 8, 11 * 4, 6, 255, 0, 0);
              while (1);
            }
            return;
          }
        } else {
          uncompressedBufferSize = bufferSizes[processingBuffer];
          memcpy(uncompressBuffer, buffers[processingBuffer],
                 uncompressedBufferSize);
        }

        uint16_t uncompressedBufferPosition = 0;
        while (uncompressedBufferPosition < uncompressedBufferSize) {
          if (uncompressBuffer[uncompressedBufferPosition] >= 128) {
#if defined(BOARD_HAS_PSRAM) && (NUM_RENDER_BUFFERS > 1)
            const uint8_t idx =
                uncompressBuffer[uncompressedBufferPosition++] - 128;
            const uint8_t yOffset = (idx / ZONES_PER_ROW) * ZONE_HEIGHT;
            const uint8_t xOffset = (idx % ZONES_PER_ROW) * ZONE_WIDTH;
            for (uint8_t y = 0; y < ZONE_HEIGHT; y++) {
              memset(&renderBuffer[currentRenderBuffer]
                                  [((yOffset + y) * TOTAL_WIDTH + xOffset) * 3],
                     0, ZONE_WIDTH * 3);
            }
#else
            display->ClearZone(uncompressBuffer[uncompressedBufferPosition++] -
                               128);
#endif
          } else {
#if defined(BOARD_HAS_PSRAM) && (NUM_RENDER_BUFFERS > 1)
            uint8_t idx = uncompressBuffer[uncompressedBufferPosition++];
            const uint8_t yOffset = (idx / ZONES_PER_ROW) * ZONE_HEIGHT;
            const uint8_t xOffset = (idx % ZONES_PER_ROW) * ZONE_WIDTH;

            for (uint8_t y = 0; y < ZONE_HEIGHT; y++) {
              for (uint8_t x = 0; x < ZONE_WIDTH; x++) {
                const uint16_t rgb565 =
                    uncompressBuffer[uncompressedBufferPosition++] +
                    (((uint16_t)uncompressBuffer[uncompressedBufferPosition++])
                     << 8);
                uint8_t rgb888[3];
                rgb888[0] = (rgb565 >> 8) & 0xf8;
                rgb888[1] = (rgb565 >> 3) & 0xfc;
                rgb888[2] = (rgb565 << 3);
                rgb888[0] |= (rgb888[0] >> 5);
                rgb888[1] |= (rgb888[1] >> 6);
                rgb888[2] |= (rgb888[2] >> 5);
                memcpy(
                    &renderBuffer[currentRenderBuffer]
                                 [((yOffset + y) * TOTAL_WIDTH + xOffset + x) *
                                  3],
                    rgb888, 3);
              }
            }
#else
            display->FillZoneRaw565(
                uncompressBuffer[uncompressedBufferPosition++],
                &uncompressBuffer[uncompressedBufferPosition]);
            uncompressedBufferPosition += RGB565_ZONE_SIZE;
#endif
          }
        }
      }
    } else {
      // Avoid busy-waiting
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

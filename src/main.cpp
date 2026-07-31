
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
  // SD card SPI pins
  #ifdef CONFIG_IDF_TARGET_ESP32S3
    // ESP32-S3 pins (HUB75 occupies other pins) — overridable via build_flags
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
    // Standard ESP32 USB build — no SD card
    #define SD_MOSI 18
    #define SD_MISO 21
    #define SD_SCK  2
    #define SD_CS   33
  #endif
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
#include <time.h>  // NTP / time

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
uint8_t* uncompressBuffer = nullptr;  // allocated in PSRAM (setup)
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

static char (*logBuffer)[LOG_LINE_LEN] = nullptr;  // allocated in PSRAM (setup)
static uint8_t logHead = 0;
static uint8_t logCount = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// RTC memory: survives Watchdog/Exception resets
RTC_DATA_ATTR static char rtcLog[RTC_LOG_LINES][RTC_LINE_LEN];
RTC_DATA_ATTR static uint8_t rtcLogHead = 0;
RTC_DATA_ATTR static uint8_t rtcLogCount = 0;
RTC_DATA_ATTR static bool     rtcLogValid   = false;
RTC_DATA_ATTR static uint32_t rtcLastUptime = 0;   // seconds of last stable run; 0 after POWERON

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
  // Also write to RTC memory
  strncpy(rtcLog[rtcLogHead], line, RTC_LINE_LEN - 1);
  rtcLog[rtcLogHead][RTC_LINE_LEN - 1] = '\0';
  rtcLogHead = (rtcLogHead + 1) % RTC_LOG_LINES;
  if (rtcLogCount < RTC_LOG_LINES) rtcLogCount++;
  rtcLogValid = true;
  portEXIT_CRITICAL(&logMux);
}
// ── Crash diagnostics (LittleFS, consistent with settings pattern) ───────────

// File-scope: no C++ guard needed, safe after PANIC reset
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

// Call directly after LittleFS.begin() — before LoadSettingsMenu() and the
// esp_reset_reason() switch in setup(). On dump the RTC buffer contains
// exactly 1 new "=== ZeDMD booting ===" entry; the rest is pre-crash log.
static void diagBoot() {
  // static: no stack pressure after PANIC (stack might be partially corrupted)
  esp_task_wdt_reset();  // 3× flash writes (crash log + diag.json) can take >2s on fragmented LFS
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

  // ── Save pre-crash state BEFORE logMsg() — dump condition is based on it ──
  bool    preLogValid = rtcLogValid;
  uint8_t preLogCount = rtcLogCount;
  uint8_t preLogHead  = rtcLogHead;
  // zero rtcLastUptime immediately — bootloop guard: next reboot sees 0
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

  // Dump only if: valid pre-crash log, no clean reboot.
  // Bootloop guard (lastUp > 60) only for BROWNOUT/PWR_GLITCH — real crashes
  // (PANIC, WDT) always dump, even if the previous boot was short.
  bool isCleanReset = (reason == ESP_RST_POWERON ||
                       reason == ESP_RST_SW       ||
                       reason == ESP_RST_DEEPSLEEP);
  bool isBrownout   = (reason == ESP_RST_BROWNOUT ||
                       reason == ESP_RST_PWR_GLITCH);
  bool isPanicOrWdt = !isCleanReset && !isBrownout;
  // PANIC/WDT: always dump (even with only 1 log line — early crash)
  // BROWNOUT/other: only dump if enough log present and no short boot
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

  esp_task_wdt_reset();  // Between crash-log write and diag.json write — both together >5s on fragmented LFS
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
volatile bool transportActive;  // volatile: set by Task_ReadSerial (Core 1)!
uint8_t transportWaitCounter;
uint16_t logoWaitCounter;
uint32_t lastDataReceived;
bool serverRunning;
uint8_t throbberColors[6] __attribute__((aligned(4))) = {0};
mz_ulong uncompressedBufferSize = 2048;
uint16_t shortId;
// Screensaver — paths dynamically in PSRAM, no fixed limit
SemaphoreHandle_t screensaverFilesMutex = nullptr;
char (*screensaverFiles)[128] = nullptr;
uint16_t screensaverFilesCapacity = 0;
uint16_t screensaverCount = 0;
uint16_t screensaverIndex = 0;
uint32_t screensaverRAWShowStart = 0;
uint8_t screensaverBrightness = 3;
uint8_t screensaverDuration = 10;  // display duration in seconds, default 10
bool screensaverShuffle = false;
bool screensaverStrictTimer = true;
volatile bool sdCardAvailable = false;
bool sdCardWarningPending = false;
uint64_t sdTotalBytes = 0;
uint32_t lfsTotal = 0;
uint32_t lfsUsed  = 0;
uint64_t sdUsedBytes = 0;
volatile bool screensaverReloadNeeded  = false;
volatile bool screensaverLoadRunning   = false;  // background task running
volatile bool sdRefreshNeeded = false;
// Cache buffers in PSRAM — dynamically allocated, atomic swap prevents null window
#define CACHE_SD_FOLDER_SIZE     256
static char* cachedSDFolders     = nullptr;
static char* cachedGifAudioFiles  = nullptr;
static char* cachedSdFiles        = nullptr;
static char  cachedSdFilesFolder[CACHE_SD_FOLDER_SIZE] = "";
volatile bool gifAudioRefreshNeeded       = false;
volatile bool weatherIconTestActive       = false;
volatile bool weatherSmallIconTestActive  = false;
volatile bool iconsReloadNeeded           = false;
volatile bool sdFilesRefreshNeeded        = false;
volatile bool sdFoldersInvalidateNeeded   = false;
volatile bool sdFoldersRefreshNeeded      = false;
volatile bool sdFilesInvalidateNeeded     = false;
bool gifAudioEnabled = true;
bool screensaverPaused = false;  // Screensaver Pause/Play
volatile uint32_t setupScreenUntil = 0;  // pause screensaver while setup screen is visible
static bool     displayTimerEnabled  = false;
static char     displayTimerFrom[6]  = "23:00";
static char     displayTimerUntil[6] = "07:00";
static bool     displayTimerBlank    = false;   // Sofort-dunkel (manuell, Button)
static bool     displayScheduledBlank = false;  // Zeitgesteuert (Timer)
static uint32_t displayTimerLastCheck = 0;
static volatile bool displayTextActive     = false;
static char   displayTextContent[128]      = "";
static uint8_t displayTextR = 255, displayTextG = 255, displayTextB = 255;
static bool   displayTextScroll            = false;
static uint32_t displayTextEnd             = 0;
static int16_t  displayTextScrollX         = 128;
static bool     displayTextNeedsClear      = true;  // ClearScreen only on first frame and wrap
#ifdef FONT_TEST_ENABLED
static volatile bool fontTestActive        = false;
static uint32_t      fontTestEnd           = 0;
static char          fontTestText[128]     = "Hallo Welt 0123";
static char          fontTestFont[32]      = "FreeSans9";
static uint8_t       fontTestLines         = 1;
static uint8_t       fontTestR = 255, fontTestG = 255, fontTestB = 255;
static bool          fontTestNeedsRender   = false;
#endif
uint8_t screensaverMode = 0;     // 0=Screensaver only, 1=Clock only, 2=Clock+Screensaver
// ntpSynced, ntpServer, clockR/G/B, dateR/G/B, clockColorChanged → clock.cpp
// Weather (mode 3) — globals now in weather.cpp

#ifdef ZEDMD_WIFI
String   mqttServer        = "";
uint16_t mqttPort          = 1883;
String   mqttTopic         = "weather/loop";
String   mqttFieldTemp     = "outTemp_C";
String   mqttFieldHumidity = "outHumidity";
String   mqttFieldWind     = "windSpeed_kph";
String   mqttFieldPressure = "barometer_mbar";
#endif
#ifdef ZEDMD_WIFI
WiFiClient   mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
uint32_t     lastMqttReconnect = 0;

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (length == 0) return;
  static char buf[2048];  // static: no stack pressure on mqttTask (8 KB stack)
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

  weatherTemp      = val(mqttFieldTemp.c_str());
  weatherHumidity  = (uint8_t)roundf(val(mqttFieldHumidity.c_str()));
  weatherWindSpeed = val(mqttFieldWind.c_str());
  weatherPressure  = (uint16_t)roundf(val(mqttFieldPressure.c_str()));
  weatherAvailable = true;
  lastMqttWeather  = millis();
  __sync_synchronize();  // all weather values visible on Core 1 before clockColorChanged
  clockColorChanged = true;
}

void mqttConnect() {
  if (mqttServer.length() == 0) return;
  if (!wifiActive || WiFi.status() != WL_CONNECTED) return;
  char clientId[20];
  snprintf(clientId, sizeof(clientId), "ZeDMD_%04X", shortId);
  if (mqttClient.connect(clientId)) {
    mqttClient.subscribe(mqttTopic.c_str());
    logMsg("MQTT: connected as %s, topic=%s", clientId, mqttTopic.c_str());
  } else {
    logMsg("MQTT: connection failed (rc=%d)", mqttClient.state());
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
uint8_t weatherPage = 0;       // 0=Uhr+Wetter, 1=Vorhersage, 2=Screensaver
uint32_t clockPhaseStart = 0;  // mode 2/6 phase timer (global for reset on mode change)
bool showingClock = false;     // mode 2/6 state: true=clock, false=GIF/text
int16_t screensaverTextScrollX    = TOTAL_WIDTH;  // mode 5/6: scroll position
bool    screensaverTextNeedsClear = true;          // mode 5/6: clear buffer on start
#ifndef SD_MMC_BUILD
SPIClass spiSD(HSPI);  // Global — must not be local! (SPI-SD builds only)
#endif
String screensaverPaths = "";      // comma-separated list of selected paths (empty = LittleFS)
static constexpr size_t SCREENSAVER_FAV_BUF    = 24576;  // 24 KB — ~700 Pfade à 32 Zeichen
static constexpr size_t SCREENSAVER_IGNORE_BUF = 16384;  // 16 KB — ~480 Pfade
char* screensaverFavorites = nullptr;  // PSRAM — no internal heap pressure from += growth
char* screensaverIgnore    = nullptr;  // PSRAM
volatile bool forcePlayPending = false;
String forcePlayFile = "";
String currentlyPlayingFile = "";  // currently playing GIF (including force-play)
volatile bool cancelSdScan = false;
String lastUploadFolder = "";      // target folder of last SD upload (for targeted cache invalidation)

// AnimatedGIF
AnimatedGIF gif;
File gifFile;

// Forward Declarations
void CleanupTmpFiles();
void LoadIcons();
void radioIconSlugsLoad();
const uint8_t* GetSmallIcon(const char* name);
const uint8_t* GetRadioIcon(const char* name);
void LoadScreensaverFiles();
void InitSDCard();
bool sdSpiMountWithFallback();
void SaveScreensaverPaths();
void LoadScreensaverPaths();
void addScreensaverFile(const char* path);
void SaveScreensaverCache();
bool TryLoadScreensaverCache();
void InvalidateAllFolderCaches();
void InvalidateFolderCache(const String& path);
void checkSDCardIdentity();
static void psramCacheSet(char** ptr, const String& json);
void SaveGifAudioCache();
bool TryLoadGifAudioCache();
void InvalidateGifAudioCache();
void ClearScreensaverFilesNow();
void TriggerGifAudioRescan();
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
void GetSDFolders();
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
void SaveDisplayText();
void LoadDisplayText();
void SaveClockColors();   // stays in main.cpp
void LoadClockColors();   // stays in main.cpp
void SaveDisplayTimer();
void LoadDisplayTimer();
void CheckDisplayTimer();
void ApplyBrightness(uint8_t base);
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
  display->DisplayText("red",   0,                      0,              255,   0,   0);
  display->DisplayText("blue",  TOTAL_WIDTH - (4 * 4),  0,                0,   0, 255);
  display->DisplayText("green", 0,                      TOTAL_HEIGHT - 6, 0, 255,   0);
  for (uint8_t i = 0; i < 6; i++) {
    display->DrawPixel(TOTAL_WIDTH - (4 * 4) - 1, i, 0, 0, 0);
    display->DrawPixel((TOTAL_WIDTH / 2) - (6 * 4) - 1, i, 0, 0, 0);
  }
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
    // Avoid busy-waiting; WDT reset prevents reboot on unexpected deadlock
    esp_task_wdt_reset();
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

  // Bulk read instead of byte-by-byte
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
#define GIF_AUDIO_DIR "/GifAudio"  // SD folder for GIF companion MP3s
static uint8_t* gifReadAheadBuf = nullptr;  // allocated in PSRAM (lazy, GIFOpenFile)
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

// GIFDraw callback — writes each frame into the renderBuffer
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *usPalette = pDraw->pPalette;
  int y = pDraw->iY + pDraw->y;

  if (y >= TOTAL_HEIGHT) return;

  s = pDraw->pPixels;

  if (pDraw->ucDisposalMethod == 2) {
    // Restore background color — ucBackground is a palette index, not RGB565
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

  // Check transparency
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

  // Render after last row
  if (pDraw->y == pDraw->iHeight - 1) {
    Render();
  }
}

// Play GIF — loops internally until endTime to avoid freeze between loops
bool PlayGIF(const String &path, uint32_t endTime = 0, bool clearFirst = true, bool loopUntilEnd = true) {
  if (!path.endsWith(".gif") && !path.endsWith(".GIF")) return false;

#ifdef WEBRADIO_ENABLED
  bool gifAudioActive = false;
  if (gifAudioEnabled && path.startsWith("SD:") && !radioIsPlaying && !radioUserActive) {
    // Extract filename from GIF path, search for MP3 in /GifAudio/:
    // 1) Exact match (base.mp3 / base.MP3)
    // 2) Fuzzy: first MP3 whose stem is contained in the GIF name (case-insensitive)
    const char* lastSlash = strrchr(path.c_str(), '/');
    const char* gifNamePtr = lastSlash ? lastSlash + 1 : path.c_str();
    const char* dotPtr = strrchr(gifNamePtr, '.');
    if (dotPtr) {
      int baseLen = (int)(dotPtr - gifNamePtr);
      if (baseLen >= 128) baseLen = 127;
      char base[128];
      strncpy(base, gifNamePtr, baseLen);
      base[baseLen] = '\0';

      char mp3Path[192] = "";
      char exact[192];
      snprintf(exact, sizeof(exact), "%s/%s.mp3", GIF_AUDIO_DIR, base);
      if      (SD.exists(exact)) strlcpy(mp3Path, exact, sizeof(mp3Path));
      else {
        snprintf(exact, sizeof(exact), "%s/%s.MP3", GIF_AUDIO_DIR, base);
        if (SD.exists(exact)) strlcpy(mp3Path, exact, sizeof(mp3Path));
      }
      if (mp3Path[0] == '\0' && cachedGifAudioFiles && strlen(cachedGifAudioFiles) > 2) {
        // Fuzzy in RAM: search cachedGifAudioFiles JSON for matching MP3 stem
        char baseLow[128];
        strlcpy(baseLow, base, sizeof(baseLow));
        for (int i = 0; baseLow[i]; i++) baseLow[i] = tolower((unsigned char)baseLow[i]);

        const char* p = cachedGifAudioFiles;
        while ((p = strstr(p, "\"name\":\"")) != nullptr) {
          p += 8;
          const char* endQ = strchr(p, '"');
          if (!endQ) break;
          int flen = (int)(endQ - p);
          char fnameBuf[256];
          if (flen >= (int)sizeof(fnameBuf)) { p = endQ; continue; }
          strncpy(fnameBuf, p, flen); fnameBuf[flen] = '\0';

          char fLow[256];
          strlcpy(fLow, fnameBuf, sizeof(fLow));
          for (int i = 0; fLow[i]; i++) fLow[i] = tolower((unsigned char)fLow[i]);

          int fLowLen = (int)strlen(fLow);
          if (fLowLen >= 4 && strcmp(fLow + fLowLen - 4, ".mp3") == 0) {
            const char* lastDot = strrchr(fLow, '.');
            int stemLen = lastDot ? (int)(lastDot - fLow) : fLowLen;
            char stemLow[256];
            strncpy(stemLow, fLow, stemLen); stemLow[stemLen] = '\0';
            if (stemLen > 0 && strstr(baseLow, stemLow) != nullptr) {
              snprintf(mp3Path, sizeof(mp3Path), "%s/%s", GIF_AUDIO_DIR, fnameBuf);
              break;
            }
          }
          p = endQ;
        }
      }
      if (mp3Path[0] != '\0') {
        radioPlayLocalFile(mp3Path);
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
    // gif.reset() instead of close()+open(): only seek to 0 + header (~2 ms instead of 15–50 ms SD open)
    if (!firstLoop) gif.reset();
    firstLoop = false;

    int frameDelay = 0;
    uint32_t frameStart = millis();
    while (gif.playFrame(false, &frameDelay) && !transportActive && !screensaverReloadNeeded
           && !forcePlayPending && !displayTextActive && !setupScreenUntil
#ifdef WEBRADIO_ENABLED
           && !radioDisplayActive
#endif
           && (endTime == 0 || millis() < endTime)) {
      if (frameDelay > 0) {
        uint32_t elapsed   = millis() - frameStart;
        uint32_t remaining = (elapsed < (uint32_t)frameDelay) ? ((uint32_t)frameDelay - elapsed) : 0;
        uint32_t waitUntil = millis() + remaining;
        while (millis() < waitUntil && !transportActive && !forcePlayPending
               && !displayTextActive && !setupScreenUntil
#ifdef WEBRADIO_ENABLED
               && !radioDisplayActive
#endif
               && (endTime == 0 || millis() < endTime)) {
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      }
      frameStart = millis();
      yield();
      esp_task_wdt_reset();
    }
    // Last frame: wait full display duration before the next loop
    if (frameDelay > 0 && !transportActive && !screensaverReloadNeeded && !forcePlayPending
        && !displayTextActive && !setupScreenUntil
#ifdef WEBRADIO_ENABLED
        && !radioDisplayActive
#endif
        && (endTime == 0 || millis() < endTime)) {
      uint32_t elapsed   = millis() - frameStart;
      uint32_t remaining = (frameDelay > (int)elapsed) ? ((uint32_t)frameDelay - elapsed) : 0;
      uint32_t waitUntil = millis() + remaining;
      while (millis() < waitUntil && !transportActive && !forcePlayPending
             && !displayTextActive && !setupScreenUntil
#ifdef WEBRADIO_ENABLED
             && !radioDisplayActive
#endif
             && (endTime == 0 || millis() < endTime)) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
  } while (loopUntilEnd && !transportActive && !screensaverReloadNeeded && !forcePlayPending
           && !displayTextActive && !setupScreenUntil
#ifdef WEBRADIO_ENABLED
           && !radioDisplayActive
#endif
           && (endTime == 0 || millis() < endTime));

  gif.close();

#ifdef WEBRADIO_ENABLED
  if (gifAudioActive) radioStopLocalFile();
#endif

  return true;
}

// ─────────────────────────────────────────────
// Central brightness wrapper — respects manual blank (button) and
// scheduled blank (timer). All screensaver paths use ApplyBrightness()
// instead of display->SetBrightness() directly; setup and transport paths remain direct.
void ApplyBrightness(uint8_t base) {
  display->SetBrightness((displayTimerBlank || displayScheduledBlank) ? 0 : base);
}

// ─────────────────────────────────────────────

void ScreenSaver() {
  ApplyBrightness(screensaverBrightness);

  if (screensaverCount > 0) {
    String path = String(screensaverFiles[screensaverIndex]);

    // Load RAW only — GIF is played in loop() via PlayGIF
    if (!path.endsWith(".gif") && !path.endsWith(".GIF")) {
      ClearScreen();  // clear hardware + renderBuffer[last] → diff finds all pixels
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
    // GIF → loop() takes care of it
  } else {
    // Fallback → logo.raw
    ClearScreen();
    File f;
    if (TOTAL_HEIGHT == 64) {
      f = LittleFS.open("/logoHD.raw", "r");
    } else {
      f = LittleFS.open("/logo.raw", "r");
    }
    if (f) {
      // Bulk read instead of byte-by-byte
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
  // Everything configurable via admin.html — display shows only the boot logo.
  // RGB color test is drawn explicitly by DisplayRGB() when needed.
  DisplayLogo();
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
            uint8_t response[64 - N_ACK_CHARS] = {};  // stack is sufficient for 59 bytes
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
    client->close();
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
  if (ESP.getMaxAllocHeap() < 6144) {
    request->send(503, "text/html",
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ZeDMD</title>"
      "<style>body{font-family:sans-serif;text-align:center;padding:2em;"
      "background:#1a1a1a;color:#eee}h2{color:#e67e22}p{color:#aaa}</style>"
      "</head><body>"
      "<h2>Kurz ausgelastet</h2>"
      "<p>Speicher kurzzeitig belegt &mdash; kein Neustart n&ouml;tig.<br>"
      "Seite wird automatisch neu geladen...</p>"
      "<script>setTimeout(function(){location.reload();},2000);</script>"
      "</body></html>");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) { request->send(404); return; }
  char etag[24];
  snprintf(etag, sizeof(etag), "\"%u\"", (unsigned)f.size());
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
      setupScreenUntil = millis() + 8000;
      logoWaitCounter = 200;  // DisplayLogo() would have reset the counter to 0 → skip logo sequence
      RefreshSetupScreen();
      DisplayRGB();
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
      brightness = (uint8_t)constrain(brightnessValue.toInt(), 0, 15);
      GetDisplayObject()->SetBrightness(brightness);
      SaveLum();
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

  // POST /restart — restart the ESP
  server->on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Restarting...");
    delay(500);
    Restart();
  });

  // POST /reset_diag — reset diagnostic counters
  server->on("/reset_diag", HTTP_POST, [](AsyncWebServerRequest *request) {
    LittleFS.remove("/diag.json");
    request->send(200, "text/plain", "OK");
  });

  // POST /delete_crashlogs — delete all crash dumps
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
    SaveScreensaverLum();  // save screensaver brightness
    Restart();  // Restart the device
  });

  // Serve debug information
  server->on("/debug_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[320];
    snprintf(buf, sizeof(buf),
      "IP Address: %s\nSSID: %s\nRSSI: %d\n"
      "Heap Free: %lu bytes\nHeap Largest Block: %lu bytes\nHeap Min Ever Free: %lu bytes\n"
      "PSRAM Free: %lu bytes\nUptime: %lu seconds\n",
      WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), WiFi.RSSI(),
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned long)ESP.getMinFreeHeap(),
      (unsigned long)ESP.getFreePsram(),
      (unsigned long)(millis() / 1000));
    request->send(200, "text/plain", buf);
  });

  server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&logMux);
    uint8_t total = logCount;
    uint8_t start = (logCount < LOG_LINES) ? 0 : logHead;
    portEXIT_CRITICAL(&logMux);
    const size_t bufSize = (size_t)total * (LOG_LINE_LEN + 1) + 4;
    char* buf = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { request->send(503, "text/plain", "OOM"); return; }
    size_t pos = 0;
    for (uint8_t i = 0; i < total; i++) {
      size_t len = strnlen(logBuffer[(start + i) % LOG_LINES], LOG_LINE_LEN - 1);
      memcpy(buf + pos, logBuffer[(start + i) % LOG_LINES], len);
      pos += len;
      buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    request->send(200, "text/plain; charset=utf-8", buf);
    heap_caps_free(buf);
  });

  // GET /diag — boot/crash statistics from /diag.json + live crashFiles list
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

  // GET /crashlog?n=0..4 — read individual RTC dump from LittleFS
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

  // GET /coredump — loads raw ELF coredump from flash partition for decoding with espcoredump.py
  server->on("/coredump", HTTP_GET, [](AsyncWebServerRequest *request) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
      request->send(404, "text/plain", "No coredump partition");
      return;
    }
    // Erased flash = 0xFFFFFFFF → no coredump present.
    // ESP-IDF does not write an ELF header directly; first uint32 is the data length.
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
    if (request->hasParam("topic", true) && request->getParam("topic", true)->value().length() > 0)
      mqttTopic = request->getParam("topic", true)->value();
    if (request->hasParam("fieldTemp", true) && request->getParam("fieldTemp", true)->value().length() > 0)
      mqttFieldTemp = request->getParam("fieldTemp", true)->value();
    if (request->hasParam("fieldHumidity", true) && request->getParam("fieldHumidity", true)->value().length() > 0)
      mqttFieldHumidity = request->getParam("fieldHumidity", true)->value();
    if (request->hasParam("fieldWind", true) && request->getParam("fieldWind", true)->value().length() > 0)
      mqttFieldWind = request->getParam("fieldWind", true)->value();
    if (request->hasParam("fieldPressure", true) && request->getParam("fieldPressure", true)->value().length() > 0)
      mqttFieldPressure = request->getParam("fieldPressure", true)->value();
    SaveMqttConfig();
    mqttClient.disconnect();
    mqttClient.setServer(mqttServer.c_str(), mqttPort);
    logMsg("MQTT: config updated — server=%s:%d topic=%s", mqttServer.c_str(), mqttPort, mqttTopic.c_str());
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
  // snprintf into static buffer — no heap growth from string concatenation
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
      ",\"mqttServer\":\"%s\",\"mqttPort\":%u,\"mqttTopic\":\"%s\""
      ",\"mqttFieldTemp\":\"%s\",\"mqttFieldHumidity\":\"%s\""
      ",\"mqttFieldWind\":\"%s\",\"mqttFieldPressure\":\"%s\"",
      mqttServer.c_str(), (unsigned)mqttPort, mqttTopic.c_str(),
      mqttFieldTemp.c_str(), mqttFieldHumidity.c_str(),
      mqttFieldWind.c_str(), mqttFieldPressure.c_str());
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
          uint8_t scalingMode = (uint8_t)constrain(scalingModeValue.toInt(), 0, 255);

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
    // 20 entries × max 128 characters + separators + brackets
    char* json = (char*)heap_caps_malloc(2700, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) { request->send(503, "text/plain", "Low memory"); return; }
    int pos = 0;
    json[pos++] = '[';
    if (screensaverFilesMutex && xSemaphoreTake(screensaverFilesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      uint16_t offset = 0;
      if (request->hasParam("offset"))
        offset = (uint16_t)constrain(request->getParam("offset")->value().toInt(), 0, (int)screensaverCount);
      uint16_t end = min((uint16_t)(offset + 20), screensaverCount);
      for (uint16_t i = offset; screensaverFiles && i < end; i++) {
        if (i > offset) json[pos++] = ',';
        pos += snprintf(json + pos, 2700 - pos, "\"%s\"", screensaverFiles[i]);
      }
      xSemaphoreGive(screensaverFilesMutex);
    }
    json[pos++] = ']';
    json[pos] = '\0';
    request->send(200, "application/json", json);
    heap_caps_free(json);
  });

  // GET /gif_preview?path=<SD:|FS:>/<path> — stream GIF file for browser preview
  server->on("/gif_preview", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("path")) {
      request->send(400, "text/plain", "Missing path");
      return;
    }
    String path = request->getParam("path")->value();

    // LittleFS: send directly
    if (path.startsWith("FS:")) {
      String fsPath = path.substring(3);
      if (!LittleFS.exists(fsPath)) { request->send(404, "text/plain", "Not found"); return; }
      request->send(LittleFS, fsPath, "image/gif");
      return;
    }

    // SD: load completely into PSRAM first, then close SD immediately, then stream from RAM.
    // Prevents SPI conflict between WebServer (Core 0) and GIF screensaver (Core 1).
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
      f.close();  // release SD immediately — no more concurrent access

      // shared_ptr: free exactly once — on EOF and on connection abort
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

  // GET /screensaver_folder_count — loaded file count + scan status
  server->on("/screensaver_folder_count", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint16_t count   = screensaverCount;
    uint16_t showing = min((uint16_t)20, count);
    bool     scanning = screensaverLoadRunning || screensaverReloadNeeded;
    request->send(200, "application/json",
      "{\"total\":" + String(count) + ",\"showing\":" + String(showing) +
      ",\"scanning\":" + (scanning ? "true" : "false") + "}");
  });

  // POST /delete_screensaver
  server->on("/delete_screensaver", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file", true)) {
      String filename = "/screensaver/" + request->getParam("file", true)->value();
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
    if (request->hasParam("clockR", true)) clockR = (uint8_t)constrain(request->getParam("clockR", true)->value().toInt(), 0, 255);
    if (request->hasParam("clockG", true)) clockG = (uint8_t)constrain(request->getParam("clockG", true)->value().toInt(), 0, 255);
    if (request->hasParam("clockB", true)) clockB = (uint8_t)constrain(request->getParam("clockB", true)->value().toInt(), 0, 255);
    if (request->hasParam("dateR",  true)) dateR  = (uint8_t)constrain(request->getParam("dateR",  true)->value().toInt(), 0, 255);
    if (request->hasParam("dateG",  true)) dateG  = (uint8_t)constrain(request->getParam("dateG",  true)->value().toInt(), 0, 255);
    if (request->hasParam("dateB",  true)) dateB  = (uint8_t)constrain(request->getParam("dateB",  true)->value().toInt(), 0, 255);
    SaveClockColors();
    clockColorChanged = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /save_screensaver_mode
  server->on("/save_screensaver_mode", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("mode", true)) {
      screensaverMode = (uint8_t)constrain(request->getParam("mode", true)->value().toInt(), 0, 6);
      clockColorChanged = true;       // immediate redraw on mode change
      weatherPhaseStart = 0;          // reset phase timer
      weatherPage = 0;
      clockPhaseStart = 0;            // reset mode 2/6 timer
      showingClock = false;           // reset mode 2/6 state
      screensaverTextScrollX    = TOTAL_WIDTH;  // reset mode 5/6 scroll
      screensaverTextNeedsClear = true;
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
      screensaverBrightness = (uint8_t)constrain(request->getParam("screensaverBrightness", true)->value().toInt(), 0, 15);
      ApplyBrightness(screensaverBrightness);
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
      screensaverDuration = (uint8_t)constrain(request->getParam("screensaverDuration", true)->value().toInt(), 1, 255);
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

  // POST /screensaver_reshuffle — reshuffle order without changing the setting
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


  // GET /screensaver_current — currently displayed file
  server->on("/screensaver_current", HTTP_GET, [](AsyncWebServerRequest *request) {
    char pathBuf[192];
    if (currentlyPlayingFile.length() > 0)
      strlcpy(pathBuf, currentlyPlayingFile.c_str(), sizeof(pathBuf));
    else if (screensaverCount > 0)
      strlcpy(pathBuf, screensaverFiles[screensaverIndex], sizeof(pathBuf));
    else
      pathBuf[0] = '\0';
    const char* fname = strrchr(pathBuf, '/');
    fname = fname ? fname + 1 : pathBuf;
    bool fav = isFavorite(pathBuf);
    bool ign = isIgnored(pathBuf);
    char json[512];
    snprintf(json, sizeof(json),
             "{\"path\":\"%s\",\"name\":\"%s\",\"favorite\":%s,\"ignored\":%s}",
             pathBuf, fname, fav ? "true" : "false", ign ? "true" : "false");
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

  // GET /get_favorites — list of all favorites
  server->on("/get_favorites", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", screensaverFavorites ? screensaverFavorites : "");
  });

  // POST /play_file — play GIF directly
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

  // GET /get_ignores — list of all ignored files
  server->on("/get_ignores", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", screensaverIgnore ? screensaverIgnore : "");
  });

  // GET /fs_info — filesystem storage info (cached at boot — usedBytes() traverses entire LFS, >5s on fragmentation → TASK_WDT)
  server->on("/fs_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    char json[80];
    snprintf(json, sizeof(json), "{\"total\":%u,\"used\":%u,\"free\":%u}",
      (unsigned)lfsTotal, (unsigned)lfsUsed,
      (unsigned)(lfsTotal - lfsUsed));
    request->send(200, "application/json", json);
  });

  // GET /sd_info — SD card storage info (cached from boot)
  server->on("/sd_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    char json[120];
    snprintf(json, sizeof(json), "{\"available\":%s,\"total\":%u,\"used\":%u,\"free\":%u}",
      sdCardAvailable ? "true" : "false",
      (uint32_t)(sdTotalBytes / 1024), (uint32_t)(sdUsedBytes / 1024),
      (uint32_t)((sdTotalBytes - sdUsedBytes) / 1024));
    request->send(200, "application/json", json);
  });

  // POST /display_text — show text on display (static or scrolling, with color + duration)
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
    SaveDisplayText();
    screensaverTextScrollX    = TOTAL_WIDTH;  // reset scroll position for mode 5/6
    screensaverTextNeedsClear = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /display_text_stop — stop display text immediately
  server->on("/display_text_stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    displayTextActive = false;
    request->send(200, "text/plain", "OK");
  });

  // GET /display_timer — read timer status
  server->on("/display_timer", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[96];
    snprintf(buf, sizeof(buf),
      "{\"enabled\":%s,\"from\":\"%s\",\"until\":\"%s\",\"blank\":%s}",
      displayTimerEnabled ? "true" : "false",
      displayTimerFrom, displayTimerUntil,
      displayTimerBlank ? "true" : "false");
    request->send(200, "application/json", buf);
  });

  // POST /display_timer — configure timer (enabled=1&from=23:00&until=07:00)
  server->on("/display_timer", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("enabled", true))
      displayTimerEnabled = request->getParam("enabled", true)->value().toInt() != 0;
    if (request->hasParam("from", true)) {
      String v = request->getParam("from", true)->value();
      if (v.length() == 5) strlcpy(displayTimerFrom, v.c_str(), sizeof(displayTimerFrom));
    }
    if (request->hasParam("until", true)) {
      String v = request->getParam("until", true)->value();
      if (v.length() == 5) strlcpy(displayTimerUntil, v.c_str(), sizeof(displayTimerUntil));
    }
    SaveDisplayTimer();
    CheckDisplayTimer();
    request->send(200, "text/plain", "OK");
  });

  // POST /display_blank — immediately blank / restore brightness (toggle)
  server->on("/display_blank", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("blank", true))
      displayTimerBlank = request->getParam("blank", true)->value().toInt() != 0;
    else
      displayTimerBlank = !displayTimerBlank;
    CheckDisplayTimer();
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"blank\":%s}", displayTimerBlank ? "true" : "false");
    request->send(200, "application/json", buf);
  });

#ifdef FONT_TEST_ENABLED
  server->on("/font_test_enabled", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "1");
  });
  server->on("/font_list", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", display->GetFontListJSON());
  });
  server->on("/font_test", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("text", true)) {
      request->send(400, "text/plain", "Missing text");
      return;
    }
    String text = request->getParam("text", true)->value();
    String font = request->hasParam("font", true) ? request->getParam("font", true)->value() : "FreeSans9";
    String col  = request->hasParam("color", true) ? request->getParam("color", true)->value() : "ffffff";
    int    dur  = request->hasParam("duration", true) ? request->getParam("duration", true)->value().toInt() : 10;
    int    lns  = request->hasParam("lines", true) ? request->getParam("lines", true)->value().toInt() : 1;
    if (col.startsWith("#")) col = col.substring(1);
    strlcpy(fontTestText, text.c_str(), sizeof(fontTestText));
    strlcpy(fontTestFont, font.c_str(), sizeof(fontTestFont));
    fontTestLines = (uint8_t)constrain(lns, 1, 3);
    fontTestR = (uint8_t)strtol(col.substring(0, 2).c_str(), nullptr, 16);
    fontTestG = (uint8_t)strtol(col.substring(2, 4).c_str(), nullptr, 16);
    fontTestB = (uint8_t)strtol(col.substring(4, 6).c_str(), nullptr, 16);
    fontTestEnd          = millis() + (uint32_t)dur * 1000;
    fontTestNeedsRender  = true;
    fontTestActive       = true;
    request->send(200, "text/plain", "OK");
  });
  server->on("/font_test_stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    fontTestActive      = false;
    fontTestEnd         = 0;
    clockColorChanged   = true;
    request->send(200, "text/plain", "OK");
  });
#endif  // FONT_TEST_ENABLED

  // POST /eject_sd — safely unmount SD card
  server->on("/eject_sd", HTTP_POST, [](AsyncWebServerRequest *request) {
    SD.end();
    sdCardAvailable = false;
    sdTotalBytes = 0;
    sdUsedBytes  = 0;
    sdFoldersInvalidateNeeded = true;
    sdFilesInvalidateNeeded   = true;
    screensaverReloadNeeded   = true;  // remove SD paths from screensaverFiles (SD not available)
    request->send(200, "text/plain", "OK");
  });

  // POST /mount_sd — mount SD card again
  server->on("/mount_sd", HTTP_POST, [](AsyncWebServerRequest *request) {
    SD.end();
    bool ok = sdSpiMountWithFallback();
    if (ok) {
      sdCardAvailable      = true;
      sdTotalBytes         = SD.cardSize();
      sdUsedBytes          = SD.usedBytes();
      checkSDCardIdentity();       // card known? otherwise invalidate caches
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

  // POST /test_weather_icons — toggle: shows all 16×16 weather icons
  server->on("/test_weather_icons", HTTP_POST, [](AsyncWebServerRequest *request) {
    weatherIconTestActive = !weatherIconTestActive;
    weatherSmallIconTestActive = false;
    request->send(200, "text/plain", weatherIconTestActive ? "ON" : "OFF");
  });

  // POST /test_weather_icons_small — toggle: shows all 8×8 weather icons (forecast size)
  server->on("/test_weather_icons_small", HTTP_POST, [](AsyncWebServerRequest *request) {
    weatherSmallIconTestActive = !weatherSmallIconTestActive;
    weatherIconTestActive = false;
    request->send(200, "text/plain", weatherSmallIconTestActive ? "ON" : "OFF");
  });

  // GET /sd_folders — returns cached status, no SD access in the webserver task!
  server->on("/sd_folders", HTTP_GET, [](AsyncWebServerRequest *request) {
    const char* folders = cachedSDFolders ? cachedSDFolders : "[]";
    size_t bufSize = screensaverPaths.length() + strlen(folders) + 64;
    char* json = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) { request->send(503, "text/plain", "Low memory"); return; }
    snprintf(json, bufSize, "{\"available\":%s,\"currentPaths\":\"%s\",\"folders\":%s}",
      sdCardAvailable ? "true" : "false", screensaverPaths.c_str(), folders);
    request->send(200, "application/json", json);
    heap_caps_free(json);
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

  // POST /cancel_scan — abort SD scan (screensaver: also resets paths)
  server->on("/cancel_scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    cancelSdScan = true;
    screensaverPaths = "";
    screensaverReloadNeeded = true;
    request->send(200, "text/plain", "OK");
  });

  // POST /screensaver_rescan — invalidate all folder caches + force rescan
  server->on("/screensaver_rescan", HTTP_POST, [](AsyncWebServerRequest *request) {
    InvalidateAllFolderCaches();
    screensaverCount        = 0;  // reset immediately so poll sees 0 before new scan starts
    screensaverReloadNeeded = true;
    sdFoldersRefreshNeeded  = true;
    request->send(200, "application/json", "{\"ok\":1}");
  });

  // POST /sd_folders_refresh — refresh folder list only (no cache clearing, no file scan)
  server->on("/sd_folders_refresh", HTTP_POST, [](AsyncWebServerRequest *request) {
    sdFoldersRefreshNeeded = true;
    request->send(200, "text/plain", "OK");
  });

  // GET /clear_screensaver_cache — delete cache files without rescanning
  server->on("/clear_screensaver_cache", HTTP_GET, [](AsyncWebServerRequest *request) {
    InvalidateAllFolderCaches();
    request->send(200, "application/json", "{\"ok\":1}");
  });

  // POST /cancel_gif_audio_scan — abort GIF audio scan
  server->on("/cancel_gif_audio_scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    cancelSdScan = true;
    request->send(200, "text/plain", "OK");
  });

  // ── GIF audio management (/GifAudio/ on SD) ──────────────────────────────────

  server->on("/gif_audio_files", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", cachedGifAudioFiles ? cachedGifAudioFiles : "[]");
  });

  server->on("/gif_audio_upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!sdCardAvailable) { request->send(503, "text/plain", "SD-Karte nicht verfügbar"); return; }
      TriggerGifAudioRescan();
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

  // POST /gif_audio_rescan — invalidate cache + force rescan
  server->on("/gif_audio_rescan", HTTP_POST, [](AsyncWebServerRequest *request) {
    InvalidateGifAudioCache();
    gifAudioRefreshNeeded = true;
    request->send(200, "application/json", "{\"ok\":1}");
  });

  // POST /upload_sd — upload to SD card
  // Optional query parameter ?folder=Name overrides screensaverPaths
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
        logMsg("[upload_sd] %s", targetPath.c_str());
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        SD.rename(targetPath + ".tmp", targetPath);
        logMsg("[upload_sd] done");
      }
    }
  );

  // Start the web server
  // GET /admin — password-protected admin page (admin.html in LittleFS)
  server->on("/admin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate("admin", "zedmd1234")) {
      return request->requestAuthentication();
    }
    sendLittleFSHtml(request, "/admin.html");
  });

  // GET /sd_files — SD files in a folder (cached; loop() builds cache)
  server->on("/sd_files", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("folder")) {
      String folder = "/" + request->getParam("folder")->value();
      if (strcmp(cachedSdFilesFolder, folder.c_str()) != 0) {
        strncpy(cachedSdFilesFolder, folder.c_str(), CACHE_SD_FOLDER_SIZE - 1);
        cachedSdFilesFolder[CACHE_SD_FOLDER_SIZE - 1] = '\0';
        sdFilesInvalidateNeeded = true;  // invalidated → loop() rebuilds
      }
      sdFilesRefreshNeeded = true;
    }
    request->send(200, "application/json", cachedSdFiles ? cachedSdFiles : "[]");
  });

  // POST /delete_sd_file
  server->on("/delete_sd_file", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (sdCardAvailable && request->hasParam("folder", true) && request->hasParam("file", true)) {
      String path = "/" + request->getParam("folder", true)->value() + "/" + request->getParam("file", true)->value();
      InvalidateFolderCache("/" + request->getParam("folder", true)->value());
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
      transport = (int8_t)constrain(request->getParam("transport", true)->value().toInt(), 0, 3);
      File f = LittleFS.open("/transport.val", "w");
      if (f) { f.write(transport); f.close(); }
      request->send(200, "text/plain", "OK");
    } else request->send(400, "text/plain", "Missing parameter");
  });

  // POST /save_udp_delay
  server->on("/save_udp_delay", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("udpDelay", true)) {
      udpDelay = (uint8_t)constrain(request->getParam("udpDelay", true)->value().toInt(), 0, 255);
      File f = LittleFS.open("/udp_delay.val", "w");
      if (f) { f.write(udpDelay); f.close(); }
      request->send(200, "text/plain", "OK");
    } else request->send(400, "text/plain", "Missing parameter");
  });

  // POST /save_usb_size
  server->on("/save_usb_size", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("usbPackageSizeMultiplier", true)) {
      usbPackageSizeMultiplier = (uint8_t)constrain(request->getParam("usbPackageSizeMultiplier", true)->value().toInt(), 1, 255);
      File f = LittleFS.open("/usb_size.val", "w");
      if (f) { f.write(usbPackageSizeMultiplier); f.close(); }
      request->send(200, "text/plain", "OK");
    } else request->send(400, "text/plain", "Missing parameter");
  });

  // POST /save_panel_settings
#ifdef DISPLAY_LED_MATRIX
  server->on("/save_panel_settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("panelClkphase", true))
      panelClkphase    = (uint8_t)constrain(request->getParam("panelClkphase", true)->value().toInt(),    0, 1);
    if (request->hasParam("panelI2sspeed", true))
      panelI2sspeed    = (uint8_t)constrain(request->getParam("panelI2sspeed", true)->value().toInt(),    0, 255);
    if (request->hasParam("panelLatchBlanking", true))
      panelLatchBlanking    = (uint8_t)constrain(request->getParam("panelLatchBlanking", true)->value().toInt(),    0, 255);
    if (request->hasParam("panelMinRefreshRate", true))
      panelMinRefreshRate   = (uint8_t)constrain(request->getParam("panelMinRefreshRate", true)->value().toInt(),   1, 255);
    if (request->hasParam("panelDriver", true))
      panelDriver      = (uint8_t)constrain(request->getParam("panelDriver", true)->value().toInt(),      0, 255);
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

  // POST /upload_icon — upload 20×20 RGBA icons to LittleFS /icons/
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

  // POST /upload_icon_small — upload 10×10 RGBA icons to LittleFS /icons_small/
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

  // POST /upload_icon_radio — upload 32×32 RGBA station logos to LittleFS /icons_radio/
  server->on("/upload_icon_radio", HTTP_POST,
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
        if (!LittleFS.exists("/icons_radio")) LittleFS.mkdir("/icons_radio");
        if (uploadFile) { uploadFile.close(); LittleFS.remove(("/icons_radio/" + filename + ".tmp").c_str()); }
        uploadFile = LittleFS.open("/icons_radio/" + filename + ".tmp", "w");
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) {
        uploadFile.close();
        String dst = "/icons_radio/" + filename;
        LittleFS.remove(dst);
        LittleFS.rename("/icons_radio/" + filename + ".tmp", dst);
        logMsg("Radio-Logo hochgeladen: %s", dst.c_str());
        iconsReloadNeeded = true;
      }
    }
  );

  // POST /ota — firmware update over WiFi (admin only)
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

  // ── Config export / import ───────────────────────────────────────────────

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
      "/mqtt_config.txt", "/weather_config.txt", "/display_timer.cfg",
      "/display_text_content.val", "/display_text_color.val",
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
    // Simple parser: searches for "\"/<key>\":\"<hex>\""
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

void SaveDisplayText() {
  File f = LittleFS.open("/display_text_content.val", "w");
  if (!f) return;
  f.write((const uint8_t*)displayTextContent, strlen(displayTextContent));
  f.close();
  File fc = LittleFS.open("/display_text_color.val", "w");
  if (!fc) return;
  fc.write(displayTextR); fc.write(displayTextG); fc.write(displayTextB);
  fc.close();
}

void LoadDisplayText() {
  File f = LittleFS.open("/display_text_content.val", "r");
  if (!f) return;
  size_t n = f.readBytes(displayTextContent, sizeof(displayTextContent) - 1);
  displayTextContent[n] = '\0';
  f.close();
  displayTextScroll = display && displayTextContent[0] &&
                      display->GetTextGFXWidth(displayTextContent) > TOTAL_WIDTH;
  File fc = LittleFS.open("/display_text_color.val", "r");
  if (!fc) return;
  if (fc.available()) displayTextR = fc.read();
  if (fc.available()) displayTextG = fc.read();
  if (fc.available()) displayTextB = fc.read();
  fc.close();
}

// DrawSegDigit(), DrawColon(), clockDisplay(), clockInit() → clock.cpp

#ifdef WEBRADIO_ENABLED
void DisplayRadio() {
  static constexpr int16_t RADIO_ICON_W = 32; // 32×32 Senderlogo
  static char     lastStation[64]  = "";
  static char     lastTitle[128]   = "";
  static uint32_t lastScroll       = 0;
  static int16_t  scrollX          = 0;
  static int16_t  lastScrollX      = -1;
  static uint32_t lastCallMillis   = 0;  // letzter Aufruf-Zeitstempel

  // Wenn seit dem letzten Aufruf >200ms vergangen sind, lief zwischenzeitlich
  // der Screensaver → renderBuffer mit GIF-Inhalt verschmutzt → alles leeren.
  uint32_t now = millis();
  bool needsClear = (now - lastCallMillis) > 200;
  lastCallMillis  = now;

  static char stationSnap[64]  = "Radio";
  static char titleSnap[128]   = "Verbinde...";
  if (radioStringMutex && xSemaphoreTake(radioStringMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (radioStationName[0]) strlcpy(stationSnap, radioStationName, sizeof(stationSnap));
    else strlcpy(stationSnap, "Radio", sizeof(stationSnap));
    if (radioTrackTitle[0])  strlcpy(titleSnap, radioTrackTitle, sizeof(titleSnap));
    else strlcpy(titleSnap, "Verbinde...", sizeof(titleSnap));
    xSemaphoreGive(radioStringMutex);
  }

  bool needRedraw = needsClear;  // nach Buffer-Reset immer neu zeichnen

  static int16_t stationScrollX     = RADIO_ICON_W;
  static int16_t lastStationScrollX = -1;

  if (strcmp(lastStation, stationSnap) != 0) {
    strlcpy(lastStation, stationSnap, sizeof(lastStation));
    stationScrollX     = RADIO_ICON_W;
    lastStationScrollX = -1;
    scrollX     = RADIO_ICON_W;
    lastScrollX = -1;
    needRedraw  = true;
    // Debug: zeigt was der Stream sendet und ob ein Icon gefunden wird
    char dbgSlug[32];
    auto slugFn = [](const char *s, char *o, size_t l) {
      size_t j = 0;
      for (size_t i = 0; s[i] && j + 1 < l; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o[j++] = c;
        else if (j > 0 && o[j-1] != '_') o[j++] = '_';
      }
      while (j > 0 && o[j-1] == '_') j--;
      o[j] = '\0';
    };
    slugFn(stationSnap, dbgSlug, sizeof(dbgSlug));
    bool hasIcon = GetRadioIcon(dbgSlug) != nullptr;
    logMsg("[radio] Station='%s' slug='%s' icon=%s", stationSnap, dbgSlug, hasIcon ? "OK" : "Fallback");
  }
  if (strcmp(lastTitle, titleSnap) != 0) {
    strlcpy(lastTitle, titleSnap, sizeof(lastTitle));
    scrollX     = RADIO_ICON_W;
    lastScrollX = -1;
    needRedraw  = true;
  }

  const int16_t textArea = TOTAL_WIDTH - RADIO_ICON_W;

  // Sendername: scrollt wenn zu breit, sonst zentriert
  int16_t stationW      = (int16_t)display->GetRadioTitleWidth(stationSnap);
  bool    stationScrolls = stationW > textArea;
  int16_t stationX;
  if (stationScrolls) {
    stationX = stationScrollX;
    if (stationScrollX != lastStationScrollX) { lastStationScrollX = stationScrollX; needRedraw = true; }
  } else {
    stationX = RADIO_ICON_W + (textArea - stationW) / 2;
  }

  // Titel: scrollt wenn zu breit, sonst zentriert
  int16_t titleW      = (int16_t)display->GetRadioTitleWidth(titleSnap);
  bool    titleScrolls = titleW > textArea;
  int16_t titleX;
  if (titleScrolls) {
    titleX = scrollX;
    if (scrollX != lastScrollX) { lastScrollX = scrollX; needRedraw = true; }
  } else {
    titleX = RADIO_ICON_W + (textArea - titleW) / 2;
  }

  // Scroll-Tick: beide Zeilen gleichzeitig um 1px weiter
  if (now - lastScroll > 40) {
    if (stationScrolls) {
      if (--stationScrollX < -stationW) stationScrollX = RADIO_ICON_W;
    }
    if (titleScrolls) {
      if (--scrollX < -titleW) scrollX = RADIO_ICON_W;
    }
    lastScroll = now;
  }

  if (!needRedraw) return;

  if (needsClear) {
    for (int i = 0; i < NUM_RENDER_BUFFERS; i++)
      memset(renderBuffer[i], 0, TOTAL_BYTES);
    // Force full-screen update: make lastRenderBuffer non-zero so every pixel
    // in the first radio frame appears changed to Render()'s diff engine.
    // Without this, black background pixels (0) match the zeroed lastRenderBuffer
    // and are not sent → old screen content (weather/GIF) bleeds through.
    if (NUM_RENDER_BUFFERS > 1)
      memset(renderBuffer[lastRenderBuffer], 1, TOTAL_BYTES);
  }

  ApplyBrightness(screensaverBrightness);
  display->RenderRadioToBuffer(renderBuffer[currentRenderBuffer],
                               stationSnap, titleSnap,
                               stationX, titleX,
                               clockR, clockG, clockB,
                               200, 200, 200);
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
  f.println(mqttTopic);
  f.println(mqttFieldTemp);
  f.println(mqttFieldHumidity);
  f.println(mqttFieldWind);
  f.println(mqttFieldPressure);
  f.close();
}
void LoadMqttConfig() {
  File f = LittleFS.open("/mqtt_config.txt", "r");
  if (!f) { SaveMqttConfig(); return; }
  mqttServer = f.readStringUntil('\n'); mqttServer.trim();
  mqttPort   = (uint16_t)f.readStringUntil('\n').toInt();
  String t   = f.readStringUntil('\n'); t.trim();
  if (t.length() > 0) mqttTopic = t;
  t = f.readStringUntil('\n'); t.trim(); if (t.length() > 0) mqttFieldTemp = t;
  t = f.readStringUntil('\n'); t.trim(); if (t.length() > 0) mqttFieldHumidity = t;
  t = f.readStringUntil('\n'); t.trim(); if (t.length() > 0) mqttFieldWind = t;
  t = f.readStringUntil('\n'); t.trim(); if (t.length() > 0) mqttFieldPressure = t;
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

void SaveDisplayTimer() {
  File f = LittleFS.open("/display_timer.cfg", "w");
  if (!f) return;
  f.write((uint8_t)displayTimerEnabled);
  f.write((const uint8_t*)displayTimerFrom,  5);
  f.write((const uint8_t*)displayTimerUntil, 5);
  f.close();
}

void LoadDisplayTimer() {
  File f = LittleFS.open("/display_timer.cfg", "r");
  if (!f) { SaveDisplayTimer(); return; }
  displayTimerEnabled = (bool)f.read();
  f.readBytes(displayTimerFrom,  5); displayTimerFrom[5]  = '\0';
  f.readBytes(displayTimerUntil, 5); displayTimerUntil[5] = '\0';
  f.close();
}

void CheckDisplayTimer() {
  if (displayTimerEnabled) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int now   = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int from  = (displayTimerFrom[0]  - '0') * 600 + (displayTimerFrom[1]  - '0') * 60
                + (displayTimerFrom[3]  - '0') * 10  + (displayTimerFrom[4]  - '0');
      int until = (displayTimerUntil[0] - '0') * 600 + (displayTimerUntil[1] - '0') * 60
                + (displayTimerUntil[3] - '0') * 10  + (displayTimerUntil[4] - '0');
      if (from > until)
        displayScheduledBlank = (now >= from || now < until);
      else
        displayScheduledBlank = (now >= from && now < until);
    }
  } else {
    displayScheduledBlank = false;
  }
  ApplyBrightness(screensaverBrightness);
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

  // sort index array (2 bytes/entry) instead of moving 128-byte blocks
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
  if (!tmp) { heap_caps_free(idx); return; }
  for (uint16_t i = 0; i < screensaverCount; i++) memcpy(tmp[i], screensaverFiles[idx[i]], 128);
  memcpy(screensaverFiles, tmp, (size_t)screensaverCount * 128);
  heap_caps_free(tmp);
  heap_caps_free(idx);
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
// Icon system: RGBA pixel-art icons from LittleFS in PSRAM.
// /icons/      → 20×20 px (emoji ticker + large weather icon)
// /icons_small/→ 10×10 px (small weather forecast icons)

#define ICON_RGBA_CH  4
#define ICON_W        20
#define ICON_H        20
#define ICON_BYTES    (ICON_W * ICON_H * ICON_RGBA_CH)
#define ICON_W_S      10
#define ICON_H_S      10
#define ICON_BYTES_S  (ICON_W_S * ICON_H_S * ICON_RGBA_CH)
#define ICON_W_R      32
#define ICON_H_R      32
#define ICON_BYTES_R  (ICON_W_R * ICON_H_R * ICON_RGBA_CH)
#define MAX_ICONS     48

struct IconEntry {
  char     name[32];
  uint8_t* data;
};

static IconEntry iconTable[MAX_ICONS];
static uint8_t   iconCount = 0;
static IconEntry iconTableSmall[MAX_ICONS];
static uint8_t   iconCountSmall = 0;

// Radio-Icons: Single-Slot-Cache + Fuzzy-Name-Liste.
// Nur das aktuell gespielte Logo im RAM (4 KB), kein Massenload.
// Fuzzy-Match: Slug-Liste im PSRAM-Cache — kein LittleFS-Scan in audio.loop().
static uint8_t  radioIconBuf[ICON_BYTES_R];
static char     radioIconCachedName[32]   = "";  // positive cache: slug → radioIconBuf
static char     radioIconFallbackName[32] = "";  // negative cache: slug → kein Icon vorhanden
static char*    radioIconSlugs            = nullptr; // PSRAM: N×32 Byte
static uint16_t radioIconSlugCount        = 0;

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

// Splits slug at '_' AND at alpha↔digit boundaries (e.g. "wdr4" → ["wdr","4"]).
// buf must be at least 64 bytes (slug + inserted separators).
static uint8_t radioSplitTokens(const char* slug, const char** toks, uint8_t maxToks,
                                 char* buf, size_t bufLen) {
  size_t j = 0;
  for (size_t i = 0; slug[i] && j < bufLen - 2; i++) {
    char c = slug[i];
    if (j > 0 && buf[j-1] != '_' && c != '_') {
      bool pa = (buf[j-1] >= 'a' && buf[j-1] <= 'z');
      bool cd = (c >= '0' && c <= '9');
      bool pd = (buf[j-1] >= '0' && buf[j-1] <= '9');
      bool ca = (c >= 'a' && c <= 'z');
      if ((pa && cd) || (pd && ca)) buf[j++] = '_';
    }
    buf[j++] = c;
  }
  buf[j] = '\0';
  uint8_t n = 0;
  for (char* p = strtok(buf, "_"); p && n < maxToks; p = strtok(nullptr, "_")) toks[n++] = p;
  return n;
}

// Token-by-token matching: exact=200 points, prefix=50 points.
// Alpha↔digit boundaries treated as token separators ("wdr4" → "wdr"+"4").
// Prevents "radio_ffn" → "90s90s_radio" winning (token "radio" is generic).
static uint16_t radioIconScore(const char* query, const char* candidate) {
  if (strcmp(query, candidate) == 0) return 400;
  char qa[64], ca[64];
  const char* qtok[12]; uint8_t qn = radioSplitTokens(query,     qtok, 12, qa, sizeof(qa));
  const char* ctok[12]; uint8_t cn = radioSplitTokens(candidate, ctok, 12, ca, sizeof(ca));
  uint16_t score = 0;
  for (uint8_t i = 0; i < qn; i++) {
    for (uint8_t j = 0; j < cn; j++) {
      if (strcmp(qtok[i], ctok[j]) == 0)
        score += 200;
      else if (strncmp(qtok[i], ctok[j], strlen(ctok[j])) == 0 ||
               strncmp(ctok[j], qtok[i], strlen(qtok[i])) == 0)
        score += 50;
    }
  }
  return score;
}

// Loads icon slugs for saved preset stations only (no full directory scan).
// Called from radioInit() after radioLoadPresets() and from radioSavePresets().
void radioIconSlugsLoad() {
  if (!radioIconSlugs)
    radioIconSlugs = (char*)heap_caps_malloc(256 * 32, MALLOC_CAP_SPIRAM);
  if (!radioIconSlugs) return;
  radioIconSlugCount       = 0;
  radioIconCachedName[0]   = '\0';
  radioIconFallbackName[0] = '\0';

  for (int i = 0; i < radioPresetCount; i++) {
    char slug[32];
    size_t j = 0;
    const char* n = radioPresets[i].name;
    for (size_t k = 0; n[k] && j + 1 < sizeof(slug); k++) {
      char c = n[k];
      if (c >= 'A' && c <= 'Z') c += 32;
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        slug[j++] = c;
      else if (j > 0 && slug[j-1] != '_')
        slug[j++] = '_';
    }
    while (j > 0 && slug[j-1] == '_') j--;
    slug[j] = '\0';
    if (!slug[0]) continue;
    char path[64];
    snprintf(path, sizeof(path), "/icons_radio/%s.rgba", slug);
    if (LittleFS.exists(path) && radioIconSlugCount < 256) {
      strlcpy(radioIconSlugs + radioIconSlugCount * 32, slug, 32);
      radioIconSlugCount++;
    }
  }
  logMsg("[icon] Slugs: %u von %d Presets gecacht", radioIconSlugCount, radioPresetCount);
}

static bool radioIconLoad(const char* slug) {
  char path[64];
  snprintf(path, sizeof(path), "/icons_radio/%s.rgba", slug);
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != ICON_BYTES_R) { if (f) f.close(); return false; }
  bool ok = (f.read(radioIconBuf, ICON_BYTES_R) == ICON_BYTES_R);
  f.close();
  return ok;
}

const uint8_t* GetRadioIcon(const char* name) {
  if (!name || !name[0]) return nullptr;
  if (strcmp(radioIconCachedName,   name) == 0) return radioIconBuf;
  if (strcmp(radioIconFallbackName, name) == 0) return nullptr;

  // "radio_xyz" → "xyz": generischer Prefix entfernen bevor gematcht wird
  const char* query = (strncmp(name, "radio_", 6) == 0 && name[6] != '\0') ? name + 6 : name;

  // Generic single words — too common in station names to match meaningfully
  static const char* const kStopwords[] = { "radio", "live", "fm", "online", "musik", nullptr };
  for (const char* const* sw = kStopwords; *sw; sw++) {
    if (strcmp(query, *sw) == 0) {
      strlcpy(radioIconFallbackName, name, sizeof(radioIconFallbackName));
      logMsg("[icon] '%s' → Fallback (generischer Name)", name);
      return nullptr;
    }
  }

  // 1. Exakter Datei-Match
  if (radioIconLoad(query)) {
    logMsg("[icon] '%s' → exact '%s'", name, query);
    radioIconFallbackName[0] = '\0';
    strlcpy(radioIconCachedName, name, sizeof(radioIconCachedName));
    return radioIconBuf;
  }

  // 2. Fuzzy-Match: gecachte Slug-Liste (PSRAM) — kein LittleFS-Scan in audio.loop()
  char bestSlug[32] = "";
  uint16_t bestScore = 0;
  for (uint16_t i = 0; i < radioIconSlugCount; i++) {
    const char* slug = radioIconSlugs + i * 32;
    uint16_t s = radioIconScore(query, slug);
    if (s > bestScore) { bestScore = s; strlcpy(bestSlug, slug, sizeof(bestSlug)); }
  }
  if (bestScore >= 100 && radioIconLoad(bestSlug)) {
    logMsg("[icon] '%s' → fuzzy '%s' (score=%u)", name, bestSlug, bestScore);
    radioIconFallbackName[0] = '\0';
    strlcpy(radioIconCachedName, name, sizeof(radioIconCachedName));
    return radioIconBuf;
  }
  strlcpy(radioIconFallbackName, name, sizeof(radioIconFallbackName));
  logMsg("[icon] '%s' → Fallback (best='%s' score=%u)", name, bestSlug, bestScore);
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
    esp_task_wdt_reset();
    // Copy name before close() — f.name() returns full path in LittleFS, extract basename
    const char* rawname = f.name();
    const char* slash = strrchr(rawname, '/');
    char fname[64];
    strncpy(fname, slash ? slash + 1 : rawname, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    uint32_t sz = f.size();
    size_t flen = strlen(fname);
    bool isRgba = flen > 5 && strcmp(fname + flen - 5, ".rgba") == 0;
    bool isDir = f.isDirectory();
    f.close();
    if (!isDir && isRgba && sz == expected_bytes) {
      char path[64];
      snprintf(path, sizeof(path), "%s/%s", dir_path, fname);
      File rf = LittleFS.open(path, "r");
      if (rf) {
        uint8_t* buf = (uint8_t*)heap_caps_malloc(expected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf && rf.read(buf, expected_bytes) == expected_bytes) {
          strncpy(table[count].name, fname, sizeof(table[count].name) - 1);
          table[count].name[sizeof(table[count].name) - 1] = '\0';
          // Endung ".rgba" abschneiden
          size_t nlen = strlen(table[count].name);
          if (nlen > 5) table[count].name[nlen - 5] = '\0';
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

void CleanupTmpFiles() {
  static const char* scanDirs[] = { "/", "/icons", "/icons_small", nullptr };
  uint8_t count = 0;
  for (int d = 0; scanDirs[d]; d++) {
    File dir = LittleFS.open(scanDirs[d]);
    if (!dir || !dir.isDirectory()) continue;
    File f = dir.openNextFile();
    while (f) {
      esp_task_wdt_reset();
      if (!f.isDirectory() && String(f.name()).endsWith(".tmp")) {
        char path[128];
        snprintf(path, sizeof(path), "%s", f.name());  // Pfad sichern vor close()
        f.close();
        LittleFS.remove(path);
        count++;
      } else {
        f.close();
      }
      f = dir.openNextFile();
    }
    dir.close();
  }
  if (count > 0) logMsg("Startup: %u verwaiste .tmp-Datei(en) geloescht", count);
}

void LoadIcons() {
  loadIconSet("/icons", iconTable, iconCount, MAX_ICONS, ICON_BYTES);
  esp_task_wdt_reset();
  logMsg("Icons: %d gross geladen", iconCount);
  // radioIconSlugsLoad() wird nach radioLoadPresets() in radioInit() aufgerufen
}

// SPI-Speed-Stufen: 40→25→20→8 MHz (ESP32-S3 PCB), bei Fehlschlag schrittweise runter.
// Wird von InitSDCard() und /mount_sd genutzt — spiSD muss bereits initialisiert sein.
bool sdSpiMountWithFallback() {
#ifdef CONFIG_IDF_TARGET_ESP32S3
  static const uint32_t speeds[] = {40000000, 25000000, 20000000, 8000000};
  static const uint8_t  nSpeeds  = 4;
#else
  static const uint32_t speeds[] = {4000000, 2000000, 1000000};
  static const uint8_t  nSpeeds  = 3;
#endif
  for (uint8_t i = 0; i < nSpeeds; i++) {
    uint32_t spd = speeds[i < nSpeeds ? i : nSpeeds - 1];
    if (SD.begin(SD_CS, spiSD, spd)) {
      logMsg("SD: Mount OK bei %lu MHz (Versuch %d)", spd / 1000000, i + 1);
      return true;
    }
    logMsg("SD: Versuch %d fehlgeschlagen (%lu MHz)", i + 1, spd / 1000000);
    SD.end();
    delay(500);
  }
  return false;
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
  mounted = sdSpiMountWithFallback();
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
  if (!screensaverFavorites || !screensaverFavorites[0]) return false;
  char p[280];
  snprintf(p, sizeof(p), "%s\n", path);
  return strstr(screensaverFavorites, p) != nullptr;
}

void SaveFavorites() {
  File f = LittleFS.open("/screensaver_favorites.txt", "w");
  if (f && screensaverFavorites) { f.print(screensaverFavorites); f.close(); }
}

void toggleFavorite(const char* path) {
  if (!screensaverFavorites) return;
  char p[280];
  snprintf(p, sizeof(p), "%s\n", path);
  char* pos = strstr(screensaverFavorites, p);
  if (pos) {
    size_t plen = strlen(p);
    memmove(pos, pos + plen, strlen(pos + plen) + 1);
  } else {
    strlcat(screensaverFavorites, p, SCREENSAVER_FAV_BUF);
  }
  SaveFavorites();
}

void LoadFavorites() {
  if (!screensaverFavorites) return;
  File f = LittleFS.open("/screensaver_favorites.txt", "r");
  if (f) {
    size_t n = f.read((uint8_t*)screensaverFavorites, SCREENSAVER_FAV_BUF - 1);
    screensaverFavorites[n] = '\0';
    f.close();
  }
}

// ── Ignore-Liste ──────────────────────────────────────────────────────────────
bool isIgnored(const char* path) {
  if (!screensaverIgnore || !screensaverIgnore[0]) return false;
  char p[280];
  snprintf(p, sizeof(p), "%s\n", path);
  return strstr(screensaverIgnore, p) != nullptr;
}

void SaveIgnore() {
  File f = LittleFS.open("/screensaver_ignore.txt", "w");
  if (f && screensaverIgnore) { f.print(screensaverIgnore); f.close(); }
}

void toggleIgnore(const char* path) {
  if (!screensaverIgnore) return;
  char p[280];
  snprintf(p, sizeof(p), "%s\n", path);
  char* pos = strstr(screensaverIgnore, p);
  if (pos) {
    size_t plen = strlen(p);
    memmove(pos, pos + plen, strlen(pos + plen) + 1);
  } else {
    strlcat(screensaverIgnore, p, SCREENSAVER_IGNORE_BUF);
  }
  SaveIgnore();
}

void LoadIgnore() {
  if (!screensaverIgnore) return;
  File f = LittleFS.open("/screensaver_ignore.txt", "r");
  if (f) {
    size_t n = f.read((uint8_t*)screensaverIgnore, SCREENSAVER_IGNORE_BUF - 1);
    screensaverIgnore[n] = '\0';
    f.close();
  }
}
// ─────────────────────────────────────────────────────────────────────────────

// ── Dateilisten-Cache ─────────────────────────────────────────────────────────
void addScreensaverFile(const char* path) {
  if (isIgnored(path)) return;
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
  strncpy(screensaverFiles[screensaverCount], path, 127);
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
  char lineBuf[128];
  while (f.available()) {
    int len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
    lineBuf[len] = '\0';
    while (len > 0 && (lineBuf[len-1] == '\r' || lineBuf[len-1] == ' ')) lineBuf[--len] = '\0';
    if (len > 0) addScreensaverFile(lineBuf);
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

// Delete all folder caches
void InvalidateAllFolderCaches() {
  LittleFS.remove("/screensaver_cache.txt");  // alter globaler Cache
  File root = LittleFS.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    esp_task_wdt_reset();
    String name = String(f.name());  // f.name() kann mit oder ohne "/" kommen
    if ((name.startsWith("/sc_") || name.startsWith("sc_")) && name.endsWith(".bin")) {
      f.close();
      String removePath = name.startsWith("/") ? name : ("/" + name);
      LittleFS.remove(removePath);
      logMsg("Cache: %s geloescht", removePath.c_str());
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

// Stubs for legacy call sites
void SaveScreensaverCache() {}
bool TryLoadScreensaverCache() { return false; }

#define GIF_AUDIO_CACHE_FILE "/gif_audio_cache.bin"

// Replaces a PSRAM cache pointer atomically.
// Allocates the new buffer first, swaps the pointer, then frees the old one —
// so no null window is possible if a web-server task reads concurrently.
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
  // PSRAM-Puffer direkt parsen — keine String-Kopie in internen Heap
  const char* p = cachedGifAudioFiles;
  if (!p || *p != '[') { f.close(); return; }
  p++;  // skip '['
  while (*p && *p != ']') {
    if (*p != '{') { p++; continue; }
    const char* end = strchr(p, '}');
    if (!end) break;
    const char* ni = strstr(p, "\"name\":\"");
    const char* si = strstr(p, "\"size\":");
    if (ni && ni < end) {
      const char* ns = ni + 8;
      const char* ne = strchr(ns, '"');
      if (ne && ne < end) {
        uint32_t sz = (si && si < end) ? (uint32_t)strtoul(si + 7, nullptr, 10) : 0;
        f.write((const uint8_t*)ns, ne - ns);
        f.printf("|%u\n", sz);
      }
    }
    p = end + 1;
    if (*p == ',') p++;
  }
  f.close();
  logMsg("GifAudioCache: gespeichert (%s)", GIF_AUDIO_CACHE_FILE);
}

bool TryLoadGifAudioCache() {
  File f = LittleFS.open(GIF_AUDIO_CACHE_FILE, "r");
  if (!f) return false;
  size_t fileSize = f.size();
  if (fileSize == 0) { f.close(); return false; }
  // JSON output is ~4× larger than the binary file (name|size\n → {"name":"...","size":...})
  size_t bufSize = fileSize * 4 + 16;
  char* buf = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); return false; }
  size_t pos = 0;
  buf[pos++] = '[';
  bool first = true;
  uint16_t count = 0;
  char line[256];
  while (f.available() && pos + 128 < bufSize) {
    int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[len] = '\0';
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == ' ')) line[--len] = '\0';
    if (len == 0) continue;
    char* sep = strchr(line, '|');
    uint32_t sz = 0;
    if (sep) { *sep = '\0'; sz = (uint32_t)strtoul(sep + 1, nullptr, 10); }
    if (!first) buf[pos++] = ',';
    pos += snprintf(buf + pos, bufSize - pos - 4, "{\"name\":\"%s\",\"size\":%u}", line, sz);
    first = false;
    count++;
  }
  f.close();
  if (count == 0) { heap_caps_free(buf); return false; }
  buf[pos++] = ']';
  buf[pos] = '\0';
  char* exact = (char*)heap_caps_realloc(buf, pos + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (exact) buf = exact;
  char* old = cachedGifAudioFiles;
  cachedGifAudioFiles = buf;
  if (old) heap_caps_free(old);
  logMsg("Cache[%s]: %d Dateien geladen", GIF_AUDIO_DIR, count);
  return true;
}

void InvalidateGifAudioCache() {
  LittleFS.remove(GIF_AUDIO_CACHE_FILE);
  logMsg("GifAudioCache: invalidiert");
}

// Leert screensaverFiles sofort unter Mutex — kein Fenster wo Webserver-Callbacks
// auf einem halbfertigen Puffer lesen.
void ClearScreensaverFilesNow() {
  if (screensaverFilesMutex) {
    xSemaphoreTake(screensaverFilesMutex, portMAX_DELAY);
    char (*oldBuf)[128] = screensaverFiles;
    screensaverFiles         = nullptr;
    screensaverCount         = 0;
    screensaverFilesCapacity = 0;
    xSemaphoreGive(screensaverFilesMutex);
    if (oldBuf) heap_caps_free(oldBuf);
  } else {
    heap_caps_free(screensaverFiles);
    screensaverFiles         = nullptr;
    screensaverCount         = 0;
    screensaverFilesCapacity = 0;
  }
}

// Full GIF-audio rescan: set RAM cache to "[]" immediately (poll sees empty list),
// delete disk cache, trigger scan in main loop.
void TriggerGifAudioRescan() {
  psramCacheSet(&cachedGifAudioFiles, "[]");
  InvalidateGifAudioCache();
  gifAudioRefreshNeeded = true;
}

// ─────────────────────────────────────────────────────────────────────────────

void LoadScreensaverFiles() {
  ClearScreensaverFilesNow();

  // Favorites mode: load paths directly from the favorites list
  if (screensaverPaths == "FAV") {
    size_t favLen = screensaverFavorites ? strlen(screensaverFavorites) : 0;
    char* favBuf = (char*)heap_caps_malloc(favLen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (favBuf) {
      if (screensaverFavorites) memcpy(favBuf, screensaverFavorites, favLen + 1);
      else                      favBuf[0] = '\0';
      char* saveptr = nullptr;
      char* token = strtok_r(favBuf, "\n", &saveptr);
      while (token) {
        char* end = token + strlen(token) - 1;
        while (end >= token && (*end == '\r' || *end == ' ')) *end-- = '\0';
        while (*token == ' ') token++;
        if (*token && !(strncmp(token, "SD:", 3) == 0 && !sdCardAvailable))
          addScreensaverFile(token);
        token = strtok_r(nullptr, "\n", &saveptr);
      }
      heap_caps_free(favBuf);
    }
    logMsg("LoadScreensaver: %d Favoriten geladen", screensaverCount);
    if (screensaverCount > 0) goto done;
    logMsg("LoadScreensaver: Keine Favoriten verfügbar, Fallback auf LittleFS");
  }

  // All selected paths (comma-separated); "FS:" = LittleFS /screensaver/
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
            esp_task_wdt_reset();
            if (!f.isDirectory()) {
              const char* fname = f.name();
              // LittleFS f.name() returns full path — extract basename
              const char* base = strrchr(fname, '/');
              base = base ? base + 1 : fname;
              if (base[0] != '.') {
                const char* dot = strrchr(base, '.');
                if (dot && (strcmp(dot, ".raw") == 0 || strcmp(dot, ".gif") == 0 || strcmp(dot, ".GIF") == 0)) {
                  char fsBuf[80];
                  snprintf(fsBuf, sizeof(fsBuf), "FS:/screensaver/%s", base);
                  addScreensaverFile(fsBuf);
                }
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

      // FIX 11: try per-folder cache — skip scan if cache is present
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
              snprintf(msg, sizeof(msg), "Reading SD %-5d", (int)screensaverCount);
              display->DisplayText(msg, 0, 13, 255, 180, 0);
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
        // FIX 11: save folder cache for next boot
        if (newFiles > 0) SaveFolderCache(sdPath, countBefore, newFiles);
      } else {
        logMsg("LoadScreensaver: Ordner nicht gefunden: %s — heap=%u errno=%d",
               sdPath.c_str(), (unsigned)esp_get_free_internal_heap_size(), errno);
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
        esp_task_wdt_reset();
        if (!f.isDirectory()) {
          const char* fname = f.name();
          if (fname && fname[0] != '.') {
            const char* dot = strrchr(fname, '.');
            if (dot && (strcmp(dot, ".raw") == 0 || strcmp(dot, ".gif") == 0 || strcmp(dot, ".GIF") == 0)) {
              char fsFallBuf[128];
              snprintf(fsFallBuf, sizeof(fsFallBuf), "FS:/screensaver/%s", fname);
              addScreensaverFile(fsFallBuf);
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

// Liest/schreibt UUID auf SD-Karte (/zedmd_id.txt) und vergleicht mit gespeicherter UUID in
// LittleFS (/sd_card_id.txt). Bei Kartenwechsel: alle Ordner-Caches invalidieren.
void checkSDCardIdentity() {
  if (!sdCardAvailable) return;

  char newUUID[37] = {0};

  File f = SD.open("/zedmd_id.txt");
  if (f) {
    size_t len = f.readBytes(newUUID, 36);
    newUUID[len] = '\0';
    f.close();
  }

  if (strlen(newUUID) < 8) {
    uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
    snprintf(newUUID, sizeof(newUUID), "%08x-%04x-%04x-%04x-%04x%08x",
             a, b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d);
    File fw = SD.open("/zedmd_id.txt", FILE_WRITE);
    if (fw) { fw.print(newUUID); fw.close(); }
    logMsg("SD: Neue Karten-ID generiert: %s", newUUID);
  }

  char storedUUID[37] = {0};
  File lf = LittleFS.open("/sd_card_id.txt", "r");
  if (lf) {
    size_t len = lf.readBytes(storedUUID, 36);
    storedUUID[len] = '\0';
    lf.close();
  }

  if (strcmp(newUUID, storedUUID) != 0) {
    if (storedUUID[0] != '\0') {
      logMsg("SD: Karte gewechselt — alle Ordner-Caches invalidiert");
      InvalidateAllFolderCaches();
    }
    File lfw = LittleFS.open("/sd_card_id.txt", "w");
    if (lfw) { lfw.print(newUUID); lfw.close(); }
  }
}

// Listet alle Ordner auf der SD Karte — schreibt direkt in cachedSDFolders (kein String-Heap)
void GetSDFolders() {
  if (!sdCardAvailable) { psramCacheSet(&cachedSDFolders, "[]"); return; }
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    psramCacheSet(&cachedSDFolders, "[]");
    return;
  }
  const size_t bufSize = 30000;  // 500 Ordner × ~60 Zeichen
  char* buf = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { root.close(); return; }
  size_t pos = 0;
  buf[pos++] = '[';
  bool first = true;
  int scanned = 0;
  File f = root.openNextFile();
  while (f && scanned < 500 && pos + 64 < bufSize) {
    scanned++;
    if (f.isDirectory()) {
      const char* name = f.name();
      size_t nlen = strlen(name);
      if (nlen > 0 && name[0] != '.' && strncmp(name, "System", 6) != 0) {
        if (!first) buf[pos++] = ',';
        buf[pos++] = '"';
        memcpy(buf + pos, name, nlen);
        pos += nlen;
        buf[pos++] = '"';
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
  buf[pos++] = ']';
  buf[pos] = '\0';
  char* exact = (char*)heap_caps_realloc(buf, pos + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (exact) buf = exact;
  char* old = cachedSDFolders;
  cachedSDFolders = buf;
  if (old) heap_caps_free(old);
}

void setup() {
  Serial.begin(115200);
  esp_task_wdt_reset();  // WDT budget may be partially consumed after USB flash — reset before delay()
  delay(2000);
  esp_task_wdt_reset();  // delay(2000) exhausted WDT budget — reset before the long setup()
  // Allocate PSRAM buffers and mutexes early — before audio/WiFi/codec init
  logBuffer             = (char (*)[LOG_LINE_LEN])heap_caps_calloc(
                            LOG_LINES, LOG_LINE_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uncompressBuffer      = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  screensaverFilesMutex = xSemaphoreCreateMutex();
  // String caches and lists in PSRAM — allocated once, prevents heap fragmentation in steady state
  cachedSDFolders      = (char*)heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  cachedGifAudioFiles  = (char*)heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  cachedSdFiles        = (char*)heap_caps_malloc(3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  screensaverFavorites = (char*)heap_caps_calloc(SCREENSAVER_FAV_BUF,    1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  screensaverIgnore    = (char*)heap_caps_calloc(SCREENSAVER_IGNORE_BUF, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (cachedSDFolders)     strcpy(cachedSDFolders,     "[]");
  if (cachedGifAudioFiles) strcpy(cachedGifAudioFiles,  "[]");
  if (cachedSdFiles)       strcpy(cachedSdFiles,        "[]");
  logMsg("=== ZeDMD booting ===");
  logMsg("[HEAP] start: free=%u", (uint32_t)ESP.getFreeHeap());
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

  // Allocate renderBuffer early — needed by DisplayLogo()/Render()
  // before the normal allocation further below is reached
  for (uint8_t i = 0; i < NUM_RENDER_BUFFERS; i++) {
#ifdef BOARD_HAS_PSRAM
    renderBuffer[i] = (uint8_t *)heap_caps_malloc(TOTAL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
#else
    renderBuffer[i] = (uint8_t *)malloc(TOTAL_BYTES);
#endif
    if (renderBuffer[i]) memset(renderBuffer[i], 0, TOTAL_BYTES);
  }

  bool fileSystemOK;
  logMsg("LittleFS.begin...");
  if (fileSystemOK = LittleFS.begin()) {
    logMsg("LittleFS OK");
    // lfs_fs_traverse_() beim Boot einmalig — auf fragmentiertem LFS >5s, friert Core0 ein.
    // Wird gecacht und danach nur noch aus lfsTotal/lfsUsed gelesen (kein Live-Aufruf im Webserver).
    esp_task_wdt_reset();
    lfsTotal = (uint32_t)LittleFS.totalBytes();
    lfsUsed  = (uint32_t)LittleFS.usedBytes();
    esp_task_wdt_reset();
    diagBoot();
    logMsg("[HEAP] nach diagBoot: free=%u", (uint32_t)ESP.getFreeHeap());

    // Display sofort nach diagBoot initialisieren — Logo erscheint bei ~2s,
    // bevor die Load*-Kette (CleanupTmpFiles + 8× Settings) das Display blockiert.
    // renderBuffer ist bereits vor LittleFS.begin() allokiert.
    esp_task_wdt_reset();  // diagBoot() kann bei mehreren Crash-Logs >1s dauern
#ifdef DISPLAY_RM67162_AMOLED
    display = new Rm67162Amoled();
#elif defined(DISPLAY_LED_MATRIX)
    display = new LedMatrix();
#endif
    display->SetBrightness(8);  // Default — wird nach LoadLum() auf echten Wert gesetzt
    DisplayLogo();
    display->DisplayText("booting", 0, 26, 255, 255, 255);
    esp_task_wdt_reset();  // Load*-Kette danach kann 10+ Sekunden dauern

    CleanupTmpFiles();
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
    display->SetBrightness(brightness);  // echten gespeicherten Wert setzen
    LoadDebug();

    logMsg("[HEAP] vor LoadIcons: free=%u", (uint32_t)ESP.getFreeHeap());
    esp_task_wdt_reset();  // loadIconSet() liest 74 RGBA-Dateien aus LittleFS — kann >4s dauern
    LoadIcons();
    logMsg("[HEAP] nach LoadIcons: free=%u", (uint32_t)ESP.getFreeHeap());
    LoadScreensaverLum();
    LoadScreensaverDuration();
    LoadScreensaverShuffle();
    LoadScreensaverStrictTimer();
    LoadGifAudioEnabled();
    LoadScreensaverMode();
    LoadDisplayText();
    LoadClockColors();
#ifdef ZEDMD_WIFI
    LoadMqttConfig();
#endif
    LoadWeatherConfig();
    LoadTimezoneConfig();
    LoadDisplayTimer();
#ifdef ZEDMD_WIFI
    weatherInit();
#endif
    LoadFavorites();
    LoadIgnore();
    logMsg("[HEAP] nach Load*-Kette: free=%u", (uint32_t)ESP.getFreeHeap());
    esp_task_wdt_reset();  // Load*() chain + LittleFS I/O can take >2s
    InitSDCard();
    if (!sdCardAvailable) sdCardWarningPending = true;
    checkSDCardIdentity();  // known card? otherwise invalidate caches
    esp_task_wdt_reset();   // checkSDCardIdentity may write SD+LittleFS
    GetSDFolders();  // populate cache at boot — writes directly into cachedSDFolders
    logMsg("[HEAP] nach SD-Init: free=%u", (uint32_t)ESP.getFreeHeap());
    esp_task_wdt_reset();  // InitSDCard + GetSDFolders can take >1s on slow cards
    LoadScreensaverPaths();
    screensaverReloadNeeded = true;  // Dateien im Hauptloop laden (nach Display+WiFi Init)
    LoadUdpDelay();
#ifdef ZEDMD_HD_HALF
    LoadYOffset();
#endif
  } else {
    logMsg("LittleFS FAILED — Panel-Defaults gelten");
    // Display mit Defaults initialisieren (kein LittleFS → kein Logo)
#ifdef DISPLAY_RM67162_AMOLED
    display = new Rm67162Amoled();
#elif defined(DISPLAY_LED_MATRIX)
    display = new LedMatrix();
#endif
    logMsg("Display OK (no LittleFS)");
    display->SetBrightness(brightness);
  }

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

  // renderBuffer already allocated above (before DisplayLogo) — just check for out-of-memory here
  for (uint8_t i = 0; i < NUM_RENDER_BUFFERS; i++) {
    if (nullptr == renderBuffer[i]) {
      display->DisplayText("out of memory", 0, 0, 255, 0, 0);
      while (1);
    }
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
      esp_task_wdt_reset();
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
      logMsg("[HEAP] vor StartWiFi: free=%u", (uint32_t)ESP.getFreeHeap());
      StartWiFi();
      logMsg("[HEAP] nach StartWiFi: free=%u", (uint32_t)ESP.getFreeHeap());
      clockInit();  // NTP nach WiFi Start
#ifdef WEBRADIO_ENABLED
      radioInit();
      logMsg("[HEAP] nach radioInit: free=%u", (uint32_t)ESP.getFreeHeap());
#endif
      mqttClient.setServer(mqttServer.c_str(), mqttPort);
      mqttClient.setCallback(onMqttMessage);
      mqttClient.setBufferSize(2048);
      vTaskDelay(pdMS_TO_TICKS(1000));  // WiFi-Stack nach Verbindungsaufbau stabilisieren
      logMsg("[HEAP] nach WiFi-Stabilisierung: free=%u", (uint32_t)ESP.getFreeHeap());
      mqttConnect();
      xTaskCreatePinnedToCore(mqttTask, "mqttTask", 8192, NULL, 1, NULL, 0);
      logMsg("[HEAP] nach mqttTask: free=%u", (uint32_t)ESP.getFreeHeap());
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

  // Keep uptime in RTC for boot-loop protection — no flash write, µs overhead
  {
    static uint32_t lastUptimeUpdate = 0;
    uint32_t nowSec = millis() / 1000;
    if (nowSec - lastUptimeUpdate >= 5) {
      lastUptimeUpdate = nowSec;
      rtcLastUptime    = nowSec;
    }
  }

  // Heap monitor: log internal SRAM heap every 30s — stored in RTC ring buffer
  // so the next crash log shows heap history before the crash.
  {
    static uint32_t lastHeapLog = 0;
    uint32_t now = millis();
    if (now - lastHeapLog >= 30000) {
      lastHeapLog = now;
      logMsg("Heap: free=%u minEver=%u PSRAM=%u",
             (uint32_t)ESP.getFreeHeap(),
             (uint32_t)ESP.getMinFreeHeap(),
             (uint32_t)ESP.getFreePsram());
    }
    if (now - displayTimerLastCheck >= 60000) {
      displayTimerLastCheck = now;
      CheckDisplayTimer();
    }
  }

  CheckMenuButton();

  // Reload screensaver files when path changed
  // screensaverLoadRunning prevents re-entrant calls on rapid path changes
  if (screensaverReloadNeeded && !screensaverLoadRunning) {
    screensaverReloadNeeded = false;
    screensaverLoadRunning  = true;
    SaveScreensaverPaths();
    display->DisplayText("loading...", 0, 26, 180, 180, 180);
    Render();
    LoadScreensaverFiles();
    screensaverLoadRunning = false;
    if (screensaverCount > 0 && sdCardAvailable) {
      display->DisplayText("SD OK     ", 0, 26, 0, 255, 100);
    } else {
      display->DisplayText("LittleFS OK", 0, 26, 180, 180, 180);
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
    GetSDFolders();
  }

  // GIF-Audio-Dateiliste cachen (statt SD-I/O im Webserver-Callback)
  if (gifAudioRefreshNeeded && sdCardAvailable) {
    gifAudioRefreshNeeded = false;
    if (!TryLoadGifAudioCache()) {
      logMsg("GifAudio: scanne %s ...", GIF_AUDIO_DIR);
      if (!SD.exists(GIF_AUDIO_DIR)) SD.mkdir(GIF_AUDIO_DIR);
      File dir = SD.open(GIF_AUDIO_DIR);
      // PSRAM-Puffer: max ~3000 Dateien × 100 Byte/Eintrag — kein String-Heap
      const size_t gifBufSize = 300 * 1024;
      char* jsonBuf = (char*)heap_caps_malloc(gifBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      size_t jsonPos = 0;
      if (jsonBuf) jsonBuf[jsonPos++] = '[';
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
        char name[256];
        strncpy(name, f.name(), sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        bool isDir = f.isDirectory();
        uint32_t sz = f.size();
        f.close();
        if (name[0] != '.' && !strstr(name, ".tmp") && !isDir && jsonBuf && jsonPos + 128 < gifBufSize) {
          if (!first) jsonBuf[jsonPos++] = ',';
          jsonPos += snprintf(jsonBuf + jsonPos, gifBufSize - jsonPos - 4,
                              "{\"name\":\"%s\",\"size\":%u}", name, sz);
          first = false;
          gifAudioCount++;
          if ((gifAudioCount % 50) == 0) {
#ifdef WEBRADIO_ENABLED
            if (!radioIsPlaying) {
#endif
              char msg[24];
              snprintf(msg, sizeof(msg), "GifAudio %-5d", gifAudioCount);
              display->DisplayText(msg, 0, 13, 255, 180, 0);
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
        if (jsonBuf) heap_caps_free(jsonBuf);
      } else if (jsonBuf) {
        jsonBuf[jsonPos++] = ']';
        jsonBuf[jsonPos] = '\0';
        char* exact = (char*)heap_caps_realloc(jsonBuf, jsonPos + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (exact) jsonBuf = exact;
        char* old = cachedGifAudioFiles;
        cachedGifAudioFiles = jsonBuf;
        if (old) heap_caps_free(old);
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
    GetSDFolders();
  }
  if (sdFilesInvalidateNeeded) {
    sdFilesInvalidateNeeded = false;
    psramCacheSet(&cachedSdFiles, String("[]"));
  }

  if (iconsReloadNeeded) {
    iconsReloadNeeded = false;
    esp_task_wdt_reset();
    LoadIcons();
  }

  // Cache SD file list for the selected folder
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

  // WiFi reconnect on lost connection — at most every 5s, no delay/return so
  // screensaver and GIF playback continue during reconnect
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

    static bool weatherSmallIconTestRendered = false;
    if (weatherSmallIconTestActive) {
      if (!weatherSmallIconTestRendered) {
        weatherSmallIconTest();
        weatherSmallIconTestRendered = true;
      }
      return;
    }
    weatherSmallIconTestRendered = false;

    if (wifiActive && !serverRunning) {
      // @see https://github.com/ESP32Async/ESPAsyncWebServer/issues/21
      // StartServer();
    }

    // ── Screensaver / Clock Logik ─────────────────────────────────────────
    // Guard BEFORE counter increment: prevents DisplayUpdate() or
    // PlayGIF() from overwriting the setup screen while it should be visible.
    if (setupScreenUntil > 0 && millis() < setupScreenUntil) return;
    setupScreenUntil = 0;

    if (!logoActive) {
      logoActive = true;
      logoWaitCounter = 199;
    }

    ++logoWaitCounter;

#ifdef ZEDMD_WIFI
    if (10 == logoWaitCounter) {   // WiFi: 2s Logo → direkt Screensaver
#else
    if (125 == logoWaitCounter) {  // USB: 25s Logo
      DisplayUpdate();
    }
    if (250 == logoWaitCounter) {  // USB: 25s → Screensaver
#endif
      screensaverIndex = 0;
      screensaverRAWShowStart = 0;
      if (screensaverCount > 0) {
        String firstFile = String(screensaverFiles[0]);
        if (firstFile.endsWith(".gif") || firstFile.endsWith(".GIF")) {
          ApplyBrightness(screensaverBrightness);
          bool isMixedMode = (screensaverMode == 2 || screensaverMode == 4);
          uint32_t endTime = screensaverStrictTimer ? (millis() + (uint32_t)screensaverDuration * 1000) : 0;
          bool loopUntilEnd = screensaverStrictTimer || (screensaverPaused && !isMixedMode);
          PlayGIF(firstFile, endTime, true, loopUntilEnd);
          if (transportActive) return;
          if (screensaverReloadNeeded) return;
#ifdef WEBRADIO_ENABLED
          if (radioDisplayActive) return;
#endif
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

    if (logoWaitCounter > ssThreshold) {

#ifdef WEBRADIO_ENABLED
      // Auto-Aus wenn Timer abgelaufen
      if (radioDisplayActive && radioDisplayUntil > 0 && millis() >= radioDisplayUntil) {
        radioDisplayActive = false;
        radioDisplayUntil  = 0;
      }
      if (radioDisplayActive) {
        DisplayRadio();
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
      }
#endif

      // Play directly requested GIF/RAW immediately (before all mode checks)
      if (forcePlayPending && forcePlayFile.length() > 0) {
        forcePlayPending = false;
        String f = forcePlayFile;
        forcePlayFile = "";
        currentlyPlayingFile = f;  // for screensaver_current endpoint
        for (uint16_t i = 0; i < screensaverCount; i++) {
          if (String(screensaverFiles[i]) == f) { screensaverIndex = i; break; }
        }
        ApplyBrightness(screensaverBrightness);
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
#ifdef WEBRADIO_ENABLED
        if (radioDisplayActive) return;
#endif
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

#ifdef FONT_TEST_ENABLED
      if (fontTestActive && fontTestEnd > 0 && millis() >= fontTestEnd) {
        fontTestActive = false;
        fontTestEnd    = 0;
        clockColorChanged = true;
      }
      if (fontTestActive) {
        if (fontTestNeedsRender) {
          fontTestNeedsRender = false;
          for (int i = 0; i < NUM_RENDER_BUFFERS; i++) memset(renderBuffer[i], 0, TOTAL_BYTES);
          if (NUM_RENDER_BUFFERS > 1) memset(renderBuffer[lastRenderBuffer], 1, TOTAL_BYTES);
          ApplyBrightness(screensaverBrightness);
          display->RenderFontTestToBuffer(renderBuffer[currentRenderBuffer],
                                          fontTestText, fontTestFont,
                                          fontTestLines,
                                          fontTestR, fontTestG, fontTestB);
          Render();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
      }
#endif  // FONT_TEST_ENABLED

      // Modus 1: Clock only
      if (screensaverMode == 1) {
        clockDisplay();
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
      }

      // Modus 2: Clock + Screensaver (im Wechsel, jeweils screensaverDuration Sekunden)
      if (screensaverMode == 2) {
        uint32_t now = millis();
        if (clockPhaseStart == 0) clockPhaseStart = now;
        if ((now - clockPhaseStart) >= (uint32_t)screensaverDuration * 1000) {
          showingClock = !showingClock;  // GIF ↔ Uhr
          clockPhaseStart = now;
          if (showingClock) clockColorChanged = true;
        }
        if (showingClock) {
          clockDisplay();
          vTaskDelay(pdMS_TO_TICKS(1000));
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
          if ((now - weatherPhaseStart) >= (uint32_t)screensaverDuration * 1000) {
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
        // weatherPage == 2: screensaver phase falls through to GIF code below
      }

      // Modus 5: Lauftext dauerhaft
      if (screensaverMode == 5) {
        if (screensaverTextNeedsClear) {
          display->ClearScreen();
          for (int i = 0; i < NUM_RENDER_BUFFERS; i++) memset(renderBuffer[i], 0, TOTAL_BYTES);
          screensaverTextNeedsClear = false;
        }
        if (displayTextContent[0]) {
          display->RenderTextGFXToBuffer(renderBuffer[currentRenderBuffer],
                                         displayTextContent, screensaverTextScrollX,
                                         displayTextR, displayTextG, displayTextB);
          Render();
          int16_t textW = (int16_t)display->GetTextGFXWidth(displayTextContent);
          if (--screensaverTextScrollX < -textW) screensaverTextScrollX = TOTAL_WIDTH;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
      }

      // Modus 6: Uhr + Lauftext im Wechsel
      if (screensaverMode == 6) {
        uint32_t now = millis();
        if (clockPhaseStart == 0) clockPhaseStart = now;
        if ((now - clockPhaseStart) >= (uint32_t)screensaverDuration * 1000) {
          showingClock = !showingClock;
          clockPhaseStart = now;
          if (showingClock) {
            clockColorChanged = true;
          } else {
            screensaverTextScrollX    = TOTAL_WIDTH;
            screensaverTextNeedsClear = true;
          }
        }
        if (showingClock) {
          clockDisplay();
          vTaskDelay(pdMS_TO_TICKS(1000));
          return;
        }
        if (screensaverTextNeedsClear) {
          display->ClearScreen();
          for (int i = 0; i < NUM_RENDER_BUFFERS; i++) memset(renderBuffer[i], 0, TOTAL_BYTES);
          screensaverTextNeedsClear = false;
        }
        if (displayTextContent[0]) {
          display->RenderTextGFXToBuffer(renderBuffer[currentRenderBuffer],
                                         displayTextContent, screensaverTextScrollX,
                                         displayTextR, displayTextG, displayTextB);
          Render();
          int16_t textW = (int16_t)display->GetTextGFXWidth(displayTextContent);
          if (--screensaverTextScrollX < -textW) screensaverTextScrollX = TOTAL_WIDTH;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
      }

      // Play directly requested GIF/RAW immediately
      if (forcePlayPending && forcePlayFile.length() > 0) {
        forcePlayPending = false;
        String f = forcePlayFile;
        forcePlayFile = "";
        currentlyPlayingFile = f;  // for screensaver_current endpoint
        for (uint16_t i = 0; i < screensaverCount; i++) {
          if (String(screensaverFiles[i]) == f) { screensaverIndex = i; break; }
        }
        ApplyBrightness(screensaverBrightness);
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
#ifdef WEBRADIO_ENABLED
        if (radioDisplayActive) return;
#endif
      }

      // Mode 0, 2 or 4 (screensaver part): play GIF/RAW
      if (screensaverCount > 0) {
        currentlyPlayingFile = "";  // normal screensaver takes over
        String currentFile = String(screensaverFiles[screensaverIndex]);
        if (currentFile.endsWith(".gif") || currentFile.endsWith(".GIF")) {
          ApplyBrightness(screensaverBrightness);
          bool isMixedMode = (screensaverMode == 2 || screensaverMode == 4);
          // Paused+non-mixed: loop forever; otherwise always loop for duration seconds
          // Strict: can cut GIF mid-frame; non-strict: same but GIF runs at least once
          uint32_t endTime = (screensaverPaused && !isMixedMode) ? 0 : (millis() + (uint32_t)screensaverDuration * 1000);
          bool loopUntilEnd = true;
          PlayGIF(currentFile, endTime, true, loopUntilEnd);
          if (transportActive) return;
          if (screensaverReloadNeeded) return;
#ifdef WEBRADIO_ENABLED
          if (radioDisplayActive) return;
#endif
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
        // No screensaver file → fallback logo.raw runs in ScreenSaver()
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

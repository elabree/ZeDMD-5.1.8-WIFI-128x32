#ifndef RADIO_H
#define RADIO_H
#if defined(WEBRADIO_ENABLED)

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// I2S-Pins: per Build-Flag überschreibbar
// Defaults = tatsächlich verwendete Pins (beide Webradio-Builds: 9/14/21)
// Nicht 12/13/11 — das sind die SPI-SD-Karten-Pins (SCK/MISO/MOSI)!
#ifndef RADIO_I2S_BCLK
#define RADIO_I2S_BCLK 9
#endif
#ifndef RADIO_I2S_LRC
#define RADIO_I2S_LRC  14
#endif
#ifndef RADIO_I2S_DOUT
#define RADIO_I2S_DOUT 21
#endif

#define MAX_RADIO_PRESETS     20
#define RADIO_DEFAULT_VOLUME  15  // 0–21

struct RadioPreset {
  char name[64];
  char url[256];
  char icon_url[256];
};

extern volatile bool    radioIsPlaying;
extern volatile bool    radioUserActive;
extern volatile bool    radioDisplayActive;
extern uint32_t         radioDisplayUntil;
extern char             radioStationName[64];
extern char             radioTrackTitle[128];
extern SemaphoreHandle_t radioStringMutex;
extern uint8_t       radioVolume;
extern RadioPreset   radioPresets[MAX_RADIO_PRESETS];
extern int           radioPresetCount;
extern volatile int  radioCurrentPreset;
extern int           radioLastPreset;

void radioInit();
void radioPlay(const char* url, int presetIndex = -1);
void radioStop();
void radioSetVolume(uint8_t vol);
void radioSetEq(int8_t bass, int8_t mid, int8_t treble);
void radioSetSwapChannels(bool swap);
void radioLoadPresets();
void radioSavePresets();
void radioRegisterRoutes(AsyncWebServer* server);
// GIF-Begleitung: spielt eine lokale SD-MP3 ab (nur wenn kein Stream aktiv)
void radioPlayLocalFile(const char* sdPath);
void radioStopLocalFile();

#endif // WEBRADIO_ENABLED
#endif // RADIO_H

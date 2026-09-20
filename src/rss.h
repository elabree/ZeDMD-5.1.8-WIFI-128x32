#pragma once
#ifdef ZEDMD_WIFI

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

extern char     rssUrl[256];
extern volatile bool rssFetching;
extern uint32_t lastRssFetch;

void        rssInit();
void        rssTrigger();
const char* rssGetHeadlines();
void        rssRegisterRoutes(AsyncWebServer* server);

#endif // ZEDMD_WIFI

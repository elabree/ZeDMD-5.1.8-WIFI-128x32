# ZeDMD Developer Reference

Entwickler-Dokumentation für das ZeDMD-5.1.8 WiFi 128×32 Projekt.

---

## Hardware

| Komponente | Details |
|---|---|
| SoC | ESP32-S3-N16R8 (16 MB Flash, 8 MB PSRAM) |
| Display | 128×32 HUB75 LED-Matrix (RGB888, 3 Byte/Pixel = 12.288 Byte/Frame) |
| SD-Karte | SPI-Bus: SCK=12, MISO=13, MOSI=11, CS=10 |
| Audio (I2S) | **2× MAX98357A (Stereo)**, Pins: BCK=9, WS=14, DATA=21 — Kanalwahl per SD-Pin-Widerstand gegen 5V: L=100 kΩ, R=390 kΩ (Breakout-Boards haben 1 MΩ onboard SD→Vin) |
| Display-Bus | ESP32-S3 LCD-Peripheral mit DMA (kein Software-Bitbanging) |

### Entwickler-Board v2

Abweichende SD/I2S-Pins — in `platformio_override.ini` als separates Environment `wifi_sd_webradio_devboard_v2` definiert (nicht im Repo).

---

## Flash-Partitionen

```
nvs        data/nvs        0x9000     20 KB   WiFi-Credentials, NVS-Variablen
otadata    data/ota        0xE000      8 KB   OTA-Boot-Slot-Auswahl
app0       app/ota_0      0x10000   3200 KB   Firmware-Slot A
app1       app/ota_1     0x330000   3200 KB   Firmware-Slot B (OTA-Target)
spiffs     data/spiffs   0x650000   9916 KB   LittleFS (Icons, HTML, Settings)
coredump   data/coredump 0xFDF000    128 KB   Panic-Coredump (fest reserviert)
```

---

## Build-System

### Umgebungen

```bash
pio run          # beide Environments bauen (default_envs in platformio_override.ini)
```

Niemals `-e` verwenden — immer beide Environments bauen.

### Build-Workflow (Pflicht-Reihenfolge)

```bash
# 1. Test-Build: prüft Kompilierung
pio run

# 2. Commit (erst nach grünem Build)
git commit -m "..."

# 3. Release-Build: bettet korrekten Git-Hash ein
pio run

# 4. Firmware kopieren
HASH=$(git rev-parse --short HEAD)
BRANCH=$(git rev-parse --abbrev-ref HEAD | sed 's|/|-|g')
cp .pio/build/S3-N16R8_128x32_wifi_sd_webradio/firmware.bin \
   ~/Desktop/Firmwares/ZeDMD_5.1.8-jb_S3-N16R8_128x32_wifi_sd_webradio_${BRANCH}_${HASH}.bin
cp .pio/build/S3-N16R8_128x32_wifi_sd_webradio/firmware.elf \
   ~/Desktop/Firmwares/ZeDMD_5.1.8-jb_S3-N16R8_128x32_wifi_sd_webradio_${BRANCH}_${HASH}.elf
# analog für devboard_v2
ls -t ~/Desktop/Firmwares/*.elf | tail -n +3 | xargs rm -f   # max 2 ELFs behalten
```

> **Warum ELF archivieren?** Coredumps sind nur mit dem ELF der exakt gleichen Firmware analysierbar. Nach jedem Rebuild wird das ELF überschrieben.

### Upload-Regeln

| Was | Wie |
|---|---|
| Firmware (`src/`) | PlatformIO flashen via USB |
| HTML/CSS/JS (`data/`) | `POST /upload_file` im Browser — kein Datenverlust, kein PlatformIO |
| Einzelne LittleFS-Dateien (`.raw`, `.png`, `.html`, …) | **ESP Connect Tool** — verbindet direkt mit LittleFS, lädt einzelne Dateien hoch ohne Datenverlust. Kein `uploadfs` (löscht alle Einstellungen!) |
| Icons (`data/icons/`) | `POST /upload_icon` oder `/upload_icon_small` |
| GIFs/Screensaver | `POST /upload_screensaver` oder direkt auf SD |

---

## Architektur

### Speicher-Strategie

```
Interner Heap (~214 KB dynamisch):
  - HTTP-Server-Puffer, FreeRTOS-Task-Stacks
  - Temporäre Puffer (<10 KB pro Allokation)
  - NIEMALS String += in Schleifen — führt zu Fragmentierung

PSRAM (8 MB, ~7.4 MB frei):
  - renderBuffer[]      3×  12.288 Byte  (~36 KB, Triple-Buffering Pixel-Daten RGB888)
  - screensaverFiles    N×    128 Byte   (~350 KB bei 2800 GIFs, via heap_caps_realloc wachsend)
  - cachedSDFolders     variabel JSON    (wenige KB, SD-Ordnerliste)
  - cachedGifAudioFiles variabel JSON    (bis ~300 KB während Scan, danach realloc auf Exactgröße)
  - logBuffer           80×   120 Byte  (~10 KB Ringpuffer)
  - uncompressBuffer         2.048 Byte  (UDP-Dekompression)
  - gifReadAheadBuf          4.096 Byte  (nur während GIF-Wiedergabe aktiv)
```

**Regel:** Alle Caches und Listen die >10 KB sein können → `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.  
JSON-Aufbau in Schleifen → direkt per `snprintf` in PSRAM-Puffer, nie per Arduino `String +=`.

### Display-Pipeline

```
UDP/TCP-Paket → Dekodierung → renderBuffer[currentRenderBuffer]
                                      ↓
                              FillPanelRaw(buf)
                                      ↓
                          DMA → HUB75 LED-Matrix
```

- `NUM_RENDER_BUFFERS = 3` (Triple-Buffering für ruckelfreie Ausgabe)
- `currentRenderBuffer` / `lastRenderBuffer` — kein Mutex in `loop()` (bewusste Design-Entscheidung für maximale FPS)
- `renderBuffer[]` wird **vor** `LittleFS.begin()` allokiert (PSRAM-Allokation früh, vor Display-Init)

### FreeRTOS

| Task | Core | Prio | Stack | Beschreibung |
|---|---|---|---|---|
| loopTask (Arduino) | Core 1 | 1 | 32 KB | setup() + loop(), HTTP-Responses, Screensaver, Display |
| radioTask | Core 0 | 11 | 16 KB | ESP32-audioI2S Webradio-Streaming (intern) |
| async_tcp | Core 0 | 10 | ~8 KB | AsyncWebServer / AsyncTCP |
| mqttTask | Core 0 | 1 | 8 KB | MQTT-Reconnect + mqttClient.loop() |
| Task_ReadSerial | Core 0/1 | 1 | 4–8 KB | Serielle ZeDMD-Protokoll-Kommunikation |
| Display-Refresh | Core 0 | — | — | DMA-gesteuert, kein FreeRTOS-Task |

**WDT-Budget:** `CONFIG_ESP_TASK_WDT_PANIC=y` — TASK_WDT löst Panic aus.  
In `setup()` nach jeder längeren Operation `esp_task_wdt_reset()` aufrufen.  
LittleFS-Writes können auch ohne Schleife >5 s dauern (Flash-Fragmentierung).

### Screensaver-Cache

```cpp
char (*screensaverFiles)[128]  // PSRAM, via heap_caps_realloc wachsend
uint16_t screensaverCount      // aktuelle Anzahl
```

Wird in `loop()` nach `screensaverReloadNeeded == true` via `LoadScreensaver()` befüllt.  
Kein Mutex für Lese-Zugriff in `loop()` — bewusst (ruckelfreie GIF-Wiedergabe + UDP-Throughput).  
Mutex nur für Web-Handler-Zugriff (`screensaverFilesMutex`).

### GIF-Audio-Cache

```cpp
char* cachedGifAudioFiles  // PSRAM, JSON-String: [{"name":"foo.mp3","size":12345},...]
```

Wird **nicht** in `loop()` befüllt, sondern einmalig beim Start via `TryLoadGifAudioCache()` aus LittleFS (`/gif_audio_cache.bin`) geladen. Nur bei Cache-Miss: vollständiger SD-Scan in `BuildGifAudioCache()` (~130 s bei 1958 Dateien), danach `SaveGifAudioCache()` schreibt kompaktes `name|size\n`-Format zurück nach LittleFS.

**Besonderheiten:**
- `/gif_audio_files`-Endpoint sendet `cachedGifAudioFiles` **direkt** als HTTP-Response — kein Serialisierungsschritt
- Alphabetische Sortierung erfolgt **clientseitig** per `localeCompare` in `gifAudioLoad()` (FAT-Verzeichnisreihenfolge ≠ alpha)
- `f.name()` muss via `strncpy` **vor** `f.close()` kopiert werden — SdFat2 invalidiert den internen Namenspuffer beim Close
- Architektur-Unterschied zu Screensaver: kein struct-Array, kein wahlfreier Index-Zugriff — geplantes Refactoring: P4 #58

**Invalidierung:**
- `InvalidateGifAudioCache()` löscht `/gif_audio_cache.bin` auf LittleFS → nächster `TryLoadGifAudioCache()`-Aufruf schlägt fehl → Neu-Scan
- RAM-Pointer `cachedGifAudioFiles` bleibt bis zum abgeschlossenen Scan mit alten Daten (oder `"[]"`) gefüllt

---

## Boot-Sequenz

```
setup()
  ├─ renderBuffer[] allokieren (PSRAM, vor LittleFS)
  ├─ LittleFS.begin() + lfsTotal/lfsUsed cachen (verhindert TASK_WDT via /fs_info)
  ├─ diagBoot()               — Crash-Log schreiben falls letzter Boot ein Crash war
  ├─ esp_task_wdt_reset()
  ├─ Display-Init + DisplayLogo() + "booting"-Text   — Logo sofort sichtbar (~2s)
  ├─ esp_task_wdt_reset()
  ├─ CleanupTmpFiles()
  ├─ Load*() × 8              — Settings aus LittleFS (kann 10+ s dauern)
  ├─ esp_task_wdt_reset()
  ├─ LoadIcons()              — RGBA-Icons aus LittleFS + radioIconSlugsLoad() (~6s bei 219 Icons)
  ├─ InitSDCard()
  ├─ GetSDFolders()           — SD-Ordner in PSRAM cachen
  └─ screensaverReloadNeeded = true

loop()
  ├─ screensaverReloadNeeded → LoadScreensaver()
  ├─ Screensaver-Tick
  ├─ Radio-Update
  ├─ Wetter-Update
  └─ sdFoldersRefreshNeeded → GetSDFolders()
```

---

## LittleFS-Inhalt (`data/`-Ordner)

Der `data/`-Ordner wird via `pio run -t buildfs` in `littlefs.bin` gepackt und ist Teil der Merged Firmware. Ein frisches Gerät bekommt damit alles auf einmal.

| Pfad | Inhalt | Anzahl |
|---|---|---|
| `data/index.html` | Web-UI Hauptseite | 1 Datei |
| `data/radio.html` | Web-UI Radio-Sektion | 1 Datei |
| `data/icons/` | 20×20 RGBA Emoji-Icons (Lauftext, Wetter) | 38 Dateien |
| `data/icons_small/` | 10×10 RGBA Emoji-Icons (kleine Darstellung) | 38 Dateien |
| `data/icons_radio/` | 32×32 RGBA Sender-Logos | 219 Dateien |

> **Wichtig:** Alle Icons und Logos liegen im Repo unter `data/`. Das Merged-Image enthält alles — kein manueller Upload nach dem Erstflash nötig.

---

## Scripts (`scripts/`-Ordner)

| Script | Sprache | Was es tut |
|---|---|---|
| `merge_firmware.py` | Python | Erstellt All-in-One Flash-Image (Bootloader + Partitionen + Firmware + LittleFS) für Erstflash per USB. Aufruf: `python3 scripts/merge_firmware.py [ENV]` |
| `upload_icons.sh` | Bash | Lädt alle RGBA-Icons aus `data/icons/`, `data/icons_small/` und `data/icons_radio/` per HTTP auf ein laufendes Gerät hoch. Aufruf: `./scripts/upload_icons.sh <IP>` |
| `convert_icons.py` | Python | Konvertiert Emoji-GIFs (aus `~/Downloads/zedmd-emoji-gifs/`) und Wetter-GIFs in RGBA-Binärdateien für `data/icons/` und `data/icons_small/`. Benötigt Pillow. |
| `make_radio_logos.py` | Python | Konvertiert DAB+-Logos (ZIPs, radioart-Ordner, manuelle PNGs) in 32×32 RGBA-Dateien für `data/icons_radio/`. Mapping via `radioart_map.csv`. |
| `extract_gif_audio.sh` | Bash | Läuft direkt auf Batocera per SSH. Extrahiert Audio-Clips aus gescrapten Spielvideos als MP3 für den `/GifAudio/`-Ordner auf der SD-Karte. |
| `batocera_game_start.sh` | Bash | Ablage auf Batocera als `/userdata/system/scripts/gameStart.sh`. Triggert beim Spielstart den `/gif_audio_play`-Endpoint des ZeDMD. IP im Script anpassen. |
| `batocera_game_stop.sh` | Bash | Ablage auf Batocera als `/userdata/system/scripts/gameStop.sh`. Stoppt GIF-Audio beim Spielende. |
| `copy_firmware.py` | Python | PlatformIO `extra_scripts`-Hook: bettet Git-Hash und Branch als C-Defines in den Build ein. Wird automatisch von PlatformIO aufgerufen, nie manuell. |
| `patch_audio_lib.py` | Python | PlatformIO `extra_scripts`-Hook: patcht `ESP32-audioI2S` bei GCC14 (min()-Typkonflikt). Wird automatisch aufgerufen. |
| `adjust_raw_img.py` | Python | Hilfstool: lädt eine 256×64 RGB-RAW-Datei und erlaubt Bildanpassungen. Für Entwicklung/Debug. |
| `display_raw_img.py` | Python | Hilfstool: zeigt eine 128×32 RGB-RAW-Datei an (ppuc.raw). Für Entwicklung/Debug. |
| `radioart_map.csv` | CSV | Mapping-Tabelle DAB+-Service-ID (hex) → Slug für `make_radio_logos.py`. |

---

## Icons

### Große Icons (20×20 RGBA)

- Pfad auf LittleFS: `/icons/<name>.rgba`
- Geladen beim Boot via `LoadIcons()` in `iconTable[]` (PSRAM)
- Genutzt für: Emoji-Lauftext, Wetter-Icons

### Kleine Icons (10×10 RGBA)

- Pfad auf LittleFS: `/icons_small/<name>.rgba`
- Werden **nicht** beim Boot geladen (zu viele, werden nicht genutzt)
- Dateien bleiben auf LittleFS, können on-demand gelesen werden

### Wetter-Icons — WMO-Code-Mapping

OpenMeteo liefert einen `weather_code` (WMO-Standard, Teilmenge). Das Display wählt daraus ein Icon:

| WMO-Code(s) | Beschreibung | Icon-Nr | Icon-Datei |
|---|---|---|---|
| 0, 1 (Tag) | Sonnig / Heiter | 0 | *(gezeichnet)* |
| 0, 1 (Nacht) | Klar | 6 | *(Mond, gezeichnet)* |
| 2 (Tag) | Teils bewölkt | 1 | *(gezeichnet)* |
| 2 (Nacht) | Teils bew. (Nacht) | 7 | *(Mond+Wolke, gezeichnet)* |
| 3 | Bedeckt | 2 | *(gezeichnet)* |
| 45, 48 | Nebel | 10 | *(gezeichnet)* |
| 51–57 | Nieselregen | 3 | *(gezeichnet)* |
| 61–67 | Regen | 8 | *(gezeichnet)* |
| 71–77 | Schnee | 4 | *(gezeichnet)* |
| 80–82 | Schauer | 9 | *(gezeichnet)* |
| 85–86 | Schneeschauer | 4 | *(gezeichnet)* |
| 95+ | Gewitter | 5 | *(gezeichnet)* |
| alle anderen | Unbekannt | 255 | `icons/question.rgba` |

Der aktuelle `weather_code` erscheint im Log als `Wetter: Code=<n> isDay=<0/1>` bei jedem OpenMeteo-Fetch (~alle 15 Min.).

**Priorität:** MQTT liefert Temperatur, Luftfeuchtigkeit, Wind, Druck mit höchster Priorität. OpenMeteo liefert immer: `weather_code` (Icon) + Forecast. OpenMeteo liefert Messwerte nur wenn MQTT >10 Min. still war (grau auf Display) oder nicht konfiguriert ist.

### Radio-Icons (on-demand)

- Pfad auf **LittleFS**: `/icons_radio/<slug>.rgba`
- Slug-Liste wird beim Boot via `radioIconSlugsLoad()` in PSRAM gecacht (256 × 32 Byte)
- Beim Senderwechsel: Fuzzy-Match gegen PSRAM-Cache, dann einzelnes RGBA-File-Read aus LittleFS
- Slug aus Sendername: Kleinbuchstaben, Sonderzeichen → `_`

---

## Crash-Analyse

### Diagnose-Endpoints

```bash
curl http://<IP>/diag                  # Boot-Statistik + verfügbare Crash-Slots
curl http://<IP>/crashlog?n=0          # Crash-Log Slot 0 (letzter Serial-Log des Absturz-Boots)
curl -o coredump.bin http://<IP>/coredump   # Roher ELF-Coredump
```

### Coredump analysieren

```bash
~/.platformio/penv/bin/python -m esp_coredump info_corefile \
  --core coredump.bin --core-format raw \
  --gdb ~/.platformio/tools/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gdb \
  ~/Desktop/Firmwares/ZeDMD_5.1.8-jb_S3-N16R8_128x32_wifi_sd_webradio_main_<HASH>.elf
```

> Coredump und ELF **müssen von derselben Firmware-Version** stammen — SHA256 muss übereinstimmen.

### addr2line für Backtrace-Adressen

```bash
~/.platformio/tools/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line \
  -e ~/Desktop/Firmwares/<name>.elf -f 0x<ADRESSE>
```

### Diagnose-Reset

```bash
curl -X POST http://<IP>/reset_diag       # Boot-Zähler zurücksetzen
curl -X POST http://<IP>/delete_crashlogs # Crash-Logs löschen
```

> **Coredump-Partition:** Immer 128 KB reserviert. Löschen gibt keinen Flash-Platz frei — nächster Crash überschreibt automatisch.

---

## Bekannte Constraints & Design-Entscheidungen

| Thema | Entscheidung | Grund |
|---|---|---|
| Kein Mutex in `loop()` für screensaverFiles | Bewusst | Ruckelfreie GIF-Wiedergabe + UDP-Throughput |
| `icons_small` nicht beim Boot laden | Bewusst | 38 Dateien × ~200 ms = 8 s Boot-Overhead, aktuell nicht genutzt |
| `delay()` in SD-Scan vermeiden | Pflicht | SPI-Kontextverlust bei `vTaskDelay()` → Scan-Hänger |
| Arduino `String +=` in Schleifen | Verboten | Heap-Fragmentierung → OOM-Crashes bei 1000+ Dateien |
| `#define` in `.cpp` für Bibliotheks-Konfiguration | Funktioniert nicht | Immer `build_flags` in `platformio.ini` nutzen |
| SD-Scan alle 50 Dateien `esp_task_wdt_reset()` | Pflicht | WDT-Budget bei großen Sammlungen |
| `f.name()` vor `f.close()` via `strncpy` kopieren | Pflicht | SdFat2-internen Puffer invalidiert `f.close()` — `const char*` danach ungültig (Bug: 1958 Dateien → nur 805 gespeichert) |

---

## API-Referenz (Web-Endpoints)

### Diagnose

| Endpoint | Methode | Beschreibung |
|---|---|---|
| `/diag` | GET | Boot-Zähler, Reset-Grund, Crash-Slots (JSON) |
| `/crashlog?n=0` | GET | Crash-Log Slot 0–9 |
| `/coredump` | GET | Roher ELF-Coredump (128 KB) |
| `/debug_info` | GET | IP, SSID, Heap, PSRAM, Uptime |
| `/log` | GET | Letzter In-Memory-Ringpuffer |
| `/fs_info` | GET | LittleFS-Speicherinfo (JSON) |
| `/sd_info` | GET | SD-Karten-Info (JSON) |

### Wartung

| Endpoint | Methode | Beschreibung |
|---|---|---|
| `/reset_diag` | POST | Diagnose-Zähler zurücksetzen |
| `/delete_crashlogs` | POST | Alle Crash-Logs löschen |
| `/restart` | POST | Gerät neu starten |
| `/reset_wifi` | POST | WLAN löschen → Access Point |
| `/eject_sd` | POST | SD sicher auswerfen |
| `/mount_sd` | POST | SD neu einbinden |

### Konfiguration

| Endpoint | Methode | Beschreibung |
|---|---|---|
| `/export_config` | GET | Alle Einstellungen als JSON |
| `/import_config` | POST | Einstellungen aus JSON wiederherstellen |
| `/get_version` | GET | Firmware-Version + Git-Hash |
| `/ota` | POST | OTA-Firmware-Update (Binary als Body) |

### Dateiverwaltung

| Endpoint | Methode | Beschreibung |
|---|---|---|
| `/upload_file` | POST | Datei in LittleFS (HTML, Icons, Logo) |
| `/upload_screensaver` | POST | GIF auf SD |
| `/upload_sd` | POST | Beliebige Datei auf SD |
| `/upload_icon` | POST | 20×20 RGBA Icon in LittleFS |
| `/upload_icon_small` | POST | 10×10 RGBA Icon in LittleFS |
| `/upload_icon_radio` | POST | Sender-Icon auf SD |
| `/delete_screensaver?file=` | GET | GIF von SD löschen |
| `/delete_sd_file?file=` | GET | SD-Datei löschen |
| `/sd_files?folder=` | GET | Dateiliste eines SD-Ordners |
| `/sd_folders` | GET | Alle SD-Ordner (aus PSRAM-Cache) |

### Display & Screensaver

| Endpoint | Methode | Beschreibung |
|---|---|---|
| `/screensaver_status` | GET | Aktueller Pfad, Index, Gesamtanzahl |
| `/screensaver_files?offset=0` | GET | Dateiliste paginiert (20/Seite) |
| `/screensaver_folder_count` | GET | Anzahl geladener Dateien |
| `/screensaver_pause` | POST | Anhalten / Fortsetzen |
| `/screensaver_reshuffle` | POST | Zufallsreihenfolge neu mischen |
| `/screensaver_rescan` | POST | Alle Ordner-Caches invalidieren + Neu-Scan erzwingen |
| `/set_screensaver_paths` | POST | Aktive Quellen konfigurieren |
| `/display_text` | POST | Text anzeigen (`text=`, `r=`, `g=`, `b=`, `scroll=`, `duration=`) |
| `/display_text_stop` | POST | Text-Anzeige beenden |
| `/display_timer` | GET | Timer-Status lesen (`enabled`, `from`, `until`, `blank`) |
| `/display_timer` | POST | Timer konfigurieren (`enabled=1`, `from=23:00`, `until=07:00`) |
| `/display_blank` | POST | Display sofort aus/ein (`blank=1` / `blank=0`; ohne Parameter: Toggle) |
| `/play_file` | POST | GIF/RAW direkt abspielen |
| `/gif_preview?file=` | GET | Erstes Frame als PNG |

### GIF-Audio

| Endpoint | Methode | Beschreibung |
|---|---|---|
| `/gif_audio_files` | GET | Dateiliste (JSON aus PSRAM) |
| `/gif_audio_upload` | POST | MP3 in `/GifAudio/` auf SD |
| `/gif_audio_delete` | POST | MP3 löschen |
| `/gif_audio_rescan` | POST | Cache invalidieren + Neu-Scan erzwingen |
| `/cancel_gif_audio_scan` | POST | Laufenden Scan abbrechen |

---

*Letzte Aktualisierung: 2026-07-17*

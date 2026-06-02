# ZeDMD 5.1.8 — WiFi Fork (128×32, ESP32-S3-N16R8)

---

> 🇬🇧 **English documentation:** [README.md](README.md)

---

> 📝 *Dieses Dokument ist eher ein persönliches Projekttagebuch als eine vollständige Anleitung. Es dokumentiert was bei mir funktioniert hat — dein Setup kann abweichen. Kein Anspruch auf Vollständigkeit; Fehler und Auslassungen sind möglich.*

---

> **Dies ist ein persönlicher Hobby-Fork von [PPUC/ZeDMD](https://github.com/PPUC/ZeDMD) v5.1.8.**
> Er wird mit der Community geteilt in der Hoffnung, dass er nützlich sein könnte — aber er kommt **ohne jeglichen Support, ohne Garantie und ohne Gewährleistung**.
> Issues und Pull Requests werden möglicherweise nicht beantwortet — nicht aus Unhöflichkeit, sondern weil dies ein Freizeitprojekt ist, das von einer einzelnen Person betreut wird.
> **Nutzung vollständig auf eigene Gefahr.**

---

https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/raw/main/docs/images/ZeDMD_WiFi_128x32_demo.mp4

---

### Screenshots / Fotos

| Uhr + Wetter | Wettervorhersage | GIF Screensaver |
|:-:|:-:|:-:|
| ![Clock](docs/images/IMG_5032.jpeg) | ![Forecast](docs/images/IMG_5033.jpeg) | ![GIF](docs/images/IMG_5034.jpeg) |

**Innenleben:**

![Internals](docs/images/IMG_5038.jpeg)

---

## 🆕 Neu in dieser Version

### Webradio
Internetradio direkt über ZeDMD streamen — über einen kleinen integrierten Lautsprecher.
Erfordert ein **MAX98357A I2S-Verstärkermodul** — Verkabelung siehe unten.

- Preset-Verwaltung über den Browser unter `/radio.html`
- Lautstärkeregler direkt auf der Hauptseite
- Sendername und Titelinfo scrollen auf der LED-Matrix
- Presets überleben Firmware-Updates (gespeichert in LittleFS)
- Stabiler Senderwechsel — kein Audio-Aussetzer beim Umschalten mehr

### SDMMC-Board-Unterstützung
Unterstützung für Boards mit **onboard SD-Karte via SDMMC-Interface** (1-Bit-Modus) hinzugefügt — kein externes SPI-Modul nötig. Siehe Pin-Tabelle für die erforderlichen HUB75-Kabeländerungen.

### Stabilität & Bugfixes
Diese Version enthält eine umfassende Überarbeitung der Speicherverwaltung und Task-Sicherheit:

- Race Condition beim Senderwechsel behoben — Audio springt nicht mehr auf den vorherigen Sender zurück
- Wetterdaten (MQTT + HTTP) jetzt sicher zwischen beiden CPU-Kernen synchronisiert
- SD-Karten-Verzeichnislisting aus den Netzwerk-Callbacks herausgelöst — kein Audio-Stottern mehr beim Webzugriff
- Wetter-HTTP-Antwort wird im PSRAM gepuffert — kein großer SRAM-Spike beim Abruf mehr
- Alle Datei-Uploads jetzt atomar (`.tmp` + Umbenennen) — abgebrochene Uploads hinterlassen keine korrupten Dateien
- `screensaverFiles`-Array durch Mutex gegen gleichzeitigen Webzugriff gesichert

---

## 🔜 Geplante Features

### GIF-Audio *(in Vorbereitung)*
Beim Abspielen eines animierten GIF-Screensavers passend dazu eine MP3-Datei von der SD-Karte abspielen.
Passende Audiodateien einfach in `/GifAudio/` auf der SD ablegen — die Upload-UI ist auf der Hauptseite bereits vorhanden.
Das Feature ist **firmware-seitig vorbereitet**, aber noch nicht vollständig implementiert und getestet.

---

## Was ist anders als beim Original-ZeDMD?

Dieser Fork ist **nur WiFi** und zielt auf den **ESP32-S3-N16R8** mit einer **128×32 LED-Matrix** ab.

### Alle hinzugefügten Features

- **WiFi OTA Firmware-Update** — neue Firmware direkt über den Browser flashen (`/admin.html`)
- **Screensaver** — GIF/RAW-Diashow mit Uhrzeit- und Wetteranzeige (Open-Meteo)
- **Screensaver-Verwaltung** — Favoriten, Ignore-Liste, Alphabetisch/Zufällig, Strict Timer, Pause/Weiter
- **Verbessertes Webinterface** — Dateiverwaltung, Favoriten-/Ignore-Buttons pro Datei
- **Admin-Seite** — WiFi, Display, Transport, MQTT, Wetter-Einstellungen
- **Webradio** — Internetradio via I2S-Verstärker (MAX98357A), Preset-Verwaltung, Lautstärke
- **Konfig Export/Import** — vollständiges Konfigurations-Backup und -Restore über den Browser (`/config_transfer.html`)

---

## Hardware — ESP32-S3-N16R8 Hinweis

### ⚠️ Kein 5V am VIN-Pin (IN-OUT Lötbrücke)

Einige ESP32-S3-N16R8 Boards liefern ab Werk keine 5V am VIN-Pin.
Falls das ZeDMD startet, aber die LED-Matrix dunkel bleibt oder seltsam reagiert, die **IN-OUT Lötbrücke** neben den 5V/GND-Pins prüfen.

**Fix:** Die IN-OUT-Brücke mit einem kleinen Lötzinn-Punkt schließen, um 5V von USB auf den VIN-Pin durchzuschleifen.

![IN-OUT Brücke](docs/images/IMG_4405.jpg)
![IN-OUT Nahaufnahme](docs/images/image.png)

> ⚠️ Hardware-Modifikationen auf eigene Gefahr!

Weitere bekannte Hardware-Probleme und allgemeine ZeDMD-Dokumentation: **[Original ZeDMD README](https://github.com/PPUC/ZeDMD#readme)**.

---

## SD-Karte

Die Screensaver GIF/RAW-Dateien können auf einer microSD-Karte gespeichert werden, die per SPI angeschlossen wird.

**Getestetes Modul:**
[Micro SD Card Module SPI (Amazon)](https://www.amazon.de/dp/B0D8Q8N7NQ)

![SD Card Module](docs/images/sd_module.jpg)

**Verkabelung ESP32-S3-N16R8:**

| SD-Modul | ESP32-S3 Pin |
|----------|-------------|
| VCC      | **3,3V** oder **5V** ¹ |
| GND      | GND         |
| MISO     | GPIO 13     |
| MOSI     | GPIO 11     |
| SCK      | GPIO 12     |
| CS       | GPIO 10     |

> ¹ Die meisten SPI SD-Module akzeptieren sowohl 3,3V als auch 5V an VCC (onboard Spannungsregler vorhanden).
> Im Zweifelsfall **3,3V** verwenden — die GPIO-Pins des ESP32-S3 sind **nicht** 5V-tolerant.

**Format:** FAT32, Dateien in Unterordnern (z.B. `/MyGIFs/`). GIF- und RAW-Dateien werden unterstützt.

---

## Webinterface

Zugriff über `http://<IP>/` (Hauptseite) und `http://<IP>/admin.html` (Admin-Seite).

### Admin-Seite — Einstellungen

| Bereich | Beschreibung |
|---------|--------------|
| **Firmware Update (OTA)** | Neue Firmware über den Browser flashen — kein USB nötig |
| **WiFi** | SSID, Passwort, Port |
| **Display** | RGB-Reihenfolge, Skalierungsmodus, Helligkeit |
| **Transport** | USB / WiFi UDP / TCP / SPI, UDP-Delay, USB-Paketgröße |
| **Panel** | Clock-Phase, I2S-Speed, Latch-Blanking, Refresh-Rate, Treiber |
| **MQTT** | Server-IP und Port für Wetterintegration |
| **Wetter (Open-Meteo)** | Breitengrad/Längengrad für lokale Wetteranzeige |
| **Web-Dateien aktualisieren** | index.html / admin.html hochladen ohne vollständigen Filesystem-Flash |
| **Konfig Export/Import** | Vollständiges Backup/Restore über `/config_transfer.html` |
| **Information** | Firmware-Version mit Build-Datum, Debug-Info |

---

## Pin-Belegung ESP32-S3-N16R8

### HUB75 LED-Matrix

| Signal | GPIO | Hinweis |
|--------|------|---------|
| R1 | 4 | |
| G1 | 5 | |
| B1 | 6 | |
| R2 | 7 | |
| G2 | 15 | |
| B2 | 16 | |
| A | 18 | |
| B | 8 | |
| C | 3 | |
| D | 42 | |
| E | 1 | nur bei 1/32 scan (64×64) |
| OE | 2 | |
| LAT | 40 | `wifi_sd_webradio` |
| LAT | **46** | `wifi_sdmmc_webradio` — Kabel umklemmen! |
| CLK | 41 | `wifi_sd_webradio` |
| CLK | **17** | `wifi_sdmmc_webradio` — Kabel umklemmen! |

> HUB75 LAT/CLK müssen **nur beim SDMMC-Board** umgeklemmt werden, da GPIO 40/41 dort intern für die SD-Karte belegt sind.

### SD-Karte

| Signal | GPIO | Build |
|--------|------|-------|
| CS   | 10 | `wifi_sd_webradio` (SPI) |
| MOSI | 11 | `wifi_sd_webradio` (SPI) |
| SCK  | 12 | `wifi_sd_webradio` (SPI) |
| MISO | 13 | `wifi_sd_webradio` (SPI) |
| DATA | 40 | `wifi_sdmmc_webradio` (SDMMC onboard) |
| CLK  | 39 | `wifi_sdmmc_webradio` (SDMMC onboard) |
| CMD  | 38 | `wifi_sdmmc_webradio` (SDMMC onboard) |

### Buttons / Tasten

| Funktion | GPIO |
|----------|------|
| UP       | 0  |
| DOWN     | 45 |
| FORWARD  | 48 |
| BACKWARD | 47 |

### Webradio (optional)

Erfordert ein **MAX98357A I2S-Verstärkermodul** und einen kleinen Lautsprecher.

**Empfohlen:**
- Verstärker: MAX98357A Breakout-Modul (z.B. Adafruit #3006 oder handelsübliche Klone)
- Lautsprecher: **8 Ω / 3 W** — z.B. **Visaton FSR 7** (77 mm, ausgezeichneter Klang für die Größe)

### MAX98357A Verkabelung — gleich für beide Builds

| MAX98357A Pin | ESP32-S3 | Hinweis |
|---------------|----------|---------|
| VIN           | **5V**       | 5V gibt mehr Headroom; 3,3V funktioniert, aber geringere Lautstärke |
| GND           | GND          | |
| BCLK          | **GPIO 9**   | I2S Bit Clock |
| LRC (WSEL)    | **GPIO 14**  | I2S Word Select (L/R) |
| DIN           | **GPIO 21**  | I2S Data |
| GAIN          | **GND**      | GND = 15 dB (max); offen = 12 dB; 3,3V = 9 dB |
| SD (Shutdown) | 3,3V oder offen | Offen = an; GND = stumm |

> Class-D Mono-Verstärker. Lautsprecher an OUT+/OUT− anschließen — **nicht** an GND (Differenzausgang/BTL).

### Build / Environment Übersicht

| Setup | Environment | Zusätzliche Hardware |
|-------|-------------|----------------------|
| Bestehendes Board + SPI SD-Modul | `S3-N16R8_128x32_wifi_sd_webradio` | MAX98357A anschließen |
| SDMMC-Board (onboard SD) | `S3-N16R8_128x32_wifi_sdmmc_webradio` | MAX98357A + HUB75 LAT/CLK umklemmen |

### Webradio — Bedienung

Nach dem Flashen erscheinen die Radio-Bedienelemente direkt auf der Hauptseite `http://<IP>/`:

| Bedienelement | Beschreibung |
|---------------|--------------|
| **▶ / ■** | Letzten Preset abspielen / Stopp |
| **Sender-Buttons** | Gespeicherten Preset direkt starten |
| **Lautstärke** | Schieberegler auf der Hauptseite |
| **Display-Toggle** | Radioinfo auf der LED-Matrix ein-/ausblenden |

Vollständige Preset-Verwaltung unter **`http://<IP>/radio.html`**.
Presets werden in LittleFS gespeichert und überleben Firmware-Updates.
Konfiguration zwischen Boards übertragen: `http://<IP>/config_transfer.html`

---

## Wetter (Open-Meteo)

Aktuelles Wetter und 3-Tages-Vorhersage von [Open-Meteo](https://open-meteo.com/) — kostenlos, kein API-Key nötig.
Koordinaten in `admin.html` unter **Wetter (Open-Meteo)** eintragen.

### ⚠️ Warum HTTP statt HTTPS?

Der Audio-Codec (MP3/AAC) belegt ~50 KB internen SRAM zur Laufzeit. Ein TLS-Handshake würde weitere ~30–40 KB zusammenhängenden internen SRAM benötigen — beim Webradio-Build ein garantierter Out-of-Memory-Absturz. Da Open-Meteo ausschließlich öffentliche Wetterdaten ohne Auth-Token liefert, ist unverschlüsseltes HTTP hier vollkommen unbedenklich.

> `http://api.open-meteo.com/v1/forecast` — von Open-Meteo explizit für Embedded/IoT-Geräte unterstützt.

---

## Installation

1. **Erstmaliges Flashen** (USB, einmalig): PlatformIO → Upload (`S3-N16R8_128x32_wifi_sd_webradio`)
2. **Zukünftige Firmware-Updates**: Browser → `http://<IP>/admin.html` → „Firmware Update (OTA)"
3. **Webinterface aktualisieren**: Browser → `http://<IP>/admin.html` → „Web-Dateien aktualisieren"

---

## Danksagung

- **[Markus Kalkbrenner / PPUC](https://github.com/PPUC/ZeDMD)** — original ZeDMD project
- **Niels (My Son)** — coding assistance & inspiration & moral support
- **[Claude Sonnet](https://anthropic.com)** — coding assistance

---

## Kommerzielle Nutzung

Dieses Projekt steht unter der GPL v2 — kommerzielle Nutzung ist unter diesen Bedingungen erlaubt.
Wenn du diesen Fork für ein Hardware-Produkt verwendest — ob kommerziell oder einfach eine selbst gemachte Platine auf die du stolz bist — würde ich mich über **2 Exemplare** als Dankeschön freuen: eines für mich, eines für meinen Sohn Niels. Keine rechtliche Pflicht, nur eine freundliche Bitte von einem Hobbybastler. 😊

---

## Lizenz

Identisch mit dem Original-Projekt — siehe [LICENSE](LICENSE).

# The Arcade — powered by ZeDMD (WiFi Fork, 128×32, ESP32-S3-N16R8)

> 🇬🇧 **English documentation:** [README.md](README.md)

**The Arcade** ist eine ESP32-S3-Firmware für eine 128×32 HUB75-LED-Matrix. Sie verwandelt ein Pinball-DMD-Panel in ein eigenständiges Smart-Display mit:

- **Uhr + Wetter** — Live-Daten von der eigenen Wetterstation per MQTT (WeeWx), mit Open-Meteo als automatischem Fallback; konfigurierbare Feldnamen, visueller Fallback-Indikator
- **GIF-Screensaver** — animierte GIFs von der SD-Karte, alphabetisch oder zufällig, optional mit synchronem MP3-Audio pro GIF
- **Webradio** — Internetradio per I2S-Verstärker (MAX98357A); Senderlogos, Titelinfo, Equalizer, L/R-Tausch
- **Web-UI** — vollständige Konfiguration und Steuerung über jeden Browser im lokalen Netzwerk; keine App, kein Account, keine Cloud

---

> 📝 *Dieses Dokument ist eher ein persönliches Projekttagebuch als eine vollständige Anleitung. Es dokumentiert was bei mir funktioniert hat — dein Setup kann abweichen. Kein Anspruch auf Vollständigkeit; Fehler und Auslassungen sind möglich.*

---

> **Das hier begann als persönlicher Hobby-Fork von [PPUC/ZeDMD](https://github.com/PPUC/ZeDMD) v5.1.8**
> und ist seitdem weit darüber hinausgewachsen — Uhr, Wetter und Webradio sind eigene
> Erweiterungen, kein Teil des ursprünglichen ZeDMD. Was tatsächlich vom Original stammt, ist
> das DMD-Protokoll und der HUB75-Display-Treiber — genau diese Basis ist der Grund, warum das
> Projekt weiterhin unter derselben GPLv2-or-later-Lizenz wie ZeDMD steht und auch auf GitHub
> bewusst als Fork davon gelistet bleibt, statt als eigenständiges Repo.
> Er wird mit der Community geteilt in der Hoffnung, dass er nützlich sein könnte — aber er kommt **ohne jeglichen Support, ohne Garantie und ohne Gewährleistung**.
> Issues und Pull Requests werden möglicherweise nicht beantwortet — nicht aus Unhöflichkeit, sondern weil dies ein Freizeitprojekt ist, das von einer einzelnen Person betreut wird.
> **Nutzung vollständig auf eigene Gefahr.**

---

> ⚠️ **Panelgröße: bisher nur mit 128×32 getestet (2× gechainte 64×32-Panels).** Die Codebasis
> hat bereits Build-Umgebungen für größere Panels (`256x64` / `S3-N16R8_256x64`, über
> `ZEDMD_HD`), und das DMD-Kern-Rendering skaliert automatisch über `TOTAL_WIDTH`/
> `TOTAL_HEIGHT` — das wurde aber **noch nicht gebaut oder getestet**. Wetter-Overlay und
> GIF-Screensaver haben hardcodierte Pixel-Positionen für 128×32, die vorher angepasst werden
> müssten.
>
> Falls du ein größeres Panel ausprobierst: der Stromversorgung deutlich mehr Aufmerksamkeit
> schenken als bei 128×32. Ein einzelnes 64×32-HUB75-Panel kann bei Vollweiß bis zu ~25W
> ziehen (5V/5A); bei 256×64 vervierfacht sich die Pixelzahl gegenüber dem hier
> standardmäßigen 128×32-Aufbau ungefähr, und der Strombedarf steigt entsprechend mit.
> Unterdimensionierte Netzteile bei großen HUB75-Ketten verursachen Spannungsabfall,
> Geisterbilder/Dimm-Artefakte und im schlimmsten Fall Schäden an Steckverbindern oder
> Verkabelung — mehrere Einspeisepunkte entlang der Kette einplanen, nicht nur eine
> Einspeisung an einer Ecke.

---

https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/raw/main/docs/images/ZeDMD_WiFi_128x32_demo.mp4

---

### Screenshots / Fotos

| Uhr + Wetter | Wettervorhersage | GIF Screensaver |
|:-:|:-:|:-:|
| ![Uhr](docs/images/IMG_5506.jpeg) | ![Vorhersage](docs/images/IMG_5482.jpeg) | ![GIF](docs/images/IMG_5505.jpeg) |

**Webradio — Stationsname und Logo auf dem Display:**

![Webradio](docs/images/IMG_5504.jpeg)

**Innenleben / Rückseite:**

| Innenleben | Rückseite |
|:-:|:-:|
| ![Innenleben](docs/images/Innenleben.jpeg) | ![Rückseite](docs/images/Rückseite.jpeg) |

---

## 🆕 Neu in dieser Version

### v1.5.3 *(diese Version)*

#### Konfigurierbare MQTT-Feldnamen
Die JSON-Feldnamen, mit denen Wetterdaten vom MQTT-Broker gelesen werden, sind jetzt im Admin-Panel konfigurierbar — kein Neuflashen nötig. Das ist wichtig, wenn die eigene Wetterstation (z. B. WeeWx) andere Feldnamen verwendet. Standardwerte entsprechen dem WeeWx-LOOP-Standardformat: `outTemp_C`, `outHumidity`, `windSpeed_kph`, `barometer_mbar`. Die Einstellung wird in LittleFS gespeichert.

#### Visueller Fallback-Indikator für Open-Meteo-Daten
Wenn MQTT länger als 10 Minuten keine Daten liefert, wechselt die Wetteranzeige auf Open-Meteo als Fallback. Alle vier Wetterwerte (Temperatur, Luftfeuchtigkeit, Wind, Luftdruck) werden in dieser Zeit in **Grau** statt Weiß angezeigt — ein sofort sichtbares Signal, dass die Daten nicht von der eigenen Wetterstation kommen. Nach 30 Minuten ohne MQTT erscheint wie bisher der orange Text **„MQTT fehlt"**.

#### Fragezeichen-Icon für unbekannte Wettercodes
Liefert Open-Meteo einen WMO-Wettercode ohne passendes Icon, zeigt das Display jetzt das **Fragezeichen-Icon** (`question.rgba` aus dem Icon-Set) statt des bisherigen, irreführenden Wolken-Icons. Im Normalbetrieb sollte das nie auftreten — alle von Open-Meteo verwendeten Codes sind abgedeckt.

---

### v1.5.2

#### Tabs im Webinterface
Die Hauptseite ist jetzt in drei Tabs unterteilt — **Screensaver**, **Display** und **Radio** — was die Navigation deutlich übersichtlicher macht, besonders auf dem Smartphone. SD-Karten-Info und der Admin-Button bleiben immer sichtbar über den Tabs.

#### Display-Timer *(zeitgesteuertes Ausschalten)*
Tägliche Ein-/Ausschaltzeiten für das LED-Display lassen sich direkt im **Display**-Tab konfigurieren — z.B. ab 23:00 Uhr aus, ab 07:00 Uhr wieder an. Zusätzlich gibt es einen **„Display off / Display on"**-Button zum sofortigen manuellen Aus- und Einschalten ohne die Timer-Einstellung zu ändern.

#### Stereo / Mono umschalten
Im **Radio**-Tab gibt es jetzt separate **Stereo**- und **Mono**-Buttons.

#### Sender-Logos auf dem LED-Display
Beim Senderwechsel wird das passende Sender-Logo (wenn hochgeladen) zusammen mit Sendername und Titelinfo auf dem LED-Display angezeigt.

#### Boot-Text verbessert
Status-Meldungen beim Start ("booting", "loading...", "SD OK", "LittleFS OK") werden jetzt unten links angezeigt und überlagern das Boot-Logo nicht mehr.

#### Rescan-Buttons
Nach einem Upload — oder jederzeit manuell — erscheint ein **Rescan**-Button in der Screensaver-Dateiliste und der GIF-Audio-Dateiliste. Startet einen sofortigen Scan der SD-Karte und des LittleFS ohne Neustart.

#### SD-Karte — bis zu 40 MHz SPI-Geschwindigkeit
Auf einer eigenen Leiterplatte (keine Dupont-Kabel) wird die SD-Karte jetzt mit 40 MHz initialisiert, mit automatischer Fallback-Kette (40 → 25 → 20 → 8 MHz). Vorher maximal 8 MHz. Spürbar schnellere GIF- und Audio-Scans.

#### Equalizer
Im **Radio**-Tab gibt es jetzt einen Drei-Band-**Equalizer (Bass / Mitten / Höhen)**. Die Einstellungen werden in LittleFS gespeichert und bleiben nach Neustart und Senderwechsel erhalten.

#### Wetter-Icons — weitere Verbesserungen
Die 8×8-Vorhersage-Icons (3-Tage-Ansicht) wurden komplett neu gezeichnet. Drei neue Icons hinzugekommen: Nieselregen, Schauer und Schnee haben jetzt eigene Symbole — vorher teilten sie sich ein generisches Regen-Icon. Insgesamt 11 verschiedene Wetter-Icons.

#### L/R Kanal-Tausch
Wenn linker und rechter Lautsprecher vertauscht verdrahtet sind, tauscht ein **L/R Swap**-Button im **Radio**-Tab die Audiokanäle per Software — kein Umverdrahten nötig. Einstellung bleibt nach Neustart erhalten.

#### Eigenes Branding
Boot-Screen und Web-UI-Header zeigen jetzt das **The Arcade**-Logo statt des generischen ZeDMD-Logos. Der PPUC-Splashscreen wurde entfernt.

---

### v1.5.1

#### Wetter-Icons — native 16×16 Pixel-Art
Alle 8 Wetter-Icons wurden als native 16×16 Pixel-Art neu gezeichnet — schärfer und detaillierter als die bisherigen hochskalierten 8×8-Bitmaps. Jedes Icon hat einen eigenen handgezeichneten Farbgradienten: Sonne mit Strahlen, Mondsichel, Wolke, Regentropfen, Schnee, Blitz und Mischbewölkt-Varianten. Die kleineren 8×8-Icons in der 3-Tages-Vorhersage wurden in v1.5.2 weiter verbessert.

#### SD-Karte — robustere Initialisierung
Die SD-Karte wird jetzt mit schrittweise langsameren SPI-Geschwindigkeiten erneut versucht (8 → 4 → 2 MHz, bis zu 6 Versuche) bevor aufgegeben wird. Bisher führte ein einzelner fehlgeschlagener Mount-Versuch dazu, dass die SD-Karte für die gesamte Session als nicht verfügbar galt. Behebt sporadische Boot-Fehler bei schlecht kontaktierten oder langsam mountenden SD-Karten.

#### Bugfixes
- Emoji-Text: Cursor-Position 0 wurde in JavaScript als falsy behandelt — der Cursor bleibt jetzt korrekt erhalten wenn ein Emoji ganz am Anfang des Eingabefeldes eingefügt wird
- Icon-Upload: Abgebrochene Uploads hinterlassen keine verwaisten `.tmp`-Dateien mehr auf LittleFS
- Icon-Ordner-Scan: Directory-Handle-Leak bei Nicht-Ordner-Einträgen behoben

---

### v1.5 und älter

### Webradio
Internetradio direkt über ZeDMD streamen — über einen kleinen integrierten Lautsprecher.
Erfordert ein **MAX98357A I2S-Verstärkermodul** — Verkabelung siehe unten.

- Dedizierte Sender-Verwaltungsseite unter `/radio.html` — Sendersuche via [radio-browser.info](https://www.radio-browser.info), Presets mit Logo-Icons speichern
- **Länder-Schnellauswahl** (🇩🇪 DE / 🇦🇹 AT / 🇨🇭 CH / 🇳🇱 NL / 🇫🇷 FR) lädt die Top-100-Sender des jeweiligen Landes mit einem Tipper; Niedersachsen-Regionalfilter weiterhin verfügbar
- Sendername, Titelinfo und Sender-Logo in der Web-Oberfläche angezeigt
- Lautstärkeregler auf der Haupt- und Radio-Seite
- Sendername und Titelinfo erscheinen beim Start 5 Sekunden auf der LED-Matrix, dann zurück zum Screensaver — „DMD 10s"-Button für erneute Anzeige
- Presets überleben Firmware-Updates (gespeichert in LittleFS)
- Stabiler Senderwechsel — kein Audio-Aussetzer beim Umschalten mehr
- Stream-URLs von radio-browser.info werden automatisch normalisiert (`?ti=`-Playlist-Hinweise werden entfernt, die die Audio-Bibliothek zum Hängen brachten)

> **Technische Anmerkung — Audio-Bibliotheks-Patch:** Der L/R-Kanaltausch erforderte einen Eingriff in die `ESP32-audioI2S`-Bibliothek, da diese Funktion dort nicht vorhanden ist. Statt die Bibliotheksdateien von Hand zu ändern (was bei jedem Library-Update verloren gehen würde), injiziert ein Build-Script (`scripts/patch_audio_lib.py`) die Änderung automatisch bei jedem Kompiliervorgang. Dieses Script entstand mit Unterstützung von **[Claude Code](https://claude.ai/code)** (KI-Coding-Assistent von Anthropic) — das die genaue Stelle in der Audio-Pipeline gefunden hat, an der die Samples sicher getauscht werden können, und das Script so gebaut hat, dass es auch bei mehrfacher Ausführung keine Doppeleinträge erzeugt. Ich hätte das alleine nicht angefasst.

> **⚠️ Wetter-API:** Open-Meteo wird über HTTP statt HTTPS abgerufen. TLS-Handshakes haben im Webradio-Build zuverlässig speicherbezogene Abstürze verursacht. Da Open-Meteo ausschließlich öffentliche Daten ohne Login liefert, ist HTTPS hier nicht erforderlich.

### GIF-Vorschau im Browser
Klick auf einen GIF-Dateinamen in der Screensaver-Dateiliste oder im „Aktuell angezeigt"-Feld öffnet eine animierte Live-Vorschau direkt im Browser — ohne das Display zu berühren. Favorit, Ignorieren und Abspielen sind direkt aus der Vorschau heraus möglich.

> **Hinweis:** Das Öffnen einer GIF-Vorschau während das Webradio läuft kann kurze Audio-Aussetzer verursachen — SD-Zugriff und Audio-Streaming teilen sich denselben CPU-Kern. Bekannte Einschränkung.

### GIF-Audio
Beim Abspielen eines animierten GIF-Screensavers passend dazu eine MP3-Datei von der SD-Karte abspielen. Der Dateiname muss dem GIF entsprechen (z.B. `demo.gif` → `demo.mp3`). Audiodateien über die Hauptseite hochladen oder direkt in `/GifAudio/` auf der SD-Karte ablegen.

- Dateiliste wird in LittleFS gecacht — nach dem ersten Boot-Scan laden Neustarts sofort (identischer Mechanismus wie der Screensaver-Datei-Cache)
- Paginierte Dateiliste in der Web-Oberfläche mit Zurück/Weiter-Buttons (20 Dateien pro Seite)
- Scan kann per **„Cancel Scan"**-Button abgebrochen werden, der nach einem Upload erscheint

> ⚠️ **Erster Boot-Scan:** Je nach Anzahl der Dateien in `/GifAudio/` kann der erste Scan über SPI sehr lange dauern. Der Fortschritt wird auf dem Display angezeigt („GifAudio XXXX"). Nach dem Scan wird die Liste in LittleFS gecacht — alle folgenden Neustarts laden in Sekunden.

> **Hinweis:** GIF-Audio spielt einmal pro GIF-Zyklus ab — Endlosschleife ist noch nicht implementiert.

### SDMMC-Board-Unterstützung *(wird nicht aktiv weiterentwickelt)*
Unterstützung für Boards mit **onboard SD-Karte via SDMMC-Interface** (1-Bit-Modus) hinzugefügt — kein externes SPI-Modul nötig. Siehe Pin-Tabelle für die erforderlichen HUB75-Kabeländerungen.

> **Hinweis:** Diese Build-Variante wird nicht aktiv gepflegt. Der Entwicklungsfokus liegt auf dem Standard-SPI-SD-Build (`wifi_sd_webradio`). Die SDMMC-Variante funktioniert möglicherweise, wird aber nicht mit jedem Release getestet.

### GIF-Screensaver-Reihenfolge
- **A-Z-Schalter** — direkt auf der Hauptseite zwischen alphabetischer und zufälliger Wiedergabereihenfolge wechseln
- **Reshuffle-Button** — GIF-Reihenfolge jederzeit neu würfeln ohne kompletten Reload

### Konfigurierbare Zeitzone
Die NTP-Zeitzone kann jetzt direkt auf der Admin-Seite eingestellt werden — kein Firmware-Rebuild nötig.
Ein Dropdown deckt die gängigsten Zeitzonen ab (Mitteleuropa, UK, UTC, US Ost/Mitte/Berg/Pazifik); die Option **Custom** akzeptiert beliebige POSIX-Zeitzone-Strings für volle Flexibilität. Die Einstellung wird in LittleFS gespeichert und übersteht Neustarts.

### SD-Karte mounten / auswerfen
Die Hauptseite zeigt jetzt neben der SD-Karten-Statusleiste einen **Mount / Eject**-Toggle-Button.
- **⏏ Eject** (grau) — SD-Karte sicher aushängen, sodass sie bei eingeschaltetem Gerät entnommen werden kann
- **⏻ Mount** (grün) — Karte nach dem Einlegen wieder einbinden; aktualisiert die Screensaver-Dateiliste und den GIF-Audio-Cache automatisch

### Stabilität & Bugfixes
Diese Version enthält eine umfassende Überarbeitung der Speicherverwaltung und Task-Sicherheit:

- **Admin-Seite stürzt nicht mehr ab** — Root-Cause per Coredump-Analyse identifiziert: lwIP-Assertion in AsyncTCP 3.3.5, ausgelöst durch den Basic-Auth-Handshake. Behoben durch Update auf AsyncTCP 3.4.10
- Race Condition beim Senderwechsel behoben — Audio springt nicht mehr auf den vorherigen Sender zurück
- Wetterdaten (MQTT + HTTP) jetzt sicher zwischen beiden CPU-Kernen synchronisiert
- SD-Karten-Verzeichnislisting aus den Netzwerk-Callbacks herausgelöst — kein Audio-Stottern mehr beim Webzugriff
- Wetter-HTTP-Antwort wird im PSRAM gepuffert — kein großer SRAM-Spike beim Abruf mehr
- Alle Datei-Uploads jetzt atomar (`.tmp` + Umbenennen) — abgebrochene Uploads hinterlassen keine korrupten Dateien
- `screensaverFiles`-Array durch Mutex gegen gleichzeitigen Webzugriff gesichert
- Audio-Task-Priorität über den Webserver-Task angehoben — reduziert Stream-Aussetzer bei gleichzeitigem Web-Traffic

---

## 🧪 Experimentelle Features

> Diese Features funktionieren, werden aber noch weiterentwickelt. Feedback willkommen.

---

### Batocera WiFi-Streaming

Batoceras DMD-Server streamt standardmäßig nur an ein einzelnes USB-DMD. Um gleichzeitig an ein WiFi-ZeDMD zu senden, muss manuell eine zweite `dmdserver`-Instanz eingerichtet werden.

Eine vollständige Schritt-für-Schritt-Anleitung — inklusive Dual-DMD-Marquee-Steuerung, Attract-/Playing-Modi und Troubleshooting — gibt es hier:

📄 **[docs/batocera-dual-dmd-DE.md](docs/batocera-dual-dmd-DE.md)** (DE) | **[docs/batocera-dual-dmd.md](docs/batocera-dual-dmd.md)** (EN)

> ⚠️ Getestet mit Batocera **v42**. Batocera **>v42** hat möglicherweise Änderungen eingeführt, die dieses Setup nicht mehr funktionsfähig machen. Vor einem Batocera-Update sorgfältig prüfen.

---

### Batocera Spielstart/Spielende-Trigger

`scripts/batocera_game_start.sh` und `scripts/batocera_game_stop.sh` triggern die GIF-Audio-Wiedergabe auf dem ZeDMD wenn ein Spiel auf Batocera startet oder endet. Das DMD empfängt weiterhin Live-Frames vom Emulator wie gewohnt — diese Scripts steuern nur den Audio-Layer.

> ⚠️ Noch nicht vollständig End-to-End getestet — der `/gif_audio_play`-Endpoint funktioniert, aber der Batocera-seitige Trigger wurde noch nicht in einer echten Spielsitzung verifiziert.

**Funktionsweise:**
- Spielstart: Batocera schickt den ROM-Namen an ZeDMD → ZeDMD spielt die passende MP3 aus `/GifAudio/` auf der SD-Karte
- Spielende: ZeDMD stoppt die Wiedergabe
- Namenskonvention: ROM-Dateiname ohne Extension → `medieval_madness.zip` → `medieval_madness.mp3`

**Einrichtung:**
1. Beide Scripts öffnen und `IP_ZEDMD` auf die feste IP des ZeDMD setzen
2. Auf Batocera kopieren:
   ```bash
   scp scripts/batocera_game_start.sh root@batocera.local:/userdata/system/scripts/gameStart.sh
   scp scripts/batocera_game_stop.sh root@batocera.local:/userdata/system/scripts/gameStop.sh
   ```
3. Passende MP3-Dateien auf der SD-Karte unter `/GifAudio/` ablegen

> ⚠️ **Falls auf Batocera bereits ein `gameStart.sh` existiert** (z.B. von einem anderen Projekt), dieses **nicht ersetzen** — den `curl`-Aufruf aus dem Script stattdessen an die bestehende Datei anhängen.

---

### Batocera Audio-Extraktionsskript

`scripts/extract_gif_audio.sh` extrahiert die ersten N Sekunden Audio aus Batocera-Scraping-Videos und speichert sie als MP3-Dateien, die direkt für ZeDMD verwendet werden können.

> ⚠️ Nur auf Batocera getestet. Erfordert `ffmpeg` und `python3` auf dem Batocera-System.

**Voraussetzungen:**
- Batocera mit SSH-Zugang
- `ffmpeg` auf Batocera vorhanden (Prüfung: `which ffmpeg`)
- `python3` auf Batocera vorhanden (Prüfung: `which python3`)
- Gescrapte Spielvideos in `gamelist.xml` (`/userdata/roms/<system>/`)

**Nutzung (per SSH auf Batocera ausführen):**
```bash
ssh root@batocera.local "bash /tmp/extract_gif_audio.sh [Optionen]"
```

| Option | Standard | Beschreibung |
|--------|----------|--------------|
| `--system` | `mame` | ROM-System-Ordnername |
| `--limit` | `10` | Maximale Anzahl zu extrahierender Dateien |
| `--duration` | `15` | Clip-Länge in Sekunden |
| `--out` | `/userdata/zedmd/gif_audio` | Ausgabeverzeichnis |
| `--game` | *(alle)* | Filter nach Spielname (Teilsuche) |

**Ablauf:**
1. Skript auf Batocera kopieren: `scp scripts/extract_gif_audio.sh root@batocera.local:/tmp/`
2. Per SSH ausführen (siehe oben)
3. Ausgabeordner im Finder öffnen: **Netzwerk → batocera → share → zedmd → gif_audio**
4. MP3s über **`http://<ZeDMD-IP>/`** → GIF-Audio hochladen

---

### Display Text

Sendet eine individuelle Textnachricht direkt über die Web-UI an die LED-Matrix.

- Statische oder scrollende Anzeige — kurze Texte werden zentriert angezeigt, längere scrollen automatisch
- Farbwahl per RGB-Colorpicker
- Konfigurierbare Anzeigedauer (5–60 Sekunden)
- Unterbricht sofort jeden laufenden Screensaver oder GIF und stellt ihn anschließend wieder her
- **Emoji-Picker** — 34 Emojis (❤️ ⭐ 🔥 😊 🎉 u.v.m.) werden als Pixel-Art-Icons direkt im Lauftext gerendert; Icons liegen als RGBA-Dateien in LittleFS (`/icons/`)

---

### Stereo-Audio

Stereo-Ausgabe mit **zwei MAX98357A-Modulen** — eines für den linken, eines für den rechten Kanal.

Der SD-Pin ist eine Spannungspegel-Kanalwahl-Brücke. Die folgenden Werte wurden gemessen und bestätigt für MAX98357A-Breakout-Boards, die bereits einen **1 MΩ Widerstand von SD nach Vin** onboard haben. Bei abweichender Boardbestückung gelten diese Werte nicht — immer Schaltplan des eigenen Boards prüfen und Spannung messen, bevor etwas angeschlossen wird.

* **Modul L (linker Kanal):** **100 kΩ** Widerstand von SD nach VCC **(5V)**.
* **Modul R (rechter Kanal):** **390 kΩ** Widerstand von SD nach VCC **(5V)**.

Die ESP32-audioI2S-Bibliothek gibt bei Stereo-Quelldateien nativ Stereo-I2S aus. Jedes Modul dekodiert automatisch seinen zugewiesenen Kanal anhand der SD-Pin-Spannung.

| MAX98357A Pin | ESP32-S3 | Hinweis |
|---------------|----------|---------|
| BCLK | **GPIO 9** | gemeinsam — beide Module |
| LRC (WSEL) | **GPIO 14** | gemeinsam — beide Module |
| DIN | **GPIO 21** | gemeinsam — beide Module |
| SD — Modul L | **100 kΩ nach VCC** | → **linker Kanal** |
| SD — Modul R | **390 kΩ nach VCC** | → **rechter Kanal** |
| VIN | **5V** *(empfohlen)* | jedes Modul separat |
| GND | **GND** | jedes Modul separat |

> ⚠️ Die obigen Widerstandswerte wurden ausschließlich bei **5V** verifiziert. Bei 3,3V-Versorgung ändert sich der Wert für den rechten Kanal auf ca. **210 kΩ** (nicht getestet). 3,3V-Versorgung wird **nicht empfohlen** — sie erhöht die Verzerrung und belastet die 3,3V-Schiene.

> ⚠️ **DISCLAIMER:**
> Diese Widerstandswerte wurden experimentell mit einem spezifischen MAX98357A-Breakout-Board ermittelt, das einen 1 MΩ Widerstand von SD nach Vin onboard hat. Andere Board-Varianten können andere Werte erfordern. **Immer das MAX98357A-Datenblatt lesen, den Schaltplan des eigenen Boards prüfen und Spannungen messen, bevor etwas angeschlossen wird.**
> **Nutzung auf eigene Gefahr — keinerlei Gewährleistung oder Haftung.**

---

## 🔜 Geplante Features

### Eigene Platine von [elabree](https://github.com/elabree) ✅
Eine dedizierte Trägerplatine, entworfen und gebaut von [elabree](https://github.com/elabree) — getestet und bestätigt funktionsfähig mit dieser Firmware. Die Platine trägt das ESP32-S3-DevKitC-1-N16R8, einen SD-Karten-Modul-Sockel und zwei MAX98357A-Stereo-Verstärker-Sockel auf einer kompakten Leiterplatte.

Zwei Revisionen verfügbar:
- **Rev1.1** — für das original Espressif ESP32-S3-DevKitC (0,9″ Pin-Reihenabstand)
- **Rev1.2** — für die meisten Clone-Boards (1″ Pin-Reihenabstand)

KiCad-Quellen und Gerber-Dateien zum Bestellen bei JLCPCB: [KiCad/ZeDMD_WiFi/](https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/tree/main/KiCad/ZeDMD_WiFi) (direkt in diesem Repository enthalten).

Ein herzliches Dankeschön an elabree für den Entwurf und die Bereitstellung dieser Platine — ohne seine Arbeit wäre das nicht möglich gewesen.

### Code-Aufräumen *(steht auf meiner Liste)*
Der Code ist hier und da ehrlich gesagt etwas gewachsen und durcheinander geraten — ich weiß das. Ich plane irgendwann aufzuräumen, aber wann genau kann ich nicht versprechen. Er funktioniert, und das zählt erstmal.

---

## Was ist anders als beim Original-ZeDMD?

Dieser Fork ist **nur WiFi** und zielt auf den **ESP32-S3-N16R8** mit einer **128×32 LED-Matrix** ab.

### Alle hinzugefügten Features

- **WiFi OTA Firmware-Update** — neue Firmware direkt über den Browser flashen (`/admin.html`); Firmware-Version mit Build-ID und Branch auf der Admin-Seite sichtbar (Format: `5.1.8-jb (Datum) [abc1234@main]`)
- **Screensaver** — GIF/RAW-Diashow mit Uhrzeit- und Wetteranzeige (Open-Meteo, plain HTTP aus RAM-Gründen)
- **Screensaver-Verwaltung** — Favoriten, Ignore-Liste, Alphabetisch/Zufällig, Strict Timer, Pause/Weiter
- **GIF-Vorschau** — Klick auf Dateinamen öffnet animierte Browser-Vorschau; Favorit/Ignorieren/Abspielen direkt aus der Vorschau
- **GIF-Audio** — passende MP3 aus `/GifAudio/` synchron zum GIF abspielen; `scripts/extract_gif_audio.sh` extrahiert Audio aus Batocera-Videodateien
- **Verbessertes Webinterface** — Dateiverwaltung, Favoriten-/Ignore-Buttons pro Datei, Seitenumbruch mit Wrap-Around
- **Admin-Seite** — WiFi, Display, Transport, MQTT, Wetter-Einstellungen
- **Webradio** — Internetradio via I2S-Verstärker (MAX98357A); Sendersuche via [radio-browser.info](https://www.radio-browser.info); Preset-Verwaltung mit Logo-Icons; LED-Matrix zeigt Senderinfo 5 s beim Start, „DMD 10s"-Button für On-Demand-Anzeige
- **Konfig Export/Import** — vollständiges Konfigurations-Backup und -Restore über den Browser (`/config_transfer.html`)
- **Display Text** — individuelle Textnachricht mit Emojis über die Web-UI an die LED-Matrix senden; statisch oder scrollend, mit Farbwahl und konfigurierbarer Dauer (5–60 s); 34 Emojis per eingebautem Picker wählbar
- **Display-Timer** — tägliche Ein-/Ausschaltzeiten für das LED-Display; „Display off / Display on"-Button für sofortiges manuelles Aus-/Einschalten
- **Tabs im Webinterface** — Hauptseite in Screensaver / Display / Radio gegliedert; SD-Info und Admin immer sichtbar
- **Stereo-Audio** *(experimentell)* — zwei MAX98357A-Module für echten Stereo-Ausgang; Kanalwahl per SD-Pin-Widerstandsbrücke (nur 5V, Werte verifiziert); Stereo/Mono-Umschalter in der Web-UI

---

## Hardware — ESP32-S3-N16R8 Hinweis

### 💡 Stromversorgung

Der ZeDMD kann je nach Betrieb ganz schön Strom ziehen — besonders wenn helle GIFs laufen, gleichzeitig Webradio streamt und WiFi aktiv ist. Ein einfacher Laptop-USB-Port oder ein billiges Handy-Ladegerät liefern dafür manchmal nicht genug stabilen Strom, was sich in unerwarteten Neustarts oder einem unstabilen Bild äußern kann.

**Bei merkwürdigem Verhalten: ein ordentliches 5V/2A-USB-Netzteil verwenden** (so eines wie bei einem guten Handy oder Tablet dabei ist). Wie viel Strom wirklich gebraucht wird, hängt stark vom Inhalt ab — dunkle GIFs ohne Audio brauchen deutlich weniger als alles auf Vollbetrieb.

---

### 🧠 Am Rande eines Nervenzusammenbruchs — was dieser kleine Chip alles gleichzeitig tut

Zu jedem Zeitpunkt erledigt der ESP32-S3 in diesem Build parallel:

- Ansteuerung einer 128×32 HUB75 LED-Matrix über DMA
- Bereitstellung einer vollständigen Web-Oberfläche mit Live-Updates über WiFi
- Streaming und Dekodierung von Webradio (MP3/AAC) über I2S in einem eigenen FreeRTOS-Task
- Lesen und Abspielen animierter GIFs von der SD-Karte
- Synchrones Abspielen passender MP3-Dateien zu diesen GIFs
- Abrufen von Live-Wetterdaten und Rendern von Icons
- MQTT, OTA, Datei-Upload und ein paginierter Dateibrowser für 2.000+ Dateien
- Verwaltung eines LittleFS-Caches, Crash-Logs und Boot-Diagnose

Das alles auf einem Chip mit **327 KB internem SRAM** — von dem WiFi-Stack, TCP-Server und Audio-Puffer zur Laufzeit den Großteil beanspruchen. Bei Spitzenlast bleiben manchmal weniger als **1 KB frei**. Der 8 MB große PSRAM rettet uns bei großen Caches und Pixel-Daten, aber alles was WiFi oder TCP anfasst muss zwingend im internen SRAM leben — und da ist es wirklich eng.

Die Firmware arbeitet hart daran, das stabil zu halten: PSRAM-basierte Caches, sorgfältige Puffergrößen, gestaffelte HTTP-Anfragen und ein Heap-Monitor der alle 30 Sekunden den freien Speicher protokolliert. Aber dieses Gerät läuft an der Grenze dessen, wofür seine Hardware-Klasse konzipiert wurde — und man merkt es, im besten Sinne.

---

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

**Scan-Performance beim ersten Start**

> ⚠️ Der erste Scan einer großen GIF-Bibliothek kann sehr lange dauern — ein Ordner mit ~5 400 Dateien benötigt über SPI etwa **35 Minuten**. Das ist eine grundsätzliche SPI/FAT32-Einschränkung: Die Arduino-SD-Library öffnet jede Datei einzeln, und FAT32-Verzeichnisse mit vielen Einträgen können nicht indexiert werden.
>
> **Nach dem ersten Scan wird die Dateiliste in LittleFS gecacht** (`sc_*.bin`). Jeder weitere Neustart lädt den Cache in Sekunden — kein SD-Scan nötig.
>
> **Tipps für akzeptable Scan-Zeiten:**
> - **Mehrere kleinere Unterordner** statt einem großen verwenden (z.B. 5 × 1 000 Dateien statt 1 × 5 000) — jeder Ordner wird unabhängig gescannt und separat gecacht.
> - **Dedizierten Upload-Ordner** nutzen (klein, nur frisch hochgeladene Dateien) — der „Upload folder"-Picker in der Web-UI ermöglicht gezieltes Hochladen ohne den großen Bibliotheksordner anzufassen.
> - Eine geplante zukünftige Optimierung ist eine **SD-seitige Indexdatei** (`.index` pro Ordner), die Directory-Traversal durch ein einzelnes sequentielles Lesen ersetzt — Scan-Zeit von Minuten auf unter einer Sekunde, unabhängig von der Dateianzahl.

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
| **Information** | Firmware-Version, Build-Datum und Git-Hash, Debug-Info |

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
| E | 1 | nicht genutzt / nur f. 256x64 relevant |
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

Erfordert ein **I2S-Verstärkermodul** und einen kleinen Lautsprecher.

![MAX98357A Modul](docs/images/max98357a.jpg)

**Getesteter Verstärker: MAX98357A**
- Breakout-Modul (z.B. Adafruit #3006 oder handelsübliche Klone)
- Lautsprecher: **8 Ω / 3 W** — z.B. **Visaton FSR 7** (77 mm, guter Klang für die Größe)
- Ausgangsleistung: bis zu 3 W — ausreichend für moderate Lautstärke im Gehäuse



### MAX98357A Verkabelung — Mono (SD- und SDMMC-Build)

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
| **Lautstärke** | Schieberegler auf der Haupt- und Radio-Seite |
| **DMD 10s** | Senderinfo 10 Sekunden auf der LED-Matrix anzeigen; beim Start automatisch 5 s sichtbar |

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

### Erstmaliges Flashen (USB, einmalig)

Beim ersten Flash muss das LittleFS-Dateisystem mitgeflasht werden — sonst lädt das Webinterface nach dem Boot nicht. Dafür das Merged-Image verwenden:

```bash
pio run                              # Firmware bauen
pio run -t buildfs                   # LittleFS-Image aus data/ bauen
python3 scripts/merge_firmware.py    # Gesamtes Flash-Image zusammensetzen
```

Dann die resultierende `*_merged.bin` aus `~/Desktop/Firmwares/` flashen:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* --baud 460800 \
  write_flash --no-compress 0x0 "ZeDMD_..._merged.bin"
```

> ⚠️ **`--no-compress` ist Pflicht.** Die LittleFS-Partition belegt 6,5 MB Flash. Ohne `--no-compress` komprimiert esptool die Übertragung — das führt bei der großen Partition zu einem Timeout und der ESP hängt mitten im Flash-Vorgang. Immer `--no-compress` verwenden.

### Nach dem ersten Boot

Das Webinterface ist sofort erreichbar. Falls HTML-Dateien aktualisiert werden müssen:
**`http://<IP>/admin.html`** → „Web-Dateien aktualisieren"

### Zukünftige Firmware-Updates (OTA, kein USB nötig)

Browser → **`http://<IP>/admin.html`** → „Firmware Update (OTA)"

---

## Danksagung

- **[Markus Kalkbrenner / PPUC](https://github.com/PPUC/ZeDMD)** — original ZeDMD project
- **[elabree](https://github.com/elabree)** — Platinenentwurf: Trägerplatine für DevKit + SD + MAX98357A ([KiCad-Dateien](https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/tree/main/KiCad))
- **Niels (My Son)** — coding assistance & inspiration & moral support
- **[Claude Sonnet](https://anthropic.com)** — coding assistance

---

## Kommerzielle Nutzung

Dieses Projekt steht unter der GPL v2 — kommerzielle Nutzung ist unter diesen Bedingungen erlaubt.

---

## Lizenz

Identisch mit dem Original-Projekt — siehe [LICENSE](LICENSE).

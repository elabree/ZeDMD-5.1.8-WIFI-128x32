# Batocera Dual-DMD Einrichtung — USB + WiFi ZeDMD

> ⚠️ **Experimentell** — getestet mit Batocera **v42**. Batocera **>v42** hat möglicherweise Änderungen, die dieses Setup nicht mehr funktionsfähig machen. Vor einem Batocera-Update sorgfältig prüfen.
>
> Diese Anleitung ist ein persönliches Setup-Protokoll. Es funktioniert auf meinem Cabinet — Hardware und Batocera-Version können bei dir abweichen. Keine Garantie, kein Support.

---

## Übersicht

Batoceras eingebauter DMD-Server (`dmd_real`) unterstützt standardmäßig nur ein einzelnes USB-DMD. Diese Anleitung fügt eine **zweite dmdserver-Instanz** hinzu, die gleichzeitig an ein WiFi-ZeDMD (128×32) streamt.

```
USB-DMD   → 256×64 px, Standard-Port   (dmd-play --hd)
WiFi-DMD  → 128×32 px, Port 9001       (dmd-play -p 9001)
```

Beide DMDs zeigen beim Durchblättern das Marquee des gewählten Spiels. Beim Spielstart übernimmt `zedmd_marquee.sh` mit Arcade-typischem Verhalten (Insert Coin, Now Playing usw.).

---

## Benötigte Dateien

**Scripts:**

| Datei | Zweck |
|-------|-------|
| `postshare.sh` | Boot-Script — startet beide dmdserver + Screensaver |
| `zedmd_marquee.sh` | Marquee-Steuerung (gameStart/gameStop-Hook) |
| `game-selected.sh` | Marquee beim Durchblättern der Spielliste |
| `system-selected.sh` | Marquee beim Durchblättern der Systemliste |

**GIF-Animationen:**

| Datei | Größe | Verwendung |
|-------|-------|------------|
| `screensaver.gif` | beliebig | Bootbild (automatisch skaliert auf beiden DMDs) |
| `insert-coin.gif` | 256×64 | Attract-Modus — USB-DMD |
| `now-playing-128x32.gif` | 128×32 | Spielmodus — WiFi-DMD |
| `press1or2.gif` | 256×64 | Credit-Anzeige — USB-DMD |
| `press1or2_128x32.gif` | 128×32 | Credit-Anzeige — WiFi-DMD (optional) |

**Config:**

| Datei | Zweck |
|-------|-------|
| `config_real2.ini` | Konfiguration für die zweite dmdserver-Instanz (WiFi-DMD) |

---

## Ordnerstruktur auf Batocera

```
/boot/
└── postshare.sh

/userdata/system/
├── scripts/
│   └── zedmd_marquee.sh
├── services/
│   └── dmd_real2                         ← zweiter DMD-Service (WiFi)
└── dmd/
    ├── screensaver.gif
    ├── insert_coin/
    │   ├── insert-coin.gif               (256×64)
    │   └── now-playing-128x32.gif        (128×32)
    ├── press1or2.gif                     (256×64)
    ├── press1or2_128x32.gif              (128×32, optional)
    ├── systems/
    │   └── mame.png / snes.png ...
    └── games/
        └── mame/
            ├── dkong.gif                 ← Spielmarquee USB-DMD (256×64)
            └── 128x32/
                └── dkong.gif             ← Spielmarquee WiFi-DMD (128×32)

/userdata/system/configs/emulationstation/scripts/
├── game-selected/
│   └── game-selected.sh
└── system-selected/
    └── system-selected.sh

/userdata/system/configs/dmdserver/
└── config_real2.ini                      ← WiFi-DMD-Server-Config
```

---

## Schritt 1 — Zweiten DMD-Service einrichten (WiFi-DMD)

**Original-Service kopieren:**
```bash
cp /usr/share/batocera/services/dmd_real /userdata/system/services/dmd_real2
```

**`dmd_real2` bearbeiten:**
```bash
nano /userdata/system/services/dmd_real2
```

```sh
#!/bin/sh
PIDFILE=/var/run/dmd_server_real2.pid

start() {
    echo -n "Starting second dmd-server (WiFi): "
    ARGS="-c /userdata/system/configs/dmdserver/config_real2.ini"
    start-stop-daemon -S -b -q -m -p $PIDFILE --exec /usr/bin/dmdserver -- ${ARGS} >/dev/null &
    echo "done"
}

stop() {
    echo -n "Stopping second dmd-server (WiFi): "
    /usr/bin/dmd-play --port 9001 --clear
    start-stop-daemon -K -q -p $PIDFILE
    echo "done"
}

status() {
    start-stop-daemon --status -q -p $PIDFILE && echo "started" || echo "stopped"
}

case "$1" in
    start)  start  ;;
    stop)   stop   ;;
    status) status ;;
esac
```

> ⚠️ Der Port (9001) wird in `config_real2.ini` definiert, **nicht** als Kommandozeilen-Argument.

**Config-Datei erstellen** unter `/userdata/system/configs/dmdserver/config_real2.ini` — Port 9001 und die WiFi-IP des ZeDMD eintragen.

**Service aktivieren:**
Batocera-Menü → System-Einstellungen → Services → `dmd_real2` aktivieren

---

## Schritt 2 — Boot-Script (`postshare.sh`)

Das Boot-Script sorgt dafür, dass beide DMDs direkt beim Start ein Bild anzeigen. Es läuft als `S12` — früh im Boot-Prozess, lange vor `S99` wo die Services starten.

`/boot/postshare.sh`:

```bash
#!/bin/bash
LOGFILE="/userdata/system/dmd.log"
echo "postshare started" >> "$LOGFILE"

# Auf USB-DMD-Gerät warten (ttyACM)
TIMEOUT=10
COUNTER=0
PATTERN="/dev/ttyACM*"
while [ -z "$(ls $PATTERN 2>/dev/null)" ] && [ $COUNTER -lt $TIMEOUT ]; do
  sleep 1; ((COUNTER++))
done

if [ -z "$(ls $PATTERN 2>/dev/null)" ]; then
  echo "Error: no ttyACM device found." >> "$LOGFILE"
  exit 1
fi

DEVICE=$(ls $PATTERN | head -n 1)
echo "Device $DEVICE ready" >> "$LOGFILE"

# USB-DMD-Server starten
pkill dmdserver
sleep 1
/usr/bin/dmdserver &
echo "dmdserver (USB) started" >> "$LOGFILE"
sleep 2

# WiFi-DMD-Server starten (mit PID-Datei — verhindert Doppelstart durch Service)
WLAN_PIDFILE=/var/run/dmd_server_real2.pid
if ! start-stop-daemon --status -q -p $WLAN_PIDFILE 2>/dev/null; then
    start-stop-daemon -S -b -q -m -p $WLAN_PIDFILE \
        --exec /usr/bin/dmdserver -- \
        -c /userdata/system/configs/dmdserver/config_real2.ini >/dev/null
    echo "dmdserver (WiFi) started" >> "$LOGFILE"
    sleep 2
else
    echo "dmdserver (WiFi) already running" >> "$LOGFILE"
fi

# Screensaver auf beiden DMDs
nohup /usr/bin/dmd-play --hd -f /userdata/system/dmd/screensaver.gif >/dev/null 2>&1 &
echo "bootlogo USB sent" >> "$LOGFILE"

nohup /usr/bin/dmd-play -p 9001 -f /userdata/system/dmd/screensaver.gif >/dev/null 2>&1 &
echo "bootlogo WiFi sent" >> "$LOGFILE"
```

```bash
chmod +x /boot/postshare.sh
```

> Der WiFi-dmdserver wird hier mit der **gleichen PID-Datei** wie der `dmd_real2`-Service gestartet. Wenn `S99userservices` später versucht `dmd_real2` zu starten, findet `start-stop-daemon` die vorhandene PID-Datei und überspringt den Start — verhindert eine doppelte Instanz.

---

## Schritt 3 — Marquee-Scripts

### `game-selected.sh` — beim Durchblättern der Spielliste

Pfad: `/userdata/system/configs/emulationstation/scripts/game-selected/game-selected.sh`

Sucht ein Marquee in dieser Reihenfolge:
1. `/userdata/system/dmd/games/<system>/<rom>.gif/png` (exakter Name)
2. `/userdata/system/dmd/games/<system>/<rommin>.gif/png` (vereinfachter Name)
3. Batocera-API (gescraptes Marquee)
4. Spielname als Text
5. Leeren (blank)

Für das WiFi-DMD wird zusätzlich nach einer optimierten 128×32-Version im Unterordner `128x32/` gesucht.

> ⚠️ Wenn `/tmp/zedmd_mode` existiert (Spiel läuft), lässt `game-selected.sh` das WiFi-DMD in Ruhe — `zedmd_marquee.sh` hat die Kontrolle.

### `system-selected.sh` — beim Durchblättern der Systemliste

Pfad: `/userdata/system/configs/emulationstation/scripts/system-selected/system-selected.sh`

Sucht nach:
1. `/userdata/system/dmd/systems/<system>.gif/png`
2. Batocera-API (System-Logo)
3. Leeren (blank)

---

## Schritt 4 — Marquee-Steuerung (`zedmd_marquee.sh`)

Pfad: `/userdata/system/scripts/zedmd_marquee.sh`
```bash
chmod +x /userdata/system/scripts/zedmd_marquee.sh
```

**Marquee-Suchreihenfolge — USB-DMD:**
1. `/userdata/system/dmd/games/<system>/<rom>.gif/png`
2. `/userdata/roms/<system>/images/<rom>-marquee.png`
3. `/userdata/roms/<system>/images/<rom>.png`
4. `/userdata/system/dmd/systems/<system>.png`

**Marquee-Suchreihenfolge — WiFi-DMD (128×32):**
1. `/userdata/system/dmd/games/<system>/128x32/<rom>.gif/png` ← bevorzugt
2. `/userdata/system/dmd/games/<system>/<rom>.gif/png`
3. Fallback: gleiches Bild wie USB-DMD (automatisch skaliert)

---

## Anzeigemodi

### ATTRACT-Modus *(Spiel gestartet, noch kein Credit)*
| DMD | Anzeige |
|-----|---------|
| USB-DMD | wechselt alle 4s: Spielmarquee ↔ `insert-coin.gif` |
| WiFi-DMD | zeigt: Spielmarquee (128×32 wenn verfügbar) |

### CREDIT-Modus *(Münze eingeworfen)*
| DMD | Anzeige |
|-----|---------|
| USB-DMD | `press1or2.gif` |
| WiFi-DMD | `press1or2_128x32.gif` (Fallback: `press1or2.gif`) |

Vorheriger Zustand (ATTRACT/PLAYING) wird gespeichert und nach dem Credit wiederhergestellt.

### PLAYING-Modus *(Start gedrückt, Credit vorhanden)*
| DMD | Anzeige |
|-----|---------|
| USB-DMD | Spielmarquee (statisch) |
| WiFi-DMD | wechselt alle 4s: `now-playing-128x32.gif` ↔ Spielmarquee |

---

## Zustandsautomat

```
ATTRACT ──[Münze]──► CREDIT ──[Start, Credits > 0]──► PLAYING
   ▲                   │                                  │
   └──[Start,          │                                  │
   Credits=0,          │ [Start, Credits=0,               │
   war ATTRACT]        │  war ATTRACT]                    │
                       └──────────────────────────────────┘

PLAYING ──[Münze]──► CREDIT ──[Start, Credits > 0]──► PLAYING
                       │
                       │ [Start, Credits=0, war PLAYING]
                       └──────────────────────────────► PLAYING
```

`/tmp/zedmd_prev_mode` speichert den Zustand vor dem Wechsel in CREDIT.

---

## Controller-Belegung

| Taste | Aktion |
|-------|--------|
| `BTN_BASE3` | Münze einwerfen (Credit +1) |
| `BTN_BASE4` / `BTN_START` | Spiel starten (Credit −1 → PLAYING); kein Credit → zurück zu ATTRACT oder PLAYING |

---

## Temporäre Laufzeit-Dateien

| Datei | Zweck |
|-------|-------|
| `/tmp/zedmd_lock` | Verhindert mehrere Script-Instanzen |
| `/tmp/zedmd_mode` | Aktueller Zustand (ATTRACT / CREDIT / PLAYING) |
| `/tmp/zedmd_prev_mode` | Zustand vor dem letzten CREDIT |
| `/tmp/zedmd_path` | Marquee-Pfad — USB-DMD |
| `/tmp/zedmd_path_128` | Marquee-Pfad — WiFi-DMD (128×32) |
| `/tmp/zedmd_credit_log` | Credits (eine Zeile = 1 Credit) |

Alle Dateien werden beim Spielende automatisch gelöscht.

---

## Abhängigkeiten

| Tool | Zweck |
|------|-------|
| `dmd-play` | DMD-Wiedergabe-Tool |
| `dmdserver` | DMD-Server (läuft zweimal: USB + WiFi) |
| `evtest` | Joystick-Event-Auslesen |
| `udevadm` | Geräteerkennung |
| `nc` | Port-Check (netcat) |
| `wget` + `jq` | Batocera-API-Abfragen (Marquee-Pfade) |

---

## Fehlerbehebung

### Doppelte WiFi-dmdserver-Instanz
**Ursache:** `postshare.sh` und `dmd_real2`-Service versuchen beide den Server zu starten.
**Lösung:** `postshare.sh` nutzt die gleiche PID-Datei wie der Service → zweiter Start wird automatisch übersprungen.

### gameStop räumt `/tmp/zedmd_*` nicht auf
**Ursache:** `pkill -9 -f zedmd_marquee.sh` hat das Script beendet bevor `rm` laufen konnte.
**Lösung:** Immer erst aufräumen, dann beenden — oder `/tmp/zedmd_lock` nutzen um Mehrfachstarts zu verhindern.

### Mehrere Script-Instanzen laufen gleichzeitig
**Ursache:** Batocera ruft Scripts mehrfach auf (z.B. bei ROM-Varianten).
**Lösung:** Lock-Mechanismus über `/tmp/zedmd_lock`.

### WiFi-DMD zeigt falsches Bild während das Spiel läuft
**Ursache:** `game-selected.sh` und `zedmd_marquee.sh` senden gleichzeitig.
**Lösung:** `game-selected.sh` prüft `/tmp/zedmd_mode` → überspringt WiFi-DMD wenn Spiel läuft.

### Kein Marquee für Spiele ohne eigene DMD-Datei
**Lösung:** `find_marquee_path()` durchsucht auch Batoceras Standard-Medienpfade:
`/userdata/roms/<system>/images/<rom>-marquee.png`

### WiFi-DMD friert ein (GIF endet, kein Loop)
**Ursache:** `--once`-Flag in `dmd-play` → GIF wird nur einmal abgespielt.
**Lösung:** `--once` entfernen → GIF läuft in Dauerschleife.

---

## Logging

```bash
# Boot-Log
cat /userdata/system/dmd.log

# Laufende Prozesse
ps aux | grep dmdserver
ps aux | grep dmd-play
ps aux | grep zedmd

# Aktueller Zustand
cat /tmp/zedmd_mode
cat /tmp/zedmd_path
cat /tmp/zedmd_credit_log

# Port 9001 prüfen
nc -z localhost 9001 && echo "OK" || echo "ERROR"

# WiFi-DMD manuell testen
dmd-play -p 9001 -f /userdata/system/dmd/screensaver.gif

# Temporäre Dateien zurücksetzen
rm -f /tmp/zedmd_*
```

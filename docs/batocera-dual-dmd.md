# Batocera Dual-DMD Setup — USB + WiFi ZeDMD

> ⚠️ **Experimental** — tested with Batocera **v42**. Verify carefully before updating Batocera.
>
> This guide is a personal setup diary. It worked on my cabinet — your hardware and Batocera version may differ. No warranty, no support.


---

## Overview

Batocera's built-in DMD server (`dmd_real`) supports only a single USB-connected DMD out of the box. This guide adds a **second DMD server instance** that streams to a WiFi ZeDMD (128×32) in parallel.

```
USB-DMD   → 256×64 px, default port   (dmd-play --hd)
WiFi-DMD  → 128×32 px, port 9001      (dmd-play -p 9001)
```

Both DMDs show the selected game's marquee while browsing. On game start, `zedmd_marquee.sh` takes over with arcade-style behaviour (Insert Coin, Now Playing, etc.).

---

## Required Files

**Scripts:**

| File | Purpose |
|------|---------|
| `postshare.sh` | Boot script — starts both DMD servers + screensaver |
| `zedmd_marquee.sh` | Marquee controller (gameStart/gameStop hook) |
| `game-selected.sh` | Marquee when scrolling the game list |
| `system-selected.sh` | Marquee when scrolling the system list |

**GIF animations:**

| File | Size | Usage |
|------|------|-------|
| `screensaver.gif` | any | Boot screen (auto-scaled on both DMDs) |
| `insert-coin.gif` | 256×64 | Attract mode — USB-DMD |
| `now-playing-128x32.gif` | 128×32 | Playing mode — WiFi-DMD |
| `press1or2.gif` | 256×64 | Credit display — USB-DMD |
| `press1or2_128x32.gif` | 128×32 | Credit display — WiFi-DMD (optional) |

**Config:**

| File | Purpose |
|------|---------|
| `config_real2.ini` | Config for the second DMD server (WiFi-DMD) |

---

## Folder Structure on Batocera

```
/boot/
└── postshare.sh

/userdata/system/
├── scripts/
│   └── zedmd_marquee.sh
├── services/
│   └── (dmd_real2 does not exist — WiFi dmdserver is started by postshare.sh)
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
            ├── dkong.gif                 ← game marquee USB-DMD (256×64)
            └── 128x32/
                └── dkong.gif             ← game marquee WiFi-DMD (128×32)

/userdata/system/configs/emulationstation/scripts/
├── game-selected/
│   └── game-selected.sh
└── system-selected/
    └── system-selected.sh

/userdata/system/configs/dmdserver/
└── config_real2.ini                      ← WiFi DMD server config
```

---

## Step 1 — Set Up the Second DMD Service (WiFi-DMD)

> ℹ️ **Actual implementation:** The service script `/userdata/system/services/dmd_real2` is **not used** in the current setup. The WiFi dmdserver is started exclusively by `postshare.sh` (Step 2). The entry `dmd_real2` in `batocera.conf` as an enabled service is harmless leftover — since no script file exists, nothing happens at boot.
>
> **Why no service script?** User services start via `S99userservices` — far too late in the boot process. `postshare.sh` runs as `S12` and therefore displays content during the very early boot phase. A pure service-script approach would leave the WiFi-DMD blank until EmulationStation is nearly ready.
>
> The section below documents the service-script approach as a **reference** for a future reinstall or migration to Batocera >v42.

**Copy the original service:**
```bash
cp /usr/share/batocera/services/dmd_real /userdata/system/services/dmd_real2
```

**Edit `dmd_real2`:**
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

> ⚠️ The port (9001) is defined in `config_real2.ini`, **not** as a command-line argument.

**Create the config file** at `/userdata/system/configs/dmdserver/config_real2.ini` — enter port 9001 and your ZeDMD's WiFi IP address.

**Enable the service:**
Batocera menu → System Settings → Services → enable `dmd_real2`

---

## Step 2 — Boot Script (`postshare.sh`)

The boot script ensures both DMDs show an image right from startup. It runs as `S12` — early in the boot process, long before `S99` where services start.

`/boot/postshare.sh`:

```bash
#!/bin/bash
LOGFILE="/userdata/system/dmd.log"
echo "postshare started" >> "$LOGFILE"

# Wait for USB-DMD device (ttyACM)
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

# Start USB-DMD server
pkill dmdserver
sleep 1
/usr/bin/dmdserver &
echo "dmdserver (USB) started" >> "$LOGFILE"
sleep 2

# Start WiFi-DMD server (with PID file — prevents double-start by service)
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

# Screensaver on both DMDs
nohup /usr/bin/dmd-play --hd -f /userdata/system/dmd/screensaver.gif >/dev/null 2>&1 &
echo "bootlogo USB sent" >> "$LOGFILE"

nohup /usr/bin/dmd-play -p 9001 -f /userdata/system/dmd/screensaver.gif >/dev/null 2>&1 &
echo "bootlogo WiFi sent" >> "$LOGFILE"
```

```bash
chmod +x /boot/postshare.sh
```

> The WiFi dmdserver is started here using the **same PID file** as the `dmd_real2` service. When `S99userservices` later tries to start `dmd_real2`, `start-stop-daemon` finds the existing PID file and skips the start — preventing a duplicate instance.

---

## Step 3 — Marquee Scripts

### `game-selected.sh` — while scrolling the game list

Path: `/userdata/system/configs/emulationstation/scripts/game-selected/game-selected.sh`

Searches for a marquee in this order:
1. `/userdata/system/dmd/games/<system>/<rom>.gif/png` (exact name)
2. `/userdata/system/dmd/games/<system>/<rommin>.gif/png` (simplified name)
3. Batocera API (scraped marquee)
4. Game name as text
5. Clear (blank)

For the WiFi-DMD, it additionally checks for an optimised 128×32 version in the `128x32/` subfolder.

> ⚠️ If `/tmp/zedmd_mode` exists (game is running), `game-selected.sh` leaves the WiFi-DMD alone — `zedmd_marquee.sh` is in control.

### `system-selected.sh` — while scrolling the system list

Path: `/userdata/system/configs/emulationstation/scripts/system-selected/system-selected.sh`

Searches for:
1. `/userdata/system/dmd/systems/<system>.gif/png`
2. Batocera API (system logo)
3. Clear (blank)

---

## Step 4 — Marquee Controller (`zedmd_marquee.sh`)

Path: `/userdata/system/scripts/zedmd_marquee.sh`
```bash
chmod +x /userdata/system/scripts/zedmd_marquee.sh
```

> ℹ️ **How is the script called?** Not via ES hooks (`game-start` / `game-end`), but by Batocera's **`emulatorlauncher`** — which automatically calls all executable scripts in `/userdata/system/scripts/` on game start and end:
>
> ```
> Game start:  zedmd_marquee.sh gameStart <system> <systemname> <emulator> <rompath>
> Game end:    zedmd_marquee.sh gameStop
> ```
>
> Example for MAME: `zedmd_marquee.sh gameStart mame mame mame /userdata/roms/mame/1942.zip`
>
> In the script: `$1` = action, `$2` = system, `$5` = ROM path. The script runs as a background daemon and monitors joystick input via `evtest` until the game ends.

**Marquee search order — USB-DMD:**
1. `/userdata/system/dmd/games/<system>/<rom>.gif/png`
2. `/userdata/roms/<system>/images/<rom>-marquee.png`
3. `/userdata/roms/<system>/images/<rom>.png`
4. `/userdata/system/dmd/systems/<system>.png`

**Marquee search order — WiFi-DMD (128×32):**
1. `/userdata/system/dmd/games/<system>/128x32/<rom>.gif/png` ← preferred
2. `/userdata/system/dmd/games/<system>/<rom>.gif/png`
3. Fallback: same image as USB-DMD (auto-scaled)

---

## Display Modes

### ATTRACT mode *(game started, no credit yet)*
| DMD | Display |
|-----|---------|
| USB-DMD | alternates every 4s: game marquee ↔ `insert-coin.gif` |
| WiFi-DMD | shows: game marquee (128×32 if available) |

### CREDIT mode *(coin inserted)*
| DMD | Display |
|-----|---------|
| USB-DMD | `press1or2.gif` |
| WiFi-DMD | `press1or2_128x32.gif` (fallback: `press1or2.gif`) |

Previous state (ATTRACT/PLAYING) is saved and restored after credit.

### PLAYING mode *(start pressed, credit available)*
| DMD | Display |
|-----|---------|
| USB-DMD | game marquee (static) |
| WiFi-DMD | alternates every 4s: `now-playing-128x32.gif` ↔ game marquee |

---

## State Machine

```
ATTRACT ──[Coin]──► CREDIT ──[Start, Credits > 0]──► PLAYING
   ▲                  │                                  │
   └──[Start,         │                                  │
   Credits=0,         │ [Start, Credits=0,               │
   was ATTRACT]       │  was ATTRACT]                    │
                      └──────────────────────────────────┘

PLAYING ──[Coin]──► CREDIT ──[Start, Credits > 0]──► PLAYING
                      │
                      │ [Start, Credits=0, was PLAYING]
                      └──────────────────────────────► PLAYING
```

`/tmp/zedmd_prev_mode` stores the state before entering CREDIT.

---

## Controller Mapping

| Button | Action |
|--------|--------|
| `BTN_BASE3` | Insert coin (Credit +1) |
| `BTN_BASE4` / `BTN_START` | Start game (Credit −1 → PLAYING); no credit → back to ATTRACT or PLAYING |

---

## Runtime Temp Files

| File | Purpose |
|------|---------|
| `/tmp/zedmd_lock` | Prevents multiple script instances |
| `/tmp/zedmd_mode` | Current state (ATTRACT / CREDIT / PLAYING) |
| `/tmp/zedmd_prev_mode` | State before last CREDIT |
| `/tmp/zedmd_path` | Marquee path — USB-DMD |
| `/tmp/zedmd_path_128` | Marquee path — WiFi-DMD (128×32) |
| `/tmp/zedmd_credit_log` | Credits (one line = 1 credit) |

All files are automatically deleted on gameStop.

---

## Dependencies

| Tool | Purpose |
|------|---------|
| `dmd-play` | DMD playback tool |
| `dmdserver` | DMD server (runs twice: USB + WiFi) |
| `evtest` | Joystick event reading |
| `udevadm` | Device detection |
| `nc` | Port check (netcat) |
| `wget` + `jq` | Batocera API queries (marquee paths) |

---

## Troubleshooting

### Duplicate WiFi dmdserver instance
**Cause:** `postshare.sh` and `dmd_real2` service both try to start the server.
**Fix:** `postshare.sh` uses the same PID file as the service → second start is skipped automatically.

### gameStop doesn't clean up `/tmp/zedmd_*`
**Cause:** `pkill -9 -f zedmd_marquee.sh` killed the script before `rm` could run.
**Fix:** Always clean up first, then kill — or use `/tmp/zedmd_lock` to prevent multiple starts.

### Multiple script instances running simultaneously
**Cause:** Batocera calls scripts multiple times (e.g. ROM variants).
**Fix:** Lock mechanism via `/tmp/zedmd_lock`.

### WiFi-DMD shows wrong image while game is running
**Cause:** `game-selected.sh` and `zedmd_marquee.sh` sending simultaneously.
**Fix:** `game-selected.sh` checks `/tmp/zedmd_mode` → skips WiFi-DMD if game is running.

### No marquee for games without a custom DMD file
**Fix:** `find_marquee_path()` also searches Batocera's standard media paths:
`/userdata/roms/<system>/images/<rom>-marquee.png`

### WiFi-DMD freezes (GIF ends, no loop)
**Cause:** `--once` flag in `dmd-play` → GIF plays only once.
**Fix:** Remove `--once` → GIF loops continuously.

---

## Logging

```bash
# Boot log
cat /userdata/system/dmd.log

# Running processes
ps aux | grep dmdserver | grep -v grep
ps aux | grep dmd-play  | grep -v grep
ps aux | grep zedmd     | grep -v grep

# Both dmdserver instances differ only by argument:
#   /usr/bin/dmdserver                            ← USB-DMD  (port 6789, dmd_real service)
#   /usr/bin/dmdserver -c .../config_real2.ini    ← WiFi-DMD (port 9001, postshare.sh)
#
# Check only the WiFi instance:
pgrep -a -f config_real2.ini

# Current state
cat /tmp/zedmd_mode
cat /tmp/zedmd_path
cat /tmp/zedmd_credit_log

# Check port 9001
nc -z localhost 9001 && echo "OK" || echo "ERROR"

# Manual WiFi-DMD test
dmd-play -p 9001 -f /userdata/system/dmd/screensaver.gif

# Reset temp files
rm -f /tmp/zedmd_*
```

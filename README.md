# The Arcade — powered by ZeDMD (WiFi Fork, 128×32, ESP32-S3-N16R8)

> 🇩🇪 **Deutschsprachige Anleitung:** [LIESMICH.md](LIESMICH.md)

**The Arcade** is an ESP32-S3 firmware for a 128×32 HUB75 LED matrix. It turns a pinball DMD panel into a standalone smart display with:

- **Clock + weather** — live data from your own weather station via MQTT (WeeWx), with Open-Meteo as automatic fallback; configurable field names, visual fallback indicator
- **GIF screensaver** — animated GIFs from SD card, sorted alphabetically or shuffled, with optional synchronised MP3 audio per GIF
- **Webradio** — internet radio via I2S amplifier (MAX98357A); station logos, track title, equaliser, L/R swap
- **Web UI** — full configuration and control from any browser on your local network; no app, no account, no cloud

---

> 📝 *This README is a personal project diary rather than a complete guide. It documents what worked for me — your setup may differ. No claim to completeness; errors and omissions are possible.*

---

> **This started as a personal hobby fork of [PPUC/ZeDMD](https://github.com/PPUC/ZeDMD) v5.1.8**
> and has since grown well beyond it — the clock, weather, and webradio features are original
> work built on top, not part of upstream ZeDMD. What *is* inherited from upstream is the core
> DMD protocol handling and the HUB75 display driver — that foundation is why this project
> stays under the same GPLv2-or-later license as ZeDMD itself, and why it remains listed as a
> GitHub fork of it rather than a standalone repo.
> It is shared with the community in the hope that it might be useful — but it comes with **absolutely no support, no warranty, and no guarantee of any kind**.
> Issues and pull requests may not be responded to. Emails and messages regarding this project will likely go unanswered — not out of disrespect, but simply because this is a spare-time project maintained by one person.
> **Use entirely at your own risk.**

---

> ⚠️ **Panel size: only tested with 128×32 (2× chained 64×32 tiles).** The codebase already
> has build environments for larger panels (`256x64` / `S3-N16R8_256x64`, via `ZEDMD_HD`) and
> the core DMD rendering scales automatically with `TOTAL_WIDTH`/`TOTAL_HEIGHT` — but this has
> **not been built or tested**. The weather overlay and GIF screensaver have hardcoded pixel
> positions sized for 128×32 that would need adapting first.
>
> If you do try a larger panel: pay **significantly more attention to power supply sizing**
> than you would for 128×32. A single 64×32 HUB75 tile can draw up to ~25W (5V/5A) at full
> white; going to 256×64 roughly **quadruples the pixel count** versus this project's default
> 128×32 setup, and current draw scales accordingly. Undersized power supplies on large HUB75
> chains cause voltage drop, ghosting/dimming artifacts, and in the worst case connector or
> wiring damage — plan for adequate power injection points along the chain, not just a single
> feed at one corner.

---

https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/raw/main/docs/images/ZeDMD_WiFi_128x32_demo.mp4

---

### Screenshots

| Clock + Weather | Weather Forecast | GIF Screensaver |
|:-:|:-:|:-:|
| ![Clock](docs/images/IMG_5506.jpeg) | ![Forecast](docs/images/IMG_5482.jpeg) | ![GIF](docs/images/IMG_5505.jpeg) |

**Webradio — station name and logo on the display:**

![Webradio](docs/images/IMG_5504.jpeg)

**Internals / Back panel:**

| Inside | Back panel |
|:-:|:-:|
| ![Internals](docs/images/Innenleben.jpeg) | ![Back panel](docs/images/Rückseite.jpeg) |

---

## 🆕 What's new in this release

### v1.5.3 *(this release)*

#### Configurable MQTT field names
The JSON field names used to read weather data from your MQTT broker are now configurable in the admin panel — no reflash required. This matters if your WeeWx (or other weather station) uses non-standard field names. Default values match the standard WeeWx LOOP output: `outTemp_C`, `outHumidity`, `windSpeed_kph`, `barometer_mbar`. Settings are saved to LittleFS and survive reboots.

#### Visual fallback indicator for OpenMeteo data
When MQTT has been silent for more than 10 minutes, the weather display switches to Open-Meteo data as a fallback. All four weather values (temperature, humidity, wind, pressure) are now shown in **grey** instead of white during this fallback period — an immediate visual cue that the data is not coming from your local weather station. After 30 minutes without MQTT, the existing orange **"MQTT fehlt"** text appears as before.

#### Unknown weather code fallback icon
If Open-Meteo returns a WMO weather code that has no matching icon, the display now shows the **question mark icon** (`question.rgba` from the icon set) instead of the misleading cloudy icon that was used before. In practice this should never appear — all codes used by Open-Meteo are handled — but it makes the system more robust.

---

### v1.5.2

#### Tabbed Web UI
The main page has been reorganised into three tabs — **Screensaver**, **Display** and **Radio** — making it much easier to navigate on both desktop and mobile. SD card & storage info plus the Admin button remain always visible above the tabs.

#### Display Timer *(scheduled display off)*
Set a daily on/off schedule for the LED matrix — useful for automatically turning the display off at night. Configure From/Until times under the **Display** tab. A **"Display off / Display on"** button lets you instantly turn the display off and on without changing the timer settings.

#### Stereo / Mono toggle
The **Radio** tab now shows separate **Stereo** and **Mono** buttons next to the volume slider. Switch the audio output to mono without a firmware rebuild — useful if you only have one speaker connected or experience phase issues.

#### Station logos on the LED matrix
When a radio station starts playing, the matching station logo (if uploaded) is shown on the LED matrix alongside the station name and track title.

#### Boot text improvements
Status messages during boot ("booting", "loading...", "SD OK", "LittleFS OK") are now shown at the bottom-left of the display so they no longer overlap the boot logo.

#### Rescan buttons
A **Rescan** button appears in the screensaver file list and the GIF audio file list after an upload — or can be triggered manually at any time. Rescans the SD card and LittleFS without a reboot and updates the file list immediately.

#### SD card SPI speed — up to 40 MHz
On a proper PCB layout (no Dupont wires), the SD card is now initialised at 40 MHz first, with an automatic fallback chain (40 → 25 → 20 → 8 MHz). Previously the maximum was 8 MHz. Faster initialisation and file access, especially noticeable during GIF and audio scans.

#### Equalizer
A three-band **equaliser (Bass / Mid / Treble)** is now available in the **Radio** tab. Settings are saved to LittleFS and survive reboots and station switches.

#### Weather icons — further improvements
The 8×8 forecast icons (3-day view) have been completely redrawn. Three new icons added: drizzle, showers and snow now have their own symbols — previously they shared a generic rain icon. Total: 11 distinct weather icons.

#### L/R channel swap
If your left and right speakers are physically wired the wrong way around, a single **L/R Swap** button in the **Radio** tab swaps the audio channels in software — no rewiring needed. Settings survive reboots.

#### Custom branding
The boot screen and web UI header now show the **The Arcade** logo instead of the generic ZeDMD logo. The PPUC splash screen has been removed.

---

### v1.5.1

#### Weather Icons — native 16×16 pixel art
All 8 weather icons have been redrawn as native 16×16 pixel art — sharper and more detailed than the scaled-up 8×8 bitmaps used before. Each icon has a unique hand-crafted color gradient: sun with rays, crescent moon, cloud, rain drops, snow, lightning bolt, and partly-cloudy variants. The smaller 8×8 icons used in the 3-day forecast were further improved in v1.5.2.

#### SD Card — more robust initialisation
The SD card now retries with progressively slower SPI speeds (8 → 4 → 2 MHz, up to 6 attempts) before giving up. Previously a single failed mount attempt would mark the SD as unavailable for the entire session. This resolves intermittent boot failures with marginally connected or slow-to-mount SD cards.

#### Bug Fixes
- Emoji text: cursor position 0 was treated as falsy in JavaScript — the cursor is now correctly preserved when inserting an emoji at the very start of the input field
- Icon upload: interrupted uploads no longer leave orphaned `.tmp` files on LittleFS
- Icon folder scan: directory handle no longer leaks when a non-directory entry is encountered

---

### v1.5 and earlier

### Webradio
Stream internet radio stations directly through the ZeDMD's built-in speaker.
Requires a **MAX98357A I2S amplifier module** — see wiring below.

- Dedicated station manager at `/radio.html` — search stations via [radio-browser.info](https://www.radio-browser.info), save presets with logo icons
- **Country quick-select buttons** (🇩🇪 DE / 🇦🇹 AT / 🇨🇭 CH / 🇳🇱 NL / 🇫🇷 FR) load the top-100 stations for that country in one tap; Niedersachsen regional filter still available
- Station name, track title and station logo displayed in the web UI
- Volume control via slider on the main page and the radio page
- Station name and track title scroll on the LED matrix for 5 seconds when a station starts, then returns to the screensaver — tap "DMD 10s" to show it again
- Presets survive firmware updates (stored in LittleFS)
- Stable station switching — no more audio dropout on channel change
- Stream URLs from radio-browser.info are automatically normalised (removes `?ti=` playlist hints that caused the audio library to hang)
- **L/R channel swap** — if your left and right speakers are physically wired the wrong way around, a single button in the **Radio** tab swaps the audio channels in software — no rewiring needed

> **Technical note — audio library patch:** The L/R swap feature required modifying the `ESP32-audioI2S` library internally, because it does not provide this function out of the box. Rather than changing the library files by hand (which would be lost on every library update), a build-time patch script (`scripts/patch_audio_lib.py`) injects the change automatically on each compile. This script was developed with the help of **[Claude Code](https://claude.ai/code)** (AI coding assistant by Anthropic) — pinpointing the exact location inside the audio pipeline where samples could be swapped safely, and making the patch idempotent (safe to run repeatedly). I would not have attempted this alone.

> **⚠️ Weather API:** Open-Meteo is accessed via HTTP instead of HTTPS. TLS handshakes consistently caused memory-related crashes in the web radio build. Since Open-Meteo provides public data without requiring a login, HTTPS is not necessary here.

### GIF Preview in the browser
Click any GIF filename in the screensaver file list or the "currently shown" field to open a live animated preview directly in the browser — without touching the display. Favourite, ignore and play controls are available inside the preview.

> **Note:** Opening a GIF preview while webradio is playing may cause brief audio stuttering — SD card access and audio streaming share the same CPU core. This is a known limitation.

### GIF Audio
Play a matching MP3 file from the SD card in sync with an animated GIF screensaver. The filename must match the GIF (e.g. `demo.gif` → `demo.mp3`). Upload audio files via the main page or drop them in `/GifAudio/` on the SD card directly.

- File list is cached in LittleFS — after the first boot scan, subsequent reboots load instantly (same mechanism as the screensaver file cache)
- Paginated file list in the web UI with previous/next buttons (20 files per page)
- Scan can be aborted via the **"Cancel Scan"** button that appears after an upload

> ⚠️ **First-boot scan time:** Depending on how many files are in `/GifAudio/`, the initial scan over SPI can take a very long time. Progress is shown on the display ("GifAudio XXXX"). After the scan the list is cached in LittleFS — every subsequent reboot loads in seconds.

> **Note:** GIF audio plays once per GIF cycle — looping is not yet implemented.

### SDMMC Board Support *(not actively maintained)*
Added support for boards with an **onboard SD card via SDMMC interface** (1-bit mode) — no external SPI module required. See pin table below for the required HUB75 cable changes.

> **Note:** This build variant is not actively maintained. Development focus is on the standard SPI SD build (`wifi_sd_webradio`). The SDMMC variant may work but is not tested with each release.

### GIF Screensaver Order
- **A-Z toggle** — switch between alphabetical and random playback order directly on the main page
- **Reshuffle button** — re-randomise the GIF order at any time without a full reload

### Configurable Timezone
The NTP clock timezone can now be set directly on the admin page — no firmware rebuild required.
A dropdown covers the most common zones (Central Europe, UK, UTC, US East/Central/Mountain/Pacific); a **Custom** option accepts any POSIX timezone string for full flexibility. The setting is stored in LittleFS and survives reboots.

### SD Card Mount / Eject
The main page now shows a **Mount / Eject** toggle button next to the SD card status bar.
- **⏏ Eject** (grey) — safely unmounts the SD card so it can be removed while the device stays powered
- **⏻ Mount** (green) — remounts the card after reinsertion; updates the screensaver file list and GIF audio cache automatically

### Stability & Bug Fixes
This release includes a comprehensive overhaul of memory management and task safety:

- **Admin page no longer crashes** — root cause identified via coredump analysis: lwIP assertion in AsyncTCP 3.3.5 triggered by the Basic Auth handshake. Fixed by updating AsyncTCP to 3.4.10
- Fixed race condition on station switching — audio no longer reverts to the previous station
- Weather data (MQTT + HTTP) now safely synchronized between CPU cores
- SD card directory listing moved out of network callbacks — no more audio stuttering during web access
- Weather HTTP response buffered in PSRAM — eliminates large internal SRAM spike during fetch
- All file uploads are now atomic (`.tmp` + rename) — interrupted uploads no longer leave corrupt files
- `screensaverFiles` array protected by mutex against concurrent web access
- Audio task priority raised above the web server task — reduces stream dropouts under concurrent web traffic

---

## 🧪 Experimental Features

> These features are functional but still being polished. Use at your own risk and expect rough edges.

---

### Batocera WiFi streaming

By default, Batocera's DMD server only streams to a single USB-connected DMD. To stream to a WiFi ZeDMD at the same time, a second `dmdserver` instance needs to be set up manually.

A full step-by-step guide — including dual-DMD marquee control, attract/playing modes, and troubleshooting — is available here:

📄 **[docs/batocera-dual-dmd.md](docs/batocera-dual-dmd.md)** (EN) | **[docs/batocera-dual-dmd-DE.md](docs/batocera-dual-dmd-DE.md)** (DE)

> ⚠️ Tested with Batocera **v42**. Batocera **>v42** may have introduced changes that break this setup. Verify carefully before updating Batocera.

---

### Batocera game start/stop trigger

`scripts/batocera_game_start.sh` and `scripts/batocera_game_stop.sh` trigger GIF audio playback on ZeDMD when a game starts or stops on Batocera. The DMD itself continues to receive live frames from the emulator as usual — these scripts only control the audio layer.

> ⚠️ Not yet fully tested end-to-end — the `/gif_audio_play` endpoint works, but the Batocera-side trigger has not been verified in a real game session.

**How it works:**
- On game start: Batocera sends the ROM name to ZeDMD → ZeDMD plays the matching MP3 from `/GifAudio/` on the SD card
- On game stop: ZeDMD stops audio playback
- Naming convention: ROM filename without extension → `medieval_madness.zip` → `medieval_madness.mp3`

**Setup:**
1. Open both scripts and set `IP_ZEDMD` to your ZeDMD's fixed IP address
2. Copy to Batocera:
   ```bash
   scp scripts/batocera_game_start.sh root@batocera.local:/userdata/system/scripts/gameStart.sh
   scp scripts/batocera_game_stop.sh root@batocera.local:/userdata/system/scripts/gameStop.sh
   ```
3. Place matching MP3 files on the SD card under `/GifAudio/`

> ⚠️ **If a `gameStart.sh` already exists on Batocera** (e.g. from another project), do **not** replace it — append the `curl` call from the script to the existing file instead.

---

### Batocera audio extraction script

`scripts/extract_gif_audio.sh` extracts the first N seconds of audio from Batocera scraped game videos and saves them as MP3 files ready for ZeDMD.

> ⚠️ Tested on Batocera only. Requires `ffmpeg` and `python3` available on the Batocera system.

**Prerequisites:**
- Batocera with SSH access
- `ffmpeg` installed on Batocera (check: `which ffmpeg`)
- `python3` installed on Batocera (check: `which python3`)
- Scraped game videos in `gamelist.xml` (`/userdata/roms/<system>/`)

**Usage (run via SSH on Batocera):**
```bash
ssh root@batocera.local "bash /tmp/extract_gif_audio.sh [options]"
```

| Option | Default | Description |
|--------|---------|-------------|
| `--system` | `mame` | ROM system folder name |
| `--limit` | `10` | Max number of files to extract |
| `--duration` | `15` | Clip length in seconds |
| `--out` | `/userdata/zedmd/gif_audio` | Output directory |
| `--game` | *(all)* | Filter by partial game name |

**Workflow:**
1. Copy the script to Batocera: `scp scripts/extract_gif_audio.sh root@batocera.local:/tmp/`
2. Run it via SSH (see above)
3. Open the output folder in Finder: **Network → batocera → share → zedmd → gif_audio**
4. Upload the MP3 files via **`http://<ZeDMD-IP>/`** → GIF-Audio section

---

### Display Text

Send a custom text message to the LED matrix directly from the web UI.

- Static or scrolling display — short texts are shown centered, longer texts scroll automatically
- Color picker for free RGB color selection
- Configurable display duration (5–60 seconds)
- Instantly interrupts any running screensaver or GIF and restores it afterwards
- **Emoji picker** — 34 emojis (❤️ ⭐ 🔥 😊 🎉 and more) rendered as pixel-art icons directly in the scrolling text; icons are stored as RGBA files in LittleFS (`/icons/`)

---

### Stereo Audio

Stereo output using **two MAX98357A modules** — one for the left channel, one for the right.

The SD pin is a voltage-level channel-select strap. The values below were measured and confirmed to work with MAX98357A breakout boards that already have a **1 MΩ resistor from SD to Vin** onboard. If your board has a different onboard resistor, these values will not apply — always check your board's schematic and measure before connecting.

* **Module L (Left Channel):** Connect a **100 kΩ** resistor from SD to VCC **(5V)**.
* **Module R (Right Channel):** Connect a **390 kΩ** resistor from SD to VCC **(5V)**.

The ESP32-audioI2S library outputs stereo I2S natively when playing stereo source files. Each module automatically decodes its designated channel based on the SD pin voltage.

| MAX98357A Pin | ESP32-S3 | Notes |
|---------------|----------|-------|
| BCLK | **GPIO 9** | shared — both modules |
| LRC (WSEL) | **GPIO 14** | shared — both modules |
| DIN | **GPIO 21** | shared — both modules |
| SD — Module L | **100 kΩ to VCC** | → **Left channel** |
| SD — Module R | **390 kΩ to VCC** | → **Right channel** |
| VIN | **5V** *(recommended)* | each module separately |
| GND | **GND** | each module separately |

> ⚠️ The resistor values above are verified at **5V only**. For 3.3V supply the right-channel value changes to approx. **210 kΩ** (untested). 3.3V supply is **not recommended** — it increases distortion and loads the 3.3V rail.

> ⚠️ **DISCLAIMER:**
> These resistor values were determined experimentally with a specific MAX98357A breakout board variant that has a 1 MΩ onboard resistor from SD to Vin. Other board variants may require different values. **Always consult the MAX98357A datasheet, check your board's actual schematic, and measure voltages before connecting.**
> **Use entirely at your own risk — no warranty or liability of any kind.**

---

## 🔜 Planned Features

### Custom PCB by [elabree](https://github.com/elabree) ✅
A dedicated carrier board designed and built by [elabree](https://github.com/elabree) — tested and confirmed working with this firmware. The board hosts the ESP32-S3-DevKitC-1-N16R8, an SD card module socket, and two MAX98357A stereo amplifier sockets on a single clean PCB.

Two revisions are available:
- **Rev1.1** — for original Espressif ESP32-S3-DevKitC (0.9″ pin row spacing)
- **Rev1.2** — for most clone boards (1″ pin row spacing)

KiCad sources and Gerber files for ordering at JLCPCB: [KiCad/ZeDMD_WiFi/](https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/tree/main/KiCad/ZeDMD_WiFi) (included in this repository).

A big thank-you to elabree for designing and contributing this board to the project — this would not have happened without his work.

### Code cleanup *(on my list)*
The code has grown organically and is honestly a bit messy in places — I know. I'm planning to clean things up at some point, but no promises on when. It works, which counts for something.

---

## What's different from original ZeDMD

This fork is **WiFi-only** and targets the **ESP32-S3-N16R8** with a **128×32 LED matrix**.

### All added features

- **WiFi OTA firmware update** — flash new firmware directly via browser (`/admin.html`); firmware version with build ID and branch shown on admin page (format: `5.1.8-jb (date) [abc1234@main]`)
- **Screensaver** — GIF/RAW slideshow with clock and weather display (Open-Meteo, plain HTTP for RAM efficiency)
- **Screensaver management** — favorites, ignore list, alphabetical/random order, strict timer, pause/resume
- **GIF Preview** — click any filename to open an animated browser preview; favorite/ignore/play controls inside the preview
- **GIF Audio** — play a matching MP3 from `/GifAudio/` on the SD card in sync with the screensaver GIF; `scripts/extract_gif_audio.sh` extracts audio from Batocera video files
- **Improved web interface** — file management, per-file favorite/ignore buttons, pagination with wrap-around
- **Admin page** — WiFi, display, transport, MQTT, weather settings
- **Webradio** — internet radio via I2S amplifier (MAX98357A); station search via [radio-browser.info](https://www.radio-browser.info); preset management with logo icons; LED matrix shows station info for 5 s on start, "DMD 10s" button for on-demand display
- **Config Export/Import** — full configuration backup and restore via browser (`/config_transfer.html`)
- **Display Text** — send custom text with emojis to the LED matrix via the web UI; static or scrolling with color selection and configurable duration (5–60 s); 34 emojis available via built-in picker
- **Display Timer** — schedule daily on/off times for the LED matrix (e.g. off at 23:00, on at 07:00); "Display off / Display on" button for instant manual control
- **Tabbed web UI** — main page organised into Screensaver / Display / Radio tabs; SD card & admin always visible
- **Stereo Audio** *(experimental)* — two MAX98357A modules for true stereo output; channel selection via SD-pin resistor strapping (5V only, values verified); Stereo/Mono toggle in the web UI

---

## Hardware — ESP32-S3-N16R8 Note

### 💡 Power supply

The ZeDMD can draw a noticeable amount of current — especially when bright GIFs are displayed, webradio is streaming, and WiFi is active all at once. A laptop USB port or a basic phone charger may not deliver enough stable power for this, which can lead to unexpected reboots or an unstable display.

**If things seem unreliable: use a decent 5V / 2A USB power adapter** (the kind that comes with a good phone or tablet). How much power is actually needed varies quite a bit depending on the content — dark GIFs with no audio use much less than a bright screensaver at full volume.

---

### 🧠 On the verge of a nervous breakdown — what this little chip is actually doing

At any given moment, the ESP32-S3 in this build is simultaneously:

- driving a 128×32 HUB75 LED matrix over DMA
- serving a full web UI with live updates over WiFi
- streaming and decoding webradio (MP3/AAC) over I2S in a dedicated FreeRTOS task
- reading and playing animated GIFs from an SD card
- playing matching MP3 files in sync with those GIFs
- fetching live weather data and rendering icons
- handling MQTT, OTA, file uploads, and a paginated file browser for 2 000+ files
- maintaining a LittleFS cache, crash logs, and boot diagnostics

All of this on a chip with **327 KB of internal SRAM** — of which the WiFi stack, TCP server, and audio buffers claim most at runtime, leaving sometimes less than **1 KB free** at peak load. The 8 MB PSRAM handles the large caches and pixel buffers, but anything touching WiFi or TCP must live in internal SRAM — and that is genuinely tight.

The firmware works hard to keep things stable: PSRAM-backed caches, careful buffer sizing, staggered HTTP requests, and a heap monitor that logs free memory every 30 seconds. But this is a device running at the edge of what its hardware class was designed for — and it shows, in the best possible way.

---

### ⚠️ Missing 5V on VIN pin (IN-OUT solder bridge)

Some ESP32-S3-N16R8 boards do not provide 5V on the VIN pin out of the box.
If your ZeDMD powers up but the LED matrix stays dark or behaves unexpectedly, check the **IN-OUT solder bridge** near the 5V/GND pins.

**Fix:** Close the IN-OUT bridge with a small solder blob to route 5V from USB to the VIN pin.

![IN-OUT bridge location](docs/images/IMG_4405.jpg)
![IN-OUT close-up](docs/images/image.png)

> ⚠️ Modifying hardware is at your own risk!

For other known hardware issues and general ZeDMD documentation see the **[original ZeDMD README](https://github.com/PPUC/ZeDMD#readme)**.

---

## SD Card

The screensaver GIF/RAW files can be stored on a microSD card connected via SPI.

**Tested module:**
[Micro SD Card Module SPI (Amazon)](https://www.amazon.de/dp/B0D8Q8N7NQ)

![SD Card Module](docs/images/sd_module.jpg)

**Wiring ESP32-S3-N16R8:**

| SD Module | ESP32-S3 Pin |
|-----------|-------------|
| VCC       | **3.3V** or **5V** ¹ |
| GND       | GND         |
| MISO      | GPIO 13     |
| MOSI      | GPIO 11     |
| SCK       | GPIO 12     |
| CS        | GPIO 10     |

> ¹ Most common SPI SD modules accept both 3.3V and 5V on VCC (they have an onboard regulator).
> Check your module's datasheet. If in doubt, use **3.3V** — the ESP32-S3 GPIO pins are **not** 5V-tolerant.

**Format:** FAT32, files in subfolders (e.g. `/MyGIFs/`). GIF and RAW files supported.

**First-boot scan performance**

> ⚠️ The first scan of a large GIF library can take a very long time — a folder with ~5 400 files takes approximately **35 minutes** over SPI. This is a fundamental SPI/FAT32 limitation: the Arduino SD library opens every file individually to read its name, and FAT32 directories with many entries cannot be indexed.
>
> **After the first scan the file list is cached in LittleFS** (`sc_*.bin`). Every subsequent reboot loads the cache in seconds — no SD scan needed.
>
> **Tips to keep scan times acceptable:**
> - Use **multiple smaller subfolders** instead of one large one (e.g. 5 × 1 000 files instead of 1 × 5 000) — each folder is scanned independently and cached separately.
> - Use a **dedicated upload folder** (small, freshly uploaded files only) — the web UI's "Upload folder" picker lets you target it specifically without touching the large library folder.
> - A planned future optimisation is an **SD-side index file** (`.index` per folder) that replaces directory traversal with a single sequential read — reducing scan time from minutes to under a second regardless of file count.

---

## Web Interface

Access via `http://<IP>/` (main page) and `http://<IP>/admin.html` (admin page).

### Admin page settings

| Section | Description |
|---------|-------------|
| **Firmware Update (OTA)** | Flash new firmware via browser — no USB needed |
| **WiFi** | SSID, password, port |
| **Display** | RGB order, scaling mode, brightness |
| **Transport** | USB / WiFi UDP / TCP / SPI, UDP delay, USB packet size |
| **Panel** | Clock phase, I2S speed, latch blanking, refresh rate, driver |
| **MQTT** | Server IP and port for weather integration |
| **Weather (Open-Meteo)** | Latitude/longitude for local weather display |
| **Update Web Files** | Upload index.html / admin.html without full filesystem flash |
| **Config Export/Import** | Full config backup/restore via `/config_transfer.html` |
| **Information** | Firmware version, build date and git hash, debug info |

---

## Pin Assignment ESP32-S3-N16R8

### HUB75 LED Matrix

| Signal | GPIO | Note |
|--------|------|------|
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
| E | 1 | not used - only for 256×64 |
| OE | 2 | |
| LAT | 40 | `wifi_sd_webradio` |
| LAT | **46** | `wifi_sdmmc_webradio` — rewire! |
| CLK | 41 | `wifi_sd_webradio` |
| CLK | **17** | `wifi_sdmmc_webradio` — rewire! |

> HUB75 LAT/CLK only need to be moved on the **SDMMC board**, because GPIO 40/41 are used internally for the SD card there.

### SD Card

| Signal | GPIO | Build |
|--------|------|-------|
| CS   | 10 | `wifi_sd_webradio` (SPI) |
| MOSI | 11 | `wifi_sd_webradio` (SPI) |
| SCK  | 12 | `wifi_sd_webradio` (SPI) |
| MISO | 13 | `wifi_sd_webradio` (SPI) |
| DATA | 40 | `wifi_sdmmc_webradio` (SDMMC onboard) |
| CLK  | 39 | `wifi_sdmmc_webradio` (SDMMC onboard) |
| CMD  | 38 | `wifi_sdmmc_webradio` (SDMMC onboard) |

### Buttons

| Function | GPIO |
|----------|------|
| UP       | 0  |
| DOWN     | 45 |
| FORWARD  | 48 |
| BACKWARD | 47 |

### Webradio (optional)

Requires an **I2S amplifier module** and a small speaker.

![MAX98357A module](docs/images/max98357a.jpg)

**Tested amplifier: MAX98357A**
- Breakout module (e.g. Adafruit #3006 or common clones)
- Speaker: **8 Ω / 3 W** — e.g. **Visaton FSR 7** (77 mm, good sound for the size)
- Output power: up to 3 W — adequate for a cabinet at moderate volume


### MAX98357A Wiring — Mono (SD and SDMMC builds)

| MAX98357A Pin | ESP32-S3 | Notes |
|---------------|----------|-------|
| VIN           | **5V**       | 5V gives more headroom; 3.3V works but lower volume |
| GND           | GND          | |
| BCLK          | **GPIO 9**   | I2S Bit Clock |
| LRC (WSEL)    | **GPIO 14**  | I2S Word Select (L/R) |
| DIN           | **GPIO 21**  | I2S Data |
| GAIN          | **GND**      | GND = 15 dB gain (max); floating = 12 dB; 3.3V = 9 dB |
| SD (Shutdown) | 3.3V or floating | Floating = on; GND = mute |

> Class-D mono amplifier. Connect speaker to OUT+/OUT− only — **not** to GND (differential/BTL output).

### Build / Environment Overview

| Setup | Environment | Extra hardware |
|-------|-------------|----------------|
| Existing board + SPI SD module | `S3-N16R8_128x32_wifi_sd_webradio` | Connect MAX98357A |
| SDMMC board (onboard SD) | `S3-N16R8_128x32_wifi_sdmmc_webradio` | Connect MAX98357A + rewire HUB75 LAT/CLK |

### Webradio — Configuration

Open `http://<IP>/` after flashing — radio controls appear directly on the main page:

| Control | Description |
|---------|-------------|
| **▶ / ■** | Play last preset / Stop |
| **Station buttons** | Start a saved preset directly |
| **Volume** | Slider on the main page and on `/radio.html` |
| **DMD 10s** | Show station info on the LED matrix for 10 seconds; auto-hides after 5 s when a station starts |

Full preset management at **`http://<IP>/radio.html`**.
Presets are stored in LittleFS (`/radio_presets.json`) and survive firmware updates.
Config transfer between boards: `http://<IP>/config_transfer.html`

---

## Weather (Open-Meteo)

Weather and 3-day forecast fetched from [Open-Meteo](https://open-meteo.com/) — free, no API key required.
Set your coordinates in `admin.html` → **Weather (Open-Meteo)**.

### ⚠️ Why HTTP, not HTTPS

The audio codec (MP3/AAC) occupies ~50 KB of internal SRAM at runtime. A TLS handshake needs an additional ~30–40 KB of contiguous internal SRAM — a guaranteed out-of-memory crash on the webradio build. Since Open-Meteo serves fully public weather data with no authentication tokens, plain HTTP is completely safe here.

> `http://api.open-meteo.com/v1/forecast` — explicitly supported by Open-Meteo for embedded/IoT devices.

---

## Installation

### First flash (USB, one time only)

The first flash must include the LittleFS filesystem image — otherwise the web interface won't load after boot. Use the merged image approach:

```bash
pio run                              # build firmware
pio run -t buildfs                   # build LittleFS image from data/
python3 scripts/merge_firmware.py    # create merged flash image
```

Then flash the resulting `*_merged.bin` from `~/Desktop/Firmwares/`:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* --baud 460800 \
  write_flash --no-compress 0x0 "ZeDMD_..._merged.bin"
```

> ⚠️ **`--no-compress` is required.** The LittleFS partition occupies 6.5 MB of flash. Without `--no-compress`, esptool compresses the transfer — this causes a timeout on the large partition and the ESP hangs mid-flash. Always use `--no-compress` for the full merged image.

### After first boot

The web interface loads immediately. Upload updated HTML files if needed:
**`http://<IP>/admin.html`** → "Update Web Files"

### Future firmware updates (OTA, no USB needed)

Browser → **`http://<IP>/admin.html`** → "Firmware Update (OTA)"

---

## Credits

- **[Markus Kalkbrenner / PPUC](https://github.com/PPUC/ZeDMD)** — original ZeDMD project
- **[elabree](https://github.com/elabree)** — PCB design: carrier board for DevKit + SD + MAX98357A ([KiCad files](https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/tree/main/KiCad))
- **Niels (My Son)** — coding assistance & inspiration & moral support
- **[Claude Sonnet](https://anthropic.com)** — coding assistance

---

## Commercial Use

This project is licensed under GPL v2 — commercial use is permitted under those terms.

---

## License

Same as the original project — see [LICENSE](LICENSE).

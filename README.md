# ZeDMD 5.1.8 — WiFi Fork (128×32, ESP32-S3-N16R8)

---

> 🇩🇪 **Deutschsprachige Anleitung:** [LIESMICH.md](LIESMICH.md)

---

> 📝 *This README is a personal project diary rather than a complete guide. It documents what worked for me — your setup may differ. No claim to completeness; errors and omissions are possible.*

---

> **This is a personal hobby fork of [PPUC/ZeDMD](https://github.com/PPUC/ZeDMD) v5.1.8.**
> It is shared with the community in the hope that it might be useful — but it comes with **absolutely no support, no warranty, and no guarantee of any kind**.
> Issues and pull requests may not be responded to. Emails and messages regarding this project will likely go unanswered — not out of disrespect, but simply because this is a spare-time project maintained by one person.
> **Use entirely at your own risk.**

---

https://github.com/jens-b/ZeDMD-5.1.8-WIFI-128x32/raw/main/docs/images/ZeDMD_WiFi_128x32_demo.mp4

---

### Screenshots

| Clock + Weather | Weather Forecast | GIF Screensaver |
|:-:|:-:|:-:|
| ![Clock](docs/images/IMG_5032.jpeg) | ![Forecast](docs/images/IMG_5033.jpeg) | ![GIF](docs/images/IMG_5034.jpeg) |

**Internals:**

![Internals](docs/images/IMG_5077.jpeg)

---

## 🆕 What's new in this release

### Webradio
Stream internet radio stations directly through the ZeDMD's built-in speaker.
Requires a **MAX98357A I2S amplifier module** — see wiring below.

- Dedicated station manager at `/radio.html` — search stations via [radio-browser.info](https://www.radio-browser.info), save presets with logo icons
- Station name, track title and station logo displayed in the web UI
- Volume control via slider on the main page and the radio page
- Station name and track title scroll on the LED matrix for 5 seconds when a station starts, then returns to the screensaver — tap "DMD 10s" to show it again
- Presets survive firmware updates (stored in LittleFS)
- Stable station switching — no more audio dropout on channel change
- Stream URLs from radio-browser.info are automatically normalised (removes `?ti=` playlist hints that caused the audio library to hang)

> **⚠️ Weather API:** Open-Meteo is accessed via HTTP instead of HTTPS. TLS handshakes consistently caused memory-related crashes in the web radio build. Since Open-Meteo provides public data without requiring a login, HTTPS is not necessary here.

### GIF Preview in the browser
Click any GIF filename in the screensaver file list or the "currently shown" field to open a live animated preview directly in the browser — without touching the display. Favourite, ignore and play controls are available inside the preview.

> **Note:** Opening a GIF preview while webradio is playing may cause brief audio stuttering — SD card access and audio streaming share the same CPU core. This is a known limitation.

### GIF Audio
Play a matching MP3 file from the SD card in sync with an animated GIF screensaver. The filename must match the GIF (e.g. `demo.gif` → `demo.mp3`). Upload audio files via the main page or drop them in `/GifAudio/` on the SD card directly.

- File list is cached in LittleFS — after the first boot scan, subsequent reboots load instantly (same mechanism as the screensaver file cache)
- Paginated file list in the web UI with previous/next buttons (20 files per page)
- Scan can be aborted via the **"Scan abbrechen"** button that appears after an upload

> **Note:** GIF audio plays once per GIF cycle — looping is not yet implemented.

### Batocera game start/stop trigger *(experimental)*

`scripts/batocera_game_start.sh` and `scripts/batocera_game_stop.sh` trigger GIF audio playback on ZeDMD when a game starts or stops on Batocera. The DMD itself continues to receive live frames from the emulator as usual — these scripts only control the audio layer.

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

### Batocera WiFi streaming *(experimental)*

By default, Batocera's DMD server only streams to a single USB-connected DMD. To stream to a WiFi ZeDMD at the same time, a second `dmdserver` instance needs to be set up manually.

Setup guide for running a second `dmdserver` instance alongside the default one, so both DMDs receive frames simultaneously (EN):

📄 **[docs/batocera-dual-dmd.md](docs/batocera-dual-dmd.md)**

📄 **[docs/batocera-dual-dmd-DE.md](docs/batocera-dual-dmd-DE.md)** (DE)

> ⚠️ Tested with Batocera **v42**. Batocera **>v42** may have introduced changes that break this setup. Verify carefully before updating Batocera.

---

### Batocera audio extraction script *(experimental)*

`scripts/extract_gif_audio.sh` extracts the first N seconds of audio from Batocera scraped game videos and saves them as MP3 files ready for ZeDMD.

> ⚠️ **Experimental** — tested on Batocera only. Requires `ffmpeg` and `python3` available on the Batocera system.

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

### SDMMC Board Support
Added support for boards with an **onboard SD card via SDMMC interface** (1-bit mode) — no external SPI module required. See pin table below for the required HUB75 cable changes.

### Stability & Bug Fixes
This release includes a comprehensive overhaul of memory management and task safety:

- Fixed race condition on station switching — audio no longer reverts to the previous station
- Weather data (MQTT + HTTP) now safely synchronized between CPU cores
- SD card directory listing moved out of network callbacks — no more audio stuttering during web access
- Weather HTTP response buffered in PSRAM — eliminates large internal SRAM spike during fetch
- All file uploads are now atomic (`.tmp` + rename) — interrupted uploads no longer leave corrupt files
- `screensaverFiles` array protected by mutex against concurrent web access

---

## 🔜 Planned Features

### Stereo Audio *(tested on SDMMC build)*
Stereo output using **two MAX98357A modules** — one for the left channel, one for the right. Tested with the SDMMC build and two MAX98357A breakout boards.

The SD pin is a voltage-level channel-select strap. The values below were measured and confirmed to work with MAX98357A breakout boards that already have a **1 MΩ resistor from SD to Vin** onboard. If your board has a different onboard resistor, these values will not apply — always check your board's schematic and measure before connecting.

* **Module L (Left Channel):** Connect a **100 kΩ** resistor from SD to VCC (3.3V or 5V).
* **Module R (Right Channel):** Connect a **370 kΩ** resistor from SD to VCC (3.3V or 5V).

The ESP32-audioI2S library outputs stereo I2S natively when playing stereo source files. Each module automatically decodes its designated channel based on the SD pin voltage.

| MAX98357A Pin | ESP32-S3 | Notes |
|---------------|----------|-------|
| BCLK | **GPIO 9** | shared — both modules |
| LRC (WSEL) | **GPIO 14** | shared — both modules |
| DIN | **GPIO 21** | shared — both modules |
| SD — Module L | **100 kΩ to VCC** | → **Left channel** |
| SD — Module R | **370 kΩ to VCC** | → **Right channel** |
| VIN | **5V** or **3.3V** | each module separately |
| GND | **GND** | each module separately |

> ⚠️ **DISCLAIMER:**
> These resistor values were determined experimentally with a specific MAX98357A breakout board variant that has a 1 MΩ onboard resistor from SD to Vin. Other board variants may require different values. **Always consult the MAX98357A datasheet, check your board's actual schematic, and measure voltages before connecting.**
> **Use entirely at your own risk — no warranty or liability of any kind.**

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

---

## Hardware — ESP32-S3-N16R8 Note

### 💡 Power supply

The ZeDMD can draw a noticeable amount of current — especially when bright GIFs are displayed, webradio is streaming, and WiFi is active all at once. A laptop USB port or a basic phone charger may not deliver enough stable power for this, which can lead to unexpected reboots or an unstable display.

**If things seem unreliable: use a decent 5V / 2A USB power adapter** (the kind that comes with a good phone or tablet). How much power is actually needed varies quite a bit depending on the content — dark GIFs with no audio use much less than a bright screensaver at full volume.

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

1. **First flash** (USB, one time only): PlatformIO → Upload (`S3-N16R8_128x32_wifi_sd_webradio`)
2. **Future firmware updates**: Browser → `http://<IP>/admin.html` → "Firmware Update (OTA)"
3. **Web interface updates**: Browser → `http://<IP>/admin.html` → "Update Web Files"

---

## Credits

- **[Markus Kalkbrenner / PPUC](https://github.com/PPUC/ZeDMD)** — original ZeDMD project
- **Niels (My Son)** — coding assistance & inspiration & moral support
- **[Claude Sonnet](https://anthropic.com)** — coding assistance

---

## Commercial Use

This project is licensed under GPL v2 — commercial use is permitted under those terms.
However, if you use this fork in a hardware product — whether commercial or a personal PCB build you're proud of — I'd love to receive **2 samples** as a thank-you: one for me, one for my son Niels. Not a legal requirement, just a friendly ask from a fellow hobbyist. 😊

---

## License

Same as the original project — see [LICENSE](LICENSE).

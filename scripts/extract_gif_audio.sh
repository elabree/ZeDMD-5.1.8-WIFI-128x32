#!/bin/bash
# extract_gif_audio.sh
#
# Extracts audio clips from Batocera scraped game videos.
# Runs directly on Batocera via SSH:
#   ssh root@batocera.local "bash /path/to/extract_gif_audio.sh [options]"
#
# Usage:
#   extract_gif_audio.sh [--system mame] [--limit 10] [--duration 15] [--out /tmp/gif_audio]
#
# Defaults: system=mame, limit=10, duration=15s, out=/tmp/gif_audio

SYSTEM="mame"
LIMIT=10
DURATION=15
OUT_DIR="/userdata/zedmd/gif_audio"
GAME_FILTER=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --system)   SYSTEM="$2";      shift 2 ;;
        --limit)    LIMIT="$2";       shift 2 ;;
        --duration) DURATION="$2";    shift 2 ;;
        --out)      OUT_DIR="$2";     shift 2 ;;
        --game)     GAME_FILTER="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

ROMS_DIR="/userdata/roms/$SYSTEM"
GAMELIST="$ROMS_DIR/gamelist.xml"

echo "================================================"
echo " ZeDMD GIF-Audio Extraktor"
echo "================================================"
echo " System   : $SYSTEM"
echo " Limit    : $LIMIT Dateien"
echo " Dauer    : ${DURATION}s pro Clip"
echo " Ausgabe  : $OUT_DIR"
echo " Filter   : ${GAME_FILTER:-"(alle)"}"
echo "================================================"

# Voraussetzungen prüfen
if [ ! -f "$GAMELIST" ]; then
    echo "[FEHLER] gamelist.xml nicht gefunden: $GAMELIST"
    echo "         Verfügbare Systeme:"
    ls /userdata/roms/ | grep -i mame | sed 's/^/           /'
    exit 1
fi

if ! command -v ffmpeg &> /dev/null; then
    echo "[FEHLER] ffmpeg nicht gefunden"
    exit 1
fi

mkdir -p "$OUT_DIR"

echo ""
echo "Verarbeite $GAMELIST ..."
echo ""

# Variablen via Umgebung übergeben — 'PYEOF' verhindert bash-Escape-Verarbeitung
GAMELIST="$GAMELIST" ROMS_DIR="$ROMS_DIR" OUT_DIR="$OUT_DIR" \
DURATION="$DURATION" LIMIT="$LIMIT" GAME_FILTER="$GAME_FILTER" python3 << 'PYEOF'
import xml.etree.ElementTree as ET
import os
import subprocess
import sys

gamelist    = os.environ['GAMELIST']
roms_dir    = os.environ['ROMS_DIR']
out_dir     = os.environ['OUT_DIR']
duration    = os.environ['DURATION']
limit       = int(os.environ['LIMIT'])
game_filter = os.environ.get('GAME_FILTER', '').lower()

try:
    tree = ET.parse(gamelist)
    root = tree.getroot()
except Exception as e:
    print(f"[FEHLER] XML-Parsing: {e}")
    sys.exit(1)

games = root.findall('game')
print(f"  Spiele in gamelist.xml : {len(games)}")

extracted = 0
skipped   = 0
errors    = 0
no_video  = 0

for game in games:
    if extracted >= limit:
        break

    path_el  = game.find('path')
    video_el = game.find('video')

    if path_el is None or not path_el.text:
        continue
    if video_el is None or not video_el.text:
        no_video += 1
        continue

    # ROM-Name bereinigen — identisch zu pixel_start.sh Logik
    rom_raw  = os.path.basename(path_el.text)
    rom_name = os.path.splitext(rom_raw)[0].replace('\\', '').strip()

    # Versteckte Dateien (.name) überspringen
    if not rom_name or rom_name.startswith('.'):
        continue

    # Spielname-Filter (Teilsuche, Groß-/Kleinschreibung egal)
    if game_filter and game_filter not in rom_name.lower():
        continue

    # Video-Pfad auflösen (relativ zu roms_dir)
    video_rel  = video_el.text.lstrip('.').lstrip('/').lstrip('\\')
    video_full = os.path.join(roms_dir, video_rel)

    if not os.path.exists(video_full):
        skipped += 1
        print(f"  [SKIP]    {rom_name:30s}  (Video nicht gefunden)")
        continue

    out_mp3 = os.path.join(out_dir, rom_name + ".mp3")

    if os.path.exists(out_mp3):
        print(f"  [EXISTS]  {rom_name:30s}  (bereits vorhanden)")
        extracted += 1
        continue

    print(f"  [EXTRACT] {rom_name:30s}  ...", end='', flush=True)

    result = subprocess.run([
        'ffmpeg', '-y',
        '-i', video_full,
        '-t', duration,       # erste N Sekunden
        '-q:a', '3',          # gute MP3-Qualität
        '-vn',                # kein Video
        '-ar', '44100',       # Sample-Rate (MAX98357A)
        out_mp3
    ], capture_output=True, text=True)

    if result.returncode == 0:
        size_kb = os.path.getsize(out_mp3) // 1024
        print(f" OK ({size_kb} KB)")
        extracted += 1
    else:
        print(f" FEHLER")
        # ffmpeg Fehler ausgeben für Debugging
        for line in result.stderr.split('\n')[-3:]:
            if line.strip():
                print(f"             {line.strip()}")
        errors += 1

print()
print(f"================================================")
print(f"  Extrahiert : {extracted}")
print(f"  Kein Video : {no_video}")
print(f"  Übersprungen: {skipped}")
print(f"  Fehler     : {errors}")
print(f"================================================")
print(f"  Dateien in : {out_dir}")
print()

PYEOF

echo ""
echo "Nächste Schritte:"
echo ""
echo "  1. MP3s im Batocera-Share auf dem Mac öffnen:"
echo "     Finder → Netzwerk → Batocera → share → zedmd → gif_audio"
echo ""
echo "  2. MP3s über ZeDMD-Webinterface hochladen:"
echo "     http://zedmd-wifi.local/ → GIF-Audio → MP3 hochladen"
echo ""


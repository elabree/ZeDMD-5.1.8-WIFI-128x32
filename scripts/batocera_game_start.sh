#!/bin/bash
# ZeDMD GIF-Audio Trigger — Spielstart
# Ablage auf Batocera: /userdata/system/scripts/gameStart.sh
# Parameter: $1=System, $2=voller ROM-Pfad
#
# MP3-Dateien müssen auf der SD-Karte unter /GifAudio/ liegen.
# Dateiname = ROM-Name ohne Extension + .mp3
# Beispiel: /userdata/roms/mame/medieval_madness.zip → medieval_madness.mp3

IP_ZEDMD="192.168.x.x"  # <-- feste IP des ZeDMD hier eintragen

# WICHTIG: Falls auf Batocera bereits ein gameStart.sh existiert (z.B. von
# RetroPixelLED-Lite oder einem anderen Projekt), diesen curl-Aufruf dort
# einfach anhängen — NICHT die bestehende Datei ersetzen!

ROM=$(basename -- "$2")
ROM="${ROM%.*}"
ROM=$(echo "$ROM" | sed 's/\\//g')

curl -s -X POST "http://$IP_ZEDMD/gif_audio_play" \
    --data-urlencode "file=${ROM}.mp3" > /dev/null &

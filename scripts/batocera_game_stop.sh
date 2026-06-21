#!/bin/bash
# ZeDMD GIF-Audio Trigger — Spielende
# Ablage auf Batocera: /userdata/system/scripts/gameStop.sh
# Parameter: $1=System, $2=voller ROM-Pfad (wird hier nicht benötigt)

IP_ZEDMD="192.168.x.x"  # <-- feste IP des ZeDMD hier eintragen

curl -s -X POST "http://$IP_ZEDMD/gif_audio_stop" > /dev/null &

#!/bin/bash
# upload_icons.sh — Alle RGBA-Icons nach ZeDMD LittleFS hochladen
# Aufruf: ./scripts/upload_icons.sh <IP-Adresse>
# Beispiel: ./scripts/upload_icons.sh 192.168.1.42

set -e

IP="${1}"
if [ -z "$IP" ]; then
  echo "Fehler: IP-Adresse fehlt."
  echo "Aufruf: $0 <IP-Adresse>"
  exit 1
fi

SCRIPT_DIR="$(dirname "$0")"
USER="admin"
PASS="zedmd1234"
TOTAL_OK=0
TOTAL_FAIL=0

upload_set() {
  local dir="$1"
  local endpoint="$2"
  local label="$3"
  local ok=0 fail=0
  echo "--- ${label} → ${endpoint} ---"
  for f in "${dir}"/*.rgba; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    printf "  %-22s → " "$name"
    HTTP=$(curl -s -o /dev/null -w "%{http_code}" \
      -u "${USER}:${PASS}" \
      -F "file=@${f};filename=${name}" \
      "http://${IP}/${endpoint}")
    if [ "$HTTP" = "200" ]; then
      echo "OK"
      ok=$((ok + 1))
    else
      echo "FEHLER (HTTP ${HTTP})"
      fail=$((fail + 1))
    fi
  done
  echo "  → ${ok} OK, ${fail} Fehler"
  echo ""
  TOTAL_OK=$((TOTAL_OK + ok))
  TOTAL_FAIL=$((TOTAL_FAIL + fail))
}

upload_set "${SCRIPT_DIR}/../data/icons"       "upload_icon"       "20×20 Icons (/icons/)"
upload_set "${SCRIPT_DIR}/../data/icons_small" "upload_icon_small" "10×10 Icons (/icons_small/)"

echo "Gesamt: ${TOTAL_OK} hochgeladen, ${TOTAL_FAIL} Fehler."

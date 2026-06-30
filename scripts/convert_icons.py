"""
convert_icons.py — GIF-Pixel-Art → RGBA-Binärdateien für ZeDMD LittleFS

Aufruf:
    python3 scripts/convert_icons.py

Eingabe:  ~/Downloads/zedmd-emoji-gifs/2x_groesser/*.gif  (20px hoch)
          ~/Downloads/zedmd-emoji-gifs/1x_original/*.gif  (10px hoch)
          ~/Downloads/zedmd-weather-pack/*.gif             (16×16, animiert)
Ausgabe:  data/icons/<name>.rgba       (20×20 × 4 Byte = 1600 Byte)
          data/icons_small/<name>.rgba (10×10 × 4 Byte =  400 Byte)

GIFs sind unterschiedlich breit aber immer genau canvas_h hoch.
Das Icon wird horizontal zentriert; Ränder bleiben transparent.
Weather-Pack GIFs sind 16×16 animiert — Frame 0 wird skaliert.
"""

import os
from PIL import Image

BASE_DIR      = os.path.dirname(os.path.dirname(__file__))
SRC_BASE      = os.path.expanduser("~/Downloads/zedmd-emoji-gifs")
WEATHER_DIR   = os.path.expanduser("~/Downloads/zedmd-weather-pack")

# Weather-Pack: GIF-Name → Icon-Name (überschreibt Wetter-Icons aus Emoji-Set)
WEATHER_MAP = {
    "clear-day":   "sun",
    "clear-night": "moon",
    "cloudy":      "cloud",
    "rain":        "rain",
    "snow":        "snow",
    "error":       "warning",
}

# DE-Dateiname → EN-Icon-Name (in LittleFS gespeichert)
NAME_MAP = {
    "alien":         "alien",
    "ausruf":        "exclaim",
    "bier":          "beer",
    "check":         "check",
    "cool":          "cool",
    "daumen-hoch":   "thumbsup",
    "daumen-runter": "thumbsdown",
    "denken":        "think",
    "feuer":         "fire",
    "frage":         "question",
    "gamepad":       "gamepad",
    "geld":          "money",
    "geschenk":      "gift",
    "glitzer":       "star_glow",
    "glocke":        "bell",
    "grinsen":       "grin",
    "herz":          "heart",
    "idee":          "bulb",
    "konfetti":      "party",
    "lachen":        "smile",
    "mega":          "speaker",
    "mond":          "moon",
    "notiz":         "memo",
    "phone":         "phone",
    "pizza":         "pizza",
    "regen":         "rain",
    "robot":         "robot",
    "schlaf":        "sleep",
    "schnee":        "snow",
    "smile":         "slight_smile",
    "sonne":         "sun",
    "stern":         "star",
    "warnung":       "warning",
    "weinen":        "cry",
    "wolke":         "cloud",
    "wuetend":       "angry",
    "x":             "cross",
}

SETS = [
    ("2x_groesser", "icons",       20),
    ("1x_original", "icons_small", 10),
]


def convert_gif(src_path: str, dst_path: str, canvas: int):
    img = Image.open(src_path).convert("RGBA")
    w, h = img.size
    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    x_off = (canvas - w) // 2
    y_off = (canvas - h) // 2
    out.paste(img, (x_off, y_off))
    raw = out.tobytes()
    assert len(raw) == canvas * canvas * 4
    with open(dst_path, "wb") as f:
        f.write(raw)


def convert_weather_gif(src_path: str, dst_path: str, target: int, frame: int = 0):
    img = Image.open(src_path)
    img.seek(frame)
    out = img.convert("RGBA").resize((target, target), Image.NEAREST)
    raw = out.tobytes()
    assert len(raw) == target * target * 4
    with open(dst_path, "wb") as f:
        f.write(raw)


for src_folder, dst_folder, canvas_px in SETS:
    src_dir = os.path.join(SRC_BASE, src_folder)
    dst_dir = os.path.join(BASE_DIR, "data", dst_folder)
    os.makedirs(dst_dir, exist_ok=True)
    ok = err = 0
    print(f"\n=== {src_folder} → {dst_folder}/ ({canvas_px}×{canvas_px}) ===")
    for de_name, en_name in sorted(NAME_MAP.items()):
        src = os.path.join(src_dir, f"{de_name}.gif")
        dst = os.path.join(dst_dir, f"{en_name}.rgba")
        if not os.path.exists(src):
            print(f"  FEHLT   {de_name}.gif")
            err += 1
            continue
        try:
            convert_gif(src, dst, canvas_px)
            print(f"  OK      {de_name}.gif → {en_name}.rgba")
            ok += 1
        except Exception as e:
            print(f"  FEHLER  {de_name}.gif: {e}")
            err += 1
    print(f"  → {ok} konvertiert, {err} Fehler, {ok*canvas_px*canvas_px*4/1024:.1f} KB")

# ── Weather-Pack ──────────────────────────────────────────────────────────────
print("\n=== zedmd-weather-pack → icons/ + icons_small/ (Frame 0, skaliert) ===")
ok = err = 0
for gif_name, icon_name in sorted(WEATHER_MAP.items()):
    src = os.path.join(WEATHER_DIR, f"{gif_name}.gif")
    if not os.path.exists(src):
        print(f"  FEHLT   {gif_name}.gif")
        err += 1
        continue
    try:
        dst20 = os.path.join(BASE_DIR, "data", "icons",       f"{icon_name}.rgba")
        dst10 = os.path.join(BASE_DIR, "data", "icons_small", f"{icon_name}.rgba")
        convert_weather_gif(src, dst20, 20)
        convert_weather_gif(src, dst10, 10)
        print(f"  OK      {gif_name}.gif → {icon_name}.rgba (20×20 + 10×10)")
        ok += 1
    except Exception as e:
        print(f"  FEHLER  {gif_name}.gif: {e}")
        err += 1
print(f"  → {ok} Icons konvertiert, {err} Fehler")

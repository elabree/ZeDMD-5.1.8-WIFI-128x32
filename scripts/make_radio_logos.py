#!/usr/bin/env python3
"""
Konvertiert Senderlogos aus DAB+ Logo-Service ZIPs, dem radioart-Ordner
(DAB+ Service-ID als Dateiname) und manuell bereitgestellten PNG-Dateien
in 32×32 RGBA-Rohdateien für das ZeDMD Radio-Icon-System.

Ausgabedateinamen: <slug>.rgba
Slug-Regel: Sendername lowercase, Nicht-Alphanumerisch → '_'

WICHTIG: Der radioart-Ordner enthält Dateien nach DAB+-Service-ID (hex, z.B. D382.bmp).
Das Webradio sendet KEINE Service-IDs — nur Stationsnamen im ICY-Header.
Deshalb brauchen wir eine Mapping-Datei (--map), die hex_id → Slug zuordnet.
Die Datei scripts/radioart_map.csv enthält die bekannten Zuordnungen.

Benutzung:
  pip3 install Pillow
  python3 scripts/make_radio_logos.py \
    --radioart-dir /Users/jens/Downloads/radioart \
    --map scripts/radioart_map.csv \
    --out /tmp/radio_logos

Dann per Browser-Upload auf das ZeDMD via /upload_icon_radio hochladen.
"""

import argparse
import io
import os
import re
import sys
import zipfile

try:
    from PIL import Image
except ImportError:
    print("Fehler: Pillow nicht installiert. Bitte: pip3 install Pillow", file=sys.stderr)
    sys.exit(1)

TARGET_SIZE = (32, 32)

# Manuelle Zusatz-Logos: Sendername (wird zum Slug) → Pfad zur PNG-Datei
# Hier NDR und andere Sender eintragen, die nicht im DAB-ZIP vorhanden sind.
# Beispiel:
#   "NDR 2":                "/Users/jens/Desktop/ndr2.png",
#   "NDR 1 Niedersachsen":  "/Users/jens/Desktop/ndr1.png",
EXTRA_FILES = {
    # "NDR 2":               "",
    # "NDR 1 Niedersachsen": "",
    # "NDR Info":            "",
    # "N-JOY":               "",
}


def load_map(map_path: str) -> dict[str, str]:
    """Liest radioart_map.csv: hex_id → slug."""
    result: dict[str, str] = {}
    if not map_path or not os.path.isfile(map_path):
        return result
    with open(map_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(',', 1)
            if len(parts) == 2:
                hex_id = parts[0].strip().upper()
                slug = parts[1].strip()
                if hex_id and slug:
                    result[hex_id] = slug
    print(f"  Mapping-Datei: {len(result)} Einträge geladen")
    return result


def slugify(name: str) -> str:
    slug = name.lower()
    slug = re.sub(r'[^a-z0-9]+', '_', slug)
    slug = slug.strip('_')
    return slug


def to_rgba_32x32(img: Image.Image) -> bytes:
    img = img.convert("RGBA")
    img = img.resize(TARGET_SIZE, Image.LANCZOS)
    return img.tobytes()


def best_128x128(names: list[str]) -> str | None:
    candidates = [n for n in names if re.search(r'128.?x.?128', n, re.IGNORECASE) and n.lower().endswith('.png')]
    if not candidates:
        candidates = [n for n in names if n.lower().endswith('.png')]
    return candidates[0] if candidates else None


def build_id_map(zip_paths: list[str]) -> dict[str, str]:
    """Baut eine Mapping-Tabelle: DAB-Service-ID (hex, 4 Zeichen) → Sendername (Slug)."""
    id_to_slug: dict[str, str] = {}
    for zip_path in zip_paths:
        if not os.path.isfile(zip_path):
            continue
        with zipfile.ZipFile(zip_path) as zf:
            for entry in zf.namelist():
                fname = entry.split('/')[-1]
                folder = entry.split('/')[-2] if entry.count('/') >= 2 else ''
                m = re.match(r'([0-9A-Fa-f]{4})_', fname)
                if m and folder:
                    hex_id = m.group(1).upper()
                    slug = slugify(folder)
                    if slug and hex_id not in id_to_slug:
                        id_to_slug[hex_id] = slug
    return id_to_slug


def process_radioart(radioart_dir: str, zip_paths: list[str], out_dir: str,
                     seen_slugs: set[str], manual_map: dict[str, str] | None = None) -> int:
    """Konvertiert BMP-Logos aus dem Radioart-Verzeichnis anhand der DAB-Service-ID.

    Zuordnung hex_id → slug: erst manual_map (--map), dann ZIP-basierte ID-Map.
    """
    if not radioart_dir or not os.path.isdir(radioart_dir):
        return 0

    # Kombiniere manuelle Map + ZIP-Map (manuelle Map hat Vorrang)
    id_map: dict[str, str] = {}
    if zip_paths:
        id_map.update(build_id_map(zip_paths))
    if manual_map:
        id_map.update(manual_map)  # überschreibt ZIP-Einträge bei Konflikt

    if not id_map:
        print("  Warnung: weder --map noch --zips angegeben — keine ID-Zuordnung möglich", file=sys.stderr)
        return 0

    processed = 0
    unmatched = 0
    print(f"\nRadioart-Verzeichnis {radioart_dir} ({len(id_map)} bekannte IDs) ...")
    for fname in sorted(os.listdir(radioart_dir)):
        if not fname.lower().endswith('.bmp'):
            continue
        hex_id = os.path.splitext(fname)[0].upper()
        slug = id_map.get(hex_id)
        if not slug:
            unmatched += 1
            continue
        if slug in seen_slugs:
            continue
        try:
            img = Image.open(os.path.join(radioart_dir, fname))
            rgba = to_rgba_32x32(img)
            out_path = os.path.join(out_dir, f"{slug}.rgba")
            with open(out_path, 'wb') as f:
                f.write(rgba)
            print(f"  {fname} ({hex_id}) → {slug}.rgba")
            seen_slugs.add(slug)
            processed += 1
        except Exception as e:
            print(f"  Fehler bei {fname}: {e}", file=sys.stderr)

    print(f"  {processed} Logos konvertiert, {unmatched} BMPs ohne bekannte ID übersprungen")
    return processed


def process_zips(zip_paths: list[str], out_dir: str, seen_slugs: set[str]) -> int:
    processed = 0

    for zip_path in zip_paths:
        if not os.path.isfile(zip_path):
            print(f"  Überspringe (nicht gefunden): {zip_path}", file=sys.stderr)
            continue
        print(f"\nVerarbeite {os.path.basename(zip_path)} ...")
        with zipfile.ZipFile(zip_path) as zf:
            by_folder: dict[str, list[str]] = {}
            for name in zf.namelist():
                parts = name.split('/')
                if len(parts) >= 3 and parts[-1]:
                    folder = parts[-2]
                    by_folder.setdefault(folder, []).append(name)

            for folder, entries in sorted(by_folder.items()):
                slug = slugify(folder)
                if not slug:
                    continue
                entry = best_128x128(entries)
                if not entry:
                    continue
                if slug in seen_slugs:
                    continue
                try:
                    data = zf.read(entry)
                    img = Image.open(io.BytesIO(data))
                    rgba = to_rgba_32x32(img)
                    out_path = os.path.join(out_dir, f"{slug}.rgba")
                    with open(out_path, 'wb') as f:
                        f.write(rgba)
                    print(f"  {folder:<30} → {slug}.rgba")
                    seen_slugs.add(slug)
                    processed += 1
                except Exception as e:
                    print(f"  Fehler bei {folder}: {e}", file=sys.stderr)

    return processed


def process_extra(extra: dict[str, str], out_dir: str, seen_slugs: set[str]) -> int:
    processed = 0
    for name, path in extra.items():
        if not path:
            continue
        slug = slugify(name)
        if not slug or slug in seen_slugs:
            continue
        if not os.path.isfile(path):
            print(f"  EXTRA nicht gefunden: {path}", file=sys.stderr)
            continue
        try:
            img = Image.open(path)
            rgba = to_rgba_32x32(img)
            out_path = os.path.join(out_dir, f"{slug}.rgba")
            with open(out_path, 'wb') as f:
                f.write(rgba)
            print(f"  EXTRA {name:<28} → {slug}.rgba")
            seen_slugs.add(slug)
            processed += 1
        except Exception as e:
            print(f"  EXTRA Fehler bei {name}: {e}", file=sys.stderr)
    return processed


def process_extra_dir(extra_dir: str, out_dir: str, seen_slugs: set[str]) -> int:
    if not extra_dir or not os.path.isdir(extra_dir):
        return 0
    processed = 0
    print(f"\nExtra-Verzeichnis {extra_dir} ...")
    for fname in sorted(os.listdir(extra_dir)):
        ext = os.path.splitext(fname)[1].lower()
        if ext not in ('.png', '.jpg', '.jpeg', '.bmp', '.gif'):
            continue
        name = os.path.splitext(fname)[0]
        slug = slugify(name)
        if not slug or slug in seen_slugs:
            continue
        try:
            img = Image.open(os.path.join(extra_dir, fname))
            rgba = to_rgba_32x32(img)
            out_path = os.path.join(out_dir, f"{slug}.rgba")
            with open(out_path, 'wb') as f:
                f.write(rgba)
            print(f"  {fname:<35} → {slug}.rgba")
            seen_slugs.add(slug)
            processed += 1
        except Exception as e:
            print(f"  Fehler bei {fname}: {e}", file=sys.stderr)
    return processed


def main():
    parser = argparse.ArgumentParser(description="DAB-Logos → 32×32 RGBA für ZeDMD")
    parser.add_argument('--zip-dir', metavar='DIR',
                        help="Verzeichnis mit dab_logos_*.zip Dateien")
    parser.add_argument('--zips', nargs='+', metavar='ZIP',
                        help="Explizite ZIP-Dateipfade")
    parser.add_argument('--radioart-dir', metavar='DIR',
                        help="Verzeichnis mit BMP-Logos (Dateinamen = DAB-Service-IDs wie 'D389.bmp')")
    parser.add_argument('--map', metavar='CSV',
                        help="Mapping-Datei hex_id→slug (z.B. scripts/radioart_map.csv); "
                             "Vorrang vor ZIP-basierter ID-Zuordnung")
    parser.add_argument('--extra-dir', metavar='DIR',
                        help="Verzeichnis mit benannten PNG/JPG-Logos (Dateiname = Sendername)")
    parser.add_argument('--out', metavar='DIR', default='/tmp/radio_logos',
                        help="Ausgabeverzeichnis (Standard: /tmp/radio_logos)")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    zip_paths = list(args.zips or [])
    if args.zip_dir:
        for fname in sorted(os.listdir(args.zip_dir)):
            if fname.lower().endswith('.zip'):
                zip_paths.append(os.path.join(args.zip_dir, fname))

    manual_map = load_map(args.map) if args.map else {}

    seen_slugs: set[str] = set()
    total = 0

    # Radioart-BMPs zuerst (bevorzugte Quelle, wenn angegeben)
    if args.radioart_dir:
        total += process_radioart(args.radioart_dir, zip_paths, args.out, seen_slugs, manual_map)

    # ZIP-PNGs für alle noch nicht vorhandenen Sender
    if zip_paths:
        total += process_zips(zip_paths, args.out, seen_slugs)

    total += process_extra(EXTRA_FILES, args.out, seen_slugs)
    total += process_extra_dir(args.extra_dir, args.out, seen_slugs)

    print(f"\n{total} Logos erstellt in {args.out}/")
    print("Upload-Befehl (Beispiel für ein Logo):")
    print(f"  curl -u admin:zedmd1234 -X POST http://<ZeDMD-IP>/upload_icon_radio \\")
    print(f"    -F 'file=@{args.out}/antenne_nds.rgba'")


if __name__ == '__main__':
    main()

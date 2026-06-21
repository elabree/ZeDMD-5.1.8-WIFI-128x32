Import("env")
import os, shutil, subprocess

try:
    _git_hash = subprocess.check_output(
        ["git", "-C", env.subst("$PROJECT_DIR"), "rev-parse", "--short", "HEAD"],
        stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    _git_hash = "unknown"

try:
    _git_branch = subprocess.check_output(
        ["git", "-C", env.subst("$PROJECT_DIR"), "rev-parse", "--abbrev-ref", "HEAD"],
        stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    _git_branch = "unknown"

env.Append(CPPDEFINES=[
    ("GIT_HASH",   '\\"' + _git_hash   + '\\"'),
    ("GIT_BRANCH", '\\"' + _git_branch + '\\"'),
])

# Wenn sich der Hash seit dem letzten Build geändert hat, main.cpp antouchen
# damit SCons es neu kompiliert und das neue CPPDEFINE einbettet.
_stamp = os.path.join(env.subst("$BUILD_DIR"), ".git_hash_stamp")
_last = ""
try:
    with open(_stamp) as f:
        _last = f.read().strip()
except Exception:
    pass

if _last != _git_hash:
    _main = os.path.join(env.subst("$PROJECT_DIR"), "src", "main.cpp")
    if os.path.exists(_main):
        os.utime(_main, None)
    os.makedirs(os.path.dirname(_stamp), exist_ok=True)
    with open(_stamp, "w") as f:
        f.write(_git_hash)
    print(f"copy_firmware: Hash geändert ({_last or 'neu'} → {_git_hash}), main.cpp wird neu kompiliert")

# Kein automatischer Copy mehr — verhindert Doppelkopien bei Test-Builds vor dem Commit.
# Firmware manuell kopieren nach: 1) Commit, 2) pio run (zweiter Build mit richtigem Hash):
#
#   HASH=$(git rev-parse --short HEAD)
#   BRANCH=$(git rev-parse --abbrev-ref HEAD | sed 's|/|-|g')
#   cp .pio/build/S3-N16R8_128x32_wifi_sd_webradio/firmware.bin \
#      ~/Desktop/Firmwares/ZeDMD_5.1.8-jb_S3-N16R8_128x32_wifi_sd_webradio_${BRANCH}_${HASH}.bin

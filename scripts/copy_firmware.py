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

def copy_firmware(source, target, env):
    firmware_src = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
    if not os.path.exists(firmware_src):
        print("copy_firmware: firmware.bin nicht gefunden, überspringe.")
        return

    dest_dir = os.path.expanduser("~/Desktop/Firmwares")
    os.makedirs(dest_dir, exist_ok=True)

    project_dir = env.subst("$PROJECT_DIR")
    try:
        git_hash = subprocess.check_output(
            ["git", "-C", project_dir, "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        git_hash = "unknown"

    env_name = env.subst("$PIOENV")
    named = os.path.join(dest_dir, f"ZeDMD_5.1.8-jb_{env_name}_{git_hash}.bin")

    # Guard: überspringen wenn Zieldatei bereits aktuell
    if os.path.exists(named) and os.path.getmtime(named) >= os.path.getmtime(firmware_src):
        print(f"copy_firmware: {os.path.basename(named)} bereits aktuell, überspringe.")
        return

    shutil.copy2(firmware_src, named)
    print(f"copy_firmware: {os.path.basename(named)} → ~/Desktop/Firmwares/")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)

Import("env")
import os, shutil, subprocess

# Git-Hash vor dem Compile als CPPDEFINE setzen
try:
    _git_hash = subprocess.check_output(
        ["git", "-C", env.subst("$PROJECT_DIR"), "rev-parse", "--short", "HEAD"],
        stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    _git_hash = "unknown"

env.Append(CPPDEFINES=[("GIT_HASH", '\\"' + _git_hash + '\\"')])

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

    shutil.copy2(firmware_src, named)
    print(f"copy_firmware: {os.path.basename(named)} → ~/Desktop/Firmwares/")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)

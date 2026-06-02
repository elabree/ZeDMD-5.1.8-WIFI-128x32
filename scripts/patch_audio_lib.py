Import("env")
import os, shutil, subprocess

# ── 1. ESP32-audioI2S 2.3.0: min()-Typkonflikt mit GCC14 patchen ─────────────
audio_cpp = os.path.join(
    env["PROJECT_LIBDEPS_DIR"],
    env["PIOENV"],
    "ESP32-audioI2S",
    "src",
    "Audio.cpp"
)

if os.path.exists(audio_cpp):
    with open(audio_cpp, "r") as f:
        src = f.read()
    patched = src.replace(
        "availableBytes = min(availableBytes, InBuff.writeSpace());",
        "availableBytes = min(availableBytes, (uint32_t)InBuff.writeSpace());"
    )
    if patched != src:
        with open(audio_cpp, "w") as f:
            f.write(patched)
        print("patch_audio_lib: Audio.cpp min()-Cast gepatcht.")

# ── 2. Firmware nach Build nach ~/Desktop/Firmwares/ kopieren ─────────────────
def copy_firmware(source, target, env):
    firmware_src = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
    if not os.path.exists(firmware_src):
        return

    dest_dir = os.path.expanduser("~/Desktop/Firmwares")
    os.makedirs(dest_dir, exist_ok=True)

    try:
        git_hash = subprocess.check_output(
            ["git", "-C", env.subst("$PROJECT_DIR"), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        git_hash = "unknown"

    env_name = env.subst("$PIOENV")
    named = os.path.join(dest_dir, f"ZeDMD_5.1.8-jb_{env_name}_{git_hash}.bin")

    shutil.copy2(firmware_src, named)
    print(f"copy_firmware: {os.path.basename(named)} → ~/Desktop/Firmwares/")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)

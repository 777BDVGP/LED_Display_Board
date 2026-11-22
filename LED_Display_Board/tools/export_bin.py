# PlatformIO post-build script (same idea as the RainGauge project). After
# every build it writes to firmware_out/:
#
#   LED_Display_Board_v<ver>.bin       the program only - for updates over WiFi
#                                      (ABOUT page: UPLOAD & INSTALL, or GitHub INSTALL)
#   LED_Display_Board_v<ver>_full.bin  bootloader + partition table + program in ONE
#                                      file, for the FIRST flash of a new ESP32 at
#                                      address 0x0 - with a browser, no PlatformIO
#
# A build with src/wifi_secrets.h has your own WiFi networks + passwords inside:
# BOTH files are then named ..._PRIVATE.bin - never publish them.
# The file system is not included: the firmware formats it on the first boot.
Import("env")
import os, re, shutil, subprocess

def fw_version(proj):
    p = os.path.join(proj, "src", "fw_update.h")
    if os.path.exists(p):
        m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', open(p, encoding="utf-8").read())
        if m:
            return m.group(1)
    return "unknown"

def export_bin(source, target, env):
    proj = env.subst("$PROJECT_DIR")
    ver  = fw_version(proj)
    out_dir = os.path.join(proj, "firmware_out")
    os.makedirs(out_dir, exist_ok=True)

    app = str(target[0])
    private = os.path.exists(os.path.join(proj, "src", "wifi_secrets.h"))
    tag = "_PRIVATE" if private else ""
    upd = os.path.join(out_dir, "LED_Display_Board_v%s%s.bin" % (ver, tag))
    shutil.copyfile(app, upd)
    print("Firmware for updates:   %s" % upd)

    full = os.path.join(out_dir, "LED_Display_Board_v%s_full%s.bin" % (ver, tag))
    parts = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):       # bootloader, partitions, ota data
        parts += [env.subst(offset), env.subst(image)]
    parts += [env.subst("$ESP32_APP_OFFSET"), app]
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    cmd = [env.subst("$PYTHONEXE"), esptool, "--chip", env.BoardConfig().get("build.mcu", "esp32"),
           "merge_bin", "-o", full,
           "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"] + parts
    if subprocess.call(cmd) == 0:
        print("Firmware for a NEW board (browser flash at 0x0): %s" % full)
        if private:
            print("  NOTE: built with src/wifi_secrets.h - your own WiFi passwords are inside, do not share it")
    else:
        print("WARNING: could not create %s" % full)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", export_bin)

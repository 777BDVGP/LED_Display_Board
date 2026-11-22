# sync_from_private.py - copy the firmware source from the private project
# (../LED_Display_Board_IDF) into this public copy (LED_Display_Board/),
# WITHOUT the private WiFi / NTFY file, then check that no private WiFi name,
# password or NTFY topic slipped in.
#
#   python sync_from_private.py
#
# Copied: src/ (all except wifi_secrets.h), CMakeLists.txt, platformio.ini,
#         partitions_led.csv, sdkconfig.esp32dev, tools/export_bin.py.
# The check reads the private src/wifi_secrets.h (only on this PC) and refuses
# when any string from it occurs anywhere in the public copy.
import os, re, shutil, sys

HERE    = os.path.dirname(os.path.abspath(__file__))
PRIVATE = os.path.join(os.path.dirname(HERE), "LED_Display_Board_IDF")
PUBLIC  = os.path.join(HERE, "LED_Display_Board")
SKIP    = {"wifi_secrets.h"}
TOP_FILES = ["CMakeLists.txt", "platformio.ini", "partitions_led.csv", "sdkconfig.esp32dev"]

def copy_tree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for name in sorted(os.listdir(src)):
        if name in SKIP:
            print("  skipped (private): %s" % name)
            continue
        s = os.path.join(src, name)
        if os.path.isfile(s):
            shutil.copyfile(s, os.path.join(dst, name))

if not os.path.isdir(PRIVATE):
    sys.exit("Private project not found: %s" % PRIVATE)

print("Copying %s -> %s" % (PRIVATE, PUBLIC))
os.makedirs(PUBLIC, exist_ok=True)
copy_tree(os.path.join(PRIVATE, "src"), os.path.join(PUBLIC, "src"))
for f in TOP_FILES:
    shutil.copyfile(os.path.join(PRIVATE, f), os.path.join(PUBLIC, f))
os.makedirs(os.path.join(PUBLIC, "tools"), exist_ok=True)
shutil.copyfile(os.path.join(PRIVATE, "tools", "export_bin.py"), os.path.join(PUBLIC, "tools", "export_bin.py"))

# -- Secret check --------------------------------------------------------------
secrets = []
sec_file = os.path.join(PRIVATE, "src", "wifi_secrets.h")
if os.path.exists(sec_file):
    text = open(sec_file, encoding="utf-8", errors="replace").read()
    text = re.sub(r"//[^\n]*", "", text)                     # ignore comments
    for s in re.findall(r'"([^"]{4,})"', text):              # SSIDs, passwords, topics
        secrets.append(s)
else:
    print("NOTE - no private wifi_secrets.h found, nothing to check against")

bad = []
for root, dirs, files in os.walk(HERE):
    dirs[:] = [d for d in dirs if d not in (".git", ".pio", "firmware_out")]
    for f in files:
        if f == os.path.basename(__file__):
            continue
        p = os.path.join(root, f)
        data = open(p, encoding="utf-8", errors="replace").read()
        for s in secrets:
            if s in data:
                bad.append(os.path.relpath(p, HERE))
if os.path.exists(os.path.join(PUBLIC, "src", "wifi_secrets.h")):
    bad.append("LED_Display_Board/src/wifi_secrets.h")
if bad:
    print("STOP - private WiFi names / passwords / NTFY topics found in: %s" % ", ".join(sorted(set(bad))))
    sys.exit(1)
print("OK - %d private strings checked, none in the public copy" % len(secrets))

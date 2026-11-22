# make_enclosure_zips.py - rebuild the two enclosure download ZIPs from the
# files in Build_Files/, so they always match the drawings.
#
#   python make_enclosure_zips.py
#
# Writes Build_Files/Enclosure_Laser_DXF.zip       (all *.dxf - laser cutting, 3 mm acrylic)
#        Build_Files/Enclosure_3D_Print_STL.zip    (all *.stl - 3D printing)
# Run it after changing a DXF or STL file, before committing.
# The ZIPs are built with fixed file dates, so they only change when a
# drawing actually changed (no needless differences in git).
import glob, os, zipfile

HERE  = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "Build_Files")
FIXED = (2026, 1, 1, 0, 0, 0)

def make(zip_name, pattern):
    files = sorted(glob.glob(os.path.join(BUILD, pattern)))
    if not files:
        raise SystemExit("No %s files in %s" % (pattern, BUILD))
    path = os.path.join(BUILD, zip_name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            info = zipfile.ZipInfo(os.path.basename(f), FIXED)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(f, "rb") as fh:
                z.writestr(info, fh.read())
    print("%-28s %d files  %7d bytes" % (zip_name, len(files), os.path.getsize(path)))
    for f in files:
        print("    " + os.path.basename(f))

make("Enclosure_Laser_DXF.zip", "*.dxf")
make("Enclosure_3D_Print_STL.zip", "*.stl")

"""Downloads the night sky for the SkyArt commandlet (docs/design/look.md "Night sky").

NASA's Deep Star Maps 2020 (https://svs.gsfc.nasa.gov/4851): every Gaia DR2, Hipparcos and Tycho-2 star and the
Milky Way, in celestial coordinates (plate carree, 0h in the middle), as a linear HDR OpenEXR. Credit: NASA/Goddard
Space Flight Center Scientific Visualization Studio; Gaia DR2: ESA/Gaia/DPAC. Free to use with that credit.

    python tools/sky_textures.py

Puts starmap_2020_8k.exr (125 MB) in game/Content/ThirdParty/Sky/ (gitignored). Python's own TLS is used because
Windows 10's curl and PowerShell cannot open svs.gsfc.nasa.gov (it wants a newer TLS than schannel offers there).
"""
import pathlib
import shutil
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "game" / "Content" / "ThirdParty" / "Sky"
BASE = "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/"
FILES = ["starmap_2020_8k.exr"]


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        out = DEST / name
        if out.exists() and out.stat().st_size > 1_000_000:
            print("have", out)
            continue
        raw = ROOT / "assets" / "raw" / "sky" / name
        if raw.exists():
            shutil.copyfile(raw, out)
            print("copied", raw)
            continue
        tmp = out.with_suffix(".part")
        urllib.request.urlretrieve(BASE + name, tmp)
        tmp.replace(out)
        print("downloaded", out, out.stat().st_size // 1_000_000, "MB")


if __name__ == "__main__":
    main()

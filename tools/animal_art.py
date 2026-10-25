#!/usr/bin/env python3
"""A4 animal art: download the free (CC0) Quaternius Ultimate Animated Animals pack (a public Google Drive
folder) listed in assets/animal_art.json into game/Content/ThirdParty/Animals/. The AnimalArt commandlet then
imports the FBX files (assets/manifest.json).

  python tools/animal_art.py        needs `pip install gdown`; skips if the files are there
"""
import json, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "animal_art.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "Animals"


def main():
    spec = json.loads(LIST.read_text(encoding="utf-8"))
    if all((DEST / f"{m}.fbx").exists() for m in spec["models"]):
        return
    with tempfile.TemporaryDirectory() as tmp:
        url = "https://drive.google.com/drive/folders/" + spec["folderId"]
        subprocess.run([sys.executable, "-m", "gdown", "--folder", url, "-O", tmp], check=True)
        DEST.mkdir(parents=True, exist_ok=True)
        for m in spec["models"]:
            shutil.copy2(Path(tmp) / "FBX" / f"{m}.fbx", DEST / f"{m}.fbx")
            print("downloaded", m)


if __name__ == "__main__":
    main()

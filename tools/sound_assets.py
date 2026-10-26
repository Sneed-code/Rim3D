#!/usr/bin/env python3
"""V21 free sounds: download the packs in assets/sound_sources.json into
game/Content/ThirdParty/Sounds/<id>/ (not in Git) and write CREDITS.txt for the ones that need credit.

  python tools/sound_assets.py            download what is missing (run on the PC, needs internet)
  python tools/sound_assets.py --check    only check that every sound cue is mapped (CI, no network)
  python tools/sound_assets.py --list     print the cue to source table
"""
import json, re, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "sound_sources.json"
CUES = ROOT / "data" / "defs" / "SoundCues" / "Cues.json"
DEST = ROOT / "game" / "Content" / "ThirdParty" / "Sounds"
HEADERS = {"User-Agent": "Rim3D-sound-assets"}


def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=300) as r:
        return r.read()


def findLinks(page, pattern):
    html = fetch(page).decode("utf-8", "replace")
    return sorted(set(re.findall(pattern, html)))


def download(src):
    folder = DEST / src["id"]
    if folder.exists() and any(folder.iterdir()):
        return "have"
    how = src["download"]
    folder.mkdir(parents=True, exist_ok=True)
    if how == "manual":
        return "manual: see " + src["page"]
    if how.startswith("scrape:"):
        links = findLinks(how[7:], r'https://kenney\.nl/media/pages/assets/[^"\s]+\.zip')
        urls = links[:1]
    elif how.startswith("page:"):
        base = "https://opengameart.org"
        links = findLinks(how[5:], r'(?:https://opengameart\.org)?/sites/default/files/[^"\s]+\.(?:ogg|txt)')
        urls = [u if u.startswith("http") else base + u for u in links]
    else:
        urls = [how]
    if not urls:
        return "no link found, download by hand from " + src["page"]
    for u in urls:
        name = u.rsplit("/", 1)[-1].replace("%20", " ").replace("%28", "(").replace("%29", ")")
        (folder / name).write_bytes(fetch(u))
        if name.endswith(".zip"):
            import zipfile
            with zipfile.ZipFile(folder / name) as z:
                z.extractall(folder)
    return "downloaded %d file(s)" % len(urls)


def mapping(data):
    cues = [c["defName"] for c in json.loads(CUES.read_text())]
    rules = [(re.compile(m["cues"]), m) for m in data["cueMap"]]
    ids = {s["id"] for s in data["sources"]}
    problems = []
    for m in data["cueMap"]:
        for s in m["from"]:
            if s not in ids:
                problems.append("cueMap names unknown source " + s)
    for m in data["music"]:
        for s in m["from"]:
            if s not in ids:
                problems.append("music names unknown source " + s)
    table = {}
    for c in cues:
        hits = [m for r, m in rules if r.search(c)]
        if not hits:
            problems.append("cue not mapped: " + c)
        table[c] = hits[0] if hits else None
    return table, problems


def main():
    data = json.loads(LIST.read_text())
    table, problems = mapping(data)
    if "--list" in sys.argv:
        for c, m in table.items():
            print(c, "<-", ", ".join(m["from"]) if m else "-")
    if "--check" in sys.argv or "--list" in sys.argv:
        for p in problems:
            print("ERROR:", p)
        gaps = sum(1 for m in table.values() if m and "GAP" in m["pick"])
        print("%d cues, %d mapped, %d in known gaps" % (len(table), sum(1 for m in table.values() if m), gaps))
        return 1 if problems else 0
    credits = []
    for s in data["sources"]:
        try:
            print(s["id"], "-", download(s))
        except Exception as e:  # network or site change: report, go on
            print(s["id"], "- failed:", e)
        if s.get("attribution"):
            credits.append("%s (%s)" % (s["credit"], s["licence"]))
    DEST.mkdir(parents=True, exist_ok=True)
    (DEST / "CREDITS.txt").write_text("\n".join(credits) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

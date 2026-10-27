#!/usr/bin/env python3
"""V21 free sounds: download the packs in assets/sound_sources.json into
game/Content/ThirdParty/Sounds/<id>/ (not in Git), write CREDITS.txt for the ones that need credit,
then cut the files assets/sound_picks.json names into the cue and music files the game plays
(Sounds/cues/<Cue>/<n>.wav, Sounds/music/<mood>/<n>.wav; needs ffmpeg, and 7-Zip for .7z packs).

  python tools/sound_assets.py            download what is missing, then cut (run on the PC, needs internet)
  python tools/sound_assets.py --cut      only cut again (after changing sound_picks.json)
  python tools/sound_assets.py --check    only check that every sound cue is mapped (CI, no network)
  python tools/sound_assets.py --list     print the cue to source table
"""
import glob, json, re, shutil, subprocess, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "sound_sources.json"
PICKS = ROOT / "assets" / "sound_picks.json"
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
        elif name.endswith(".7z"):
            sevenZip = shutil.which("7z") or r"C:\Program Files\7-Zip\7z.exe"
            subprocess.run([sevenZip, "x", "-y", "-bd", "-o" + str(folder), str(folder / name)], check=True,
                           stdout=subprocess.DEVNULL)
            (folder / name).unlink()
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


def pickProblems(picks, cues):
    return ["sound_picks.json names unknown cue " + c for c in picks["cues"] if c not in cues]


def expand(patterns):
    files = []
    for pat in patterns:
        files += [h for h in sorted(glob.glob(str(DEST / pat))) if h not in files]
    return files


def peakDb(ffmpeg, src, start, chain):
    args = [ffmpeg, "-hide_banner", "-nostats"]
    if start:
        args += ["-ss", str(start)]
    args += ["-i", src, "-af", ",".join(chain + ["volumedetect"]), "-f", "null", "-"]
    out = subprocess.run(args, capture_output=True, text=True).stderr
    m = re.search(r"max_volume: (-?[0-9.]+) dB", out)
    return float(m.group(1)) if m else 0.0


def cutOne(ffmpeg, src, out, channels, pick, music):
    """One file to 16-bit 44.1 kHz WAV: cut, silence at the start trimmed, peak set to -1 dB (music: loudness)."""
    chain = []
    if pick.get("trimSilence"):
        chain.append("silenceremove=start_periods=1:start_threshold=-45dB")
    if pick.get("seconds"):
        chain.append("atrim=duration=%g" % pick["seconds"])
        chain.append("areverse,afade=t=in:d=0.04,areverse")  # no click where it is cut
    if pick.get("loop"):
        chain.append("afade=t=in:d=0.02,areverse,afade=t=in:d=0.02,areverse")
    if music:
        chain.append("loudnorm=I=-20:TP=-2")
    else:
        gain = -1.0 - peakDb(ffmpeg, src, pick.get("start"), chain) + float(pick.get("gain", 0.0))
        chain.append("volume=%.2fdB" % min(gain, 24.0))
    args = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"]
    if pick.get("start"):
        args += ["-ss", str(pick["start"])]
    args += ["-i", src, "-af", ",".join(chain), "-ac", str(channels), "-ar", "44100", "-c:a", "pcm_s16le", out]
    subprocess.run(args, check=True)


def cut():
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ffmpeg not found: install it (winget install Gyan.FFmpeg) and run with --cut")
        return 1
    picks = json.loads(PICKS.read_text())
    cues = {c["defName"]: c for c in json.loads(CUES.read_text())}
    for p in pickProblems(picks, cues):
        print("ERROR:", p)
    for folder in ("cues", "music"):
        if (DEST / folder).exists():
            shutil.rmtree(DEST / folder)
    made, missing = 0, []
    for name, pick in picks["cues"].items():
        files = expand(pick["files"])
        if not files or name not in cues:
            missing.append(name)
            continue
        out = DEST / "cues" / name
        out.mkdir(parents=True, exist_ok=True)
        channels = 2 if cues[name]["range"] == 0 else 1  # heard everywhere: stereo; with a place: mono
        pick = dict(pick, loop=pick.get("loop", cues[name]["loop"]))
        for i, f in enumerate(files[:8]):
            cutOne(ffmpeg, f, str(out / ("%d.wav" % i)), channels, pick, False)
            made += 1
    for mood, patterns in picks["music"].items():
        out = DEST / "music" / mood
        out.mkdir(parents=True, exist_ok=True)
        for i, f in enumerate(expand(patterns)):
            cutOne(ffmpeg, f, str(out / ("%d.wav" % i)), 2, {}, mood != "stinger")
            made += 1
    have = sum(1 for c in cues if (DEST / "cues" / c).exists())
    print("cut %d files: %d of %d cues have real sounds, the rest keep their tones" % (made, have, len(cues)))
    if missing:
        print("picked but no file found:", ", ".join(missing))
    return 0


def main():
    data = json.loads(LIST.read_text())
    table, problems = mapping(data)
    if PICKS.exists():
        problems += pickProblems(json.loads(PICKS.read_text()), table)
    if "--cut" in sys.argv:
        return cut()
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
    return cut()


if __name__ == "__main__":
    sys.exit(main())

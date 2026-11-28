#!/usr/bin/env python3
"""Downloads the real shield models listed in assets/shield_real.json (free, CC0) into assets/raw/<raw>/ (not in Git).
tools/shield_real.py (Blender) then writes each as game/Content/ThirdParty/Shields/<id>.glb over the Codex-made one.

  python tools/shield_download.py     download what is missing or changed

Each download leaves source.txt beside the files naming the URL, so a model whose URL changes in the list is fetched again.
"""
import io, json, urllib.request, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIST = ROOT / "assets" / "shield_real.json"
AGENT = {"User-Agent": "Rim3D-shield-download"}


def main():
    models = json.loads(LIST.read_text(encoding="utf-8"))["models"]
    failed = 0
    for m in models:
        dest = ROOT / "assets" / "raw" / m["raw"]
        stamp = dest / "source.txt"
        if stamp.exists() and stamp.read_text(encoding="utf-8").strip() == m["url"] and (dest / m["fbx"]).exists():
            print(f"{m['id']}: have {m['name']}")
            continue
        try:
            with urllib.request.urlopen(urllib.request.Request(m["url"], headers=AGENT), timeout=300) as r:
                data = r.read()
            dest.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for info in z.infolist():
                    target = (dest / info.filename).resolve()
                    if dest.resolve() not in target.parents and target != dest.resolve():
                        raise ValueError(f"unsafe path in zip: {info.filename}")
                    if info.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(z.read(info))
            stamp.write_text(m["url"] + "\n", encoding="utf-8")
            print(f"{m['id']}: downloaded {m['name']} ({len(data) // 1024} KB) into {dest}")
        except Exception as e:  # noqa: BLE001 - one bad download should not stop the rest
            failed += 1
            print(f"{m['id']}: could not download {m['url']}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

# Builds the character creator's base bodies (docs/design/makehuman.md, "Character creator"): each base is one MakeHuman
# character exported with a morph per slider. A morph is the difference between the base and a variant build of it with
# one setting changed (a macro slider, detail targets, horns or tail shrunk away), so any mix of sliders is a blend of
# morphs on one skinned mesh in the game.
#   python tools/makehuman/mh_creator.py [tools/makehuman/creator.json] [base name ...]
import copy
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BLENDER = os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe")


def merge(base, change):
    """The base config with a variant's changes: "macro" and "targets" are merged key by key, anything else replaced."""
    out = copy.deepcopy(base)
    for key, value in change.items():
        if key in ("macro", "targets") and isinstance(value, dict):
            out.setdefault(key, {})
            for k, v in value.items():
                if k == "race" and isinstance(v, dict):
                    out[key].setdefault("race", {}).update(v)
                else:
                    out[key][k] = v
        else:
            out[key] = copy.deepcopy(value)
    return out


def run(cfg, work, label):
    path = os.path.join(work, label + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=1)
    started = time.time()
    result = subprocess.run([BLENDER, "-b", "-noaudio", "--python", os.path.join(HERE, "mh_export.py"), "--", path],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = [l for l in result.stdout.splitlines() if l.startswith("[mh_export]") and ("morph" in l or "done" in l or "vertices" in l)]
    print(f"  {label}: exit {result.returncode}, {time.time() - started:.0f} s", *(["\n    " + l for l in lines]))
    if result.returncode != 0:
        print(result.stdout[-3000:], result.stderr[-3000:])
        raise SystemExit(f"{label} failed")


def main():
    args = sys.argv[1:]
    config = args[0] if args and args[0].endswith(".json") else os.path.join(HERE, "creator.json")
    only = [a for a in args if not a.endswith(".json")]
    spec = json.load(open(config, encoding="utf-8"))
    for base in spec["bases"]:
        if only and base["name"] not in only:
            continue
        print("base", base["name"])
        work = tempfile.mkdtemp(prefix="mh_creator_" + base["name"] + "_")
        cfg = copy.deepcopy(base["config"])
        cfg["out"] = f"game/Saved/MakeHuman/{base['name']}/{base['name']}.fbx"
        # Grown pubic hair: one layout, chosen on the base, so every variant has the same hairs (a first pass makes it).
        if cfg.get("pubic_grow"):
            cfg["pubic_layout"] = os.path.join(work, "pubic_layout.json")
            first = dict(cfg, dump_vertices=os.path.join(work, "_layout.npz"), skin_normal=False)
            first["out"] = os.path.join(work, "_layout", "_layout.fbx")
            run(first, work, "_layout")
        morphs = {}
        for name, change in base["morphs"].items():
            variant = merge(cfg, change)
            variant["out"] = os.path.join(work, name, name + ".fbx")   # its skin copies stay out of the way
            variant["dump_vertices"] = os.path.join(work, name + ".npz")
            variant["skin_normal"] = False
            run(variant, work, name)
            morphs[name] = variant["dump_vertices"]
        cfg["morphs"] = morphs
        run(cfg, work, base["name"])


if __name__ == "__main__":
    main()

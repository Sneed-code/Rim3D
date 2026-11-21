# Fits every garment in assets/makehuman_garments.json to the character creator's bases and writes each as an FBX of its
# own, with the base's body-shape morphs (docs/design/clothes-draw.md "MakeHuman bodies"):
#   game/Saved/MakeHuman/<Base>/Garments/<Garment>/<Garment>.fbx (+ materials.json)
# Like mh_creator.py, a morph is the difference between the base and a variant build with one slider changed, but only
# the sliders that change the body's shape are built (face expressions, horns, tails and pubic hair do not move clothes).
# The bodies themselves are not exported again. Then: UnrealEditor-Cmd game\Rim3D.uproject -run=MakeHumanArt -Garments
#   python tools/makehuman/mh_garments.py [base name ...]
import copy
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mh_creator import merge, run  # noqa: E402

# Creator morphs that do not change a garment's shape.
SKIP_PREFIXES = ("Face_", "No", "Pubic", "Genital")
# Settings that only matter for the body's own export (and cost time).
BODY_ONLY = ("pubic_grow", "pubic_layout", "wrinkles", "pigment_mask", "body_areas", "skin_normal_strength", "save_blend")


def garments_for(base, spec):
    out = []
    for g in spec["garments"]:
        if g.get("bases") and base not in g["bases"]:
            continue  # a women's or men's cut
        mhclo = g["mhclo"]
        if isinstance(mhclo, dict):
            mhclo = mhclo.get(base)
        if mhclo:
            out.append({"name": g["name"], "mhclo": mhclo})
    return out


def main():
    only = sys.argv[1:]
    creator = json.load(open(os.path.join(HERE, "creator.json"), encoding="utf-8"))
    spec = json.load(open(os.path.join(REPO, "assets", "makehuman_garments.json"), encoding="utf-8"))
    for base in creator["bases"]:
        name = base["name"]
        if (only and name not in only) or name not in spec.get("bases", []):
            continue
        garments = garments_for(name, spec)
        print("base", name, len(garments), "garments")
        work = tempfile.mkdtemp(prefix="mh_garments_" + name + "_")
        cfg = copy.deepcopy(base["config"])
        for key in BODY_ONLY:
            cfg.pop(key, None)
        cfg["skin_normal"] = False
        cfg["garments"] = garments
        cfg["garments_only"] = True
        cfg["garments_out"] = os.path.join(REPO, "game", "Saved", "MakeHuman", name, "Garments")
        cfg["out"] = os.path.join(work, "_base", "_base.fbx")
        morphs = {}
        for morph, change in base["morphs"].items():
            if morph.startswith(SKIP_PREFIXES):
                continue
            variant = merge(cfg, change)
            variant["out"] = os.path.join(work, morph, morph + ".fbx")
            variant["dump_vertices"] = os.path.join(work, morph + ".npz")
            run(variant, work, morph)
            morphs[morph] = variant["dump_vertices"]
        cfg["morphs"] = morphs
        run(cfg, work, name)
        print("garments of", name, "in", cfg["garments_out"], "morphs:", ", ".join(morphs))


if __name__ == "__main__":
    main()

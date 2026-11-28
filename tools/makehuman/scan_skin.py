# The scanned skin bake (docs/design/scanned-skin.md): the MetaHuman Creator's free scanned skin (exported by the
# ScanSkin commandlet to game/Saved/ScanSkin) moved onto each MakeHuman creator base's texture layout. For each base:
#   1. Blender lines the MetaHuman body and face up with the base by their skeletons and draws both faces from the front,
#   2. OpenCV's facial landmark model (LBF, 68 points) finds the eyes, nose and mouth on both drawings,
#   3. Blender moves the MetaHuman face onto the base's landmarks, settles it onto the skin and bakes the colour, normal,
#      roughness and cavity maps (skin_scan*.png beside the base's FBX; MakeHumanArt uses them for the 4K skin).
#   python tools/makehuman/scan_skin.py [base ...]
# Pictures of each step go to game/Saved/ScanSkin/work (faces with their landmarks, the coverage).
import json
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("RIM3D_ROOT") or os.path.abspath(os.path.join(HERE, "..", ".."))
SCAN = os.environ.get("SCAN_SKIN_DIR") or os.path.join(REPO, "game", "Saved", "ScanSkin")
BLENDER = os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe")
LBF_URL = "https://raw.githubusercontent.com/kurnianggoro/GSOC2017/master/data/lbfmodel.yaml"


def ensure_cv2():
    try:
        import cv2  # noqa: F401
        if hasattr(cv2, "face"):
            return
    except ImportError:
        pass
    print("[scan_skin] installing OpenCV (contrib, headless) for the landmark model")
    subprocess.run([sys.executable, "-m", "pip", "install", "--user", "--quiet", "opencv-contrib-python-headless"], check=True)
    import importlib
    import site
    importlib.reload(site)


def lbf_model():
    path = os.path.join(SCAN, "lbfmodel.yaml")
    if not os.path.exists(path) or os.path.getsize(path) < 10_000_000:
        print("[scan_skin] downloading the facial landmark model (LBF, 54 MB)")
        tmp = path + ".part"
        urllib.request.urlretrieve(LBF_URL, tmp)
        os.replace(tmp, path)
    return path


def blender(mode, base, work):
    cmd = [BLENDER, "-b", "--factory-startup", "-noaudio", "-P", os.path.join(HERE, "scan_skin_blend.py"), "--", mode, base, work]
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = [l for l in result.stdout.splitlines() if l.startswith("[scan_skin") or "Error" in l or "Traceback" in l]
    print("\n".join("    " + l for l in lines))
    with open(os.path.join(work, f"{base}_{mode}.log"), "w", encoding="utf-8") as f:
        f.write(result.stdout + "\n" + result.stderr)
    if result.returncode != 0:
        print(result.stdout[-3000:], result.stderr[-3000:])
        raise SystemExit(f"{base}: Blender {mode} failed")


def find_landmarks(image_path, model, cameras_box=None):
    """68 facial landmarks on a front view (OpenCV LBF). The face box comes from OpenCV's face finder, or, when it finds
    none, from where the face sits in the drawing (centred, a known size)."""
    import cv2
    import numpy as np
    img = cv2.imread(image_path)
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    gray = cv2.equalizeHist(gray)
    h, w = gray.shape
    finder = cv2.CascadeClassifier(os.path.join(cv2.data.haarcascades, "haarcascade_frontalface_alt2.xml"))
    faces = finder.detectMultiScale(gray, scaleFactor=1.05, minNeighbors=3, minSize=(w // 4, h // 4))
    if len(faces):
        box = max(faces, key=lambda f: f[2] * f[3])
        how = "found"
    else:
        # The drawing is 0.30 m across, centred halfway from the eyes to the mouth: a face box like the finder's.
        box = np.array([int(w * 0.20), int(h * 0.22), int(w * 0.60), int(h * 0.60)])
        how = "assumed"
    facemark = cv2.face.createFacemarkLBF()
    facemark.loadModel(model)
    ok, marks = facemark.fit(gray, np.array([box], dtype=np.int32))
    if not ok or not len(marks):
        return None, how, box
    pts = np.asarray(marks[0], dtype=np.float64).reshape(-1, 2).tolist()
    vis = img.copy()
    x, y, bw, bh = [int(v) for v in box]
    cv2.rectangle(vis, (x, y), (x + bw, y + bh), (255, 160, 0), 1)
    for i, (px, py) in enumerate(pts):
        cv2.circle(vis, (int(px), int(py)), 2, (0, 255, 0) if i >= 27 else (0, 0, 255), -1)
    cv2.imwrite(image_path.replace(".png", "_marks.png"), vis)
    return pts, how, box


def main():
    config = json.load(open(os.path.join(REPO, "assets", "scan_skin.json"), encoding="utf-8"))
    only = sys.argv[1:]
    if not os.path.exists(os.path.join(SCAN, "mesh_body.json")):
        raise SystemExit("game/Saved/ScanSkin has no MetaHuman meshes yet (the ScanSkin commandlet makes them)")
    ensure_cv2()
    model = lbf_model()
    work = os.path.join(SCAN, "work")
    os.makedirs(work, exist_ok=True)
    for base in config["bases"]:
        if only and base not in only:
            continue
        if not os.path.exists(os.path.join(REPO, "game", "Saved", "MakeHuman", base, base + ".fbx")):
            print("[scan_skin]", base, "has no FBX yet; skipped")
            continue
        print("[scan_skin] base", base)
        blender("render", base, work)
        found = {}
        for side in ("target", "source"):
            pts, how, box = find_landmarks(os.path.join(work, f"{base}_{side}_face.png"), model)
            print(f"    {side} face: {'68 landmarks' if pts else 'NO landmarks'} (face box {how})")
            found[side] = pts
        with open(os.path.join(work, f"{base}_landmarks.json"), "w", encoding="utf-8") as f:
            json.dump(found, f)
        blender("bake", base, work)


if __name__ == "__main__":
    main()

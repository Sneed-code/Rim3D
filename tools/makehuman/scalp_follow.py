"""Makes every creator slider move the scalp the way the hair can follow it (sneed 2026-11-28: "some races or elongated
skull types have the scalp visible ... a system wide fix").

The game moves a person's strand hair with their head sliders by one scale per axis about the skull's centre and a move
(StrandHair.cpp, from grow_hair.py's head_axes). Most sliders change the skull like that, but some don't: MakeHuman's
brow ridge target also pushes the back of the crown out, and with the ethnic base shapes a Melanesian preset left skin up
to 10 mm out through the hair at the back of the crown while the hair floated 9 mm off everywhere else. So each morph's
change is replaced, on the hair-covered scalp, by the scale per axis and move that fits it best (least squares over the
scalp), blended back to the morph's own change across a band below the hairline (forehead, temples, nape) and kept away
from the ears. The face below the hairline is untouched; the scalp then changes exactly as the game moves the hair, for
any mix of sliders (the fits add up), so no skin is ever left outside the hair and the hair never floats.

Plain Python with numpy (no Blender): mh_creator.py runs it on the variant builds' vertex dumps (mh_common.dump_vertices,
object space) before the base is built with them. Works in any object space whose axes are the body's (the up axis and
the face's direction are found from the body and the eyes)."""
import numpy as np

# grow_hair.py's hairline: elevation (degrees above the skull centre) by azimuth from the front (degrees).
HAIRLINE = [(0, 22), (30, 20), (55, 15), (75, 4), (90, 6), (105, -4), (125, -22), (150, -36), (180, -42)]
# Morphs left as they are: ears (pointed, large, sticking out; the cat head's no-ears), and live facial expression units.
KEEP = ("ImpEars", "ElfEars", "ShapeEarsLo", "ShapeEarsHi", "ShapeEarsOutLo", "ShapeEarsOutHi", "CatHead")
KEEP_PREFIX = ("Face_",)


def _smooth(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _sphere(points):
    A = np.c_[2 * points, np.ones(len(points))]
    b = (points ** 2).sum(1)
    sol = np.linalg.lstsq(A, b, rcond=None)[0]
    return sol[:3], float(np.sqrt(sol[3] + (sol[:3] ** 2).sum()))


class Frame:
    """The body's head in its own object space: up and forward axes, the skull sphere fitted to the crown."""

    def __init__(self, body, eyes=None):
        v = body
        ext = v.max(0) - v.min(0)
        self.up_axis = int(np.argmax(ext))
        height = float(ext[self.up_axis])
        up = np.zeros(3)
        up[self.up_axis] = 1.0
        # The head is at the end nearer the eyes (or, without them, the end whose top slice is narrower than the feet's span).
        if eyes is not None and len(eyes):
            eye = eyes.mean(0)
            if eye[self.up_axis] < v[:, self.up_axis].mean():
                up = -up
        self.up = up
        h = v @ up
        top = h.max()
        # Skull: a sphere fitted to the crown as grow_hair.py fits it (the head above 55% of the way up from the head bone,
        # which sits 8.5% of the body's height under the top: 6 cm under it), within 12 cm of the head's middle.
        crown_cut = top - 0.0385 * height
        head = v[h > top - 0.15 * height]
        mid = head.mean(0)
        side = v - mid
        side -= np.outer(side @ up, up)
        near = (h > crown_cut) & (np.linalg.norm(side, axis=1) < 0.075 * height)
        self.skull, self.radius = _sphere(v[near])
        # Forward: from the skull's centre to the eyes (MakeHuman faces -Y in Blender when there are none).
        if eyes is not None and len(eyes):
            f = eyes.mean(0) - self.skull
        else:
            f = np.array([0.0, -1.0, 0.0])
        f -= up * (f @ up)
        self.forward = f / max(np.linalg.norm(f), 1e-9)
        self.right = np.cross(self.forward, self.up)
        self.height = height

    def angles(self, p):
        d = p - self.skull
        r = np.linalg.norm(d, axis=-1)
        dn = d / np.maximum(r, 1e-12)[..., None]
        az = np.degrees(np.arctan2(dn @ self.right, dn @ self.forward))
        el = np.degrees(np.arcsin(np.clip(dn @ self.up, -1.0, 1.0)))
        lateral = np.abs(d @ self.right)
        return az, el, r, lateral

    def scalp(self, p, below=3.0):
        """grow_hair.scalp_ids: inside the hairline (`below` degrees lower to take in its edge), not the ears."""
        az, el, r, lateral = self.angles(p)
        hl = np.interp(np.abs(az), *zip(*HAIRLINE))
        inside = (el > hl - below) & (r < self.radius * 1.35)
        inside &= ~((lateral > self.radius * 0.93) & (el < 22))
        return inside

    def weight(self, p):
        """1 on the hair-covered scalp, 0 on the face, below the nape and on the ears, smooth between: full 5 degrees
        under the hairline (past the edge the hair grows from, and grow_hair's measure of it), none 17 degrees under it."""
        az, el, r, lateral = self.angles(p)
        hl = np.interp(np.abs(az), *zip(*HAIRLINE))
        w = _smooth((el - (hl - 17.0)) / 12.0)
        w *= 1.0 - _smooth((r / self.radius - 1.2) / 0.2)
        # The ears (outside the skull's sides, low): their own change.
        ear = _smooth((lateral / self.radius - 0.8) / 0.12) * (1.0 - _smooth((el - 18.0) / 14.0))
        return w * (1.0 - ear)


def fit_axes(frame, base, moved, mask):
    """The scale along each of the body's axes about the skull centre and the move that best carry base[mask] to
    moved[mask] (least squares): what the game does to the hair (grow_hair.head_axes measures the same)."""
    s = np.zeros(3)
    t = np.zeros(3)
    P = base[mask]
    D = moved[mask] - P
    c = frame.skull
    for a in range(3):
        A = np.c_[P[:, a] - c[a], np.ones(len(P))]
        s[a], t[a] = np.linalg.lstsq(A, D[:, a], rcond=None)[0]
    return s, t


def follow(frame, base_meshes, morph_meshes, body_name, log=print):
    """One morph's meshes ({name: Nx3}) with the scalp's change made a scale per axis and a move (see the top). Every mesh
    is changed by the same rule by where its vertices are (wig cards and hats sit on the scalp too)."""
    base = base_meshes[body_name]
    moved = morph_meshes[body_name]
    mask = frame.scalp(base)
    if np.abs(moved[mask] - base[mask]).max() < 1e-6 * frame.height:
        return morph_meshes, 0.0
    s, t = fit_axes(frame, base, moved, mask)
    out = {}
    worst = 0.0
    for name, m in morph_meshes.items():
        b = base_meshes.get(name)
        if b is None or b.shape != m.shape:
            out[name] = m
            continue
        w = frame.weight(b)[:, None]
        fitted = b + (b - frame.skull) * s + t
        new = m + (fitted - m) * w
        if name == body_name:
            worst = float(np.linalg.norm((new - m)[mask], axis=1).max())
        out[name] = new.astype(m.dtype)
    return out, worst


def follow_npz(base_path, morph_paths, out_dir, log=print):
    """mh_creator.py's step: {name: variant npz} -> {name: new npz in out_dir} for every morph that moves the scalp."""
    import os
    base_npz = np.load(base_path)
    base = {k: base_npz[k].reshape(-1, 3).astype(np.float64) for k in base_npz.files}
    body_name = max(base, key=lambda k: (("body" in k.lower()) and not any(x in k.lower() for x in ("hair", "pubic")), len(base[k])))
    eyes = next((base[k] for k in base if "high-poly" in k.lower() or k.lower().endswith("eyes")), None)
    frame = Frame(base[body_name], eyes)
    log("scalp follow: body %s, up axis %d, skull radius %.3f (height %.3f), %d scalp vertices" % (
        body_name, frame.up_axis, frame.radius, frame.height, int(frame.scalp(base[body_name]).sum())))
    os.makedirs(out_dir, exist_ok=True)
    result = {}
    for name, path in morph_paths.items():
        if name in KEEP or name.startswith(KEEP_PREFIX):
            result[name] = path
            continue
        data = np.load(path)
        meshes = {k: data[k].reshape(-1, 3).astype(np.float64) for k in data.files}
        if body_name not in meshes or meshes[body_name].shape != base[body_name].shape:
            result[name] = path
            continue
        new, worst = follow(frame, base, meshes, body_name, log)
        if worst <= 0.0:
            result[name] = path
            continue
        target = os.path.join(out_dir, name + ".npz")
        np.savez(target, **{k: v.reshape(-1).astype(np.float32) for k, v in new.items()})
        log("scalp follow: %s scalp moved by up to %.4f to fit the hair" % (name, worst))
        result[name] = target
    return result

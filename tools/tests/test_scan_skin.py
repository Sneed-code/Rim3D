"""The scanned skin bake's line-up maths (tools/makehuman/scan_skin_blend.py), without Blender: bpy and mathutils are
stubbed, only the numpy parts are tested (the whole bake was tried on a stand-in body with Blender's Python module)."""
import os
import sys
import types
import unittest

try:
    import numpy as np
except ImportError:  # the CI runner's Python may have no numpy: nothing to test there
    np = None

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "makehuman"))
ssb = None
if np is not None:
    for name in ("bpy", "mathutils", "mathutils.bvhtree"):
        sys.modules.setdefault(name, types.ModuleType(name))
    sys.modules["mathutils"].Matrix = sys.modules["mathutils"].Vector = object
    sys.modules["mathutils.bvhtree"].BVHTree = object
    import scan_skin_blend as ssb  # noqa: E402


def rotation(axis, degrees):
    axis = np.asarray(axis, float) / np.linalg.norm(axis)
    a = np.radians(degrees)
    k = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + np.sin(a) * k + (1 - np.cos(a)) * k @ k


@unittest.skipIf(np is None, "needs numpy")
class ScanSkinMaths(unittest.TestCase):
    def test_umeyama_recovers_a_similarity(self):
        rng = np.random.default_rng(3)
        src = rng.normal(size=(30, 3))
        rot = rotation([1, 2, 3], 25)
        dst = 1.1 * src @ rot.T + np.array([0.3, -0.2, 0.5])
        scale, r, t = ssb.umeyama(src, dst)
        self.assertAlmostEqual(scale, 1.1, places=6)
        np.testing.assert_allclose(r, rot, atol=1e-6)
        np.testing.assert_allclose(scale * src @ r.T + t, dst, atol=1e-6)

    def test_spline_passes_through_the_landmarks(self):
        rng = np.random.default_rng(4)
        src = rng.normal(size=(25, 3)) * 0.05
        dst = src + rng.normal(size=(25, 3)) * 0.003
        model = ssb.tps_fit(src, dst, smooth=1e-9)
        np.testing.assert_allclose(src + ssb.tps_eval(model, src), dst, atol=1e-6)

    def test_spine_joints_land_on_the_same_share_of_the_chain(self):
        source = {n: np.array([0, 0, z]) for n, z in zip(ssb.CHAINS[0], [1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6])}
        target = {"pelvis": np.array([0, 0, 0.9]), "spine_01": np.array([0, 0, 1.0]), "spine_02": np.array([0, 0, 1.2]),
                  "spine_03": np.array([0, 0, 1.3]), "neck_01": np.array([0, 0, 1.5])}
        out = ssb.chain_targets(source, target)
        np.testing.assert_allclose(out["pelvis"], target["pelvis"])
        np.testing.assert_allclose(out["neck_01"], target["neck_01"])
        np.testing.assert_allclose(out["spine_03"], [0, 0, 1.2])  # half way along both chains

    def test_turn_between_with_a_roll(self):
        rot = rotation([0, 1, 1], 40)
        a, a2 = np.array([1.0, 0, 0]), np.array([0, 0, 1.0])
        r = ssb.turn_between(a, rot @ a, a2, rot @ a2)
        np.testing.assert_allclose(r, rot, atol=1e-9)
        r = ssb.turn_between(a, rot @ a)
        np.testing.assert_allclose(r @ a, rot @ a, atol=1e-9)

    def test_an_arm_follows_its_bones(self):
        """A straight arm in the source, bent down 30 degrees and 10% longer in the target: a point on the forearm goes
        with the forearm."""
        part = types.SimpleNamespace()
        part.bones = ["upperarm_l", "lowerarm_l", "hand_l"]
        part.parents = [-1, 0, 1]
        part.joints = {"upperarm_l": np.array([0.0, 0, 0]), "lowerarm_l": np.array([0.3, 0, 0]), "hand_l": np.array([0.55, 0, 0])}
        part.points = np.array([[0.4, 0, 0.02]])
        part.skin = [[(1, 1.0)]]
        part.name = "arm"
        down = rotation([0, 1, 0], 30)
        target = {"upperarm_l": np.zeros(3), "lowerarm_l": down @ np.array([0.33, 0, 0]), "hand_l": down @ np.array([0.605, 0, 0])}
        ssb.skin_part(part, target)
        expected = target["lowerarm_l"] + down @ np.array([0.11, 0, 0.02])
        np.testing.assert_allclose(part.points[0], expected, atol=1e-9)


if __name__ == "__main__":
    unittest.main()

"""Tests for tools/update.py: the art step list is valid and the planner runs only what changed."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import update  # noqa: E402

STEPS = [
    {"name": "fit", "run": [{"python": ["fit.py"]}]},
    {"name": "bodies", "after": ["fit"], "run": [{"commandlet": "MakeHumanArt"}]},
    {"name": "garments", "after": ["fit"], "coveredBy": ["bodies"], "run": [{"commandlet": "MakeHumanArt", "args": ["-Garments"]}]},
    {"name": "hair", "needs": ["blender"], "run": [{"blender": ["x"]}]},
    {"name": "blood", "after": ["bodies"], "run": [{"commandlet": "BloodArt"}]},
]
PRINTS = {s["name"]: s["name"] + "-v1" for s in STEPS}


def done(prints):
    return {name: {"done": fp} for name, fp in prints.items()}


def decisions(stamps, prints=PRINTS, forced=(), have=lambda n: True, root=Path(".")):
    return {s["name"]: d for s, d, _ in update.plan(STEPS, stamps, prints, set(forced), have, root)}


class ArtStepsFile(unittest.TestCase):
    def test_real_list_is_valid(self):
        steps = update.load_steps()
        self.assertGreater(len(steps), 20)
        names = [s["name"] for s in steps]
        # The steps PC builds needed by hand on 2026-11-21 are all there.
        for needed in ("hair-grow", "hair-grooms", "garments-fit", "makehuman-garments", "blood-art", "wet-look", "blast-art", "sounds", "gun-art"):
            self.assertIn(needed, names)
        # WetLook goes over every material, so it comes last.
        self.assertEqual(names[-1], "wet-look")

    def test_every_commandlet_exists(self):
        for step in update.load_steps():
            for command in step["run"]:
                if "commandlet" in command:
                    sources = update.commandlet_sources(command["commandlet"])
                    self.assertIn(f"game/Source/Rim3DEditor/{command['commandlet']}Commandlet.cpp", sources, step["name"])

    def test_check_steps_catches_mistakes(self):
        self.assertTrue(update.check_steps([{"name": "a", "after": ["b"], "run": [{"python": []}]}, {"name": "b", "run": [{"python": []}]}]))
        self.assertTrue(update.check_steps([{"name": "a", "run": [{"python": [], "blender": []}]}]))
        self.assertTrue(update.check_steps([{"name": "a", "run": []}]))
        self.assertFalse(update.check_steps(STEPS))


class Planner(unittest.TestCase):
    def test_first_run_runs_everything(self):
        d = decisions({})
        self.assertEqual(d["fit"], "run")
        self.assertEqual(d["bodies"], "run")
        self.assertEqual(d["garments"], "covered")  # the full MakeHumanArt run makes the garments too
        self.assertEqual(d["blood"], "run")

    def test_nothing_changed_runs_nothing(self):
        self.assertEqual(set(decisions(done(PRINTS)).values()), {"skip"})

    def test_a_change_runs_the_step_and_what_comes_after(self):
        prints = dict(PRINTS, fit="fit-v2")
        d = decisions(done(PRINTS), prints)
        self.assertEqual(d["fit"], "run")
        self.assertEqual(d["bodies"], "run")
        self.assertEqual(d["blood"], "run")  # after bodies, which runs after fit
        self.assertEqual(d["hair"], "skip")

    def test_garments_alone(self):
        prints = dict(PRINTS, garments="garments-v2")
        d = decisions(done(PRINTS), prints)
        self.assertEqual(d["garments"], "run")
        self.assertEqual(d["bodies"], "skip")

    def test_missing_need_waits(self):
        d = decisions({}, have=lambda n: n != "blender")
        self.assertEqual(d["hair"], "wait")

    def test_forced(self):
        self.assertEqual(decisions(done(PRINTS), forced=["blood"])["blood"], "run")

    def test_failed_on_same_inputs_is_not_retried(self):
        stamps = done(PRINTS)
        stamps["blood"] = {"failed": "blood-v1"}
        self.assertEqual(decisions(stamps)["blood"], "failed")
        self.assertEqual(decisions(stamps, forced=["blood"])["blood"], "run")
        self.assertEqual(decisions(stamps, dict(PRINTS, blood="blood-v2"))["blood"], "run")

    def test_missing_output_runs(self):
        steps = [{"name": "a", "makes": ["nothing/here"], "run": [{"python": []}]}]
        with tempfile.TemporaryDirectory() as tmp:
            out = update.plan(steps, {"a": {"done": "x"}}, {"a": "x"}, set(), lambda n: True, Path(tmp))
            self.assertEqual(out[0][1], "run")
            (Path(tmp) / "nothing" / "here").mkdir(parents=True)
            out = update.plan(steps, {"a": {"done": "x"}}, {"a": "x"}, set(), lambda n: True, Path(tmp))
            self.assertEqual(out[0][1], "skip")


class Fingerprints(unittest.TestCase):
    def test_raw_files_change_the_fingerprint(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "raw").mkdir()
            step = {"name": "a", "raw": ["raw"], "run": [{"python": []}]}
            empty = update.fingerprint(step, root)
            (root / "raw" / "gun.fbx").write_bytes(b"1234")
            one = update.fingerprint(step, root)
            self.assertNotEqual(empty, one)
            self.assertEqual(one, update.fingerprint(step, root))

    def test_command_changes_the_fingerprint(self):
        a = update.fingerprint({"name": "a", "run": [{"python": ["x.py"]}]})
        b = update.fingerprint({"name": "a", "run": [{"python": ["x.py", "--cut"]}]})
        self.assertNotEqual(a, b)

    def test_git_inputs_change_with_the_file(self):
        a = update.fingerprint({"name": "a", "inputs": ["assets/guns.json"], "run": [{"python": []}]})
        b = update.fingerprint({"name": "a", "inputs": ["assets/guns.json", "assets/shuttle.json"], "run": [{"python": []}]})
        self.assertNotEqual(a, b)


if __name__ == "__main__":
    unittest.main()

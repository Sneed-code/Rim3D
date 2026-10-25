"""Tests for tools/check_defs.py: the reference checks, and that the real data files pass."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent


def run_checker(root, *extra):
    return subprocess.run([sys.executable, str(TOOLS / "check_defs.py"), str(root), *extra],
                          capture_output=True, text=True)


def write_defs(root, files):
    for rel, defs in files.items():
        path = Path(root) / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(defs), encoding="utf-8")


class ReferenceTests(unittest.TestCase):
    def test_repo_data_passes(self):
        result = run_checker(TOOLS.parent / "data" / "defs")
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_unknown_reference_fails(self):
        with tempfile.TemporaryDirectory() as root:
            write_defs(root, {
                "Skills/Skills.json": [{"defName": "Mining"}],
                "Backstories/Backstories.json": [{"defName": "Miner", "skillGains": {"Minnig": 3}}],
            })
            result = run_checker(root)
            self.assertEqual(result.returncode, 1)
            self.assertIn("unknown Skills def 'Minnig'", result.stdout)
            self.assertEqual(run_checker(root, "--no-references").returncode, 0)

    def test_abstract_reference_fails(self):
        with tempfile.TemporaryDirectory() as root:
            write_defs(root, {
                "Things/Things.json": [
                    {"defName": "ItemBase", "abstract": True},
                    {"defName": "Plant_X", "plant": {"harvestedThingDef": "ItemBase"}},
                ],
            })
            result = run_checker(root)
            self.assertEqual(result.returncode, 1)
            self.assertIn("abstract Things def 'ItemBase'", result.stdout)

    def test_body_tree_is_walked(self):
        with tempfile.TemporaryDirectory() as root:
            write_defs(root, {
                "BodyParts/BodyParts.json": [{"defName": "Torso"}, {"defName": "Arm"}],
                "Bodies/Bodies.json": [{"defName": "Human", "corePart": {
                    "def": "Torso", "parts": [{"def": "Arm", "parts": [{"def": "Hnad"}]}]}}],
            })
            result = run_checker(root)
            self.assertEqual(result.returncode, 1)
            self.assertIn("unknown BodyParts def 'Hnad'", result.stdout)

    def test_missing_target_kind_is_not_checked(self):
        with tempfile.TemporaryDirectory() as root:
            write_defs(root, {"Things/Things.json": [{"defName": "Human", "race": {"body": "Human"}}]})
            self.assertEqual(run_checker(root).returncode, 0)


if __name__ == "__main__":
    unittest.main()

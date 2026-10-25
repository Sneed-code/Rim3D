"""Tests for tools/naming.py (the rename table in docs/design/naming.md) and its use by the importer.

Run: python3 -m unittest discover -s tools/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

import naming  # noqa: E402
import rimworld_import as imp  # noqa: E402
import rimworld_import_spec as spec  # noqa: E402


class NamingTableTests(unittest.TestCase):
    def test_real_table_parses(self):
        applied = naming.load_renames()
        self.assertEqual(applied["Impid"], naming.Rename("Emberkin", "Emberkin"))
        self.assertEqual(applied["Cassandra"].label, "Orrin")          # "(steady build-up)" dropped
        self.assertEqual(applied["PsychicSoothe"], naming.Rename("ResonanceCalm", "resonance calm"))
        self.assertNotIn("Pirate", applied)                             # kept names are not renames
        self.assertNotIn("Plasteel", applied)                           # proposed, not applied
        self.assertIn("Plasteel", naming.load_renames(section="Proposed"))

    def test_our_names_are_unique(self):
        ours = [r.def_name for r in naming.load_renames().values()]
        self.assertEqual(len(ours), len(set(ours)))

    def test_rows_without_two_def_names_are_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            md = Path(tmp) / "n.md"
            md.write_text(
                "## Applied\n\n| a | b | c | d |\n| --- | --- | --- | --- |\n"
                "| glitterworld | | lumen world | |\n"
                "| Foo | `Foo` | bar | `Bar` |\n"
                "| Two | `A`, `B` | x, y | `X`, `Y` |\n\n"
                "## Proposed\n\n| Baz | `Baz` | qux | `Qux` |\n", encoding="utf-8")
            got = naming.load_renames(md)
            self.assertEqual(got, {"Foo": naming.Rename("Bar", "bar"),
                                   "A": naming.Rename("X", "x"), "B": naming.Rename("Y", "y")})


class ImporterRenameTests(unittest.TestCase):
    def test_renamed_def_and_references_follow(self):
        ks = spec.KINDS[0]
        faction = {"defName": "Empire", "label": "Empire", "source": "Royalty",
                   "pawnGroupMakers": [{"options": {"Empire_Trooper": 5, "Mech_Scyther": 1}}],
                   "enemy": "Insect"}
        other = {"defName": "Pirate", "label": "pirates", "allies": ["Empire"]}
        results = [imp.KindResult(ks, [faction, other], [], [])]
        used = imp.apply_renames(results, naming.load_renames())
        concord, pirate = results[0].defs
        self.assertEqual(concord["defName"], "Concord")
        self.assertEqual(concord["label"], "the Concord")
        self.assertEqual(concord["pawnGroupMakers"][0]["options"], {"Empire_Trooper": 5, "Remnant_Reaper": 1})
        self.assertEqual(concord["enemy"], "Chitinid")
        self.assertEqual(pirate["allies"], ["Concord"])
        self.assertEqual(pirate["label"], "pirates")
        self.assertEqual(used, {"Empire": "Concord", "Mech_Scyther": "Remnant_Reaper", "Insect": "Chitinid"})


if __name__ == "__main__":
    unittest.main()

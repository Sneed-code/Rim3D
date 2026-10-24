"""Tests for tools/rimworld_import.py, on the tiny XML fixtures in tools/tests/samples.

Run: python3 -m unittest discover -s tools/tests
"""
import json
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
REPO = TOOLS.parent
SAMPLES = Path(__file__).resolve().parent / "samples"
sys.path.insert(0, str(TOOLS))

import rimworld_import as imp  # noqa: E402
import rimworld_import_spec as spec  # noqa: E402


def things_by_name(result):
    kinds = {r.spec.folder: r for r in result.kinds}
    return {d["defName"]: d for d in kinds["Things"].defs}, kinds


def parse(xml):
    return ET.fromstring(xml)


class ValueTests(unittest.TestCase):
    def test_coerce(self):
        self.assertEqual(imp.coerce("12"), 12)
        self.assertEqual(imp.coerce("-0.5"), -0.5)
        self.assertIs(imp.coerce("True"), True)
        self.assertIs(imp.coerce("false"), False)
        self.assertEqual(imp.coerce("(3,1)"), [3, 1])
        self.assertEqual(imp.coerce("(0.5, 1, 2)"), [0.5, 1, 2])
        self.assertEqual(imp.coerce("0.3~1.2"), [0.3, 1.2])
        self.assertEqual(imp.coerce("100~250"), [100, 250])
        self.assertEqual(imp.coerce("Verb_Shoot"), "Verb_Shoot")
        self.assertEqual(imp.coerce("VegetarianRoughAnimal, Fungus"), "VegetarianRoughAnimal, Fungus")
        self.assertIsNone(imp.coerce("   "))

    def test_labels_stay_text(self):
        self.assertEqual(imp.coerce("5", "label"), "5")

    def test_number_cleanup(self):
        self.assertEqual(imp.tidy_number(2880.0), 2880)
        self.assertEqual(imp.tidy_number(0.1533333333), 0.153333)
        self.assertIs(imp.tidy_number(True), True)


class InheritanceTests(unittest.TestCase):
    def merged(self, parent, child):
        return imp._merge(parse(parent), parse(child))

    def test_child_wins_and_objects_merge(self):
        out = self.merged(
            "<T><a>1</a><stats><x>1</x><y>2</y></stats></T>",
            "<T><b>3</b><stats><y>9</y></stats></T>")
        self.assertEqual(out.find("a").text, "1")
        self.assertEqual(out.find("b").text, "3")
        self.assertEqual(out.find("stats/x").text, "1")
        self.assertEqual(out.find("stats/y").text, "9")

    def test_child_list_replaces_parent_list(self):
        out = self.merged("<T><l><li>a</li><li>b</li></l></T>", "<T><l><li>c</li></l></T>")
        self.assertEqual([e.text for e in out.findall("l/li")], ["c"])

    def test_parent_list_kept_when_child_is_silent(self):
        out = self.merged("<T><l><li>a</li></l></T>", "<T><z>1</z></T>")
        self.assertEqual([e.text for e in out.findall("l/li")], ["a"])

    def test_inherit_false(self):
        out = self.merged("<T><c><Steel>10</Steel><Wood>5</Wood></c></T>",
                          '<T><c Inherit="False"><Gold>1</Gold></c></T>')
        self.assertEqual([e.tag for e in out.find("c")], ["Gold"])

    def test_leaf_overrides_object(self):
        out = self.merged("<T><c><a>1</a></c></T>", "<T><c>7</c></T>")
        self.assertEqual(out.find("c").text, "7")

    def test_unknown_parent_is_a_warning(self):
        warnings = []
        node = imp.Node("ThingDef", parse("<ThingDef ParentName='Nope'><defName>X</defName></ThingDef>"),
                        "Core", Path("x.xml"), None, "Nope", False)
        out = imp.resolve_inheritance([node], warnings)
        self.assertIn(("ThingDef", "X"), out)
        self.assertTrue(any("Nope" in w for w in warnings))

    def test_cycle_does_not_hang(self):
        warnings = []
        a = imp.Node("ThingDef", parse("<ThingDef Name='A' ParentName='B'><defName>A</defName></ThingDef>"),
                     "Core", Path("x.xml"), "A", "B", False)
        b = imp.Node("ThingDef", parse("<ThingDef Name='B' ParentName='A'><defName>B</defName></ThingDef>"),
                     "Core", Path("x.xml"), "B", "A", False)
        imp.resolve_inheritance([a, b], warnings)
        self.assertTrue(any("cycle" in w for w in warnings))


class ClockConstantsTests(unittest.TestCase):
    def test_constants_match_clock_h(self):
        text = (REPO / "game/Plugins/ColonySim/Source/ColonySim/Public/ColonySim/Clock.h").read_text()

        def const(name):
            return float(re.search(name + r"\s*=\s*([\d.]+)", text).group(1))

        self.assertEqual(const("kStepsPerRealSecondAt1x"), spec.STEPS_PER_REAL_SECOND_AT_1X)
        self.assertEqual(const("kSecondsPerHour"), spec.SECONDS_PER_HOUR)
        self.assertEqual(const("kRimWorldTicksPerHour"), spec.RIMWORLD_TICKS_PER_HOUR)

    def test_converters(self):
        c = spec.UNIT_CONVERTERS
        self.assertAlmostEqual(c["ticks"](2500), 3600)      # one RimWorld hour is one game hour
        self.assertAlmostEqual(c["ticks"](60000), 86400)    # and a day is a day
        self.assertAlmostEqual(c["rt_seconds"](1), 30)      # one real second at 1x is 30 steps
        self.assertAlmostEqual(c["rt_ticks"](60), 30)
        self.assertAlmostEqual(c["rt_speed"](30), 1)        # 30 m per real second is 1 m per step


class SampleImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.result = imp.run_import(SAMPLES, ["Core"])
        cls.things, cls.kinds = things_by_name(cls.result)

    def test_only_listed_things_come_out(self):
        self.assertNotIn("NotWanted", self.things)
        for name in ("Wall", "FueledStove", "Steel", "Human", "Gun_Revolver", "Plant_Berry"):
            self.assertIn(name, self.things)

    def test_glob_selects_all_matches(self):
        self.assertIn("Plant_TreeOak", self.things)
        self.assertIn("Plant_TreePine", self.things)

    def test_abstract_bases_flattened_and_not_emitted(self):
        self.assertNotIn("BuildingBase", self.things)
        wall = self.things["Wall"]
        self.assertEqual(wall["category"], "Building")
        self.assertEqual(wall["stats"]["maxHitPoints"], 200)      # child overrides parent
        self.assertEqual(wall["stats"]["flammability"], 1)        # inherited
        self.assertEqual(wall["size"], [1, 1])
        self.assertNotIn("parent", wall)
        self.assertNotIn("abstract", wall)

    def test_two_level_inheritance(self):
        stove = self.things["FueledStove"]
        self.assertEqual(stove["category"], "Building")
        self.assertEqual(stove["stats"]["flammability"], 1)
        self.assertEqual(stove["costs"], {"Steel": 80, "WoodLog": 5})

    def test_comp_classes_become_types(self):
        types = [c["type"] for c in self.things["FueledStove"]["comps"]]
        self.assertEqual(types, ["Refuelable", "Flickable"])   # the child's list replaced the base's

    def test_prose_and_presentation_dropped(self):
        text = json.dumps(self.result.kinds[0].defs) + json.dumps(list(self.things.values()))
        self.assertNotIn("Flavour text", text)
        self.assertNotIn("texPath", text)
        self.assertNotIn("Prose", json.dumps([d for r in self.result.kinds for d in r.defs]))
        self.assertGreater(self.result.drops[("Things", "description")], 0)

    def test_work_and_ingest_ticks_become_game_seconds(self):
        self.assertEqual(self.things["FueledStove"]["stats"]["workToBuild"], 2880)   # 2000 ticks
        self.assertEqual(self.things["Wall"]["stats"]["workToBuild"], 72)            # 50 ticks
        self.assertEqual(self.things["RawBerries"]["ingestible"]["baseIngestSeconds"], 360)
        plant = self.things["Plant_Berry"]["plant"]
        self.assertEqual(plant["sowWork"], 144)
        self.assertEqual(plant["growDays"], 6)                                       # days unchanged
        self.assertEqual(plant["visualSizeRange"], [0.3, 1.2])

    def test_combat_timings_use_the_sim_clock(self):
        verb = self.things["Gun_Revolver"]["verbs"][0]
        self.assertEqual(verb["warmupTime"], 9)                  # 0.3 real seconds = 9 steps
        self.assertEqual(verb["secondsBetweenBurstShots"], 6)    # 12 ticks = 0.2 s = 6 steps
        self.assertEqual(verb["range"], 25.9)
        self.assertEqual(self.things["Gun_Revolver"]["tools"][0]["cooldownTime"], 60)
        self.assertEqual(self.things["Bullet_Revolver"]["projectile"]["speed"], 3)   # 90 m/s = 3 m/step
        self.assertAlmostEqual(self.things["Human"]["stats"]["moveSpeed"], 4.6 / 30, places=5)

    def test_stat_dict_keys_are_lower_camel_and_def_keys_are_not(self):
        steel = self.things["Steel"]
        self.assertEqual(steel["stuffProps"]["statOffsets"], {"beauty": 0.5})
        self.assertEqual(steel["stuffProps"]["statFactors"]["workToMake"], 2.5)      # a factor, not ticks
        self.assertEqual(self.things["FueledStove"]["costs"]["Steel"], 80)

    def test_projectile_pulled_in_as_dependency(self):
        kinds = self.kinds
        self.assertIn("Bullet_Revolver", kinds["Things"].dependencies + [d["defName"] for d in kinds["Things"].defs])
        # Berry bush names RawBerries, which is also listed; no duplicates either way.
        names = [d["defName"] for d in kinds["Things"].defs]
        self.assertEqual(len(names), len(set(names)))

    def test_dependencies_are_followed(self):
        # Wall's costs are empty, but the Pirate kind names Human, which is already selected.
        # Use a pack with a gun whose bullet is not listed to check the pull-in.
        with tempfile.TemporaryDirectory() as tmp:
            defs = Path(tmp) / "Data" / "Core" / "Defs"
            defs.mkdir(parents=True)
            (defs / "a.xml").write_text(
                "<Defs><ThingDef><defName>Gun_Revolver</defName><verbs><li>"
                "<defaultProjectile>Bullet_Odd</defaultProjectile></li></verbs></ThingDef>"
                "<ThingDef><defName>Bullet_Odd</defName><category>Projectile</category></ThingDef></Defs>")
            res = imp.run_import(Path(tmp), ["Core"])
            things = next(r for r in res.kinds if r.spec.folder == "Things")
            self.assertEqual(things.dependencies, ["Bullet_Odd"])
            self.assertIn("Bullet_Odd", [d["defName"] for d in things.defs])

    def test_missing_names_reported(self):
        terrain = self.kinds["Terrain"]
        self.assertIn("Sand", terrain.missing)
        self.assertNotIn("Soil", terrain.missing)

    def test_unresolved_references_reported(self):
        self.assertIn("Things/Wall.stuffCategories", self.result.unresolved)
        self.assertEqual(self.result.unresolved["Recipes/CookMealSimple.products"], ["MealSimple"])

    def test_race_dictionary_is_not_mistaken_for_references(self):
        self.assertNotIn("Things/Human.race", self.result.unresolved)

    def test_later_pack_overrides_earlier(self):
        res = imp.run_import(SAMPLES, ["Core", "Royalty"])
        soil = next(d for r in res.kinds if r.spec.folder == "Terrain" for d in r.defs if d["defName"] == "Soil")
        self.assertEqual(soil["fertility"], 2)
        self.assertEqual(soil["source"], "Royalty")

    def test_thing_category_parent_is_renamed(self):
        cats = {d["defName"]: d for d in self.kinds["ThingCategories"].defs}
        self.assertEqual(cats["Resources"]["parentCategory"], "Root")
        self.assertNotIn("parent", cats["Resources"])

    def test_both_skill_gain_shapes_become_a_dictionary(self):
        stories = {d["defName"]: d for d in self.kinds["Backstories"].defs}
        self.assertEqual(stories["TestOldStyle"]["skillGains"], {"Mining": 3, "Shooting": -2})
        self.assertEqual(stories["TestNewStyle"]["skillGains"], {"Shooting": 4})

    def test_backstory_filter(self):
        self.assertNotIn("TestTribal", {d["defName"] for d in self.kinds["Backstories"].defs})

    def test_trait_degrees_and_thought_stages(self):
        trait = self.kinds["Traits"].defs[0]
        self.assertEqual([x["degree"] for x in trait["degreeDatas"]], [1, 2])
        self.assertEqual(trait["degreeDatas"][0]["statOffsets"], {"workSpeedGlobal": 0.35})
        thoughts = {d["defName"]: d for d in self.kinds["Thoughts"].defs}
        self.assertEqual(thoughts["AteWithoutTable"]["stages"][0]["baseMoodEffect"], -3)
        self.assertEqual(thoughts["Hungry"]["workerClass"], "ThoughtWorker_Hunger")
        self.assertIs(thoughts["Hungry"]["stages"][0]["visible"], False)

    def test_body_tree(self):
        body = self.kinds["Bodies"].defs[0]
        self.assertEqual(body["corePart"]["def"], "Torso")
        self.assertEqual(body["corePart"]["parts"][0]["def"], "Head")
        self.assertEqual(body["corePart"]["parts"][0]["groups"], ["FullHead"])

    def test_recipe_and_faction(self):
        recipe = self.kinds["Recipes"].defs[0]
        self.assertEqual(recipe["workAmount"], 432)
        self.assertEqual(recipe["products"], {"MealSimple": 1})
        self.assertEqual(recipe["ingredients"][0]["filter"]["categories"], ["FoodRaw"])
        pirate = self.kinds["PawnKinds"].defs[0]
        self.assertEqual(pirate["weaponMoney"], [100, 250])
        faction = self.kinds["Factions"].defs[0]
        self.assertEqual(faction["pawnGroupMakers"][0]["options"], {"Pirate": 10})

    def test_every_def_has_defname_label_source_and_no_parent(self):
        for r in self.result.kinds:
            for d in r.defs:
                self.assertTrue(d["defName"])
                self.assertEqual(d["source"], "Core")
                self.assertNotIn("parent", d)
                self.assertNotIn("abstract", d)


class OutputTests(unittest.TestCase):
    def run_cli(self, out, *extra):
        return subprocess.run(
            [sys.executable, str(TOOLS / "rimworld_import.py"), "--rimworld", str(SAMPLES),
             "--out-defs", str(out / "defs"), "--out-ref", str(out / "ref"), *extra],
            capture_output=True, text=True)

    def test_cli_writes_files_that_pass_the_data_checker(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            proc = self.run_cli(out)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertTrue((out / "defs/Things/Things.rimworld.json").is_file())
            self.assertTrue((out / "ref/Things.md").is_file())
            self.assertTrue((out / "ref/import-report.md").is_file())
            check = subprocess.run([sys.executable, str(TOOLS / "check_defs.py"), str(out / "defs")],
                                   capture_output=True, text=True)
            self.assertEqual(check.returncode, 0, check.stdout)

    def test_output_is_identical_between_runs(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.run_cli(Path(a))
            self.run_cli(Path(b))
            files = sorted(p.relative_to(a) for p in Path(a).rglob("*") if p.is_file())
            self.assertTrue(files)
            for f in files:
                self.assertEqual((Path(a) / f).read_bytes(), (Path(b) / f).read_bytes(), str(f))

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = self.run_cli(Path(tmp), "--dry-run")
            self.assertEqual(proc.returncode, 0)
            self.assertEqual(list(Path(tmp).rglob("*.json")), [])

    def test_strict_fails_when_names_are_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_cli(Path(tmp), "--strict").returncode, 1)

    def test_collision_with_hand_written_def_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            terrain = Path(tmp) / "defs" / "Terrain"
            terrain.mkdir(parents=True)
            (terrain / "Terrain.json").write_text('[{"defName": "Soil"}]')
            proc = self.run_cli(Path(tmp))
            self.assertEqual(proc.returncode, 2)
            self.assertIn("Soil", proc.stderr)
            self.assertFalse((terrain / "Terrain.rimworld.json").exists())

    def test_find(self):
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "rimworld_import.py"), "--rimworld", str(SAMPLES),
             "--find", "^gun_", "--class", "ThingDef"], capture_output=True, text=True)
        self.assertIn("ThingDef\tGun_Revolver", proc.stdout)
        self.assertNotIn("Wall", proc.stdout)

    def test_reference_table_is_compact_and_has_numbers(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.run_cli(Path(tmp))
            text = (Path(tmp) / "ref/Things.md").read_text()
            self.assertIn("| FueledStove | fueled stove |", text)
            self.assertIn("2880", text)
            self.assertNotIn("Flavour text", text)

    def test_no_rimworld_folder_is_a_clear_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run(
                [sys.executable, str(TOOLS / "rimworld_import.py"), "--rimworld", tmp],
                capture_output=True, text=True)
            self.assertEqual(proc.returncode, 2)
            self.assertIn("No RimWorld Defs folder", proc.stderr)


if __name__ == "__main__":
    unittest.main()

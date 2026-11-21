#!/usr/bin/env python3
"""Checks every JSON def file under data/defs.

Each file is a JSON array of defs. The folder a file sits in is its def kind
(Terrain, Things, ...). Checks: valid JSON, every def has a unique defName
within its kind, every parent exists in the same kind with no cycles, and the
fields listed in REFERENCES name a concrete def of the right kind (a typo in a
hand-written file is caught here, not in the game). A reference into a kind with
no folder is not checked. Exits 1 and lists every problem if anything is wrong.
An optional argument checks another folder instead of data/defs (the importer's tests use this,
with --no-references because their small samples leave references dangling).
"""
import json
import sys
from pathlib import Path

ARGS = [a for a in sys.argv[1:] if a != "--no-references"]
CHECK_REFERENCES = "--no-references" not in sys.argv[1:]
ROOT = Path(ARGS[0]) if ARGS else Path(__file__).resolve().parent.parent / "data" / "defs"

# (kind, field path, kind the values name). Path steps are separated by dots; "x[]" walks the items
# of list x, "x{}" takes the keys of object x, and "**" is the node plus every node nested in its
# "parts" lists (the body tree).
REFERENCES = [
    ("Things", "plant.harvestedThingDef", "Things"),
    ("Things", "hold.style", "HoldStyles"),
    ("Things", "mineable.dropDef", "Things"),
    ("Things", "race.body", "Bodies"),
    ("Things", "costs{}", "Things"),
    ("Things", "race.hitboxes", "Hitboxes"),
    ("Things", "race.meatDef", "Things"),
    ("Things", "race.leatherDef", "Things"),
    ("Things", "race.milk.thing", "Things"),
    ("Things", "race.wool.thing", "Things"),
    ("Things", "race.eggs.unfertilizedThing", "Things"),
    ("Things", "race.eggs.fertilizedThing", "Things"),
    ("Things", "hatches.into", "Things"),
    ("Things", "verbs[].defaultProjectile", "Projectiles"),
    ("Things", "apparel.bodyPartGroups[]", "BodyPartGroups"),
    ("Projectiles", "projectile.damageDef", "DamageTypes"),
    ("Projectiles", "explosion.damageDef", "DamageTypes"),
    ("Things", "thrown.projectile", "Projectiles"),
    ("Hitboxes", "body", "Bodies"),
    ("Recipes", "recipeUsers[]", "Things"),
    ("Recipes", "products{}", "Things"),
    ("Recipes", "workSkill", "Skills"),
    ("Recipes", "ingredients[].items[]", "Things"),
    ("Recipes", "surgery.addsHediff", "Hediffs"),
    ("Recipes", "surgery.ingredient", "Things"),
    ("Recipes", "surgery.part", "BodyParts"),
    ("Hediffs", "addedPart.part", "BodyParts"),
    ("Hediffs", "addedPart.item", "Things"),
    ("Biomes", "wildPlants{}", "Things"),
    ("Biomes", "animals{}", "Things"),
    ("Things", "race.wildBiomes{}", "Biomes"),
    ("Biomes", "weather{}", "Weathers"),
    ("WorldGen", "biomeRules[].biome", "Biomes"),
    ("WorldGen", "waterRules[].biome", "Biomes"),
    ("Bodies", "corePart.**.def", "BodyParts"),
    ("Bodies", "corePart.**.groups[]", "BodyPartGroups"),
    ("WorkTypes", "relevantSkills[]", "Skills"),
    ("Traits", "conflictingTraits[]", "Traits"),
    ("Traits", "disallowedPassions[]", "Skills"),
    ("Traits", "degreeDatas[].skillGains{}", "Skills"),
    ("Backstories", "skillGains{}", "Skills"),
    ("Backstories", "forcedTraits[].trait", "Traits"),
    ("Backstories", "disallowedTraits[]", "Traits"),
    ("DamageTypes", "hediff", "Hediffs"),
    ("DamageTypes", "hediffSolid", "Hediffs"),
    ("Capacities", "parts[].part", "BodyParts"),
    ("Capacities", "limbs.core", "BodyParts"),
    ("Capacities", "limbs.segments[]", "BodyParts"),
    ("Capacities", "limbs.digit", "BodyParts"),
    ("Capacities", "capacityFactors[].capacity", "Capacities"),
    ("Hediffs", "stages[].capacityOffsets{}", "Capacities"),
    ("Hediffs", "stages[].capacityMax{}", "Capacities"),
    ("Scenarios", "startingThings[].thing", "Things"),
    ("Scenarios", "nearbyThings[].thing", "Things"),
    ("Scenarios", "scatteredThings[].thing", "Things"),
    ("Scenarios", "colonistKind", "Things"),
    ("Factions", "pawnKinds[].kind", "PawnKinds"),
    ("Factions", "visitorKinds[].kind", "PawnKinds"),
    ("Factions", "guardKinds[]", "PawnKinds"),
    ("Factions", "traderGuardKinds[]", "PawnKinds"),
    ("Factions", "traderKinds[]", "PawnKinds"),
    ("Factions", "traders.base[]", "TraderKinds"),
    ("Factions", "traders.caravan[]", "TraderKinds"),
    ("Factions", "traders.orbital[]", "TraderKinds"),
    ("TraderKinds", "stock[].thing", "Things"),
    ("Quests", "faction", "Factions"),
    ("Quests", "rewards[].thing", "Things"),
    ("Incidents", "hediff", "Hediffs"),
    ("Incidents", "faction", "Factions"),
    ("Rules", "defaultNarrator", "Narrators"),
    ("Rules", "defaultDifficulty", "Difficulties"),
    ("Rules", "minIncidentChanceFactorByNarrator{}", "Narrators"),
    ("PawnKinds", "race", "Things"),
    ("PawnKinds", "weapons[]", "Things"),
    ("PawnKinds", "apparel[].thing", "Things"),
    ("PawnKinds", "apparel[].stuff", "Things"),
    ("PawnKinds", "faction", "Factions"),
    ("PawnKinds", "lootTable", "LootTables"),
    ("LootTables", "items[].thing", "Things"),
    ("LootTables", "stacks.from[].thing", "Things"),
    ("Scenarios", "biome", "Biomes"),
    ("Scenarios", "lineage", "Lineages"),
    ("RecreationActivities", "kind", "RecreationKinds"),
    ("RecreationActivities", "building", "Things"),
    ("Things", "building.joyKind", "RecreationKinds"),
    ("Interactions", "initiatorThought", "Thoughts"),
    ("Interactions", "recipientThought", "Thoughts"),
    ("Interactions", "rejectedThought", "Thoughts"),
    ("Interactions", "acceptedThought", "Thoughts"),
    ("Interactions", "bond", "Bonds"),
    ("Bonds", "deathThought", "Thoughts"),
    ("Rules", "catharsisThought", "Thoughts"),
    ("Research", "techLevel", "TechLevels"),
    ("Talents", "requires{}", "Talents"),
    ("Research", "prerequisites[]", "Research"),
    ("Research", "hiddenPrerequisites[]", "Research"),
    ("Research", "unlocks.Things[]", "Things"),
    ("Research", "unlocks.Recipes[]", "Recipes"),
    ("Research", "unlocks.Terrain[]", "Terrain"),
    ("Scenarios", "colonistPawnKind", "PawnKinds"),
    ("Scenarios", "colonistApparel[].thing", "Things"),
    ("Scenarios", "colonistApparel[].stuff", "Things"),
    ("Outfits", "allowed[]", "Things"),
    ("Outfits", "disallowed[]", "Things"),
    ("Lineages", "genes[]", "Genes"),
    ("Genes", "aptitudes{}", "Skills"),
    ("Scenarios", "lineageWeights{}", "Lineages"),
    ("Rules", "rooms[].things[]", "Things"),
    ("Rules", "yard[]", "Things"),
]

# Kinds whose defs may carry "researchPrerequisite" (a project name or a list of them). Every project
# it names must list the def under unlocks.<kind>, and every unlocks entry must be named back.
RESEARCH_GATED_KINDS = ("Things", "Recipes", "Terrain")


# Cues the sim plays for an event with no def to name them (Sound.cpp, docs/design/sound.md).
EVENT_CUES = {
    "Impact_Armor", "Impact_Building", "Impact_Ground", "Impact_Rock", "Melee_Clang", "Melee_Block", "Punch", "Dodge", "Alert_Suspicious", "Alert_Spotted",
    "Grenade_Throw", "Grenade_Landed", "Trap_Spring", "Trap_Rearm", "Window_Shatter", "Item_PickUp", "Item_PutDown", "Power_On",
    "Power_Off", "Short_Circuit", "Roof_Collapse", "Fire_Ignite", "Fire_Burning", "Fire_Out", "Alarm", "Raid_Horn",
    "Research_Done", "Work_Mine", "Work_Chop", "Work_Harvest", "Work_Build", "Work_Cook", "Work_Smith", "Work_Craft",
    "Work_Tailor", "Work_Research", "Work_Clean", "Work_Tend", "Work_Stonecut", "Work_Butcher", "Work_Eat",
    "Work_Refuel", "Work_Handle", "Work_Chat", "Work_Extinguish",
    "Voice_LovinCouple", "Voice_LovinFem",  # Unreal, from the Lovin event (soundLovinCue)
    # MeleeHit by material (soundMeleeCue): weapon wood or steel on flesh, wood, steel or stone
    "Hit_Wood_Flesh", "Hit_Wood_Wood", "Hit_Wood_Steel", "Hit_Wood_Stone",
    "Hit_Steel_Flesh", "Hit_Steel_Wood", "Hit_Steel_Steel", "Hit_Steel_Stone",
    "Ui_KillConfirm",  # Unreal, the kill feed on the player's own kill (KillFeedSubsystem.cpp)
}


def walk(node, steps):
    """Yields the values at a REFERENCES path."""
    if not steps:
        yield node
        return
    step, rest = steps[0], steps[1:]
    if step == "**":
        stack = [node]
        while stack:
            current = stack.pop()
            yield from walk(current, rest)
            if isinstance(current, dict) and isinstance(current.get("parts"), list):
                stack.extend(current["parts"])
        return
    if not isinstance(node, dict):
        return
    if step.endswith("[]"):
        items = node.get(step[:-2])
        for item in items if isinstance(items, list) else []:
            yield from walk(item, rest)
    elif step.endswith("{}"):
        keys = node.get(step[:-2])
        for key in keys if isinstance(keys, dict) else {}:
            yield from walk(key, rest)
    elif step in node:
        yield from walk(node[step], rest)


def gate_of(kind_defs, name):
    """The def's researchPrerequisite as a list, taken from the nearest ancestor that has one."""
    while name in kind_defs:
        entry = kind_defs[name][1]
        if "researchPrerequisite" in entry:
            value = entry["researchPrerequisite"]
            return [value] if isinstance(value, str) else list(value)
        name = entry.get("parent")
    return []


def check_research_unlocks(defs_by_kind):
    """researchPrerequisite on a def and unlocks on a project must say the same thing."""
    problems = []
    projects = defs_by_kind["Research"]
    listed = set()
    for project, (file, entry) in projects.items():
        for kind, names in (entry.get("unlocks") or {}).items():
            for name in names:
                listed.add((kind, name, project))
    for kind in RESEARCH_GATED_KINDS:
        for name, (file, entry) in defs_by_kind.get(kind, {}).items():
            gate = gate_of(defs_by_kind[kind], name)
            if entry.get("abstract") is True:
                continue
            for project in gate:
                if project not in projects:
                    problems.append(f"{file}: {kind} '{name}' researchPrerequisite names unknown Research def '{project}'")
                elif (kind, name, project) not in listed:
                    problems.append(f"{file}: {kind} '{name}' needs '{project}' but it is not in that project's unlocks")
    for kind, name, project in sorted(listed):
        if project in projects and project not in gate_of(defs_by_kind.get(kind, {}), name):
            problems.append(f"{projects[project][0]}: Research '{project}' unlocks {kind} '{name}' but its researchPrerequisite does not name it")
    return problems


def check_garment_cuts(defs_by_kind):
    """Every garment a def's visual.cuts names ('+' joins several) must be in assets/makehuman_garments.json."""
    path = Path(__file__).resolve().parent.parent / "assets" / "makehuman_garments.json"
    if not path.exists():
        return []
    garments = {g["name"] for g in json.loads(path.read_text(encoding="utf-8"))["garments"]}
    problems = []
    for kind, kind_defs in defs_by_kind.items():
        for name, (file, entry) in kind_defs.items():
            visual = entry.get("visual")
            cuts = visual.get("cuts") if isinstance(visual, dict) else None
            if cuts is None:
                continue
            if not isinstance(cuts, list) or not all(isinstance(c, str) and c for c in cuts):
                problems.append(f"{file}: {kind} '{name}' visual.cuts must be a list of garment names")
                continue
            for cut in cuts:
                for garment in cut.split("+"):
                    if garment not in garments:
                        problems.append(f"{file}: {kind} '{name}' visual.cuts names unknown garment '{garment}'")
    return problems


def check_sound_cues(defs_by_kind):
    """Every cue a def names under visual.sound must be a SoundCues def; returns the problems and the cues used."""
    problems = []
    cues = defs_by_kind.get("SoundCues", {})
    used = set()
    for kind, kind_defs in defs_by_kind.items():
        for name, (file, entry) in kind_defs.items():
            visual = entry.get("visual")
            sound = visual.get("sound") if isinstance(visual, dict) else None
            if sound is None:
                continue
            if not isinstance(sound, dict):
                problems.append(f"{file}: {kind} '{name}' visual.sound must be an object of slot: cue")
                continue
            for slot, cue in sound.items():
                if slot == "material":  # what it sounds like when struck or striking (melee by material)
                    if cue not in ("Wood", "Steel", "Stone", "Flesh"):
                        problems.append(f"{file}: {kind} '{name}' visual.sound.material must be Wood, Steel, Stone or Flesh")
                    continue
                if not isinstance(cue, str) or cue not in cues:
                    problems.append(f"{file}: {kind} '{name}' visual.sound.{slot} names unknown SoundCues def '{cue}'")
                else:
                    used.add(cue)
    for name, (file, entry) in cues.items():
        far = entry.get("far")
        if far is not None:  # the distant layer a shot or a blast turns into (sound.md "Distance layers")
            if far not in cues:
                problems.append(f"{file}: SoundCues '{name}' far names unknown SoundCues def '{far}'")
            elif not isinstance(entry.get("farFrom"), (int, float)) or entry["farFrom"] <= 0:
                problems.append(f"{file}: SoundCues '{name}' with far needs farFrom (metres)")
            else:
                used.add(far)
        tone = entry.get("tone")
        if not isinstance(tone, dict) or tone.get("wave") not in ("sine", "square", "saw", "noise") or not tone.get("hz") or not tone.get("seconds"):
            problems.append(f"{file}: SoundCues '{name}' needs tone {{wave, hz, seconds}}")
        volume = entry.get("volume")
        if entry.get("loop") not in (True, False) or not isinstance(volume, (int, float)) or not 0 < volume <= 1 or entry.get("priority") not in (0, 1, 2, 3):
            problems.append(f"{file}: SoundCues '{name}' needs loop, volume (0 to 1) and priority (0 to 3)")
    return problems, used


def check_hold_styles(defs_by_kind):
    """Hold styles (docs/design/weapon-hold.md): grip, hand and hands are known words, points are [x, y, z], a two-handed
    style has an off point, and attackStyle is a MeleeRules.styleAttacks list."""
    problems = []
    rules = defs_by_kind.get("Rules", {}).get("MeleeRules")
    attack_styles = set((rules[1].get("styleAttacks") or {}).keys()) if rules else set()
    point = lambda v: isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v)
    place_keys = ("ready", "aim", "block", "fpReady", "fpAim", "fpBlock")

    def check_places(where, places):
        # Where the weapon is held: {anchor, point, at, turn} per state (docs/design/weapon-hold.md).
        if not isinstance(places, dict):
            problems.append(f"{where} places must be an object")
            return
        for key, place in places.items():
            if key not in place_keys:
                problems.append(f"{where} places.{key} is not one of {', '.join(place_keys)}")
            elif not isinstance(place, dict) or not point(place.get("at")):
                problems.append(f"{where} places.{key} needs at: [x, y, z]")
            else:
                if place.get("anchor", "Chest") not in ("Chest", "Head", "View"):
                    problems.append(f"{where} places.{key} anchor must be Chest, Head or View")
                if place.get("point", "Main") not in ("Main", "Sight", "Stock"):
                    problems.append(f"{where} places.{key} point must be Main, Sight or Stock")
                if "turn" in place and not point(place["turn"]):
                    problems.append(f"{where} places.{key} turn must be [pitch, yaw, roll]")
                if key.startswith("fp") != (place.get("anchor", "Chest") == "View"):
                    problems.append(f"{where} places.{key}: first-person places use anchor View, third-person ones Chest or Head")

    for name, (file, entry) in defs_by_kind.get("HoldStyles", {}).items():
        if "places" in entry:
            check_places(f"{file}: HoldStyles '{name}'", entry["places"])
        for key in ("support", "offTurn"):
            if key in entry and not point(entry[key]):
                problems.append(f"{file}: HoldStyles '{name}' {key} must be [x, y, z]")
        if entry.get("grip", "Blade") not in ("None", "Blade", "Gun", "Bow", "Shield", "Pole"):
            problems.append(f"{file}: HoldStyles '{name}' grip must be None, Blade, Gun, Bow, Shield or Pole")
        if entry.get("hand", "Right") not in ("Right", "Left"):
            problems.append(f"{file}: HoldStyles '{name}' hand must be Right or Left")
        if entry.get("hands", 1) not in (0, 1, 2):
            problems.append(f"{file}: HoldStyles '{name}' hands must be 0, 1 or 2")
        for key in ("main", "off"):
            if key in entry and not point(entry[key]):
                problems.append(f"{file}: HoldStyles '{name}' {key} must be [x, y, z]")
        if entry.get("hands", 1) == 2 and entry.get("grip") != "Bow" and "off" not in entry:
            problems.append(f"{file}: HoldStyles '{name}' is two-handed and needs an off point")
        style = entry.get("attackStyle", "")
        if style and attack_styles and style not in attack_styles:
            problems.append(f"{file}: HoldStyles '{name}' attackStyle '{style}' is not a MeleeRules.styleAttacks list")
    for name, (file, entry) in defs_by_kind.get("Things", {}).items():
        hold = entry.get("hold")
        if hold is None:
            continue
        if not isinstance(hold, dict) or "style" not in hold:
            problems.append(f"{file}: Things '{name}' hold needs a style")
            continue
        for key in ("main", "off", "support", "offTurn"):
            if key in hold and not point(hold[key]):
                problems.append(f"{file}: Things '{name}' hold.{key} must be [x, y, z]")
        if "places" in hold:
            check_places(f"{file}: Things '{name}' hold", hold["places"])
    return problems


def main() -> int:
    problems = []
    defs_by_kind = {}
    for path in sorted(ROOT.rglob("*.json")):
        kind = path.relative_to(ROOT).parts[0]
        try:
            entries = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as err:
            problems.append(f"{path}: invalid JSON: {err}")
            continue
        if not isinstance(entries, list):
            problems.append(f"{path}: must be a JSON array of defs")
            continue
        kind_defs = defs_by_kind.setdefault(kind, {})
        for i, entry in enumerate(entries):
            name = entry.get("defName") if isinstance(entry, dict) else None
            if not isinstance(name, str) or not name:
                problems.append(f"{path}: entry {i} has no defName")
                continue
            if name in kind_defs:
                problems.append(f"{path}: duplicate {kind} defName '{name}' (also in {kind_defs[name][0]})")
                continue
            kind_defs[name] = (path, entry)

    for kind, kind_defs in defs_by_kind.items():
        for name, (path, entry) in kind_defs.items():
            seen = {name}
            parent = entry.get("parent")
            while parent is not None:
                if parent not in kind_defs:
                    problems.append(f"{path}: {kind} '{name}' has unknown parent '{parent}'")
                    break
                if parent in seen:
                    problems.append(f"{path}: {kind} '{name}' has a parent cycle through '{parent}'")
                    break
                seen.add(parent)
                parent = kind_defs[parent][1].get("parent")

    for kind, path, target in REFERENCES:
        if not CHECK_REFERENCES or kind not in defs_by_kind or target not in defs_by_kind:
            continue
        targets = defs_by_kind[target]
        for name, (file, entry) in defs_by_kind[kind].items():
            for value in walk(entry, path.split(".")):
                if not isinstance(value, str) or value not in targets:
                    problems.append(f"{file}: {kind} '{name}' {path} names unknown {target} def '{value}'")
                elif targets[value][1].get("abstract") is True:
                    problems.append(f"{file}: {kind} '{name}' {path} names abstract {target} def '{value}'")

    if CHECK_REFERENCES and "Research" in defs_by_kind:
        problems.extend(check_research_unlocks(defs_by_kind))

    if CHECK_REFERENCES and "HoldStyles" in defs_by_kind:
        problems.extend(check_hold_styles(defs_by_kind))
        problems.extend(check_garment_cuts(defs_by_kind))

    if CHECK_REFERENCES and "SoundCues" in defs_by_kind:
        sound_problems, used = check_sound_cues(defs_by_kind)
        problems.extend(sound_problems)
        for name in sorted(set(defs_by_kind["SoundCues"]) - used - EVENT_CUES):
            problems.append(f"{defs_by_kind['SoundCues'][name][0]}: SoundCues '{name}' is used by no def and no event")

    for problem in problems:
        print(problem)
    count = sum(len(d) for d in defs_by_kind.values())
    print(f"{count} defs checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

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
    ("Bodies", "corePart.**.def", "BodyParts"),
    ("Bodies", "corePart.**.groups[]", "BodyPartGroups"),
    ("WorkTypes", "relevantSkills[]", "Skills"),
    ("Traits", "conflictingTraits[]", "Traits"),
    ("Traits", "disallowedPassions[]", "Skills"),
    ("Traits", "degreeDatas[].skillGains{}", "Skills"),
    ("Backstories", "skillGains{}", "Skills"),
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
    ("Research", "prerequisites[]", "Research"),
    ("Research", "hiddenPrerequisites[]", "Research"),
    ("Research", "unlocks.Things[]", "Things"),
    ("Research", "unlocks.Recipes[]", "Recipes"),
    ("Research", "unlocks.Terrain[]", "Terrain"),
    ("Lineages", "genes[]", "Genes"),
    ("Genes", "aptitudes{}", "Skills"),
    ("Scenarios", "lineageWeights{}", "Lineages"),
]

# Kinds whose defs may carry "researchPrerequisite" (a project name or a list of them). Every project
# it names must list the def under unlocks.<kind>, and every unlocks entry must be named back.
RESEARCH_GATED_KINDS = ("Things", "Recipes", "Terrain")


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

    for problem in problems:
        print(problem)
    count = sum(len(d) for d in defs_by_kind.values())
    print(f"{count} defs checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

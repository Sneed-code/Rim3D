"""What the RimWorld importer imports and how it converts it.

This file is data, not logic: the list of RimWorld defs the prototype needs, the fields that are
dropped, and which numbers are durations or speeds that need converting. To import something new,
add a KindSpec (or a name to an existing one) here; `rimworld_import.py` does the rest.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

# --- Time constants. These mirror game/Plugins/ColonySim/Source/ColonySim/Public/ColonySim/Clock.h;
# a test checks that they still match. -------------------------------------------------------
SECONDS_PER_HOUR = 3600.0
RIMWORLD_TICKS_PER_HOUR = 2500.0
STEPS_PER_REAL_SECOND_AT_1X = 30.0
RIMWORLD_TICKS_PER_REAL_SECOND = 60.0

# Converters for numbers that are not plain. Everything not listed here is copied unchanged
# (percentages, days, hours, per-day rates, years, metres and counts all carry over as they are,
# because our day is as long as RimWorld's in game time).
UNIT_CONVERTERS: Dict[str, Callable[[float], float]] = {
    # RimWorld ticks (also work amounts, which RimWorld counts in ticks at work speed 1)
    # to game seconds.
    "ticks": lambda v: v * SECONDS_PER_HOUR / RIMWORLD_TICKS_PER_HOUR,
    # Combat timings RimWorld gives in real seconds at its normal speed. Our pawns and bullets
    # move in real time at 1x, so they become game seconds (= sim steps) at 30 steps per second.
    "rt_seconds": lambda v: v * STEPS_PER_REAL_SECOND_AT_1X,
    "rt_ticks": lambda v: v / RIMWORLD_TICKS_PER_REAL_SECOND * STEPS_PER_REAL_SECOND_AT_1X,
    # Metres (cells) per real second at 1x to metres per game second.
    "rt_speed": lambda v: v / STEPS_PER_REAL_SECOND_AT_1X,
}

# (context, field) -> converter. The context is the nearest enclosing field that is not a list
# item (so "verbs" for the fields of a gun's verb, "statBases" for a stat). "*" matches any.
UNITS: Dict[Tuple[str, str], str] = {
    ("*", "baseIngestTicks"): "ticks",
    ("*", "sowWork"): "ticks",
    ("*", "harvestWork"): "ticks",
    ("*", "workAmount"): "ticks",
    ("*", "disappearsAfterTicks"): "ticks",
    ("statBases", "WorkToMake"): "ticks",
    ("statBases", "WorkToBuild"): "ticks",
    ("statBases", "MoveSpeed"): "rt_speed",
    ("verbs", "warmupTime"): "rt_seconds",
    ("verbs", "ticksBetweenBurstShots"): "rt_ticks",
    ("tools", "cooldownTime"): "rt_seconds",
    ("projectile", "speed"): "rt_speed",
}

# --- Naming ---------------------------------------------------------------------------------
# Field renames, applied at any depth, to match the names used in docs/architecture.md.
RENAMES: Dict[str, str] = {"statBases": "stats", "costList": "costs"}

# Dictionaries whose keys are stat names (PascalCase in RimWorld). The keys become lowerCamel
# ("MaxHitPoints" -> "maxHitPoints"). Keys of other dictionaries are def names and stay as they are.
STAT_DICTS = {"statBases", "statOffsets", "statFactors", "equippedStatOffsets"}

# Fields that are always text, even when they look like numbers.
STRING_FIELDS = {
    "defName", "label", "labelShort", "title", "titleShort", "skillLabel", "labelNoun",
    "pawnLabel", "gerundLabel", "verb", "gerund", "jobString", "reportString", "slot",
}

# --- Dropped fields -------------------------------------------------------------------------
# Presentation, sound and long prose. Unreal gets its own `visual` fields later; RimWorld's
# descriptions and flavour text are never committed. The import report counts what was dropped.
DROP_FIELDS = {
    "description", "baseDescription", "descriptionHyper", "helpText", "letterText",
    "graphicData", "graphic", "uiIconPath", "uiIconScale", "uiIconOffset", "uiIconForStackCount",
    "iconPath", "blueprintGraphicData", "colorGenerator", "uiOrder", "designatorDropdown",
    "inspectorTabs", "drawerType", "altitudeLayer", "drawGUIOverlay", "drawPlaceWorkersWhileSelected",
    "placeWorkers", "soundImpactDefault", "soundInteract", "soundDrop", "soundPickup", "soundCast",
    "soundCastTail", "soundAmbient", "soundOpen", "soundClose", "soundWorking", "effectWorking",
    "muzzleFlashScale", "ingestEffect", "ingestSound", "meleeHitSound", "soundMeleeHit",
    "soundMeleeHitPawn", "soundMeleeHitBuilding", "soundMeleeMiss", "researchViewX",
    "researchViewY", "modExtensions", "hairColors", "lifeStageAges_graphics", "headerTooltip",
}


# --- What to import -------------------------------------------------------------------------
@dataclass
class KindSpec:
    folder: str                     # output folder under data/defs, also the "kind"
    rw_class: str                   # RimWorld XML element name, e.g. "ThingDef"
    names: List[str]                # defNames or glob patterns ("*" = every def of the class)
    where: Optional[Callable[[dict], bool]] = None   # extra filter on the converted def
    limit: Optional[Tuple[str, int]] = None          # (field, n): at most n defs per field value
    summary: List[str] = field(default_factory=list)  # dotted fields shown in the reference table
    rename_fields: Dict[str, str] = field(default_factory=dict)  # top-level renames for this kind
    title: str = ""


def _civil_backstory(d: dict) -> bool:
    cats = d.get("spawnCategories") or []
    return any(c in ("Offworld", "Civil") for c in cats)


KINDS: List[KindSpec] = [
    KindSpec(
        "Terrain", "TerrainDef",
        ["Soil", "SoilRich", "Sand", "Gravel", "WaterShallow", "WaterDeep", "Mud", "MarshyTerrain",
         "*_Rough"],
        summary=["fertility", "pathCost", "stats.beauty", "stats.cleanliness"],
        title="Terrain",
    ),
    KindSpec(
        "Things", "ThingDef",
        [
            # Rock, stone and ore.
            "Granite", "Limestone", "Sandstone", "Slate", "Marble",
            "ChunkGranite", "ChunkLimestone", "ChunkSandstone", "ChunkSlate", "ChunkMarble",
            "BlocksGranite", "BlocksLimestone", "BlocksSandstone", "BlocksSlate", "BlocksMarble",
            "MineableSteel", "MineableComponentsIndustrial", "MineableSilver", "MineableGold",
            "MineablePlasteel", "MineableUranium", "MineableJade",
            # Materials and items.
            "WoodLog", "Steel", "Silver", "Gold", "Plasteel", "ComponentIndustrial",
            # Food.
            "RawBerries", "RawPotatoes", "MealSimple", "MealSurvivalPack",
            # Plants.
            "Plant_Tree*", "Plant_Grass", "Plant_TallGrass", "Plant_Bush", "Plant_Berry",
            "Plant_Brambles", "Plant_Dandelion",
            # Buildings.
            "Wall", "Door", "Bed", "SleepingSpot", "Bedroll", "Table*c", "FueledStove",
            # People and guns (and the bullets they fire).
            "Human",
            "Gun_Revolver", "Gun_Autopistol", "Gun_BoltActionRifle", "Gun_PumpShotgun",
            "Bullet_Revolver", "Bullet_Autopistol", "Bullet_BoltActionRifle", "Bullet_Shotgun",
        ],
        summary=["category", "stats.maxHitPoints", "stats.workToMake", "stats.workToBuild",
                 "stats.marketValue", "stats.nutrition", "costs", "size"],
        title="Things (items, buildings, plants, rock, projectiles, the Human race)",
    ),
    KindSpec(
        "ThingCategories", "ThingCategoryDef", ["*"],
        rename_fields={"parent": "parentCategory"},
        summary=["parentCategory"],
        title="Thing categories (used by recipe ingredient filters and stockpiles)",
    ),
    KindSpec(
        "StuffCategories", "StuffCategoryDef", ["*"],
        summary=["label"],
        title="Stuff categories (metallic, woody, stony...)",
    ),
    KindSpec(
        "Recipes", "RecipeDef", ["CookMealSimple", "CookMealSurvival"],
        summary=["workAmount", "workSkill", "ingredients", "products"],
        title="Recipes",
    ),
    KindSpec(
        "Needs", "NeedDef", ["Food", "Rest", "Mood"],
        summary=["baseLevel", "fallPerDay", "major"],
        title="Needs",
    ),
    KindSpec(
        "Thoughts", "ThoughtDef",
        ["AteWithoutTable", "AteRawFood", "AteFineMeal", "AteLavishMeal", "SleptOnGround",
         "SleptOutside", "Hungry*", "Tired*", "NeedFood*", "NeedRest*", "SoakingWet", "Disturbed*"],
        summary=["durationDays", "stackLimit", "workerClass", "stages"],
        title="Thoughts (mood effects)",
    ),
    KindSpec(
        "Skills", "SkillDef", ["*"],
        summary=["skillLabel", "disablingWorkTags", "usuallyDefinedInBackstories"],
        title="Skills",
    ),
    KindSpec(
        "Traits", "TraitDef",
        ["Industriousness", "Nerves", "Kind", "Brawler", "Bloodlust", "ShootingAccuracy", "Beauty",
         "Tough", "Nimble", "Jogger", "Neurotic", "Gourmand", "Greedy", "Abrasive", "Pyromaniac",
         "FastLearner", "SlowLearner", "Wimp", "Psychopath", "Cannibal"],
        summary=["commonality", "degreeDatas", "conflictingTraits"],
        title="Traits",
    ),
    KindSpec(
        "Backstories", "BackstoryDef", ["*"],
        where=_civil_backstory, limit=("slot", 40),
        summary=["slot", "skillGains", "workDisables"],
        title="Backstories (up to 40 childhood and 40 adulthood, spawned on the rim or in civil society)",
    ),
    KindSpec(
        "WorkTypes", "WorkTypeDef", ["*"],
        summary=["naturalPriority", "workTags", "relevantSkills"],
        title="Work types",
    ),
    KindSpec(
        "WorkGivers", "WorkGiverDef", ["*"],
        summary=["workType", "priorityInType", "giverClass"],
        title="Work givers (what each work type scans for)",
    ),
    KindSpec(
        "Stats", "StatDef",
        ["*Speed", "WorkSpeedGlobal", "ShootingAccuracy*", "AimingDelayFactor", "PainShockThreshold",
         "FoodPoisonChance", "BedRestEffectiveness", "Nutrition", "Comfort", "Beauty",
         "MarketValue", "MaxHitPoints", "Flammability", "Mass"],
        summary=["category", "defaultBaseValue", "skillNeedFactors"],
        title="Stats that skills and gear feed into",
    ),
    KindSpec(
        "BodyPartGroups", "BodyPartGroupDef", ["*"],
        summary=["label", "listOrder"],
        title="Body part groups",
    ),
    KindSpec(
        "BodyParts", "BodyPartDef", ["*"],
        summary=["hitPoints", "bleedRate", "permanentInjuryChanceFactor", "skinCovered"],
        title="Body part kinds",
    ),
    KindSpec(
        "Bodies", "BodyDef", ["Human"],
        summary=["corePart.def"],
        title="Bodies",
    ),
    KindSpec(
        "DamageTypes", "DamageDef",
        ["Bullet", "Blunt", "Cut", "Stab", "Scratch", "Bite", "Flame", "Bomb"],
        summary=["defaultDamage", "defaultArmorPenetration", "armorCategory", "hediff"],
        title="Damage types",
    ),
    KindSpec(
        "Hediffs", "HediffDef",
        ["Cut", "Gunshot", "Bruise", "Scratch", "Bite", "Stab", "Burn", "BloodLoss", "Malnutrition"],
        summary=["hediffClass", "injuryProps", "lethalSeverity"],
        title="Health conditions (injuries, blood loss, malnutrition)",
    ),
    KindSpec(
        "PawnKinds", "PawnKindDef", ["Colonist", "Pirate"],
        summary=["race", "combatPower", "weaponMoney", "weaponTags"],
        title="Pawn kinds",
    ),
    KindSpec(
        "Factions", "FactionDef", ["PlayerColony", "Pirate"],
        summary=["permanentEnemy", "raidCommonality", "techLevel"],
        title="Factions",
    ),
]

# When a selected def mentions one of these fields, the Things it names are imported too
# (one more round each time, up to a few rounds). "costs" and "products" are dicts keyed by def.
FOLLOW_FIELDS = {"harvestedThingDef", "defaultProjectile", "mineableThing", "race", "costs",
                 "products", "smeltProducts", "butcherProducts"}

# Fields whose values name other defs, for the unresolved-reference check in the report:
# field -> output kind. Values may be a name, a list of names, or a dict keyed by name.
REFERENCE_FIELDS: Dict[str, str] = {
    "costs": "Things",
    "harvestedThingDef": "Things",
    "defaultProjectile": "Things",
    "mineableThing": "Things",
    "products": "Things",
    "race": "Things",
    "body": "Bodies",
    "damageDef": "DamageTypes",
    "workType": "WorkTypes",
    "relevantSkills": "Skills",
    "stuffCategories": "StuffCategories",
    "thingCategories": "ThingCategories",
    "hediff": "Hediffs",
    "conflictingTraits": "Traits",
    "workSkill": "Skills",
}

#include "ColonySim/Combat.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Inspirations.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Talents.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace colony {

// Everything read from the defs once: guns, apparel, projectiles, hitboxes and the rules.
struct CombatModel {
    enum class ArmorKind : uint8_t { None, Sharp, Blunt, Heat };

    struct Gun {
        bool valid = false;
        DefId projectile;
        float range = 0.0f;
        float minRange = 0.0f;
        int warmup = 0;    // steps
        int cooldown = 0;  // steps
        int burst = 1;
        int burstGap = 0;  // steps
        float accuracy[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // touch, short, medium, long
        float recoil = 0.0f;
        int magazine = 0;    // rounds; 0 for none (never reloads)
        int reloadTime = 0;  // steps, at the skill where reloadTimeFactorBySkill is 1
        // The player's gun kick per shot of a spray (P2), degrees: up, then to the right. The shot
        // after the last entry repeats it. Empty: no kick.
        std::vector<std::pair<float, float>> kicks;
    };
    struct Apparel {
        bool valid = false;
        std::vector<std::string> groups;
        std::vector<std::string> layers;
        int outerness = 0;  // its outermost layer's position in apparelLayersOuterFirst
    };
    struct Bullet {
        bool valid = false;
        DefId damageType;
        float damage = 0.0f;
        float penetration = 0.0f;
        float speed = 0.0f;  // metres per step, tuning applied
        // A rocket or shell: the projectile def's `explosion` block, set off where it lands (R2).
        float blastRadius = 0.0f;
        float blastDamage = 0.0f;
        float blastPenetration = 0.0f;
        DefId blastType;
    };
    struct Shape {
        int part = -1;
        bool box = false;
        float a[3] = {0.0f, 0.0f, 0.0f};  // capsule end, or box minimum
        float b[3] = {0.0f, 0.0f, 0.0f};  // capsule end, or box maximum
        float r = 0.0f;
    };
    struct Pose {
        float aim[3] = {0.0f, 0.0f, 1.0f};
        float muzzle = 1.0f;
        float reach = 0.0f;  // furthest any shape gets from the pawn's centre on the ground
        float top = 0.0f;
        std::vector<Shape> shapes;
    };
    // A pawn def's hitboxes, with part names resolved against its body.
    struct Kind {
        bool valid = false;
        Pose poses[3];  // by Stance
        // For each part a hitbox names: the part and its outside parts no hitbox names, which a
        // hit on that hitbox can land on (by their share of the body), e.g. a hand and its fingers.
        std::vector<std::vector<int>> landsOn;  // by part index
        std::vector<BodyPartInfo> parts;
        float natural[3] = {0, 0, 0};  // race.armor sharp, blunt, heat: a hide or hull under any apparel
    };

    std::vector<Gun> guns;          // by Things DefId value
    std::vector<Apparel> apparel;   // by Things DefId value
    std::vector<Bullet> bullets;    // by Projectiles DefId value
    std::vector<ArmorKind> armorKind;  // by DamageTypes DefId value
    std::vector<Kind> kinds;        // by Things DefId value
    std::vector<uint8_t> melee;     // by Things DefId value: 1 for a melee weapon (an item with `tools`, F8)
    // What a weapon's quality and stuff do to it (QualityRules levels, stuffProps.statFactors).
    struct WeaponQuality {
        float melee = 1.0f;     // damage and armor penetration of a blow
        float accuracy = 1.0f;  // shooting accuracy
        float damage = 1.0f;    // damage and armor penetration of a bullet
    };
    std::vector<WeaponQuality> weaponQuality;  // by quality level
    int defaultQuality = 2;
    std::vector<float> stuffSharp;  // by Things DefId value of a stuff: meleeSharpDamage (1 if it says nothing)
    std::vector<float> stuffBlunt;  // meleeBluntDamage
    std::vector<uint8_t> slot;      // by Things DefId value: the WeaponSlot of a weapon, 255 for none (P1)
    int switchSteps[kWeaponSlotCount] = {36, 18, 27};  // CombatRules weaponSwitchSteps, by the slot switched to
    DefId bluntDamage;

    float accuracyBySkill[21] = {};
    float accuracyDistance[4] = {3.0f, 12.0f, 25.0f, 40.0f};
    float minHitChance = 0.0201f;
    int defaultSkill = 4;
    // How much of a standing man facing the shooter a shot lands on when its direction is picked
    // evenly in a circle of radius spreadRadius[k] (metres, at the target) around his chest.
    std::vector<float> spreadRadius;
    std::vector<float> hitShare;
    float playerSpreadDistance = 25.0f;
    float gravity = 0.0109f;
    float recoilRecovery = 0.002f;
    float recoilSkillReduction = 0.025f;
    int maxFlight = 120;
    std::vector<std::string> layerOrder;
    // F9's blasts: ExplosiveRules, and per damage type the explosion block of its def.
    struct Blast {
        float buildingFactor = 1.0f;
        float impassableFactor = 1.0f;
        float wallFactor = 1.0f;
        int fragmentsMin = 1;
        int fragmentsMax = 1;
        float fragmentMinDamage = 1e30f;
    };
    std::vector<Blast> blast;  // by DamageTypes DefId value
    float edgeFactor = 0.4f;
    float stanceFactor[3] = {1.0f, 0.85f, 0.5f};
    float blastHeight = 0.3f;
    float knockMinDamage = 8.0f;
    float knockBaseSteps = 20.0f;
    float knockStepsPerDamage = 0.6f;
    float knockMaxSteps = 90.0f;
    float knockbackMetres = 1.5f;
    // F5's aiming by Shooting skill: the value at skill 0 and at skill 20, linear in between.
    float reactionSteps[2] = {15.0f, 5.0f};
    float lead[2] = {0.4f, 1.0f};
    float leadError[2] = {0.4f, 0.05f};
    float reloadFactor[2] = {1.3f, 0.7f};
    float absorbDivisor = 4.0f;  // ApparelRules: apparel loses 1 hit point per this much damage it absorbs
    // P2's gun kick: it holds for kickHold steps after a shot, then wears off by kickRecovery degrees
    // a step; a spray restarts after kickReset steps without a shot; the kick never passes kickMax.
    int kickHold = 5;
    int kickReset = 15;
    float kickRecovery = 0.2f;
    float kickMax = 10.0f;
    // P2's dodge roll, all in steps but the distance (metres, along the whole roll).
    int rollStartUp = 3;
    int rollActive = 15;
    int rollHitProof = 12;  // the first this many steps of the roll
    int rollRecovery = 9;
    int rollCooldown = 6;
    float rollDistance = 4.0f;

    explicit CombatModel(const DefDatabase& defs, const HealthSystem& health);

    // The spread radius at the target that lands on a standing man with this chance.
    float radiusFor(float chance) const;
    void buildHitShares(const Kind& target);

    const Gun* gun(DefId def) const {
        return def.valid() && def.value < guns.size() && guns[def.value].valid ? &guns[def.value] : nullptr;
    }
    const Apparel* wear(DefId def) const {
        return def.valid() && def.value < apparel.size() && apparel[def.value].valid ? &apparel[def.value] : nullptr;
    }
    const Bullet* bullet(DefId def) const {
        return def.valid() && def.value < bullets.size() && bullets[def.value].valid ? &bullets[def.value] : nullptr;
    }
    const Kind* kind(DefId def) const {
        return def.valid() && def.value < kinds.size() && kinds[def.value].valid ? &kinds[def.value] : nullptr;
    }
};

namespace {

constexpr const char* kCombatThingsKind = "Things";
constexpr const char* kCombatStanceNames[3] = {"Standing", "Crouching", "Prone"};

float combatNum(const Def& def, const std::string& path, double fallback) {
    return static_cast<float>(def.number(path.c_str(), fallback));
}

void combatReadPose(const Def& def, const std::string& base, const std::vector<BodyPartInfo>& parts,
                    CombatModel::Pose& pose) {
    for (int k = 0; k < 3; ++k) {
        pose.aim[k] = static_cast<float>(def.numberAt((base + ".aimPoint").c_str(), static_cast<size_t>(k), k == 2 ? 1.0 : 0.0));
    }
    pose.muzzle = combatNum(def, base + ".muzzleHeight", 1.0);
    const std::string shapesPath = base + ".shapes";
    const size_t n = def.count(shapesPath.c_str());
    for (size_t i = 0; i < n; ++i) {
        const std::string sp = shapesPath + "." + std::to_string(i);
        const std::string label = def.text((sp + ".part").c_str());
        CombatModel::Shape shape;
        for (size_t p = 0; p < parts.size(); ++p) {
            if (parts[p].label == label) {
                shape.part = static_cast<int>(p);
                break;
            }
        }
        if (shape.part < 0) {
            continue;  // a part this body doesn't have
        }
        shape.box = def.has((sp + ".box").c_str());
        const std::string arr = sp + (shape.box ? ".box" : ".capsule");
        for (int k = 0; k < 3; ++k) {
            shape.a[k] = static_cast<float>(def.numberAt(arr.c_str(), static_cast<size_t>(k)));
            shape.b[k] = static_cast<float>(def.numberAt(arr.c_str(), static_cast<size_t>(k + 3)));
        }
        shape.r = shape.box ? 0.0f : static_cast<float>(def.numberAt(arr.c_str(), 6));
        for (const float* pt : {shape.a, shape.b}) {
            const float reach = std::sqrt(pt[0] * pt[0] + pt[1] * pt[1]) + shape.r;
            pose.reach = std::max(pose.reach, reach);
            pose.top = std::max(pose.top, pt[2] + shape.r);
        }
        if (shape.box) {
            // The far corners of a box.
            const float fx = std::max(std::fabs(shape.a[0]), std::fabs(shape.b[0]));
            const float fy = std::max(std::fabs(shape.a[1]), std::fabs(shape.b[1]));
            pose.reach = std::max(pose.reach, std::sqrt(fx * fx + fy * fy));
        }
        pose.shapes.push_back(shape);
    }
}

}  // namespace

CombatModel::CombatModel(const DefDatabase& defs, const HealthSystem& health) {
    guns.resize(defs.size());
    apparel.resize(defs.size());
    bullets.resize(defs.size());
    armorKind.resize(defs.size(), ArmorKind::None);
    kinds.resize(defs.size());
    blast.resize(defs.size());
    melee.resize(defs.size(), 0);
    stuffSharp.resize(defs.size(), 1.0f);
    stuffBlunt.resize(defs.size(), 1.0f);
    slot.resize(defs.size(), 255);

    if (const Def* rules = defs.find("Rules", "ExplosiveRules")) {
        edgeFactor = combatNum(*rules, "edgeFactor", edgeFactor);
        for (size_t i = 0; i < 3; ++i) {
            stanceFactor[i] = static_cast<float>(rules->numberAt("stanceFactor", i, stanceFactor[i]));
        }
        blastHeight = combatNum(*rules, "blastHeight", blastHeight);
        knockMinDamage = combatNum(*rules, "knockdownMinDamage", knockMinDamage);
        knockBaseSteps = combatNum(*rules, "knockdownBaseSteps", knockBaseSteps);
        knockStepsPerDamage = combatNum(*rules, "knockdownStepsPerDamage", knockStepsPerDamage);
        knockMaxSteps = combatNum(*rules, "knockdownMaxSteps", knockMaxSteps);
        knockbackMetres = combatNum(*rules, "knockbackMetres", knockbackMetres);
    }

    if (const Def* rules = defs.find("Rules", "CombatRules")) {
        for (size_t i = 0; i < 21; ++i) {
            accuracyBySkill[i] = static_cast<float>(rules->numberAt("shootingAccuracyBySkill", i, 0.95));
        }
        const char* names[4] = {"touch", "short", "medium", "long"};
        for (int i = 0; i < 4; ++i) {
            accuracyDistance[i] = combatNum(*rules, std::string("accuracyDistances.") + names[i], accuracyDistance[i]);
        }
        minHitChance = combatNum(*rules, "minHitChance", minHitChance);
        defaultSkill = static_cast<int>(rules->integer("defaultShootingSkill", defaultSkill));
        playerSpreadDistance = combatNum(*rules, "playerSpreadDistance", playerSpreadDistance);
        gravity = combatNum(*rules, "gravity", gravity);
        recoilRecovery = combatNum(*rules, "recoilRecoveryPerSecond", recoilRecovery);
        recoilSkillReduction = combatNum(*rules, "recoilSkillReduction", recoilSkillReduction);
        maxFlight = static_cast<int>(rules->integer("maxFlightSeconds", maxFlight));
        for (size_t i = 0; i < rules->count("apparelLayersOuterFirst"); ++i) {
            layerOrder.push_back(rules->textAt("apparelLayersOuterFirst", i));
        }
        const auto bySkill = [&](const char* key, float (&out)[2]) {
            for (size_t i = 0; i < 2; ++i) {
                out[i] = static_cast<float>(rules->numberAt(key, i, out[i]));
            }
        };
        bySkill("reactionStepsBySkill", reactionSteps);
        bySkill("leadBySkill", lead);
        bySkill("leadErrorBySkill", leadError);
        bySkill("reloadTimeFactorBySkill", reloadFactor);
        kickHold = std::max(0, static_cast<int>(rules->integer("recoilKickHoldSteps", kickHold)));
        kickReset = std::max(1, static_cast<int>(rules->integer("recoilKickResetSteps", kickReset)));
        kickRecovery = std::max(0.0f, combatNum(*rules, "recoilKickRecoveryPerStep", kickRecovery));
        kickMax = std::max(0.0f, combatNum(*rules, "recoilKickMaxDegrees", kickMax));
        rollStartUp = std::max(0, static_cast<int>(rules->integer("dodgeRoll.startUpSteps", rollStartUp)));
        rollActive = std::max(1, static_cast<int>(rules->integer("dodgeRoll.rollSteps", rollActive)));
        rollHitProof = std::clamp(static_cast<int>(rules->integer("dodgeRoll.hitProofSteps", rollHitProof)), 0, rollActive);
        rollRecovery = std::max(0, static_cast<int>(rules->integer("dodgeRoll.recoverySteps", rollRecovery)));
        rollCooldown = std::max(0, static_cast<int>(rules->integer("dodgeRoll.cooldownSteps", rollCooldown)));
        rollDistance = std::max(0.0f, combatNum(*rules, "dodgeRoll.distance", rollDistance));
        const char* slotNames[kWeaponSlotCount] = {"Primary", "Sidearm", "Melee"};
        for (int i = 0; i < kWeaponSlotCount; ++i) {
            switchSteps[i] = std::max(
                1, static_cast<int>(rules->integer((std::string("weaponSwitchSteps.") + slotNames[i]).c_str(), switchSteps[i])));
        }
    } else {
        for (size_t i = 0; i < 21; ++i) {
            accuracyBySkill[i] = 0.89f + 0.005f * static_cast<float>(i);
        }
    }

    if (const Def* rules = defs.find("Rules", "ApparelRules")) {
        absorbDivisor = std::max(0.01f, combatNum(*rules, "absorbDamageDivisor", absorbDivisor));
    }

    if (const Def* rules = defs.find("Rules", "QualityRules")) {
        const size_t levels = rules->count("levels");
        for (size_t i = 0; i < levels; ++i) {
            const std::string base = "levels." + std::to_string(i);
            WeaponQuality q;
            q.melee = combatNum(*rules, base + ".meleeDamageFactor", 1.0f);
            q.accuracy = combatNum(*rules, base + ".rangedAccuracyFactor", 1.0f);
            q.damage = combatNum(*rules, base + ".rangedDamageFactor", 1.0f);
            weaponQuality.push_back(q);
            if (rules->text(("levels." + std::to_string(i) + ".def").c_str()) == rules->text("defaultLevel")) {
                defaultQuality = static_cast<int>(i);
            }
        }
    }
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.has("stuffProps")) {
            stuffSharp[id.value] = combatNum(def, "stuffProps.statFactors.meleeSharpDamage", 1.0f);
            stuffBlunt[id.value] = combatNum(def, "stuffProps.statFactors.meleeBluntDamage", 1.0f);
        }
    }

    for (const DefId id : defs.idsOfKind("DamageTypes")) {
        const std::string category = defs.get(id).text("armorCategory");
        armorKind[id.value] = category == "Sharp"   ? ArmorKind::Sharp
                              : category == "Blunt" ? ArmorKind::Blunt
                              : category == "Heat"  ? ArmorKind::Heat
                                                    : ArmorKind::None;
    }
    bluntDamage = defs.idOf("DamageTypes", "Blunt");
    for (const DefId id : defs.idsOfKind("DamageTypes")) {
        const Def& def = defs.get(id);
        if (!def.has("explosion")) {
            continue;
        }
        Blast& b = blast[id.value];
        b.buildingFactor = combatNum(def, "explosion.buildingFactor", 1.0);
        b.impassableFactor = combatNum(def, "explosion.impassableFactor", 1.0);
        b.wallFactor = combatNum(def, "explosion.wallFactor", 1.0);
        b.fragmentsMin = std::max(1, static_cast<int>(def.integer("explosion.fragmentsMin", 1)));
        b.fragmentsMax = std::max(b.fragmentsMin, static_cast<int>(def.integer("explosion.fragmentsMax", 1)));
        b.fragmentMinDamage = combatNum(def, "explosion.fragmentMinDamage", 1e30);
    }

    for (const DefId id : defs.idsOfKind("Projectiles")) {
        const Def& def = defs.get(id);
        Bullet& b = bullets[id.value];
        b.damageType = defs.idOf("DamageTypes", def.text("projectile.damageDef"));
        b.damage = combatNum(def, "projectile.damageAmountBase", 0.0);
        b.penetration = combatNum(def, "projectile.armorPenetrationBase", 0.0);
        b.speed = combatNum(def, "projectile.speed", 0.0);
        b.valid = b.damageType.valid() && b.speed > 0.0f;
        if (def.has("explosion")) {
            b.blastRadius = combatNum(def, "explosion.radius", 0.0);
            b.blastDamage = combatNum(def, "explosion.damage", 0.0);
            b.blastPenetration = combatNum(def, "explosion.armorPenetration", 0.0);
            b.blastType = defs.idOf("DamageTypes", def.text("explosion.damageDef"));
        }
    }

    for (const DefId id : defs.idsOfKind(kCombatThingsKind)) {
        const Def& def = defs.get(id);
        if (def.has("verbs.0.defaultProjectile")) {
            Gun& g = guns[id.value];
            g.projectile = defs.idOf("Projectiles", def.text("verbs.0.defaultProjectile"));
            g.range = combatNum(def, "verbs.0.range", 0.0);
            g.minRange = combatNum(def, "verbs.0.minRange", 0.0);
            g.warmup = static_cast<int>(std::lround(def.number("verbs.0.warmupTime")));
            g.cooldown = static_cast<int>(std::lround(def.number("stats.rangedWeaponCooldown")));
            g.burst = std::max(1, static_cast<int>(def.integer("verbs.0.burstShotCount", 1)));
            g.burstGap = static_cast<int>(std::lround(def.number("verbs.0.secondsBetweenBurstShots")));
            const char* acc[4] = {"stats.accuracyTouch", "stats.accuracyShort", "stats.accuracyMedium", "stats.accuracyLong"};
            for (int i = 0; i < 4; ++i) {
                g.accuracy[i] = combatNum(def, acc[i], 1.0);
            }
            g.recoil = combatNum(def, "recoilPerShot", 0.0);
            g.magazine = std::max(0, static_cast<int>(def.integer("magazineSize", 0)));
            g.reloadTime = std::max(0, static_cast<int>(std::lround(def.number("reloadTime"))));
            for (size_t k = 0; k < def.count("recoilKicks"); ++k) {
                const std::string at = "recoilKicks." + std::to_string(k);
                g.kicks.emplace_back(combatNum(def, at + ".up", 0.0), combatNum(def, at + ".side", 0.0));
            }
            g.valid = bullet(g.projectile) != nullptr;
            if (g.valid) {
                slot[id.value] = static_cast<uint8_t>(def.text("weaponSlot", "Primary") == "Sidearm" ? WeaponSlot::Sidearm
                                                                                                      : WeaponSlot::Primary);
            }
        }
        if (def.has("tools") && !def.has("race") && !def.has("verbs.0.defaultProjectile")) {
            melee[id.value] = 1;
            slot[id.value] = static_cast<uint8_t>(WeaponSlot::Melee);
        }
        if (def.has("apparel")) {
            Apparel& a = apparel[id.value];
            for (size_t i = 0; i < def.count("apparel.bodyPartGroups"); ++i) {
                a.groups.push_back(def.textAt("apparel.bodyPartGroups", i));
            }
            a.outerness = static_cast<int>(layerOrder.size());
            for (size_t i = 0; i < def.count("apparel.layers"); ++i) {
                a.layers.push_back(def.textAt("apparel.layers", i));
                const auto at = std::find(layerOrder.begin(), layerOrder.end(), a.layers.back());
                a.outerness = std::min(a.outerness, static_cast<int>(at - layerOrder.begin()));
            }
            a.valid = true;
        }
        if (def.has("race.hitboxes")) {
            const Def* boxes = defs.find("Hitboxes", def.text("race.hitboxes"));
            Kind& k = kinds[id.value];
            k.parts = health.bodyParts(id);
            k.natural[0] = static_cast<float>(def.number("race.armor.sharp", 0.0));
            k.natural[1] = static_cast<float>(def.number("race.armor.blunt", 0.0));
            k.natural[2] = static_cast<float>(def.number("race.armor.heat", 0.0));
            if (boxes == nullptr || k.parts.empty()) {
                continue;
            }
            for (int s = 0; s < 3; ++s) {
                combatReadPose(*boxes, std::string("stances.") + kCombatStanceNames[s], k.parts, k.poses[s]);
            }
            std::vector<uint8_t> named(k.parts.size(), 0);
            for (const Pose& pose : k.poses) {
                for (const Shape& shape : pose.shapes) {
                    named[static_cast<size_t>(shape.part)] = 1;
                }
            }
            k.landsOn.resize(k.parts.size());
            for (size_t p = 0; p < k.parts.size(); ++p) {
                if (named[p] == 0) {
                    continue;
                }
                // The part, then every outside part below it, stopping at parts with their own hitbox.
                for (size_t q = 0; q < k.parts.size(); ++q) {
                    if (k.parts[q].inside || (q != p && named[q] != 0)) {
                        continue;
                    }
                    int up = static_cast<int>(q);
                    while (up >= 0 && static_cast<size_t>(up) != p &&
                           (up == static_cast<int>(q) || named[static_cast<size_t>(up)] == 0)) {
                        up = k.parts[static_cast<size_t>(up)].parent;
                    }
                    if (up >= 0 && static_cast<size_t>(up) == p) {
                        k.landsOn[p].push_back(static_cast<int>(q));
                    }
                }
            }
            k.valid = !k.poses[0].shapes.empty();
        }
    }

    std::string standard = "Human";
    if (const Def* rules = defs.find("Rules", "CombatRules")) {
        standard = rules->text("standardTarget", standard);
    }
    if (const Kind* target = kind(defs.idOf(kCombatThingsKind, standard))) {
        buildHitShares(*target);
    }
}

namespace {

struct CombatVec {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

float combatDot(const CombatVec& a, const CombatVec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
CombatVec combatSub(const CombatVec& a, const CombatVec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// Earliest t in [0, 1] where p + t * d is inside the box, or -1.
float combatSegmentBox(const CombatVec& p, const CombatVec& d, const float* lo, const float* hi) {
    float t0 = 0.0f;
    float t1 = 1.0f;
    const float o[3] = {p.x, p.y, p.z};
    const float v[3] = {d.x, d.y, d.z};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(v[k]) < 1e-9f) {
            if (o[k] < lo[k] || o[k] > hi[k]) {
                return -1.0f;
            }
            continue;
        }
        float a = (lo[k] - o[k]) / v[k];
        float b = (hi[k] - o[k]) / v[k];
        if (a > b) {
            std::swap(a, b);
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) {
            return -1.0f;
        }
    }
    return t0;
}

// Earliest distance along the unit ray (o, u) to the sphere (c, r), within [0, len]; -1 if none.
float combatRaySphere(const CombatVec& o, const CombatVec& u, const CombatVec& c, float r, float len) {
    const CombatVec oc = combatSub(o, c);
    const float c2 = combatDot(oc, oc) - r * r;
    if (c2 <= 0.0f) {
        return 0.0f;  // starts inside
    }
    const float b = combatDot(oc, u);
    const float h = b * b - c2;
    if (h < 0.0f || b > 0.0f) {
        return -1.0f;
    }
    const float t = -b - std::sqrt(h);
    return t <= len ? t : -1.0f;
}

// Earliest t in [0, 1] where p + t * d touches the capsule (a, b, r), or -1.
float combatSegmentCapsule(const CombatVec& p, const CombatVec& d, const CombatVec& a, const CombatVec& b, float r) {
    const float len = std::sqrt(combatDot(d, d));
    if (len < 1e-9f) {
        return -1.0f;
    }
    const CombatVec u{d.x / len, d.y / len, d.z / len};
    float best = -1.0f;
    const auto keep = [&best](float t) {
        if (t >= 0.0f && (best < 0.0f || t < best)) {
            best = t;
        }
    };
    keep(combatRaySphere(p, u, a, r, len));
    keep(combatRaySphere(p, u, b, r, len));
    const CombatVec ba = combatSub(b, a);
    const float baba = combatDot(ba, ba);
    if (baba > 1e-9f) {
        const CombatVec oa = combatSub(p, a);
        const float bard = combatDot(ba, u);
        const float baoa = combatDot(ba, oa);
        const float rdoa = combatDot(u, oa);
        const float oaoa = combatDot(oa, oa);
        const float qa = baba - bard * bard;
        const float qb = baba * rdoa - baoa * bard;
        const float qc = baba * oaoa - baoa * baoa - r * r * baba;
        if (qc <= 0.0f && baoa >= 0.0f && baoa <= baba) {
            keep(0.0f);  // starts inside the body of the capsule
        } else if (qa > 1e-9f) {
            const float h = qb * qb - qa * qc;
            if (h >= 0.0f) {
                const float t = (-qb - std::sqrt(h)) / qa;
                const float y = baoa + t * bard;
                if (t >= 0.0f && t <= len && y > 0.0f && y < baba) {
                    keep(t);
                }
            }
        }
    }
    return best < 0.0f ? -1.0f : best / len;
}

// a^d for a fractional distance, without pow (every platform must agree): the whole metres by
// multiplying, the rest by a straight line.
float combatPowDistance(float a, float d) {
    const int whole = static_cast<int>(d);
    float result = 1.0f;
    for (int i = 0; i < whole; ++i) {
        result *= a;
    }
    const float rest = d - static_cast<float>(whole);
    return result * (1.0f - rest * (1.0f - a));
}

float combatLerp(float a, float b, float t) { return a + (b - a) * t; }

// A CombatRules value given at Shooting skill 0 and 20, at this skill.
float combatBySkill(const float (&v)[2], int skill) {
    return combatLerp(v[0], v[1], static_cast<float>(std::clamp(skill, 0, 20)) / 20.0f);
}

Stance combatStanceOf(const entt::registry& reg, Entity pawn) {
    const Health* health = reg.try_get<Health>(pawn);
    if (health != nullptr && health->incapacitated()) {
        return Stance::Prone;
    }
    const Posture* posture = reg.try_get<Posture>(pawn);
    return posture != nullptr ? posture->stance : Stance::Standing;
}

}  // namespace

void CombatModel::buildHitShares(const Kind& target) {
    // Look at the man from the front through a 2 cm grid and note how far from his chest each
    // point of him is; the share inside a circle is then that area over the circle's.
    const Pose& pose = target.poses[static_cast<int>(Stance::Standing)];
    constexpr float kCell = 0.02f;
    std::vector<float> distances;
    for (int iz = 0; iz < 110; ++iz) {
        for (int iy = -40; iy < 40; ++iy) {
            const float y = (static_cast<float>(iy) + 0.5f) * kCell;
            const float z = (static_cast<float>(iz) + 0.5f) * kCell;
            const CombatVec from{-3.0f, y, z};
            const CombatVec along{6.0f, 0.0f, 0.0f};
            bool hit = false;
            for (const Shape& shape : pose.shapes) {
                const float t = shape.box ? combatSegmentBox(from, along, shape.a, shape.b)
                                          : combatSegmentCapsule(from, along, CombatVec{shape.a[0], shape.a[1], shape.a[2]},
                                                                 CombatVec{shape.b[0], shape.b[1], shape.b[2]}, shape.r);
                hit = hit || t >= 0.0f;
            }
            if (hit) {
                const float dy = y - pose.aim[1];
                const float dz = z - pose.aim[2];
                distances.push_back(std::sqrt(dy * dy + dz * dz));
            }
        }
    }
    std::sort(distances.begin(), distances.end());
    size_t inside = 0;
    for (float radius = 0.02f; radius < 40.0f; radius *= 1.06f) {
        while (inside < distances.size() && distances[inside] <= radius) {
            ++inside;
        }
        const float area = static_cast<float>(inside) * kCell * kCell;
        spreadRadius.push_back(radius);
        hitShare.push_back(std::min(1.0f, area / (3.14159265f * radius * radius)));
    }
}

float CombatModel::radiusFor(float chance) const {
    if (spreadRadius.empty()) {
        return 0.42f / std::sqrt(chance);  // no Human hitboxes: a man as a disc of the same area
    }
    for (size_t k = 1; k < spreadRadius.size(); ++k) {
        if (hitShare[k] < chance) {
            const float span = hitShare[k - 1] - hitShare[k];
            const float t = span > 0.0f ? (hitShare[k - 1] - chance) / span : 0.0f;
            return combatLerp(spreadRadius[k - 1], spreadRadius[k], std::clamp(t, 0.0f, 1.0f));
        }
    }
    return spreadRadius.back();
}

CombatSystem::CombatSystem(Things& things, Map& map, HealthSystem& health)
    : things_(things),
      map_(map),
      health_(health),
      model_(std::make_unique<CombatModel>(things.defs(), health)),
      apparelStats_(std::make_unique<ApparelStats>(things.defs())) {}

CombatSystem::~CombatSystem() = default;

namespace {

// The gun of a weapon item with its fitted parts' factors put on (P4, docs/design/repair.md): the def's own gun when it
// has no parts, else `scratch` changed. Accuracy is not in the copy (the spread asks `WeaponMods` itself).
const CombatModel::Gun* combatGunOf(const CombatModel& model, const entt::registry& reg, Entity weapon, CombatModel::Gun& scratch) {
    const CombatModel::Gun* base = model.gun(reg.get<Thing>(weapon).def);
    const WeaponMods* mods = base != nullptr ? reg.try_get<WeaponMods>(weapon) : nullptr;
    if (mods == nullptr) {
        return base;
    }
    scratch = *base;
    scratch.range *= mods->range;
    if (base->magazine > 0) {
        const int scaled = static_cast<int>(std::lround(static_cast<float>(base->magazine) * mods->magazine));
        scratch.magazine = mods->magazine > 1.0f ? std::max(base->magazine + 1, scaled) : std::max(1, scaled);
    }
    scratch.reloadTime = static_cast<int>(std::lround(static_cast<float>(base->reloadTime) * mods->reload));
    scratch.cooldown = static_cast<int>(std::lround(static_cast<float>(base->cooldown) * mods->cooldown));
    scratch.warmup = static_cast<int>(std::lround(static_cast<float>(base->warmup) * mods->warmup));
    scratch.recoil *= mods->recoil;
    for (auto& kick : scratch.kicks) {
        kick.first *= mods->recoil;
        kick.second *= mods->recoil;
    }
    return &scratch;
}

}  // namespace

int CombatSystem::shootingSkill(Entity pawn) const {
    if (const Colonist* colonist = things_.registry().try_get<Colonist>(pawn)) {
        for (const SkillRecord& skill : colonist->skills) {
            if (skill.def == "Shooting") {
                return std::clamp(skill.level, 0, 20);
            }
        }
    }
    return std::clamp(model_->defaultSkill, 0, 20);
}

float CombatSystem::spread(DefId weapon, int skill, float distance, bool player, float accuracyFactor) const {
    const CombatModel::Gun* gun = model_->gun(weapon);
    if (gun == nullptr) {
        return 0.0f;
    }
    const auto gunAccuracy = [&](float d) {
        const float* at = model_->accuracyDistance;
        const float* acc = gun->accuracy;
        if (d <= at[0]) {
            return acc[0];
        }
        for (int i = 1; i < 4; ++i) {
            if (d <= at[i]) {
                return combatLerp(acc[i - 1], acc[i], (d - at[i - 1]) / (at[i] - at[i - 1]));
            }
        }
        return acc[3];
    };
    if (player) {
        const float d = model_->playerSpreadDistance;
        const float hit = std::clamp(gunAccuracy(d) * accuracyFactor, model_->minHitChance, 1.0f);
        return model_->radiusFor(hit) / d;
    }
    const float d = std::max(distance, 1.0f);
    const float shooter = combatPowDistance(model_->accuracyBySkill[std::clamp(skill, 0, 20)], d);
    const float hit = std::clamp(shooter * gunAccuracy(d) * accuracyFactor, model_->minHitChance, 1.0f);
    return model_->radiusFor(hit) / d;
}

bool CombatSystem::aimPoint(Entity pawn, float& x, float& y, float& z) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing, Position, Posture>(pawn)) {
        return false;
    }
    const Position& pos = reg.get<Position>(pawn);
    const Posture& posture = reg.get<Posture>(pawn);
    const CombatModel::Kind* kind = model_->kind(reg.get<Thing>(pawn).def);
    const float ground = map_.heightAt(pos.x, pos.y);
    if (kind == nullptr) {
        x = pos.x;
        y = pos.y;
        z = ground + eyeHeight(combatStanceOf(reg, pawn)) * 0.7f;
        return true;
    }
    const float* a = kind->poses[static_cast<int>(combatStanceOf(reg, pawn))].aim;
    x = pos.x + a[0] * posture.facingX - a[1] * posture.facingY;
    y = pos.y + a[0] * posture.facingY + a[1] * posture.facingX;
    z = ground + a[2];
    return true;
}

bool CombatSystem::hitboxesOf(Entity pawn, std::vector<HitboxView>& out) const {
    out.clear();
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing, Position, Posture>(pawn)) {
        return false;
    }
    const Health* health = reg.try_get<Health>(pawn);
    const CombatModel::Kind* kind = model_->kind(reg.get<Thing>(pawn).def);
    if (kind == nullptr || (health != nullptr && health->dead)) {
        return false;
    }
    const Position& pos = reg.get<Position>(pawn);
    const Posture& posture = reg.get<Posture>(pawn);
    const float ground = map_.heightAt(pos.x, pos.y);
    const float fx = posture.facingX;
    const float fy = posture.facingY;
    const auto world = [&](const float* l, float& wx, float& wy, float& wz) {
        wx = pos.x + l[0] * fx - l[1] * fy;
        wy = pos.y + l[0] * fy + l[1] * fx;
        wz = ground + l[2];
    };
    for (const CombatModel::Shape& shape : kind->poses[static_cast<int>(combatStanceOf(reg, pawn))].shapes) {
        if (health_.partHitPoints(pawn, shape.part) <= 0.0f) {
            continue;
        }
        HitboxView v;
        v.part = shape.part;
        v.box = shape.box;
        v.forwardX = fx;
        v.forwardY = fy;
        if (shape.box) {
            const float centre[3] = {0.5f * (shape.a[0] + shape.b[0]), 0.5f * (shape.a[1] + shape.b[1]),
                                     0.5f * (shape.a[2] + shape.b[2])};
            world(centre, v.ax, v.ay, v.az);
            v.bx = 0.5f * (shape.b[0] - shape.a[0]);
            v.by = 0.5f * (shape.b[1] - shape.a[1]);
            v.bz = 0.5f * (shape.b[2] - shape.a[2]);
        } else {
            world(shape.a, v.ax, v.ay, v.az);
            world(shape.b, v.bx, v.by, v.bz);
            v.radius = shape.r;
        }
        out.push_back(v);
    }
    return true;
}

TraceHit CombatSystem::trace(float ax, float ay, float az, float bx, float by, float bz, int level, Entity skipPawn,
                             CellPos skipCell, bool pawns) const {
    TraceHit best;
    const float dx = bx - ax;
    const float dy = by - ay;
    const float dz = bz - az;
    const auto at = [&](float t, TraceHit& h) {
        h.t = t;
        h.x = ax + dx * t;
        h.y = ay + dy * t;
        h.z = az + dz * t;
    };

    // The ground and what stands on it, cell by cell along the line (a grid walk).
    {
        int cx = static_cast<int>(std::floor(ax));
        int cy = static_cast<int>(std::floor(ay));
        const int stepX = dx > 0.0f ? 1 : -1;
        const int stepY = dy > 0.0f ? 1 : -1;
        constexpr float kFar = std::numeric_limits<float>::max();
        const float deltaX = dx != 0.0f ? 1.0f / std::fabs(dx) : kFar;
        const float deltaY = dy != 0.0f ? 1.0f / std::fabs(dy) : kFar;
        float nextX = dx != 0.0f ? (static_cast<float>(cx + (dx > 0.0f ? 1 : 0)) - ax) / dx : kFar;
        float nextY = dy != 0.0f ? (static_cast<float>(cy + (dy > 0.0f ? 1 : 0)) - ay) / dy : kFar;
        float tIn = 0.0f;
        const int cells = static_cast<int>(std::fabs(dx) + std::fabs(dy)) + 3;
        for (int i = 0; i < cells; ++i) {
            const CellPos c{cx, cy, level};
            if (!map_.inBounds(c)) {
                break;
            }
            const float tOut = std::min(std::min(nextX, nextY), 1.0f);
            const float z0 = az + dz * tIn;
            const float z1 = az + dz * tOut;
            const uint8_t obstacle = map_.obstacleDm(c);
            const bool rock = map_.terrain(c) == Terrain::Rock;
            if (rock) {
                at(tIn, best);
                best.kind = ProjectileHitKind::Rock;
                break;
            }
            if (obstacle > 0 && !(c == skipCell)) {
                const float top = obstacle == kObstacleTallDm ? kFar : map_.groundHeight(c) + 0.1f * static_cast<float>(obstacle);
                if (std::min(z0, z1) < top) {
                    const float t = z0 < top ? tIn : tIn + (tOut - tIn) * (z0 - top) / (z0 - z1);
                    at(t, best);
                    best.kind = ProjectileHitKind::Building;
                    best.thing = map_.buildingAt(c);
                    break;
                }
            }
            const float g0 = map_.heightAt(ax + dx * tIn, ay + dy * tIn);
            const float g1 = map_.heightAt(ax + dx * tOut, ay + dy * tOut);
            if (z1 < g1) {
                const float above0 = z0 - g0;
                const float above1 = z1 - g1;
                const float t = above0 <= 0.0f ? tIn : tIn + (tOut - tIn) * above0 / (above0 - above1);
                at(t, best);
                best.kind = ProjectileHitKind::Ground;
                break;
            }
            if (tOut >= 1.0f) {
                break;
            }
            if (nextX < nextY) {
                cx += stepX;
                tIn = nextX;
                nextX += deltaX;
            } else {
                cy += stepY;
                tIn = nextY;
                nextY += deltaY;
            }
        }
    }

    // Pawns' hitboxes, nearer than whatever the line met above. Ties go to the lower id so the
    // registry's order can't matter.
    const entt::registry& reg = things_.registry();
    const float len2 = dx * dx + dy * dy;
    for (const auto [pawn, thing, pos, posture] : reg.view<const Thing, const Position, const Posture>().each()) {
        if (!pawns || pawn == skipPawn || pos.level != level) {
            continue;
        }
        if (const DodgeRoll* roll = reg.try_get<DodgeRoll>(pawn); roll != nullptr && roll->proof) {
            continue;  // rolling out of the way (P2): the bullet goes through
        }
        const Health* health = reg.try_get<Health>(pawn);
        const CombatModel::Kind* kind = model_->kind(thing.def);
        if (kind == nullptr || (health != nullptr && health->dead)) {
            continue;
        }
        const CombatModel::Pose& pose = kind->poses[static_cast<int>(combatStanceOf(reg, pawn))];
        // Quick test: the line's closest approach to the pawn on the ground.
        const float px = pos.x - ax;
        const float py = pos.y - ay;
        float s = len2 > 0.0f ? (px * dx + py * dy) / len2 : 0.0f;
        s = std::clamp(s, 0.0f, 1.0f);
        const float ex = ax + dx * s - pos.x;
        const float ey = ay + dy * s - pos.y;
        if (ex * ex + ey * ey > pose.reach * pose.reach) {
            continue;
        }
        const float ground = map_.heightAt(pos.x, pos.y);
        if (std::min(az, bz) > ground + pose.top || std::max(az, bz) < ground) {
            continue;
        }
        // Into the pawn's frame: x forward, y left, z up from the ground under it.
        const float fx = posture.facingX;
        const float fy = posture.facingY;
        const float rx = ax - pos.x;
        const float ry = ay - pos.y;
        const CombatVec lp{rx * fx + ry * fy, -rx * fy + ry * fx, az - ground};
        const CombatVec ld{dx * fx + dy * fy, -dx * fy + dy * fx, dz};
        for (const CombatModel::Shape& shape : pose.shapes) {
            float t = -1.0f;
            if (shape.box) {
                t = combatSegmentBox(lp, ld, shape.a, shape.b);
            } else {
                t = combatSegmentCapsule(lp, ld, CombatVec{shape.a[0], shape.a[1], shape.a[2]},
                                         CombatVec{shape.b[0], shape.b[1], shape.b[2]}, shape.r);
            }
            const bool better = best.kind == ProjectileHitKind::None
                                    ? t <= best.t
                                    : (t < best.t || (t == best.t && best.kind == ProjectileHitKind::Pawn && pawn < best.thing));
            if (t < 0.0f || !better) {
                continue;
            }
            if (health_.partHitPoints(pawn, shape.part) <= 0.0f) {
                continue;  // that part is gone: nothing there to hit
            }
            at(t, best);
            best.kind = ProjectileHitKind::Pawn;
            best.thing = pawn;
            best.part = shape.part;
        }
    }
    return best;
}

Entity CombatSystem::fire(Entity pawn, float x, float y, float z, float spreadSlope, Rng& rng, float kickUp,
                          float kickSide) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Gear, Position, Posture>(pawn)) {
        return kNoEntity;
    }
    const Entity weapon = reg.get<Gear>(pawn).weapon;
    if (weapon == kNoEntity || !reg.valid(weapon)) {
        return kNoEntity;
    }
    const DefId weaponDef = reg.get<Thing>(weapon).def;
    const Position pos = reg.get<Position>(pawn);
    const CombatModel::Kind* kind = model_->kind(reg.get<Thing>(pawn).def);
    const Stance stance = combatStanceOf(reg, pawn);
    const float muzzle = kind != nullptr ? kind->poses[static_cast<int>(stance)].muzzle : eyeHeight(stance) - 0.15f;
    const Entity shot = launch(pawn, weaponDef, pos.x, pos.y, map_.heightAt(pos.x, pos.y) + muzzle, pos.level, cellOf(pos), x, y,
                               z, spreadSlope, rng, kickUp, kickSide, &reg.get<Posture>(pawn));
    if (shot != kNoEntity) {
        reg.get<Projectile>(shot).damageFactor = rangedDamageFactor(weapon);
    }
    return shot;
}

Entity CombatSystem::fireFrom(Entity launcher, DefId weapon, float ox, float oy, float oz, int level, CellPos startCell,
                              float x, float y, float z, float spreadSlope, Rng& rng) {
    return launch(launcher, weapon, ox, oy, oz, level, startCell, x, y, z, spreadSlope, rng, 0.0f, 0.0f, nullptr);
}

Entity CombatSystem::launch(Entity pawn, DefId weaponDef, float ox, float oy, float oz, int level, CellPos startCell,
                            float x, float y, float z, float spreadSlope, Rng& rng, float kickUp, float kickSide,
                            Posture* posture) {
    entt::registry& reg = things_.registry();
    const CombatModel::Gun* gun = model_->gun(weaponDef);
    if (gun == nullptr) {
        return kNoEntity;
    }
    const CombatModel::Bullet& bullet = *model_->bullet(gun->projectile);

    float dx = x - ox;
    float dy = y - oy;
    const float dz = z - oz;
    float flat = std::sqrt(dx * dx + dy * dy);
    if (flat < 0.01f) {
        dx = posture != nullptr ? posture->facingX : 1.0f;
        dy = posture != nullptr ? posture->facingY : 0.0f;
        flat = 1.0f;
    } else if (posture != nullptr) {
        posture->facingX = dx / flat;
        posture->facingY = dy / flat;
    }
    // Aim above the point so the drop brings the shot onto it (the lower of the two arcs).
    const float v = bullet.speed;
    const float g = model_->gravity;
    const float v2 = v * v;
    const float disc = v2 * v2 - g * (g * flat * flat + 2.0f * dz * v2);
    const float slope = disc >= 0.0f ? (v2 - std::sqrt(disc)) / (g * flat) : v2 / (g * flat);
    CombatVec dir{dx / flat, dy / flat, slope};
    float n = std::sqrt(combatDot(dir, dir));
    dir = {dir.x / n, dir.y / n, dir.z / n};

    // Spread: a direction picked evenly in a circle around the aim.
    CombatVec right{dir.y, -dir.x, 0.0f};
    n = std::sqrt(combatDot(right, right));
    right = n > 1e-6f ? CombatVec{right.x / n, right.y / n, 0.0f} : CombatVec{0.0f, -1.0f, 0.0f};
    const CombatVec up{right.y * dir.z - right.z * dir.y, right.z * dir.x - right.x * dir.z,
                       right.x * dir.y - right.y * dir.x};
    if (spreadSlope > 0.0f) {
        float u = 0.0f;
        float w = 0.0f;
        for (int tries = 0; tries < 16; ++tries) {
            const float cu = 2.0f * rng.unit() - 1.0f;
            const float cw = 2.0f * rng.unit() - 1.0f;
            if (cu * cu + cw * cw <= 1.0f) {
                u = cu;
                w = cw;
                break;
            }
        }
        dir = {dir.x + (right.x * u + up.x * w) * spreadSlope, dir.y + (right.y * u + up.y * w) * spreadSlope,
               dir.z + (right.z * u + up.z * w) * spreadSlope};
        n = std::sqrt(combatDot(dir, dir));
        dir = {dir.x / n, dir.y / n, dir.z / n};
    }
    // The player's gun kick (P2): the shot goes up and to the right of the aim.
    if (kickUp != 0.0f || kickSide != 0.0f) {
        dir = {dir.x + right.x * kickSide + up.x * kickUp, dir.y + right.y * kickSide + up.y * kickUp,
               dir.z + right.z * kickSide + up.z * kickUp};
        n = std::sqrt(combatDot(dir, dir));
        dir = {dir.x / n, dir.y / n, dir.z / n};
    }

    const Entity shot = reg.create();
    Projectile p;
    p.def = gun->projectile;
    p.weapon = weaponDef;
    p.launcher = pawn;
    p.x = ox;
    p.y = oy;
    p.z = oz;
    p.vx = dir.x * v;
    p.vy = dir.y * v;
    p.vz = dir.z * v;
    p.level = level;
    p.startCell = startCell;
    p.seed = (static_cast<uint64_t>(rng.next()) << 32) | rng.next();
    reg.emplace<Projectile>(shot, p);
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::ShotFired;
        e.thing = pawn;
        e.def = weaponDef;
        e.target = shot;
        e.x = ox;
        e.y = oy;
        e.z = oz;
        events_->push_back(std::move(e));
    }
    return shot;
}

bool CombatSystem::shoot(Entity pawn, Entity target, float x, float y, float z, bool player, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!things_.spawned(pawn) || !reg.all_of<Posture>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    const Gear* gear = reg.try_get<Gear>(pawn);
    if (gear == nullptr || gear->weapon == kNoEntity || model_->gun(reg.get<Thing>(gear->weapon).def) == nullptr) {
        return fail("that pawn has no gun");
    }
    if (health_.incapacitated(pawn)) {
        return fail("that pawn can't shoot (downed or dead)");
    }
    if (reg.all_of<KnockedDown>(pawn)) {
        return fail("that pawn is knocked down");
    }
    if (reg.all_of<Stunned>(pawn)) {
        return fail("that pawn is stunned");
    }
    if (target != kNoEntity && (target == pawn || !things_.spawned(target) || !reg.all_of<Posture>(target))) {
        return fail("the target is not a pawn on the map");
    }
    if (rollLocked(pawn)) {
        return fail("rolling");
    }
    if (switching(pawn)) {
        return fail("switching weapons");
    }
    Shooter& s = reg.get_or_emplace<Shooter>(pawn);
    if (s.phase == ShotPhase::Reloading) {
        return fail("reloading");
    }
    if (s.phase == ShotPhase::Firing || s.phase == ShotPhase::Cooling) {
        return fail("not ready to fire yet");
    }
    CombatModel::Gun moddedGun;
    const CombatModel::Gun& gun = *combatGunOf(*model_, reg, gear->weapon, moddedGun);
    if (gun.magazine > 0 && roundsLeft(pawn) <= 0) {
        startReload(pawn, s, gear->weapon, 0);
        return fail("reloading");
    }
    s.target = target;
    s.aimX = x;
    s.aimY = y;
    s.aimZ = z;
    s.player = player;
    if (s.phase == ShotPhase::Ready || player) {
        s.shotsLeft = gun.burst;
        s.phase = player || gun.warmup <= 0 ? ShotPhase::Firing : ShotPhase::Aiming;
        s.stepsLeft = player ? 0 : gun.warmup;
    }
    return true;
}

bool CombatSystem::setStance(Entity pawn, Stance stance) {
    Posture* posture = things_.registry().try_get<Posture>(pawn);
    if (posture == nullptr || things_.registry().any_of<KnockedDown, Stunned>(pawn)) {
        return false;
    }
    posture->stance = stance;
    return true;
}

void CombatSystem::updateShooter(Entity pawn, Shooter& s, Rng& rng) {
    entt::registry& reg = things_.registry();
    s.recoil = std::max(0.0f, s.recoil - model_->recoilRecovery);
    const Gear* gear = reg.try_get<Gear>(pawn);
    CombatModel::Gun moddedGun;
    const CombatModel::Gun* gun =
        gear != nullptr && gear->weapon != kNoEntity ? combatGunOf(*model_, reg, gear->weapon, moddedGun) : nullptr;
    if (s.phase == ShotPhase::Aiming || s.phase == ShotPhase::Firing || s.phase == ShotPhase::Reloading) {
        if (gun == nullptr || (s.phase != ShotPhase::Reloading &&
                                  (health_.incapacitated(pawn) || !things_.spawned(pawn) || reg.any_of<KnockedDown, Stunned>(pawn)))) {
            s.phase = ShotPhase::Ready;  // the shot (or reload) is off
            s.shotsLeft = 0;
            s.stepsLeft = 0;
            return;
        }
    }
    switch (s.phase) {
        case ShotPhase::Ready:
            return;
        case ShotPhase::Cooling:
            if (--s.stepsLeft <= 0) {
                s.phase = ShotPhase::Ready;
                s.stepsLeft = 0;
            }
            return;
        case ShotPhase::Reloading:
            if (health_.incapacitated(pawn)) {
                return;  // a downed pawn's hands stop; it goes on when it gets up
            }
            if (--s.stepsLeft <= 0) {
                s.phase = ShotPhase::Ready;
                s.stepsLeft = 0;
                reg.emplace_or_replace<Magazine>(gear->weapon, Magazine{gun->magazine});
            }
            return;
        case ShotPhase::Aiming:
            if (--s.stepsLeft > 0) {
                return;
            }
            s.phase = ShotPhase::Firing;
            s.stepsLeft = 0;
            break;
        case ShotPhase::Firing:
            if (s.stepsLeft > 0 && --s.stepsLeft > 0) {
                return;
            }
            break;
    }
    // Fire one shot of the burst, at the target as it stands now if it still can be hit.
    if (s.target != kNoEntity) {
        const Health* health = reg.valid(s.target) ? reg.try_get<Health>(s.target) : nullptr;
        if (health != nullptr && health->dead) {
            s.target = kNoEntity;
        } else if (!aimPoint(s.target, s.aimX, s.aimY, s.aimZ)) {
            s.target = kNoEntity;
        }
    }
    const Position& pos = reg.get<Position>(pawn);
    const float dx = s.aimX - pos.x;
    const float dy = s.aimY - pos.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const int skill = shootingSkill(pawn);
    const DefId weaponDef = reg.get<Thing>(gear->weapon).def;
    // Leading a moving target (F5): by how much follows skill, with an error that shrinks with it.
    float fx = s.aimX;
    float fy = s.aimY;
    float fz = s.aimZ;
    if (s.target != kNoEntity && !s.player) {
        const Mover* mover = reg.try_get<Mover>(s.target);
        if (mover != nullptr && mover->vx * mover->vx + mover->vy * mover->vy > 1e-8f) {
            const float lead = combatBySkill(model_->lead, skill) +
                               (2.0f * rng.unit() - 1.0f) * combatBySkill(model_->leadError, skill);
            leadPoint(pawn, s.target, lead, fx, fy, fz);
        }
    }
    // The player's gun kick (P2): this shot goes where the shots before it have lifted the aim, then
    // its own kick is added for the next.
    constexpr float kDegreesToSlope = 0.0174533f;  // small angles: the tangent is the angle
    float kickUp = 0.0f;
    float kickSide = 0.0f;
    const bool kicks = s.player && !gun->kicks.empty();
    if (kicks) {
        if (const RecoilKick* kick = reg.try_get<RecoilKick>(pawn)) {
            kickUp = kick->up * kDegreesToSlope;
            kickSide = kick->side * kDegreesToSlope;
        }
    }
    const float accuracyFactor =
        weather_ != nullptr ? weather_->accuracyAt(CellPos{static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)), pos.level}) : 1.0f;
    float steady = talents_ != nullptr ? static_cast<float>(talents_->statFactor(pawn, "aimSpread")) : 1.0f;
    if (inspirations_ != nullptr) {
        steady *= inspirations_->statFactor(pawn, "aimSpread");
    }
    const WeaponMods* mods = reg.try_get<WeaponMods>(gear->weapon);
    const float partsAccuracy = mods != nullptr ? mods->accuracy : 1.0f;
    const float quality = rangedAccuracyFactor(gear->weapon);
    fire(pawn, fx, fy, fz, spread(weaponDef, skill, distance, s.player, accuracyFactor * quality * partsAccuracy) * steady + s.recoil, rng,
         kickUp, kickSide);
    const float skillFactor = std::max(0.0f, 1.0f - model_->recoilSkillReduction * static_cast<float>(skill));
    s.recoil += gun->recoil * skillFactor;
    if (kicks) {
        RecoilKick& kick = reg.get_or_emplace<RecoilKick>(pawn);
        const auto& entry = gun->kicks[static_cast<size_t>(std::min<int>(kick.shots, static_cast<int>(gun->kicks.size()) - 1))];
        kick.up += entry.first * skillFactor;
        kick.side += entry.second * skillFactor;
        const float length = std::sqrt(kick.up * kick.up + kick.side * kick.side);
        if (length > model_->kickMax && length > 0.0f) {
            kick.up *= model_->kickMax / length;
            kick.side *= model_->kickMax / length;
        }
        ++kick.shots;
        kick.sinceShot = 0;
    }
    if (gun->magazine > 0) {
        Magazine& magazine = reg.get_or_emplace<Magazine>(gear->weapon, Magazine{gun->magazine});
        if (--magazine.rounds <= 0) {
            magazine.rounds = 0;
            startReload(pawn, s, gear->weapon, gun->cooldown);  // the burst ends with the magazine
            return;
        }
    }
    if (--s.shotsLeft > 0) {
        s.stepsLeft = std::max(1, gun->burstGap);
        return;
    }
    const int cooldown = inspirations_ != nullptr
                             ? static_cast<int>(std::lround(static_cast<float>(gun->cooldown) * inspirations_->statFactor(pawn, "fireCooldown")))
                             : gun->cooldown;
    s.phase = cooldown > 0 ? ShotPhase::Cooling : ShotPhase::Ready;
    s.stepsLeft = cooldown;
}

void CombatSystem::startReload(Entity pawn, Shooter& s, Entity weapon, int extraSteps) {
    entt::registry& reg = things_.registry();
    CombatModel::Gun moddedGun;
    const CombatModel::Gun& gun = *combatGunOf(*model_, reg, weapon, moddedGun);
    const float factor = combatBySkill(model_->reloadFactor, shootingSkill(pawn)) *
                         (talents_ != nullptr ? static_cast<float>(talents_->statFactor(pawn, "reloadTime")) : 1.0f);
    s.phase = ShotPhase::Reloading;
    s.shotsLeft = 0;
    s.stepsLeft = std::max(1, extraSteps + static_cast<int>(std::lround(static_cast<float>(gun.reloadTime) * factor)));
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.action = PawnAction::Reload;
        e.thing = pawn;
        e.def = reg.get<Thing>(pawn).def;
        e.target = weapon;
        if (const Position* pos = reg.try_get<Position>(pawn)) {
            e.cell = cellOf(*pos);
        }
        events_->push_back(std::move(e));
    }
}

bool CombatSystem::reload(Entity pawn, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!things_.spawned(pawn) || !reg.all_of<Posture>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    const Gear* gear = reg.try_get<Gear>(pawn);
    CombatModel::Gun moddedGun;
    const CombatModel::Gun* gun =
        gear != nullptr && gear->weapon != kNoEntity ? combatGunOf(*model_, reg, gear->weapon, moddedGun) : nullptr;
    if (gun == nullptr || gun->magazine <= 0) {
        return fail("that pawn has no gun to reload");
    }
    if (health_.incapacitated(pawn)) {
        return fail("that pawn can't reload (downed or dead)");
    }
    if (rollLocked(pawn)) {
        return fail("rolling");
    }
    if (switching(pawn)) {
        return fail("switching weapons");
    }
    Shooter& s = reg.get_or_emplace<Shooter>(pawn);
    if (s.phase == ShotPhase::Aiming || s.phase == ShotPhase::Firing || s.phase == ShotPhase::Reloading) {
        return fail(s.phase == ShotPhase::Reloading ? "reloading already" : "busy shooting");
    }
    if (roundsLeft(pawn) >= gun->magazine) {
        return fail("the magazine is full");
    }
    startReload(pawn, s, gear->weapon, s.phase == ShotPhase::Cooling ? s.stepsLeft : 0);
    return true;
}

int CombatSystem::roundsLeft(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    if (gear == nullptr || gear->weapon == kNoEntity) {
        return 0;
    }
    CombatModel::Gun moddedGun;
    const CombatModel::Gun* gun = combatGunOf(*model_, reg, gear->weapon, moddedGun);
    if (gun == nullptr || gun->magazine <= 0) {
        return 0;
    }
    const Magazine* magazine = reg.try_get<Magazine>(gear->weapon);
    return magazine != nullptr ? magazine->rounds : gun->magazine;
}

int CombatSystem::magazineSize(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    CombatModel::Gun moddedGun;
    const CombatModel::Gun* gun =
        gear != nullptr && gear->weapon != kNoEntity ? combatGunOf(*model_, reg, gear->weapon, moddedGun) : nullptr;
    return gun != nullptr ? gun->magazine : 0;
}

bool CombatSystem::reloading(Entity pawn) const {
    const Shooter* s = things_.registry().try_get<Shooter>(pawn);
    return s != nullptr && s->phase == ShotPhase::Reloading;
}

void CombatSystem::cancelAim(Entity pawn) {
    if (Shooter* s = things_.registry().try_get<Shooter>(pawn); s != nullptr && s->phase == ShotPhase::Aiming) {
        s->phase = ShotPhase::Ready;
        s->shotsLeft = 0;
        s->stepsLeft = 0;
    }
}

int CombatSystem::reactionSteps(Entity pawn) const {
    return std::max(0, static_cast<int>(std::lround(combatBySkill(model_->reactionSteps, shootingSkill(pawn)))));
}

float CombatSystem::gravity() const {
    return model_->gravity;
}

bool CombatSystem::leadPoint(Entity shooter, Entity target, float lead, float& x, float& y, float& z) const {
    const entt::registry& reg = things_.registry();
    if (!reg.all_of<Position, Gear>(shooter)) {
        return false;
    }
    const Entity weapon = reg.get<Gear>(shooter).weapon;
    if (weapon == kNoEntity) {
        return false;
    }
    const Position& from = reg.get<Position>(shooter);
    return leadPointFrom(from.x, from.y, reg.get<Thing>(weapon).def, target, lead, x, y, z);
}

bool CombatSystem::leadPointFrom(float fromX, float fromY, DefId weapon, Entity target, float lead, float& x, float& y,
                                 float& z) const {
    const entt::registry& reg = things_.registry();
    if (!aimPoint(target, x, y, z)) {
        return false;
    }
    const CombatModel::Gun* gun = model_->gun(weapon);
    if (gun == nullptr) {
        return false;
    }
    const Mover* mover = reg.try_get<Mover>(target);
    if (mover == nullptr) {
        return true;
    }
    // The bullet flies its first step in the step it is fired and the target moves at the start of
    // each step after, so it meets the target about half a step short of distance / speed. Twice
    // round, since the lead changes the distance.
    const float speed = model_->bullet(gun->projectile)->speed;
    const float x0 = x;
    const float y0 = y;
    const float ground0 = map_.heightAt(x0, y0);
    for (int i = 0; i < 2; ++i) {
        const float dx = x - fromX;
        const float dy = y - fromY;
        const float t = std::max(0.0f, std::sqrt(dx * dx + dy * dy) / speed - 0.5f);
        x = x0 + mover->vx * t * lead;
        y = y0 + mover->vy * t * lead;
    }
    z += map_.heightAt(x, y) - ground0;
    return true;
}

bool CombatSystem::gunStats(DefId weapon, GunStats& out) const {
    const CombatModel::Gun* gun = model_->gun(weapon);
    if (gun == nullptr) {
        return false;
    }
    out.range = gun->range;
    out.minRange = gun->minRange;
    out.warmup = gun->warmup;
    out.cooldown = gun->cooldown;
    out.burst = gun->burst;
    out.burstGap = gun->burstGap;
    return true;
}

int CombatSystem::shotReadyIn(Entity pawn) const {
    const Shooter* s = things_.registry().try_get<Shooter>(pawn);
    if (s == nullptr || s->phase == ShotPhase::Ready) {
        return 0;
    }
    if (s->phase == ShotPhase::Cooling || s->phase == ShotPhase::Reloading) {
        return s->stepsLeft;
    }
    const Gear* gear = things_.registry().try_get<Gear>(pawn);
    CombatModel::Gun moddedGun;
    const CombatModel::Gun* gun = gear != nullptr && gear->weapon != kNoEntity
                                      ? combatGunOf(*model_, things_.registry(), gear->weapon, moddedGun)
                                      : nullptr;
    if (gun == nullptr) {
        return s->stepsLeft;
    }
    return s->stepsLeft + std::max(0, s->shotsLeft - 1) * std::max(1, gun->burstGap) + gun->cooldown;
}

void CombatSystem::emitHit(Entity projectile, const Projectile& p, const TraceHit& hit, float amount) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = EventType::ProjectileHit;
    e.thing = projectile;
    e.def = p.def;
    e.hit = hit.kind;
    e.target = hit.thing;
    e.part = hit.part;
    e.amount = amount;
    e.x = hit.x;
    e.y = hit.y;
    e.z = hit.z;
    e.cell = CellPos{static_cast<int>(std::floor(hit.x)), static_cast<int>(std::floor(hit.y)), p.level};
    events_->push_back(std::move(e));
}

void CombatSystem::impact(Entity projectile, Projectile& p, const TraceHit& hit) {
    entt::registry& reg = things_.registry();
    const CombatModel::Bullet& bullet = *model_->bullet(p.def);
    TraceHit h = hit;
    if (bullet.blastRadius > 0.0f) {
        // A rocket does nothing on its own: wherever it ends (pawn, wall, ground, out of range) it bursts.
        emitHit(projectile, p, h, 0.0f);
        Explosion ex;
        ex.x = hit.x;
        ex.y = hit.y;
        ex.z = hit.z;
        ex.level = p.level;
        ex.radius = bullet.blastRadius;
        ex.damage = bullet.blastDamage;
        ex.damageType = bullet.blastType;
        ex.penetration = bullet.blastPenetration;
        ex.source = projectile;
        ex.instigator = p.launcher;
        ex.seed = p.seed;
        explode(ex);
        return;
    }
    switch (hit.kind) {
        case ProjectileHitKind::Building: {
            HitPoints* hp = reg.valid(hit.thing) ? reg.try_get<HitPoints>(hit.thing) : nullptr;
            const int damage = hp != nullptr ? std::max(1, static_cast<int>(std::lround(bullet.damage * p.damageFactor))) : 0;
            emitHit(projectile, p, h, static_cast<float>(damage));
            if (hp != nullptr) {
                hp->current -= damage;
                if (hp->current <= 0) {
                    if (beforeDestroy_) {
                        beforeDestroy_(hit.thing);
                    }
                    things_.destroy(hit.thing);
                }
            }
            break;
        }
        case ProjectileHitKind::Pawn: {
            if (bullet.damage <= 0.0f) {
                emitHit(projectile, p, h, 0.0f);
                break;
            }
            const PawnHitResult result = hitPawn(hit.thing, hit.part, bullet.damageType, bullet.damage * p.damageFactor, bullet.penetration * p.damageFactor, p.seed);
            h.part = result.part;
            if (result.deflected) {
                h.kind = ProjectileHitKind::Armor;
                emitHit(projectile, p, h, 0.0f);
                wearApparel(hit.thing, result);
                return;
            }
            emitHit(projectile, p, h, result.amount);
            health_.applyHit(hit.thing, result.damageType, result.amount, result.part);
            wearApparel(hit.thing, result);
            if (p.launcher != kNoEntity) {
                noteHurt(hit.thing, p.launcher);
            }
            break;
        }
        default:
            emitHit(projectile, p, h, 0.0f);
            break;
    }
}

// RimWorld's armor rule, per apparel piece covering the part hit, outermost layer first: armor
// less penetration is the share of hits it changes; half of those bounce off (damage 0), the rest
// do half damage and a sharp hit turns blunt. Bullets, melee blows and blast fragments all use it.
PawnHitResult CombatSystem::armorPass(Entity pawn, int part, DefId damageType, float amount, float penetration,
                                      Rng& rng) const {
    const entt::registry& reg = things_.registry();
    PawnHitResult result;
    result.part = part;
    result.damageType = damageType;
    result.amount = amount;
    const CombatModel::Kind* kind = model_->kind(reg.get<Thing>(pawn).def);
    if (kind == nullptr) {
        return result;
    }
    const CombatModel::ArmorKind armorKind =
        damageType.value < model_->armorKind.size() ? model_->armorKind[damageType.value] : CombatModel::ArmorKind::None;
    const Gear* gear = reg.try_get<Gear>(pawn);
    if (armorKind == CombatModel::ArmorKind::None) {
        return result;
    }
    std::vector<std::pair<int, Entity>> layers;
    for (const Entity worn : gear != nullptr ? gear->apparel : std::vector<Entity>{}) {
        const CombatModel::Apparel* a = model_->wear(reg.get<Thing>(worn).def);
        if (a == nullptr) {
            continue;
        }
        bool covers = false;
        for (const std::string& group : kind->parts[static_cast<size_t>(part)].groups) {
            covers = covers || std::find(a->groups.begin(), a->groups.end(), group) != a->groups.end();
        }
        if (covers) {
            layers.emplace_back(a->outerness, worn);
        }
    }
    std::stable_sort(layers.begin(), layers.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    const ArmorType armorType = armorKind == CombatModel::ArmorKind::Sharp   ? ArmorType::Sharp
                                : armorKind == CombatModel::ArmorKind::Blunt ? ArmorType::Blunt
                                                                             : ArmorType::Heat;
    // A piece that stops or softens a hit takes 1 hit point of damage for every 4 points of the damage
    // that reached it (C15, RimWorld wiki "Apparel"), rounded at random.
    const auto wearOut = [&](Entity worn, float reaching) {
        const float lost = reaching / model_->absorbDivisor;
        int points = static_cast<int>(lost);
        if (rng.unit() < lost - static_cast<float>(points)) {
            ++points;
        }
        result.wear.emplace_back(worn, points);
    };
    for (const auto& [outerness, worn] : layers) {
        (void)outerness;
        const float armor = std::max(0.0f, apparelStats_->armor(reg, worn, armorType) - penetration);
        const float roll = rng.unit();
        if (roll < armor * 0.5f) {
            result.deflected = true;
            wearOut(worn, result.amount);
            result.amount = 0.0f;
            return result;
        }
        if (roll < armor) {
            wearOut(worn, result.amount);
            result.amount *= 0.5f;
            if (armorKind == CombatModel::ArmorKind::Sharp && model_->bluntDamage.valid()) {
                result.damageType = model_->bluntDamage;
            }
        }
    }
    // The body's own armor (mechanoid hulls, chitin) is the innermost layer; nothing wears out.
    const float natural = std::max(0.0f, kind->natural[static_cast<size_t>(armorType)] - penetration);
    if (natural > 0.0f) {
        const float roll = rng.unit();
        if (roll < natural * 0.5f) {
            result.deflected = true;
            result.amount = 0.0f;
        } else if (roll < natural) {
            result.amount *= 0.5f;
            if (armorKind == CombatModel::ArmorKind::Sharp && model_->bluntDamage.valid()) {
                result.damageType = model_->bluntDamage;
            }
        }
    }
    return result;
}

PawnHitResult CombatSystem::hitPawn(Entity pawn, int hitboxPart, DefId damageType, float amount, float penetration,
                                    uint64_t seed) const {
    const entt::registry& reg = things_.registry();
    PawnHitResult result;
    result.damageType = damageType;
    result.amount = amount;
    const CombatModel::Kind* kind = model_->kind(reg.get<Thing>(pawn).def);
    if (kind == nullptr || hitboxPart < 0) {
        return result;  // no hitboxes: the caller picks the part (HealthSystem::applyDamage)
    }
    Rng rng(seed);
    // Which part under the hitbox: the part or its fingers, toes, eyes... by their share.
    int part = hitboxPart;
    const std::vector<int>& choices = kind->landsOn[static_cast<size_t>(hitboxPart)];
    float total = 0.0f;
    for (const int c : choices) {
        total += health_.partHitPoints(pawn, c) > 0.0f ? kind->parts[static_cast<size_t>(c)].ownOutside : 0.0f;
    }
    if (total > 0.0f) {
        const float roll = rng.unit() * total;
        float acc = 0.0f;
        for (const int c : choices) {
            if (health_.partHitPoints(pawn, c) <= 0.0f || kind->parts[static_cast<size_t>(c)].ownOutside <= 0.0f) {
                continue;
            }
            acc += kind->parts[static_cast<size_t>(c)].ownOutside;
            part = c;
            if (roll < acc) {
                break;
            }
        }
    }
    part = health_.goInside(pawn, damageType, part, rng);
    result.part = part;
    return armorPass(pawn, part, damageType, amount, penetration, rng);
}

bool CombatSystem::hurtPawn(Entity pawn, DefId damageType, float amount, Rng& rng) {
    const int part = pickOutsidePart(pawn, rng);
    if (part < 0) {
        return false;
    }
    const PawnHitResult hit = armorPass(pawn, part, damageType, amount, 0.0f, rng);
    bool alive = true;
    if (!hit.deflected && hit.amount > 0.0f) {
        alive = health_.applyHit(pawn, hit.damageType, hit.amount, part);
    }
    wearApparel(pawn, hit);
    return alive;
}

float CombatSystem::spreadRadiusFor(float chance) const {
    // From the large end: below a few centimetres the 2 cm grid makes the share jump about, which
    // the long-range chances aimed shots ask for never reach but melee's high ones do.
    const std::vector<float>& r = model_->spreadRadius;
    const std::vector<float>& share = model_->hitShare;
    if (r.empty()) {
        return model_->radiusFor(std::clamp(chance, 0.0001f, 1.0f));
    }
    if (share.back() >= chance) {
        return r.back();
    }
    for (size_t k = r.size() - 1; k > 0; --k) {
        if (share[k - 1] >= chance) {
            const float span = share[k - 1] - share[k];
            const float t = span > 0.0f ? (share[k - 1] - chance) / span : 0.0f;
            return r[k - 1] + (r[k] - r[k - 1]) * std::clamp(t, 0.0f, 1.0f);
        }
    }
    return r.front();
}

bool CombatSystem::isMeleeWeapon(DefId def) const {
    return def.valid() && def.value < model_->melee.size() && model_->melee[def.value] != 0;
}

namespace {
int combatQualityOf(const entt::registry& reg, Entity item, int fallback, size_t levels) {
    if (item == kNoEntity || !reg.valid(item)) {
        return fallback;
    }
    const Quality* q = reg.try_get<Quality>(item);
    return q != nullptr ? std::min(static_cast<int>(q->level), static_cast<int>(levels) - 1) : fallback;
}
}  // namespace

float CombatSystem::meleeDamageFactor(Entity weapon, DefId damageType) const {
    const entt::registry& reg = things_.registry();
    if (weapon == kNoEntity || !reg.valid(weapon) || model_->weaponQuality.empty()) {
        return 1.0f;
    }
    float factor = model_->weaponQuality[static_cast<size_t>(
                                             combatQualityOf(reg, weapon, model_->defaultQuality, model_->weaponQuality.size()))]
                       .melee;
    if (const MadeOf* made = reg.try_get<MadeOf>(weapon); made != nullptr && made->stuff.valid() && made->stuff.value < model_->stuffSharp.size()) {
        const CombatModel::ArmorKind kind =
            damageType.valid() && damageType.value < model_->armorKind.size() ? model_->armorKind[damageType.value] : CombatModel::ArmorKind::None;
        if (kind == CombatModel::ArmorKind::Sharp) {
            factor *= model_->stuffSharp[made->stuff.value];
        } else if (kind == CombatModel::ArmorKind::Blunt) {
            factor *= model_->stuffBlunt[made->stuff.value];
        }
    }
    return factor;
}

float CombatSystem::rangedAccuracyFactor(Entity weapon) const {
    if (model_->weaponQuality.empty()) {
        return 1.0f;
    }
    return model_->weaponQuality[static_cast<size_t>(
                                     combatQualityOf(things_.registry(), weapon, model_->defaultQuality, model_->weaponQuality.size()))]
        .accuracy;
}

float CombatSystem::rangedDamageFactor(Entity weapon) const {
    if (model_->weaponQuality.empty()) {
        return 1.0f;
    }
    return model_->weaponQuality[static_cast<size_t>(
                                     combatQualityOf(things_.registry(), weapon, model_->defaultQuality, model_->weaponQuality.size()))]
        .damage;
}

bool CombatSystem::isGun(DefId def) const {
    return model_->gun(def) != nullptr;
}

void CombatSystem::step(Rng& rng) {
    entt::registry& reg = things_.registry();
    stepRolls();
    stepKicks();
    stepSwitches();
    // Pawns face where they aim, else where they walk.
    for (const auto [pawn, posture, mover] : reg.view<Posture, const Mover>().each()) {
        const Shooter* s = reg.try_get<Shooter>(pawn);
        const Position* pos = reg.try_get<Position>(pawn);
        if (s != nullptr && pos != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
            float ax = s->aimX;
            float ay = s->aimY;
            float az = 0.0f;
            if (s->target != kNoEntity) {
                aimPoint(s->target, ax, ay, az);
            }
            const float dx = ax - pos->x;
            const float dy = ay - pos->y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.01f) {
                posture.facingX = dx / len;
                posture.facingY = dy / len;
            }
        } else if (!health_.incapacitated(pawn)) {
            const float speed = std::sqrt(mover.vx * mover.vx + mover.vy * mover.vy);
            if (speed > 1e-4f) {
                posture.facingX = mover.vx / speed;
                posture.facingY = mover.vy / speed;
            }
        }
    }

    // Triggers, in id order: they share the random stream.
    scratch_.clear();
    for (const Entity pawn : reg.view<Shooter>()) {
        scratch_.push_back(pawn);
    }
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        updateShooter(pawn, reg.get<Shooter>(pawn), rng);
    }

    // Pawns thrown down by a blast (F9) get up when their time is over.
    scratch_.clear();
    for (const Entity pawn : reg.view<KnockedDown>()) {
        scratch_.push_back(pawn);
    }
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        KnockedDown& k = reg.get<KnockedDown>(pawn);
        if (--k.stepsLeft > 0 && !health_.incapacitated(pawn)) {
            continue;
        }
        if (Posture* posture = reg.try_get<Posture>(pawn)) {
            posture->stance = k.before;
        }
        reg.remove<KnockedDown>(pawn);
    }

    // Projectiles, in id order: one may knock down a wall another would have hit.
    scratch_.clear();
    for (const Entity shot : reg.view<Projectile>()) {
        scratch_.push_back(shot);
    }
    std::sort(scratch_.begin(), scratch_.end());
    const float g = model_->gravity;
    for (const Entity shot : scratch_) {
        Projectile& p = reg.get<Projectile>(shot);
        ++p.age;
        const float nx = p.x + p.vx;
        const float ny = p.y + p.vy;
        const float nz = p.z + p.vz - 0.5f * g;
        TraceHit hit = trace(p.x, p.y, p.z, nx, ny, nz, p.level, p.launcher, p.startCell);
        const CellPos next{static_cast<int>(std::floor(nx)), static_cast<int>(std::floor(ny)), p.level};
        if (hit.kind == ProjectileHitKind::None && (p.age > model_->maxFlight || !map_.inBounds(next))) {
            hit.kind = ProjectileHitKind::Gone;
            hit.x = nx;
            hit.y = ny;
            hit.z = nz;
        }
        if (hit.kind != ProjectileHitKind::None) {
            impact(shot, p, hit);
            reg.destroy(shot);
            continue;
        }
        p.x = nx;
        p.y = ny;
        p.z = nz;
        p.vz -= g;
    }
}

int CombatSystem::knockedDownFor(Entity pawn) const {
    const KnockedDown* k = things_.registry().valid(pawn) ? things_.registry().try_get<KnockedDown>(pawn) : nullptr;
    return k != nullptr ? k->stepsLeft : 0;
}

// An outside part of the pawn's body, by its share of the body, among those still there.
int CombatSystem::pickOutsidePart(Entity pawn, Rng& rng) const {
    const CombatModel::Kind* kind = model_->kind(things_.registry().get<Thing>(pawn).def);
    if (kind == nullptr) {
        return -1;
    }
    float total = 0.0f;
    for (size_t q = 0; q < kind->parts.size(); ++q) {
        if (!kind->parts[q].inside && health_.partHitPoints(pawn, static_cast<int>(q)) > 0.0f) {
            total += kind->parts[q].ownOutside;
        }
    }
    if (total <= 0.0f) {
        return -1;
    }
    const float roll = rng.unit() * total;
    float acc = 0.0f;
    int pick = -1;
    for (size_t q = 0; q < kind->parts.size(); ++q) {
        if (kind->parts[q].inside || health_.partHitPoints(pawn, static_cast<int>(q)) <= 0.0f || kind->parts[q].ownOutside <= 0.0f) {
            continue;
        }
        acc += kind->parts[q].ownOutside;
        pick = static_cast<int>(q);
        if (roll < acc) {
            break;
        }
    }
    return pick;
}

ExplosionResult CombatSystem::explode(const Explosion& ex) {
    ExplosionResult result;
    entt::registry& reg = things_.registry();
    if (ex.radius <= 0.0f || ex.damage <= 0.0f) {
        return result;
    }
    Rng rng(ex.seed);
    const CombatModel::Blast blast = ex.damageType.valid() && ex.damageType.value < model_->blast.size()
                                         ? model_->blast[ex.damageType.value]
                                         : CombatModel::Blast{};
    const float edge = model_->edgeFactor;
    const auto falloff = [&](float d) { return 1.0f - (1.0f - edge) * std::clamp(d / ex.radius, 0.0f, 1.0f); };
    const float bx = ex.x;
    const float by = ex.y;
    const float bz = ex.z + model_->blastHeight;
    const CellPos nowhere{-100000, -100000, ex.level};

    if (events_ != nullptr) {
        Event e;
        e.type = EventType::Explosion;
        e.thing = ex.source;
        e.target = ex.instigator;
        e.def = ex.damageType;
        e.amount = ex.radius;
        e.x = ex.x;
        e.y = ex.y;
        e.z = ex.z;
        e.cell = CellPos{static_cast<int>(std::floor(ex.x)), static_cast<int>(std::floor(ex.y)), ex.level};
        events_->push_back(std::move(e));
    }

    // ---- Pawns, in id order.
    scratch_.clear();
    for (const auto [pawn, thing, pos, posture] : reg.view<const Thing, const Position, const Posture>().each()) {
        (void)thing;
        (void)posture;
        if (pos.level != ex.level) {
            continue;
        }
        const float dx = pos.x - bx;
        const float dy = pos.y - by;
        if (dx * dx + dy * dy <= (ex.radius + 1.0f) * (ex.radius + 1.0f)) {
            scratch_.push_back(pawn);
        }
    }
    std::sort(scratch_.begin(), scratch_.end());
    const std::vector<Entity> pawns = scratch_;
    for (const Entity pawn : pawns) {
        const Health* health = reg.try_get<Health>(pawn);
        const CombatModel::Kind* kind = model_->kind(reg.get<Thing>(pawn).def);
        if (kind == nullptr || (health != nullptr && health->dead)) {
            continue;
        }
        const Position pos = reg.get<Position>(pawn);
        const int stance = static_cast<int>(combatStanceOf(reg, pawn));
        const float ground = map_.heightAt(pos.x, pos.y);
        float chest[3];
        if (!aimPoint(pawn, chest[0], chest[1], chest[2])) {
            continue;
        }
        const float samples[3][3] = {{chest[0], chest[1], chest[2]},
                                     {pos.x, pos.y, ground + 0.25f},
                                     {pos.x, pos.y, ground + std::max(0.3f, kind->poses[stance].top - 0.1f)}};
        float sum = 0.0f;
        float nearest = ex.radius;
        for (const auto& sample : samples) {
            const float sx = sample[0] - bx;
            const float sy = sample[1] - by;
            const float sz = sample[2] - bz;
            const float d = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (d > ex.radius) {
                continue;
            }
            const TraceHit hit = trace(bx, by, bz, sample[0], sample[1], sample[2], ex.level, kNoEntity, nowhere, false);
            if (hit.kind != ProjectileHitKind::None && hit.t < 0.97f) {
                continue;  // a wall, a rock or the ground between
            }
            sum += falloff(d);
            nearest = std::min(nearest, d);
        }
        const float amount = ex.damage * (sum / 3.0f) * model_->stanceFactor[stance];
        if (amount < 1.0f) {
            continue;
        }
        // Fragments: a hard enough blast splits into 2 to 4 pieces, each on a part of its own.
        int pieces = 1;
        if (amount >= blast.fragmentMinDamage && blast.fragmentsMax > blast.fragmentsMin) {
            pieces = blast.fragmentsMin + static_cast<int>(rng.below(static_cast<uint32_t>(blast.fragmentsMax - blast.fragmentsMin + 1)));
        } else if (amount >= blast.fragmentMinDamage) {
            pieces = blast.fragmentsMin;
        }
        float weights[8];
        pieces = std::min(pieces, 8);
        float weightSum = 0.0f;
        for (int i = 0; i < pieces; ++i) {
            weights[i] = 0.5f + rng.unit();
            weightSum += weights[i];
        }
        float dealt = 0.0f;
        for (int i = 0; i < pieces; ++i) {
            const int part = pickOutsidePart(pawn, rng);
            if (part < 0) {
                break;
            }
            const PawnHitResult piece = armorPass(pawn, part, ex.damageType, amount * weights[i] / weightSum, ex.penetration, rng);
            if (piece.deflected || piece.amount <= 0.0f) {
                wearApparel(pawn, piece);
                continue;
            }
            const bool alive = health_.applyHit(pawn, piece.damageType, piece.amount, part);
            wearApparel(pawn, piece);
            if (!alive) {
                break;
            }
            dealt += piece.amount;
        }
        if (dealt > 0.0f) {
            ++result.pawnsHurt;
        }
        // Knock-down: the blast throws the pawn down and away, armor or not.
        if (amount < model_->knockMinDamage || (health != nullptr && health->dead) || !things_.spawned(pawn)) {
            continue;
        }
        Posture& posture = reg.get<Posture>(pawn);
        const int steps = static_cast<int>(std::min(model_->knockMaxSteps, model_->knockBaseSteps + model_->knockStepsPerDamage * amount));
        KnockedDown& knocked = reg.get_or_emplace<KnockedDown>(pawn, KnockedDown{0, posture.stance});
        knocked.stepsLeft = std::max(knocked.stepsLeft, steps);
        posture.stance = Stance::Prone;
        if (Shooter* shooter = reg.try_get<Shooter>(pawn);
            shooter != nullptr && (shooter->phase == ShotPhase::Aiming || shooter->phase == ShotPhase::Firing)) {
            shooter->phase = ShotPhase::Ready;
            shooter->shotsLeft = 0;
            shooter->stepsLeft = 0;
        }
        Position& at = reg.get<Position>(pawn);
        float pushX = at.x - bx;
        float pushY = at.y - by;
        float len = std::sqrt(pushX * pushX + pushY * pushY);
        if (len < 0.05f) {
            pushX = posture.facingX;
            pushY = posture.facingY;
            len = 1.0f;
        }
        const float share = std::clamp(1.0f - nearest / ex.radius, 0.0f, 1.0f);
        for (const float scale : {1.0f, 0.5f}) {
            const float push = model_->knockbackMetres * share * scale;
            const float nx = at.x + pushX / len * push;
            const float ny = at.y + pushY / len * push;
            if (push > 0.01f && straightWalkIsClear(map_, at.x, at.y, nx, ny, kPawnRadius, at.level)) {
                at.x = nx;
                at.y = ny;
                break;
            }
        }
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->path.clear();
            mover->next = 0;
            mover->vx = 0.0f;
            mover->vy = 0.0f;
        }
        ++result.pawnsKnocked;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::PawnKnockedDown;
            e.thing = pawn;
            e.def = reg.get<Thing>(pawn).def;
            e.amount = static_cast<float>(knocked.stepsLeft);
            e.x = at.x;
            e.y = at.y;
            e.z = map_.heightAt(at.x, at.y);
            e.cell = cellOf(at);
            events_->push_back(std::move(e));
        }
    }

    // ---- Buildings: each once, at its nearest cell, in cell order. A cell behind a wall is shielded.
    struct BlastTarget {
        Entity building;
        float distance;
    };
    std::vector<BlastTarget> targets;
    const int reach = static_cast<int>(std::ceil(ex.radius)) + 1;
    const int cx = static_cast<int>(std::floor(ex.x));
    const int cy = static_cast<int>(std::floor(ex.y));
    for (int y = cy - reach; y <= cy + reach; ++y) {
        for (int x = cx - reach; x <= cx + reach; ++x) {
            const CellPos c{x, y, ex.level};
            if (!map_.inBounds(c)) {
                continue;
            }
            const Entity building = map_.buildingAt(c);
            if (building == kNoEntity || !reg.valid(building)) {
                continue;
            }
            const float nx = std::clamp(ex.x, static_cast<float>(x), static_cast<float>(x + 1));
            const float ny = std::clamp(ex.y, static_cast<float>(y), static_cast<float>(y + 1));
            const float d = std::sqrt((ex.x - nx) * (ex.x - nx) + (ex.y - ny) * (ex.y - ny));
            if (d > ex.radius) {
                continue;
            }
            // The cells strictly between the blast's cell and this one (a grid walk): a wall among them shields it.
            bool shielded = false;
            const int steps = std::max(std::abs(x - cx), std::abs(y - cy));
            for (int i = 1; i < steps && !shielded; ++i) {
                const CellPos mid{cx + (x - cx) * i / steps, cy + (y - cy) * i / steps, ex.level};
                shielded = map_.inBounds(mid) && map_.obstacleDm(mid) > 0 && map_.buildingAt(mid) != building;
            }
            if (shielded) {
                continue;
            }
            const auto at = std::find_if(targets.begin(), targets.end(), [&](const BlastTarget& t) { return t.building == building; });
            if (at == targets.end()) {
                targets.push_back(BlastTarget{building, d});
            } else {
                at->distance = std::min(at->distance, d);
            }
        }
    }
    for (const BlastTarget& target : targets) {
        HitPoints* hp = reg.try_get<HitPoints>(target.building);
        if (hp == nullptr) {
            continue;
        }
        const Footprint* footprint = reg.try_get<Footprint>(target.building);
        float factor = blast.buildingFactor;
        if (footprint != nullptr && footprint->blocksMovement) {
            factor *= blast.impassableFactor;
            if (things_.defs().get(reg.get<Thing>(target.building).def).flag("building.holdsRoof")) {
                factor *= blast.wallFactor;
            }
        }
        const int damage = std::max(1, static_cast<int>(std::lround(ex.damage * falloff(target.distance) * factor)));
        hp->current -= damage;
        ++result.buildingsHit;
        if (hp->current <= 0) {
            ++result.buildingsDestroyed;
            if (beforeDestroy_) {
                beforeDestroy_(target.building);
            }
            things_.destroy(target.building);
        }
    }
    return result;
}

bool CombatSystem::damageBuilding(Entity building, int damage) {
    entt::registry& reg = things_.registry();
    HitPoints* hp = reg.valid(building) ? reg.try_get<HitPoints>(building) : nullptr;
    if (hp == nullptr) {
        return false;
    }
    hp->current -= std::max(0, damage);
    if (hp->current > 0) {
        return false;
    }
    if (beforeDestroy_) {
        beforeDestroy_(building);
    }
    things_.destroy(building);
    return true;
}

void CombatSystem::drop(Entity pawn, Entity item) {
    entt::registry& reg = things_.registry();
    if (Gear* gear = reg.try_get<Gear>(pawn)) {
        if (gear->weapon == item) {
            gear->weapon = kNoEntity;
        }
        gear->apparel.erase(std::remove(gear->apparel.begin(), gear->apparel.end(), item), gear->apparel.end());
        gear->stowed.erase(std::remove(gear->stowed.begin(), gear->stowed.end(), item), gear->stowed.end());
    }
    if (WeaponSwitch* change = reg.try_get<WeaponSwitch>(pawn); change != nullptr && change->stepsLeft > 0) {
        // Dropping the weapon switched to, or the one in the hands, ends the switch: nothing is left to swap.
        const Gear* gear = reg.try_get<Gear>(pawn);
        if (change->to == item || gear == nullptr || gear->weapon == kNoEntity) {
            change->to = kNoEntity;
            change->stepsLeft = 0;
        }
    }
    reg.remove<EquippedBy>(item);
    const Position* pos = reg.try_get<Position>(pawn);
    if (pos == nullptr || !things_.spawnAt(item, cellOf(*pos))) {
        if (beforeDestroy_) {
            beforeDestroy_(item);
        }
        things_.destroy(item);
    }
}

bool CombatSystem::unequip(Entity pawn, Entity item) {
    entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    if (gear == nullptr || !reg.valid(item) ||
        (gear->weapon != item && std::find(gear->apparel.begin(), gear->apparel.end(), item) == gear->apparel.end() &&
         std::find(gear->stowed.begin(), gear->stowed.end(), item) == gear->stowed.end())) {
        return false;
    }
    drop(pawn, item);
    return true;
}

void CombatSystem::strikePawn(Entity pawn, DefId damageType, float amount, float penetration, Rng& rng) {
    const int part = pickOutsidePart(pawn, rng);
    if (part < 0) {
        health_.applyDamage(pawn, damageType, amount, -1, rng);
        return;
    }
    const PawnHitResult hit = armorPass(pawn, part, damageType, amount, penetration, rng);
    if (!hit.deflected && hit.amount > 0.0f) {
        health_.applyHit(pawn, hit.damageType, hit.amount, part);
    }
    wearApparel(pawn, hit);
}

void CombatSystem::wearApparel(Entity pawn, const PawnHitResult& result) {
    for (const auto& [item, points] : result.wear) {
        damageApparel(pawn, item, points);
    }
}

void CombatSystem::damageApparel(Entity pawn, Entity item, int hitPoints) {
    entt::registry& reg = things_.registry();
    HitPoints* hp = reg.valid(item) ? reg.try_get<HitPoints>(item) : nullptr;
    if (hp == nullptr || hitPoints <= 0) {
        return;
    }
    // Worn apparel wears its durability, not its hit points (docs/design/apparel-durability.md): `hitPoints` of
    // wear is that share of the maximum. At 0 durability it stays on, tattered; only fragile basic clothes then
    // take the rest off their hit points and can be destroyed.
    const float before = ApparelStats::condition(reg, item);
    const float wear = static_cast<float>(hitPoints) / static_cast<float>(std::max(1, hp->max));
    reg.emplace_or_replace<Durability>(item, Durability{std::max(0.0f, before - wear)});
    if (wear <= before || !apparelStats_->fragile(reg.get<Thing>(item).def)) {
        return;
    }
    const int excess = std::max(1, static_cast<int>(std::lround((wear - before) * static_cast<float>(hp->max))));
    hp->current = std::max(0, hp->current - excess);
    if (hp->current > 0) {
        return;
    }
    // Worn out: gone, and the pawn is no longer wearing it.
    const DefId def = reg.get<Thing>(item).def;
    if (Gear* gear = reg.try_get<Gear>(pawn)) {
        gear->apparel.erase(std::remove(gear->apparel.begin(), gear->apparel.end(), item), gear->apparel.end());
    }
    if (beforeDestroy_) {
        beforeDestroy_(item);
    }
    things_.destroy(item);
    if (events_ != nullptr) {
        Event event;
        event.type = EventType::ApparelDestroyed;
        event.thing = pawn;
        event.def = def;
        events_->push_back(std::move(event));
    }
}

void CombatSystem::dropGear(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Gear* gear = reg.try_get<Gear>(pawn);
    if (gear == nullptr) {
        return;
    }
    std::vector<Entity> items = gear->apparel;
    items.insert(items.begin(), gear->stowed.begin(), gear->stowed.end());
    if (gear->weapon != kNoEntity) {
        items.insert(items.begin(), gear->weapon);
    }
    for (const Entity item : items) {
        drop(pawn, item);
    }
    reg.remove<Gear>(pawn);
}

bool CombatSystem::equip(Entity pawn, Entity item, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!health_.tracks(pawn) || reg.get<Health>(pawn).dead) {
        return fail("that pawn can't hold anything (no body, or dead)");
    }
    if (!reg.valid(item) || !reg.all_of<Thing>(item) || reg.get<Thing>(item).category != ThingCategory::Item) {
        return fail("that is not an item");
    }
    if (const EquippedBy* by = reg.try_get<EquippedBy>(item)) {
        if (by->pawn == pawn) {
            return true;
        }
        return fail("someone else has that");
    }
    const DefId def = reg.get<Thing>(item).def;
    const CombatModel::Apparel* wear = model_->wear(def);
    const bool weapon = model_->gun(def) != nullptr || isMeleeWeapon(def);
    if (!weapon && wear == nullptr) {
        return fail("that is neither a weapon nor apparel");
    }
    Gear& gear = reg.get_or_emplace<Gear>(pawn);
    if (weapon) {
        if (gear.weapon != kNoEntity) {
            drop(pawn, gear.weapon);
        }
        WeaponSlot slot = WeaponSlot::Primary;
        weaponSlot(def, slot);
        const std::vector<Entity> stowed = reg.get<Gear>(pawn).stowed;
        for (const Entity other : stowed) {
            WeaponSlot otherSlot = WeaponSlot::Primary;
            if (weaponSlot(reg.get<Thing>(other).def, otherSlot) && otherSlot == slot) {
                drop(pawn, other);  // one weapon to a slot
            }
        }
    } else {
        std::vector<Entity> replaced;
        for (const Entity worn : gear.apparel) {
            const CombatModel::Apparel* other = model_->wear(reg.get<Thing>(worn).def);
            bool clash = false;
            for (const std::string& layer : wear->layers) {
                for (const std::string& group : wear->groups) {
                    clash = clash || (other != nullptr &&
                                      std::find(other->layers.begin(), other->layers.end(), layer) != other->layers.end() &&
                                      std::find(other->groups.begin(), other->groups.end(), group) != other->groups.end());
                }
            }
            if (clash) {
                replaced.push_back(worn);
            }
        }
        for (const Entity worn : replaced) {
            drop(pawn, worn);
        }
    }
    things_.despawn(item);
    Gear& after = reg.get<Gear>(pawn);
    if (weapon) {
        after.weapon = item;
    } else {
        after.apparel.push_back(item);
    }
    reg.emplace<EquippedBy>(item, EquippedBy{pawn});
    return true;
}

// ---- Several weapons (P1) ---------------------------------------------------------------------

bool CombatSystem::weaponSlot(DefId def, WeaponSlot& out) const {
    if (!def.valid() || def.value >= model_->slot.size() || model_->slot[def.value] == 255) {
        return false;
    }
    out = static_cast<WeaponSlot>(model_->slot[def.value]);
    return true;
}

bool CombatSystem::carryWeapon(Entity pawn, Entity item, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!health_.tracks(pawn) || reg.get<Health>(pawn).dead) {
        return fail("that pawn can't hold anything (no body, or dead)");
    }
    if (!reg.valid(item) || !reg.all_of<Thing>(item) || reg.get<Thing>(item).category != ThingCategory::Item) {
        return fail("that is not an item");
    }
    if (const EquippedBy* by = reg.try_get<EquippedBy>(item)) {
        return by->pawn == pawn ? true : fail("someone else has that");
    }
    WeaponSlot slot = WeaponSlot::Primary;
    if (!weaponSlot(reg.get<Thing>(item).def, slot)) {
        return fail("that is not a weapon");
    }
    Gear& gear = reg.get_or_emplace<Gear>(pawn);
    // One weapon to a slot: the one carried there goes to the ground.
    const auto sameSlot = [&](Entity other) {
        WeaponSlot otherSlot = WeaponSlot::Primary;
        return weaponSlot(reg.get<Thing>(other).def, otherSlot) && otherSlot == slot;
    };
    if (gear.weapon != kNoEntity && sameSlot(gear.weapon)) {
        drop(pawn, gear.weapon);
    }
    const std::vector<Entity> stowed = reg.get<Gear>(pawn).stowed;
    for (const Entity other : stowed) {
        if (sameSlot(other)) {
            drop(pawn, other);
        }
    }
    things_.despawn(item);
    Gear& after = reg.get<Gear>(pawn);
    if (after.weapon == kNoEntity) {
        after.weapon = item;
    } else {
        after.stowed.push_back(item);
    }
    reg.emplace<EquippedBy>(item, EquippedBy{pawn});
    return true;
}

Entity CombatSystem::weaponIn(Entity pawn, WeaponSlot slot) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    if (gear == nullptr) {
        return kNoEntity;
    }
    const auto in = [&](Entity item) {
        WeaponSlot found = WeaponSlot::Primary;
        return item != kNoEntity && reg.valid(item) && weaponSlot(reg.get<Thing>(item).def, found) && found == slot;
    };
    if (in(gear->weapon)) {
        return gear->weapon;
    }
    for (const Entity item : gear->stowed) {
        if (in(item)) {
            return item;
        }
    }
    return kNoEntity;
}

bool CombatSystem::handsSlot(Entity pawn, WeaponSlot& out) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
    return gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon) &&
           weaponSlot(reg.get<Thing>(gear->weapon).def, out);
}

bool CombatSystem::switching(Entity pawn) const {
    return switchStepsLeft(pawn) > 0;
}

int CombatSystem::switchStepsLeft(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const WeaponSwitch* change = reg.valid(pawn) ? reg.try_get<WeaponSwitch>(pawn) : nullptr;
    return change != nullptr ? change->stepsLeft : 0;
}

bool CombatSystem::switchTarget(Entity pawn, WeaponSlot& out) const {
    const entt::registry& reg = things_.registry();
    const WeaponSwitch* change = reg.valid(pawn) ? reg.try_get<WeaponSwitch>(pawn) : nullptr;
    return change != nullptr && change->stepsLeft > 0 && reg.valid(change->to) && weaponSlot(reg.get<Thing>(change->to).def, out);
}

bool CombatSystem::switchWeapon(Entity pawn, WeaponSlot slot, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!things_.spawned(pawn) || !reg.all_of<Posture>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    if (health_.incapacitated(pawn)) {
        return fail("that pawn can't switch weapons (downed or dead)");
    }
    if (reg.all_of<KnockedDown>(pawn)) {
        return fail("that pawn is knocked down");
    }
    if (reg.all_of<Stunned>(pawn)) {
        return fail("that pawn is stunned");
    }
    if (rollLocked(pawn)) {
        return fail("rolling");
    }
    if (switching(pawn)) {
        return fail("switching already");
    }
    const Entity item = weaponIn(pawn, slot);
    if (item == kNoEntity) {
        return fail("that pawn carries nothing in that slot");
    }
    if (reg.get<Gear>(pawn).weapon == item) {
        return fail("that weapon is in the hands already");
    }
    if (const Shooter* s = reg.try_get<Shooter>(pawn);
        s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing || s->phase == ShotPhase::Reloading)) {
        return fail("busy shooting");
    }
    if (const Melee* m = reg.try_get<Melee>(pawn); m != nullptr && m->phase != MeleePhase::Ready) {
        return fail("busy swinging");
    }
    WeaponSwitch& change = reg.get_or_emplace<WeaponSwitch>(pawn);
    change.to = item;
    change.stepsLeft = model_->switchSteps[static_cast<int>(slot)];
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.action = PawnAction::Switch;
        e.thing = pawn;
        e.def = reg.get<Thing>(pawn).def;
        e.target = item;
        e.cell = cellOf(reg.get<Position>(pawn));
        e.amount = static_cast<float>(change.stepsLeft);
        events_->push_back(std::move(e));
    }
    return true;
}

void CombatSystem::stepSwitches() {
    entt::registry& reg = things_.registry();
    for (const auto [pawn, change] : reg.view<WeaponSwitch>().each()) {
        if (change.stepsLeft <= 0) {
            continue;
        }
        Gear* gear = reg.try_get<Gear>(pawn);
        if (gear == nullptr || std::find(gear->stowed.begin(), gear->stowed.end(), change.to) == gear->stowed.end() ||
            health_.incapacitated(pawn)) {
            change.to = kNoEntity;  // the weapon is gone, or the pawn can't go on
            change.stepsLeft = 0;
            continue;
        }
        if (--change.stepsLeft > 0) {
            continue;
        }
        const Entity to = change.to;
        gear->stowed.erase(std::find(gear->stowed.begin(), gear->stowed.end(), to));
        if (gear->weapon != kNoEntity) {
            gear->stowed.push_back(gear->weapon);
        }
        gear->weapon = to;
        change.to = kNoEntity;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::WeaponSwitched;
            e.thing = pawn;
            e.def = reg.get<Thing>(to).def;
            e.target = to;
            events_->push_back(std::move(e));
        }
    }
}

void CombatSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    const bool has[] = {reg.all_of<Posture>(e), reg.all_of<Gear>(e), reg.all_of<EquippedBy>(e), reg.all_of<Shooter>(e),
                        reg.all_of<Projectile>(e), reg.all_of<Magazine>(e), reg.all_of<RecoilKick>(e),
                        reg.all_of<DodgeRoll>(e), reg.all_of<KnockedDown>(e), reg.all_of<WeaponSwitch>(e)};
    uint64_t mask = 0;
    for (size_t bit = 0; bit < sizeof has / sizeof has[0]; ++bit) {
        mask |= has[bit] ? (uint64_t{1} << bit) : 0;
    }
    h.add(mask);
    if (const Posture* p = reg.try_get<Posture>(e)) {
        h.add(static_cast<int>(p->stance));
        h.add(p->facingX);
        h.add(p->facingY);
    }
    if (const Gear* g = reg.try_get<Gear>(e)) {
        h.add(entt::to_integral(g->weapon));
        h.add(static_cast<uint64_t>(g->apparel.size()));
        for (const Entity a : g->apparel) {
            h.add(entt::to_integral(a));
        }
        h.add(static_cast<uint64_t>(g->stowed.size()));
        for (const Entity a : g->stowed) {
            h.add(entt::to_integral(a));
        }
    }
    if (const WeaponSwitch* change = reg.try_get<WeaponSwitch>(e)) {
        h.add(entt::to_integral(change->to));
        h.add(change->stepsLeft);
    }
    if (const EquippedBy* by = reg.try_get<EquippedBy>(e)) {
        h.add(entt::to_integral(by->pawn));
    }
    if (const Shooter* s = reg.try_get<Shooter>(e)) {
        h.add(static_cast<int>(s->phase));
        h.add(s->stepsLeft);
        h.add(s->shotsLeft);
        h.add(entt::to_integral(s->target));
        h.add(s->aimX);
        h.add(s->aimY);
        h.add(s->aimZ);
        h.add(s->player);
        h.add(s->recoil);
    }
    if (const Projectile* p = reg.try_get<Projectile>(e)) {
        h.add(p->def.value);
        h.add(p->weapon.value);
        h.add(entt::to_integral(p->launcher));
        for (const float f : {p->x, p->y, p->z, p->vx, p->vy, p->vz}) {
            h.add(f);
        }
        h.add(p->level);
        h.add(p->age);
        h.add(p->startCell.x);
        h.add(p->startCell.y);
        h.add(p->startCell.level);
        h.add(p->seed);
    }
    if (const Magazine* m = reg.try_get<Magazine>(e)) {
        h.add(m->rounds);
    }
    if (const KnockedDown* k = reg.try_get<KnockedDown>(e)) {
        h.add(k->stepsLeft);
        h.add(static_cast<int>(k->before));
    }
    if (const RecoilKick* k = reg.try_get<RecoilKick>(e)) {
        h.add(k->up);
        h.add(k->side);
        h.add(k->shots);
        h.add(k->sinceShot);
    }
    if (const DodgeRoll* r = reg.try_get<DodgeRoll>(e)) {
        h.add(static_cast<int>(r->phase));
        h.add(r->stepsLeft);
        h.add(r->rolled);
        h.add(r->dirX);
        h.add(r->dirY);
        h.add(r->cooldown);
        h.add(r->proof);
    }
}

void CombatSystem::kickOf(Entity pawn, float& up, float& side) const {
    const entt::registry& reg = things_.registry();
    const RecoilKick* kick = reg.valid(pawn) ? reg.try_get<RecoilKick>(pawn) : nullptr;
    up = kick != nullptr ? kick->up : 0.0f;
    side = kick != nullptr ? kick->side : 0.0f;
}

void CombatSystem::stepKicks() {
    entt::registry& reg = things_.registry();
    for (const auto [pawn, kick] : reg.view<RecoilKick>().each()) {
        ++kick.sinceShot;
        if (kick.sinceShot >= model_->kickReset) {
            kick.shots = 0;
        }
        // Held for a moment after a shot, then it drops straight back toward the aim.
        if (kick.sinceShot > model_->kickHold && (kick.up != 0.0f || kick.side != 0.0f)) {
            const float length = std::sqrt(kick.up * kick.up + kick.side * kick.side);
            if (length <= model_->kickRecovery) {
                kick.up = 0.0f;
                kick.side = 0.0f;
            } else {
                const float scale = 1.0f - model_->kickRecovery / length;
                kick.up *= scale;
                kick.side *= scale;
            }
        }
    }
}

RollPhase CombatSystem::rollPhase(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const DodgeRoll* roll = reg.valid(pawn) ? reg.try_get<DodgeRoll>(pawn) : nullptr;
    return roll != nullptr ? roll->phase : RollPhase::Idle;
}

int CombatSystem::rollStepsLeft(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const DodgeRoll* roll = reg.valid(pawn) ? reg.try_get<DodgeRoll>(pawn) : nullptr;
    return roll != nullptr && roll->phase != RollPhase::Idle ? roll->stepsLeft : 0;
}

bool CombatSystem::hitProof(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const DodgeRoll* roll = reg.valid(pawn) ? reg.try_get<DodgeRoll>(pawn) : nullptr;
    return roll != nullptr && roll->proof;
}

bool CombatSystem::rollDirection(Entity pawn, float& x, float& y) const {
    const entt::registry& reg = things_.registry();
    const DodgeRoll* roll = reg.valid(pawn) ? reg.try_get<DodgeRoll>(pawn) : nullptr;
    if (roll == nullptr || roll->phase == RollPhase::Idle) {
        return false;
    }
    x = roll->dirX;
    y = roll->dirY;
    return true;
}

bool CombatSystem::dodgeRoll(Entity pawn, float dx, float dy, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!things_.spawned(pawn) || !reg.all_of<Posture, Position, Mover, PlayerControl>(pawn)) {
        return fail("that pawn isn't under direct control");
    }
    if (health_.incapacitated(pawn)) {
        return fail("that pawn can't roll (downed or dead)");
    }
    Posture& posture = reg.get<Posture>(pawn);
    if (posture.stance == Stance::Prone) {
        return fail("get up before rolling");
    }
    DodgeRoll& roll = reg.get_or_emplace<DodgeRoll>(pawn);
    if (roll.phase != RollPhase::Idle) {
        return fail("already rolling");
    }
    if (roll.cooldown > 0) {
        return fail("not ready to roll again yet");
    }
    float length = std::isfinite(dx) && std::isfinite(dy) ? std::sqrt(dx * dx + dy * dy) : 0.0f;
    if (length < 1e-4f) {
        dx = -posture.facingX;  // no direction: a backstep
        dy = -posture.facingY;
        length = std::sqrt(dx * dx + dy * dy);
    }
    roll.dirX = length > 1e-6f ? dx / length : 1.0f;
    roll.dirY = length > 1e-6f ? dy / length : 0.0f;
    roll.phase = model_->rollStartUp > 0 ? RollPhase::StartUp : RollPhase::Rolling;
    roll.stepsLeft = model_->rollStartUp > 0 ? model_->rollStartUp : model_->rollActive;
    roll.rolled = 0;
    roll.proof = false;
    posture.facingX = roll.dirX;
    posture.facingY = roll.dirY;
    // The roll ends a shot being aimed or fired; a reload goes on.
    if (Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
        s->phase = ShotPhase::Ready;
        s->shotsLeft = 0;
        s->stepsLeft = 0;
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.action = PawnAction::Roll;
        e.thing = pawn;
        e.def = reg.get<Thing>(pawn).def;
        e.cell = cellOf(reg.get<Position>(pawn));
        e.x = roll.dirX;
        e.y = roll.dirY;
        e.amount = static_cast<float>(model_->rollStartUp + model_->rollActive + model_->rollRecovery);
        events_->push_back(std::move(e));
    }
    return true;
}

// Runs every roll one step: the phase it is in acts, then counts down into the next.
void CombatSystem::stepRolls() {
    entt::registry& reg = things_.registry();
    scratch_.clear();
    for (const Entity pawn : reg.view<DodgeRoll>()) {
        scratch_.push_back(pawn);
    }
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        DodgeRoll& roll = reg.get<DodgeRoll>(pawn);
        roll.proof = false;
        if (roll.phase == RollPhase::Idle) {
            roll.cooldown = std::max(0, roll.cooldown - 1);
            continue;
        }
        if (health_.incapacitated(pawn) || !things_.spawned(pawn)) {
            roll.phase = RollPhase::Idle;  // knocked down mid-roll: it ends where the pawn lies
            roll.stepsLeft = 0;
            roll.cooldown = model_->rollCooldown;
            continue;
        }
        switch (roll.phase) {
            case RollPhase::StartUp:
                if (--roll.stepsLeft <= 0) {
                    roll.phase = RollPhase::Rolling;
                    roll.stepsLeft = model_->rollActive;
                    roll.rolled = 0;
                }
                break;
            case RollPhase::Rolling: {
                roll.proof = roll.rolled < model_->rollHitProof;
                ++roll.rolled;
                Position& pos = reg.get<Position>(pawn);
                const float stride = model_->rollDistance / static_cast<float>(model_->rollActive);
                const float dx = roll.dirX * stride;
                const float dy = roll.dirY * stride;
                // The whole move, or sliding along one axis (a wall), or staying where it is.
                const float tries[3][2] = {{dx, dy}, {dx, 0.0f}, {0.0f, dy}};
                float moveX = 0.0f;
                float moveY = 0.0f;
                for (const auto& t : tries) {
                    if ((t[0] != 0.0f || t[1] != 0.0f) && pawnFitsAt(map_, pos.x + t[0], pos.y + t[1], kPawnRadius, pos.level)) {
                        moveX = t[0];
                        moveY = t[1];
                        break;
                    }
                }
                pos.x += moveX;
                pos.y += moveY;
                if (Mover* mover = reg.try_get<Mover>(pawn)) {
                    mover->vx = moveX;
                    mover->vy = moveY;
                }
                if (--roll.stepsLeft <= 0) {
                    roll.phase = RollPhase::Recovery;
                    roll.stepsLeft = model_->rollRecovery;
                }
                break;
            }
            case RollPhase::Recovery:
                if (--roll.stepsLeft <= 0) {
                    roll.phase = RollPhase::Idle;
                    roll.cooldown = model_->rollCooldown;
                }
                break;
            case RollPhase::Idle:
                break;
        }
        if (roll.phase == RollPhase::Recovery && roll.stepsLeft <= 0) {  // no recovery configured
            roll.phase = RollPhase::Idle;
            roll.cooldown = model_->rollCooldown;
        }
    }
}

}  // namespace colony

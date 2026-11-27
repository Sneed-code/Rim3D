#include "ColonySim/Melee.h"
#include "ColonySim/Plants.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Apparel.h"
#include "ColonySim/BodyMotion.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Hold.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Patch.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace colony {

namespace {

// A dead body (a corpse the player's swing went down at).
bool meleeDeadBody(const entt::registry& reg, Entity e) {
    const Health* h = e != kNoEntity && reg.valid(e) ? reg.try_get<Health>(e) : nullptr;
    return h != nullptr && h->dead;
}

// The player's own swing at someone standing: the clip is drawn on the upper body and the feet stay the player's
// (BodyMotion::freeFeet). A blow at someone on the ground keeps the whole body's clip.
bool meleeFreeFeet(const Melee& m) {
    return m.player && !m.low && m.kind == SoulsAttack::None;  // an Elden Ring style attack is the whole body's
}

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float meleeLerp(float a, float b, float t) { return a + (b - a) * t; }

// A MeleeRules value given at Melee skill 0 and 20, at this skill.
float meleeBySkill(const float (&v)[2], int skill) {
    return meleeLerp(v[0], v[1], static_cast<float>(std::clamp(skill, 0, kSkillLevelLimit)) / 20.0f);  // goes on past 20
}

void meleeReadRules(const DefDatabase& defs, MeleeRules& r) {
    const Def* def = defs.find("Rules", "MeleeRules");
    if (def == nullptr) {
        return;
    }
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(def->number(key, out)); };
    const auto whole = [&](const char* key, int& out) { out = static_cast<int>(def->integer(key, out)); };
    const auto bySkill = [&](const char* key, float (&out)[2]) {
        for (size_t i = 0; i < 2; ++i) {
            out[i] = static_cast<float>(def->numberAt(key, i, out[i]));
        }
    };
    whole("defaultMeleeSkill", r.defaultSkill);
    bySkill("skillSpeedBySkill", r.skillSpeed);
    bySkill("skillDamageBySkill", r.skillDamage);
    bySkill("aimWobbleBySkill", r.aimWobble);
    if (def->has("reactionTraitSteps")) {
        r.reactionTraitSteps.clear();
        for (size_t i = 0; i < def->count("reactionTraitSteps"); ++i) {
            const std::string point = "reactionTraitSteps." + std::to_string(i);
            r.reactionTraitSteps.emplace_back(def->textAt(point.c_str(), 0), static_cast<float>(def->numberAt(point.c_str(), 1)));
        }
    }
    whole("guardBlocks", r.guardBlocks);
    whole("guardWindowSteps", r.guardWindowSteps);
    whole("guardBreakSteps", r.guardBreakSteps);
    whole("dodgeRecoverySteps", r.dodgeRecoverySteps);
    whole("recoveryNoDefendSteps", r.recoveryNoDefendSteps);
    whole("guardWaitSteps", r.guardWaitSteps);
    num("guardFacingCos", r.guardFacingCos);
    num("rollClearance", r.rollClearance);
    whole("aiRollEverySteps", r.aiRollEverySteps);
    num("armorPenetrationPerDamage", r.penetrationPerDamage);
    if (def->has("chargeTraits")) {
        r.chargeTraits.clear();
        for (size_t i = 0; i < def->count("chargeTraits"); ++i) {
            r.chargeTraits.push_back(def->textAt("chargeTraits", i));
        }
    }
    num("bodyReach", r.bodyReach);
    num("animalReachBase", r.animalReachBase);
    num("animalReachPerRoot", r.animalReachPerRoot);
    num("defaultWeaponReach", r.defaultWeaponReach);
    num("shoulderBelowEye", r.shoulderBelowEye);
    num("strikeDepth", r.strikeDepth);
    num("reachSlack", r.reachSlack);
    num("closeIn", r.closeIn);
    num("standGround", r.standGround);
    num("windupShare", r.windupShare);
    whole("minWindup", r.minWindup);
    num("attackTimeFactor", r.attackTimeFactor);
    num("unarmedTimeFactor", r.unarmedTimeFactor);
    num("unarmedPowerFactor", r.unarmedPowerFactor);
    whole("unarmedMinWindup", r.unarmedMinWindup);
    num("unarmedWindupShare", r.unarmedWindupShare);
    whole("unarmedGapSteps", r.unarmedGapSteps);
    whole("playerFistChainSteps", r.playerFistChainSteps);
    num("lowTimeFactor", r.lowTimeFactor);
    r.playerStepIn = def->flag("playerStepIn", r.playerStepIn);
    r.stepInFreesFeet = def->flag("stepInFreesFeet", r.stepInFreesFeet);
    num("stepInMaxSpeed", r.stepInMaxSpeed);
    whole("commitSteps", r.commitSteps);
    bySkill("reactionStepsBySkill", r.reactionSteps);
    num("reactionJitter", r.reactionJitter);
    whole("blockSteps", r.blockSteps);
    whole("blockRecoverySteps", r.blockRecoverySteps);
    whole("playerBlockSteps", r.playerBlockSteps);
    whole("playerBlockHoldSteps", r.playerBlockHoldSteps);
    whole("dodgeHoldSteps", r.dodgeHoldSteps);
    num("strongPowerShare", r.strongPowerShare);
    num("arcHalfDegrees", r.arcHalfDegrees);
    whole("arcRaysPerSide", r.arcRaysPerSide);
    if (def->has("arcDamageTypes")) {
        r.arcDamageTypes.clear();
        for (size_t i = 0; i < def->count("arcDamageTypes"); ++i) {
            r.arcDamageTypes.push_back(def->textAt("arcDamageTypes", i));
        }
    }
    num("reachHeightFollow", r.reachHeightFollow);
    num("maxAttackClipSeconds", r.maxAttackClipSeconds);
    num("maxAttackHitSeconds", r.maxAttackHitSeconds);
    num("maxAttackTravel", r.maxAttackTravel);
    num("focusRange", r.focusRange);
    num("focusBreakRange", r.focusBreakRange);
    num("focusFrontCos", r.focusFrontCos);
    num("focusBehindFactor", r.focusBehindFactor);
    num("aiFocusRange", r.aiFocusRange);
    num("lockBigBodySize", r.lockBigBodySize);
    whole("lockSightLostSteps", r.lockSightLostSteps);
    num("orbitRadialShare", r.orbitRadialShare);
    whole("focusRoundSteps", r.focusRoundSteps);
    num("flickMaxDegrees", r.flickMaxDegrees);
    num("assistCos", r.assistCos);
    num("playerReachSlack", r.playerReachSlack);
    num("corpseReachExtra", r.corpseReachExtra);
    num("strikeTrackDegrees", r.strikeTrackDegrees);
    bySkill("defendChanceBySkill", r.defendChance);
    num("parryShare", r.parryShare);
    num("backstepShare", r.backstepShare);
    num("backstepMetres", r.backstepMetres);
    whole("backstepSteps", r.backstepSteps);
    r.backstepSteps = std::max(1, r.backstepSteps);
    whole("footworkEverySteps", r.footworkEverySteps);
    bySkill("footworkChanceBySkill", r.footworkChance);
    num("footworkTraitBonus", r.footworkTraitBonus);
    if (def->has("footworkTraits")) {
        r.footworkTraits.clear();
        for (size_t i = 0; i < def->count("footworkTraits"); ++i) {
            r.footworkTraits.push_back(def->textAt("footworkTraits", i));
        }
    }
    num("recklessFootwork", r.recklessFootwork);
    num("circleMetres", r.circleMetres);
    whole("circleSteps", r.circleSteps);
    num("circleFlip", r.circleFlip);
    num("backShare", r.backShare);
    num("circleShare", r.circleShare);
    num("backMetres", r.backMetres);
    whole("backSteps", r.backSteps);
    num("crowdedShare", r.crowdedShare);
    num("chaseSeconds", r.chaseSeconds);
    num("lineOfFireRange", r.lineOfFireRange);
    num("lineOfFireClearance", r.lineOfFireClearance);
    r.footworkEverySteps = std::max(1, r.footworkEverySteps);
    r.circleSteps = std::max(1, r.circleSteps);
    r.backSteps = std::max(1, r.backSteps);
    r.unarmedStyle = def->text("unarmedStyle", r.unarmedStyle);
    for (const std::string& style : def->keys("styleAttacks")) {
        std::vector<std::string> clips;
        const std::string path = "styleAttacks." + style;
        for (size_t i = 0; i < def->count(path.c_str()); ++i) {
            clips.push_back(def->textAt(path.c_str(), i));
        }
        r.styleAttacks.emplace_back(style, std::move(clips));
    }
    for (const std::string& key : def->keys("lowAttacks")) {
        std::vector<std::string> clips;
        const std::string path = "lowAttacks." + key;
        for (size_t i = 0; i < def->count(path.c_str()); ++i) {
            clips.push_back(def->textAt(path.c_str(), i));
        }
        r.lowAttacks.emplace_back(key, std::move(clips));
    }
    for (const std::string& weapon : def->keys("weaponStyle")) {
        r.weaponStyle.emplace_back(weapon, def->text(("weaponStyle." + weapon).c_str()));
    }
    for (const std::string& key : def->keys("finishers")) {
        const std::string base = "finishers." + key + ".";
        MeleeFinisher f;
        f.key = key;
        f.move = def->text((base + "move").c_str(), f.move);
        f.clip = def->text((base + "clip").c_str(), f.clip);
        f.clipSlack = std::max(0.0f, static_cast<float>(def->number((base + "clipSlack").c_str(), f.clipSlack)));
        f.clipHitSeconds = std::max(0.0f, static_cast<float>(def->number((base + "clipHitSeconds").c_str(), f.clipHitSeconds)));
        f.seconds = std::max(0.2f, static_cast<float>(def->number((base + "seconds").c_str(), f.seconds)));
        f.hitSeconds = std::clamp(static_cast<float>(def->number((base + "hitSeconds").c_str(), f.hitSeconds)), 0.1f, f.seconds - 0.05f);
        f.standOff = std::max(0.2f, static_cast<float>(def->number((base + "standOff").c_str(), f.standOff)));
        f.kneel = def->flag((base + "kneel").c_str(), f.kneel);
        f.bend = static_cast<float>(def->number((base + "bend").c_str(), f.bend));
        r.finishers.push_back(std::move(f));
    }
    num("finisherStepMetres", r.finisherStepMetres);
    r.arcRaysPerSide = std::clamp(r.arcRaysPerSide, 0, 8);
    r.minWindup = std::max(1, r.minWindup);
    r.unarmedMinWindup = std::max(1, r.unarmedMinWindup);
    r.attackTimeFactor = std::max(0.05f, r.attackTimeFactor);
    r.unarmedTimeFactor = std::max(0.05f, r.unarmedTimeFactor);
    r.unarmedPowerFactor = std::max(0.0f, r.unarmedPowerFactor);
    if (r.unarmedWindupShare < 0.0f) {
        r.unarmedWindupShare = r.windupShare;
    }
    r.unarmedWindupShare = std::clamp(r.unarmedWindupShare, 0.0f, 1.0f);
    r.unarmedGapSteps = std::max(0, r.unarmedGapSteps);
    r.playerFistChainSteps = std::max(0, r.playerFistChainSteps);
    r.lowTimeFactor = std::max(0.05f, r.lowTimeFactor);
    r.commitSteps = std::max(0, r.commitSteps);
    r.guardBlocks = std::max(1, r.guardBlocks);
    for (float& f : r.skillSpeed) {
        f = std::max(0.05f, f);
    }
}

// Steps of a tool's swing spent winding up; the rest of its cooldown is recovery.
int meleeWindup(int cooldown, int minWindup, float windupShare) {
    return std::clamp(static_cast<int>(std::lround(static_cast<float>(cooldown) * windupShare)), minWindup,
                      std::max(minWindup, cooldown - 1));
}

constexpr float kMotionStepSecondsMelee = 1.0f / 30.0f;  // a step's real seconds: Mover speeds are metres per step

bool meleeAble(const entt::registry& reg, Entity pawn) {
    const Health* h = reg.try_get<Health>(pawn);
    // A stunned (X2) or knocked-down (poise, part C) pawn can't swing, block or dodge.
    return (h == nullptr || !h->incapacitated()) && !reg.any_of<Stunned, Patching, KnockedDown>(pawn);
}

}  // namespace

MeleeSystem::MeleeSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, const DefDatabase& defs,
                         const GameTime& time, uint64_t seed)
    : things_(things), map_(map), combat_(combat), health_(health), defs_(defs), time_(time), seed_(seed) {
    meleeReadRules(defs_, rules_);
    soulsReadRules(defs_, souls_);
    arcTypes_.assign(defs_.size(), 0);
    for (const std::string& name : rules_.arcDamageTypes) {
        const DefId id = defs_.idOf("DamageTypes", name);
        if (id.valid() && id.value < arcTypes_.size()) {
            arcTypes_[id.value] = 1;
        }
    }
    const Def* rulesDef = defs_.find("Rules", "MeleeRules");
    tools_.resize(defs_.size());
    for (const DefId id : defs_.idsOfKind("Things")) {
        const Def& def = defs_.get(id);
        const size_t n = def.count("tools");
        const bool weapon = combat_.isMeleeWeapon(id);
        if (n == 0 || (!weapon && !def.has("race"))) {
            continue;
        }
        ToolSet& set = tools_[id.value];
        const float cooldownFactor = static_cast<float>(def.number("stats.meleeCooldownMultiplier", 1.0));
        for (size_t i = 0; i < n; ++i) {
            const std::string base = "tools." + std::to_string(i);
            Tool t;
            t.label = def.text((base + ".label").c_str());
            t.damageType = defs_.idOf("DamageTypes", def.textAt((base + ".capacities").c_str(), 0));
            t.power = static_cast<float>(def.number((base + ".power").c_str(), 0.0));
            // sneed 2026-11-23: swings paced by attackTimeFactor; a person's own fists and head by unarmedTimeFactor
            // alone (quick, weak jabs with a short pause after each).
            const bool unarmedPerson = !weapon && def.text("race.intelligence") == "Humanlike";
            const float timeFactor = unarmedPerson ? rules_.unarmedTimeFactor : rules_.attackTimeFactor;
            t.cooldown = std::max(2, static_cast<int>(std::lround(def.number((base + ".cooldownTime").c_str(), 60.0) * cooldownFactor *
                                                                  static_cast<double>(timeFactor))));
            t.minWindup = unarmedPerson ? rules_.unarmedMinWindup
                                        : std::max(1, static_cast<int>(std::lround(static_cast<float>(rules_.minWindup) * rules_.attackTimeFactor)));
            t.windupShare = unarmedPerson ? rules_.unarmedWindupShare : rules_.windupShare;
            if (unarmedPerson) {
                t.power *= rules_.unarmedPowerFactor;
                t.gapSteps = rules_.unarmedGapSteps;
            }
            t.penetration = static_cast<float>(
                def.number((base + ".armorPenetration").c_str(), static_cast<double>(t.power * rules_.penetrationPerDamage)));
            // Ours: a tool is picked by chanceFactor times power, so the strongest is used most (RimWorld
            // weighs by damage too, by a rule the wiki doesn't give).
            t.weight = static_cast<float>(def.number((base + ".chanceFactor").c_str(), 1.0)) * std::max(0.1f, t.power);
            if (t.damageType.valid() && t.power > 0.0f) {
                set.tools.push_back(std::move(t));
            }
        }
        set.reach = rules_.bodyReach;
        if (!weapon && (def.text("race.intelligence") == "Animal" || def.text("race.intelligence") == "Mechanoid")) {
            set.reach = rules_.animalReachBase +
                        rules_.animalReachPerRoot * std::sqrt(static_cast<float>(std::max(0.1, def.number("race.baseBodySize", 1.0))));
        }
        if (weapon) {
            set.reach = rules_.defaultWeaponReach;
            if (rulesDef != nullptr) {
                set.reach = static_cast<float>(rulesDef->number(("weaponReach." + def.defName()).c_str(), set.reach));
            }
        }
        // The style's attack clips (body-motion.md): only the short, quick ones.
        // The weapon's hold style says which attack clips it swings with (docs/design/weapon-hold.md); MeleeRules'
        // weaponStyle can still override one weapon.
        std::string style = weapon ? std::string() : rules_.unarmedStyle;
        if (weapon) {
            WeaponHold hold;
            if (holdFor(defs_, id, hold)) {
                style = hold.attackStyle;
            }
        }
        for (const auto& [name, st] : rules_.weaponStyle) {
            if (weapon && name == def.defName()) {
                style = st;
            }
        }
        const BodyMotionSystem& motion = combat_.motion();
        // Only short, quick clips that stay near the target.
        const auto usable = [&](const std::string& clipName) -> const MotionClip* {
            const MotionClip* c = motion.clip(clipName);
            if (c == nullptr || c->hitSeconds <= 0.0f || c->hitSeconds > rules_.maxAttackHitSeconds ||
                c->seconds > rules_.maxAttackClipSeconds) {
                return nullptr;
            }
            // A clip that carries the body further than this is a charge, not a swing: it would run past the target.
            float travel = 0.0f;
            for (size_t i = 0; i < c->forward.size() && i < c->right.size(); ++i) {
                travel = std::max(travel, std::sqrt(c->forward[i] * c->forward[i] + c->right[i] * c->right[i]));
            }
            return travel > rules_.maxAttackTravel ? nullptr : c;
        };
        // The weapon's own list (a spear thrusts, sneed 2026-11-18) wins when any of its clips is measured; else its style's.
        const auto takeClips = [&](const std::string& key) {
            for (const auto& [name, clips] : rules_.styleAttacks) {
                if (name != key) {
                    continue;
                }
                for (const std::string& clipName : clips) {
                    const MotionClip* c = usable(clipName);
                    if (c == nullptr) {
                        continue;
                    }
                    const float lunge = std::max(0.0f, motion.forwardAt(c->def, c->hitSeconds));
                    set.lunge = set.attackClips.empty() ? lunge : std::min(set.lunge, lunge);
                    set.attackClips.push_back(c->def);
                }
            }
        };
        if (weapon) {
            takeClips(def.defName());
        }
        if (set.attackClips.empty()) {
            takeClips(style);
        }
        // Blows at the downed: the weapon's own list if it has one, else its style's (a body's tools: the unarmed style).
        const std::vector<std::string>* low = nullptr;
        for (const auto& [key, clips] : rules_.lowAttacks) {
            if (weapon && key == def.defName()) {
                low = &clips;
            } else if (low == nullptr && key == style) {
                low = &clips;
            }
        }
        if (low != nullptr && !set.attackClips.empty()) {
            for (const std::string& clipName : *low) {
                if (const MotionClip* c = usable(clipName)) {
                    set.lowAttackClips.push_back(c->def);
                }
            }
        }
        // Its Elden Ring style moveset (souls-combat.md): listed by weapon (or a creature's race def), else by style.
        for (size_t i = 0; i < souls_.movesets.size(); ++i) {
            const SoulsMoveset& ms = souls_.movesets[i];
            const bool named = std::find(ms.weapons.begin(), ms.weapons.end(), def.defName()) != ms.weapons.end();
            const bool styled = weapon && std::find(ms.styles.begin(), ms.styles.end(), style) != ms.styles.end();
            if (named || (styled && set.moveset < 0)) {
                set.moveset = static_cast<int>(i);
            }
        }
        // A finisher at the downed (sneed 2026-11-20): the weapon's own entry if it has one, else its style's.
        if (weapon) {
            for (size_t i = 0; i < rules_.finishers.size(); ++i) {
                if (rules_.finishers[i].key == def.defName()) {
                    set.finisher = static_cast<int>(i) + 1;
                } else if (set.finisher == 0 && rules_.finishers[i].key == style) {
                    set.finisher = static_cast<int>(i) + 1;
                }
            }
        }
    }
}

MeleeSystem::~MeleeSystem() = default;

int64_t MeleeSystem::now() const {
    return time_.steps;  // real time: swings and blocks keep their pace whatever the day length
}

// ---- What a pawn fights with -----------------------------------------------------------------

const MeleeSystem::ToolSet* MeleeSystem::toolsOf(Entity pawn, bool& weapon, bool fists) const {
    const entt::registry& reg = things_.registry();
    weapon = false;
    if (const Gear* gear = reg.try_get<Gear>(pawn); !fists && gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon)) {
        const DefId def = reg.get<Thing>(gear->weapon).def;
        if (def.value < tools_.size() && !tools_[def.value].tools.empty()) {
            weapon = true;
            return &tools_[def.value];
        }
    }
    // A gun or nothing in hand: fists (or teeth and claws).
    const DefId def = reg.get<Thing>(pawn).def;
    if (def.value < tools_.size() && !tools_[def.value].tools.empty()) {
        return &tools_[def.value];
    }
    return nullptr;
}

bool MeleeSystem::strongSwing(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    if (m == nullptr) {
        return false;
    }
    if (m->kind != SoulsAttack::None) {
        return m->kind == SoulsAttack::Heavy || m->kind == SoulsAttack::Charged || m->kind == SoulsAttack::RunningHeavy ||
               m->kind == SoulsAttack::JumpHeavy || m->kind == SoulsAttack::GuardCounter;
    }
    const Tool* tool = toolOf(pawn, *m);
    if (tool == nullptr) {
        return false;
    }
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon, !m->weaponTool);
    if (set == nullptr || set->tools.size() < 2) {
        return false;
    }
    float sum = 0.0f;
    for (const Tool& t : set->tools) {
        sum += t.power;
    }
    const float mean = sum / static_cast<float>(set->tools.size());
    return mean > 0.0f && tool->power >= rules_.strongPowerShare * mean;
}

const MeleeSystem::Tool* MeleeSystem::toolOf(Entity pawn, const Melee& m) const {
    const entt::registry& reg = things_.registry();
    const ToolSet* set = nullptr;
    if (m.weaponTool) {
        const Gear* gear = reg.try_get<Gear>(pawn);
        if (gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon)) {
            const DefId def = reg.get<Thing>(gear->weapon).def;
            set = def.value < tools_.size() ? &tools_[def.value] : nullptr;
        }
    } else {
        const DefId def = reg.get<Thing>(pawn).def;
        set = def.value < tools_.size() ? &tools_[def.value] : nullptr;
    }
    return set != nullptr && m.tool < set->tools.size() ? &set->tools[m.tool] : nullptr;
}

float MeleeSystem::reach(Entity pawn, bool fists) const {
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon, fists);
    if (set == nullptr) {
        return 0.0f;
    }
    // A taller person reaches further (their arm, not their weapon: reachHeightFollow).
    const PawnLooks* looks = things_.registry().try_get<PawnLooks>(pawn);
    const float height = looks != nullptr ? std::clamp(looks->height, 0.5f, 1.5f) : 1.0f;
    return set->reach * (1.0f + (height - 1.0f) * rules_.reachHeightFollow);
}

float MeleeSystem::engageDistance(Entity pawn, Entity target, bool fists) const {
    const entt::registry& reg = things_.registry();
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon, fists);
    if (set == nullptr) {
        return 0.0f;
    }
    const BodyMotionSystem& motion = combat_.motion();
    float d = reach(pawn, fists);
    if (reg.valid(target)) {
        d += std::max(0.0f, motion.bodyRadius(target) - motion.rules().bodyRadius);  // a big body's flank is nearer than its centre
    }
    if (motion.hasClips(pawn)) {
        d += set->lunge * motion.sizeScale(pawn);  // its clips step in before the blow
    }
    return d;
}

bool MeleeSystem::inReach(Entity pawn, Entity target, bool fists) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.valid(target) || !reg.all_of<Position>(pawn) || !reg.all_of<Position>(target)) {
        return false;
    }
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(target);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float r = engageDistance(pawn, target, fists) + rules_.reachSlack;
    return a.level == b.level && dx * dx + dy * dy <= r * r;
}

bool MeleeSystem::charges(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.try_get<Gear>(pawn);
    const bool hasWeapon = gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon);
    if (hasWeapon && combat_.isGun(reg.get<Thing>(gear->weapon).def)) {
        return false;  // armed for shooting
    }
    if (hasWeapon && combat_.isMeleeWeapon(reg.get<Thing>(gear->weapon).def)) {
        return true;
    }
    if (const Melee* m = reg.try_get<Melee>(pawn); m != nullptr && m->response == FightResponse::Fists) {
        return true;  // it fights with what it has, its fists if nothing (docs/design/focus.md)
    }
    if (const Colonist* colonist = reg.try_get<Colonist>(pawn)) {
        for (const TraitRecord& trait : colonist->traits) {
            if (std::find(rules_.chargeTraits.begin(), rules_.chargeTraits.end(), trait.def) != rules_.chargeTraits.end()) {
                return true;
            }
        }
    }
    return false;
}

int MeleeSystem::meleeSkill(Entity pawn) const {
    if (const Colonist* colonist = things_.registry().try_get<Colonist>(pawn)) {
        if (const SkillRecord* skill = colonist->skill("Melee")) {
            return std::clamp(skill->level, 0, kSkillLevelLimit);
        }
    }
    return std::clamp(rules_.defaultSkill, 0, 20);
}

float MeleeSystem::swingSpeed(Entity pawn) const { return meleeBySkill(rules_.skillSpeed, meleeSkill(pawn)); }

float MeleeSystem::swingDamage(Entity pawn) const { return meleeBySkill(rules_.skillDamage, meleeSkill(pawn)); }

float MeleeSystem::aimWobble(Entity pawn) const { return std::max(0.0f, meleeBySkill(rules_.aimWobble, meleeSkill(pawn))); }

// Armour weight slows a swing (docs/design/armour.md); melee skill speeds it up or slows it down (real-time-combat.md).
float MeleeSystem::swingTimeFactor(Entity pawn) const {
    return combat_.attackTimeFactor(pawn) / swingSpeed(pawn);  // armour weight and exhaustion (CombatSystem)
}

float MeleeSystem::reactionSteps(Entity pawn) const {
    float steps = meleeBySkill(rules_.reactionSteps, meleeSkill(pawn));
    if (const Colonist* colonist = things_.registry().try_get<Colonist>(pawn)) {
        for (const TraitRecord& trait : colonist->traits) {
            for (const auto& [name, offset] : rules_.reactionTraitSteps) {
                if (trait.def == name) {
                    steps += offset;
                }
            }
        }
    }
    return std::max(1.0f, steps);
}

bool MeleeSystem::guardBroken(Entity pawn) const {
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    return m != nullptr && now() < m->guardBrokenUntil;
}

bool MeleeSystem::guardedAgainst(Entity target, Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(target) || !reg.valid(pawn) || !meleeAble(reg, target) || guardBroken(target)) {
        return false;
    }
    const bool shieldUp = reg.all_of<Blocking>(target) && combat_.apparelStats().activeShield(reg, target) != kNoEntity;
    if (!shieldUp && !blocking(target)) {
        return false;
    }
    const Position* tp = reg.try_get<Position>(target);
    const Position* pp = reg.try_get<Position>(pawn);
    const Posture* posture = reg.try_get<Posture>(target);
    if (tp == nullptr || pp == nullptr || posture == nullptr) {
        return false;
    }
    const float dx = pp->x - tp->x;
    const float dy = pp->y - tp->y;
    const float len = std::sqrt(dx * dx + dy * dy);
    return len < 0.01f || (posture->facingX * dx + posture->facingY * dy) / len >= rules_.guardFacingCos;
}

// Real-time-combat.md phase 2: don't swing into a raised guard. Hold, facing it, until it drops (a shield-bearer lowers
// its shield to strike, a parry ends with the blow) or guardWaitSteps have passed; then press anyway, since every parry
// wears the guard down.
bool MeleeSystem::holdSwing(Entity pawn, Entity target) {
    entt::registry& reg = things_.registry();
    if (!guardedAgainst(target, pawn)) {
        if (Melee* m = reg.try_get<Melee>(pawn)) {
            m->guardWaitFrom = 0;
        }
        return false;
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    if (m.guardWaitFrom == 0) {
        m.guardWaitFrom = now();
    }
    if (now() - m.guardWaitFrom >= rules_.guardWaitSteps) {
        m.guardWaitFrom = 0;  // pressing now; the next wait starts afresh
        return false;
    }
    const Position& to = reg.get<Position>(target);
    face(pawn, to.x, to.y);
    return true;
}

std::vector<DefId> MeleeSystem::attackClipsOf(DefId def) const {
    if (!def.valid() || def.value >= tools_.size()) {
        return {};
    }
    return tools_[def.value].attackClips;
}

std::vector<DefId> MeleeSystem::lowAttackClipsOf(DefId def) const {
    if (!def.valid() || def.value >= tools_.size()) {
        return {};
    }
    return tools_[def.value].lowAttackClips;
}

int MeleeSystem::finisherFor(DefId weapon) const {
    return weapon.valid() && weapon.value < tools_.size() ? tools_[weapon.value].finisher : 0;
}

bool MeleeSystem::finisherPlaysClip(int finisher) const {
    if (finisher <= 0 || static_cast<size_t>(finisher) > rules_.finishers.size()) {
        return false;
    }
    const MeleeFinisher& f = rules_.finishers[static_cast<size_t>(finisher - 1)];
    const MotionClip* c = f.clip.empty() ? nullptr : combat_.motion().clip(f.clip);
    return c != nullptr && c->hitSeconds > 0.0f;
}

float MeleeSystem::finisherClipHit(int finisher) const {
    if (!finisherPlaysClip(finisher)) {
        return 0.0f;
    }
    const MeleeFinisher& f = rules_.finishers[static_cast<size_t>(finisher - 1)];
    const MotionClip* c = combat_.motion().clip(f.clip);
    return f.clipHitSeconds > 0.0f ? std::clamp(f.clipHitSeconds, 0.05f, c->seconds - 0.05f) : c->hitSeconds;
}

const MeleeFinisher* MeleeSystem::finisherOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    if (m == nullptr || m->finisher == 0 || m->phase == MeleePhase::Ready || m->finisher > rules_.finishers.size()) {
        return nullptr;
    }
    return &rules_.finishers[m->finisher - 1];
}

float MeleeSystem::finisherProgress(Entity pawn) const {
    const Melee* m = finisherOf(pawn) != nullptr ? things_.registry().try_get<Melee>(pawn) : nullptr;
    if (m == nullptr || m->finisherSteps <= 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(now() - m->finisherFrom) / static_cast<float>(m->finisherSteps), 0.0f, 1.0f);
}

bool MeleeSystem::ready(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !meleeAble(reg, pawn)) {
        return false;
    }
    const Melee* m = reg.try_get<Melee>(pawn);
    return (m == nullptr || (m->phase == MeleePhase::Ready && now() >= m->stunUntil)) && !combat_.switching(pawn) &&
           !isDressing(reg, pawn) && !reg.all_of<Blocking>(pawn);
}

bool MeleeSystem::winding(Entity pawn) const {
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    return m != nullptr && m->phase == MeleePhase::Windup;
}

bool MeleeSystem::freeFeetToWalk(Entity pawn, float move) {
    if (!combat_.motion().holdsBody(pawn)) {
        return true;
    }
    // An Elden Ring style attack past its cancel point ends when the player walks (souls-combat.md).
    if (souls_.enabled && move >= 0.005f && soulsCancel(pawn)) {
        return true;
    }
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    if (!rules_.stepInFreesFeet || move < 0.005f || m == nullptr || !meleeFreeFeet(*m) || m->finisher != 0 ||
        (m->phase != MeleePhase::Windup && m->phase != MeleePhase::Recovery)) {
        return false;
    }
    combat_.motion().setLayers(pawn, true, true);
    return true;
}

bool MeleeSystem::blocking(Entity pawn) const {
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    return m != nullptr && now() >= m->blockFrom && now() < m->blockUntil;
}

// ---- Orders ----------------------------------------------------------------------------------

bool MeleeSystem::attack(Entity pawn, Entity target, bool player, std::string* why, bool fists) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    // Elden Ring style (souls-combat.md): a pawn with a moveset swings a light attack of its combo; a blow at a body on
    // the ground keeps its low blow or finisher below.
    if (souls_.enabled && !fists && hasMoveset(pawn) && !soulsLowTarget(pawn, target, player)) {
        return soulsAttack(pawn, target, player, SoulsPress{}, why);
    }
    if (!things_.spawned(pawn) || !reg.all_of<Posture, Mover>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    if (!meleeAble(reg, pawn)) {
        return fail("that pawn can't fight (downed or dead)");
    }
    if (combat_.rollLocked(pawn)) {
        return fail("rolling");
    }
    if (const Melee* stunned = reg.try_get<Melee>(pawn); stunned != nullptr && now() < stunned->stunUntil) {
        return fail("staggered");
    }
    if (combat_.switching(pawn)) {
        return fail("switching weapons");
    }
    if (isDressing(reg, pawn)) {
        return fail("putting on armour");
    }
    if (reg.all_of<Blocking>(pawn)) {
        return fail("holding up a shield");
    }
    if (const Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
        return fail("busy shooting");
    }
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon, fists);
    if (const Melee* m = reg.try_get<Melee>(pawn); m != nullptr && m->phase != MeleePhase::Ready) {
        // The player's bare fists chain: the next punch may start in the last playerFistChainSteps of the recovery.
        const bool chain = player && !weapon && set != nullptr && m->phase == MeleePhase::Recovery && m->finisher == 0 &&
                           m->stepsLeft <= rules_.playerFistChainSteps;
        if (!chain) {
            return fail("not ready to swing yet");
        }
    }
    if (set == nullptr) {
        return fail("that pawn has nothing to fight with");
    }
    if (target == kNoEntity && !player) {
        return fail("a swing needs a target");
    }
    if (target != kNoEntity) {
        const Health* h = reg.valid(target) ? reg.try_get<Health>(target) : nullptr;
        if (target == pawn || !things_.spawned(target) || !reg.all_of<Posture>(target) || (h != nullptr && h->dead)) {
            return fail("the target isn't a living pawn on the map");
        }
        // The player walks on under its swing (#361), so a target a little past its reach still gets it: it lands only if
        // the player really closes in (the blow itself is as long as the reach).
        if (!inReach(pawn, target, fists) && !(player && inPlayerReach(pawn, target))) {
            return fail("the target is out of reach");
        }
    }
    // Stamina (souls-combat.md): every swing costs some, and none is left for one at 0.
    if (souls_.enabled) {
        if (stamina(pawn) <= 0.0f) {
            return fail("out of stamina");
        }
        Melee probe;
        probe.weaponTool = weapon;
        spendStamina(pawn, soulsCost(pawn, probe));
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    m.kind = SoulsAttack::None;
    m.seed = splitMix64(seed_ ^ (static_cast<uint64_t>(now()) << 20) ^ (uint64_t{entt::to_integral(pawn)} * 0x9E3779B97F4A7C15ULL));
    Rng rng(m.seed);
    float total = 0.0f;
    for (const Tool& t : set->tools) {
        total += t.weight;
    }
    const float roll = rng.unit() * total;
    float acc = 0.0f;
    m.tool = 0;
    for (size_t i = 0; i < set->tools.size(); ++i) {
        acc += set->tools[i].weight;
        m.tool = static_cast<uint8_t>(i);
        if (roll < acc) {
            break;
        }
    }
    const Tool& tool = set->tools[m.tool];
    // Worn armour's weight slows a swing (docs/design/armour.md), melee skill changes its pace (real-time-combat.md).
    int windup =
        std::max(1, static_cast<int>(std::lround(static_cast<float>(meleeWindup(tool.cooldown, tool.minWindup, tool.windupShare)) * swingTimeFactor(pawn))));
    m.phase = MeleePhase::Windup;
    m.target = target;
    // A blow at a body on the ground goes down at it (the strike already aims at its aim point there): Unreal plays a
    // downward poke, smash or hack (docs/design/focus.md).
    // The player's swing at nobody, with nobody living to take it, goes down at a corpse lying before it (sneed,
    // 2026-11-22): the downward clip now, the corpse its target when the line is fixed (commit).
    const Entity corpse = player && target == kNoEntity && playerTarget(pawn) == kNoEntity ? corpseAhead(pawn) : kNoEntity;
    m.low = (target != kNoEntity && (health_.incapacitated(target) || reg.get<Posture>(target).stance == Stance::Prone)) ||
            corpse != kNoEntity;
    if (corpse != kNoEntity) {
        const Position& to = reg.get<Position>(corpse);
        face(pawn, to.x, to.y);
    }
    // sneed 2026-11-24: blows at a body on the ground come quicker (MeleeRules lowTimeFactor).
    if (m.low) {
        windup = std::max(1, static_cast<int>(std::lround(static_cast<float>(windup) * rules_.lowTimeFactor)));
    }
    // The body on the ground the blow is for: the target, or the corpse before a player swinging at nobody (sneed
    // 2026-11-24: a corpse gets the very same move, finisher and all, as a downed enemy would).
    const Entity lowBody = target != kNoEntity ? target : corpse;
    // At a body on the ground with a weapon that has one, a finisher (sneed 2026-11-20): its own slow move, timed by
    // MeleeRules finishers, the attacker stepping to its place beside the body and holding still till it is over.
    m.finisher = 0;
    m.finisherSteps = 0;
    // A finisher with a whole-body clip (sneed 2026-11-22) plays it where the attacker stands, so only close enough to
    // the body; further off (or with no clips for this body) the weapon's low blow below.
    const MeleeFinisher* finisherRule = m.low && lowBody != kNoEntity && weapon && set->finisher > 0 && static_cast<size_t>(set->finisher) <= rules_.finishers.size()
                                            ? &rules_.finishers[static_cast<size_t>(set->finisher - 1)]
                                            : nullptr;
    const MotionClip* finisherClip = nullptr;
    float clipBlowAt = 0.0f;  // when in the clip its blow lands
    float finisherShift = 0.0f;  // metres it moves in (out, if negative) over the clip to be at standOff at the blow
    if (finisherRule != nullptr && finisherPlaysClip(set->finisher) && combat_.motion().hasClips(pawn)) {
        // (A clip not measured yet leaves the old drawn finisher.)
        finisherClip = combat_.motion().clip(finisherRule->clip);
        clipBlowAt = finisherClipHit(set->finisher);
        const Position& a = reg.get<Position>(pawn);
        const Position& b = reg.get<Position>(lowBody);
        const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
        // Where the clip alone would leave it at the blow: its own step forward (scaled to the body) taken off.
        finisherShift = d - combat_.motion().forwardAt(finisherClip->def, clipBlowAt) * combat_.motion().sizeScale(pawn) -
                        finisherRule->standOff;
        if (finisherShift > finisherRule->clipSlack) {
            finisherRule = nullptr;  // too far off: the plain low blow
            finisherClip = nullptr;
        } else {
            // Right on top of the body it steps back only so far (the blow lands a little short of its place rather than
            // the finisher giving way to the arms-only low blow: PC pictures 2026-11-22, the gladius's never played).
            finisherShift = std::max(finisherShift, -finisherRule->clipSlack);
        }
    }
    if (finisherRule != nullptr) {
        const MeleeFinisher& f = *finisherRule;
        m.finisher = static_cast<uint8_t>(set->finisher);
        m.finisherFrom = now();
        m.finisherSteps = std::max(2, static_cast<int>(std::lround(f.seconds * 30.0f * rules_.attackTimeFactor * rules_.lowTimeFactor)));
        windup = std::clamp(static_cast<int>(std::lround(f.hitSeconds * 30.0f * rules_.attackTimeFactor * rules_.lowTimeFactor)), 1, m.finisherSteps - 1);
        if (target == kNoEntity) {
            m.target = lowBody;  // the corpse: the finisher holds over it (it is the target when the line is fixed anyway)
        }
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->vx = 0.0f;  // the player's last walk no longer turns it
            mover->vy = 0.0f;
        }
    }
    if (finisherClip != nullptr) {
        // The whole body plays the clip, timed so its blow lands on the hit step; the finisher lasts as long as the clip.
        const Position& a = reg.get<Position>(pawn);
        const Position& b = reg.get<Position>(lowBody);
        combat_.motion().playTimed(pawn, finisherClip->def, clipBlowAt, windup, b.x - a.x, b.y - a.y, false);
        // To its place by the body during the clip's start (no glide before it): done by two thirds of the wind-up.
        if (std::fabs(finisherShift) > 0.02f) {
            combat_.motion().shiftClip(pawn, finisherShift, std::max(4, windup * 2 / 3));
        }
        const float stepsPerClipSecond = static_cast<float>(windup) / clipBlowAt;
        m.finisherSteps = windup + std::max(1, static_cast<int>(std::lround((finisherClip->seconds - clipBlowAt) * stepsPerClipSecond)));
    } else if (finisherRule != nullptr) {
        const MeleeFinisher& f = *finisherRule;
        // To its place: standOff from the body's centre, on the side it came from.
        const Position& a = reg.get<Position>(pawn);
        const Position& b = reg.get<Position>(lowBody);
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 0.01f && std::fabs(d - f.standOff) > 0.05f && !combat_.motion().moving(pawn)) {
            dx /= d;
            dy /= d;
            const float metres = d - f.standOff;  // positive: in towards the body
            const int steps = std::max(3, static_cast<int>(std::ceil(std::fabs(metres) / std::max(0.01f, rules_.finisherStepMetres))));
            combat_.motion().push(pawn, metres > 0.0f ? -dx : dx, metres > 0.0f ? -dy : dy, std::fabs(metres), std::min(steps, windup));
        }
    }
    m.stepsLeft = windup;
    if (target != kNoEntity) {
        m.engagedAt = now();
    }
    m.footworkUntil = 0;
    m.weaponTool = weapon;
    m.player = player;
    m.committed = false;
    ++swings_;
    if (target != kNoEntity) {
        const Position& to = reg.get<Position>(target);
        face(pawn, to.x, to.y);
        prepareDefense(target, pawn, windup, m.seed);
    }
    // A person's swing is one of its style's attack clips, timed so the clip's blow lands on the hit step; the clip's
    // step forward moves the body through the world (docs/design/body-motion.md). Faster swings play the clip faster.
    // A blow at a body on the ground plays one of the downward clips when the weapon has any (focus.md).
    if (m.finisher == 0 && !set->attackClips.empty() && combat_.motion().hasClips(pawn) && !reg.all_of<BodyMotion>(pawn)) {
        const std::vector<DefId>& clips = m.low && !set->lowAttackClips.empty() ? set->lowAttackClips : set->attackClips;
        Rng pick(splitMix64(m.seed ^ 0xC11FULL));
        const DefId clipDef = clips[static_cast<size_t>(pick.unit() * static_cast<float>(clips.size())) % clips.size()];
        const Posture& posture = reg.get<Posture>(pawn);
        combat_.motion().playTimed(pawn, clipDef, combat_.motion().clip(clipDef)->hitSeconds, windup, posture.facingX, posture.facingY,
                                   false);
        // The player's swing is the arms' and the chest's: the player keeps walking under it (sneed 2026-11-18). A
        // blow at someone on the ground (a stomp, an overhead smash) stays the whole body's.
        // From standing (sneed 2026-11-23) the whole body plays it: the clip's step forward carries the feet into the
        // blow (freeFeetToWalk hands it to the upper body if the player sets off during it).
        if (meleeFreeFeet(m)) {
            bool standing = false;
            if (rules_.playerStepIn) {
                const Mover* mover = reg.try_get<Mover>(pawn);
                const float perStep = mover != nullptr ? std::sqrt(mover->vx * mover->vx + mover->vy * mover->vy) : 0.0f;
                standing = perStep < rules_.stepInMaxSpeed * kMotionStepSecondsMelee;
            }
            if (!standing) {
                combat_.motion().setLayers(pawn, true, true);
            }
        }
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::MeleeSwing;
        e.thing = pawn;
        e.target = target;
        e.def = weapon ? reg.get<Thing>(reg.get<Gear>(pawn).weapon).def : reg.get<Thing>(pawn).def;
        e.amount = static_cast<float>(windup);
        e.message = tool.label;
        e.count = strongSwing(pawn) ? 1 : 0;
        e.cell = cellOf(reg.get<Position>(pawn));
        events_->push_back(std::move(e));
    }
    return true;
}

bool MeleeSystem::block(Entity pawn, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!things_.spawned(pawn) || !reg.all_of<Posture, Mover>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    if (!meleeAble(reg, pawn)) {
        return fail("that pawn can't fight (downed or dead)");
    }
    if (combat_.rollLocked(pawn)) {
        return fail("rolling");
    }
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon);
    // A melee weapon blocks; so do bare fists (sneed 2026-11-22: "right click with a melee weapon or fists doesn't
    // block"), a person's guard: they stop a punch, and take a weapon's blow on the arm (strike). A gun aims instead.
    if (const Gear* gear = reg.try_get<Gear>(pawn); !weapon && gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon)) {
        return fail("a gun aims, it doesn't block");
    }
    if (set == nullptr || (!weapon && !combat_.motion().hasClips(pawn))) {
        return fail("only a melee weapon or a person's fists can block");
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    // Held (the player keeps the button down): a block already up, or about to be, is kept up a moment longer each
    // time, so it lasts as long as the button is held and drops soon after it is let go.
    if (now() < m.blockUntil && m.phase == MeleePhase::Ready) {
        m.blockUntil = std::max(m.blockUntil, now() + rules_.playerBlockHoldSteps);
        return true;
    }
    if (m.phase != MeleePhase::Ready) {
        return fail("not ready to block yet");
    }
    m.blockFrom = now() + rules_.blockSteps;
    m.blockUntil = m.blockFrom + rules_.playerBlockSteps;
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.action = PawnAction::Block;
        e.thing = pawn;
        e.def = reg.get<Thing>(pawn).def;
        e.cell = cellOf(reg.get<Position>(pawn));
        events_->push_back(std::move(e));
    }
    return true;
}

// ---- Defending -------------------------------------------------------------------------------

// A swing at `defender` has begun. It reads it after its reaction time (Melee skill, Nimble), then does the sensible
// thing if it read it in time (docs/design/real-time-combat.md): a shield on its arm goes up (the fighter AI's
// shieldAct), a melee weapon parries unless its guard is broken, otherwise a fighter sidesteps. No dice: a late
// read, a fast swing or a wall at its side and the blow lands. The player defends by hand (MeleeCommand Block, the
// roll), so a controlled pawn does nothing by itself.
void MeleeSystem::prepareDefense(Entity defender, Entity attacker, int stepsToHit, uint64_t seed) {
    entt::registry& reg = things_.registry();
    if (reg.all_of<PlayerControl>(defender) || !meleeAble(reg, defender) || !reg.all_of<Mover, Posture>(defender)) {
        return;
    }
    Melee& d = reg.get_or_emplace<Melee>(defender);
    d.threat = attacker;
    d.defense = MeleeDefense::None;
    d.dodged = false;
    // Busy with its own swing or a shot, or just after a sidestep or its own strike: no defense.
    const Shooter* s = reg.try_get<Shooter>(defender);
    if (d.phase == MeleePhase::Windup || (s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) ||
        now() < d.noDefendUntil) {
        return;
    }
    if (combat_.apparelStats().activeShield(reg, defender) != kNoEntity && (reg.all_of<Fighter>(defender) || reg.all_of<Blocking>(defender))) {
        return;  // the fighter AI raises the shield, or it is up already: the shield takes the blow, not a weapon parry (shields.md)
    }
    Rng rng(splitMix64(seed ^ 0xD0D6EULL ^ (uint64_t{entt::to_integral(defender)} << 32)));
    const float reaction = reactionSteps(defender) + (2.0f * rng.unit() - 1.0f) * rules_.reactionJitter;
    const int64_t noticed = now() + std::max(1, static_cast<int>(std::lround(reaction)));
    const int64_t hitAt = now() + stepsToHit;
    if (noticed >= hitAt) {
        return;  // too quick for it
    }
    // Read in time, it defends as often as its skill says (sneed 2026-11-19: enemies that dodge, by skill): a parry
    // with a weapon in hand (parryShare of the time for a fighter, which can also get out of the way), else a step back
    // or aside (a roll from a heavy blow).
    if (rng.unit() >= meleeBySkill(rules_.defendChance, meleeSkill(defender))) {
        return;
    }
    bool weapon = false;
    toolsOf(defender, weapon);
    const bool fighter = reg.all_of<Fighter>(defender);
    if (weapon && !guardBroken(defender) && (!fighter || rng.unit() < rules_.parryShare)) {
        d.defense = MeleeDefense::Block;
        d.reactAt = noticed;
        d.blockFrom = noticed + rules_.blockSteps;
        d.blockUntil = hitAt + 1;
    } else if (fighter) {
        // It moves once the attacker is committed to the line (moving sooner, the swing would follow it).
        d.defense = rng.unit() < rules_.backstepShare ? MeleeDefense::Backstep : MeleeDefense::Dodge;
        d.reactAt = std::max(noticed, hitAt - rules_.commitSteps);
    }
}

void MeleeSystem::defend(Entity defender, Melee& d) {
    entt::registry& reg = things_.registry();
    const MeleeDefense defense = d.defense;
    d.defense = MeleeDefense::None;
    const Melee* swing = reg.valid(d.threat) ? reg.try_get<Melee>(d.threat) : nullptr;
    if (swing == nullptr || swing->phase != MeleePhase::Windup || swing->target != defender || !meleeAble(reg, defender) ||
        !things_.spawned(defender)) {
        return;  // the swing is off
    }
    if (defense == MeleeDefense::Block) {
        if (d.blockFrom > now() + swing->stepsLeft) {
            d.blockFrom = d.blockUntil = 0;  // read too late to raise the weapon in time
            return;
        }
        const Position& from = reg.get<Position>(d.threat);
        face(defender, from.x, from.y);
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::PawnAction;
            e.action = PawnAction::Block;
            e.thing = defender;
            e.def = reg.get<Thing>(defender).def;
            e.target = d.threat;
            e.cell = cellOf(reg.get<Position>(defender));
            events_->push_back(std::move(e));
        }
        return;
    }
    if ((defense == MeleeDefense::Dodge || defense == MeleeDefense::Backstep) && swing->stepsLeft > 0) {
        // A heavy blow is worth a roll clear of it (phase 2); a light one a step back out of reach or aside.
        if (!(strongSwing(d.threat) && roll(defender, d)) && !(defense == MeleeDefense::Backstep && backstep(defender, d))) {
            sidestep(defender, d);
        }
    }
}

// The AI's dodge roll (real-time-combat.md phase 2): the player's roll (hit-proof frames, CombatSystem::dodgeRoll),
// across and away from the attacker, to whichever side has rollClearance of open ground. False if neither has.
bool MeleeSystem::roll(Entity defender, Melee& d) {
    entt::registry& reg = things_.registry();
    if (!reg.all_of<Fighter>(defender) || combat_.rollLocked(defender) || now() < d.nextRollAt) {
        return false;
    }
    const Position pos = reg.get<Position>(defender);
    const Position& from = reg.get<Position>(d.threat);
    float ax = pos.x - from.x;
    float ay = pos.y - from.y;
    const float len = std::sqrt(ax * ax + ay * ay);
    if (len < 0.01f) {
        return false;
    }
    ax /= len;
    ay /= len;
    const float first = (Rng(d.reactAt ^ seed_ ^ 0x5011ULL).unit() < 0.5f) ? 1.0f : -1.0f;
    for (const float sign : {first, -first}) {
        // Half across, half away: out of reach and off the line.
        float rx = -ay * sign + ax;
        float ry = ax * sign + ay;
        const float rl = std::sqrt(rx * rx + ry * ry);
        rx /= rl;
        ry /= rl;
        if (!straightWalkIsClear(map_, pos.x, pos.y, pos.x + rx * rules_.rollClearance, pos.y + ry * rules_.rollClearance, kPawnRadius,
                                 pos.level)) {
            continue;
        }
        Mover& mover = reg.get<Mover>(defender);
        mover.path.clear();
        mover.next = 0;
        if (!combat_.dodgeRoll(defender, rx, ry, nullptr, true)) {
            return false;
        }
        d.dodged = true;
        d.nextRollAt = now() + rules_.aiRollEverySteps;
        ++rolls_;
        ++dodges_;
        const int total = combat_.rollTotalSteps();
        d.noDefendUntil = std::max(d.noDefendUntil, now() + total + rules_.dodgeRecoverySteps);
        if (Fighter* f = reg.try_get<Fighter>(defender)) {
            f->dodgeUntil = std::max(f->dodgeUntil, now() + total);
            f->nextPlan = std::min(f->nextPlan, f->dodgeUntil);
        }
        return true;
    }
    return false;
}

// A quick step straight back from the attacker, out of its reach (sneed 2026-11-19), through the body motion system
// (against walls and bodies). False if the way back is blocked.
bool MeleeSystem::backstep(Entity defender, Melee& d) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(defender);
    const Position& from = reg.get<Position>(d.threat);
    float ax = pos.x - from.x;
    float ay = pos.y - from.y;
    const float len = std::sqrt(ax * ax + ay * ay);
    if (len < 0.01f) {
        return false;
    }
    ax /= len;
    ay /= len;
    if (!straightWalkIsClear(map_, pos.x, pos.y, pos.x + ax * rules_.backstepMetres, pos.y + ay * rules_.backstepMetres, kPawnRadius,
                             pos.level)) {
        return false;
    }
    Mover& mover = reg.get<Mover>(defender);
    mover.path.clear();
    mover.next = 0;
    if (!combat_.motion().push(defender, ax, ay, rules_.backstepMetres, rules_.backstepSteps)) {
        return false;
    }
    d.dodged = true;
    ++dodges_;
    const Melee& swing = reg.get<Melee>(d.threat);
    d.noDefendUntil = std::max(d.noDefendUntil, now() + swing.stepsLeft + rules_.dodgeRecoverySteps);
    if (Fighter* f = reg.try_get<Fighter>(defender)) {
        f->dodgeUntil = std::max(f->dodgeUntil, now() + std::max(swing.stepsLeft, rules_.backstepSteps) + rules_.dodgeHoldSteps);
        f->nextPlan = std::min(f->nextPlan, f->dodgeUntil);
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.action = PawnAction::Dodge;
        e.thing = defender;
        e.def = reg.get<Thing>(defender).def;
        e.target = d.threat;
        e.cell = cellOf(reg.get<Position>(defender));
        events_->push_back(std::move(e));
    }
    return true;
}

// A one-cell step to the side of the line from the attacker, whichever side is free. Nothing if
// walls and other pawns' cells leave no room.
void MeleeSystem::sidestep(Entity defender, Melee& d) {
    entt::registry& reg = things_.registry();
    const Position pos = reg.get<Position>(defender);
    const Position& from = reg.get<Position>(d.threat);
    const float dx = pos.x - from.x;
    const float dy = pos.y - from.y;
    const CellPos here = cellOf(pos);
    const float first = (Rng(d.reactAt ^ seed_).unit() < 0.5f) ? 1.0f : -1.0f;
    for (const float sign : {first, -first}) {
        const Dir8Step step = dir8Step(dir8Toward(-dy * sign, dx * sign));
        const CellPos n{here.x + step.dx, here.y + step.dy, here.level};
        if (!map_.walkable(n) || map_.obstacleDm(n) > 0 || map_.isDoor(n) ||
            !straightWalkIsClear(map_, pos.x, pos.y, static_cast<float>(n.x) + 0.5f, static_cast<float>(n.y) + 0.5f,
                                 kPawnRadius, here.level)) {
            continue;
        }
        // A quick step that way: the body really moves there, against walls and bodies (docs/design/body-motion.md).
        Mover& mover = reg.get<Mover>(defender);
        mover.path.clear();
        mover.next = 0;
        combat_.motion().sidestep(defender, -dy * sign, dx * sign);
        d.dodged = true;
        ++dodges_;
        d.noDefendUntil = std::max(d.noDefendUntil, now() + reg.get<Melee>(d.threat).stepsLeft + rules_.dodgeRecoverySteps);
        if (Fighter* f = reg.try_get<Fighter>(defender)) {
            const Melee& swing = reg.get<Melee>(d.threat);
            f->dodgeUntil = std::max(f->dodgeUntil, now() + swing.stepsLeft + rules_.dodgeHoldSteps);
            f->nextPlan = std::min(f->nextPlan, f->dodgeUntil);
        }
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::PawnAction;
            e.action = PawnAction::Dodge;
            e.thing = defender;
            e.def = reg.get<Thing>(defender).def;
            e.target = d.threat;
            e.cell = n;
            events_->push_back(std::move(e));
        }
        return;
    }
}

// ---- Swinging --------------------------------------------------------------------------------

namespace {

// Normalised blend of two unit facings, w 0 gives a, 1 gives b. A blend of near-opposite facings goes round by the
// side b lies on (through the facing at right angles to a), so a half turn never passes through nothing.
void meleeTurnBlend(float ax, float ay, float bx, float by, float w, float& ox, float& oy) {
    const float dot = ax * bx + ay * by;
    if (dot < 0.0f) {
        const float side = (ax * by - ay * bx) >= 0.0f ? 1.0f : -1.0f;
        const float mx = -ay * side;  // a turned a quarter towards b
        const float my = ax * side;
        if (w < 0.5f) {
            meleeTurnBlend(ax, ay, mx, my, w * 2.0f, ox, oy);
        } else {
            meleeTurnBlend(mx, my, bx, by, (w - 0.5f) * 2.0f, ox, oy);
        }
        return;
    }
    const float x = ax + (bx - ax) * w;
    const float y = ay + (by - ay) * w;
    const float len = std::sqrt(x * x + y * y);
    if (len > 1e-6f) {
        ox = x / len;
        oy = y / len;
    } else {
        ox = bx;
        oy = by;
    }
}

}  // namespace

void MeleeSystem::steerRoll(Entity pawn, Entity target) {
    entt::registry& reg = things_.registry();
    Posture* posture = reg.try_get<Posture>(pawn);
    const Position* pos = reg.try_get<Position>(pawn);
    if (posture == nullptr || pos == nullptr || target == kNoEntity || !reg.valid(target) || !things_.spawned(target)) {
        return;
    }
    const Position* at = reg.try_get<Position>(target);
    if (at == nullptr || at->level != pos->level) {
        return;
    }
    // A gun in hand faces its free aim after the roll (lock-on.md), so it has nothing to turn to.
    if (const Gear* gear = reg.try_get<Gear>(pawn);
        gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon) && combat_.isGun(reg.get<Thing>(gear->weapon).def)) {
        return;
    }
    float rx = 0.0f;
    float ry = 0.0f;
    if (!combat_.rollDirection(pawn, rx, ry)) {
        return;
    }
    const float dx = at->x - pos->x;
    const float dy = at->y - pos->y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.05f) {
        return;
    }
    // Held on the roll's way for the first quarter of the tumble, then turned round with an ease in and out over the rest
    // and the recovery (about two thirds of a second), ending on the target as the roll ends: the axis swivels, the path
    // doesn't.
    float from = 0.0f;
    const float progress = combat_.rollProgress(pawn, &from);
    const float t = std::clamp((progress - from) / std::max(1e-4f, 1.0f - from), 0.0f, 1.0f);
    const float w = t * t * (3.0f - 2.0f * t);
    meleeTurnBlend(rx, ry, dx / len, dy / len, w, posture->facingX, posture->facingY);
}

void MeleeSystem::face(Entity pawn, float x, float y) {
    entt::registry& reg = things_.registry();
    Posture* posture = reg.try_get<Posture>(pawn);
    const Position* pos = reg.try_get<Position>(pawn);
    if (posture == nullptr || pos == nullptr) {
        return;
    }
    const float dx = x - pos->x;
    const float dy = y - pos->y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.01f) {
        posture->facingX = dx / len;
        posture->facingY = dy / len;
    }
}

bool MeleeSystem::shoulder(Entity pawn, float& x, float& y, float& z) const {
    const entt::registry& reg = things_.registry();
    const Position* pos = reg.try_get<Position>(pawn);
    const Posture* posture = reg.try_get<Posture>(pawn);
    if (pos == nullptr || posture == nullptr) {
        return false;
    }
    x = pos->x;
    y = pos->y;
    z = map_.heightAt(pos->x, pos->y) + eyeHeight(posture->stance) - rules_.shoulderBelowEye;
    return true;
}

// Fixes where the strike goes: the target's chest as it is now, moved by a small wobble of the hand
// (MeleeRules aimWobbleBySkill): no hit-chance dice.
// The player's swing goes straight at the target, or straight ahead at chest height.
void MeleeSystem::commit(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    m.committed = true;
    if (m.player && m.target == kNoEntity) {
        // Swung at nobody (no lock in reach when the key went down): whoever walked into it since.
        const Entity late = playerTarget(pawn);
        if (late != kNoEntity && inPlayerReach(pawn, late)) {
            m.target = late;
        } else if (const Entity corpse = corpseAhead(pawn); corpse != kNoEntity) {
            m.target = corpse;  // a body lying before it takes the blow (sneed, 2026-11-22)
        }
    }
    float ox = 0.0f;
    float oy = 0.0f;
    float oz = 0.0f;
    shoulder(pawn, ox, oy, oz);
    // Locked on or not, a swing at someone goes at its focus point (lock-on.md); one at a body on the ground at its aim
    // point there.
    const bool hasTarget = m.target != kNoEntity && reg.valid(m.target) && things_.spawned(m.target) &&
                           (m.low || meleeDeadBody(reg, m.target) ? combat_.aimPoint(m.target, m.aimX, m.aimY, m.aimZ)
                                  : focusPoint(pawn, m.target, !m.weaponTool, m.aimX, m.aimY, m.aimZ));
    if (!hasTarget) {
        const Posture& posture = reg.get<Posture>(pawn);
        const float r = reach(pawn);
        m.aimX = ox + posture.facingX * r;
        m.aimY = oy + posture.facingY * r;
        m.aimZ = oz - 0.2f;
        return;
    }
    face(pawn, m.aimX, m.aimY);
    if (m.player) {
        return;
    }
    float dx = m.aimX - ox;
    float dy = m.aimY - oy;
    float dz = 0.0f;  // the strike is level (see strike)
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.01f) {
        return;
    }
    dx /= len;
    dy /= len;
    dz /= len;
    // Two directions across the strike: sideways (level) and up.
    float rx = dy;
    float ry = -dx;
    const float rn = std::sqrt(rx * rx + ry * ry);
    rx = rn > 1e-6f ? rx / rn : 0.0f;
    ry = rn > 1e-6f ? ry / rn : -1.0f;
    const float ux = -dz * ry;
    const float uy = dz * rx;
    const float uz = rx * dy - ry * dx;
    Rng rng(splitMix64(m.seed ^ 0x5701CEULL));
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
    const float radius = aimWobble(pawn);
    m.aimX += (rx * u + ux * w) * radius;
    m.aimY += (ry * u + uy * w) * radius;
    m.aimZ += uz * w * radius;
}

// sneed 2026-11-19 ("when I'm swinging at someone it doesn't always hit, and they don't dodge"): the line fixed at
// commit lets a dodge read in time get away, but a target that only shuffled (footwork, a walk) was missed too. A target
// that didn't dodge this blow is followed to it, if it moved no more than strikeTrackDegrees off the line; the blow is
// still only as long as the reach, so one that stepped out of reach is missed.
void MeleeSystem::followToBlow(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    if (m.target == kNoEntity || !reg.valid(m.target) || !things_.spawned(m.target) || !reg.all_of<Position>(pawn)) {
        return;
    }
    const Melee* tm = reg.try_get<Melee>(m.target);
    if (tm != nullptr && tm->dodged && tm->threat == pawn) {
        return;
    }
    const Position& pos = reg.get<Position>(pawn);
    const float oldX = m.aimX;
    const float oldY = m.aimY;
    const float oldZ = m.aimZ;
    Posture& posture = reg.get<Posture>(pawn);
    const float faceX = posture.facingX;
    const float faceY = posture.facingY;
    commit(pawn, m);  // the same choice (seeded), at where the target is now
    const float ax = oldX - pos.x;
    const float ay = oldY - pos.y;
    const float bx = m.aimX - pos.x;
    const float by = m.aimY - pos.y;
    const float la = std::sqrt(ax * ax + ay * ay);
    const float lb = std::sqrt(bx * bx + by * by);
    const float most = std::cos(rules_.strikeTrackDegrees * 3.14159265f / 180.0f);
    if (la > 0.01f && lb > 0.01f && (ax * bx + ay * by) / (la * lb) < most) {
        m.aimX = oldX;  // it got off the line: the blow goes where it was going
        m.aimY = oldY;
        m.aimZ = oldZ;
        posture.facingX = faceX;
        posture.facingY = faceY;
    }
}

bool MeleeSystem::inPlayerReach(Entity pawn, Entity target) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.valid(target) || !reg.all_of<Position>(pawn) || !reg.all_of<Position>(target)) {
        return false;
    }
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(target);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float r = engageDistance(pawn, target) + rules_.reachSlack + rules_.playerReachSlack;
    return a.level == b.level && dx * dx + dy * dy <= r * r;
}

// The hit moment: a level strike from the attacker's centre through the aimed point (as a bullet
// would cross a man, so the hit chance table holds), as far as the reach allows, against hitboxes
// and walls. What it meets first is hit.
namespace {

// What a weapon, shield, armour piece or building is made of, for the sound of a blow on it.
DefId meleeSoundStuffOf(const entt::registry& reg, Entity thing) {
    if (thing == kNoEntity || !reg.valid(thing)) return DefId{};
    if (const MadeOf* made = reg.try_get<MadeOf>(thing)) return made->stuff;
    if (const BuiltOf* built = reg.try_get<BuiltOf>(thing)) return built->stuff;
    return DefId{};
}

void meleeSoundStruck(const entt::registry& reg, Entity thing, Event& e) {
    if (const Thing* t = reg.try_get<Thing>(thing)) e.struckDef = t->def;
    e.struckStuff = meleeSoundStuffOf(reg, thing);
}

}  // namespace

void MeleeSystem::strike(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    if (!m.committed) {
        commit(pawn, m);
    } else {
        followToBlow(pawn, m);
    }
    const Tool* tool = toolOf(pawn, m);
    if (tool == nullptr || !reg.all_of<Position>(pawn)) {
        return;  // the weapon is gone
    }
    // A weapon's quality and stuff scale the blow (damage and armor penetration); fists and claws never.
    float blow = 1.0f;
    if (m.weaponTool) {
        const Gear* held = reg.try_get<Gear>(pawn);
        blow = held != nullptr ? combat_.meleeDamageFactor(held->weapon, tool->damageType) : 1.0f;
    }
    blow *= swingDamage(pawn) * combat_.traitMeleeDamage(pawn);  // melee skill (real-time-combat.md) and traits (Wimp)
    if (m.kind != SoulsAttack::None) {
        blow *= souls_.kinds[static_cast<size_t>(m.kind)].damage;  // a heavy, charged or jumping blow (souls-combat.md)
    }
    const float power = tool->power * blow;
    const float penetration = tool->penetration * blow;
    const Position& pos = reg.get<Position>(pawn);
    const float ox = pos.x;
    const float oy = pos.y;
    const float oz = std::max(m.aimZ, map_.heightAt(pos.x, pos.y) + 0.1f);
    float dx = m.aimX - ox;
    float dy = m.aimY - oy;
    const float flat = std::sqrt(dx * dx + dy * dy);
    if (flat < 0.01f) {
        dx = reg.get<Posture>(pawn).facingX;
        dy = reg.get<Posture>(pawn).facingY;
    } else {
        dx /= flat;
        dy /= flat;
    }
    // As far as the aimed point and a little past it, but never further than the reach. The player's swing doesn't
    // step the body in (its feet are the player's), so its blow reaches as far as the clip's step would have taken it.
    float strikeReach = reach(pawn);
    if (meleeFreeFeet(m)) {
        bool weaponHeld = false;
        if (const ToolSet* set = toolsOf(pawn, weaponHeld, !m.weaponTool)) {
            strikeReach += set->lunge * combat_.motion().sizeScale(pawn);
        }
    }
    const float length = std::min(flat + rules_.strikeDepth, strikeReach);
    const float ez = oz;
    // One line of the blow at `angle` radians off the aimed line: walls and hitboxes first, then a target without
    // hitboxes (animals until they get theirs), hit if the line passes within its body, which is then hurt where
    // health picks.
    float ex = ox + dx * length;
    float ey = oy + dy * length;
    const auto line = [&](float angle) {
        const float c = std::cos(angle);
        const float sn = std::sin(angle);
        const float lx = dx * c - dy * sn;
        const float ly = dx * sn + dy * c;
        const float lex = ox + lx * length;
        const float ley = oy + ly * length;
        TraceHit found = combat_.trace(ox, oy, oz, lex, ley, ez, pos.level, pawn, cellOf(pos), true, nullptr, true);
        if (found.kind != ProjectileHitKind::Pawn && found.kind != ProjectileHitKind::Building && found.kind != ProjectileHitKind::Plant &&
            m.target != kNoEntity &&
            reg.valid(m.target) && things_.spawned(m.target)) {
            std::vector<HitboxView> boxes;
            if (!combat_.hitboxesOf(m.target, boxes)) {
                const Position& t = reg.get<Position>(m.target);
                const float along = std::clamp((t.x - ox) * lx + (t.y - oy) * ly, 0.0f, length);
                const float cx = ox + lx * along - t.x;
                const float cy = oy + ly * along - t.y;
                const float body = std::max(kPawnRadius, combat_.motion().bodyRadius(m.target));
                if (cx * cx + cy * cy <= body * body && t.level == pos.level) {
                    found.kind = ProjectileHitKind::Pawn;
                    found.thing = m.target;
                    found.part = -1;
                    found.x = t.x;
                    found.y = t.y;
                    found.z = oz;
                }
            }
        }
        return found;
    };
    TraceHit hit = line(0.0f);
    // sneed's arc (part C): a cut or a blunt blow sweeps through its arc, so one that misses on the aimed line still
    // meets whatever stands in the swing, from the weapon side across (but not a pawn that sidestepped this blow in
    // time). A stab or a bite only goes straight.
    if (hit.kind == ProjectileHitKind::None && rules_.arcRaysPerSide > 0 && arcSwing(tool->damageType)) {
        const float half = rules_.arcHalfDegrees * 3.14159265f / 180.0f;
        for (int side : {1, -1}) {
            for (int i = 1; i <= rules_.arcRaysPerSide && hit.kind == ProjectileHitKind::None; ++i) {
                const float angle = static_cast<float>(side) * half * static_cast<float>(i) / static_cast<float>(rules_.arcRaysPerSide);
                TraceHit swept = line(angle);
                // A sidestep read in time is timed to the blade: the sweep doesn't catch the one who dodged it.
                const Melee* dodger = swept.kind == ProjectileHitKind::Pawn ? reg.try_get<Melee>(swept.thing) : nullptr;
                if (swept.kind == ProjectileHitKind::Pawn && !(dodger != nullptr && dodger->dodged && dodger->threat == pawn)) {
                    hit = swept;
                } else if (m.target == kNoEntity && (swept.kind == ProjectileHitKind::Building || swept.kind == ProjectileHitKind::Plant)) {
                    hit = swept;  // a swing at nobody (the player's at a wall or a tree) meets what stands in its arc
                }
            }
        }
    }

    // A critical attack always meets its held victim, through the middle of the body (souls-combat.md).
    if (soulsCriticalKind(m.kind) && hit.kind != ProjectileHitKind::Pawn && m.target != kNoEntity && reg.valid(m.target) &&
        things_.spawned(m.target) && reg.all_of<Position>(m.target)) {
        const Position& t = reg.get<Position>(m.target);
        const float gap = std::sqrt((t.x - ox) * (t.x - ox) + (t.y - oy) * (t.y - oy));
        if (t.level == pos.level && gap <= strikeReach + souls_.criticalRange) {
            hit.kind = ProjectileHitKind::Pawn;
            hit.thing = m.target;
            hit.part = combat_.hitboxPart(m.target, "torso");
            hit.x = t.x;
            hit.y = t.y;
            hit.z = oz;
        }
    }

    Event e;
    e.type = EventType::MeleeHit;
    e.thing = pawn;
    e.def = m.weaponTool ? reg.get<Thing>(reg.get<Gear>(pawn).weapon).def : reg.get<Thing>(pawn).def;
    e.target = hit.kind == ProjectileHitKind::Pawn ? hit.thing : m.target;
    e.hit = hit.kind;
    e.x = hit.kind == ProjectileHitKind::None ? ex : hit.x;
    e.y = hit.kind == ProjectileHitKind::None ? ey : hit.y;
    e.z = hit.kind == ProjectileHitKind::None ? ez : hit.z;
    e.cell = CellPos{static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y)), pos.level};
    e.message = "missed";
    if (m.weaponTool) {
        e.stuff = meleeSoundStuffOf(reg, reg.get<Gear>(pawn).weapon);
    }
    if (hit.kind == ProjectileHitKind::Building && reg.valid(hit.thing)) {
        meleeSoundStruck(reg, hit.thing, e);
    }
    if (hit.kind == ProjectileHitKind::Plant) {
        e.cell = hit.cell;
        e.struckDef = combat_.plants() != nullptr ? combat_.plants()->defAt(hit.cell) : DefId{};
    }
    // Anything that can be hurt takes the blow: buildings, walls, doors and trees (sneed 2026-11-20,
    // docs/design/structure-damage.md). The event is built first: the thing may be gone after it.
    if (hit.kind == ProjectileHitKind::Building || hit.kind == ProjectileHitKind::Plant) {
        const int taken = combat_.damageStruck(hit, power, true);
        if (taken > 0) {
            e.message = "hit";
            e.amount = static_cast<float>(taken);
            ++structureHits_;
        }
    }
    if (hit.kind == ProjectileHitKind::Pawn) {
        const Entity victim = hit.thing;
        Melee* vm = reg.try_get<Melee>(victim);
        const Position& vp = reg.get<Position>(victim);
        const Posture& vpost = reg.get<Posture>(victim);
        const bool facing = vpost.facingX * (pos.x - vp.x) + vpost.facingY * (pos.y - vp.y) > 0.0f;
        // Bare fists up against a weapon (sneed 2026-11-22): the blow is stopped on the forearm, which takes it.
        const Gear* victimGear = reg.try_get<Gear>(victim);
        const bool victimArmed = victimGear != nullptr && victimGear->weapon != kNoEntity && reg.valid(victimGear->weapon);
        if (vm != nullptr && blocking(victim) && facing && meleeAble(reg, victim) && !victimArmed && m.weaponTool) {
            const int arm = combat_.hitboxPart(victim, "left arm");
            if (arm >= 0) {
                hit.part = arm;
                ++armBlocks_;
            }
        }
        // Elden Ring style guard (souls-combat.md): a raised shield or weapon facing the blow takes it for stamina.
        if (souls_.enabled && soulsGuard(pawn, m, victim, power, tool->damageType, e)) {
            if (events_ != nullptr) {
                events_->push_back(std::move(e));
            }
            return;
        }
        if (!souls_.enabled && vm != nullptr && blocking(victim) && facing && meleeAble(reg, victim) && !(!victimArmed && m.weaponTool)) {
            // The weapon takes it, and the blocker staggers: no swing of its own for a moment.
            e.message = "blocked";
            ++blocks_;
            if (const Gear* parry = reg.try_get<Gear>(victim); parry != nullptr && parry->weapon != kNoEntity && reg.valid(parry->weapon)) {
                meleeSoundStruck(reg, parry->weapon, e);
            }
            // Guard: parries each within guardWindowSteps of the last wear it down; the last one breaks it (a long stagger, no
            // parrying), so the next blow gets through unless it sidesteps.
            if (now() - vm->guardFrom > rules_.guardWindowSteps) {
                vm->guardCount = 0;  // it got its breath back since the last parry
            }
            vm->guardFrom = now();
            ++vm->guardCount;
            int stagger = rules_.blockRecoverySteps;
            if (vm->guardCount >= rules_.guardBlocks) {
                vm->guardCount = 0;
                vm->guardBrokenUntil = now() + rules_.guardBreakSteps;
                stagger = rules_.guardBreakSteps;
                ++guardBreaks_;
                e.count = 1;  // the guard broke
            }
            if (vm->phase == MeleePhase::Ready) {
                vm->phase = MeleePhase::Recovery;
                vm->stepsLeft = stagger;
            }
        } else if (hit.part < 0) {
            Rng rng(splitMix64(m.seed ^ 0xB0D7ULL));
            health_.applyDamage(victim, tool->damageType, power, -1, rng);
            e.message = "hit";
            e.amount = power;
            ++hits_;
            combat_.noteHurt(victim, pawn);
            if (souls_.enabled) {
                soulsPoise(pawn, m, victim, tool->power, HitFrom{true, pos.x, pos.y, true});
            }
        } else {
            const PawnHitResult result =
                combat_.hitPawn(victim, hit.part, tool->damageType, power, penetration, splitMix64(m.seed ^ 0x417ULL),
                                HitFrom{true, pos.x, pos.y});
            e.part = result.part;
            if (result.deflected) {
                e.hit = ProjectileHitKind::Armor;
                e.message = result.shieldBlocked ? "blocked" : "deflected";  // a raised shield stopped all of it
                const Entity struck = result.shieldBlocked ? result.shield : (result.wear.empty() ? kNoEntity : result.wear.front().first);
                if (struck != kNoEntity && reg.valid(struck)) {
                    meleeSoundStruck(reg, struck, e);
                }
            } else {
                e.message = "hit";
                e.amount = result.amount;
                ++hits_;
            }
            if (events_ != nullptr) {
                events_->push_back(e);
            }
            // A corpse takes the blow (it piles up until the piece comes off) but staggers no one and angers no one.
            const bool corpse = health_.tracks(victim) && reg.get<Health>(victim).dead;
            if (!result.deflected) {
                health_.applyHit(victim, result.damageType, result.amount, result.part);
            }
            combat_.wearApparel(victim, result);
            if (!result.deflected && !corpse) {
                if (souls_.enabled) {
                    soulsPoise(pawn, m, victim, tool->power, HitFrom{true, pos.x, pos.y, true});  // Elden Ring poise (souls-combat.md)
                } else {
                    combat_.jolt(victim, result.part, tool->damageType, power, result.amount, HitFrom{true, pos.x, pos.y, true});  // poise and stagger (part C)
                }
                combat_.noteHurt(victim, pawn);
            }
            return;
        }
    } else if (m.target != kNoEntity && reg.valid(m.target)) {
        const Melee* tm = reg.try_get<Melee>(m.target);
        if (tm != nullptr && tm->dodged && tm->threat == pawn) {
            e.message = "dodged";
        }
    }
    if (events_ != nullptr) {
        events_->push_back(std::move(e));
    }
}

bool MeleeSystem::arcSwing(DefId damageType) const {
    return damageType.valid() && damageType.value < arcTypes_.size() && arcTypes_[damageType.value] != 0;
}

void MeleeSystem::step() {
    entt::registry& reg = things_.registry();
    soulsStep();
    if (reg.view<Melee>().empty()) {
        return;
    }
    order_.clear();
    for (const Entity pawn : reg.view<Melee>()) {
        order_.push_back(pawn);
    }
    std::sort(order_.begin(), order_.end());
    // Defenders first: a sidestep or a raised weapon that is due happens before this step's strikes.
    for (const Entity pawn : order_) {
        Melee& d = reg.get<Melee>(pawn);
        if (d.defense != MeleeDefense::None && now() < d.stunUntil) {
            d.defense = MeleeDefense::None;  // reeling: it can't defend (souls-combat.md)
        } else if (d.defense != MeleeDefense::None && now() >= d.reactAt) {
            defend(pawn, d);
        }
    }
    for (const Entity pawn : order_) {
        Melee& m = reg.get<Melee>(pawn);
        // A finisher whose body is gone (taken off the map) ends there: nothing is left to strike or kneel over.
        if (m.finisher != 0 && m.phase != MeleePhase::Ready &&
            (m.target == kNoEntity || !reg.valid(m.target) || !things_.spawned(m.target))) {
            m.phase = MeleePhase::Ready;
            m.stepsLeft = 0;
            m.finisher = 0;
            continue;
        }
        switch (m.phase) {
            case MeleePhase::Ready:
                break;
            case MeleePhase::Recovery:
                if (m.kind != SoulsAttack::None) {
                    soulsStepRecovery(pawn, m);  // the combo's next blow (it may start a new wind-up)
                    if (m.phase != MeleePhase::Recovery) {
                        break;
                    }
                }
                if (--m.stepsLeft <= 0) {
                    m.phase = MeleePhase::Ready;
                    m.stepsLeft = 0;
                    m.endedAt = now();
                }
                break;
            case MeleePhase::Windup:
                if (!things_.spawned(pawn) || !meleeAble(reg, pawn)) {
                    m.phase = MeleePhase::Ready;  // the swing is off
                    m.stepsLeft = 0;
                    break;
                }
                if (!m.committed && m.target != kNoEntity && reg.valid(m.target) && things_.spawned(m.target)) {
                    const Position& to = reg.get<Position>(m.target);
                    face(pawn, to.x, to.y);  // follows the target until the line is fixed
                }
                if (m.kind != SoulsAttack::None && soulsStepWindup(pawn, m)) {
                    break;  // a heavy held drawn back (charging)
                }
                --m.stepsLeft;
                if (!m.committed && m.stepsLeft <= rules_.commitSteps) {
                    commit(pawn, m);
                }
                if (m.stepsLeft <= 0) {
                    strike(pawn, m);
                    m.noDefendUntil = std::max(m.noDefendUntil, now() + rules_.recoveryNoDefendSteps);
                    const Tool* tool = toolOf(pawn, m);
                    if (m.phase != MeleePhase::Windup) {
                        break;  // the strike ended it (it struck a guard that broke it, or the like)
                    }
                    m.phase = MeleePhase::Recovery;
                    if (m.kind != SoulsAttack::None) {
                        m.stepsLeft = std::max(1, m.recoverySteps);
                        break;
                    }
                    m.stepsLeft = m.finisher != 0 ? std::max(1, m.finisherSteps - static_cast<int>(now() - m.finisherFrom))
                                  : tool != nullptr
                                      ? std::max(1, static_cast<int>(std::lround(
                                                        static_cast<float>(tool->cooldown - meleeWindup(tool->cooldown, tool->minWindup, tool->windupShare)) *
                                                        swingTimeFactor(pawn) * (m.low ? rules_.lowTimeFactor : 1.0f)))) + tool->gapSteps
                                      : 1;
                }
                break;
        }
    }
}

void MeleeSystem::hashThing(Entity e, StateHasher& h) const {
    if (const Stamina* st = things_.registry().try_get<Stamina>(e)) {
        h.add(uint64_t{0x57A41});
        h.add(st->value);
        h.add(st->lastUse);
    }
    if (const PoiseMeter* p = things_.registry().try_get<PoiseMeter>(e)) {
        h.add(uint64_t{0x9015E});
        h.add(p->value);
        h.add(p->lastHit);
        h.add(p->lastBreak);
        h.add(p->knockdownImmuneUntil);
    }
    const Melee* m = things_.registry().try_get<Melee>(e);
    if (m == nullptr) {
        return;
    }
    h.add(uint64_t{0x3E1EE});
    h.add(static_cast<int>(m->phase));
    h.add(m->stepsLeft);
    h.add(entt::to_integral(m->target));
    h.add(static_cast<int>(m->tool));
    h.add(m->weaponTool);
    h.add(m->player);
    h.add(m->committed);
    h.add(m->aimX);
    h.add(m->aimY);
    h.add(m->aimZ);
    h.add(m->seed);
    h.add(entt::to_integral(m->threat));
    h.add(static_cast<int>(m->defense));
    h.add(m->reactAt);
    h.add(m->blockFrom);
    h.add(m->blockUntil);
    h.add(m->dodged);
    h.add(static_cast<int>(m->guardCount));
    h.add(m->guardFrom);
    h.add(m->guardBrokenUntil);
    h.add(m->noDefendUntil);
    h.add(m->guardWaitFrom);
    h.add(m->nextRollAt);
    h.add(entt::to_integral(m->focus));
    h.add(m->focusPlayer);
    h.add(m->focusSeenAt);
    h.add(static_cast<uint64_t>(m->focusTried.size()));
    for (const Entity tried : m->focusTried) {
        h.add(entt::to_integral(tried));
    }
    h.add(m->focusTriedAt);
    h.add(m->low);
    h.add(static_cast<int>(m->circleDir));
    h.add(m->nextFootworkAt);
    h.add(m->footworkUntil);
    h.add(m->engagedAt);
    h.add(static_cast<int>(m->response));
    h.add(static_cast<int>(m->finisher));
    h.add(m->finisherFrom);
    h.add(m->finisherSteps);
    h.add(static_cast<int>(m->kind));
    h.add(static_cast<int>(m->combo));
    h.add(m->windupSteps);
    h.add(m->recoverySteps);
    h.add(m->endedAt);
    h.add(static_cast<int>(m->buffered));
    h.add(entt::to_integral(m->bufferedTarget));
    h.add(m->bufferedHold);
    h.add(m->holding);
    h.add(m->chargeSteps);
    h.add(m->chargeRate);
    h.add(m->aiChargeFor);
    h.add(static_cast<int>(m->aiChain));
    h.add(m->stunUntil);
    h.add(m->counterUntil);
    h.add(m->guardStun);
    h.add(m->rollEndsAt);
}

// ---- Fight response, focus and footwork (docs/design/focus.md) ----------------------------------

const char* fightResponseName(FightResponse response) {
    switch (response) {
        case FightResponse::Attack: return "Attack";
        case FightResponse::Fists: return "Fists";
        case FightResponse::Flee: return "Flee";
        case FightResponse::FleeOnSight: return "FleeOnSight";
    }
    return "Attack";
}

bool fightResponseFromName(const std::string& name, FightResponse& out) {
    for (FightResponse r : {FightResponse::Attack, FightResponse::Fists, FightResponse::Flee, FightResponse::FleeOnSight}) {
        if (name == fightResponseName(r)) {
            out = r;
            return true;
        }
    }
    return false;
}

bool MeleeSystem::setResponse(Entity pawn, FightResponse response, std::string* why) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Posture, Mover>(pawn)) {
        if (why != nullptr) {
            *why = "that is not a pawn";
        }
        return false;
    }
    if (response > FightResponse::FleeOnSight) {
        if (why != nullptr) {
            *why = "no such fight response";
        }
        return false;
    }
    reg.get_or_emplace<Melee>(pawn).response = response;
    return true;
}

FightResponse MeleeSystem::responseOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return m != nullptr ? m->response : FightResponse::Attack;
}

Entity MeleeSystem::focusOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return m != nullptr ? m->focus : kNoEntity;
}

namespace {

// A pawn one can lock on to: spawned, alive, on the pawn's level, within `range`.
bool meleeFocusable(const Things& things, Entity pawn, Entity target, float range) {
    const entt::registry& reg = things.registry();
    if (target == pawn || !reg.valid(target) || !things.spawned(target) || !reg.all_of<Thing, Position, Posture>(target) ||
        reg.get<Thing>(target).category != ThingCategory::Pawn) {
        return false;
    }
    const Health* h = reg.try_get<Health>(target);
    if (h != nullptr && h->dead) {
        return false;
    }
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(target);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return a.level == b.level && dx * dx + dy * dy <= range * range;
}

}  // namespace

bool MeleeSystem::focusCommand(Entity pawn, Entity target, FocusMode mode, std::string* why, float viewX, float viewY,
                               bool anySide) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!reg.valid(pawn) || !things_.spawned(pawn) || !reg.all_of<Posture, Mover, Position>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    if (mode == FocusMode::Clear) {
        Melee* m = reg.try_get<Melee>(pawn);
        if (m == nullptr || m->focus == kNoEntity) {
            return fail("not focused on anyone");
        }
        m->focus = kNoEntity;
        m->focusPlayer = false;
        return true;
    }
    if (mode == FocusMode::Set) {
        if (!meleeFocusable(things_, pawn, target, rules_.focusBreakRange)) {
            return fail("that can't be focused on (not a living pawn near enough)");
        }
        Melee& m = reg.get_or_emplace<Melee>(pawn);
        m.focus = target;
        m.focusPlayer = true;
        m.focusSeenAt = now();
        return true;
    }
    if (mode != FocusMode::Next && mode != FocusMode::Left && mode != FocusMode::Right) {
        return fail("no such focus mode");
    }
    return focusNext(pawn, mode, viewX, viewY, anySide, why);
}

// Tab and flicks (sneed 2026-11-19: "tab doesn't shuffle or work intuitively to target closest"). The old Tab took the
// next one after the held target in a list sorted by distance each press; walking to the second nearest made it the
// nearest, so Tab swapped between the two nearest for ever. Now a round: each Tab gives the nearest not yet given.
bool MeleeSystem::focusNext(Entity pawn, FocusMode mode, float viewX, float viewY, bool anySide, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    const Position& pos = reg.get<Position>(pawn);
    const Posture& posture = reg.get<Posture>(pawn);
    float lookX = viewX;
    float lookY = viewY;
    if (const float l = std::sqrt(lookX * lookX + lookY * lookY); l > 1e-4f) {
        lookX /= l;
        lookY /= l;
    } else {
        lookX = posture.facingX;
        lookY = posture.facingY;
    }
    const FactionId own = reg.all_of<Faction>(pawn) ? reg.get<Faction>(pawn).id : kPlayerFaction;
    struct Pick {
        int group = 0;  // 0 standing enemies, 1 downed enemies, 2 standing others, 3 downed others
        float score = 0.0f;
        float angle = 0.0f;  // radians anticlockwise from the reference direction (flicks)
        Entity pawn = kNoEntity;
    };
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    const bool holding = m.focus != kNoEntity && reg.valid(m.focus) && reg.all_of<Position>(m.focus);
    // A flick turns from the target held (else from the view).
    float refX = lookX;
    float refY = lookY;
    if (holding) {
        const Position& t = reg.get<Position>(m.focus);
        const float l = std::sqrt((t.x - pos.x) * (t.x - pos.x) + (t.y - pos.y) * (t.y - pos.y));
        if (l > 0.01f) {
            refX = (t.x - pos.x) / l;
            refY = (t.y - pos.y) / l;
        }
    }
    std::vector<Pick> picks;
    for (const auto [e, thing, p] : reg.view<const Thing, const Position>().each()) {
        if (!meleeFocusable(things_, pawn, e, rules_.focusRange)) {
            continue;
        }
        const bool enemy = hostile_ ? hostile_(pawn, e) : true;
        const FactionId side = reg.all_of<Faction>(e) ? reg.get<Faction>(e).id : kPlayerFaction;
        if (!anySide && !enemy && side == own && !reg.all_of<Animal>(e)) {
            continue;  // its own side
        }
        const bool down = health_.incapacitated(e);
        const float dx = p.x - pos.x;
        const float dy = p.y - pos.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        const bool front = d < 0.01f || (dx * lookX + dy * lookY) / d >= rules_.focusFrontCos;
        const float angle = d < 0.01f ? 0.0f : std::atan2(refX * dy - refY * dx, refX * dx + refY * dy);
        picks.push_back(Pick{(enemy ? 0 : 2) + (down ? 1 : 0), front ? d : d * rules_.focusBehindFactor, angle, e});
    }
    if (picks.empty()) {
        return fail("nobody near enough to focus on");
    }
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
        if (a.group != b.group) {
            return a.group < b.group;
        }
        if (a.score != b.score) {
            return a.score < b.score;
        }
        return a.pawn < b.pawn;
    });
    if (picks.front().group <= 1 && !anySide) {
        // Enemies about: nobody else (the practice range keeps everyone: its targets turn on you one by one).
        picks.erase(std::remove_if(picks.begin(), picks.end(), [](const Pick& p) { return p.group > 1; }), picks.end());
    }
    Entity chosen = kNoEntity;
    if (mode == FocusMode::Left || mode == FocusMode::Right) {
        // The nearest turn that way from the held target (standing ones first), within flickMaxDegrees. Right turns from
        // +x towards +y: on screen (Unreal's +Y is to the right of +X) the player's right.
        const float sign = mode == FocusMode::Right ? 1.0f : -1.0f;
        const float most = rules_.flickMaxDegrees * 3.14159265f / 180.0f;
        int bestGroup = 99;
        float best = 0.0f;
        for (const Pick& p : picks) {
            const float turn = p.angle * sign;
            if (p.pawn == m.focus || turn <= 0.01f || turn > most) {
                continue;
            }
            const int g = p.group % 2;
            if (chosen == kNoEntity || g < bestGroup || (g == bestGroup && turn < best)) {
                chosen = p.pawn;
                best = turn;
                bestGroup = g;
            }
        }
        if (chosen == kNoEntity) {
            // Nobody that way (PC gun range 2026-11-20: one other target left, on the other side, and the flick did nothing):
            // the flick still switches, to the other target nearest in turn, standing ones first.
            for (const Pick& p : picks) {
                const float turn = std::fabs(p.angle);
                const int g = p.group % 2;
                if (p.pawn != m.focus && (chosen == kNoEntity || g < bestGroup || (g == bestGroup && turn < best))) {
                    chosen = p.pawn;
                    best = turn;
                    bestGroup = g;
                }
            }
        }
        if (chosen == kNoEntity) {
            return fail("nobody else to focus on");
        }
    } else {
        // Tab: a round starts again after focusRoundSteps without a Tab; the target held counts as given.
        if (now() - m.focusTriedAt > rules_.focusRoundSteps) {
            m.focusTried.clear();
        }
        const auto tried = [&m](Entity e) { return std::find(m.focusTried.begin(), m.focusTried.end(), e) != m.focusTried.end(); };
        if (holding && !tried(m.focus)) {
            m.focusTried.push_back(m.focus);
        }
        for (const Pick& p : picks) {
            if (!tried(p.pawn)) {
                chosen = p.pawn;
                break;
            }
        }
        if (chosen == kNoEntity) {
            // Everyone given: round again from the nearest, not the one held.
            m.focusTried.clear();
            for (const Pick& p : picks) {
                if (p.pawn != m.focus) {
                    chosen = p.pawn;
                    break;
                }
            }
            if (chosen == kNoEntity) {
                chosen = picks.front().pawn;
            }
            if (holding) {
                m.focusTried.push_back(m.focus);
            }
        }
        m.focusTried.push_back(chosen);
        m.focusTriedAt = now();
    }
    m.focus = chosen;
    m.focusPlayer = true;
    m.focusSeenAt = now();
    return true;
}

void MeleeSystem::setAiFocus(Entity pawn, Entity target) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn)) {
        return;
    }
    Melee* m = reg.try_get<Melee>(pawn);
    if (m != nullptr && m->focusPlayer) {
        return;
    }
    if (target == kNoEntity) {
        if (m != nullptr) {
            m->focus = kNoEntity;
        }
        return;
    }
    if (m == nullptr) {
        m = &reg.emplace<Melee>(pawn);
    }
    m->focus = target;
}

void MeleeSystem::faceFocus() {
    entt::registry& reg = things_.registry();
    for (const auto [pawn, m] : reg.view<Melee>().each()) {
        if (m.focus == kNoEntity) {
            if (m.threat != kNoEntity && combat_.rollLocked(pawn)) {
                steerRoll(pawn, m.threat);  // an AI's dodge keeps its eyes on whoever it dodged
            }
            continue;
        }
        const bool keep = things_.spawned(pawn) && reg.all_of<Position, Posture>(pawn) && !health_.incapacitated(pawn) &&
                          meleeFocusable(things_, pawn, m.focus, m.focusPlayer ? rules_.focusBreakRange : rules_.aiFocusRange + 2.0f);
        if (!keep) {
            m.focus = kNoEntity;
            m.focusPlayer = false;
            continue;
        }
        const Position& t = reg.get<Position>(m.focus);
        // The player's lock lets go when it hasn't seen the target for lockSightLostSteps (lock-on.md). Looked at
        // every tenth step, from its eyes to the target's.
        if (m.focusPlayer && (now() + entt::to_integral(pawn)) % 10 == 0) {
            const Position& p = reg.get<Position>(pawn);
            const Viewpoint from{p.x, p.y, p.level, reg.get<Posture>(pawn).stance};
            const Viewpoint to{t.x, t.y, t.level, reg.get<Posture>(m.focus).stance};
            if (map_.cover().lineOfSight(from, to)) {
                m.focusSeenAt = now();
            } else if (now() - m.focusSeenAt >= rules_.lockSightLostSteps) {
                m.focus = kNoEntity;
                m.focusPlayer = false;
                continue;
            }
        }
        if (combat_.rollLocked(pawn)) {
            steerRoll(pawn, m.focus);  // a roll goes its own way, the body turning round to the focus as it goes
            continue;
        }
        const Shooter* s = reg.try_get<Shooter>(pawn);
        if (s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
            continue;  // it faces where it aims
        }
        // A gun in hand keeps the lock but not the facing: the aim is free (sneed 2026-11-18, lock-on.md).
        if (const Gear* gear = reg.try_get<Gear>(pawn);
            gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon) && combat_.isGun(reg.get<Thing>(gear->weapon).def)) {
            continue;
        }
        face(pawn, t.x, t.y);
    }
}

// Where a swing goes (docs/design/lock-on.md): sneed's focus point. A big body (lockBigBodySize and up) is struck at the
// centre of its body part nearest the attacker's shoulder, picked fresh at each swing; fists go at a person's head; anything
// else at the chest (aimPoint). Only where it is aimed: the strike's swept line decides what it meets.
bool MeleeSystem::focusPoint(Entity attacker, Entity target, bool fists, float& x, float& y, float& z) const {
    if (!combat_.aimPoint(target, x, y, z)) {
        return false;
    }
    const bool big = combat_.bodySizeOf(target) >= rules_.lockBigBodySize;
    if (!big && !fists) {
        return true;
    }
    std::vector<HitboxView> boxes;
    if (!combat_.hitboxesOf(target, boxes) || boxes.empty()) {
        return true;
    }
    float ox = 0.0f;
    float oy = 0.0f;
    float oz = 0.0f;
    if (!shoulder(attacker, ox, oy, oz)) {
        return true;
    }
    int head = -1;
    if (!big) {
        const std::vector<std::string> zones = health_.zoneNames(target);
        for (size_t i = 0; i < zones.size(); ++i) {
            if (zones[i] == "Head") {
                head = static_cast<int>(i);
                break;
            }
        }
        if (head < 0) {
            return true;  // no head to punch at: the chest
        }
    }
    bool found = false;
    float best = 0.0f;
    for (const HitboxView& b : boxes) {
        const float cx = b.box ? b.ax : 0.5f * (b.ax + b.bx);
        const float cy = b.box ? b.ay : 0.5f * (b.ay + b.by);
        const float cz = b.box ? b.az : 0.5f * (b.az + b.bz);
        float score = 0.0f;
        if (big) {
            score = (cx - ox) * (cx - ox) + (cy - oy) * (cy - oy) + (cz - oz) * (cz - oz);  // nearest
        } else if (health_.partZone(target, b.part) == head) {
            score = -cz;  // the head's highest shape (the skull over the neck)
        } else {
            continue;
        }
        if (!found || score < best) {
            found = true;
            best = score;
            x = cx;
            y = cy;
            z = cz;
        }
    }
    return true;
}

// Elden Ring circling (lock-on.md): locked on with no gun, A and D walk round the target. The camera faces it, so a
// sideways key press is a step along the tangent, which would drift outwards; the step is put back on the circle.
void MeleeSystem::orbitMove(Entity pawn, float& dx, float& dy) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    if (m == nullptr || m->focus == kNoEntity || !reg.valid(m->focus) || !reg.all_of<Position>(m->focus) || !reg.all_of<Position>(pawn)) {
        return;
    }
    if (const Gear* gear = reg.try_get<Gear>(pawn);
        gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon) && combat_.isGun(reg.get<Thing>(gear->weapon).def)) {
        return;
    }
    const Position& p = reg.get<Position>(pawn);
    const Position& t = reg.get<Position>(m->focus);
    const float rx = p.x - t.x;
    const float ry = p.y - t.y;
    const float r = std::sqrt(rx * rx + ry * ry);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (p.level != t.level || r < 0.5f || len < 1e-5f) {
        return;
    }
    const float radial = (dx * rx + dy * ry) / r;
    if (std::fabs(radial) > rules_.orbitRadialShare * len) {
        return;  // walking in or backing off
    }
    const float nx = rx + dx;
    const float ny = ry + dy;
    const float n = std::sqrt(nx * nx + ny * ny);
    if (n < 1e-5f) {
        return;
    }
    dx = t.x + nx * (r / n) - p.x;
    dy = t.y + ny * (r / n) - p.y;
}

Entity MeleeSystem::playerTarget(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Position, Posture>(pawn)) {
        return kNoEntity;
    }
    const Melee* m = reg.try_get<Melee>(pawn);
    if (m != nullptr && m->focus != kNoEntity && meleeFocusable(things_, pawn, m->focus, rules_.focusBreakRange) &&
        inPlayerReach(pawn, m->focus)) {
        return m->focus;
    }
    // No focus in reach: a body on the ground before it (within 60 degrees), nearest first (swung straight ahead, a level
    // blow passes over a man lying at its feet); else anyone not on its side on their feet ahead (within assistCos),
    // nearest first (sneed 2026-11-19: swings that land; 2026-11-20: neutrals too).
    const Position& pos = reg.get<Position>(pawn);
    const Posture& posture = reg.get<Posture>(pawn);
    Entity best = kNoEntity;
    float bestD = 0.0f;
    bool bestDown = false;
    for (const auto [e, thing, p] : reg.view<const Thing, const Position>().each()) {
        if (thing.category != ThingCategory::Pawn || !meleeFocusable(things_, pawn, e, reach(pawn) + 3.0f) || !inPlayerReach(pawn, e)) {
            continue;
        }
        const bool down = health_.incapacitated(e);
        if (!down && !(notFriendly_ ? notFriendly_(pawn, e) : (hostile_ && hostile_(pawn, e)))) {
            continue;  // its own side (sneed 2026-11-20: anyone else, even a neutral, draws the swing)
        }
        const float dx = p.x - pos.x;
        const float dy = p.y - pos.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 0.01f && (dx * posture.facingX + dy * posture.facingY) / d < (down ? 0.5f : rules_.assistCos)) {
            continue;
        }
        if (down && !inReach(pawn, e)) {
            continue;
        }
        if (best == kNoEntity || (down && !bestDown) || (down == bestDown && d < bestD)) {
            best = e;
            bestD = d;
            bestDown = down;
        }
    }
    return best;
}

Entity MeleeSystem::corpseAhead(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Position, Posture>(pawn)) {
        return kNoEntity;
    }
    const Position& pos = reg.get<Position>(pawn);
    const Posture& posture = reg.get<Posture>(pawn);
    Entity best = kNoEntity;
    float bestD = 0.0f;
    for (const auto [e, thing, p, h] : reg.view<const Thing, const Position, const Health>().each()) {
        if (e == pawn || !h.dead || thing.category != ThingCategory::Pawn || p.level != pos.level || !things_.spawned(e) ||
            !health_.takesCorpseHits(e)) {
            continue;
        }
        const float dx = p.x - pos.x;
        const float dy = p.y - pos.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float r = engageDistance(pawn, e) + rules_.reachSlack + rules_.playerReachSlack + rules_.corpseReachExtra;
        if (d > r || (d > 0.01f && (dx * posture.facingX + dy * posture.facingY) / d < 0.5f)) {
            continue;
        }
        if (best == kNoEntity || d < bestD) {
            best = e;
            bestD = d;
        }
    }
    return best;
}

bool MeleeSystem::engagedRecently(Entity pawn) const {
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    return m != nullptr && m->engagedAt > 0 && static_cast<float>(now() - m->engagedAt) <= rules_.chaseSeconds * 30.0f;
}

bool MeleeSystem::footwork(Entity pawn, Entity target, bool reckless) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.valid(target) || !reg.all_of<Position, Posture>(pawn) || !reg.all_of<Position>(target)) {
        return false;
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    if (m.phase == MeleePhase::Windup || now() < m.footworkUntil || now() < m.nextFootworkAt || combat_.rollLocked(pawn) ||
        combat_.motion().moving(pawn) || !meleeAble(reg, pawn)) {
        return now() < m.footworkUntil;
    }
    m.nextFootworkAt = now() + rules_.footworkEverySteps;
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(target);
    float dx = a.x - b.x;  // from the target to the pawn
    float dy = a.y - b.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 0.01f) {
        dx = -reg.get<Posture>(pawn).facingX;
        dy = -reg.get<Posture>(pawn).facingY;
    } else {
        dx /= d;
        dy /= d;
    }
    Rng rng(splitMix64(seed_ ^ (static_cast<uint64_t>(now()) << 24) ^ (uint64_t{entt::to_integral(pawn)} * 0xF00DF00DULL)));
    const float engage = engageDistance(pawn, target);
    // How often it moves: more for a skilled or nimble fighter, less for one that presses or is hurt in the legs.
    float chance = meleeBySkill(rules_.footworkChance, meleeSkill(pawn));
    if (const Colonist* colonist = reg.try_get<Colonist>(pawn)) {
        for (const TraitRecord& trait : colonist->traits) {
            if (std::find(rules_.footworkTraits.begin(), rules_.footworkTraits.end(), trait.def) != rules_.footworkTraits.end()) {
                chance += rules_.footworkTraitBonus;
            }
        }
    }
    if (reckless) {
        chance *= rules_.recklessFootwork;
    }
    if (const Health* h = reg.try_get<Health>(pawn)) {
        chance *= std::clamp(h->movingFactor, 0.0f, 1.0f);
    }
    // A blow coming at it while it recovers from its own (past the punish window right after its swing, which stays open)
    // or with its guard broken: sometimes a step back out of reach. Not always: a fight that is all stepping back never ends.
    const Melee* tm = reg.try_get<Melee>(target);
    const bool open = (m.phase == MeleePhase::Recovery && now() >= m.noDefendUntil) || guardBroken(pawn);
    const bool incoming = tm != nullptr && tm->phase == MeleePhase::Windup && tm->target == pawn;
    const auto move = [&](float mx, float my, float metres, int steps) {
        if (!combat_.motion().push(pawn, mx, my, metres, steps)) {
            return false;
        }
        m.footworkUntil = now() + steps;
        ++footworks_;
        return true;
    };
    if (incoming && open) {
        if (rng.unit() < chance * rules_.backShare) {
            return move(dx, dy, rules_.backMetres, rules_.backSteps);
        }
        return false;
    }
    // A friend aiming or firing at the same enemy: hold still, so the shot it lined up stays clear of it.
    const FactionId own = reg.all_of<Faction>(pawn) ? reg.get<Faction>(pawn).id : kPlayerFaction;
    for (const auto [e, shooter] : reg.view<const Shooter>().each()) {
        if (e != pawn && shooter.target == target &&
            (shooter.phase == ShotPhase::Aiming || shooter.phase == ShotPhase::Firing) &&
            (reg.all_of<Faction>(e) ? reg.get<Faction>(e).id : kPlayerFaction) == own) {
            return false;
        }
    }
    if (d < engage * rules_.crowdedShare) {
        return move(dx, dy, std::min(rules_.backMetres, engage * rules_.crowdedShare - d + 0.1f), rules_.backSteps);
    }
    if (rng.unit() >= chance * rules_.circleShare) {
        return false;
    }
    if (rng.unit() < rules_.circleFlip) {
        m.circleDir = static_cast<int8_t>(-m.circleDir);
    }
    // Round the target at the same distance: along the circle, pulled back to the distance it fights at. Never into a
    // friend's line of fire at the target (a gunman behind it shooting past): the other way, or not at all.
    const float want = std::max(0.7f, engage - rules_.closeIn);
    const float pull = std::clamp((want - d) / std::max(0.1f, rules_.circleMetres), -0.5f, 0.5f);
    const auto inLine = [&](float px, float py) {
        for (const auto [e, gear, sp] : reg.view<const Gear, const Position>().each()) {
            if (e == pawn || e == target || gear.weapon == kNoEntity || !reg.valid(gear.weapon) || sp.level != a.level ||
                !combat_.isGun(reg.get<Thing>(gear.weapon).def) || !things_.spawned(e) ||
                (reg.all_of<Faction>(e) ? reg.get<Faction>(e).id : kPlayerFaction) != own) {
                continue;
            }
            const float sx = b.x - sp.x;
            const float sy = b.y - sp.y;
            const float len2 = sx * sx + sy * sy;
            if (len2 < 0.01f || len2 > rules_.lineOfFireRange * rules_.lineOfFireRange) {
                continue;
            }
            const float t = std::clamp(((px - sp.x) * sx + (py - sp.y) * sy) / len2, 0.0f, 1.0f);
            const float cx = sp.x + sx * t - px;
            const float cy = sp.y + sy * t - py;
            if (cx * cx + cy * cy < rules_.lineOfFireClearance * rules_.lineOfFireClearance) {
                return true;
            }
        }
        return false;
    };
    for (int tries = 0; tries < 2; ++tries) {
        const float side = static_cast<float>(m.circleDir);
        float tx = -dy * side + dx * pull;
        float ty = dx * side + dy * pull;
        const float tl = std::sqrt(tx * tx + ty * ty);
        if (tl > 1e-4f && !inLine(a.x + tx / tl * rules_.circleMetres, a.y + ty / tl * rules_.circleMetres)) {
            return move(tx, ty, rules_.circleMetres, rules_.circleSteps);
        }
        m.circleDir = static_cast<int8_t>(-m.circleDir);
    }
    return false;
}

}  // namespace colony

// Elden Ring style melee (sneed 2026-11-27, docs/design/souls-combat.md): stamina, poise, light and heavy attacks with
// combos, charged heavies, running, jumping, rolling and guard-counter attacks, a guard that eats stamina and breaks.
// MeleeSystem's own swings, strikes and defences stay in Melee.cpp; this file holds what SoulsRules adds on top of them.

#include "ColonySim/Melee.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/BodyMotion.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Patch.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr const char* kSoulsKindNames[kSoulsAttackKinds] = {"Light",     "Heavy", "Charged",      "Running", "RunningHeavy",
                                                            "Jump",      "JumpHeavy", "Roll", "GuardCounter", "Riposte",
                                                            "Backstab",  "GroundStab"};

// The pawn can act at all (Melee.cpp's meleeAble): not downed, stunned, patching or knocked down.
bool soulsAble(const entt::registry& reg, Entity pawn) {
    const Health* h = reg.try_get<Health>(pawn);
    return (h == nullptr || !h->incapacitated()) && !reg.any_of<Stunned, Patching, KnockedDown>(pawn);
}

// The heavy chain's kinds (a heavy press after one of these goes on with the heavy combo; a light press starts the
// light one over).
bool soulsHeavyChain(SoulsAttack kind) {
    return kind == SoulsAttack::Heavy || kind == SoulsAttack::Charged || kind == SoulsAttack::RunningHeavy ||
           kind == SoulsAttack::JumpHeavy || kind == SoulsAttack::GuardCounter;
}

size_t soulsIndex(SoulsAttack kind) { return std::min(static_cast<size_t>(kind), kSoulsAttackKinds - 1); }

// What a shield or a weapon is made of, for the sound of a blow on it (Melee.cpp's meleeSoundStruck).
void soulsSoundStruck(const entt::registry& reg, Entity thing, Event& e) {
    if (thing == kNoEntity || !reg.valid(thing)) {
        return;
    }
    if (const Thing* t = reg.try_get<Thing>(thing)) {
        e.struckDef = t->def;
    }
    if (const MadeOf* made = reg.try_get<MadeOf>(thing)) {
        e.struckStuff = made->stuff;
    } else if (const BuiltOf* built = reg.try_get<BuiltOf>(thing)) {
        e.struckStuff = built->stuff;
    }
}

}  // namespace

const char* soulsAttackName(SoulsAttack kind) {
    return kind == SoulsAttack::None || kind == SoulsAttack::Count ? "None" : kSoulsKindNames[static_cast<size_t>(kind)];
}

bool soulsAttackFromName(const std::string& name, SoulsAttack& out) {
    for (size_t i = 0; i < kSoulsAttackKinds; ++i) {
        if (name == kSoulsKindNames[i]) {
            out = static_cast<SoulsAttack>(i);
            return true;
        }
    }
    return false;
}

void soulsReadRules(const DefDatabase& defs, SoulsRules& r) {
    // Elden Ring's shape by default (stamina 10/20/30), overridden by the def.
    const float defaults[kSoulsAttackKinds][4] = {
        // stamina, damage, poise, speed
        {10.0f, 1.0f, 1.0f, 1.6f},   // Light
        {20.0f, 1.5f, 1.6f, 1.0f},   // Heavy
        {28.0f, 2.0f, 2.4f, 1.0f},   // Charged
        {12.0f, 1.1f, 1.0f, 1.3f},   // Running
        {22.0f, 1.6f, 1.8f, 1.1f},   // RunningHeavy
        {14.0f, 1.3f, 1.5f, 1.0f},   // Jump
        {24.0f, 1.9f, 2.2f, 0.9f},   // JumpHeavy
        {10.0f, 1.0f, 1.0f, 1.5f},   // Roll
        {20.0f, 1.8f, 2.4f, 1.1f},   // GuardCounter
        {10.0f, 3.2f, 0.0f, 1.0f},   // Riposte (Elden Ring: about 3x for a straight sword)
        {10.0f, 2.6f, 0.0f, 1.0f},   // Backstab (ripostes about 25% above backstabs)
        {10.0f, 1.8f, 0.0f, 1.0f},   // GroundStab
    };
    for (size_t i = 0; i < kSoulsAttackKinds; ++i) {
        r.kinds[i].stamina = defaults[i][0];
        r.kinds[i].damage = defaults[i][1];
        r.kinds[i].poise = defaults[i][2];
        r.kinds[i].speed = defaults[i][3];
    }
    const Def* def = defs.find("Rules", "SoulsRules");
    if (def == nullptr) {
        return;
    }
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(def->number(key, out)); };
    const auto whole = [&](const char* key, int& out) { out = static_cast<int>(def->integer(key, out)); };
    r.enabled = def->flag("enabled", r.enabled);
    num("staminaMax", r.staminaMax);
    num("regenSeconds", r.regenSeconds);
    whole("regenDelaySteps", r.regenDelaySteps);
    num("rollCost", r.rollCost);
    num("blockShare", r.blockShare);
    num("guardHoldDrainPerSecond", r.guardHoldDrainPerSecond);
    num("unarmedCost", r.unarmedCost);
    num("animalCost", r.animalCost);
    num("personPoise", r.personPoise);
    num("poisePerBodySize", r.poisePerBodySize);
    num("minPoise", r.minPoise);
    num("noKnockdownBodySize", r.noKnockdownBodySize);
    whole("poiseRegenSteps", r.poiseRegenSteps);
    whole("knockdownImmuneSteps", r.knockdownImmuneSteps);
    whole("doubleBreakSteps", r.doubleBreakSteps);
    whole("staggerSteps", r.staggerSteps);
    whole("postureBreakSteps", r.postureBreakSteps);
    whole("poiseImmuneSteps", r.poiseImmuneSteps);
    num("hyperArmorTaken", r.hyperArmorTaken);
    num("animalPoisePerPower", r.animalPoisePerPower);
    num("chainShare", r.chainShare);
    num("cancelShare", r.cancelShare);
    whole("comboResetSteps", r.comboResetSteps);
    whole("rollAttackSteps", r.rollAttackSteps);
    whole("openingSteps", r.openingSteps);
    whole("blockedDelaySteps", r.blockedDelaySteps);
    r.earlyPressBreaksChain = def->flag("earlyPressBreaksChain", r.earlyPressBreaksChain);
    num("shieldRoom", r.shieldRoom);
    num("shieldPushPerStep", r.shieldPushPerStep);
    num("chargeShare", r.chargeShare);
    whole("minChargeSteps", r.minChargeSteps);
    whole("maxChargeSteps", r.maxChargeSteps);
    num("shieldNegation", r.shieldNegation);
    num("weaponNegation", r.weaponNegation);
    num("guardCos", r.guardCos);
    whole("guardBreakSteps", r.guardBreakSteps);
    whole("blockStunSteps", r.blockStunSteps);
    whole("counterSteps", r.counterSteps);
    num("criticalRange", r.criticalRange);
    num("backstabRange", r.backstabRange);
    num("backstabCos", r.backstabCos);
    whole("criticalHoldSteps", r.criticalHoldSteps);
    whole("impaleHoldSteps", r.impaleHoldSteps);
    num("impaleStandOff", r.impaleStandOff);
    num("criticalKnockdown", r.criticalKnockdown);
    num("idealInset", r.idealInset);
    num("warpBack", r.warpBack);
    num("aiStaminaFloor", r.aiStaminaFloor);
    for (size_t i = 0; i < 2; ++i) {
        r.aiComboMax[i] = static_cast<int>(def->numberAt("aiComboMaxBySkill", i, r.aiComboMax[i]));
    }
    num("aiHeavyChance", r.aiHeavyChance);
    num("aiHeavyVsGuard", r.aiHeavyVsGuard);
    num("aiChargeChance", r.aiChargeChance);
    num("aiLeapMin", r.aiLeapMin);
    num("aiLeapMax", r.aiLeapMax);
    num("aiLeapChance", r.aiLeapChance);
    num("aiGuardChance", r.aiGuardChance);
    num("aiCounterChance", r.aiCounterChance);
    r.staminaMax = std::max(1.0f, r.staminaMax);
    r.regenSeconds = std::max(0.05f, r.regenSeconds);
    r.chainShare = std::clamp(r.chainShare, 0.0f, 1.0f);
    r.cancelShare = std::clamp(r.cancelShare, 0.0f, 1.0f);
    r.chargeShare = std::clamp(r.chargeShare, 0.05f, 0.95f);
    for (size_t i = 0; i < kSoulsAttackKinds; ++i) {
        const std::string base = std::string("kinds.") + kSoulsKindNames[i] + ".";
        SoulsKind& k = r.kinds[i];
        k.stamina = static_cast<float>(def->number((base + "stamina").c_str(), k.stamina));
        k.damage = static_cast<float>(def->number((base + "damage").c_str(), k.damage));
        k.poise = static_cast<float>(def->number((base + "poise").c_str(), k.poise));
        k.speed = std::max(0.1f, static_cast<float>(def->number((base + "speed").c_str(), k.speed)));
        k.knockdown = def->flag((base + "knockdown").c_str(), k.knockdown);
        k.hyperArmor = def->flag((base + "hyperArmor").c_str(), k.hyperArmor);
        k.maxWarp = static_cast<float>(def->number((base + "maxWarp").c_str(), k.maxWarp));
    }
    r.movesets.clear();
    for (const std::string& key : def->keys("movesets")) {
        const std::string base = "movesets." + key + ".";
        SoulsMoveset ms;
        ms.key = key;
        const std::string weapons = base + "weapons";
        for (size_t i = 0; i < def->count(weapons.c_str()); ++i) {
            ms.weapons.push_back(def->textAt(weapons.c_str(), i));
        }
        const std::string styles = base + "styles";
        for (size_t i = 0; i < def->count(styles.c_str()); ++i) {
            ms.styles.push_back(def->textAt(styles.c_str(), i));
        }
        ms.speed = std::max(0.1f, static_cast<float>(def->number((base + "speed").c_str(), ms.speed)));
        ms.weaponPoise = static_cast<float>(def->number((base + "weaponPoise").c_str(), ms.weaponPoise));
        for (size_t i = 0; i < kSoulsAttackKinds; ++i) {
            const std::string clips = base + "clips." + kSoulsKindNames[i];
            for (size_t j = 0; j < def->count(clips.c_str()); ++j) {
                ms.clips[i].push_back(def->textAt(clips.c_str(), j));
            }
        }
        r.movesets.push_back(std::move(ms));
    }
}

// ---- Stamina and poise -----------------------------------------------------------------------

float MeleeSystem::stamina(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Stamina* s = reg.valid(pawn) ? reg.try_get<Stamina>(pawn) : nullptr;
    return s != nullptr ? s->value : souls_.staminaMax;
}

float MeleeSystem::staminaShare(Entity pawn) const {
    return std::clamp(stamina(pawn) / souls_.staminaMax, 0.0f, 1.0f);
}

bool MeleeSystem::spendStamina(Entity pawn, float points) {
    entt::registry& reg = things_.registry();
    if (!souls_.enabled || !reg.valid(pawn)) {
        return true;
    }
    Stamina* s = reg.try_get<Stamina>(pawn);
    if (s == nullptr) {
        s = &reg.emplace<Stamina>(pawn, Stamina{souls_.staminaMax, now()});
    }
    if (s->value <= 0.0f) {
        return false;
    }
    s->value = std::max(0.0f, s->value - std::max(0.0f, points));
    s->lastUse = now();
    return true;
}

float MeleeSystem::bodySizeOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Thing* t = reg.valid(pawn) ? reg.try_get<Thing>(pawn) : nullptr;
    return t != nullptr && t->def.valid() ? static_cast<float>(defs_.get(t->def).number("race.baseBodySize", 1.0)) : 1.0f;
}

bool MeleeSystem::humanlike(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Thing* t = reg.valid(pawn) ? reg.try_get<Thing>(pawn) : nullptr;
    return t != nullptr && t->def.valid() && defs_.get(t->def).text("race.intelligence") == "Humanlike";
}

float MeleeSystem::poiseMax(Entity pawn) const {
    return humanlike(pawn) ? souls_.personPoise : std::max(souls_.minPoise, souls_.poisePerBodySize * bodySizeOf(pawn));
}

float MeleeSystem::poiseShare(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const PoiseMeter* p = reg.valid(pawn) ? reg.try_get<PoiseMeter>(pawn) : nullptr;
    if (p == nullptr || p->value < 0.0f || now() - p->lastHit > souls_.poiseRegenSteps) {
        return 1.0f;
    }
    return std::clamp(p->value / std::max(1.0f, poiseMax(pawn)), 0.0f, 1.0f);
}

// ---- Movesets and presses --------------------------------------------------------------------

const SoulsMoveset* MeleeSystem::movesetOf(Entity pawn) const {
    if (!souls_.enabled || !things_.registry().valid(pawn)) {
        return nullptr;
    }
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon);
    if (set == nullptr || set->moveset < 0 || static_cast<size_t>(set->moveset) >= souls_.movesets.size()) {
        return nullptr;
    }
    return &souls_.movesets[static_cast<size_t>(set->moveset)];
}

bool MeleeSystem::hasMoveset(Entity pawn) const { return movesetOf(pawn) != nullptr; }

bool MeleeSystem::soulsStunned(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return m != nullptr && now() < m->stunUntil;
}

bool MeleeSystem::counterOpen(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return m != nullptr && now() < m->counterUntil;
}

bool MeleeSystem::charging(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return m != nullptr && m->phase == MeleePhase::Windup && m->chargeSteps >= 0;
}

SoulsAttack MeleeSystem::attackKind(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return m != nullptr && m->phase != MeleePhase::Ready ? m->kind : SoulsAttack::None;
}

bool MeleeSystem::exposed(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    return souls_.enabled && m != nullptr && now() < m->stunUntil && !m->guardStun && now() >= m->critImmuneUntil &&
           !reg.any_of<KnockedDown>(pawn) && !health_.incapacitated(pawn);
}

SoulsAttack MeleeSystem::criticalFor(Entity pawn, Entity target) const {
    const entt::registry& reg = things_.registry();
    const SoulsMoveset* ms = movesetOf(pawn);
    if (ms == nullptr || target == kNoEntity || target == pawn || !reg.valid(target) || !things_.spawned(target) ||
        !reg.all_of<Position, Posture>(target) || !reg.all_of<Position, Posture>(pawn)) {
        return SoulsAttack::None;
    }
    const Health* h = reg.try_get<Health>(target);
    if ((h != nullptr && h->dead) || health_.incapacitated(target)) {
        return SoulsAttack::None;  // downed or dead: the old finishers
    }
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(target);
    if (a.level != b.level) {
        return SoulsAttack::None;
    }
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    const float extra = std::max(0.0f, combat_.motion().bodyRadius(target) - kPawnRadius);
    const auto hasClip = [ms](SoulsAttack kind) { return !ms->clips[soulsIndex(kind)].empty(); };
    if (reg.all_of<KnockedDown>(target)) {
        return hasClip(SoulsAttack::GroundStab) && d <= souls_.criticalRange + extra ? SoulsAttack::GroundStab : SoulsAttack::None;
    }
    if (exposed(target) && hasClip(SoulsAttack::Riposte) && d <= souls_.criticalRange + extra) {
        return SoulsAttack::Riposte;
    }
    const Posture& vp = reg.get<Posture>(target);
    const float fl = std::sqrt(vp.facingX * vp.facingX + vp.facingY * vp.facingY);
    if (hasClip(SoulsAttack::Backstab) && d > 1e-3f && fl > 1e-3f && d <= souls_.backstabRange + extra &&
        bodySizeOf(target) < souls_.noKnockdownBodySize && (vp.facingX * dx + vp.facingY * dy) / (d * fl) < souls_.backstabCos) {
        return SoulsAttack::Backstab;
    }
    return SoulsAttack::None;
}

// A blow at a body on the ground (a downed enemy, a corpse before the player) keeps its low blows and finishers; one
// knocked down but still in the fight takes a ground stab when the moveset has one.
bool MeleeSystem::soulsLowTarget(Entity pawn, Entity target, bool player) const {
    const entt::registry& reg = things_.registry();
    if (target != kNoEntity) {
        if (!reg.valid(target)) {
            return false;
        }
        if (reg.all_of<KnockedDown>(target) && criticalFor(pawn, target) == SoulsAttack::GroundStab) {
            return false;
        }
        const Health* h = reg.try_get<Health>(target);
        const Posture* p = reg.try_get<Posture>(target);
        return (h != nullptr && h->dead) || health_.incapacitated(target) || (p != nullptr && p->stance == Stance::Prone);
    }
    return player && playerTarget(pawn) == kNoEntity && corpseAhead(pawn) != kNoEntity;
}

SoulsAttack MeleeSystem::soulsKindFor(Entity pawn, const Melee* m, const SoulsPress& press) const {
    const bool rolled = combat_.rollLocked(pawn) || (m != nullptr && m->rollEndsAt > 0 && now() <= m->rollEndsAt + souls_.rollAttackSteps);
    if (press.heavy) {
        if (m != nullptr && now() < m->counterUntil) {
            return SoulsAttack::GuardCounter;
        }
        if (press.jumping) {
            return SoulsAttack::JumpHeavy;
        }
        return press.running ? SoulsAttack::RunningHeavy : SoulsAttack::Heavy;
    }
    if (press.jumping) {
        return SoulsAttack::Jump;
    }
    if (rolled) {
        return SoulsAttack::Roll;
    }
    return press.running ? SoulsAttack::Running : SoulsAttack::Light;
}

float MeleeSystem::soulsCost(Entity attacker, const Melee& m) const {
    if (m.kind != SoulsAttack::None) {
        return souls_.kinds[soulsIndex(m.kind)].stamina;
    }
    if (humanlike(attacker)) {
        return m.weaponTool ? souls_.kinds[0].stamina : souls_.unarmedCost;
    }
    return souls_.animalCost;
}

bool MeleeSystem::soulsAttack(Entity pawn, Entity target, bool player, const SoulsPress& press, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!souls_.enabled || movesetOf(pawn) == nullptr || soulsLowTarget(pawn, target, player)) {
        return attack(pawn, target, player, why);  // the old swing (it checks the rest)
    }
    if (!things_.spawned(pawn) || !reg.all_of<Posture, Mover>(pawn)) {
        return fail("that is not a pawn on the map");
    }
    if (!soulsAble(reg, pawn)) {
        return fail("that pawn can't fight (downed or dead)");
    }
    if (combat_.switching(pawn)) {
        return fail("switching weapons");
    }
    if (isDressing(reg, pawn)) {
        return fail("putting on armour");
    }
    if (const Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
        return fail("busy shooting");
    }
    if (target != kNoEntity) {
        const Health* h = reg.valid(target) ? reg.try_get<Health>(target) : nullptr;
        if (target == pawn || !things_.spawned(target) || !reg.all_of<Posture>(target) || (h != nullptr && h->dead)) {
            return fail("the target isn't a living pawn on the map");
        }
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    if (now() < m.stunUntil) {
        return fail("staggered");
    }
    SoulsAttack kind = soulsKindFor(pawn, &m, press);
    // A press at a foe that is open to it is a critical attack (Elden Ring: R1 near a stance-broken foe, or behind one).
    // sneed 2026-11-28: any open hit on a foe whose poise broke is the riposte (the impale); a backstab is a light press;
    // one knocked to the ground takes a stab whatever the press.
    if (target != kNoEntity && !press.jumping) {
        const SoulsAttack crit = criticalFor(pawn, target);
        if (crit == SoulsAttack::GroundStab || crit == SoulsAttack::Riposte || (crit != SoulsAttack::None && !press.heavy)) {
            kind = crit;
        }
    }
    // In reach: a lunging kind reaches as far as it may be carried in. The player swings anyway (Elden Ring): at nobody,
    // straight ahead, when the target is too far.
    if (target != kNoEntity) {
        const Position& a = reg.get<Position>(pawn);
        const Position& b = reg.get<Position>(target);
        const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
        // A lunging kind's clip (a running heavy's leap) carries the body on by its own step.
        float clipStep = 0.0f;
        if (const SoulsMoveset* ms = movesetOf(pawn); ms != nullptr && !ms->clips[soulsIndex(kind)].empty()) {
            if (const MotionClip* c = combat_.motion().clip(ms->clips[soulsIndex(kind)].front()); c != nullptr && c->hitSeconds > 0.0f) {
                clipStep = std::max(0.0f, combat_.motion().forwardAt(c->def, c->hitSeconds)) * combat_.motion().sizeScale(pawn);
            }
        }
        const float reachTo = std::max(engageDistance(pawn, target), reach(pawn) + clipStep) + rules_.reachSlack +
                              souls_.kinds[soulsIndex(kind)].maxWarp + (player ? rules_.playerReachSlack : 0.0f);
        if (a.level != b.level || d > reachTo) {
            if (!player) {
                return fail("the target is out of reach");
            }
            target = kNoEntity;
        }
    }
    // Too early (sneed 2026-11-28: time the attacks, don't spam): a press during the wind-up is dropped and the attack
    // under way won't chain.
    if (souls_.earlyPressBreaksChain && player && m.kind != SoulsAttack::None && m.phase == MeleePhase::Windup && m.finisher == 0) {
        m.mistimed = true;
        m.buffered = SoulsAttack::None;
        m.bufferedTarget = kNoEntity;
        m.bufferedHold = false;
        return true;
    }
    // Mid-roll, or mid-attack before its combo opens: kept, it plays as soon as it can (Elden Ring's input buffer).
    const bool chainOpen = m.kind != SoulsAttack::None && m.phase == MeleePhase::Recovery && !m.mistimed && now() >= m.attackAfter &&
                           m.recoverySteps - m.stepsLeft >= static_cast<int>(std::lround(static_cast<float>(m.recoverySteps) * souls_.chainShare));
    if (m.mistimed && m.kind != SoulsAttack::None && m.phase == MeleePhase::Recovery && m.finisher == 0) {
        return true;  // the chain was broken by a press too early: this one is dropped too, the recovery plays out
    }
    if (combat_.rollLocked(pawn) || now() < m.openUntil || now() < m.attackAfter ||
        (m.kind != SoulsAttack::None && m.phase != MeleePhase::Ready && m.finisher == 0 && !chainOpen)) {
        m.buffered = kind;
        m.bufferedTarget = target;
        m.bufferedHold = press.hold;
        return true;
    }
    if (m.phase != MeleePhase::Ready && !chainOpen) {
        return fail("not ready to swing yet");
    }
    if (stamina(pawn) <= 0.0f) {
        return fail("out of stamina");
    }
    return soulsStart(pawn, target, player, kind, press.hold, why);
}

bool MeleeSystem::soulsRelease(Entity pawn) {
    entt::registry& reg = things_.registry();
    Melee* m = reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    if (m == nullptr) {
        return false;
    }
    m->holding = false;
    m->bufferedHold = false;
    return true;
}

// Starts an attack of `kind`: its clip (the next of its combo), timed so the blow lands when the clip's does, played
// on the whole body (the feet go where the clip takes them, not where the keys say), the attacker turned to its target
// and carried in or back to stand right for the blow.
bool MeleeSystem::soulsStart(Entity pawn, Entity target, bool player, SoulsAttack kind, bool hold, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon);
    const SoulsMoveset* ms = movesetOf(pawn);
    if (set == nullptr || ms == nullptr || set->tools.empty()) {
        return fail("that pawn has nothing to fight with");
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    const size_t k = soulsIndex(kind);
    // The combo: the next blow of the chain if the last attack was of it and isn't long over.
    const bool heavyChain = soulsHeavyChain(kind);
    const bool continuing = m.kind != SoulsAttack::None && soulsHeavyChain(m.kind) == heavyChain &&
                            (m.phase != MeleePhase::Ready || now() - m.endedAt <= souls_.comboResetSteps);
    const std::vector<std::string>* list = &ms->clips[k];
    if (list->empty()) {
        list = heavyChain ? &ms->clips[static_cast<size_t>(SoulsAttack::Heavy)] : &ms->clips[0];
    }
    if (list->empty()) {
        list = &ms->clips[0];
    }
    unsigned combo = 0;
    if ((kind == SoulsAttack::Light || kind == SoulsAttack::Heavy) && continuing && !list->empty()) {
        combo = (static_cast<unsigned>(m.combo) + 1u) % static_cast<unsigned>(list->size());
    }
    const BodyMotionSystem& motion = combat_.motion();
    const MotionClip* clip = nullptr;
    for (size_t i = 0; i < list->size() && clip == nullptr; ++i) {
        const MotionClip* c = motion.clip((*list)[(combo + i) % list->size()]);
        if (c != nullptr && c->hitSeconds > 0.0f) {
            clip = c;
        }
    }
    if (clip == nullptr && !set->attackClips.empty()) {
        clip = motion.clip(set->attackClips.front());  // the moveset's clips aren't measured yet: the style's own
    }
    const SoulsKind& rule = souls_.kinds[k];
    if (!spendStamina(pawn, rule.stamina)) {
        return fail("out of stamina");
    }
    // The blade: the hardest-hitting sweeping tool (a sword's edge), else the hardest-hitting.
    uint8_t toolIndex = 0;
    float best = -1.0f;
    for (size_t i = 0; i < set->tools.size(); ++i) {
        const float score = set->tools[i].power + (arcSwing(set->tools[i].damageType) ? 1000.0f : 0.0f);
        if (score > best) {
            best = score;
            toolIndex = static_cast<uint8_t>(i);
        }
    }
    // Pace: the clip's own, times the kind's and the moveset's speed, slowed by armour and quickened by skill.
    const float factor = combat_.attackTimeFactor(pawn) / swingSpeed(pawn) / std::max(0.1f, rule.speed * ms->speed);
    const float hitSeconds = clip != nullptr && clip->hitSeconds > 0.0f ? clip->hitSeconds : 0.5f;
    const float clipSeconds = clip != nullptr ? std::max(clip->seconds, hitSeconds + 0.1f) : 1.0f;
    const int windup = std::max(3, static_cast<int>(std::lround(hitSeconds * 30.0f * factor)));
    const int recovery = std::max(4, static_cast<int>(std::lround((clipSeconds - hitSeconds) * 30.0f * factor)));
    combat_.motion().stop(pawn);  // the last blow's clip (a combo goes straight on)
    m.phase = MeleePhase::Windup;
    m.stepsLeft = windup;
    m.windupSteps = windup;
    m.recoverySteps = recovery;
    m.target = target;
    m.tool = toolIndex;
    m.weaponTool = weapon;
    m.player = player;
    m.committed = false;
    m.low = false;
    m.finisher = 0;
    m.finisherSteps = 0;
    m.kind = kind;
    m.combo = static_cast<uint8_t>(combo);
    m.buffered = SoulsAttack::None;
    m.bufferedTarget = kNoEntity;
    m.bufferedHold = false;
    m.mistimed = false;
    m.holding = hold && kind == SoulsAttack::Heavy;
    m.chargeSteps = -1;
    m.aiChargeFor = 0;
    m.counterUntil = 0;
    m.guardStun = false;
    m.footworkUntil = 0;
    m.blockUntil = std::min(m.blockUntil, now());  // a weapon's guard drops to swing
    m.seed = splitMix64(seed_ ^ (static_cast<uint64_t>(now()) << 20) ^ (uint64_t{entt::to_integral(pawn)} * 0x9E3779B97F4A7C15ULL) ^
                        (static_cast<uint64_t>(k) << 8));
    reg.remove<Blocking>(pawn);  // a shield comes down to attack
    if (Mover* mover = reg.try_get<Mover>(pawn)) {
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
    ++swings_;
    if (target != kNoEntity) {
        m.engagedAt = now();
        const Position& to = reg.get<Position>(target);
        face(pawn, to.x, to.y);
        if (kind == SoulsAttack::Riposte) {
            ++ripostes_;
            // The impale goes in from the front: the reeling victim turns to meet it (PC 2026-11-28: it stood side on and
            // the blade went past it).
            const Position& from = reg.get<Position>(pawn);
            face(target, from.x, from.y);
        }
        if (kind == SoulsAttack::Riposte || kind == SoulsAttack::Backstab) {
            // Caught (Elden Ring's critical lock): the victim can't guard, roll or strike back until it is thrown down.
            soulsInterrupt(target, windup + souls_.criticalHoldSteps, true);
        } else if (!soulsCriticalKind(kind)) {
            prepareDefense(target, pawn, windup, m.seed);
        }
    }
    if (clip != nullptr && combat_.motion().hasClips(pawn)) {
        const Posture& posture = reg.get<Posture>(pawn);
        combat_.motion().playTimed(pawn, clip->def, hitSeconds, windup, posture.facingX, posture.facingY, false);
        combat_.motion().setLayers(pawn, false, false);  // the whole body: no walking under a sword blow
        if (target != kNoEntity) {
            // Line up the blow (Elden Ring's tracking): stand idealInset inside the reach when it lands.
            const Position& a = reg.get<Position>(pawn);
            const Position& b = reg.get<Position>(target);
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
            const float clipStep = std::max(0.0f, motion.forwardAt(clip->def, hitSeconds)) * motion.sizeScale(pawn);
            const float bodyExtra = std::max(0.0f, motion.bodyRadius(target) - kPawnRadius);
            float ideal = std::max(0.55f, reach(pawn) - souls_.idealInset) + bodyExtra;
            if (reg.all_of<Blocking>(target)) {
                ideal = std::max(ideal, souls_.shieldRoom + bodyExtra);  // not into a raised shield (the shield's room)
            }
            if (kind == SoulsAttack::Riposte) {
                ideal = souls_.impaleStandOff + bodyExtra;  // in close: the blade goes all the way through
            }
            const float shift = std::clamp(d - clipStep - ideal, -souls_.warpBack, rule.maxWarp);
            // Over most of the wind-up, so the carry is a lean into the blow, not a glide (sneed 2026-11-28: "moves weird").
            int steps = std::max(3, windup * 5 / 6);
            if (m.holding) {
                steps = std::min(steps, std::max(2, static_cast<int>(std::lround(static_cast<float>(windup) * souls_.chargeShare)) - 1));
            }
            if (std::fabs(shift) > 0.02f) {
                combat_.motion().shiftClip(pawn, shift, steps);
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
        e.message = set->tools[toolIndex].label;
        e.count = heavyChain ? 1 : 0;
        e.cell = cellOf(reg.get<Position>(pawn));
        events_->push_back(std::move(e));
    }
    return true;
}

// ---- Rolls, guards, canceling -----------------------------------------------------------------

void MeleeSystem::soulsEnd(Entity pawn, Melee& m) {
    m.phase = MeleePhase::Ready;
    m.stepsLeft = 0;
    m.endedAt = now();
    m.holding = false;
    m.chargeSteps = -1;
    m.buffered = SoulsAttack::None;
    combat_.motion().stop(pawn);
}

bool MeleeSystem::soulsRollGate(Entity pawn, std::string* why) {
    entt::registry& reg = things_.registry();
    if (!souls_.enabled) {
        return true;
    }
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    Melee* m = reg.try_get<Melee>(pawn);
    if (m != nullptr) {
        if (now() < m->stunUntil) {
            return fail("staggered");
        }
        if (now() < m->openUntil) {
            return fail("recovering from the attack");
        }
        if (m->kind != SoulsAttack::None && m->phase == MeleePhase::Windup) {
            return fail("mid-attack");
        }
        if (m->kind != SoulsAttack::None && m->phase == MeleePhase::Recovery) {
            if (m->recoverySteps - m->stepsLeft < static_cast<int>(std::lround(static_cast<float>(m->recoverySteps) * souls_.cancelShare))) {
                return fail("mid-attack");
            }
        }
    }
    if (stamina(pawn) <= 0.0f) {
        return fail("out of stamina");
    }
    if (m != nullptr && m->kind != SoulsAttack::None && m->phase == MeleePhase::Recovery) {
        soulsEnd(pawn, *m);
    }
    spendStamina(pawn, souls_.rollCost);
    reg.get_or_emplace<Melee>(pawn).rollEndsAt = now() + combat_.rollTotalSteps();
    return true;
}

bool MeleeSystem::soulsGuardGate(Entity pawn, std::string* why) {
    entt::registry& reg = things_.registry();
    if (!souls_.enabled) {
        return true;
    }
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    Melee* m = reg.try_get<Melee>(pawn);
    if (m == nullptr) {
        return true;
    }
    if (now() < m->stunUntil && !m->guardStun) {
        return fail("staggered");
    }
    if (now() < m->openUntil) {
        return fail("recovering from the attack");
    }
    if (m->kind != SoulsAttack::None && m->phase == MeleePhase::Windup) {
        return fail("mid-attack");
    }
    if (m->kind != SoulsAttack::None && m->phase == MeleePhase::Recovery) {
        if (m->recoverySteps - m->stepsLeft < static_cast<int>(std::lround(static_cast<float>(m->recoverySteps) * souls_.cancelShare))) {
            return fail("mid-attack");
        }
        soulsEnd(pawn, *m);
    }
    return true;
}

bool MeleeSystem::soulsCancel(Entity pawn) {
    entt::registry& reg = things_.registry();
    Melee* m = souls_.enabled && reg.valid(pawn) ? reg.try_get<Melee>(pawn) : nullptr;
    if (m == nullptr || m->kind == SoulsAttack::None || m->phase != MeleePhase::Recovery) {
        return false;
    }
    if (m->recoverySteps - m->stepsLeft < static_cast<int>(std::lround(static_cast<float>(m->recoverySteps) * souls_.cancelShare))) {
        return false;
    }
    if (m->buffered != SoulsAttack::None) {
        return false;  // a press is waiting: the combo goes on
    }
    soulsEnd(pawn, *m);
    m->openUntil = now() + souls_.openingSteps;  // walking out of it still leaves the opening (sneed 2026-11-28)
    return true;
}

// Stops what the pawn is doing for `steps`: its attack (wind-up or recovery), a held charge, a waiting press, and (with
// `dropGuard`) its raised shield or weapon.
void MeleeSystem::soulsInterrupt(Entity pawn, int steps, bool dropGuard) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn)) {
        return;
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
    if (m.finisher == 0 && (m.phase == MeleePhase::Windup || (m.phase == MeleePhase::Recovery && m.kind != SoulsAttack::None))) {
        m.phase = steps > 0 ? MeleePhase::Recovery : MeleePhase::Ready;
        m.stepsLeft = std::max(0, steps);
        m.recoverySteps = std::max(0, steps);
        m.kind = SoulsAttack::None;
        m.endedAt = now();
    }
    m.buffered = SoulsAttack::None;
    m.holding = false;
    m.chargeSteps = -1;
    m.aiChain = 0;
    m.defense = MeleeDefense::None;
    m.stunUntil = std::max(m.stunUntil, now() + steps);
    m.guardStun = false;
    if (dropGuard) {
        reg.remove<Blocking>(pawn);
        m.blockUntil = std::min(m.blockUntil, now());
    }
}

// A blow on a raised guard (a shield, or a weapon held up) facing the attacker: the guard takes it for stamina. True if
// it did (the event says how); false if the blow goes on to the body.
bool MeleeSystem::soulsGuard(Entity attacker, const Melee& m, Entity victim, float power, DefId damageType, Event& e) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(victim) || !soulsAble(reg, victim) || !reg.all_of<Position, Posture>(victim)) {
        return false;
    }
    const Blocking* shield = reg.try_get<Blocking>(victim);
    const bool weaponGuard = shield == nullptr && blocking(victim);
    if (shield == nullptr && !weaponGuard) {
        return false;
    }
    const Gear* victimGear = reg.try_get<Gear>(victim);
    const bool victimArmed = victimGear != nullptr && victimGear->weapon != kNoEntity && reg.valid(victimGear->weapon);
    if (weaponGuard && !victimArmed && m.weaponTool) {
        return false;  // bare fists up against a blade: the forearm takes it (Melee.cpp's arm block)
    }
    const Position& vp = reg.get<Position>(victim);
    const Position& ap = reg.get<Position>(attacker);
    const Posture& posture = reg.get<Posture>(victim);
    float gx = shield != nullptr && shield->hasDir ? shield->dirX : posture.facingX;
    float gy = shield != nullptr && shield->hasDir ? shield->dirY : posture.facingY;
    float tx = ap.x - vp.x;
    float ty = ap.y - vp.y;
    const float tl = std::sqrt(tx * tx + ty * ty);
    const float gl = std::sqrt(gx * gx + gy * gy);
    if (tl < 1e-4f || gl < 1e-4f || (gx * tx + gy * ty) / (tl * gl) < souls_.guardCos) {
        return false;  // from the side or behind: the guard doesn't cover it
    }
    Melee& vm = reg.get_or_emplace<Melee>(victim);
    Stamina* st = reg.try_get<Stamina>(victim);
    if (st == nullptr) {
        st = &reg.emplace<Stamina>(victim, Stamina{souls_.staminaMax, now()});
    }
    st->value -= soulsCost(attacker, m) * souls_.blockShare;
    st->lastUse = now();
    e.message = "blocked";
    ++blocks_;
    const float negation = std::clamp(shield != nullptr ? souls_.shieldNegation : souls_.weaponNegation, 0.0f, 1.0f);
    if (shield != nullptr) {
        const Entity held = combat_.apparelStats().activeShield(reg, victim);
        soulsSoundStruck(reg, held, e);
        e.hit = ProjectileHitKind::Armor;  // on the shield, as a shield's catch is
        combat_.shieldTakes(victim, held, power * negation, negation >= 1.0f, splitMix64(m.seed ^ 0x5D1EULL));
    } else if (const Gear* gear = reg.try_get<Gear>(victim); gear != nullptr) {
        soulsSoundStruck(reg, gear->weapon, e);
    }
    const HitFrom from{true, ap.x, ap.y, true};
    if (st->value < 0.0f) {
        // Out of stamina: the guard breaks (it drops, and the blocker reels for guardBreakSteps).
        st->value = 0.0f;
        e.count = 1;
        ++guardBreaks_;
        soulsInterrupt(victim, souls_.guardBreakSteps, true);
        combat_.stagger(victim, from);
        if (combat_.motion().hasClips(victim)) {
            combat_.motion().setLayers(victim, false, false);
        }
    } else {
        vm.stunUntil = std::max(vm.stunUntil, now() + souls_.blockStunSteps);
        vm.guardStun = true;
        vm.counterUntil = now() + souls_.blockStunSteps + souls_.counterSteps;
        vm.attackAfter = std::max(vm.attackAfter, now() + souls_.blockStunSteps + souls_.blockedDelaySteps);
    }
    // sneed 2026-11-28: a blow that meets a guard puts the attacker's next attack back by blockedDelaySteps (0.15 s), from
    // when its combo would have opened.
    if (Melee* am = reg.try_get<Melee>(attacker); am != nullptr && am->kind != SoulsAttack::None) {
        const int chainAt = static_cast<int>(std::lround(static_cast<float>(am->recoverySteps) * souls_.chainShare));
        am->attackAfter = std::max(am->attackAfter, now() + chainAt + souls_.blockedDelaySteps);
    }
    // What the guard lets through (a weapon's guard: a share of the blow).
    const float chip = power * (1.0f - negation);
    if (chip > 0.05f && damageType.valid()) {
        Rng rng(splitMix64(m.seed ^ 0xC41BULL));
        health_.applyDamage(victim, damageType, chip, -1, rng);
        e.amount = chip;
    }
    return true;
}

// A melee hit's poise damage. At zero the victim's poise breaks: a stagger, or a knockdown (a knocking-down kind, or a
// second break soon after the first), never a knockdown within knockdownImmuneSteps of the last.
void MeleeSystem::soulsPoise(Entity attacker, const Melee& m, Entity victim, float toolPower, const HitFrom& from) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(victim) || !things_.spawned(victim) || !soulsAble(reg, victim) || !reg.all_of<Posture, Position>(victim)) {
        return;
    }
    if (soulsCriticalKind(m.kind)) {
        // A riposte or a backstab throws the victim to the ground (no poise needed, no immunity); a ground stab keeps it
        // down where it lies.
        if (m.kind == SoulsAttack::Riposte) {
            // sneed 2026-11-28: the impale "is fast but the target is stunned as you do it, then staggers": held through
            // the blow, it reels back on its feet afterwards (no second riposte while it does).
            // Stunned on the blade for impaleHoldSteps (a recoil where it stands), then it reels back off it.
            const int hold = std::max(0, souls_.impaleHoldSteps);
            soulsInterrupt(victim, hold + souls_.staggerSteps, true);
            Melee& vm = reg.get_or_emplace<Melee>(victim);
            vm.critImmuneUntil = now() + hold + souls_.staggerSteps + 1;
            if (hold > 0 && from.known) {
                vm.impaleStaggerAt = now() + hold;
                vm.impaleFromX = from.x;
                vm.impaleFromY = from.y;
                const Position& at = reg.get<Position>(victim);
                // Doubled over the blade (the flinch toward the blow, not one that throws the arms and shield up in front).
                if (combat_.motion().throwBody(victim, from.x - at.x, from.y - at.y, false, 0.25f, false)) {
                    if (BodyMotion* recoil = reg.try_get<BodyMotion>(victim)) {
                        recoil->scale *= 0.15f;  // a recoil where it stands, on the blade: not carried off it
                    }
                }
                // The blade stays in: the impaler's thrust holds at the blow as long, then draws back.
                BodyMotion* thrust = reg.try_get<BodyMotion>(attacker);
                if (thrust != nullptr && thrust->clip.valid() && thrust->rate > 0.0f) {
                    Melee& am = reg.get_or_emplace<Melee>(attacker);
                    am.impaleResumeAt = now() + hold;
                    am.impaleRate = thrust->rate;
                    thrust->rate = 0.0f;
                }
            } else {
                combat_.stagger(victim, from);
            }
            if (combat_.motion().hasClips(victim)) {
                combat_.motion().setLayers(victim, false, false);
            }
        } else if (m.kind != SoulsAttack::GroundStab && bodySizeOf(victim) < souls_.noKnockdownBodySize) {
            soulsInterrupt(victim, 0, true);
            combat_.knockDown(victim, from, souls_.criticalKnockdown);
        }
        return;
    }
    const SoulsMoveset* ms = m.kind != SoulsAttack::None ? movesetOf(attacker) : nullptr;
    float damage = ms != nullptr ? ms->weaponPoise * souls_.kinds[soulsIndex(m.kind)].poise : toolPower * souls_.animalPoisePerPower;
    const Melee* vm = reg.try_get<Melee>(victim);
    if (vm != nullptr && vm->phase == MeleePhase::Windup && vm->kind != SoulsAttack::None && souls_.kinds[soulsIndex(vm->kind)].hyperArmor) {
        damage *= souls_.hyperArmorTaken;  // poise holds through a heavy blow's wind-up
    }
    PoiseMeter& p = reg.get_or_emplace<PoiseMeter>(victim);
    const float most = poiseMax(victim);
    if (now() < p.immuneUntil) {
        return;  // it broke a moment ago: 10 s before it can break again (sneed 2026-11-28)
    }
    if (p.value < 0.0f || now() - p.lastHit > souls_.poiseRegenSteps) {
        p.value = most;
    }
    p.value -= damage;
    p.lastHit = now();
    if (p.value > 0.0f) {
        return;
    }
    p.value = most;
    const bool second = now() - p.lastBreak <= souls_.doubleBreakSteps;
    p.lastBreak = now();
    p.immuneUntil = now() + souls_.poiseImmuneSteps;
    const bool knocks = m.kind != SoulsAttack::None && souls_.kinds[soulsIndex(m.kind)].knockdown;
    if ((knocks || second) && now() >= p.knockdownImmuneUntil && bodySizeOf(victim) < souls_.noKnockdownBodySize &&
        combat_.knockDown(victim, from, damage)) {
        p.knockdownImmuneUntil = now() + souls_.knockdownImmuneSteps;
        soulsInterrupt(victim, 0, true);
        return;
    }
    // Its stance broken: it reels for postureBreakSteps, open to a riposte.
    soulsInterrupt(victim, souls_.postureBreakSteps, true);
    combat_.stagger(victim, from);
    if (combat_.motion().hasClips(victim)) {
        combat_.motion().setLayers(victim, false, false);  // a stumble on the whole body, not a flinch over walking legs
    }
}

// ---- The step --------------------------------------------------------------------------------

// The wind-up's extra for an Elden Ring style attack: turning the clip with the attacker as it tracks its target, and
// a heavy held at its charge point. True when the wind-up stands still this step (held).
bool MeleeSystem::soulsStepWindup(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    BodyMotion* bm = reg.try_get<BodyMotion>(pawn);
    if (!m.committed && bm != nullptr && bm->clip.valid()) {
        // The clip's path turns with the body as it follows its target (the step forward goes at it).
        const Posture& posture = reg.get<Posture>(pawn);
        bm->faceX = posture.facingX;
        bm->faceY = posture.facingY;
    }
    // The impale's victim, caught, stands where it is turned to the blade through the thrust (nothing else may walk or
    // turn it meanwhile: it stepped aside and the thrust went past, PC 2026-11-28).
    if (m.kind == SoulsAttack::Riposte && m.target != kNoEntity && reg.valid(m.target) && reg.all_of<Posture>(m.target)) {
        const Position& from = reg.get<Position>(pawn);
        face(m.target, from.x, from.y);
        if (Mover* held = reg.try_get<Mover>(m.target)) {
            held->path.clear();
            held->vx = 0.0f;
            held->vy = 0.0f;
        }
    }
    if (m.kind == SoulsAttack::Heavy && m.chargeSteps == -1 && m.holding &&
        m.windupSteps - m.stepsLeft >= static_cast<int>(std::lround(static_cast<float>(m.windupSteps) * souls_.chargeShare))) {
        m.chargeSteps = 0;
        m.chargeRate = bm != nullptr ? bm->rate : 1.0f;
        if (bm != nullptr) {
            bm->rate = 0.0f;  // the body holds the blow drawn back
        }
    }
    if (m.chargeSteps < 0) {
        return false;
    }
    ++m.chargeSteps;
    if (!m.player && m.chargeSteps >= m.aiChargeFor) {
        m.holding = false;
    }
    if (!m.holding || m.chargeSteps >= souls_.maxChargeSteps) {
        if (m.chargeSteps >= souls_.minChargeSteps) {
            m.kind = SoulsAttack::Charged;
            spendStamina(pawn, std::max(0.0f, souls_.kinds[static_cast<size_t>(SoulsAttack::Charged)].stamina -
                                                  souls_.kinds[static_cast<size_t>(SoulsAttack::Heavy)].stamina));
            const int rest = std::max(1, m.stepsLeft);
            prepareDefense(m.target, pawn, rest, m.seed ^ 0xC4A8ULL);
        }
        if (bm != nullptr) {
            bm->rate = m.chargeRate;
        }
        m.chargeSteps = -2;
        m.holding = false;
    }
    return true;
}

// The recovery's extra: the next blow of the combo (a waiting press, or the fighter AI's planned chain) once the
// recovery has run chainShare of its length.
void MeleeSystem::soulsStepRecovery(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    if (m.recoverySteps - m.stepsLeft < static_cast<int>(std::lround(static_cast<float>(m.recoverySteps) * souls_.chainShare)) ||
        now() < m.stunUntil || now() < m.attackAfter || !soulsAble(reg, pawn)) {
        return;
    }
    if (m.mistimed) {
        m.buffered = SoulsAttack::None;  // a press came too early: no chain from this attack
        m.aiChain = 0;
        return;
    }
    SoulsAttack next = m.buffered;
    Entity target = m.bufferedTarget;
    bool hold = m.bufferedHold;
    if (next == SoulsAttack::None && !m.player && m.aiChain > 0) {
        next = soulsHeavyChain(m.kind) ? SoulsAttack::Heavy : SoulsAttack::Light;
        target = m.target;
        hold = false;
        --m.aiChain;
    }
    if (next == SoulsAttack::None || stamina(pawn) <= 0.0f) {
        m.buffered = SoulsAttack::None;
        return;
    }
    if (target != kNoEntity && (!reg.valid(target) || !things_.spawned(target) || health_.incapacitated(target))) {
        target = kNoEntity;
    }
    if (m.player && target == kNoEntity) {
        target = playerTarget(pawn);
        if (target != kNoEntity && (soulsLowTarget(pawn, target, true) || !inPlayerReach(pawn, target))) {
            target = kNoEntity;
        }
    }
    if (!m.player && (target == kNoEntity || !inReach(pawn, target))) {
        m.buffered = SoulsAttack::None;
        m.aiChain = 0;
        return;
    }
    const uint8_t chain = m.aiChain;
    if (soulsStart(pawn, target, m.player, next, hold, nullptr)) {
        m.aiChain = chain;
    }
}

void MeleeSystem::soulsStep() {
    if (!souls_.enabled) {
        return;
    }
    entt::registry& reg = things_.registry();
    // Stamina comes back over regenSeconds when the pawn isn't attacking, rolling or guarding.
    order_.clear();
    for (const Entity pawn : reg.view<Stamina>()) {
        order_.push_back(pawn);
    }
    std::sort(order_.begin(), order_.end());
    const float perStep = souls_.staminaMax / (souls_.regenSeconds * 30.0f);
    for (const Entity pawn : order_) {
        Stamina& s = reg.get<Stamina>(pawn);
        const Melee* m = reg.try_get<Melee>(pawn);
        const bool busy = m != nullptr && m->phase != MeleePhase::Ready;
        const bool guarding = reg.all_of<Blocking>(pawn) || blocking(pawn);
        if (guarding && souls_.guardHoldDrainPerSecond > 0.0f) {
            s.value = std::max(0.0f, s.value - souls_.guardHoldDrainPerSecond / 30.0f);
            s.lastUse = now();
        }
        if (busy || guarding || combat_.rollLocked(pawn) || now() - s.lastUse < souls_.regenDelaySteps) {
            continue;
        }
        s.value = std::min(souls_.staminaMax, s.value + perStep);
        if (s.value >= souls_.staminaMax) {
            reg.remove<Stamina>(pawn);  // full: nothing to keep
        }
    }
    // A press kept through a roll (a rolling attack) plays once the roll is over.
    order_.clear();
    for (const Entity pawn : reg.view<Melee>()) {
        order_.push_back(pawn);
    }
    std::sort(order_.begin(), order_.end());
    for (const Entity pawn : order_) {
        Melee& m = reg.get<Melee>(pawn);
        // The impaler draws its blade back out.
        if (m.impaleResumeAt != 0 && now() >= m.impaleResumeAt) {
            m.impaleResumeAt = 0;
            if (BodyMotion* thrust = reg.try_get<BodyMotion>(pawn); thrust != nullptr && thrust->rate == 0.0f) {
                thrust->rate = m.impaleRate;
            }
        }
        // On the blade it stands still; off it now, it staggers back (sneed 2026-11-28).
        if (m.impaleStaggerAt != 0 && now() < m.impaleStaggerAt) {
            if (Mover* held = reg.try_get<Mover>(pawn)) {
                held->path.clear();
                held->vx = 0.0f;
                held->vy = 0.0f;
            }
        }
        if (m.impaleStaggerAt != 0 && now() >= m.impaleStaggerAt) {
            m.impaleStaggerAt = 0;
            const Health* hurt = reg.try_get<Health>(pawn);
            if ((hurt == nullptr || !hurt->incapacitated()) && !reg.all_of<KnockedDown>(pawn) && things_.spawned(pawn)) {
                HitFrom from;
                from.known = true;
                from.blow = true;
                from.x = m.impaleFromX;
                from.y = m.impaleFromY;
                combat_.stagger(pawn, from);
                if (combat_.motion().hasClips(pawn)) {
                    combat_.motion().setLayers(pawn, false, false);
                }
            }
        }
        if (m.buffered == SoulsAttack::None || m.phase != MeleePhase::Ready || combat_.rollLocked(pawn) || now() < m.openUntil ||
            now() < m.attackAfter) {
            continue;
        }
        const SoulsAttack next = m.buffered;
        Entity target = m.bufferedTarget;
        const bool hold = m.bufferedHold;
        m.buffered = SoulsAttack::None;
        if (now() < m.stunUntil || !soulsAble(reg, pawn) || stamina(pawn) <= 0.0f || movesetOf(pawn) == nullptr) {
            continue;
        }
        if (target != kNoEntity && (!reg.valid(target) || !things_.spawned(target) || soulsLowTarget(pawn, target, m.player))) {
            target = kNoEntity;
        }
        if (m.player && target == kNoEntity) {
            const Entity t = playerTarget(pawn);
            target = t != kNoEntity && !soulsLowTarget(pawn, t, true) && inPlayerReach(pawn, t) ? t : kNoEntity;
        }
        if (!m.player && target == kNoEntity) {
            continue;
        }
        soulsStart(pawn, target, m.player, next, hold, nullptr);
    }
    soulsShieldRoom();
}

// The shield's room (sneed 2026-11-28: "the shield can clip the enemy, make sure there's a barrier when you hold it up"):
// a raised shield keeps any body in front of it shieldRoom metres off, centre to centre. The two are eased apart, half
// each (a shove both ways), by at most shieldPushPerStep a step, never into a wall.
void MeleeSystem::soulsShieldRoom() {
    entt::registry& reg = things_.registry();
    order_.clear();
    for (const Entity pawn : reg.view<Blocking>()) {
        order_.push_back(pawn);
    }
    if (order_.empty()) {
        return;
    }
    std::sort(order_.begin(), order_.end());
    const auto pushable = [&](Entity e) {
        return reg.valid(e) && things_.spawned(e) && reg.all_of<Position, Posture>(e) && soulsAble(reg, e) &&
               !combat_.rollLocked(e);
    };
    for (const Entity pawn : order_) {
        if (!pushable(pawn) || combat_.apparelStats().activeShield(reg, pawn) == kNoEntity) {
            continue;
        }
        const Blocking& b = reg.get<Blocking>(pawn);
        const Posture& posture = reg.get<Posture>(pawn);
        float gx = b.hasDir ? b.dirX : posture.facingX;
        float gy = b.hasDir ? b.dirY : posture.facingY;
        const float gl = std::sqrt(gx * gx + gy * gy);
        if (gl < 1e-4f) {
            continue;
        }
        gx /= gl;
        gy /= gl;
        for (const auto [other, thing, op] : reg.view<const Thing, Position>().each()) {
            if (other == pawn || thing.category != ThingCategory::Pawn || !pushable(other)) {
                continue;
            }
            Position& a = reg.get<Position>(pawn);
            if (op.level != a.level) {
                continue;
            }
            float dx = op.x - a.x;
            float dy = op.y - a.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float room = souls_.shieldRoom + std::max(0.0f, combat_.motion().bodyRadius(other) - kPawnRadius);
            if (d >= room || d < 1e-3f) {
                continue;
            }
            dx /= d;
            dy /= d;
            if (dx * gx + dy * gy < souls_.guardCos) {
                continue;  // beside or behind the shield: not in its way
            }
            const float push = std::min(room - d, 2.0f * souls_.shieldPushPerStep) * 0.5f;
            bool moved = false;
            if (pawnFitsAt(map_, op.x + dx * push, op.y + dy * push, kPawnRadius, op.level)) {
                op.x += dx * push;
                op.y += dy * push;
                moved = true;
            }
            if (pawnFitsAt(map_, a.x - dx * push, a.y - dy * push, kPawnRadius, a.level)) {
                a.x -= dx * push;
                a.y -= dy * push;
                moved = true;
            }
            if (moved) {
                ++shieldPushes_;
            }
        }
    }
}

// ---- The fighter AI --------------------------------------------------------------------------

bool MeleeSystem::aiSoulsTurn(Entity pawn, Entity victim) {
    entt::registry& reg = things_.registry();
    if (!souls_.enabled || movesetOf(pawn) == nullptr) {
        return false;
    }
    const Melee* m = reg.try_get<Melee>(pawn);
    if (m != nullptr && (now() < m->stunUntil || now() < m->openUntil)) {
        return true;  // reeling, stunned by a block, or still committed after its chain: nothing to do
    }
    if (m != nullptr && m->phase == MeleePhase::Ready && now() < m->attackAfter) {
        footwork(pawn, victim, false);  // its blow met a guard (or it took one): a moment before the next
        return true;
    }
    const bool idle = m == nullptr || m->phase == MeleePhase::Ready;
    if (idle && stamina(pawn) < souls_.aiStaminaFloor) {
        reg.remove<Blocking>(pawn);  // a raised guard stops the bar refilling
        footwork(pawn, victim, false);
        return true;
    }
    if (!idle || !ready(pawn)) {
        footwork(pawn, victim, false);
        return true;
    }
    Rng rng(splitMix64(seed_ ^ (static_cast<uint64_t>(now()) << 18) ^ (uint64_t{entt::to_integral(pawn)} * 0xA24BAED4963EE407ULL)));
    SoulsPress press;
    const float roll = rng.unit();
    const bool counter = counterOpen(pawn) && roll < souls_.aiCounterChance;
    const bool guarded = reg.all_of<Blocking>(victim) || guardedAgainst(victim, pawn);
    press.heavy = counter || (guarded ? roll < souls_.aiHeavyVsGuard : roll < souls_.aiHeavyChance);
    const Melee* vm = reg.try_get<Melee>(victim);
    const bool open = vm != nullptr && (vm->phase == MeleePhase::Recovery || now() < vm->stunUntil || now() < vm->openUntil);
    press.hold = press.heavy && !counter && open && rng.unit() < souls_.aiChargeChance;
    if (!soulsAttack(pawn, victim, false, press, nullptr)) {
        footwork(pawn, victim, false);
        return true;
    }
    Melee& mine = reg.get<Melee>(pawn);
    if (press.hold) {
        mine.aiChargeFor = souls_.minChargeSteps + static_cast<int>(rng.unit() * static_cast<float>(std::max(1, souls_.maxChargeSteps - souls_.minChargeSteps)));
    }
    const int skill = meleeSkill(pawn);
    const float t = static_cast<float>(std::clamp(skill, 0, 20)) / 20.0f;
    const int most = static_cast<int>(std::lround(static_cast<float>(souls_.aiComboMax[0]) +
                                                  (static_cast<float>(souls_.aiComboMax[1] - souls_.aiComboMax[0])) * t));
    const int presses = 1 + static_cast<int>(rng.unit() * static_cast<float>(std::max(1, most)));
    mine.aiChain = press.heavy ? 0 : static_cast<uint8_t>(std::clamp(std::min(presses, most) - 1, 0, 8));
    return true;
}

bool MeleeSystem::aiLeap(Entity pawn, Entity victim) {
    entt::registry& reg = things_.registry();
    if (!souls_.enabled || movesetOf(pawn) == nullptr || !ready(pawn) || stamina(pawn) < souls_.aiStaminaFloor ||
        !reg.valid(victim) || !things_.spawned(victim) || soulsLowTarget(pawn, victim, false)) {
        return false;
    }
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(victim);
    const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    if (a.level != b.level || d < souls_.aiLeapMin || d > souls_.aiLeapMax) {
        return false;
    }
    Rng rng(splitMix64(seed_ ^ (static_cast<uint64_t>(now()) << 22) ^ (uint64_t{entt::to_integral(pawn)} * 0x51ED27ULL)));
    if (rng.unit() >= souls_.aiLeapChance) {
        return false;
    }
    SoulsPress press;
    press.heavy = true;
    press.running = true;
    return soulsAttack(pawn, victim, false, press, nullptr);
}

}  // namespace colony

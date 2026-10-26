#include "ColonySim/Melee.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
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

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float meleeLerp(float a, float b, float t) { return a + (b - a) * t; }

// A MeleeRules value given at Melee skill 0 and 20, at this skill.
float meleeBySkill(const float (&v)[2], int skill) {
    return meleeLerp(v[0], v[1], static_cast<float>(std::clamp(skill, 0, 20)) / 20.0f);
}

// A RimWorld stat curve: straight lines between the points, flat beyond the ends.
float meleeCurve(const std::vector<float>& xs, const std::vector<float>& ys, float x) {
    if (xs.empty()) {
        return 0.0f;
    }
    if (x <= xs.front()) {
        return ys.front();
    }
    for (size_t i = 1; i < xs.size(); ++i) {
        if (x <= xs[i]) {
            const float span = xs[i] - xs[i - 1];
            return meleeLerp(ys[i - 1], ys[i], span > 0.0f ? (x - xs[i - 1]) / span : 1.0f);
        }
    }
    return ys.back();
}

void meleeReadCurve(const Def& def, const char* key, std::vector<float>& xs, std::vector<float>& ys) {
    const size_t n = def.count(key);
    if (n == 0) {
        return;
    }
    xs.clear();
    ys.clear();
    for (size_t i = 0; i < n; ++i) {
        const std::string point = std::string(key) + "." + std::to_string(i);
        xs.push_back(static_cast<float>(def.numberAt(point.c_str(), 0)));
        ys.push_back(static_cast<float>(def.numberAt(point.c_str(), 1)));
    }
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
    meleeReadCurve(*def, "hitChanceCurve", r.hitCurveX, r.hitCurveY);
    meleeReadCurve(*def, "dodgeChanceCurve", r.dodgeCurveX, r.dodgeCurveY);
    whole("defaultMeleeSkill", r.defaultSkill);
    num("hitCapacityWeight", r.hitCapacityWeight);
    num("hitCapacityCap", r.hitCapacityCap);
    num("dodgeMovingWeight", r.dodgeMovingWeight);
    num("dodgeSightWeight", r.dodgeSightWeight);
    num("dodgeSightCap", r.dodgeSightCap);
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
    whole("commitSteps", r.commitSteps);
    bySkill("reactionStepsBySkill", r.reactionSteps);
    num("reactionJitter", r.reactionJitter);
    bySkill("blockChanceBySkill", r.blockChance);
    whole("blockSteps", r.blockSteps);
    whole("blockRecoverySteps", r.blockRecoverySteps);
    whole("playerBlockSteps", r.playerBlockSteps);
    whole("dodgeHoldSteps", r.dodgeHoldSteps);
    r.minWindup = std::max(1, r.minWindup);
    r.commitSteps = std::max(0, r.commitSteps);
}

// Steps of a tool's swing spent winding up; the rest of its cooldown is recovery.
int meleeWindup(const MeleeRules& r, int cooldown) {
    return std::clamp(static_cast<int>(std::lround(static_cast<float>(cooldown) * r.windupShare)), r.minWindup,
                      std::max(r.minWindup, cooldown - 1));
}

bool meleeAble(const entt::registry& reg, Entity pawn) {
    const Health* h = reg.try_get<Health>(pawn);
    return h == nullptr || !h->incapacitated();
}

}  // namespace

MeleeSystem::MeleeSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, const DefDatabase& defs,
                         const GameTime& time, uint64_t seed)
    : things_(things), map_(map), combat_(combat), health_(health), defs_(defs), time_(time), seed_(seed) {
    meleeReadRules(defs_, rules_);
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
            t.cooldown = std::max(2, static_cast<int>(std::lround(def.number((base + ".cooldownTime").c_str(), 60.0) * cooldownFactor)));
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
        if (!weapon && def.text("race.intelligence") == "Animal") {
            set.reach = rules_.animalReachBase +
                        rules_.animalReachPerRoot * std::sqrt(static_cast<float>(std::max(0.1, def.number("race.baseBodySize", 1.0))));
        }
        if (weapon) {
            set.reach = rules_.defaultWeaponReach;
            if (rulesDef != nullptr) {
                set.reach = static_cast<float>(rulesDef->number(("weaponReach." + def.defName()).c_str(), set.reach));
            }
        }
    }
}

MeleeSystem::~MeleeSystem() = default;

int64_t MeleeSystem::now() const {
    return time_.seconds;
}

// ---- What a pawn fights with -----------------------------------------------------------------

const MeleeSystem::ToolSet* MeleeSystem::toolsOf(Entity pawn, bool& weapon) const {
    const entt::registry& reg = things_.registry();
    weapon = false;
    if (const Gear* gear = reg.try_get<Gear>(pawn); gear != nullptr && gear->weapon != kNoEntity && reg.valid(gear->weapon)) {
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

float MeleeSystem::reach(Entity pawn) const {
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon);
    return set != nullptr ? set->reach : 0.0f;
}

bool MeleeSystem::inReach(Entity pawn, Entity target) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.valid(target) || !reg.all_of<Position>(pawn) || !reg.all_of<Position>(target)) {
        return false;
    }
    const Position& a = reg.get<Position>(pawn);
    const Position& b = reg.get<Position>(target);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float r = reach(pawn) + rules_.reachSlack;
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
            return std::clamp(skill->level, 0, 20);
        }
    }
    return std::clamp(rules_.defaultSkill, 0, 20);
}

float MeleeSystem::capacity(Entity pawn, const char* name) const {
    return health_.tracks(pawn) ? health_.capacityValue(pawn, name) : 1.0f;
}

// The sum of a stat's offsets over the pawn's traits (Traits defs' degreeDatas.statOffsets).
float MeleeSystem::traitOffset(Entity pawn, const char* stat) const {
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    if (colonist == nullptr) {
        return 0.0f;
    }
    float total = 0.0f;
    for (const TraitRecord& trait : colonist->traits) {
        const Def* def = defs_.find("Traits", trait.def);
        if (def == nullptr) {
            continue;
        }
        for (size_t i = 0; i < def->count("degreeDatas"); ++i) {
            const std::string base = "degreeDatas." + std::to_string(i);
            if (def->integer((base + ".degree").c_str(), 0) == trait.degree) {
                total += static_cast<float>(def->number((base + ".statOffsets." + stat).c_str(), 0.0));
            }
        }
    }
    return total;
}

// RimWorld: Melee skill plus 12 per 100% of Manipulation and Sight above normal (up to 150%), plus
// traits (Brawler +4), through the hit chance curve.
float MeleeSystem::hitChance(Entity pawn) const {
    const float cap = rules_.hitCapacityCap;
    const float score = static_cast<float>(meleeSkill(pawn)) +
                        rules_.hitCapacityWeight * (std::min(capacity(pawn, "Manipulation"), cap) - 1.0f) +
                        rules_.hitCapacityWeight * (std::min(capacity(pawn, "Sight"), cap) - 1.0f) +
                        traitOffset(pawn, "meleeHitChance");
    return std::clamp(meleeCurve(rules_.hitCurveX, rules_.hitCurveY, score), 0.01f, 1.0f);
}

// RimWorld: Melee skill plus 18 per 100% of Moving above or below normal, 8 per 100% of Sight (up to
// 140%), plus traits (Nimble +15), through the dodge chance curve.
float MeleeSystem::dodgeChance(Entity pawn) const {
    const float score = static_cast<float>(meleeSkill(pawn)) + rules_.dodgeMovingWeight * (capacity(pawn, "Moving") - 1.0f) +
                        rules_.dodgeSightWeight * (std::min(capacity(pawn, "Sight"), rules_.dodgeSightCap) - 1.0f) +
                        traitOffset(pawn, "meleeDodgeChance");
    return std::clamp(meleeCurve(rules_.dodgeCurveX, rules_.dodgeCurveY, score), 0.0f, 1.0f);
}

float MeleeSystem::blockChance(Entity pawn) const {
    bool weapon = false;
    toolsOf(pawn, weapon);
    return weapon ? std::clamp(meleeBySkill(rules_.blockChance, meleeSkill(pawn)), 0.0f, 1.0f) : 0.0f;
}

bool MeleeSystem::ready(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !meleeAble(reg, pawn)) {
        return false;
    }
    const Melee* m = reg.try_get<Melee>(pawn);
    return (m == nullptr || m->phase == MeleePhase::Ready) && !combat_.switching(pawn);
}

bool MeleeSystem::winding(Entity pawn) const {
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    return m != nullptr && m->phase == MeleePhase::Windup;
}

bool MeleeSystem::blocking(Entity pawn) const {
    const Melee* m = things_.registry().try_get<Melee>(pawn);
    return m != nullptr && now() >= m->blockFrom && now() < m->blockUntil;
}

// ---- Orders ----------------------------------------------------------------------------------

bool MeleeSystem::attack(Entity pawn, Entity target, bool player, std::string* why) {
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
    if (combat_.switching(pawn)) {
        return fail("switching weapons");
    }
    if (const Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
        return fail("busy shooting");
    }
    if (const Melee* m = reg.try_get<Melee>(pawn); m != nullptr && m->phase != MeleePhase::Ready) {
        return fail("not ready to swing yet");
    }
    bool weapon = false;
    const ToolSet* set = toolsOf(pawn, weapon);
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
        if (!inReach(pawn, target)) {
            return fail("the target is out of reach");
        }
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
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
    const int windup = meleeWindup(rules_, tool.cooldown);
    m.phase = MeleePhase::Windup;
    m.stepsLeft = windup;
    m.target = target;
    m.weaponTool = weapon;
    m.player = player;
    m.committed = false;
    ++swings_;
    if (target != kNoEntity) {
        const Position& to = reg.get<Position>(target);
        face(pawn, to.x, to.y);
        prepareDefense(target, pawn, windup, m.seed);
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::MeleeSwing;
        e.thing = pawn;
        e.target = target;
        e.def = weapon ? reg.get<Thing>(reg.get<Gear>(pawn).weapon).def : reg.get<Thing>(pawn).def;
        e.amount = static_cast<float>(windup);
        e.message = tool.label;
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
    toolsOf(pawn, weapon);
    if (!weapon) {
        return fail("only a melee weapon can block");
    }
    Melee& m = reg.get_or_emplace<Melee>(pawn);
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

// A swing at `defender` has begun. It reads it after its reaction time (Melee skill), then, if it
// can, sidesteps as often as RimWorld's dodge chance says or else raises its weapon to block. The
// player defends by hand (MeleeCommand Block), so a controlled pawn does nothing by itself.
void MeleeSystem::prepareDefense(Entity defender, Entity attacker, int stepsToHit, uint64_t seed) {
    entt::registry& reg = things_.registry();
    if (reg.all_of<PlayerControl>(defender) || !meleeAble(reg, defender) || !reg.all_of<Mover, Posture>(defender)) {
        return;
    }
    Melee& d = reg.get_or_emplace<Melee>(defender);
    d.threat = attacker;
    d.defense = MeleeDefense::None;
    d.dodged = false;
    // Busy with its own swing or a shot: no defense (RimWorld: no dodging while aiming or firing).
    const Shooter* s = reg.try_get<Shooter>(defender);
    if (d.phase == MeleePhase::Windup || (s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing))) {
        return;
    }
    Rng rng(splitMix64(seed ^ 0xD0D6EULL ^ (uint64_t{entt::to_integral(defender)} << 32)));
    const int skill = meleeSkill(defender);
    const float reaction = meleeBySkill(rules_.reactionSteps, skill) + (2.0f * rng.unit() - 1.0f) * rules_.reactionJitter;
    const int64_t noticed = now() + std::max(1, static_cast<int>(std::lround(reaction)));
    const int64_t hitAt = now() + stepsToHit;
    const float dodgeRoll = rng.unit();
    const float blockRoll = rng.unit();
    if (reg.all_of<Fighter>(defender) && dodgeRoll < dodgeChance(defender)) {
        // It moves once the attacker is committed to the line (moving sooner, the swing would follow it).
        d.defense = MeleeDefense::Dodge;
        d.reactAt = std::max(noticed, hitAt - rules_.commitSteps);
    } else if (blockRoll < blockChance(defender)) {
        d.defense = MeleeDefense::Block;
        d.reactAt = noticed;
        d.blockFrom = noticed + rules_.blockSteps;
        d.blockUntil = hitAt + 1;
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
    if (defense == MeleeDefense::Dodge && swing->stepsLeft > 0) {
        sidestep(defender, d);
    }
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
        reg.get<Mover>(defender).walkTo(pos.x, pos.y, std::vector<CellPos>{n});
        d.dodged = true;
        ++dodges_;
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

// Fixes where the strike goes: the target's chest as it is now, moved by a random point in the circle
// around it that a standing man fills with the attacker's hit chance (RimWorld's melee hit chance).
// The player's swing goes straight at the target, or straight ahead at chest height.
void MeleeSystem::commit(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    m.committed = true;
    float ox = 0.0f;
    float oy = 0.0f;
    float oz = 0.0f;
    shoulder(pawn, ox, oy, oz);
    const bool hasTarget = m.target != kNoEntity && reg.valid(m.target) && things_.spawned(m.target) &&
                           combat_.aimPoint(m.target, m.aimX, m.aimY, m.aimZ);
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
    const float radius = combat_.spreadRadiusFor(hitChance(pawn));
    m.aimX += (rx * u + ux * w) * radius;
    m.aimY += (ry * u + uy * w) * radius;
    m.aimZ += uz * w * radius;
}

// The hit moment: a level strike from the attacker's centre through the aimed point (as a bullet
// would cross a man, so the hit chance table holds), as far as the reach allows, against hitboxes
// and walls. What it meets first is hit.
void MeleeSystem::strike(Entity pawn, Melee& m) {
    entt::registry& reg = things_.registry();
    if (!m.committed) {
        commit(pawn, m);
    }
    const Tool* tool = toolOf(pawn, m);
    if (tool == nullptr || !reg.all_of<Position>(pawn)) {
        return;  // the weapon is gone
    }
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
    const float dz = 0.0f;
    // As far as the aimed point and a little past it, but never further than the reach.
    const float length = std::min(flat + rules_.strikeDepth, reach(pawn));
    const float ex = ox + dx * length;
    const float ey = oy + dy * length;
    const float ez = oz;
    TraceHit hit = combat_.trace(ox, oy, oz, ex, ey, ez, pos.level, pawn, cellOf(pos));

    // A target without hitboxes (animals until they get theirs): hit if the strike passes within its
    // body, which is then hurt where health picks.
    if (hit.kind != ProjectileHitKind::Pawn && hit.kind != ProjectileHitKind::Building && m.target != kNoEntity &&
        reg.valid(m.target) && things_.spawned(m.target)) {
        std::vector<HitboxView> boxes;
        if (!combat_.hitboxesOf(m.target, boxes)) {
            const Position& t = reg.get<Position>(m.target);
            const float px = t.x - ox;
            const float py = t.y - oy;
            const float along = std::clamp(px * dx + py * dy, 0.0f, length);
            const float cx = ox + dx * along - t.x;
            const float cy = oy + dy * along - t.y;
            if (cx * cx + cy * cy <= kPawnRadius * kPawnRadius && t.level == pos.level) {
                hit.kind = ProjectileHitKind::Pawn;
                hit.thing = m.target;
                hit.part = -1;
                hit.x = t.x;
                hit.y = t.y;
                hit.z = oz + dz * along;
            }
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
    if (hit.kind == ProjectileHitKind::Pawn) {
        const Entity victim = hit.thing;
        Melee* vm = reg.try_get<Melee>(victim);
        const Position& vp = reg.get<Position>(victim);
        const Posture& vpost = reg.get<Posture>(victim);
        const bool facing = vpost.facingX * (pos.x - vp.x) + vpost.facingY * (pos.y - vp.y) > 0.0f;
        if (vm != nullptr && blocking(victim) && facing && meleeAble(reg, victim)) {
            // The weapon takes it, and the blocker staggers: no swing of its own for a moment.
            e.message = "blocked";
            ++blocks_;
            if (vm->phase == MeleePhase::Ready) {
                vm->phase = MeleePhase::Recovery;
                vm->stepsLeft = rules_.blockRecoverySteps;
            }
        } else if (hit.part < 0) {
            Rng rng(splitMix64(m.seed ^ 0xB0D7ULL));
            health_.applyDamage(victim, tool->damageType, tool->power, -1, rng);
            e.message = "hit";
            e.amount = tool->power;
            ++hits_;
            combat_.noteHurt(victim, pawn);
        } else {
            const PawnHitResult result =
                combat_.hitPawn(victim, hit.part, tool->damageType, tool->power, tool->penetration, splitMix64(m.seed ^ 0x417ULL));
            e.part = result.part;
            if (result.deflected) {
                e.hit = ProjectileHitKind::Armor;
                e.message = "deflected";
            } else {
                e.message = "hit";
                e.amount = result.amount;
                ++hits_;
            }
            if (events_ != nullptr) {
                events_->push_back(e);
            }
            if (!result.deflected) {
                health_.applyHit(victim, result.damageType, result.amount, result.part);
            }
            combat_.wearApparel(victim, result);
            if (!result.deflected) {
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

void MeleeSystem::step() {
    entt::registry& reg = things_.registry();
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
        if (d.defense != MeleeDefense::None && now() >= d.reactAt) {
            defend(pawn, d);
        }
    }
    for (const Entity pawn : order_) {
        Melee& m = reg.get<Melee>(pawn);
        switch (m.phase) {
            case MeleePhase::Ready:
                break;
            case MeleePhase::Recovery:
                if (--m.stepsLeft <= 0) {
                    m.phase = MeleePhase::Ready;
                    m.stepsLeft = 0;
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
                --m.stepsLeft;
                if (!m.committed && m.stepsLeft <= rules_.commitSteps) {
                    commit(pawn, m);
                }
                if (m.stepsLeft <= 0) {
                    strike(pawn, m);
                    const Tool* tool = toolOf(pawn, m);
                    m.phase = MeleePhase::Recovery;
                    m.stepsLeft = tool != nullptr ? std::max(1, tool->cooldown - meleeWindup(rules_, tool->cooldown)) : 1;
                }
                break;
        }
    }
}

void MeleeSystem::hashThing(Entity e, StateHasher& h) const {
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
}

}  // namespace colony

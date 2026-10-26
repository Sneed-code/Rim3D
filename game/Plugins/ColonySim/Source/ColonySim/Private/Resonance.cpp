#include "ColonySim/Resonance.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

float resoDist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

float resoCellDist(const Position& p, CellPos c) {
    return resoDist(p.x, p.y, static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f);
}

bool resoAlive(const entt::registry& reg, Entity pawn) {
    const Health* h = reg.try_get<Health>(pawn);
    return h == nullptr || !h->dead;
}

}  // namespace

ResonanceRules ResonanceRules::read(const DefDatabase& defs) {
    ResonanceRules r;
    const Def* d = defs.find("Rules", "ResonanceRules");
    if (d == nullptr) {
        return r;
    }
    r.maxLevel = std::max(1, static_cast<int>(d->integer("maxLevel", r.maxLevel)));
    r.strainLimitBase = static_cast<float>(d->number("strainLimitBase", r.strainLimitBase));
    r.strainLimitPerLevel = static_cast<float>(d->number("strainLimitPerLevel", r.strainLimitPerLevel));
    r.strainRecoveryPerStep = static_cast<float>(d->number("strainRecoveryPerStep", r.strainRecoveryPerStep));
    r.strainRecoveryPerLevel = static_cast<float>(d->number("strainRecoveryPerLevel", r.strainRecoveryPerLevel));
    r.strainRecoveryPerPain = static_cast<float>(d->number("strainRecoveryPerPain", r.strainRecoveryPerPain));
    if (const size_t n = d->count("focusDecayPerDay"); n > 0) {
        r.focusBandBelow.clear();
        r.focusDecayPerDay.clear();
        for (size_t i = 0; i < n; ++i) {
            const std::string at = "focusDecayPerDay." + std::to_string(i) + ".";
            r.focusBandBelow.push_back(static_cast<float>(d->number((at + "below").c_str(), 2.0)));
            r.focusDecayPerDay.push_back(static_cast<float>(d->number((at + "perDay").c_str(), 0.0)));
        }
    }
    if (const size_t n = d->count("minFocusByLevel"); n > 0) {
        r.minFocusByLevel.clear();
        for (size_t i = 0; i < n; ++i) {
            r.minFocusByLevel.push_back(static_cast<float>(d->numberAt("minFocusByLevel", i)));
        }
    }
    r.startFocus = std::clamp(static_cast<float>(d->number("startFocus", r.startFocus)), 0.0f, 1.0f);
    r.aiEvery = std::max(1, static_cast<int>(d->integer("aiEvery", r.aiEvery)));
    r.aiNumbPain = static_cast<float>(d->number("aiNumbPain", r.aiNumbPain));
    r.aiStunReach = static_cast<float>(d->number("aiStunReach", r.aiStunReach));
    r.aiBurdenRange = static_cast<float>(d->number("aiBurdenRange", r.aiBurdenRange));
    r.aiJumpThreat = static_cast<float>(d->number("aiJumpThreat", r.aiJumpThreat));
    r.aiJumpMin = static_cast<float>(d->number("aiJumpMin", r.aiJumpMin));
    return r;
}

ResonanceSystem::ResonanceSystem(Things& things, Map& map, HealthSystem& health, JobSystem& jobs, FighterSystem& fighters,
                                 const DefDatabase& defs, const GameTime& time, uint64_t seed)
    : things_(things), map_(map), health_(health), jobs_(jobs), fighters_(fighters), defs_(defs), time_(time), seed_(seed) {
    rules_ = ResonanceRules::read(defs_);
    for (const DefId id : defs_.idsOfKind("Powers")) {
        const Def& def = defs_.get(id);
        PowerDef p;
        p.def = id;
        p.defName = def.defName();
        const std::string kind = def.text("effect.kind");
        if (kind == "stun") {
            p.kind = PowerDef::Kind::Stun;
        } else if (kind == "painFactor") {
            p.kind = PowerDef::Kind::PainFactor;
        } else if (kind == "movingMax") {
            p.kind = PowerDef::Kind::MovingMax;
        } else if (kind == "jump") {
            p.kind = PowerDef::Kind::Jump;
        } else {
            continue;  // a power this build can't cast yet
        }
        p.level = std::max(1, static_cast<int>(def.integer("level", 1)));
        p.strain = static_cast<float>(def.number("strain"));
        p.focusCost = static_cast<float>(def.number("focusCost"));
        p.warmup = std::max(1, static_cast<int>(def.integer("warmup", 8)));
        p.range = static_cast<float>(def.number("range"));
        p.needsSight = def.flag("needsSight", true);
        p.self = def.flag("self");
        p.hostile = def.flag("hostile");
        p.value = static_cast<float>(def.number("effect.value", 1.0));
        p.duration = std::max(0, static_cast<int>(def.integer("effect.duration")));
        p.bySensitivity = def.flag("effect.bySensitivity");
        p.maxBodySize = static_cast<float>(def.number("effect.maxBodySize", 3.5));
        powers_.push_back(std::move(p));
    }
}

const PowerDef* ResonanceSystem::power(DefId def) const {
    for (const PowerDef& p : powers_) {
        if (p.def == def) {
            return &p;
        }
    }
    return nullptr;
}

const PowerDef* ResonanceSystem::power(const std::string& defName) const {
    for (const PowerDef& p : powers_) {
        if (p.defName == defName) {
            return &p;
        }
    }
    return nullptr;
}

int64_t ResonanceSystem::now() const { return time_.seconds; }

float ResonanceSystem::sensitivity(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing>(pawn)) {
        return 0.0f;
    }
    const float base = static_cast<float>(defs_.get(reg.get<Thing>(pawn).def).number("stats.resonanceSensitivity", 1.0));
    const PawnGenes* genes = reg.try_get<PawnGenes>(pawn);
    return std::max(0.0f, genes != nullptr ? geneStat(*genes, "ResonanceSensitivity", base) : base);
}

float ResonanceSystem::strainLimit(Entity pawn) const {
    const Attunement* a = attunement(pawn);
    if (a == nullptr) {
        return 0.0f;
    }
    return (rules_.strainLimitBase + static_cast<float>(a->level)) * rules_.strainLimitPerLevel * sensitivity(pawn);
}

const Attunement* ResonanceSystem::attunement(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) ? reg.try_get<Attunement>(pawn) : nullptr;
}

bool ResonanceSystem::knows(Entity pawn, DefId power) const {
    const Attunement* a = attunement(pawn);
    return a != nullptr && std::find(a->powers.begin(), a->powers.end(), power) != a->powers.end();
}

bool ResonanceSystem::casting(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<Casting>(pawn);
}

// ---- Commands ---------------------------------------------------------------------------------

bool ResonanceSystem::attune(Entity pawn, int level, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!things_.exists(pawn) || !reg.all_of<Health, Posture>(pawn) || !resoAlive(reg, pawn)) {
        error = "that is not a living pawn";
        return false;
    }
    if (level < 0 || level > rules_.maxLevel) {
        error = "attunement levels go from 0 to " + std::to_string(rules_.maxLevel);
        return false;
    }
    if (level == 0) {
        reg.remove<Attunement>(pawn);
        reg.remove<Casting>(pawn);
        return true;
    }
    if (sensitivity(pawn) <= 0.0f) {
        error = "that pawn is deaf to resonance";
        return false;
    }
    const bool fresh = !reg.all_of<Attunement>(pawn);
    Attunement& a = reg.get_or_emplace<Attunement>(pawn);
    const int from = fresh ? 1 : a.level + 1;
    if (fresh) {
        a.focus = rules_.startFocus;
    }
    a.level = level;
    // RimWorld: each new level gives one power of that level, at random. The pick is made from the
    // game's seed, the pawn and the level, so it doesn't depend on what else happened this step.
    for (int l = from; l <= level; ++l) {
        std::vector<DefId> choices;
        for (const PowerDef& p : powers_) {
            if (p.level == l && !knows(pawn, p.def)) {
                choices.push_back(p.def);
            }
        }
        if (choices.empty()) {
            continue;
        }
        Rng rng(splitMix64(seed_ ^ 0x7265736FULL ^ (uint64_t{entt::to_integral(pawn)} << 8) ^ static_cast<uint64_t>(l)));
        const DefId pick = choices[rng.below(static_cast<uint32_t>(choices.size()))];
        a.powers.insert(std::upper_bound(a.powers.begin(), a.powers.end(), pick,
                                         [](DefId x, DefId y) { return x.value < y.value; }),
                        pick);
    }
    return true;
}

bool ResonanceSystem::learn(Entity pawn, DefId powerId, std::string& error) {
    entt::registry& reg = things_.registry();
    Attunement* a = reg.valid(pawn) ? reg.try_get<Attunement>(pawn) : nullptr;
    const PowerDef* p = power(powerId);
    if (a == nullptr) {
        error = "that pawn is not attuned";
        return false;
    }
    if (p == nullptr) {
        error = "there is no such power";
        return false;
    }
    if (p->level > a->level) {
        error = "that power needs attunement level " + std::to_string(p->level);
        return false;
    }
    if (knows(pawn, powerId)) {
        error = "that pawn knows that power already";
        return false;
    }
    a->powers.insert(std::upper_bound(a->powers.begin(), a->powers.end(), powerId,
                                      [](DefId x, DefId y) { return x.value < y.value; }),
                     powerId);
    return true;
}

// Not downed, knocked down, stunned, firing a burst, throwing, swinging, rolling or switching weapons. A pawn
// aiming may: the cast drops the aim.
bool ResonanceSystem::canAct(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(pawn) || health_.incapacitated(pawn) || reg.any_of<KnockedDown, Stunned, Casting>(pawn)) {
        return false;
    }
    if (const Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && s->phase == ShotPhase::Firing) {
        return false;
    }
    if (const Thrower* t = reg.try_get<Thrower>(pawn); t != nullptr && t->phase == ThrowPhase::Winding) {
        return false;
    }
    if (const Melee* m = reg.try_get<Melee>(pawn); m != nullptr && m->phase != MeleePhase::Ready) {
        return false;
    }
    if (const DodgeRoll* r = reg.try_get<DodgeRoll>(pawn); r != nullptr && r->phase != RollPhase::Idle) {
        return false;
    }
    return !reg.all_of<WeaponSwitch>(pawn);
}

bool ResonanceSystem::sees(Entity from, Entity to) const {
    const entt::registry& reg = things_.registry();
    const Position& a = reg.get<Position>(from);
    const Position& b = reg.get<Position>(to);
    if (a.level != b.level) {
        return false;
    }
    if (from == to) {
        return true;
    }
    const Posture* pb = reg.try_get<Posture>(to);
    const Stance stance = health_.incapacitated(to) ? Stance::Prone : (pb != nullptr ? pb->stance : Stance::Standing);
    return map_.cover().lineOfSight(Viewpoint{a.x, a.y, a.level, Stance::Standing}, Viewpoint{b.x, b.y, b.level, stance});
}

bool ResonanceSystem::seesCell(Entity from, CellPos cell) const {
    const Position& a = things_.registry().get<Position>(from);
    if (a.level != cell.level) {
        return false;
    }
    if (cellOf(a) == cell) {
        return true;
    }
    return map_.cover().lineOfSight(Viewpoint{a.x, a.y, a.level, Stance::Standing}, Viewpoint::at(cell, Stance::Standing));
}

bool ResonanceSystem::checkTarget(Entity pawn, const PowerDef& p, Entity target, CellPos cell, std::string& error) const {
    const entt::registry& reg = things_.registry();
    if (target == kNoEntity) {
        target = pawn;
    }
    if (!things_.spawned(target) || !reg.all_of<Health, Posture>(target) || !resoAlive(reg, target)) {
        error = "the target isn't a living pawn on the map";
        return false;
    }
    if (target == pawn && !p.self) {
        error = "that power can't be used on oneself";
        return false;
    }
    const Position& at = reg.get<Position>(pawn);
    const Position& to = reg.get<Position>(target);
    if (to.level != at.level || resoDist(at.x, at.y, to.x, to.y) > p.range) {
        error = "the target is out of range";
        return false;
    }
    if (p.needsSight && !sees(pawn, target)) {
        error = "the target is out of sight";
        return false;
    }
    if (p.kind == PowerDef::Kind::Jump) {
        if (static_cast<float>(defs_.get(reg.get<Thing>(target).def).number("race.baseBodySize", 1.0)) > p.maxBodySize) {
            error = "the target is too big to move";
            return false;
        }
        if (!map_.inBounds(cell) || !map_.walkable(cell)) {
            error = "nobody can stand there";
            return false;
        }
        if (resoCellDist(at, cell) > p.range) {
            error = "that place is out of range";
            return false;
        }
        if (!seesCell(target, cell)) {
            error = "the target can't see that place";
            return false;
        }
    } else if (sensitivity(target) <= 0.0f) {
        error = "the target is deaf to resonance";
        return false;
    }
    return true;
}

bool ResonanceSystem::cast(Entity pawn, DefId powerId, Entity target, CellPos cell, bool player, std::string& error) {
    entt::registry& reg = things_.registry();
    const Attunement* a = attunement(pawn);
    const PowerDef* p = power(powerId);
    if (a == nullptr) {
        error = "that pawn is not attuned";
        return false;
    }
    if (p == nullptr) {
        error = "there is no such power";
        return false;
    }
    if (!knows(pawn, powerId)) {
        error = "that pawn doesn't know that power";
        return false;
    }
    if (p->level > a->level) {
        error = "that power needs attunement level " + std::to_string(p->level);
        return false;
    }
    if (!canAct(pawn)) {
        error = reg.all_of<Casting>(pawn) ? "already using a power" : "that pawn can't act now";
        return false;
    }
    const size_t band = static_cast<size_t>(p->level);
    const float minFocus = band < rules_.minFocusByLevel.size() ? rules_.minFocusByLevel[band] : 0.0f;
    if (a->focus < p->focusCost || a->focus < minFocus) {
        error = "not enough focus";
        return false;
    }
    if (a->strain + p->strain > strainLimit(pawn)) {
        error = "too much strain";
        return false;
    }
    if (!checkTarget(pawn, *p, target, cell, error)) {
        return false;
    }
    Casting& c = reg.emplace_or_replace<Casting>(pawn);
    c.power = powerId;
    c.target = target == kNoEntity ? pawn : target;
    c.cell = cell;
    c.stepsLeft = p->warmup;
    c.player = player;
    // It stands still through the warm-up: an aim is dropped, a fighter stops walking, a job waits (JobSystem's
    // pause gate).
    if (Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && s->phase == ShotPhase::Aiming) {
        s->phase = ShotPhase::Ready;
        s->stepsLeft = 0;
        s->shotsLeft = 0;
    }
    if (Mover* mover = reg.try_get<Mover>(pawn)) {
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
    if (reg.all_of<Fighter>(pawn)) {
        fighters_.replan(pawn);
    }
    if (Posture* posture = reg.try_get<Posture>(pawn); posture != nullptr && c.target != pawn) {
        const Position& at = reg.get<Position>(pawn);
        const Position& to = reg.get<Position>(c.target);
        const float d = resoDist(at.x, at.y, to.x, to.y);
        if (d > 0.01f) {
            posture->facingX = (to.x - at.x) / d;
            posture->facingY = (to.y - at.y) / d;
        }
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnAction;
        e.thing = pawn;
        e.def = reg.get<Thing>(pawn).def;
        e.action = PawnAction::Cast;
        e.target = c.target;
        e.cell = cell;
        e.amount = static_cast<float>(c.stepsLeft);
        events_->push_back(e);
    }
    return true;
}

// ---- Effects ----------------------------------------------------------------------------------

void ResonanceSystem::healthMods(Entity pawn, HealthMods& mods) const {
    const entt::registry& reg = things_.registry();
    if (const ResonanceEffects* fx = reg.valid(pawn) ? reg.try_get<ResonanceEffects>(pawn) : nullptr) {
        if (fx->numbSteps > 0) {
            mods.painFactor *= fx->painFactor;
        }
        if (fx->burdenSteps > 0) {
            mods.movingMax = std::min(mods.movingMax, fx->movingMax);
        }
    }
}

void ResonanceSystem::effectsChanged(Entity pawn) {
    if (Health* h = things_.registry().try_get<Health>(pawn)) {
        h->dirty = true;  // pain and Moving are worked out again at the next health step
    }
}

void ResonanceSystem::stun(Entity pawn, int steps) {
    entt::registry& reg = things_.registry();
    Stunned& s = reg.get_or_emplace<Stunned>(pawn);
    s.stepsLeft = std::max(s.stepsLeft, steps);
    // Whatever it was doing stops: a shot or throw in hand is dropped by their own systems, a cast here.
    reg.remove<Casting>(pawn);
    if (Mover* mover = reg.try_get<Mover>(pawn)) {
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
    if (reg.all_of<Fighter>(pawn)) {
        fighters_.replan(pawn);
    }
}

void ResonanceSystem::jump(Entity pawn, CellPos cell) {
    entt::registry& reg = things_.registry();
    Position& pos = reg.get<Position>(pawn);
    pos.x = static_cast<float>(cell.x) + 0.5f;
    pos.y = static_cast<float>(cell.y) + 0.5f;
    pos.level = cell.level;
    if (Mover* mover = reg.try_get<Mover>(pawn)) {
        mover->path.clear();
        mover->next = 0;
        mover->vx = 0.0f;
        mover->vy = 0.0f;
        mover->fromX = pos.x;
        mover->fromY = pos.y;
        mover->gaveUp = false;
    }
    // Where it was going is wrong now: a fighter plans again, a worker drops its job.
    if (reg.all_of<Fighter>(pawn)) {
        fighters_.replan(pawn);
    } else if (!reg.all_of<PlayerControl>(pawn)) {
        jobs_.interrupt(pawn, "jumped");
    }
}

void ResonanceSystem::release(Entity pawn, const Casting& c) {
    entt::registry& reg = things_.registry();
    const PowerDef* p = power(c.power);
    Attunement* a = reg.try_get<Attunement>(pawn);
    std::string why;
    if (p == nullptr || a == nullptr || !checkTarget(pawn, *p, c.target, c.cell, why) ||
        a->strain + p->strain > strainLimit(pawn) || a->focus < p->focusCost) {
        return;  // the target went out of reach meanwhile: the power fizzles, at no cost
    }
    a->strain += p->strain;
    a->focus = std::max(0.0f, a->focus - p->focusCost);
    const Entity target = c.target;
    const float sens = sensitivity(target);
    const int duration = p->bySensitivity ? static_cast<int>(std::lround(static_cast<float>(p->duration) * sens)) : p->duration;
    switch (p->kind) {
        case PowerDef::Kind::Stun:
            stun(target, duration);
            break;
        case PowerDef::Kind::PainFactor: {
            ResonanceEffects& fx = reg.get_or_emplace<ResonanceEffects>(target);
            fx.numbSteps = std::max(fx.numbSteps, duration);
            fx.painFactor = p->value;
            effectsChanged(target);
            break;
        }
        case PowerDef::Kind::MovingMax: {
            ResonanceEffects& fx = reg.get_or_emplace<ResonanceEffects>(target);
            fx.burdenSteps = std::max(fx.burdenSteps, duration);
            fx.movingMax = p->value;
            effectsChanged(target);
            break;
        }
        case PowerDef::Kind::Jump:
            jump(target, c.cell);
            break;
    }
    if (events_ != nullptr) {
        const Position& to = reg.get<Position>(target);
        Event e;
        e.type = EventType::PowerCast;
        e.thing = pawn;
        e.def = p->def;
        e.target = target;
        e.cell = c.cell;
        e.x = to.x;
        e.y = to.y;
        e.z = map_.heightAt(to.x, to.y);
        e.amount = a->strain;
        e.count = duration;
        events_->push_back(e);
    }
}

// ---- Each step --------------------------------------------------------------------------------

void ResonanceSystem::step() {
    entt::registry& reg = things_.registry();

    // Stuns and effects run out.
    scratch_.assign(reg.view<Stunned>().begin(), reg.view<Stunned>().end());
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        if (--reg.get<Stunned>(pawn).stepsLeft <= 0) {
            reg.remove<Stunned>(pawn);
        }
    }
    scratch_.assign(reg.view<ResonanceEffects>().begin(), reg.view<ResonanceEffects>().end());
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        ResonanceEffects& fx = reg.get<ResonanceEffects>(pawn);
        bool changed = false;
        if (fx.numbSteps > 0 && --fx.numbSteps == 0) {
            fx.painFactor = 1.0f;
            changed = true;
        }
        if (fx.burdenSteps > 0 && --fx.burdenSteps == 0) {
            fx.movingMax = 1.0f;
            changed = true;
        }
        if (changed) {
            effectsChanged(pawn);
        }
        if (fx.numbSteps == 0 && fx.burdenSteps == 0) {
            reg.remove<ResonanceEffects>(pawn);
        }
    }

    // Strain falls (faster at higher levels and in pain), focus fades by its band.
    scratch_.assign(reg.view<Attunement>().begin(), reg.view<Attunement>().end());
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        Attunement& a = reg.get<Attunement>(pawn);
        if (!resoAlive(reg, pawn)) {
            continue;
        }
        const Health* h = reg.try_get<Health>(pawn);
        const float pain = h != nullptr ? h->pain : 0.0f;
        const float recovery = rules_.strainRecoveryPerStep * (1.0f + rules_.strainRecoveryPerLevel * static_cast<float>(a.level - 1)) *
                               (1.0f + rules_.strainRecoveryPerPain * pain);
        a.strain = std::max(0.0f, a.strain - recovery);
        for (size_t b = 0; b < rules_.focusBandBelow.size(); ++b) {
            if (a.focus < rules_.focusBandBelow[b]) {
                a.focus = std::max(0.0f, a.focus - rules_.focusDecayPerDay[b] / static_cast<float>(kSecondsPerDay));
                break;
            }
        }
    }

    // Casts wind up and go off; one that can't go on is dropped.
    scratch_.assign(reg.view<Casting>().begin(), reg.view<Casting>().end());
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        if (!reg.all_of<Casting>(pawn)) {
            continue;  // stunned by a power that went off earlier in this loop
        }
        Casting& c = reg.get<Casting>(pawn);
        if (!things_.spawned(pawn) || health_.incapacitated(pawn) || reg.any_of<KnockedDown, Stunned>(pawn) ||
            !reg.all_of<Attunement>(pawn)) {
            reg.remove<Casting>(pawn);
            continue;
        }
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->vx = 0.0f;
            mover->vy = 0.0f;
        }
        if (--c.stepsLeft > 0) {
            continue;
        }
        const Casting done = c;
        reg.remove<Casting>(pawn);
        release(pawn, done);
    }

    // Attuned fighters choose a power, every aiEvery steps each (spread by pawn id).
    scratch_.clear();
    for (const Entity pawn : reg.view<Attunement, Fighter>()) {
        if ((now() + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.aiEvery == 0) {
            scratch_.push_back(pawn);
        }
    }
    std::sort(scratch_.begin(), scratch_.end());
    const std::vector<Entity> thinkers = scratch_;
    for (const Entity pawn : thinkers) {
        think(pawn);
    }
}

// The fighter AI's use of powers (docs/design/resonance.md): numb a friend (or itself) in pain, jump away
// to its spot when falling back from an enemy at arm's length, stun an enemy that is about to hit a
// friend or is shooting, burden one closing in. One power per look, in that order.
void ResonanceSystem::think(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Fighter& f = reg.get<Fighter>(pawn);
    const Attunement& a = reg.get<Attunement>(pawn);
    if (f.mode != FightMode::Fight || reg.all_of<PlayerControl>(pawn) || !canAct(pawn)) {
        return;
    }
    const FactionId side = fighters_.factionOf(pawn);
    const Position& self = reg.get<Position>(pawn);
    const float limit = strainLimit(pawn);
    const auto usable = [&](PowerDef::Kind kind) -> const PowerDef* {
        for (const DefId id : a.powers) {
            const PowerDef* p = power(id);
            const size_t band = p != nullptr ? static_cast<size_t>(p->level) : 0;
            const float minFocus = band < rules_.minFocusByLevel.size() ? rules_.minFocusByLevel[band] : 0.0f;
            if (p != nullptr && p->kind == kind && p->level <= a.level && a.strain + p->strain <= limit &&
                a.focus >= p->focusCost && a.focus >= minFocus) {
                return p;
            }
        }
        return nullptr;
    };
    std::string why;
    // Friends: living pawns of its side on the map, in id order (it is one of them).
    std::vector<Entity> friends;
    for (const Entity e : reg.view<Health, Posture, Position>()) {
        if (resoAlive(reg, e) && fighters_.factionOf(e) == side && !reg.all_of<Animal>(e)) {
            friends.push_back(e);
        }
    }
    std::sort(friends.begin(), friends.end());

    if (const PowerDef* p = usable(PowerDef::Kind::PainFactor)) {
        Entity best = kNoEntity;
        float bestPain = rules_.aiNumbPain;
        for (const Entity e : friends) {
            const ResonanceEffects* fx = reg.try_get<ResonanceEffects>(e);
            const float pain = reg.get<Health>(e).pain;
            if ((fx == nullptr || fx->numbSteps == 0) && pain >= bestPain && (e != pawn || p->self) &&
                checkTarget(pawn, *p, e, CellPos{}, why)) {
                if (pain > bestPain || best == kNoEntity) {
                    best = e;
                    bestPain = pain;
                }
            }
        }
        if (best != kNoEntity && cast(pawn, p->def, best, CellPos{}, false, why)) {
            return;
        }
    }

    // Enemies it knows of, able, on the map.
    struct Foe {
        Entity pawn;
        float distance;
        int priority;  // 2: within reach of a friend, 1: shooting, 0: neither
    };
    std::vector<Foe> foes;
    float nearest = 1e9f;
    for (const KnownEnemy& k : f.known) {
        if (!things_.spawned(k.pawn) || health_.incapacitated(k.pawn) || reg.all_of<Animal>(k.pawn)) {
            continue;
        }
        const Position& at = reg.get<Position>(k.pawn);
        if (at.level != self.level) {
            continue;
        }
        Foe foe{k.pawn, resoDist(self.x, self.y, at.x, at.y), 0};
        nearest = std::min(nearest, foe.distance);
        for (const Entity e : friends) {
            const Position& fp = reg.get<Position>(e);
            if (!health_.incapacitated(e) && fp.level == at.level && resoDist(fp.x, fp.y, at.x, at.y) <= rules_.aiStunReach) {
                foe.priority = 2;
                break;
            }
        }
        if (foe.priority == 0) {
            if (const Shooter* s = reg.try_get<Shooter>(k.pawn); s != nullptr && (s->phase == ShotPhase::Aiming || s->phase == ShotPhase::Firing)) {
                foe.priority = 1;
            }
        }
        foes.push_back(foe);
    }
    std::sort(foes.begin(), foes.end(), [](const Foe& x, const Foe& y) {
        return x.priority != y.priority ? x.priority > y.priority : (x.distance != y.distance ? x.distance < y.distance : x.pawn < y.pawn);
    });

    // Falling back with an enemy at arm's length: away first.
    if (const PowerDef* p = usable(PowerDef::Kind::Jump)) {
        if (f.fallingBack && nearest <= rules_.aiJumpThreat && f.hasSpot && resoCellDist(self, f.spot) >= rules_.aiJumpMin &&
            checkTarget(pawn, *p, pawn, f.spot, why) && cast(pawn, p->def, pawn, f.spot, false, why)) {
            return;
        }
    }

    if (const PowerDef* p = usable(PowerDef::Kind::Stun)) {
        for (const Foe& foe : foes) {
            if (foe.priority > 0 && !reg.all_of<Stunned>(foe.pawn) && checkTarget(pawn, *p, foe.pawn, CellPos{}, why)) {
                if (cast(pawn, p->def, foe.pawn, CellPos{}, false, why)) {
                    return;
                }
            }
        }
    }

    if (const PowerDef* p = usable(PowerDef::Kind::MovingMax)) {
        for (const Foe& foe : foes) {
            const ResonanceEffects* fx = reg.try_get<ResonanceEffects>(foe.pawn);
            const Mover* mover = reg.try_get<Mover>(foe.pawn);
            const bool walking = mover != nullptr && !mover->arrived();
            if (foe.distance <= rules_.aiBurdenRange && walking && (fx == nullptr || fx->burdenSteps == 0) &&
                !reg.all_of<Stunned>(foe.pawn) && checkTarget(pawn, *p, foe.pawn, CellPos{}, why)) {
                if (cast(pawn, p->def, foe.pawn, CellPos{}, false, why)) {
                    return;
                }
            }
        }
    }

}

void ResonanceSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const Attunement* a = reg.try_get<Attunement>(e)) {
        h.add(a->level);
        h.add(a->strain);
        h.add(a->focus);
        h.add(static_cast<uint32_t>(a->powers.size()));
        for (const DefId p : a->powers) {
            h.add(p.value);
        }
    }
    if (const Casting* c = reg.try_get<Casting>(e)) {
        h.add(c->power.value);
        h.add(static_cast<uint32_t>(entt::to_integral(c->target)));
        h.add(c->cell.x);
        h.add(c->cell.y);
        h.add(c->cell.level);
        h.add(c->stepsLeft);
        h.add(c->player);
    }
    if (const Stunned* s = reg.try_get<Stunned>(e)) {
        h.add(s->stepsLeft);
    }
    if (const ResonanceEffects* fx = reg.try_get<ResonanceEffects>(e)) {
        h.add(fx->numbSteps);
        h.add(fx->painFactor);
        h.add(fx->burdenSteps);
        h.add(fx->movingMax);
    }
}

}  // namespace colony

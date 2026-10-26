#include "ColonySim/Defense.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Power.h"
#include "ColonySim/Prisoners.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace colony {

// Everything read from the defs once: turrets, traps and the blasts buildings make.
struct DefenseModel {
    struct Blast {
        bool valid = false;
        float radius = 0.0f;
        float damage = 0.0f;
        float penetration = 0.0f;
        DefId damageType;
    };
    struct TurretInfo {
        bool valid = false;
        DefId gun;
        GunStats stats;
        int skill = 8;
        float muzzleHeight = 0.8f;
    };
    struct TrapInfo {
        bool valid = false;
        bool explodes = false;     // an IED
        int hits = 5;              // a spike trap: the stabs
        float damage = 0.0f;       // total of the stabs, before the random factor
        DefId damageType;
        float damageLow = 0.7f;    // the random factor on the total
        float damageHigh = 1.3f;
        float destroyChance = 0.0f;  // chance a spike trap is gone after it goes off
        float penetration = 0.0f;    // armor penetration of each stab
        float rearmSeconds = 13.0f;  // work a colonist's rearming takes
        bool rearmable = true;
    };

    std::vector<TurretInfo> turrets;  // by Things DefId value
    std::vector<TrapInfo> traps;
    std::vector<Blast> blasts;        // by Things DefId value: what a building makes when it explodes
    std::vector<uint8_t> explodeOnDestroy;

    DefenseModel(const DefDatabase& defs, const CombatSystem& combat) {
        turrets.resize(defs.size());
        traps.resize(defs.size());
        blasts.resize(defs.size());
        explodeOnDestroy.resize(defs.size(), 0);
        for (const DefId id : defs.idsOfKind("Things")) {
            const Def& def = defs.get(id);
            if (def.has("explosion")) {
                Blast& b = blasts[id.value];
                b.radius = static_cast<float>(def.number("explosion.radius"));
                b.damage = static_cast<float>(def.number("explosion.damage"));
                b.penetration = static_cast<float>(def.number("explosion.armorPenetration"));
                b.damageType = defs.idOf("DamageTypes", def.text("explosion.damageDef"));
                b.valid = b.radius > 0.0f && b.damage > 0.0f && b.damageType.valid();
                explodeOnDestroy[id.value] = def.flag("explodeWhenDestroyed") ? 1 : 0;
            }
            if (def.has("turret.gun")) {
                TurretInfo& t = turrets[id.value];
                t.gun = defs.idOf("Things", def.text("turret.gun"));
                t.skill = std::clamp(static_cast<int>(def.integer("turret.shootingSkill", 8)), 0, 20);
                t.muzzleHeight = static_cast<float>(def.number("turret.muzzleHeight", 0.8));
                t.valid = t.gun.valid() && combat.gunStats(t.gun, t.stats) && t.stats.range > 0.0f;
            }
            if (def.has("trap")) {
                TrapInfo& t = traps[id.value];
                t.explodes = def.flag("trap.explodes");
                t.hits = std::max(1, static_cast<int>(def.integer("trap.hits", 5)));
                t.damage = static_cast<float>(def.number("trap.damage"));
                t.damageType = defs.idOf("DamageTypes", def.text("trap.damageDef", "Stab"));
                t.damageLow = static_cast<float>(def.number("trap.damageFactorLow", 0.7));
                t.damageHigh = static_cast<float>(def.number("trap.damageFactorHigh", 1.3));
                t.destroyChance = static_cast<float>(def.number("trap.destroyChance"));
                t.penetration = static_cast<float>(def.number("trap.armorPenetration"));
                t.rearmSeconds = static_cast<float>(def.number("trap.rearmSeconds", 13.0));
                t.rearmable = def.flag("trap.rearmable", !t.explodes);
                t.valid = t.explodes ? blasts[id.value].valid : (t.damage > 0.0f && t.damageType.valid());
            }
        }
    }

    const TurretInfo* turret(DefId def) const {
        return def.valid() && def.value < turrets.size() && turrets[def.value].valid ? &turrets[def.value] : nullptr;
    }
    const TrapInfo* trap(DefId def) const {
        return def.valid() && def.value < traps.size() && traps[def.value].valid ? &traps[def.value] : nullptr;
    }
    const Blast* blast(DefId def) const {
        return def.valid() && def.value < blasts.size() && blasts[def.value].valid ? &blasts[def.value] : nullptr;
    }
};

DefenseNumbers DefenseNumbers::read(const DefDatabase& defs) {
    DefenseNumbers n;
    if (const Def* rules = defs.find("Rules", "DefenseRules")) {
        n.scanEvery = std::max(1, static_cast<int>(rules->integer("scanEvery", n.scanEvery)));
        n.lead = static_cast<float>(rules->number("lead", n.lead));
        n.noticeEvery = std::max(1, static_cast<int>(rules->integer("noticeEvery", n.noticeEvery)));
        n.noticeRadius = static_cast<float>(rules->number("noticeRadius", n.noticeRadius));
        n.noticeChance = static_cast<float>(rules->number("noticeChance", n.noticeChance));
        n.knownSpringChance = static_cast<float>(rules->number("knownSpringChance", n.knownSpringChance));
        n.avoidMargin = static_cast<float>(rules->number("avoidMargin", n.avoidMargin));
        n.detourTries = static_cast<int>(rules->integer("detourTries", n.detourTries));
    }
    return n;
}

namespace {

constexpr int kDefenseMaxBlastsPerStep = 64;  // chain reactions go on in the next step
constexpr float kDefenseSampleStep = 0.2f;    // metres between the spots a walk is tested at

float defenseDistance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

}  // namespace

DefenseSystem::DefenseSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, PowerGrid& power,
                             const DefDatabase& defs, const GameTime& time)
    : things_(things),
      map_(map),
      combat_(combat),
      health_(health),
      power_(power),
      time_(time),
      numbers_(DefenseNumbers::read(defs)),
      model_(std::make_unique<DefenseModel>(defs, combat)) {}

DefenseSystem::~DefenseSystem() = default;

bool DefenseSystem::isTurret(DefId def) const { return model_->turret(def) != nullptr; }
bool DefenseSystem::isTrap(DefId def) const { return model_->trap(def) != nullptr; }

FactionId DefenseSystem::factionOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Faction* f = reg.valid(pawn) ? reg.try_get<Faction>(pawn) : nullptr;
    return f != nullptr ? f->id : kPlayerFaction;
}

void DefenseSystem::collectPawns() {
    entt::registry& reg = things_.registry();
    pawns_.clear();
    for (const auto [e, thing, pos, posture] : reg.view<const Thing, const Position, const Posture>().each()) {
        if (thing.category != ThingCategory::Pawn) {
            continue;
        }
        const Health* h = reg.try_get<Health>(e);
        if (h != nullptr && h->dead) {
            continue;
        }
        PawnInfo p;
        p.pawn = e;
        p.faction = factionOf(e);
        p.x = pos.x;
        p.y = pos.y;
        p.level = pos.level;
        p.able = h == nullptr || !h->downed;
        p.animal = reg.all_of<Animal>(e) || reg.all_of<Prisoner>(e);  // turrets leave prisoners alone (C22)
        p.fighter = reg.all_of<Fighter>(e);
        pawns_.push_back(p);
    }
    std::sort(pawns_.begin(), pawns_.end(), [](const PawnInfo& a, const PawnInfo& b) { return a.pawn < b.pawn; });
}

// ---- Turrets ---------------------------------------------------------------------------------

// A pawn the turret may shoot: able, not an animal, not on the colony's side; within range and seen.
bool DefenseSystem::canSee(const Position& pos, float height, Entity target, float range) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(target) || !reg.all_of<Position, Posture>(target)) {
        return false;
    }
    const Position& tp = reg.get<Position>(target);
    if (tp.level != pos.level || defenseDistance(pos.x, pos.y, tp.x, tp.y) > range) {
        return false;
    }
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    if (!combat_.aimPoint(target, ax, ay, az)) {
        return false;
    }
    const TraceHit hit = combat_.trace(pos.x, pos.y, map_.heightAt(pos.x, pos.y) + height, ax, ay, az, pos.level,
                                       kNoEntity, cellOf(pos), false);
    return hit.kind == ProjectileHitKind::None;
}

Entity DefenseSystem::pickTarget(const Position& pos, float range, float height) const {
    Entity best = kNoEntity;
    float bestDistance = range + 1.0f;
    for (const PawnInfo& p : pawns_) {
        if (!p.able || p.animal || p.faction == kPlayerFaction || p.level != pos.level) {
            continue;
        }
        const float d = defenseDistance(pos.x, pos.y, p.x, p.y);
        if (d > range || d >= bestDistance) {
            continue;
        }
        if (canSee(pos, height, p.pawn, range)) {
            best = p.pawn;
            bestDistance = d;
        }
    }
    return best;
}

void DefenseSystem::stepTurret(Entity building, Turret& turret, Rng& rng) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(building);
    const DefenseModel::TurretInfo* info = model_->turret(reg.get<Thing>(building).def);
    if (info == nullptr) {
        return;
    }
    if (!power_.running(building)) {
        turret = Turret{};  // no power: it forgets its target, and the burst it was on
        return;
    }
    const GunStats& gun = info->stats;
    const auto stillGood = [&]() {
        if (turret.target == kNoEntity || !reg.valid(turret.target)) {
            return false;
        }
        const Health* h = reg.try_get<Health>(turret.target);
        if (h != nullptr && (h->dead || h->downed)) {
            return false;
        }
        return canSee(pos, info->muzzleHeight, turret.target, gun.range);
    };
    switch (turret.phase) {
        case TurretPhase::Idle: {
            if ((time_.seconds + static_cast<int64_t>(entt::to_integral(building))) % numbers_.scanEvery != 0) {
                return;
            }
            const Entity target = pickTarget(pos, gun.range, info->muzzleHeight);
            if (target == kNoEntity) {
                return;
            }
            turret.target = target;
            turret.phase = TurretPhase::Warming;
            turret.stepsLeft = gun.warmup;
            turret.shotsLeft = gun.burst;
            return;
        }
        case TurretPhase::Warming:
            if (!stillGood()) {
                turret = Turret{};
                return;
            }
            if (--turret.stepsLeft > 0) {
                return;
            }
            turret.phase = TurretPhase::Firing;
            turret.stepsLeft = 0;
            [[fallthrough]];
        case TurretPhase::Firing: {
            if (turret.stepsLeft > 0) {
                --turret.stepsLeft;
                return;
            }
            if (!stillGood()) {
                turret.phase = TurretPhase::Cooling;
                turret.stepsLeft = gun.cooldown;
                turret.target = kNoEntity;
                return;
            }
            float ax = 0.0f;
            float ay = 0.0f;
            float az = 0.0f;
            if (!combat_.leadPointFrom(pos.x, pos.y, info->gun, turret.target, numbers_.lead, ax, ay, az)) {
                turret = Turret{};
                return;
            }
            const float distance = defenseDistance(pos.x, pos.y, ax, ay);
            const float spread = combat_.spread(info->gun, info->skill, distance, false);
            combat_.fireFrom(building, info->gun, pos.x, pos.y, map_.heightAt(pos.x, pos.y) + info->muzzleHeight,
                             pos.level, cellOf(pos), ax, ay, az, spread, rng);
            ++shots_;
            if (--turret.shotsLeft > 0) {
                turret.stepsLeft = std::max(1, gun.burstGap);
            } else {
                turret.phase = TurretPhase::Cooling;
                turret.stepsLeft = gun.cooldown;
                turret.target = kNoEntity;
            }
            return;
        }
        case TurretPhase::Cooling:
            if (--turret.stepsLeft <= 0) {
                turret = Turret{};
            }
            return;
    }
}

// ---- Traps -----------------------------------------------------------------------------------

const std::vector<DefenseSystem::ArmedTrap>& DefenseSystem::armedTraps() const {
    const entt::registry& reg = things_.registry();
    const size_t traps = reg.view<Trap>().size();
    if (armedAt_ == time_.seconds && armedCount_ == traps) {
        return armed_;
    }
    armedCount_ = traps;
    armed_.clear();
    for (const auto [e, trap, pos] : reg.view<const Trap, const Position>().each()) {
        if (!trap.sprung) {
            armed_.push_back(ArmedTrap{e, cellOf(pos)});
        }
    }
    std::sort(armed_.begin(), armed_.end(), [](const ArmedTrap& a, const ArmedTrap& b) { return a.trap < b.trap; });
    armedAt_ = time_.seconds;
    return armed_;
}

bool DefenseSystem::knows(Entity pawn, Entity trap) const {
    const entt::registry& reg = things_.registry();
    const Trap* t = reg.valid(trap) ? reg.try_get<Trap>(trap) : nullptr;
    if (t == nullptr) {
        return false;
    }
    const FactionId faction = factionOf(pawn);
    return faction == kPlayerFaction || std::binary_search(t->known.begin(), t->known.end(), faction);
}

bool DefenseSystem::avoidsCell(FactionId faction, CellPos cell) const {
    if (faction == kPlayerFaction) {
        return false;
    }
    const entt::registry& reg = things_.registry();
    for (const ArmedTrap& a : armedTraps()) {
        if (a.cell == cell && std::binary_search(reg.get<Trap>(a.trap).known.begin(), reg.get<Trap>(a.trap).known.end(), faction)) {
            return true;
        }
    }
    return false;
}

bool DefenseSystem::segmentHits(float x0, float y0, float x1, float y1, CellPos cell) const {
    const float m = numbers_.avoidMargin;
    const float length = defenseDistance(x0, y0, x1, y1);
    const int n = std::max(1, static_cast<int>(std::ceil(length / kDefenseSampleStep)));
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float x = x0 + (x1 - x0) * t;
        const float y = y0 + (y1 - y0) * t;
        if (x >= static_cast<float>(cell.x) - m && x <= static_cast<float>(cell.x + 1) + m &&
            y >= static_cast<float>(cell.y) - m && y <= static_cast<float>(cell.y + 1) + m) {
            return true;
        }
    }
    return false;
}

void DefenseSystem::detour(FactionId faction, float startX, float startY, std::vector<CellPos>& route) const {
    if (route.empty() || faction == kPlayerFaction) {
        return;
    }
    const entt::registry& reg = things_.registry();
    std::vector<CellPos> cells;
    for (const ArmedTrap& a : armedTraps()) {
        const Trap& t = reg.get<Trap>(a.trap);
        if (std::binary_search(t.known.begin(), t.known.end(), faction)) {
            cells.push_back(a.cell);
        }
    }
    if (cells.empty()) {
        return;
    }
    const int level = route.back().level;
    const auto cx = [](CellPos c) { return static_cast<float>(c.x) + 0.5f; };
    const auto cy = [](CellPos c) { return static_cast<float>(c.y) + 0.5f; };
    const auto crossing = [&](float x0, float y0, float x1, float y1, CellPos* which) {
        for (const CellPos& c : cells) {
            if (c.level == level && segmentHits(x0, y0, x1, y1, c)) {
                if (which != nullptr) {
                    *which = c;
                }
                return true;
            }
        }
        return false;
    };

    for (int tries = 0; tries < numbers_.detourTries; ++tries) {
        bool changed = false;
        float fromX = startX;
        float fromY = startY;
        for (size_t i = 0; i < route.size(); ++i) {
            const bool last = i + 1 == route.size();
            const float toX = cx(route[i]);
            const float toY = cy(route[i]);
            CellPos trap;
            // The walk's own end is where the fighter chose to be; planning keeps it off known traps.
            if (!crossing(fromX, fromY, toX, toY, &trap) || (last && trap == route[i])) {
                fromX = toX;
                fromY = toY;
                continue;
            }
            if (!last && route[i] == trap) {
                route.erase(route.begin() + static_cast<std::ptrdiff_t>(i));  // a waypoint on the trap: walk past it instead
                changed = true;
                break;
            }
            // The cell beside the trap that makes the shortest walk to this waypoint and misses every known trap.
            float bestLength = 1e30f;
            CellPos best;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const CellPos c{trap.x + dx, trap.y + dy, trap.level};
                    if ((dx == 0 && dy == 0) || !map_.walkable(c) || map_.isDoor(c)) {
                        continue;
                    }
                    if (crossing(cx(c), cy(c), cx(c), cy(c), nullptr) || crossing(fromX, fromY, cx(c), cy(c), nullptr) ||
                        crossing(cx(c), cy(c), toX, toY, nullptr)) {
                        continue;
                    }
                    if (!straightWalkIsClear(map_, fromX, fromY, cx(c), cy(c), kPawnRadius, level) ||
                        !straightWalkIsClear(map_, cx(c), cy(c), toX, toY, kPawnRadius, level)) {
                        continue;
                    }
                    const float length = defenseDistance(fromX, fromY, cx(c), cy(c)) + defenseDistance(cx(c), cy(c), toX, toY);
                    if (length < bestLength) {
                        bestLength = length;
                        best = c;
                    }
                }
            }
            if (bestLength >= 1e30f) {
                fromX = toX;
                fromY = toY;
                continue;  // no way round: it goes through
            }
            route.insert(route.begin() + static_cast<std::ptrdiff_t>(i), best);
            changed = true;
            break;
        }
        if (!changed) {
            return;
        }
    }
}

void DefenseSystem::learn(Trap& trap, FactionId faction) {
    if (faction == kPlayerFaction || std::binary_search(trap.known.begin(), trap.known.end(), faction)) {
        return;
    }
    trap.known.insert(std::upper_bound(trap.known.begin(), trap.known.end(), faction), faction);
}

bool DefenseSystem::needsRearm(Entity trap) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(trap) || !things_.spawned(trap)) {
        return false;
    }
    const Trap* t = reg.try_get<Trap>(trap);
    const DefenseModel::TrapInfo* info = model_->trap(reg.get<Thing>(trap).def);
    return t != nullptr && t->sprung && info != nullptr && info->rearmable;
}

float DefenseSystem::rearmSeconds(Entity trap) const {
    const entt::registry& reg = things_.registry();
    const DefenseModel::TrapInfo* info = reg.valid(trap) ? model_->trap(reg.get<Thing>(trap).def) : nullptr;
    return info != nullptr ? info->rearmSeconds : 0.0f;
}

bool DefenseSystem::rearm(Entity trap, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    Trap* t = reg.valid(trap) ? reg.try_get<Trap>(trap) : nullptr;
    if (t == nullptr || !things_.spawned(trap)) {
        return fail("that is not a trap");
    }
    if (!t->sprung) {
        return fail("the trap is armed already");
    }
    const DefenseModel::TrapInfo* info = model_->trap(reg.get<Thing>(trap).def);
    if (info == nullptr || !info->rearmable) {
        return fail("that trap can't be used again");
    }
    t->sprung = false;
    armedAt_ = -1;
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::TrapRearmed;
        e.thing = trap;
        e.def = reg.get<Thing>(trap).def;
        e.cell = cellOf(reg.get<Position>(trap));
        events_->push_back(std::move(e));
    }
    return true;
}

void DefenseSystem::spring(Entity trap, Entity victim, Rng& rng) {
    entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(trap).def;
    const DefenseModel::TrapInfo* info = model_->trap(def);
    if (info == nullptr) {
        return;
    }
    Trap& t = reg.get<Trap>(trap);
    const Position pos = reg.get<Position>(trap);
    t.sprung = true;
    armedAt_ = -1;
    ++sprung_;
    learn(t, factionOf(victim));  // whoever walked onto it, its whole side knows where it was
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::TrapSprung;
        e.thing = trap;
        e.def = def;
        e.target = victim;
        e.cell = cellOf(pos);
        e.x = pos.x;
        e.y = pos.y;
        e.z = map_.heightAt(pos.x, pos.y);
        events_->push_back(std::move(e));
    }
    if (info->explodes) {
        const DefenseModel::Blast* blast = model_->blast(def);
        if (blast != nullptr) {
            Explosion ex;
            ex.x = pos.x;
            ex.y = pos.y;
            ex.z = map_.heightAt(pos.x, pos.y);
            ex.level = pos.level;
            ex.radius = blast->radius;
            ex.damage = blast->damage;
            ex.damageType = blast->damageType;
            ex.penetration = blast->penetration;
            ex.source = kNoEntity;
            ex.instigator = kNoEntity;
            ex.seed = (static_cast<uint64_t>(rng.next()) << 32) | rng.next();
            combat_.explode(ex);
        }
        if (reg.valid(trap)) {  // the blast may have taken it already
            if (beforeDestroy_) {
                beforeDestroy_(trap);
            }
            things_.destroy(trap);
        }
        return;
    }
    // A spike trap: a few stabs on parts that health picks, the total anywhere from 70% to 130%.
    const float total = info->damage * (info->damageLow + (info->damageHigh - info->damageLow) * rng.unit());
    const float each = total / static_cast<float>(info->hits);
    for (int i = 0; i < info->hits; ++i) {
        const Health* h = reg.try_get<Health>(victim);
        if (h == nullptr || h->dead) {
            break;
        }
        combat_.strikePawn(victim, info->damageType, each, info->penetration, rng);
    }
    if (info->destroyChance > 0.0f && rng.unit() < info->destroyChance) {
        if (beforeDestroy_) {
            beforeDestroy_(trap);
        }
        things_.destroy(trap);
    }
}

void DefenseSystem::stepTraps(Rng& rng) {
    entt::registry& reg = things_.registry();
    const std::vector<ArmedTrap> armed = armedTraps();  // a copy: springing changes the list
    if (armed.empty()) {
        return;
    }
    // Enemy fighters near an armed trap may notice it.
    if (time_.seconds % numbers_.noticeEvery == 0) {
        for (const ArmedTrap& a : armed) {
            Trap& t = reg.get<Trap>(a.trap);
            const Position& tp = reg.get<Position>(a.trap);
            for (const PawnInfo& p : pawns_) {
                if (!p.fighter || !p.able || p.faction == kPlayerFaction || p.level != tp.level ||
                    std::binary_search(t.known.begin(), t.known.end(), p.faction) ||
                    defenseDistance(p.x, p.y, tp.x, tp.y) > numbers_.noticeRadius) {
                    continue;
                }
                if (rng.unit() < numbers_.noticeChance) {
                    learn(t, p.faction);
                    // Its whole side knows now: fighters on their way plan again, round the trap.
                    if (onLearn_) {
                        for (const PawnInfo& mate : pawns_) {
                            if (mate.fighter && mate.able && mate.faction == p.faction) {
                                onLearn_(mate.pawn);
                            }
                        }
                    }
                }
            }
        }
    }
    // Anyone standing on an armed trap who doesn't know it springs it, one pawn at a time.
    for (const ArmedTrap& a : armed) {
        if (!reg.valid(a.trap) || reg.get<Trap>(a.trap).sprung) {
            continue;
        }
        for (const PawnInfo& p : pawns_) {
            if (p.level != a.cell.level || static_cast<int>(std::floor(p.x)) != a.cell.x ||
                static_cast<int>(std::floor(p.y)) != a.cell.y || !reg.valid(p.pawn)) {
                continue;
            }
            const Health* h = reg.try_get<Health>(p.pawn);
            if (h != nullptr && h->dead) {
                continue;
            }
            if (knows(p.pawn, a.trap)) {
                const bool own = p.faction == kPlayerFaction;
                if (own || numbers_.knownSpringChance <= 0.0f || rng.unit() >= numbers_.knownSpringChance) {
                    continue;
                }
            }
            spring(a.trap, p.pawn, rng);
            break;
        }
    }
}

// ---- Buildings shot to pieces ----------------------------------------------------------------

void DefenseSystem::queueBlast(Entity building, const Position& pos) {
    const entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(building).def;
    const DefenseModel::Blast* blast = model_->blast(def);
    if (blast == nullptr || model_->explodeOnDestroy[def.value] == 0) {
        return;
    }
    Blast b;
    b.explosion.x = pos.x;
    b.explosion.y = pos.y;
    b.explosion.z = map_.heightAt(pos.x, pos.y);
    b.explosion.level = pos.level;
    b.explosion.radius = blast->radius;
    b.explosion.damage = blast->damage;
    b.explosion.damageType = blast->damageType;
    b.explosion.penetration = blast->penetration;
    blasts_.push_back(std::move(b));
}

void DefenseSystem::onDestroy(Entity building) {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(building) || !reg.all_of<Thing, Position, Footprint>(building)) {
        return;
    }
    if (const Trap* t = reg.try_get<Trap>(building); t != nullptr && t->sprung) {
        return;  // already went off (an IED), or is spent
    }
    armedAt_ = -1;
    queueBlast(building, reg.get<Position>(building));
}

void DefenseSystem::step(Rng& rng) {
    entt::registry& reg = things_.registry();
    // Blasts of buildings that were shot to pieces, which may take more of them down (the rest wait a step).
    int done = 0;
    while (!blasts_.empty() && done < kDefenseMaxBlastsPerStep) {
        Blast b = std::move(blasts_.front());
        blasts_.erase(blasts_.begin());
        b.explosion.seed = (static_cast<uint64_t>(rng.next()) << 32) | rng.next();
        combat_.explode(b.explosion);
        ++done;
    }
    const bool turrets = !reg.view<Turret>().empty();
    const bool traps = !reg.view<Trap>().empty();
    if (!turrets && !traps) {
        return;
    }
    collectPawns();
    if (turrets) {
        order_.clear();
        for (const Entity e : reg.view<Turret>()) {
            order_.push_back(e);
        }
        std::sort(order_.begin(), order_.end());
        for (const Entity building : order_) {
            if (reg.valid(building) && reg.all_of<Position>(building)) {
                stepTurret(building, reg.get<Turret>(building), rng);
            }
        }
    }
    if (traps) {
        stepTraps(rng);
    }
}

void DefenseSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const Turret* t = reg.try_get<Turret>(e)) {
        h.add(uint64_t{0x7075});
        h.add(static_cast<int>(t->phase));
        h.add(t->stepsLeft);
        h.add(t->shotsLeft);
        h.add(entt::to_integral(t->target));
    }
    if (const Trap* t = reg.try_get<Trap>(e)) {
        h.add(uint64_t{0x7472});
        h.add(t->sprung);
        h.add(static_cast<uint64_t>(t->known.size()));
        for (const FactionId f : t->known) {
            h.add(f);
        }
    }
}

}  // namespace colony

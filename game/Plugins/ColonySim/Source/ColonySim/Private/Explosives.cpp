#include "ColonySim/Explosives.h"

#include "ColonySim/Combat.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace colony {

// Everything read from the defs once: grenade items, the projectiles they throw, the rules.
struct ExplosiveModel {
    struct Item {
        bool valid = false;
        DefId projectile;
        float range = 0.0f;
        int warmup = 0;    // steps
        int cooldown = 0;  // steps
        float missRadius = 0.0f;
    };
    struct Proj {
        bool valid = false;
        float speed = 0.0f;  // metres per step
        int fuse = 0;        // steps after landing
        float radius = 0.0f;
        float damage = 0.0f;
        float penetration = 0.0f;
        DefId damageType;
    };

    std::vector<Item> items;  // by Things DefId value
    std::vector<Proj> projs;  // by Projectiles DefId value
    float missDistance = 9.0f;
    float missFactor[2] = {1.0f, 0.5f};
    float playerMissFactor = 0.6f;
    int playerWarmup = 0;
    float lead[2] = {0.4f, 1.0f};
    float landingTolerance = 1.0f;
    int beltLimit = 10;
    float releaseDrop = 0.2f;
    int maxFlight = 200;

    explicit ExplosiveModel(const DefDatabase& defs) {
        items.resize(defs.size());
        projs.resize(defs.size());
        if (const Def* rules = defs.find("Rules", "ExplosiveRules")) {
            missDistance = static_cast<float>(rules->number("missDistance", missDistance));
            for (size_t i = 0; i < 2; ++i) {
                missFactor[i] = static_cast<float>(rules->numberAt("missFactorBySkill", i, missFactor[i]));
                lead[i] = static_cast<float>(rules->numberAt("leadBySkill", i, lead[i]));
            }
            playerMissFactor = static_cast<float>(rules->number("playerMissFactor", playerMissFactor));
            playerWarmup = static_cast<int>(rules->integer("playerWarmup", playerWarmup));
            landingTolerance = static_cast<float>(rules->number("landingTolerance", landingTolerance));
            beltLimit = static_cast<int>(rules->integer("beltLimit", beltLimit));
            releaseDrop = static_cast<float>(rules->number("releaseDrop", releaseDrop));
        }
        for (const DefId id : defs.idsOfKind("Projectiles")) {
            const Def& def = defs.get(id);
            if (!def.has("explosion") || !def.has("thrown")) {
                continue;
            }
            Proj& p = projs[id.value];
            p.speed = static_cast<float>(def.number("thrown.speed"));
            p.fuse = static_cast<int>(def.integer("thrown.fuse"));
            p.radius = static_cast<float>(def.number("explosion.radius"));
            p.damage = static_cast<float>(def.number("explosion.damage"));
            p.penetration = static_cast<float>(def.number("explosion.armorPenetration"));
            p.damageType = defs.idOf("DamageTypes", def.text("explosion.damageDef"));
            p.valid = p.speed > 0.0f && p.radius > 0.0f && p.damage > 0.0f && p.damageType.valid();
        }
        for (const DefId id : defs.idsOfKind("Things")) {
            const Def& def = defs.get(id);
            if (!def.has("thrown.projectile")) {
                continue;
            }
            Item& item = items[id.value];
            item.projectile = defs.idOf("Projectiles", def.text("thrown.projectile"));
            item.range = static_cast<float>(def.number("thrown.range"));
            item.warmup = static_cast<int>(std::lround(def.number("thrown.warmupTime")));
            item.cooldown = static_cast<int>(std::lround(def.number("thrown.cooldown")));
            item.missRadius = static_cast<float>(def.number("thrown.missRadius"));
            item.valid = item.projectile.valid() && proj(item.projectile) != nullptr;
        }
    }

    const Item* item(DefId def) const {
        return def.valid() && def.value < items.size() && items[def.value].valid ? &items[def.value] : nullptr;
    }
    const Proj* proj(DefId def) const {
        return def.valid() && def.value < projs.size() && projs[def.value].valid ? &projs[def.value] : nullptr;
    }
};

namespace {

float explosiveBySkill(const float (&v)[2], int skill) {
    const float t = std::clamp(static_cast<float>(skill) / 20.0f, 0.0f, 1.0f);
    return v[0] + (v[1] - v[0]) * t;
}

float explosiveDistance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

}  // namespace

ExplosiveSystem::ExplosiveSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health,
                                 const DefDatabase& defs)
    : things_(things), map_(map), combat_(combat), health_(health), model_(std::make_unique<ExplosiveModel>(defs)) {}

ExplosiveSystem::~ExplosiveSystem() = default;

bool ExplosiveSystem::isThrown(DefId item) const { return model_->item(item) != nullptr; }

float ExplosiveSystem::throwRange(DefId item) const {
    const ExplosiveModel::Item* i = model_->item(item);
    return i != nullptr ? i->range : 0.0f;
}

float ExplosiveSystem::blastRadius(DefId item) const {
    const ExplosiveModel::Item* i = model_->item(item);
    return i != nullptr ? model_->proj(i->projectile)->radius : 0.0f;
}

// ---- The belt --------------------------------------------------------------------------------

int ExplosiveSystem::count(Entity pawn, DefId item) const {
    const entt::registry& reg = things_.registry();
    const Belt* belt = reg.valid(pawn) ? reg.try_get<Belt>(pawn) : nullptr;
    int total = 0;
    if (belt != nullptr) {
        for (const BeltStack& s : belt->stacks) {
            if (!item.valid() || s.item == item) {
                total += s.count;
            }
        }
    }
    return total;
}

void ExplosiveSystem::dropBelt(Entity pawn) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Belt>(pawn)) {
        return;
    }
    const Belt belt = reg.get<Belt>(pawn);
    reg.remove<Belt>(pawn);
    if (!things_.spawned(pawn)) {
        return;
    }
    const CellPos cell = cellOf(reg.get<Position>(pawn));
    for (const BeltStack& s : belt.stacks) {
        if (s.count > 0) {
            things_.spawn(s.item, cell, s.count);
        }
    }
}

DefId ExplosiveSystem::firstItem(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Belt* belt = reg.valid(pawn) ? reg.try_get<Belt>(pawn) : nullptr;
    if (belt != nullptr) {
        for (const BeltStack& s : belt->stacks) {
            if (s.count > 0) {
                return s.item;
            }
        }
    }
    return DefId{};
}

int ExplosiveSystem::give(Entity pawn, DefId item, int amount) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Posture>(pawn) || !isThrown(item) || amount <= 0) {
        return 0;
    }
    const int room = std::max(0, model_->beltLimit - count(pawn));
    const int n = std::min(room, amount);
    if (n == 0) {
        return 0;
    }
    Belt& belt = reg.get_or_emplace<Belt>(pawn);
    const auto at = std::find_if(belt.stacks.begin(), belt.stacks.end(), [&](const BeltStack& s) { return s.item == item; });
    if (at != belt.stacks.end()) {
        at->count += n;
    } else {
        belt.stacks.push_back(BeltStack{item, n});
    }
    return n;
}

bool ExplosiveSystem::take(Entity pawn, Entity item, std::string* why) {
    entt::registry& reg = things_.registry();
    const auto fail = [why](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    if (!reg.valid(pawn) || !reg.all_of<Posture, Thing>(pawn)) {
        return fail("that is not a pawn");
    }
    if (health_.incapacitated(pawn)) {
        return fail("that pawn is downed or dead");
    }
    if (!reg.valid(item) || !reg.all_of<Thing, StackCount>(item) || !isThrown(reg.get<Thing>(item).def)) {
        return fail("that is not a grenade");
    }
    if (const EquippedBy* by = reg.try_get<EquippedBy>(item); by != nullptr && by->pawn != pawn) {
        return fail("someone else has that");
    }
    const int have = reg.get<StackCount>(item).count;
    const int taken = give(pawn, reg.get<Thing>(item).def, have);
    if (taken == 0) {
        return fail("the belt is full");
    }
    if (taken >= have) {
        things_.destroy(item);
    } else {
        reg.get<StackCount>(item).count = have - taken;
    }
    return true;
}

// ---- Throwing --------------------------------------------------------------------------------

float ExplosiveSystem::releaseHeight(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(pawn);
    const Health* health = reg.try_get<Health>(pawn);
    const Stance stance = health != nullptr && health->downed ? Stance::Prone : reg.get<Posture>(pawn).stance;
    return map_.heightAt(pos.x, pos.y) + std::max(0.3f, eyeHeight(stance) - model_->releaseDrop);
}

bool ExplosiveSystem::throwing(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Thrower* t = reg.valid(pawn) ? reg.try_get<Thrower>(pawn) : nullptr;
    return t != nullptr && t->phase == ThrowPhase::Winding;
}

int ExplosiveSystem::throwReadyIn(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Thrower* t = reg.valid(pawn) ? reg.try_get<Thrower>(pawn) : nullptr;
    if (t == nullptr || t->phase == ThrowPhase::Ready) {
        return 0;
    }
    if (t->phase == ThrowPhase::Cooling) {
        return t->stepsLeft;
    }
    const ExplosiveModel::Item* item = model_->item(t->item);
    return t->stepsLeft + (item != nullptr ? item->cooldown : 0);
}

bool ExplosiveSystem::throwAt(Entity pawn, DefId item, Entity target, float x, float y, bool player, std::string* why) {
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
    if (!item.valid()) {
        item = firstItem(pawn);
    }
    if (!item.valid() || count(pawn, item) <= 0 || !isThrown(item)) {
        return fail("that pawn has no such grenade");
    }
    if (health_.incapacitated(pawn)) {
        return fail("that pawn can't throw (downed or dead)");
    }
    if (reg.all_of<KnockedDown>(pawn)) {
        return fail("that pawn is knocked down");
    }
    if (reg.all_of<Stunned>(pawn)) {
        return fail("that pawn is stunned");
    }
    if (const Shooter* shooter = reg.try_get<Shooter>(pawn);
        shooter != nullptr && (shooter->phase == ShotPhase::Aiming || shooter->phase == ShotPhase::Firing)) {
        return fail("that pawn is shooting");
    }
    if (target != kNoEntity && (target == pawn || !things_.spawned(target))) {
        return fail("the target is not on the map");
    }
    Thrower& t = reg.get_or_emplace<Thrower>(pawn);
    if (t.phase == ThrowPhase::Cooling) {
        return fail("not ready to throw yet");
    }
    t.item = item;
    t.target = target;
    t.aimX = x;
    t.aimY = y;
    if (t.phase == ThrowPhase::Ready) {
        t.phase = ThrowPhase::Winding;
        t.player = player;
        t.stepsLeft = player ? model_->playerWarmup : model_->item(item)->warmup;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::PawnAction;
            e.action = PawnAction::Throw;
            e.thing = pawn;
            e.def = reg.get<Thing>(pawn).def;
            e.cell = cellOf(reg.get<Position>(pawn));
            events_->push_back(std::move(e));
        }
    } else {
        t.player = t.player || player;
    }
    return true;
}

namespace {

// Where a grenade that met `hit` comes to rest: on the ground at that point, beside a wall or rock it
// struck (backed off a quarter metre), or on top of low cover it came down onto.
void explosiveLanding(const Map& map, const Grenade& g, const TraceHit& hit, float& endX, float& endY, float& endZ) {
    endX = hit.x;
    endY = hit.y;
    endZ = hit.z;
    if (hit.kind == ProjectileHitKind::Building || hit.kind == ProjectileHitKind::Rock) {
        const CellPos c{static_cast<int>(std::floor(hit.x)), static_cast<int>(std::floor(hit.y)), g.level};
        const uint8_t obstacle = map.inBounds(c) ? map.obstacleDm(c) : uint8_t{0};
        const bool onTop = hit.kind == ProjectileHitKind::Building && obstacle > 0 && obstacle != kObstacleTallDm &&
                           std::fabs(hit.z - (map.groundHeight(c) + 0.1f * static_cast<float>(obstacle))) < 0.05f;
        if (onTop) {
            return;
        }
        const float len = std::sqrt(g.vx * g.vx + g.vy * g.vy);
        if (len > 1e-6f) {
            endX = hit.x - g.vx / len * 0.25f;
            endY = hit.y - g.vy / len * 0.25f;
        }
    }
    endZ = map.heightAt(endX, endY) + 0.04f;
}

}  // namespace

// Flies a copy of the grenade until it lands. False if it never does within the flight limit.
bool ExplosiveSystem::simulate(Grenade g, float& endX, float& endY, float& endZ, int& steps) const {
    const float gravity = combat_.gravity();
    for (int i = 1; i <= model_->maxFlight; ++i) {
        const float nx = g.x + g.vx;
        const float ny = g.y + g.vy;
        const float nz = g.z + g.vz - 0.5f * gravity;
        const CellPos next{static_cast<int>(std::floor(nx)), static_cast<int>(std::floor(ny)), g.level};
        if (!map_.inBounds(next)) {
            endX = g.x;
            endY = g.y;
            endZ = map_.heightAt(g.x, g.y) + 0.04f;
            steps = i;
            return true;
        }
        const TraceHit hit = combat_.trace(g.x, g.y, g.z, nx, ny, nz, g.level, g.thrower, g.startCell);
        if (hit.kind != ProjectileHitKind::None) {
            explosiveLanding(map_, g, hit, endX, endY, endZ);
            steps = i;
            return true;
        }
        g.x = nx;
        g.y = ny;
        g.z = nz;
        g.vz -= gravity;
    }
    return false;
}

bool ExplosiveSystem::planThrow(Entity pawn, DefId item, float tx, float ty, ThrowPlan& out) const {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(pawn) || !reg.all_of<Posture>(pawn)) {
        return false;
    }
    if (!item.valid()) {
        item = firstItem(pawn);
    }
    const ExplosiveModel::Item* model = model_->item(item);
    if (model == nullptr || count(pawn, item) <= 0) {
        return false;
    }
    const ExplosiveModel::Proj& proj = *model_->proj(model->projectile);
    const Position& pos = reg.get<Position>(pawn);
    Grenade g;
    g.def = model->projectile;
    g.item = item;
    g.thrower = pawn;
    g.x = pos.x;
    g.y = pos.y;
    g.z = releaseHeight(pawn);
    g.level = pos.level;
    g.startCell = cellOf(pos);

    const float dx = tx - pos.x;
    const float dy = ty - pos.y;
    const float flat = std::sqrt(dx * dx + dy * dy);
    const float dirX = flat > 0.01f ? dx / flat : 1.0f;
    const float dirY = flat > 0.01f ? dy / flat : 0.0f;
    const float dz = map_.heightAt(tx, ty) + 0.04f - g.z;
    const float v = proj.speed;
    const float gravity = combat_.gravity();
    const float disc = v * v * v * v - gravity * (gravity * flat * flat + 2.0f * dz * v * v);
    float slopes[2];
    int slopeCount = 0;
    if (flat < 0.3f) {
        slopes[slopeCount++] = 4.0f;  // almost straight up: it comes down at the feet
    } else if (disc >= 0.0f) {
        const float root = std::sqrt(disc);
        slopes[slopeCount++] = (v * v - root) / (gravity * flat);
        slopes[slopeCount++] = (v * v + root) / (gravity * flat);
    } else {
        slopes[slopeCount++] = 1.0f;  // out of reach: as far as it goes
    }

    bool have = false;
    float bestMiss = 0.0f;
    for (int i = 0; i < slopeCount; ++i) {
        const float cosine = 1.0f / std::sqrt(1.0f + slopes[i] * slopes[i]);
        g.vx = dirX * v * cosine;
        g.vy = dirY * v * cosine;
        g.vz = slopes[i] * v * cosine;
        float ex = 0.0f;
        float ey = 0.0f;
        float ez = 0.0f;
        int steps = 0;
        if (!simulate(g, ex, ey, ez, steps)) {
            continue;
        }
        const float miss = explosiveDistance(ex, ey, tx, ty);
        if (!have || miss < bestMiss) {
            have = true;
            bestMiss = miss;
            out.vx = g.vx;
            out.vy = g.vy;
            out.vz = g.vz;
            out.landX = ex;
            out.landY = ey;
            out.flightSteps = steps;
        }
        if (miss <= model_->landingTolerance) {
            break;  // the flatter arc, if it gets there
        }
    }
    out.reaches = have && bestMiss <= model_->landingTolerance && disc >= 0.0f;
    return have;
}

void ExplosiveSystem::release(Entity pawn, Thrower& t, Rng& rng) {
    entt::registry& reg = things_.registry();
    const ExplosiveModel::Item* model = model_->item(t.item);
    if (model == nullptr || count(pawn, t.item) <= 0) {
        t.phase = ThrowPhase::Ready;
        return;
    }
    const Position pos = reg.get<Position>(pawn);
    const int skill = combat_.shootingSkill(pawn);
    float tx = t.aimX;
    float ty = t.aimY;
    ThrowPlan plan;
    if (t.target != kNoEntity && things_.spawned(t.target)) {
        const Position& at = reg.get<Position>(t.target);
        tx = at.x;
        ty = at.y;
        // Lead a pawn that is walking: where it will be when the grenade lands, twice round.
        if (const Mover* mover = reg.try_get<Mover>(t.target); mover != nullptr && !t.player) {
            const float lead = explosiveBySkill(model_->lead, skill);
            for (int pass = 0; pass < 2; ++pass) {
                if (!planThrow(pawn, t.item, tx, ty, plan)) {
                    break;
                }
                tx = at.x + mover->vx * static_cast<float>(plan.flightSteps) * lead;
                ty = at.y + mover->vy * static_cast<float>(plan.flightSteps) * lead;
            }
        }
    }
    // The throw is never exact: a miss radius that grows with distance and shrinks with skill.
    const float distance = explosiveDistance(pos.x, pos.y, tx, ty);
    float missRadius = model->missRadius * std::min(1.0f, distance / model_->missDistance) * explosiveBySkill(model_->missFactor, skill);
    if (t.player) {
        missRadius *= model_->playerMissFactor;
    }
    if (missRadius > 0.0f) {
        for (int tries = 0; tries < 16; ++tries) {
            const float cu = 2.0f * rng.unit() - 1.0f;
            const float cw = 2.0f * rng.unit() - 1.0f;
            if (cu * cu + cw * cw <= 1.0f) {
                tx += cu * missRadius;
                ty += cw * missRadius;
                break;
            }
        }
    }
    // A throw beyond the pawn's range goes as far as the arm does.
    if (distance > model->range && distance > 0.01f) {
        const float scale = model->range / explosiveDistance(pos.x, pos.y, tx, ty);
        if (scale < 1.0f) {
            tx = pos.x + (tx - pos.x) * scale;
            ty = pos.y + (ty - pos.y) * scale;
        }
    }
    if (!planThrow(pawn, t.item, tx, ty, plan)) {
        t.phase = ThrowPhase::Ready;
        return;
    }
    Posture& posture = reg.get<Posture>(pawn);
    const float flat = explosiveDistance(pos.x, pos.y, tx, ty);
    if (flat > 0.01f) {
        posture.facingX = (tx - pos.x) / flat;
        posture.facingY = (ty - pos.y) / flat;
    }

    Grenade g;
    g.def = model->projectile;
    g.item = t.item;
    g.thrower = pawn;
    g.x = pos.x;
    g.y = pos.y;
    g.z = releaseHeight(pawn);
    g.vx = plan.vx;
    g.vy = plan.vy;
    g.vz = plan.vz;
    g.level = pos.level;
    g.startCell = cellOf(pos);
    g.seed = (static_cast<uint64_t>(rng.next()) << 32) | rng.next();
    const Entity grenade = reg.create();
    reg.emplace<Grenade>(grenade, g);

    // One grenade leaves the belt.
    Belt& belt = reg.get<Belt>(pawn);
    for (auto it = belt.stacks.begin(); it != belt.stacks.end(); ++it) {
        if (it->item == t.item) {
            if (--it->count <= 0) {
                belt.stacks.erase(it);
            }
            break;
        }
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::GrenadeThrown;
        e.thing = pawn;
        e.def = t.item;
        e.target = grenade;
        e.x = g.x;
        e.y = g.y;
        e.z = g.z;
        e.amount = static_cast<float>(plan.flightSteps);
        e.cell = g.startCell;
        events_->push_back(std::move(e));
    }
    t.phase = model->cooldown > 0 ? ThrowPhase::Cooling : ThrowPhase::Ready;
    t.stepsLeft = model->cooldown;
}

Entity ExplosiveSystem::place(DefId projectile, float x, float y, int level, Entity thrower, Rng& rng) {
    const ExplosiveModel::Proj* proj = model_->proj(projectile);
    if (proj == nullptr || !map_.inBounds(CellPos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), level})) {
        return kNoEntity;
    }
    entt::registry& reg = things_.registry();
    Grenade g;
    g.def = projectile;
    g.thrower = thrower;
    g.x = x;
    g.y = y;
    g.z = map_.heightAt(x, y) + 0.04f;
    g.level = level;
    g.fuse = proj->fuse;
    g.startCell = CellPos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), level};
    g.seed = (static_cast<uint64_t>(rng.next()) << 32) | rng.next();
    const Entity grenade = reg.create();
    reg.emplace<Grenade>(grenade, g);
    return grenade;
}

// ---- Each step -------------------------------------------------------------------------------

void ExplosiveSystem::flyOrBlow(Entity grenade, Rng& rng) {
    (void)rng;
    entt::registry& reg = things_.registry();
    Grenade& g = reg.get<Grenade>(grenade);
    const ExplosiveModel::Proj& proj = *model_->proj(g.def);
    ++g.age;
    if (g.fuse < 0) {
        const float gravity = combat_.gravity();
        const float nx = g.x + g.vx;
        const float ny = g.y + g.vy;
        const float nz = g.z + g.vz - 0.5f * gravity;
        const CellPos next{static_cast<int>(std::floor(nx)), static_cast<int>(std::floor(ny)), g.level};
        float ex = 0.0f;
        float ey = 0.0f;
        float ez = 0.0f;
        bool landed = false;
        if (!map_.inBounds(next)) {
            ex = g.x;
            ey = g.y;
            ez = map_.heightAt(g.x, g.y) + 0.04f;
            landed = true;
        } else {
            const TraceHit hit = combat_.trace(g.x, g.y, g.z, nx, ny, nz, g.level, g.thrower, g.startCell);
            if (hit.kind != ProjectileHitKind::None) {
                explosiveLanding(map_, g, hit, ex, ey, ez);
                landed = true;
            }
        }
        if (!landed) {
            g.x = nx;
            g.y = ny;
            g.z = nz;
            g.vz -= gravity;
            return;
        }
        g.x = ex;
        g.y = ey;
        g.z = ez;
        g.vx = 0.0f;
        g.vy = 0.0f;
        g.vz = 0.0f;
        g.fuse = proj.fuse;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::GrenadeLanded;
            e.thing = grenade;
            e.def = g.def;
            e.x = g.x;
            e.y = g.y;
            e.z = g.z;
            e.amount = static_cast<float>(g.fuse);
            e.cell = CellPos{static_cast<int>(std::floor(g.x)), static_cast<int>(std::floor(g.y)), g.level};
            events_->push_back(std::move(e));
        }
        return;
    }
    if (--g.fuse > 0) {
        return;
    }
    Explosion ex;
    ex.x = g.x;
    ex.y = g.y;
    ex.z = g.z;
    ex.level = g.level;
    ex.radius = proj.radius;
    ex.damage = proj.damage;
    ex.damageType = proj.damageType;
    ex.penetration = proj.penetration;
    ex.source = grenade;
    ex.instigator = g.thrower;
    ex.seed = g.seed;
    reg.destroy(grenade);  // gone before the blast, so nothing it does can see it
    // The source id in the event stays: Unreal forgets it with the Explosion.
    combat_.explode(ex);
}

void ExplosiveSystem::step(Rng& rng) {
    entt::registry& reg = things_.registry();

    scratch_.clear();
    for (const Entity pawn : reg.view<Thrower>()) {
        scratch_.push_back(pawn);
    }
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        Thrower& t = reg.get<Thrower>(pawn);
        if (t.phase == ThrowPhase::Ready) {
            continue;
        }
        if (t.phase == ThrowPhase::Cooling) {
            if (--t.stepsLeft <= 0) {
                t.phase = ThrowPhase::Ready;
                t.stepsLeft = 0;
            }
            continue;
        }
        // Winding up: a pawn that can't throw drops the grenade back on its belt.
        if (!things_.spawned(pawn) || health_.incapacitated(pawn) || reg.any_of<KnockedDown, Stunned>(pawn)) {
            t.phase = ThrowPhase::Ready;
            t.stepsLeft = 0;
            continue;
        }
        // It faces where it throws.
        {
            const Position& pos = reg.get<Position>(pawn);
            float ax = t.aimX;
            float ay = t.aimY;
            if (t.target != kNoEntity && things_.spawned(t.target)) {
                ax = reg.get<Position>(t.target).x;
                ay = reg.get<Position>(t.target).y;
            }
            const float len = explosiveDistance(pos.x, pos.y, ax, ay);
            if (len > 0.01f) {
                Posture& posture = reg.get<Posture>(pawn);
                posture.facingX = (ax - pos.x) / len;
                posture.facingY = (ay - pos.y) / len;
            }
        }
        if (--t.stepsLeft <= 0) {
            release(pawn, t, rng);
        }
    }

    scratch_.clear();
    for (const Entity grenade : reg.view<Grenade>()) {
        scratch_.push_back(grenade);
    }
    std::sort(scratch_.begin(), scratch_.end());
    const std::vector<Entity> grenades = scratch_;  // blasts reuse scratch_
    for (const Entity grenade : grenades) {
        if (reg.valid(grenade) && reg.all_of<Grenade>(grenade)) {
            flyOrBlow(grenade, rng);
        }
    }
}

void ExplosiveSystem::dangers(std::vector<GrenadeDanger>& out) const {
    const entt::registry& reg = things_.registry();
    out.clear();
    for (const auto [entity, g] : reg.view<const Grenade>().each()) {
        const ExplosiveModel::Proj* proj = model_->proj(g.def);
        if (proj == nullptr) {
            continue;
        }
        GrenadeDanger d;
        d.grenade = entity;
        d.level = g.level;
        d.radius = proj->radius;
        d.age = g.age;
        d.thrower = g.thrower;
        if (g.fuse >= 0) {
            d.x = g.x;
            d.y = g.y;
            d.z = g.z;
            d.stepsToBlast = g.fuse;
        } else {
            int steps = 0;
            if (!simulate(g, d.x, d.y, d.z, steps)) {
                continue;
            }
            d.stepsToBlast = steps + proj->fuse;
        }
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const GrenadeDanger& a, const GrenadeDanger& b) { return a.grenade < b.grenade; });
}

void ExplosiveSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    const bool has[] = {reg.all_of<Belt>(e), reg.all_of<Thrower>(e), reg.all_of<Grenade>(e)};
    uint64_t mask = 0;
    for (size_t bit = 0; bit < sizeof has / sizeof has[0]; ++bit) {
        mask |= has[bit] ? (uint64_t{1} << bit) : 0;
    }
    h.add(mask);
    if (const Belt* b = reg.try_get<Belt>(e)) {
        h.add(static_cast<uint64_t>(b->stacks.size()));
        for (const BeltStack& s : b->stacks) {
            h.add(s.item.value);
            h.add(s.count);
        }
    }
    if (const Thrower* t = reg.try_get<Thrower>(e)) {
        h.add(static_cast<int>(t->phase));
        h.add(t->stepsLeft);
        h.add(t->item.value);
        h.add(entt::to_integral(t->target));
        h.add(t->aimX);
        h.add(t->aimY);
        h.add(t->player);
    }
    if (const Grenade* g = reg.try_get<Grenade>(e)) {
        h.add(g->def.value);
        h.add(g->item.value);
        h.add(entt::to_integral(g->thrower));
        for (const float f : {g->x, g->y, g->z, g->vx, g->vy, g->vz}) {
            h.add(f);
        }
        h.add(g->level);
        h.add(g->age);
        h.add(g->fuse);
        h.add(g->startCell.x);
        h.add(g->startCell.y);
        h.add(g->startCell.level);
        h.add(g->seed);
    }
}

}  // namespace colony

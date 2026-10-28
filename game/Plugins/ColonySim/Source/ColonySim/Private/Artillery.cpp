#include "ColonySim/Artillery.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Power.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace colony {

// Everything read from the defs once: which buildings are artillery, their guns and how their shots fly.
struct ArtilleryModel {
    struct Info {
        bool valid = false;
        DefId gun;
        GunStats stats;
        DefId projectile;
        float speed = 1.0f;        // the projectile's metres per step
        float blastRadius = 0.0f;  // its explosion
        float blastDamage = 0.0f;
        float blastPenetration = 0.0f;
        DefId blastType;
        bool overhead = false;     // shells over everything (a mortar) rather than projectiles in a line
        bool autoTarget = false;   // picks enemies by itself while manned
        bool crewScaled = false;   // the miss radius is times the crew's Mortar Miss Radius Multiplier
        bool spreadCells = false;  // each shot of a volley goes for a cell no other took
        bool ammoPerVolley = false;  // the fuel's `perUse` is taken once a volley, not once a shot
        bool singleVolley = false;   // the aim is used up by one volley
        float missRadius = 0.0f;
        float muzzleHeight = 1.0f;
        float ammo = 0.0f;           // the def's `fuel.perUse`
    };
    std::vector<Info> infos;  // by Things DefId value

    ArtilleryModel(const DefDatabase& defs, const CombatSystem& combat) {
        infos.resize(defs.size());
        for (const DefId id : defs.idsOfKind("Things")) {
            const Def& def = defs.get(id);
            if (!def.has("artillery.gun")) {
                continue;
            }
            Info& a = infos[id.value];
            a.gun = defs.idOf("Things", def.text("artillery.gun"));
            a.overhead = def.flag("artillery.overhead");
            a.autoTarget = def.flag("artillery.autoTarget");
            a.crewScaled = def.flag("artillery.crewScaled");
            a.spreadCells = def.flag("artillery.spreadCells");
            a.ammoPerVolley = def.flag("artillery.ammoPerVolley");
            a.singleVolley = def.flag("artillery.singleVolley");
            a.missRadius = static_cast<float>(def.number("artillery.missRadius"));
            a.muzzleHeight = static_cast<float>(def.number("artillery.muzzleHeight", 1.0));
            a.ammo = def.has("fuel") ? static_cast<float>(def.number("fuel.perUse")) : 0.0f;
            if (!a.gun.valid() || !combat.gunStats(a.gun, a.stats) || a.stats.range <= 0.0f) {
                continue;
            }
            a.projectile = defs.idOf("Projectiles", defs.get(a.gun).text("verbs.0.defaultProjectile"));
            if (!a.projectile.valid()) {
                continue;
            }
            const Def& shot = defs.get(a.projectile);
            a.speed = std::max(0.05f, static_cast<float>(shot.number("projectile.speed", 1.0)));
            a.blastRadius = static_cast<float>(shot.number("explosion.radius"));
            a.blastDamage = static_cast<float>(shot.number("explosion.damage"));
            a.blastPenetration = static_cast<float>(shot.number("explosion.armorPenetration"));
            a.blastType = defs.idOf("DamageTypes", shot.text("explosion.damageDef", "Bomb"));
            a.valid = true;
        }
    }

    const Info* info(DefId def) const {
        return def.valid() && def.value < infos.size() && infos[def.value].valid ? &infos[def.value] : nullptr;
    }
};

ArtilleryNumbers ArtilleryNumbers::read(const DefDatabase& defs) {
    ArtilleryNumbers n;
    if (const Def* rules = defs.find("Rules", "ArtilleryRules")) {
        n.scanEvery = std::max(1, static_cast<int>(rules->integer("scanEvery", n.scanEvery)));
        n.reach = static_cast<float>(rules->number("reach", n.reach));
        n.apexShare = static_cast<float>(rules->number("apexShare", n.apexShare));
        n.spreadTries = std::max(1, static_cast<int>(rules->integer("spreadTries", n.spreadTries)));
        n.missBase = static_cast<float>(rules->number("missBase", n.missBase));
        n.missPerShooting = static_cast<float>(rules->number("missPerShooting", n.missPerShooting));
        n.missManipulation = static_cast<float>(rules->number("missManipulation", n.missManipulation));
    }
    return n;
}

namespace {

float artilleryDistance(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

// Metres from a point to the nearest edge of a rect of cells (0 inside it).
float artilleryDistanceToRect(float x, float y, const CellRect& r) {
    const float dx = std::max({static_cast<float>(r.minX) - x, 0.0f, x - static_cast<float>(r.minX + r.sizeX)});
    const float dy = std::max({static_cast<float>(r.minY) - y, 0.0f, y - static_cast<float>(r.minY + r.sizeY)});
    return std::sqrt(dx * dx + dy * dy);
}

constexpr float kArtilleryPi = 3.14159265358979f;

}  // namespace

void artilleryShellAt(const ArtilleryShell& s, const Map& map, float apexShare, float& x, float& y, float& z, float& vx,
                      float& vy, float& vz) {
    const float steps = static_cast<float>(std::max(1, s.steps));
    const float t = std::clamp(static_cast<float>(s.age) / steps, 0.0f, 1.0f);
    const CellPos to{static_cast<int>(std::floor(s.toX)), static_cast<int>(std::floor(s.toY)), s.level};
    const float toZ = map.inBounds(to) ? map.heightAt(s.toX, s.toY, s.level) : s.fromZ;
    const float apex = apexShare * artilleryDistance(s.fromX, s.fromY, s.toX, s.toY);
    x = s.fromX + (s.toX - s.fromX) * t;
    y = s.fromY + (s.toY - s.fromY) * t;
    z = s.fromZ + (toZ - s.fromZ) * t + 4.0f * apex * t * (1.0f - t);
    vx = (s.toX - s.fromX) / steps;
    vy = (s.toY - s.fromY) / steps;
    vz = ((toZ - s.fromZ) + 4.0f * apex * (1.0f - 2.0f * t)) / steps;
}

ArtillerySystem::ArtillerySystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, PowerGrid& power,
                                 JobSystem& jobs, const DefDatabase& defs, const GameTime& time)
    : things_(things),
      map_(map),
      combat_(combat),
      health_(health),
      power_(power),
      jobs_(jobs),
      time_(time),
      numbers_(ArtilleryNumbers::read(defs)),
      model_(std::make_unique<ArtilleryModel>(defs, combat)) {}

ArtillerySystem::~ArtillerySystem() = default;

bool ArtillerySystem::isArtillery(DefId def) const { return model_->info(def) != nullptr; }

float ArtillerySystem::range(Entity building) const {
    const entt::registry& reg = things_.registry();
    const ArtilleryModel::Info* info = reg.valid(building) && reg.all_of<Thing>(building) ? model_->info(reg.get<Thing>(building).def) : nullptr;
    return info != nullptr ? info->stats.range : 0.0f;
}

float ArtillerySystem::minRange(Entity building) const {
    const entt::registry& reg = things_.registry();
    const ArtilleryModel::Info* info = reg.valid(building) && reg.all_of<Thing>(building) ? model_->info(reg.get<Thing>(building).def) : nullptr;
    return info != nullptr ? info->stats.minRange : 0.0f;
}

float ArtillerySystem::missFactor(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing>(pawn)) {
        return 1.0f;
    }
    const float shooting = static_cast<float>(jobs_.skillLevel(pawn, "Shooting"));
    const float sight = std::min(1.0f, health_.capacityValue(pawn, "Sight"));
    const float manipulation = health_.capacityValue(pawn, "Manipulation");
    const float f = 1.0f + numbers_.missBase - numbers_.missPerShooting * shooting + (1.0f - sight) -
                    numbers_.missManipulation * (manipulation - 1.0f);
    return std::max(0.0f, f);
}

float ArtillerySystem::missRadius(Entity building, Entity crew) const {
    const entt::registry& reg = things_.registry();
    const ArtilleryModel::Info* info = reg.valid(building) && reg.all_of<Thing>(building) ? model_->info(reg.get<Thing>(building).def) : nullptr;
    if (info == nullptr) {
        return 0.0f;
    }
    return info->crewScaled && crew != kNoEntity ? info->missRadius * missFactor(crew) : info->missRadius;
}

// ---- Commands --------------------------------------------------------------------------------

bool ArtillerySystem::checkBuilding(Entity building, std::string& error) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(building) || !reg.all_of<Artillery, Thing, Position>(building) || model_->info(reg.get<Thing>(building).def) == nullptr) {
        error = "that isn't a mortar or rocket launcher";
        return false;
    }
    return true;
}

bool ArtillerySystem::man(Entity building, Entity pawn, std::string& error) {
    if (!checkBuilding(building, error)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing, Position, Worker>(pawn) || reg.all_of<Animal>(pawn) || reg.all_of<Prisoner>(pawn)) {
        error = "only a colonist can man it";
        return false;
    }
    if (const Faction* f = reg.try_get<Faction>(pawn); f != nullptr && f->id != kPlayerFaction) {
        error = "only a colonist can man it";
        return false;
    }
    if (health_.incapacitated(pawn)) {
        error = "the pawn can't (downed or dead)";
        return false;
    }
    if (const Colonist* c = reg.try_get<Colonist>(pawn);
        c != nullptr && std::binary_search(c->disabledWorkTags.begin(), c->disabledWorkTags.end(), std::string("Violent"))) {
        error = "the pawn is incapable of violence";
        return false;
    }
    if (reg.all_of<Fighter>(pawn) && !reg.all_of<PlayerControl>(pawn)) {
        error = "the pawn is fighting: undraft it first";
        return false;
    }
    Artillery& a = reg.get<Artillery>(building);
    const Position& bp = reg.get<Position>(building);
    const Position& pp = reg.get<Position>(pawn);
    if (reg.all_of<PlayerControl>(pawn)) {
        if (pp.level != bp.level ||
            artilleryDistanceToRect(pp.x, pp.y, reg.get<Footprint>(building).rect()) > numbers_.reach) {
            error = "too far: step up to it first";
            return false;
        }
    } else if (!jobs_.orderMan(pawn, building, error)) {  // undrafts the pawn and walks it to the gun
        return false;
    }
    // Someone else at a post on another gun leaves that one (it can man one at a time).
    for (const auto [other, oa] : reg.view<Artillery>().each()) {
        if (other != building && oa.crew == pawn) {
            oa.crew = kNoEntity;
        }
    }
    if (a.crew != kNoEntity && a.crew != pawn && reg.valid(a.crew)) {
        const Worker* w = reg.try_get<Worker>(a.crew);
        if (w != nullptr && w->job.type == JobType::ManArtillery && w->job.site == building) {
            jobs_.interrupt(a.crew, "another colonist took the gun");
        }
    }
    a.crew = pawn;
    return true;
}

bool ArtillerySystem::stopManning(Entity building, std::string& error) {
    if (!checkBuilding(building, error)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    Artillery& a = reg.get<Artillery>(building);
    if (a.crew == kNoEntity) {
        error = "nobody mans it";
        return false;
    }
    if (reg.valid(a.crew)) {
        const Worker* w = reg.try_get<Worker>(a.crew);
        if (w != nullptr && w->job.type == JobType::ManArtillery && w->job.site == building) {
            jobs_.interrupt(a.crew, "ordered off the gun");
        }
    }
    a.crew = kNoEntity;
    if (a.phase == TurretPhase::Warming || a.phase == TurretPhase::Firing) {
        a.phase = TurretPhase::Idle;
        a.stepsLeft = 0;
        a.target = kNoEntity;
    }
    return true;
}

// A point it may shoot at: on the map, within its ranges, and for a mortar not under a mountain's roof.
bool ArtillerySystem::targetAllowed(Entity building, float x, float y, std::string* why) const {
    const entt::registry& reg = things_.registry();
    const ArtilleryModel::Info* info = model_->info(reg.get<Thing>(building).def);
    const Position& pos = reg.get<Position>(building);
    const CellPos cell{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), pos.level};
    if (!map_.inBounds(cell)) {
        if (why != nullptr) {
            *why = "that is off the map";
        }
        return false;
    }
    const float d = artilleryDistance(pos.x, pos.y, x, y);
    if (d > info->stats.range) {
        if (why != nullptr) {
            *why = "out of range";
        }
        return false;
    }
    if (d < info->stats.minRange) {
        if (why != nullptr) {
            *why = "too close: inside its minimum range";
        }
        return false;
    }
    if (info->overhead && map_.roof(cell) == Roof::Thick) {
        if (why != nullptr) {
            *why = "it is under a mountain's roof";
        }
        return false;
    }
    return true;
}

bool ArtillerySystem::aim(Entity building, Entity target, float x, float y, std::string& error) {
    if (!checkBuilding(building, error)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    if (target != kNoEntity) {
        const Health* h = reg.valid(target) ? reg.try_get<Health>(target) : nullptr;
        if (!reg.valid(target) || !reg.all_of<Thing, Position, Posture>(target) || (h != nullptr && h->dead) ||
            reg.get<Position>(target).level != reg.get<Position>(building).level) {
            error = "that isn't a living pawn on the map";
            return false;
        }
        x = reg.get<Position>(target).x;
        y = reg.get<Position>(target).y;
    }
    if (!targetAllowed(building, x, y, &error)) {
        return false;
    }
    Artillery& a = reg.get<Artillery>(building);
    a.hasAim = true;
    a.aimX = x;
    a.aimY = y;
    a.aimPawn = target;
    if (a.phase == TurretPhase::Warming) {  // a new aim while winding up: it aims there instead
        a.target = target;
        a.volleyX = x;
        a.volleyY = y;
    }
    return true;
}

bool ArtillerySystem::clearAim(Entity building, std::string& error) {
    if (!checkBuilding(building, error)) {
        return false;
    }
    Artillery& a = things_.registry().get<Artillery>(building);
    a.hasAim = false;
    a.aimPawn = kNoEntity;
    return true;
}

bool ArtillerySystem::setHoldFire(Entity building, bool hold, std::string& error) {
    if (!checkBuilding(building, error)) {
        return false;
    }
    things_.registry().get<Artillery>(building).holdFire = hold;
    return true;
}

// ---- Crew ------------------------------------------------------------------------------------

// A crew of another faction: a siege's raider, whom the raid system walks to the gun.
bool ArtillerySystem::foreignCrew(Entity crew) const {
    const Faction* f = things_.registry().try_get<Faction>(crew);
    return f != nullptr && f->id != kPlayerFaction;
}

bool ArtillerySystem::setForeignCrew(Entity building, Entity pawn) {
    std::string error;
    if (!checkBuilding(building, error)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    if (pawn != kNoEntity && (!reg.valid(pawn) || !reg.all_of<Position>(pawn) || !foreignCrew(pawn))) {
        return false;
    }
    Artillery& a = reg.get<Artillery>(building);
    a.crew = pawn;
    if (pawn == kNoEntity && (a.phase == TurretPhase::Warming || a.phase == TurretPhase::Firing)) {
        a.phase = TurretPhase::Idle;
        a.stepsLeft = 0;
        a.target = kNoEntity;
    }
    return true;
}

// The crew is at its post: the player's colonist within reach, or a colonist whose ManArtillery job stands it beside
// the gun.
bool ArtillerySystem::atPost(Entity building, const Artillery& a) const {
    const entt::registry& reg = things_.registry();
    if (a.crew == kNoEntity || !reg.valid(a.crew) || !reg.all_of<Position>(a.crew) || health_.incapacitated(a.crew)) {
        return false;
    }
    const Position& pp = reg.get<Position>(a.crew);
    const Position& bp = reg.get<Position>(building);
    if (pp.level != bp.level) {
        return false;
    }
    if (reg.all_of<PlayerControl>(a.crew) || foreignCrew(a.crew)) {
        return artilleryDistanceToRect(pp.x, pp.y, reg.get<Footprint>(building).rect()) <= numbers_.reach;
    }
    const Worker* w = reg.try_get<Worker>(a.crew);
    return w != nullptr && w->job.type == JobType::ManArtillery && w->job.site == building && w->job.step >= kManArtilleryPostStep &&
           cellOf(pp) == w->job.cell;
}

// The crew is no more: dead, downed, gone, its job ended, or the player's colonist walked off.
bool ArtillerySystem::crewGone(Entity building, const Artillery& a) const {
    const entt::registry& reg = things_.registry();
    if (a.crew == kNoEntity) {
        return false;
    }
    if (!reg.valid(a.crew) || !reg.all_of<Position>(a.crew) || health_.incapacitated(a.crew)) {
        return true;
    }
    if (reg.all_of<PlayerControl>(a.crew)) {
        return !atPost(building, a);
    }
    if (foreignCrew(a.crew)) {
        return false;  // the raid system says when a siege's crew leaves
    }
    const Worker* w = reg.try_get<Worker>(a.crew);
    return w == nullptr || w->job.type != JobType::ManArtillery || w->job.site != building;
}

bool ArtillerySystem::manned(Entity building) const {
    const entt::registry& reg = things_.registry();
    const Artillery* a = reg.valid(building) ? reg.try_get<Artillery>(building) : nullptr;
    return a != nullptr && reg.all_of<Position>(building) && atPost(building, *a);
}

// RimWorld: a gun whose shells fly overhead can't fire from under any roof.
bool ArtillerySystem::coveredFromAbove(Entity building) const {
    const CellRect r = things_.registry().get<Footprint>(building).rect();
    for (int y = r.minY; y < r.minY + r.sizeY; ++y) {
        for (int x = r.minX; x < r.minX + r.sizeX; ++x) {
            const CellPos c{x, y, r.level};
            if (map_.inBounds(c) && map_.roof(c) != Roof::None) {
                return true;
            }
        }
    }
    return false;
}

bool ArtillerySystem::hasAmmo(Entity building, float amount) const {
    return amount <= 0.0f || fuel_ == nullptr || fuel_->canUse(building, amount);
}

// Where the player's aim is now: the picked pawn while it lives on the map (it is followed), else the point.
bool ArtillerySystem::aimPoint(Entity building, Artillery& a, float& x, float& y) const {
    const entt::registry& reg = things_.registry();
    if (a.aimPawn != kNoEntity) {
        const Health* h = reg.valid(a.aimPawn) ? reg.try_get<Health>(a.aimPawn) : nullptr;
        if (!reg.valid(a.aimPawn) || !reg.all_of<Position>(a.aimPawn) || (h != nullptr && h->dead) ||
            reg.get<Position>(a.aimPawn).level != reg.get<Position>(building).level) {
            a.hasAim = false;  // the pawn is dead or gone: the aim is over
            a.aimPawn = kNoEntity;
            return false;
        }
        a.aimX = reg.get<Position>(a.aimPawn).x;
        a.aimY = reg.get<Position>(a.aimPawn).y;
    }
    x = a.aimX;
    y = a.aimY;
    return true;
}

// The nearest able enemy within its ranges, anywhere (no line of sight: the shells fly over), not under a mountain. A
// siege's raider crew goes for the colony's pawns; the colony's for any other faction's.
Entity ArtillerySystem::pickTarget(Entity building) const {
    const entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(building);
    const Artillery* gun = reg.try_get<Artillery>(building);
    const bool raiders = gun != nullptr && gun->crew != kNoEntity && reg.valid(gun->crew) && foreignCrew(gun->crew);
    Entity best = kNoEntity;
    float bestDistance = 0.0f;
    for (const auto [e, thing, p, posture] : reg.view<const Thing, const Position, const Posture>().each()) {
        (void)posture;
        if (thing.category != ThingCategory::Pawn || p.level != pos.level) {
            continue;
        }
        const Faction* f = reg.try_get<Faction>(e);
        const bool colonys = f != nullptr ? f->id == kPlayerFaction : reg.all_of<Colonist>(e);  // a colonist may have no Faction
        if ((raiders ? !colonys : (f == nullptr || colonys)) || health_.incapacitated(e) ||
            (!raiders && reg.all_of<Animal>(e)) || reg.all_of<Prisoner>(e) || reg.all_of<Visitor>(e)) {
            continue;
        }
        const float d = artilleryDistance(pos.x, pos.y, p.x, p.y);
        if (best != kNoEntity && (d > bestDistance || (d == bestDistance && e > best))) {
            continue;
        }
        if (!targetAllowed(building, p.x, p.y, nullptr)) {
            continue;
        }
        best = e;
        bestDistance = d;
    }
    return best;
}

// ---- Firing ----------------------------------------------------------------------------------

void ArtillerySystem::fire(Entity building, Artillery& a, float x, float y, Rng& rng) {
    entt::registry& reg = things_.registry();
    const ArtilleryModel::Info& info = *model_->info(reg.get<Thing>(building).def);
    const Position pos = reg.get<Position>(building);
    // Where it lands: anywhere within the miss radius, evenly (RimWorld picks a cell within it).
    const float radius = missRadius(building, a.crew);
    float tx = x;
    float ty = y;
    for (int tries = 0; tries < (info.spreadCells ? numbers_.spreadTries : 1); ++tries) {
        const float r = radius * std::sqrt(rng.unit());
        const float angle = 2.0f * kArtilleryPi * rng.unit();
        tx = std::clamp(x + r * std::cos(angle), 0.01f, static_cast<float>(map_.width()) - 0.01f);
        ty = std::clamp(y + r * std::sin(angle), 0.01f, static_cast<float>(map_.height()) - 0.01f);
        const CellPos cell{static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)), pos.level};
        if (!info.spreadCells || std::find(a.hitCells.begin(), a.hitCells.end(), cell) == a.hitCells.end()) {
            break;
        }
    }
    if (info.spreadCells) {
        a.hitCells.push_back(CellPos{static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)), pos.level});
    }
    const float ox = pos.x;
    const float oy = pos.y;
    const float oz = map_.heightAt(pos.x, pos.y, pos.level) + info.muzzleHeight;
    ++shots_;
    if (!info.overhead) {
        // A rocket: an ordinary projectile, straight at the ground there; it bursts on whatever it meets first.
        combat_.fireFrom(building, info.gun, ox, oy, oz, pos.level, cellOf(pos), tx, ty, map_.heightAt(tx, ty, pos.level), 0.0f, rng);
        return;
    }
    const Entity shell = reg.create();
    ArtilleryShell s;
    s.def = info.projectile;
    s.gun = info.gun;
    s.launcher = building;
    s.fromX = ox;
    s.fromY = oy;
    s.fromZ = oz;
    s.toX = tx;
    s.toY = ty;
    s.level = pos.level;
    s.steps = std::max(1, static_cast<int>(std::ceil(artilleryDistance(ox, oy, tx, ty) / info.speed)));
    s.seed = (static_cast<uint64_t>(rng.next()) << 32) | rng.next();
    reg.emplace<ArtilleryShell>(shell, s);
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::ShotFired;
        e.thing = building;
        e.def = info.gun;
        e.target = shell;
        e.x = ox;
        e.y = oy;
        e.z = oz;
        events_->push_back(std::move(e));
    }
}

void ArtillerySystem::stepBuilding(Entity building, Artillery& a, Rng& rng) {
    entt::registry& reg = things_.registry();
    const ArtilleryModel::Info* info = model_->info(reg.get<Thing>(building).def);
    if (info == nullptr || reg.all_of<ConstructionSite>(building)) {
        return;
    }
    if (crewGone(building, a)) {
        a.crew = kNoEntity;
    }
    const GunStats& gun = info->stats;
    const auto stand = [&]() {  // stops a volley that has not started or is under way; the cooldown stays
        if (a.phase == TurretPhase::Warming || a.phase == TurretPhase::Firing) {
            a.phase = TurretPhase::Idle;
            a.stepsLeft = 0;
            a.shotsLeft = 0;
            a.target = kNoEntity;
            a.hitCells.clear();
        }
    };
    if (!power_.running(building) || !atPost(building, a)) {
        stand();  // warm-up and cooldown count only while it is manned
        return;
    }
    if (a.phase == TurretPhase::Cooling) {
        if (--a.stepsLeft <= 0) {
            a.phase = TurretPhase::Idle;
            a.stepsLeft = 0;
        }
        return;
    }
    if (a.holdFire || (info->overhead && coveredFromAbove(building))) {
        stand();
        return;
    }
    const float perShot = info->ammoPerVolley ? 0.0f : info->ammo;
    const float perVolley = info->ammoPerVolley ? info->ammo : 0.0f;
    if (a.phase == TurretPhase::Idle) {
        if (!hasAmmo(building, perShot + perVolley)) {
            return;
        }
        float x = 0.0f;
        float y = 0.0f;
        Entity target = kNoEntity;
        if (a.hasAim) {
            if (!aimPoint(building, a, x, y) || !targetAllowed(building, x, y, nullptr)) {
                return;  // the picked pawn is out of range for now (or gone)
            }
            target = a.aimPawn;
        } else if (info->autoTarget) {
            if ((time_.seconds + static_cast<int64_t>(entt::to_integral(building))) % numbers_.scanEvery != 0) {
                return;
            }
            target = pickTarget(building);
            if (target == kNoEntity) {
                return;
            }
            x = reg.get<Position>(target).x;
            y = reg.get<Position>(target).y;
        } else {
            return;
        }
        a.target = target;
        a.volleyX = x;
        a.volleyY = y;
        a.phase = TurretPhase::Warming;
        a.stepsLeft = gun.warmup;
        a.shotsLeft = gun.burst;
        a.hitCells.clear();
        if (a.stepsLeft > 0) {
            return;
        }
    }
    if (a.phase == TurretPhase::Warming) {
        if (--a.stepsLeft > 0) {
            return;
        }
        if (!hasAmmo(building, perShot + perVolley)) {
            stand();
            return;
        }
        if (perVolley > 0.0f && fuel_ != nullptr) {
            fuel_->use(building, perVolley);
        }
        a.phase = TurretPhase::Firing;
        a.stepsLeft = 0;
    }
    // Firing.
    if (a.stepsLeft > 0) {
        --a.stepsLeft;
        return;
    }
    if (!hasAmmo(building, perShot)) {
        stand();
        return;
    }
    // A pawn target is followed while it lives on the map; after that the volley goes on at its last point.
    if (a.target != kNoEntity) {
        const Health* h = reg.valid(a.target) ? reg.try_get<Health>(a.target) : nullptr;
        if (reg.valid(a.target) && reg.all_of<Position>(a.target) && (h == nullptr || !h->dead) &&
            targetAllowed(building, reg.get<Position>(a.target).x, reg.get<Position>(a.target).y, nullptr)) {
            a.volleyX = reg.get<Position>(a.target).x;
            a.volleyY = reg.get<Position>(a.target).y;
        } else {
            a.target = kNoEntity;
        }
    }
    fire(building, a, a.volleyX, a.volleyY, rng);
    if (perShot > 0.0f && fuel_ != nullptr) {
        fuel_->use(building, perShot);
    }
    if (--a.shotsLeft > 0) {
        a.stepsLeft = std::max(0, gun.burstGap - 1);
        return;
    }
    a.phase = TurretPhase::Cooling;
    a.stepsLeft = gun.cooldown;
    a.target = kNoEntity;
    a.hitCells.clear();
    if (info->singleVolley) {
        a.hasAim = false;
        a.aimPawn = kNoEntity;
    }
}

// Shells fly their arc and land: a mountain's thick roof takes them, anywhere else they burst on the ground (a thin
// roof doesn't stop them).
void ArtillerySystem::stepShells() {
    entt::registry& reg = things_.registry();
    order_.clear();
    for (const Entity e : reg.view<ArtilleryShell>()) {
        order_.push_back(e);
    }
    std::sort(order_.begin(), order_.end());
    for (const Entity shell : order_) {
        ArtilleryShell& s = reg.get<ArtilleryShell>(shell);
        if (++s.age < s.steps) {
            continue;
        }
        const ArtilleryShell landed = s;
        reg.destroy(shell);
        ++landed_;
        const CellPos cell{static_cast<int>(std::floor(landed.toX)), static_cast<int>(std::floor(landed.toY)), landed.level};
        const float z = map_.inBounds(cell) ? map_.heightAt(landed.toX, landed.toY, landed.level) : 0.0f;
        const bool stopped = !map_.inBounds(cell) || map_.roof(cell) == Roof::Thick;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::ProjectileHit;
            e.thing = shell;
            e.def = landed.def;
            e.hit = !map_.inBounds(cell) ? ProjectileHitKind::Gone : stopped ? ProjectileHitKind::Rock : ProjectileHitKind::Ground;
            e.x = landed.toX;
            e.y = landed.toY;
            e.z = z;
            e.cell = cell;
            events_->push_back(std::move(e));
        }
        if (stopped) {
            continue;
        }
        const ArtilleryModel::Info* info = nullptr;
        Explosion ex;
        ex.x = landed.toX;
        ex.y = landed.toY;
        ex.z = z;
        ex.level = landed.level;
        // The blast is the projectile's: look it up by the shell's own def, whatever became of the mortar.
        for (const ArtilleryModel::Info& i : model_->infos) {
            if (i.valid && i.projectile == landed.def) {
                info = &i;
                break;
            }
        }
        if (info == nullptr) {
            continue;
        }
        ex.radius = info->blastRadius;
        ex.damage = info->blastDamage;
        ex.penetration = info->blastPenetration;
        ex.damageType = info->blastType;
        ex.source = shell;
        ex.instigator = landed.launcher;
        ex.seed = landed.seed;
        combat_.explode(ex);
    }
}

void ArtillerySystem::step(Rng& rng) {
    entt::registry& reg = things_.registry();
    if (!reg.view<ArtilleryShell>().empty()) {
        stepShells();
    }
    if (reg.view<Artillery>().empty()) {
        return;
    }
    order_.clear();
    for (const Entity e : reg.view<Artillery>()) {
        order_.push_back(e);
    }
    std::sort(order_.begin(), order_.end());
    const std::vector<Entity> buildings = order_;
    for (const Entity building : buildings) {
        if (reg.valid(building) && reg.all_of<Artillery, Thing, Position, Footprint>(building)) {
            stepBuilding(building, reg.get<Artillery>(building), rng);
        }
    }
}

void ArtillerySystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const Artillery* a = reg.try_get<Artillery>(e)) {
        h.add(uint64_t{0x6172});
        h.add(static_cast<int>(a->phase));
        h.add(a->stepsLeft);
        h.add(a->shotsLeft);
        h.add(entt::to_integral(a->crew));
        h.add(a->holdFire);
        h.add(a->hasAim);
        h.add(a->aimX);
        h.add(a->aimY);
        h.add(entt::to_integral(a->aimPawn));
        h.add(entt::to_integral(a->target));
        h.add(a->volleyX);
        h.add(a->volleyY);
        h.add(static_cast<uint64_t>(a->hitCells.size()));
        for (const CellPos& c : a->hitCells) {
            h.add(c.x);
            h.add(c.y);
            h.add(c.level);
        }
    }
    if (const ArtilleryShell* s = reg.try_get<ArtilleryShell>(e)) {
        h.add(uint64_t{0x7368});
        h.add(s->def.value);
        h.add(entt::to_integral(s->launcher));
        h.add(s->toX);
        h.add(s->toY);
        h.add(s->level);
        h.add(s->steps);
        h.add(s->age);
        h.add(s->seed);
    }
}

}  // namespace colony

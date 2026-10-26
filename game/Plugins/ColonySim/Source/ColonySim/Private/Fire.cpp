// Fire (task E4, docs/design/fire.md).
#include "ColonySim/Fire.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// Cells a fire can reach from one cell: the eight round it, and the twelve more at a knight's move or two straight
// (the wiki: "2 straight, or 1 diagonal plus 1 straight").
constexpr int kFireOffsets[20][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
                                     {-2, 0},  {2, 0},  {0, -2}, {0, 2},  {-2, -1}, {-2, 1}, {2, -1}, {2, 1},
                                     {-1, -2}, {1, -2}, {-1, 2}, {1, 2}};

int fireRoundRandom(float x, Rng& rng) {
    int n = static_cast<int>(std::floor(x));
    if (rng.unit() < x - static_cast<float>(n)) {
        ++n;
    }
    return n;
}

float fireNum(const Def& d, const char* key, float fallback) {
    return static_cast<float>(d.number(key, static_cast<double>(fallback)));
}

}  // namespace

FireNumbers FireNumbers::read(const DefDatabase& defs) {
    FireNumbers n;
    if (const Def* d = defs.find("Rules", "FireRules")) {
        n.ticksToSeconds = d->number("ticksToSeconds", n.ticksToSeconds);
        n.passSeconds = std::max(1, static_cast<int>(d->number("passSeconds", n.passSeconds)));
        n.startSize = fireNum(*d, "startSize", n.startSize);
        n.maxSize = fireNum(*d, "maxSize", n.maxSize);
        n.growthPerTick = fireNum(*d, "growthPerTick", n.growthPerTick);
        n.shrinkPerTick = fireNum(*d, "shrinkPerTick", n.shrinkPerTick);
        n.damageBase = fireNum(*d, "damageBase", n.damageBase);
        n.damagePerSize = fireNum(*d, "damagePerSize", n.damagePerSize);
        n.damageMax = fireNum(*d, "damageMax", n.damageMax);
        n.plantDamageFactor = fireNum(*d, "plantDamageFactor", n.plantDamageFactor);
        n.spreadPerTick = fireNum(*d, "spreadPerTick", n.spreadPerTick);
        n.emberFactor = fireNum(*d, "emberFactor", n.emberFactor);
        n.emberMinSize = fireNum(*d, "emberMinSize", n.emberMinSize);
        n.catchPerTick = fireNum(*d, "catchPerTick", n.catchPerTick);
        n.pawnFlammability = fireNum(*d, "pawnFlammability", n.pawnFlammability);
        n.pawnDamagePerTick = fireNum(*d, "pawnDamagePerTick", n.pawnDamagePerTick);
        n.pawnBurnTicks = static_cast<int>(d->number("pawnBurnTicks", n.pawnBurnTicks));
        n.rainOutPerTick = fireNum(*d, "rainOutPerTick", n.rainOutPerTick);
        n.snowBlocksAt = fireNum(*d, "snowBlocksAt", n.snowBlocksAt);
        n.autoIgniteC = fireNum(*d, "autoIgniteC", n.autoIgniteC);
        n.hotRoomIgniteChance = fireNum(*d, "hotRoomIgniteChance", n.hotRoomIgniteChance);
        n.hotPawnIgniteChance = fireNum(*d, "hotPawnIgniteChance", n.hotPawnIgniteChance);
        n.blastIgniteChance = fireNum(*d, "blastIgniteChance", n.blastIgniteChance);
        n.heatPerSecond = fireNum(*d, "heatPerSecond", n.heatPerSecond);
        n.heatMaxC = fireNum(*d, "heatMaxC", n.heatMaxC);
        n.beatPerTick = fireNum(*d, "beatPerTick", n.beatPerTick);
        n.floorBurnPasses = static_cast<int>(d->number("floorBurnPasses", n.floorBurnPasses));
        n.burntFadeDays = fireNum(*d, "burntFadeDays", n.burntFadeDays);
        n.autoHomeRadius = static_cast<int>(d->number("autoHomeRadius", n.autoHomeRadius));
        n.startHomeRadius = static_cast<int>(d->number("startHomeRadius", n.startHomeRadius));
    }
    return n;
}

Fire::Fire(Map& map, Things& things, Plants& plants, Floors& floors, Filth& filth, Rooms& rooms, TemperatureSystem& temperature,
           HealthSystem& health, Weather& weather, const DefDatabase& defs, uint64_t seed)
    : map_(map),
      things_(things),
      plants_(plants),
      floors_(floors),
      filth_(filth),
      rooms_(rooms),
      temperature_(temperature),
      health_(health),
      weather_(weather),
      defs_(defs),
      seed_(splitMix64(seed ^ 0x6669726531ULL)),
      numbers_(FireNumbers::read(defs)) {
    flammability_.assign(defs.size(), 0.0f);
    stuffFactor_.assign(defs.size(), 1.0f);
    maxHp_.assign(defs.size(), 0.0f);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& d = defs.get(id);
        flammability_[id.value] = fireNum(d, "stats.flammability", 0.0f);
        stuffFactor_[id.value] = fireNum(d, "stuffProps.statFactors.flammability", 1.0f);
        maxHp_[id.value] = fireNum(d, "stats.maxHitPoints", 0.0f);
    }
    for (const DefId id : defs.idsOfKind("Terrain")) {
        const Def& d = defs.get(id);
        if (id.value < flammability_.size()) {
            flammability_[id.value] = fireNum(d, "stats.flammability", 0.0f);
        }
    }
    burnType_ = defs.idOf("DamageTypes", "Burn");
    home_.assign(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), 0);
}

Fire::~Fire() = default;

uint64_t Fire::rngSeed(int64_t now, int index, uint64_t what) const {
    return splitMix64(seed_ ^ (static_cast<uint64_t>(index) * 0x9E3779B97F4A7C15ULL) ^ (static_cast<uint64_t>(now) << 20) ^
                      (what * 0xD6E8FEB86659FD93ULL));
}

void Fire::emit(EventType type, Entity thing, CellPos cell, DefId def, float amount, const char* message) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.thing = thing;
    e.cell = cell;
    e.def = def;
    e.amount = amount;
    e.x = static_cast<float>(cell.x) + 0.5f;
    e.y = static_cast<float>(cell.y) + 0.5f;
    if (message != nullptr) {
        e.message = message;
    }
    events_->push_back(std::move(e));
}

// ---- Questions ------------------------------------------------------------------------------

bool Fire::burning(CellPos cell) const {
    return map_.inBounds(cell) && fires_.count(map_.index(cell)) != 0;
}

float Fire::sizeAt(CellPos cell) const {
    if (!map_.inBounds(cell)) {
        return 0.0f;
    }
    const auto it = fires_.find(map_.index(cell));
    return it == fires_.end() ? 0.0f : it->second.size;
}

bool Fire::burnt(CellPos cell) const {
    return map_.inBounds(cell) && burnt_.count(map_.index(cell)) != 0;
}

bool Fire::pawnBurning(Entity pawn) const {
    return things_.registry().valid(pawn) && things_.registry().all_of<Burning>(pawn);
}

size_t Fire::burningPawns() const {
    return things_.registry().view<Burning>().size();
}

float Fire::defFlammability(DefId def) const {
    return def.value < flammability_.size() ? flammability_[def.value] : 0.0f;
}

float Fire::thingFlammability(Entity thing) const {
    const entt::registry& reg = things_.registry();
    const Thing* t = reg.try_get<Thing>(thing);
    if (t == nullptr) {
        return 0.0f;
    }
    float f = defFlammability(t->def);
    if (const BuiltOf* built = reg.try_get<BuiltOf>(thing); built != nullptr && built->stuff.valid() && built->stuff.value < stuffFactor_.size()) {
        f *= stuffFactor_[built->stuff.value];
    }
    return f;
}

float Fire::flammabilityAt(CellPos c) const {
    if (!map_.inBounds(c)) {
        return 0.0f;
    }
    float f = 0.0f;
    const Entity building = map_.buildingAt(c);
    if (building != kNoEntity) {
        f = std::max(f, thingFlammability(building));
    }
    for (const Entity item : things_.itemsAt(c)) {
        f = std::max(f, thingFlammability(item));
    }
    if (plants_.has(c)) {
        f = std::max(f, defFlammability(plants_.defAt(c)));
    }
    const DefId floor = floors_.at(c);
    if (floor.valid()) {
        f = std::max(f, defFlammability(floor));
    }
    return f;
}

float Fire::pawnFlammability(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Thing* t = reg.try_get<Thing>(pawn);
    if (t == nullptr) {
        return 0.0f;
    }
    float f = defs_.get(t->def).has("stats.flammability") ? defFlammability(t->def) : numbers_.pawnFlammability;
    if (pawnFactor_) {
        f *= pawnFactor_(pawn);
    }
    return f;
}

bool Fire::canBurnGround(CellPos c) const {
    if (!map_.inBounds(c)) {
        return false;
    }
    const Terrain t = map_.terrain(c);
    return t != Terrain::Water && t != Terrain::Rock;
}

// Rain or deep snow on a cell under the open sky: nothing catches there.
bool Fire::wetAt(CellPos c) const {
    if (!weather_.outdoors(c)) {
        return false;
    }
    if (weather_.snowDepth(c) >= numbers_.snowBlocksAt) {
        return true;
    }
    const WeatherEffects effects = weather_.effects();
    return effects.extinguishesFire && effects.rainRate > 0.0f;
}

// ---- Starting -------------------------------------------------------------------------------

bool Fire::ignite(CellPos cell, float size, bool force) {
    if (!canBurnGround(cell)) {
        return false;
    }
    const int index = map_.index(cell);
    if (fires_.count(index) != 0) {
        return false;
    }
    if (!force && (wetAt(cell) || !(flammabilityAt(cell) > 0.0f))) {
        return false;
    }
    FireCell fire;
    fire.size = std::clamp(size > 0.0f ? size : numbers_.startSize, 0.01f, numbers_.maxSize);
    fires_[index] = fire;
    ++started_;
    ++changes_;
    emit(EventType::FireStarted, kNoEntity, cell, DefId{}, fire.size);
    return true;
}

bool Fire::igniteRadius(float x, float y, int level, float radius, float chance, int64_t now) {
    bool any = false;
    const int r = static_cast<int>(std::ceil(radius));
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const CellPos c{cx + dx, cy + dy, level};
            if (!map_.inBounds(c)) {
                continue;
            }
            const float ex = static_cast<float>(c.x) + 0.5f - x;
            const float ey = static_cast<float>(c.y) + 0.5f - y;
            if (ex * ex + ey * ey > radius * radius) {
                continue;
            }
            Rng rng(rngSeed(now, map_.index(c), 5), 5);
            if (rng.unit() < chance && ignite(c)) {
                any = true;
            }
        }
    }
    // The cell the blast went off in always burns a moment, even on bare ground (the fuel was in the blast).
    if (ignite(CellPos{cx, cy, level}, 0.0f, true)) {
        any = true;
    }
    const entt::registry& reg = things_.registry();
    scratch_.clear();
    std::vector<Entity> pawns;
    for (const auto [pawn, pos, health] : reg.view<const Position, const Health>().each()) {
        (void)health;
        const float dx = pos.x - x;
        const float dy = pos.y - y;
        if (pos.level == level && dx * dx + dy * dy <= radius * radius) {
            pawns.push_back(pawn);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    for (const Entity pawn : pawns) {
        Rng rng(rngSeed(now, static_cast<int>(entt::to_integral(pawn)), 6), 6);
        if (rng.unit() < chance && setPawnAlight(pawn, now)) {
            any = true;
        }
    }
    return any;
}

bool Fire::setPawnAlight(Entity pawn, int64_t now, bool force) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || reg.all_of<Burning>(pawn) || !health_.tracks(pawn)) {
        return false;
    }
    const Health* health = reg.try_get<Health>(pawn);
    if (health == nullptr || health->dead || (!force && !(pawnFlammability(pawn) > 0.0f))) {
        return false;
    }
    Rng rng(rngSeed(now, static_cast<int>(entt::to_integral(pawn)), 4), 4);
    const double seconds = static_cast<double>(numbers_.pawnBurnTicks) * numbers_.ticksToSeconds * (0.75 + 0.5 * rng.unit());
    reg.emplace<Burning>(pawn, Burning{now + static_cast<int64_t>(seconds)});
    ++pawnsBurnt_;
    CellPos cell;
    if (const Position* pos = reg.try_get<Position>(pawn)) {
        cell = cellOf(*pos);
    }
    emit(EventType::PawnCaughtFire, pawn, cell);
    return true;
}

bool Fire::putOutPawn(Entity pawn) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Burning>(pawn)) {
        return false;
    }
    reg.remove<Burning>(pawn);
    CellPos cell;
    if (const Position* pos = reg.try_get<Position>(pawn)) {
        cell = cellOf(*pos);
    }
    emit(EventType::PawnFireOut, pawn, cell);
    return true;
}

// ---- Putting out ----------------------------------------------------------------------------

bool Fire::extinguish(CellPos cell, float amount) {
    if (!map_.inBounds(cell)) {
        return false;
    }
    const int index = map_.index(cell);
    const auto it = fires_.find(index);
    if (it == fires_.end()) {
        return false;
    }
    it->second.size -= amount;
    ++changes_;
    if (it->second.size > 0.0f) {
        return false;
    }
    if (it->second.age >= 2) {
        markBurnt(index, now_);
    }
    fires_.erase(it);
    emit(EventType::FireOut, kNoEntity, cell, DefId{}, 0.0f, "beaten");
    // Whoever stood in it is put out with it.
    std::vector<Entity> here;
    for (const auto [pawn, burn, pos] : things_.registry().view<const Burning, const Position>().each()) {
        (void)burn;
        if (cellOf(pos) == cell) {
            here.push_back(pawn);
        }
    }
    std::sort(here.begin(), here.end());
    for (const Entity pawn : here) {
        putOutPawn(pawn);
    }
    return true;
}

// ---- Burnt ground ---------------------------------------------------------------------------

void Fire::markBurnt(int index, int64_t now) {
    burnt_[index] = now;
    ++burntChanges_;
}

// ---- The home area --------------------------------------------------------------------------

bool Fire::home(CellPos cell) const {
    return map_.inBounds(cell) && home_[static_cast<size_t>(map_.index(cell))] != 0;
}

std::vector<int> Fire::homeCells() const {
    std::vector<int> out;
    out.reserve(homeCount_);
    for (size_t i = 0; i < home_.size(); ++i) {
        if (home_[i] != 0) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

int Fire::setHome(const CellRect& area, bool add) {
    autoHome_ = false;
    homeStarted_ = true;
    int changed = 0;
    for (int y = area.minY; y < area.minY + area.sizeY; ++y) {
        for (int x = area.minX; x < area.minX + area.sizeX; ++x) {
            const CellPos c{x, y, area.level};
            if (!map_.inBounds(c)) {
                continue;
            }
            uint8_t& cell = home_[static_cast<size_t>(map_.index(c))];
            if ((cell != 0) == add) {
                continue;
            }
            cell = add ? 1 : 0;
            homeCount_ = add ? homeCount_ + 1 : homeCount_ - 1;
            homeHash_ ^= splitMix64(static_cast<uint64_t>(map_.index(c)) + 1);
            ++changed;
        }
    }
    if (changed > 0) {
        ++homeChanges_;
    }
    return changed;
}

void Fire::stampHome(CellPos centre, int radius) {
    bool changed = false;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            const CellPos c{centre.x + dx, centre.y + dy, centre.level};
            if (!map_.inBounds(c) || map_.terrain(c) == Terrain::Rock) {
                continue;
            }
            uint8_t& cell = home_[static_cast<size_t>(map_.index(c))];
            if (cell == 0) {
                cell = 1;
                ++homeCount_;
                homeHash_ ^= splitMix64(static_cast<uint64_t>(map_.index(c)) + 1);
                changed = true;
            }
        }
    }
    if (changed) {
        ++homeChanges_;
    }
}

// ---- Each game second -----------------------------------------------------------------------

void Fire::step(int64_t now, size_t from) {
    now_ = now;
    const entt::registry& reg = things_.registry();
    if (!homeStarted_) {
        homeStarted_ = true;
        if (autoHome_) {
            for (const auto [e, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
                (void)e;
                if (thing.category == ThingCategory::Building) {
                    stampHome(footprint.origin, numbers_.autoHomeRadius);
                }
            }
            for (const auto [e, colonist, pos] : reg.view<const Colonist, const Position>().each()) {
                (void)e;
                (void)colonist;
                stampHome(cellOf(pos), numbers_.startHomeRadius);
            }
        }
    }
    if (events_ != nullptr) {
        // Events appended while this runs (FireStarted...) are not looked at.
        const size_t end = events_->size();
        for (size_t i = from; i < end; ++i) {
            const EventType type = (*events_)[i].type;
            if (type == EventType::ThingSpawned) {
                const Entity thing = (*events_)[i].thing;
                if (autoHome_ && reg.valid(thing) && reg.all_of<Thing, Footprint>(thing) &&
                    reg.get<Thing>(thing).category == ThingCategory::Building) {
                    stampHome(reg.get<Footprint>(thing).origin, numbers_.autoHomeRadius);
                }
            } else if (type == EventType::Explosion && burnType_.valid() && (*events_)[i].def == burnType_) {
                const Event e = (*events_)[i];
                igniteRadius(e.x, e.y, e.cell.level, e.amount, numbers_.blastIgniteChance, now);
            }
        }
    }
    if (now % numbers_.passSeconds == 0) {
        pass(now);
    }
    if (now % kSecondsPerHour == 0 && !burnt_.empty()) {
        const int64_t fade = static_cast<int64_t>(numbers_.burntFadeDays * static_cast<float>(kSecondsPerDay));
        bool any = false;
        for (auto it = burnt_.begin(); it != burnt_.end();) {
            if (now - it->second >= fade) {
                it = burnt_.erase(it);
                any = true;
            } else {
                ++it;
            }
        }
        if (any) {
            ++burntChanges_;
        }
    }
}

// What the fire does to what is in its cell. Damage is a share of a hit point, rounded at random.
void Fire::burnThings(CellPos c, FireCell& fire, float damage, Rng& rng) {
    entt::registry& reg = things_.registry();
    const int index = map_.index(c);
    const auto burnUp = [&](Entity thing) {
        things_.destroy(thing);
        ++burntUp_;
        filth_.add(c, "Ash", 1);
        markBurnt(index, now_);
    };
    const Entity building = map_.buildingAt(c);
    if (building != kNoEntity && thingFlammability(building) > 0.0f) {
        if (HitPoints* hp = reg.try_get<HitPoints>(building)) {
            hp->current -= fireRoundRandom(damage, rng);
            if (hp->current <= 0) {
                burnUp(building);
            }
        }
    }
    const std::vector<Entity> items = things_.itemsAt(c);
    for (const Entity item : items) {
        if (!reg.valid(item) || !(thingFlammability(item) > 0.0f)) {
            continue;
        }
        if (HitPoints* hp = reg.try_get<HitPoints>(item)) {
            hp->current -= fireRoundRandom(damage, rng);
            if (hp->current <= 0) {
                burnUp(item);
            }
        }
    }
    if (plants_.has(c)) {
        const DefId def = plants_.defAt(c);
        if (defFlammability(def) > 0.0f) {
            fire.plantDamage = static_cast<uint16_t>(
                std::min(60000, static_cast<int>(fire.plantDamage) + fireRoundRandom(damage * numbers_.plantDamageFactor, rng)));
            const float hp = def.value < maxHp_.size() && maxHp_[def.value] > 0.0f ? maxHp_[def.value] : 50.0f;
            if (static_cast<float>(fire.plantDamage) >= hp) {
                plants_.remove(c);
                fire.plantDamage = 0;
                markBurnt(index, now_);
                emit(EventType::PlantRemoved, kNoEntity, c, def, 0.0f, "burnt");
            }
        }
    }
}

// Embers: each cell within reach that can burn catches with a chance that grows with the fire and the cell's
// flammability. The cells are only listed here; the pass lights them once every fire has been looked at, so the
// order fires are visited in changes nothing.
void Fire::spreadFrom(CellPos c, const FireCell& fire, int64_t now, int index, std::vector<int>& ignitions, Rng& rng) {
    (void)now;
    (void)index;
    const double ticks = static_cast<double>(numbers_.passSeconds) / numbers_.ticksToSeconds;
    for (const auto& off : kFireOffsets) {
        const bool ember = off[0] * off[0] + off[1] * off[1] > 2;
        if (ember && fire.size < numbers_.emberMinSize) {
            continue;
        }
        const CellPos n{c.x + off[0], c.y + off[1], c.level};
        if (!canBurnGround(n) || fires_.count(map_.index(n)) != 0) {
            continue;
        }
        const float flam = flammabilityAt(n);
        if (!(flam > 0.0f)) {
            continue;
        }
        const float p = static_cast<float>(numbers_.spreadPerTick * ticks) * fire.size * flam * (ember ? numbers_.emberFactor : 1.0f);
        if (rng.unit() < p) {
            ignitions.push_back(map_.index(n));
        }
    }
}

void Fire::pass(int64_t now) {
    entt::registry& reg = things_.registry();
    const double ticks = static_cast<double>(numbers_.passSeconds) / numbers_.ticksToSeconds;
    const WeatherEffects effects = weather_.effects();
    const float rain = effects.extinguishesFire ? effects.rainRate : 0.0f;

    // ---- Pawns on fire: they burn, and set fire to the ground they stand on.
    std::vector<Entity> burningPawns;
    for (const auto [pawn, burn] : reg.view<Burning>().each()) {
        (void)burn;
        burningPawns.push_back(pawn);
    }
    std::sort(burningPawns.begin(), burningPawns.end());
    std::vector<int> pawnCells;
    for (const Entity pawn : burningPawns) {
        const Health* health = reg.try_get<Health>(pawn);
        const Position* pos = reg.try_get<Position>(pawn);
        if (health == nullptr || health->dead || pos == nullptr) {
            reg.remove<Burning>(pawn);
            continue;
        }
        if (now >= reg.get<Burning>(pawn).until) {
            putOutPawn(pawn);
            continue;
        }
        Rng rng(rngSeed(now, static_cast<int>(entt::to_integral(pawn)), 7), 7);
        const int amount = fireRoundRandom(static_cast<float>(numbers_.pawnDamagePerTick * ticks), rng);
        if (amount > 0 && !health_.applyDamage(pawn, burnType_, static_cast<float>(amount), -1, rng)) {
            reg.remove<Burning>(pawn);
            continue;
        }
        const CellPos cell = cellOf(*pos);
        if (canBurnGround(cell)) {
            pawnCells.push_back(map_.index(cell));
            if (fires_.count(map_.index(cell)) == 0 && !wetAt(cell)) {
                ignite(cell, 0.0f, true);
            }
        }
    }
    std::sort(pawnCells.begin(), pawnCells.end());

    if (fires_.empty()) {
        return;
    }
    std::vector<int> keys;
    keys.reserve(fires_.size());
    for (const auto& [index, fire] : fires_) {
        (void)fire;
        keys.push_back(index);
    }
    std::vector<int> ignitions;
    std::vector<int> out;
    std::vector<RoomId> hotRooms;
    for (const int index : keys) {
        const CellPos c = map_.cellAt(index);
        FireCell& fire = fires_[index];
        Rng rng(rngSeed(now, index, 1), 1);
        const bool pawnHere = std::binary_search(pawnCells.begin(), pawnCells.end(), index);

        if (weather_.outdoors(c)) {
            if (weather_.snowDepth(c) >= numbers_.snowBlocksAt) {
                fire.size = 0.0f;
            } else if (rain > 0.0f) {
                fire.size -= static_cast<float>(numbers_.rainOutPerTick * ticks) * rain;
            }
        }
        if (fire.size > 0.0f) {
            float flam = flammabilityAt(c);
            if (flam > 0.0f) {
                const float damage = std::clamp(numbers_.damageBase + numbers_.damagePerSize * fire.size, numbers_.damageBase,
                                                numbers_.damageMax) *
                                     static_cast<float>(ticks);
                burnThings(c, fire, damage, rng);
                flam = flammabilityAt(c);
            }
            if (pawnHere) {
                flam = std::max(flam, numbers_.pawnFlammability);
            }
            if (flam > 0.0f) {
                const float cap = std::max(numbers_.startSize, numbers_.maxSize * std::min(1.0f, flam));
                if (fire.size < cap) {
                    fire.size = std::min(cap, fire.size + static_cast<float>(numbers_.growthPerTick * ticks));
                } else if (fire.size > cap) {
                    fire.size = std::max(cap, fire.size - static_cast<float>(numbers_.shrinkPerTick * ticks));
                }
            } else {
                fire.size -= static_cast<float>(numbers_.shrinkPerTick * ticks);
            }
        }
        if (fire.age < 60000) {
            ++fire.age;
        }
        if (fire.size > 0.0f) {
            temperature_.addHeat(c, static_cast<double>(numbers_.heatPerSecond * fire.size), static_cast<double>(numbers_.passSeconds),
                                 numbers_.heatMaxC);
            if (temperature_.temperatureAt(c) >= numbers_.autoIgniteC) {
                if (const Room* room = rooms_.roomOf(c)) {
                    hotRooms.push_back(room->id);
                }
            }
            spreadFrom(c, fire, now, index, ignitions, rng);
        }
        if (fire.size <= 0.0f) {
            out.push_back(index);
        }
    }
    for (const int index : out) {
        const auto it = fires_.find(index);
        const CellPos c = map_.cellAt(index);
        if (it->second.age >= 2) {
            markBurnt(index, now);
        }
        // A floor that burns is burnt through if the fire stayed long enough.
        if (it->second.age >= numbers_.floorBurnPasses) {
            const DefId floor = floors_.at(c);
            if (floor.valid() && defFlammability(floor) > 0.0f) {
                floors_.set(c, DefId{});
            }
        }
        fires_.erase(it);
        emit(EventType::FireOut, kNoEntity, c, DefId{}, 0.0f, "burnt out");
    }

    // ---- Rooms that got too hot to hold anything: everything flammable in them catches.
    std::sort(hotRooms.begin(), hotRooms.end());
    hotRooms.erase(std::unique(hotRooms.begin(), hotRooms.end()), hotRooms.end());
    for (const RoomId id : hotRooms) {
        Rng rng(rngSeed(now, static_cast<int>(id), 3), 3);
        for (const CellPos& c : rooms_.cellsOf(rooms_.room(id))) {
            if (fires_.count(map_.index(c)) == 0 && flammabilityAt(c) > 0.0f && rng.unit() < numbers_.hotRoomIgniteChance) {
                ignitions.push_back(map_.index(c));
            }
        }
    }
    std::sort(ignitions.begin(), ignitions.end());
    ignitions.erase(std::unique(ignitions.begin(), ignitions.end()), ignitions.end());
    for (const int index : ignitions) {
        ignite(map_.cellAt(index));
    }

    // ---- Pawns standing in the fire (or in a room that is too hot) catch.
    std::vector<Entity> candidates;
    for (const auto [pawn, pos, health] : reg.view<const Position, const Health>().each()) {
        if (!health.dead && !reg.all_of<Burning>(pawn)) {
            candidates.push_back(pawn);
            (void)pos;
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (const Entity pawn : candidates) {
        const CellPos cell = cellOf(reg.get<Position>(pawn));
        if (!map_.inBounds(cell)) {
            continue;
        }
        float chance = 0.0f;
        const auto it = fires_.find(map_.index(cell));
        if (it != fires_.end()) {
            chance = static_cast<float>(numbers_.catchPerTick * ticks) * it->second.size * pawnFlammability(pawn);
        }
        if (!hotRooms.empty()) {
            if (const Room* room = rooms_.roomOf(cell);
                room != nullptr && std::binary_search(hotRooms.begin(), hotRooms.end(), room->id)) {
                chance = std::max(chance, numbers_.hotPawnIgniteChance);
            }
        }
        if (chance <= 0.0f) {
            continue;
        }
        Rng rng(rngSeed(now, static_cast<int>(entt::to_integral(pawn)), 2), 2);
        if (rng.unit() < chance) {
            setPawnAlight(pawn, now);
        }
    }
    ++changes_;
}

// ---- Saving ---------------------------------------------------------------------------------

FireSaved Fire::saved() const {
    FireSaved s;
    for (const auto& [index, fire] : fires_) {
        s.fires.push_back(FireRecord{index, fire});
    }
    for (const auto& [index, at] : burnt_) {
        s.burnt.push_back(BurntRecord{index, at});
    }
    s.home = homeCells();
    s.autoHome = autoHome_;
    s.homeStarted = homeStarted_;
    s.started = started_;
    s.burntUp = burntUp_;
    s.pawnsBurnt = pawnsBurnt_;
    return s;
}

bool Fire::restore(const FireSaved& saved, std::string& error) {
    const int cells = map_.width() * map_.height();
    fires_.clear();
    burnt_.clear();
    std::fill(home_.begin(), home_.end(), uint8_t{0});
    homeCount_ = 0;
    homeHash_ = 0;
    for (const FireRecord& r : saved.fires) {
        if (r.cell < 0 || r.cell >= cells || !(r.fire.size > 0.0f) || !(r.fire.size <= numbers_.maxSize + 0.001f)) {
            error = "the save file has a fire that can't burn";
            return false;
        }
        fires_[r.cell] = r.fire;
    }
    for (const BurntRecord& r : saved.burnt) {
        if (r.cell < 0 || r.cell >= cells) {
            error = "the save file has burnt ground off the map";
            return false;
        }
        burnt_[r.cell] = r.at;
    }
    for (const int index : saved.home) {
        if (index < 0 || index >= cells) {
            error = "the save file has a home area cell off the map";
            return false;
        }
        if (home_[static_cast<size_t>(index)] == 0) {
            home_[static_cast<size_t>(index)] = 1;
            ++homeCount_;
            homeHash_ ^= splitMix64(static_cast<uint64_t>(index) + 1);
        }
    }
    autoHome_ = saved.autoHome;
    homeStarted_ = saved.homeStarted;
    started_ = saved.started;
    burntUp_ = saved.burntUp;
    pawnsBurnt_ = saved.pawnsBurnt;
    ++changes_;
    ++burntChanges_;
    ++homeChanges_;
    return true;
}

void Fire::hashState(StateHasher& h) const {
    if (fires_.empty() && burnt_.empty() && homeCount_ == 0 && started_ == 0 && pawnsBurnt_ == 0) {
        return;
    }
    h.add(static_cast<uint64_t>(fires_.size()));
    for (const auto& [index, fire] : fires_) {
        h.add(index);
        h.add(fire.size);
        h.add(static_cast<int>(fire.age));
        h.add(static_cast<int>(fire.plantDamage));
    }
    h.add(static_cast<uint64_t>(burnt_.size()));
    for (const auto& [index, at] : burnt_) {
        h.add(index);
        h.add(at);
    }
    h.add(static_cast<uint64_t>(homeCount_));
    h.add(homeHash_);
    h.add(autoHome_);
    h.add(homeStarted_);
    h.add(started_);
    h.add(burntUp_);
    h.add(pawnsBurnt_);
}

void Fire::hashThing(Entity e, StateHasher& h) const {
    if (const Burning* b = things_.registry().try_get<Burning>(e)) {
        h.add(b->until);
    }
}

}  // namespace colony

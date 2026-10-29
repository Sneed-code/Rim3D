#include "ColonySim/Monsters.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

constexpr int kMonsterCheckEvery = 30;     // game seconds between looks at the mounds and insects
constexpr float kMonsterWakeRange = 60.0f; // insects hunt a colonist this close to them
constexpr float kMonsterMinDistance = 20.0f;  // an infestation starts this far from every colonist, at least
constexpr float kMonsterMaxDistance = 60.0f;  // ... and (if it can) no further than this from the colony's middle

struct MonsterKind {
    std::string defName;
    float power = 0.0f;
    std::string weapon;
};

float monsterDist(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

}  // namespace

struct MonsterSystem::Impl {
    Things& things;
    Map& map;
    CombatSystem& combat;
    HealthSystem& health;
    FighterSystem& fighters;
    RaidSystem& raids;
    AnimalSystem& animals;
    const DefDatabase& defs;
    const GameTime& time;
    Rng& rng;
    DefId hiveDef;
    DefId jellyDef;
    std::vector<DefId> insectDefs;
    // Numbers of the hive def.
    float jellyAmount = 20;
    double jellyDaysMin = 0.433, jellyDaysMax = 0.5;
    int jellyCap = 40;
    int destroyJelly = 30;
    float spreadRange = 10;
    size_t maxHives = 30;
    // Ours: brood and spread intervals.
    double broodDaysMin = 0.4, broodDaysMax = 0.6;
    double spreadDaysMin = 2.0, spreadDaysMax = 3.5;
    size_t insectsPerHive = 6;

    Impl(Things& t, Map& m, CombatSystem& c, HealthSystem& h, FighterSystem& f, RaidSystem& r, AnimalSystem& a,
         const DefDatabase& d, const GameTime& tm, Rng& g)
        : things(t), map(m), combat(c), health(h), fighters(f), raids(r), animals(a), defs(d), time(tm), rng(g) {
        hiveDef = defs.idOf("Things", "Hive");
        jellyDef = defs.idOf("Things", "InsectJelly");
        if (hiveDef.valid()) {
            const Def& def = defs.get(hiveDef);
            for (size_t i = 0; i < def.count("hive.insects"); ++i) {
                const DefId id = defs.idOf("Things", def.textAt("hive.insects", i));
                if (id.valid()) {
                    insectDefs.push_back(id);
                }
            }
            jellyAmount = static_cast<float>(def.number("hive.jellyAmount", jellyAmount));
            jellyDaysMin = def.number("hive.jellyIntervalDays.0", jellyDaysMin);
            jellyDaysMax = def.number("hive.jellyIntervalDays.1", jellyDaysMax);
            jellyCap = static_cast<int>(def.number("hive.jellyCap", jellyCap));
            destroyJelly = static_cast<int>(def.number("hive.destroyJelly", destroyJelly));
            spreadRange = static_cast<float>(def.number("hive.spreadRange", spreadRange));
            maxHives = static_cast<size_t>(def.number("hive.maxHives", static_cast<double>(maxHives)));
        }
    }

    std::vector<Entity> hives() const {
        std::vector<Entity> out;
        const entt::registry& reg = things.registry();
        for (const auto [e, thing] : reg.view<const Thing>().each()) {
            if (thing.def == hiveDef && things.spawned(e)) {
                out.push_back(e);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // The player's able colonists on the map.
    std::vector<Entity> colonists() const {
        std::vector<Entity> out;
        const entt::registry& reg = things.registry();
        for (const auto [pawn, colonist, pos] : reg.view<const Colonist, const Position>().each()) {
            (void)colonist;
            (void)pos;
            if (fighters.factionOf(pawn) == kPlayerFaction && !health.incapacitated(pawn)) {
                out.push_back(pawn);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    Entity nearestOf(const std::vector<Entity>& pawns, float x, float y, float& dist) const {
        const entt::registry& reg = things.registry();
        Entity best = kNoEntity;
        dist = 1e9f;
        for (const Entity p : pawns) {
            const Position& pos = reg.get<Position>(p);
            const float d = monsterDist(pos.x, pos.y, x, y);
            if (d < dist) {
                dist = d;
                best = p;
            }
        }
        return best;
    }

    // A free ground cell near (x, y), not under a building, no pawn on it.
    bool freeCellNear(CellPos around, int radius, CellPos& out) const {
        for (int r = 0; r <= radius; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) {
                        continue;
                    }
                    const CellPos c{around.x + dx, around.y + dy, around.level};
                    if (map.walkable(c) && map.obstacleDm(c) == 0 && !map.isDoor(c)) {
                        out = c;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // True when `due` falls in this check: once per period, at a point of the period set by `h`.
    bool due(uint64_t h, uint64_t salt, double daysMin, double daysMax) const {
        const uint64_t a = splitMix64(h ^ (salt * 0x9E3779B97F4A7C15ull));
        const uint64_t b = splitMix64(a);
        const double days = daysMin + (daysMax - daysMin) * static_cast<double>(a % 1000) / 1000.0;
        const int64_t period = std::max<int64_t>(kMonsterCheckEvery * 2, static_cast<int64_t>(days * kSecondsPerDay));
        return (time.seconds + static_cast<int64_t>(b % static_cast<uint64_t>(period))) % period < kMonsterCheckEvery;
    }

    int jelliesAround(CellPos hive) const {
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                for (const Entity item : things.itemsAt(CellPos{hive.x + dx, hive.y + dy, hive.level})) {
                    const entt::registry& reg = things.registry();
                    if (reg.get<Thing>(item).def == jellyDef) {
                        n += reg.all_of<StackCount>(item) ? reg.get<StackCount>(item).count : 1;
                    }
                }
            }
        }
        return n;
    }

    void dropJelly(CellPos hive, int amount) {
        if (!jellyDef.valid()) {
            return;
        }
        CellPos at;
        if (!freeCellNear(hive, 2, at)) {
            return;
        }
        while (amount > 0) {
            const int stack = std::min(amount, 75);
            things.spawn(jellyDef, at, stack);
            amount -= stack;
        }
    }

    Entity spawnInsect(CellPos near, const std::vector<Entity>& colony) {
        if (insectDefs.empty()) {
            return kNoEntity;
        }
        CellPos at;
        if (!freeCellNear(near, 3, at)) {
            return kNoEntity;
        }
        const Entity insect = things.spawn(insectDefs[rng.below(static_cast<uint32_t>(insectDefs.size()))], at);
        if (insect == kNoEntity) {
            return kNoEntity;
        }
        if (!colony.empty()) {
            float d;
            const Entity target = nearestOf(colony, static_cast<float>(at.x) + 0.5f, static_cast<float>(at.y) + 0.5f, d);
            animals.enrage(insect, AnimalTemper::Manhunter, target);
        }
        return insect;
    }

    size_t insectCount() const {
        size_t n = 0;
        const entt::registry& reg = things.registry();
        for (const auto [e, animal, thing] : reg.view<const Animal, const Thing>().each()) {
            (void)animal;
            if (std::find(insectDefs.begin(), insectDefs.end(), thing.def) != insectDefs.end() && things.spawned(e) &&
                !health.incapacitated(e)) {
                ++n;
            }
        }
        return n;
    }

    bool hiveSpot(CellPos from, CellPos& out) {
        for (int tries = 0; tries < 40; ++tries) {
            const int dx = static_cast<int>(rng.below(static_cast<uint32_t>(2 * static_cast<int>(spreadRange) + 1))) - static_cast<int>(spreadRange);
            const int dy = static_cast<int>(rng.below(static_cast<uint32_t>(2 * static_cast<int>(spreadRange) + 1))) - static_cast<int>(spreadRange);
            const CellPos c{from.x + dx, from.y + dy, from.level};
            if (std::abs(dx) <= 1 && std::abs(dy) <= 1) {
                continue;
            }
            if (monsterDist(0, 0, static_cast<float>(dx), static_cast<float>(dy)) > spreadRange || !map.walkable(c) ||
                map.obstacleDm(c) != 0 || map.isDoor(c) || map.roof(c) == Roof::None) {
                continue;
            }
            bool crowded = false;
            for (int ay = -1; ay <= 1 && !crowded; ++ay) {
                for (int ax = -1; ax <= 1; ++ax) {
                    if (map.obstacleDm(CellPos{c.x + ax, c.y + ay, c.level}) != 0 || !map.walkable(CellPos{c.x + ax, c.y + ay, c.level})) {
                        crowded = crowded || (ax == 0 && ay == 0);
                    }
                }
            }
            if (!crowded) {
                out = c;
                return true;
            }
        }
        return false;
    }

    std::vector<MonsterKind> mechKinds() const {
        std::vector<MonsterKind> kinds;
        for (const DefId id : defs.idsOfKind("Things")) {
            const Def& def = defs.get(id);
            if (def.text("race.intelligence") == "Mechanoid" && def.number("race.combatPower") > 0.0) {
                kinds.push_back(MonsterKind{def.defName(), static_cast<float>(def.number("race.combatPower")), def.text("race.weapon")});
            }
        }
        std::sort(kinds.begin(), kinds.end(), [](const MonsterKind& a, const MonsterKind& b) { return a.defName < b.defName; });
        return kinds;
    }

    // Spends `points` on kinds at random among those still affordable; at least one.
    std::vector<size_t> buy(const std::vector<MonsterKind>& kinds, float points) {
        std::vector<size_t> chosen;
        float left = points;
        for (int guard = 0; guard < 300; ++guard) {
            std::vector<size_t> ok;
            for (size_t i = 0; i < kinds.size(); ++i) {
                if (kinds[i].power <= left) {
                    ok.push_back(i);
                }
            }
            if (ok.empty()) {
                break;
            }
            const size_t pick = ok[rng.below(static_cast<uint32_t>(ok.size()))];
            chosen.push_back(pick);
            left -= kinds[pick].power;
        }
        if (chosen.empty() && !kinds.empty()) {
            size_t cheapest = 0;
            for (size_t i = 1; i < kinds.size(); ++i) {
                if (kinds[i].power < kinds[cheapest].power) {
                    cheapest = i;
                }
            }
            chosen.push_back(cheapest);
        }
        return chosen;
    }
};

MonsterSystem::MonsterSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, FighterSystem& fighters,
                             RaidSystem& raids, AnimalSystem& animals, const DefDatabase& defs, const GameTime& time, Rng& rng)
    : impl_(new Impl(things, map, combat, health, fighters, raids, animals, defs, time, rng)) {}

MonsterSystem::~MonsterSystem() {
    delete impl_;
}

size_t MonsterSystem::hiveCount() const {
    return impl_->hives().size();
}

Entity MonsterSystem::spawnHive(CellPos cell) {
    Impl& s = *impl_;
    if (!s.hiveDef.valid() || !s.map.walkable(cell) || s.map.obstacleDm(cell) != 0) {
        return kNoEntity;
    }
    return s.things.spawn(s.hiveDef, cell);
}

bool MonsterSystem::infest(float points, std::string& error, bool underMountain) {
    Impl& s = *impl_;
    if (!s.hiveDef.valid() || s.insectDefs.empty()) {
        error = "the defs have no brood mound or insects";
        return false;
    }
    if (points <= 0.0f) {
        points = s.raids.threatPoints();
    }
    const std::vector<Entity> colony = s.colonists();
    const entt::registry& reg = s.things.registry();
    float midX = static_cast<float>(s.map.width()) * 0.5f;
    float midY = static_cast<float>(s.map.height()) * 0.5f;
    if (!colony.empty()) {
        midX = midY = 0.0f;
        for (const Entity p : colony) {
            midX += reg.get<Position>(p).x / static_cast<float>(colony.size());
            midY += reg.get<Position>(p).y / static_cast<float>(colony.size());
        }
    }
    // Where: a roofed cell 20 to 60 m from the colony's middle and 20 m from every colonist; else any open cell like that.
    const int hiveCount = std::clamp(1 + static_cast<int>(points / 500.0f), 1, 4);
    CellPos first;
    bool found = false;
    if (underMountain) {
        // Every cell under overhead mountain in the ring, then one of them at random.
        std::vector<CellPos> spots;
        const int reach = static_cast<int>(kMonsterMaxDistance) + 1;
        for (int y = static_cast<int>(midY) - reach; y <= static_cast<int>(midY) + reach; ++y) {
            for (int x = static_cast<int>(midX) - reach; x <= static_cast<int>(midX) + reach; ++x) {
                const CellPos c{x, y, 0};
                const float mx = static_cast<float>(x) + 0.5f - midX;
                const float my = static_cast<float>(y) + 0.5f - midY;
                const float fromMid2 = mx * mx + my * my;
                if (fromMid2 < kMonsterMinDistance * kMonsterMinDistance || fromMid2 > kMonsterMaxDistance * kMonsterMaxDistance || !s.map.inBounds(c) || s.map.roof(c) != Roof::Thick ||
                    !s.map.walkable(c) || s.map.obstacleDm(c) != 0 || s.map.isDoor(c)) {
                    continue;
                }
                float d;
                s.nearestOf(colony, static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f, d);
                if (colony.empty() || d >= kMonsterMinDistance) {
                    spots.push_back(c);
                }
            }
        }
        if (spots.empty()) {
            error = "no overhead mountain near the colony";
            return false;
        }
        first = spots[s.rng.below(static_cast<uint32_t>(spots.size()))];
        found = true;
    }
    for (int pass = 0; pass < 2 && !found; ++pass) {
        for (int tries = 0; tries < 600 && !found; ++tries) {
            const float angle = s.rng.unit() * 6.2831853f;
            const float dist = kMonsterMinDistance + s.rng.unit() * (kMonsterMaxDistance - kMonsterMinDistance);
            const CellPos c{static_cast<int>(midX + std::cos(angle) * dist), static_cast<int>(midY + std::sin(angle) * dist), 0};
            if (!s.map.walkable(c) || s.map.obstacleDm(c) != 0 || s.map.isDoor(c) || (pass == 0 && s.map.roof(c) == Roof::None)) {
                continue;
            }
            float d;
            s.nearestOf(colony, static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f, d);
            if (!colony.empty() && d < kMonsterMinDistance) {
                continue;
            }
            first = c;
            found = true;
        }
    }
    if (!found) {
        error = "nowhere for an infestation to start";
        return false;
    }
    std::vector<Entity> placed;
    for (int i = 0; i < hiveCount; ++i) {
        CellPos at = first;
        if (i > 0 && !s.hiveSpot(first, at)) {
            break;
        }
        const Entity hive = spawnHive(at);
        if (hive != kNoEntity) {
            placed.push_back(hive);
        }
    }
    if (placed.empty()) {
        error = "no mound could be placed";
        return false;
    }
    // The insects it comes with: bought with the points like raiders, by combat power.
    std::vector<MonsterKind> kinds;
    for (const DefId id : s.insectDefs) {
        const Def& def = s.defs.get(id);
        kinds.push_back(MonsterKind{def.defName(), static_cast<float>(def.number("race.combatPower", 50.0)), ""});
    }
    size_t k = 0;
    for (const size_t i : s.buy(kinds, points)) {
        (void)i;
        const CellPos near = cellOf(reg.get<Position>(placed[k++ % placed.size()]));
        s.spawnInsect(near, colony);
    }
    return true;
}

bool MonsterSystem::remnantCluster(float points, int edge, std::string& error) {
    Impl& s = *impl_;
    if (edge < -1 || edge > 3) {
        error = "the edge must be -1 (any) or 0 to 3";
        return false;
    }
    const std::vector<MonsterKind> kinds = s.mechKinds();
    if (kinds.empty()) {
        error = "the defs have no mechanoids";
        return false;
    }
    if (points <= 0.0f) {
        points = s.raids.threatPoints();
    }
    const std::vector<Entity> colony = s.colonists();
    const entt::registry& reg = s.things.registry();
    CellPos target{s.map.width() / 2, s.map.height() / 2, 0};
    if (!colony.empty()) {
        float d;
        const Entity e = s.nearestOf(colony, static_cast<float>(s.map.width()) * 0.5f, static_cast<float>(s.map.height()) * 0.5f, d);
        target = cellOf(reg.get<Position>(e));
    }
    const int w = s.map.width();
    const int h = s.map.height();
    const int firstSide = edge >= 0 ? edge : static_cast<int>(s.rng.below(4));
    CellPos entry;
    bool found = false;
    for (int attempt = 0; attempt < (edge >= 0 ? 40 : 160) && !found; ++attempt) {
        const int side = (firstSide + attempt / 40) % 4;
        const int along = static_cast<int>(s.rng.below(static_cast<uint32_t>(side % 2 == 0 ? w : h)));
        const int d = 3;
        const CellPos guess = side == 0 ? CellPos{along, h - 1 - d, 0}
                            : side == 1 ? CellPos{w - 1 - d, along, 0}
                            : side == 2 ? CellPos{along, d, 0}
                                        : CellPos{d, along, 0};
        CellPos c;
        if (s.freeCellNear(guess, 6, c) && s.map.regions().canReach(c, target)) {
            entry = c;
            found = true;
        }
    }
    if (!found) {
        error = "no way in from that edge to the colony";
        return false;
    }
    const FactionId side = s.raids.factionId("Remnant");
    int made = 0;
    for (const size_t i : s.buy(kinds, points)) {
        // Three metres apart, in a loose block round the entry.
        CellPos at;
        const CellPos spot{entry.x + (made % 3) * 3 - 3, entry.y + (made / 3) * 3, 0};
        if (!s.freeCellNear(spot, 4, at)) {
            continue;
        }
        const Entity mech = s.things.spawn(kinds[i].defName, at);
        if (mech == kNoEntity) {
            continue;
        }
        entt::registry& r = s.things.registry();
        r.emplace_or_replace<Faction>(mech, Faction{side});
        if (!kinds[i].weapon.empty()) {
            const Entity gun = s.things.spawn(kinds[i].weapon, at);
            if (gun != kNoEntity && !s.combat.equip(mech, gun)) {
                s.things.destroy(gun);
            }
        }
        Fighter& f = s.fighters.enlist(mech, "raiding");
        f.advances = true;
        f.hasGoal = true;
        f.goal = target;
        ++made;
    }
    if (made == 0) {
        error = "the mechanoids couldn't be placed";
        return false;
    }
    return true;
}

void MonsterSystem::step(const std::vector<Event>& events, size_t from) {
    Impl& s = *impl_;
    if (!s.hiveDef.valid()) {
        return;
    }
    for (size_t i = from; i < events.size(); ++i) {
        const Event& ev = events[i];
        if (ev.type == EventType::ThingDespawned && ev.def == s.hiveDef) {
            s.dropJelly(ev.cell, s.destroyJelly);
        }
    }
    if (s.time.seconds % kMonsterCheckEvery != 0) {
        return;
    }
    const std::vector<Entity> hives = s.hives();
    const std::vector<Entity> colony = s.colonists();
    const entt::registry& reg = s.things.registry();
    size_t insects = hives.empty() ? 0 : s.insectCount();
    size_t total = hives.size();
    for (const Entity hive : hives) {
        const uint64_t h = splitMix64(static_cast<uint64_t>(entt::to_integral(hive)) + 0x51ull);
        const CellPos at = cellOf(reg.get<Position>(hive));
        if (s.due(h, 1, s.jellyDaysMin, s.jellyDaysMax) && s.jelliesAround(at) < s.jellyCap) {
            s.dropJelly(at, static_cast<int>(s.jellyAmount));
        }
        if (s.due(h, 2, s.broodDaysMin, s.broodDaysMax) && insects < hives.size() * s.insectsPerHive) {
            if (s.spawnInsect(at, colony) != kNoEntity) {
                ++insects;
            }
        }
        if (s.due(h, 3, s.spreadDaysMin, s.spreadDaysMax) && total < s.maxHives) {
            CellPos spot;
            if (s.hiveSpot(at, spot) && spawnHive(spot) != kNoEntity) {
                ++total;
            }
        }
    }
    // Insects keep hunting while a colonist is within wake range of them.
    if (!colony.empty()) {
        for (const auto [e, animal, thing] : reg.view<const Animal, const Thing>().each()) {
            if (std::find(s.insectDefs.begin(), s.insectDefs.end(), thing.def) == s.insectDefs.end() || !s.things.spawned(e) ||
                s.health.incapacitated(e)) {
                continue;
            }
            if (animal.temper != AnimalTemper::Calm && (animal.mode == AnimalMode::Chase || animal.mode == AnimalMode::Fight)) {
                continue;
            }
            const Position& pos = reg.get<Position>(e);
            float d;
            const Entity target = s.nearestOf(colony, pos.x, pos.y, d);
            if (target != kNoEntity && d <= kMonsterWakeRange) {
                s.animals.enrage(e, AnimalTemper::Manhunter, target);
            }
        }
    }
}

}  // namespace colony

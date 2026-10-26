#include "ColonySim/Raids.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float raidDist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

float raidCurve(const std::vector<RaidRules::Point>& curve, float x) {
    if (curve.empty()) {
        return 0.0f;
    }
    if (x <= curve.front().x) {
        return curve.front().y;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (x <= curve[i].x) {
            const float span = curve[i].x - curve[i - 1].x;
            const float t = span > 0.0f ? (x - curve[i - 1].x) / span : 1.0f;
            return curve[i - 1].y + (curve[i].y - curve[i - 1].y) * t;
        }
    }
    return curve.back().y;
}

void raidReadCurve(const Def& def, const char* key, std::vector<RaidRules::Point>& out) {
    const size_t n = def.count(key);
    if (n == 0) {
        return;
    }
    out.clear();
    for (size_t i = 0; i < n; ++i) {
        const std::string base = std::string(key) + "." + std::to_string(i);
        out.push_back(RaidRules::Point{static_cast<float>(def.number((base + ".0").c_str())),
                                       static_cast<float>(def.number((base + ".1").c_str()))});
    }
}

void raidReadRules(const DefDatabase& defs, RaidRules& r) {
    const Def* def = defs.find("Rules", "RaidRules");
    if (def == nullptr) {
        return;
    }
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(def->number(key, out)); };
    const auto whole = [&](const char* key, int& out) { out = static_cast<int>(def->integer(key, out)); };
    raidReadCurve(*def, "wealthPoints", r.wealthPoints);
    raidReadCurve(*def, "pointsPerColonist", r.pointsPerColonist);
    raidReadCurve(*def, "daysPassedFactor", r.daysPassedFactor);
    num("buildingWealthFactor", r.buildingWealthFactor);
    num("threatScale", r.threatScale);
    num("adaptation", r.adaptation);
    num("minPoints", r.minPoints);
    num("maxPoints", r.maxPoints);
    num("fleeLostFraction", r.fleeLostFraction);
    num("giveUpHoursMin", r.giveUpHoursMin);
    num("giveUpHoursMax", r.giveUpHoursMax);
    whole("squadThinkEvery", r.squadThinkEvery);
    num("advanceRatio", r.advanceRatio);
    num("holdRatio", r.holdRatio);
    num("hysteresis", r.hysteresis);
    num("retargetDistance", r.retargetDistance);
    whole("edgeDepth", r.edgeDepth);
    whole("alarmThinkEvery", r.alarmThinkEvery);
    num("sightRange", r.sightRange);
    num("keepWorkingRadius", r.keepWorkingRadius);
    num("defenseLine", r.defenseLine);
    num("defenderLeash", r.defenderLeash);
    num("spotLeash", r.spotLeash);
    num("lineMoves", r.lineMoves);
    whole("shelterSearch", r.shelterSearch);
    num("shelterFlee", r.shelterFlee);
    num("weightAssault", r.weightAssault);
    num("weightSappers", r.weightSappers);
    num("weightDropPods", r.weightDropPods);
    num("weightThieves", r.weightThieves);
    num("weightKidnappers", r.weightKidnappers);
    num("blockedBoost", r.blockedBoost);
    num("dropPodsMinPoints", r.dropPodsMinPoints);
    num("dropMinDistance", r.dropMinDistance);
    num("dropMaxDistance", r.dropMaxDistance);
    num("sapDamage", r.sapDamage);
    num("sapReach", r.sapReach);
    whole("sapSearch", r.sapSearch);
    num("grabReach", r.grabReach);
    num("stealMass", r.stealMass);
    num("kidnapRange", r.kidnapRange);
    num("seizeRange", r.seizeRange);
    r.squadThinkEvery = std::max(1, r.squadThinkEvery);
    r.alarmThinkEvery = std::max(1, r.alarmThinkEvery);
    r.edgeDepth = std::max(0, r.edgeDepth);
    r.giveUpHoursMax = std::max(r.giveUpHoursMin, r.giveUpHoursMax);
}

float raidCentre(int v) {
    return static_cast<float>(v) + 0.5f;
}

}  // namespace

const char* defenseRoleName(DefenseRole role) {
    switch (role) {
        case DefenseRole::Defender: return "Defender";
        case DefenseRole::Shelter: return "Shelter";
        case DefenseRole::KeepWorking: return "KeepWorking";
    }
    return "?";
}

const char* squadStateName(SquadState state) {
    switch (state) {
        case SquadState::Advance: return "Advance";
        case SquadState::Hold: return "Hold";
        case SquadState::FallBack: return "FallBack";
        case SquadState::Rout: return "Rout";
    }
    return "?";
}

const char* raidStrategyName(RaidStrategy strategy) {
    switch (strategy) {
        case RaidStrategy::Assault: return "Assault";
        case RaidStrategy::Sappers: return "Sappers";
        case RaidStrategy::DropPods: return "DropPods";
        case RaidStrategy::Thieves: return "Thieves";
        case RaidStrategy::Kidnappers: return "Kidnappers";
    }
    return "?";
}

bool raidStrategyFromName(const std::string& name, RaidStrategy& out) {
    for (int i = 0; i < kRaidStrategyCount; ++i) {
        const RaidStrategy candidate = static_cast<RaidStrategy>(i);
        if (name == raidStrategyName(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

RaidSystem::RaidSystem(Things& things, Map& map, Rooms& rooms, CombatSystem& combat, HealthSystem& health, JobSystem& jobs,
                       FighterSystem& fighters, const DefDatabase& defs, const GameTime& time, Rng& rng)
    : things_(things),
      map_(map),
      rooms_(rooms),
      combat_(combat),
      health_(health),
      jobs_(jobs),
      fighters_(fighters),
      defs_(defs),
      time_(time),
      rng_(rng) {
    raidReadRules(defs_, rules_);
    marketValue_.assign(defs_.size(), 0.0f);
    armor_.assign(defs_.size(), 0.0f);
    for (const DefId id : defs_.idsOfKind("Things")) {
        const Def& def = defs_.get(id);
        marketValue_[id.value] = static_cast<float>(def.number("stats.marketValue", 0.0));
        if (def.has("apparel")) {
            armor_[id.value] = static_cast<float>(def.number("stats.armorRatingSharp", 0.0));
        }
    }
}

RaidSystem::~RaidSystem() = default;

int64_t RaidSystem::now() const {
    return time_.seconds;
}

// ---- Raid points ------------------------------------------------------------------------------

float RaidSystem::colonyWealth() const {
    const entt::registry& reg = things_.registry();
    double items = 0.0;
    double buildings = 0.0;
    // In id order, so the sum is the same after a load (the registry's own order is not).
    std::vector<Entity> all(reg.view<const Thing>().begin(), reg.view<const Thing>().end());
    std::sort(all.begin(), all.end());
    for (const Entity e : all) {
        const Thing& thing = reg.get<Thing>(e);
        const float value = thing.def.value < marketValue_.size() ? marketValue_[thing.def.value] : 0.0f;
        if (value <= 0.0f) {
            continue;
        }
        if (thing.category == ThingCategory::Item) {
            // On the map, or carried and worn by the player's pawns.
            const EquippedBy* by = reg.try_get<EquippedBy>(e);
            if (by != nullptr ? fighters_.factionOf(by->pawn) != kPlayerFaction : !reg.all_of<Position>(e)) {
                continue;
            }
            const StackCount* stack = reg.try_get<StackCount>(e);
            items += static_cast<double>(value) * (stack != nullptr ? stack->count : 1);
        } else if (thing.category == ThingCategory::Building && reg.all_of<Position>(e)) {
            buildings += value;
        } else if (thing.category == ThingCategory::Pawn && things_.spawned(e) && fighters_.factionOf(e) == kPlayerFaction) {
            // Colonists and tame animals count for their own value too (T1, wiki "Raid points").
            const Health* h = reg.try_get<Health>(e);
            items += h != nullptr && h->dead ? 0.0 : static_cast<double>(value);
        }
    }
    return static_cast<float>(items + buildings * rules_.buildingWealthFactor);
}

int RaidSystem::colonistCount() const {
    const entt::registry& reg = things_.registry();
    int count = 0;
    for (const auto [pawn, colonist] : reg.view<const Colonist>().each()) {
        const Health* h = reg.try_get<Health>(pawn);
        if (things_.spawned(pawn) && fighters_.factionOf(pawn) == kPlayerFaction && (h == nullptr || !h->dead)) {
            ++count;
        }
    }
    return count;
}

float RaidSystem::threatPoints(float scale) const {
    const float wealth = colonyWealth();
    const float pawnPoints = static_cast<float>(colonistCount()) * raidCurve(rules_.pointsPerColonist, wealth);
    float points = raidCurve(rules_.wealthPoints, wealth) + pawnPoints;
    const float days = static_cast<float>(now()) / static_cast<float>(kSecondsPerDay);
    points *= rules_.threatScale * raidCurve(rules_.daysPassedFactor, days) * rules_.adaptation * scale;
    return std::clamp(points, rules_.minPoints, rules_.maxPoints);
}

// ---- Commands ---------------------------------------------------------------------------------

FactionId RaidSystem::factionId(const std::string& faction) {
    for (size_t i = 0; i < state_.factions.size(); ++i) {
        if (state_.factions[i] == faction) {
            return static_cast<FactionId>(i + 2);
        }
    }
    state_.factions.push_back(faction);
    return static_cast<FactionId>(state_.factions.size() + 1);
}

bool RaidSystem::startRaid(const std::string& faction, float points, int edge, std::string& error) {
    return startRaid(faction, points, edge, RaidStrategy::Assault, -1.0f, error);
}

bool RaidSystem::startRaid(const std::string& faction, float points, int edge, RaidStrategy strategy, float hour,
                           std::string& error) {
    const Def* factionDef = defs_.find("Factions", faction);
    if (factionDef == nullptr) {
        error = "no Factions def '" + faction + "'";
        return false;
    }
    if (edge < -1 || edge > 3) {
        error = "the edge must be -1 (any) or 0 to 3";
        return false;
    }
    if (!(points >= 0.0f) || points > 1e6f) {
        error = "raid points must be 0 (from the colony) or more";
        return false;
    }
    if (!(hour < 24.0f) || (hour < 0.0f && hour != -1.0f)) {
        error = "the hour must be -1 (now) or 0 up to 24";
        return false;
    }
    if (hour >= 0.0f) {
        // Held until that hour of the day: the next one, today or tomorrow.
        const int64_t dayStart = now() - now() % kSecondsPerDay;
        int64_t at = dayStart + static_cast<int64_t>(hour * static_cast<float>(kSecondsPerHour));
        if (at <= now()) {
            at += kSecondsPerDay;
        }
        state_.pending.push_back(PendingRaid{faction, points, edge, strategy, at});
        return true;
    }
    // The kinds it can send, with their weights.
    std::vector<PawnKind> kinds;
    std::vector<float> weights;
    for (size_t i = 0; i < factionDef->count("pawnKinds"); ++i) {
        const std::string base = "pawnKinds." + std::to_string(i);
        const Def* kindDef = defs_.find("PawnKinds", factionDef->text((base + ".kind").c_str()));
        if (kindDef == nullptr) {
            error = "faction '" + faction + "' names a PawnKinds def that doesn't exist";
            return false;
        }
        PawnKind k;
        k.defName = kindDef->defName();
        k.race = kindDef->text("race", "Human");
        k.combatPower = static_cast<float>(kindDef->number("combatPower", 0.0));
        for (size_t w = 0; w < kindDef->count("weapons"); ++w) {
            k.weapons.push_back(kindDef->textAt("weapons", w));
        }
        for (size_t a = 0; a < kindDef->count("apparel"); ++a) {
            const std::string ab = "apparel." + std::to_string(a);
            k.apparel.push_back(PawnKind::Wear{kindDef->text((ab + ".thing").c_str()),
                                               static_cast<float>(kindDef->number((ab + ".chance").c_str(), 1.0))});
        }
        if (k.combatPower <= 0.0f || !defs_.idOf("Things", k.race).valid()) {
            error = "pawn kind '" + k.defName + "' has no combat power or an unknown race";
            return false;
        }
        kinds.push_back(std::move(k));
        weights.push_back(static_cast<float>(factionDef->number((base + ".weight").c_str(), 1.0)));
    }
    if (kinds.empty()) {
        error = "faction '" + faction + "' has no pawn kinds to raid with";
        return false;
    }
    entt::registry& reg = things_.registry();

    // What it goes for: the player's colonist nearest the middle of the map, else the middle.
    const float mid = static_cast<float>(map_.width()) * 0.5f;
    CellPos target{map_.width() / 2, map_.height() / 2, 0};
    bool hasTarget = false;
    float bestD = 0.0f;
    for (const auto [pawn, colonist, pos] : reg.view<const Colonist, const Position>().each()) {
        if (fighters_.factionOf(pawn) != kPlayerFaction || health_.incapacitated(pawn)) {
            continue;
        }
        const float d = raidDist(pos.x, pos.y, mid, static_cast<float>(map_.height()) * 0.5f);
        const CellPos c = cellOf(pos);
        if (!hasTarget || d < bestD || (d == bestD && (c.y < target.y || (c.y == target.y && c.x < target.x)))) {
            target = c;
            bestD = d;
            hasTarget = true;
        }
    }
    if (!hasTarget && !nearestFree(raidCentre(target.x), raidCentre(target.y), 0, target, 30, target)) {
        error = "nowhere for a raid to go";
        return false;
    }

    // Where it comes: a cell near the chosen edge that can reach the target (sappers settle for one that can't when
    // walls shut the colony off), or, for drop pods, round the target.
    CellPos entry;
    bool found = false;
    if (strategy == RaidStrategy::DropPods) {
        entry = target;
        found = true;
    } else {
        found = pickEntry(edge, target, true, entry);
        if (!found && strategy == RaidStrategy::Sappers) {
            found = pickEntry(edge, target, false, entry);
        }
    }
    if (!found) {
        error = "no way in from that edge to the colony";
        return false;
    }
    if (strategy == RaidStrategy::Thieves) {
        bool loose = false;
        for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
            if (thing.category == ThingCategory::Item) {
                loose = true;
                break;
            }
        }
        if (!loose) {
            error = "there is nothing lying about for thieves to steal";
            return false;
        }
    }

    // Buy raiders with the points, as RimWorld does: a kind at random by weight among those the
    // points left still pay for, until none does. At least one.
    if (points <= 0.0f) {
        points = threatPoints();
    }
    std::vector<size_t> chosen;
    float left = points;
    for (int guard = 0; guard < 500; ++guard) {
        float total = 0.0f;
        for (size_t i = 0; i < kinds.size(); ++i) {
            total += kinds[i].combatPower <= left ? weights[i] : 0.0f;
        }
        if (total <= 0.0f) {
            break;
        }
        float pick = rng_.unit() * total;
        size_t which = kinds.size();
        for (size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i].combatPower > left || weights[i] <= 0.0f) {
                continue;
            }
            which = i;
            if (pick < weights[i]) {
                break;
            }
            pick -= weights[i];
        }
        chosen.push_back(which);
        left -= kinds[which].combatPower;
    }
    if (chosen.empty()) {
        size_t cheapest = 0;
        for (size_t i = 1; i < kinds.size(); ++i) {
            if (kinds[i].combatPower < kinds[cheapest].combatPower) {
                cheapest = i;
            }
        }
        chosen.push_back(cheapest);
    }

    // Cells for them: round the target for drop pods, else around the entry, a metre apart (every other cell).
    std::vector<CellPos> cells;
    if (strategy == RaidStrategy::DropPods) {
        const float tx = raidCentre(target.x);
        const float ty = raidCentre(target.y);
        for (size_t i = 0; i < chosen.size(); ++i) {
            for (int attempt = 0; attempt < 120; ++attempt) {
                const float dx = (rng_.unit() * 2.0f - 1.0f) * rules_.dropMaxDistance;
                const float dy = (rng_.unit() * 2.0f - 1.0f) * rules_.dropMaxDistance;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d < rules_.dropMinDistance || d > rules_.dropMaxDistance) {
                    continue;
                }
                const CellPos c{static_cast<int>(std::floor(tx + dx)), static_cast<int>(std::floor(ty + dy)), 0};
                if (map_.onGround(c) && map_.walkable(c) && map_.obstacleDm(c) == 0 && map_.buildingAt(c) == kNoEntity &&
                    std::find(cells.begin(), cells.end(), c) == cells.end()) {
                    cells.push_back(c);
                    break;
                }
            }
        }
        if (!cells.empty()) {
            entry = cells.front();
        }
    } else {
        std::vector<CellPos> open{entry};
        std::set<int> seen{map_.index(entry)};
        for (size_t head = 0; head < open.size() && cells.size() < chosen.size() && open.size() < 20000; ++head) {
            const CellPos c = open[head];
            if ((c.x + c.y) % 2 == 0 && map_.obstacleDm(c) == 0 && !map_.isDoor(c)) {
                cells.push_back(c);
            }
            const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
            for (const CellPos& n : around) {
                if (map_.walkable(n) && seen.insert(map_.index(n)).second) {
                    open.push_back(n);
                }
            }
        }
    }
    if (cells.size() < chosen.size()) {
        error = strategy == RaidStrategy::DropPods ? "no room for the drop pods near the colony" : "no room at the edge for the raid";
        return false;
    }

    Squad squad;
    squad.id = state_.nextSquad++;
    squad.faction = faction;
    squad.side = factionId(faction);
    squad.points = points;
    squad.strategy = strategy;
    squad.arrivedAt = now();
    const float hourSeconds = static_cast<float>(kSecondsPerHour);
    squad.giveUpAt = now() + static_cast<int64_t>((rules_.giveUpHoursMin + rng_.unit() * (rules_.giveUpHoursMax - rules_.giveUpHoursMin)) * hourSeconds);
    squad.entry = entry;
    squad.hasTarget = true;
    squad.target = target;
    squad.nextThink = now() + rules_.squadThinkEvery;
    for (size_t i = 0; i < chosen.size(); ++i) {
        const PawnKind& kind = kinds[chosen[i]];
        const CellPos cell = cells[i];
        const Entity pawn = things_.spawn(kind.race, cell);
        if (pawn == kNoEntity || !reg.all_of<Mover, Posture>(pawn)) {
            things_.destroy(pawn);
            continue;
        }
        // A whole person, made like a colonist; raiders who won't fight are made again.
        const uint64_t base = splitMix64((static_cast<uint64_t>(rng_.next()) << 32) ^ rng_.next());
        Colonist person;
        for (uint64_t tries = 0; tries < 20; ++tries) {
            std::string ignored;
            if (!generateColonist(defs_, base + tries, false, person, ignored)) {
                break;
            }
            if (std::find(person.disabledWorkTags.begin(), person.disabledWorkTags.end(), "Violent") == person.disabledWorkTags.end()) {
                break;
            }
        }
        if (looks_) {
            looks_(pawn, base, person.female);
        }
        reg.emplace<Colonist>(pawn, std::move(person));
        reg.emplace<Faction>(pawn, Faction{squad.side});
        reg.emplace<Raider>(pawn, Raider{squad.id, kind.defName, kNoEntity, kNoEntity, strategy == RaidStrategy::Kidnappers});
        if (!kind.weapons.empty()) {
            const std::string& gun = kind.weapons[rng_.below(static_cast<uint32_t>(kind.weapons.size()))];
            const Entity item = things_.spawn(gun, cell);
            if (item != kNoEntity && !combat_.equip(pawn, item)) {
                things_.destroy(item);
            }
        }
        for (const PawnKind::Wear& wear : kind.apparel) {
            if (rng_.unit() < wear.chance) {
                const Entity item = things_.spawn(wear.thing, cell);
                if (item != kNoEntity && !combat_.equip(pawn, item)) {
                    things_.destroy(item);
                }
            }
        }
        Fighter& f = fighters_.enlist(pawn, "raiding");
        f.advances = true;
        f.squad = squad.id;
        f.hasGoal = true;
        f.goal = target;
        squad.members.push_back(pawn);
        if (strategy == RaidStrategy::DropPods && events_ != nullptr) {
            Event landed;
            landed.type = EventType::DropPodLanded;
            landed.thing = pawn;
            landed.def = reg.get<Thing>(pawn).def;
            landed.cell = cell;
            landed.squad = squad.id;
            events_->push_back(std::move(landed));
        }
    }
    std::sort(squad.members.begin(), squad.members.end());
    squad.started = static_cast<int>(squad.members.size());
    if (squad.started == 0) {
        error = "the raiders couldn't be made";
        return false;
    }
    squadEvent(squad, EventType::RaidArrived, faction);
    state_.squads.push_back(std::move(squad));
    return true;
}

// A cell near the chosen edge (-1: any) where a raid can walk in, that can reach `target` when `needReach`. A raid
// with no chosen edge tries the others in turn when the first has no way in (water, rock).
bool RaidSystem::pickEntry(int edge, CellPos target, bool needReach, CellPos& entry) {
    const int w = map_.width();
    const int h = map_.height();
    const int firstSide = edge >= 0 ? edge : static_cast<int>(rng_.below(4));
    Regions& regions = map_.regions();
    for (int attempt = 0; attempt < (edge >= 0 ? 40 : 160); ++attempt) {
        const int side = (firstSide + attempt / 40) % 4;
        const int along = static_cast<int>(rng_.below(static_cast<uint32_t>(side % 2 == 0 ? w : h)));
        const int d = rules_.edgeDepth;
        const CellPos guess = side == 0 ? CellPos{along, h - 1 - d, 0}
                            : side == 1 ? CellPos{w - 1 - d, along, 0}
                            : side == 2 ? CellPos{along, d, 0}
                                        : CellPos{d, along, 0};
        CellPos c;
        if (nearestFree(raidCentre(guess.x), raidCentre(guess.y), 0, CellPos{-1, -1, 0}, 6, c) && (!needReach || regions.canReach(c, target))) {
            entry = c;
            return true;
        }
    }
    return false;
}

bool RaidSystem::soundAlarm(bool on, std::string& error) {
    if (on) {
        if (state_.alarm && state_.manual) {
            error = "the alarm is on already";
            return false;
        }
        state_.manual = true;
        state_.suppressed = false;
        setAlarm(true, "sounded");
        state_.nextAlarmThink = now();
        return true;
    }
    if (!state_.alarm) {
        error = "the alarm is off already";
        return false;
    }
    state_.manual = false;
    // Enemies still about: don't sound it again by itself until they are gone.
    const entt::registry& reg = things_.registry();
    for (const auto [pawn, faction] : reg.view<const Faction>().each()) {
        if (faction.id != kPlayerFaction && !reg.all_of<Animal>(pawn) && !reg.all_of<Prisoner>(pawn) && things_.spawned(pawn) &&
            !health_.incapacitated(pawn)) {
            state_.suppressed = true;
            break;
        }
    }
    setAlarm(false, "stopped");
    return true;
}

bool RaidSystem::setRole(Entity pawn, DefenseRole role, bool hasSpot, CellPos spot, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Colonist, Mover>(pawn) || fighters_.factionOf(pawn) != kPlayerFaction) {
        error = "that isn't one of the colony's pawns";
        return false;
    }
    if (static_cast<int>(role) > static_cast<int>(DefenseRole::KeepWorking)) {
        error = "no such defense role";
        return false;
    }
    if (hasSpot && (role != DefenseRole::Defender || !map_.walkable(spot))) {
        error = role != DefenseRole::Defender ? "only a Defender has a defense spot" : "nobody can stand there";
        return false;
    }
    DefensePlan plan{role, hasSpot, hasSpot ? spot : CellPos{}};
    reg.emplace_or_replace<DefensePlan>(pawn, plan);
    // Called up already: it takes its new role at the next alarm check.
    if (const Fighter* f = reg.try_get<Fighter>(pawn); f != nullptr && f->defending) {
        fighters_.standDown(pawn);
        state_.nextAlarmThink = now();
    }
    return true;
}

bool RaidSystem::markShelter(CellPos cell, bool clear, std::string& error) {
    if (clear) {
        if (!state_.hasShelter) {
            error = "no shelter is marked";
            return false;
        }
        state_.hasShelter = false;
        return true;
    }
    if (!map_.walkable(cell) || !rooms_.indoors(cell)) {
        error = "a shelter has to be a room under a roof";
        return false;
    }
    state_.hasShelter = true;
    state_.shelter = cell;
    return true;
}

DefenseRole RaidSystem::roleOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const DefensePlan* plan = reg.valid(pawn) ? reg.try_get<DefensePlan>(pawn) : nullptr;
    return plan != nullptr ? plan->role : DefenseRole::Defender;
}

// ---- Each step --------------------------------------------------------------------------------

void RaidSystem::step() {
    for (Squad& squad : state_.squads) {
        if (now() >= squad.nextThink) {
            squad.nextThink = now() + rules_.squadThinkEvery;
            thinkSquad(squad);
        }
        if (squad.state == SquadState::Rout) {
            leaveAtEdge(squad);
        }
    }
    stepStrategies();
    // A squad with nobody left standing on the map is over (its downed stay where they fell).
    state_.squads.erase(std::remove_if(state_.squads.begin(), state_.squads.end(),
                                       [&](const Squad& s) {
                                           for (const Entity m : s.members) {
                                               if (things_.spawned(m) && !health_.incapacitated(m)) {
                                                   return false;
                                               }
                                           }
                                           return true;
                                       }),
                        state_.squads.end());
    if (now() >= state_.nextAlarmThink) {
        state_.nextAlarmThink = now() + rules_.alarmThinkEvery;
        thinkAlarm();
    }
}

// How much a pawn counts in a fight: its health, its gun and its armor (ours: RimWorld's squads
// don't weigh strength, they only count losses).
float RaidSystem::strength(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* h = reg.try_get<Health>(pawn);
    if (h != nullptr && h->incapacitated()) {
        return 0.0f;
    }
    float s = h != nullptr ? std::clamp((1.0f - h->pain) * std::min(1.0f, h->movingFactor), 0.1f, 1.0f) : 1.0f;
    const Gear* gear = reg.try_get<Gear>(pawn);
    s *= gear != nullptr && gear->weapon != kNoEntity ? 1.0f : 0.3f;
    if (gear != nullptr) {
        float armor = 0.0f;
        for (const Entity worn : gear->apparel) {
            const DefId def = reg.get<Thing>(worn).def;
            armor += def.value < armor_.size() ? armor_[def.value] : 0.0f;
        }
        s *= 1.0f + 0.5f * armor;
    }
    return s;
}

void RaidSystem::thinkSquad(Squad& squad) {
    entt::registry& reg = things_.registry();
    squad.members.erase(std::remove_if(squad.members.begin(), squad.members.end(),
                                       [&](Entity m) {
                                           const Raider* r = reg.valid(m) ? reg.try_get<Raider>(m) : nullptr;
                                           return r == nullptr || r->squad != squad.id;
                                       }),
                        squad.members.end());
    if (squad.state == SquadState::Rout) {
        return;
    }
    int able = 0;
    float own = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    std::vector<Entity> enemies;
    for (const Entity m : squad.members) {
        if (!things_.spawned(m) || health_.incapacitated(m)) {
            continue;
        }
        ++able;
        own += strength(m);
        const Position& p = reg.get<Position>(m);
        cx += p.x;
        cy += p.y;
        if (const Fighter* f = reg.try_get<Fighter>(m)) {
            for (const KnownEnemy& k : f->known) {
                enemies.push_back(k.pawn);
            }
        }
    }
    if (able == 0) {
        return;
    }
    cx /= static_cast<float>(able);
    cy /= static_cast<float>(able);
    std::sort(enemies.begin(), enemies.end());
    enemies.erase(std::unique(enemies.begin(), enemies.end()), enemies.end());
    float theirs = 0.0f;
    for (const Entity e : enemies) {
        if (things_.spawned(e)) {
            theirs += strength(e);
        }
    }

    // RimWorld's rule: a raid that has lost enough of its pawns flees; one that has been at it too
    // long gives up. Otherwise morale follows the strength it sees against it.
    SquadState next = squad.state;
    const int lost = squad.started - able;
    if (static_cast<float>(lost) >= rules_.fleeLostFraction * static_cast<float>(squad.started) && lost > 0) {
        next = SquadState::Rout;
    } else if (now() >= squad.giveUpAt) {
        next = SquadState::Rout;
        squad.gaveUp = true;
    } else {
        // A margin in favour of the state it is in, so it doesn't flip back and forth at a threshold.
        const float up = 1.0f + rules_.hysteresis;
        const float down = 1.0f - rules_.hysteresis;
        const float advanceAt = rules_.advanceRatio * (squad.state == SquadState::Advance ? down : up);
        const float holdAt = rules_.holdRatio * (squad.state == SquadState::FallBack ? up : down);
        if (theirs <= 0.0f || own >= advanceAt * theirs) {
            next = SquadState::Advance;
        } else if (own >= holdAt * theirs) {
            next = SquadState::Hold;
        } else {
            next = SquadState::FallBack;
        }
    }

    // The target: the colony's pawn nearest the squad, moved only when it has gone a fair way.
    bool found = false;
    CellPos nearest;
    float bestD = 0.0f;
    for (const auto [pawn, pos, mover] : reg.view<const Position, const Mover>().each()) {
        if (fighters_.factionOf(pawn) != kPlayerFaction || reg.all_of<Animal>(pawn) || health_.incapacitated(pawn)) {
            continue;
        }
        const float d = raidDist(pos.x, pos.y, cx, cy);
        const CellPos c = cellOf(pos);
        if (!found || d < bestD || (d == bestD && (c.y < nearest.y || (c.y == nearest.y && c.x < nearest.x)))) {
            nearest = c;
            bestD = d;
            found = true;
        }
    }
    const bool retarget = found && (!squad.hasTarget || raidDist(raidCentre(nearest.x), raidCentre(nearest.y), raidCentre(squad.target.x),
                                                                raidCentre(squad.target.y)) > rules_.retargetDistance);
    if (retarget) {
        squad.hasTarget = true;
        squad.target = nearest;
    }

    const bool changed = next != squad.state;
    squad.state = next;
    if (next != SquadState::Rout) {
        switch (squad.strategy) {
            case RaidStrategy::Sappers: thinkBreach(squad); break;
            case RaidStrategy::Thieves: thinkLoot(squad); break;
            case RaidStrategy::Kidnappers: thinkKidnap(squad); break;
            case RaidStrategy::Assault:
            case RaidStrategy::DropPods: break;
        }
    }
    for (const Entity m : squad.members) {
        Fighter* f = reg.try_get<Fighter>(m);
        if (f == nullptr || !things_.spawned(m)) {
            continue;
        }
        if (next == SquadState::Rout) {
            f->mode = FightMode::Flee;
            f->hasGoal = true;
            f->goal = exitCell(m, squad);
            f->retreat = false;
            fighters_.replan(m);
            continue;
        }
        const Raider& raider = reg.get<Raider>(m);
        if (raider.loot != kNoEntity) {
            // Loaded with loot: it runs for the edge whatever the odds.
            if (f->mode != FightMode::Flee) {
                flee(m, squad);
            }
            continue;
        }
        f->advances = next == SquadState::Advance;
        if (f->retreat != (next == SquadState::FallBack)) {
            f->retreat = next == SquadState::FallBack;
            f->nextPlan = std::min(f->nextPlan, now());
        }
        // Where it heads while it knows no enemy: the squad's target, the wall it digs through, or what it is after.
        CellPos goal = squad.target;
        bool hasGoal = squad.hasTarget;
        if (squad.hasBreach) {
            goal = squad.breachFrom;
            hasGoal = true;
        } else if (raider.goalThing != kNoEntity && things_.spawned(raider.goalThing)) {
            goal = cellOf(reg.get<Position>(raider.goalThing));
            hasGoal = true;
        }
        if (hasGoal && (!f->hasGoal || f->goal != goal)) {
            f->hasGoal = true;
            f->goal = goal;
        }
        if (squad.strategy == RaidStrategy::Thieves) {
            // A thief goes for its loot whatever is about, and fights like the rest when there is none.
            const bool seizing = raider.goalThing != kNoEntity && things_.spawned(raider.goalThing);
            if (seizing != (f->mode == FightMode::Seize)) {
                f->mode = seizing ? FightMode::Seize : FightMode::Fight;
                fighters_.replan(m);
            }
        }
    }
    if (changed) {
        squadEvent(squad, EventType::SquadChanged, squad.gaveUp && next == SquadState::Rout ? "gave up" : squadStateName(next));
    }
}

// The nearest edge cell it can reach, looked for along the edge nearest to it, then the others.
CellPos RaidSystem::exitCell(Entity pawn, const Squad& squad) {
    const Position& p = things_.registry().get<Position>(pawn);
    const CellPos here = cellOf(p);
    const int w = map_.width();
    const int h = map_.height();
    struct Side {
        float d;
        int edge;
    };
    Side sides[4] = {{static_cast<float>(h - 1 - here.y), 0}, {static_cast<float>(w - 1 - here.x), 1},
                     {static_cast<float>(here.y), 2}, {static_cast<float>(here.x), 3}};
    std::sort(std::begin(sides), std::end(sides), [](const Side& a, const Side& b) { return a.d != b.d ? a.d < b.d : a.edge < b.edge; });
    Regions& regions = map_.regions();
    for (const Side& s : sides) {
        for (int off = 0; off <= 40; ++off) {
            for (const int sign : {1, -1}) {
                const int along = (s.edge % 2 == 0 ? here.x : here.y) + sign * off;
                const CellPos c = s.edge == 0 ? CellPos{along, h - 1, 0}
                                : s.edge == 1 ? CellPos{w - 1, along, 0}
                                : s.edge == 2 ? CellPos{along, 0, 0}
                                              : CellPos{0, along, 0};
                if (map_.inBounds(c) && map_.walkable(c) && regions.canReach(here, c)) {
                    return c;
                }
            }
        }
    }
    return squad.entry;
}

// Routed raiders who reach the edge of the map leave it, with what they carry.
void RaidSystem::leaveAtEdge(Squad& squad) {
    entt::registry& reg = things_.registry();
    const int w = map_.width();
    const int h = map_.height();
    std::vector<Entity> leaving;
    for (const Entity m : squad.members) {
        if (!things_.spawned(m) || health_.incapacitated(m)) {
            continue;
        }
        const CellPos c = cellOf(reg.get<Position>(m));
        if (c.x <= 1 || c.y <= 1 || c.x >= w - 2 || c.y >= h - 2) {
            leaving.push_back(m);
        }
    }
    for (const Entity m : leaving) {
        squad.members.erase(std::find(squad.members.begin(), squad.members.end(), m));
        leaveMap(m);
    }
}

void RaidSystem::leaveMap(Entity pawn) {
    entt::registry& reg = things_.registry();
    // What it carries goes with it: a stolen item vanishes, a kidnapped colonist is gone for good.
    if (Raider* raider = reg.try_get<Raider>(pawn); raider != nullptr && raider->loot != kNoEntity) {
        const Entity loot = raider->loot;
        raider->loot = kNoEntity;
        if (reg.valid(loot) && reg.all_of<Kidnapped>(loot)) {
            if (events_ != nullptr) {
                Event e;
                e.type = EventType::PawnKidnapped;
                e.thing = pawn;
                e.target = loot;
                e.def = reg.get<Thing>(loot).def;
                e.cell = cellOf(reg.get<Position>(pawn));
                e.squad = raider->squad;
                e.message = "left the map";
                events_->push_back(std::move(e));
            }
            removeForGood(loot);
        } else if (reg.valid(loot)) {
            reg.remove<Carrying>(pawn);
            things_.destroy(loot);
        }
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::PawnLeftMap;
        e.thing = pawn;
        e.def = reg.get<Thing>(pawn).def;
        e.cell = cellOf(reg.get<Position>(pawn));
        if (const Raider* r = reg.try_get<Raider>(pawn)) {
            e.squad = r->squad;
        }
        events_->push_back(std::move(e));
    }
    if (const Gear* gear = reg.try_get<Gear>(pawn)) {
        std::vector<Entity> carried = gear->apparel;
        carried.insert(carried.end(), gear->stowed.begin(), gear->stowed.end());
        if (gear->weapon != kNoEntity) {
            carried.push_back(gear->weapon);
        }
        for (const Entity item : carried) {
            things_.destroy(item);
        }
    }
    jobs_.forget(pawn);
    fighters_.forget(pawn);
    things_.destroy(pawn);
}

// ---- Strategies (F12, docs/design/raid-strategies.md) ----------------------------------------------

// The lowest-id able colonist of the player, else none.
static Entity raidFirstColonist(const entt::registry& reg, const FighterSystem& fighters, const HealthSystem& health) {
    Entity first = kNoEntity;
    for (const auto [pawn, colonist, pos] : reg.view<const Colonist, const Position>().each()) {
        if (fighters.factionOf(pawn) != kPlayerFaction || health.incapacitated(pawn)) {
            continue;
        }
        if (first == kNoEntity || pawn < first) {
            first = pawn;
        }
    }
    return first;
}

// True when no edge's middle can reach the colony: walls (or water) shut it off.
bool RaidSystem::colonyWalledOff() {
    const entt::registry& reg = things_.registry();
    const Entity colonist = raidFirstColonist(reg, fighters_, health_);
    if (colonist == kNoEntity) {
        return false;
    }
    const CellPos target = cellOf(reg.get<Position>(colonist));
    const int w = map_.width();
    const int h = map_.height();
    const int d = rules_.edgeDepth;
    const CellPos mids[4] = {{w / 2, h - 1 - d, 0}, {w - 1 - d, h / 2, 0}, {w / 2, d, 0}, {d, h / 2, 0}};
    Regions& regions = map_.regions();
    for (const CellPos& mid : mids) {
        CellPos c;
        if (nearestFree(raidCentre(mid.x), raidCentre(mid.y), 0, CellPos{-1, -1, 0}, 30, c) && regions.canReach(c, target)) {
            return false;
        }
    }
    return true;
}

RaidStrategy RaidSystem::pickStrategy(float points) {
    if (!(points > 0.0f)) {
        points = threatPoints();
    }
    const entt::registry& reg = things_.registry();
    float weight[kRaidStrategyCount] = {rules_.weightAssault, rules_.weightSappers, rules_.weightDropPods, rules_.weightThieves,
                                        rules_.weightKidnappers};
    if (colonyWalledOff()) {
        weight[static_cast<int>(RaidStrategy::Assault)] = 0.0f;
        weight[static_cast<int>(RaidStrategy::Thieves)] = 0.0f;
        weight[static_cast<int>(RaidStrategy::Kidnappers)] = 0.0f;
        weight[static_cast<int>(RaidStrategy::Sappers)] *= rules_.blockedBoost;
    }
    if (points < rules_.dropPodsMinPoints) {
        weight[static_cast<int>(RaidStrategy::DropPods)] = 0.0f;
    }
    bool loose = false;
    for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        if (thing.category == ThingCategory::Item) {
            loose = true;
            break;
        }
    }
    if (!loose) {
        weight[static_cast<int>(RaidStrategy::Thieves)] = 0.0f;
    }
    if (raidFirstColonist(reg, fighters_, health_) == kNoEntity) {
        weight[static_cast<int>(RaidStrategy::Kidnappers)] = 0.0f;
    }
    float total = 0.0f;
    for (const float w : weight) {
        total += w;
    }
    if (!(total > 0.0f)) {
        return RaidStrategy::Assault;
    }
    float pick = rng_.unit() * total;
    for (int i = 0; i < kRaidStrategyCount; ++i) {
        if (weight[i] <= 0.0f) {
            continue;
        }
        if (pick < weight[i]) {
            return static_cast<RaidStrategy>(i);
        }
        pick -= weight[i];
    }
    return RaidStrategy::Assault;
}

bool RaidSystem::blockedFromColony(CellPos from, CellPos target) const {
    return !map_.regions().canReach(from, target);
}

// The wall to dig through: of the buildings that block the way and touch the ground the raiders can walk on, the one
// nearest the target, walls before anything else. `approach` is the cell beside it they dig from.
bool RaidSystem::findBreach(const Squad& squad, CellPos from, CellPos& wall, CellPos& approach) const {
    if (!map_.onGround(from) || !squad.hasTarget) {
        return false;
    }
    const entt::registry& reg = things_.registry();
    const int w = map_.width();
    const int h = map_.height();
    std::vector<uint8_t> seen(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    std::vector<CellPos> open{from};
    seen[static_cast<size_t>(map_.index(from))] = 1;
    bool have[2] = {false, false};
    float best[2] = {0.0f, 0.0f};
    CellPos wallAt[2];
    CellPos fromAt[2];
    for (size_t head = 0; head < open.size() && head < static_cast<size_t>(rules_.sapSearch); ++head) {
        const CellPos c = open[head];
        const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
        for (const CellPos& n : around) {
            if (!map_.onGround(n)) {
                continue;
            }
            if (map_.walkable(n)) {
                uint8_t& mark = seen[static_cast<size_t>(map_.index(n))];
                if (mark == 0) {
                    mark = 1;
                    open.push_back(n);
                }
                continue;
            }
            const Entity building = map_.buildingAt(n);
            if (building == kNoEntity || !reg.all_of<HitPoints>(building) || map_.passage(n) != Passage::Blocked) {
                continue;
            }
            const int tier = defs_.get(reg.get<Thing>(building).def).flag("building.holdsRoof") ? 0 : 1;
            const float dx = static_cast<float>(n.x - squad.target.x);
            const float dy = static_cast<float>(n.y - squad.target.y);
            const float d = dx * dx + dy * dy;
            if (!have[tier] || d < best[tier]) {
                have[tier] = true;
                best[tier] = d;
                wallAt[tier] = n;
                fromAt[tier] = c;
            }
        }
    }
    const int tier = have[0] ? 0 : have[1] ? 1 : -1;
    if (tier < 0) {
        return false;
    }
    wall = wallAt[tier];
    approach = fromAt[tier];
    return true;
}

// Sappers: when walls shut the colony off from where the squad stands, pick the wall to dig through.
void RaidSystem::thinkBreach(Squad& squad) {
    entt::registry& reg = things_.registry();
    if (squad.hasBreach) {
        if (map_.buildingAt(squad.breach) != kNoEntity) {
            return;
        }
        squad.hasBreach = false;
    }
    if (!squad.hasTarget) {
        return;
    }
    Entity lead = kNoEntity;
    for (const Entity m : squad.members) {
        if (things_.spawned(m) && !health_.incapacitated(m) && reg.get<Raider>(m).loot == kNoEntity) {
            lead = m;
            break;
        }
    }
    if (lead == kNoEntity) {
        return;
    }
    const CellPos from = cellOf(reg.get<Position>(lead));
    if (!blockedFromColony(from, squad.target)) {
        return;
    }
    CellPos wall;
    CellPos approach;
    if (!findBreach(squad, from, wall, approach)) {
        return;
    }
    squad.hasBreach = true;
    squad.breach = wall;
    squad.breachFrom = approach;
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::SappersBreaking;
        e.cell = wall;
        e.squad = squad.id;
        e.count = static_cast<int>(squad.members.size());
        events_->push_back(std::move(e));
    }
}

// Thieves: each one without a haul goes for the most valuable loose item nobody else is going for that it can reach.
void RaidSystem::thinkLoot(Squad& squad) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> claimed;
    for (const Entity m : squad.members) {
        const Raider& r = reg.get<Raider>(m);
        if (r.goalThing != kNoEntity && things_.spawned(r.goalThing)) {
            claimed.push_back(r.goalThing);
        }
    }
    std::sort(claimed.begin(), claimed.end());
    struct Candidate {
        float value;
        Entity item;
    };
    std::vector<Candidate> candidates;
    bool listed = false;
    Regions& regions = map_.regions();
    for (const Entity m : squad.members) {
        Raider& r = reg.get<Raider>(m);
        if (r.loot != kNoEntity || !things_.spawned(m) || health_.incapacitated(m)) {
            continue;
        }
        if (r.goalThing != kNoEntity && things_.spawned(r.goalThing)) {
            continue;
        }
        r.goalThing = kNoEntity;
        if (!listed) {
            listed = true;
            for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
                if (thing.category != ThingCategory::Item || std::binary_search(claimed.begin(), claimed.end(), item)) {
                    continue;
                }
                const float market = thing.def.value < marketValue_.size() ? marketValue_[thing.def.value] : 0.0f;
                candidates.push_back(Candidate{(market > 0.0f ? market : 0.1f) * static_cast<float>(stack.count), item});
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                return a.value != b.value ? a.value > b.value : a.item < b.item;
            });
        }
        const CellPos here = cellOf(reg.get<Position>(m));
        for (size_t i = 0; i < candidates.size() && i < 12; ++i) {
            if (candidates[i].item == kNoEntity) {
                continue;
            }
            const CellPos there = cellOf(reg.get<Position>(candidates[i].item));
            if (regions.canReach(here, there)) {
                r.goalThing = candidates[i].item;
                candidates[i].item = kNoEntity;  // taken
                break;
            }
        }
    }
}

// Kidnappers: each one without a captive goes for the nearest downed colonist (never the protagonist) in range.
void RaidSystem::thinkKidnap(Squad& squad) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> claimed;
    for (const Entity m : squad.members) {
        const Raider& r = reg.get<Raider>(m);
        if (r.goalThing != kNoEntity) {
            claimed.push_back(r.goalThing);
        }
    }
    Regions& regions = map_.regions();
    for (const Entity m : squad.members) {
        Raider& r = reg.get<Raider>(m);
        if (r.loot != kNoEntity || !things_.spawned(m) || health_.incapacitated(m)) {
            continue;
        }
        if (r.goalThing != kNoEntity) {
            const Entity v = r.goalThing;
            const Health* vh = reg.valid(v) ? reg.try_get<Health>(v) : nullptr;
            if (vh != nullptr && vh->downed && !vh->dead && things_.spawned(v) && !reg.all_of<Kidnapped>(v)) {
                continue;
            }
            claimed.erase(std::remove(claimed.begin(), claimed.end(), v), claimed.end());
            r.goalThing = kNoEntity;
        }
        const Position& mp = reg.get<Position>(m);
        const CellPos here = cellOf(mp);
        Entity best = kNoEntity;
        float bestD = 0.0f;
        for (const auto [pawn, colonist, pos, health] : reg.view<const Colonist, const Position, const Health>().each()) {
            if (!health.downed || health.dead || colonist.protagonist || fighters_.factionOf(pawn) != kPlayerFaction ||
                reg.all_of<Kidnapped>(pawn) || reg.all_of<Animal>(pawn) ||
                std::find(claimed.begin(), claimed.end(), pawn) != claimed.end()) {
                continue;
            }
            const float d = raidDist(pos.x, pos.y, mp.x, mp.y);
            if (d > rules_.kidnapRange || (best != kNoEntity && (d > bestD || (d == bestD && pawn > best)))) {
                continue;
            }
            if (!regions.canReach(here, cellOf(pos))) {
                continue;
            }
            best = pawn;
            bestD = d;
        }
        if (best != kNoEntity) {
            r.goalThing = best;
            claimed.push_back(best);
        }
    }
}

// A raider can't carry on (down, dead): the stolen item lies where it fell, the colonist is put down.
void RaidSystem::dropLoot(Entity pawn) {
    entt::registry& reg = things_.registry();
    Raider* r = reg.try_get<Raider>(pawn);
    if (r == nullptr || r->loot == kNoEntity) {
        return;
    }
    const Entity loot = r->loot;
    r->loot = kNoEntity;
    if (!reg.valid(loot)) {
        return;
    }
    if (reg.all_of<Kidnapped>(loot)) {
        reg.remove<Kidnapped>(loot);
        return;
    }
    if (const Carrying* carrying = reg.try_get<Carrying>(pawn); carrying != nullptr && carrying->thing == loot) {
        reg.remove<Carrying>(pawn);
    }
    if (!things_.spawnAt(loot, cellOf(reg.get<Position>(pawn)))) {
        things_.destroy(loot);
    }
}

// Runs for the nearest edge it can reach.
void RaidSystem::flee(Entity pawn, Squad& squad) {
    Fighter* f = things_.registry().try_get<Fighter>(pawn);
    if (f == nullptr) {
        return;
    }
    f->mode = FightMode::Flee;
    f->hasGoal = true;
    f->goal = exitCell(pawn, squad);
    f->retreat = false;
    fighters_.replan(pawn);
}

// Every second: night raids start when their hour comes, sappers dig, thieves and kidnappers lift what they came
// for, carry it, and leave the map with it.
void RaidSystem::stepStrategies() {
    entt::registry& reg = things_.registry();
    for (size_t i = 0; i < state_.pending.size();) {
        if (now() < state_.pending[i].at) {
            ++i;
            continue;
        }
        const PendingRaid pending = state_.pending[i];
        state_.pending.erase(state_.pending.begin() + static_cast<std::ptrdiff_t>(i));
        std::string error;
        if (!startRaid(pending.faction, pending.points, pending.edge, pending.strategy, -1.0f, error) && events_ != nullptr) {
            Event e;
            e.type = EventType::CommandFailed;
            e.message = "the " + std::string(raidStrategyName(pending.strategy)) + " raid waiting for its hour couldn't start: " + error;
            events_->push_back(std::move(e));
        }
    }
    const int w = map_.width();
    const int h = map_.height();
    for (Squad& squad : state_.squads) {
        if (squad.hasBreach) {
            const Entity wall = map_.buildingAt(squad.breach);
            if (wall == kNoEntity) {
                squad.hasBreach = false;
                squad.nextThink = now();
            } else {
                int diggers = 0;
                const float cx = raidCentre(squad.breach.x);
                const float cy = raidCentre(squad.breach.y);
                for (const Entity m : squad.members) {
                    if (!things_.spawned(m) || health_.incapacitated(m)) {
                        continue;
                    }
                    const Position& p = reg.get<Position>(m);
                    if (p.level == squad.breach.level && raidDist(p.x, p.y, cx, cy) <= rules_.sapReach) {
                        ++diggers;
                    }
                }
                if (diggers > 0 && combat_.damageBuilding(wall, static_cast<int>(std::lround(static_cast<float>(diggers) * rules_.sapDamage)))) {
                    squad.hasBreach = false;
                    squad.nextThink = now();
                }
            }
        }
        const std::vector<Entity> members = squad.members;
        for (const Entity m : members) {
            Raider* r = reg.valid(m) ? reg.try_get<Raider>(m) : nullptr;
            if (r == nullptr || !things_.spawned(m)) {
                continue;
            }
            const bool down = health_.incapacitated(m);
            if (r->loot != kNoEntity) {
                if (down) {
                    dropLoot(m);
                    continue;
                }
                const Position& p = reg.get<Position>(m);
                if (things_.spawned(r->loot) && reg.all_of<Kidnapped>(r->loot)) {
                    Position& v = reg.get<Position>(r->loot);
                    v.x = p.x;
                    v.y = p.y;
                    v.level = p.level;
                }
                const CellPos c = cellOf(p);
                if (c.x <= 1 || c.y <= 1 || c.x >= w - 2 || c.y >= h - 2) {
                    squad.members.erase(std::find(squad.members.begin(), squad.members.end(), m));
                    leaveMap(m);
                }
                continue;
            }
            if (r->goalThing == kNoEntity || down) {
                continue;
            }
            const Entity goal = r->goalThing;
            const Position& p = reg.get<Position>(m);
            if (!things_.spawned(goal)) {
                r->goalThing = kNoEntity;
                squad.nextThink = now();
                continue;
            }
            const Position& gp = reg.get<Position>(goal);
            const float away = gp.level != p.level ? 1e9f : raidDist(gp.x, gp.y, p.x, p.y);
            if (squad.strategy == RaidStrategy::Kidnappers) {
                // Close to the downed colonist it stops fighting and goes to lift it; further off it fights again.
                Fighter* kf = reg.try_get<Fighter>(m);
                if (kf != nullptr && away <= rules_.seizeRange && kf->mode == FightMode::Fight) {
                    kf->mode = FightMode::Seize;
                    kf->hasGoal = true;
                    kf->goal = cellOf(gp);
                    fighters_.replan(m);
                } else if (kf != nullptr && away > rules_.seizeRange + 3.0f && kf->mode == FightMode::Seize) {
                    kf->mode = FightMode::Fight;
                    fighters_.replan(m);
                }
            }
            if (away > rules_.grabReach) {
                continue;
            }
            if (squad.strategy == RaidStrategy::Thieves) {
                const DefId def = reg.get<Thing>(goal).def;
                const int stack = reg.get<StackCount>(goal).count;
                const double mass = std::max(0.001, defs_.get(def).number("stats.mass", 1.0));
                const int count = std::clamp(static_cast<int>(static_cast<double>(rules_.stealMass) / mass), 1, stack);
                Entity carried = goal;
                if (count < stack) {
                    carried = things_.splitOff(goal, count);
                } else {
                    things_.despawn(goal);
                }
                r->goalThing = kNoEntity;
                if (carried == kNoEntity) {
                    squad.nextThink = now();
                    continue;
                }
                reg.emplace_or_replace<Carrying>(m, Carrying{carried});
                r->loot = carried;
                if (events_ != nullptr) {
                    Event e;
                    e.type = EventType::ItemStolen;
                    e.thing = m;
                    e.target = carried;
                    e.def = def;
                    e.cell = cellOf(p);
                    e.squad = squad.id;
                    e.count = count;
                    events_->push_back(std::move(e));
                }
                flee(m, squad);
            } else if (squad.strategy == RaidStrategy::Kidnappers) {
                const Health* vh = reg.try_get<Health>(goal);
                if (vh == nullptr || !vh->downed || vh->dead || reg.all_of<Kidnapped>(goal)) {
                    r->goalThing = kNoEntity;
                    squad.nextThink = now();
                    continue;
                }
                reg.emplace<Kidnapped>(goal, Kidnapped{m});
                r->loot = goal;
                r->goalThing = kNoEntity;
                if (events_ != nullptr) {
                    Event e;
                    e.type = EventType::PawnKidnapped;
                    e.thing = m;
                    e.target = goal;
                    e.def = reg.get<Thing>(goal).def;
                    e.cell = cellOf(p);
                    e.squad = squad.id;
                    e.message = "lifted";
                    events_->push_back(std::move(e));
                }
                flee(m, squad);
            }
        }
    }
}

// A colonist carried off the map: its gear, bed and jobs go with it.
void RaidSystem::removeForGood(Entity pawn) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> belongings;
    if (const Carrying* carrying = reg.try_get<Carrying>(pawn)) {
        belongings.push_back(carrying->thing);
        reg.remove<Carrying>(pawn);
    }
    if (const Gear* gear = reg.try_get<Gear>(pawn)) {
        belongings.insert(belongings.end(), gear->apparel.begin(), gear->apparel.end());
        belongings.insert(belongings.end(), gear->stowed.begin(), gear->stowed.end());
        if (gear->weapon != kNoEntity) {
            belongings.push_back(gear->weapon);
        }
        reg.remove<Gear>(pawn);
    }
    for (const Entity item : belongings) {
        if (reg.valid(item)) {
            reg.remove<EquippedBy>(item);
            things_.destroy(item);
        }
    }
    for (const auto [bed, owners] : reg.view<Bed>().each()) {
        owners.owners.erase(std::remove(owners.owners.begin(), owners.owners.end(), pawn), owners.owners.end());
    }
    reg.remove<Kidnapped>(pawn);
    jobs_.forget(pawn);
    fighters_.forget(pawn);
    forget(pawn);
    things_.destroy(pawn);
}

void RaidSystem::squadEvent(const Squad& squad, EventType type, std::string message) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.squad = squad.id;
    e.cell = squad.entry;
    e.count = static_cast<int>(squad.members.size());
    e.amount = squad.points;
    e.message = std::move(message);
    events_->push_back(std::move(e));
}

void RaidSystem::forget(Entity thing) {
    entt::registry& reg = things_.registry();
    // A raider that goes while carrying a colonist puts it down.
    if (reg.valid(thing)) {
        if (const Raider* gone = reg.try_get<Raider>(thing); gone != nullptr && gone->loot != kNoEntity && reg.valid(gone->loot)) {
            reg.remove<Kidnapped>(gone->loot);
        }
    }
    for (Squad& squad : state_.squads) {
        squad.members.erase(std::remove(squad.members.begin(), squad.members.end(), thing), squad.members.end());
        for (const Entity m : squad.members) {
            Raider* r = reg.valid(m) ? reg.try_get<Raider>(m) : nullptr;
            if (r != nullptr) {
                r->goalThing = r->goalThing == thing ? kNoEntity : r->goalThing;
                r->loot = r->loot == thing ? kNoEntity : r->loot;
            }
        }
    }
}

// ---- The alarm --------------------------------------------------------------------------------

bool RaidSystem::canSee(Entity watcher, Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Position& a = reg.get<Position>(watcher);
    const Position& b = reg.get<Position>(pawn);
    if (a.level != b.level || raidDist(a.x, a.y, b.x, b.y) > rules_.sightRange) {
        return false;
    }
    const Posture* posture = reg.try_get<Posture>(pawn);
    const Stance stance = posture != nullptr ? posture->stance : Stance::Standing;
    return map_.cover().lineOfSight(Viewpoint{a.x, a.y, a.level, Stance::Standing}, Viewpoint{b.x, b.y, b.level, stance});
}

void RaidSystem::setAlarm(bool on, const char* why) {
    entt::registry& reg = things_.registry();
    if (on == state_.alarm) {
        return;
    }
    state_.alarm = on;
    if (on) {
        // Home: the middle of the colony's buildings, else of its colonists.
        // In id order, so the sum is the same after a load.
        std::vector<Entity> buildings;
        std::vector<Entity> colonists;
        for (const auto [e, thing, pos] : reg.view<const Thing, const Position>().each()) {
            if (thing.category == ThingCategory::Building && reg.all_of<Footprint>(e)) {
                buildings.push_back(e);
            } else if (reg.all_of<Colonist>(e) && fighters_.factionOf(e) == kPlayerFaction) {
                colonists.push_back(e);
            }
        }
        std::vector<Entity>& from = buildings.empty() ? colonists : buildings;
        std::sort(from.begin(), from.end());
        float x = 0.0f;
        float y = 0.0f;
        const int n = static_cast<int>(from.size());
        for (const Entity e : from) {
            x += reg.get<Position>(e).x;
            y += reg.get<Position>(e).y;
        }
        state_.hasHome = n > 0;
        state_.homeX = n > 0 ? x / static_cast<float>(n) : 0.0f;
        state_.homeY = n > 0 ? y / static_cast<float>(n) : 0.0f;
    } else {
        std::vector<Entity> called;
        for (const auto [pawn, f] : reg.view<const Fighter>().each()) {
            if (f.defending) {
                called.push_back(pawn);
            }
        }
        std::sort(called.begin(), called.end());
        for (const Entity pawn : called) {
            fighters_.standDown(pawn);
        }
        state_.hasThreat = false;
    }
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::AlarmChanged;
        e.message = std::string(on ? "on: " : "off: ") + why;
        events_->push_back(std::move(e));
    }
}

void RaidSystem::thinkAlarm() {
    entt::registry& reg = things_.registry();
    // Enemies on the map that still fight; routed ones running off don't count.
    std::vector<Entity> hostiles;
    for (const auto [pawn, faction, pos] : reg.view<const Faction, const Position>().each()) {
        // Wild animals aren't enemies (Animals.h) until they hunt, fight back or go manhunter (N5).
        const Animal* wild = reg.try_get<Animal>(pawn);
        if (faction.id == kPlayerFaction || (wild != nullptr && wild->temper == AnimalTemper::Calm) || health_.incapacitated(pawn) ||
            reg.all_of<Prisoner>(pawn)) {
            continue;
        }
        const Fighter* f = reg.try_get<Fighter>(pawn);
        if (f != nullptr && f->mode == FightMode::Flee) {
            continue;
        }
        hostiles.push_back(pawn);
    }
    std::sort(hostiles.begin(), hostiles.end());
    if (hostiles.empty()) {
        state_.suppressed = false;
        if (state_.alarm && !state_.manual) {
            setAlarm(false, "all clear");
        }
        if (!state_.alarm) {
            return;
        }
    }
    // The colony's pawns, and which enemies one of them sees (or a fighter of theirs knows of).
    std::vector<Entity> ours;
    for (const auto [pawn, pos, mover] : reg.view<const Position, const Mover>().each()) {
        if (fighters_.factionOf(pawn) == kPlayerFaction && !health_.incapacitated(pawn)) {
            ours.push_back(pawn);
        }
    }
    std::sort(ours.begin(), ours.end());
    std::vector<Entity> spotted;
    for (const Entity enemy : hostiles) {
        bool seen = false;
        for (const Entity pawn : ours) {
            const Fighter* f = reg.try_get<Fighter>(pawn);
            if (f != nullptr && std::any_of(f->known.begin(), f->known.end(), [&](const KnownEnemy& k) { return k.pawn == enemy; })) {
                seen = true;
                break;
            }
        }
        for (size_t i = 0; i < ours.size() && !seen; ++i) {
            seen = canSee(ours[i], enemy);
        }
        if (seen) {
            spotted.push_back(enemy);
        }
    }
    if (!spotted.empty()) {
        float x = 0.0f;
        float y = 0.0f;
        for (const Entity e : spotted) {
            x += reg.get<Position>(e).x;
            y += reg.get<Position>(e).y;
        }
        state_.hasThreat = true;
        state_.threatX = x / static_cast<float>(spotted.size());
        state_.threatY = y / static_cast<float>(spotted.size());
        state_.threatSeenAt = now();
        if (!state_.alarm && !state_.suppressed) {
            setAlarm(true, "enemies spotted");
        }
    }
    if (!state_.alarm) {
        return;
    }
    for (const Entity pawn : ours) {
        if (reg.all_of<Colonist>(pawn) && !reg.all_of<PlayerControl>(pawn)) {
            callUp(pawn, spotted);
        }
    }
}

// What one colonist does while the alarm is on, by its role.
void RaidSystem::callUp(Entity pawn, const std::vector<Entity>& spotted) {
    entt::registry& reg = things_.registry();
    Fighter* f = reg.try_get<Fighter>(pawn);
    if (f != nullptr && !f->defending) {
        return;  // drafted: the player commands it
    }
    const DefensePlan* plan = reg.try_get<DefensePlan>(pawn);
    DefenseRole role = plan != nullptr ? plan->role : DefenseRole::Defender;
    // A colonist with no plan of its own and nothing in its hands runs for shelter instead of punching a wolf
    // (RimWorld's default hostility response: an unarmed colonist flees); an armed one defends.
    if (plan == nullptr) {
        const Gear* gear = reg.try_get<Gear>(pawn);
        if (gear == nullptr || gear->weapon == kNoEntity || !reg.valid(gear->weapon)) {
            role = DefenseRole::Shelter;
        }
    }
    const Position& pos = reg.get<Position>(pawn);
    if (role == DefenseRole::KeepWorking) {
        if (f != nullptr) {
            return;  // it has been drawn into the fight already: it stays in it until the alarm stops
        }
        bool close = false;
        for (const Entity e : spotted) {
            const Position& p = reg.get<Position>(e);
            close = close || raidDist(p.x, p.y, pos.x, pos.y) <= rules_.keepWorkingRadius;
        }
        if (!close) {
            return;
        }
        Fighter& fighter = fighters_.enlist(pawn, "enemies close");
        fighter.defending = true;
        fighter.anchor = cellOf(pos);
        fighter.leash = rules_.defenderLeash;
        return;
    }
    if (role == DefenseRole::Shelter) {
        if (f != nullptr && f->mode == FightMode::Shelter) {
            return;
        }
        const CellPos goal = shelterCell(pawn);
        Fighter& fighter = f != nullptr ? *f : fighters_.enlist(pawn, "alarm: to the shelter");
        fighter.defending = true;
        fighter.mode = FightMode::Shelter;
        fighter.hasGoal = true;
        fighter.goal = goal;
        fighters_.replan(pawn);
        return;
    }
    // Defender: holds its spot, or a point on the line between home and the threat.
    const CellPos point = defensePoint(pawn, plan);
    if (f == nullptr) {
        Fighter& fighter = fighters_.enlist(pawn, "alarm");
        fighter.defending = true;
        fighter.moveOrdered = true;
        fighter.anchor = point;
        fighter.leash = plan != nullptr && plan->hasSpot ? rules_.spotLeash : rules_.defenderLeash;
        return;
    }
    const bool hasSpot = plan != nullptr && plan->hasSpot;
    // In contact: a Defender with no spot of its own is let off the line and fights like any
    // fighter, so an enemy out of range of the line can't shoot it from afar. Out of contact again,
    // it goes back to the line.
    const bool contact = !hasSpot && std::any_of(f->known.begin(), f->known.end(), [&](const KnownEnemy& k) {
        return things_.spawned(k.pawn) && !health_.incapacitated(k.pawn);
    });
    if (f->mode == FightMode::Fight && contact) {
        if (f->moveOrdered) {
            f->moveOrdered = false;
            f->leash = 0.0f;
        }
        return;
    }
    if (f->mode != FightMode::Fight || !f->moveOrdered ||
        raidDist(raidCentre(point.x), raidCentre(point.y), raidCentre(f->anchor.x), raidCentre(f->anchor.y)) > rules_.lineMoves) {
        f->mode = FightMode::Fight;
        f->hasGoal = false;
        f->moveOrdered = true;
        f->anchor = point;
        f->leash = hasSpot ? rules_.spotLeash : rules_.defenderLeash;
        f->nextPlan = std::min(f->nextPlan, now());
    }
}

// The nearest free (walkable, no sight-blocking building, not a door) cell to (x, y) within
// `radius` cells, reachable from `from` unless from.x is negative.
bool RaidSystem::nearestFree(float x, float y, int level, CellPos from, int radius, CellPos& out) {
    const CellPos c0{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), level};
    Regions& regions = map_.regions();
    for (int r = 0; r <= radius; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const CellPos c{c0.x + dx, c0.y + dy, level};
                if (!map_.inBounds(c) || !map_.walkable(c) || map_.obstacleDm(c) > 0 || map_.isDoor(c)) {
                    continue;
                }
                if (from.x >= 0 && !regions.canReach(from, c)) {
                    continue;
                }
                out = c;
                return true;
            }
        }
    }
    return false;
}

CellPos RaidSystem::defensePoint(Entity pawn, const DefensePlan* plan) {
    const Position& pos = things_.registry().get<Position>(pawn);
    const CellPos here = cellOf(pos);
    if (plan != nullptr && plan->hasSpot) {
        return plan->spot;
    }
    if (!state_.hasThreat || !state_.hasHome) {
        return here;
    }
    const float dx = state_.threatX - state_.homeX;
    const float dy = state_.threatY - state_.homeY;
    const float d = std::sqrt(dx * dx + dy * dy);
    const float along = std::min(rules_.defenseLine, d * 0.5f);
    const float px = d > 0.0f ? state_.homeX + dx / d * along : state_.homeX;
    const float py = d > 0.0f ? state_.homeY + dy / d * along : state_.homeY;
    CellPos out;
    return nearestFree(px, py, pos.level, here, 8, out) ? out : here;
}

// A cell in the marked shelter room no other sheltering colonist goes to, else the nearest
// indoor cell, else a cell away from the threat.
CellPos RaidSystem::shelterCell(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(pawn);
    const CellPos here = cellOf(pos);
    std::vector<CellPos> taken;
    for (const auto [other, f] : reg.view<const Fighter>().each()) {
        if (other != pawn && f.mode == FightMode::Shelter && f.hasGoal) {
            taken.push_back(f.goal);
        }
    }
    const auto isFree = [&](CellPos c) {
        return map_.walkable(c) && map_.obstacleDm(c) == 0 && !map_.isDoor(c) && std::find(taken.begin(), taken.end(), c) == taken.end();
    };
    Regions& regions = map_.regions();
    if (state_.hasShelter && rooms_.indoors(state_.shelter)) {
        if (const Room* room = rooms_.roomOf(state_.shelter)) {
            for (const CellPos& c : rooms_.cellsOf(*room)) {
                if (isFree(c) && regions.canReach(here, c)) {
                    return c;
                }
            }
        }
    }
    // Breadth first from the pawn for the nearest roofed cell.
    std::vector<CellPos> open{here};
    std::set<int> seen{map_.index(here)};
    for (size_t head = 0; head < open.size() && open.size() < static_cast<size_t>(rules_.shelterSearch); ++head) {
        const CellPos c = open[head];
        if (isFree(c) && rooms_.indoors(c)) {
            return c;
        }
        const CellPos around[4] = {{c.x + 1, c.y, c.level}, {c.x - 1, c.y, c.level}, {c.x, c.y + 1, c.level}, {c.x, c.y - 1, c.level}};
        for (const CellPos& n : around) {
            if (map_.inBounds(n) && map_.walkable(n) && seen.insert(map_.index(n)).second) {
                open.push_back(n);
            }
        }
    }
    if (state_.hasThreat) {
        const float dx = pos.x - state_.threatX;
        const float dy = pos.y - state_.threatY;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 0.0f) {
            CellPos out;
            if (nearestFree(pos.x + dx / d * rules_.shelterFlee, pos.y + dy / d * rules_.shelterFlee, pos.level, here, 8, out)) {
                return out;
            }
        }
    }
    return here;
}

// ---- Saves and hashes -------------------------------------------------------------------------

bool RaidSystem::restore(RaidState state, std::string& error) {
    for (const std::string& faction : state.factions) {
        if (defs_.find("Factions", faction) == nullptr) {
            error = "the save names a faction the defs don't have: '" + faction + "'";
            return false;
        }
    }
    for (const Squad& squad : state.squads) {
        if (squad.side < 2 || squad.side >= state.factions.size() + 2 || squad.id == 0 || squad.id >= state.nextSquad) {
            error = "the save has a raid squad that doesn't fit";
            return false;
        }
    }
    for (const PendingRaid& p : state.pending) {
        if (defs_.find("Factions", p.faction) == nullptr || static_cast<int>(p.strategy) >= kRaidStrategyCount) {
            error = "the save has a waiting raid that doesn't fit the defs";
            return false;
        }
    }
    state_ = std::move(state);
    return true;
}

void RaidSystem::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(state_.factions.size()));
    for (const std::string& f : state_.factions) {
        h.add(f.data(), f.size());
    }
    h.add(state_.nextSquad);
    h.add(static_cast<uint64_t>(state_.pending.size()));
    for (const PendingRaid& p : state_.pending) {
        h.add(p.faction.data(), p.faction.size());
        h.add(p.points);
        h.add(p.edge);
        h.add(static_cast<int>(p.strategy));
        h.add(p.at);
    }
    h.add(static_cast<uint64_t>(state_.squads.size()));
    for (const Squad& s : state_.squads) {
        h.add(s.id);
        h.add(s.faction.data(), s.faction.size());
        h.add(static_cast<int>(s.strategy));
        h.add(s.hasBreach);
        h.add(s.breach.x);
        h.add(s.breach.y);
        h.add(s.breachFrom.x);
        h.add(s.breachFrom.y);
        h.add(s.side);
        h.add(s.points);
        h.add(s.started);
        h.add(s.arrivedAt);
        h.add(s.giveUpAt);
        h.add(static_cast<int>(s.state));
        h.add(s.gaveUp);
        h.add(s.entry.x);
        h.add(s.entry.y);
        h.add(s.hasTarget);
        h.add(s.target.x);
        h.add(s.target.y);
        h.add(s.nextThink);
        h.add(static_cast<uint64_t>(s.members.size()));
        for (const Entity m : s.members) {
            h.add(entt::to_integral(m));
        }
    }
    h.add(state_.alarm);
    h.add(state_.manual);
    h.add(state_.suppressed);
    h.add(state_.hasThreat);
    h.add(state_.threatX);
    h.add(state_.threatY);
    h.add(state_.threatSeenAt);
    h.add(state_.hasHome);
    h.add(state_.homeX);
    h.add(state_.homeY);
    h.add(state_.hasShelter);
    h.add(state_.shelter.x);
    h.add(state_.shelter.y);
    h.add(state_.shelter.level);
    h.add(state_.nextAlarmThink);
}

void RaidSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const DefensePlan* plan = reg.try_get<DefensePlan>(e)) {
        h.add(uint64_t{0xDEFE});
        h.add(static_cast<int>(plan->role));
        h.add(plan->hasSpot);
        h.add(plan->spot.x);
        h.add(plan->spot.y);
        h.add(plan->spot.level);
    }
    if (const Raider* r = reg.try_get<Raider>(e)) {
        h.add(uint64_t{0x4A1D});
        h.add(r->squad);
        h.add(r->kind.data(), r->kind.size());
        h.add(entt::to_integral(r->goalThing));
        h.add(entt::to_integral(r->loot));
        h.add(r->sparesDowned);
    }
    if (const Kidnapped* k = reg.try_get<Kidnapped>(e)) {
        h.add(uint64_t{0x4B1D});
        h.add(entt::to_integral(k->by));
    }
}

}  // namespace colony

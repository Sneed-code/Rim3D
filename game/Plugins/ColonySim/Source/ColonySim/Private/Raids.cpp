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
        }
    }
    return static_cast<float>(items + buildings * rules_.buildingWealthFactor);
}

float RaidSystem::threatPoints() const {
    const float wealth = colonyWealth();
    const entt::registry& reg = things_.registry();
    float pawnPoints = 0.0f;
    for (const auto [pawn, colonist] : reg.view<const Colonist>().each()) {
        const Health* h = reg.try_get<Health>(pawn);
        if (!things_.spawned(pawn) || fighters_.factionOf(pawn) != kPlayerFaction || (h != nullptr && h->dead)) {
            continue;
        }
        pawnPoints += raidCurve(rules_.pointsPerColonist, wealth);
    }
    float points = raidCurve(rules_.wealthPoints, wealth) + pawnPoints;
    const float days = static_cast<float>(now()) / static_cast<float>(kSecondsPerDay);
    points *= rules_.threatScale * raidCurve(rules_.daysPassedFactor, days) * rules_.adaptation;
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

    // Where it walks in: a cell near the chosen edge that can reach the target.
    const int w = map_.width();
    const int h = map_.height();
    const int side = edge >= 0 ? edge : static_cast<int>(rng_.below(4));
    Regions& regions = map_.regions();
    bool found = false;
    CellPos entry;
    for (int attempt = 0; attempt < 40 && !found; ++attempt) {
        const int along = static_cast<int>(rng_.below(static_cast<uint32_t>(side % 2 == 0 ? w : h)));
        const int d = rules_.edgeDepth;
        const CellPos guess = side == 0 ? CellPos{along, h - 1 - d, 0}
                            : side == 1 ? CellPos{w - 1 - d, along, 0}
                            : side == 2 ? CellPos{along, d, 0}
                                        : CellPos{d, along, 0};
        CellPos c;
        if (nearestFree(raidCentre(guess.x), raidCentre(guess.y), 0, CellPos{-1, -1, 0}, 6, c) && regions.canReach(c, target)) {
            entry = c;
            found = true;
        }
    }
    if (!found) {
        error = "no way in from that edge to the colony";
        return false;
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

    // Cells for them around the entry, a metre apart (every other cell).
    std::vector<CellPos> cells;
    {
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
        error = "no room at the edge for the raid";
        return false;
    }

    Squad squad;
    squad.id = state_.nextSquad++;
    squad.faction = faction;
    squad.side = factionId(faction);
    squad.points = points;
    squad.arrivedAt = now();
    const float hour = static_cast<float>(kSecondsPerHour);
    squad.giveUpAt = now() + static_cast<int64_t>((rules_.giveUpHoursMin + rng_.unit() * (rules_.giveUpHoursMax - rules_.giveUpHoursMin)) * hour);
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
        reg.emplace<Raider>(pawn, Raider{squad.id, kind.defName});
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
        if (faction.id != kPlayerFaction && !reg.all_of<Animal>(pawn) && things_.spawned(pawn) &&
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
        f->advances = next == SquadState::Advance;
        if (f->retreat != (next == SquadState::FallBack)) {
            f->retreat = next == SquadState::FallBack;
            f->nextPlan = std::min(f->nextPlan, now());
        }
        if (squad.hasTarget && f->goal != squad.target) {
            f->hasGoal = true;
            f->goal = squad.target;
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
    for (Squad& squad : state_.squads) {
        squad.members.erase(std::remove(squad.members.begin(), squad.members.end(), thing), squad.members.end());
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
        if (faction.id == kPlayerFaction || (wild != nullptr && wild->temper == AnimalTemper::Calm) || health_.incapacitated(pawn)) {
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
    state_ = std::move(state);
    return true;
}

void RaidSystem::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(state_.factions.size()));
    for (const std::string& f : state_.factions) {
        h.add(f.data(), f.size());
    }
    h.add(state_.nextSquad);
    h.add(static_cast<uint64_t>(state_.squads.size()));
    for (const Squad& s : state_.squads) {
        h.add(s.id);
        h.add(s.faction.data(), s.faction.size());
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
    }
}

}  // namespace colony

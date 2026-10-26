// Prisoners (task C22, docs/design/prisoners.md): the component, the numbers, capture and what a warden's talk does.
// The jobs that carry, feed and talk are in PrisonerJobs.cpp.
#include "ColonySim/Prisoners.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

const char* prisonerModeName(PrisonerMode mode) {
    switch (mode) {
        case PrisonerMode::NoInteraction: return "NoInteraction";
        case PrisonerMode::ReduceResistance: return "ReduceResistance";
        case PrisonerMode::AttemptRecruit: return "AttemptRecruit";
        case PrisonerMode::Release: return "Release";
    }
    return "?";
}

bool prisonerModeFromName(const std::string& name, PrisonerMode& out) {
    for (const PrisonerMode mode : {PrisonerMode::NoInteraction, PrisonerMode::ReduceResistance, PrisonerMode::AttemptRecruit,
                                    PrisonerMode::Release}) {
        if (name == prisonerModeName(mode)) {
            out = mode;
            return true;
        }
    }
    return false;
}

PrisonerRules PrisonerRules::read(const DefDatabase& defs) {
    PrisonerRules r;
    const Def* rules = defs.find("Rules", "PrisonerRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(rules->number(key, out)); };
    num("initialResistance.0", r.initialResistanceMin);
    num("initialResistance.1", r.initialResistanceMax);
    num("resistancePerChat", r.resistancePerChat);
    if (const size_t n = rules->count("moodFactor")) {
        r.moodFactor.clear();
        for (size_t i = 0; i < n; ++i) {
            const std::string at = "moodFactor." + std::to_string(i);
            r.moodFactor.emplace_back(static_cast<float>(rules->number((at + ".0").c_str())),
                                      static_cast<float>(rules->number((at + ".1").c_str())));
        }
        std::sort(r.moodFactor.begin(), r.moodFactor.end());
    }
    num("negotiationBase", r.negotiationBase);
    num("negotiationPerLevel", r.negotiationPerLevel);
    r.defaultSkillLevel = static_cast<int>(rules->integer("defaultSkillLevel", r.defaultSkillLevel));
    r.chatIntervalSeconds = std::max<int64_t>(1, rules->integer("chatIntervalSeconds", r.chatIntervalSeconds));
    r.chatWorkSeconds = std::max<int64_t>(1, rules->integer("chatWorkSeconds", r.chatWorkSeconds));
    r.feedWorkSeconds = std::max<int64_t>(1, rules->integer("feedWorkSeconds", r.feedWorkSeconds));
    num("chatXp", r.chatXp);
    num("recruitChanceBase", r.recruitChanceBase);
    num("recruitChanceMin", r.recruitChanceMin);
    num("recruitChanceMax", r.recruitChanceMax);
    num("captureSafeRadius", r.captureSafeRadius);
    num("feedBelow", r.feedBelow);
    r.edgeSearch = static_cast<int>(rules->integer("edgeSearch", r.edgeSearch));
    if (r.initialResistanceMax < r.initialResistanceMin) {
        r.initialResistanceMax = r.initialResistanceMin;
    }
    return r;
}

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float prisonerCurve(const std::vector<std::pair<float, float>>& points, float x) {
    if (points.empty()) {
        return 1.0f;
    }
    if (x <= points.front().first) {
        return points.front().second;
    }
    for (size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].first) {
            const float span = points[i].first - points[i - 1].first;
            const float t = span > 0.0f ? (x - points[i - 1].first) / span : 1.0f;
            return points[i - 1].second + (points[i].second - points[i - 1].second) * t;
        }
    }
    return points.back().second;
}

}  // namespace

PrisonerSystem::PrisonerSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, JobSystem& jobs,
                               FighterSystem& fighters, RaidSystem& raids, const DefDatabase& defs, const GameTime& time, Rng& rng)
    : things_(things),
      map_(map),
      combat_(combat),
      health_(health),
      jobs_(jobs),
      fighters_(fighters),
      raids_(raids),
      defs_(defs),
      time_(time),
      rng_(rng) {
    rules_ = PrisonerRules::read(defs_);
}

int64_t PrisonerSystem::now() const {
    return time_.seconds;
}

void PrisonerSystem::emit(EventType type, Entity thing, Entity target, float amount, const char* message) const {
    if (events_ == nullptr) {
        return;
    }
    const entt::registry& reg = things_.registry();
    Event e;
    e.type = type;
    e.thing = thing;
    e.target = target;
    e.amount = amount;
    e.message = message;
    if (reg.valid(thing)) {
        if (const Thing* t = reg.try_get<Thing>(thing)) {
            e.def = t->def;
        }
        if (const Position* p = reg.try_get<Position>(thing)) {
            e.cell = cellOf(*p);
        }
    }
    events_->push_back(std::move(e));
}

bool PrisonerSystem::isPrisoner(Entity pawn) const {
    return things_.registry().valid(pawn) && things_.registry().all_of<Prisoner>(pawn);
}

bool PrisonerSystem::isPrisonBed(Entity bed) const {
    return things_.registry().valid(bed) && things_.registry().all_of<PrisonBed>(bed);
}

std::vector<Entity> PrisonerSystem::prisoners() const {
    std::vector<Entity> out;
    for (const Entity e : things_.registry().view<Prisoner>()) {
        out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

int PrisonerSystem::count() const {
    return static_cast<int>(things_.registry().view<Prisoner>().size());
}

bool PrisonerSystem::capturable(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(pawn) || !reg.all_of<Health, Mover>(pawn) || reg.all_of<Prisoner>(pawn) ||
        reg.all_of<Animal>(pawn)) {
        return false;
    }
    const Health& health = reg.get<Health>(pawn);
    const FactionId side = fighters_.factionOf(pawn);
    return health.downed && !health.dead && side != kPlayerFaction && side != kWildFaction;
}

bool PrisonerSystem::findPrisonBed(Entity pawn, Entity helper, Entity& bed, CellPos& cell) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Position>(pawn) || !reg.valid(helper) || !reg.all_of<Position>(helper)) {
        return false;
    }
    const Position& at = reg.get<Position>(pawn);
    const CellPos from = cellOf(reg.get<Position>(helper));
    bool found = false;
    float bestDist = 0.0f;
    for (const auto [candidate, thing, owners, footprint, mark] :
         reg.view<const Thing, const Bed, const Footprint, const PrisonBed>().each()) {
        (void)mark;
        if (!things_.spawned(candidate)) {
            continue;
        }
        const int slots = std::max(1, static_cast<int>(defs_.get(thing.def).integer("building.sleepingSlots", 1)));
        const CellRect rect = footprint.rect();
        const auto own = std::find(owners.owners.begin(), owners.owners.end(), pawn);
        int index = -1;
        if (own != owners.owners.end()) {
            index = static_cast<int>(own - owners.owners.begin());
        } else if (static_cast<int>(owners.owners.size()) < slots) {
            index = static_cast<int>(owners.owners.size());
        }
        if (index < 0 || index >= rect.sizeX * rect.sizeY) {
            continue;
        }
        const CellPos c{rect.minX + index % rect.sizeX, rect.minY + index / rect.sizeX, rect.level};
        if (c.level != at.level || !map_.walkable(c) || jobs_.bedOccupied(c, pawn) ||
            jobs_.reservedByOther(JobTarget{kNoEntity, c}, helper) || jobs_.avoiding(helper, JobTarget{candidate, CellPos{}}) ||
            !jobs_.canReach(from, c)) {
            continue;
        }
        const float dx = at.x - (static_cast<float>(c.x) + 0.5f);
        const float dy = at.y - (static_cast<float>(c.y) + 0.5f);
        const float dist = dx * dx + dy * dy;
        if (found && (dist > bestDist || (dist == bestDist && candidate >= bed))) {
            continue;
        }
        found = true;
        bestDist = dist;
        bed = candidate;
        cell = c;
    }
    return found;
}

// ---- Commands -----------------------------------------------------------------------------------

bool PrisonerSystem::setMode(Entity pawn, PrisonerMode mode, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!isPrisoner(pawn)) {
        error = "that pawn is no prisoner";
        return false;
    }
    if (mode == PrisonerMode::Release && reg.get<Health>(pawn).downed) {
        error = "a prisoner who can't walk can't be released";
        return false;
    }
    reg.get<Prisoner>(pawn).mode = mode;
    return true;
}

bool PrisonerSystem::setPrisonBed(Entity bed, bool on, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(bed) || !reg.all_of<Bed, Thing>(bed)) {
        error = "that isn't a bed";
        return false;
    }
    for (const Entity owner : reg.get<Bed>(bed).owners) {
        if (isPrisoner(owner) != on) {
            error = on ? "a colonist sleeps in that bed" : "a prisoner sleeps in that bed";
            return false;
        }
    }
    if (on) {
        reg.emplace_or_replace<PrisonBed>(bed, PrisonBed{true});
    } else {
        reg.remove<PrisonBed>(bed);
    }
    return true;
}

// ---- Capture ------------------------------------------------------------------------------------

bool PrisonerSystem::imprison(Entity pawn, Entity bed, Entity warden, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!capturable(pawn)) {
        error = "that pawn can't be captured";
        return false;
    }
    if (!isPrisonBed(bed) || !reg.all_of<Bed>(bed)) {
        error = "that isn't a prison bed";
        return false;
    }
    jobs_.interrupt(pawn, "captured");
    if (!reg.all_of<Worker>(pawn)) {
        jobs_.addWorker(pawn, {}, true);
    }
    reg.remove<WorkSettings>(pawn);  // a prisoner does no work
    Prisoner prisoner;
    prisoner.resistance = rng_.range(rules_.initialResistanceMin, rules_.initialResistanceMax);
    prisoner.capturedAt = now();
    prisoner.nextChat = now();
    reg.emplace<Prisoner>(pawn, prisoner);
    if (!jobs_.assignBed(bed, pawn, error)) {
        reg.remove<Prisoner>(pawn);
        return false;
    }
    // Its weapons go on the floor of the cell; it stops fighting and leaves its raid squad.
    if (const Gear* gear = reg.try_get<Gear>(pawn)) {
        std::vector<Entity> weapons = gear->stowed;
        if (gear->weapon != kNoEntity) {
            weapons.push_back(gear->weapon);
        }
        for (const Entity weapon : weapons) {
            combat_.unequip(pawn, weapon);
        }
    }
    fighters_.standDown(pawn);
    raids_.forget(pawn);
    reg.remove<Raider>(pawn);
    emit(EventType::PrisonerCaptured, pawn, warden, reg.get<Prisoner>(pawn).resistance, "");
    return true;
}

// ---- Talking ------------------------------------------------------------------------------------

float PrisonerSystem::moodOf(Entity pawn) const {
    const Mood* mood = things_.registry().try_get<Mood>(pawn);
    return mood != nullptr ? mood->level : 50.0f;
}

float PrisonerSystem::resistanceReduction(Entity warden, Entity prisoner) const {
    const int level = jobs_.skillLevel(warden, "Social");
    const float negotiation = std::max(0.1f, rules_.negotiationBase + rules_.negotiationPerLevel * static_cast<float>(level)) *
                              std::max(0.0f, health_.capacityValue(warden, "Talking"));
    // The warden's opinion (C20) will be a third factor here; until then it is 1.
    return rules_.resistancePerChat * prisonerCurve(rules_.moodFactor, moodOf(prisoner)) * negotiation;
}

float PrisonerSystem::recruitChance(Entity prisoner) const {
    return std::clamp(rules_.recruitChanceBase * prisonerCurve(rules_.moodFactor, moodOf(prisoner)), rules_.recruitChanceMin,
                      rules_.recruitChanceMax);
}

bool PrisonerSystem::chatDue(Entity prisoner) const {
    const entt::registry& reg = things_.registry();
    if (!isPrisoner(prisoner) || !reg.all_of<Health, Position>(prisoner)) {
        return false;
    }
    const Prisoner& p = reg.get<Prisoner>(prisoner);
    const Health& health = reg.get<Health>(prisoner);
    if (p.mode == PrisonerMode::NoInteraction || now() < p.nextChat || health.downed || health.dead) {
        return false;
    }
    if (p.mode == PrisonerMode::ReduceResistance && p.resistance <= 0.0f) {
        return false;  // nothing left to wear down
    }
    return true;
}

void PrisonerSystem::interact(Entity warden, Entity prisoner) {
    entt::registry& reg = things_.registry();
    if (!isPrisoner(prisoner)) {
        return;
    }
    Prisoner& p = reg.get<Prisoner>(prisoner);
    p.nextChat = now() + rules_.chatIntervalSeconds;
    if (p.mode == PrisonerMode::Release) {
        release(prisoner, warden);
        return;
    }
    if (p.resistance > 0.0f) {
        const float reduction = resistanceReduction(warden, prisoner);
        p.resistance = std::max(0.0f, p.resistance - reduction);
        emit(EventType::PrisonerChatted, warden, prisoner, p.resistance, "reduced");
        return;
    }
    if (p.mode != PrisonerMode::AttemptRecruit) {
        return;
    }
    // The roll comes from the second and the two pawns, so who talks first can't change it.
    Rng roll(splitMix64(static_cast<uint64_t>(now()) ^ (static_cast<uint64_t>(entt::to_integral(warden)) << 32) ^
                        static_cast<uint64_t>(entt::to_integral(prisoner)) ^ 0x7265637275ULL));
    if (roll.unit() < recruitChance(prisoner)) {
        recruit(prisoner, warden);
    } else {
        emit(EventType::PrisonerChatted, warden, prisoner, p.resistance, "recruit failed");
    }
}

namespace {

// A prisoner no longer: its bed slot is let go.
void prisonerLeaveBed(entt::registry& reg, Entity pawn) {
    for (const auto [bed, beds] : reg.view<Bed>().each()) {
        (void)bed;
        beds.owners.erase(std::remove(beds.owners.begin(), beds.owners.end(), pawn), beds.owners.end());
    }
}

}  // namespace

bool PrisonerSystem::recruit(Entity prisoner, Entity warden) {
    entt::registry& reg = things_.registry();
    if (!isPrisoner(prisoner)) {
        return false;
    }
    jobs_.interrupt(prisoner, "recruited");
    reg.remove<Prisoner>(prisoner);
    prisonerLeaveBed(reg, prisoner);
    std::string error;
    fighters_.setFaction(prisoner, kPlayerFaction, error);
    const Colonist* colonist = reg.try_get<Colonist>(prisoner);
    reg.emplace_or_replace<WorkSettings>(prisoner, jobs_.defaultWorkSettings(colonist != nullptr ? colonist->disabledWorkTags
                                                                                                 : std::vector<std::string>{}));
    emit(EventType::PrisonerRecruited, prisoner, warden, 0.0f, "");
    return true;
}

CellPos PrisonerSystem::edgeGoal(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const CellPos here = cellOf(reg.get<Position>(pawn));
    const int w = map_.width();
    const int h = map_.height();
    struct Side {
        int d;
        int edge;
    };
    Side sides[4] = {{h - 1 - here.y, 0}, {w - 1 - here.x, 1}, {here.y, 2}, {here.x, 3}};
    std::sort(std::begin(sides), std::end(sides), [](const Side& a, const Side& b) { return a.d != b.d ? a.d < b.d : a.edge < b.edge; });
    Regions& regions = map_.regions();
    for (const Side& s : sides) {
        for (int off = 0; off <= rules_.edgeSearch; ++off) {
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
    return here;
}

bool PrisonerSystem::release(Entity prisoner, Entity warden) {
    entt::registry& reg = things_.registry();
    if (!isPrisoner(prisoner) || !things_.spawned(prisoner) || reg.get<Health>(prisoner).downed) {
        return false;
    }
    jobs_.interrupt(prisoner, "freed");
    reg.remove<Prisoner>(prisoner);
    prisonerLeaveBed(reg, prisoner);
    Fighter& f = fighters_.enlist(prisoner, "freed");
    f.mode = FightMode::Flee;
    f.advances = false;
    f.hasGoal = true;
    f.goal = edgeGoal(prisoner);
    fighters_.replan(prisoner);
    emit(EventType::PrisonerReleased, prisoner, warden, 0.0f, "");
    return true;
}

int PrisonerSystem::breakOut(const char* why) {
    int broke = 0;
    for (const Entity pawn : prisoners()) {
        if (!things_.spawned(pawn) || health_.incapacitated(pawn)) {
            continue;
        }
        if (release(pawn, kNoEntity)) {
            ++broke;
        }
    }
    if (broke > 0 && events_ != nullptr) {
        Event e;
        e.type = EventType::PrisonBreak;
        e.count = broke;
        e.message = why;
        events_->push_back(std::move(e));
    }
    return broke;
}

// ---- Each step ----------------------------------------------------------------------------------

void PrisonerSystem::step() {
    if (now() % 15 != 0) {
        return;
    }
    entt::registry& reg = things_.registry();
    std::vector<Entity> leaving;
    const int w = map_.width();
    const int h = map_.height();
    for (const auto [pawn, fighter, faction, pos] : reg.view<const Fighter, const Faction, const Position>().each()) {
        if (fighter.mode != FightMode::Flee || faction.id == kPlayerFaction || reg.all_of<Raider>(pawn) || reg.all_of<Animal>(pawn)) {
            continue;
        }
        const CellPos c = cellOf(pos);
        if (c.x <= 1 || c.y <= 1 || c.x >= w - 2 || c.y >= h - 2) {
            leaving.push_back(pawn);
        }
    }
    std::sort(leaving.begin(), leaving.end());
    for (const Entity pawn : leaving) {
        raids_.leaveMap(pawn);
    }
}

void PrisonerSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const Prisoner* p = reg.try_get<Prisoner>(e)) {
        h.add(static_cast<int>(p->mode));
        h.add(p->resistance);
        h.add(p->capturedAt);
        h.add(p->nextChat);
    }
    if (const PrisonBed* bed = reg.try_get<PrisonBed>(e)) {
        h.add(bed->marked);
    }
}

}  // namespace colony

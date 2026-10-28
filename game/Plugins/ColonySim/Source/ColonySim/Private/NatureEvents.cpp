#include "ColonySim/NatureEvents.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

namespace {

// A whole number from a def's two-number range `path`, drawn from `rng`.
int natureRangeInt(const Def& def, const char* path, Rng& rng, int fallbackLo, int fallbackHi) {
    const int lo = static_cast<int>(def.numberAt(path, 0, fallbackLo));
    const int hi = std::max(lo, static_cast<int>(def.numberAt(path, 1, fallbackHi)));
    return lo + static_cast<int>(rng.below(static_cast<uint32_t>(hi - lo + 1)));
}

std::string natureNameOf(const Colonist& c) {
    return c.nickName.empty() ? c.firstName + " " + c.lastName : c.nickName;
}

}  // namespace

NatureEvents::NatureEvents(NarratorSystem& narrator, RaidSystem& raids, const DefDatabase& defs, const GameTime& time, Map& map,
                           Things& things, Plants& plants, AnimalSystem& animals, Husbandry& husbandry, HealthSystem& health,
                           uint64_t seed)
    : narrator_(narrator), raids_(raids), defs_(defs), time_(time), map_(map), things_(things), plants_(plants),
      animals_(animals), husbandry_(husbandry), health_(health), rng_(splitMix64(seed ^ 0x6E617475726531ULL), 3) {
    narrator_.setWorker("WandererJoin", [this](IncidentFiring& f) { return wanderer(f); });
    narrator_.setWorker("RefugeePodCrash", [this](IncidentFiring& f) { return refugee(f); });
    narrator_.setWorker("ResourcePodCrash", [this](IncidentFiring& f) { return cargoPod(f); });
    narrator_.setWorker("HerdMigration", [this](IncidentFiring& f) { return herd(f); });
    narrator_.setWorker("AnimalSelfTame", [this](IncidentFiring& f) { return animalJoins(f); });
    for (const char* name : {"ColdSnap", "HeatWave", "Eclipse", "Aurora"}) {
        narrator_.setWorker(name, [this](IncidentFiring& f) { return spell(f); });
    }
    narrator_.setWorker("BerryBloom", [this](IncidentFiring& f) { return bloom(f); });
    narrator_.setWorker("HealrootBloom", [this](IncidentFiring& f) { return bloom(f); });
}

NatureEvents::~NatureEvents() = default;

CellPos NatureEvents::colonyMiddle() {
    CellPos target;
    CellPos entry;
    std::string error;
    if (!raids_.findEntry(-1, target, entry, error)) {
        return CellPos{map_.width() / 2, map_.height() / 2, 0};
    }
    return target;
}

bool NatureEvents::openSpot(double lo, double hi, CellPos& out) {
    const CellPos middle = colonyMiddle();
    for (int tries = 0; tries < 200; ++tries) {
        const double radius = lo + static_cast<double>(rng_.unit()) * (hi - lo);
        const double angle = static_cast<double>(rng_.unit()) * 6.283185307179586;
        const CellPos c{middle.x + static_cast<int>(std::lround(radius * std::cos(angle))),
                        middle.y + static_cast<int>(std::lround(radius * std::sin(angle))), 0};
        if (map_.inBounds(c) && map_.walkable(c) && map_.roof(c) == Roof::None) {
            out = c;
            return true;
        }
    }
    return false;
}

// ---- Workers --------------------------------------------------------------------------------

bool NatureEvents::wanderer(IncidentFiring& f) {
    if (!makeColonist_) {
        f.error = "nothing can make a colonist here";
        return false;
    }
    CellPos target;
    CellPos entry;
    if (!raids_.findEntry(-1, target, entry, f.error)) {
        return false;
    }
    const Entity pawn = makeColonist_(entry, splitMix64((static_cast<uint64_t>(rng_.next()) << 32) ^ rng_.next()));
    if (pawn == kNoEntity) {
        f.error = "no room for a wanderer at the edge";
        return false;
    }
    const Colonist& c = things_.registry().get<Colonist>(pawn);
    f.hasCell = true;
    f.cell = entry;
    f.text = natureNameOf(c) + ", a wanderer, has walked in from the edge of the map and joined the colony.";
    return true;
}

bool NatureEvents::refugee(IncidentFiring& f) {
    const Def* def = defs_.find("Incidents", f.incident);
    if (def == nullptr || !makeColonist_) {
        f.error = "nothing can make a colonist here";
        return false;
    }
    CellPos spot;
    if (!openSpot(def->numberAt("spotRadius", 0, 10), def->numberAt("spotRadius", 1, 40), spot)) {
        f.error = "no open ground for a pod to land on";
        return false;
    }
    const Entity pawn = makeColonist_(spot, splitMix64((static_cast<uint64_t>(rng_.next()) << 32) ^ rng_.next()));
    if (pawn == kNoEntity) {
        f.error = "no room for the pod";
        return false;
    }
    // Hurt by the landing until it can't stand, a few blows at most (the doctors take it from there).
    const DefId blunt = health_.damageTypeId("Blunt");
    const int blows = natureRangeInt(*def, "injuries", rng_, 2, 4);
    for (int i = 0; i < blows && !health_.incapacitated(pawn); ++i) {
        health_.applyDamage(pawn, blunt, 6.0f, -1, rng_);
    }
    for (int i = 0; i < 12 && !health_.incapacitated(pawn); ++i) {
        health_.applyDamage(pawn, blunt, 6.0f, -1, rng_);
    }
    const Colonist& c = things_.registry().get<Colonist>(pawn);
    f.hasCell = true;
    f.cell = spot;
    f.text = "A pod has crashed nearby. " + natureNameOf(c) + " is hurt inside it and needs help.";
    return true;
}

bool NatureEvents::cargoPod(IncidentFiring& f) {
    const Def* def = defs_.find("Incidents", f.incident);
    if (def == nullptr || def->count("contents") == 0) {
        f.error = "the cargo pod has no contents";
        return false;
    }
    CellPos spot;
    if (!openSpot(def->numberAt("spotRadius", 0, 10), def->numberAt("spotRadius", 1, 40), spot)) {
        f.error = "no open ground for a pod to land on";
        return false;
    }
    double total = 0.0;
    for (size_t i = 0; i < def->count("contents"); ++i) {
        total += def->number(("contents." + std::to_string(i) + ".weight").c_str(), 1.0);
    }
    double pick = static_cast<double>(rng_.unit()) * total;
    size_t chosen = 0;
    for (size_t i = 0; i < def->count("contents"); ++i) {
        pick -= def->number(("contents." + std::to_string(i) + ".weight").c_str(), 1.0);
        if (pick < 0.0) {
            chosen = i;
            break;
        }
    }
    const std::string base = "contents." + std::to_string(chosen);
    const std::string thing = def->text((base + ".thing").c_str());
    const DefId item = defs_.idOf("Things", thing);
    if (!item.valid()) {
        f.error = "the cargo pod holds a thing that isn't in the data: " + thing;
        return false;
    }
    const int lo = static_cast<int>(def->numberAt((base + ".count").c_str(), 0, 1));
    const int hi = std::max(lo, static_cast<int>(def->numberAt((base + ".count").c_str(), 1, 1)));
    int left = lo + static_cast<int>(rng_.below(static_cast<uint32_t>(hi - lo + 1)));
    const int total0 = left;
    const int limit = std::max(1, static_cast<int>(defs_.get(item).integer("stackLimit", 1)));
    // Stacks side by side round the landing spot.
    for (int ring = 0; ring < 4 && left > 0; ++ring) {
        for (int dy = -ring; dy <= ring && left > 0; ++dy) {
            for (int dx = -ring; dx <= ring && left > 0; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) {
                    continue;
                }
                const CellPos c{spot.x + dx, spot.y + dy, 0};
                if (!map_.inBounds(c) || !map_.walkable(c)) {
                    continue;
                }
                const int n = std::min(left, limit);
                if (things_.spawn(item, c, n) != kNoEntity) {
                    left -= n;
                }
            }
        }
    }
    if (left == total0) {
        f.error = "no room for the cargo pod's contents";
        return false;
    }
    f.hasCell = true;
    f.cell = spot;
    f.text = "A cargo pod has fallen from the sky with " + std::to_string(total0 - left) + " " + defs_.get(item).label() + ".";
    return true;
}

bool NatureEvents::herd(IncidentFiring& f) {
    const Def* def = defs_.find("Incidents", f.incident);
    if (def == nullptr) {
        return false;
    }
    const int count = natureRangeInt(*def, "count", rng_, 8, 16);
    const int stay = natureRangeInt(*def, "stayHours", rng_, 24, 72);
    CellPos entry;
    const std::vector<Entity> placed = animals_.arrive("", count, stay, entry);
    if (placed.empty()) {
        f.error = "no herd could come in";
        return false;
    }
    const Thing* first = things_.registry().try_get<Thing>(placed.front());
    const std::string kind = first != nullptr ? defs_.get(first->def).label() : "animals";
    f.hasCell = true;
    f.cell = entry;
    f.text = "A herd of " + std::to_string(placed.size()) + " " + kind + " is passing through the area.";
    return true;
}

bool NatureEvents::animalJoins(IncidentFiring& f) {
    const Def* def = defs_.find("Incidents", f.incident);
    const double maxWildness = def != nullptr ? def->number("maxWildness", 0.8) : 0.8;
    const CellPos middle = colonyMiddle();
    // A calm wild animal: a plant-eater or a small one, not one that bites back for nothing. The nearest to the colony
    // among those not too wild, ties by id.
    Entity best = kNoEntity;
    double bestD = 0.0;
    for (const auto [e, a, pos] : things_.registry().view<const Animal, const Position>().each()) {
        if (!a.wild || a.temper != AnimalTemper::Calm || husbandry_.wildness(e) > maxWildness || husbandry_.wildness(e) <= 0.0 ||
            !animals_.isAdult(e) || animals_.hostile(e)) {
            continue;
        }
        const double d = std::hypot(static_cast<double>(pos.x) - middle.x, static_cast<double>(pos.y) - middle.y);
        if (best == kNoEntity || d < bestD) {
            best = e;
            bestD = d;
        }
    }
    if (best == kNoEntity || !husbandry_.joinColony(best)) {
        f.error = "no animal near enough is tame enough to join";
        return false;
    }
    const Thing& thing = things_.registry().get<Thing>(best);
    f.hasCell = true;
    f.cell = cellOf(things_.registry().get<Position>(best));
    f.text = "A wild " + defs_.get(thing.def).label() + " has taken to the colony and joined it.";
    return true;
}

bool NatureEvents::spell(IncidentFiring& f) {
    const Def* def = defs_.find("Incidents", f.incident);
    if (def == nullptr) {
        return false;
    }
    if (isActive(f.incident)) {
        f.error = "a " + def->label() + " is already on";
        return false;
    }
    int64_t seconds = 0;
    if (def->count("durationDays") > 0) {
        seconds = static_cast<int64_t>(static_cast<double>(def->numberAt("durationDays", 0)) * 86400.0 +
                                       static_cast<double>(rng_.unit()) *
                                           (def->numberAt("durationDays", 1) - def->numberAt("durationDays", 0)) * 86400.0);
    } else {
        seconds = static_cast<int64_t>((def->numberAt("durationHours", 0, 6) +
                                        static_cast<double>(rng_.unit()) * (def->numberAt("durationHours", 1, 12) - def->numberAt("durationHours", 0, 6))) *
                                       3600.0);
    }
    conditions_.push_back(NatureCondition{f.incident, time_.seconds, time_.seconds + std::max<int64_t>(seconds, 3600)});
    const double hours = static_cast<double>(seconds) / 3600.0;
    const std::string how = hours >= 36.0 ? std::to_string(static_cast<int>(std::lround(hours / 24.0))) + " days"
                                          : std::to_string(static_cast<int>(std::lround(hours))) + " hours";
    if (f.incident == "ColdSnap") {
        f.text = "A cold snap has set in: the air will be about " + std::to_string(static_cast<int>(-def->number("temperatureOffset"))) +
                 " degrees colder for " + how + ".";
    } else if (f.incident == "HeatWave") {
        f.text = "A heat wave has set in: the air will be about " + std::to_string(static_cast<int>(def->number("temperatureOffset"))) +
                 " degrees hotter for " + how + ".";
    } else if (f.incident == "Eclipse") {
        f.text = "The sun is going dark in an eclipse for " + how + ". Solar panels will give almost nothing.";
    } else {
        f.text = "An aurora is lighting the night sky for " + how + ".";
    }
    return true;
}

bool NatureEvents::bloom(IncidentFiring& f) {
    const Def* def = defs_.find("Incidents", f.incident);
    if (def == nullptr) {
        return false;
    }
    const DefId plant = defs_.idOf("Things", def->text("plant"));
    if (!plant.valid()) {
        f.error = "the bloom names a plant that isn't in the data";
        return false;
    }
    const int want = natureRangeInt(*def, "count", rng_, 8, 14);
    const int radius = static_cast<int>(def->number("radius", 5));
    const float growth = static_cast<float>(def->number("growth", 0.5));
    // A spot where something grows: try some, plant the sprouts round the one with the most room.
    int placed = 0;
    CellPos where;
    for (int attempt = 0; attempt < 30 && placed == 0; ++attempt) {
        CellPos spot;
        if (!openSpot(def->numberAt("spotRadius", 0, 15), def->numberAt("spotRadius", 1, 45), spot)) {
            continue;
        }
        for (int i = 0; i < want * 6 && placed < want; ++i) {
            const CellPos c{spot.x + static_cast<int>(rng_.below(static_cast<uint32_t>(2 * radius + 1))) - radius,
                            spot.y + static_cast<int>(rng_.below(static_cast<uint32_t>(2 * radius + 1))) - radius, 0};
            if (map_.inBounds(c) && map_.walkable(c) && map_.roof(c) == Roof::None) {
                if (plants_.has(c)) {
                    continue;
                }
                if (plants_.place(c, plant, growth)) {
                    ++placed;
                }
            }
        }
        if (placed > 0) {
            where = spot;
        }
    }
    if (placed == 0) {
        f.error = "nowhere the plants would take";
        return false;
    }
    f.hasCell = true;
    f.cell = where;
    f.text = std::to_string(placed) + " " + defs_.get(plant).label() + " plants have sprouted not far from the colony.";
    return true;
}

// ---- Spells ---------------------------------------------------------------------------------

std::vector<NatureCondition> NatureEvents::active() const {
    std::vector<NatureCondition> out;
    for (const NatureCondition& c : conditions_) {
        if (c.startsAt <= time_.seconds && time_.seconds < c.endsAt) {
            out.push_back(c);
        }
    }
    return out;
}

bool NatureEvents::isActive(const std::string& incident) const {
    for (const NatureCondition& c : conditions_) {
        if (c.incident == incident && c.startsAt <= time_.seconds && time_.seconds < c.endsAt) {
            return true;
        }
    }
    return false;
}

double NatureEvents::temperatureOffset(int64_t seconds) const {
    double sum = 0.0;
    for (const NatureCondition& c : conditions_) {
        if (c.startsAt <= seconds && seconds < c.endsAt) {
            if (const Def* def = defs_.find("Incidents", c.incident)) {
                sum += def->number("temperatureOffset", 0.0);
            }
        }
    }
    return sum;
}

float NatureEvents::skyFactor() const {
    float factor = 1.0f;
    for (const NatureCondition& c : conditions_) {
        if (c.startsAt <= time_.seconds && time_.seconds < c.endsAt) {
            if (const Def* def = defs_.find("Incidents", c.incident)) {
                factor *= static_cast<float>(def->number("skyFactor", 1.0));
            }
        }
    }
    return factor;
}

float NatureEvents::skyFloor() const {
    float floor = 0.0f;
    for (const NatureCondition& c : conditions_) {
        if (c.startsAt <= time_.seconds && time_.seconds < c.endsAt) {
            if (const Def* def = defs_.find("Incidents", c.incident)) {
                floor = std::max(floor, static_cast<float>(def->number("nightGlow", 0.0)));
            }
        }
    }
    return floor;
}

// ---- Saving ---------------------------------------------------------------------------------

NatureState NatureEvents::state() const {
    NatureState s;
    s.rngState = rng_.state();
    s.rngIncrement = rng_.increment();
    // Spells that are over are no use to anyone.
    for (const NatureCondition& c : conditions_) {
        if (c.endsAt > time_.seconds) {
            s.conditions.push_back(c);
        }
    }
    return s;
}

void NatureEvents::restore(const NatureState& state) {
    rng_.restore(state.rngState, state.rngIncrement);
    conditions_ = state.conditions;
}

void NatureEvents::hashState(StateHasher& h) const {
    h.add(rng_.state());
    h.add(rng_.increment());
    for (const NatureCondition& c : conditions_) {
        if (c.endsAt > time_.seconds) {
            h.add(c.incident.data(), c.incident.size());
            h.add(c.startsAt);
            h.add(c.endsAt);
        }
    }
}

}  // namespace colony

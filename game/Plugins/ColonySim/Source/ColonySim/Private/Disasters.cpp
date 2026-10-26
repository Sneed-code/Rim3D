#include "ColonySim/Disasters.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Events.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Power.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace colony {

namespace {

struct DisasterRange {
    double lo = 0.0;
    double hi = 0.0;
};

DisasterRange disasterRangeOf(const Def* def, const std::string& path, double lo, double hi) {
    if (def == nullptr || def->count(path.c_str()) != 2) {
        return DisasterRange{lo, hi};
    }
    return DisasterRange{def->number((path + ".0").c_str(), lo), def->number((path + ".1").c_str(), hi)};
}

std::string disasterHours(double hours) {
    if (hours >= 48.0) {
        return std::to_string(static_cast<int>(hours / 24.0 + 0.5)) + " days";
    }
    return std::to_string(static_cast<int>(hours + 0.5)) + " hours";
}

}  // namespace

// The numbers from the DisasterRules def (data/defs/Rules). Every one has a default, so a missing def still works.
struct DisasterSystem::Numbers {
    DisasterRange solarFlareDays{0.15, 0.5};
    DisasterRange toxicDays{2.5, 10.5};
    double toxicPlantKillPerHour = 0.6;
    double toxicBuildupPerDay = 0.4;
    double toxicRecoverPerDay = 0.1;
    double toxicAnimalShare = 0.5;
    DisasterRange winterDays{7.5, 40.0};
    double winterTemperature = -7.0;
    double winterGlowLoss = 0.3;
    double winterRampHours = 20.0;
    DisasterRange stormHours{6.0, 14.0};
    double stormRadius = 21.0;
    double stormStrikeSeconds = 240.0;
    double stormDamage = 30.0;
    double stormBlast = 2.0;
    DisasterRange droneDays{0.75, 1.75};
    DisasterRange soothDays{1.5, 3.0};
    double blightKill = 0.7;
    DisasterRange blightRadius{2.0, 4.0};
    DisasterRange blightHours{24.0, 72.0};
    double blightSpreadPerHour = 0.03;
    DisasterRange outbreakShare{0.1, 0.5};
    DisasterRange packHours{24.0, 54.0};
    double animalPowerPerBodySize = 100.0;
    double animalPowerMin = 20.0;
    int packMax = 30;
    DisasterRange madHours{2.0, 4.0};
    double madRange = 60.0;
    double circuitBlastPerRootWd = 0.1;
    double circuitBlastMin = 1.0;
    double circuitBlastMax = 5.0;
    double circuitFireChance = 0.5;
    DefId toxicBuildup;
    DefId burn;

    void read(const DefDatabase& defs) {
        toxicBuildup = defs.idOf("Hediffs", "ToxicBuildup");
        burn = defs.idOf("DamageTypes", "Burn");
        if (toxicBuildup.valid()) {
            const Def& hediff = defs.get(toxicBuildup);
            toxicBuildupPerDay = hediff.number("severityPerDay.outdoorsInFallout", toxicBuildupPerDay);
            toxicRecoverPerDay = -hediff.number("severityPerDay.recoverPerDay", -toxicRecoverPerDay);
        }
        const Def* r = defs.find("Rules", "DisasterRules");
        if (r == nullptr) {
            return;
        }
        solarFlareDays = disasterRangeOf(r, "solarFlare.durationDays", solarFlareDays.lo, solarFlareDays.hi);
        toxicDays = disasterRangeOf(r, "toxicFallout.durationDays", toxicDays.lo, toxicDays.hi);
        toxicPlantKillPerHour = r->number("toxicFallout.plantKillPerHour", toxicPlantKillPerHour);
        toxicAnimalShare = r->number("toxicFallout.animalShare", toxicAnimalShare);
        winterDays = disasterRangeOf(r, "volcanicWinter.durationDays", winterDays.lo, winterDays.hi);
        winterTemperature = r->number("volcanicWinter.temperatureOffset", winterTemperature);
        winterGlowLoss = r->number("volcanicWinter.glowLoss", winterGlowLoss);
        winterRampHours = r->number("volcanicWinter.rampHours", winterRampHours);
        stormHours = disasterRangeOf(r, "flashstorm.durationHours", stormHours.lo, stormHours.hi);
        stormRadius = r->number("flashstorm.radius", stormRadius);
        stormStrikeSeconds = r->number("flashstorm.strikeSeconds", stormStrikeSeconds);
        stormDamage = r->number("flashstorm.damage", stormDamage);
        stormBlast = r->number("flashstorm.blastRadius", stormBlast);
        droneDays = disasterRangeOf(r, "psychicDrone.durationDays", droneDays.lo, droneDays.hi);
        soothDays = disasterRangeOf(r, "psychicSoothe.durationDays", soothDays.lo, soothDays.hi);
        blightKill = r->number("blight.killChance", blightKill);
        blightRadius = disasterRangeOf(r, "blight.radius", blightRadius.lo, blightRadius.hi);
        blightHours = disasterRangeOf(r, "blight.witherHours", blightHours.lo, blightHours.hi);
        blightSpreadPerHour = r->number("blight.spreadPerHour", blightSpreadPerHour);
        outbreakShare = disasterRangeOf(r, "outbreak.victimShare", outbreakShare.lo, outbreakShare.hi);
        packHours = disasterRangeOf(r, "manhunterPack.durationHours", packHours.lo, packHours.hi);
        animalPowerPerBodySize = r->number("manhunterPack.powerPerBodySize", animalPowerPerBodySize);
        animalPowerMin = r->number("manhunterPack.powerMin", animalPowerMin);
        packMax = static_cast<int>(r->integer("manhunterPack.maxAnimals", packMax));
        madHours = disasterRangeOf(r, "madAnimal.durationHours", madHours.lo, madHours.hi);
        madRange = r->number("madAnimal.range", madRange);
        circuitBlastPerRootWd = r->number("shortCircuit.blastPerRootWd", circuitBlastPerRootWd);
        circuitBlastMin = r->number("shortCircuit.blastMin", circuitBlastMin);
        circuitBlastMax = r->number("shortCircuit.blastMax", circuitBlastMax);
        circuitFireChance = r->number("shortCircuit.fireChance", circuitFireChance);
    }
};

const char* disasterKindName(DisasterKind kind) {
    switch (kind) {
        case DisasterKind::SolarFlare: return "solar flare";
        case DisasterKind::ToxicFallout: return "toxic fallout";
        case DisasterKind::VolcanicWinter: return "volcanic winter";
        case DisasterKind::Flashstorm: return "flashstorm";
        case DisasterKind::PsychicDrone: return "psychic drone";
        case DisasterKind::PsychicSoothe: return "psychic soothe";
    }
    return "";
}

DisasterSystem::DisasterSystem(NarratorSystem& narrator, PowerGrid& power, Weather& weather, Plants& plants,
                               Farming& farming, Things& things, HealthSystem& health, MoodSystem& mood,
                               AnimalSystem& animals, Fire& fire, CombatSystem& combat, PrisonerSystem& prisoners,
                               const Map& map, const DefDatabase& defs, const GameTime& time, uint64_t seed)
    : narrator_(narrator),
      power_(power),
      weather_(weather),
      plants_(plants),
      farming_(farming),
      things_(things),
      health_(health),
      mood_(mood),
      animals_(animals),
      fire_(fire),
      combat_(combat),
      prisoners_(prisoners),
      map_(map),
      defs_(defs),
      time_(time),
      rng_(splitMix64(seed ^ 0x44697361737465ULL), 17),
      numbers_(std::make_unique<Numbers>()) {
    numbers_->read(defs_);
    registerWorkers();
    weather_.setTemperatureOffset([this](int64_t t) {
        double offset = 0.0;
        for (const DisasterCondition& c : state_.active) {
            if (c.kind == DisasterKind::VolcanicWinter && t >= c.startedAt && t < c.endsAt) {
                offset += numbers_->winterTemperature;
            }
        }
        return offset;
    });
    // Mood: the drone, the soothe and the fallout are situations that hold while the condition is on.
    mood_.addSituation("psychicDrone", [this](Entity pawn) {
        for (const DisasterCondition& c : state_.active) {
            const Colonist* col = things_.registry().try_get<Colonist>(pawn);
            if (c.kind == DisasterKind::PsychicDrone && col != nullptr && col->female == c.female) {
                return c.level;
            }
        }
        return -1;
    });
    mood_.addSituation("psychicSoothe", [this](Entity pawn) {
        for (const DisasterCondition& c : state_.active) {
            const Colonist* col = things_.registry().try_get<Colonist>(pawn);
            if (c.kind == DisasterKind::PsychicSoothe && col != nullptr && col->female == c.female) {
                return 0;
            }
        }
        return -1;
    });
    mood_.addSituation("toxicFallout", [this](Entity pawn) {
        const entt::registry& reg = things_.registry();
        const Position* pos = reg.try_get<Position>(pawn);
        if (!isActive(DisasterKind::ToxicFallout) || pos == nullptr || !reg.all_of<Colonist>(pawn)) {
            return -1;
        }
        return weather_.outdoors(cellOf(*pos)) ? 0 : -1;
    });
}

DisasterSystem::~DisasterSystem() = default;

bool DisasterSystem::isActive(DisasterKind kind) const {
    return std::any_of(state_.active.begin(), state_.active.end(), [&](const DisasterCondition& c) { return c.kind == kind; });
}

std::vector<Entity> DisasterSystem::colonists() const {
    const entt::registry& reg = things_.registry();
    std::vector<Entity> out;
    for (const Entity e : reg.view<Colonist>()) {
        const Health* h = reg.try_get<Health>(e);
        if (reg.all_of<Position>(e) && things_.spawned(e) && (h == nullptr || !h->dead)) {
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<CellPos> DisasterSystem::colonistCells() const {
    std::vector<CellPos> out;
    for (const Entity e : colonists()) {
        out.push_back(cellOf(things_.registry().get<Position>(e)));
    }
    return out;
}

bool DisasterSystem::begin(DisasterKind kind, double hours) {
    DisasterCondition c;
    c.kind = kind;
    c.startedAt = time_.seconds;
    c.endsAt = time_.seconds + static_cast<int64_t>(hours * static_cast<double>(kSecondsPerHour));
    if (kind == DisasterKind::Flashstorm) {
        const std::vector<CellPos> cells = colonistCells();
        c.centre = cells.empty() ? CellPos{map_.width() / 2, map_.height() / 2, 0} : cells[rng_.below(static_cast<uint32_t>(cells.size()))];
        c.radius = static_cast<float>(numbers_->stormRadius);
        c.nextStrike = c.startedAt + 1;
    }
    if (kind == DisasterKind::PsychicDrone || kind == DisasterKind::PsychicSoothe) {
        c.female = rng_.below(2) == 1;
    }
    if (kind == DisasterKind::PsychicDrone) {
        c.level = static_cast<int>(rng_.below(3));  // low, medium or high: the extreme one is a ship part's
    }
    return begin(c);
}

bool DisasterSystem::begin(DisasterCondition condition) {
    if (isActive(condition.kind)) {
        return false;
    }
    state_.active.push_back(condition);
    ++state_.started;
    emit(static_cast<int>(EventType::DisasterStarted), condition, "started");
    return true;
}

void DisasterSystem::emit(int type, const DisasterCondition& c, const char* verb) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = static_cast<EventType>(type);
    e.message = disasterKindName(c.kind);
    e.count = static_cast<int>(c.kind);
    e.amount = static_cast<float>(static_cast<double>(c.endsAt - c.startedAt) / static_cast<double>(kSecondsPerHour));
    e.cell = c.centre;
    (void)verb;
    events_->push_back(std::move(e));
}

void DisasterSystem::step() {
    const int64_t now = time_.seconds;
    for (size_t i = 0; i < state_.active.size();) {
        if (now >= state_.active[i].endsAt) {
            emit(static_cast<int>(EventType::DisasterEnded), state_.active[i], "ended");
            state_.active.erase(state_.active.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    power_.setBlackout(isActive(DisasterKind::SolarFlare));
    float scale = 1.0f;
    for (const DisasterCondition& c : state_.active) {
        if (c.kind == DisasterKind::VolcanicWinter) {
            const double ramp = numbers_->winterRampHours * static_cast<double>(kSecondsPerHour);
            const double t = ramp > 0.0 ? std::min(1.0, static_cast<double>(now - c.startedAt) / ramp) : 1.0;
            scale *= static_cast<float>(1.0 - numbers_->winterGlowLoss * t);
        }
    }
    weather_.setGlowScale(scale);
    for (DisasterCondition& c : state_.active) {
        if (c.kind == DisasterKind::Flashstorm && now >= c.nextStrike) {
            strike(c);
        }
    }
    if (now % kSecondsPerHour == 0) {
        hourly();
    }
}

void DisasterSystem::strike(DisasterCondition& storm) {
    const float r = storm.radius;
    CellPos cell = storm.centre;
    for (int tries = 0; tries < 24; ++tries) {
        const int dx = static_cast<int>(rng_.below(static_cast<uint32_t>(2 * static_cast<int>(r) + 1))) - static_cast<int>(r);
        const int dy = static_cast<int>(rng_.below(static_cast<uint32_t>(2 * static_cast<int>(r) + 1))) - static_cast<int>(r);
        const CellPos c{storm.centre.x + dx, storm.centre.y + dy, 0};
        if (static_cast<float>(dx * dx + dy * dy) <= r * r && map_.inBounds(c)) {
            cell = c;
            break;
        }
    }
    storm.nextStrike = time_.seconds + std::max<int64_t>(
        1, static_cast<int64_t>(numbers_->stormStrikeSeconds * (0.5 + static_cast<double>(rng_.unit()))));
    if (!map_.inBounds(cell)) {
        return;
    }
    if (numbers_->burn.valid()) {
        Explosion ex;
        ex.x = static_cast<float>(cell.x) + 0.5f;
        ex.y = static_cast<float>(cell.y) + 0.5f;
        ex.z = map_.groundHeight(cell);
        ex.level = 0;
        ex.radius = static_cast<float>(numbers_->stormBlast);
        ex.damage = static_cast<float>(numbers_->stormDamage);
        ex.damageType = numbers_->burn;
        ex.seed = rng_.next();
        combat_.explode(ex);
    }
    fire_.ignite(cell);
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::LightningStrike;
        e.cell = cell;
        e.x = static_cast<float>(cell.x) + 0.5f;
        e.y = static_cast<float>(cell.y) + 0.5f;
        e.z = map_.groundHeight(cell);
        events_->push_back(std::move(e));
    }
}

bool DisasterSystem::isBlighted(CellPos cell) const {
    return std::any_of(state_.blighted.begin(), state_.blighted.end(), [&](const BlightedPlant& b) { return b.cell == cell; });
}

bool DisasterSystem::blightPlant(CellPos cell, DefId crop) {
    if (!plants_.has(cell) || plants_.defAt(cell) != crop || isBlighted(cell)) {
        return false;
    }
    const double hours = rng_.range(static_cast<float>(numbers_->blightHours.lo), static_cast<float>(numbers_->blightHours.hi));
    state_.blighted.push_back(BlightedPlant{cell, time_.seconds + static_cast<int64_t>(hours * static_cast<double>(kSecondsPerHour))});
    return true;
}

// Crop blight, once an hour: each blighted plant may pass it to the sown plants of its kind next to it, and
// withers when its time is up. A plant that was harvested or replaced is dropped from the list.
void DisasterSystem::blightHour() {
    if (state_.blighted.empty()) {
        return;
    }
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    const std::vector<BlightedPlant> now = state_.blighted;
    std::vector<BlightedPlant> keep;
    for (const BlightedPlant& b : now) {
        const DefId crop = plants_.has(b.cell) ? plants_.defAt(b.cell) : DefId{};
        if (!crop.valid()) {
            continue;
        }
        if (time_.seconds >= b.dies) {
            plants_.remove(b.cell);
            if (events_ != nullptr) {
                Event e;
                e.type = EventType::PlantRemoved;
                e.cell = b.cell;
                e.def = crop;
                e.message = "blighted";
                events_->push_back(std::move(e));
            }
            continue;
        }
        keep.push_back(b);
    }
    state_.blighted = keep;
    for (const BlightedPlant& b : keep) {
        const DefId crop = plants_.defAt(b.cell);
        for (int i = 0; i < 4; ++i) {
            const CellPos c{b.cell.x + kDx[i], b.cell.y + kDy[i], 0};
            if (map_.inBounds(c) && plants_.has(c) && plants_.defAt(c) == crop && farming_.zoneAt(c) != kNoGrowingZone &&
                static_cast<double>(rng_.unit()) < numbers_->blightSpreadPerHour) {
                blightPlant(c, crop);
            }
        }
    }
}

void DisasterSystem::hourly() {
    toxicHour();
    blightHour();
}

// Toxic fallout, once an hour: plants under the open sky die, and pawns there take buildup (animals at a share of
// it). Whoever is sheltered, or after the fallout, shakes it off slowly.
void DisasterSystem::toxicHour() {
    entt::registry& reg = things_.registry();
    const bool fallout = isActive(DisasterKind::ToxicFallout);
    if (fallout) {
        for (int y = 0; y < map_.height(); ++y) {
            for (int x = 0; x < map_.width(); ++x) {
                const CellPos c{x, y, 0};
                if (plants_.has(c) && weather_.outdoors(c) && static_cast<double>(rng_.unit()) < numbers_->toxicPlantKillPerHour) {
                    const DefId def = plants_.defAt(c);
                    plants_.remove(c);
                    if (events_ != nullptr) {
                        Event e;
                        e.type = EventType::PlantRemoved;
                        e.cell = c;
                        e.def = def;
                        e.message = "poisoned";
                        events_->push_back(std::move(e));
                    }
                }
            }
        }
    }
    if (!numbers_->toxicBuildup.valid()) {
        return;
    }
    std::vector<Entity> pawns;
    for (const Entity e : reg.view<Health, Position>()) {
        pawns.push_back(e);
    }
    std::sort(pawns.begin(), pawns.end());
    for (const Entity e : pawns) {
        if (!things_.spawned(e) || reg.get<Health>(e).dead) {
            continue;
        }
        const float before = health_.environmentSeverity(e, numbers_->toxicBuildup);
        const bool exposed = fallout && weather_.outdoors(cellOf(reg.get<Position>(e)));
        if (exposed) {
            const double share = reg.all_of<Animal>(e) ? numbers_->toxicAnimalShare : 1.0;
            health_.setEnvironmentSeverity(e, numbers_->toxicBuildup,
                                           before + static_cast<float>(numbers_->toxicBuildupPerDay * share / 24.0));
        } else if (before > 0.0f) {
            health_.setEnvironmentSeverity(e, numbers_->toxicBuildup, before - static_cast<float>(numbers_->toxicRecoverPerDay / 24.0));
        }
    }
}

void DisasterSystem::registerWorkers() {
    const Numbers& n = *numbers_;

    narrator_.setWorker("SolarFlare", [this, &n](IncidentFiring& f) {
        if (isActive(DisasterKind::SolarFlare) || power_.nets().empty()) {
            f.error = "no power for a solar flare to take, or one is on";
            return false;
        }
        const double hours = rng_.range(static_cast<float>(n.solarFlareDays.lo), static_cast<float>(n.solarFlareDays.hi)) * 24.0;
        begin(DisasterKind::SolarFlare, hours);
        f.text = "Solar flare: all electrical devices stop for " + disasterHours(hours) + ".";
        return true;
    });

    narrator_.setWorker("ToxicFallout", [this, &n](IncidentFiring& f) {
        if (isActive(DisasterKind::ToxicFallout)) {
            f.error = "toxic fallout is already falling";
            return false;
        }
        const double hours = rng_.range(static_cast<float>(n.toxicDays.lo), static_cast<float>(n.toxicDays.hi)) * 24.0;
        begin(DisasterKind::ToxicFallout, hours);
        f.text = "Toxic fallout: stay under a roof for " + disasterHours(hours) + ". Plants outside will die.";
        return true;
    });

    narrator_.setWorker("VolcanicWinter", [this, &n](IncidentFiring& f) {
        if (isActive(DisasterKind::VolcanicWinter)) {
            f.error = "a volcanic winter is already on";
            return false;
        }
        const double hours = rng_.range(static_cast<float>(n.winterDays.lo), static_cast<float>(n.winterDays.hi)) * 24.0;
        begin(DisasterKind::VolcanicWinter, hours);
        f.text = "Volcanic winter: ash dims the sun and the air is colder for " + disasterHours(hours) + ".";
        return true;
    });

    narrator_.setWorker("Flashstorm", [this, &n](IncidentFiring& f) {
        if (isActive(DisasterKind::Flashstorm) || colonists().empty()) {
            f.error = "a flashstorm is already on, or there is no colony";
            return false;
        }
        const double hours = rng_.range(static_cast<float>(n.stormHours.lo), static_cast<float>(n.stormHours.hi));
        begin(DisasterKind::Flashstorm, hours);
        const DisasterCondition& storm = state_.active.back();
        f.text = "Flashstorm: lightning strikes round the colony for " + disasterHours(hours) + ".";
        f.hasCell = true;
        f.cell = storm.centre;
        return true;
    });

    narrator_.setWorker("PsychicDrone", [this, &n](IncidentFiring& f) {
        if (isActive(DisasterKind::PsychicDrone) || isActive(DisasterKind::PsychicSoothe)) {
            f.error = "a psychic wave is already on";
            return false;
        }
        bool any[2] = {false, false};
        for (const Entity e : colonists()) {
            any[things_.registry().get<Colonist>(e).female ? 1 : 0] = true;
        }
        if (!any[0] && !any[1]) {
            f.error = "no colonists";
            return false;
        }
        const double hours = rng_.range(static_cast<float>(n.droneDays.lo), static_cast<float>(n.droneDays.hi)) * 24.0;
        begin(DisasterKind::PsychicDrone, hours);
        DisasterCondition& c = state_.active.back();
        if (!any[c.female ? 1 : 0]) {
            c.female = !c.female;  // the sex that has colonists
        }
        static const char* levels[4] = {"low", "medium", "high", "extreme"};
        f.text = std::string("Psychic drone: a ") + levels[c.level] + " drone weighs on all " + (c.female ? "women" : "men") +
                 " for " + disasterHours(hours) + ".";
        return true;
    });

    narrator_.setWorker("PsychicSoothe", [this, &n](IncidentFiring& f) {
        if (isActive(DisasterKind::PsychicDrone) || isActive(DisasterKind::PsychicSoothe)) {
            f.error = "a psychic wave is already on";
            return false;
        }
        bool any[2] = {false, false};
        for (const Entity e : colonists()) {
            any[things_.registry().get<Colonist>(e).female ? 1 : 0] = true;
        }
        if (!any[0] && !any[1]) {
            f.error = "no colonists";
            return false;
        }
        const double hours = rng_.range(static_cast<float>(n.soothDays.lo), static_cast<float>(n.soothDays.hi)) * 24.0;
        begin(DisasterKind::PsychicSoothe, hours);
        DisasterCondition& c = state_.active.back();
        if (!any[c.female ? 1 : 0]) {
            c.female = !c.female;
        }
        f.text = std::string("Psychic soothe: a calming wave lifts the mood of all ") + (c.female ? "women" : "men") + " for " +
                 disasterHours(hours) + ".";
        return true;
    });

    narrator_.setWorker("ShortCircuit", [this, &n](IncidentFiring& f) {
        // A random net that has something to power or a generator, then a random member of it.
        std::vector<uint32_t> nets;
        for (const PowerNetView& view : power_.nets()) {
            if (view.generators + view.consumers + view.batteries > 0) {
                nets.push_back(view.id);
            }
        }
        if (nets.empty()) {
            f.error = "no power net to short out";
            return false;
        }
        const uint32_t id = nets[rng_.below(static_cast<uint32_t>(nets.size()))];
        const std::vector<Entity> members = power_.membersOf(id);
        if (members.empty()) {
            f.error = "no power net to short out";
            return false;
        }
        const Entity at = members[rng_.below(static_cast<uint32_t>(members.size()))];
        const Position pos = things_.registry().get<Position>(at);
        double released = 0.0;
        if (!power_.shortCircuit(at, released, f.error)) {
            return false;
        }
        const CellPos cell = cellOf(pos);
        const double radius = std::clamp(n.circuitBlastMin + std::sqrt(released) * n.circuitBlastPerRootWd, n.circuitBlastMin,
                                         n.circuitBlastMax);
        if (released > 0.0 && n.burn.valid()) {
            Explosion ex;
            ex.x = pos.x;
            ex.y = pos.y;
            ex.z = map_.groundHeight(cell);
            ex.level = pos.level;
            ex.radius = static_cast<float>(radius);
            ex.damage = 20.0f;
            ex.damageType = n.burn;
            ex.seed = rng_.next();
            combat_.explode(ex);
        }
        fire_.igniteRadius(pos.x, pos.y, pos.level, static_cast<float>(radius), static_cast<float>(n.circuitFireChance),
                           time_.seconds);
        f.text = released > 0.0 ? "Short circuit: a power net shorted out and its batteries blew." : "Short circuit: a power conduit burnt out.";
        f.hasCell = true;
        f.cell = cell;
        return true;
    });

    narrator_.setWorker("Blight", [this, &n](IncidentFiring& f) {
        // A sown crop plant picked at random; the plants of its kind round it in growing zones catch the blight.
        std::vector<CellPos> crops;
        for (const GrowingZone& zone : farming_.zones()) {
            for (const CellPos& c : zone.cells) {
                if (plants_.has(c) && plants_.defAt(c) == zone.crop) {
                    crops.push_back(c);
                }
            }
        }
        if (crops.empty()) {
            f.error = "no crops to blight";
            return false;
        }
        const CellPos centre = crops[rng_.below(static_cast<uint32_t>(crops.size()))];
        const DefId crop = plants_.defAt(centre);
        const float radius = rng_.range(static_cast<float>(n.blightRadius.lo), static_cast<float>(n.blightRadius.hi));
        const int reach = static_cast<int>(radius);
        int caught = 0;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const CellPos c{centre.x + dx, centre.y + dy, 0};
                if (static_cast<float>(dx * dx + dy * dy) > radius * radius || !map_.inBounds(c) || !plants_.has(c) ||
                    plants_.defAt(c) != crop || farming_.zoneAt(c) == kNoGrowingZone) {
                    continue;
                }
                if ((c == centre || static_cast<double>(rng_.unit()) < n.blightKill) && blightPlant(c, crop)) {
                    ++caught;
                }
            }
        }
        f.text = "Crop blight: " + std::to_string(caught) + " " + defs_.get(crop).label() +
                 " plants caught the blight. It spreads to the next plants and withers them over days.";
        f.hasCell = true;
        f.cell = centre;
        return true;
    });

    // Disease outbreaks: every Incidents def of category Disease names its hediff; a share of the colonists catches it.
    for (const DefId id : defs_.idsOfKind("Incidents")) {
        const Def& incident = defs_.get(id);
        if (incident.text("category") != "Disease") {
            continue;
        }
        const std::string hediff = incident.text("hediff");
        narrator_.setWorker(incident.defName(), [this, &n, hediff](IncidentFiring& f) {
            const DefId disease = health_.diseaseId(hediff);
            const std::vector<Entity> people = colonists();
            if (!disease.valid() || people.empty()) {
                f.error = "no colonists, or no such disease";
                return false;
            }
            const double share = rng_.range(static_cast<float>(n.outbreakShare.lo), static_cast<float>(n.outbreakShare.hi));
            const int victims = std::max(1, static_cast<int>(std::lround(share * static_cast<double>(people.size()))));
            const std::vector<Entity> caught = health_.outbreak(disease, people, victims, rng_);
            if (caught.empty()) {
                f.error = "everyone is immune or already sick";
                return false;
            }
            f.text = "Disease outbreak: " + std::to_string(caught.size()) + (caught.size() == 1 ? " colonist has " : " colonists have ") +
                     defs_.get(disease).label() + ".";
            f.hasCell = true;
            f.cell = cellOf(things_.registry().get<Position>(caught.front()));
            return true;
        });
    }

    narrator_.setWorker("PrisonBreak", [this](IncidentFiring& f) {
        const int broke = prisoners_.breakOut("prison break");
        if (broke == 0) {
            f.error = "no prisoner who can walk";
            return false;
        }
        f.text = "Prison break: " + std::to_string(broke) + (broke == 1 ? " prisoner has" : " prisoners have") +
                 " broken out and are making for the map's edge.";
        return true;
    });

    narrator_.setWorker("ManhunterPack", [this, &n](IncidentFiring& f) {
        const std::vector<AnimalSystem::WildKind> kinds = animals_.wildKinds();
        const std::vector<Entity> people = colonists();
        if (kinds.empty() || people.empty()) {
            f.error = "no wild animals to go mad, or no colony";
            return false;
        }
        auto power = [&](const AnimalSystem::WildKind& k) {
            return std::max(n.animalPowerMin, n.animalPowerPerBodySize * static_cast<double>(k.bodySize));
        };
        std::vector<const AnimalSystem::WildKind*> fit;
        double total = 0.0;
        for (const AnimalSystem::WildKind& k : kinds) {
            if (power(k) <= static_cast<double>(f.points)) {
                fit.push_back(&k);
                total += k.commonality;
            }
        }
        const AnimalSystem::WildKind* pick = nullptr;
        if (fit.empty()) {
            pick = &*std::min_element(kinds.begin(), kinds.end(), [&](const auto& a, const auto& b) { return power(a) < power(b); });
        } else {
            double roll = static_cast<double>(rng_.unit()) * total;
            for (const AnimalSystem::WildKind* k : fit) {
                pick = k;
                roll -= k->commonality;
                if (roll <= 0.0) {
                    break;
                }
            }
        }
        const int count = std::clamp(static_cast<int>(static_cast<double>(f.points) / power(*pick)), 1, n.packMax);
        const double hours = rng_.range(static_cast<float>(n.packHours.lo), static_cast<float>(n.packHours.hi));
        const Entity target = people[rng_.below(static_cast<uint32_t>(people.size()))];
        CellPos where;
        const int came = animals_.spawnManhunters(pick->def, count, hours, target, where);
        if (came == 0) {
            f.error = "the pack could not come in";
            return false;
        }
        f.text = "Manhunter pack: " + std::to_string(came) + " " + defs_.get(pick->def).label() + " have gone mad and are coming for the colony.";
        f.hasCell = true;
        f.cell = where;
        return true;
    });

    narrator_.setWorker("MadAnimal", [this, &n](IncidentFiring& f) {
        const double hours = rng_.range(static_cast<float>(n.madHours.lo), static_cast<float>(n.madHours.hi));
        const Entity animal = animals_.goMad(colonistCells(), static_cast<float>(n.madRange), hours, rng_);
        if (animal == kNoEntity) {
            f.error = "no animal near the colony to go mad";
            return false;
        }
        const entt::registry& reg = things_.registry();
        f.text = "Mad animal: a " + defs_.get(reg.get<Thing>(animal).def).label() + " has gone berserk.";
        f.hasCell = true;
        f.cell = cellOf(reg.get<Position>(animal));
        return true;
    });
}

DisasterState DisasterSystem::state() const {
    DisasterState s = state_;
    s.rngState = rng_.state();
    s.rngIncrement = rng_.increment();
    return s;
}

void DisasterSystem::restore(const DisasterState& state) {
    state_ = state;
    rng_.restore(state.rngState, state.rngIncrement);
    state_.rngState = 0;
    state_.rngIncrement = 0;
}

void DisasterSystem::hashState(StateHasher& h) const {
    h.add(rng_.state());
    h.add(rng_.increment());
    h.add(state_.started);
    h.add(static_cast<uint64_t>(state_.blighted.size()));
    for (const BlightedPlant& b : state_.blighted) {
        h.add(b.cell.x);
        h.add(b.cell.y);
        h.add(b.dies);
    }
    h.add(static_cast<uint64_t>(state_.active.size()));
    for (const DisasterCondition& c : state_.active) {
        h.add(static_cast<int>(c.kind));
        h.add(c.startedAt);
        h.add(c.endsAt);
        h.add(c.level);
        h.add(c.female);
        h.add(c.centre.x);
        h.add(c.centre.y);
        h.add(c.radius);
        h.add(c.nextStrike);
    }
}

}  // namespace colony

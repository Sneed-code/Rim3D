#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class AnimalSystem;
class DefDatabase;
class HealthSystem;
class Husbandry;
class Map;
class NarratorSystem;
class Plants;
class RaidSystem;
class StateHasher;
class Things;
struct GameTime;

// A spell the sky or the weather is under because of an incident: a cold snap, a heat wave, an
// eclipse or an aurora. It starts when the incident does and ends at `endsAt` (game seconds).
struct NatureCondition {
    std::string incident;  // Incidents defName
    int64_t startsAt = 0;
    int64_t endsAt = 0;
};

// What is saved (section "nature").
struct NatureState {
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    std::vector<NatureCondition> conditions;
};

// Events: nature and good news (task T2, docs/design/nature-events.md). The workers for the Incidents
// WandererJoin, ResourcePodCrash, RefugeePodCrash, HerdMigration, AnimalSelfTame, ColdSnap, HeatWave,
// Eclipse, Aurora, BerryBloom and HealrootBloom, registered with the narrator when this is made. Each
// does its thing and writes the letter's text; the narrator sends the letter.
//
// The four spells are kept here (they end by themselves): `temperatureOffset` goes to Weather, `skyFactor`
// to Weather too and `skyFloor` to Lights, which the sim wires up once.
class COLONY_API NatureEvents {
public:
    // Makes a newcomer colonist standing in a cell from a seed, or kNoEntity (the sim knows the scenario).
    using ColonistMaker = std::function<Entity(CellPos, uint64_t)>;

    NatureEvents(NarratorSystem& narrator, RaidSystem& raids, const DefDatabase& defs, const GameTime& time, Map& map,
                 Things& things, Plants& plants, AnimalSystem& animals, Husbandry& husbandry, HealthSystem& health,
                 uint64_t seed);
    ~NatureEvents();
    NatureEvents(const NatureEvents&) = delete;
    NatureEvents& operator=(const NatureEvents&) = delete;

    void setColonistMaker(ColonistMaker maker) { makeColonist_ = std::move(maker); }

    // Degrees added to the outdoor temperature at a game second by the cold snap and heat wave then.
    double temperatureOffset(int64_t seconds) const;
    // What the eclipse leaves of the sun's glow now (1 when there is none).
    float skyFactor() const;
    // The least glow the open sky has now: the aurora's (0 when there is none).
    float skyFloor() const;
    // The spells in force now.
    std::vector<NatureCondition> active() const;
    bool isActive(const std::string& incident) const;

    NatureState state() const;
    void restore(const NatureState& state);
    void hashState(StateHasher& h) const;

private:
    bool wanderer(IncidentFiring& f);
    bool refugee(IncidentFiring& f);
    bool cargoPod(IncidentFiring& f);
    bool herd(IncidentFiring& f);
    bool animalJoins(IncidentFiring& f);
    bool spell(IncidentFiring& f);
    bool bloom(IncidentFiring& f);
    // An open-sky cell `lo` to `hi` metres from the colony's middle (the player's colonist nearest the map's).
    bool openSpot(double lo, double hi, CellPos& out);
    CellPos colonyMiddle();

    NarratorSystem& narrator_;
    RaidSystem& raids_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Map& map_;
    Things& things_;
    Plants& plants_;
    AnimalSystem& animals_;
    Husbandry& husbandry_;
    HealthSystem& health_;
    Rng rng_;
    ColonistMaker makeColonist_;
    std::vector<NatureCondition> conditions_;
};

}  // namespace colony

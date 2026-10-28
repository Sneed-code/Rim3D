#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace colony {

class AnimalSystem;
class CombatSystem;
class Farming;
class Fire;
class HealthSystem;
class Map;
class MoodSystem;
class NarratorSystem;
class Plants;
class PowerGrid;
class StateHasher;
class Things;
class Weather;
struct Event;
struct GameTime;

// Disasters and bad events (task T3, docs/design/disasters.md): the narrator's trouble that is not a raid, a
// disease or a prison break. Each is an incident worker (`NarratorSystem::setWorker`) that either does its
// harm at once (short circuit, crop blight, manhunter pack, mad animal) or starts a **condition** that lasts a
// while and is worked out here every step (solar flare, toxic fallout, volcanic winter, flashstorm, psychic
// drone, psychic soothe).
//
// Unreal does not include this header: it reads the Letter, DisasterStarted, DisasterEnded and LightningStrike
// events, and the shorthand in `Sim::disasters()` once the snapshot has a view of it.

enum class DisasterKind : uint8_t {
    SolarFlare,     // every power net is dark
    ToxicFallout,   // pawns outdoors take toxic buildup, plants outdoors die
    VolcanicWinter, // colder and darker
    Flashstorm,     // lightning strikes in a circle
    PsychicDrone,   // one sex's mood falls (`level` 0 to 3)
    PsychicSoothe,  // one sex's mood rises
};
inline constexpr int kDisasterKindCount = 6;

COLONY_API const char* disasterKindName(DisasterKind kind);

// One condition that is on now. Saved as a whole (section "disasters").
struct DisasterCondition {
    DisasterKind kind = DisasterKind::SolarFlare;
    int64_t startedAt = 0;  // game second
    int64_t endsAt = 0;
    int level = 0;          // drone: 0 low to 3 extreme
    bool female = false;    // drone and soothe: the sex it works on
    CellPos centre;         // flashstorm: the middle of the storm
    float radius = 0.0f;    // flashstorm
    int64_t nextStrike = 0; // flashstorm: game second of the next bolt
};

// A crop plant with the blight: it spreads to the plants of its kind next to it and dies at `dies`.
struct BlightedPlant {
    CellPos cell;
    int64_t dies = 0;  // game second
};

struct DisasterState {
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    std::vector<DisasterCondition> active;  // in the order they began
    uint32_t started = 0;                   // conditions begun so far
    std::vector<BlightedPlant> blighted;    // in the order they caught it
};

class COLONY_API DisasterSystem {
public:
    DisasterSystem(NarratorSystem& narrator, PowerGrid& power, Weather& weather, Plants& plants, Farming& farming,
                   Things& things, HealthSystem& health, MoodSystem& mood, AnimalSystem& animals, Fire& fire,
                   CombatSystem& combat, const Map& map, const DefDatabase& defs, const GameTime& time, uint64_t seed);
    ~DisasterSystem();
    DisasterSystem(const DisasterSystem&) = delete;
    DisasterSystem& operator=(const DisasterSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }

    // One game second, after the clock has advanced: ends the conditions that are over and works the rest.
    void step();

    // Conditions on now.
    const std::vector<DisasterCondition>& active() const { return state_.active; }
    bool isActive(DisasterKind kind) const;
    // Plants with the blight now.
    const std::vector<BlightedPlant>& blighted() const { return state_.blighted; }
    // Starts a condition now for `hours` game hours without the narrator (tests, dev tools). False if one of that kind is on.
    bool begin(DisasterKind kind, double hours);

    // After loading a save.
    DisasterState state() const;
    void restore(const DisasterState& state);
    void hashState(StateHasher& h) const;

private:
    struct Numbers;
    bool begin(DisasterCondition condition);
    void registerWorkers();
    void emit(int type, const DisasterCondition& c, const char* verb);
    void hourly();
    void toxicHour();
    void blightHour();
    bool blightPlant(CellPos cell, DefId crop);
    bool isBlighted(CellPos cell) const;
    void strike(DisasterCondition& storm);
    std::vector<Entity> colonists() const;
    std::vector<CellPos> colonistCells() const;

    NarratorSystem& narrator_;
    PowerGrid& power_;
    Weather& weather_;
    Plants& plants_;
    Farming& farming_;
    Things& things_;
    HealthSystem& health_;
    MoodSystem& mood_;
    AnimalSystem& animals_;
    Fire& fire_;
    CombatSystem& combat_;
    const Map& map_;
    const DefDatabase& defs_;
    const GameTime& time_;
    std::vector<Event>* events_ = nullptr;
    Rng rng_;
    DisasterState state_;  // the random stream in it is only filled in by state()
    std::unique_ptr<Numbers> numbers_;
};

}  // namespace colony

#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <array>
#include <cstdint>
#include <vector>

namespace colony {

class HealthSystem;
class Things;

// Food and rest, hourly schedules and beds (task C6). See docs/design/needs.md.
//
// Unreal does not include this header: it reads each pawn's food and rest from the snapshot and
// changes schedules and bed owners through commands (docs/design/bridge.md).

// What a pawn should be doing in an hour of the day (RimWorld's timetable).
enum class ScheduleKind : uint8_t {
    Anything,  // work, and sleep when tired
    Work,      // work; sleeps only when very tired
    Joy,       // recreation: no work; the pawn plays, sits or strolls (Recreation.h)
    Sleep,     // go to bed when rested less than NeedRules.rest.fallAsleepBelow
};

COLONY_API const char* scheduleKindName(ScheduleKind kind);

// A pawn's food and rest, 0 (empty) to 1 (full), as RimWorld's Food and Rest needs. Pawns that
// work have them when the defs have a NeedRules def.
struct Needs {
    float food = 1.0f;
    float rest = 1.0f;
    // Malnutrition severity, 0 to 1: rises while food is 0, falls otherwise; death at 1.
    float malnutrition = 0.0f;
    // The game second before which the pawn doesn't look for food again after finding none.
    int64_t foodSearchAfter = 0;
};

// One entry per hour of the day, 0 to 23.
struct Schedule {
    std::array<ScheduleKind, 24> hours{};  // all Anything
};

// On a bed building (a def with `building.bed`): who sleeps in it. A pawn owns at most one bed.
struct Bed {
    std::vector<Entity> owners;  // at most the def's `building.sleepingSlots`; slot i is the i-th footprint cell
};

// The numbers from the NeedRules def (data/defs/Rules). `present` is false when the defs have
// none, in which case pawns get no needs.
struct COLONY_API NeedRules {
    bool present = false;

    // Food: 1 is a full stomach. A pawn seeks food below `seekBelow`.
    float foodSeekBelow = 0.3f;
    float foodHungryBelow = 0.25f;
    float foodRavenousBelow = 0.125f;
    // Fall rate is the pawn def's `race.foodPerDay` times these factors for the three bands.
    float foodFallFactorFed = 1.0f;
    float foodFallFactorHungry = 0.5f;
    float foodFallFactorRavenous = 0.25f;
    float defaultFoodPerDay = 1.6f;
    float malnutritionPerDay = 0.48f;
    double ingestSeconds = 720.0;  // eating one sitting of a food whose def has no `ingestible.baseIngestSeconds`
    int maxNumToIngestAtOnce = 20;
    float eatSpotRadius = 40.0f;  // how far from the food a chair at a table is looked for, metres

    // Rest: 1 is fully rested.
    float restFallPerDayRested = 0.95f;
    float restFallPerDayDrowsy = 0.66f;
    float restFallPerDayTired = 0.28f;
    float restFallPerDayExhausted = 0.57f;
    float restDrowsyBelow = 0.28f;
    float restTiredBelow = 0.14f;
    float restExhaustedBelow = 0.01f;
    float restGainPerDay = 2.285714f;  // asleep, at rest effectiveness 1
    float groundRestEffectiveness = 0.8f;
    float fallAsleepBelow = 0.75f;      // in a Sleep hour
    float wakeAt = 1.0f;                // asleep in a Sleep hour
    float wakeOutsideSleepAt = 0.7f;    // asleep in any other hour
    // The default schedule: Sleep from `sleepFromHour` through `sleepToHour` (wrapping midnight).
    int sleepFromHour = 22;
    int sleepToHour = 5;
    // ... and Joy from `joyFromHour` through `joyToHour` (-1: none), read from JoyRules (Recreation.h).
    int joyFromHour = -1;
    int joyToHour = -1;

    static NeedRules read(const DefDatabase& defs);
    Schedule standardSchedule() const;
};

// Which band a level is in.
enum class FoodBand : uint8_t { Fed, Hungry, Ravenous, Malnourished };
enum class RestBand : uint8_t { Rested, Drowsy, Tired, Exhausted };
COLONY_API FoodBand foodBandOf(const NeedRules& rules, float food);
COLONY_API RestBand restBandOf(const NeedRules& rules, float rest);

// Runs every pawn's needs once per game second: food and rest fall, rest rises in bed,
// malnutrition builds when the stomach is empty and kills at 1. Eating and sleeping are jobs
// (Jobs.h); this only keeps the numbers.
class COLONY_API NeedsSystem {
public:
    NeedsSystem(Things& things, HealthSystem& health, const DefDatabase& defs);

    const NeedRules& rules() const { return rules_; }
    // The rest gained per game day lying in `bed` (kNoEntity: on the ground): the rules' rate times
    // the bed def's `stats.bedRestEffectiveness`.
    float restGainPerDay(Entity bed) const;

    void step();

private:
    Things& things_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    NeedRules rules_;
};

}  // namespace colony

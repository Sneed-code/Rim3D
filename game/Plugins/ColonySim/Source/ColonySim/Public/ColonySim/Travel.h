#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class AnimalSystem;
class Fire;
class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class MentalBreakSystem;
class NeedsSystem;
class RaidSystem;
class StateHasher;
class Things;
struct GameTime;

// Night skip and fast travel (task P5, docs/design/skip-travel.md). Both let game time run on while the
// player waits for nothing: the sim keeps stepping, in batches inside one `Sim::step`, with the narrator
// held (no incident is rolled and none that was due comes at the end).
//
// A night skip runs to the end of the sleeping hours when every colonist who is on a Sleep hour and tired
// is in bed and nothing threatens the colony. Fast travel moves the controlled colonist to a place it has
// visited, after the time the walk would take. Either one stops at once if a threat appears (a raid, an
// enemy or hostile animal on the map, a fire, a mental break, a drafted colonist) or on CancelSkipCommand.
//
// Unreal does not include this header: it reads `Snapshot::travel` and the SkipStarted / SkipEnded events,
// and sends SkipNightCommand, FastTravelCommand and CancelSkipCommand.

enum class SkipKind : uint8_t { None, Night, Travel };

// The numbers (TravelRules in data/defs/Rules). All of them ours: RimWorld has neither.
struct TravelRules {
    int visitedBlock = 8;             // metres a block of the visited grid is wide
    float visitedRadius = 16.0f;      // metres round the controlled colonist that count as visited
    float minDistance = 20.0f;        // fast travel to somewhere closer than this is refused: walk
    float detour = 1.3f;              // the walk takes this many times the straight line
    float defaultSpeed = 0.15333f;    // metres per game second when the pawn has no speed of its own (a human walks 0.153)
    int nightBatch = 1200;            // game seconds a night skip runs per Sim::step
    int travelBatch = 120;            // the same for fast travel
    int threatCheckSeconds = 10;      // game seconds between looks for a threat while skipping
    bool freezeAnimals = true;        // animals, their husbandry and breeding stand still during a night skip
    static TravelRules read(const DefDatabase& defs);
};

// What the system keeps between steps; saved as a whole (section "travel").
struct TravelState {
    SkipKind kind = SkipKind::None;
    int64_t startedAt = 0;      // game second
    int64_t endsAt = 0;         // game second the skip ends
    Entity pawn = kNoEntity;    // night: the colonist that was under control (and is let go); travel: who walks
    float destX = 0.0f;         // travel: where the pawn lands, metres
    float destY = 0.0f;
    uint32_t nights = 0;        // night skips that ran to the end
    uint32_t travels = 0;       // fast travels that arrived
    int64_t skipped = 0;        // game seconds skipped in all
    std::vector<uint32_t> visited;  // visited blocks by index (row by row), in order
};

class COLONY_API TravelSystem {
public:
    TravelSystem(Things& things, Map& map, JobSystem& jobs, RaidSystem& raids, Fire& fire, AnimalSystem& animals,
                 FighterSystem& fighters, MentalBreakSystem& breaks, HealthSystem& health, NeedsSystem& needs,
                 const DefDatabase& defs, const GameTime& time);
    ~TravelSystem();
    TravelSystem(const TravelSystem&) = delete;
    TravelSystem& operator=(const TravelSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    const TravelRules& rules() const { return rules_; }

    // ---- Questions
    bool skipping() const { return state_.kind != SkipKind::None; }
    SkipKind kind() const { return state_.kind; }
    // Game seconds a Sim::step runs on while skipping (0 when not).
    int batch() const;
    // Whether a night skip freezes animals now.
    bool freezingAnimals() const { return state_.kind == SkipKind::Night && rules_.freezeAnimals; }
    // Why the colony is not safe now, in plain words; empty if it is.
    std::string threat() const;
    // Whether a night skip could start now; false, with a reason in plain words, if not.
    bool canSkipNight(std::string& why) const;
    // The game second a night skip from now would end.
    int64_t morningAfter() const;
    // Whether the block under a cell has been visited.
    bool visited(CellPos cell) const;
    size_t visitedBlocks() const;
    // The seconds the walk to a cell would take the pawn (0 if it can't go there; `why` says why).
    int64_t travelSeconds(Entity pawn, float x, float y, std::string& why) const;

    // ---- Commands. False, with a reason in plain words, when they can't be done.
    bool startNight(std::string& error);
    bool startTravel(Entity pawn, float x, float y, std::string& error);
    // Ends the skip now. `why` goes into the event. False if nothing is being skipped.
    bool cancel(const std::string& why, std::string& error);

    // ---- Each step
    // Marks the blocks round a controlled colonist as visited.
    void noteVisits();
    // After a step while skipping: looks for a threat now and then, and ends the skip when its time is up.
    void afterStep();
    // The game seconds skips have run since the last call (a skip that ended): the narrator is held back by them.
    int64_t takeHold() {
        const int64_t held = hold_;
        hold_ = 0;
        return held;
    }

    // After loading a save. False if it doesn't fit the map.
    bool restore(TravelState state, std::string& error);
    TravelState state() const;
    // The progress counters and the running skip, without the visited list (state() copies that).
    const TravelState& progress() const { return state_; }
    size_t visitedCount() const { return visitedCount_; }
    void hashState(StateHasher& h) const;

private:
    void finish(const std::string& why, bool arrived);
    int blocksWide() const;
    int blocksHigh() const;
    bool markBlock(int bx, int by);
    void emit(EventType type, const std::string& message, float amount);

    Things& things_;
    Map& map_;
    JobSystem& jobs_;
    RaidSystem& raids_;
    Fire& fire_;
    AnimalSystem& animals_;
    FighterSystem& fighters_;
    MentalBreakSystem& breaks_;
    HealthSystem& health_;
    NeedsSystem& needs_;
    const DefDatabase& defs_;
    const GameTime& time_;
    TravelRules rules_;
    TravelState state_;
    std::vector<uint8_t> grid_;  // one byte per block, 1 when visited; made on first use
    size_t visitedCount_ = 0;
    int64_t hold_ = 0;
    int lastBlockX_ = -1;
    int lastBlockY_ = -1;
    std::vector<Event>* events_ = nullptr;
};

COLONY_API const char* skipKindName(SkipKind kind);

}  // namespace colony

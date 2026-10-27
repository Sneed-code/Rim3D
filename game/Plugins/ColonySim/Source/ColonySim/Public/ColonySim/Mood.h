#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace colony {

struct Event;
class NeedsSystem;
class RoomStats;
class Rooms;
class TemperatureSystem;
class Things;

// Mood and thoughts (task C9). See docs/design/mood.md.
//
// Unreal does not include this header: it reads each pawn's mood and break band from the snapshot
// and the thoughts through `Sim::mood().thoughts(pawn)`.

// How close a pawn is to a mental break, from its mood against the break thresholds. The breaks
// themselves come later; for now the band is only recorded (and sent as a PawnMoodBand event).
enum class MoodBand : uint8_t { Stable, Minor, Major, Extreme };

COLONY_API const char* moodBandName(MoodBand band);

// A thought that lasts a while after something happened (ate without a table...).
struct Memory {
    std::string def;      // Thoughts def, by defName
    int64_t expires = 0;  // game second it fades
};

// A colonist's mood, 0 to 100. Only pawns with needs have one (and only when the defs have MoodRules).
struct Mood {
    float level = 50.0f;
    MoodBand band = MoodBand::Stable;
    std::vector<Memory> memories;
};

// One thought as the colonist panel shows it.
struct ThoughtView {
    std::string def;
    std::string label;
    float offset = 0.0f;
    bool situation = false;  // true while a state holds (hungry); false for a memory that fades
    int64_t expires = 0;     // memories only
};

// The numbers from the MoodRules def (data/defs/Rules). `present` is false when the defs have
// none, in which case pawns get no mood.
struct COLONY_API MoodRules {
    bool present = false;
    float baseMood = 50.0f;
    float minorBreakThreshold = 0.35f;  // of the 0 to 100 scale
    float majorBreakThreshold = 0.20f;
    float extremeBreakThreshold = 0.05f;
    float risePerHour = 12.0f;  // the level moves towards the target at most this fast
    float fallPerHour = 8.0f;
    double memoryDays = 0.6;  // for a memory def without `durationDays`
    // The hours of the day that count as night for night owls (`nightFromHour` through `nightToHour`, wrapping midnight).
    int nightFromHour = 21;
    int nightToHour = 4;
    bool isNightHour(int hour) const {
        return nightFromHour <= nightToHour ? (hour >= nightFromHour && hour <= nightToHour)
                                            : (hour >= nightFromHour || hour <= nightToHour);
    }

    static MoodRules read(const DefDatabase& defs);
};

// Keeps every pawn's mood once per game second, after the jobs: the target is the base mood plus
// trait offsets, the situations that hold now and the memories not yet faded; the level moves
// towards it; the break band follows the level. Memories are added from the step's events.
class COLONY_API MoodSystem {
public:
    MoodSystem(Things& things, const NeedsSystem& needs, const DefDatabase& defs);

    // A condition other systems evaluate (task C15 on): a Thoughts def whose `situation.condition` is
    // `condition` counts for a pawn while `stage` gives a stage number of the def's `stages` (0 first), and
    // not at all for a negative number. `situation.trait` and `situation.notTrait` limit a thought to
    // pawns with, or without, a trait.
    using SituationStage = std::function<int(Entity pawn)>;
    void addSituation(const std::string& condition, SituationStage stage);

    // Where a pawn slept decides "slept outside" (docs/design/rooms.md); without rooms nothing is outside.
    void setRooms(Rooms* rooms) { rooms_ = rooms; }
    // The room and beauty thoughts (C17, docs/design/room-stats.md) read the room stats and the beauty need.
    void setRoomStats(RoomStats* roomStats) { roomStats_ = roomStats; }
    // Where the temperature thoughts (Cold, Hot) get the degrees outside a pawn's comfort from.
    void setTemperature(TemperatureSystem* temperature) { temperature_ = temperature; }

    // Which skill a pawn's job is using right now (Skills `defName`), or empty: passion for it
    // lifts the mood while the work goes on (docs/design/skills.md). Without one, no work thoughts.
    void setActiveSkill(std::function<std::string(Entity)> activeSkill) { activeSkill_ = std::move(activeSkill); }

    const MoodRules& rules() const { return rules_; }
    // The mood the pawn is heading for (not clamped to the 0 to 100 scale), or its base if it has none.
    float target(Entity pawn, int64_t now) const;
    // The thoughts that count right now, strongest first (ties by name).
    std::vector<ThoughtView> thoughts(Entity pawn, int64_t now) const;
    // The three thresholds as mood levels (0 to 100) for this pawn: its traits move them.
    void breakLevels(Entity pawn, float& minor, float& major, float& extreme) const;

    // Starts the memories whose thought def has this `trigger` (docs/design/mood.md): another system
    // raises one, like the social system's (C20). Nothing happens to a pawn with no mood yet.
    void remember(Entity pawn, const std::string& trigger, int64_t now);

    // `events` are the step's events; `now` the game second the step ran in.
    void step(int64_t now, std::vector<Event>& events);

private:
    struct Thought {
        DefId def;
        float offset = 0.0f;
        double durationDays = 0.0;
        std::string trigger;    // memories: what starts it (see Mood.cpp)
        std::string need;       // situations: "food" or "rest"
        std::string band;       // situations: the band name
        std::string passion;    // situations: "Minor" or "Major", while working at a skill with that passion
        // Situations with stages (D9): `condition` names a state RoomStats knows, `thresholds` are
        // the values each stage ends at (one fewer than the stages), `stages` their offsets and labels
        // (and the room roles a stage counts in, none: any), `roomRoles` the roles the thought is
        // about at all (none: any).
        struct Stage {
            float offset = 0.0f;
            std::string label;
            std::vector<std::string> roles;
        };
        std::string condition;
        std::vector<float> thresholds;
        std::vector<Stage> stages;
        std::vector<std::string> roomRoles;
        std::string trait;      // staged by another system (addSituation): only for pawns with this trait
        std::string notTrait;   // ...and not for pawns with this one
        std::vector<std::string> notTraits;  // ...and not for pawns with any of these (`situation.notTraits`)
        std::vector<std::string> anyTraits;  // ...only for pawns with at least one of these (`situation.anyTraits`)
    };
    // A situation with stages (`situation.condition` and `stages` in the def): which stage holds is
    // decided by a measured number against the def's thresholds (Mood.cpp, `conditionValue`).
    struct StagedThought {
        DefId def;
        std::string condition;
        std::vector<float> thresholds;  // upper bounds of every stage but the last
        std::vector<std::string> labels;
        std::vector<float> offsets;
    };
    // The number a staged thought's condition measures for this pawn, or false if it has none now.
    bool conditionValue(Entity pawn, const std::string& condition, float& value) const;
    // A condition other systems evaluate (task C15 on): a Thoughts def whose `situation.condition` is
    // `condition` counts for a pawn while `stage` gives a stage number of the def's `stages` (0 first), and
    // not at all for a negative number. `situation.trait` and `situation.notTrait` limit a thought to
    // pawns with, or without, a trait.
    struct Situation {
        std::string condition;
        SituationStage stage;
    };
    bool hasTrait(Entity pawn, const std::string& trait) const;
    // Cannibals and psychopaths are not troubled by people as meat or leather.
    bool eatsPeople(Entity pawn) const { return hasTrait(pawn, "Cannibal") || hasTrait(pawn, "Psychopath"); }
    float traitMoodOffset(Entity pawn) const;
    float traitBreakOffset(Entity pawn) const;
    void addMemory(Mood& mood, const std::string& trigger, int64_t now);

    Things& things_;
    Rooms* rooms_ = nullptr;
    RoomStats* roomStats_ = nullptr;
    TemperatureSystem* temperature_ = nullptr;
    const NeedsSystem& needs_;
    const DefDatabase& defs_;
    MoodRules rules_;
    std::vector<Thought> thoughts_;
    std::function<std::string(Entity)> activeSkill_;
    std::vector<StagedThought> staged_;
    std::vector<Situation> situations_;
};

}  // namespace colony

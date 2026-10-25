#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

struct Event;
class NeedsSystem;
class Rooms;
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

    static MoodRules read(const DefDatabase& defs);
};

// Keeps every pawn's mood once per game second, after the jobs: the target is the base mood plus
// trait offsets, the situations that hold now and the memories not yet faded; the level moves
// towards it; the break band follows the level. Memories are added from the step's events.
class COLONY_API MoodSystem {
public:
    MoodSystem(Things& things, const NeedsSystem& needs, const DefDatabase& defs);

    // Where a pawn slept decides "slept outside" (docs/design/rooms.md); without rooms nothing is outside.
    void setRooms(Rooms* rooms) { rooms_ = rooms; }

    const MoodRules& rules() const { return rules_; }
    // The mood the pawn is heading for (not clamped to the 0 to 100 scale), or its base if it has none.
    float target(Entity pawn, int64_t now) const;
    // The thoughts that count right now, strongest first (ties by name).
    std::vector<ThoughtView> thoughts(Entity pawn, int64_t now) const;
    // The three thresholds as mood levels (0 to 100) for this pawn: its traits move them.
    void breakLevels(Entity pawn, float& minor, float& major, float& extreme) const;

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
    };
    float traitMoodOffset(Entity pawn) const;
    float traitBreakOffset(Entity pawn) const;
    void addMemory(Mood& mood, const std::string& trigger, int64_t now);

    Things& things_;
    Rooms* rooms_ = nullptr;
    const NeedsSystem& needs_;
    const DefDatabase& defs_;
    MoodRules rules_;
    std::vector<Thought> thoughts_;
};

}  // namespace colony

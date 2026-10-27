#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <functional>
#include <string>
#include <vector>

namespace colony {

// Sound cues (task V20, docs/design/sound.md). The sim makes no sound: this turns an event into
// the names of the `SoundCues` defs Unreal should play, read from the `visual.sound` slots of the
// defs involved (a gun's `shot`, a damage type's `hit`, a terrain's `step`...). Plain C++, no Unreal.

enum class SoundMode : uint8_t {
    Once,       // play the cue once
    LoopStart,  // start the looping cue; it runs until a LoopStop with the same cue and the same entity or cell
    LoopStop,
};

// One thing to play. The place is `x, y, z` in metres when `hasPos` (z is the base of the cell's level
// when it came from a cell: Unreal puts it on the ground); else the position of `entity`, followed as it
// moves; with neither it is heard everywhere (an alarm, a horn).
struct SoundPlay {
    std::string cue;
    SoundMode mode = SoundMode::Once;
    bool hasPos = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    Entity entity = kNoEntity;
    CellPos cell;  // the loop's identity when it has no entity (a fire)
};

// The thing's def, so an event that only names an entity (PawnInjured, a bench) can find its slots.
// Null: those events give no sound.
using SoundDefOf = std::function<DefId(Entity)>;

// The cue in a def's `visual.sound.<slot>`; empty if the def has none.
COLONY_API std::string soundSlot(const Def& def, const char* slot);

// The cue of terrain's `step` for a Terrain def (a floor, or the ground); "Step_Dirt" for an unknown one.
COLONY_API std::string soundStepCue(const DefDatabase& db, DefId terrain);

// Appends what `event` sounds like to `out`; nothing for an event that makes no noise. Same event and
// same defs always give the same cues.
COLONY_API void soundForEvent(const DefDatabase& db, const Event& event, const SoundDefOf& defOf,
                              std::vector<SoundPlay>& out);

}  // namespace colony

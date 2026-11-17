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

// A pawn's own voice (docs/design/sound.md "Voices"). A pawn has one voice: Unreal lets a new voice sound cut off the one
// still playing from the same pawn (a scream cuts a hit grunt short), never sounds from anyone else.
enum class SoundVoice : uint8_t {
    None,    // not a voice
    Hit,     // a grunt at a hard melee hit; Unreal plays at most one per pawn every `cooldown` seconds (the cue def's)
    Scream,  // downed or killed: no cooldown, cuts that pawn's hit grunt, never cut by one
};

// The gender of a pawn for its voice: no voice for Unknown (animals, mechanoids, things).
enum class SoundGender : uint8_t { Unknown, Male, Female };

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
    SoundVoice voice = SoundVoice::None;  // set: `entity` is the pawn whose voice it is (and `x, y, z` where it was)
};

// The thing's def, so an event that only names an entity (PawnInjured, a bench) can find its slots.
// Null: those events give no sound.
using SoundDefOf = std::function<DefId(Entity)>;

// A humanlike pawn's gender, for its voice; null or Unknown: no voice sounds.
using SoundGenderOf = std::function<SoundGender(Entity)>;

// The cue in a def's `visual.sound.<slot>`; empty if the def has none.
COLONY_API std::string soundSlot(const Def& def, const char* slot);

// The cue of terrain's `step` for a Terrain def (a floor, or the ground); "Step_Dirt" for an unknown one.
COLONY_API std::string soundStepCue(const DefDatabase& db, DefId terrain);

// Appends what `event` sounds like to `out`; nothing for an event that makes no noise. Same event and
// same defs always give the same cues. With `genderOf`, pawns with a `visual.sound` slot for their gender's voice
// (`femaleHit`, `femaleScream`, `maleHit`, `maleScream`) also cry out: a hit grunt for a melee hit or a shot that does more than
// the cue's `minDamage` (3), `chance` of the time (0.5, decided by a hash of the event so it never touches the game's
// random numbers), and a scream every time when downed and every time a fight kills them (downed first or not).
COLONY_API void soundForEvent(const DefDatabase& db, const Event& event, const SoundDefOf& defOf,
                              std::vector<SoundPlay>& out, const SoundGenderOf& genderOf = {});

// The lovin sound for a pair (docs/design/sound.md "Lovin"): a man and a woman `Voice_LovinCouple` (sneed's six
// recordings), two women or a woman alone (`b` Unknown) `Voice_LovinFem` (the three women's ones), two men or a man alone
// nothing (empty).
COLONY_API std::string soundLovinCue(SoundGender a, SoundGender b);

// The death causes that make a pawn scream (PawnDied's `message`): violence and the blood loss it caused, not cold or illness.
COLONY_API bool soundViolentDeath(const std::string& cause);

}  // namespace colony

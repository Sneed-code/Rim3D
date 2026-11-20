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
// moves; with neither it is heard everywhere (an alarm, a horn). Work at a cell has both: the cell, and the worker in
// `entity`, so Unreal can repeat a work cue (its `repeat` seconds) while that pawn keeps working.
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
    float delay = 0.0f;  // real seconds to wait before it starts (a spent case lands after the shot, debris after a blast)
};

// The thing's def, so an event that only names an entity (PawnInjured, a bench) can find its slots.
// Null: those events give no sound.
using SoundDefOf = std::function<DefId(Entity)>;

// A humanlike pawn's gender, for its voice; null or Unknown: no voice sounds.
using SoundGenderOf = std::function<SoundGender(Entity)>;

// The `step` cue of the ground or floor in a cell (Unreal's StepCueAt), so a bullet in the ground sounds like what it hit:
// dirt, sand, water, stone, wood. Null: dirt.
using SoundStepAt = std::function<std::string(CellPos)>;

// The cue in a def's `visual.sound.<slot>`; empty if the def has none.
COLONY_API std::string soundSlot(const Def& def, const char* slot);

// The cue of terrain's `step` for a Terrain def (a floor, or the ground); "Step_Dirt" for an unknown one.
COLONY_API std::string soundStepCue(const DefDatabase& db, DefId terrain);

// Appends what `event` sounds like to `out`; nothing for an event that makes no noise. Same event and
// same defs always give the same cues. With `genderOf`, pawns with a `visual.sound` slot for their gender's voice
// (`femaleHit`, `femaleScream`, `maleHit`, `maleScream`) also cry out: a hit grunt for a melee hit or a shot that does more than
// the cue's `minDamage` (3), `chance` of the time (0.5, decided by a hash of the event so it never touches the game's
// random numbers), and a scream every time when downed and every time a fight kills them (downed first or not).
//
// Impacts by surface (sneed, 2026-11-20, docs/design/sound.md "Every weapon, surface and job"): a projectile's
// `visual.sound.impactSet` ("Impact" for bullets, "Arrow" for arrows and the pila) names the family, and what it struck the
// surface: `<Set>_<Flesh|Wood|Steel|Stone|Dirt|Sand|Water>`, falling back to the older general cues. Blasts by their source's
// `explosion` slot or their size; a severed part `Gore_Sever` (`Gore_Decap` for a head); a felled tree `Tree_Fall`.
COLONY_API void soundForEvent(const DefDatabase& db, const Event& event, const SoundDefOf& defOf,
                              std::vector<SoundPlay>& out, const SoundGenderOf& genderOf = {},
                              const SoundStepAt& stepAt = {});

// What a step cue says about the ground for a bullet: "Dirt" (grass, soil, carpet, leaves, mud, snow), "Sand", "Water",
// "Stone" (stone, tile), "Wood", "Steel" (metal).
COLONY_API std::string soundSurfaceOfStep(const std::string& stepCue);

// The cue a ProjectileHit plays on what it struck (empty for none): see soundForEvent.
COLONY_API std::string soundImpactCue(const DefDatabase& db, const Event& event, const SoundStepAt& stepAt = {});

// The lovin sound for a pair (docs/design/sound.md "Lovin"): a man and a woman `Voice_LovinCouple` (sneed's six
// recordings), two women or a woman alone (`b` Unknown) `Voice_LovinFem` (the three women's ones), two men or a man alone
// nothing (empty).
COLONY_API std::string soundLovinCue(SoundGender a, SoundGender b);

// Melee by material (docs/design/sound.md): "Wood", "Steel", "Stone" or "Flesh" (cloth, leather) for a stuff or a def
// (its `visual.sound.material`, its stuff categories, its `visual.surface`); empty when it says nothing.
COLONY_API std::string soundMaterialOf(const DefDatabase& db, DefId def);
// The cue a MeleeHit plays: a blade in flesh its own `hit`; a blunt blow `Hit_<Wood|Steel>_Flesh`; a blow on a parrying
// weapon, shield, armour, wall or rock `Hit_<Wood|Steel>_<Flesh|Wood|Steel|Stone>`; fists `Punch` or `Melee_Clang`.
COLONY_API std::string soundMeleeCue(const DefDatabase& db, const Event& event);
// The death causes that make a pawn scream (PawnDied's `message`): violence and the blood loss it caused, not cold or illness.
COLONY_API bool soundViolentDeath(const std::string& cause);

}  // namespace colony

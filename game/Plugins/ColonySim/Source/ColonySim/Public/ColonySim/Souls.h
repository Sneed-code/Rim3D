#pragma once

#include "ColonySim/Api.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class DefDatabase;

// Elden Ring style melee (sneed 2026-11-27, docs/design/souls-combat.md): stamina, poise, light and heavy attacks with
// combos, charged heavies, running, jumping, rolling and guard-counter attacks, a guard that eats stamina and breaks.
// All of it is switched by SoulsRules `enabled` (data/defs/Rules/SoulsRules.json): off, melee is what it was before.
// Stamina and poise are on every pawn that fights in melee, people, animals and big creatures alike; movesets (the
// attack clips per kind) are per weapon kind, so a new weapon or a creature gets its own list of clips and nothing else.

// A pawn's stamina (Elden Ring's green bar). Absent: full. Spent by attacks, rolls and blocked hits; comes back over
// SoulsRules `regenSeconds` when the pawn isn't attacking, rolling or holding up its guard.
struct Stamina {
    float value = 100.0f;
    int64_t lastUse = 0;  // step it last spent any
};

// A pawn's poise meter (Elden Ring's hidden stance). Absent: full. Each melee hit takes its attack's poise damage off;
// at zero the pawn's poise breaks: a stagger that stops what it is doing, or a knockdown (a heavy kind's break, or a
// second break soon after the first), never two knockdowns within `knockdownImmuneSteps`. It refills when it breaks,
// and after `regenSteps` without a hit.
struct PoiseMeter {
    float value = -1.0f;            // below 0: full (its max is worked out from the body)
    int64_t lastHit = 0;            // step it last took poise damage
    int64_t lastBreak = -1000000;   // step its poise last broke
    int64_t knockdownImmuneUntil = 0;
};

// The kinds of attack (Elden Ring's R1, R2, charged R2, running R1/R2, jumping R1/R2, rolling R1, guard counter), and
// the critical attacks (sneed 2026-11-27): a riposte through the chest of a foe whose guard or poise just broke, a
// backstab from behind, and a stab down into a foe knocked to the ground.
enum class SoulsAttack : uint8_t {
    Light,
    Heavy,
    Charged,
    Running,
    RunningHeavy,
    Jump,
    JumpHeavy,
    Roll,
    GuardCounter,
    Riposte,
    Backstab,
    GroundStab,
    Count,
    None = 255,
};
constexpr size_t kSoulsAttackKinds = static_cast<size_t>(SoulsAttack::Count);
COLONY_API const char* soulsAttackName(SoulsAttack kind);
COLONY_API bool soulsAttackFromName(const std::string& name, SoulsAttack& out);
// A critical attack (riposte, backstab, ground stab): it can't be guarded or dodged and always meets its target.
inline bool soulsCriticalKind(SoulsAttack kind) {
    return kind == SoulsAttack::Riposte || kind == SoulsAttack::Backstab || kind == SoulsAttack::GroundStab;
}

// One kind's numbers. Stamina is a share of a 100 point bar (sneed: light 10, heavy 20).
struct SoulsKind {
    float stamina = 10.0f;      // points it costs
    float damage = 1.0f;        // times the weapon's blow
    float poise = 1.0f;         // times the weapon's poise damage
    float speed = 1.0f;         // the clip plays this much faster than its own pace
    bool knockdown = false;     // its poise break knocks down instead of staggering
    bool hyperArmor = false;    // while it winds up, poise damage taken is times SoulsRules hyperArmorTaken
    float maxWarp = 0.8f;       // metres the attacker may be carried in to stand right for the blow (back: warpBack)
};

// A weapon kind's (or a creature's) clips per attack kind: MotionClips def names. The light and heavy lists are combo
// chains: each press in a chain plays the next. An empty kind falls back to light (Running, Roll) or heavy (the rest).
struct SoulsMoveset {
    std::string key;                  // the name in SoulsRules `movesets`
    std::vector<std::string> weapons;  // weapon defs that use it (or race defs, for a creature's body)
    std::vector<std::string> styles;   // or every weapon of these hold attack styles
    std::array<std::vector<std::string>, kSoulsAttackKinds> clips;
    float speed = 1.0f;               // the whole moveset's pace
    float weaponPoise = 12.0f;        // poise damage of a light hit
};

// The numbers (Rules/SoulsRules). Elden Ring's where it has them (as players measured them), sneed's where he set
// them (the stamina bar), else ours.
struct SoulsRules {
    bool enabled = false;
    // Stamina (sneed 2026-11-27): a 100 bar; light 10, heavy 20, a blocked hit half its attack's cost, a roll 30; it
    // refills over 2 s of no use; holding up a guard stops it refilling (for now). An action needs more than 0 left.
    float staminaMax = 100.0f;
    float regenSeconds = 2.0f;
    int regenDelaySteps = 0;          // steps after a use before it starts refilling
    float rollCost = 30.0f;
    float blockShare = 0.5f;
    float guardHoldDrainPerSecond = 0.0f;  // a raised guard's own drain (0: it only stops the refill)
    float unarmedCost = 5.0f;         // a person's punch
    float animalCost = 10.0f;         // an animal's or a creature's bite or claw, by default
    // Poise. A person's is `personPoise`; an animal's scales with its body size.
    float personPoise = 30.0f;
    float poisePerBodySize = 30.0f;
    float minPoise = 10.0f;
    float noKnockdownBodySize = 2.5f;  // a body this big only staggers
    int poiseRegenSteps = 900;        // Elden Ring: poise refills 30 s after the last hit
    int knockdownImmuneSteps = 300;   // no second knockdown sooner (sneed: knockdowns can't be chained)
    int doubleBreakSteps = 150;       // a second break this soon after the first knocks down
    int staggerSteps = 30;            // a poise break's stagger: no attacking, guarding or rolling
    float hyperArmorTaken = 0.5f;     // poise damage taken while a hyper-armoured attack winds up
    float animalPoisePerPower = 0.8f;  // a creature's poise damage per point of its tool's power (no moveset)
    // Combos and canceling, as shares of the attack's recovery (the part of its clip after the blow).
    float chainShare = 0.3f;          // the next attack of the combo can start this far into the recovery
    float cancelShare = 0.55f;        // a roll, a step or a raised guard can cut it short from here on
    int comboResetSteps = 12;         // the combo starts over this long after an attack ended
    int rollAttackSteps = 15;         // an attack this soon after a roll is a rolling attack
    // Charging a heavy (holding the button): it holds at chargeShare of its wind-up, for up to maxChargeSteps; held
    // at least minChargeSteps it is a charged heavy.
    float chargeShare = 0.55f;
    int minChargeSteps = 12;
    int maxChargeSteps = 30;
    // The guard. A raised shield takes a frontal blow for `shieldNegation` of its damage, a weapon for
    // `weaponNegation`; the blocker loses the attack's cost times blockShare in stamina, and with too little its guard
    // breaks (the guard drops, guardBreakSteps of stagger). A blocked hit stuns the blocker for blockStunSteps, and
    // opens a guard counter (a heavy press) for counterSteps.
    float shieldNegation = 1.0f;
    float weaponNegation = 0.6f;
    float guardCos = 0.25f;           // a guard covers blows within about 75 degrees of straight ahead
    int guardBreakSteps = 45;
    int blockStunSteps = 10;
    int counterSteps = 30;
    // Critical attacks (Elden Ring: a light attack at a foe whose stance or guard just broke is a riposte, one from
    // behind a backstab; both lock the two together and throw the victim to the ground, 2.5 to 4 times a normal blow).
    // A riposte needs the victim stunned by a poise or guard break, in front within criticalRange metres; a backstab
    // needs the attacker within backstabRange metres behind (the victim's facing and the way to the attacker at a cosine
    // under backstabCos). The victim is held for the wind-up and criticalHoldSteps after, then knocked down.
    float criticalRange = 1.8f;
    float backstabRange = 1.5f;
    float backstabCos = -0.6f;        // about 53 degrees either side of straight behind
    int criticalHoldSteps = 12;
    float criticalKnockdown = 6.0f;   // the knockdown's force (CombatSystem::knockDown's push)
    float guardBreakPoise = 2.0f;     // a guard break takes this share of the blocker's poise on top
    // Lining up the blow (Elden Ring's tracking): the attacker turns to its target until `trackShare` of the wind-up,
    // and is carried in or back to stand `idealInset` metres inside its reach at the blow.
    float trackShare = 0.85f;
    float idealInset = 0.4f;
    float warpBack = 0.3f;
    // The fighter AI (ours).
    float aiStaminaFloor = 25.0f;     // below this it backs off and lets its bar refill
    int aiComboMax[2] = {1, 3};       // light presses in a row at Melee skill 0 and 20
    float aiHeavyChance = 0.25f;
    float aiHeavyVsGuard = 0.6f;      // heavy against a raised guard (it eats stamina)
    float aiChargeChance = 0.35f;     // a heavy that is charged (when the target is open)
    float aiLeapMin = 2.6f;           // a charging fighter leaps in with a running heavy from this far...
    float aiLeapMax = 4.2f;           // ...to this far
    float aiLeapChance = 0.02f;       // per step it is in that band
    float aiGuardChance = 0.7f;       // a shield-bearer raises its guard against a swing at it
    float aiCounterChance = 0.6f;     // and counters after a block
    std::array<SoulsKind, kSoulsAttackKinds> kinds{};
    std::vector<SoulsMoveset> movesets;
};

COLONY_API void soulsReadRules(const DefDatabase& defs, SoulsRules& rules);

}  // namespace colony

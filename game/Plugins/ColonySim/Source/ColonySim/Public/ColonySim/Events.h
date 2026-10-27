#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"

#include <cstdint>
#include <string>

namespace colony {

// Sim to Unreal: what happened during one step, for sounds, effects, animations and UI messages.
// Positions and states are in the snapshot; events say what changed. See docs/design/bridge.md.
enum class EventType : uint8_t {
    ThingSpawned,    // a thing appeared on the map (made, or put down)
    ThingDespawned,  // a thing left the map but still exists (e.g. picked up to carry)
    ThingDestroyed,  // a thing stopped existing; forget its id (a ThingDespawned comes first if it was on the map)
    CommandFailed,   // a command could not be applied
    PawnInjured,     // a pawn got an injury (docs/design/health.md)
    PawnPartLost,    // a pawn lost a body part (a PawnInjured does not come with it)
    PawnDowned,      // a pawn can no longer act (pain shock, unconscious, or can't move)
    PawnGotUp,       // a downed pawn recovered enough to act again
    PawnDied,
    PawnAction,      // a pawn did something with its hands that Unreal animates (picked up, put down)
    JobStarted,      // a pawn took a job (docs/design/jobs.md)
    JobEnded,        // a pawn's job ended: `message` says "done", "failed: why" or "interrupted: why"
    PawnAte,         // a pawn finished eating (docs/design/needs.md); `part` 1 if the meal had human flesh in it
    PawnWokeUp,      // a pawn got up after sleeping
    FrameStarted,    // a blueprint got its first materials and is a frame now (docs/design/construction.md)
    PawnMoodBand,    // a pawn's mood crossed a mental break threshold (docs/design/mood.md)
    CellMined,       // a rock or ore cell was dug out (docs/design/mining.md)
    PlantRemoved,    // a plant was felled or cut down
    PlantHarvested,  // a plant was picked and stays, grown back to a lower stage
    DesignationsChanged,  // marks were added, removed or finished: read the snapshot's designations
    ShotFired,       // a pawn fired a projectile (docs/design/combat.md)
    ProjectileHit,   // a projectile stopped: it hit something, or flew off the map or too long
    RecipeDone,      // a pawn finished one run of a bill (docs/design/bills.md)
    PowerChanged,    // a building gained or lost power (docs/design/power.md)
    ShortCircuit,    // a short circuit emptied a net's batteries (docs/design/power.md)
    ItemSpoiled,     // food rotted away or an item wore out in the open (docs/design/spoilage.md)
    PawnPoisoned,    // a pawn ate a poisoned meal and got food poisoning
    ResearchDone,    // a research project was finished (docs/design/research.md)
    GrenadeThrown,   // a pawn released a grenade (docs/design/explosives.md)
    GrenadeLanded,   // a thrown grenade came to rest; its fuse is running
    Explosion,       // a blast went off
    PawnKnockedDown, // a blast threw a pawn to the ground
    RaidArrived,     // a raid walked in (docs/design/raids.md)
    SquadChanged,    // a raid squad's morale changed
    AlarmChanged,    // the colony alarm went on or off
    PawnLeftMap,     // a pawn walked off the map and is gone (a ThingDestroyed follows)
    AnimalTamed,     // a wild animal became the colony's (docs/design/husbandry.md)
    AnimalReleased,  // a tame animal went wild again
    AnimalTrained,   // one successful training attempt: a step towards a trick
    AnimalAttemptFailed,  // a taming or training attempt did not work
    AnimalSlaughtered,    // a colonist killed a tame animal
    AnimalStarved,        // a tame animal died of hunger
    MeleeSwing,      // a pawn began a melee swing: the hit comes `amount` steps later (docs/design/melee.md)
    MeleeHit,        // the hit moment of a swing: `message` says "hit", "deflected", "blocked", "dodged" or "missed"
    PawnTended,      // a doctor finished tending a patient (docs/design/medicine.md)
    PawnScarred,     // a healed injury left a scar or an old wound (docs/design/medicine.md)
    PawnSick,        // a pawn caught a disease, or a wound turned infected (docs/design/diseases.md)
    PawnRecovered,   // a pawn's disease ran its course (docs/design/diseases.md)
    ApparelDestroyed,  // a worn piece wore out or was shot to pieces: `thing` the wearer, `def` the piece's def (docs/design/apparel.md)
    WeatherChanged,  // the weather began to change into another kind (docs/design/weather.md)
    SeasonChanged,   // a new season began
    TalentPoint,     // the protagonist earned a talent point (docs/design/talents.md)
    AnimalMated,     // a male mated with a female (docs/design/breeding.md)
    AnimalBorn,      // a tame animal was born to a mother, or hatched
    EggLaid,         // a hen laid an egg
    EggHatched,      // a fertilized egg hatched into a young animal
    AnimalGathered,  // a colonist milked or sheared an animal
    TrapSprung,      // a pawn set off a trap (docs/design/defense.md)
    TrapRearmed,     // a sprung trap was set again
    WeaponSwitched,  // a pawn finished switching weapons (P1, docs/design/weapons.md): `thing` the pawn, `def` the weapon now in its hands, `target` that weapon
    PlantSown,       // a colonist sowed a crop in a growing zone: `cell`, `def` the crop (docs/design/farming.md)
    Letter,          // the narrator sent an incident (docs/design/narrator.md): `def` the Incidents def, `message` the
                     // letter's text, `count` its LetterKind, `amount` its threat points, `cell` where (if it has a
                     // place), `squad` a raid's squad
    FireStarted,     // a fire began in a cell (docs/design/fire.md)
    FireOut,         // a fire went out: burnt out, rained out or beaten out
    PawnCaughtFire,  // a pawn is on fire
    PawnFireOut,     // a pawn is no longer on fire
    Interaction,     // one colonist talked to another (C20, docs/design/social.md): `thing` the talker, `target` the one spoken to, `message` the Interactions def, `count` 1 if it worked, 0 if refused, `amount` the listener's opinion of the talker after
    BondChanged,     // a stored bond formed or ended: `thing` and `target` the two, `message` the bond kind ("Lover", "Spouse", "ExLover", "ExSpouse"), `count` 1 formed, 0 ended
    SocialFight,     // a fight with fists began or ended: `thing` and `target` the two, `message` "started" or "ended: why"
    PowerCast,       // a resonance power went off (docs/design/resonance.md): `thing` the caster, `def` the Powers def, `target` the pawn it
                     // was used on, `cell` where a jump took it, (x, y, z) where the target is now, `amount` the caster's strain now,
                     // `count` the steps the effect lasts (0 for a jump)
    PrisonerCaptured,   // a downed enemy was laid in a prison bed: `thing` the prisoner, `target` the warden (docs/design/prisoners.md)
    PrisonerChatted,    // a warden finished a talk with a prisoner: `thing` the warden, `target` the prisoner, `amount` its resistance now, `message` "reduced", "recruit failed" or "no change"
    PrisonerRecruited,  // a prisoner joined the colony: `thing` the new colonist, `target` the warden
    PrisonerReleased,   // a prisoner was set free and walks off the map: `thing` the pawn, `target` the warden (none for a dev command)
    PrisonBreak,        // prisoners broke out (the hook for T3): `count` how many, `message` the cause
    OutOfFuel,       // a building's fuel ran out (docs/design/fuel.md)
    Refueled,        // a pawn put fuel into a building
    RoofCollapsed,   // a roof or an upper floor fell (M3, docs/design/roofs.md): `cell`, `message` "roof" (a built roof), "mountain" (the thick roof of a mined-out cell) or "floor" (an upper floor), `count` how many things and pawns it hit
    FirefoamPopped,     // a firefoam popper went off (docs/design/fire.md): `thing` the popper (destroyed right after), `def` its def, `cell` its cell, `amount` the foam radius
    GearRepaired,       // a pawn mended a piece of gear at a bench (docs/design/repair.md): `thing` the pawn, `target` the gear, `def` its def, `cell` where the pawn stood, `amount` the hit points restored
    WeaponUpgraded,     // a pawn fitted an upgrade part to a gun: `thing` the pawn, `target` the gun, `def` the part's def, `cell` where the pawn stood
    AnimalHunted,       // a hunter's target died (docs/design/hunting.md): `thing` the animal (a corpse now), `target` the hunter, `cell` where
    AnimalButchered,    // a corpse was cut up (docs/design/hunting.md): `thing` the butcher, `def` the animal's def, `target` the table, `cell` where, `amount` the meat, `part` the leather
    PawnMentalBreak,       // a colonist snapped: a mental break began (docs/design/mental-breaks.md)
    PawnMentalBreakEnded,  // it ended: `message` says how ("recovered", "downed", "died", "stopped")
    PawnInsulted,          // a pawn in an insulting spree insulted another
    PawnLeftColony,        // a pawn gave up and walked off the map for good
    VisitArrived,          // visitors, a trade caravan or an orbital trader came (C23, docs/design/trade.md): `squad` the visit id, `thing` the trader pawn (none for visitors and a ship), `amount` the VisitKind (0 visitors, 1 caravan, 2 orbital), `message` the Factions defName
    VisitLeft,             // a visit is over and gone: `squad` the visit id, `amount` its VisitKind, `message` the faction
    TradeDone,             // the colony traded: `squad` the visit id, `thing` the trader, `amount` the silver the colony got (minus: paid), `message` the faction (and what was given free)
    GoodwillChanged,       // a faction's goodwill moved: `amount` by how much, `message` the Factions defName
    PawnTookDrug,          // a pawn took a dose of a drug (docs/design/drugs.md): `thing` the pawn, `def` the drug's Things def, `amount` the high's severity now, `cell` where
    PawnAddicted,          // it became addicted: `thing` the pawn, `def` the Chemicals def, `message` its defName, `amount` the need
    AddictionCured,        // an addiction healed away: the same fields
    DrugOverdose,          // a dose took the overdose past minor (0.5), major (0.75) or fatal (1): `thing` the pawn, `def` the drug, `amount` the severity, `message` "minor", "major" or "fatal"
    DisasterStarted,       // a disaster condition began (T3, docs/design/disasters.md): `message` its name ("solar flare", "toxic fallout", "volcanic winter", "flashstorm", "psychic drone", "psychic soothe"), `count` its DisasterKind, `amount` its length in game hours, `cell` a flashstorm's centre
    DisasterEnded,         // it is over (same fields)
    LightningStrike,       // a flashstorm's bolt hit `cell` at (x, y, z); the blast and fires that follow are their own events
    QuestOffered,          // a quest was offered (docs/design/quests.md): `count` its id, `def` its Quests def, `message` the def's name
    QuestAccepted,         // the player accepted it
    QuestDeclined,         // the player declined it
    QuestExpired,          // nobody answered in time (or, once accepted, the raid could not be sent): `message` says which
    QuestCompleted,        // it was done and paid: `message` says how ("joined", "delivered", "the raid is over", "no raid came"); `squad` the raid
    QuestFailed,           // it went wrong: `message` says why
    DropPodLanded,         // a raider's pod came down (F12, docs/design/raid-strategies.md): `thing` the raider, `cell` where, `squad`
    SappersBreaking,       // raiders began digging through a wall (F12): `cell` the wall, `squad`, `count` how many dig
    ItemStolen,            // a thief lifted loose items (F12): `thing` the thief, `target` the item it carries, `def` its def, `count`
    PawnKidnapped,         // a raider lifted a downed colonist (F12): `thing` the kidnapper, `target` the colonist. `message` "left the map" when it got away with it
    InspirationStarted,    // a colonist became inspired (docs/design/inspirations.md): `thing` the pawn, `message` the Inspirations defName, `amount` its days
    InspirationEnded,      // it ended: `message` "why:defName" (used, expired, dismissed), `amount` how many game seconds it lasted
    ShipReactorStarted,    // the ship reactor began its activation (docs/design/ship.md): `thing` the reactor, `amount` days it takes
    ShipWave,              // a wave of raiders came because of the reactor: `amount` the wave number (the raid's own events follow)
    ShipReactorReady,      // the activation finished: the ship can launch
    ShipReactorLost,       // the reactor was destroyed or taken down: the activation is lost, `message` says which
    ShipLaunched,          // the ship left and the game is won: `amount` how many pawns went (a PawnLeftMap each)
    PrisonerExecuted,      // a warden executed a prisoner (docs/design/prisoners.md): `thing` the prisoner (dead), `target` the warden, `message` "guilty" or "innocent"
    OrganHarvested,        // an organ was taken from a living pawn: `thing` the victim, `target` the surgeon, `part` the body part, `message` "colonist" or "prisoner" (anyone who is not one of ours)
    OperationDone,      // a doctor finished an operation (H3, docs/design/surgery.md): `thing` the doctor, `target` the patient, `def` the Recipes def, `part` the body part, `amount` the success chance, `message` "success", "failed" or "the patient died"
    DeepDepositFound,      // a scanner found deep resources (docs/design/deep-mining.md): `def` the item, `cell` the middle of the lump, `count` its cells, `thing` the scanner (none for the long-range hook)
    DeepPortionDrilled,    // a deep drill finished a portion: `thing` the drill, `def` what came up (a rock chunk if nothing is under it), `amount` how many, `cell` where it was taken from
};

// What stopped a projectile (ProjectileHit).
enum class ProjectileHitKind : uint8_t {
    None,      // (trace only) nothing in the way
    Ground,    // the ground
    Rock,      // a rock cell (mountain)
    Building,  // a building (`target`); it lost `amount` hit points
    Pawn,      // a pawn (`target`) in the body part `part`; health events follow
    Armor,     // a pawn's armor stopped it (`target`, `part`)
    Gone,      // flew off the map or for too long, hitting nothing
};

// What a PawnAction event's pawn did. Later jobs add theirs (chop, mine, build, eat); Unreal plays
// an animation for each, and a short default one for any it doesn't know.
enum class PawnAction : uint8_t {
    PickUp,   // took `target` off the map to carry it
    PutDown,  // put the carried `target` down (it gets a ThingSpawned too), or the stack it was added to
    Eat,      // started eating the carried `target`
    LieDown,  // lay down to sleep in the bed `target`, or on the ground (no target)
    GetUp,    // got up from sleeping
    Build,    // started work on `target`, a frame (once per job; the work itself takes game seconds)
    Deconstruct,  // started taking down `target`, a building
    Mine,     // swung the pick at the rock in `cell`: once for every hit, for sounds and dust
    Cut,      // started cutting down or felling the plant in `cell`; the work goes on until its JobEnded
    Harvest,  // started picking from the plant in `cell`
    Reload,   // started reloading the gun `target` (docs/design/aiming.md); it is ready again when shotReadyIn is 0
    Dodge,    // sidestepped into `cell` out of the way of the projectile `target`
    Cook,         // started work on a bill at `target`, a work building (once per run of the bill)
    Research,     // started research at `target`, a research bench (once per job)
    Clean,        // started cleaning the filth in `cell`; a layer comes off every few seconds until its JobEnded
    Throw,        // started winding up a throw (docs/design/explosives.md); the release is a GrenadeThrown event
    Dive,         // ran or dived toward `cell` to get away from the grenade `target`
    Handle,       // started taming, training or feeding `target`, an animal; goes on until its JobEnded
    Slaughter,    // started to kill `target`, a tame animal
    Block,        // raised its melee weapon against the swing of `target` (none: the player's own block)
    Switch,       // started switching weapons (P1): `target` the weapon coming to the hands, `amount` the steps it takes
    Roll,         // started a dodge roll (P2): `cell` where it began, (x, y) the unit direction, `amount` the steps from start-up to the end of recovery
    Extinguish,   // started beating out the fire in `cell`; goes on until its JobEnded (docs/design/fire.md)
    Tend,         // started tending `target`, a patient (once per job; the work takes game seconds)
    LiftPawn,     // lifted `target`, a downed pawn, to carry it (its position follows this pawn's until LayPawn)
    LayPawn,      // laid the carried `target` down in a bed (or where it stood, if the bed was lost)
    Cast,         // started the warm-up of a resonance power (X2) at `target` (a jump: to `cell`); `amount` the steps until it goes off
    Chat,         // started talking to `target`, a prisoner (once per job; the talk takes game seconds)
    FeedPawn,     // started handing food to `target`, a prisoner
    Refuel,       // started putting fuel into `target`, a building (once per refuel job; the work goes on until its JobEnded)
    Repair,       // started mending gear or fitting a part at `target`, a bench (once per RepairGear job; the work goes on until its JobEnded)
    Butcher,      // started cutting up `target`, a corpse (once per job; the work goes on until its JobEnded)
    DropCorpse,   // put the carried `target`, a corpse, down in `cell`
    Rearm,        // started setting the sprung trap `target` again (once per job; the work goes on until its JobEnded)
    Punch,        // swung a fist at `target`, a pawn or a thing (a tantrum or berserk break: docs/design/mental-breaks.md)
    Operate,      // started an operation on `target`, a patient in bed (once per job; the work goes on until its JobEnded)
    Scan,         // started working a ground-penetrating scanner `target` (once per job; the work goes on until its JobEnded)
    Drill,        // started working a deep drill `target` (once per job; the work goes on until its JobEnded)
};

// One flat struct for every event type; each type uses only the fields named for it.
struct Event {
    EventType type = EventType::ThingSpawned;
    Entity thing = kNoEntity;  // ThingSpawned, ThingDespawned, ThingDestroyed; PawnAction, Job*: the pawn
    DefId def;                 // ThingSpawned, ThingDespawned, ThingDestroyed; Job*: the pawn's def
    CellPos cell;              // ThingSpawned: its cell (for a building, the footprint origin); Job*: the job's cell
    JobType job = JobType::None;  // JobStarted, JobEnded
    uint64_t command = 0;      // CommandFailed: the id Sim::submit returned
    std::string message;       // CommandFailed: why, in plain words; PawnDied: the cause ("blood loss"); JobEnded: how it ended
    PawnAction action = PawnAction::PickUp;  // PawnAction
    Entity target = kNoEntity;               // PawnAction: the thing it was done to; Job*: what the job is about, if any
    // PawnInjured: `def` is the injury's Hediffs def; PawnPartLost: the Hediffs def is MissingBodyPart.
    // `thing` is the pawn, `cell` where it stands, `part` the index of the body part in its Bodies
    // def (depth-first, torso 0), `amount` the hit points of damage dealt to it.
    int part = -1;
    // PawnAte: `thing` the pawn, `def` the food's Things def, `amount` the nutrition gained, `cell`
    // where it stood and `message` "table" (in a chair at a table) or "ground". PawnWokeUp: `thing`
    // the pawn, `target` the bed it slept in or none, `message` "bed" or "ground", `amount` its rest.
    // PawnMoodBand: `thing` the pawn, `message` the new band ("stable", "minor", "major", "extreme"),
    // `amount` its mood.
    // CellMined: `cell` the cell, `def` the rock or ore's Things def, `amount` its hit points. The
    // chunk or ore it left comes as a ThingSpawned. PlantRemoved, PlantHarvested: `cell`, `def` the
    // plant's Things def, `amount` how many items it gave (they come as a ThingSpawned); `message`
    // "felled" (a tree) or "cut" for removed, "picked" for harvested. PawnAction Mine, Cut, Harvest:
    // `thing` the pawn, `cell` the cell worked on, `def` the pawn's def.
    // RecipeDone: `thing` the pawn, `def` the Recipes def, `target` the work building, `cell` where the
    // pawn stood, `amount` how many products were made, `message` "bill finished" if that was the
    // bill's last run (a "Do X times" bill that reached 0, which is removed), else empty.
    // PowerChanged: `thing` the building (a generator, consumer or battery; not conduits), `def` its
    // def, `cell` its origin, `message` "on" (in a net that works and switched on) or "off".
    // ResearchDone: `thing` the pawn who finished it (none if a dev command did), `message` the Research
    // def's defName, `amount` its cost in points.
    // PawnTended: `thing` the doctor, `target` the patient, `def` the medicine used (none if there was none),
    // `amount` the tend quality (0 to 1.3), `message` "medicine" or "no medicine", "self" if the doctor tended itself.
    // PawnScarred: `thing` the pawn, `def` the Scar or OldWound hediff, `part` the body part, `amount` its severity.
    // PawnSick: `thing` the pawn, `def` the Disease hediff, `part` the wound's body part for an infection (-1 otherwise),
    // `amount` its severity. PawnRecovered: `thing` the pawn, `def` the disease, `part` as above.
    // AnimalTamed, AnimalReleased, AnimalSlaughtered, AnimalStarved: `thing` the animal, `target` the colonist
    // (none for a release or a starving), `cell` where. AnimalTrained: also `message` the trick (Trainable name),
    // `amount` the steps it has now and `part` the steps it needs. AnimalAttemptFailed: `message` "tame" or the
    // trick, `amount` the chance the attempt had.
    // AnimalMated: `thing` the male, `target` the female, `def` their def, `cell` where; `message` "pregnant" (she carries young
    // now) or "fertilized" (a hen's next egg is), else empty. AnimalBorn: `thing` the young animal, `target` its mother (none
    // when it hatched), `amount` the litter size. EggLaid: `thing` the egg item (the stack it joined), `target` the hen, `def` the
    // egg, `message` "fertilized" or empty, `amount` the eggs laid. EggHatched: `thing` the young animal, `def` the egg's def.
    // AnimalGathered: `thing` the animal, `target` the colonist, `def` the product, `amount` how many, `message` "milk" or "wool".
    // TrapSprung: `thing` the trap (it may be gone: an IED always is), `def` its def, `target` the pawn that set it off,
    // `cell` its cell, (x, y, z) where. The hurt come as PawnInjured and the blast as Explosion. TrapRearmed: `thing`,
    // `def`, `cell`. A turret's shots are ShotFired with `thing` the turret and `def` its gun's Things def.
    // FireStarted: `cell`, `amount` its size. FireOut: `cell`, `message` "burnt out" (nothing left) or "beaten" (a colonist put
    // it out); rain and snow end fires as "burnt out". PawnCaughtFire, PawnFireOut: `thing` the pawn, `cell` where it stands.
    // Things the fire destroys come as ThingDestroyed, a plant as PlantRemoved with `message` "burnt".
    // TalentPoint: `thing` the protagonist, `def` its def, `amount` the points it can spend now.
    // ShortCircuit: `thing` the building it happened at, `cell` its origin, `amount` the energy
    // released from the net's batteries, Wd.
    // ItemSpoiled: `thing` the item (it is destroyed right after; forget its id), `def` its def, `cell` where it
    // lay, `message` "rotted" or "weathered". PawnPoisoned: `thing` the pawn, `def` the meal's def, `target` the
    // meal, `cell` where the pawn stands.
    // WeatherChanged: `def` the new Weathers def, `message` its label, `amount` the outdoor temperature.
    // SeasonChanged: `message` the season's name, `amount` its number (0 spring to 3 winter).
    // OutOfFuel: `thing` the building, `def` its Things def, `cell` the cell of its position. Refueled: the same, and
    // `amount` how many items went in. PawnAction Refuel: `thing` the pawn, `target` the building.
    // PawnMentalBreak: `thing` the pawn, `message` the MentalBreaks defName, `amount` its mood.
    // PawnMentalBreakEnded: `thing` the pawn, `message` how it ended, `amount` how many game seconds it lasted.
    // PawnInsulted: `thing` the one insulting, `target` the one insulted. PawnLeftColony: `thing` the pawn (a
    // ThingDestroyed for it and what it took comes with the event), `message` the break. PawnAction Punch:
    // `target` the pawn or thing hit.
    float amount = 0.0f;
    // PawnAction Roll: see there.
    // GrenadeThrown: `thing` the thrower, `def` the grenade's Things def, `target` the grenade entity
    // (it is in the snapshot's projectiles), (x, y, z) where it left the hand, `amount` the steps until
    // it lands. GrenadeLanded: `thing` the grenade entity, `def` its Projectiles def, (x, y, z) where it
    // lies, `amount` the fuse in steps. Explosion: `thing` the grenade entity (none for a dev blast),
    // `target` who threw it, `def` the damage type's DamageTypes def, `amount` the radius, (x, y, z) the
    // centre, `cell` its cell; hurt pawns and destroyed buildings follow as their own events.
    // PawnKnockedDown: `thing` the pawn, `amount` the steps it stays down, (x, y, z) where it landed.
    // ShotFired: `thing` the shooter, `def` its gun's Things def, `target` the projectile, (x, y, z)
    // the muzzle. ProjectileHit: `thing` the projectile (forget its id), `def` its Projectiles def,
    // `hit` what stopped it, `target` the pawn or building hit, `part` the body part hit (index as
    // above), `amount` the damage dealt after armor, (x, y, z) where. Metres, z above sea level.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    ProjectileHitKind hit = ProjectileHitKind::None;
    // MeleeSwing: `thing` the attacker, `target` whom it swings at (none: straight ahead), `def` its
    // melee weapon's Things def (or its own def for fists, teeth...), `message` the tool ("blade",
    // "left fist"), `amount` steps to the hit moment, `cell` where it stands. MeleeHit: the same
    // `thing` and `def`; `target` the pawn struck (or the one aimed at if none was), `hit` Pawn, Armor
    // (deflected), Building or None, `part` and `amount` as for ProjectileHit, (x, y, z) where the
    // strike ended. Health events follow a hit.
    // RaidArrived: `squad` its id, `message` the Factions def, `count` its pawns, `amount` its raid
    // points, `cell` where it walked in. SquadChanged: `squad`, `message` the new state ("Advance",
    // "Hold", "FallBack", "Rout", or "gave up" for a rout because time ran out), `count` its pawns.
    // AlarmChanged: `message` "on: why" or "off: why". PawnLeftMap: `thing` the pawn, `cell` where
    // it left, `squad` its raid squad.
    uint32_t squad = 0;
    int count = 0;
};

COLONY_API const char* eventTypeName(EventType type);
COLONY_API const char* pawnActionName(PawnAction action);
COLONY_API const char* projectileHitKindName(ProjectileHitKind kind);

}  // namespace colony

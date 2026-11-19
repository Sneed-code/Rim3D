#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class CombatSystem;
class FighterSystem;
class HealthSystem;
class InspirationSystem;
class JobSystem;
class Map;
class RaidSystem;
class Rng;
class Rooms;
class TradeSystem;
class StateHasher;
class Things;
struct GameTime;

// Prisoners (task C22). See docs/design/prisoners.md.
//
// A downed enemy is carried to a prison bed by a warden and becomes a prisoner: it keeps its own side (`Faction`) but nobody
// shoots it, it lives in its cell, wardens feed it and talk to it, and what the talk does is the prisoner's mode: wear
// down its resistance, recruit it when the resistance is gone, or set it free. Unreal does not include this header: it
// reads `prisoner`, `prisonerMode`, `resistance` and `prisonBed` from the snapshot and sends SetPrisonerModeCommand and
// SetPrisonBedCommand.

// What a warden's talk with a prisoner does (RimWorld's prisoner interaction mode).
enum class PrisonerMode : uint8_t {
    NoInteraction,     // nobody talks to it
    ReduceResistance,  // each talk lowers its resistance, down to 0
    AttemptRecruit,    // the same, and once the resistance is 0 each talk tries to recruit it
    Release,           // the next talk sets it free (it walks off the map)
    Execute,           // a warden kills it (docs/design/prisoners.md, Executions)
    Enslave,           // each talk lowers its will; once the will is 0 the next talk makes it the colony's slave (Slaves)
    ReduceWill,        // each talk lowers its will, down to 0, and no more
};

COLONY_API const char* prisonerModeName(PrisonerMode mode);
// Parses a name from prisonerModeName; false if it is not one.
COLONY_API bool prisonerModeFromName(const std::string& name, PrisonerMode& out);

// On a pawn held in a prison bed. Every field is saved.
struct Prisoner {
    PrisonerMode mode = PrisonerMode::ReduceResistance;
    float resistance = 0.0f;   // 0 when it can be recruited
    int64_t capturedAt = 0;    // game second
    int64_t nextChat = 0;      // game second from which a warden may talk to it again
    int64_t guiltyUntil = 0;   // it harmed the colony (it was captured in a raid) until this game second; an execution before is justified
    float will = 0.0f;         // worn down by Enslave and ReduceWill; 0 when it can be enslaved
};

// On a pawn the colony holds as a slave (docs/design/prisoners.md, Slaves): it is on the player's side and works, but not as a
// warden, hunter, artist or researcher, and slower; its suppression falls by itself, wardens raise it, and slaves whose
// suppression is low may rise up. Every field is saved.
struct Slave {
    float suppression = 0.5f;   // 0 to 1
    int64_t enslavedAt = 0;     // game second
    int64_t nextSuppress = 0;   // game second from which a warden may suppress it again
    uint32_t homeFaction = 0;   // the side (FactionId) it was taken from; a rebel or a freed slave goes back to it
};

// On a colonist or visitor the player has ordered arrested: a warden takes it to a prison bed. `by` is the warden carrying it, none until
// it is lifted; a carried pawn thinks and moves no more.
struct ArrestOrder {
    Entity by = kNoEntity;
};

// On a bed building: only prisoners sleep in it, and downed enemies are carried to it.
struct PrisonBed {
    bool marked = true;
};

// The numbers from PrisonerRules (data/defs/Rules). `present` is false when the defs have none; the same values are used.
struct COLONY_API PrisonerRules {
    bool present = false;
    float initialResistanceMin = 5.0f;     // RimWorld wiki, the Warrior pawn kind's example range
    float initialResistanceMax = 9.0f;
    float resistancePerChat = 1.0f;        // RimWorld wiki: 1.0 times the three factors below
    // The prisoner's mood level (0 to 100) against the factor on a talk's effect (RimWorld wiki: 20% at 0, 100% at 50, 150% at 100).
    std::vector<std::pair<float, float>> moodFactor = {{0.0f, 0.2f}, {50.0f, 1.0f}, {100.0f, 1.5f}};
    // Ours: the negotiation ability of a warden is `negotiationBase` plus `negotiationPerLevel` per Social level, times Talking.
    float negotiationBase = 0.5f;
    float negotiationPerLevel = 0.05f;
    int defaultSkillLevel = 8;
    int64_t chatIntervalSeconds = 21600;   // "every several hours" (6 game hours)
    int64_t chatWorkSeconds = 360;
    int64_t feedWorkSeconds = 120;
    float chatXp = 100.0f;
    float recruitChanceBase = 0.5f;        // times the mood factor, between the two limits
    float recruitChanceMin = 0.05f;
    float recruitChanceMax = 0.95f;
    float captureSafeRadius = 25.0f;       // wardens leave a downed enemy alone while an able one is this close
    float feedBelow = 0.3f;                // wardens feed a prisoner whose food is under this
    int64_t executeWorkSeconds = 60;       // ours: the cut itself is quick
    int64_t guiltyHours = 24;              // RimWorld wiki: a prisoner is guilty if it harmed the colony in the past 24 hours
    int edgeSearch = 40;                   // cells along the map edge looked at for a way out
    // Slaves (Ideology; docs/design/prisoners.md, Slaves).
    float initialWillMin = 1.0f;           // ours: the wiki says only that will starts much lower than resistance
    float initialWillMax = 3.0f;
    float willPerChat = 1.0f;              // the wiki: will works like resistance (times the same factors)
    float startSuppression = 0.5f;         // ours
    // Suppression lost per day while at or above each share, highest share first (RimWorld wiki: 20% from 30%, 10% from 15%, 5% below).
    std::vector<std::pair<float, float>> suppressionFall = {{0.3f, 0.2f}, {0.15f, 0.1f}, {0.0f, 0.05f}};
    float suppressBelow = 0.7f;            // ours: wardens suppress a slave whose suppression is under this
    int64_t suppressIntervalSeconds = 10800;  // ours
    int64_t suppressWorkSeconds = 259;     // RimWorld wiki: 180 ticks (x 1.44)
    // Suppression power = base + perLevel x Social, times Talking (ours, fitted to the wiki's Social 10: 28%).
    float suppressPowerBase = 0.08f;
    float suppressPowerPerLevel = 0.02f;
    // The suppression a slave has against the factor on a warden's power (the wiki's 2x at 0%; the rest ours).
    std::vector<std::pair<float, float>> suppressCurve = {{0.0f, 2.0f}, {0.5f, 1.0f}, {1.0f, 0.5f}};
    float suppressXp = 50.0f;              // ours
    // Rebellion: mean time between rebellions for one awake slave, divided by the factors (RimWorld wiki).
    float rebellionMtbDays = 45.0f;
    std::vector<std::pair<float, float>> rebellionSuppression = {{0.0f, 5.0f}, {0.333f, 1.5f}, {0.5f, 1.0f}, {1.0f, 0.25f}};
    std::vector<std::pair<float, float>> rebellionSlaveCount = {{1.0f, 1.0f}, {2.0f, 0.75f}, {5.0f, 0.5f}, {10.0f, 0.3f}, {20.0f, 0.2f}};
    std::vector<std::pair<float, float>> rebellionMood = {{0.0f, 1.5f}, {50.0f, 1.0f}, {100.0f, 0.8f}};
    float rebellionUnattended = 20.0f;     // no free colonist able to stand on the map
    float slaveWorkSpeed = 0.85f;          // RimWorld wiki: 85% global work speed
    std::vector<std::string> slaveDisabledWorkTags = {"Social", "Violent", "Artistic", "Intellectual"};  // warden, hunt, art, research

    static PrisonerRules read(const DefDatabase& defs);
};

class COLONY_API PrisonerSystem {
public:
    PrisonerSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, JobSystem& jobs, FighterSystem& fighters,
                   RaidSystem& raids, const DefDatabase& defs, const GameTime& time, Rng& rng);

    const PrisonerRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Executions cost goodwill with the prisoner's faction (TradeSystem::noteExecuted); without it they cost none.
    void setTrade(TradeSystem* trade) { trade_ = trade; }
    // The rooms a prison cell must be closed by; without them every cell counts as closed.
    void setRooms(Rooms* rooms) { rooms_ = rooms; }

    bool isPrisoner(Entity pawn) const;
    bool isPrisonBed(Entity bed) const;
    // How many prisoners the colony holds.
    int count() const;
    // The prisoners, in id order.
    std::vector<Entity> prisoners() const;

    // Whether wardens may capture this pawn now: a living, downed pawn of another side that is no animal (not wild, not a prisoner
    // already) on the map.
    bool capturable(Entity pawn) const;
    // Whether the cell is in a fully enclosed room (walls and doors all round, a roof over every cell): the only place a prison
    // bed takes a prisoner. A door counts as closed; it is not in any room.
    bool cellEnclosed(CellPos cell) const;
    // Whether the player's order to arrest `pawn` can be carried out: a living colonist of the player's side, or a visitor or
    // trader of a faction, on the map; not an animal, a raider, a prisoner already, the protagonist or the one the player is walking.
    bool arrestable(Entity pawn) const;
    // `capturable`, or `arrestable` with an order: what a capture job may carry to a bed.
    bool carryable(Entity pawn) const;
    // The prison bed slot a warden (`helper`) would put `pawn` in: free, reachable, the nearest to the pawn. False if there is none.
    bool findPrisonBed(Entity pawn, Entity helper, Entity& bed, CellPos& cell) const;

    // Commands. False, with a reason in plain words, when they can't be done.
    bool setMode(Entity pawn, PrisonerMode mode, std::string& error);
    bool setPrisonBed(Entity bed, bool on, std::string& error);
    // Orders a colonist or a visitor arrested (or cancels the order).
    bool setArrest(Entity pawn, bool on, std::string& error);

    // The end of a capture: `pawn` becomes the colony's prisoner in `bed` (the bed's slot is its own, its weapons are dropped, it stops
    // fighting and leaves its raid squad). `warden` is only for the event. Also a dev entry point for tests.
    bool imprison(Entity pawn, Entity bed, Entity warden, std::string& error);
    // A warden has lifted `pawn` for an arrest: it stops what it is doing and goes where the warden goes. A visitor leaves its
    // visit and its faction turns hostile (TradeSystem::noteArrested).
    void seize(Entity pawn, Entity warden);

    // Whether a warden may start a talk with this prisoner now: it has a mode that wants one, its time has come, it is not
    // downed, it lies or stands somewhere a warden can walk to, and (Release) it can walk.
    bool chatDue(Entity prisoner) const;
    // One talk: the prisoner's resistance falls, or it is recruited, or it is released, by its mode. Sets the next time.
    void interact(Entity warden, Entity prisoner);
    // How much a talk by `warden` lowers `prisoner`'s resistance now.
    float resistanceReduction(Entity warden, Entity prisoner) const;
    // The chance a recruit attempt on `prisoner` works now (once its resistance is 0).
    float recruitChance(Entity prisoner) const;
    // The prisoner joins the colony (its side becomes the player's, it works and fights for it), or is set free (it walks off the
    // map). False if it is no prisoner (or, for a release, can't walk).
    bool recruit(Entity prisoner, Entity warden);
    bool release(Entity prisoner, Entity warden);
    // Whether the prisoner harmed the colony in the past `guiltyHours` (it was captured out of a raid then): killing it is justified.
    bool guilty(Entity prisoner) const;
    // A warden kills the prisoner at once (death cause Executed). Event PrisonerExecuted. False if it is no living prisoner.
    bool execute(Entity prisoner, Entity warden);
    // The hook for surgery: a part was taken from the living `victim` by `surgeon` for its organ (not a prosthetic). Raises the
    // OrganHarvested event the colony's moods read. `part` is the body part index.
    void noteOrganHarvested(Entity victim, Entity surgeon, int part);
    // The hook for T3's prison break incident: every prisoner who can walk breaks out (it is a prisoner no more and fights
    // its own side's fight, with no weapon, walking off the map; a PrisonBreak event). Returns how many.
    int breakOut(const char* why);

    // ---- Slaves (docs/design/prisoners.md, Slaves)
    bool isSlave(Entity pawn) const;
    int slaveCount() const;
    // The prisoner becomes the colony's slave (its side becomes the player's, it works within the slave limits). False if it is
    // no living prisoner.
    bool enslave(Entity prisoner, Entity warden);
    // Whether a warden may suppress this slave now: its suppression is under `suppressBelow`, its time has come, and it is up.
    bool suppressDue(Entity slave) const;
    // How much a warden's suppression raises this slave's suppression now.
    float suppressAmount(Entity warden, Entity slave) const;
    // One suppression talk.
    void suppress(Entity warden, Entity slave);
    // What the player can do with a slave: "Emancipate" (it becomes a free colonist) or "Release" (it goes back to its own
    // side and walks off the map). False, with a reason, when it can't be done.
    bool slaveOrder(Entity slave, const std::string& order, std::string& error);
    // The chance that one awake slave starts a rebellion in the next `seconds` (for tests and the info panel).
    float rebellionChance(Entity slave, int64_t seconds) const;
    // Every slave who can walk rises up: it is a slave no more, goes back to its own side and runs for the map edge.
    // `leader` started it. Returns how many. Event SlaveRebellion.
    int rebel(Entity leader);
    // The work speed factor for a pawn: `slaveWorkSpeed` for a slave, else 1.
    float workSpeedFactor(Entity pawn) const;
    // How long a warden's talk with the pawn takes (a suppression is shorter than a prisoner's talk).
    int64_t talkSeconds(Entity pawn) const;

    // Freed prisoners that reach the map edge leave it, and an arrest whose warden dropped the job lets the pawn go. Slaves'
    // suppression falls and they may rebel. Runs once a step, after the fighters.
    void step();

    // A warden's inspired recruitment gives its next attempt a better chance (docs/design/inspirations.md).
    void setInspirations(InspirationSystem* inspirations) { inspirations_ = inspirations; }

    // After a prisoner or bed is destroyed (or a pawn leaves): nothing to do now, kept for symmetry with the other systems.
    void hashThing(Entity e, StateHasher& h) const;

private:
    int64_t now() const;
    float moodOf(Entity pawn) const;
    CellPos edgeGoal(Entity pawn) const;
    // A seized visitor whose arrest was cancelled: no longer a visitor, it walks off the map.
    void letGo(Entity pawn);
    // A pawn that is no longer held goes back to `side` and runs for the map edge.
    void runOff(Entity pawn, uint32_t side, const char* why);
    void stepSlaves(int64_t seconds);
    void emit(EventType type, Entity thing, Entity target, float amount, const char* message) const;

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    JobSystem& jobs_;
    FighterSystem& fighters_;
    InspirationSystem* inspirations_ = nullptr;
    RaidSystem& raids_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Rng& rng_;
    PrisonerRules rules_;
    std::vector<Event>* events_ = nullptr;
    TradeSystem* trade_ = nullptr;
    Rooms* rooms_ = nullptr;
};

}  // namespace colony

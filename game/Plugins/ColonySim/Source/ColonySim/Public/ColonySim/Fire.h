#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace colony {

class Filth;
class Floors;
class HealthSystem;
class Map;
class Plants;
class Rooms;
class StateHasher;
class TemperatureSystem;
class Things;
class Weather;

// Fire (task E4, docs/design/fire.md): a sparse layer of burning cells. A fire starts from an explosion
// of Burn damage, a burning pawn, a hot room or a call to `ignite` (lightning, short circuits and
// fire-starting breaks use it later), grows on what is flammable in its cell, burns it down to ash,
// spreads to flammable cells up to two cells away, heats the room it is in, and is put out by rain,
// snow, or colonists with the Firefighter work type. Pawns standing in it catch fire and burn.
//
// Only cells with a fire cost anything: one pass every `passSeconds` visits them in cell order, so a
// colony with no fire pays for one empty check a minute.
//
// Unreal does not include this header: it draws the snapshot's fires, burnt cells and burning pawns and
// reacts to FireStarted, FireOut, PawnCaughtFire and PawnFireOut.

// A fire in one cell. `size` goes from 0 up to the rules' maxSize.
struct FireCell {
    float size = 0.0f;
    uint16_t age = 0;          // passes it has burned
    uint16_t plantDamage = 0;  // hit points burnt off the plant in the cell
};

// On a pawn that is on fire; `until` is the game second it burns out by itself.
struct Burning {
    int64_t until = 0;
};

// The numbers (FireRules in data/defs/Rules/FireRules.json marks each as RimWorld's from memory or ours).
// Rates are per RimWorld tick (1.44 game seconds) and multiplied by the ticks in a pass.
struct COLONY_API FireNumbers {
    double ticksToSeconds = 1.44;
    int passSeconds = 60;
    float startSize = 0.1f;
    float maxSize = 1.75f;
    float growthPerTick = 0.00055f;
    float shrinkPerTick = 0.0011f;   // a fire with nothing left to burn
    float damageBase = 0.0125f;      // hit points per tick to what burns: clamp(base + perSize x size, base, max)
    float damagePerSize = 0.0036f;
    float damageMax = 0.05f;
    float plantDamageFactor = 6.0f;
    float spreadPerTick = 0.0075f;   // chance per tick, times size and the target's flammability
    float emberFactor = 0.2f;        // a jump of two cells is this much as likely
    float emberMinSize = 0.7f;       // and needs a fire this big
    float catchPerTick = 0.01f;      // a pawn in a fire catches fire, times size and its flammability
    float pawnFlammability = 0.7f;
    float pawnDamagePerTick = 0.0333f;  // 2 heat damage per RimWorld second (60 ticks)
    int pawnBurnTicks = 1800;
    float rainOutPerTick = 0.002f;   // size lost per tick per point of rain rate, outdoors
    float snowBlocksAt = 10.0f;      // snow this deep (percent) stops fire outdoors
    float autoIgniteC = 235.0f;
    float hotRoomIgniteChance = 0.25f;
    float hotPawnIgniteChance = 0.1f;
    float blastIgniteChance = 0.6f;  // a flame blast lights each flammable cell and pawn in its radius with this chance
    float heatPerSecond = 14.0f;     // RimWorld heat units a fire of size 1 pushes into its room each second
    float heatMaxC = 500.0f;
    float beatPerTick = 0.00167f;    // size a colonist beats out per tick at speed 1
    int floorBurnPasses = 10;        // passes a fire must burn on a flammable floor to destroy it
    float burntFadeDays = 6.0f;
    int autoHomeRadius = 5;
    int startHomeRadius = 12;

    static FireNumbers read(const DefDatabase& defs);
};

struct FireRecord {
    int cell = 0;
    FireCell fire;
};

struct BurntRecord {
    int cell = 0;
    int64_t at = 0;  // game second the cell burnt
};

// Everything the sim saves that is not on a pawn (the section "fire", docs/design/saves.md).
struct FireSaved {
    std::vector<FireRecord> fires;
    std::vector<BurntRecord> burnt;
    std::vector<int> home;  // cell indices
    bool autoHome = true;
    bool homeStarted = false;
    uint64_t started = 0;
    uint64_t burntUp = 0;
    uint64_t pawnsBurnt = 0;
};

class COLONY_API Fire {
public:
    Fire(Map& map, Things& things, Plants& plants, Floors& floors, Filth& filth, Rooms& rooms, TemperatureSystem& temperature,
         HealthSystem& health, Weather& weather, const DefDatabase& defs, uint64_t seed);
    ~Fire();
    Fire(const Fire&) = delete;
    Fire& operator=(const Fire&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    const FireNumbers& numbers() const { return numbers_; }
    // Set by the sim: a pawn's flammability factor from its genes (0 for fire-resistant skin).
    void setPawnFactor(std::function<float(Entity)> factor) { pawnFactor_ = std::move(factor); }

    // One game second, `now` the game second. The sim's event list (set with setEventLog) holds this step's
    // events from `from` on: explosions of Burn damage start fires, new buildings widen the home area.
    // The pass over the fires runs every `passSeconds`.
    void step(int64_t now, size_t from = 0);

    // ---- Starting
    // A fire of `size` in the cell (the start size if 0). False if the cell can't burn: off the map, water, rock,
    // already burning, wet (rain or snow outdoors) or with nothing flammable in it, unless `force`.
    bool ignite(CellPos cell, float size = 0.0f, bool force = false);
    // Lights every cell within `radius` metres of a point that can burn, each with `chance`, and sets the pawns there
    // alight with the same chance (a flame blast, a molotov). Returns whether anything caught.
    bool igniteRadius(float x, float y, int level, float radius, float chance, int64_t now);
    // Sets a pawn on fire. False if it can't burn (dead, no body, already burning, fire-resistant) unless `force`.
    bool setPawnAlight(Entity pawn, int64_t now, bool force = false);
    // Puts out a burning pawn.
    bool putOutPawn(Entity pawn);

    // ---- Questions
    bool burning(CellPos cell) const;
    float sizeAt(CellPos cell) const;
    size_t count() const { return fires_.size(); }
    // Every fire by cell index, in cell order.
    const std::map<int, FireCell>& all() const { return fires_; }
    // Goes up whenever a fire starts, is put out or changes size (sizes are rounded to hundredths for this).
    uint64_t changes() const { return changes_; }
    bool pawnBurning(Entity pawn) const;
    size_t burningPawns() const;
    // How flammable a cell is now (0 to 1.3 and more for grass): its building, items, plant and floor, at most.
    float flammabilityAt(CellPos cell) const;
    // A pawn's own flammability (0 for fire-resistant).
    float pawnFlammability(Entity pawn) const;

    // ---- Burnt ground
    bool burnt(CellPos cell) const;
    const std::map<int, int64_t>& burntCells() const { return burnt_; }
    uint64_t burntChanges() const { return burntChanges_; }

    // ---- Putting out
    // Beats `amount` of size off the fire in a cell; a fire at nothing is out, and so is any pawn burning there.
    // Returns true when the fire is out.
    bool extinguish(CellPos cell, float amount);
    // Size a colonist at full speed beats out in one game second.
    float beatPerSecond() const { return static_cast<float>(numbers_.beatPerTick / numbers_.ticksToSeconds); }

    // ---- The home area: where colonists with the Firefighter work type put fires out
    bool home(CellPos cell) const;
    size_t homeCount() const { return homeCount_; }
    uint64_t homeChanges() const { return homeChanges_; }
    // Every home cell by index, in cell order.
    std::vector<int> homeCells() const;
    // Adds the rectangle to the home area or takes it off; returns the cells that changed. The first call turns the
    // automatic growth round new buildings off (RimWorld's "expand automatically").
    int setHome(const CellRect& area, bool add);
    bool autoHome() const { return autoHome_; }
    void setAutoHome(bool on) { autoHome_ = on; }

    // Counters.
    uint64_t started() const { return started_; }
    uint64_t burntUp() const { return burntUp_; }
    uint64_t pawnsBurnt() const { return pawnsBurnt_; }

    // Saving, loading and the state hash (docs/design/saves.md).
    FireSaved saved() const;
    bool restore(const FireSaved& saved, std::string& error);
    void hashState(StateHasher& h) const;
    void hashThing(Entity e, StateHasher& h) const;

private:
    bool canBurnGround(CellPos c) const;
    bool wetAt(CellPos c) const;
    float thingFlammability(Entity thing) const;
    float defFlammability(DefId def) const;
    void pass(int64_t now);
    void burnThings(CellPos c, FireCell& fire, float damage, Rng& rng);
    void spreadFrom(CellPos c, const FireCell& fire, int64_t now, int index, std::vector<int>& ignitions, Rng& rng);
    void catchPawns(int64_t now);
    void stampHome(CellPos centre, int radius);
    void markBurnt(int index, int64_t now);
    void emit(EventType type, Entity thing, CellPos cell, DefId def = DefId{}, float amount = 0.0f, const char* message = nullptr);
    uint64_t rngSeed(int64_t now, int index, uint64_t what) const;

    Map& map_;
    Things& things_;
    Plants& plants_;
    Floors& floors_;
    Filth& filth_;
    Rooms& rooms_;
    TemperatureSystem& temperature_;
    HealthSystem& health_;
    Weather& weather_;
    const DefDatabase& defs_;
    uint64_t seed_;
    FireNumbers numbers_;
    std::vector<float> flammability_;  // by DefId.value: the def's stats.flammability
    std::vector<float> stuffFactor_;   // by DefId.value: stuffProps.statFactors.flammability
    std::vector<float> maxHp_;         // by DefId.value
    DefId burnType_;
    DefId ashKind_;
    std::function<float(Entity)> pawnFactor_;
    std::vector<Event>* events_ = nullptr;

    std::map<int, FireCell> fires_;
    std::map<int, int64_t> burnt_;
    std::vector<uint8_t> home_;
    size_t homeCount_ = 0;
    bool autoHome_ = true;
    bool homeStarted_ = false;
    int64_t now_ = 0;  // the game second of the step being run, for stamping burnt ground
    uint64_t homeChanges_ = 0;
    uint64_t homeHash_ = 0;
    uint64_t changes_ = 0;
    uint64_t burntChanges_ = 0;
    uint64_t started_ = 0;
    uint64_t burntUp_ = 0;
    uint64_t pawnsBurnt_ = 0;
    std::vector<int> scratch_;
};

}  // namespace colony

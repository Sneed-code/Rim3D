#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Rooms.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class Filth;
class Floors;
class Map;
class Plants;
class StateHasher;
class Things;

// Room stats, beauty and the beauty need (task C17, docs/design/room-stats.md), by RimWorld's
// formulas. Beauty of a cell comes from the floor (or bare ground), the building, the items, the
// plant and the filth in it; a room's beauty, space, wealth, cleanliness and impressiveness come
// from its cells; a colonist's beauty need follows what it sees around it.

// Stats of one enclosed room.
struct RoomStatValues {
    float beauty = 0.0f;
    float space = 0.0f;
    float wealth = 0.0f;
    float cleanliness = 0.0f;
    float impressiveness = 0.0f;
};

// The stage names the wiki gives each stat ("cramped", "somewhat spacious"...).
COLONY_API const char* beautyLabel(float beauty);
COLONY_API const char* spaceLabel(float space);
COLONY_API const char* wealthLabel(float wealth);
COLONY_API const char* cleanlinessLabel(float cleanliness);
COLONY_API const char* impressivenessLabel(float impressiveness);

// The numbers in the RoomStatRules def (with these values when it is missing).
struct COLONY_API RoomStatNumbers {
    int beautyFullSizeCells = 40;
    float beautySmallBase = 20.0f;
    float beautySmallHalf = 0.5f;
    float spacePerCell = 1.4f;
    float spaceBlockerReduction = 0.9f;
    float wealthDivisor = 1500.0f;
    float beautyDivisor = 3.0f;
    float spaceDivisor = 125.0f;
    float cleanlinessBase = 1.0f;
    float cleanlinessDivisor = 2.5f;
    float averageWeight = 65.0f;
    float minWeight = 35.0f;
    float spaceCap = 500.0f;
    float spaceCapKeep = 0.25f;
    float workTicksFactor = 0.0036f;       // market value per RimWorld tick of work
    float ticksPerWorkSecond = 0.6944444f; // workToBuild is in game seconds

    static RoomStatNumbers read(const DefDatabase& defs);
};

// The numbers in the BeautyNeedRules def.
struct COLONY_API BeautyNeedNumbers {
    bool present = false;
    float sightRadius = 8.7f;
    float neutralMin = 0.35f;
    float neutralMax = 0.65f;
    float beautyAtNeutralMin = -3.5f;
    float beautyAtNeutralMax = 2.4f;
    float risePerHour = 0.12f;
    float fallPerHour = 0.04f;
    int sampleEverySeconds = 30;

    static BeautyNeedNumbers read(const DefDatabase& defs);
};

// RimWorld's impressiveness from the four room stats (the wiki's formula): each stat is scaled and
// softened with a logarithm above 1, the result weighs the average 65 and the lowest 35, and space
// puts a soft cap on it.
COLONY_API float roomImpressiveness(const RoomStatNumbers& numbers, float wealth, float beauty, float space,
                                    float cleanliness);

// A colonist's beauty need: 0 to 1, 35% to 65% neutral. Colonists with needs have one once the
// defs have a BeautyNeedRules def.
struct BeautyNeed {
    float level = 0.5f;
    float perceived = 0.0f;       // average beauty of the cells seen at the last sample
    int64_t sampledAt = -1;       // game second of the last sample, -1 for none yet
};

// Saved state (docs/design/saves.md, section "roomstats").
struct RoomStatsState {
    struct Need {
        uint32_t pawn = 0;
        float level = 0.5f;
        float perceived = 0.0f;
        int64_t sampledAt = -1;
    };
    std::vector<Need> needs;
};

class COLONY_API RoomStats {
public:
    RoomStats(Map& map, Things& things, Rooms& rooms, const Floors& floors, const Filth& filth, const Plants& plants,
              const DefDatabase& defs);

    const RoomStatNumbers& numbers() const { return numbers_; }
    const BeautyNeedNumbers& needNumbers() const { return need_; }

    // One game second, after the rooms: works the stats of every enclosed room out again if anything
    // they read changed (the rooms, buildings, floors, items, plants or filth), and moves the beauty
    // need of every colonist. The stats are always those of the current state, so a loaded game
    // (which calls refresh()) and the game that was saved agree.
    void step(int64_t now);
    // Works the room stats out now, if anything changed since the last time.
    void refresh();

    // Stats of the room a cell is in, or null for the outdoors, a wall or a door. Ids shift when
    // the rooms are rebuilt; this looks the room up each time.
    const RoomStatValues* statsAt(CellPos c);
    // By room id, as of the last refresh; null for the outdoors.
    const RoomStatValues* statsOf(RoomId room) const;

    // Beauty of one cell: floor or bare ground (if `withGround`), building, items, plant, filth. A
    // multi-cell building counts once if `counted` is passed and keeps its entities.
    float cellBeauty(CellPos c, bool withGround = true, std::vector<Entity>* counted = nullptr);
    // Cleanliness of one cell: floor or bare ground, the building (a butcher table is dirty), filth.
    float cellCleanliness(CellPos c);
    // The average beauty of the cells within sight of `from` (the beauty need's input); 0 if none.
    float perceivedBeauty(CellPos from);
    // Market value of a finished building (its materials and its work); 0 for a def with none.
    float marketValueOf(Entity building) const;
    float marketValueOfFloor(DefId floor) const;

    // The beauty need of a pawn, or null.
    const BeautyNeed* beautyNeed(Entity pawn) const;
    // Where the need heads for a given perceived beauty, 0 to 1.
    float needTarget(float perceivedBeauty) const;

    // For the mood system. The value a situation condition has for the pawn now, and the role of the
    // room it stands in (None for the outdoors); false if the condition does not apply to it.
    // Conditions: beautyNeed (value: the need level), roomSpace, roomImpressiveness, roomCleanliness
    // (the stat of the enclosed, roofed room the pawn stands in) and sleepSharedRoom (the beds of
    // the barracks it stands in, 3 or more).
    bool conditionValue(Entity pawn, const std::string& condition, double& value, RoomRole& role);

    void hashState(StateHasher& h) const;
    RoomStatsState state() const;
    bool restore(const RoomStatsState& state, std::string& error);

private:
    float statBeauty(DefId def, bool outdoors) const;
    void computeRoom(const Room& room, RoomStatValues& out);
    uint64_t version() const;
    void updateNeeds(int64_t now);

    Map& map_;
    Things& things_;
    Rooms& rooms_;
    const Floors& floors_;
    const Filth& filth_;
    const Plants& plants_;
    const DefDatabase& defs_;
    RoomStatNumbers numbers_;
    BeautyNeedNumbers need_;
    // Per def id: beauty indoors and outdoors, cleanliness, market value of the materials and work,
    // and whether a building can be stood on.
    std::vector<float> beautyIndoors_, beautyOutdoors_, cleanliness_, baseValue_, stuffBeauty_, stuffCount_;
    std::vector<uint8_t> standable_, impassable_;
    DefId groundDef_[5];  // by Terrain
    std::vector<std::pair<int, int>> sightOffsets_;  // within the sight radius, row order
    std::vector<RoomStatValues> stats_;              // by room id
    std::vector<uint8_t> hasStats_;                  // by room id
    uint64_t seenVersion_ = 0;
    bool built_ = false;
    std::map<uint32_t, BeautyNeed> needs_;  // by entity id
};

}  // namespace colony

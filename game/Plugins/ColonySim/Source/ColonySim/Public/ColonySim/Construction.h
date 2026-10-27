#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace colony {

class Map;
class Plants;
class Rng;
class Things;
class UpperFloors;

// Building: blueprints, frames, materials, finished buildings and taking them down. See
// docs/design/construction.md.
//
// Unreal does not include this header: it reads sites through the snapshot and events and builds
// through commands (docs/design/bridge.md).

// One material of a site: how many its def costs and how many pawns have delivered.
struct MaterialCount {
    DefId item;
    int needed = 0;
    int have = 0;
};

// On a blueprint or a frame, which are things of category Building (their `Thing::def` is the
// building they will become) but are not on the map's building layer: they block nothing, hold no
// footprint, and the regions don't see them. `Construction::siteAt` finds the one covering a cell.
struct ConstructionSite {
    ConstructionStage stage = ConstructionStage::Blueprint;
    DefId stuff;  // what it is made of; invalid for a building with no stuff
    CellPos origin;
    Rot4 rotation = Rot4::North;
    int sizeX = 1;  // as placed facing North
    int sizeY = 1;
    std::vector<MaterialCount> materials;
    float workTotal = 0.0f;  // game seconds of work at speed 1 (the def's `workToBuild` with the stuff's factor)
    float workDone = 0.0f;

    CellRect rect() const { return footprintRect(origin, sizeX, sizeY, rotation); }
    bool hasAllMaterials() const;
    int materialsNeeded() const;
    int materialsHave() const;
};

// On a finished building: what it was built from, so taking it down gives the right materials back.
struct BuiltOf {
    DefId stuff;
};

// A building the player has marked to be taken down (RimWorld's deconstruct designation).
struct MarkedForDeconstruct {};

// The numbers in the ConstructionRules def (with these values when the def is missing).
struct ConstructionNumbers {
    double buildWorkPerSecond = 1.7;  // work a pawn at construction speed 1 does per game second
    double deconstructWorkPerSecond = 1.0;
    double deconstructWorkMin = 28.8;
    double deconstructWorkMax = 4320.0;
    double deconstructRefundFactor = 0.5;
    double skillSpeedBase = 0.3;       // construction speed = base + perLevel x Construction skill
    double skillSpeedPerLevel = 0.0875;
    int defaultSkillLevel = 8;         // for a worker with no skills (a test pawn)
};

class COLONY_API Construction {
public:
    struct Cost {
        DefId item;
        int count = 0;
    };

    Construction(Map& map, Things& things, Plants& plants, const DefDatabase& defs, Rng& rng,
                 std::vector<Event>& events);

    const ConstructionNumbers& numbers() const { return numbers_; }
    // Upper floors (task M2, docs/design/levels.md): a def with `building.upperFloor` is planned,
    // delivered to and built like any building, one cell at a time, and when it is done the floor
    // is laid (UpperFloors::lay) instead of a building spawned. Without this, none can be planned.
    void setUpperFloors(UpperFloors* floors) { upperFloors_ = floors; }
    bool isUpperFloor(DefId def) const;
    // Art (`building.art`, docs/design/art.md): sculptures. An artist builds them (Artistic skill, the Art
    // work type), the work follows the stuff's `workToMake` factor, and the finished piece has a quality.
    bool isArt(DefId def) const;

    // ---- What a building costs --------------------------------------------------------------
    // True if the def is a Things def of category Building that can be built (it has
    // `stats.workToBuild`).
    bool buildable(DefId def) const;
    // True if `stuff` can make the building (it names a category in `stuffCategories` that the
    // stuff's `stuffProps.categories` has), or if the building takes no stuff and `stuff` is invalid.
    bool stuffFits(DefId def, DefId stuff) const;
    // The building's costs in order: `costs` by def name, then `costStuffCount` of the stuff.
    // Costs of one item are added up.
    std::vector<Cost> costOf(DefId def, DefId stuff) const;
    float workToBuild(DefId def, DefId stuff) const;
    int maxHitPoints(DefId def, DefId stuff) const;  // 0 if the def has none

    // ---- Placing, cancelling, finishing -----------------------------------------------------
    // A blueprint of `def` made of `stuff`, its footprint starting at `cell` facing `rot`. Every
    // cell must be on the map, walkable ground, free of buildings and of other sites. Returns
    // kNoEntity, with `error` in plain words, if it can't be placed.
    Entity placeBlueprint(DefId def, DefId stuff, CellPos cell, Rot4 rot, std::string& error);
    // A blueprint or a frame is taken away and everything delivered to it is put back on the
    // ground; a building marked for deconstruction is unmarked. False, with `error`, if `thing`
    // is none of these.
    bool cancel(Entity thing, std::string& error);
    // Marks a finished building to be taken down. False if it isn't one, or it is marked already.
    bool markDeconstruct(Entity building, std::string& error);

    // Hands over materials. Returns how many of `count` the site took (none if it needs no more
    // of that item). The first delivery turns a blueprint into a frame.
    int deliver(Entity site, DefId item, int count);
    // Turns a frame with all its materials into the finished building (an upper floor's frame lays
    // its floor and returns kNoEntity; if it can't be laid any more the materials drop below): pawns standing in the way
    // of a building that blocks movement are moved out, items lying on it are moved off, then it
    // is built (full hit points) and the plants under it are gone. Returns the building, or kNoEntity if it can't stand there any
    // more (the materials are given back).
    Entity complete(Entity frame);
    // Takes a finished building down and puts back `deconstructRefundFactor` of its materials,
    // rounded at random. False if it isn't a building.
    bool deconstruct(Entity building);

    // ---- Looking up -------------------------------------------------------------------------
    // The blueprint or frame covering a cell, or kNoEntity.
    Entity siteAt(CellPos cell) const;
    // Work the job at a site or building needs: total game seconds at speed 1.
    float deconstructWork(Entity building) const;
    // A walkable cell beside the rect (sharing a side, never a corner) that `from` can reach,
    // nearest to `from`; false if there is none. Where a pawn stands to build or take down. A rect
    // over open air on an upper level (an upper floor) can also be worked from the level below,
    // under it or beside that.
    bool workCell(const CellRect& rect, CellPos from, CellPos& out) const;
    // Construction speed from the pawn's Construction skill (`skillSpeedBase` + `skillSpeedPerLevel`
    // x level); `defaultSkillLevel` if it has no skills.
    float skillSpeed(Entity pawn, const char* skillName = "Construction") const;

    // After loading a save: rebuilds the cell index from the sites. False, with `error`, if two
    // sites overlap or one stands where a building is.
    bool afterLoad(std::string& error);

private:
    void indexSite(Entity site, bool add);
    // Puts `count` of an item def on the ground near a rect, in stacks (not inside the rect if `skipRect`).
    void dropNear(DefId item, int count, const CellRect& rect, bool skipRect = false);
    void clearFootprint(Entity building, const CellRect& rect, bool blocksMovement, bool standable);
    int stackLimit(DefId def) const;

    Map& map_;
    Things& things_;
    Plants& plants_;
    const DefDatabase& defs_;
    Rng& rng_;
    std::vector<Event>& events_;
    ConstructionNumbers numbers_;
    UpperFloors* upperFloors_ = nullptr;
    std::unordered_map<uint64_t, Entity> siteAt_;  // by packed cell; looked up only, never iterated
};

}  // namespace colony

#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Rooms.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace colony {

class HealthSystem;
class Map;
class Things;
class StateHasher;

// Room temperature and what it does to pawns (task E2). See docs/design/temperature.md.
//
// Every enclosed room under a roof has a temperature that moves towards the outside through its
// walls, doors, vents and roof, and is pushed by heaters, coolers and campfires. The outdoors, and
// any room with an open roof, is at the outdoor temperature, which the sim sets each step
// (`setOutdoor`; E1's seasons and weather will set it from the biome's curve). A pawn feels the
// temperature of the cell it stands in; outside its comfortable range it gets cold or hot
// (mood thoughts), outside its safe range hypothermia or heatstroke builds (Health environment
// hediffs), and in hard frost it can lose fingers and toes to frostbite.
//
// Unreal does not include this header: it reads `Snapshot::outdoorTemperature` and each room's
// temperature from the snapshot.

// The numbers in the TemperatureRules def (with these values when the def is missing).
struct COLONY_API TemperatureNumbers {
    int intervalSeconds = 60;  // rooms and pawns are worked out this often (game seconds)
    // Heat flow through each kind of boundary, per game hour, as a share of the temperature
    // difference times one boundary cell, divided by the room's cell count.
    float wall = 0.04f;        // each wall cell edge round the room
    float doorClosed = 0.06f;
    float doorOpen = 6.0f;
    float vent = 3.0f;
    float roofThin = 0.024f;   // each roofed cell, towards the outdoor temperature
    float roofThick = 0.004f;  // under rock, towards the ground temperature
    float rock = 0.004f;       // a rock cell round the room, towards the ground temperature
    float safeMarginC = 10.0f; // the safe range is the comfortable range this much wider
    float frostbiteSeverity = 3.0f;
    // A colonist walks to a warm room when its hypothermia reaches the first and stays until it is under the second.
    float seekWarmthSeverity = 0.12f;
    float warmEnoughSeverity = 0.03f;

    static TemperatureNumbers read(const DefDatabase& defs);
};

// An enclosed room's temperature as saved: the room is named by its anchor cell.
struct RoomTemperature {
    CellPos anchor;
    float celsius = 0.0f;
};

class COLONY_API TemperatureSystem {
public:
    TemperatureSystem(Map& map, Things& things, Rooms& rooms, HealthSystem& health, const DefDatabase& defs,
                      uint64_t seed);

    const TemperatureNumbers& numbers() const { return numbers_; }

    // The sim sets these each step before `step`.
    void setOutdoor(float celsius) { outdoor_ = celsius; }
    // The temperature under rock: the year's mean.
    void setGround(float celsius) { ground_ = celsius; }
    float outdoor() const { return outdoor_; }
    float ground() const { return ground_; }

    // Whether a heater, cooler or campfire is running right now. Until power (E5) and fuel (C11)
    // install their test, every one is, so a heater needs no power yet.
    void setSourceRunning(std::function<bool(Entity)> running) { running_ = std::move(running); }
    // Moves a pawn's comfortable range (min, max) for what it wears: C15 installs it.
    void setComfortModifier(std::function<void(Entity, float&, float&)> modifier) { modifier_ = std::move(modifier); }

    // The temperature of a cell: its room's if it is in an enclosed room under a roof, else (the
    // outdoors, a room with an open roof) the outdoor temperature. A door is the mean of its sides.
    float temperatureAt(CellPos cell);
    // What a pawn feels: the temperature at the cell it stands in.
    float ambientOf(Entity pawn);
    // A room's temperature; the outdoor temperature for the outdoors and for ids that are gone.
    float roomTemperature(RoomId room) const;
    // The pawn's comfortable range from its def's `stats.comfyTemperatureMin/Max`, moved by the
    // modifier. False if the def gives none.
    bool comfortRange(Entity pawn, float& min, float& max) const;
    // Degrees under the comfortable minimum or over the maximum at the pawn's cell (0 inside the
    // range or when it has none). Mood reads these.
    float degreesBelowComfort(Entity pawn);
    float degreesAboveComfort(Entity pawn);

    // Pushes heat into the enclosed room a cell is in, as a heat pusher would: `heatPerSecond` RimWorld heat units
    // a second for `seconds` game seconds, shared by the room's cells, never taking the room past `maxTemp`.
    // Heat pushed into the open air is lost. Fires use it (docs/design/fire.md).
    void addHeat(CellPos cell, double heatPerSecond, double seconds, float maxTemp);

    // One game second, after the rooms are brought up to date. `now` is the game second.
    void step(int64_t now);

    // ---- Saving
    // The temperature of every enclosed room under a roof, by anchor cell, in anchor order.
    std::vector<RoomTemperature> roomTemperatures();
    // Puts the temperatures back after loading (rooms not listed start at the outdoor temperature).
    void restore(const std::vector<RoomTemperature>& temperatures);
    void hashState(StateHasher& hasher);

private:
    struct SourceDef {
        bool pusher = false;
        float heatPerSecond = 0.0f;
        bool hasMax = false;
        float maxTemp = 0.0f;
        bool hasMin = false;
        float minTemp = 0.0f;
        bool isWall = false;
        bool isVent = false;
    };

    void syncRooms();
    void storeCells();
    void exchange();
    void exposePawns(int64_t now);
    bool usesRoomTemperature(const Room& room) const { return room.enclosed() && !room.outdoors(); }
    float temperatureBeyond(CellPos cell);
    // The temperature on the other side of an upper floor (the room above or below), or -1000 when
    // no room is there (a wall, water): no heat goes that way.
    float temperatureThroughFloor(CellPos cell);
    // The slot of a cell in cellTemp_, making its level's layer first if `make`; null if none.
    float* cellTempOf(CellPos c, bool make);
    RoomId roomOfSource(Entity source);
    void refreshSources();

    Map& map_;
    Things& things_;
    Rooms& rooms_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    uint64_t seed_;
    TemperatureNumbers numbers_;
    float outdoor_ = 10.0f;
    float ground_ = 10.0f;
    std::function<bool(Entity)> running_;
    std::function<void(Entity, float&, float&)> modifier_;

    std::vector<SourceDef> sourceDefs_;  // by def id
    DefId hypothermia_, heatstroke_, frostbite_;
    float coldPerDay_ = 0.08f;
    float hotPerDay_ = 0.08f;
    float coldRecoverPerDay_ = 0.9f;
    float hotRecoverPerDay_ = 0.9f;
    float frostbiteBelow_ = -10.0f;
    float frostbiteChancePerHour_ = 0.0004f;

    std::vector<float> roomTemp_;     // by RoomId; meaningful for rooms that use room temperature
    // Per level, per map cell: the temperature its room had at the last change, NaN if none. An
    // upper level's layer is made when it first has a room.
    std::vector<std::vector<float>> cellTemp_;
    std::vector<CellPos> storedCells_;  // the cells cellTemp_ holds a number for
    uint64_t seenRebuilds_ = ~uint64_t{0};
    uint64_t sourceChanges_ = ~uint64_t{0};
    std::vector<Entity> sources_;     // heaters, coolers, campfires, by entity id
    std::vector<float> frozen_;       // scratch: room temperatures at the start of an exchange
    std::vector<float> flow_;         // scratch: the change in each room from heat through its boundary
};

}  // namespace colony

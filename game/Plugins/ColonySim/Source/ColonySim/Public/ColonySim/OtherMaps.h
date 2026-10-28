#pragma once

#include "ColonySim/Entity.h"
#include "ColonySim/Social.h"

#include <map>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;

// Other maps (W5, docs/design/other-maps.md). Each visited tile is a sim of its own (`OtherMap` in
// Sim.h); this header has what the colony's sim keeps about the pawns who are there.

// On a colonist who is on another map: the entity stays in the colony's sim with this part only, so
// beds, allowed areas and the others' bonds and memories of it still name it, and it is filled again
// with everything the pawn is when it comes back. Saved in the "maps" section.
struct AwayPawn {
    int tile = -1;             // the other map's tile
    Entity there = kNoEntity;  // the pawn's entity in that map's sim
    // Its own bonds and memories when it left. Those about pawns who stayed here are put back on its
    // return; ties among the travellers come from the trip.
    Social social;
};

// The numbers (OtherMapRules in data/defs/Rules). Ours: RimWorld makes every map the size the player
// picked for the colony; our game design gives other maps 256 to 512 m.
struct OtherMapRules {
    int mapSize = 256;
    int settlementMapSize = 384;
    static OtherMapRules read(const DefDatabase& defs);
};

// A settlement on its map (W6, docs/design/settlements.md): SettlementRules in data/defs/Rules.
struct SettlementRules {
    struct Room {
        std::string kind;
        float weight = 1.0f;
        bool beds = false;                // beds along two walls
        std::vector<std::string> things;  // Things defNames, put inside in this order
    };
    int baseSizeMin = 34;  // cells a side of the base (recalled from RimWorld's settlement generator)
    int baseSizeMax = 38;
    int residentsMin = 8;  // ours
    int residentsMax = 12;
    int blocks = 3;        // the base is cut into blocks x blocks; the middle one is the yard
    float roomChance = 0.85f;
    std::map<std::string, std::vector<std::string>> wallStuff;       // tech level -> Things defNames
    std::map<std::string, std::vector<std::string>> furnitureStuff;
    std::vector<std::string> perimeterTechLevels;  // factions of these tech levels wall their base in
    std::vector<Room> rooms;
    std::vector<std::string> yard;  // put in the yard, beside the trader's spot
    static SettlementRules read(const DefDatabase& defs);
};

}  // namespace colony

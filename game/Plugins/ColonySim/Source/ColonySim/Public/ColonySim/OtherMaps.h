#pragma once

#include "ColonySim/Entity.h"
#include "ColonySim/Social.h"

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

}  // namespace colony

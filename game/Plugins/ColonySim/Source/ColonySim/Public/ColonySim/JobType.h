#pragma once

#include "ColonySim/Api.h"

#include <cstdint>

namespace colony {

// What a job is (docs/design/jobs.md). Here rather than in Jobs.h so the snapshot and events can
// name a job without the job system. New job types go at the end: saves store the number.
enum class JobType : uint8_t {
    None,        // no job: the pawn thinks at its next step
    Wait,        // stand still until a game second (idle)
    HaulToCell,  // carry an item to a storage cell
    Ingest,      // fetch food, take it to a chair at a table (or eat where it lies) and eat it
    LayDown,     // go to a bed (or lie on the ground) and sleep
    DeliverResources,  // carry materials to a blueprint or frame (construction.md)
    BuildFrame,        // work on a frame whose materials are all there
    Deconstruct,       // take down a building marked for it
    Mine,          // walk next to a designated rock or ore cell and dig it out (docs/design/mining.md)
    CutPlant,      // walk to a designated plant and cut it down (a tree gives wood)
    HarvestPlant,  // walk to a designated plant and pick what it gives
    DoBill,            // fetch ingredients to a work building, work, make the product (bills.md)
    Recreate,          // walk to a place and play, sit or stroll for a while, for joy (recreation.md)
    Research,          // walk to a research bench and add points to the current project (research.md)
    CleanFilth,        // walk to a cell with filth and clean it, a layer at a time (docs/design/room-stats.md)
    WearApparel,       // walk to clothes lying on the map and put them on, or take off a worn piece (apparel.md)
    StripPawn,         // walk to a downed or dead pawn marked for it and take its clothes and weapon (apparel.md)
    TameAnimal,        // fetch animal food, walk to a marked wild animal, feed it and try to tame it (docs/design/husbandry.md)
    TrainAnimal,       // the same for one training attempt on a tame animal
    SlaughterAnimal,   // walk to a marked tame animal and kill it
    FeedAnimal,        // fetch animal food and carry it to a hungry tame animal
    GatherAnimal,      // walk to a tame animal with full milk or wool and milk or shear it (docs/design/breeding.md)
    SowCrop,           // walk next to a cell of a growing zone and sow the zone's crop (docs/design/farming.md)
    ExtinguishFire,    // walk next to a fire and beat it out (docs/design/fire.md)
    BedRest,           // go to a bed and lie in it until the injuries have healed (docs/design/medicine.md)
    TendPatient,       // fetch medicine, walk to a patient and tend its injuries (docs/design/medicine.md)
    Rescue,            // carry a downed colonist to a bed (docs/design/medicine.md)
    CapturePrisoner,   // carry a downed enemy to a prison bed and make it a prisoner (docs/design/prisoners.md)
    FeedPrisoner,      // fetch food, walk to a hungry prisoner and hand it over
    WardenInteract,    // walk to a prisoner and talk to it: resistance, recruiting or release
};

COLONY_API const char* jobTypeName(JobType type);

}  // namespace colony

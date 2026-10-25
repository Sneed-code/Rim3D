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
    DeliverResources,  // carry materials to a blueprint or frame (construction.md)
    BuildFrame,        // work on a frame whose materials are all there
    Deconstruct,       // take down a building marked for it
};

COLONY_API const char* jobTypeName(JobType type);

}  // namespace colony

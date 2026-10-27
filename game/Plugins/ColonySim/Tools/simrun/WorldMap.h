// The world map (docs/design/world-map.md): `simrun --world FILE.png [--seed N] [--world-size WxH] [--check]`.
#pragma once

#include "ColonySim/Defs.h"

#include <memory>
#include <string>

namespace worldmap {

// Makes the world, prints its numbers, draws it (biomes, hills, rivers, roads, settlements, the
// suggested start tile) and with `check` fails (returns 1) if it is not a believable planet or
// not the same world when made twice. Returns 2 if the data has no WorldGen def.
int run(unsigned long long seed, int width, int height, const std::string& png, bool check,
        std::shared_ptr<const colony::DefDatabase> defs);

}  // namespace worldmap

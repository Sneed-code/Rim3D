#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Sim.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;

// Saving and loading the whole game state. See docs/design/saves.md.
//
// A save is a versioned binary file: the settings, the clock and random stream, every thing with
// all its components, the applied command log, commands still queued, and the state-hash
// checkpoints. Things name their defs by `defName`, never by id, so adding defs doesn't break a
// save. What can be rebuilt is left out: the map's ground (generated again from the seed and
// checked against a hash in the file) and regions.
//
// Loading a save and running on gives the same state hash, step for step, as never saving.

// Writes the sim as it is now. Commands submitted but not yet applied are included and apply at
// the next step after loading.
COLONY_API std::vector<uint8_t> saveSim(const Sim& sim);

struct SaveLoadResult {
    std::unique_ptr<Sim> sim;  // null if the load failed
    std::string error;         // one line saying why, empty on success
    // Things worth knowing that didn't stop the load: other sim version, other data files.
    std::vector<std::string> warnings;
};

// Makes a sim from a save, using `defs` (the data files of this run) for the defs it names.
// Fails, with a message, on a damaged or truncated file, a format this build can't read, a
// def that no longer exists, or a map generator that now gives a different map.
// The loaded sim's events are empty: Unreal rebuilds its view from a snapshot.
COLONY_API SaveLoadResult loadSim(const std::vector<uint8_t>& bytes, std::shared_ptr<const DefDatabase> defs);

// A readable JSON dump for debugging: the same content as the binary save, with names. Not a
// save format; nothing reads it back.
COLONY_API std::string saveToJson(const Sim& sim);
// The same dump of a save file's bytes, with no defs needed. Returns false with `error` set if
// the bytes aren't a readable save.
COLONY_API bool saveFileToJson(const std::vector<uint8_t>& bytes, std::string& json, std::string& error);

COLONY_API bool readSaveFile(const std::string& path, std::vector<uint8_t>& out, std::string& error);
COLONY_API bool writeSaveFile(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error);

}  // namespace colony

#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Sim.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace colony {

// A replay is the sim's settings plus its command log: playing it on the same build gives the
// same game, step for step. It also carries state hashes so a replay run can say whether it
// matched and, if not, from roughly when. Files are JSON text; docs/design/bridge.md has the
// format. A replay with no hashes is a scripted scenario.
struct Replay {
    std::string simVersion;  // kSimVersion of the build that recorded it
    uint64_t defsHash = 0;   // DefDatabase::contentHash() of the defs it ran with
    SimSettings settings;
    std::vector<LoggedCommand> commands;  // in step order
    std::vector<Checkpoint> checkpoints;  // in step order
    int64_t endStep = -1;                 // game second it was recorded at; -1 if not given
    bool hasEndHash = false;
    uint64_t endHash = 0;  // Sim::stateHash() at endStep
};

// Everything the sim has done so far: settings, applied commands, checkpoints, and the current
// time and state hash as the end. Commands still waiting for the next step are not included.
COLONY_API Replay recordReplay(const Sim& sim);

COLONY_API std::string writeReplay(const Replay& replay);
// Parses a replay file's text. On failure returns false with one line in `error`.
COLONY_API bool readReplay(const std::string& text, Replay& out, std::string& error);

COLONY_API bool loadReplayFile(const std::string& path, Replay& out, std::string& error);
COLONY_API bool saveReplayFile(const std::string& path, const Replay& replay, std::string& error);

// Submits a replay's commands at the right steps. Call submitDue before each Sim::step.
class COLONY_API ReplayFeeder {
public:
    explicit ReplayFeeder(const Replay& replay) : replay_(replay) {}
    // Submits every command logged for the step the sim is about to run (and any it missed).
    void submitDue(Sim& sim);

private:
    const Replay& replay_;
    size_t next_ = 0;
};

// Steps a sim made from `replay.settings` (and not stepped yet) up to game second `endStep`,
// feeding it the replay's commands.
COLONY_API void playReplay(const Replay& replay, Sim& sim, int64_t endStep);

struct ReplayCheck {
    int compared = 0;              // checkpoints (and end hash) both had
    int64_t firstMismatch = -1;    // game second of the first hash that differed; -1 if none
    bool matched() const { return firstMismatch < 0; }
};

// Compares the hashes `expected` recorded with the ones `sim` produced while playing it.
COLONY_API ReplayCheck checkReplay(const Replay& expected, const Sim& sim);

}  // namespace colony

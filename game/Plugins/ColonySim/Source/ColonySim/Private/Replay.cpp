#include "ColonySim/Replay.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Version.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>

// The sim builds without exceptions, so the JSON library must not throw. With this defined a
// wrong-typed read aborts, so every field's type is checked before it is read.
#ifndef JSON_NOEXCEPTION
#define JSON_NOEXCEPTION
#endif
#include "nlohmann/json.hpp"

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
using ReplayJson = nlohmann::json;
using ReplayOrderedJson = nlohmann::ordered_json;  // writing: keys stay in the order set

constexpr const char* kReplayFormat = "rim3d-replay";
constexpr int64_t kReplayFormatVersion = 1;

const char* const kReplayRotNames[] = {"North", "East", "South", "West"};

std::string replayHex(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

bool replayParseHex(const ReplayJson& j, uint64_t& out) {
    if (!j.is_string()) {
        return false;
    }
    const std::string& text = j.get_ref<const std::string&>();
    if (text.empty() || text.size() > 16) {
        return false;
    }
    char* end = nullptr;
    out = std::strtoull(text.c_str(), &end, 16);
    return end == text.c_str() + text.size();
}

ReplayOrderedJson replayCommandJson(const LoggedCommand& logged) {
    ReplayOrderedJson j = ReplayOrderedJson::object();
    j["step"] = logged.step;
    j["type"] = commandName(logged.command);
    std::visit(
        [&j](const auto& c) {
            using Type = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<Type, SpawnThingCommand>) {
                j["def"] = c.def;
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
                j["count"] = c.count;
                j["rot"] = kReplayRotNames[static_cast<int>(c.rot)];
            } else {
                static_assert(std::is_same_v<Type, DestroyThingCommand>, "write every command type");
                j["thing"] = entt::to_integral(c.thing);
            }
        },
        logged.command);
    return j;
}

// Reads JSON fields with type checks. Each reader leaves `out` alone when the key is absent and
// returns false (with `error` set) when it is present with the wrong type.
class ReplayReader {
public:
    explicit ReplayReader(std::string& error) : error_(error) {}

    template <typename Int>
    bool integer(const ReplayJson& obj, const char* key, Int& out, const std::string& where) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            return true;
        }
        if (!it->is_number_integer()) {
            return fail(where + "." + key + " must be a whole number");
        }
        if constexpr (std::is_unsigned_v<Int>) {
            if (it->is_number_unsigned()) {
                out = static_cast<Int>(it->get<uint64_t>());
                return true;
            }
            return fail(where + "." + key + " must not be negative");
        } else {
            out = static_cast<Int>(it->get<int64_t>());
            return true;
        }
    }

    bool flag(const ReplayJson& obj, const char* key, bool& out, const std::string& where) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            return true;
        }
        if (!it->is_boolean()) {
            return fail(where + "." + key + " must be true or false");
        }
        out = it->get<bool>();
        return true;
    }

    bool text(const ReplayJson& obj, const char* key, std::string& out, const std::string& where) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            return true;
        }
        if (!it->is_string()) {
            return fail(where + "." + key + " must be text");
        }
        out = it->get<std::string>();
        return true;
    }

    bool hash(const ReplayJson& obj, const char* key, uint64_t& out, bool& present, const std::string& where) {
        const auto it = obj.find(key);
        present = it != obj.end();
        if (present && !replayParseHex(*it, out)) {
            return fail(where + "." + key + " must be a hash in hex, e.g. \"00ff12ab34cd56ef\"");
        }
        return true;
    }

    bool fail(std::string message) {
        error_ = std::move(message);
        return false;
    }

private:
    std::string& error_;
};

bool replayReadCommand(ReplayReader& r, const ReplayJson& j, const std::string& where, LoggedCommand& out) {
    if (!j.is_object()) {
        return r.fail(where + " must be an object");
    }
    if (!j.contains("step") || !j.contains("type")) {
        return r.fail(where + " needs \"step\" and \"type\"");
    }
    std::string type;
    if (!r.integer(j, "step", out.step, where) || !r.text(j, "type", type, where)) {
        return false;
    }
    if (out.step < 0) {
        return r.fail(where + ".step must not be negative");
    }
    if (type == "SpawnThing") {
        SpawnThingCommand c;
        std::string rot = kReplayRotNames[0];
        if (!r.text(j, "def", c.def, where) || !r.integer(j, "count", c.count, where) || !r.text(j, "rot", rot, where)) {
            return false;
        }
        const auto cell = j.find("cell");
        if (cell == j.end() || !cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() ||
            !(*cell)[1].is_number_integer() || !(*cell)[2].is_number_integer()) {
            return r.fail(where + ".cell must be [x, y, level]");
        }
        c.cell = CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()};
        bool rotFound = false;
        for (int i = 0; i < 4; ++i) {
            if (rot == kReplayRotNames[i]) {
                c.rot = static_cast<Rot4>(i);
                rotFound = true;
            }
        }
        if (!rotFound) {
            return r.fail(where + ".rot must be North, East, South or West");
        }
        out.command = std::move(c);
    } else if (type == "DestroyThing") {
        uint32_t thing = entt::to_integral(kNoEntity);
        if (!r.integer(j, "thing", thing, where)) {
            return false;
        }
        out.command = DestroyThingCommand{static_cast<Entity>(thing)};
    } else {
        return r.fail(where + ".type: unknown command '" + type + "'");
    }
    return true;
}

}  // namespace

Replay recordReplay(const Sim& sim) {
    Replay replay;
    replay.simVersion = kSimVersion;
    replay.defsHash = sim.defs().contentHash();
    replay.settings = sim.settings();
    replay.commands = sim.commandLog();
    replay.checkpoints = sim.checkpoints();
    replay.endStep = sim.time().seconds;
    replay.hasEndHash = true;
    replay.endHash = sim.stateHash();
    return replay;
}

std::string writeReplay(const Replay& replay) {
    const SimSettings& s = replay.settings;
    ReplayOrderedJson settings = ReplayOrderedJson::object();
    settings["seed"] = s.seed;
    settings["mapSize"] = s.mapSize;
    settings["pawns"] = s.pawns;
    settings["items"] = s.items;
    settings["flatMap"] = s.flatMap;
    settings["pawnDef"] = s.pawnDef;
    settings["haulItemDef"] = s.haulItemDef;
    settings["checkpointEvery"] = s.checkpointEvery;
    settings["pathBudget"] = s.pathBudget;

    // One command and one checkpoint per line, so files diff and read well.
    std::string out = "{\n";
    out += "  \"format\": \"" + std::string(kReplayFormat) + "\",\n";
    out += "  \"version\": " + std::to_string(kReplayFormatVersion) + ",\n";
    out += "  \"simVersion\": " + ReplayJson(replay.simVersion).dump() + ",\n";
    out += "  \"defsHash\": \"" + replayHex(replay.defsHash) + "\",\n";
    out += "  \"settings\": " + settings.dump() + ",\n";
    if (replay.endStep >= 0) {
        out += "  \"endStep\": " + std::to_string(replay.endStep) + ",\n";
    }
    if (replay.hasEndHash) {
        out += "  \"endHash\": \"" + replayHex(replay.endHash) + "\",\n";
    }
    out += "  \"commands\": [";
    for (size_t i = 0; i < replay.commands.size(); ++i) {
        out += (i == 0 ? "\n    " : ",\n    ") + replayCommandJson(replay.commands[i]).dump();
    }
    out += replay.commands.empty() ? "],\n" : "\n  ],\n";
    out += "  \"checkpoints\": [";
    for (size_t i = 0; i < replay.checkpoints.size(); ++i) {
        const Checkpoint& c = replay.checkpoints[i];
        out += (i == 0 ? "\n    " : ",\n    ");
        out += "{\"step\": " + std::to_string(c.step) + ", \"hash\": \"" + replayHex(c.hash) + "\"}";
    }
    out += replay.checkpoints.empty() ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}

bool readReplay(const std::string& text, Replay& out, std::string& error) {
    out = Replay();
    ReplayReader r(error);
    const ReplayJson root = ReplayJson::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return r.fail("not a JSON object");
    }
    std::string format;
    int64_t version = 0;
    if (!r.text(root, "format", format, "replay") || format != kReplayFormat) {
        return r.fail("not a replay: \"format\" must be \"" + std::string(kReplayFormat) + "\"");
    }
    if (!r.integer(root, "version", version, "replay") || version != kReplayFormatVersion) {
        return r.fail("replay format version " + std::to_string(version) + " is not supported (this build reads " +
                      std::to_string(kReplayFormatVersion) + ")");
    }
    bool hasDefsHash = false;
    if (!r.text(root, "simVersion", out.simVersion, "replay") ||
        !r.hash(root, "defsHash", out.defsHash, hasDefsHash, "replay") ||
        !r.integer(root, "endStep", out.endStep, "replay") ||
        !r.hash(root, "endHash", out.endHash, out.hasEndHash, "replay")) {
        return false;
    }

    const auto settings = root.find("settings");
    if (settings == root.end() || !settings->is_object()) {
        return r.fail("replay needs a \"settings\" object");
    }
    SimSettings& s = out.settings;
    const std::string where = "settings";
    if (!r.integer(*settings, "seed", s.seed, where) || !r.integer(*settings, "mapSize", s.mapSize, where) ||
        !r.integer(*settings, "pawns", s.pawns, where) || !r.integer(*settings, "items", s.items, where) ||
        !r.flag(*settings, "flatMap", s.flatMap, where) || !r.text(*settings, "pawnDef", s.pawnDef, where) ||
        !r.text(*settings, "haulItemDef", s.haulItemDef, where) ||
        !r.integer(*settings, "checkpointEvery", s.checkpointEvery, where) ||
        !r.integer(*settings, "pathBudget", s.pathBudget, where)) {
        return false;
    }
    if (s.mapSize <= 0 || s.mapSize > 4096 || s.pawns < 0 || s.items < 0 || s.checkpointEvery < 0 ||
        s.pathBudget <= 0) {
        return r.fail(
            "settings: mapSize must be 1 to 4096; pawns, items and checkpointEvery not negative; pathBudget above 0");
    }

    if (const auto commands = root.find("commands"); commands != root.end()) {
        if (!commands->is_array()) {
            return r.fail("\"commands\" must be an array");
        }
        for (size_t i = 0; i < commands->size(); ++i) {
            LoggedCommand logged;
            const std::string at = "commands[" + std::to_string(i) + "]";
            if (!replayReadCommand(r, (*commands)[i], at, logged)) {
                return false;
            }
            if (!out.commands.empty() && logged.step < out.commands.back().step) {
                return r.fail(at + ".step is earlier than the command before it");
            }
            out.commands.push_back(std::move(logged));
        }
    }
    if (const auto checkpoints = root.find("checkpoints"); checkpoints != root.end()) {
        if (!checkpoints->is_array()) {
            return r.fail("\"checkpoints\" must be an array");
        }
        for (size_t i = 0; i < checkpoints->size(); ++i) {
            const ReplayJson& j = (*checkpoints)[i];
            const std::string at = "checkpoints[" + std::to_string(i) + "]";
            Checkpoint c;
            bool hasHash = false;
            if (!j.is_object() || !j.contains("step") || !r.integer(j, "step", c.step, at) ||
                !r.hash(j, "hash", c.hash, hasHash, at) || !hasHash) {
                return r.fail(at + " must be {\"step\": N, \"hash\": \"...\"}");
            }
            if (!out.checkpoints.empty() && c.step <= out.checkpoints.back().step) {
                return r.fail(at + ".step must be later than the checkpoint before it");
            }
            out.checkpoints.push_back(c);
        }
    }
    error.clear();
    return true;
}

bool loadReplayFile(const std::string& path, Replay& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot read file";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (!readReplay(text.str(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool saveReplayFile(const std::string& path, const Replay& replay, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const std::string text = writeReplay(replay);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        error = path + ": cannot write file";
        return false;
    }
    return true;
}

void ReplayFeeder::submitDue(Sim& sim) {
    while (next_ < replay_.commands.size() && replay_.commands[next_].step <= sim.time().seconds) {
        sim.submit(replay_.commands[next_].command);
        ++next_;
    }
}

void playReplay(const Replay& replay, Sim& sim, int64_t endStep) {
    ReplayFeeder feeder(replay);
    while (sim.time().seconds < endStep) {
        feeder.submitDue(sim);
        sim.step();
    }
}

ReplayCheck checkReplay(const Replay& expected, const Sim& sim) {
    ReplayCheck check;
    const std::vector<Checkpoint>& got = sim.checkpoints();
    size_t g = 0;
    for (const Checkpoint& want : expected.checkpoints) {
        while (g < got.size() && got[g].step < want.step) {
            ++g;
        }
        if (g == got.size() || got[g].step != want.step) {
            continue;  // the run didn't record this one (shorter run or other checkpointEvery)
        }
        ++check.compared;
        if (got[g].hash != want.hash) {
            check.firstMismatch = want.step;
            return check;
        }
    }
    if (expected.hasEndHash && expected.endStep == sim.time().seconds) {
        ++check.compared;
        if (sim.stateHash() != expected.endHash) {
            check.firstMismatch = expected.endStep;
        }
    }
    return check;
}

}  // namespace colony

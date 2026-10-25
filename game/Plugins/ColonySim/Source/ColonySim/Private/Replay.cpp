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
constexpr StoragePriority kReplayPriorities[] = {StoragePriority::Low, StoragePriority::Normal, StoragePriority::Preferred,
                                                 StoragePriority::Important, StoragePriority::Critical};

ReplayOrderedJson replayRectJson(const CellRect& r) {
    return ReplayOrderedJson::array({r.minX, r.minY, r.sizeX, r.sizeY, r.level});
}
const char* const kReplayStanceNames[] = {"Standing", "Crouching", "Prone"};

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
            } else if constexpr (std::is_same_v<Type, DestroyThingCommand>) {
                j["thing"] = entt::to_integral(c.thing);
            } else if constexpr (std::is_same_v<Type, DamagePawnCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["damageType"] = c.damageType;
                j["amount"] = c.amount;
                j["part"] = c.part;
            } else if constexpr (std::is_same_v<Type, TendPawnCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["quality"] = c.quality;
            } else if constexpr (std::is_same_v<Type, SetWorkPriorityCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["workType"] = c.workType;
                j["priority"] = c.priority;
            } else if constexpr (std::is_same_v<Type, CreateStockpileCommand>) {
                j["area"] = replayRectJson(c.area);
                j["priority"] = storagePriorityName(c.priority);
            } else if constexpr (std::is_same_v<Type, RemoveZoneCellsCommand>) {
                j["area"] = replayRectJson(c.area);
            } else if constexpr (std::is_same_v<Type, SetStockpilePriorityCommand>) {
                j["zone"] = c.zone;
                j["priority"] = storagePriorityName(c.priority);
            } else if constexpr (std::is_same_v<Type, SetStockpileAllowedCommand>) {
                j["zone"] = c.zone;
                j["def"] = c.def;
                j["allowed"] = c.allowed;
            } else if constexpr (std::is_same_v<Type, OrderHaulCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["thing"] = entt::to_integral(c.thing);
            } else if constexpr (std::is_same_v<Type, ControlPawnCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
            } else if constexpr (std::is_same_v<Type, DirectMoveCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                // A float is written as the double that holds it exactly, so it reads back the same.
                j["x"] = static_cast<double>(c.x);
                j["y"] = static_cast<double>(c.y);
            } else if constexpr (std::is_same_v<Type, SetScheduleCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["hour"] = c.hour;
                j["kind"] = scheduleKindName(c.kind);
            } else if constexpr (std::is_same_v<Type, AssignBedCommand>) {
                j["bed"] = entt::to_integral(c.bed);
                j["pawn"] = entt::to_integral(c.pawn);
            } else if constexpr (std::is_same_v<Type, EquipCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["item"] = entt::to_integral(c.item);
            } else if constexpr (std::is_same_v<Type, ShootCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["target"] = entt::to_integral(c.target);
                j["point"] = {static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.z)};
                j["player"] = c.player;
            } else {
                static_assert(std::is_same_v<Type, SetStanceCommand>, "write every command type");
                j["pawn"] = entt::to_integral(c.pawn);
                j["stance"] = kReplayStanceNames[static_cast<int>(c.stance)];
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

    bool number(const ReplayJson& obj, const char* key, float& out, const std::string& where) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            return true;
        }
        if (!it->is_number()) {
            return fail(where + "." + key + " must be a number");
        }
        out = static_cast<float>(it->get<double>());
        return true;
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

// "area": [minX, minY, sizeX, sizeY, level]
bool replayReadRect(ReplayReader& r, const ReplayJson& j, const std::string& where, CellRect& out) {
    const auto area = j.find("area");
    if (area == j.end() || !area->is_array() || area->size() != 5) {
        return r.fail(where + ".area must be [minX, minY, sizeX, sizeY, level]");
    }
    int v[5] = {};
    for (size_t i = 0; i < 5; ++i) {
        if (!(*area)[i].is_number_integer()) {
            return r.fail(where + ".area must be [minX, minY, sizeX, sizeY, level]");
        }
        v[i] = (*area)[i].get<int>();
    }
    if (v[2] < 0 || v[3] < 0) {
        return r.fail(where + ".area must not have a negative size");
    }
    out = CellRect{v[0], v[1], v[2], v[3], v[4]};
    return true;
}

// "priority": "Low", "Normal" (the default), "Preferred", "Important" or "Critical"
bool replayReadPriority(ReplayReader& r, const ReplayJson& j, const std::string& where, StoragePriority& out) {
    std::string name = storagePriorityName(StoragePriority::Normal);
    if (!r.text(j, "priority", name, where)) {
        return false;
    }
    for (const StoragePriority p : kReplayPriorities) {
        if (name == storagePriorityName(p)) {
            out = p;
            return true;
        }
    }
    return r.fail(where + ".priority must be Low, Normal, Preferred, Important or Critical");
}


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
    } else if (type == "ControlPawn") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        out.command = ControlPawnCommand{static_cast<Entity>(pawn)};
    } else if (type == "DirectMove") {
        DirectMoveCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        for (const auto& [key, field] : {std::pair<const char*, float*>{"x", &c.x}, std::pair<const char*, float*>{"y", &c.y}}) {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number()) {
                return r.fail(where + "." + key + " must be a number");
            }
            *field = static_cast<float>(it->get<double>());
        }
        out.command = c;
    } else if (type == "DamagePawn") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        DamagePawnCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "damageType", c.damageType, where) ||
            !r.number(j, "amount", c.amount, where) || !r.integer(j, "part", c.part, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "TendPawn") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        TendPawnCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.number(j, "quality", c.quality, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "SetWorkPriority") {
        SetWorkPriorityCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "workType", c.workType, where) ||
            !r.integer(j, "priority", c.priority, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "CreateStockpile") {
        CreateStockpileCommand c;
        if (!replayReadRect(r, j, where, c.area) || !replayReadPriority(r, j, where, c.priority)) {
            return false;
        }
        out.command = c;
    } else if (type == "RemoveZoneCells") {
        RemoveZoneCellsCommand c;
        if (!replayReadRect(r, j, where, c.area)) {
            return false;
        }
        out.command = c;
    } else if (type == "SetStockpilePriority") {
        SetStockpilePriorityCommand c;
        if (!r.integer(j, "zone", c.zone, where) || !replayReadPriority(r, j, where, c.priority)) {
            return false;
        }
        out.command = c;
    } else if (type == "SetStockpileAllowed") {
        SetStockpileAllowedCommand c;
        if (!r.integer(j, "zone", c.zone, where) || !r.text(j, "def", c.def, where) ||
            !r.flag(j, "allowed", c.allowed, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "OrderHaul") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t thing = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "thing", thing, where)) {
            return false;
        }
        out.command = OrderHaulCommand{static_cast<Entity>(pawn), static_cast<Entity>(thing)};
    } else if (type == "SetSchedule") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        SetScheduleCommand c;
        std::string kind = scheduleKindName(c.kind);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "hour", c.hour, where) || !r.text(j, "kind", kind, where)) {
            return false;
        }
        bool known = false;
        for (const ScheduleKind k : {ScheduleKind::Anything, ScheduleKind::Work, ScheduleKind::Joy, ScheduleKind::Sleep}) {
            if (kind == scheduleKindName(k)) {
                c.kind = k;
                known = true;
            }
        }
        if (!known) {
            return r.fail(where + ".kind must be Anything, Work, Joy or Sleep");
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = c;
    } else if (type == "AssignBed") {
        uint32_t bed = entt::to_integral(kNoEntity);
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "bed", bed, where) || !r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        out.command = AssignBedCommand{static_cast<Entity>(bed), static_cast<Entity>(pawn)};
    } else if (type == "Equip") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t item = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "item", item, where)) {
            return false;
        }
        out.command = EquipCommand{static_cast<Entity>(pawn), static_cast<Entity>(item)};
    } else if (type == "Shoot") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t target = entt::to_integral(kNoEntity);
        ShootCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "target", target, where) ||
            !r.flag(j, "player", c.player, where)) {
            return false;
        }
        const auto point = j.find("point");
        if (point != j.end()) {
            if (!point->is_array() || point->size() != 3 || !(*point)[0].is_number() || !(*point)[1].is_number() ||
                !(*point)[2].is_number()) {
                return r.fail(where + ".point must be [x, y, z]");
            }
            c.x = (*point)[0].get<float>();
            c.y = (*point)[1].get<float>();
            c.z = (*point)[2].get<float>();
        }
        c.pawn = static_cast<Entity>(pawn);
        c.target = static_cast<Entity>(target);
        out.command = c;
    } else if (type == "SetStance") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        std::string stance = kReplayStanceNames[0];
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "stance", stance, where)) {
            return false;
        }
        SetStanceCommand c;
        c.pawn = static_cast<Entity>(pawn);
        bool found = false;
        for (int i = 0; i < 3; ++i) {
            if (stance == kReplayStanceNames[i]) {
                c.stance = static_cast<Stance>(i);
                found = true;
            }
        }
        if (!found) {
            return r.fail(where + ".stance must be Standing, Crouching or Prone");
        }
        out.command = c;
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
    if (!s.scenario.empty()) {
        settings["scenario"] = s.scenario;
    }

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
        !r.integer(*settings, "pathBudget", s.pathBudget, where) || !r.text(*settings, "scenario", s.scenario, where)) {
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

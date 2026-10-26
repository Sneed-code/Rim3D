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
            } else if constexpr (std::is_same_v<Type, PlaceBlueprintCommand>) {
                j["def"] = c.def;
                j["stuff"] = c.stuff;
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
                j["rot"] = kReplayRotNames[static_cast<int>(c.rot)];
            } else if constexpr (std::is_same_v<Type, CancelBuildCommand>) {
                j["thing"] = entt::to_integral(c.thing);
            } else if constexpr (std::is_same_v<Type, DeconstructCommand>) {
                j["thing"] = entt::to_integral(c.thing);
            } else if constexpr (std::is_same_v<Type, EquipCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["item"] = entt::to_integral(c.item);
            } else if constexpr (std::is_same_v<Type, ShootCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["target"] = entt::to_integral(c.target);
                j["point"] = {static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.z)};
                j["player"] = c.player;
            } else if constexpr (std::is_same_v<Type, SetStanceCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["stance"] = kReplayStanceNames[static_cast<int>(c.stance)];
            } else if constexpr (std::is_same_v<Type, DesignateCommand>) {
                j["area"] = replayRectJson(c.area);
                j["kind"] = designationKindName(c.kind);
            } else if constexpr (std::is_same_v<Type, CancelDesignationsCommand>) {
                j["area"] = replayRectJson(c.area);
                j["kind"] = designationKindName(c.kind);
            } else if constexpr (std::is_same_v<Type, DraftCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["drafted"] = c.drafted;
            } else if constexpr (std::is_same_v<Type, MoveOrderCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
            } else if constexpr (std::is_same_v<Type, AttackOrderCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["target"] = entt::to_integral(c.target);
            } else if constexpr (std::is_same_v<Type, SetFactionCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["faction"] = c.faction;
            } else if constexpr (std::is_same_v<Type, SetLineageCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["lineage"] = c.lineage;
            } else if constexpr (std::is_same_v<Type, ReloadCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
            } else if constexpr (std::is_same_v<Type, AddBillCommand>) {
                j["building"] = entt::to_integral(c.building);
                j["recipe"] = c.recipe;
                j["mode"] = billModeName(c.mode);
                j["count"] = c.count;
            } else if constexpr (std::is_same_v<Type, RemoveBillCommand>) {
                j["building"] = entt::to_integral(c.building);
                j["bill"] = c.bill;
            } else if constexpr (std::is_same_v<Type, SetBillCommand>) {
                j["building"] = entt::to_integral(c.building);
                j["bill"] = c.bill;
                j["mode"] = billModeName(c.mode);
                j["count"] = c.count;
                j["suspended"] = c.suspended;
            } else if constexpr (std::is_same_v<Type, HandsOnCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["mode"] = c.mode == HandsOnMode::Work ? "Work" : (c.mode == HandsOnMode::PutDown ? "PutDown" : "Stop");
                j["thing"] = entt::to_integral(c.thing);
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
            } else if constexpr (std::is_same_v<Type, FlickCommand>) {
                j["thing"] = entt::to_integral(c.thing);
                j["on"] = c.on;
            } else if constexpr (std::is_same_v<Type, PlaceFloorCommand>) {
                j["area"] = replayRectJson(c.area);
                j["floor"] = c.floor;
            } else if constexpr (std::is_same_v<Type, AddFilthCommand>) {
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
                j["kind"] = c.kind;
                j["layers"] = c.layers;
            } else if constexpr (std::is_same_v<Type, SetRefuelCommand>) {
                j["building"] = entt::to_integral(c.building);
                j["autoRefuel"] = c.autoRefuel;
                j["fillNow"] = c.fillNow;
            } else if constexpr (std::is_same_v<Type, SetBillOptionsCommand>) {
                j["building"] = entt::to_integral(c.building);
                j["bill"] = c.bill;
                j["skillMin"] = c.skillMin;
                j["skillMax"] = c.skillMax;
                j["radius"] = c.radius;
                j["storeZone"] = c.storeZone;
                j["unpauseAt"] = c.unpauseAt;
                j["disallowed"] = c.disallowed;
            } else if constexpr (std::is_same_v<Type, MoveBillCommand>) {
                j["building"] = entt::to_integral(c.building);
                j["bill"] = c.bill;
                j["index"] = c.index;
            } else if constexpr (std::is_same_v<Type, ShortCircuitCommand>) {
                j["thing"] = entt::to_integral(c.thing);
            } else if constexpr (std::is_same_v<Type, SetResearchCommand>) {
                j["project"] = c.project;
            } else if constexpr (std::is_same_v<Type, UnlockResearchCommand>) {
                j["project"] = c.project;
            } else if constexpr (std::is_same_v<Type, ThrowCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["item"] = c.item;
                j["target"] = entt::to_integral(c.target);
                j["point"] = {static_cast<double>(c.x), static_cast<double>(c.y)};
                j["player"] = c.player;
            } else if constexpr (std::is_same_v<Type, ExplodeCommand>) {
                j["point"] = {static_cast<double>(c.x), static_cast<double>(c.y)};
                j["radius"] = static_cast<double>(c.radius);
                j["damage"] = static_cast<double>(c.damage);
                j["damageType"] = c.damageType;
                j["penetration"] = static_cast<double>(c.penetration);
            } else if constexpr (std::is_same_v<Type, StartRaidCommand>) {
                j["faction"] = c.faction;
                j["points"] = static_cast<double>(c.points);
                j["edge"] = c.edge;
            } else if constexpr (std::is_same_v<Type, SoundAlarmCommand>) {
                j["on"] = c.on;
            } else if constexpr (std::is_same_v<Type, SetDefenseRoleCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["role"] = c.role;
                j["hasSpot"] = c.hasSpot;
                j["spot"] = {c.spot.x, c.spot.y, c.spot.level};
            } else if constexpr (std::is_same_v<Type, MarkShelterCommand>) {
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
                j["clear"] = c.clear;
            } else if constexpr (std::is_same_v<Type, MarkAnimalCommand>) {
                j["animal"] = entt::to_integral(c.animal);
                j["mark"] = c.mark;
                j["on"] = c.on;
            } else if constexpr (std::is_same_v<Type, MarkAnimalsInAreaCommand>) {
                j["area"] = replayRectJson(c.area);
                j["mark"] = c.mark;
                j["on"] = c.on;
            } else if constexpr (std::is_same_v<Type, SetAnimalNameCommand>) {
                j["animal"] = entt::to_integral(c.animal);
                j["name"] = c.name;
            } else if constexpr (std::is_same_v<Type, SetTrainingCommand>) {
                j["animal"] = entt::to_integral(c.animal);
                j["trainable"] = c.trainable;
                j["wanted"] = c.wanted;
            } else if constexpr (std::is_same_v<Type, CreateAnimalZoneCommand> || std::is_same_v<Type, RemoveAnimalZoneCellsCommand>) {
                j["area"] = replayRectJson(c.area);
            } else if constexpr (std::is_same_v<Type, AssignAnimalZoneCommand>) {
                j["animal"] = entt::to_integral(c.animal);
                j["zone"] = c.zone;
            } else if constexpr (std::is_same_v<Type, SetBreedingCommand>) {
                j["animal"] = entt::to_integral(c.animal);
                j["zone"] = c.zone;
                j["allow"] = c.allow;
            } else if constexpr (std::is_same_v<Type, MeleeCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["target"] = entt::to_integral(c.target);
                j["mode"] = c.mode == MeleeMode::Attack ? "Attack" : "Block";
            } else if constexpr (std::is_same_v<Type, DodgeRollCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["dx"] = static_cast<double>(c.dx);
                j["dy"] = static_cast<double>(c.dy);
            } else if constexpr (std::is_same_v<Type, SetOutfitCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["outfit"] = c.outfit;
            } else if constexpr (std::is_same_v<Type, EditOutfitCommand>) {
                j["outfit"] = c.outfit;
                j["disallowed"] = c.disallowed;
                j["hitPointsMin"] = c.hitPointsMin;
                j["hitPointsMax"] = c.hitPointsMax;
                j["qualityMin"] = c.qualityMin;
                j["qualityMax"] = c.qualityMax;
            } else if constexpr (std::is_same_v<Type, AddOutfitCommand>) {
                j["name"] = c.name;
            } else if constexpr (std::is_same_v<Type, RemoveOutfitCommand>) {
                j["outfit"] = c.outfit;
            } else if constexpr (std::is_same_v<Type, StripCommand>) {
                j["thing"] = entt::to_integral(c.thing);
                j["strip"] = c.strip;
            } else if constexpr (std::is_same_v<Type, InteractCommand>) {
                j["initiator"] = entt::to_integral(c.initiator);
                j["recipient"] = entt::to_integral(c.recipient);
                j["interaction"] = c.interaction;
                j["outcome"] = c.outcome;
            } else if constexpr (std::is_same_v<Type, SetWeatherCommand>) {
                j["weather"] = c.weather;
                j["hours"] = c.hours;
            } else if constexpr (std::is_same_v<Type, SpendTalentPointCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["talent"] = c.talent;
            } else if constexpr (std::is_same_v<Type, RearmTrapCommand>) {
                j["trap"] = entt::to_integral(c.trap);
            } else if constexpr (std::is_same_v<Type, CreateGrowingZoneCommand>) {
                j["area"] = replayRectJson(c.area);
                j["crop"] = c.crop;
            } else if constexpr (std::is_same_v<Type, RemoveGrowingZoneCellsCommand>) {
                j["area"] = replayRectJson(c.area);
            } else if constexpr (std::is_same_v<Type, SetGrowingZoneCropCommand>) {
                j["zone"] = c.zone;
                j["crop"] = c.crop;
            } else if constexpr (std::is_same_v<Type, AddTalentXpCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["amount"] = c.amount;
            } else if constexpr (std::is_same_v<Type, CarryWeaponCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["item"] = entt::to_integral(c.item);
            } else if constexpr (std::is_same_v<Type, SetNarratorCommand>) {
                j["narrator"] = c.narrator;
                j["difficulty"] = c.difficulty;
            } else if constexpr (std::is_same_v<Type, FireIncidentCommand>) {
                j["incident"] = c.incident;
                j["points"] = c.points;
            } else if constexpr (std::is_same_v<Type, SwitchWeaponCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["slot"] = static_cast<int>(c.slot);
            } else if constexpr (std::is_same_v<Type, IgniteCommand>) {
                j["cell"] = {c.cell.x, c.cell.y, c.cell.level};
                j["size"] = c.size;
                j["pawn"] = entt::to_integral(c.pawn);
                j["force"] = c.force;
            } else if constexpr (std::is_same_v<Type, SetHomeAreaCommand>) {
                j["area"] = replayRectJson(c.area);
                j["add"] = c.add;
            } else if constexpr (std::is_same_v<Type, SetMedicalCareCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["care"] = c.care;
            } else if constexpr (std::is_same_v<Type, SetPrisonerModeCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["mode"] = c.mode;
            } else if constexpr (std::is_same_v<Type, SetPrisonBedCommand>) {
                j["bed"] = entt::to_integral(c.bed);
                j["on"] = c.on;
            } else if constexpr (std::is_same_v<Type, PlaceUpperFloorCommand>) {
                j["area"] = replayRectJson(c.area);
                j["remove"] = c.remove;
            } else if constexpr (std::is_same_v<Type, StartMentalBreakCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
                j["breakDef"] = c.breakDef;
            } else if constexpr (std::is_same_v<Type, EndMentalBreakCommand>) {
                j["pawn"] = entt::to_integral(c.pawn);
            } else {
                static_assert(std::is_same_v<Type, QuestCommand>, "write every command type");
                j["action"] = c.action;
                j["quest"] = c.quest;
                j["kind"] = c.kind;
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


// "kind": "Mine", "CutPlant" or "HarvestPlant"; "None" (cancelling only) means every kind.
bool replayReadDesignation(ReplayReader& r, const ReplayJson& j, const std::string& where, DesignationKind& out) {
    std::string name = designationKindName(out);
    if (!r.text(j, "kind", name, where)) {
        return false;
    }
    for (const DesignationKind k : {DesignationKind::None, DesignationKind::Mine, DesignationKind::CutPlant, DesignationKind::HarvestPlant}) {
        if (name == designationKindName(k)) {
            out = k;
            return true;
        }
    }
    return r.fail(where + ".kind must be None, Mine, CutPlant or HarvestPlant");
}

// "mode": "Count" (the default), "UntilHave" or "Forever"
bool replayReadBillMode(ReplayReader& r, const ReplayJson& j, const std::string& where, BillMode& out) {
    std::string name = billModeName(BillMode::Count);
    if (!r.text(j, "mode", name, where)) {
        return false;
    }
    for (const BillMode m : {BillMode::Count, BillMode::UntilHave, BillMode::Forever}) {
        if (name == billModeName(m)) {
            out = m;
            return true;
        }
    }
    return r.fail(where + ".mode must be Count, UntilHave or Forever");
}

bool replayReadCellAndRot(ReplayReader& r, const ReplayJson& j, const std::string& where, CellPos& cellOut, Rot4& rotOut) {
    std::string rot = kReplayRotNames[0];
    if (!r.text(j, "rot", rot, where)) {
        return false;
    }
    const auto cell = j.find("cell");
    if (cell == j.end() || !cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() ||
        !(*cell)[1].is_number_integer() || !(*cell)[2].is_number_integer()) {
        return r.fail(where + ".cell must be [x, y, level]");
    }
    cellOut = CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()};
    for (int i = 0; i < 4; ++i) {
        if (rot == kReplayRotNames[i]) {
            rotOut = static_cast<Rot4>(i);
            return true;
        }
    }
    return r.fail(where + ".rot must be North, East, South or West");
}

// A cell as [x, y, level]; left alone when the key is absent.
bool replayCell(ReplayReader& r, const ReplayJson& j, const char* key, CellPos& out, const std::string& where) {
    const auto cell = j.find(key);
    if (cell == j.end()) {
        return true;
    }
    if (!cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() || !(*cell)[1].is_number_integer() ||
        !(*cell)[2].is_number_integer()) {
        return r.fail(where + "." + key + " must be [x, y, level]");
    }
    out = CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()};
    return true;
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
    } else if (type == "SetLineage") {
        SetLineageCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "lineage", c.lineage, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
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
    } else if (type == "PlaceBlueprint") {
        PlaceBlueprintCommand c;
        if (!r.text(j, "def", c.def, where) || !r.text(j, "stuff", c.stuff, where) ||
            !replayReadCellAndRot(r, j, where, c.cell, c.rot)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "CancelBuild" || type == "Deconstruct") {
        uint32_t thing = entt::to_integral(kNoEntity);
        if (!r.integer(j, "thing", thing, where)) {
            return false;
        }
        if (type == "CancelBuild") {
            out.command = CancelBuildCommand{static_cast<Entity>(thing)};
        } else {
            out.command = DeconstructCommand{static_cast<Entity>(thing)};
        }
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
    } else if (type == "Designate") {
        DesignateCommand c;
        if (!replayReadRect(r, j, where, c.area) || !replayReadDesignation(r, j, where, c.kind)) {
            return false;
        }
        if (c.kind == DesignationKind::None) {
            return r.fail(where + ".kind must be Mine, CutPlant or HarvestPlant");
        }
        out.command = c;
    } else if (type == "CancelDesignations") {
        CancelDesignationsCommand c;
        if (!replayReadRect(r, j, where, c.area) || !replayReadDesignation(r, j, where, c.kind)) {
            return false;
        }
        out.command = c;
    } else if (type == "Draft") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        DraftCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.flag(j, "drafted", c.drafted, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = c;
    } else if (type == "MoveOrder") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        const auto cell = j.find("cell");
        if (cell == j.end() || !cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() ||
            !(*cell)[1].is_number_integer() || !(*cell)[2].is_number_integer()) {
            return r.fail(where + ".cell must be [x, y, level]");
        }
        out.command = MoveOrderCommand{static_cast<Entity>(pawn),
                                       CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()}};
    } else if (type == "AttackOrder") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t target = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "target", target, where)) {
            return false;
        }
        out.command = AttackOrderCommand{static_cast<Entity>(pawn), static_cast<Entity>(target)};
    } else if (type == "SetFaction") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        SetFactionCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "faction", c.faction, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = c;
    } else if (type == "Reload") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        out.command = ReloadCommand{static_cast<Entity>(pawn)};
    } else if (type == "AddBill") {
        AddBillCommand c;
        uint32_t building = entt::to_integral(kNoEntity);
        if (!r.integer(j, "building", building, where) || !r.text(j, "recipe", c.recipe, where) ||
            !replayReadBillMode(r, j, where, c.mode) || !r.integer(j, "count", c.count, where)) {
            return false;
        }
        c.building = static_cast<Entity>(building);
        out.command = std::move(c);
    } else if (type == "RemoveBill") {
        RemoveBillCommand c;
        uint32_t building = entt::to_integral(kNoEntity);
        if (!r.integer(j, "building", building, where) || !r.integer(j, "bill", c.bill, where)) {
            return false;
        }
        c.building = static_cast<Entity>(building);
        out.command = c;
    } else if (type == "SetBill") {
        SetBillCommand c;
        uint32_t building = entt::to_integral(kNoEntity);
        if (!r.integer(j, "building", building, where) || !r.integer(j, "bill", c.bill, where) ||
            !replayReadBillMode(r, j, where, c.mode) || !r.integer(j, "count", c.count, where) ||
            !r.flag(j, "suspended", c.suspended, where)) {
            return false;
        }
        c.building = static_cast<Entity>(building);
        out.command = c;
    } else if (type == "SetBillOptions") {
        SetBillOptionsCommand c;
        uint32_t building = entt::to_integral(kNoEntity);
        if (!r.integer(j, "building", building, where) || !r.integer(j, "bill", c.bill, where) ||
            !r.integer(j, "skillMin", c.skillMin, where) || !r.integer(j, "skillMax", c.skillMax, where) ||
            !r.number(j, "radius", c.radius, where) || !r.integer(j, "storeZone", c.storeZone, where) ||
            !r.integer(j, "unpauseAt", c.unpauseAt, where)) {
            return false;
        }
        const auto list = j.find("disallowed");
        if (list != j.end()) {
            if (!list->is_array()) {
                return r.fail(where + ".disallowed must be a list of def names");
            }
            for (const ReplayJson& name : *list) {
                if (!name.is_string()) {
                    return r.fail(where + ".disallowed must be a list of def names");
                }
                c.disallowed.push_back(name.get<std::string>());
            }
        }
        c.building = static_cast<Entity>(building);
        out.command = std::move(c);
    } else if (type == "MoveBill") {
        MoveBillCommand c;
        uint32_t building = entt::to_integral(kNoEntity);
        if (!r.integer(j, "building", building, where) || !r.integer(j, "bill", c.bill, where) ||
            !r.integer(j, "index", c.index, where)) {
            return false;
        }
        c.building = static_cast<Entity>(building);
        out.command = c;
    } else if (type == "SetRefuel") {
        SetRefuelCommand c;
        uint32_t building = entt::to_integral(kNoEntity);
        if (!r.integer(j, "building", building, where) || !r.flag(j, "autoRefuel", c.autoRefuel, where) ||
            !r.flag(j, "fillNow", c.fillNow, where)) {
            return false;
        }
        c.building = static_cast<Entity>(building);
        out.command = c;
    } else if (type == "HandsOn") {
        HandsOnCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t thing = entt::to_integral(kNoEntity);
        std::string mode = "Work";
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "thing", thing, where) || !r.text(j, "mode", mode, where)) {
            return false;
        }
        if (mode == "Work") {
            c.mode = HandsOnMode::Work;
        } else if (mode == "PutDown") {
            c.mode = HandsOnMode::PutDown;
        } else if (mode == "Stop") {
            c.mode = HandsOnMode::Stop;
        } else {
            return r.fail(where + ".mode must be Work, PutDown or Stop");
        }
        const auto cell = j.find("cell");
        if (cell == j.end() || !cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() ||
            !(*cell)[1].is_number_integer() || !(*cell)[2].is_number_integer()) {
            return r.fail(where + ".cell must be [x, y, level]");
        }
        c.pawn = static_cast<Entity>(pawn);
        c.thing = static_cast<Entity>(thing);
        c.cell = CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()};
        out.command = c;
    } else if (type == "Flick") {
        uint32_t thing = entt::to_integral(kNoEntity);
        FlickCommand c;
        if (!r.integer(j, "thing", thing, where) || !r.flag(j, "on", c.on, where)) {
            return false;
        }
        c.thing = static_cast<Entity>(thing);
        out.command = c;
    } else if (type == "SetOutfit") {
        SetOutfitCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "outfit", c.outfit, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = c;
    } else if (type == "EditOutfit") {
        EditOutfitCommand c;
        if (!r.integer(j, "outfit", c.outfit, where) || !r.number(j, "hitPointsMin", c.hitPointsMin, where) ||
            !r.number(j, "hitPointsMax", c.hitPointsMax, where) || !r.integer(j, "qualityMin", c.qualityMin, where) ||
            !r.integer(j, "qualityMax", c.qualityMax, where)) {
            return false;
        }
        const auto list = j.find("disallowed");
        if (list != j.end()) {
            if (!list->is_array()) {
                return r.fail(where + ".disallowed must be a list of def names");
            }
            for (const ReplayJson& name : *list) {
                if (!name.is_string()) {
                    return r.fail(where + ".disallowed must be a list of def names");
                }
                c.disallowed.push_back(name.get<std::string>());
            }
        }
        out.command = std::move(c);
    } else if (type == "AddOutfit") {
        AddOutfitCommand c;
        if (!r.text(j, "name", c.name, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "RemoveOutfit") {
        RemoveOutfitCommand c;
        if (!r.integer(j, "outfit", c.outfit, where)) {
            return false;
        }
        out.command = c;
    } else if (type == "Strip") {
        StripCommand c;
        uint32_t thing = entt::to_integral(kNoEntity);
        if (!r.integer(j, "thing", thing, where) || !r.flag(j, "strip", c.strip, where)) {
            return false;
        }
        c.thing = static_cast<Entity>(thing);
        out.command = c;
    } else if (type == "Interact") {
        InteractCommand c;
        uint32_t initiator = entt::to_integral(kNoEntity);
        uint32_t recipient = entt::to_integral(kNoEntity);
        int64_t outcome = -1;
        if (!r.integer(j, "initiator", initiator, where) || !r.integer(j, "recipient", recipient, where) ||
            !r.text(j, "interaction", c.interaction, where) || !r.integer(j, "outcome", outcome, where)) {
            return false;
        }
        c.initiator = static_cast<Entity>(initiator);
        c.recipient = static_cast<Entity>(recipient);
        c.outcome = static_cast<int>(outcome);
        out.command = std::move(c);
    } else if (type == "ShortCircuit") {
        uint32_t thing = entt::to_integral(kNoEntity);
        if (!r.integer(j, "thing", thing, where)) {
            return false;
        }
        out.command = ShortCircuitCommand{static_cast<Entity>(thing)};
    } else if (type == "Throw") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t target = entt::to_integral(kNoEntity);
        ThrowCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "item", c.item, where) || !r.integer(j, "target", target, where) ||
            !r.flag(j, "player", c.player, where)) {
            return false;
        }
        const auto point = j.find("point");
        if (point != j.end()) {
            if (!point->is_array() || point->size() != 2 || !(*point)[0].is_number() || !(*point)[1].is_number()) {
                return r.fail(where + ".point must be [x, y]");
            }
            c.x = (*point)[0].get<float>();
            c.y = (*point)[1].get<float>();
        }
        c.pawn = static_cast<Entity>(pawn);
        c.target = static_cast<Entity>(target);
        out.command = c;
    } else if (type == "Explode") {
        ExplodeCommand c;
        if (!r.number(j, "radius", c.radius, where) || !r.number(j, "damage", c.damage, where) ||
            !r.text(j, "damageType", c.damageType, where) || !r.number(j, "penetration", c.penetration, where)) {
            return false;
        }
        const auto point = j.find("point");
        if (point == j.end() || !point->is_array() || point->size() != 2 || !(*point)[0].is_number() || !(*point)[1].is_number()) {
            return r.fail(where + ".point must be [x, y]");
        }
        c.x = (*point)[0].get<float>();
        c.y = (*point)[1].get<float>();
        out.command = c;
    } else if (type == "DodgeRoll") {
        DodgeRollCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        for (const auto& [key, field] : {std::pair<const char*, float*>{"dx", &c.dx}, std::pair<const char*, float*>{"dy", &c.dy}}) {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number()) {
                return r.fail(where + "." + key + " must be a number");
            }
            *field = static_cast<float>(it->get<double>());
        }
        out.command = c;
    } else if (type == "SetResearch" || type == "UnlockResearch") {
        std::string project;
        if (!r.text(j, "project", project, where)) {
            return false;
        }
        if (type == "SetResearch") {
            out.command = SetResearchCommand{project};
        } else {
            out.command = UnlockResearchCommand{project};
        }
    } else if (type == "MarkAnimal") {
        MarkAnimalCommand c;
        uint32_t animal = entt::to_integral(kNoEntity);
        if (!r.integer(j, "animal", animal, where) || !r.text(j, "mark", c.mark, where) || !r.flag(j, "on", c.on, where)) {
            return false;
        }
        c.animal = static_cast<Entity>(animal);
        out.command = std::move(c);
    } else if (type == "SetTraining") {
        SetTrainingCommand c;
        uint32_t animal = entt::to_integral(kNoEntity);
        if (!r.integer(j, "animal", animal, where) || !r.text(j, "trainable", c.trainable, where) ||
            !r.flag(j, "wanted", c.wanted, where)) {
            return false;
        }
        c.animal = static_cast<Entity>(animal);
        out.command = std::move(c);
    } else if (type == "MarkAnimalsInArea") {
        MarkAnimalsInAreaCommand c;
        if (!replayReadRect(r, j, where, c.area) || !r.text(j, "mark", c.mark, where) || !r.flag(j, "on", c.on, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "SetAnimalName") {
        SetAnimalNameCommand c;
        uint32_t animal = entt::to_integral(kNoEntity);
        if (!r.integer(j, "animal", animal, where) || !r.text(j, "name", c.name, where)) {
            return false;
        }
        c.animal = static_cast<Entity>(animal);
        out.command = std::move(c);
    } else if (type == "CreateAnimalZone") {
        CreateAnimalZoneCommand c;
        if (!replayReadRect(r, j, where, c.area)) {
            return false;
        }
        out.command = c;
    } else if (type == "RemoveAnimalZoneCells") {
        RemoveAnimalZoneCellsCommand c;
        if (!replayReadRect(r, j, where, c.area)) {
            return false;
        }
        out.command = c;
    } else if (type == "AssignAnimalZone") {
        AssignAnimalZoneCommand c;
        uint32_t animal = entt::to_integral(kNoEntity);
        if (!r.integer(j, "animal", animal, where) || !r.integer(j, "zone", c.zone, where)) {
            return false;
        }
        c.animal = static_cast<Entity>(animal);
        out.command = c;
    } else if (type == "SetBreeding") {
        SetBreedingCommand c;
        uint32_t animal = entt::to_integral(kNoEntity);
        if (!r.integer(j, "animal", animal, where) || !r.integer(j, "zone", c.zone, where) || !r.flag(j, "allow", c.allow, where)) {
            return false;
        }
        c.animal = static_cast<Entity>(animal);
        out.command = c;
    } else if (type == "SpendTalentPoint") {
        SpendTalentPointCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "talent", c.talent, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "CreateGrowingZone") {
        CreateGrowingZoneCommand c;
        if (!replayReadRect(r, j, where, c.area) || !r.text(j, "crop", c.crop, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "RemoveGrowingZoneCells") {
        RemoveGrowingZoneCellsCommand c;
        if (!replayReadRect(r, j, where, c.area)) {
            return false;
        }
        out.command = c;
    } else if (type == "SetNarrator") {
        SetNarratorCommand c;
        if (!r.text(j, "narrator", c.narrator, where) || !r.text(j, "difficulty", c.difficulty, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "FireIncident") {
        FireIncidentCommand c;
        if (!r.text(j, "incident", c.incident, where) || !r.number(j, "points", c.points, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "SetGrowingZoneCrop") {
        SetGrowingZoneCropCommand c;
        if (!r.integer(j, "zone", c.zone, where) || !r.text(j, "crop", c.crop, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "RearmTrap") {
        RearmTrapCommand c;
        uint32_t trap = entt::to_integral(kNoEntity);
        if (!r.integer(j, "trap", trap, where)) {
            return false;
        }
        c.trap = static_cast<Entity>(trap);
        out.command = std::move(c);
    } else if (type == "CarryWeapon") {
        CarryWeaponCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t item = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "item", item, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        c.item = static_cast<Entity>(item);
        out.command = std::move(c);
    } else if (type == "SwitchWeapon") {
        SwitchWeaponCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t slot = 0;
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "slot", slot, where)) {
            return false;
        }
        if (slot >= static_cast<uint32_t>(kWeaponSlotCount)) {
            return r.fail(where + ".slot must be 0 (Primary), 1 (Sidearm) or 2 (Melee)");
        }
        c.pawn = static_cast<Entity>(pawn);
        c.slot = static_cast<WeaponSlot>(slot);
        out.command = std::move(c);
    } else if (type == "AddTalentXp") {
        AddTalentXpCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "amount", c.amount, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "SetMedicalCare") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        SetMedicalCareCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "care", c.care, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "SetPrisonerMode") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        SetPrisonerModeCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "mode", c.mode, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "SetPrisonBed") {
        uint32_t bed = entt::to_integral(kNoEntity);
        SetPrisonBedCommand c;
        if (!r.integer(j, "bed", bed, where) || !r.flag(j, "on", c.on, where)) {
            return false;
        }
        c.bed = static_cast<Entity>(bed);
        out.command = std::move(c);
    } else if (type == "PlaceUpperFloor") {
        PlaceUpperFloorCommand c;
        if (!replayReadRect(r, j, where, c.area) || !r.flag(j, "remove", c.remove, where)) {
            return false;
        }
        out.command = c;
    } else if (type == "PlaceFloor") {
        PlaceFloorCommand c;
        if (!replayReadRect(r, j, where, c.area) || !r.text(j, "floor", c.floor, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "AddFilth") {
        AddFilthCommand c;
        if (!r.text(j, "kind", c.kind, where) || !r.integer(j, "layers", c.layers, where)) {
            return false;
        }
        const auto cell = j.find("cell");
        if (cell == j.end() || !cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() ||
            !(*cell)[1].is_number_integer() || !(*cell)[2].is_number_integer()) {
            return r.fail(where + ".cell must be [x, y, level]");
        }
        c.cell = CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()};
        out.command = std::move(c);
    } else if (type == "Ignite") {
        IgniteCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.number(j, "size", c.size, where) || !r.integer(j, "pawn", pawn, where) || !r.flag(j, "force", c.force, where)) {
            return false;
        }
        const auto cell = j.find("cell");
        if (cell == j.end() || !cell->is_array() || cell->size() != 3 || !(*cell)[0].is_number_integer() ||
            !(*cell)[1].is_number_integer() || !(*cell)[2].is_number_integer()) {
            return r.fail(where + ".cell must be [x, y, level]");
        }
        c.cell = CellPos{(*cell)[0].get<int>(), (*cell)[1].get<int>(), (*cell)[2].get<int>()};
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "SetHomeArea") {
        SetHomeAreaCommand c;
        if (!replayReadRect(r, j, where, c.area) || !r.flag(j, "add", c.add, where)) {
            return false;
        }
        out.command = std::move(c);
    } else if (type == "StartRaid") {
        StartRaidCommand c;
        if (!r.text(j, "faction", c.faction, where) || !r.number(j, "points", c.points, where) ||
            !r.integer(j, "edge", c.edge, where)) {
            return false;
        }
        out.command = c;
    } else if (type == "SoundAlarm") {
        SoundAlarmCommand c;
        if (!r.flag(j, "on", c.on, where)) {
            return false;
        }
        out.command = c;
    } else if (type == "SetDefenseRole") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        SetDefenseRoleCommand c;
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "role", c.role, where) || !r.flag(j, "hasSpot", c.hasSpot, where) ||
            !replayCell(r, j, "spot", c.spot, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = c;
    } else if (type == "MarkShelter") {
        MarkShelterCommand c;
        if (!replayCell(r, j, "cell", c.cell, where) || !r.flag(j, "clear", c.clear, where)) {
            return false;
        }
        out.command = c;
    } else if (type == "Melee") {
        MeleeCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        uint32_t target = entt::to_integral(kNoEntity);
        std::string mode = "Attack";
        if (!r.integer(j, "pawn", pawn, where) || !r.integer(j, "target", target, where) || !r.text(j, "mode", mode, where)) {
            return false;
        }
        if (mode == "Attack") {
            c.mode = MeleeMode::Attack;
        } else if (mode == "Block") {
            c.mode = MeleeMode::Block;
        } else {
            return r.fail(where + ".mode must be Attack or Block");
        }
        c.pawn = static_cast<Entity>(pawn);
        c.target = static_cast<Entity>(target);
        out.command = c;
    } else if (type == "SetWeather") {
        SetWeatherCommand c;
        if (!r.text(j, "weather", c.weather, where) || !r.number(j, "hours", c.hours, where)) {
            return false;
        }
        out.command = c;
    } else if (type == "StartMentalBreak") {
        StartMentalBreakCommand c;
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where) || !r.text(j, "breakDef", c.breakDef, where)) {
            return false;
        }
        c.pawn = static_cast<Entity>(pawn);
        out.command = std::move(c);
    } else if (type == "EndMentalBreak") {
        uint32_t pawn = entt::to_integral(kNoEntity);
        if (!r.integer(j, "pawn", pawn, where)) {
            return false;
        }
        out.command = EndMentalBreakCommand{static_cast<Entity>(pawn)};
    } else if (type == "Quest") {
        QuestCommand c;
        if (!r.text(j, "action", c.action, where) || !r.integer(j, "quest", c.quest, where) || !r.text(j, "kind", c.kind, where)) {
            return false;
        }
        out.command = std::move(c);
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
    if (s.wildAnimals != 0) {
        settings["wildAnimals"] = s.wildAnimals;
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
        !r.integer(*settings, "pathBudget", s.pathBudget, where) || !r.text(*settings, "scenario", s.scenario, where) ||
        !r.integer(*settings, "wildAnimals", s.wildAnimals, where)) {
        return false;
    }
    if (s.mapSize <= 0 || s.mapSize > 4096 || s.pawns < 0 || s.items < 0 || s.checkpointEvery < 0 ||
        s.pathBudget <= 0 || s.wildAnimals < 0) {
        return r.fail(
            "settings: mapSize must be 1 to 4096; pawns, items, checkpointEvery and wildAnimals not negative; pathBudget above 0");
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

#include "ColonySim/Temperature.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr float kTempNone = -1000.0f;  // a cell that is in no enclosed room
constexpr int kTempSides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

float tempFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

// Game seconds a RimWorld "heat per second" lasts: one RimWorld second is 60 ticks, and a game
// hour is 2,500 ticks. So a heater's 21 per second is 21 * (2500 / 60) = 875 energy per game hour.
constexpr double kTempEnergyPerGameSecond = kRimWorldTicksPerHour / 60.0 / static_cast<double>(kSecondsPerHour);

}  // namespace

TemperatureNumbers TemperatureNumbers::read(const DefDatabase& defs) {
    TemperatureNumbers n;
    const Def* def = defs.find("Rules", "TemperatureRules");
    if (def == nullptr) {
        return n;
    }
    n.intervalSeconds = std::max(1, static_cast<int>(def->integer("intervalSeconds", n.intervalSeconds)));
    n.wall = tempFloat(*def, "conductancePerHour.wall", n.wall);
    n.doorClosed = tempFloat(*def, "conductancePerHour.doorClosed", n.doorClosed);
    n.doorOpen = tempFloat(*def, "conductancePerHour.doorOpen", n.doorOpen);
    n.vent = tempFloat(*def, "conductancePerHour.vent", n.vent);
    n.roofThin = tempFloat(*def, "conductancePerHour.roofThin", n.roofThin);
    n.roofThick = tempFloat(*def, "conductancePerHour.roofThick", n.roofThick);
    n.rock = tempFloat(*def, "conductancePerHour.rock", n.rock);
    n.safeMarginC = tempFloat(*def, "safeMarginC", n.safeMarginC);
    n.frostbiteSeverity = tempFloat(*def, "frostbiteSeverity", n.frostbiteSeverity);
    return n;
}

TemperatureSystem::TemperatureSystem(Map& map, Things& things, Rooms& rooms, HealthSystem& health,
                                     const DefDatabase& defs, uint64_t seed)
    : map_(map), things_(things), rooms_(rooms), health_(health), defs_(defs), seed_(seed),
      numbers_(TemperatureNumbers::read(defs)) {
    sourceDefs_.assign(defs.size(), SourceDef{});
    for (uint32_t i = 0; i < defs.size(); ++i) {
        const Def& d = defs.get(DefId{i});
        if (d.kind() != "Things" || d.text("category") != "Building") {
            continue;
        }
        SourceDef& s = sourceDefs_[i];
        s.pusher = d.has("heatPusher.heatPerSecond");
        s.heatPerSecond = tempFloat(d, "heatPusher.heatPerSecond", 0.0f);
        s.hasMax = d.has("heatPusher.maxTemp");
        s.maxTemp = tempFloat(d, "heatPusher.maxTemp", 0.0f);
        s.hasMin = d.has("heatPusher.minTemp");
        s.minTemp = tempFloat(d, "heatPusher.minTemp", 0.0f);
        s.isWall = d.flag("building.holdsRoof");
        s.isVent = d.flag("building.vent");
    }
    hypothermia_ = defs.idOf("Hediffs", "Hypothermia");
    heatstroke_ = defs.idOf("Hediffs", "Heatstroke");
    frostbite_ = defs.idOf("Hediffs", "Frostbite");
    if (const Def* d = defs.find("Hediffs", "Hypothermia")) {
        coldPerDay_ = tempFloat(*d, "severityPerDay.coldBelowComfortMinC", coldPerDay_);
        coldRecoverPerDay_ = -tempFloat(*d, "severityPerDay.recoverPerDay", -coldRecoverPerDay_);
    }
    if (const Def* d = defs.find("Hediffs", "Heatstroke")) {
        hotPerDay_ = tempFloat(*d, "severityPerDay.hotAboveComfortMaxC", hotPerDay_);
        hotRecoverPerDay_ = -tempFloat(*d, "severityPerDay.recoverPerDay", -hotRecoverPerDay_);
    }
    if (const Def* d = defs.find("Hediffs", "Frostbite")) {
        frostbiteBelow_ = tempFloat(*d, "appliesBelowC", frostbiteBelow_);
        frostbiteChancePerHour_ = tempFloat(*d, "chancePerHourBelow", frostbiteChancePerHour_);
    }
}

// ---- Rooms come and go ----------------------------------------------------------------------------

// When the rooms were rebuilt, every new room starts at the mean temperature its cells had (cells
// that were outdoors count as the outdoor temperature), so merging two rooms mixes them and
// splitting one keeps each half's heat.
void TemperatureSystem::syncRooms() {
    rooms_.update();
    if (rooms_.rebuilds() == seenRebuilds_) {
        return;
    }
    seenRebuilds_ = rooms_.rebuilds();
    if (cellTemp_.empty()) {
        cellTemp_.assign(static_cast<size_t>(map_.width()) * static_cast<size_t>(map_.height()), kTempNone);
    }
    const std::vector<Room>& list = rooms_.rooms();
    std::vector<float> fresh(list.size(), outdoor_);
    for (const Room& room : list) {
        if (!usesRoomTemperature(room)) {
            continue;
        }
        double sum = 0.0;
        const auto cells = rooms_.cellsOf(room);
        for (const CellPos& c : cells) {
            const float v = cellTemp_[static_cast<size_t>(map_.index(c))];
            sum += v == kTempNone ? outdoor_ : v;
        }
        if (!cells.empty()) {
            fresh[room.id] = static_cast<float>(sum / static_cast<double>(cells.size()));
        }
    }
    roomTemp_ = std::move(fresh);
    storeCells();
}

// cellTemp_ always holds the temperature of the room each cell is in now.
void TemperatureSystem::storeCells() {
    for (const int index : storedCells_) {
        cellTemp_[static_cast<size_t>(index)] = kTempNone;
    }
    storedCells_.clear();
    for (const Room& room : rooms_.rooms()) {
        if (!usesRoomTemperature(room)) {
            continue;
        }
        for (const CellPos& c : rooms_.cellsOf(room)) {
            const int index = map_.index(c);
            cellTemp_[static_cast<size_t>(index)] = roomTemp_[room.id];
            storedCells_.push_back(index);
        }
    }
}

// ---- Asking ---------------------------------------------------------------------------------------

float TemperatureSystem::roomTemperature(RoomId room) const {
    const std::vector<Room>& list = rooms_.rooms();
    if (room >= list.size() || room >= roomTemp_.size() || !usesRoomTemperature(list[room])) {
        return outdoor_;
    }
    return roomTemp_[room];
}

float TemperatureSystem::temperatureAt(CellPos cell) {
    syncRooms();
    if (!map_.inBounds(cell)) {
        return outdoor_;
    }
    if (const Room* room = rooms_.roomOf(cell)) {
        return usesRoomTemperature(*room) ? roomTemp_[room->id] : outdoor_;
    }
    if (map_.isDoor(cell)) {
        const std::vector<RoomId> beside = rooms_.roomsBesideDoor(cell);
        if (beside.empty()) {
            return outdoor_;
        }
        double sum = 0.0;
        for (const RoomId id : beside) {
            sum += roomTemperature(id);
        }
        return static_cast<float>(sum / static_cast<double>(beside.size()));
    }
    return outdoor_;
}

float TemperatureSystem::ambientOf(Entity pawn) {
    const entt::registry& reg = things_.registry();
    const Position* pos = reg.valid(pawn) ? reg.try_get<Position>(pawn) : nullptr;
    return pos != nullptr ? temperatureAt(cellOf(*pos)) : outdoor_;
}

bool TemperatureSystem::comfortRange(Entity pawn, float& min, float& max) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing>(pawn)) {
        return false;
    }
    const Def& def = defs_.get(reg.get<Thing>(pawn).def);
    if (!def.has("stats.comfyTemperatureMin") || !def.has("stats.comfyTemperatureMax")) {
        return false;
    }
    min = tempFloat(def, "stats.comfyTemperatureMin", 16.0f);
    max = tempFloat(def, "stats.comfyTemperatureMax", 26.0f);
    if (modifier_) {
        modifier_(pawn, min, max);
    }
    return true;
}

float TemperatureSystem::degreesBelowComfort(Entity pawn) {
    float min = 0.0f;
    float max = 0.0f;
    if (!comfortRange(pawn, min, max)) {
        return 0.0f;
    }
    return std::max(0.0f, min - ambientOf(pawn));
}

float TemperatureSystem::degreesAboveComfort(Entity pawn) {
    float min = 0.0f;
    float max = 0.0f;
    if (!comfortRange(pawn, min, max)) {
        return 0.0f;
    }
    return std::max(0.0f, ambientOf(pawn) - max);
}

// ---- Each game second -----------------------------------------------------------------------------

void TemperatureSystem::step(int64_t now) {
    syncRooms();
    if (now % numbers_.intervalSeconds != 0) {
        return;
    }
    exchange();
    exposePawns(now);
}

void TemperatureSystem::refreshSources() {
    if (map_.buildingChanges() == sourceChanges_) {
        return;
    }
    sourceChanges_ = map_.buildingChanges();
    sources_.clear();
    const entt::registry& reg = things_.registry();
    for (const auto [entity, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
        (void)footprint;
        if (thing.def.value < sourceDefs_.size() && sourceDefs_[thing.def.value].pusher) {
            sources_.push_back(entity);
        }
    }
    std::sort(sources_.begin(), sources_.end());
}

// The temperature on the far side of a wall, from the temperatures at the start of the interval.
float TemperatureSystem::temperatureBeyond(CellPos cell) {
    if (!map_.inBounds(cell)) {
        return outdoor_;
    }
    if (map_.terrain(cell) == Terrain::Rock) {
        return ground_;
    }
    const RoomId id = rooms_.roomAt(cell);
    if (id != kNoRoom && usesRoomTemperature(rooms_.room(id))) {
        return frozen_[id];
    }
    return outdoor_;
}

void TemperatureSystem::exchange() {
    refreshSources();
    const std::vector<Room>& list = rooms_.rooms();
    frozen_ = roomTemp_;
    const float hours = static_cast<float>(numbers_.intervalSeconds) / static_cast<float>(kSecondsPerHour);
    const entt::registry& reg = things_.registry();

    // Heat through the boundary, worked out for every room from the temperatures at the start.
    flow_.assign(list.size(), 0.0f);
    for (const Room& room : list) {
        if (!usesRoomTemperature(room)) {
            continue;
        }
        const float t = frozen_[room.id];
        double sum = 0.0;
        for (const CellPos& c : rooms_.cellsOf(room)) {
            const Roof roof = map_.roof(c);
            if (roof == Roof::Thin) {
                sum += numbers_.roofThin * (outdoor_ - t);
            } else if (roof == Roof::Thick) {
                sum += numbers_.roofThick * (ground_ - t);
            }
            for (const auto& side : kTempSides) {
                const CellPos n{c.x + side[0], c.y + side[1], c.level};
                if (!map_.inBounds(n)) {
                    sum += numbers_.wall * (outdoor_ - t);
                    continue;
                }
                if (map_.isDoor(n)) {
                    const Entity door = map_.buildingAt(n);
                    const Door* state = door != kNoEntity ? reg.try_get<Door>(door) : nullptr;
                    const float g = state != nullptr && state->open ? numbers_.doorOpen : numbers_.doorClosed;
                    sum += g * (temperatureBeyond(CellPos{n.x + side[0], n.y + side[1], n.level}) - t);
                } else if (map_.terrain(n) == Terrain::Rock) {
                    sum += numbers_.rock * (ground_ - t);
                } else if (map_.passage(n) == Passage::Blocked) {
                    const Entity building = map_.buildingAt(n);
                    if (building == kNoEntity) {
                        continue;
                    }
                    const SourceDef& s = sourceDefs_[reg.get<Thing>(building).def.value];
                    if (s.isWall) {
                        sum += (s.isVent ? numbers_.vent : numbers_.wall) *
                               (temperatureBeyond(CellPos{n.x + side[0], n.y + side[1], n.level}) - t);
                    }
                }
            }
        }
        flow_[room.id] = static_cast<float>(sum * static_cast<double>(hours) / static_cast<double>(room.cellCount));
    }

    // Then the heaters, coolers and campfires, in entity order, each held by its own limit.
    std::vector<float> now = frozen_;
    for (const Room& room : list) {
        if (usesRoomTemperature(room)) {
            now[room.id] += flow_[room.id];
        }
    }
    for (const Entity source : sources_) {
        if (running_ && !running_(source)) {
            continue;
        }
        const SourceDef& s = sourceDefs_[reg.get<Thing>(source).def.value];
        const RoomId id = roomOfSource(source);
        if (id == kNoRoom || !usesRoomTemperature(list[id])) {
            continue;  // heat pushed outdoors is lost
        }
        float& t = now[id];
        const float dt = static_cast<float>(static_cast<double>(s.heatPerSecond) * kTempEnergyPerGameSecond *
                                            static_cast<double>(numbers_.intervalSeconds) /
                                            static_cast<double>(list[id].cellCount));
        if (s.heatPerSecond > 0.0f) {
            if (!s.hasMax) {
                t += dt;
            } else if (t < s.maxTemp) {
                t = std::min(s.maxTemp, t + dt);
            }
        } else if (s.heatPerSecond < 0.0f) {
            if (!s.hasMin) {
                t += dt;
            } else if (t > s.minTemp) {
                t = std::max(s.minTemp, t + dt);
            }
        }
    }
    roomTemp_ = std::move(now);
    storeCells();
}

// The enclosed room a heat source is in: the room of one of its cells, else of a cell next to it.
RoomId TemperatureSystem::roomOfSource(Entity source) {
    const entt::registry& reg = things_.registry();
    const Footprint* footprint = reg.try_get<Footprint>(source);
    if (footprint == nullptr) {
        return kNoRoom;
    }
    const CellRect rect = footprint->rect();
    for (int y = rect.minY - 1; y <= rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX - 1; x <= rect.minX + rect.sizeX; ++x) {
            const bool corner = (x < rect.minX || x >= rect.minX + rect.sizeX) && (y < rect.minY || y >= rect.minY + rect.sizeY);
            const CellPos c{x, y, rect.level};
            if (corner || !map_.inBounds(c)) {
                continue;
            }
            const RoomId id = rooms_.roomAt(c);
            if (id != kNoRoom) {
                return id;
            }
        }
    }
    return kNoRoom;
}

// ---- Pawns ----------------------------------------------------------------------------------------

void TemperatureSystem::exposePawns(int64_t now) {
    if (!hypothermia_.valid() && !heatstroke_.valid() && !frostbite_.valid()) {
        return;
    }
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const auto [entity, thing, health, pos] : reg.view<const Thing, const Health, const Position>().each()) {
        if (thing.category == ThingCategory::Pawn && !health.dead) {
            pawns.push_back(entity);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    const float days = static_cast<float>(numbers_.intervalSeconds) / static_cast<float>(kSecondsPerDay);
    const float hours = static_cast<float>(numbers_.intervalSeconds) / static_cast<float>(kSecondsPerHour);
    for (const Entity pawn : pawns) {
        float min = 0.0f;
        float max = 0.0f;
        if (!comfortRange(pawn, min, max) || !health_.tracks(pawn)) {
            continue;
        }
        const float ambient = ambientOf(pawn);
        const float safeMin = min - numbers_.safeMarginC;
        const float safeMax = max + numbers_.safeMarginC;
        if (hypothermia_.valid()) {
            const float before = health_.environmentSeverity(pawn, hypothermia_);
            if (ambient < safeMin) {
                health_.setEnvironmentSeverity(pawn, hypothermia_, before + (safeMin - ambient) * coldPerDay_ * days);
            } else if (before > 0.0f) {
                health_.setEnvironmentSeverity(pawn, hypothermia_, before - coldRecoverPerDay_ * days);
            }
        }
        if (heatstroke_.valid()) {
            const float before = health_.environmentSeverity(pawn, heatstroke_);
            if (ambient > safeMax) {
                health_.setEnvironmentSeverity(pawn, heatstroke_, before + (ambient - safeMax) * hotPerDay_ * days);
            } else if (before > 0.0f) {
                health_.setEnvironmentSeverity(pawn, heatstroke_, before - hotRecoverPerDay_ * days);
            }
        }
        if (frostbite_.valid() && ambient < frostbiteBelow_ && health_.partsOfDef(reg.get<Thing>(pawn).def, "Finger").size() > 0) {
            Rng rng(splitMix64(seed_ ^ 0x66726F7374ULL ^ (static_cast<uint64_t>(entt::to_integral(pawn)) * 0x9E3779B97F4A7C15ULL) ^
                               static_cast<uint64_t>(now)));
            if (rng.unit() < frostbiteChancePerHour_ * hours) {
                std::vector<int> parts = health_.partsOfDef(reg.get<Thing>(pawn).def, "Finger");
                const std::vector<int> toes = health_.partsOfDef(reg.get<Thing>(pawn).def, "Toe");
                parts.insert(parts.end(), toes.begin(), toes.end());
                health_.addInjury(pawn, parts[rng.below(static_cast<uint32_t>(parts.size()))], frostbite_,
                                  numbers_.frostbiteSeverity);
            }
        }
    }
}

// ---- Saving ---------------------------------------------------------------------------------------

std::vector<RoomTemperature> TemperatureSystem::roomTemperatures() {
    syncRooms();
    std::vector<RoomTemperature> out;
    for (const Room& room : rooms_.rooms()) {
        if (usesRoomTemperature(room)) {
            out.push_back(RoomTemperature{room.anchor, roomTemp_[room.id]});
        }
    }
    std::sort(out.begin(), out.end(), [&](const RoomTemperature& a, const RoomTemperature& b) {
        return map_.index(a.anchor) < map_.index(b.anchor);
    });
    return out;
}

void TemperatureSystem::restore(const std::vector<RoomTemperature>& temperatures) {
    syncRooms();
    std::fill(roomTemp_.begin(), roomTemp_.end(), outdoor_);
    for (const RoomTemperature& saved : temperatures) {
        if (!map_.inBounds(saved.anchor)) {
            continue;
        }
        const RoomId id = rooms_.roomAt(saved.anchor);
        if (id != kNoRoom && usesRoomTemperature(rooms_.room(id))) {
            roomTemp_[id] = saved.celsius;
        }
    }
    storeCells();
}

void TemperatureSystem::hashState(StateHasher& hasher) {
    const std::vector<RoomTemperature> temperatures = roomTemperatures();
    hasher.add(static_cast<uint64_t>(temperatures.size()));
    for (const RoomTemperature& t : temperatures) {
        hasher.add(t.anchor.x);
        hasher.add(t.anchor.y);
        hasher.add(t.celsius);
    }
}

}  // namespace colony

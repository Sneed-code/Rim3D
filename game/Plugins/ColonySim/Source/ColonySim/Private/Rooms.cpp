#include "ColonySim/Rooms.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

RoomRole roomsRoleFromName(const std::string& name) {
    for (const RoomRole role : {RoomRole::Bedroom, RoomRole::Barracks, RoomRole::Kitchen, RoomRole::DiningRoom}) {
        if (name == roomRoleName(role)) {
            return role;
        }
    }
    return RoomRole::None;
}

std::vector<RoomNumbers::RoleRule> roomsDefaultRoles() {
    RoomNumbers::RoleRule barracks;
    barracks.role = RoomRole::Barracks;
    barracks.minBeds = 2;
    RoomNumbers::RoleRule bedroom;
    bedroom.role = RoomRole::Bedroom;
    bedroom.minBeds = 1;
    RoomNumbers::RoleRule kitchen;
    kitchen.role = RoomRole::Kitchen;
    kitchen.minStoves = 1;
    RoomNumbers::RoleRule dining;
    dining.role = RoomRole::DiningRoom;
    dining.minTables = 1;
    return {barracks, bedroom, kitchen, dining};
}

constexpr int kRoomsNeighbours[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

}  // namespace

const char* roomRoleName(RoomRole role) {
    switch (role) {
        case RoomRole::None: return "None";
        case RoomRole::Bedroom: return "Bedroom";
        case RoomRole::Barracks: return "Barracks";
        case RoomRole::Kitchen: return "Kitchen";
        case RoomRole::DiningRoom: return "DiningRoom";
    }
    return "?";
}

RoomNumbers RoomNumbers::read(const DefDatabase& defs) {
    RoomNumbers n;
    n.roles = roomsDefaultRoles();
    const Def* def = defs.find("Rules", "RoomRules");
    if (def == nullptr) {
        return n;
    }
    n.roofSupportRadius = static_cast<float>(def->number("roofSupportRadius", n.roofSupportRadius));
    n.hugeRoomCells = std::max(1, static_cast<int>(def->integer("hugeRoomCells", n.hugeRoomCells)));
    if (def->count("roleOrder") > 0) {
        n.roles.clear();
        for (size_t i = 0; i < def->count("roleOrder"); ++i) {
            const std::string name = def->textAt("roleOrder", i);
            RoleRule rule;
            rule.role = roomsRoleFromName(name);
            if (rule.role == RoomRole::None) {
                continue;
            }
            const std::string base = "roles." + name + ".";
            rule.minBeds = static_cast<int>(def->integer((base + "minBeds").c_str(), 0));
            rule.minTables = static_cast<int>(def->integer((base + "minTables").c_str(), 0));
            rule.minChairs = static_cast<int>(def->integer((base + "minChairs").c_str(), 0));
            rule.minStoves = static_cast<int>(def->integer((base + "minStoves").c_str(), 0));
            n.roles.push_back(rule);
        }
    }
    return n;
}

Rooms::Rooms(Map& map, Things& things, const DefDatabase& defs)
    : map_(map), things_(things), numbers_(RoomNumbers::read(defs)) {
    isBed_.assign(defs.size(), 0);
    isTable_.assign(defs.size(), 0);
    isChair_.assign(defs.size(), 0);
    isStove_.assign(defs.size(), 0);
    holdsRoof_.assign(defs.size(), 0);
    for (uint32_t i = 0; i < defs.size(); ++i) {
        const Def& d = defs.get(DefId{i});
        if (d.kind() != "Things" || d.text("category") != "Building") {
            continue;
        }
        isBed_[i] = d.flag("building.bed") ? 1 : 0;
        isTable_[i] = d.flag("building.isTable") ? 1 : 0;
        isChair_[i] = d.flag("building.isSittable") ? 1 : 0;
        isStove_[i] = d.flag("building.isStove") ? 1 : 0;
        holdsRoof_[i] = d.flag("building.holdsRoof") ? 1 : 0;
    }
    const int reach = static_cast<int>(std::floor(numbers_.roofSupportRadius));
    const float r2 = numbers_.roofSupportRadius * numbers_.roofSupportRadius;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            if ((dx != 0 || dy != 0) && static_cast<float>(dx * dx + dy * dy) <= r2) {
                supportOffsets_.emplace_back(dx, dy);
            }
        }
    }
    std::sort(supportOffsets_.begin(), supportOffsets_.end(), [](const auto& a, const auto& b) {
        const int da = a.first * a.first + a.second * a.second;
        const int db = b.first * b.first + b.second * b.second;
        return da != db ? da < db : (a.second != b.second ? a.second < b.second : a.first < b.first);
    });
}

void Rooms::update() {
    Regions& regions = map_.regions();
    regions.update();
    const uint64_t regionChanges = regions.changeCount();
    const uint64_t buildingChanges = map_.buildingChanges();
    if (built_ && regionChanges == regionChanges_ && buildingChanges == buildingChanges_) {
        return;
    }
    const bool structural = !built_ || regionChanges != regionChanges_;
    built_ = true;
    regionChanges_ = regionChanges;
    buildingChanges_ = buildingChanges;
    if (structural) {
        rebuild();
    } else {
        scanContents();
    }
}

void Rooms::rebuild() {
    ++rebuilds_;
    const Regions& regions = map_.regions();
    const size_t limit = regions.idLimit();
    rooms_.clear();
    roomOfRegion_.assign(limit, kNoRoom);
    roomRegions_.clear();
    firstRegion_.clear();
    cells_.clear();

    // Rooms: regions joined across every border that is not a door.
    for (RegionId id = 0; id < limit; ++id) {
        const Region& start = regions.region(id);
        if (!start.alive || start.kind != RegionKind::Normal || roomOfRegion_[id] != kNoRoom) {
            continue;
        }
        Room room;
        room.id = static_cast<RoomId>(rooms_.size());
        const size_t first = roomRegions_.size();
        firstRegion_.push_back(static_cast<uint32_t>(first));
        roomRegions_.push_back(id);
        roomOfRegion_[id] = room.id;
        int minX = start.bounds.minX, minY = start.bounds.minY;
        int maxX = minX + start.bounds.sizeX - 1, maxY = minY + start.bounds.sizeY - 1;
        room.anchor = start.anchor;
        room.bounds.level = start.bounds.level;
        for (size_t head = first; head < roomRegions_.size(); ++head) {
            const Region& r = regions.region(roomRegions_[head]);
            room.cellCount += r.cellCount;
            ++room.regionCount;
            minX = std::min(minX, r.bounds.minX);
            minY = std::min(minY, r.bounds.minY);
            maxX = std::max(maxX, r.bounds.minX + r.bounds.sizeX - 1);
            maxY = std::max(maxY, r.bounds.minY + r.bounds.sizeY - 1);
            if (r.anchor.y < room.anchor.y || (r.anchor.y == room.anchor.y && r.anchor.x < room.anchor.x)) {
                room.anchor = r.anchor;
            }
            for (const RegionLink& link : r.links) {
                if (regions.region(link.other).kind == RegionKind::Normal && roomOfRegion_[link.other] == kNoRoom) {
                    roomOfRegion_[link.other] = room.id;
                    roomRegions_.push_back(link.other);
                }
            }
        }
        room.bounds = CellRect{minX, minY, maxX - minX + 1, maxY - minY + 1, room.bounds.level};
        room.touchesEdge = minX == 0 || minY == 0 || maxX == map_.width() - 1 || maxY == map_.height() - 1;
        room.huge = room.cellCount > numbers_.hugeRoomCells;
        rooms_.push_back(room);
    }
    firstRegion_.push_back(static_cast<uint32_t>(roomRegions_.size()));

    // The cells of the rooms that can be roofed.
    for (Room& room : rooms_) {
        if (!room.enclosed()) {
            continue;
        }
        room.firstCell = static_cast<uint32_t>(cells_.size());
        for (uint32_t k = firstRegion_[room.id]; k < firstRegion_[room.id + 1]; ++k) {
            const RegionId rid = roomRegions_[k];
            const CellRect& b = regions.region(rid).bounds;
            for (int y = b.minY; y < b.minY + b.sizeY; ++y) {
                for (int x = b.minX; x < b.minX + b.sizeX; ++x) {
                    const CellPos c{x, y, b.level};
                    if (regions.regionAtNow(c) == rid) {
                        cells_.push_back(c);
                    }
                }
            }
        }
        room.listedCells = static_cast<uint32_t>(cells_.size()) - room.firstCell;
    }

    roof();
    scanContents();
}

bool Rooms::roofHolderAt(int x, int y) const {
    const CellPos c{x, y, 0};
    if (map_.terrain(c) == Terrain::Rock) {
        return true;
    }
    const Entity building = map_.buildingAt(c);
    return building != kNoEntity && holdsRoof_[things_.registry().get<Thing>(building).def.value] != 0;
}

bool Rooms::supported(int x, int y) const {
    for (const auto& [dx, dy] : supportOffsets_) {
        const int nx = x + dx;
        const int ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < map_.width() && ny < map_.height() && roofHolderAt(nx, ny)) {
            return true;
        }
    }
    return false;
}

// The thin roof follows the walls: it is worked out afresh from the enclosed rooms every time they
// change, so opening a wall takes the roof off (RimWorld's would collapse, which comes later).
void Rooms::roof() {
    for (const CellPos& c : thinCells_) {
        Roof& slot = map_.roof_[static_cast<size_t>(map_.index(c))];
        if (slot == Roof::Thin) {
            slot = Roof::None;
        }
    }
    std::vector<CellPos> fresh;
    const auto mark = [&](int x, int y) {
        const CellPos c{x, y, 0};
        Roof& slot = map_.roof_[static_cast<size_t>(map_.index(c))];
        if (slot != Roof::None) {
            return false;
        }
        slot = Roof::Thin;
        fresh.push_back(c);
        return true;
    };
    for (const Room& room : rooms_) {
        if (!room.enclosed()) {
            continue;
        }
        for (const CellPos& c : cellsOf(room)) {
            if (!supported(c.x, c.y) || !mark(c.x, c.y)) {
                continue;
            }
            // The walls, doors and furniture round a roofed cell are under the roof too.
            for (const auto& n : kRoomsNeighbours) {
                const int nx = c.x + n[0];
                const int ny = c.y + n[1];
                if (map_.inBounds(CellPos{nx, ny, 0}) && map_.passage(CellPos{nx, ny, 0}) != Passage::Open) {
                    mark(nx, ny);
                }
            }
        }
    }
    std::sort(fresh.begin(), fresh.end(), [&](const CellPos& a, const CellPos& b) { return map_.index(a) < map_.index(b); });
    if (fresh != thinCells_) {
        ++roofChanges_;
        thinCells_ = std::move(fresh);
    }
}

// What is in each room and the role that gives it. Only enclosed rooms are looked at.
void Rooms::scanContents() {
    const entt::registry& reg = things_.registry();
    std::vector<Entity> seen;
    for (Room& room : rooms_) {
        room.beds = room.tables = room.chairs = room.stoves = 0;
        room.role = RoomRole::None;
        if (!room.enclosed()) {
            room.openRoofCount = room.cellCount;
            continue;
        }
        room.openRoofCount = 0;
        seen.clear();
        for (const CellPos& c : cellsOf(room)) {
            if (map_.roof(c) == Roof::None) {
                ++room.openRoofCount;
            }
            const Entity building = map_.buildingAt(c);
            if (building != kNoEntity) {
                seen.push_back(building);
            }
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        for (const Entity building : seen) {
            const uint32_t def = reg.get<Thing>(building).def.value;
            room.beds += isBed_[def];
            room.tables += isTable_[def];
            room.chairs += isChair_[def];
            room.stoves += isStove_[def];
        }
        if (room.outdoors()) {
            continue;
        }
        for (const RoomNumbers::RoleRule& rule : numbers_.roles) {
            if (room.beds >= rule.minBeds && room.tables >= rule.minTables && room.chairs >= rule.minChairs &&
                room.stoves >= rule.minStoves) {
                room.role = rule.role;
                break;
            }
        }
    }
}

std::span<const CellPos> Rooms::cellsOf(const Room& room) const {
    return std::span<const CellPos>(cells_.data() + room.firstCell, room.listedCells);
}

RoomId Rooms::roomAt(CellPos c) {
    update();
    const RegionId region = map_.regions().regionAtNow(c);
    return region != kNoRegion && region < roomOfRegion_.size() ? roomOfRegion_[region] : kNoRoom;
}

const Room* Rooms::roomOf(CellPos c) {
    const RoomId id = roomAt(c);
    return id == kNoRoom ? nullptr : &rooms_[id];
}

std::vector<RoomId> Rooms::roomsBesideDoor(CellPos door) {
    std::vector<RoomId> out;
    for (const auto& n : kRoomsNeighbours) {
        if (n[0] != 0 && n[1] != 0) {
            continue;  // the four sides only
        }
        const RoomId id = roomAt(CellPos{door.x + n[0], door.y + n[1], door.level});
        if (id != kNoRoom) {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

RoomRole Rooms::roleAt(CellPos c) {
    const Room* room = roomOf(c);
    return room != nullptr ? room->role : RoomRole::None;
}

bool Rooms::indoors(CellPos c) {
    const Room* room = roomOf(c);
    if (room != nullptr) {
        return !room->outdoors();
    }
    return map_.inBounds(c) && map_.roof(c) != Roof::None;
}

}  // namespace colony

#include "ColonySim/Areas.h"

#include "ColonySim/Hash.h"

#include <algorithm>

namespace colony {

namespace {

bool areasCellBefore(const CellPos& a, const CellPos& b) {
    return a.level != b.level ? a.level < b.level : a.y != b.y ? a.y < b.y : a.x < b.x;
}

}  // namespace

uint64_t Areas::key(CellPos c) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(c.level)) << 48) | (static_cast<uint64_t>(static_cast<uint32_t>(c.y) & 0xFFFFFF) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(c.x) & 0xFFFFFF);
}

Area* Areas::findMutable(AreaId id) {
    for (Area& area : areas_) {
        if (area.id == id) {
            return &area;
        }
    }
    return nullptr;
}

const Area* Areas::find(AreaId id) const {
    for (const Area& area : areas_) {
        if (area.id == id) {
            return &area;
        }
    }
    return nullptr;
}

const std::set<uint64_t>* Areas::cellsOf(AreaId id) const {
    for (size_t i = 0; i < areas_.size(); ++i) {
        if (areas_[i].id == id) {
            return &members_[i];
        }
    }
    return nullptr;
}

AreaId Areas::create(const std::string& name) {
    Area area;
    area.id = nextId_++;
    area.name = name.empty() ? "Area " + std::to_string(area.id) : name;
    areas_.push_back(std::move(area));
    members_.emplace_back();
    ++changeCount_;
    return areas_.back().id;
}

int Areas::paint(AreaId id, const CellRect& rect, bool add) {
    Area* area = findMutable(id);
    if (area == nullptr) {
        return 0;
    }
    std::set<uint64_t>& set = members_[static_cast<size_t>(area - areas_.data())];
    int changed = 0;
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            if (x < 0 || y < 0 || rect.level < 0) {
                continue;
            }
            if (add ? set.insert(key(c)).second : set.erase(key(c)) > 0) {
                ++changed;
            }
        }
    }
    if (changed > 0) {
        ++changeCount_;
    }
    return changed;
}

bool Areas::remove(AreaId id) {
    for (size_t i = 0; i < areas_.size(); ++i) {
        if (areas_[i].id == id) {
            areas_.erase(areas_.begin() + static_cast<std::ptrdiff_t>(i));
            members_.erase(members_.begin() + static_cast<std::ptrdiff_t>(i));
            assigned_.erase(std::remove_if(assigned_.begin(), assigned_.end(), [&](const auto& p) { return p.second == id; }),
                            assigned_.end());
            ++changeCount_;
            return true;
        }
    }
    return false;
}

bool Areas::assign(Entity pawn, AreaId id, std::string& error) {
    if (pawn == kNoEntity) {
        error = "no such pawn";
        return false;
    }
    if (id != kNoArea && find(id) == nullptr) {
        error = "no such area";
        return false;
    }
    assigned_.erase(std::remove_if(assigned_.begin(), assigned_.end(), [&](const auto& p) { return p.first == pawn; }), assigned_.end());
    if (id != kNoArea) {
        assigned_.emplace_back(pawn, id);
        std::sort(assigned_.begin(), assigned_.end());
    }
    ++changeCount_;
    return true;
}

AreaId Areas::areaOf(Entity pawn) const {
    for (const auto& p : assigned_) {
        if (p.first == pawn) {
            return p.second;
        }
    }
    return kNoArea;
}

bool Areas::contains(AreaId id, CellPos cell) const {
    const std::set<uint64_t>* set = cellsOf(id);
    return set != nullptr && set->count(key(cell)) > 0;
}

bool Areas::allows(Entity pawn, CellPos cell) const {
    if (assigned_.empty()) {
        return true;
    }
    const AreaId id = areaOf(pawn);
    return id == kNoArea || contains(id, cell);
}

std::vector<CellPos> Areas::cellsIn(AreaId id) const {
    std::vector<CellPos> cells;
    if (const std::set<uint64_t>* set = cellsOf(id)) {
        for (const uint64_t k : *set) {
            cells.push_back(CellPos{static_cast<int>(k & 0xFFFFFF), static_cast<int>((k >> 24) & 0xFFFFFF), static_cast<int>(k >> 48)});
        }
        std::sort(cells.begin(), cells.end(), areasCellBefore);
    }
    return cells;
}

AreasSaved Areas::saved() const {
    AreasSaved out;
    out.nextId = nextId_;
    out.areas = areas_;
    for (size_t i = 0; i < areas_.size(); ++i) {
        out.areas[i].cells = cellsIn(areas_[i].id);
    }
    out.assigned = assigned_;
    return out;
}

bool Areas::restore(const AreasSaved& saved, std::string& error) {
    areas_.clear();
    members_.clear();
    assigned_.clear();
    AreaId last = 0;
    for (const Area& area : saved.areas) {
        if (area.id <= last || area.id >= saved.nextId) {
            error = "the save file has areas with bad ids";
            return false;
        }
        last = area.id;
        areas_.push_back(area);
        members_.emplace_back();
        for (const CellPos& c : area.cells) {
            members_.back().insert(key(c));
        }
    }
    for (const auto& p : saved.assigned) {
        if (find(p.second) == nullptr) {
            error = "the save file limits a pawn to an area that is not there";
            return false;
        }
        assigned_.push_back(p);
    }
    std::sort(assigned_.begin(), assigned_.end());
    nextId_ = saved.nextId;
    ++changeCount_;
    return true;
}

void Areas::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(nextId_));
    h.add(static_cast<uint64_t>(areas_.size()));
    for (size_t i = 0; i < areas_.size(); ++i) {
        h.add(static_cast<uint64_t>(areas_[i].id));
        h.add(static_cast<uint64_t>(members_[i].size()));
        for (const uint64_t k : members_[i]) {
            h.add(k);
        }
    }
    h.add(static_cast<uint64_t>(assigned_.size()));
    for (const auto& p : assigned_) {
        h.add(static_cast<uint64_t>(entt::to_integral(p.first)));
        h.add(static_cast<uint64_t>(p.second));
    }
}

}  // namespace colony

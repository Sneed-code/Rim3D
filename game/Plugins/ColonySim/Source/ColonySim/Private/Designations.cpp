#include "ColonySim/Designations.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"

namespace colony {

const char* designationKindName(DesignationKind kind) {
    switch (kind) {
        case DesignationKind::None: return "None";
        case DesignationKind::Mine: return "Mine";
        case DesignationKind::CutPlant: return "CutPlant";
        case DesignationKind::HarvestPlant: return "HarvestPlant";
    }
    return "?";
}

DesignationKind Designations::at(CellPos c) const {
    if (!map_.onGround(c)) {
        return DesignationKind::None;
    }
    const auto it = marks_.find(map_.index(c));
    return it == marks_.end() ? DesignationKind::None : it->second;
}

bool Designations::set(CellPos c, DesignationKind kind) {
    if (!map_.onGround(c)) {
        return false;
    }
    if (kind == DesignationKind::None) {
        marks_.erase(map_.index(c));
    } else {
        marks_[map_.index(c)] = kind;
    }
    return true;
}

size_t Designations::countOf(DesignationKind kind) const {
    size_t n = 0;
    for (const auto& mark : marks_) {
        n += mark.second == kind ? 1 : 0;
    }
    return n;
}

void Designations::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(marks_.size()));
    for (const auto& [cell, kind] : marks_) {
        h.add(cell);
        h.add(static_cast<int>(kind));
    }
}

bool Designations::restore(const std::vector<std::pair<int, DesignationKind>>& marks) {
    marks_.clear();
    for (const auto& [cell, kind] : marks) {
        if (kind == DesignationKind::None || cell < 0 || cell >= map_.width() * map_.height()) {
            marks_.clear();
            return false;
        }
        marks_[cell] = kind;
    }
    return true;
}

}  // namespace colony

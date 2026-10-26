#include "ColonySim/ThreatMap.h"

#include "ColonySim/Map.h"

#include <algorithm>
#include <cmath>

namespace colony {

ThreatMap::ThreatMap(Map& map) : map_(map) {}

void ThreatMap::clear() {
    const size_t width = static_cast<size_t>(map_.width());
    for (int y = minY_; y <= maxY_; ++y) {
        std::fill_n(threat_.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * width + static_cast<size_t>(minX_)),
                    maxX_ - minX_ + 1, 0.0f);
    }
    minX_ = minY_ = 0;
    maxX_ = maxY_ = -1;
    sources_ = 0;
}

void ThreatMap::addSource(const Viewpoint& from, float range, float weight, Stance targetStance) {
    const CellPos origin{static_cast<int>(std::floor(from.x)), static_cast<int>(std::floor(from.y)), from.level};
    if (!map_.onGround(origin) || range < 0.0f) {
        return;
    }
    if (threat_.empty()) {
        threat_.assign(static_cast<size_t>(map_.width()) * static_cast<size_t>(map_.height()), 0.0f);
    }
    const Cover& cover = map_.cover();
    const int reach = static_cast<int>(std::ceil(range));
    const int x0 = std::max(0, origin.x - reach);
    const int x1 = std::min(map_.width() - 1, origin.x + reach);
    const int y0 = std::max(0, origin.y - reach);
    const int y1 = std::min(map_.height() - 1, origin.y + reach);
    const float rangeSquared = range * range;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const CellPos c{x, y, origin.level};
            const float dx = static_cast<float>(x) + 0.5f - from.x;
            const float dy = static_cast<float>(y) + 0.5f - from.y;
            if (dx * dx + dy * dy > rangeSquared || !map_.walkable(c)) {
                continue;
            }
            if (cover.lineOfSight(from, Viewpoint::at(c, targetStance))) {
                threat_[static_cast<size_t>(map_.index(c))] += weight;
            }
        }
    }
    if (maxX_ < minX_) {
        minX_ = x0;
        maxX_ = x1;
        minY_ = y0;
        maxY_ = y1;
    } else {
        minX_ = std::min(minX_, x0);
        maxX_ = std::max(maxX_, x1);
        minY_ = std::min(minY_, y0);
        maxY_ = std::max(maxY_, y1);
    }
    ++sources_;
}

float ThreatMap::threatAt(CellPos c) const {
    if (threat_.empty() || !map_.onGround(c)) {
        return 0.0f;
    }
    return threat_[static_cast<size_t>(map_.index(c))];
}

}  // namespace colony

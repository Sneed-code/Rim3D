#include "ColonySim/Regions.h"

#include "ColonySim/Map.h"

#include <algorithm>
#include <functional>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
const std::vector<RegionId>& regionNoIds() {
    static const std::vector<RegionId> empty;
    return empty;
}

}  // namespace

Regions::Regions(const Map& map) : map_(map) {
    blocksX_ = (map.width() + kBlockSize - 1) / kBlockSize;
    blocksY_ = (map.height() + kBlockSize - 1) / kBlockSize;
    blocks_ = blocksX_ * blocksY_ * map.levels();
    regionOf_.assign(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()) *
                         static_cast<size_t>(map.levels()),
                     kNoRegion);
    blockRegions_.resize(static_cast<size_t>(blocks_));
    blockDirty_.assign(static_cast<size_t>(blocks_), 0);
}

size_t Regions::cellSlot(CellPos c) const {
    return static_cast<size_t>(c.level) * static_cast<size_t>(map_.width()) * static_cast<size_t>(map_.height()) +
           static_cast<size_t>(map_.index(c));
}

RegionId Regions::regionAt(CellPos c) {
    update();
    return map_.inBounds(c) ? regionOf_[cellSlot(c)] : kNoRegion;
}

uint32_t Regions::groupAt(CellPos c) {
    const RegionId id = regionAt(c);
    return id == kNoRegion ? kNoGroup : regions_[id].group;
}

bool Regions::canReach(CellPos from, CellPos to) {
    const uint32_t group = groupAt(from);
    return group != kNoGroup && group == groupAt(to);
}

const std::vector<RegionId>& Regions::regionsInBlock(int blockX, int blockY, int level) {
    update();
    if (blockX < 0 || blockY < 0 || level < 0 || blockX >= blocksX_ || blockY >= blocksY_ || level >= map_.levels()) {
        return regionNoIds();
    }
    return blockRegions_[static_cast<size_t>((level * blocksY_ + blockY) * blocksX_ + blockX)];
}

size_t Regions::regionCount() {
    update();
    return liveRegions_;
}

size_t Regions::groupCount() {
    update();
    return groupCells_.size();
}

int Regions::groupCellCount(uint32_t group) {
    update();
    return group < groupCells_.size() ? groupCells_[group] : 0;
}

void Regions::markDirty(CellPos c) {
    if (!built_ || !map_.inBounds(c)) {
        return;  // before the first build every block is built anyway
    }
    const int block = blockOf(c.x, c.y, c.level);
    if (blockDirty_[static_cast<size_t>(block)] == 0) {
        blockDirty_[static_cast<size_t>(block)] = 1;
        dirtyBlocks_.push_back(block);
    }
}

void Regions::update() {
    if (!built_) {
        built_ = true;
        for (int block = 0; block < blocks_; ++block) {
            blockDirty_[static_cast<size_t>(block)] = 1;
            dirtyBlocks_.push_back(block);
        }
    }
    if (!dirtyBlocks_.empty()) {
        rebuildBlocks();
    }
}

void Regions::rebuildBlocks() {
    // Block order, not the order cells changed in, so the same map always gives the same ids.
    std::sort(dirtyBlocks_.begin(), dirtyBlocks_.end());
    for (const int block : dirtyBlocks_) {
        for (const RegionId id : blockRegions_[static_cast<size_t>(block)]) {
            removeRegion(id);
        }
        blockRegions_[static_cast<size_t>(block)].clear();
    }
    for (const int block : dirtyBlocks_) {
        floodBlock(block);
    }

    // Links: every cell side between two different regions where at least one side is in a
    // rebuilt block. Links between two untouched regions are still in place.
    pendingLinks_.clear();
    for (const int block : dirtyBlocks_) {
        linkBlock(block);
    }
    std::sort(pendingLinks_.begin(), pendingLinks_.end(),
              [](const PendingLink& p, const PendingLink& q) { return p.a != q.a ? p.a < q.a : p.b < q.b; });
    std::vector<RegionId> touched;
    for (size_t i = 0; i < pendingLinks_.size();) {
        const RegionId a = pendingLinks_[i].a;
        const RegionId b = pendingLinks_[i].b;
        int faces = 0;
        // Face middles are whole or half metres, so these sums are exact in any order.
        float sumX = 0.0f;
        float sumY = 0.0f;
        for (; i < pendingLinks_.size() && pendingLinks_[i].a == a && pendingLinks_[i].b == b; ++i) {
            ++faces;
            sumX += pendingLinks_[i].x;
            sumY += pendingLinks_[i].y;
        }
        const float x = sumX / static_cast<float>(faces);
        const float y = sumY / static_cast<float>(faces);
        regions_[a].links.push_back(RegionLink{b, faces, x, y});
        regions_[b].links.push_back(RegionLink{a, faces, x, y});
        touched.push_back(a);
        touched.push_back(b);
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const RegionId id : touched) {
        std::vector<RegionLink>& links = regions_[id].links;
        std::sort(links.begin(), links.end(), [](const RegionLink& p, const RegionLink& q) { return p.other < q.other; });
    }

    for (const int block : dirtyBlocks_) {
        blockDirty_[static_cast<size_t>(block)] = 0;
    }
    blocksRebuilt_ += dirtyBlocks_.size();
    dirtyBlocks_.clear();
    relabelGroups();
}

void Regions::removeRegion(RegionId id) {
    for (const RegionLink& link : regions_[id].links) {
        std::vector<RegionLink>& back = regions_[link.other].links;
        back.erase(std::remove_if(back.begin(), back.end(), [id](const RegionLink& l) { return l.other == id; }),
                   back.end());
    }
    regions_[id] = Region{};
    freeIds_.push_back(id);
    std::push_heap(freeIds_.begin(), freeIds_.end(), std::greater<RegionId>());
    --liveRegions_;
}

RegionId Regions::newRegion() {
    // Lowest free id first, so rebuilding an unchanged block gives its regions back their ids.
    RegionId id;
    if (freeIds_.empty()) {
        id = static_cast<RegionId>(regions_.size());
        regions_.emplace_back();
    } else {
        std::pop_heap(freeIds_.begin(), freeIds_.end(), std::greater<RegionId>());
        id = freeIds_.back();
        freeIds_.pop_back();
    }
    regions_[id].alive = true;
    ++liveRegions_;
    return id;
}

void Regions::floodBlock(int block) {
    const int perLevel = blocksX_ * blocksY_;
    const int level = block / perLevel;
    const int x0 = (block % perLevel) % blocksX_ * kBlockSize;
    const int y0 = (block % perLevel) / blocksX_ * kBlockSize;
    const int x1 = std::min(x0 + kBlockSize, map_.width());
    const int y1 = std::min(y0 + kBlockSize, map_.height());
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            regionOf_[cellSlot(CellPos{x, y, level})] = kNoRegion;
        }
    }

    std::vector<RegionId>& inBlock = blockRegions_[static_cast<size_t>(block)];
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const CellPos start{x, y, level};
            if (regionOf_[cellSlot(start)] != kNoRegion || !map_.walkable(start)) {
                continue;
            }
            const RegionId id = newRegion();
            inBlock.push_back(id);
            Region& region = regions_[id];
            region.anchor = start;
            regionOf_[cellSlot(start)] = id;
            if (map_.isDoor(start)) {
                region.kind = RegionKind::Door;
                region.cellCount = 1;
                region.bounds = CellRect{x, y, 1, 1, level};
                continue;
            }

            // 4-connected flood over walkable, non-door cells of this block.
            int minX = x;
            int maxX = x;
            int minY = y;
            int maxY = y;
            floodQueue_.clear();
            floodQueue_.push_back(y * map_.width() + x);
            for (size_t head = 0; head < floodQueue_.size(); ++head) {
                const int cx = floodQueue_[head] % map_.width();
                const int cy = floodQueue_[head] / map_.width();
                minX = std::min(minX, cx);
                maxX = std::max(maxX, cx);
                minY = std::min(minY, cy);
                maxY = std::max(maxY, cy);
                const CellPos around[4] = {{cx + 1, cy, level}, {cx - 1, cy, level}, {cx, cy + 1, level}, {cx, cy - 1, level}};
                for (const CellPos& n : around) {
                    if (n.x < x0 || n.y < y0 || n.x >= x1 || n.y >= y1) {
                        continue;
                    }
                    const size_t slot = cellSlot(n);
                    if (regionOf_[slot] != kNoRegion || !map_.walkable(n) || map_.isDoor(n)) {
                        continue;
                    }
                    regionOf_[slot] = id;
                    floodQueue_.push_back(n.y * map_.width() + n.x);
                }
            }
            region.cellCount = static_cast<int>(floodQueue_.size());
            region.bounds = CellRect{minX, minY, maxX - minX + 1, maxY - minY + 1, level};
        }
    }
    std::sort(inBlock.begin(), inBlock.end());
}

void Regions::linkBlock(int block) {
    const int perLevel = blocksX_ * blocksY_;
    const int level = block / perLevel;
    const int x0 = (block % perLevel) % blocksX_ * kBlockSize;
    const int y0 = (block % perLevel) / blocksX_ * kBlockSize;
    const int x1 = std::min(x0 + kBlockSize, map_.width());
    const int y1 = std::min(y0 + kBlockSize, map_.height());
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const CellPos c{x, y, level};
            const size_t slot = cellSlot(c);
            const RegionId r = regionOf_[slot];
            if (r == kNoRegion) {
                continue;
            }
            // Only the +x and +y sides, plus the -x and -y sides that face an untouched block,
            // so each side is counted once.
            struct Side {
                CellPos n;
                float fx;
                float fy;
            };
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const Side sides[4] = {{{x + 1, y, level}, fx + 1.0f, fy + 0.5f},
                                   {{x, y + 1, level}, fx + 0.5f, fy + 1.0f},
                                   {{x - 1, y, level}, fx, fy + 0.5f},
                                   {{x, y - 1, level}, fx + 0.5f, fy}};
            for (int i = 0; i < 4; ++i) {
                const Side& side = sides[i];
                if (!map_.inBounds(side.n)) {
                    continue;
                }
                const bool backward = i >= 2;
                if (backward && blockDirty_[static_cast<size_t>(blockOf(side.n.x, side.n.y, level))] != 0) {
                    continue;  // the cell on the other side counts it as its +x or +y side
                }
                const RegionId s = regionOf_[cellSlot(side.n)];
                if (s == kNoRegion || s == r) {
                    continue;
                }
                pendingLinks_.push_back(PendingLink{std::min(r, s), std::max(r, s), side.fx, side.fy});
            }
        }
    }
}

void Regions::relabelGroups() {
    for (Region& region : regions_) {
        region.group = kNoGroup;
    }
    groupCells_.clear();
    for (RegionId first = 0; first < regions_.size(); ++first) {
        if (!regions_[first].alive || regions_[first].group != kNoGroup) {
            continue;
        }
        const uint32_t group = static_cast<uint32_t>(groupCells_.size());
        int cells = 0;
        floodQueue_.clear();
        floodQueue_.push_back(static_cast<int>(first));
        regions_[first].group = group;
        for (size_t head = 0; head < floodQueue_.size(); ++head) {
            const Region& region = regions_[static_cast<RegionId>(floodQueue_[head])];
            cells += region.cellCount;
            for (const RegionLink& link : region.links) {
                if (regions_[link.other].group == kNoGroup) {
                    regions_[link.other].group = group;
                    floodQueue_.push_back(static_cast<int>(link.other));
                }
            }
        }
        groupCells_.push_back(cells);
    }
    ++groupRelabels_;
}

}  // namespace colony

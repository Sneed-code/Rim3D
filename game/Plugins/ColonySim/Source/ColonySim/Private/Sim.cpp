#include "ColonySim/Sim.h"

#include <cmath>
#include <cstdlib>

namespace colony {

namespace {

// Moves the pawn along its path by one step's worth of jogging. Returns true
// once the end of the path is reached.
bool haulAdvance(Pawn& pawn, int mapWidth) {
    float budget = kPawnMetresPerStep;
    while (budget > 0.0f && pawn.pathPos < pawn.path.size()) {
        const int c = pawn.path[pawn.pathPos];
        const float tx = static_cast<float>(c % mapWidth) + 0.5f;
        const float ty = static_cast<float>(c / mapWidth) + 0.5f;
        const float dx = tx - pawn.x;
        const float dy = ty - pawn.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= budget) {
            pawn.x = tx;
            pawn.y = ty;
            budget -= dist;
            ++pawn.pathPos;
        } else {
            pawn.x += dx / dist * budget;
            pawn.y += dy / dist * budget;
            budget = 0.0f;
        }
    }
    return pawn.pathPos >= pawn.path.size();
}

int haulPawnCell(const Pawn& pawn, int mapWidth) {
    return static_cast<int>(pawn.y) * mapWidth + static_cast<int>(pawn.x);
}

}  // namespace

Sim::Sim(const SimSettings& settings)
    : settings_(settings),
      map_(MapSettings{settings.mapSize, settings.mapSize, splitMix64(settings.seed ^ 0x6D6170ULL)}),
      pathfinder_(map_),
      time_(),
      rng_(splitMix64(settings.seed ^ 0x7061776EULL)) {
    placeStockpileAndThings();
}

void Sim::placeStockpileAndThings() {
    // Stockpile: the walkable cell nearest the middle, searched in growing squares.
    const int size = map_.width();
    const int mid = size / 2;
    for (int r = 0; r < size && stockpileCell_ < 0; ++r) {
        for (int y = mid - r; y <= mid + r && stockpileCell_ < 0; ++y) {
            for (int x = mid - r; x <= mid + r; ++x) {
                const bool onRing = std::abs(x - mid) == r || std::abs(y - mid) == r;
                if (onRing && map_.inBounds(x, y) && map_.walkable(x, y)) {
                    stockpileCell_ = map_.index(x, y);
                    break;
                }
            }
        }
    }
    if (stockpileCell_ < 0) {
        return;
    }

    // Everything reachable from the stockpile (4-connected, so always pathable).
    std::vector<int> reachable{stockpileCell_};
    std::vector<uint8_t> visited(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
    visited[static_cast<size_t>(stockpileCell_)] = 1;
    for (size_t head = 0; head < reachable.size(); ++head) {
        const int c = reachable[head];
        const int cx = c % size;
        const int cy = c / size;
        const int nx[4] = {cx + 1, cx - 1, cx, cx};
        const int ny[4] = {cy, cy, cy + 1, cy - 1};
        for (int d = 0; d < 4; ++d) {
            if (!map_.inBounds(nx[d], ny[d]) || !map_.walkable(nx[d], ny[d])) {
                continue;
            }
            const int n = map_.index(nx[d], ny[d]);
            if (visited[static_cast<size_t>(n)] == 0) {
                visited[static_cast<size_t>(n)] = 1;
                reachable.push_back(n);
            }
        }
    }

    items_.reserve(static_cast<size_t>(settings_.items));
    for (int i = 0; i < settings_.items; ++i) {
        const uint32_t pick = rng_.below(static_cast<uint32_t>(reachable.size()));
        items_.push_back(Item{reachable[pick], false, false});
    }
    pawns_.resize(static_cast<size_t>(settings_.pawns));
    for (Pawn& pawn : pawns_) {
        pawn.x = static_cast<float>(stockpileCell_ % size) + 0.5f;
        pawn.y = static_cast<float>(stockpileCell_ / size) + 0.5f;
    }
}

int Sim::itemsLeft() const {
    int left = 0;
    for (const Item& item : items_) {
        left += item.stored ? 0 : 1;
    }
    return left;
}

void Sim::startNextHaul(Pawn& pawn) {
    // Nearest free item in a straight line. A linear scan is fine for the Stage 0
    // test; the real job system keeps ready-made haul lists per chunk.
    const int w = map_.width();
    int best = -1;
    float bestDist = 0.0f;
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& item = items_[i];
        if (item.reserved || item.stored) {
            continue;
        }
        const float dx = static_cast<float>(item.cell % w) + 0.5f - pawn.x;
        const float dy = static_cast<float>(item.cell / w) + 0.5f - pawn.y;
        const float dist = dx * dx + dy * dy;
        if (best < 0 || dist < bestDist) {
            best = static_cast<int>(i);
            bestDist = dist;
        }
    }
    if (best < 0) {
        return;
    }
    Item& item = items_[static_cast<size_t>(best)];
    item.reserved = true;
    pawn.pathPos = 0;
    if (!pathfinder_.findPath(haulPawnCell(pawn, w), item.cell, pawn.path)) {
        ++stats_.failedJobs;  // item stays reserved so nobody retries it
        return;
    }
    pawn.item = best;
    pawn.job = PawnJob::GoToItem;
}

void Sim::updatePawn(Pawn& pawn) {
    const int w = map_.width();
    switch (pawn.job) {
        case PawnJob::Idle:
            startNextHaul(pawn);
            break;
        case PawnJob::GoToItem:
            if (haulAdvance(pawn, w)) {
                pawn.pathPos = 0;
                if (pathfinder_.findPath(haulPawnCell(pawn, w), stockpileCell_, pawn.path)) {
                    pawn.job = PawnJob::CarryToStockpile;
                } else {
                    ++stats_.failedJobs;
                    pawn.job = PawnJob::Idle;
                    pawn.item = -1;
                }
            }
            break;
        case PawnJob::CarryToStockpile:
            if (haulAdvance(pawn, w)) {
                Item& item = items_[static_cast<size_t>(pawn.item)];
                item.cell = stockpileCell_;
                item.stored = true;
                ++stats_.hauls;
                pawn.job = PawnJob::Idle;
                pawn.item = -1;
            }
            break;
    }
}

void Sim::step() {
    for (Pawn& pawn : pawns_) {
        updatePawn(pawn);
    }
    time_.seconds += 1;
}

void Sim::stepMany(int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        step();
    }
}

}  // namespace colony

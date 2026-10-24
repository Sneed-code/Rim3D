#include "ColonySim/Pathfinder.h"

#include "ColonySim/Map.h"

#include <algorithm>
#include <cstdlib>

namespace colony {

namespace {

constexpr float kPathDiagonalCost = 1.41421356f;

float pathOctile(int ax, int ay, int bx, int by) {
    const int dx = std::abs(ax - bx);
    const int dy = std::abs(ay - by);
    return static_cast<float>(dx + dy) + (kPathDiagonalCost - 2.0f) * static_cast<float>(std::min(dx, dy));
}

// Min-heap order. Equal f goes to the smaller h, then the lower cell index so searches are deterministic.
struct PathHeapAfter {
    bool operator()(const Pathfinder::OpenNode& a, const Pathfinder::OpenNode& b) const {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        if (a.h != b.h) {
            return a.h > b.h;
        }
        return a.cell > b.cell;
    }
};

}  // namespace

Pathfinder::Pathfinder(const Map& map) : map_(map) {
    const size_t cells = static_cast<size_t>(map.width()) * static_cast<size_t>(map.height());
    g_.assign(cells, 0.0f);
    parent_.assign(cells, -1);
    seen_.assign(cells, 0);
    closed_.assign(cells, 0);
}

bool Pathfinder::findPath(int start, int goal, std::vector<int>& out) {
    ++searches_;
    out.clear();
    const int w = map_.width();
    const int gx = goal % w;
    const int gy = goal / w;
    if (start == goal) {
        return true;
    }
    if (!map_.walkable(gx, gy)) {
        return false;
    }

    if (++generation_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0u);
        std::fill(closed_.begin(), closed_.end(), 0u);
        generation_ = 1;
    }
    const uint32_t gen = generation_;
    const auto at = [](auto& v, int i) -> auto& { return v[static_cast<size_t>(i)]; };

    open_.clear();
    at(g_, start) = 0.0f;
    at(parent_, start) = -1;
    at(seen_, start) = gen;
    const float startH = pathOctile(start % w, start / w, gx, gy);
    open_.push_back(OpenNode{startH, startH, start});

    static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    while (!open_.empty()) {
        std::pop_heap(open_.begin(), open_.end(), PathHeapAfter{});
        const int c = open_.back().cell;
        open_.pop_back();
        if (at(closed_, c) == gen) {
            continue;  // stale duplicate
        }
        at(closed_, c) = gen;
        ++cellsExpanded_;

        if (c == goal) {
            for (int i = goal; i != start; i = at(parent_, i)) {
                out.push_back(i);
            }
            std::reverse(out.begin(), out.end());
            return true;
        }

        const int cx = c % w;
        const int cy = c / w;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + kDx[d];
            const int ny = cy + kDy[d];
            if (!map_.inBounds(nx, ny) || !map_.walkable(nx, ny)) {
                continue;
            }
            const bool diagonal = d >= 4;
            if (diagonal && (!map_.walkable(nx, cy) || !map_.walkable(cx, ny))) {
                continue;
            }
            const int n = map_.index(nx, ny);
            if (at(closed_, n) == gen) {
                continue;
            }
            const float ng = at(g_, c) + (diagonal ? kPathDiagonalCost : 1.0f);
            if (at(seen_, n) != gen || ng < at(g_, n)) {
                at(seen_, n) = gen;
                at(g_, n) = ng;
                at(parent_, n) = c;
                const float nh = pathOctile(nx, ny, gx, gy);
                open_.push_back(OpenNode{ng + nh, nh, n});
                std::push_heap(open_.begin(), open_.end(), PathHeapAfter{});
            }
        }
    }
    return false;
}

}  // namespace colony

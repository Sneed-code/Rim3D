#include "ColonySim/Cover.h"

#include "ColonySim/Map.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr float kCoverFar = 1.0e30f;

}  // namespace

Cover::Cover(const Map& map) : map_(map) {}

void Cover::markDirty(CellPos c) {
    if (built_ && map_.onGround(c)) {
        dirty_.push_back(c);
    }
}

void Cover::computeCell(int index) {
    const CellPos c = map_.cellAt(index);
    const float ground = map_.groundHeight(c);
    for (int d = 0; d < kDir8Count; ++d) {
        const Dir8Step step = dir8Step(static_cast<Dir8>(d));
        const CellPos n{c.x + step.dx, c.y + step.dy, c.level};
        uint8_t dm = 0;
        if (map_.onGround(n)) {
            const uint8_t obstacle = map_.obstacleDm(n);
            if (obstacle == kObstacleTallDm) {
                dm = kObstacleTallDm;
            } else {
                // Taller than the ground in `c`: a building, or a rise in the ground.
                const float height = map_.groundHeight(n) + static_cast<float>(obstacle) * 0.1f - ground;
                const long tenths = std::lround(height * 10.0f);
                dm = static_cast<uint8_t>(std::clamp(tenths, 0L, static_cast<long>(kObstacleTallDm) - 1));
            }
        }
        coverDm_[static_cast<size_t>(index) * kDir8Count + static_cast<size_t>(d)] = dm;
    }
    ++cellsRecomputed_;
}

void Cover::update() {
    const int cells = map_.width() * map_.height();
    if (!built_) {
        coverDm_.assign(static_cast<size_t>(cells) * kDir8Count, 0);
        for (int i = 0; i < cells; ++i) {
            computeCell(i);
        }
        built_ = true;
        dirty_.clear();
        return;
    }
    if (dirty_.empty()) {
        return;
    }
    // A changed cell changes its own cover and that of the eight cells around it.
    scratch_.clear();
    for (const CellPos& c : dirty_) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const CellPos n{c.x + dx, c.y + dy, c.level};
                if (map_.onGround(n)) {
                    scratch_.push_back(map_.index(n));
                }
            }
        }
    }
    dirty_.clear();
    std::sort(scratch_.begin(), scratch_.end());
    scratch_.erase(std::unique(scratch_.begin(), scratch_.end()), scratch_.end());
    for (const int i : scratch_) {
        computeCell(i);
    }
}

float Cover::coverHeight(CellPos c, Dir8 d) {
    update();
    if (!map_.onGround(c)) {
        return 0.0f;
    }
    return static_cast<float>(coverDm_[static_cast<size_t>(map_.index(c)) * kDir8Count + static_cast<size_t>(d)]) * 0.1f;
}

float Cover::coverFraction(CellPos c, Dir8 d, Stance stance) {
    return std::min(1.0f, coverHeight(c, d) / bodyHeight(stance));
}

float Cover::coverAgainst(CellPos c, Stance stance, float fromX, float fromY) {
    const float dx = fromX - (static_cast<float>(c.x) + 0.5f);
    const float dy = fromY - (static_cast<float>(c.y) + 0.5f);
    if (dx == 0.0f && dy == 0.0f) {
        return 0.0f;
    }
    return coverFraction(c, dir8Toward(dx, dy), stance);
}

// Does the cell (x, y) block a line whose height above sea level is heightA where it enters the
// cell and heightB where it leaves? The line is straight, so its lowest point in the cell is one
// of the two.
bool Cover::cellBlocks(int x, int y, float heightA, float heightB) const {
    const CellPos c{x, y, 0};
    if (!map_.onGround(c)) {
        return true;
    }
    const uint8_t obstacle = map_.obstacleDm(c);
    if (obstacle == kObstacleTallDm) {
        return true;
    }
    const float top = map_.groundHeight(c) + static_cast<float>(obstacle) * 0.1f;
    return std::min(heightA, heightB) < top;
}

bool Cover::lineOfSight(const Viewpoint& from, const Viewpoint& to) const {
    ++sightQueries_;
    const CellPos a{static_cast<int>(std::floor(from.x)), static_cast<int>(std::floor(from.y)), from.level};
    const CellPos b{static_cast<int>(std::floor(to.x)), static_cast<int>(std::floor(to.y)), to.level};
    if (!map_.onGround(a) || !map_.onGround(b)) {
        return false;
    }
    if (a == b) {
        return true;
    }

    const float eyeA = map_.groundHeight(a) + eyeHeight(from.stance);
    const float eyeB = map_.groundHeight(b) + eyeHeight(to.stance);
    const auto heightAt = [&](float t) { return eyeA + (eyeB - eyeA) * t; };

    // Walk the cells the line crosses (Amanatides and Woo). t runs from 0 at `from` to 1 at `to`.
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const int stepX = dx > 0.0f ? 1 : -1;
    const int stepY = dy > 0.0f ? 1 : -1;
    const float deltaX = dx != 0.0f ? 1.0f / std::fabs(dx) : kCoverFar;
    const float deltaY = dy != 0.0f ? 1.0f / std::fabs(dy) : kCoverFar;
    float nextX = dx > 0.0f ? (static_cast<float>(a.x + 1) - from.x) / dx
                  : dx < 0.0f ? (from.x - static_cast<float>(a.x)) / -dx
                              : kCoverFar;
    float nextY = dy > 0.0f ? (static_cast<float>(a.y + 1) - from.y) / dy
                  : dy < 0.0f ? (from.y - static_cast<float>(a.y)) / -dy
                              : kCoverFar;
    int x = a.x;
    int y = a.y;
    const int maxSteps = std::abs(b.x - a.x) + std::abs(b.y - a.y) + 2;
    for (int i = 0; i < maxSteps; ++i) {
        const float enter = std::min(nextX, nextY);
        if (nextX == nextY) {
            // The line goes through a corner. It only passes if one of the two cells it touches
            // there is open: it can't squeeze between two diagonal blockers.
            const float h = heightAt(enter);
            if (cellBlocks(x + stepX, y, h, h) && cellBlocks(x, y + stepY, h, h)) {
                return false;
            }
            x += stepX;
            y += stepY;
            nextX += deltaX;
            nextY += deltaY;
        } else if (nextX < nextY) {
            x += stepX;
            nextX += deltaX;
        } else {
            y += stepY;
            nextY += deltaY;
        }
        if (x == b.x && y == b.y) {
            return true;
        }
        const float leave = std::min(1.0f, std::min(nextX, nextY));
        if (cellBlocks(x, y, heightAt(enter), heightAt(leave))) {
            return false;
        }
    }
    return false;
}

}  // namespace colony

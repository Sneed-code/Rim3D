#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Grid.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace colony {

class Map;

// Cover and line of sight. See docs/design/cover.md.
//
// RimWorld rates cover with a flat percentage; here things have a real height, so a pawn's stance
// matters: a sandbag hides a crouching pawn and not a standing one, and a hill hides what is
// behind it.

// How a pawn holds its body. Eye height is where it sees from, body height is the top of its
// silhouette. Metres above the ground it stands on. These are our own numbers (RimWorld has no
// heights).
enum class Stance : uint8_t { Standing, Crouching, Prone };

inline constexpr float eyeHeight(Stance s) {
    return s == Stance::Standing ? 1.6f : s == Stance::Crouching ? 1.0f : 0.3f;
}
inline constexpr float bodyHeight(Stance s) {
    return s == Stance::Standing ? 1.8f : s == Stance::Crouching ? 1.1f : 0.4f;
}

// How tall things are, in decimetres (0.1 m), because that fits a byte. A building's height comes
// from its def's `fillPercent`: 1 (a wall, a door, a boulder) is the tallest value, which blocks
// all sight; anything lower is that share of kFullFillDm. Sandbags (0.55) come out at 1.1 m.
inline constexpr uint8_t kObstacleTallDm = 255;
inline constexpr double kFullFillDm = 20.0;

inline uint8_t obstacleFromFill(double fillPercent) {
    if (fillPercent >= 1.0) {
        return kObstacleTallDm;
    }
    if (fillPercent <= 0.0) {
        return 0;
    }
    return static_cast<uint8_t>(std::lround(fillPercent * kFullFillDm));
}

// The eight compass directions, with +y as north (as in RimWorld).
enum class Dir8 : uint8_t { N, NE, E, SE, S, SW, W, NW };
inline constexpr int kDir8Count = 8;

struct Dir8Step {
    int dx;
    int dy;
};

inline constexpr Dir8Step dir8Step(Dir8 d) {
    constexpr Dir8Step steps[kDir8Count] = {{0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};
    return steps[static_cast<int>(d)];
}

// The compass direction closest to the vector (dx, dy). The vector must not be zero. No
// trigonometry, so every platform agrees: a vector within 22.5 degrees of an axis is on that axis.
inline Dir8 dir8Toward(float dx, float dy) {
    constexpr float kTan22 = 0.41421356f;
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);
    if (ay <= ax * kTan22) {
        return dx >= 0.0f ? Dir8::E : Dir8::W;
    }
    if (ax <= ay * kTan22) {
        return dy >= 0.0f ? Dir8::N : Dir8::S;
    }
    if (dx >= 0.0f) {
        return dy >= 0.0f ? Dir8::NE : Dir8::SE;
    }
    return dy >= 0.0f ? Dir8::NW : Dir8::SW;
}

// Where someone sees from or is seen at: a point in metres on a level, and how it stands.
struct Viewpoint {
    float x = 0.0f;
    float y = 0.0f;
    int level = 0;
    Stance stance = Stance::Standing;

    static Viewpoint at(CellPos c, Stance stance = Stance::Standing) {
        return Viewpoint{static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f, c.level, stance};
    }
};

class COLONY_API Cover {
public:
    // Nothing is computed until the first cover query, so maps used only for terrain cost nothing.
    explicit Cover(const Map& map);

    // Cover: how much a pawn standing in `c` is hidden from fire coming from one side. The
    // cell on that side decides it: its building, its rock, or ground that rises above `c`.

    // Height in metres of what shelters `c` from direction `d` (0 for none). 25.5 is a wall.
    float coverHeight(CellPos c, Dir8 d);
    // Share of the pawn's body that height hides, 0 to 1.
    float coverFraction(CellPos c, Dir8 d, Stance stance);
    // The same for fire coming from the point (fromX, fromY) in metres, which must not be the
    // centre of `c` itself.
    float coverAgainst(CellPos c, Stance stance, float fromX, float fromY);

    // Line of sight: true if `from` can see the eyes of `to`. Follows the straight line between
    // the two eye points over the ground and buildings; a cell blocks it when the line passes
    // below its top. The two end cells never block. A line squeezing between two diagonal
    // blockers is blocked, one grazing a single corner is not. Both points must be on the map
    // and on the same level. Seeing is mutual when both have the same stance.
    bool lineOfSight(const Viewpoint& from, const Viewpoint& to) const;

    // The map calls this when the obstacle in a cell changes (and terrain editing will).
    void markDirty(CellPos c);
    // Recomputes the cover of the cells around changed cells. Cover queries do this themselves.
    void update();

    // Counters for tests and simrun.
    uint64_t cellsRecomputed() const { return cellsRecomputed_; }
    uint64_t sightQueries() const { return sightQueries_; }

private:
    void computeCell(int index);
    bool cellBlocks(int x, int y, float heightA, float heightB) const;

    const Map& map_;
    bool built_ = false;
    std::vector<uint8_t> coverDm_;  // kDir8Count per cell, in decimetres
    std::vector<CellPos> dirty_;
    std::vector<int> scratch_;
    uint64_t cellsRecomputed_ = 0;
    mutable uint64_t sightQueries_ = 0;
};

}  // namespace colony

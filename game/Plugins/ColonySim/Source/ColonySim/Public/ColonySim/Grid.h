#pragma once

#include <cstdint>

namespace colony {

// One cell of the 1 m grid. Levels stack about 3 m apart for multi-storey buildings; level 0 is
// the ground. Every grid, region and path function takes a CellPos, even while only level 0 is
// stored, so stairs and upper floors come later without changing those interfaces.
struct CellPos {
    int x = 0;
    int y = 0;
    int level = 0;

    friend bool operator==(const CellPos&, const CellPos&) = default;
};

// Facing of a building, as RimWorld's Rot4. East and West swap a footprint's width and depth.
enum class Rot4 : uint8_t { North, East, South, West };

// A rectangle of cells on one level: minX..minX+sizeX-1 by minY..minY+sizeY-1.
struct CellRect {
    int minX = 0;
    int minY = 0;
    int sizeX = 1;
    int sizeY = 1;
    int level = 0;

    bool contains(CellPos c) const {
        return c.level == level && c.x >= minX && c.y >= minY && c.x < minX + sizeX && c.y < minY + sizeY;
    }
};

// The cells a building of `sizeX` by `sizeY` (its def's `size`, as placed facing North) covers
// when its footprint starts at `origin` (the corner with the lowest x and y) and faces `rot`.
inline CellRect footprintRect(CellPos origin, int sizeX, int sizeY, Rot4 rot) {
    const bool sideways = rot == Rot4::East || rot == Rot4::West;
    return CellRect{origin.x, origin.y, sideways ? sizeY : sizeX, sideways ? sizeX : sizeY, origin.level};
}

}  // namespace colony

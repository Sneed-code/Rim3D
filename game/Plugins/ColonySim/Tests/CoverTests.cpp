#include "ColonySim/Cover.h"
#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Sim.h"
#include "ColonySim/ThreatMap.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace colony;

namespace {

// A flat 64 m test map with its things.
struct CoverTestWorld {
    Map map{MapSettings{64, 64, 1, true}};
    Things things{map, testDefs()};
    Cover& cover() { return map.cover(); }

    // A wall of no def, straight into the map, for tests that don't need a Thing.
    void block(int x, int y, uint8_t dm = kObstacleTallDm) {
        map.setBuilding(CellPos{x, y, 0}, kNoEntity, dm == kObstacleTallDm ? Passage::Blocked : Passage::Open, dm);
    }
};

bool coverSees(Cover& cover, CellPos a, Stance sa, CellPos b, Stance sb) {
    return cover.lineOfSight(Viewpoint::at(a, sa), Viewpoint::at(b, sb));
}

// A slow line of sight to check the fast one against: samples the line every 5 cm and asks
// whether it is below the top of the cell it is in.
bool coverSeesBySampling(const Map& map, const Viewpoint& from, const Viewpoint& to) {
    const CellPos a{static_cast<int>(std::floor(from.x)), static_cast<int>(std::floor(from.y)), 0};
    const CellPos b{static_cast<int>(std::floor(to.x)), static_cast<int>(std::floor(to.y)), 0};
    const float eyeA = map.groundHeight(a) + eyeHeight(from.stance);
    const float eyeB = map.groundHeight(b) + eyeHeight(to.stance);
    const float length = std::hypot(to.x - from.x, to.y - from.y);
    const int samples = static_cast<int>(length / 0.05f);
    for (int i = 1; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const CellPos c{static_cast<int>(std::floor(from.x + (to.x - from.x) * t)),
                        static_cast<int>(std::floor(from.y + (to.y - from.y) * t)), 0};
        if (c == a || c == b) {
            continue;
        }
        const uint8_t obstacle = map.obstacleDm(c);
        if (obstacle == kObstacleTallDm || eyeA + (eyeB - eyeA) * t < map.groundHeight(c) + static_cast<float>(obstacle) * 0.1f) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Cover: a def's fillPercent becomes a height") {
    CHECK(obstacleFromFill(0.0) == 0);
    CHECK(obstacleFromFill(-1.0) == 0);
    CHECK(obstacleFromFill(0.55) == 11);  // sandbags: 1.1 m
    CHECK(obstacleFromFill(0.99) == 20);
    CHECK(obstacleFromFill(1.0) == kObstacleTallDm);
    CHECK(obstacleFromFill(1.5) == kObstacleTallDm);
}

TEST_CASE("Cover: directions") {
    CHECK(dir8Toward(0, 5) == Dir8::N);
    CHECK(dir8Toward(5, 5) == Dir8::NE);
    CHECK(dir8Toward(5, 0) == Dir8::E);
    CHECK(dir8Toward(3, -3) == Dir8::SE);
    CHECK(dir8Toward(0, -1) == Dir8::S);
    CHECK(dir8Toward(-4, -4) == Dir8::SW);
    CHECK(dir8Toward(-9, 1) == Dir8::W);
    CHECK(dir8Toward(-2, 2) == Dir8::NW);
    CHECK(dir8Toward(10, 4) == Dir8::E);   // 21.8 degrees: still east
    CHECK(dir8Toward(10, 5) == Dir8::NE);  // 26.6 degrees
    for (int d = 0; d < kDir8Count; ++d) {
        const Dir8Step s = dir8Step(static_cast<Dir8>(d));
        CHECK(dir8Toward(static_cast<float>(s.dx), static_cast<float>(s.dy)) == static_cast<Dir8>(d));
    }
}

TEST_CASE("Sight: open ground is visible both ways, off the map is not") {
    CoverTestWorld w;
    CHECK(coverSees(w.cover(), CellPos{5, 5, 0}, Stance::Standing, CellPos{40, 33, 0}, Stance::Standing));
    CHECK(coverSees(w.cover(), CellPos{40, 33, 0}, Stance::Standing, CellPos{5, 5, 0}, Stance::Standing));
    CHECK(coverSees(w.cover(), CellPos{5, 5, 0}, Stance::Prone, CellPos{5, 5, 0}, Stance::Standing));
    CHECK_FALSE(coverSees(w.cover(), CellPos{5, 5, 0}, Stance::Standing, CellPos{-1, 5, 0}, Stance::Standing));
    CHECK_FALSE(coverSees(w.cover(), CellPos{5, 5, 0}, Stance::Standing, CellPos{64, 5, 0}, Stance::Standing));
    CHECK_FALSE(coverSees(w.cover(), CellPos{5, 5, 0}, Stance::Standing, CellPos{5, 5, 1}, Stance::Standing));
}

TEST_CASE("Sight: a wall blocks it, and removing the wall restores it") {
    CoverTestWorld w;
    const Entity wall = w.things.spawn("Wall", CellPos{10, 10, 0});
    CHECK(w.map.obstacleDm(CellPos{10, 10, 0}) == kObstacleTallDm);
    CHECK_FALSE(coverSees(w.cover(), CellPos{5, 10, 0}, Stance::Standing, CellPos{15, 10, 0}, Stance::Standing));
    CHECK_FALSE(coverSees(w.cover(), CellPos{15, 10, 0}, Stance::Standing, CellPos{5, 10, 0}, Stance::Standing));
    CHECK(coverSees(w.cover(), CellPos{5, 9, 0}, Stance::Standing, CellPos{15, 9, 0}, Stance::Standing));
    CHECK(coverSees(w.cover(), CellPos{5, 10, 0}, Stance::Standing, CellPos{9, 10, 0}, Stance::Standing));
    w.things.destroy(wall);
    CHECK(coverSees(w.cover(), CellPos{5, 10, 0}, Stance::Standing, CellPos{15, 10, 0}, Stance::Standing));
}

TEST_CASE("Sight: a wall on the end cell does not hide the pawn standing next to it") {
    CoverTestWorld w;
    w.block(10, 10);
    CHECK(coverSees(w.cover(), CellPos{9, 10, 0}, Stance::Standing, CellPos{5, 10, 0}, Stance::Standing));
    CHECK(coverSees(w.cover(), CellPos{5, 10, 0}, Stance::Standing, CellPos{9, 10, 0}, Stance::Standing));
}

TEST_CASE("Sight: low cover hides a crouching pawn but not a standing one") {
    CoverTestWorld w;
    w.things.spawn("Sandbags", CellPos{20, 10, 0});  // 1.1 m
    CHECK(w.map.obstacleDm(CellPos{20, 10, 0}) == 11);
    const CellPos shooter{10, 10, 0};
    const CellPos behind{21, 10, 0};
    CHECK(coverSees(w.cover(), shooter, Stance::Standing, behind, Stance::Standing));
    CHECK_FALSE(coverSees(w.cover(), shooter, Stance::Standing, behind, Stance::Crouching));
    CHECK_FALSE(coverSees(w.cover(), shooter, Stance::Standing, behind, Stance::Prone));
    // A crouching shooter is hidden too, and the other way round.
    CHECK_FALSE(coverSees(w.cover(), behind, Stance::Crouching, shooter, Stance::Standing));
    CHECK(coverSees(w.cover(), behind, Stance::Standing, shooter, Stance::Standing));
    // Up close the line is steep enough to look over the sandbags.
    CHECK(coverSees(w.cover(), CellPos{19, 10, 0}, Stance::Standing, behind, Stance::Crouching));
}

TEST_CASE("Sight: a closed door blocks it like a wall") {
    CoverTestWorld w;
    w.things.spawn("Door", CellPos{10, 10, 0});
    CHECK(w.map.obstacleDm(CellPos{10, 10, 0}) == kObstacleTallDm);
    CHECK(w.map.isDoor(CellPos{10, 10, 0}));
    CHECK_FALSE(coverSees(w.cover(), CellPos{5, 10, 0}, Stance::Standing, CellPos{15, 10, 0}, Stance::Standing));
}

TEST_CASE("Sight: a line cannot squeeze between two diagonal walls, but can graze one corner") {
    CoverTestWorld w;
    // From the middle of (3,3) to the middle of (6,6) the line goes through the corners at
    // (4,4), (5,5) and (6,6).
    const CellPos a{3, 3, 0};
    const CellPos b{6, 6, 0};
    w.block(5, 4);
    CHECK(coverSees(w.cover(), a, Stance::Standing, b, Stance::Standing));  // one wall: grazes its corner
    w.block(4, 5);
    CHECK_FALSE(coverSees(w.cover(), a, Stance::Standing, b, Stance::Standing));  // both: no gap
    CHECK_FALSE(coverSees(w.cover(), b, Stance::Standing, a, Stance::Standing));
}

TEST_CASE("Sight: the fast line matches a slow one on random walls and hills") {
    Map map(MapSettings{96, 96, 7, false});
    Rng rng(99);
    for (int i = 0; i < 120; ++i) {
        const int x = static_cast<int>(rng.below(96));
        const int y = static_cast<int>(rng.below(96));
        const uint8_t dm = rng.below(3) == 0 ? kObstacleTallDm : static_cast<uint8_t>(5 + rng.below(20));
        map.setBuilding(CellPos{x, y, 0}, kNoEntity, Passage::Open, dm);
    }
    Cover& cover = map.cover();
    const Stance stances[] = {Stance::Standing, Stance::Crouching, Stance::Prone};
    int disagree = 0;
    const int pairs = 3000;
    for (int i = 0; i < pairs; ++i) {
        const Viewpoint a{rng.unit() * 96.0f, rng.unit() * 96.0f, 0, stances[rng.below(3)]};
        const Viewpoint b{rng.unit() * 96.0f, rng.unit() * 96.0f, 0, stances[rng.below(3)]};
        const bool fast = cover.lineOfSight(a, b);
        const bool slow = coverSeesBySampling(map, a, b);
        // Sampling at 5 cm can step over the very edge of a cell, so allow rare differences.
        disagree += fast != slow ? 1 : 0;
        // Seeing is mutual for equal stances.
        const Viewpoint a2{a.x, a.y, 0, b.stance};
        CHECK(cover.lineOfSight(a2, b) == cover.lineOfSight(b, a2));
    }
    CHECK(disagree <= pairs / 100);
}

TEST_CASE("Sight: ground that rises hides what is behind it") {
    // Find, on a generated map, two open cells on opposite sides of a hill: the straight line
    // between the eyes dips below the ground in between.
    Map map(MapSettings{128, 128, 3, false});
    Cover& cover = map.cover();
    int blocked = 0;
    int seen = 0;
    for (int y = 0; y < 128; y += 7) {
        for (int x = 0; x + 40 < 128; x += 5) {
            const CellPos a{x, y, 0};
            const CellPos b{x + 40, y, 0};
            if (!map.walkable(a) || !map.walkable(b)) {
                continue;
            }
            const bool see = coverSees(cover, a, Stance::Standing, b, Stance::Standing);
            (see ? seen : blocked) += 1;
            CHECK(see == coverSeesBySampling(map, Viewpoint::at(a), Viewpoint::at(b)));
        }
    }
    CHECK(blocked > 0);
    CHECK(seen > 0);
}

TEST_CASE("Cover: a wall gives full cover from its side only") {
    CoverTestWorld w;
    const CellPos c{20, 20, 0};
    w.block(20, 21);  // north of c
    Cover& cover = w.cover();
    CHECK(cover.coverFraction(c, Dir8::N, Stance::Standing) == doctest::Approx(1.0f));
    CHECK(cover.coverHeight(c, Dir8::N) == doctest::Approx(25.5f));
    for (const Dir8 d : {Dir8::NE, Dir8::E, Dir8::SE, Dir8::S, Dir8::SW, Dir8::W, Dir8::NW}) {
        CHECK(cover.coverFraction(c, d, Stance::Standing) == 0.0f);
    }
    // The cells around the wall see it from their side.
    CHECK(cover.coverFraction(CellPos{19, 20, 0}, Dir8::NE, Stance::Prone) == doctest::Approx(1.0f));
    CHECK(cover.coverFraction(CellPos{21, 22, 0}, Dir8::SW, Stance::Prone) == doctest::Approx(1.0f));
    CHECK(cover.coverAgainst(c, Stance::Standing, 20.5f, 50.0f) == doctest::Approx(1.0f));
    CHECK(cover.coverAgainst(c, Stance::Standing, 20.5f, 5.0f) == 0.0f);
    CHECK(cover.coverAgainst(c, Stance::Standing, 20.5f, 20.5f) == 0.0f);
}

TEST_CASE("Cover: sandbags cover more of a crouching pawn than of a standing one") {
    CoverTestWorld w;
    w.things.spawn("Sandbags", CellPos{30, 30, 0});
    Cover& cover = w.cover();
    const CellPos c{29, 30, 0};  // west of the sandbags
    CHECK(cover.coverHeight(c, Dir8::E) == doctest::Approx(1.1f));
    CHECK(cover.coverFraction(c, Dir8::E, Stance::Standing) == doctest::Approx(1.1f / 1.8f));
    CHECK(cover.coverFraction(c, Dir8::E, Stance::Crouching) == doctest::Approx(1.0f));
    CHECK(cover.coverFraction(c, Dir8::E, Stance::Prone) == doctest::Approx(1.0f));
    CHECK(cover.coverAgainst(c, Stance::Standing, 60.0f, 31.0f) == doctest::Approx(1.1f / 1.8f));  // east, a bit north
}

TEST_CASE("Cover: map edges, rock and rises in the ground") {
    CoverTestWorld w;
    CHECK(w.cover().coverFraction(CellPos{0, 0, 0}, Dir8::S, Stance::Standing) == 0.0f);
    CHECK(w.cover().coverFraction(CellPos{0, 0, 0}, Dir8::W, Stance::Standing) == 0.0f);
    CHECK(w.cover().coverFraction(CellPos{-1, 0, 0}, Dir8::W, Stance::Standing) == 0.0f);

    // On a generated map, cover from a neighbour is its height above this cell's ground.
    Map map(MapSettings{128, 128, 5, false});
    Cover& cover = map.cover();
    int rockSides = 0;
    int rises = 0;
    for (int y = 1; y < 127; ++y) {
        for (int x = 1; x < 127; ++x) {
            const CellPos c{x, y, 0};
            for (int d = 0; d < kDir8Count; ++d) {
                const Dir8Step s = dir8Step(static_cast<Dir8>(d));
                const CellPos n{x + s.dx, y + s.dy, 0};
                const float h = cover.coverHeight(c, static_cast<Dir8>(d));
                if (map.terrain(n) == Terrain::Rock) {
                    CHECK(h == doctest::Approx(25.5f));
                    ++rockSides;
                } else {
                    const float expected = std::clamp(map.groundHeight(n) - map.groundHeight(c), 0.0f, 25.4f);
                    CHECK(std::fabs(h - expected) <= 0.051f);  // stored in decimetres
                    rises += h > 0.0f ? 1 : 0;
                }
            }
        }
    }
    CHECK(rockSides > 0);
    CHECK(rises > 0);
}

TEST_CASE("Cover: only the cells around a change are recomputed, and the result matches a fresh build") {
    CoverTestWorld w;
    Cover& cover = w.cover();
    cover.update();
    CHECK(cover.cellsRecomputed() == 64u * 64u);

    w.block(20, 20);
    CHECK(cover.cellsRecomputed() == 64u * 64u);  // nothing yet: queries do the work
    cover.update();
    CHECK(cover.cellsRecomputed() == 64u * 64u + 9u);

    // Twenty walls in a row cost only the cells around them, once each.
    const uint64_t before = cover.cellsRecomputed();
    for (int x = 30; x < 50; ++x) {
        w.block(x, 40);
    }
    cover.update();
    CHECK(cover.cellsRecomputed() - before == 22u * 3u);

    // Changing nothing costs nothing: same height again, an unrelated passage change.
    w.map.setBuilding(CellPos{20, 20, 0}, kNoEntity, Passage::Blocked, kObstacleTallDm);
    cover.update();
    CHECK(cover.cellsRecomputed() - before == 22u * 3u);

    // Edits at the map's corner and removing walls.
    w.block(0, 0, 10);
    w.block(63, 63, 10);
    w.map.clearBuilding(CellPos{20, 20, 0});
    w.map.clearBuilding(CellPos{35, 40, 0});

    Cover fresh(w.map);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            for (int d = 0; d < kDir8Count; ++d) {
                CHECK(cover.coverHeight(CellPos{x, y, 0}, static_cast<Dir8>(d)) ==
                      fresh.coverHeight(CellPos{x, y, 0}, static_cast<Dir8>(d)));
            }
        }
    }
}

TEST_CASE("Cover: building and destroying through Things keeps cover current") {
    CoverTestWorld w;
    Cover& cover = w.cover();
    CHECK(cover.coverFraction(CellPos{10, 10, 0}, Dir8::E, Stance::Standing) == 0.0f);
    const Entity wall = w.things.spawn("Wall", CellPos{11, 10, 0});
    CHECK(cover.coverFraction(CellPos{10, 10, 0}, Dir8::E, Stance::Standing) == doctest::Approx(1.0f));
    w.things.despawn(wall);
    CHECK(cover.coverFraction(CellPos{10, 10, 0}, Dir8::E, Stance::Standing) == 0.0f);
    // A bed has no fillPercent: no cover and no shadow.
    w.things.spawn("Bed", CellPos{11, 10, 0});
    CHECK(cover.coverFraction(CellPos{10, 10, 0}, Dir8::E, Stance::Standing) == 0.0f);
    CHECK(coverSees(cover, CellPos{5, 10, 0}, Stance::Prone, CellPos{15, 10, 0}, Stance::Prone));
}

TEST_CASE("Cover: the obstacle layer is part of the map hash") {
    CoverTestWorld a;
    CoverTestWorld b;
    CHECK(a.map.contentHash() == b.map.contentHash());
    b.block(3, 3, 7);
    CHECK(a.map.contentHash() != b.map.contentHash());
}

TEST_CASE("Threat map: what a source can see within range, and nothing else") {
    CoverTestWorld w;
    for (int y = 28; y <= 36; ++y) {
        w.block(40, y);  // a wall east of the source with a shadow behind it
    }
    ThreatMap threat(w.map);
    const Viewpoint source = Viewpoint::at(CellPos{32, 32, 0});
    CHECK(threat.threatAt(CellPos{32, 32, 0}) == 0.0f);  // before any source, even an unallocated map
    threat.addSource(source, 20.0f, 2.0f);
    CHECK(threat.sourceCount() == 1);
    CHECK(threat.threatAt(CellPos{32, 32, 0}) == 2.0f);
    CHECK(threat.threatAt(CellPos{25, 32, 0}) == 2.0f);
    CHECK(threat.threatAt(CellPos{39, 32, 0}) == 2.0f);   // in front of the wall
    CHECK(threat.threatAt(CellPos{40, 32, 0}) == 0.0f);   // the wall itself can't be walked
    CHECK(threat.threatAt(CellPos{45, 32, 0}) == 0.0f);   // in its shadow
    CHECK(threat.threatAt(CellPos{45, 45, 0}) == 2.0f);   // beside the shadow
    CHECK(threat.threatAt(CellPos{32, 53, 0}) == 0.0f);   // out of range (21 m)
    CHECK(threat.threatAt(CellPos{32, 51, 0}) == 2.0f);   // in range (19 m)
    CHECK(threat.threatAt(CellPos{-3, 5, 0}) == 0.0f);

    // Matches line of sight cell by cell.
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const CellPos c{x, y, 0};
            const float dx = static_cast<float>(x) - 32.0f;
            const float dy = static_cast<float>(y) - 32.0f;
            const bool expected = dx * dx + dy * dy <= 400.0f && w.map.walkable(c) &&
                                  w.cover().lineOfSight(source, Viewpoint::at(c));
            CHECK(threat.threatAt(c) == (expected ? 2.0f : 0.0f));
        }
    }

    // A second source adds up; a crouching target is harder to hit than a standing one.
    threat.addSource(Viewpoint::at(CellPos{32, 40, 0}), 5.0f, 1.0f);
    CHECK(threat.threatAt(CellPos{32, 38, 0}) == 3.0f);
    CHECK(threat.sourceCount() == 2);

    threat.clear();
    CHECK(threat.sourceCount() == 0);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            CHECK(threat.threatAt(CellPos{x, y, 0}) == 0.0f);
        }
    }
}

TEST_CASE("Threat map: low cover shelters crouching pawns only") {
    CoverTestWorld w;
    w.things.spawn("Sandbags", CellPos{40, 32, 0});
    ThreatMap threat(w.map);
    threat.addSource(Viewpoint::at(CellPos{30, 32, 0}), 30.0f, 1.0f, Stance::Crouching);
    CHECK(threat.threatAt(CellPos{41, 32, 0}) == 0.0f);
    CHECK(threat.threatAt(CellPos{39, 32, 0}) == 1.0f);
    threat.clear();
    threat.addSource(Viewpoint::at(CellPos{30, 32, 0}), 30.0f, 1.0f, Stance::Standing);
    CHECK(threat.threatAt(CellPos{41, 32, 0}) == 1.0f);
}

TEST_CASE("Threat map: the sim keeps one per faction") {
    Sim sim(SimSettings{1, 64, 0, 0, true});
    ThreatMap& a = sim.threatMap(1);
    ThreatMap& b = sim.threatMap(2);
    CHECK(&a != &b);
    CHECK(&sim.threatMap(1) == &a);
    a.addSource(Viewpoint::at(CellPos{10, 10, 0}), 5.0f, 1.0f);
    CHECK(a.threatAt(CellPos{10, 12, 0}) == 1.0f);
    CHECK(b.threatAt(CellPos{10, 12, 0}) == 0.0f);
    CHECK(sim.cover().coverFraction(CellPos{10, 10, 0}, Dir8::N, Stance::Standing) == 0.0f);
}

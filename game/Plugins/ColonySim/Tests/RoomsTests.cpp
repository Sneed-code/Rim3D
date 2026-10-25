#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Events.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define ROOMS_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the roof holders, the roles and the door numbers are part of what is tested.
std::shared_ptr<const DefDatabase> roomsDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim roomsSim(int size = 48) { return Sim(SimSettings{5, size, 0, 0, true}, roomsDefs()); }

// A ring of walls round the rectangle x0..x1 by y0..y1, leaving out `gaps`. The walls are the
// ring; the room is the inside.
void roomsRing(Sim& sim, int x0, int y0, int x1, int y1, const std::vector<CellPos>& gaps = {}) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const bool ring = x == x0 || x == x1 || y == y0 || y == y1;
            const CellPos c{x, y, 0};
            if (ring && std::find(gaps.begin(), gaps.end(), c) == gaps.end()) {
                sim.things().spawn("Wall", c);
            }
        }
    }
    sim.rooms().update();
}

int roomsCountRoof(const Sim& sim, Roof roof) {
    int n = 0;
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            n += sim.map().roof(CellPos{x, y, 0}) == roof ? 1 : 0;
        }
    }
    return n;
}

const Room* roomsAt(Sim& sim, int x, int y) { return sim.rooms().roomOf(CellPos{x, y, 0}); }

// Walks `pawn` to `goal` along the pathfinder's path; returns the steps it took, or -1.
int roomsWalk(Sim& sim, Entity pawn, CellPos goal, int limit = 600) {
    const Position pos = sim.things().registry().get<Position>(pawn);
    std::vector<CellPos> path;
    if (!sim.pathfinder().findPath(cellOf(pos), goal, path)) {
        return -1;
    }
    sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, path);
    for (int i = 0; i < limit; ++i) {
        if (sim.things().registry().get<Mover>(pawn).arrived()) {
            return i;
        }
        sim.step();
    }
    return -1;
}

const Door& roomsDoor(const Sim& sim, Entity door) { return sim.things().registry().get<Door>(door); }

}  // namespace

TEST_CASE("Rooms: the mountain has a thick roof and nothing else is roofed") {
    Sim sim(SimSettings{42, 128, 0, 0, false});
    int rock = 0;
    int thick = 0;
    int other = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const CellPos c{x, y, 0};
            const bool isRock = sim.map().terrain(c) == Terrain::Rock;
            rock += isRock ? 1 : 0;
            thick += sim.map().roof(c) == Roof::Thick ? 1 : 0;
            other += (sim.map().roof(c) != Roof::None) != isRock ? 1 : 0;
        }
    }
    CHECK(rock > 0);
    CHECK(thick == rock);
    CHECK(other == 0);
}

TEST_CASE("Rooms: walls round a space make a roofed room, and the roof follows the walls") {
    Sim sim = roomsSim();
    roomsRing(sim, 10, 10, 16, 16);
    const Room* room = roomsAt(sim, 13, 13);
    ROOMS_REQUIRE(room != nullptr);
    CHECK(room->cellCount == 25);
    CHECK(room->enclosed());
    CHECK_FALSE(room->outdoors());
    CHECK(room->openRoofCount == 0);
    CHECK(room->role == RoomRole::None);
    CHECK(room->bounds.minX == 11);
    CHECK(room->bounds.sizeX == 5);
    CHECK(room->anchor == CellPos{11, 11, 0});
    CHECK(sim.rooms().cellsOf(*room).size() == 25);
    // The roof covers the inside and the walls, and nothing outside.
    CHECK(sim.map().roof(CellPos{13, 13, 0}) == Roof::Thin);
    CHECK(sim.map().roof(CellPos{10, 10, 0}) == Roof::Thin);
    CHECK(sim.map().roof(CellPos{9, 13, 0}) == Roof::None);
    CHECK(roomsCountRoof(sim, Roof::Thin) == 49);
    CHECK(sim.rooms().thinRoofCells().size() == 49);
    CHECK(sim.rooms().indoors(CellPos{13, 13, 0}));
    CHECK_FALSE(sim.rooms().indoors(CellPos{5, 5, 0}));
    // The outdoors is a room too, and it is not roofed.
    const Room* outside = roomsAt(sim, 5, 5);
    ROOMS_REQUIRE(outside != nullptr);
    CHECK(outside->outdoors());
    CHECK(outside->id != room->id);

    // Take a wall down: the room joins the outdoors and the roof goes.
    const uint64_t changes = sim.rooms().roofChanges();
    sim.things().destroy(sim.map().buildingAt(CellPos{10, 13, 0}));
    sim.rooms().update();
    CHECK(sim.rooms().roofChanges() > changes);
    CHECK(roomsCountRoof(sim, Roof::Thin) == 0);
    CHECK(sim.rooms().roomAt(CellPos{13, 13, 0}) == sim.rooms().roomAt(CellPos{5, 5, 0}));
    CHECK_FALSE(sim.rooms().indoors(CellPos{13, 13, 0}));

    // Wall it up again and the roof returns.
    sim.things().spawn("Wall", CellPos{10, 13, 0});
    sim.rooms().update();
    CHECK(roomsCountRoof(sim, Roof::Thin) == 49);
}

TEST_CASE("Rooms: a door is not in a room, it joins the two rooms beside it") {
    Sim sim = roomsSim();
    const CellPos door{13, 10, 0};
    roomsRing(sim, 10, 10, 16, 16, {door});
    sim.things().spawn("Door", door);
    sim.rooms().update();
    const Room* inside = roomsAt(sim, 13, 13);
    ROOMS_REQUIRE(inside != nullptr);
    CHECK(inside->cellCount == 25);  // the door cell isn't one of them
    CHECK(inside->enclosed());
    CHECK_FALSE(inside->outdoors());
    CHECK(sim.rooms().roomAt(door) == kNoRoom);
    const std::vector<RoomId> beside = sim.rooms().roomsBesideDoor(door);
    ROOMS_REQUIRE(beside.size() == 2);
    CHECK(beside[0] != beside[1]);
    CHECK((beside[0] == inside->id || beside[1] == inside->id));
    // The doorway is under the roof like the wall it is in, and the roof holds on it.
    CHECK(sim.map().roof(door) == Roof::Thin);
    CHECK(sim.rooms().indoors(door));
    CHECK(roomsCountRoof(sim, Roof::Thin) == 49);
}

TEST_CASE("Rooms: a room too wide for the roof to span is open sky, and one at the map edge is outdoors") {
    Sim sim = roomsSim(64);
    roomsRing(sim, 5, 5, 26, 26);  // 20 x 20 inside
    const Room* big = roomsAt(sim, 15, 15);
    ROOMS_REQUIRE(big != nullptr);
    CHECK(big->cellCount == 400);
    CHECK(big->enclosed());
    CHECK(big->outdoors());
    CHECK(sim.map().roof(CellPos{15, 15, 0}) == Roof::None);
    CHECK(sim.map().roof(CellPos{6, 15, 0}) == Roof::Thin);
    // The cells left open are those with no wall within 6.9 m.
    int expectedOpen = 0;
    for (int y = 6; y <= 25; ++y) {
        for (int x = 6; x <= 25; ++x) {
            bool near = false;
            for (int wy = 5; wy <= 26 && !near; ++wy) {
                for (int wx = 5; wx <= 26 && !near; ++wx) {
                    const bool ring = wx == 5 || wx == 26 || wy == 5 || wy == 26;
                    near = ring && (wx - x) * (wx - x) + (wy - y) * (wy - y) <= 47;  // 6.9 squared is 47.61
                }
            }
            expectedOpen += near ? 0 : 1;
        }
    }
    CHECK(expectedOpen > 0);
    CHECK(big->openRoofCount == expectedOpen);
    // Beds in open sky don't make a bedroom.
    sim.things().spawn("Bed", CellPos{15, 15, 0});
    sim.step();
    CHECK(roomsAt(sim, 15, 15)->role == RoomRole::None);

    // A pocket in the map's corner reaches the edge: outdoors, never roofed.
    for (int i = 0; i <= 3; ++i) {
        sim.things().spawn("Wall", CellPos{3, i, 0});
        if (i < 3) {
            sim.things().spawn("Wall", CellPos{i, 3, 0});
        }
    }
    sim.rooms().update();
    const Room* pocket = roomsAt(sim, 1, 1);
    ROOMS_REQUIRE(pocket != nullptr);
    CHECK(pocket->cellCount == 9);
    CHECK(pocket->touchesEdge);
    CHECK(pocket->outdoors());
    CHECK(sim.map().roof(CellPos{1, 1, 0}) == Roof::None);
}

TEST_CASE("Rooms: what is in a room gives it a role") {
    Sim sim = roomsSim();
    roomsRing(sim, 10, 10, 16, 16);
    const auto role = [&] {
        sim.step();  // the end of a step brings the rooms up to date
        return roomsAt(sim, 13, 13)->role;
    };
    CHECK(role() == RoomRole::None);
    sim.things().spawn("Table1x2", CellPos{14, 14, 0});
    CHECK(role() == RoomRole::DiningRoom);
    sim.things().spawn("Stool", CellPos{13, 14, 0});
    CHECK(role() == RoomRole::DiningRoom);
    CHECK(roomsAt(sim, 13, 13)->tables == 1);
    CHECK(roomsAt(sim, 13, 13)->chairs == 1);
    sim.things().spawn("FueledStove", CellPos{11, 15, 0});  // 3 wide: still counted once
    CHECK(role() == RoomRole::Kitchen);
    CHECK(roomsAt(sim, 13, 13)->stoves == 1);
    sim.things().spawn("Bed", CellPos{11, 11, 0});  // 1 wide, 2 deep: counted once
    CHECK(role() == RoomRole::Bedroom);
    CHECK(roomsAt(sim, 13, 13)->beds == 1);
    sim.things().spawn("Bed", CellPos{13, 11, 0});
    CHECK(role() == RoomRole::Barracks);
    CHECK(roomsAt(sim, 13, 13)->beds == 2);
    CHECK(sim.rooms().roleAt(CellPos{13, 13, 0}) == RoomRole::Barracks);
    CHECK(sim.rooms().roleAt(CellPos{5, 5, 0}) == RoomRole::None);
    CHECK(sim.rooms().roleAt(CellPos{10, 10, 0}) == RoomRole::None);  // a wall

    // Taking the beds out again.
    sim.things().destroy(sim.map().buildingAt(CellPos{13, 11, 0}));
    sim.things().destroy(sim.map().buildingAt(CellPos{11, 11, 0}));
    CHECK(role() == RoomRole::Kitchen);
    CHECK(std::string(roomRoleName(RoomRole::DiningRoom)) == "DiningRoom");

    // The snapshot lists the enclosed room and the roof.
    Snapshot snap;
    sim.writeSnapshot(snap);
    ROOMS_REQUIRE(snap.rooms.size() == 1);
    CHECK(snap.rooms[0].role == RoomRole::Kitchen);
    CHECK(snap.rooms[0].cellCount == 25);
    CHECK_FALSE(snap.rooms[0].outdoors);
    CHECK(snap.roofCells.size() == 49);
    CHECK(snap.roofChanges == sim.rooms().roofChanges());
}

TEST_CASE("Rooms: colonists build a walled room with a door and it is roofed when the last wall stands") {
    Sim sim = roomsSim(64);
    for (int i = 0; i < 3; ++i) {
        sim.submit(SpawnThingCommand{"Human", CellPos{30 + i, 30, 0}});
    }
    sim.submit(SpawnThingCommand{"Steel", CellPos{28, 28, 0}, 75});
    sim.submit(SpawnThingCommand{"Steel", CellPos{28, 29, 0}, 75});
    sim.step();
    const CellPos door{22, 24, 0};
    for (int y = 20; y <= 24; ++y) {
        for (int x = 20; x <= 24; ++x) {
            if (x == 20 || x == 24 || y == 20 || y == 24) {
                const bool isDoor = CellPos{x, y, 0} == door;
                sim.submit(PlaceBlueprintCommand{isDoor ? "Door" : "Wall", "Steel", CellPos{x, y, 0}, Rot4::North});
            }
        }
    }
    sim.step();
    const auto ringDone = [&] {
        for (int i = 20; i <= 24; ++i) {
            for (const CellPos c : {CellPos{i, 20, 0}, CellPos{i, 24, 0}, CellPos{20, i, 0}, CellPos{24, i, 0}}) {
                if (sim.map().buildingAt(c) == kNoEntity) {
                    return false;
                }
            }
        }
        return true;
    };
    bool roofedEarly = false;
    int steps = 0;
    for (; steps < 12000 && !ringDone(); ++steps) {
        sim.step();
        roofedEarly = roofedEarly || (!ringDone() && sim.map().roof(CellPos{22, 22, 0}) != Roof::None);
    }
    const Room* room = roomsAt(sim, 22, 22);
    ROOMS_REQUIRE(room != nullptr);
    CHECK(steps < 12000);
    CHECK_FALSE(roofedEarly);  // not before the ring was closed
    CHECK(room->cellCount == 9);
    CHECK_FALSE(room->outdoors());
    CHECK(sim.map().roof(CellPos{22, 22, 0}) == Roof::Thin);
    CHECK(sim.map().passage(door) == Passage::Door);
    CHECK(sim.stats().failedJobs < 5);  // builders jostling for places beside the walls lose one now and then
}

TEST_CASE("Doors: a pawn waits for a closed door to open, then the door stays open a while and closes") {
    const DoorNumbers numbers = DoorNumbers::read(*roomsDefs());
    CHECK(numbers.openSteps == 23);  // 45 ticks, and a tick is half a step
    CHECK(numbers.closeDelaySteps == 55);
    CHECK(numbers.pathCost == 3.5f);

    const CellPos doorCell{20, 24, 0};
    const auto wall = [](Sim& sim, bool withDoor) {
        for (int x = 10; x <= 30; ++x) {
            if (x != 20) {
                sim.things().spawn("Wall", CellPos{x, 24, 0});
            }
        }
        return withDoor ? sim.things().spawn("Door", CellPos{20, 24, 0}) : kNoEntity;
    };

    // The same walk through a gap with nothing in it...
    Sim open = roomsSim();
    wall(open, false);
    const Entity walker = open.things().spawn("Human", CellPos{20, 20, 0});
    const int openSteps = roomsWalk(open, walker, CellPos{20, 28, 0});
    ROOMS_REQUIRE(openSteps > 0);

    // ... and through a door.
    Sim sim = roomsSim();
    const Entity door = wall(sim, true);
    ROOMS_REQUIRE(door != kNoEntity);
    CHECK_FALSE(roomsDoor(sim, door).open);
    const Entity pawn = sim.things().spawn("Human", CellPos{20, 20, 0});
    std::vector<CellPos> path;
    ROOMS_REQUIRE(sim.pathfinder().findPath(CellPos{20, 20, 0}, CellPos{20, 28, 0}, path));
    const Position start = sim.things().registry().get<Position>(pawn);
    sim.things().registry().get<Mover>(pawn).walkTo(start.x, start.y, path);
    int doorSteps = -1;
    bool enteredClosed = false;
    bool sawHalfOpen = false;
    Snapshot snap;
    for (int i = 0; i < 600 && doorSteps < 0; ++i) {
        const Door& state = roomsDoor(sim, door);
        const CellPos at = cellOf(sim.things().registry().get<Position>(pawn));
        enteredClosed = enteredClosed || (at == doorCell && !state.open && i > 0 && state.progress == 0 && i < 40);
        if (state.progress > 0 && !state.open) {
            sim.writeSnapshot(snap);
            const float f = snap.find(door)->doorOpen;
            sawHalfOpen = sawHalfOpen || (f > 0.0f && f < 1.0f);
        }
        if (sim.things().registry().get<Mover>(pawn).arrived()) {
            doorSteps = i;
        }
        sim.step();
    }
    ROOMS_REQUIRE(doorSteps > 0);
    CHECK_FALSE(enteredClosed);
    CHECK(sawHalfOpen);
    // The door cost the pawn about the time it took to open, not more, not less.
    CHECK(doorSteps - openSteps >= numbers.openSteps - 4);
    CHECK(doorSteps - openSteps <= numbers.openSteps + 6);
    CHECK(sim.stats().repaths == 0);

    // The pawn has gone through; the door closes after the delay.
    CHECK(roomsDoor(sim, door).open);
    sim.writeSnapshot(snap);
    CHECK(snap.find(door)->doorOpen == 1.0f);
    sim.stepMany(numbers.closeDelaySteps + 5);
    CHECK_FALSE(roomsDoor(sim, door).open);
    sim.writeSnapshot(snap);
    CHECK(snap.find(door)->doorOpen == 0.0f);
}

TEST_CASE("Doors: a second pawn passes an open door without waiting, and a pawn in the doorway holds it open") {
    Sim sim = roomsSim();
    for (int x = 10; x <= 30; ++x) {
        if (x != 20) {
            sim.things().spawn("Wall", CellPos{x, 24, 0});
        }
    }
    const Entity door = sim.things().spawn("Door", CellPos{20, 24, 0});
    const Entity first = sim.things().spawn("Human", CellPos{20, 21, 0});
    const Entity second = sim.things().spawn("Human", CellPos{20, 19, 0});
    ROOMS_REQUIRE(roomsWalk(sim, first, CellPos{20, 27, 0}) > 0);
    CHECK(roomsDoor(sim, door).open);
    const int hold = roomsDoor(sim, door).hold;
    CHECK(hold > 0);
    // The second pawn reaches the door while it is still open: no wait.
    const int t = roomsWalk(sim, second, CellPos{20, 27, 0});
    ROOMS_REQUIRE(t > 0);
    CHECK(t < 60);  // 8 m at 4.6 m/s is about 54 steps; a wait would add 23

    // A pawn that stands in the doorway keeps it open for as long as it stays.
    const Entity waiter = sim.things().spawn("Human", CellPos{20, 24, 0});
    sim.stepMany(300);
    CHECK(roomsDoor(sim, door).open);
    sim.things().destroy(waiter);
    sim.stepMany(100);
    CHECK_FALSE(roomsDoor(sim, door).open);
}

TEST_CASE("Doors: a path takes a gap a few metres longer rather than a door, but not a far one") {
    const auto crosses = [](int gapX) {
        Sim sim = roomsSim();
        for (int x = 10; x <= 34; ++x) {
            if (x != 20 && x != gapX) {
                sim.things().spawn("Wall", CellPos{x, 24, 0});
            }
        }
        sim.things().spawn("Door", CellPos{20, 24, 0});
        std::vector<CellPos> path;
        const bool found = sim.pathfinder().findPath(CellPos{20, 20, 0}, CellPos{20, 28, 0}, path);
        const bool viaDoor = std::find(path.begin(), path.end(), CellPos{20, 24, 0}) != path.end();
        const bool viaGap = std::find(path.begin(), path.end(), CellPos{gapX, 24, 0}) != path.end();
        return found && viaDoor != viaGap ? (viaDoor ? 1 : 2) : 0;
    };
    CHECK(crosses(22) == 2);  // 9.7 m through the gap beats 8 m plus 3.5 for the door
    CHECK(crosses(26) == 1);  // 15.3 m doesn't
}

TEST_CASE("Doors: the state of every door is saved, and a loaded game goes on the same") {
    // A pawn walks through a door; the game is saved when the door is opening and when it is open.
    for (const bool saveWhenOpen : {false, true}) {
        Sim original = roomsSim();
        for (int x = 10; x <= 30; ++x) {
            if (x != 20) {
                original.things().spawn("Wall", CellPos{x, 24, 0});
            }
        }
        const Entity door = original.things().spawn("Door", CellPos{20, 24, 0});
        const Entity pawn = original.things().spawn("Human", CellPos{20, 20, 0});
        std::vector<CellPos> path;
        ROOMS_REQUIRE(original.pathfinder().findPath(CellPos{20, 20, 0}, CellPos{20, 28, 0}, path));
        const Position start = original.things().registry().get<Position>(pawn);
        original.things().registry().get<Mover>(pawn).walkTo(start.x, start.y, path);
        for (int i = 0; i < 400 && !(saveWhenOpen ? roomsDoor(original, door).open : roomsDoor(original, door).progress > 3); ++i) {
            original.step();
        }
        if (saveWhenOpen) {
            original.stepMany(10);
        }
        ROOMS_REQUIRE(roomsDoor(original, door).open == saveWhenOpen);
        ROOMS_REQUIRE(roomsDoor(original, door).progress > 0);

        const std::vector<uint8_t> bytes = saveSim(original);
        SaveLoadResult loaded = loadSim(bytes, roomsDefs());
        ROOMS_REQUIRE(loaded.error.empty());
        ROOMS_REQUIRE(loaded.sim != nullptr);
        CHECK(loaded.sim->stateHash() == original.stateHash());
        CHECK(saveSim(*loaded.sim) == bytes);
        const Entity again = loaded.sim->map().buildingAt(CellPos{20, 24, 0});
        ROOMS_REQUIRE(again != kNoEntity);
        CHECK(roomsDoor(*loaded.sim, again).open == roomsDoor(original, door).open);
        CHECK(roomsDoor(*loaded.sim, again).progress == roomsDoor(original, door).progress);
        CHECK(roomsDoor(*loaded.sim, again).hold == roomsDoor(original, door).hold);
        CHECK(loaded.sim->rooms().rooms().size() == original.rooms().rooms().size());
        for (int i = 0; i < 200; ++i) {
            original.step();
            loaded.sim->step();
            ROOMS_REQUIRE(loaded.sim->stateHash() == original.stateHash());
        }
        CHECK_FALSE(roomsDoor(original, door).open);  // it closed again, in both
        CHECK(saveSim(*loaded.sim) == saveSim(original));
        CHECK(saveToJson(original).find("\"door\"") != std::string::npos);
    }
}

TEST_CASE("Doors: an open door lets a pawn see through, a closed one blocks sight, also after a load") {
    Sim sim = roomsSim();
    for (int x = 10; x <= 30; ++x) {
        if (x != 20) {
            sim.things().spawn("Wall", CellPos{x, 24, 0});
        }
    }
    const Entity door = sim.things().spawn("Door", CellPos{20, 24, 0});
    const Viewpoint north = Viewpoint::at(CellPos{20, 28, 0}, Stance::Standing);
    const Viewpoint south = Viewpoint::at(CellPos{20, 20, 0}, Stance::Standing);
    CHECK_FALSE(sim.cover().lineOfSight(north, south));
    CHECK(sim.map().obstacleDm(CellPos{20, 24, 0}) > 0);

    const Entity pawn = sim.things().spawn("Human", CellPos{20, 22, 0});
    ROOMS_REQUIRE(roomsWalk(sim, pawn, CellPos{20, 26, 0}) > 0);
    ROOMS_REQUIRE(roomsDoor(sim, door).open);
    CHECK(sim.map().obstacleDm(CellPos{20, 24, 0}) == 0);
    CHECK(sim.cover().lineOfSight(north, south));

    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, roomsDefs());
    ROOMS_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->cover().lineOfSight(north, south));

    sim.stepMany(100);
    CHECK_FALSE(roomsDoor(sim, door).open);
    CHECK_FALSE(sim.cover().lineOfSight(north, south));
    loaded.sim->stepMany(100);
    CHECK_FALSE(loaded.sim->cover().lineOfSight(north, south));
    CHECK(loaded.sim->stateHash() == sim.stateHash());
}

TEST_CASE("Rooms: waking up outside starts the slept outside thought, indoors does not") {
    SimSettings settings{3, 96, 0, 0, true};
    settings.scenario = "Crashlanded";
    Sim sim(settings, roomsDefs());
    sim.step();  // the colonists get their mood
    Entity pawn = kNoEntity;
    for (const auto [entity, mood] : sim.things().registry().view<const Mood>().each()) {
        pawn = entity;
        break;
    }
    ROOMS_REQUIRE(pawn != kNoEntity);
    roomsRing(sim, 10, 10, 16, 16);
    const auto sleptOutside = [&](CellPos cell) {
        Event woke;
        woke.type = EventType::PawnWokeUp;
        woke.thing = pawn;
        woke.cell = cell;
        woke.message = "bed";
        std::vector<Event> events{woke};
        sim.mood().step(sim.time().seconds, events);
        for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
            if (t.label == "slept outside") {
                return true;
            }
        }
        return false;
    };
    CHECK_FALSE(sleptOutside(CellPos{13, 13, 0}));
    CHECK(sleptOutside(CellPos{5, 5, 0}));
}

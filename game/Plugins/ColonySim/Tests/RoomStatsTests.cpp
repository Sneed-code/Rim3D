#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define RS_REQUIRE(...)        \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the floors, the filth kinds and the rules are what is tested.
std::shared_ptr<const DefDatabase> rsDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim rsSim(int size = 48) { return Sim(SimSettings{5, size, 0, 0, true}, rsDefs()); }

// A wall ring round x0..x1 by y0..y1; the room is the inside (5 by 5 for 10..16).
void rsRing(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    sim.rooms().update();
    sim.roomStats().refresh();
}

const RoomStatValues* rsStats(Sim& sim, int x, int y) {
    sim.roomStats().refresh();
    return sim.roomStats().statsAt(CellPos{x, y, 0});
}

Entity rsColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    return pawn;
}

}  // namespace

TEST_CASE("Room stats: impressiveness follows the wiki's worked example") {
    const RoomStatNumbers numbers = RoomStatNumbers::read(*rsDefs());
    // Wealth 3000, space 25, cleanliness 0, beauty 3: "somewhat impressive", 70.
    const float value = roomImpressiveness(numbers, 3000.0f, 3.0f, 25.0f, 0.0f);
    CHECK(std::floor(value) == doctest::Approx(70.0f));
    CHECK(std::string(impressivenessLabel(value)) == "somewhat impressive");
    // The space cap: a huge empty room with great everything else is held down by its small space.
    CHECK(roomImpressiveness(numbers, 1.0e6f, 300.0f, 25.0f, 5.0f) < roomImpressiveness(numbers, 1.0e6f, 300.0f, 2500.0f, 5.0f));
}

TEST_CASE("Room stats: labels are the wiki's stages") {
    CHECK(std::string(spaceLabel(12.4f)) == "cramped");
    CHECK(std::string(spaceLabel(12.5f)) == "rather tight");
    CHECK(std::string(spaceLabel(35.0f)) == "average-sized");
    CHECK(std::string(beautyLabel(-4.0f)) == "hideous");
    CHECK(std::string(beautyLabel(2.4f)) == "pretty");
    CHECK(std::string(wealthLabel(699.0f)) == "somewhat poor");
    CHECK(std::string(cleanlinessLabel(-0.5f)) == "dirty");
    CHECK(std::string(cleanlinessLabel(0.4f)) == "sterile");
    CHECK(std::string(impressivenessLabel(19.9f)) == "awful");
    CHECK(std::string(impressivenessLabel(240.0f)) == "wondrously impressive");
}

TEST_CASE("Room stats: an empty five by five room has 35 space and bare soil under it") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    const RoomStatValues* stats = rsStats(sim, 13, 13);
    RS_REQUIRE(stats != nullptr);
    CHECK(stats->space == doctest::Approx(35.0f));
    // Soil is -3 indoors in 25 cells; the walls round it add nothing; the room counts as 20 + 25 / 2 cells.
    CHECK(stats->beauty == doctest::Approx(-75.0f / 32.5f));
    CHECK(stats->cleanliness == doctest::Approx(-1.0f));
    CHECK(stats->wealth > 0.0f);  // the walls round it
    CHECK(sim.roomStats().statsAt(CellPos{5, 5, 0}) == nullptr);  // the outdoors has none
}

TEST_CASE("Room stats: furniture you can't stand on takes space, a stool doesn't") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    sim.things().spawn("Stool", CellPos{11, 11, 0});
    RS_REQUIRE(rsStats(sim, 13, 13) != nullptr);
    CHECK(rsStats(sim, 13, 13)->space == doctest::Approx(35.0f));
    sim.things().spawn("Table1x2", CellPos{13, 13, 0});
    CHECK(rsStats(sim, 13, 13)->space == doctest::Approx(35.0f - 1.8f));
}

TEST_CASE("Room stats: floors raise beauty, cleanliness and wealth") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    const RoomStatValues before = *rsStats(sim, 13, 13);
    sim.submit(PlaceFloorCommand{CellRect{11, 11, 5, 5, 0}, "StoneTile"});
    sim.step();
    const RoomStatValues* after = rsStats(sim, 13, 13);
    RS_REQUIRE(after != nullptr);
    CHECK(sim.floors().count() == 25);
    CHECK(after->beauty == doctest::Approx(25.0f / 32.5f));  // 1 a cell
    CHECK(after->cleanliness == doctest::Approx(0.2f));
    CHECK(after->wealth > before.wealth);
    CHECK(after->impressiveness > before.impressiveness);
    // Taking the floors off puts the soil back.
    sim.submit(PlaceFloorCommand{CellRect{11, 11, 5, 5, 0}, ""});
    sim.step();
    CHECK(sim.floors().count() == 0);
    CHECK(rsStats(sim, 13, 13)->beauty == doctest::Approx(before.beauty));
    // Unknown floors, and floors that change nothing, fail.
    const size_t failed = sim.events().size();
    (void)failed;
    sim.submit(PlaceFloorCommand{CellRect{11, 11, 1, 1, 0}, "Granite_Rough"});  // not a floor def
    sim.step();
    bool sawFailure = false;
    for (const Event& e : sim.events()) {
        sawFailure = sawFailure || e.type == EventType::CommandFailed;
    }
    CHECK(sawFailure);
}

TEST_CASE("Filth: layers stack to a limit, lower beauty and cleanliness, and clean off one at a time") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    const CellPos cell{13, 13, 0};
    const float clean = sim.roomStats().cellBeauty(cell);
    CHECK(sim.filth().add(cell, "Dirt"));
    CHECK(sim.filth().add(cell, "Dirt", 5));  // up to the limit
    RS_REQUIRE(sim.filth().at(cell).size() == 1);
    CHECK(sim.filth().at(cell)[0].thickness == 3);
    CHECK_FALSE(sim.filth().add(cell, "Dirt"));  // already as thick as it gets
    CHECK(sim.filth().add(cell, "Blood"));
    CHECK(sim.filth().totalLayers() == 4);
    // Indoors the beauty is the filth's own: dirt -15 and blood -30. The pile's thickness doesn't scale it.
    CHECK(sim.roomStats().cellBeauty(cell) == doctest::Approx(clean - 45.0f));
    CHECK(sim.roomStats().cellCleanliness(cell) == doctest::Approx(-1.0f - 5.0f - 10.0f));
    // Outdoors it counts 30%.
    const CellPos out{30, 30, 0};
    const float outClean = sim.roomStats().cellBeauty(out);
    CHECK(sim.filth().add(out, "Dirt"));
    CHECK(sim.roomStats().cellBeauty(out) == doctest::Approx(outClean - 4.5f));
    // One layer at a time, the first pile first: dirt has 3 layers.
    CHECK(sim.filth().clean(cell));
    CHECK(sim.filth().at(cell)[0].thickness == 2);
    CHECK(sim.filth().clean(cell));
    CHECK(sim.filth().clean(cell));
    CHECK(sim.filth().at(cell).size() == 1);  // dirt gone, blood left
    CHECK(sim.filth().clean(cell));
    CHECK_FALSE(sim.filth().has(cell));
    CHECK_FALSE(sim.filth().clean(cell));
    // Not on rock, water or a wall.
    CHECK_FALSE(sim.filth().add(CellPos{10, 10, 0}, "Dirt"));
    CHECK_FALSE(sim.filth().add(CellPos{-1, 0, 0}, "Dirt"));
    CHECK_FALSE(sim.filth().add(cell, "NoSuchFilth"));
}

TEST_CASE("Filth: layers under the open sky fade over days, quicker in rain, and those under a roof stay") {
    const auto fill = [](Sim& sim) {
        rsRing(sim, 10, 10, 16, 16);
        for (int i = 0; i < 20; ++i) {
            sim.filth().add(CellPos{20 + i, 30, 0}, "Blood", 3);
            sim.filth().add(CellPos{20 + i, 20, 0}, "AnimalFilth", 3);
        }
        for (int x = 11; x <= 15; ++x) {
            sim.filth().add(CellPos{x, 13, 0}, "Blood", 3);
        }
    };
    const auto outdoorLayers = [](Sim& sim) {
        size_t total = 0;
        for (int i = 0; i < 20; ++i) {
            for (const int y : {20, 30}) {
                for (const FilthPile& pile : sim.filth().at(CellPos{20 + i, y, 0})) {
                    total += pile.thickness;
                }
            }
        }
        return total;
    };
    Sim dry = rsSim();
    Sim wet = rsSim();
    fill(dry);
    fill(wet);
    const size_t start = outdoorLayers(dry);
    CHECK(start == 120);
    for (int hour = 0; hour < 72; ++hour) {
        dry.filth().fadeHour(0.0f);
        wet.filth().fadeHour(1.0f);
    }
    // After three days of a mean six: some is gone, not all, and rain takes more.
    CHECK(outdoorLayers(dry) < start);
    CHECK(outdoorLayers(dry) > start / 5);
    CHECK(outdoorLayers(wet) < outdoorLayers(dry));
    for (int hour = 0; hour < 24 * 60; ++hour) {
        dry.filth().fadeHour(0.0f);
    }
    CHECK(outdoorLayers(dry) == 0);
    // The blood inside the roofed room is still there.
    CHECK(dry.filth().totalLayers() == 15);
}

TEST_CASE("Filth: dirt stuck to shoes on bare ground falls off on a floor") {
    Sim sim = rsSim(64);
    // A floor strip 10 cells long next to bare ground, walked back and forth.
    sim.submit(PlaceFloorCommand{CellRect{20, 20, 10, 1, 0}, "WoodFloor"});
    sim.step();
    const Entity pawn = sim.things().spawn("Human", CellPos{19, 20, 0});
    RS_REQUIRE(pawn != kNoEntity);
    int64_t steps = 0;
    for (int trip = 0; trip < 120 && steps < 40000; ++trip) {
        for (const CellPos goal : {CellPos{19, 24, 0}, CellPos{29, 20, 0}, CellPos{19, 20, 0}}) {
            std::vector<CellPos> path;
            const Position pos = sim.things().registry().get<Position>(pawn);
            RS_REQUIRE(sim.pathfinder().findPath(cellOf(pos), goal, path));
            sim.things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, path);
            for (int i = 0; i < 400 && !sim.things().registry().get<Mover>(pawn).arrived(); ++i, ++steps) {
                sim.step();
            }
        }
    }
    int onFloor = 0;
    int onGround = 0;
    for (const auto& [index, piles] : sim.filth().all()) {
        (void)piles;
        const CellPos c = sim.map().cellAt(index);
        (sim.floors().at(c).valid() ? onFloor : onGround) += 1;
    }
    CHECK(onFloor > 0);
    CHECK(onGround == 0);  // dirt is dropped on floors only
}

TEST_CASE("Filth: a bleeding pawn leaves blood") {
    Sim sim = rsSim();
    const Entity pawn = sim.things().spawn("Human", CellPos{20, 20, 0});
    RS_REQUIRE(pawn != kNoEntity);
    sim.submit(DamagePawnCommand{pawn, "Cut", 12.0f, -1});
    for (int i = 0; i < 4 * 3600; ++i) {
        sim.step();
        if (sim.filth().totalLayers() > 0) {
            break;
        }
    }
    const int blood = sim.filth().kindIndex("Blood");
    RS_REQUIRE(blood >= 0);
    bool found = false;
    for (const auto& [index, piles] : sim.filth().all()) {
        (void)index;
        for (const FilthPile& pile : piles) {
            found = found || pile.kind == blood;
        }
    }
    CHECK(found);
}

TEST_CASE("Cleaning: a colonist cleans indoor filth a layer at a time, and leaves the outdoors") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    sim.filth().add(CellPos{13, 13, 0}, "Dirt", 2);
    sim.filth().add(CellPos{40, 40, 0}, "Dirt", 1);  // outdoors: waits for the home area
    const Entity pawn = rsColonist(sim, CellPos{11, 11, 0});
    bool cleaned = false;
    int64_t at = 0;
    for (int i = 0; i < 1200; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::PawnAction && e.action == PawnAction::Clean) {
                cleaned = true;
            }
        }
        if (!sim.filth().has(CellPos{13, 13, 0})) {
            at = i;
            break;
        }
    }
    CHECK(cleaned);
    // Two layers of dirt, 35 ticks each at 1.44 seconds a tick, plus the walk.
    CHECK(at >= 100);
    CHECK(at < 200);
    CHECK(sim.filth().has(CellPos{40, 40, 0}));
    (void)pawn;
    // It is a job of the Cleaning work type: with Cleaning off nobody does it.
    sim.filth().add(CellPos{12, 12, 0}, "Blood");
    sim.submit(SetWorkPriorityCommand{pawn, "Cleaning", 0});
    sim.stepMany(900);
    CHECK(sim.filth().has(CellPos{12, 12, 0}));
    sim.submit(SetWorkPriorityCommand{pawn, "Cleaning", 1});
    sim.stepMany(900);
    CHECK_FALSE(sim.filth().has(CellPos{12, 12, 0}));
}

TEST_CASE("Beauty need: a colonist starts at what it sees, and filth drags it down slowly") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    sim.submit(PlaceFloorCommand{CellRect{11, 11, 5, 5, 0}, "StoneTile"});
    const Entity pawn = rsColonist(sim, CellPos{13, 13, 0});
    sim.stepMany(2);
    const BeautyNeed* need = sim.roomStats().beautyNeed(pawn);
    RS_REQUIRE(need != nullptr);
    // Stone floors (1) in the room and walls round it: it sees a nice place.
    CHECK(need->perceived > 0.5f);
    CHECK(need->level > 0.55f);
    const float start = need->level;
    // The same room under blood and dirt is ugly; the need falls, but no faster than 4% an hour.
    for (int y = 11; y <= 15; ++y) {
        for (int x = 11; x <= 15; ++x) {
            sim.filth().add(CellPos{x, y, 0}, "Vomit", 3);
        }
    }
    sim.submit(SetWorkPriorityCommand{pawn, "Cleaning", 0});
    sim.stepMany(3600);
    need = sim.roomStats().beautyNeed(pawn);
    RS_REQUIRE(need != nullptr);
    CHECK(need->perceived < -10.0f);
    CHECK(need->level < start);
    CHECK(need->level > start - 0.05f);
    // The snapshot has it.
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    RS_REQUIRE(view != nullptr);
    CHECK(view->beautyNeed == doctest::Approx(need->level));
}

TEST_CASE("Beauty need: the thought has a stage for each band of the need") {
    Sim sim = rsSim();
    const Entity pawn = rsColonist(sim, CellPos{20, 20, 0});
    sim.stepMany(2);
    const auto thoughtOffset = [&](float level) {
        RoomStatsState state = sim.roomStats().state();
        for (RoomStatsState::Need& n : state.needs) {
            n.level = level;
            n.sampledAt = sim.time().seconds;  // not sampled again at once
        }
        std::string error;
        sim.roomStats().restore(state, error);
        for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
            if (t.def == "Beauty") {
                return t.offset;
            }
        }
        return 0.0f;
    };
    CHECK(thoughtOffset(0.0f) == doctest::Approx(-15.0f));
    CHECK(thoughtOffset(0.1f) == doctest::Approx(-10.0f));
    CHECK(thoughtOffset(0.2f) == doctest::Approx(-5.0f));
    CHECK(thoughtOffset(0.5f) == doctest::Approx(0.0f));
    CHECK(thoughtOffset(0.7f) == doctest::Approx(5.0f));
    CHECK(thoughtOffset(0.9f) == doctest::Approx(10.0f));
    CHECK(thoughtOffset(1.0f) == doctest::Approx(15.0f));
}

TEST_CASE("Room thoughts: a bedroom's impressiveness, cleanliness and space come from its stats") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    sim.things().spawn("Bed", CellPos{11, 11, 0});
    const Entity pawn = rsColonist(sim, CellPos{14, 14, 0});
    sim.stepMany(2);
    const Room* room = sim.rooms().roomOf(CellPos{14, 14, 0});
    RS_REQUIRE(room != nullptr);
    CHECK(room->role == RoomRole::Bedroom);
    const RoomStatValues* stats = sim.roomStats().statsAt(CellPos{14, 14, 0});
    RS_REQUIRE(stats != nullptr);
    float impressive = 0.0f;
    float space = 0.0f;
    float dirty = 0.0f;
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == "RoomImpressive") impressive = t.offset;
        if (t.def == "Space") space = t.offset;
        if (t.def == "Filthy") dirty = t.offset;
    }
    // A bare little bedroom on soil: under 20 is "awful" (-2, bedrooms only), under 12.5 space is cramped (-3),
    // and cleanliness -1 (soil) is "dirty" only under -1.1, so no thought.
    CHECK(stats->impressiveness < 20.0f);
    CHECK(impressive == doctest::Approx(-2.0f));
    CHECK(stats->space > 12.5f);
    CHECK(space == doctest::Approx(0.0f));
    CHECK(dirty == doctest::Approx(0.0f));
    // Dirt makes it dirty.
    for (int x = 11; x <= 15; ++x) {
        sim.filth().add(CellPos{x, 13, 0}, "Vomit", 1);
        sim.filth().add(CellPos{x, 14, 0}, "Vomit", 1);
        sim.filth().add(CellPos{x, 12, 0}, "Vomit", 1);
    }
    sim.stepMany(2);
    dirty = 0.0f;
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == "Filthy") dirty = t.offset;
    }
    CHECK(sim.roomStats().statsAt(CellPos{14, 14, 0})->cleanliness < -1.1f);
    CHECK(dirty == doctest::Approx(-3.0f));
    // The outdoors has no room thoughts at all.
    const Entity outside = rsColonist(sim, CellPos{30, 30, 0});
    sim.stepMany(2);
    for (const ThoughtView& t : sim.mood().thoughts(outside, sim.time().seconds)) {
        CHECK(t.def != "RoomImpressive");
        CHECK(t.def != "Space");
        CHECK(t.def != "Filthy");
    }
}

TEST_CASE("Room stats: floors, filth and needs survive a save, and the loaded game agrees step for step") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    sim.submit(PlaceFloorCommand{CellRect{11, 11, 5, 5, 0}, "WoodFloor"});
    sim.submit(AddFilthCommand{CellPos{12, 12, 0}, "Dirt", 2});
    const Entity pawn = rsColonist(sim, CellPos{14, 14, 0});
    sim.stepMany(300);
    RS_REQUIRE(sim.roomStats().beautyNeed(pawn) != nullptr);
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, rsDefs());
    RS_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(loaded.sim->floors().count() == 25);
    CHECK(loaded.sim->filth().totalLayers() == sim.filth().totalLayers());
    sim.stepMany(2000);
    loaded.sim->stepMany(2000);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(loaded.sim->roomStats().beautyNeed(pawn)->level == doctest::Approx(sim.roomStats().beautyNeed(pawn)->level));
}

TEST_CASE("Room stats: the same seed and commands give the same game") {
    const auto run = [] {
        Sim sim = rsSim();
        rsRing(sim, 10, 10, 16, 16);
        sim.submit(PlaceFloorCommand{CellRect{11, 11, 5, 5, 0}, "StoneTile"});
        rsColonist(sim, CellPos{14, 14, 0});
        sim.filth().add(CellPos{12, 12, 0}, "Blood", 1);
        sim.stepMany(1500);
        return sim.stateHash();
    };
    CHECK(run() == run());
}

TEST_CASE("Room stats: sterile tiles make a room very clean and a little ugly") {
    Sim sim = rsSim();
    rsRing(sim, 10, 10, 16, 16);
    sim.submit(PlaceFloorCommand{CellRect{11, 11, 5, 5, 0}, "SterileTile"});
    sim.step();
    const RoomStatValues* stats = rsStats(sim, 13, 13);
    RS_REQUIRE(stats != nullptr);
    CHECK(stats->cleanliness == doctest::Approx(0.6f));
    CHECK(stats->beauty < 0.0f);
    const Def& tile = rsDefs()->get(rsDefs()->idOf("Terrain", "SterileTile"));
    CHECK(tile.text("researchPrerequisite") == "SterileMaterials");
    CHECK(tile.integer("costs.Steel") == 3);
    CHECK(tile.integer("costs.Silver") == 12);
}

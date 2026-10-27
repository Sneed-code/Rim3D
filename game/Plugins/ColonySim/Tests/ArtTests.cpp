#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define ART_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> artDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim artSim() { return Sim(SimSettings{5, 48, 0, 0, true}, artDefs()); }

// A finished sculpture of a def, stuff and quality (-1: none rolled, so normal), standing in a cell.
Entity artPlace(Sim& sim, const std::string& def, const std::string& stuff, int quality, CellPos cell) {
    const Entity e = sim.things().spawn(def, cell);
    if (e == kNoEntity) {
        return e;
    }
    sim.things().registry().emplace<BuiltOf>(e, BuiltOf{sim.defs().idOf("Things", stuff)});
    if (quality >= 0) {
        sim.things().registry().emplace<Quality>(e, Quality{static_cast<uint8_t>(quality)});
    }
    return e;
}

void artRing(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    sim.rooms().update();
}

}  // namespace

TEST_CASE("Art: sculpture beauty follows the wiki's table of sizes, stuffs and qualities") {
    Sim sim = artSim();
    const RoomStats& stats = sim.roomStats();
    const auto beauty = [&](const char* def, const char* stuff, int quality, int x) {
        const Entity e = artPlace(sim, def, stuff, quality, CellPos{x, 5, 0});
        return e == kNoEntity ? -1000.0f : stats.buildingBeauty(e, false);
    };
    // Normal steel: 50, 100 and 400 for small, large and grand.
    CHECK(beauty("SculptureSmall", "Steel", -1, 5) == doctest::Approx(50.0f));
    CHECK(beauty("SculptureLarge", "Steel", 2, 7) == doctest::Approx(100.0f));
    CHECK(beauty("SculptureGrand", "Steel", 2, 10) == doctest::Approx(400.0f));
    // Quality multiplies it: awful -10%, poor 50%, good 200%, excellent 300%, masterwork 500%, legendary 800%.
    CHECK(beauty("SculptureSmall", "Steel", 0, 14) == doctest::Approx(-5.0f));
    CHECK(beauty("SculptureSmall", "Steel", 1, 16) == doctest::Approx(25.0f));
    CHECK(beauty("SculptureSmall", "Steel", 3, 18) == doctest::Approx(100.0f));
    CHECK(beauty("SculptureSmall", "Steel", 4, 20) == doctest::Approx(150.0f));
    CHECK(beauty("SculptureSmall", "Steel", 5, 22) == doctest::Approx(250.0f));
    CHECK(beauty("SculptureSmall", "Steel", 6, 24) == doctest::Approx(400.0f));
    // The stuff: wood is as steel, sandstone blocks 1.1, marble blocks 1.35 plus a flat 0.5 (68 for a small one).
    CHECK(beauty("SculptureSmall", "WoodLog", 2, 26) == doctest::Approx(50.0f));
    CHECK(beauty("SculptureSmall", "BlocksSandstone", 2, 28) == doctest::Approx(55.0f));
    CHECK(beauty("SculptureSmall", "BlocksMarble", 2, 30) == doctest::Approx(68.0f));
    // A building that is not art ignores quality.
    const Entity stool = artPlace(sim, "Stool", "WoodLog", 6, CellPos{32, 5, 0});
    ART_REQUIRE(stool != kNoEntity);
    CHECK(stats.buildingBeauty(stool, false) == doctest::Approx(0.0f));
}

TEST_CASE("Art: a sculpture makes a room beautiful, wealthy and impressive, and quality matters") {
    const auto roomWith = [](int quality) {
        Sim sim = artSim();
        artRing(sim, 10, 10, 16, 16);
        sim.roomStats().refresh();
        const RoomStatValues empty = *sim.roomStats().statsAt(CellPos{13, 13, 0});
        artPlace(sim, "SculptureLarge", "Steel", quality, CellPos{12, 12, 0});
        sim.stepMany(2);
        sim.roomStats().refresh();
        const RoomStatValues* with = sim.roomStats().statsAt(CellPos{13, 13, 0});
        struct Result {
            RoomStatValues empty, with;
        };
        return Result{empty, with != nullptr ? *with : RoomStatValues{}};
    };
    const auto normal = roomWith(2);
    // 100 beauty over a room of 20 + 25 / 2 = 32.5 cells.
    CHECK(normal.with.beauty - normal.empty.beauty == doctest::Approx(100.0f / 32.5f).epsilon(0.02));
    CHECK(normal.with.wealth > normal.empty.wealth + 200.0f);
    CHECK(normal.with.impressiveness > normal.empty.impressiveness);
    const auto awful = roomWith(0);
    const auto legendary = roomWith(6);
    CHECK(awful.with.beauty < normal.empty.beauty);  // an awful sculpture is ugly
    CHECK(legendary.with.beauty > normal.with.beauty);
    CHECK(legendary.with.wealth > normal.with.wealth);  // five times the value
    CHECK(awful.with.wealth < normal.with.wealth);
}

TEST_CASE("Art: an artist builds a sculpture, it gets a quality and the Art work type gates it") {
    Sim sim = artSim();
    const Entity pawn = sim.things().spawn("Human", CellPos{10, 10, 0});
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Artistic", 20, Passion::None, 0.0f, false});
    colonist.skills.push_back(SkillRecord{"Construction", 0, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    sim.things().spawn("WoodLog", CellPos{8, 8, 0}, 50);
    sim.submit(PlaceBlueprintCommand{"SculptureSmall", "WoodLog", CellPos{14, 10, 0}, Rot4::North});
    sim.step();
    ART_REQUIRE(sim.construction().siteAt(CellPos{14, 10, 0}) != kNoEntity);

    // Work is 25920 x wood's 0.7 = 18144, at 1.7 a second times the artist's speed (0.3 + 20 x 0.0875).
    CHECK(sim.construction().workToBuild(sim.defs().idOf("Things", "SculptureSmall"), sim.defs().idOf("Things", "WoodLog")) ==
          doctest::Approx(18144.0f));

    // Without the Art work type nobody builds it (the frame is not Construction's work).
    sim.submit(SetWorkPriorityCommand{pawn, "Art", 0});
    sim.stepMany(900);
    CHECK(sim.map().buildingAt(CellPos{14, 10, 0}) == kNoEntity);
    CHECK(sim.construction().siteAt(CellPos{14, 10, 0}) != kNoEntity);

    sim.submit(SetWorkPriorityCommand{pawn, "Art", 1});
    bool built = false;
    for (int i = 0; i < 12000 && !built; ++i) {
        sim.step();
        built = sim.map().buildingAt(CellPos{14, 10, 0}) != kNoEntity && sim.construction().siteAt(CellPos{14, 10, 0}) == kNoEntity;
    }
    ART_REQUIRE(built);
    const Entity sculpture = sim.map().buildingAt(CellPos{14, 10, 0});
    const entt::registry& reg = sim.things().registry();
    CHECK(reg.get<BuiltOf>(sculpture).stuff == sim.defs().idOf("Things", "WoodLog"));
    CHECK(reg.all_of<Quality>(sculpture));
    // At skill 20 the table never gives awful.
    CHECK(reg.get<Quality>(sculpture).level >= 1);
    // The artist, not the builder, learned from it.
    const Colonist& after = reg.get<Colonist>(pawn);
    CHECK(after.skill("Artistic")->xpToday > 0.0f);
    CHECK(after.skill("Construction")->xpToday == doctest::Approx(0.0f));

    // The quality is in the save.
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, artDefs());
    ART_REQUIRE(loaded.error.empty());
    ART_REQUIRE(loaded.sim != nullptr);
    const Entity again = loaded.sim->map().buildingAt(CellPos{14, 10, 0});
    ART_REQUIRE(again != kNoEntity);
    CHECK(loaded.sim->things().registry().get<Quality>(again).level == reg.get<Quality>(sculpture).level);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
}

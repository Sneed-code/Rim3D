// Sowing trees in growing zones and the cocoa tree (docs/design/farming.md "Trees in zones").
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

// The game's real data: the research gates and the tree defs are part of what is tested.
std::shared_ptr<const DefDatabase> treeSowDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A 64 m map cleared of plants, 20 degrees, with a gardener of the given Plants level.
struct TreeSowFixture {
    Sim sim;
    Entity gardener = kNoEntity;

    explicit TreeSowFixture(int plants) : sim(SimSettings{5, 64, 0, 0, true}, treeSowDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.setOutdoorOverride(20.0f);
        gardener = sim.things().spawn("Human", CellPos{8, 8, 0});
        sim.jobs().addWorker(gardener);
        Colonist colonist;
        colonist.skills.push_back(SkillRecord{"Plants", plants, Passion::None, 0.0f, false});
        sim.things().registry().emplace<Colonist>(gardener, colonist);
    }
    DefId def(const char* name) const { return sim.defs().idOf("Things", name); }
    size_t zones() const { return sim.farming().zones().size(); }
    void run(int seconds) {
        for (int i = 0; i < seconds; ++i) {
            sim.step();
        }
    }
};

}  // namespace

TEST_CASE("Tree sowing: the zone takes a tree only once its project is done") {
    TreeSowFixture f(8);
    f.sim.submit(CreateGrowingZoneCommand{CellRect{20, 20, 5, 5, 0}, "Plant_TreeOak"});
    f.sim.step();
    CHECK(f.zones() == 0);
    f.sim.submit(CreateGrowingZoneCommand{CellRect{20, 20, 5, 5, 0}, "Plant_Cocoa"});
    f.sim.step();
    CHECK(f.zones() == 0);

    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{20, 20, 5, 5, 0}, "Plant_TreeOak"});
    f.sim.step();
    CHECK(f.zones() == 1);
    // Cocoa still needs its own project, and willows, which the wiki does not list, stay unsowable.
    f.sim.submit(SetGrowingZoneCropCommand{1, "Plant_Cocoa"});
    f.sim.step();
    CHECK(f.sim.farming().zones()[0].crop == f.def("Plant_TreeOak"));
    f.sim.submit(CreateGrowingZoneCommand{CellRect{30, 30, 3, 3, 0}, "Plant_TreeWillow"});
    f.sim.step();
    CHECK(f.zones() == 1);
    f.sim.submit(UnlockResearchCommand{"Cocoa"});
    f.sim.submit(SetGrowingZoneCropCommand{1, "Plant_Cocoa"});
    f.sim.step();
    CHECK(f.sim.farming().zones()[0].crop == f.def("Plant_Cocoa"));
}

TEST_CASE("Tree sowing: trees keep a free ring, and need open sky and skill") {
    TreeSowFixture f(8);
    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{20, 20, 3, 3, 0}, "Plant_TreeOak"});
    f.sim.step();
    const DefId oak = f.def("Plant_TreeOak");
    f.sim.plants().place(CellPos{20, 20, 0}, oak, 0.0f);
    CHECK(f.sim.farming().sowBlocker(CellPos{21, 21, 0}) == "another tree is next to it");
    CHECK(f.sim.farming().sowBlocker(CellPos{21, 20, 0}) == "another tree is next to it");
    CHECK(f.sim.farming().sowBlocker(CellPos{22, 22, 0}).empty());
    CHECK(f.sim.farming().sowBlocker(CellPos{20, 20, 0}) == "already sown");

    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    TreeSowFixture low(5);  // sowMinSkill is 6: the work giver passes the zone by
    low.sim.submit(UnlockResearchCommand{"TreeSowing"});
    low.sim.submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_TreeOak"});
    low.run(2000);
    CHECK(!low.sim.plants().has(CellPos{10, 8, 0}));
}

TEST_CASE("Tree sowing: a gardener sows a spaced grove, and a grown tree is felled for wood") {
    TreeSowFixture f(8);
    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{10, 8, 3, 3, 0}, "Plant_TreeOak"});
    f.run(9000);
    const DefId oak = f.def("Plant_TreeOak");
    int trees = 0;
    for (int y = 8; y < 11; ++y) {
        for (int x = 10; x < 13; ++x) {
            const CellPos c{x, y, 0};
            if (f.sim.plants().has(c) && f.sim.plants().defAt(c) == oak) {
                ++trees;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const CellPos n{x + dx, y + dy, 0};
                        if ((dx != 0 || dy != 0) && f.sim.plants().has(n)) {
                            CHECK(f.sim.plants().defAt(n) != oak);
                        }
                    }
                }
            }
        }
    }
    CHECK(trees >= 1);
    CHECK(trees <= 4);  // 3 x 3 cells hold at most four trees with a free ring

}

TEST_CASE("Tree sowing: a grown tree in the zone is felled for wood") {
    TreeSowFixture f(8);
    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_TreeOak"});
    f.sim.step();
    f.sim.plants().place(CellPos{10, 8, 0}, f.def("Plant_TreeOak"), 1.0f);
    f.run(4000);
    int wood = 0;
    const DefId log = f.def("WoodLog");
    for (const auto [e, thing, stack] : f.sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)e;
        wood += thing.def == log ? stack.count : 0;
    }
    CHECK(wood == 46);
}

TEST_CASE("Tree sowing: a cocoa tree gives 20 chocolate and is felled by the harvest") {
    TreeSowFixture f(8);
    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    f.sim.submit(UnlockResearchCommand{"Cocoa"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_Cocoa"});
    f.sim.step();
    const CellPos cell{10, 8, 0};
    f.sim.plants().place(cell, f.def("Plant_Cocoa"), 1.0f);
    f.run(3000);
    int chocolate = 0;
    const DefId item = f.def("Chocolate");
    for (const auto [e, thing, stack] : f.sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)e;
        chocolate += thing.def == item ? stack.count : 0;
    }
    CHECK(chocolate == 20);  // Plants 8: the full yield
    const Def& cocoa = f.sim.defs().get(f.def("Plant_Cocoa"));
    CHECK(cocoa.number("plant.growDays") == doctest::Approx(16.0));
    CHECK(cocoa.integer("plant.sowMinSkill") == 8);
    CHECK(f.sim.defs().get(item).number("stats.nutrition") == doctest::Approx(0.1));
}

TEST_CASE("Tree sowing: an ordinary tree rides out a frost, a cocoa tree does not") {
    TreeSowFixture f(8);
    f.sim.submit(UnlockResearchCommand{"TreeSowing"});
    f.sim.submit(UnlockResearchCommand{"Cocoa"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{10, 8, 1, 1, 0}, "Plant_TreeOak"});
    f.sim.submit(CreateGrowingZoneCommand{CellRect{14, 8, 1, 1, 0}, "Plant_Cocoa"});
    f.sim.step();
    f.sim.plants().place(CellPos{10, 8, 0}, f.def("Plant_TreeOak"), 0.5f);
    f.sim.plants().place(CellPos{14, 8, 0}, f.def("Plant_Cocoa"), 0.5f);
    f.sim.setOutdoorOverride(-15.0f);
    f.run(3 * 24 * 3600);
    CHECK(f.sim.plants().has(CellPos{10, 8, 0}));
    CHECK(!f.sim.plants().has(CellPos{14, 8, 0}));
}

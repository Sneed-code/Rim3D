// The autocannon and uranium slug turrets (docs/design/defense.md "Heavy turrets"): ammunition on the fuel system and
// a gun's minimum range.
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defense.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Power.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> heavyDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

struct HeavyFixture {
    std::unique_ptr<Sim> owned;
    Sim& sim;
    Entity turret = kNoEntity;

    // The 2 x 2 turret at (40,40) (cells 40..41 x 40..41, centre (41,41)), powered by a generator off to the south-west
    // along a conduit that touches its west side at x = 39 (conduits do not block a line of fire).
    explicit HeavyFixture(const char* turretDef) : owned(make()), sim(*owned) {
        testFueled(sim, sim.things().spawn("WoodFiredGenerator", CellPos{32, 35, 0}));
        for (int x = 34; x <= 38; ++x) {
            sim.things().spawn("PowerConduit", CellPos{x, 36, 0});
        }
        for (int y = 36; y <= 40; ++y) {
            sim.things().spawn("PowerConduit", CellPos{39, y, 0});
        }
        turret = sim.things().spawn(turretDef, CellPos{40, 40, 0});
        sim.step();
    }
    static std::unique_ptr<Sim> make() {
        SimSettings settings;
        settings.seed = 31;
        settings.mapSize = 96;
        settings.flatMap = true;
        return std::make_unique<Sim>(settings, heavyDefs());
    }
    float ammo() const { return sim.jobs().fuel().fraction(turret) * sim.jobs().fuel().specOf(turret).capacity; }
    // A hostile held still `metres` east of the turret's centre (41, 41); steps `steps` and counts the turret's shots.
    int shoot(float metres, int steps, float dirX = 1.0f, float dirY = 0.0f) {
        const float x = 41.0f + metres * dirX;
        const float y = 41.0f + metres * dirY;
        const Entity pawn = sim.things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
        sim.submit(SetFactionCommand{pawn, 2});
        int shots = 0;
        for (int i = 0; i < steps; ++i) {
            Position& pos = sim.things().registry().get<Position>(pawn);
            pos.x = x;
            pos.y = y;
            sim.step();
            for (const Event& e : sim.events()) {
                shots += e.type == EventType::ShotFired && e.thing == turret ? 1 : 0;
            }
        }
        return shots;
    }
};

}  // namespace

TEST_CASE("Heavy turrets: the wiki's numbers") {
    const DefDatabase& defs = *heavyDefs();
    const Def* cannon = defs.find("Things", "Turret_Autocannon");
    const Def* slug = defs.find("Things", "Turret_UraniumSlug");
    CHECK((cannon != nullptr && slug != nullptr));
    if (cannon == nullptr || slug == nullptr) {
        return;
    }
    CHECK(cannon->integer("costs.Steel") == 350);
    CHECK(cannon->integer("costs.Plasteel") == 40);
    CHECK(cannon->integer("costs.ComponentIndustrial") == 6);
    CHECK(cannon->number("power.consumption") == doctest::Approx(150.0));
    CHECK(cannon->number("fuel.capacity") == doctest::Approx(180.0));
    CHECK(slug->integer("costs.Uranium") == 60);
    CHECK(slug->number("fuel.capacity") == doctest::Approx(60.0));
    CHECK(cannon->text("researchPrerequisite") == "AutocannonTurret");
    CHECK(slug->text("researchPrerequisite") == "UraniumSlugTurret");
    GunStats stats;
    const std::unique_ptr<Sim> simOwner = HeavyFixture::make();
    Sim& sim = *simOwner;
    CHECK(sim.combat().gunStats(sim.defs().idOf("Things", "Gun_AutocannonTurret"), stats));
    CHECK(stats.range == doctest::Approx(32.9f));
    CHECK(stats.minRange == doctest::Approx(8.9f));
    CHECK(stats.burst == 3);
    CHECK(sim.combat().gunStats(sim.defs().idOf("Things", "Gun_UraniumSlugTurret"), stats));
    CHECK(stats.range == doctest::Approx(45.9f));
    CHECK(stats.minRange == doctest::Approx(11.9f));
    CHECK(stats.burst == 1);
}

TEST_CASE("Heavy turrets: an autocannon turret shoots a target 14 m away and uses 2 steel a shot") {
    HeavyFixture f("Turret_Autocannon");
    CHECK(f.sim.power().running(f.turret));
    CHECK(f.sim.jobs().fuel().add(f.turret, 180) == 180);
    const int shots = f.shoot(14.0f, 400);
    CHECK(shots >= 3);
    CHECK(f.ammo() == doctest::Approx(180.0f - 2.0f * static_cast<float>(shots)));
}

TEST_CASE("Heavy turrets: an empty turret holds its fire, and stops in the middle of a burst when the steel runs out") {
    HeavyFixture f("Turret_Autocannon");
    CHECK(f.shoot(14.0f, 300) == 0);  // no steel
    CHECK(f.sim.jobs().fuel().add(f.turret, 4) == 4);  // two shots; the burst is three
    CHECK(f.shoot(14.0f, 600) == 2);
    CHECK(f.ammo() == doctest::Approx(0.0f));
}

TEST_CASE("Heavy turrets: a target inside the minimum range is not shot, one outside it is") {
    HeavyFixture f("Turret_Autocannon");
    f.sim.jobs().fuel().add(f.turret, 180);
    CHECK(f.shoot(5.0f, 300) == 0);  // 5 m: inside 8.9
    HeavyFixture g("Turret_Autocannon");
    g.sim.jobs().fuel().add(g.turret, 180);
    CHECK(g.shoot(12.0f, 300) >= 1);
    HeavyFixture h("Turret_Autocannon");
    h.sim.jobs().fuel().add(h.turret, 180);
    CHECK(h.shoot(40.0f, 300) == 0);  // beyond 32.9
}

TEST_CASE("Heavy turrets: a 2 x 2 turret sees and shoots out of every side, its own cells do not block") {
    const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}, {0.7071f, -0.7071f}, {-0.7071f, -0.7071f}};
    for (const auto& d : dirs) {
        HeavyFixture f("Turret_Autocannon");
        f.sim.jobs().fuel().add(f.turret, 180);
        const int shots = f.shoot(14.0f, 400, d[0], d[1]);
        CHECK_MESSAGE(shots >= 1, "direction ", d[0], ", ", d[1]);
    }
}

TEST_CASE("Heavy turrets: a uranium slug turret fires single shots at 40 m, 2 uranium each") {
    HeavyFixture f("Turret_UraniumSlug");
    CHECK(f.sim.jobs().fuel().add(f.turret, 60) == 60);
    const int shots = f.shoot(40.0f, 500);
    CHECK(shots >= 2);
    CHECK(f.ammo() == doctest::Approx(60.0f - 2.0f * static_cast<float>(shots)));
    HeavyFixture g("Turret_UraniumSlug");
    g.sim.jobs().fuel().add(g.turret, 60);
    CHECK(g.shoot(9.0f, 300) == 0);  // inside 11.9
}

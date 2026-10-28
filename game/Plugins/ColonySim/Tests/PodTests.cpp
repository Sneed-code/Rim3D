#include "ColonySim/Caravans.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Research.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"
#include "ColonySim/TransportPods.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Transport pods (docs/design/transport-pods.md).

// Tests build without exceptions, so a failed must-have check ends the test case by returning.
#define POD_REQUIRE(cond) \
    do {                  \
        CHECK((cond));    \
        if (!(cond)) {    \
            return;       \
        }                 \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> podDefs() {
    static std::shared_ptr<const DefDatabase> db;
    if (db == nullptr) {
        auto loaded = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *loaded, errors);
        db = loaded;
    }
    return db;
}

// A flat 64 m map with no world, or the Crashlanded start on a world.
SimSettings podSettings(bool world) {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 64;
    s.checkpointEvery = 0;
    s.wildAnimals = 0;
    if (world) {
        s.scenario = "Crashlanded";
        s.world.enabled = true;
    } else {
        s.flatMap = true;
    }
    return s;
}

struct PodSite {
    Entity launcher = kNoEntity;
    std::vector<Entity> pods;
};

// A launcher (1 x 2, at x, y..y+1) with `pods` pods east of it, the launcher filled with `fuel` chemfuel. On a map
// with things already, the first free spot from (x, y) along the row is used.
PodSite podBuild(Sim& sim, int x, int y, int pods, int fuel) {
    PodSite site;
    for (int tryX = x; tryX < sim.map().width() - 4 && site.launcher == kNoEntity; tryX += 3) {
        for (int tryY = y; tryY < sim.map().height() - 4 && site.launcher == kNoEntity; tryY += 3) {
            bool free = true;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = -1; dx < 3; ++dx) {
                    const CellPos c{tryX + dx, tryY + dy, 0};
                    free = free && sim.map().walkable(c) && sim.things().itemsAt(c).empty();
                }
            }
            if (!free) {
                continue;
            }
            site.launcher = sim.things().spawn("PodLauncher", CellPos{tryX, tryY, 0});
            for (int i = 0; i < pods; ++i) {
                site.pods.push_back(sim.things().spawn("TransportPod", CellPos{tryX + 1, tryY + i, 0}));
            }
        }
    }
    if (site.launcher != kNoEntity && fuel > 0) {
        sim.jobs().fuel().add(site.launcher, fuel);
    }
    return site;
}

Entity podColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.things().registry().emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn);
    return pawn;
}

// Colonists who aren't the protagonist, by id.
std::vector<Entity> podScenarioPawns(Sim& sim, size_t count) {
    std::vector<Entity> pawns;
    for (const auto [e, c, pos] : sim.things().registry().view<const Colonist, const Position>().each()) {
        (void)pos;
        if (!c.protagonist) {
            pawns.push_back(e);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    pawns.resize(std::min(pawns.size(), count));
    return pawns;
}

uint64_t podLoad(Sim& sim, const std::vector<Entity>& pods, const std::vector<Entity>& pawns, const std::vector<Entity>& items) {
    PodCommand c;
    c.action = "Load";
    c.pods = pods;
    c.pawns = pawns;
    c.items = items;
    return sim.submit(c);
}

uint64_t podLaunch(Sim& sim, int group, int tile, const char* arrival, int x = -1, int y = -1) {
    PodCommand c;
    c.action = "Launch";
    c.group = group;
    c.tile = tile;
    c.arrival = arrival;
    c.x = x;
    c.y = y;
    return sim.submit(c);
}

bool podFailed(const Sim& sim, uint64_t id, std::string* why = nullptr) {
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed && e.command == id) {
            if (why != nullptr) {
                *why = e.message;
            }
            return true;
        }
    }
    return false;
}

// Steps until an event of `type` (or `limit` steps); the event is copied to `out`.
bool podStepUntil(Sim& sim, EventType type, int limit, Event* out = nullptr) {
    for (int i = 0; i < limit; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == type) {
                if (out != nullptr) {
                    *out = e;
                }
                return true;
            }
        }
    }
    return false;
}

int podCountOnMap(const Sim& sim, const char* def) {
    int n = 0;
    for (const auto [e, thing, pos, stack] : sim.things().registry().view<const Thing, const Position, const StackCount>().each()) {
        (void)e;
        (void)pos;
        if (sim.defs().get(thing.def).defName() == def) {
            n += stack.count;
        }
    }
    return n;
}

// A land tile a caravan can reach, `far` tiles or more east or west of the start.
int podFarTile(const Sim& sim, int far) {
    const World& w = *sim.world();
    const int start = w.startTile();
    for (int d = far; d < far + 20; ++d) {
        for (const int sign : {1, -1}) {
            const int x = ((w.tileX(start) + sign * d) % w.width() + w.width()) % w.width();
            const int tile = w.index(x, w.tileY(start));
            if (w.canVisit(tile) && w.tile(tile).settlement == kNoSettlement && !sim.caravans().findPath(start, tile, 0).empty()) {
                return tile;
            }
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Pods: the Transport pod project unlocks the launcher and the pod") {
    Sim sim(podSettings(false), podDefs());
    const DefId launcher = sim.defs().idOf("Things", "PodLauncher");
    const DefId pod = sim.defs().idOf("Things", "TransportPod");
    POD_REQUIRE(launcher.valid() && pod.valid());
    CHECK(sim.construction().buildable(launcher));
    CHECK(sim.construction().buildable(pod));
    CHECK_FALSE(sim.research().unlocked(sim.defs().get(launcher)));
    CHECK_FALSE(sim.research().unlocked(sim.defs().get(pod)));
    sim.submit(UnlockResearchCommand{"TransportPod"});
    sim.step();
    CHECK(sim.research().unlocked(sim.defs().get(launcher)));
    CHECK(sim.research().unlocked(sim.defs().get(pod)));
    // The wiki's numbers.
    const Def& l = sim.defs().get(launcher);
    CHECK(l.number("fuel.capacity") == doctest::Approx(150.0));
    CHECK(l.text("fuel.item") == "Chemfuel");
    CHECK(sim.defs().get(pod).number("transportPod.massCapacity") == doctest::Approx(150.0));
    CHECK(sim.pods().rules().fuelPerTile == doctest::Approx(2.25));
}

TEST_CASE("Pods: each launcher fuels the pod touching it along an edge; the range is its fuel over 2.25") {
    Sim sim(podSettings(false), podDefs());
    // The launcher covers (20,20) and (20,21); pods at (21,20) and (21,21) both touch it, (21,22) only at a corner.
    const Entity launcher = sim.things().spawn("PodLauncher", CellPos{20, 20, 0});
    const Entity first = sim.things().spawn("TransportPod", CellPos{21, 20, 0});
    const Entity second = sim.things().spawn("TransportPod", CellPos{21, 21, 0});
    const Entity corner = sim.things().spawn("TransportPod", CellPos{21, 22, 0});
    POD_REQUIRE(launcher != kNoEntity && first != kNoEntity && second != kNoEntity && corner != kNoEntity);
    CHECK(sim.pods().launcherOf(first) == launcher);
    CHECK(sim.pods().launcherOf(second) == kNoEntity);  // one pod a launcher
    CHECK(sim.pods().launcherOf(corner) == kNoEntity);
    // A second launcher west of the second pod's row fuels it.
    const Entity other = sim.things().spawn("PodLauncher", CellPos{22, 21, 0});
    CHECK(sim.pods().launcherOf(second) == other);
    PodGroup g;
    g.pods = {first};
    CHECK(sim.pods().rangeOf(g) == -1);  // empty
    sim.jobs().fuel().add(launcher, 150);
    CHECK(sim.pods().rangeOf(g) == 66);  // the wiki's full range
    sim.jobs().fuel().use(launcher, 150.0f - 22.5f);
    CHECK(sim.pods().rangeOf(g) == 10);
}

TEST_CASE("Pods: loading is refused for too much, non-pods and things named twice") {
    Sim sim(podSettings(false), podDefs());
    const PodSite site = podBuild(sim, 20, 20, 1, 150);
    POD_REQUIRE(site.pods.size() == 1);
    const Entity a = podColonist(sim, CellPos{10, 10, 0});
    const Entity b = podColonist(sim, CellPos{11, 10, 0});
    const Entity c = podColonist(sim, CellPos{12, 10, 0});
    const Entity steel = sim.things().spawn("Steel", CellPos{14, 14, 0}, 75);
    std::string why;
    // Three people are 180 kg: more than a pod's 150.
    uint64_t id = podLoad(sim, site.pods, {a, b, c}, {});
    sim.step();
    CHECK(podFailed(sim, id, &why));
    CHECK(why.find("too heavy") != std::string::npos);
    id = podLoad(sim, {site.launcher}, {a}, {});
    sim.step();
    CHECK(podFailed(sim, id));
    id = podLoad(sim, site.pods, {a}, {steel, steel});
    sim.step();
    CHECK(podFailed(sim, id));
    id = podLoad(sim, site.pods, {}, {});
    sim.step();
    CHECK(podFailed(sim, id));
    // Two people and 75 steel (37.5 kg) fit: 157.5 doesn't, one person and the steel does.
    id = podLoad(sim, site.pods, {a, b}, {steel});
    sim.step();
    CHECK(podFailed(sim, id));
    id = podLoad(sim, site.pods, {a}, {steel});
    sim.step();
    CHECK_FALSE(podFailed(sim, id));
    CHECK(sim.pods().groups().size() == 1);
    // The pod is busy now.
    id = podLoad(sim, site.pods, {b}, {});
    sim.step();
    CHECK(podFailed(sim, id));
}

TEST_CASE("Pods: haulers carry the stacks in, the passenger gets in, and the pods land on the map") {
    Sim sim(podSettings(false), podDefs());
    const PodSite site = podBuild(sim, 30, 30, 1, 150);
    POD_REQUIRE(site.pods.size() == 1);
    const Entity rider = podColonist(sim, CellPos{10, 10, 0});
    podColonist(sim, CellPos{12, 12, 0});  // a hauler who stays
    const Entity steel = sim.things().spawn("Steel", CellPos{16, 16, 0}, 75);
    const Entity components = sim.things().spawn("ComponentIndustrial", CellPos{18, 16, 0}, 5);
    podLoad(sim, site.pods, {rider}, {steel, components});
    Event ready;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsReady, 2 * kSecondsPerHour, &ready));
    const PodGroup* g = sim.pods().find(ready.count);
    POD_REQUIRE(g != nullptr);
    CHECK(g->state == PodGroupState::Ready);
    CHECK(g->wanted.empty());
    CHECK(g->loaded.size() == 2);
    CHECK_FALSE(sim.things().spawned(rider));
    CHECK(podCountOnMap(sim, "Steel") == 0);
    CHECK(podCountOnMap(sim, "ComponentIndustrial") == 0);
    Snapshot snap;
    sim.writeSnapshot(snap);
    POD_REQUIRE(snap.podGroups.size() == 1);
    CHECK(snap.podGroups[0].aboard == std::vector<Entity>{rider});
    CHECK(snap.podGroups[0].massKg == doctest::Approx(60.0 + 37.5 + 3.0));
    CHECK(snap.podGroups[0].rangeTiles == 66);

    // No world: the pods can only come down on this map. It burns RimWorld's least, 1 chemfuel.
    const uint64_t id = podLaunch(sim, ready.count, -1, "Land", 50, 50);
    Event launched;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsLaunched, 2, &launched));
    CHECK_FALSE(podFailed(sim, id));
    CHECK_FALSE(sim.things().exists(site.pods[0]));
    CHECK(sim.things().registry().get<Fuel>(site.launcher).amount == doctest::Approx(149.0));
    CHECK(sim.pods().groups().empty());
    POD_REQUIRE(sim.pods().flights().size() == 1);
    Event arrived;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsArrived, 2 * kSecondsPerHour, &arrived));
    CHECK(arrived.message == "landed");
    POD_REQUIRE(sim.things().spawned(rider));
    const Position& at = sim.things().registry().get<Position>(rider);
    CHECK(std::abs(at.x - 50.5f) < 3.0f);
    CHECK(std::abs(at.y - 50.5f) < 3.0f);
    CHECK(podCountOnMap(sim, "Steel") == 75);
    CHECK(podCountOnMap(sim, "ComponentIndustrial") == 5);
    // The rider works again.
    sim.stepMany(60);
    CHECK(sim.things().registry().all_of<Worker>(rider));
    CHECK_FALSE(sim.things().registry().all_of<PodPassenger>(rider));
}

TEST_CASE("Pods: cancelling puts what was loaded back beside the pods; a lost pod spills its load") {
    for (const bool destroyPod : {false, true}) {
        Sim sim(podSettings(false), podDefs());
        const PodSite site = podBuild(sim, 30, 30, 1, 150);
        POD_REQUIRE(site.pods.size() == 1);
        const Entity rider = podColonist(sim, CellPos{10, 10, 0});
        const Entity steel = sim.things().spawn("Steel", CellPos{16, 16, 0}, 75);
        podLoad(sim, site.pods, {rider}, {steel});
        Event ready;
        POD_REQUIRE(podStepUntil(sim, EventType::PodsReady, 2 * kSecondsPerHour, &ready));
        CHECK(podCountOnMap(sim, "Steel") == 0);
        if (destroyPod) {
            DestroyThingCommand d;
            d.thing = site.pods[0];
            sim.submit(d);
        } else {
            PodCommand c;
            c.action = "Cancel";
            c.group = ready.count;
            sim.submit(c);
        }
        POD_REQUIRE(podStepUntil(sim, EventType::PodsCancelled, 2));
        CHECK(sim.pods().groups().empty());
        CHECK(sim.things().spawned(rider));
        CHECK(podCountOnMap(sim, "Steel") == 75);
        const Position& at = sim.things().registry().get<Position>(rider);
        CHECK(std::abs(at.x - 31.5f) < 3.0f);
    }
}

TEST_CASE("Pods: a launch to a world tile burns 2.25 chemfuel a tile and forms a caravan there") {
    Sim sim(podSettings(true), podDefs());
    POD_REQUIRE(sim.world() != nullptr);
    const std::vector<Entity> riders = podScenarioPawns(sim, 1);
    POD_REQUIRE(riders.size() == 1);
    const PodSite site = podBuild(sim, 8, 8, 1, 20);
    POD_REQUIRE(site.pods.size() == 1);
    const Entity steel = sim.things().spawn("Steel", cellOf(sim.things().registry().get<Position>(riders[0])), 40);
    POD_REQUIRE(steel != kNoEntity);
    podLoad(sim, site.pods, riders, {steel});
    Event ready;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsReady, 3 * kSecondsPerHour, &ready));
    const int tile = podFarTile(sim, 10);
    POD_REQUIRE(tile >= 0);
    const int tiles = sim.pods().distance(sim.world()->startTile(), tile);
    CHECK(tiles >= 10);
    std::string why;
    // 20 chemfuel reach 8 tiles.
    uint64_t id = podLaunch(sim, ready.count, tile, "Caravan");
    sim.step();
    CHECK(podFailed(sim, id, &why));
    CHECK(why.find("out of range") != std::string::npos);
    // Landing on a map nobody has been to is refused; a gift needs a settlement.
    id = podLaunch(sim, ready.count, tile, "Land");
    sim.step();
    CHECK(podFailed(sim, id));
    id = podLaunch(sim, ready.count, tile, "Gift");
    sim.step();
    CHECK(podFailed(sim, id));
    sim.jobs().fuel().add(site.launcher, 130);
    const float before = sim.things().registry().get<Fuel>(site.launcher).amount;
    id = podLaunch(sim, ready.count, tile, "Caravan");
    POD_REQUIRE(podStepUntil(sim, EventType::PodsLaunched, 2));
    CHECK_FALSE(podFailed(sim, id));
    CHECK(sim.things().registry().get<Fuel>(site.launcher).amount == doctest::Approx(before - 2.25f * static_cast<float>(tiles)));
    // 50 ticks a tile.
    POD_REQUIRE(sim.pods().flights().size() == 1);
    CHECK(sim.pods().flights()[0].seconds == doctest::Approx(rimWorldTicksToSeconds(50.0 * tiles)));
    Event arrived;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsArrived, 2 * kSecondsPerHour, &arrived));
    CHECK(arrived.message == "caravan");
    const Caravan* c = sim.caravans().find(arrived.part);
    POD_REQUIRE(c != nullptr);
    CHECK(c->tile == tile);
    CHECK(c->state == CaravanState::Waiting);
    POD_REQUIRE(c->members.size() == 1);
    CHECK(c->members[0].pawn == riders[0]);
    CHECK(c->items == std::vector<Entity>{steel});

    // The caravan walks onto the tile's map; then pods with goods alone can land there.
    CaravanCommand visit;
    visit.action = "Visit";
    visit.caravan = arrived.part;
    sim.submit(visit);
    sim.step();
    POD_REQUIRE(sim.otherMap(tile) != nullptr);
    const PodSite more = podBuild(sim, 30, 8, 1, 150);
    POD_REQUIRE(more.pods.size() == 1);
    const Entity components = sim.things().spawn("ComponentIndustrial", CellPos{more.pods[0] == kNoEntity ? 0 : 33, 9, 0}, 6);
    POD_REQUIRE(components != kNoEntity);
    podLoad(sim, more.pods, {}, {components});
    POD_REQUIRE(podStepUntil(sim, EventType::PodsReady, 3 * kSecondsPerHour, &ready));
    // A settlement can't be landed on: that is an attack.
    for (const WorldSettlement& s : sim.world()->settlements()) {
        id = podLaunch(sim, ready.count, s.tile, "Land");
        sim.step();
        CHECK(podFailed(sim, id, &why));
        CHECK(why.find("attack") != std::string::npos);
        break;
    }
    podLaunch(sim, ready.count, tile, "Land", 20, 20);
    POD_REQUIRE(podStepUntil(sim, EventType::PodsArrived, 2 * kSecondsPerHour, &arrived));
    CHECK(arrived.message == "landed");
    CHECK(podCountOnMap(*sim.otherMap(tile), "ComponentIndustrial") == 6);
    CHECK(podCountOnMap(*sim.otherMap(tile), "Steel") == 40);  // the caravan's, beside the riders
}

TEST_CASE("Pods: goods sent to a settlement are a gift, worth goodwill by their market value") {
    Sim sim(podSettings(true), podDefs());
    POD_REQUIRE(sim.world() != nullptr);
    const World& w = *sim.world();
    int target = -1;
    for (const WorldSettlement& s : w.settlements()) {
        if (sim.pods().distance(w.startTile(), s.tile) <= 60 && sim.trade().goodwill(s.faction) < 90.0f) {
            target = static_cast<int>(&s - w.settlements().data());
            break;
        }
    }
    POD_REQUIRE(target >= 0);
    const WorldSettlement& s = w.settlements()[static_cast<size_t>(target)];
    const PodSite site = podBuild(sim, 8, 8, 1, 150);
    POD_REQUIRE(site.pods.size() == 1);
    const std::vector<Entity> riders = podScenarioPawns(sim, 1);
    POD_REQUIRE(!riders.empty());
    const Entity silver = sim.things().spawn("Silver", cellOf(sim.things().registry().get<Position>(riders[0])), 400);
    POD_REQUIRE(silver != kNoEntity);
    podLoad(sim, site.pods, {}, {silver});
    Event ready;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsReady, 3 * kSecondsPerHour, &ready));
    const float goodwill = sim.trade().goodwill(s.faction);
    podLaunch(sim, ready.count, s.tile, "Gift");
    Event arrived;
    POD_REQUIRE(podStepUntil(sim, EventType::PodsArrived, 2 * kSecondsPerHour, &arrived));
    CHECK(arrived.message == "gift");
    // 400 silver over 40 silver a point.
    CHECK(sim.trade().goodwill(s.faction) == doctest::Approx(std::min(100.0f, goodwill + 10.0f)));
    CHECK_FALSE(sim.things().exists(silver));
}

TEST_CASE("Pods: a save while loading and in flight loads to the same game, and the command replays") {
    Sim original(podSettings(false), podDefs());
    const PodSite site = podBuild(original, 30, 30, 1, 150);
    POD_REQUIRE(site.pods.size() == 1);
    const Entity rider = podColonist(original, CellPos{10, 10, 0});
    podColonist(original, CellPos{12, 12, 0});
    const Entity steel = original.things().spawn("Steel", CellPos{16, 16, 0}, 75);
    podLoad(original, site.pods, {rider}, {steel});
    original.stepMany(60);
    for (int round = 0; round < 2; ++round) {
        SaveLoadResult loaded = loadSim(saveSim(original), podDefs());
        INFO(loaded.error);
        POD_REQUIRE(loaded.sim != nullptr);
        Sim& copy = *loaded.sim;
        CHECK(copy.pods().groups().size() + copy.pods().flights().size() == 1);
        CHECK(copy.stateHash() == original.stateHash());
        original.stepMany(600);
        copy.stepMany(600);
        CHECK(copy.stateHash() == original.stateHash());
        if (round == 0) {
            for (int i = 0; i < 2 * kSecondsPerHour && original.pods().groups()[0].state != PodGroupState::Ready; ++i) {
                original.step();
            }
            POD_REQUIRE(original.pods().groups()[0].state == PodGroupState::Ready);
            podLaunch(original, original.pods().groups()[0].id, -1, "Land", 50, 50);
            original.stepMany(10);
            POD_REQUIRE(original.pods().flights().size() == 1);
        }
    }
    Replay read;
    std::string error;
    POD_REQUIRE(readReplay(writeReplay(recordReplay(original)), read, error));
    POD_REQUIRE(read.commands.size() == 2);
    const auto* c = std::get_if<PodCommand>(&read.commands[1].command);
    POD_REQUIRE(c != nullptr);
    CHECK(c->action == "Launch");
    CHECK(c->arrival == "Land");
    CHECK(c->x == 50);
}

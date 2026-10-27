#include "ColonySim/Defs.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Power.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Weather.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define TEMP_REQUIRE(...)      \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the climate numbers, the heaters and the hediffs are part of what is tested.
std::shared_ptr<const DefDatabase> tempDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// `realPower` leaves the heaters and coolers to the power grid; otherwise they all run.
std::unique_ptr<Sim> tempSim(float outdoor, int size = 48, bool realPower = false) {
    auto sim = std::make_unique<Sim>(SimSettings{5, size, 0, 0, true}, tempDefs());
    sim->setOutdoorOverride(outdoor);
    if (!realPower) {
        sim->temperature().setSourceRunning([](Entity) { return true; });
    }
    return sim;
}

// A ring of walls round x0..x1 by y0..y1, leaving out `gaps`.
void tempRing(Sim& sim, int x0, int y0, int x1, int y1, const std::vector<CellPos>& gaps = {}) {
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

// The temperature of the room round a cell (the outdoor temperature if it is not in one).
float tempAt(Sim& sim, int x, int y) { return sim.temperature().temperatureAt(CellPos{x, y, 0}); }

// Runs the temperature system alone for some game hours, on its own clock, with the outdoor
// temperature as it is. Quick enough to run days of heat flow.
int64_t tempClock = 0;
void tempHours(Sim& sim, double hours) {
    const int64_t interval = sim.temperature().numbers().intervalSeconds;
    const int64_t end = tempClock + static_cast<int64_t>(hours * 3600.0);
    for (; tempClock < end; tempClock += interval) {
        sim.temperature().step(tempClock);
    }
}

HealthReport tempReport(Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    return report;
}

float tempCapacity(const HealthReport& report, const std::string& name) {
    for (const HealthReport::CapacityLine& c : report.capacities) {
        if (c.name == name) {
            return c.value;
        }
    }
    return -1.0f;
}

const HealthReport::EnvironmentLine* tempIllness(const HealthReport& report, const std::string& name) {
    for (const HealthReport::EnvironmentLine& line : report.environment) {
        if (line.name == name) {
            return &line;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("Temperature: the outdoor climate is E1's weather curve, and the ground is the year's mean") {
    Sim sim(SimSettings{5, 24, 0, 0, true}, tempDefs());
    const auto at = [&](int64_t day, int hour) { return sim.outdoorTemperatureAt(day * 86400 + hour * 3600); };
    for (int64_t day : {0, 22, 37, 52}) {
        for (int hour : {0, 5, 11, 15, 20}) {
            CHECK(at(day, hour) == static_cast<float>(sim.weather().temperatureAt(day * 86400 + hour * 3600)));
        }
    }
    // Mid-summer (day 22) is warmer than mid-winter (day 52), and the day's mean is near the season's.
    CHECK(at(22, 15) > at(52, 15) + 10.0f);
    CHECK((at(22, 5) + at(22, 15)) * 0.5f == doctest::Approx(19.0f).epsilon(0.5));  // summer's mean, give or take the drift
    CHECK(sim.groundTemperature() == doctest::Approx((9.0f + 19.0f + 9.0f - 2.0f) / 4.0f));
    // The override beats it, and clearing it brings it back.
    sim.setOutdoorOverride(-5.0f);
    CHECK(sim.outdoorTemperature() == -5.0f);
    CHECK(sim.groundTemperature() == -5.0f);
    sim.setOutdoorOverride(std::nullopt);
    CHECK(sim.outdoorTemperature() == sim.outdoorTemperatureAt(0));
}

TEST_CASE("Temperature: a closed room slowly follows the outdoors, with the time the rules say") {
    auto sim = tempSim(20.0f);
    tempRing(*sim, 10, 10, 16, 16);
    const Room* room = sim->rooms().roomOf(CellPos{13, 13, 0});
    TEMP_REQUIRE(room != nullptr);
    TEMP_REQUIRE(room->enclosed() && !room->outdoors());
    CHECK(room->cellCount == 25);
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(20.0f));  // a new room starts at the outdoor temperature

    // 20 wall edges at 0.04 and 25 roof cells at 0.024 an hour, shared by 25 cells: 0.056 an hour,
    // so after 18 hours 1/e of the 20 degrees is left.
    sim->setOutdoorOverride(0.0f);
    tempHours(*sim, 18.0);
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(20.0f / 2.71828f).epsilon(0.05));
    tempHours(*sim, 6.0 * 24.0);
    CHECK(tempAt(*sim, 13, 13) < 0.1f);
    CHECK(tempAt(*sim, 13, 13) >= 0.0f);
    // Outside the walls it is the outdoor temperature.
    CHECK(tempAt(*sim, 30, 30) == 0.0f);
}

TEST_CASE("Temperature: an open door lets the room follow the outdoors much faster than a shut one") {
    const auto cooled = [](bool open) {
        auto sim = tempSim(20.0f);
        const CellPos doorCell{13, 10, 0};
        tempRing(*sim, 10, 10, 16, 16, {doorCell});
        const Entity door = sim->things().spawn("Door", doorCell);
        sim->rooms().update();
        sim->temperature().step(0);
        CHECK(door != kNoEntity);
        sim->things().registry().get<Door>(door).open = open;
        sim->setOutdoorOverride(0.0f);
        tempClock = 60;
        tempHours(*sim, 3.0);
        return tempAt(*sim, 13, 13);
    };
    const float shut = cooled(false);
    const float open = cooled(true);
    CHECK(shut == doctest::Approx(20.0f * std::exp(-0.057f * 3.0f)).epsilon(0.03));
    CHECK(open < shut - 4.0f);
    tempClock = 0;
}

TEST_CASE("Temperature: a heater warms only while its power net runs it, a campfire needs no power") {
    // No power: the heater stays cold, the campfire (no power part) works.
    {
        auto sim = tempSim(0.0f, 48, true);
        tempRing(*sim, 10, 10, 16, 16);
        const Entity heater = sim->things().spawn("Heater", CellPos{12, 12, 0});
        TEMP_REQUIRE(heater != kNoEntity);
        sim->rooms().update();
        tempClock = 0;
        tempHours(*sim, 6.0);
        CHECK_FALSE(sim->power().running(heater));
        CHECK(tempAt(*sim, 13, 13) < 1.0f);
    }
    {
        auto sim = tempSim(0.0f, 48, true);
        tempRing(*sim, 10, 10, 16, 16);
        TEMP_REQUIRE(sim->things().spawn("Campfire", CellPos{12, 12, 0}) != kNoEntity);
        sim->rooms().update();
        tempClock = 0;
        tempHours(*sim, 6.0);
        CHECK(tempAt(*sim, 13, 13) > 10.0f);
    }
    // On a net with a charged battery the heater warms the room.
    {
        auto sim = tempSim(0.0f, 48, true);
        sim->power().setSun([](const GameTime&) { return 0.0; });
        tempRing(*sim, 10, 10, 16, 16);
        const Entity heater = sim->things().spawn("Heater", CellPos{12, 12, 0});
        sim->things().spawn("PowerConduit", CellPos{13, 12, 0});
        const Entity battery = sim->things().spawn("Battery", CellPos{14, 12, 0});
        TEMP_REQUIRE(heater != kNoEntity && battery != kNoEntity);
        sim->things().registry().get<PowerPart>(battery).stored = 100.0;
        sim->rooms().update();
        sim->power().update();
        sim->step();
        tempClock = 0;
        tempHours(*sim, 6.0);
        CHECK(sim->power().running(heater));
        CHECK(tempAt(*sim, 15, 15) > 10.0f);
    }
}

TEST_CASE("Temperature: a heater warms a room to its limit and holds it, a cooler cools to its") {
    // Units first: with the room at the outdoor temperature, one minute of a 21 heat-per-second heater
    // in 25 cells is 21 * (2500 / 60) / 3600 * 60 / 25 degrees.
    {
        auto sim = tempSim(10.0f);
        tempRing(*sim, 10, 10, 16, 16);
        TEMP_REQUIRE(sim->things().spawn("Heater", CellPos{12, 12, 0}) != kNoEntity);
        sim->rooms().update();
        sim->temperature().step(0);
        CHECK(tempAt(*sim, 13, 13) == doctest::Approx(10.0f + 21.0f * (2500.0f / 60.0f) / 3600.0f * 60.0f / 25.0f).epsilon(0.002));
    }
    // Winter: the heater brings a 0 degree room up to 21 within a few hours and stays at 21 for days.
    {
        auto sim = tempSim(0.0f);
        tempRing(*sim, 10, 10, 16, 16);
        TEMP_REQUIRE(sim->things().spawn("Heater", CellPos{12, 12, 0}) != kNoEntity);
        sim->rooms().update();
        tempClock = 0;
        tempHours(*sim, 1.0);
        CHECK(tempAt(*sim, 13, 13) > 10.0f);
        tempHours(*sim, 4.0);
        CHECK(tempAt(*sim, 13, 13) > 20.0f);
        tempHours(*sim, 48.0);
        CHECK(tempAt(*sim, 13, 13) == doctest::Approx(21.0f).epsilon(0.001));
        // A heater that is switched off (power or fuel out) stops warming, and the room cools.
        sim->temperature().setSourceRunning([](Entity) { return false; });
        tempHours(*sim, 48.0);
        CHECK(tempAt(*sim, 13, 13) < 3.0f);
    }
    // Summer: a cooler holds a 40 degree room at 10.
    {
        auto sim = tempSim(40.0f);
        tempRing(*sim, 10, 10, 16, 16);
        TEMP_REQUIRE(sim->things().spawn("Cooler", CellPos{12, 12, 0}) != kNoEntity);
        sim->rooms().update();
        tempClock = 0;
        tempHours(*sim, 48.0);
        CHECK(tempAt(*sim, 13, 13) == doctest::Approx(10.0f).epsilon(0.002));
    }
    // A campfire inside warms a cold room, but not past its own 28 degrees.
    {
        auto sim = tempSim(-10.0f);
        tempRing(*sim, 10, 10, 16, 16);
        TEMP_REQUIRE(sim->things().spawn("Campfire", CellPos{12, 12, 0}) != kNoEntity);
        sim->rooms().update();
        tempClock = 0;
        tempHours(*sim, 24.0);
        CHECK(tempAt(*sim, 13, 13) == doctest::Approx(28.0f).epsilon(0.001));
    }
    // Heat pushed into the open air is lost: a campfire outdoors changes nothing.
    {
        auto sim = tempSim(-10.0f);
        TEMP_REQUIRE(sim->things().spawn("Campfire", CellPos{30, 30, 0}) != kNoEntity);
        tempClock = 0;
        tempHours(*sim, 6.0);
        CHECK(tempAt(*sim, 30, 30) == -10.0f);
        CHECK(tempAt(*sim, 31, 30) == -10.0f);
    }
}

TEST_CASE("Temperature: rooms that join mix their heat, and a room that is split keeps what each half had") {
    auto sim = tempSim(0.0f);
    tempRing(*sim, 10, 10, 16, 16);
    tempRing(*sim, 16, 10, 22, 16);  // shares the wall at x = 16
    const Room* a = sim->rooms().roomOf(CellPos{13, 13, 0});
    const Room* b = sim->rooms().roomOf(CellPos{19, 13, 0});
    TEMP_REQUIRE(a != nullptr && b != nullptr);
    TEMP_REQUIRE(a->id != b->id);
    TEMP_REQUIRE(a->enclosed() && !a->outdoors() && b->enclosed() && !b->outdoors());
    tempClock = 0;
    sim->temperature().step(0);
    sim->temperature().restore({RoomTemperature{a->anchor, 10.0f}, RoomTemperature{b->anchor, 30.0f}});
    CHECK(tempAt(*sim, 13, 13) == 10.0f);
    CHECK(tempAt(*sim, 19, 13) == 30.0f);

    // Take the shared wall down between y = 11 and 15: one room of 25 + 25 + 5 cells, the five that
    // were wall having been outdoors (0 degrees).
    std::vector<Entity> wall;
    for (int y = 11; y <= 15; ++y) {
        wall.push_back(sim->map().buildingAt(CellPos{16, y, 0}));
        TEMP_REQUIRE(wall.back() != kNoEntity);
    }
    for (const Entity e : wall) {
        sim->things().destroy(e);
    }
    sim->rooms().update();
    sim->temperature().step(60);
    const Room* both = sim->rooms().roomOf(CellPos{13, 13, 0});
    TEMP_REQUIRE(both != nullptr);
    CHECK(both == sim->rooms().roomOf(CellPos{19, 13, 0}));
    TEMP_REQUIRE(both->enclosed() && !both->outdoors());
    CHECK(both->cellCount == 55);
    const float mixed = (25.0f * 10.0f + 25.0f * 30.0f) / 55.0f;
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(mixed).epsilon(0.005));

    // Put the wall back and each half starts at the mixed temperature; give one half more heat and
    // the other still has the old.
    for (int y = 11; y <= 15; ++y) {
        sim->things().spawn("Wall", CellPos{16, y, 0});
    }
    sim->rooms().update();
    sim->temperature().step(120);
    CHECK(sim->rooms().roomOf(CellPos{13, 13, 0}) != sim->rooms().roomOf(CellPos{19, 13, 0}));
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(mixed).epsilon(0.01));
    CHECK(tempAt(*sim, 19, 13) == doctest::Approx(mixed).epsilon(0.01));
    tempClock = 0;
}

TEST_CASE("Temperature: a room with a wall between it and a warm room is warmed through the wall") {
    auto sim = tempSim(0.0f);
    tempRing(*sim, 10, 10, 16, 16);
    tempRing(*sim, 16, 10, 22, 16);
    sim->things().spawn("Heater", CellPos{19, 13, 0});
    sim->rooms().update();
    tempClock = 0;
    tempHours(*sim, 24.0);
    const float heated = tempAt(*sim, 19, 13);
    const float next = tempAt(*sim, 13, 13);
    CHECK(heated == doctest::Approx(21.0f).epsilon(0.001));
    // The room next door is warmer than the outdoors thanks to the shared wall.
    CHECK(next > 1.5f);
    CHECK(next < heated);
}

TEST_CASE("Temperature: a colonist outdoors in hard frost gets hypothermia, loses the use of the hands, and dies of it") {
    auto sim = tempSim(-30.0f);
    const Entity pawn = sim->things().spawn("Human", CellPos{24, 24, 0});
    TEMP_REQUIRE(pawn != kNoEntity);
    float min = 0.0f;
    float max = 0.0f;
    TEMP_REQUIRE(sim->temperature().comfortRange(pawn, min, max));
    CHECK(min == 16.0f);  // no clothes: the def's range
    CHECK(max == 26.0f);
    CHECK(sim->temperature().degreesBelowComfort(pawn) == doctest::Approx(46.0f));
    CHECK(sim->temperature().degreesAboveComfort(pawn) == 0.0f);

    sim->stepMany(86400 / 5);
    HealthReport report = tempReport(*sim, pawn);
    const HealthReport::EnvironmentLine* cold = tempIllness(report, "hypothermia");
    TEMP_REQUIRE(cold != nullptr);
    // Safe range's low end is 10 degrees under the comfortable one: 36 degrees under it, 0.08 a day each.
    CHECK(cold->severity == doctest::Approx(36.0f * 0.08f * 0.2f).epsilon(0.02));
    CHECK(cold->stage == "major");
    CHECK(tempCapacity(report, "Manipulation") < 0.6f);
    CHECK(tempCapacity(report, "Moving") < 0.8f);
    CHECK_FALSE(report.dead);

    sim->stepMany(86400 * 5 / 10);
    report = tempReport(*sim, pawn);
    CHECK(report.dead);
    CHECK(report.cause == DeathCause::Hypothermia);
    CHECK(std::string(deathCauseName(report.cause)) == "hypothermia");
}

TEST_CASE("Temperature: heatstroke from heat, and a cold pawn that comes in from the frost recovers") {
    {
        auto sim = tempSim(55.0f);
        const Entity pawn = sim->things().spawn("Human", CellPos{24, 24, 0});
        TEMP_REQUIRE(pawn != kNoEntity);
        sim->stepMany(86400 / 2);
        const HealthReport report = tempReport(*sim, pawn);
        const HealthReport::EnvironmentLine* hot = tempIllness(report, "heatstroke");
        TEMP_REQUIRE(hot != nullptr);
        // Safe range's high end: 26 + 10 = 36 (no clothes), so 19 degrees over.
        CHECK(hot->severity == doctest::Approx(19.0f * 0.08f * 0.5f).epsilon(0.02));
        CHECK(tempCapacity(report, "Consciousness") < 1.0f);
        CHECK(tempIllness(report, "hypothermia") == nullptr);
    }
    {
        auto sim = tempSim(-30.0f);
        const Entity pawn = sim->things().spawn("Human", CellPos{24, 24, 0});
        TEMP_REQUIRE(pawn != kNoEntity);
        sim->stepMany(86400 / 8);
        CHECK(tempIllness(tempReport(*sim, pawn), "hypothermia") != nullptr);
        sim->setOutdoorOverride(15.0f);
        sim->stepMany(86400 / 2);
        const HealthReport report = tempReport(*sim, pawn);
        CHECK(tempIllness(report, "hypothermia") == nullptr);
        CHECK(tempCapacity(report, "Manipulation") == doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("Temperature: a pawn in a heated room is fine in a freezing winter, and one outside the walls is not") {
    auto sim = tempSim(-25.0f);
    tempRing(*sim, 10, 10, 16, 16);
    sim->things().spawn("Heater", CellPos{12, 12, 0});
    const Entity inside = sim->things().spawn("Human", CellPos{14, 14, 0});
    const Entity outside = sim->things().spawn("Human", CellPos{30, 30, 0});
    TEMP_REQUIRE(inside != kNoEntity && outside != kNoEntity);
    sim->stepMany(86400 / 3);
    CHECK(sim->temperature().ambientOf(inside) == doctest::Approx(21.0f).epsilon(0.01));
    CHECK(sim->temperature().ambientOf(outside) == -25.0f);
    CHECK(tempIllness(tempReport(*sim, inside), "hypothermia") == nullptr);
    CHECK(tempIllness(tempReport(*sim, outside), "hypothermia") != nullptr);
}

TEST_CASE("Temperature: frostbite is an injury of a finger or toe, and a pawn can have it put on") {
    auto sim = tempSim(10.0f);
    const Entity pawn = sim->things().spawn("Human", CellPos{24, 24, 0});
    TEMP_REQUIRE(pawn != kNoEntity);
    const DefId def = sim->things().registry().get<Thing>(pawn).def;
    const std::vector<int> fingers = sim->health().partsOfDef(def, "Finger");
    const std::vector<int> toes = sim->health().partsOfDef(def, "Toe");
    CHECK(fingers.size() == 10);
    CHECK(toes.size() == 10);
    const DefId frostbite = sim->defs().idOf("Hediffs", "Frostbite");
    TEMP_REQUIRE(frostbite.valid());
    CHECK(sim->health().addInjury(pawn, fingers[0], frostbite, 3.0f));
    CHECK_FALSE(sim->health().addInjury(pawn, 9999, frostbite, 3.0f));
    CHECK_FALSE(sim->health().addInjury(pawn, fingers[1], frostbite, 0.0f));
    const HealthReport report = tempReport(*sim, pawn);
    TEMP_REQUIRE(report.injuries.size() == 1);
    CHECK(report.injuries[0].injury == "frostbite");
    CHECK(report.injuries[0].part == sim->health().partLabels(def)[static_cast<size_t>(fingers[0])]);
}

TEST_CASE("Temperature: the snapshot has the outdoor temperature and each room's") {
    auto sim = tempSim(12.5f);
    tempRing(*sim, 10, 10, 16, 16);
    sim->things().spawn("Heater", CellPos{12, 12, 0});
    sim->stepMany(7200);
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.outdoorTemperature == 12.5f);
    TEMP_REQUIRE(snap.rooms.size() == 1);
    CHECK(snap.rooms[0].temperature == doctest::Approx(sim->temperature().temperatureAt(CellPos{13, 13, 0})));
    CHECK(snap.rooms[0].temperature > 18.0f);
}

TEST_CASE("Temperature: the same game twice is the same, and a save in the middle of a cold night loads back the same") {
    const auto make = [] {
        auto sim = tempSim(-20.0f, 48, true);
        sim->power().setSun([](const GameTime&) { return 0.0; });
        tempRing(*sim, 10, 10, 16, 16);
        tempRing(*sim, 16, 10, 22, 16);
        sim->things().spawn("Heater", CellPos{12, 12, 0});
        sim->things().spawn("PowerConduit", CellPos{13, 12, 0});
        const Entity battery = sim->things().spawn("Battery", CellPos{14, 12, 0});
        if (battery != kNoEntity) {
            sim->things().registry().get<PowerPart>(battery).stored = 100.0;
        }
        sim->things().spawn("Human", CellPos{14, 14, 0});
        sim->things().spawn("Human", CellPos{30, 30, 0});
        return sim;
    };
    auto a = make();
    auto b = make();
    a->stepMany(3 * 3600);
    b->stepMany(3 * 3600);
    CHECK(a->stateHash() == b->stateHash());

    const std::vector<uint8_t> bytes = saveSim(*a);
    SaveLoadResult loaded = loadSim(bytes, tempDefs());
    TEMP_REQUIRE(loaded.error.empty());
    TEMP_REQUIRE(loaded.sim != nullptr);
    loaded.sim->setOutdoorOverride(-20.0f);
    CHECK(loaded.sim->stateHash() == a->stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    CHECK(loaded.sim->temperature().temperatureAt(CellPos{13, 13, 0}) == a->temperature().temperatureAt(CellPos{13, 13, 0}));
    CHECK(loaded.sim->temperature().temperatureAt(CellPos{19, 13, 0}) == a->temperature().temperatureAt(CellPos{19, 13, 0}));

    // And on: an hour with a wall coming down in the middle of it.
    for (Sim* sim : {a.get(), loaded.sim.get()}) {
        sim->stepMany(1800);
        sim->things().destroy(sim->map().buildingAt(CellPos{16, 12, 0}));
        sim->stepMany(1800);
    }
    CHECK(loaded.sim->stateHash() == a->stateHash());
    CHECK(saveSim(*loaded.sim) == saveSim(*a));
    CHECK(saveToJson(*a).find("\"roomTemperatures\"") != std::string::npos);
    CHECK(saveToJson(*a).find("\"environment\"") != std::string::npos);
}

TEST_CASE("Temperature: a colonist feels cold or hot by the degrees outside the comfortable range, and it shows in their thoughts and mood") {
    SimSettings settings{5, 128, 0, 0, true};
    settings.scenario = "Crashlanded";
    Sim sim(settings, tempDefs());
    TEMP_REQUIRE(sim.setupErrors().empty());
    sim.setOutdoorOverride(15.0f);
    sim.step();
    Entity colonist = kNoEntity;
    for (const auto [entity, needs] : sim.things().registry().view<const Needs>().each()) {
        colonist = colonist == kNoEntity ? entity : colonist;
    }
    TEMP_REQUIRE(colonist != kNoEntity);
    const auto thought = [&](const std::string& def) -> const ThoughtView* {
        static std::vector<ThoughtView> held;
        held = sim.mood().thoughts(colonist, sim.time().seconds);
        for (const ThoughtView& t : held) {
            if (t.def == def) {
                return &t;
            }
        }
        return nullptr;
    };
    CHECK(thought("Cold") == nullptr);
    CHECK(thought("Hot") == nullptr);
    const float comfortable = sim.mood().target(colonist, sim.time().seconds);

    // The colonist's clothes (T-shirt and pants) widen the range from the def's 16 to 26.
    float lowest = 0.0f;
    float highest = 0.0f;
    TEMP_REQUIRE(sim.temperature().comfortRange(colonist, lowest, highest));
    CHECK(lowest < 16.0f);
    CHECK(highest >= 26.0f);

    sim.setOutdoorOverride(lowest - 3.0f);
    const ThoughtView* chilly = thought("Cold");
    TEMP_REQUIRE(chilly != nullptr);
    CHECK(chilly->label == "chilly");
    CHECK(chilly->offset == -3.0f);
    CHECK(chilly->situation);
    CHECK(sim.mood().target(colonist, sim.time().seconds) == doctest::Approx(comfortable - 3.0f));

    sim.setOutdoorOverride(lowest - 9.0f);
    const ThoughtView* freezing = thought("Cold");
    TEMP_REQUIRE(freezing != nullptr);
    CHECK(freezing->label == "freezing");
    CHECK(freezing->offset == -8.0f);

    sim.setOutdoorOverride(highest + 3.0f);
    const ThoughtView* warm = thought("Hot");
    TEMP_REQUIRE(warm != nullptr);
    CHECK(warm->label == "warm");
    sim.setOutdoorOverride(highest + 12.0f);
    const ThoughtView* scorching = thought("Hot");
    TEMP_REQUIRE(scorching != nullptr);
    CHECK(scorching->label == "scorching");
    CHECK(thought("Cold") == nullptr);
}

TEST_CASE("Temperature: three days of deep winter, a heated room keeps its people well and the open ground does not") {
    auto sim = tempSim(-18.0f);
    const CellPos doorCell{13, 10, 0};
    tempRing(*sim, 10, 10, 16, 16, {doorCell});
    sim->things().spawn("Door", doorCell);
    sim->things().spawn("Heater", CellPos{12, 12, 0});
    std::vector<Entity> inside;
    for (const CellPos& c : {CellPos{12, 14, 0}, CellPos{14, 14, 0}, CellPos{14, 12, 0}}) {
        inside.push_back(sim->things().spawn("Human", c));
    }
    const Entity outside = sim->things().spawn("Human", CellPos{30, 30, 0});
    TEMP_REQUIRE(outside != kNoEntity);
    sim->stepMany(3 * 86400);
    for (const Entity pawn : inside) {
        const HealthReport report = tempReport(*sim, pawn);
        CHECK_FALSE(report.dead);
        CHECK(report.environment.empty());
    }
    CHECK(sim->temperature().temperatureAt(CellPos{13, 13, 0}) == doctest::Approx(21.0f).epsilon(0.01));
    const HealthReport frozen = tempReport(*sim, outside);
    CHECK(frozen.dead);
    CHECK(frozen.cause == DeathCause::Hypothermia);
}

TEST_CASE("Temperature: a thermostat sets what a heater or cooler holds, and it saves and replays") {
    const auto make = [](Entity& heater, Entity& cooler) {
        auto sim = tempSim(-10.0f, 48, false);
        sim->temperature().setSourceRunning([](Entity) { return true; });
        tempRing(*sim, 10, 10, 16, 16);
        tempRing(*sim, 16, 10, 22, 16);
        heater = sim->things().spawn("Heater", CellPos{12, 12, 0});
        cooler = sim->things().spawn("Cooler", CellPos{19, 12, 0});
        sim->rooms().update();
        return sim;
    };
    Entity heater = kNoEntity;
    Entity cooler = kNoEntity;
    auto sim = make(heater, cooler);
    TEMP_REQUIRE(heater != kNoEntity && cooler != kNoEntity);

    // Without a setting the def's limits hold: the heater 21, the cooler 10.
    CHECK_FALSE(sim->temperature().hasThermostat(heater));
    CHECK(sim->temperature().thermostat(heater) == 21.0f);
    CHECK(sim->temperature().thermostat(cooler) == 10.0f);
    tempClock = 0;
    tempHours(*sim, 12.0);
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(21.0f).epsilon(0.02));

    // A lower target holds the room there (the heater stops, the room drifts down to it, the heater keeps it).
    sim->submit(SetThermostatCommand{heater, 15.0f, false});
    sim->step();
    CHECK(sim->temperature().thermostat(heater) == 15.0f);
    tempHours(*sim, 24.0);
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(15.0f).epsilon(0.03));
    // A higher one than the def's works too.
    sim->submit(SetThermostatCommand{heater, 26.0f, false});
    sim->step();
    tempHours(*sim, 12.0);
    CHECK(tempAt(*sim, 13, 13) == doctest::Approx(26.0f).epsilon(0.02));
    // The cooler takes a target as well: set to 18 in a 20 degree outdoors it pulls the room down to it.
    sim->setOutdoorOverride(30.0f);
    sim->submit(SetThermostatCommand{cooler, 18.0f, false});
    sim->step();
    tempHours(*sim, 30.0);
    CHECK(tempAt(*sim, 19, 13) == doctest::Approx(18.0f).epsilon(0.03));

    // Refusals: no thermostat on a wall or a campfire, a target out of range, not a number.
    const Entity wall = sim->map().buildingAt(CellPos{10, 10, 0});
    const Entity campfire = sim->things().spawn("Campfire", CellPos{30, 30, 0});
    std::string why;
    CHECK_FALSE(sim->temperature().setThermostat(wall, 20.0f, &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(sim->temperature().setThermostat(campfire, 20.0f));
    CHECK_FALSE(sim->temperature().setThermostat(heater, 80.0f));
    CHECK_FALSE(sim->temperature().setThermostat(heater, std::nanf("")));
    CHECK(sim->temperature().thermostat(heater) == 26.0f);

    // Clearing goes back to the def's limit.
    sim->submit(SetThermostatCommand{heater, 0.0f, true});
    sim->step();
    CHECK_FALSE(sim->temperature().hasThermostat(heater));
    CHECK(sim->temperature().thermostat(heater) == 21.0f);
    sim->submit(SetThermostatCommand{heater, 24.0f, false});
    sim->step();

    // A save loads back with the thermostats and the same hash; a wall-less heater that is destroyed forgets its setting.
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, tempDefs());
    TEMP_REQUIRE(loaded.error.empty());
    TEMP_REQUIRE(loaded.sim != nullptr);
    loaded.sim->setOutdoorOverride(30.0f);
    CHECK(loaded.sim->temperature().thermostat(heater) == 24.0f);
    CHECK(loaded.sim->temperature().thermostat(cooler) == 18.0f);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    CHECK(saveToJson(*sim).find("\"thermostats\"") != std::string::npos);
    sim->things().destroy(heater);
    sim->step();
    CHECK(sim->temperature().thermostats().size() == 1);
}

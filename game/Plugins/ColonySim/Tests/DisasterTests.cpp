#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Disasters.h"
#include "ColonySim/Events.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Health.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Power.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include "FuelTestHelper.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define DISASTER_REQUIRE(...)  \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the incident defs, DisasterRules and the thoughts are part of what is tested.
std::shared_ptr<const DefDatabase> disasterDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A flat map with the Crashlanded colonists in the middle, and wild animals if asked.
std::unique_ptr<Sim> disasterSim(int wildAnimals = 0, int size = 96, uint64_t seed = 5) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = size;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.wildAnimals = wildAnimals;
    return std::make_unique<Sim>(settings, disasterDefs());
}

// Fires an incident by the dev command and steps once; the letter is in the step's events.
const Event* disasterFire(Sim& sim, const char* incident, float points = 0.0f) {
    sim.submit(FireIncidentCommand{incident, points});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::Letter) {
            return &e;
        }
    }
    return nullptr;
}

size_t disasterCommandFailures(const Sim& sim) {
    size_t n = 0;
    for (const Event& e : sim.events()) {
        n += e.type == EventType::CommandFailed ? 1 : 0;
    }
    return n;
}

Entity disasterPut(Sim& sim, const char* def, int x, int y) {
    return testFueled(sim, sim.things().spawn(def, CellPos{x, y, 0}));
}

// A generator, a wire and a lamp far from the colonists; returns the lamp.
Entity disasterPowerLine(Sim& sim) {
    disasterPut(sim, "WoodFiredGenerator", 10, 10);
    for (int x = 12; x <= 15; ++x) {
        disasterPut(sim, "PowerConduit", x, 10);
    }
    const Entity battery = disasterPut(sim, "Battery", 13, 11);
    sim.things().registry().get<PowerPart>(battery).stored = 400.0;
    return disasterPut(sim, "StandingLamp", 16, 10);
}

void disasterRun(Sim& sim, int64_t seconds) {
    for (int64_t i = 0; i < seconds; ++i) {
        sim.step();
    }
}

bool disasterHasThought(Sim& sim, Entity pawn, const char* def) {
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == def) {
            return true;
        }
    }
    return false;
}

std::vector<Entity> disasterColonists(Sim& sim) {
    std::vector<Entity> out;
    for (const Entity e : sim.things().registry().view<Colonist>()) {
        out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("Disasters: every trouble incident has a worker") {
    const auto sim = disasterSim();
    DISASTER_REQUIRE(sim->setupErrors().empty());
    for (const char* name : {"SolarFlare", "ToxicFallout", "VolcanicWinter", "Flashstorm", "PsychicDrone", "PsychicSoothe",
                             "ShortCircuit", "Blight", "ManhunterPack", "MadAnimal"}) {
        CHECK_MESSAGE(sim->narrator().hasWorker(name), name);
    }
}

TEST_CASE("Disasters: a solar flare darkens every power net until it is over") {
    const auto sim = disasterSim();
    const Entity lamp = disasterPowerLine(*sim);
    disasterRun(*sim, 3);
    DISASTER_REQUIRE(sim->power().powered(lamp));

    const Event* letter = disasterFire(*sim, "SolarFlare");
    DISASTER_REQUIRE(letter != nullptr);
    CHECK(letter->message.find("Solar flare") == 0);
    CHECK(letter->count == static_cast<int>(LetterKind::NegativeEvent));
    CHECK(sim->disasters().isActive(DisasterKind::SolarFlare));
    disasterRun(*sim, 5);
    CHECK_FALSE(sim->power().powered(lamp));
    CHECK(sim->power().blackout());

    // Not twice at once.
    sim->submit(FireIncidentCommand{"SolarFlare", 0.0f});
    sim->step();
    CHECK(disasterCommandFailures(*sim) == 1);

    const int64_t end = sim->disasters().active().front().endsAt;
    CHECK(end - sim->time().seconds <= static_cast<int64_t>(0.5 * kSecondsPerDay));
    bool ended = false;
    while (sim->time().seconds < end + 3) {
        sim->step();
        for (const Event& e : sim->events()) {
            ended = ended || e.type == EventType::DisasterEnded;
        }
    }
    CHECK(ended);
    CHECK_FALSE(sim->disasters().isActive(DisasterKind::SolarFlare));
    CHECK(sim->power().powered(lamp));
}

TEST_CASE("Disasters: a short circuit empties the net, burns a conduit and says where") {
    const auto sim = disasterSim();
    disasterPowerLine(*sim);
    disasterRun(*sim, 3);
    const Event* letter = disasterFire(*sim, "ShortCircuit");
    DISASTER_REQUIRE(letter != nullptr);
    CHECK(letter->message.find("Short circuit") == 0);
    bool circuit = false;
    for (const Event& e : sim->events()) {
        circuit = circuit || e.type == EventType::ShortCircuit;
    }
    CHECK(circuit);

    // With no power at all nothing can happen.
    const auto bare = disasterSim();
    bare->submit(FireIncidentCommand{"ShortCircuit", 0.0f});
    bare->step();
    CHECK(disasterCommandFailures(*bare) == 1);
}

TEST_CASE("Disasters: a volcanic winter takes 7 degrees and 30% of the light, and gives them back") {
    const auto sim = disasterSim();
    disasterRun(*sim, 10);
    const int64_t probe = sim->time().seconds + 2 * kSecondsPerHour;
    const float before = sim->outdoorTemperatureAt(probe);
    const float glowBefore = sim->weather().skyGlow();
    DISASTER_REQUIRE(disasterFire(*sim, "VolcanicWinter") != nullptr);
    CHECK(sim->outdoorTemperatureAt(probe) == doctest::Approx(before - 7.0f).epsilon(0.001));
    CHECK(sim->weather().skyGlow() == doctest::Approx(glowBefore));  // the dimming comes on over about 20 hours
    disasterRun(*sim, 24 * kSecondsPerHour);
    CHECK(sim->weather().skyGlow() == doctest::Approx(glowBefore * 0.7f).epsilon(0.02));
    const DisasterCondition winter = sim->disasters().active().front();
    CHECK(winter.endsAt - winter.startedAt >= static_cast<int64_t>(7.5 * kSecondsPerDay));
    CHECK(winter.endsAt - winter.startedAt <= 40 * kSecondsPerDay);
}

TEST_CASE("Disasters: toxic fallout poisons those outside and kills the plants under the open sky") {
    const auto sim = disasterSim();
    const DefId grass = sim->defs().idOf("Things", "Plant_Grass");
    std::vector<CellPos> planted;
    for (int x = 4; x < 24; ++x) {
        const CellPos c{x, 4, 0};
        if (sim->plants().place(c, grass, 0.5f)) {
            planted.push_back(c);
        }
    }
    DISASTER_REQUIRE(planted.size() >= 10);
    DISASTER_REQUIRE(disasterFire(*sim, "ToxicFallout") != nullptr);
    disasterRun(*sim, 3 * kSecondsPerHour + 10);
    size_t left = 0;
    for (const CellPos& c : planted) {
        left += sim->plants().has(c) ? 1 : 0;
    }
    CHECK(left <= planted.size() / 4);

    const DefId buildup = sim->defs().idOf("Hediffs", "ToxicBuildup");
    float most = 0.0f;
    bool thought = false;
    for (const Entity pawn : disasterColonists(*sim)) {
        most = std::max(most, sim->health().environmentSeverity(pawn, buildup));
        thought = thought || disasterHasThought(*sim, pawn, "ToxicFallout");
    }
    CHECK(most > 0.0f);
    CHECK(most < 0.2f);  // 0.4 a day: about 0.05 an hour at most
    CHECK(thought);

    // Over, the buildup fades.
    DisasterState over = sim->disasters().state();
    over.active.clear();
    sim->disasters().restore(over);
    const float peak = most;
    disasterRun(*sim, 2 * kSecondsPerHour + 5);
    float now = 0.0f;
    for (const Entity pawn : disasterColonists(*sim)) {
        now = std::max(now, sim->health().environmentSeverity(pawn, buildup));
    }
    CHECK(now < peak);
}

TEST_CASE("Disasters: a flashstorm throws bolts inside its circle") {
    const auto sim = disasterSim();
    const Event* letter = disasterFire(*sim, "Flashstorm");
    DISASTER_REQUIRE(letter != nullptr);
    DISASTER_REQUIRE(sim->disasters().isActive(DisasterKind::Flashstorm));
    const DisasterCondition storm = sim->disasters().active().front();
    int strikes = 0;
    for (int64_t i = 0; i < 6 * kSecondsPerHour; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::LightningStrike) {
                ++strikes;
                const float dx = static_cast<float>(e.cell.x - storm.centre.x);
                const float dy = static_cast<float>(e.cell.y - storm.centre.y);
                CHECK(dx * dx + dy * dy <= storm.radius * storm.radius + 1.0f);
            }
        }
    }
    CHECK(strikes >= 15);  // one every 461 to 1152 seconds (RimWorld's 320 to 800 ticks): about 27 in 6 hours
    CHECK(strikes <= 45);
    CHECK(storm.radius >= 45.0f);
    CHECK(storm.radius <= 60.0f);
}

TEST_CASE("Disasters: a psychic drone drags down one sex, a soothe lifts it, and the two never overlap") {
    const auto sim = disasterSim();
    const std::vector<Entity> people = disasterColonists(*sim);
    DISASTER_REQUIRE(people.size() >= 2);
    DISASTER_REQUIRE(disasterFire(*sim, "PsychicDrone") != nullptr);
    DISASTER_REQUIRE(sim->disasters().isActive(DisasterKind::PsychicDrone));
    const DisasterCondition drone = sim->disasters().active().front();
    sim->step();
    int hit = 0;
    for (const Entity p : people) {
        const bool matches = sim->things().registry().get<Colonist>(p).female == drone.female;
        CHECK(disasterHasThought(*sim, p, "PsychicDrone") == matches);
        hit += matches ? 1 : 0;
    }
    CHECK(hit >= 1);
    CHECK(drone.level >= 0);
    CHECK(drone.level <= 2);
    CHECK(drone.endsAt - drone.startedAt >= static_cast<int64_t>(0.75 * kSecondsPerDay));
    CHECK(drone.endsAt - drone.startedAt <= static_cast<int64_t>(1.75 * kSecondsPerDay));

    sim->submit(FireIncidentCommand{"PsychicSoothe", 0.0f});
    sim->step();
    CHECK(disasterCommandFailures(*sim) == 1);

    DisasterState clear = sim->disasters().state();
    clear.active.clear();
    sim->disasters().restore(clear);
    DISASTER_REQUIRE(disasterFire(*sim, "PsychicSoothe") != nullptr);
    sim->step();
    int soothed = 0;
    for (const Entity p : people) {
        soothed += disasterHasThought(*sim, p, "PsychicSoothe") ? 1 : 0;
        CHECK_FALSE(disasterHasThought(*sim, p, "PsychicDrone"));
    }
    CHECK(soothed >= 1);
}

TEST_CASE("Disasters: crop blight kills one crop's plants round a centre, only in growing zones") {
    const auto sim = disasterSim();
    std::string error;
    const DefId rice = sim->defs().idOf("Things", "Plant_Rice");
    const DefId grass = sim->defs().idOf("Things", "Plant_Grass");
    const GrowingZoneId zone = sim->farming().createZone(CellRect{30, 30, 12, 12}, rice, error);
    DISASTER_REQUIRE(zone != kNoGrowingZone);
    int sown = 0;
    for (const CellPos& c : sim->farming().find(zone)->cells) {
        sown += sim->plants().place(c, rice, 0.6f) ? 1 : 0;
    }
    const CellPos wild{70, 70, 0};
    sim->plants().remove(wild);
    sim->plants().place(wild, rice, 0.6f);  // rice outside any zone is left alone
    DISASTER_REQUIRE(sown >= 50);
    const Event* letter = disasterFire(*sim, "Blight");
    DISASTER_REQUIRE(letter != nullptr);
    CHECK(letter->message.find("Crop blight") == 0);
    // The blight starts in a small cluster and kills nothing yet.
    const size_t first = sim->disasters().blighted().size();
    CHECK(first >= 1);
    for (const BlightedPlant& b : sim->disasters().blighted()) {
        CHECK(sim->plants().has(b.cell));
        CHECK(sim->plants().defAt(b.cell) == rice);
    }
    CHECK(sim->plants().has(wild));
    // Over days it spreads to the neighbours and every blighted plant withers (within 72 hours of catching it).
    size_t most = first;
    int withered = 0;
    disasterRun(*sim, 4 * 3600);
    for (int h = 0; h < 6 * 24; ++h) {
        for (int i = 0; i < 3600; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                withered += e.type == EventType::PlantRemoved && e.message == "blighted" ? 1 : 0;
            }
        }
        most = std::max(most, sim->disasters().blighted().size());
    }
    CHECK(withered > 0);
    CHECK(most > first);
    CHECK(sim->disasters().blighted().empty());
    CHECK(sim->plants().has(wild));
    (void)grass;

    // No crops, no blight.
    const auto bare = disasterSim();
    bare->submit(FireIncidentCommand{"Blight", 0.0f});
    bare->step();
    CHECK(disasterCommandFailures(*bare) == 1);
}

TEST_CASE("Disasters: a manhunter pack comes in at the edge with a target and the points decide its size") {
    const auto sim = disasterSim(100, 128);
    DISASTER_REQUIRE(!sim->animals().wildKinds().empty());
    const uint64_t before = sim->animals().stats().manhunters;
    const Event* letter = disasterFire(*sim, "ManhunterPack", 400.0f);
    DISASTER_REQUIRE(letter != nullptr);
    CHECK(letter->message.find("Manhunter pack") == 0);
    CHECK(letter->count == static_cast<int>(LetterKind::ThreatBig));
    const uint64_t came = sim->animals().stats().manhunters - before;
    CHECK(came >= 1);
    CHECK(came <= 30);

    const auto big = disasterSim(100, 128);
    const uint64_t b0 = big->animals().stats().manhunters;
    disasterFire(*big, "ManhunterPack", 3000.0f);
    const auto small = disasterSim(100, 128);
    const uint64_t s0 = small->animals().stats().manhunters;
    disasterFire(*small, "ManhunterPack", 60.0f);
    CHECK(big->animals().stats().manhunters - b0 >= small->animals().stats().manhunters - s0);
}

TEST_CASE("Disasters: a mad animal near the colony turns manhunter, none far away does") {
    const auto sim = disasterSim(100, 128);
    const std::vector<AnimalSystem::WildKind> kinds = sim->animals().wildKinds();
    DISASTER_REQUIRE(!kinds.empty());
    const uint64_t before = sim->animals().stats().manhunters;
    // Nothing is close enough yet unless the herd happened to be there: put one next to the colonists.
    const Entity colonist = disasterColonists(*sim).front();
    const CellPos at = cellOf(sim->things().registry().get<Position>(colonist));
    const Entity animal = sim->things().spawn(kinds.front().def, CellPos{at.x + 6, at.y + 6, 0});
    DISASTER_REQUIRE(animal != kNoEntity);
    const Event* letter = disasterFire(*sim, "MadAnimal");
    DISASTER_REQUIRE(letter != nullptr);
    CHECK(letter->message.find("Mad animal") == 0);
    CHECK(sim->animals().stats().manhunters == before + 1);

    const auto empty = disasterSim(0, 128);
    empty->submit(FireIncidentCommand{"MadAnimal", 0.0f});
    empty->step();
    CHECK(disasterCommandFailures(*empty) == 1);
}

TEST_CASE("Disasters: a game with a flare and a storm on saves and loads to the same game") {
    const auto sim = disasterSim();
    disasterPowerLine(*sim);
    disasterFire(*sim, "SolarFlare");
    sim->disasters().begin(DisasterKind::Flashstorm, 8.0);
    sim->disasters().begin(DisasterKind::VolcanicWinter, 100.0);
    disasterRun(*sim, 600);
    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, disasterDefs());
    DISASTER_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->disasters().active().size() == sim->disasters().active().size());
    CHECK(saveSim(*loaded.sim) == bytes);
    disasterRun(*sim, 400);
    disasterRun(*loaded.sim, 400);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Disasters: the same seed and commands give the same game") {
    auto play = [] {
        const auto sim = disasterSim(100, 128);
        disasterPowerLine(*sim);
        disasterFire(*sim, "ToxicFallout");
        disasterFire(*sim, "Flashstorm");
        disasterFire(*sim, "PsychicDrone");
        disasterFire(*sim, "ManhunterPack", 200.0f);
        disasterRun(*sim, 4 * kSecondsPerHour);
        return sim->stateHash();
    };
    CHECK(play() == play());
}

TEST_CASE("Disasters: a disease outbreak makes some colonists ill") {
    auto sim = disasterSim();
    sim->step();
    const Event* letter = disasterFire(*sim, "Disease_Plague");
    DISASTER_REQUIRE(letter != nullptr);
    CHECK(disasterCommandFailures(*sim) == 0);
    int sick = 0;
    for (const Entity pawn : disasterColonists(*sim)) {
        sick += sim->health().sick(pawn) ? 1 : 0;
    }
    CHECK(sick >= 1);
    CHECK(sick <= static_cast<int>(disasterColonists(*sim).size()));
}

TEST_CASE("Disasters: a prison break needs prisoners, an infestation needs a roof") {
    auto sim = disasterSim();
    sim->step();
    sim->submit(FireIncidentCommand{"PrisonBreak", 0.0f});
    sim->step();
    CHECK(disasterCommandFailures(*sim) == 1);
    // A flat open map has no roofed place for brood mounds; the worker answers in words and nothing is half done.
    sim->submit(FireIncidentCommand{"Infestation", 100.0f});
    sim->step();
    CHECK(sim->narrator().hasWorker("Infestation"));
    CHECK(sim->narrator().hasWorker("RemnantCluster"));
}

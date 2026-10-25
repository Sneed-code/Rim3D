#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define WEATHER_REQUIRE(...)   \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> weatherRealDefs() {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    return db;
}

// A tiny climate: the same temperature all year with no daily swing, a clear weather and two
// others, so tests can say exactly what happens. `weather` is the biome's commonalities.
std::shared_ptr<const DefDatabase> weatherSmallDefs(double temperature, const std::string& weather) {
    const std::string biome = R"([{"defName": "TemperateForest", "plantDensity": 0,
        "climate": {"seasonMeanTemperature": {"spring": )" + std::to_string(temperature) + ", \"summer\": " +
                              std::to_string(temperature) + ", \"fall\": " + std::to_string(temperature) + ", \"winter\": " +
                              std::to_string(temperature) + R"(}, "dailySwing": 0, "growingSeasons": ["summer"]},
        "weather": )" + weather + "}]";
    const std::vector<DefSource> sources = {
        {"Things", "test.json", R"([{"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}}])"},
        {"Terrain", "test.json", R"([{"defName": "Soil"}])"},
        {"Biomes", "test.json", biome},
        {"Weathers", "test.json", R"([
            {"defName": "Clear", "label": "clear", "durationHours": [1, 1], "temperatureRange": [-100, 100]},
            {"defName": "Fog", "label": "fog", "durationHours": [4, 4], "temperatureRange": [-100, 100], "accuracyFactor": 0.5,
             "sightFactor": 0.5, "skyGlow": 0.85},
            {"defName": "Rain", "label": "rain", "durationHours": [4, 4], "temperatureRange": [0, 100], "moveSpeedFactor": 0.9,
             "accuracyFactor": 0.8, "rainRate": 0.5, "extinguishesFire": true},
            {"defName": "SnowHard", "label": "heavy snow", "durationHours": [10, 10], "temperatureRange": [-100, -0.5],
             "moveSpeedFactor": 0.8, "snowRate": 1.2}
        ])"}};
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build(sources, "", *db, errors);
    return db;
}

SimSettings weatherSimSettings(uint64_t seed, int size = 64) {
    SimSettings s;
    s.seed = seed;
    s.mapSize = size;
    s.flatMap = true;
    s.checkpointEvery = 0;
    return s;
}

// The weather on its own over a small flat map, stepped second by second like the sim does.
struct WeatherRig {
    Map map;
    std::vector<Event> events;
    Weather weather;
    int64_t now = 0;

    WeatherRig(const DefDatabase& defs, uint64_t seed = 7, int size = 32)
        : map(MapSettings{size, size, seed, true}), weather(map, defs, seed, "TemperateForest") {
        weather.setEventLog(&events);
    }
    void run(int64_t seconds) {
        for (int64_t i = 0; i < seconds; ++i) {
            ++now;
            weather.step(now);
        }
    }
    void runHours(double hours) { run(static_cast<int64_t>(hours * 3600.0)); }
};

const CellPos kWeatherCell{10, 10, 0};

}  // namespace

TEST_CASE("Weather: the seasonal mean runs from one season's middle to the next") {
    auto defs = weatherRealDefs();
    WeatherRig rig(*defs);
    const Weather& w = rig.weather;
    const auto at = [&](double day) { return w.seasonalMean(static_cast<int64_t>(day * 86400.0)); };
    // The temperate forest's means: spring 9, summer 19, fall 9, winter -2, each at its season's middle.
    CHECK(at(7.5) == doctest::Approx(9.0).epsilon(0.001));
    CHECK(at(22.5) == doctest::Approx(19.0).epsilon(0.001));
    CHECK(at(37.5) == doctest::Approx(9.0).epsilon(0.001));
    CHECK(at(52.5) == doctest::Approx(-2.0).epsilon(0.001));
    // Between two middles it stays between their means, and the year comes round: day 60 is day 0.
    CHECK(at(15.0) == doctest::Approx(14.0).epsilon(0.001));
    CHECK(at(60.0) == doctest::Approx(at(0.0)));
    CHECK(at(0.0) == doctest::Approx(3.5).epsilon(0.001));  // half way between winter and spring
    for (double day = 0.0; day < 120.0; day += 0.25) {
        CHECK(at(day) >= -2.0 - 1e-9);
        CHECK(at(day) <= 19.0 + 1e-9);
    }
    CHECK_FALSE(w.growingSeason(3));
    CHECK(w.growingSeason(1));
}

TEST_CASE("Weather: a day is coldest before dawn and warmest in the afternoon, by the biome's swing") {
    auto defs = weatherRealDefs();
    WeatherRig rig(*defs);
    const Weather& w = rig.weather;
    CHECK(w.dailySwing() == doctest::Approx(14.0));
    double meanSwing = 0.0;
    int n = 0;
    for (int day = 0; day < 60; ++day) {
        const int64_t base = static_cast<int64_t>(day) * 86400;
        const double warm = w.temperatureAt(base + 16 * 3600 + 1200);
        const double cold = w.temperatureAt(base + 4 * 3600 + 1200);
        meanSwing += warm - cold;
        ++n;
        // Within the day the warmest hour is in the afternoon and the coldest before dawn, give or
        // take the drift's slope over the day.
        int hottest = 0;
        int coldest = 0;
        for (int h = 0; h < 24; ++h) {
            const double t = w.temperatureAt(base + h * 3600);
            if (t > w.temperatureAt(base + hottest * 3600)) hottest = h;
            if (t < w.temperatureAt(base + coldest * 3600)) coldest = h;
        }
        CHECK(std::abs(w.temperatureAt(base + hottest * 3600) - w.temperatureAt(base + coldest * 3600)) <= 14.0 + 8.0);
    }
    meanSwing /= n;
    CHECK(meanSwing == doctest::Approx(14.0).epsilon(0.1));  // the drift averages out over the 60 days
}

TEST_CASE("Weather: the temperature is the same for the same seed and moves with the seed") {
    auto defs = weatherRealDefs();
    WeatherRig a(*defs, 7), b(*defs, 7), c(*defs, 8);
    bool differs = false;
    for (int64_t t = 0; t < 60 * 86400; t += 7919) {
        CHECK(a.weather.temperatureAt(t) == b.weather.temperatureAt(t));
        differs = differs || a.weather.temperatureAt(t) != c.weather.temperatureAt(t);
        // The random drift stays within its amplitude of seasonal mean plus daily cycle.
        const double drift = a.weather.temperatureAt(t) - a.weather.seasonalMean(t);
        CHECK(std::abs(drift) <= 7.0 + 7.0 + 0.1);
    }
    CHECK(differs);
}

TEST_CASE("Weather: a year of weather follows the biome and the temperature") {
    auto defs = weatherRealDefs();
    WeatherRig rig(*defs, 3);
    rig.run(2 * 60 * 86400);  // two years
    std::vector<std::string> names;
    int changes = 0;
    int seasons = 0;
    for (const Event& e : rig.events) {
        if (e.type == EventType::SeasonChanged) {
            ++seasons;
            continue;
        }
        WEATHER_REQUIRE(e.type == EventType::WeatherChanged);
        ++changes;
        names.push_back(e.message);
        // Snow only when it is freezing, rain only when it is not, at the moment the weather is chosen.
        if (e.message == "light snow" || e.message == "heavy snow") {
            CHECK(e.amount <= -0.5f + 1e-3f);
        }
        if (e.message == "rain") {
            CHECK(e.amount >= 0.0f);
        }
    }
    CHECK(seasons == 8);
    CHECK(changes > 60);
    const auto count = [&](const char* label) { return std::count(names.begin(), names.end(), label); };
    CHECK(count("clear") > count("fog"));
    CHECK(count("clear") > count("rain"));
    CHECK(count("light snow") + count("heavy snow") > 0);   // winters are cold enough
    CHECK(count("rain") > 0);
}

TEST_CASE("Weather: each weather lasts as long as its definition says") {
    auto defs = weatherSmallDefs(10.0, R"({"Clear": 1, "Fog": 1, "Rain": 1})");
    WeatherRig rig(*defs, 5);
    int64_t startedAt = rig.weather.weatherStartedAt();
    DefId was = rig.weather.current();
    int checked = 0;
    for (int64_t t = 1; t < 20 * 86400; ++t) {
        rig.now = t;
        rig.weather.step(t);
        if (rig.weather.weatherStartedAt() != startedAt) {
            const int64_t length = t - startedAt;
            const std::string was_ = defs->get(was).defName();
            CHECK(length == (was_ == "Clear" ? 3600 : 4 * 3600));
            ++checked;
            startedAt = rig.weather.weatherStartedAt();
            was = rig.weather.current();
        }
    }
    CHECK(checked > 20);
}

TEST_CASE("Weather: the effects blend from the old weather into the new over the transition") {
    auto defs = weatherSmallDefs(10.0, R"({"Clear": 1})");
    WeatherRig rig(*defs);
    CHECK(rig.weather.effects().accuracyFactor == doctest::Approx(1.0f));
    rig.run(10);
    CHECK(rig.weather.force("Fog", 3.0));
    CHECK(rig.weather.transition() == doctest::Approx(0.0f));
    CHECK(rig.weather.effects().accuracyFactor == doctest::Approx(1.0f));
    rig.run(2880);  // half of the 1.6 hour transition
    CHECK(rig.weather.transition() == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(rig.weather.effects().accuracyFactor == doctest::Approx(0.75f).epsilon(0.01));
    CHECK(rig.weather.effects().sightFactor == doctest::Approx(0.75f).epsilon(0.01));
    rig.run(3000);
    CHECK(rig.weather.transition() == doctest::Approx(1.0f));
    CHECK(rig.weather.effects().accuracyFactor == doctest::Approx(0.5f));
    CHECK(rig.weather.effects().skyGlow == doctest::Approx(0.85f));
    CHECK_FALSE(rig.weather.force("NoSuchWeather"));
    // A WeatherChanged event came with the change.
    int changed = 0;
    for (const Event& e : rig.events) {
        changed += e.type == EventType::WeatherChanged && e.message == "fog" ? 1 : 0;
    }
    CHECK(changed == 1);
}

TEST_CASE("Weather: the weather affects only cells under the open sky") {
    auto defs = weatherSmallDefs(10.0, R"({"Clear": 1})");
    WeatherRig rig(*defs);
    rig.weather.force("Rain", 5.0);
    rig.run(3 * 3600);
    CHECK(rig.weather.outdoors(kWeatherCell));
    CHECK(rig.weather.moveFactorAt(kWeatherCell) == doctest::Approx(0.9f));
    CHECK(rig.weather.accuracyAt(kWeatherCell) == doctest::Approx(0.8f));
    CHECK(rig.weather.sightAt(kWeatherCell) == doctest::Approx(1.0f));
    CHECK_FALSE(rig.weather.outdoors(CellPos{-1, 0, 0}));
}

TEST_CASE("Weather: snow falls by the weather's rate and slows walking by its depth") {
    auto defs = weatherSmallDefs(-10.0, R"({"SnowHard": 1})");
    WeatherRig rig(*defs);
    // Clear for an hour, then the only weather this cold climate has: heavy snow, 5% an hour at rate 1, 6% at 1.2,
    // and the rate eases in over the transition of 1.6 hours.
    rig.runHours(1.0 + 1.0);
    WEATHER_REQUIRE(defs->get(rig.weather.current()).defName() == "SnowHard");
    CHECK(rig.weather.snowDepth(kWeatherCell) == doctest::Approx(3.75f).epsilon(0.01));  // 0.625 of the rate
    CHECK(rig.weather.snowCellCount() == 32u * 32u);
    rig.runHours(1.0);  // the transition is over
    const float before = rig.weather.snowDepth(kWeatherCell);
    rig.runHours(4.0);
    CHECK(rig.weather.snowDepth(kWeatherCell) - before == doctest::Approx(24.0f).epsilon(0.01));
    CHECK(rig.weather.moveFactorAt(kWeatherCell) == doctest::Approx(0.8f * 0.76f).epsilon(0.001));  // weather x thin snow
    rig.runHours(0.0);
    while (rig.weather.snowDepth(kWeatherCell) < 50.0f && rig.now < 20 * 3600) {
        rig.runHours(1.0);
    }
    CHECK(rig.weather.moveFactorAt(kWeatherCell) == doctest::Approx(0.8f * 0.62f).epsilon(0.001));
    // Never deeper than 100%.
    rig.runHours(60.0);
    CHECK(rig.weather.snowDepth(kWeatherCell) == doctest::Approx(100.0f));
    CHECK(rig.weather.snowSpeedFactor(0.0f) == 1.0f);
    CHECK(rig.weather.snowSpeedFactor(2.9f) == 1.0f);
    CHECK(rig.weather.snowSpeedFactor(25.0f) == doctest::Approx(0.76f));
    CHECK(rig.weather.snowSpeedFactor(60.0f) == doctest::Approx(0.62f));
    CHECK(rig.weather.snowSpeedFactor(75.0f) == doctest::Approx(0.52f));
    CHECK(rig.weather.snowSpeedFactor(100.0f) == doctest::Approx(0.52f));
}

TEST_CASE("Weather: snow melts by RimWorld's rule, not at all below freezing") {
    // The state to put back: clear weather ending far off, and a cell under half a metre of snow.
    for (const double temperature : {-5.0, 0.0, 5.0, 10.0, 20.0}) {
        auto defs = weatherSmallDefs(temperature, R"({"Clear": 1})");
        WeatherRig rig(*defs);
        WeatherState state = rig.weather.state();
        state.endsAt = 1000000000;
        WEATHER_REQUIRE(rig.weather.restore(state, {WeatherSnowRecord{rig.map.index(kWeatherCell), 5000}}, 0));
        rig.runHours(1.0);
        const double melt = temperature <= 0.0 ? 0.0 : (temperature < 10.0 ? 0.058 * temperature * temperature : 0.58 * temperature);
        CHECK(rig.weather.snowDepth(kWeatherCell) == doctest::Approx(50.0 - melt).epsilon(0.001));
    }
}

TEST_CASE("Weather: restoring refuses snow that is off the map or out of order") {
    auto defs = weatherSmallDefs(0.0, R"({"Clear": 1})");
    WeatherRig rig(*defs);
    const WeatherState state = rig.weather.state();
    CHECK_FALSE(rig.weather.restore(state, {WeatherSnowRecord{32 * 32, 100}}, 0));
    CHECK_FALSE(rig.weather.restore(state, {WeatherSnowRecord{5, 100}, WeatherSnowRecord{5, 100}}, 0));
    CHECK_FALSE(rig.weather.restore(state, {WeatherSnowRecord{5, 20000}}, 0));
    CHECK_FALSE(rig.weather.restore(state, {WeatherSnowRecord{5, 0}}, 0));
    CHECK(rig.weather.restore(state, {WeatherSnowRecord{5, 100}, WeatherSnowRecord{6, 200}}, 0));
    CHECK(rig.weather.snowCellCount() == 2u);
}

TEST_CASE("Weather: with no climate defs the weather stays clear and the temperature is fixed") {
    Sim sim(weatherSimSettings(1), nullptr);
    sim.stepMany(3 * 3600);
    CHECK_FALSE(sim.weather().current().valid());
    CHECK(sim.weather().effects().moveSpeedFactor == 1.0f);
    CHECK(sim.weather().outdoorTemperature() == doctest::Approx(15.0));
    CHECK(sim.weather().moveFactorAt(kWeatherCell) == 1.0f);
}

TEST_CASE("Weather: rain slows a walking pawn by the weather's factor") {
    auto defs = weatherSmallDefs(10.0, R"({"Clear": 1})");
    float walked[2] = {0.0f, 0.0f};
    for (int rain = 0; rain < 2; ++rain) {
        Sim sim(weatherSimSettings(2), defs);
        if (rain != 0) {
            sim.submit(SetWeatherCommand{"Rain", 6.0f});
        }
        const Entity pawn = sim.things().spawn("Human", CellPos{5, 20, 0});
        WEATHER_REQUIRE(pawn != kNoEntity);
        sim.step();  // the weather command
        sim.stepMany(3 * 3600);  // past the transition
        const Position start = sim.things().registry().get<Position>(pawn);
        std::vector<CellPos> path;
        for (int x = 6; x < 60; ++x) {
            path.push_back(CellPos{x, 20, 0});
        }
        sim.things().registry().get<Mover>(pawn).walkTo(start.x, start.y, path);
        sim.stepMany(100);
        walked[rain] = sim.things().registry().get<Position>(pawn).x - start.x;
    }
    CHECK(walked[0] > 5.0f);
    CHECK(walked[1] / walked[0] == doctest::Approx(0.9f).epsilon(0.02));
}

TEST_CASE("Weather: fog and rain cut a shooter's hit chance") {
    auto defs = weatherRealDefs();
    Sim sim(weatherSimSettings(4, 128), defs);
    const DefId rifle = defs->idOf("Things", "Gun_AssaultRifle");
    WEATHER_REQUIRE(rifle.valid());
    const float clear = sim.combat().spread(rifle, 8, 30.0f, false, 1.0f);
    const float foggy = sim.combat().spread(rifle, 8, 30.0f, false, 0.5f);
    CHECK(foggy > clear);
    CHECK(sim.combat().spread(rifle, 8, 30.0f, false) == clear);
    const float player = sim.combat().spread(rifle, 8, 30.0f, true, 1.0f);
    CHECK(sim.combat().spread(rifle, 8, 30.0f, true, 0.5f) > player);
}

TEST_CASE("Weather: cloud and fog dim the sun") {
    auto defs = weatherRealDefs();
    Sim sim(weatherSimSettings(5, 64), defs);
    sim.submit(SetWeatherCommand{"Clear", 20.0f});
    sim.stepMany(11 * 3600);  // 11:00, a clear day: the sun at full
    CHECK(sim.lights().skyGlow() == doctest::Approx(1.0f));
    sim.submit(SetWeatherCommand{"RainyThunderstorm", 6.0f});
    sim.stepMany(2 * 3600);  // past the transition
    CHECK(sim.lights().skyGlow() == doctest::Approx(0.45f).epsilon(0.001));
    CHECK(sim.lights().lightAt(kWeatherCell) == doctest::Approx(0.45f).epsilon(0.001));
    sim.stepMany(8 * 3600);  // 21:00: night is dark whatever the weather
    CHECK(sim.lights().skyGlow() == doctest::Approx(0.0f));
}

TEST_CASE("Weather: the dev command sets the weather, and a bad name fails") {
    auto defs = weatherRealDefs();
    Sim sim(weatherSimSettings(4, 128), defs);
    sim.submit(SetWeatherCommand{"Fog", 2.0f});
    sim.submit(SetWeatherCommand{"NoSuchWeather", 0.0f});
    sim.step();
    CHECK(sim.weather().current() == defs->idOf("Weathers", "Fog"));
    CHECK(sim.weather().weatherEndsAt() - sim.weather().weatherStartedAt() == 7200);
    int failed = 0;
    int changed = 0;
    for (const Event& e : sim.events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
        changed += e.type == EventType::WeatherChanged ? 1 : 0;
    }
    CHECK(failed == 1);
    CHECK(changed == 1);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.weather.weather == defs->idOf("Weathers", "Fog"));
    CHECK(snapshot.weather.season == 0);
    CHECK(snapshot.weather.dayOfYear == 0);
    CHECK(snapshot.weather.temperature == doctest::Approx(static_cast<float>(sim.weather().outdoorTemperature())));
}

TEST_CASE("Weather: the same seed gives the same weather and snow, another seed does not") {
    auto defs = weatherRealDefs();
    uint64_t hashes[3] = {};
    const uint64_t seeds[3] = {11, 11, 12};
    for (int i = 0; i < 3; ++i) {
        Sim sim(weatherSimSettings(seeds[i], 64), defs);
        sim.submit(SetWeatherCommand{"SnowHard", 8.0f});
        sim.stepMany(10 * 3600);
        hashes[i] = sim.weather().hash();
        CHECK(sim.weather().snowCellCount() > 0u);
    }
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] != hashes[2]);
}

TEST_CASE("Weather: saving and loading mid-snow gives the same game on") {
    auto defs = weatherRealDefs();
    Sim original(weatherSimSettings(21, 128), defs);
    original.submit(SetWeatherCommand{"SnowHard", 6.0f});
    original.stepMany(4 * 3600 + 17);
    WEATHER_REQUIRE(original.weather().snowCellCount() > 0u);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    WEATHER_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.weather().current() == original.weather().current());
    CHECK(copy.weather().snowCellCount() == original.weather().snowCellCount());
    CHECK(copy.weather().snowDepth(CellPos{40, 40, 0}) == original.weather().snowDepth(CellPos{40, 40, 0}));
    CHECK(copy.weather().outdoorTemperature() == original.weather().outdoorTemperature());
    // And on for two days, through the end of this weather and the choice of the next ones.
    for (int hour = 0; hour < 48; ++hour) {
        original.stepMany(3600);
        copy.stepMany(3600);
        CHECK(copy.stateHash() == original.stateHash());
    }
    CHECK(saveSim(copy) == saveSim(original));
}

TEST_CASE("Weather: a save without the weather section loads with clear weather at the saved time") {
    auto defs = weatherRealDefs();
    Sim original(weatherSimSettings(22, 64), defs);
    original.stepMany(2 * 3600);
    std::string json = saveToJson(original);
    CHECK(json.find("\"weather\"") != std::string::npos);
    // The snapshot of a loaded sim names the same date.
    SaveLoadResult loaded = loadSim(saveSim(original), defs);
    WEATHER_REQUIRE(loaded.sim != nullptr);
    Snapshot a, b;
    original.writeSnapshot(a);
    loaded.sim->writeSnapshot(b);
    CHECK(a.weather.dayOfYear == b.weather.dayOfYear);
    CHECK(a.weather.temperature == b.weather.temperature);
}

TEST_CASE("Weather: a replay with a weather command plays back to the same hashes") {
    auto defs = weatherRealDefs();
    SimSettings settings = weatherSimSettings(23, 64);
    settings.checkpointEvery = 600;
    Sim sim(settings, defs);
    sim.submit(SetWeatherCommand{"Rain", 3.0f});
    sim.stepMany(3600);
    sim.submit(SetWeatherCommand{"Fog", 1.5f});
    sim.stepMany(3600);
    const Replay recorded = recordReplay(sim);
    const std::string text = writeReplay(recorded);
    CHECK(text.find("SetWeather") != std::string::npos);
    Replay parsed;
    std::string error;
    WEATHER_REQUIRE(readReplay(text, parsed, error));
    CHECK(error.empty());
    WEATHER_REQUIRE(parsed.commands.size() == 2);
    const SetWeatherCommand* second = std::get_if<SetWeatherCommand>(&parsed.commands[1].command);
    WEATHER_REQUIRE(second != nullptr);
    CHECK(second->weather == "Fog");
    CHECK(second->hours == doctest::Approx(1.5f));
    Sim again(parsed.settings, defs);
    playReplay(parsed, again, parsed.endStep);
    CHECK(checkReplay(parsed, again).matched());
    CHECK(again.stateHash() == sim.stateHash());
}

TEST_CASE("Weather: wind follows the weather and drifts, and power reads it") {
    auto defs = weatherRealDefs();
    Sim sim(weatherSimSettings(6, 64), defs);
    sim.submit(SetWeatherCommand{"Clear", 1000.0f});
    double lowest = 10.0;
    double highest = -1.0;
    for (int hour = 0; hour < 5 * 24; ++hour) {
        sim.stepMany(3600);
        lowest = std::min(lowest, sim.weather().wind());
        highest = std::max(highest, sim.weather().wind());
    }
    CHECK(lowest >= 0.0);
    CHECK(highest <= 1.5);
    CHECK(highest - lowest > 0.5);  // gusts come and go
    // Fog is stiller than a storm, once each has fully taken over.
    sim.submit(SetWeatherCommand{"Fog", 1000.0f});
    sim.stepMany(3 * 3600);
    const double foggy = sim.weather().wind();
    CHECK(foggy <= 0.5 * 1.75 + 1e-9);
    sim.submit(SetWeatherCommand{"RainyThunderstorm", 1000.0f});
    sim.stepMany(3 * 3600);
    CHECK(sim.weather().wind() >= 0.25 * 1.5 - 1e-9);
    // With no climate defs the wind is steady.
    Sim bare(weatherSimSettings(6, 64), nullptr);
    bare.stepMany(5 * 3600);
    CHECK(bare.weather().wind() == 1.0);
}

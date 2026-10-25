#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class Map;
struct Event;

// Name of a season: 0 spring, 1 summer, 2 fall, 3 winter (GameTime::season()).
COLONY_API const char* seasonName(int season);

// What the weather does to the ground and to what happens under the open sky, with the next
// weather blended in over the transition. All factors are 1 and all rates 0 when nothing is set.
struct COLONY_API WeatherEffects {
    float skyGlow = 1.0f;          // share of the sun's light that gets through
    float moveSpeedFactor = 1.0f;  // pawns under the open sky
    float accuracyFactor = 1.0f;   // shooters under the open sky
    float sightFactor = 1.0f;      // how far a pawn under the open sky sees
    float windSpeedFactor = 1.0f;  // times the wind's gusts
    float rainRate = 0.0f;
    float snowRate = 0.0f;
    float wetness = 0.0f;
    bool extinguishesFire = false;
    bool lightning = false;
};

// One snow cell as saved: the cell's index in the map and its depth in hundredths of a percent.
struct WeatherSnowRecord {
    int cell = 0;
    uint16_t depth = 0;
};

// The weather's own state as saved, apart from the snow.
struct WeatherState {
    DefId current;
    DefId previous;
    int64_t startedAt = 0;  // game second the current weather began
    int64_t endsAt = 0;     // game second the next one is chosen
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
};

// Seasons, outdoor temperature, weather and snow (docs/design/weather.md), from the biome's
// `climate` and `weather` and the Weathers defs.
//
// The year is 60 days of four seasons (`GameTime`). The outdoor temperature is the season's mean
// (smoothly carried from one season's middle to the next) plus a cycle over the day plus a slow
// random drift, a pure function of the seed and the time. Weather lasts for a time, then the next
// is picked by the biome's commonalities among the kinds that can happen at the temperature then.
// Snow falls and melts hourly on cells under the open sky.
//
// Everything is worked out with plain arithmetic (no sin or cos), so every platform gives the same
// bits. With no biome or no Weathers defs the weather stays clear and the temperature constant.
class COLONY_API Weather {
public:
    Weather(const Map& map, const DefDatabase& defs, uint64_t seed, const std::string& biome);
    ~Weather();
    Weather(const Weather&) = delete;
    Weather& operator=(const Weather&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }

    // The clock has just advanced to `seconds`: starts the next weather when the current one is
    // over, updates the snow on the hour, and says so in events (WeatherChanged, SeasonChanged).
    void step(int64_t seconds);

    // ---- Temperature -----------------------------------------------------------------------

    // Outdoor temperature in degrees Celsius at a game second, and now.
    double temperatureAt(int64_t seconds) const;
    double outdoorTemperature() const { return temperatureAt(now_); }
    // The season's mean at a game second (no day cycle or drift), and the biome's daily swing.
    double seasonalMean(int64_t seconds) const;
    double dailySwing() const { return dailySwing_; }
    // The mean of the four seasons' means: the temperature under rock.
    double yearMeanTemperature() const {
        return (seasonMean_[0] + seasonMean_[1] + seasonMean_[2] + seasonMean_[3]) * 0.25;
    }
    // Seasons the biome grows crops in (a bit per season, bit 0 spring).
    bool growingSeason(int season) const { return (growingSeasons_ >> season & 1u) != 0; }

    // ---- Weather ---------------------------------------------------------------------------

    DefId current() const { return state_.current; }
    DefId previous() const { return state_.previous; }
    // 0 when the current weather has just begun to 1 once it has fully taken over from the previous.
    float transition() const;
    // The effects now: the previous weather's, blended into the current one's over the transition.
    WeatherEffects effects() const;
    // The share of the sun's light that gets through now (effects().skyGlow, but cheap to ask often).
    float skyGlow() const { return cachedEffects().skyGlow; }
    // The wind now as a factor of a wind turbine's output: the weather's wind factor (still in fog, strong
    // in storms and snow) times gusts that drift over hours, 0 to `windMax` (1.5). Worked out from the
    // seed and the time, so it has no state of its own.
    double wind() const;
    // How much faster things lying under the open sky wear now: the wiki's 5 while it rains, else 1. For
    // SpoilageSystem::setOutdoorFactor.
    float deteriorationFactor() const { return cachedEffects().rainRate > 0.001f ? static_cast<float>(rainDeterioration_) : 1.0f; }
    // Game second the current weather ends, and the one it began.
    int64_t weatherEndsAt() const { return state_.endsAt; }
    int64_t weatherStartedAt() const { return state_.startedAt; }
    // Forces the weather now, for tests and the dev command: it lasts `hours` hours (0: its usual
    // length), then the next is chosen as usual. False if no Weathers def has this name.
    bool force(const std::string& defName, double hours = 0.0);

    // ---- What a pawn at a cell feels -------------------------------------------------------

    // Under the open sky: no roof, thin or thick. Weather affects only cells that are.
    bool outdoors(CellPos c) const;
    // Walking speed factor at a cell: the weather's and the snow's, 1 under a roof.
    float moveFactorAt(CellPos c) const;
    // Hit chance factor for a shooter standing in a cell, and the share of its sight range.
    float accuracyAt(CellPos c) const;
    float sightAt(CellPos c) const;

    // ---- Snow ------------------------------------------------------------------------------

    // Depth in percent, 0 to 100. Only cells under the open sky and not water hold snow.
    float snowDepth(CellPos c) const;
    // Share of walking speed kept on snow of this depth (percent): 1 up to a dusting, 0.52 on thick snow.
    float snowSpeedFactor(float depthPercent) const;
    // Cells with any snow, and a number that goes up whenever the snow changes (Unreal redraws then).
    size_t snowCellCount() const { return snowCells_; }
    uint64_t snowChanges() const { return snowChanges_; }

    // Fingerprint of the weather and the snow; the snow's part is kept as it changes.
    uint64_t hash() const;

    // For saves.
    WeatherState state() const;
    std::vector<WeatherSnowRecord> snowRecords() const;
    // Puts the weather back. False (leaving the weather as it was) if a record is off the map.
    bool restore(const WeatherState& state, const std::vector<WeatherSnowRecord>& snow, int64_t now);

private:
    struct Kind {
        DefId def;
        std::string name;
        std::string label;
        float hoursMin = 6.0f;
        float hoursMax = 12.0f;
        double tempMin = -100.0;
        double tempMax = 100.0;
        double commonality = 0.0;
        WeatherEffects effects;
    };

    const Kind* kindOf(DefId def) const;
    const WeatherEffects& cachedEffects() const;
    double drift(int64_t seconds) const;
    void chooseNext(int64_t seconds);
    void begin(const Kind* kind, int64_t seconds, double hours);
    void updateSnow();
    bool holdsSnow(int index) const;
    void announce(const Kind* kind);

    const Map& map_;
    std::vector<Event>* events_ = nullptr;
    uint64_t seed_;
    std::vector<Kind> kinds_;
    std::vector<int> kindOfDef_;  // by DefId.value: index into kinds_ or -1
    DefId clear_;
    Rng rng_;
    WeatherState state_;  // the random stream in it is only filled in by state()
    int64_t now_ = 0;
    mutable WeatherEffects cache_;  // effects() as of game second `cacheAt_`
    mutable int64_t cacheAt_ = -1;

    // Climate.
    double seasonMean_[kSeasonsPerYear] = {15.0, 15.0, 15.0, 15.0};
    double dailySwing_ = 0.0;
    unsigned growingSeasons_ = 0xF;
    double dayCyclePhase_ = 0.32;
    double driftAmplitude_ = 0.0;
    double driftDays_ = 2.0;
    double windGustDays_ = 0.25;
    double windGustMin_ = 0.25;
    double windGustMax_ = 1.75;
    double windMax_ = 1.5;
    double rainDeterioration_ = 5.0;
    double transitionSeconds_ = 5760.0;

    // Snow, in hundredths of a percent per cell.
    std::vector<uint16_t> snow_;
    size_t snowCells_ = 0;
    uint64_t snowHash_ = 0;
    uint64_t snowChanges_ = 0;
    std::vector<double> speedDepths_;
    std::vector<double> speedFactors_;
    double snowPerHour_ = 5.0;
    double meltLow_ = 0.058;
    double meltHigh_ = 0.58;
};

}  // namespace colony

#include "ColonySim/Weather.h"

#include "ColonySim/Events.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr const char* kWeatherSeasonNames[kSeasonsPerYear] = {"spring", "summer", "fall", "winter"};
constexpr uint16_t kWeatherMaxSnow = 10000;  // hundredths of a percent: 100%

double weatherFloor(double x) { return std::floor(x); }

// cos(2 pi u) from plain arithmetic: Bhaskara's sine approximation (error under 0.002), so every
// platform gives the same bits where libm's cos may differ in the last one.
double weatherCosTurns(double u) {
    double v = u + 0.25;  // cos(2 pi u) = sin(2 pi (u + 1/4))
    v -= weatherFloor(v);
    const bool negative = v >= 0.5;
    const double w = negative ? v - 0.5 : v;
    const double q = w * (0.5 - w);
    const double s = 64.0 * q / (5.0 - 16.0 * q);
    return negative ? -s : s;
}

double weatherSmooth(double t) { return t * t * (3.0 - 2.0 * t); }

double weatherLattice(uint64_t seed, int64_t i) {
    const uint64_t h = splitMix64(splitMix64(seed ^ 0x6472696674ULL) ^ static_cast<uint64_t>(i));
    return static_cast<double>(h >> 40) * (1.0 / 16777216.0);
}

float weatherLerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

const char* seasonName(int season) {
    return season >= 0 && season < kSeasonsPerYear ? kWeatherSeasonNames[season] : "?";
}

Weather::Weather(const Map& map, const DefDatabase& defs, uint64_t seed, const std::string& biome)
    : map_(map),
      seed_(seed),
      rng_(splitMix64(seed ^ 0x7765617468ULL)),
      snow_(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), 0) {
    transitionSeconds_ = 5760.0;
    speedDepths_ = {3.0, 25.0, 50.0, 75.0};
    speedFactors_ = {1.0, 0.76, 0.62, 0.52};
    double defaultMean = 15.0;
    if (const Def* rules = defs.find("Rules", "ClimateRules")) {
        dayCyclePhase_ = rules->number("dayCyclePhase", dayCyclePhase_);
        driftAmplitude_ = rules->number("randomVariationAmplitude", driftAmplitude_);
        driftDays_ = std::max(0.1, rules->number("randomVariationDays", driftDays_));
        transitionSeconds_ = rules->number("transitionHours", 1.6) * static_cast<double>(kSecondsPerHour);
        defaultMean = rules->number("defaultMeanTemperature", defaultMean);
        windGustDays_ = std::max(0.01, rules->number("windGustDays", windGustDays_));
        windGustMin_ = rules->numberAt("windGustRange", 0, windGustMin_);
        windGustMax_ = rules->numberAt("windGustRange", 1, windGustMax_);
        windMax_ = rules->number("windMax", windMax_);
        snowPerHour_ = rules->number("snowPercentPerHourAtRate1", snowPerHour_);
        meltLow_ = rules->number("snowMeltBelow10PerDegreeSquared", meltLow_);
        meltHigh_ = rules->number("snowMeltAbove10PerDegree", meltHigh_);
        const size_t n = std::min(rules->count("snowSpeedDepths"), rules->count("snowSpeedFactors"));
        if (n > 0) {
            speedDepths_.clear();
            speedFactors_.clear();
            for (size_t i = 0; i < n; ++i) {
                speedDepths_.push_back(rules->numberAt("snowSpeedDepths", i));
                speedFactors_.push_back(rules->numberAt("snowSpeedFactors", i, 1.0));
            }
        }
    }
    for (double& mean : seasonMean_) {
        mean = defaultMean;
    }
    const Def* biomeDef = defs.find("Biomes", biome);
    if (biomeDef != nullptr && biomeDef->has("climate.seasonMeanTemperature")) {
        for (int s = 0; s < kSeasonsPerYear; ++s) {
            seasonMean_[s] = biomeDef->number((std::string("climate.seasonMeanTemperature.") + kWeatherSeasonNames[s]).c_str(),
                                              defaultMean);
        }
        dailySwing_ = biomeDef->number("climate.dailySwing", 0.0);
        growingSeasons_ = 0;
        for (size_t i = 0; i < biomeDef->count("climate.growingSeasons"); ++i) {
            const std::string name = biomeDef->textAt("climate.growingSeasons", i);
            for (int s = 0; s < kSeasonsPerYear; ++s) {
                if (name == kWeatherSeasonNames[s]) {
                    growingSeasons_ |= 1u << s;
                }
            }
        }
    } else {
        driftAmplitude_ = 0.0;  // no biome: a constant temperature
    }

    kindOfDef_.assign(defs.size(), -1);
    for (const DefId id : defs.idsOfKind("Weathers")) {
        const Def& def = defs.get(id);
        Kind k;
        k.def = id;
        k.name = def.defName();
        k.label = def.label();
        k.hoursMin = static_cast<float>(def.numberAt("durationHours", 0, 6.0));
        k.hoursMax = std::max(k.hoursMin, static_cast<float>(def.numberAt("durationHours", 1, k.hoursMin)));
        k.tempMin = def.numberAt("temperatureRange", 0, -100.0);
        k.tempMax = def.numberAt("temperatureRange", 1, 100.0);
        if (biomeDef != nullptr) {
            k.commonality = biomeDef->number(("weather." + def.defName()).c_str(), 0.0);
        }
        k.effects.skyGlow = static_cast<float>(def.number("skyGlow", 1.0));
        k.effects.moveSpeedFactor = static_cast<float>(def.number("moveSpeedFactor", 1.0));
        k.effects.accuracyFactor = static_cast<float>(def.number("accuracyFactor", 1.0));
        k.effects.sightFactor = static_cast<float>(def.number("sightFactor", 1.0));
        k.effects.windSpeedFactor = static_cast<float>(def.number("windSpeedFactor", 1.0));
        k.effects.rainRate = static_cast<float>(def.number("rainRate", 0.0));
        k.effects.snowRate = static_cast<float>(def.number("snowRate", 0.0));
        k.effects.wetness = static_cast<float>(def.number("wetness", 0.0));
        k.effects.extinguishesFire = def.flag("extinguishesFire");
        k.effects.lightning = def.flag("lightning");
        kindOfDef_[id.value] = static_cast<int>(kinds_.size());
        kinds_.push_back(std::move(k));
    }
    clear_ = defs.idOf("Weathers", "Clear");
    if (const Kind* clear = kindOf(clear_)) {
        begin(clear, 0, 0.0);
    }
}

Weather::~Weather() = default;

const Weather::Kind* Weather::kindOf(DefId def) const {
    if (!def.valid() || def.value >= kindOfDef_.size() || kindOfDef_[def.value] < 0) {
        return nullptr;
    }
    return &kinds_[static_cast<size_t>(kindOfDef_[def.value])];
}

// ---- Temperature -----------------------------------------------------------------------------

double Weather::seasonalMean(int64_t seconds) const {
    const int64_t inYear = seconds % (kDaysPerYear * kSecondsPerDay);
    // In seasons since the year began, taken from the middle of the first (the season's mean is
    // where its middle is), so the mean runs smoothly from one season's middle to the next's.
    const double p = static_cast<double>(inYear) / static_cast<double>(kDaysPerSeason * kSecondsPerDay) - 0.5;
    const double i0 = weatherFloor(p);
    const int first = ((static_cast<int>(i0) % kSeasonsPerYear) + kSeasonsPerYear) % kSeasonsPerYear;
    const int second = (first + 1) % kSeasonsPerYear;
    return seasonMean_[first] + (seasonMean_[second] - seasonMean_[first]) * weatherSmooth(p - i0);
}

double Weather::drift(int64_t seconds) const {
    if (driftAmplitude_ <= 0.0) {
        return 0.0;
    }
    const double x = static_cast<double>(seconds) / (driftDays_ * static_cast<double>(kSecondsPerDay));
    const double i = weatherFloor(x);
    const double a = weatherLattice(seed_, static_cast<int64_t>(i));
    const double b = weatherLattice(seed_, static_cast<int64_t>(i) + 1);
    const double v = a + (b - a) * weatherSmooth(x - i);
    return (2.0 * v - 1.0) * driftAmplitude_;
}

double Weather::temperatureAt(int64_t seconds) const {
    const double dayPercent = static_cast<double>(seconds % kSecondsPerDay) / static_cast<double>(kSecondsPerDay);
    return seasonalMean(seconds) + weatherCosTurns(dayPercent + dayCyclePhase_) * dailySwing_ * 0.5 + drift(seconds);
}

// ---- Weather ---------------------------------------------------------------------------------

float Weather::transition() const {
    if (transitionSeconds_ <= 0.0) {
        return 1.0f;
    }
    const double t = static_cast<double>(now_ - state_.startedAt) / transitionSeconds_;
    return static_cast<float>(std::clamp(t, 0.0, 1.0));
}

WeatherEffects Weather::effects() const {
    const Kind* now = kindOf(state_.current);
    if (now == nullptr) {
        return WeatherEffects{};
    }
    const Kind* before = kindOf(state_.previous);
    const float t = transition();
    if (before == nullptr || before == now || t >= 1.0f) {
        return now->effects;
    }
    const WeatherEffects& a = before->effects;
    const WeatherEffects& b = now->effects;
    WeatherEffects out;
    out.skyGlow = weatherLerp(a.skyGlow, b.skyGlow, t);
    out.moveSpeedFactor = weatherLerp(a.moveSpeedFactor, b.moveSpeedFactor, t);
    out.accuracyFactor = weatherLerp(a.accuracyFactor, b.accuracyFactor, t);
    out.sightFactor = weatherLerp(a.sightFactor, b.sightFactor, t);
    out.windSpeedFactor = weatherLerp(a.windSpeedFactor, b.windSpeedFactor, t);
    out.rainRate = weatherLerp(a.rainRate, b.rainRate, t);
    out.snowRate = weatherLerp(a.snowRate, b.snowRate, t);
    out.wetness = weatherLerp(a.wetness, b.wetness, t);
    out.extinguishesFire = t >= 0.5f ? b.extinguishesFire : a.extinguishesFire;
    out.lightning = t >= 0.5f ? b.lightning : a.lightning;
    return out;
}

void Weather::begin(const Kind* kind, int64_t seconds, double hours) {
    state_.previous = state_.current.valid() ? state_.current : kind->def;
    state_.current = kind->def;
    state_.startedAt = seconds;
    const double length = hours > 0.0 ? hours : static_cast<double>(rng_.range(kind->hoursMin, kind->hoursMax));
    state_.endsAt = seconds + std::max<int64_t>(1, std::llround(length * static_cast<double>(kSecondsPerHour)));
}

WeatherState Weather::state() const {
    WeatherState out = state_;
    out.rngState = rng_.state();
    out.rngIncrement = rng_.increment();
    return out;
}

void Weather::announce(const Kind* kind) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = EventType::WeatherChanged;
    e.def = kind->def;
    e.message = kind->label;
    e.amount = static_cast<float>(outdoorTemperature());
    events_->push_back(std::move(e));
}

void Weather::chooseNext(int64_t seconds) {
    const double temperature = temperatureAt(seconds);
    double total = 0.0;
    for (const Kind& k : kinds_) {
        if (k.commonality > 0.0 && temperature >= k.tempMin && temperature <= k.tempMax) {
            total += k.commonality;
        }
    }
    const Kind* chosen = nullptr;
    if (total > 0.0) {
        double r = static_cast<double>(rng_.unit()) * total;
        for (const Kind& k : kinds_) {
            if (k.commonality > 0.0 && temperature >= k.tempMin && temperature <= k.tempMax) {
                chosen = &k;
                if (r < k.commonality) {
                    break;
                }
                r -= k.commonality;
            }
        }
    }
    if (chosen == nullptr) {
        chosen = kindOf(clear_);
    }
    if (chosen == nullptr) {
        state_.endsAt = seconds + kSecondsPerHour;
        return;
    }
    const bool changed = chosen->def != state_.current;
    begin(chosen, seconds, 0.0);
    if (changed) {
        announce(chosen);
    }
}

bool Weather::force(const std::string& defName, double hours) {
    for (const Kind& k : kinds_) {
        if (k.name == defName) {
            const bool changed = k.def != state_.current;
            begin(&k, now_, hours);
            if (changed) {
                announce(&k);
            }
            return true;
        }
    }
    return false;
}

void Weather::step(int64_t seconds) {
    now_ = seconds;
    cacheAt_ = -1;
    if (seconds % kSecondsPerDay == 0 && (seconds / kSecondsPerDay) % kDaysPerSeason == 0 && events_ != nullptr) {
        Event e;
        e.type = EventType::SeasonChanged;
        const int season = GameTime{seconds}.season();
        e.message = seasonName(season);
        e.amount = static_cast<float>(season);
        events_->push_back(std::move(e));
    }
    if (state_.current.valid() && seconds >= state_.endsAt) {
        chooseNext(seconds);
    }
    if (seconds % kSecondsPerHour == 0) {
        updateSnow();
    }
}

double Weather::wind() const {
    if (kinds_.empty()) {
        return 1.0;  // no climate data: a steady wind of the default strength
    }
    const double x = static_cast<double>(now_) / (windGustDays_ * static_cast<double>(kSecondsPerDay));
    const double i = weatherFloor(x);
    const double a = weatherLattice(seed_ ^ 0x77696E64ULL, static_cast<int64_t>(i));
    const double b = weatherLattice(seed_ ^ 0x77696E64ULL, static_cast<int64_t>(i) + 1);
    const double gust = windGustMin_ + (windGustMax_ - windGustMin_) * (a + (b - a) * weatherSmooth(x - i));
    return std::clamp(static_cast<double>(cachedEffects().windSpeedFactor) * gust, 0.0, windMax_);
}

// ---- What a pawn feels -----------------------------------------------------------------------

bool Weather::outdoors(CellPos c) const {
    return map_.inBounds(c) && map_.roof(c) == Roof::None;
}

const WeatherEffects& Weather::cachedEffects() const {
    if (cacheAt_ != now_) {
        cache_ = effects();
        cacheAt_ = now_;
    }
    return cache_;
}

float Weather::moveFactorAt(CellPos c) const {
    if (!outdoors(c)) {
        return 1.0f;
    }
    return cachedEffects().moveSpeedFactor * snowSpeedFactor(snowDepth(c));
}

float Weather::accuracyAt(CellPos c) const {
    return outdoors(c) ? cachedEffects().accuracyFactor : 1.0f;
}

float Weather::sightAt(CellPos c) const {
    return outdoors(c) ? cachedEffects().sightFactor : 1.0f;
}

// ---- Snow ------------------------------------------------------------------------------------

float Weather::snowDepth(CellPos c) const {
    if (!map_.inBounds(c)) {
        return 0.0f;
    }
    return static_cast<float>(snow_[static_cast<size_t>(map_.index(c))]) * 0.01f;
}

float Weather::snowSpeedFactor(float depthPercent) const {
    float factor = 1.0f;
    for (size_t i = 0; i < speedDepths_.size(); ++i) {
        if (static_cast<double>(depthPercent) >= speedDepths_[i]) {
            factor = static_cast<float>(speedFactors_[i]);
        }
    }
    return factor;
}

bool Weather::holdsSnow(int index) const {
    const CellPos c = map_.cellAt(index);
    return map_.terrain(c) != Terrain::Water && map_.roof(c) == Roof::None;
}

void Weather::updateSnow() {
    const WeatherEffects e = effects();
    const double fall = static_cast<double>(e.snowRate) * snowPerHour_ * 100.0;
    if (fall <= 0.0 && snowCells_ == 0) {
        return;
    }
    // RimWorld's melt: nothing at or below freezing, the square of the temperature (times a small
    // factor) up to 10 degrees, then linear.
    const double t = temperatureAt(now_);
    const double melt = t <= 0.0 ? 0.0 : (t < 10.0 ? meltLow_ * t * t : meltHigh_ * t) * 100.0;
    const int64_t net = std::llround(fall) - std::llround(melt);
    bool changed = false;
    size_t cells = 0;
    uint64_t hash = 0;
    const int width = map_.width();
    const int height = map_.height();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            uint16_t& depth = snow_[static_cast<size_t>(index)];
            if (depth == 0 && net <= 0) {
                continue;
            }
            int64_t next = 0;
            if (holdsSnow(index)) {
                next = std::clamp<int64_t>(static_cast<int64_t>(depth) + net, 0, kWeatherMaxSnow);
            }
            if (next != depth) {
                depth = static_cast<uint16_t>(next);
                changed = true;
            }
            if (depth != 0) {
                ++cells;
                hash += splitMix64(static_cast<uint64_t>(index) * 65536u + depth);
            }
        }
    }
    snowCells_ = cells;
    snowHash_ = hash;
    if (changed) {
        ++snowChanges_;
    }
}

std::vector<WeatherSnowRecord> Weather::snowRecords() const {
    std::vector<WeatherSnowRecord> out;
    out.reserve(snowCells_);
    for (size_t i = 0; i < snow_.size(); ++i) {
        if (snow_[i] != 0) {
            out.push_back(WeatherSnowRecord{static_cast<int>(i), snow_[i]});
        }
    }
    return out;
}

bool Weather::restore(const WeatherState& state, const std::vector<WeatherSnowRecord>& snow, int64_t now) {
    int previousCell = -1;
    for (const WeatherSnowRecord& r : snow) {
        if (r.cell <= previousCell || static_cast<size_t>(r.cell) >= snow_.size() || r.depth == 0 || r.depth > kWeatherMaxSnow) {
            return false;
        }
        previousCell = r.cell;
    }
    if ((state.current.valid() && kindOf(state.current) == nullptr) || (state.previous.valid() && kindOf(state.previous) == nullptr)) {
        return false;
    }
    std::fill(snow_.begin(), snow_.end(), uint16_t{0});
    snowCells_ = 0;
    snowHash_ = 0;
    for (const WeatherSnowRecord& r : snow) {
        snow_[static_cast<size_t>(r.cell)] = r.depth;
        ++snowCells_;
        snowHash_ += splitMix64(static_cast<uint64_t>(r.cell) * 65536u + r.depth);
    }
    state_ = state;
    rng_.restore(state.rngState, state.rngIncrement);
    now_ = now;
    cacheAt_ = -1;
    return true;
}

uint64_t Weather::hash() const {
    StateHasher h;
    h.add(state_.current.value);
    h.add(state_.previous.value);
    h.add(state_.startedAt);
    h.add(state_.endsAt);
    h.add(rng_.state());
    h.add(rng_.increment());
    h.add(snowHash_);
    h.add(static_cast<uint64_t>(snowCells_));
    return h.value();
}

}  // namespace colony

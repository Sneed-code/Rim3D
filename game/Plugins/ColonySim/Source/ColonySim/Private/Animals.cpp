#include "ColonySim/Animals.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

namespace {

constexpr int64_t kAnimalSecondsPerYear = kDaysPerYear * kSecondsPerDay;
constexpr int kAnimalWalkCheck = 5;        // seconds between looks at an animal that is walking
constexpr int kAnimalSleepCheck = 120;     // and at one that is asleep
constexpr int kAnimalDownedCheck = 10;
constexpr int kAnimalTries = 6;            // spots tried for one walk
constexpr int64_t kAnimalEntryGrace = 2 * kSecondsPerHour;  // a newcomer stays this long before it may leave

int64_t animalYearsToSeconds(double years) {
    return static_cast<int64_t>(years * static_cast<double>(kAnimalSecondsPerYear));
}

float animalDist2(float ax, float ay, float bx, float by) {
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

// Each decision gets a stream of its own from the animal and the second, so the order animals are
// visited in can't change anything (docs/design/animal-life.md).
Rng animalRngFor(uint64_t seed, Entity e, int64_t now) {
    return Rng(splitMix64(seed ^ (static_cast<uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ULL) ^
                          (static_cast<uint64_t>(now) << 17)),
               7);
}

bool animalDead(const entt::registry& reg, Entity e) {
    const Health* h = reg.try_get<Health>(e);
    return h != nullptr && h->dead;
}

bool animalAble(const entt::registry& reg, Entity e) {
    const Health* h = reg.try_get<Health>(e);
    return h == nullptr || !h->incapacitated();
}

// The cell `t` steps round the square ring of Chebyshev radius `r` about `centre` (r >= 1; 8r cells).
CellPos animalRingCell(CellPos centre, int r, int t) {
    const int side = 2 * r;
    const int k = t / side;
    const int i = t % side;
    switch (k) {
        case 0: return CellPos{centre.x - r + i, centre.y - r, centre.level};
        case 1: return CellPos{centre.x + r, centre.y - r + i, centre.level};
        case 2: return CellPos{centre.x + r - i, centre.y + r, centre.level};
        default: return CellPos{centre.x - r, centre.y + r - i, centre.level};
    }
}

}  // namespace

Animal makeAnimalFor(const Def& def) {
    Animal a;
    const size_t stages = def.count("race.lifeStages");
    double adultYears = 0.0;
    if (stages > 0) {
        adultYears = def.number(("race.lifeStages." + std::to_string(stages - 1) + ".minAgeYears").c_str(), 0.0);
    }
    a.bornAt = -(animalYearsToSeconds(adultYears) + kSecondsPerDay);
    return a;
}

AnimalSystem::AnimalSystem(Things& things, const Map& map, Plants& plants, HealthSystem& health,
                           const DefDatabase& defs, const GameTime& time, uint64_t seed, const std::string& biome)
    : things_(things),
      map_(map),
      plants_(plants),
      health_(health),
      defs_(defs),
      time_(time),
      seed_(seed),
      rng_(splitMix64(seed ^ 0x616E696D616CULL)) {
    if (const Def* r = defs.find("Rules", "AnimalRules")) {
        rules_.idleMin = static_cast<int>(r->number("idleMin", rules_.idleMin));
        rules_.idleMax = static_cast<int>(r->number("idleMax", rules_.idleMax));
        rules_.wanderChance = static_cast<float>(r->number("wanderChance", static_cast<double>(rules_.wanderChance)));
        rules_.wanderMin = static_cast<float>(r->number("wanderMin", static_cast<double>(rules_.wanderMin)));
        rules_.wanderMax = static_cast<float>(r->number("wanderMax", static_cast<double>(rules_.wanderMax)));
        rules_.wanderSpeed = static_cast<float>(r->number("wanderSpeed", static_cast<double>(rules_.wanderSpeed)));
        rules_.herdRadius = static_cast<float>(r->number("herdRadius", static_cast<double>(rules_.herdRadius)));
        rules_.herdPull = static_cast<float>(r->number("herdPull", static_cast<double>(rules_.herdPull)));
        rules_.grazeRadius = static_cast<float>(r->number("grazeRadius", static_cast<double>(rules_.grazeRadius)));
        rules_.hungryBelow = static_cast<float>(r->number("hungryBelow", static_cast<double>(rules_.hungryBelow)));
        rules_.fullAt = static_cast<float>(r->number("fullAt", static_cast<double>(rules_.fullAt)));
        rules_.minPlantGrowth = static_cast<float>(r->number("minPlantGrowth", static_cast<double>(rules_.minPlantGrowth)));
        rules_.eatSeconds = static_cast<int>(r->number("eatSeconds", rules_.eatSeconds));
        rules_.sleepStartHour = static_cast<int>(r->number("sleepStartHour", rules_.sleepStartHour));
        rules_.sleepEndHour = static_cast<int>(r->number("sleepEndHour", rules_.sleepEndHour));
        rules_.nocturnalStartHour = static_cast<int>(r->number("nocturnalStartHour", rules_.nocturnalStartHour));
        rules_.nocturnalEndHour = static_cast<int>(r->number("nocturnalEndHour", rules_.nocturnalEndHour));
        for (size_t i = 0; i < r->count("nocturnal"); ++i) {
            rules_.nocturnal.push_back(r->textAt("nocturnal", i));
        }
        rules_.edgeReach = static_cast<float>(r->number("edgeReach", static_cast<double>(rules_.edgeReach)));
        rules_.leaveChance = static_cast<float>(r->number("leaveChance", static_cast<double>(rules_.leaveChance)));
        rules_.entryChance = static_cast<float>(r->number("entryChance", static_cast<double>(rules_.entryChance)));
        rules_.underTarget = static_cast<float>(r->number("underTarget", static_cast<double>(rules_.underTarget)));
        rules_.overTarget = static_cast<float>(r->number("overTarget", static_cast<double>(rules_.overTarget)));
        rules_.areaPerDensity = static_cast<float>(r->number("areaPerDensity", static_cast<double>(rules_.areaPerDensity)));
        rules_.groupSpread = static_cast<float>(r->number("groupSpread", static_cast<double>(rules_.groupSpread)));
        rules_.startClearRadius = static_cast<float>(r->number("startClearRadius", static_cast<double>(rules_.startClearRadius)));
        rules_.corpseDays = static_cast<int>(r->number("corpseDays", rules_.corpseDays));
        rules_.startAgeMax = static_cast<float>(r->number("startAgeMax", static_cast<double>(rules_.startAgeMax)));
    }
    rules_.idleMin = std::max(1, rules_.idleMin);
    rules_.idleMax = std::max(rules_.idleMin, rules_.idleMax);

    // What animals eat: any plant that is not a tree, has nutrition, and isn't picked by hand (berry
    // bushes and crops are the colony's).
    nutrition_.assign(defs.size(), 0.0f);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.text("category") == "Plant" && !def.flag("plant.isTree") && !def.has("plant.harvestedThingDef")) {
            nutrition_[id.value] = static_cast<float>(def.number("stats.nutrition", 0.0));
        }
    }

    const Def* biomeDef = defs.find("Biomes", biome);
    hasBiome_ = biomeDef != nullptr;
    density_ = hasBiome_ ? static_cast<float>(biomeDef->number("animalDensity", 0.0)) : 0.0f;
    kindOfDef_.assign(defs.size(), -1);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.text("category") != "Pawn" || def.text("race.intelligence") != "Animal") {
            continue;
        }
        Kind k;
        k.def = id;
        k.topSpeed = static_cast<float>(def.number("stats.moveSpeed", 0.1));
        k.bodySize = static_cast<float>(def.number("race.baseBodySize", 1.0));
        k.foodPerDay = static_cast<float>(def.number("race.foodPerDay", 1.6 * static_cast<double>(k.bodySize)));
        const std::string diet = def.text("race.diet");
        k.grazer = diet == "Herbivore" || diet == "Omnivore";
        k.nocturnal = std::find(rules_.nocturnal.begin(), rules_.nocturnal.end(), def.defName()) != rules_.nocturnal.end();
        k.domestic = def.flag("race.domestic");
        if (def.has("race.herd")) {
            k.herdMin = std::max(1, static_cast<int>(def.number("race.herd.minSize", 1.0)));
            k.herdMax = std::max(k.herdMin, static_cast<int>(def.number("race.herd.maxSize", static_cast<double>(k.herdMin))));
        }
        k.lifeExpectancyDays = def.number("race.lifeExpectancy", 10.0) * static_cast<double>(kDaysPerYear);
        const size_t stages = def.count("race.lifeStages");
        for (size_t i = 0; i < stages; ++i) {
            const std::string at = "race.lifeStages." + std::to_string(i) + ".";
            Stage s;
            s.minAgeSeconds = animalYearsToSeconds(def.number((at + "minAgeYears").c_str(), 0.0));
            s.bodySizeFactor = static_cast<float>(def.number((at + "bodySizeFactor").c_str(), 1.0));
            s.foodFactor = static_cast<float>(def.number((at + "foodFactor").c_str(), 1.0));
            k.stages.push_back(s);
        }
        if (k.stages.empty()) {
            k.stages.push_back(Stage{});
        }
        k.adultAgeSeconds = k.stages.back().minAgeSeconds;
        if (hasBiome_ && !k.domestic) {
            // The biome's own list (RimWorld's numbers) decides; an animal's `race.wildBiomes` (ours) is
            // used only for a biome that has no list.
            if (biomeDef->count("animals") > 0) {
                k.commonality = biomeDef->number(("animals." + def.defName()).c_str(), 0.0);
            } else {
                k.commonality = def.number(("race.wildBiomes." + biome).c_str(), 0.0);
            }
        }
        totalCommonality_ += k.commonality;
        kindOfDef_[id.value] = static_cast<int>(kinds_.size());
        kinds_.push_back(std::move(k));
    }
}

AnimalSystem::~AnimalSystem() = default;

// ---- Questions ------------------------------------------------------------------------------

const AnimalSystem::Kind* AnimalSystem::kindOf(Entity animal) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Animal, Thing>(animal)) {
        return nullptr;
    }
    const uint32_t def = reg.get<Thing>(animal).def.value;
    if (def >= kindOfDef_.size() || kindOfDef_[def] < 0) {
        return nullptr;
    }
    return &kinds_[static_cast<size_t>(kindOfDef_[def])];
}

int AnimalSystem::stageAt(const Kind& kind, int64_t ageSeconds) const {
    for (size_t i = kind.stages.size(); i-- > 0;) {
        if (ageSeconds >= kind.stages[i].minAgeSeconds) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

float AnimalSystem::capacity(const Kind& kind, int64_t ageSeconds) const {
    return kind.bodySize * kind.stages[static_cast<size_t>(stageAt(kind, ageSeconds))].bodySizeFactor;
}

float AnimalSystem::foodFallPerSecond(const Kind& kind, int64_t ageSeconds) const {
    return kind.foodPerDay * kind.stages[static_cast<size_t>(stageAt(kind, ageSeconds))].foodFactor /
           static_cast<float>(kSecondsPerDay);
}

int AnimalSystem::lifeStage(Entity animal) const {
    const Kind* kind = kindOf(animal);
    return kind != nullptr ? stageAt(*kind, time_.seconds - things_.registry().get<Animal>(animal).bornAt) : -1;
}

bool AnimalSystem::isAdult(Entity animal) const {
    const Kind* kind = kindOf(animal);
    return kind != nullptr &&
           stageAt(*kind, time_.seconds - things_.registry().get<Animal>(animal).bornAt) == static_cast<int>(kind->stages.size()) - 1;
}

float AnimalSystem::bodySize(Entity animal) const {
    const Kind* kind = kindOf(animal);
    return kind != nullptr ? capacity(*kind, time_.seconds - things_.registry().get<Animal>(animal).bornAt) : 0.0f;
}

float AnimalSystem::foodShare(Entity animal) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr) {
        return -1.0f;
    }
    const Animal& a = things_.registry().get<Animal>(animal);
    const int64_t age = time_.seconds - a.bornAt;
    const float cap = capacity(*kind, age);
    if (!a.started || cap <= 0.0f || !(kind->grazer || !a.wild)) {
        return 1.0f;
    }
    const float now = a.food - foodFallPerSecond(*kind, age) * static_cast<float>(time_.seconds - a.foodAt);
    return std::clamp(now / cap, 0.0f, 1.0f);
}

bool AnimalSystem::asleep(Entity animal) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(animal) && reg.all_of<Animal>(animal) && reg.get<Animal>(animal).mode == AnimalMode::Sleep;
}

Entity AnimalSystem::leaderOf(Entity animal) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Animal>(animal)) {
        return kNoEntity;
    }
    const uint32_t herd = reg.get<Animal>(animal).herd;
    if (herd == 0) {
        return animal;
    }
    Entity best = kNoEntity;
    for (const auto [e, a] : reg.view<const Animal>().each()) {
        if (a.herd == herd && animalAble(reg, e) && (best == kNoEntity || e < best)) {
            best = e;
        }
    }
    return best != kNoEntity ? best : animal;
}

size_t AnimalSystem::livingCount() const {
    size_t n = 0;
    const entt::registry& reg = things_.registry();
    for (const Entity e : reg.view<const Animal>()) {
        n += animalDead(reg, e) ? 0 : 1;
    }
    return n;
}

size_t AnimalSystem::wildCount() const {
    size_t n = 0;
    const entt::registry& reg = things_.registry();
    for (const auto [e, a] : reg.view<const Animal>().each()) {
        n += a.wild && !animalDead(reg, e) ? 1 : 0;
    }
    return n;
}

float AnimalSystem::wildWeight() const {
    float w = 0.0f;
    const entt::registry& reg = things_.registry();
    for (const auto [e, a] : reg.view<const Animal>().each()) {
        if (!a.wild || animalDead(reg, e)) {
            continue;
        }
        const Kind* kind = kindOf(e);
        if (kind != nullptr) {
            w += capacity(*kind, time_.seconds - a.bornAt);
        }
    }
    return w;
}

float AnimalSystem::targetWeight() const {
    const float area = static_cast<float>(map_.width()) * static_cast<float>(map_.height());
    return density_ * (area / rules_.areaPerDensity) * static_cast<float>(saved_.percent) / 100.0f;
}

// ---- Saving ---------------------------------------------------------------------------------

AnimalSaved AnimalSystem::saved() const {
    AnimalSaved s = saved_;
    s.rngState = rng_.state();
    s.rngIncrement = rng_.increment();
    return s;
}

void AnimalSystem::restore(const AnimalSaved& state) {
    saved_ = state;
    rng_.restore(state.rngState, state.rngIncrement);
}

void AnimalSystem::hashState(StateHasher& h) const {
    if (!saved_.populated) {
        return;
    }
    h.add(saved_.percent);
    h.add(rng_.state());
    h.add(rng_.increment());
    h.add(saved_.nextHerd);
    h.add(saved_.over);
    h.add(saved_.stats.arrived);
    h.add(saved_.stats.left);
    h.add(saved_.stats.grazed);
    h.add(saved_.stats.cleared);
}

void AnimalSystem::hashThing(Entity e, StateHasher& h) const {
    const Animal* a = things_.registry().try_get<Animal>(e);
    if (a == nullptr) {
        return;
    }
    h.add(a->started);
    h.add(a->wild);
    h.add(a->feeding);
    h.add(static_cast<int>(a->sleepShift));
    h.add(a->herd);
    h.add(a->bornAt);
    h.add(a->diedAt);
    h.add(a->food);
    h.add(a->foodAt);
    h.add(static_cast<int>(a->mode));
    h.add(a->until);
    h.add(a->nextThink);
    h.add(a->staysUntil);
    h.add(a->goal.x);
    h.add(a->goal.y);
    h.add(a->goal.level);
}

// ---- Filling the map ------------------------------------------------------------------------

int AnimalSystem::pickKind(Rng& rng) const {
    double r = static_cast<double>(rng.unit()) * totalCommonality_;
    int last = -1;
    for (size_t i = 0; i < kinds_.size(); ++i) {
        if (kinds_[i].commonality <= 0.0) {
            continue;
        }
        last = static_cast<int>(i);
        if (r < kinds_[i].commonality) {
            return last;
        }
        r -= kinds_[i].commonality;
    }
    return last;
}

bool AnimalSystem::pickGroundCell(Rng& rng, bool clearOfColonists, CellPos& out) const {
    const int margin = 4;
    const int w = map_.width() - 2 * margin;
    const int h = map_.height() - 2 * margin;
    if (w <= 0 || h <= 0) {
        return false;
    }
    const entt::registry& reg = things_.registry();
    const float clear2 = rules_.startClearRadius * rules_.startClearRadius;
    for (int tries = 0; tries < 40; ++tries) {
        const CellPos c{margin + static_cast<int>(rng.below(static_cast<uint32_t>(w))),
                        margin + static_cast<int>(rng.below(static_cast<uint32_t>(h))), 0};
        if (!map_.walkable(c)) {
            continue;
        }
        bool near = false;
        if (clearOfColonists) {
            for (const Entity pawn : reg.view<const Colonist>()) {
                const Position* pos = reg.try_get<Position>(pawn);
                near = near || (pos != nullptr && animalDist2(pos->x, pos->y, static_cast<float>(c.x) + 0.5f,
                                                              static_cast<float>(c.y) + 0.5f) < clear2);
            }
        }
        if (!near) {
            out = c;
            return true;
        }
    }
    return false;
}

bool AnimalSystem::pickEdgeCell(Rng& rng, CellPos& out) const {
    const int w = map_.width();
    const int h = map_.height();
    for (int tries = 0; tries < 20; ++tries) {
        const int side = static_cast<int>(rng.below(4));
        const int along = static_cast<int>(rng.below(static_cast<uint32_t>(side < 2 ? w : h)));
        for (int in = 0; in < 6; ++in) {
            CellPos c{};
            switch (side) {
                case 0: c = CellPos{along, in, 0}; break;
                case 1: c = CellPos{along, h - 1 - in, 0}; break;
                case 2: c = CellPos{in, along, 0}; break;
                default: c = CellPos{w - 1 - in, along, 0}; break;
            }
            if (map_.walkable(c)) {
                out = c;
                return true;
            }
        }
    }
    return false;
}

float AnimalSystem::placeGroup(int kindIndex, CellPos centre, Rng& rng, bool adultsOnly, bool entering) {
    const Kind& kind = kinds_[static_cast<size_t>(kindIndex)];
    entt::registry& reg = things_.registry();
    const int n = kind.herdMin + static_cast<int>(rng.below(static_cast<uint32_t>(kind.herdMax - kind.herdMin + 1)));
    const uint32_t herd = n >= 2 ? saved_.nextHerd++ : 0;
    const int64_t now = time_.seconds;
    const double adultYears = static_cast<double>(kind.adultAgeSeconds) / static_cast<double>(kAnimalSecondsPerYear);
    const double lifeYears = kind.lifeExpectancyDays / static_cast<double>(kDaysPerYear);
    const double oldest = std::max(adultYears + 0.01, lifeYears * static_cast<double>(rules_.startAgeMax));
    std::vector<CellPos> used;
    float weight = 0.0f;
    for (int i = 0; i < n; ++i) {
        for (int tries = 0; tries < 12; ++tries) {
            CellPos c = centre;
            if (i > 0) {
                const int spread = static_cast<int>(rules_.groupSpread);
                c.x += static_cast<int>(rng.below(static_cast<uint32_t>(2 * spread + 1))) - spread;
                c.y += static_cast<int>(rng.below(static_cast<uint32_t>(2 * spread + 1))) - spread;
            }
            if (!map_.walkable(c) || std::find(used.begin(), used.end(), c) != used.end()) {
                continue;
            }
            const Entity e = things_.spawn(kind.def, c);
            if (e == kNoEntity) {
                continue;
            }
            used.push_back(c);
            Animal& a = reg.get<Animal>(e);
            a.herd = herd;
            const double ageYears = adultsOnly ? adultYears + static_cast<double>(rng.unit()) * (oldest - adultYears) : 0.0;
            a.bornAt = now - animalYearsToSeconds(ageYears);
            a.staysUntil = entering ? now + kAnimalEntryGrace : 0;
            weight += capacity(kind, now - a.bornAt);
            break;
        }
    }
    return weight;
}

void AnimalSystem::populate(int percent) {
    if (saved_.populated || percent <= 0 || !hasBiome_ || totalCommonality_ <= 0.0) {
        return;
    }
    saved_.populated = true;
    saved_.percent = percent;
    const float target = targetWeight();
    float total = 0.0f;
    for (int attempts = 0; total < target && attempts < 5000; ++attempts) {
        const int kind = pickKind(rng_);
        CellPos c;
        if (kind < 0 || !pickGroundCell(rng_, true, c)) {
            continue;
        }
        total += placeGroup(kind, c, rng_, true, false);
    }
}

// ---- Each step ------------------------------------------------------------------------------

void AnimalSystem::rebuildLeaders() {
    leaders_.clear();
    const entt::registry& reg = things_.registry();
    for (const auto [e, a] : reg.view<const Animal>().each()) {
        if (a.herd == 0 || !animalAble(reg, e)) {
            continue;
        }
        const auto it = leaders_.find(a.herd);
        if (it == leaders_.end()) {
            leaders_.emplace(a.herd, e);
        } else if (e < it->second) {
            it->second = e;
        }
    }
}

void AnimalSystem::step() {
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    due_.clear();
    bool herds = false;
    for (const auto [e, a, mover] : reg.view<Animal, Mover>().each()) {
        mover.hold = a.mode == AnimalMode::Sleep || a.mode == AnimalMode::Eat || a.mode == AnimalMode::EatItem;
        if (husbandry_ != nullptr) {
            const AnimalCare* care = reg.try_get<AnimalCare>(e);
            mover.hold = mover.hold || (care != nullptr && care->heldUntil > now);
        }
        if (a.nextThink <= now) {
            due_.push_back(e);
            herds = herds || a.herd != 0;
        }
    }
    if (!due_.empty()) {
        std::sort(due_.begin(), due_.end());
        if (herds) {
            rebuildLeaders();
        }
        for (const Entity e : due_) {
            think(e);
        }
    }
    if (saved_.populated && now > 0 && now % kSecondsPerHour == 0) {
        hourly(now);
    }
}

void AnimalSystem::hold(Entity animal, int64_t until) {
    entt::registry& reg = things_.registry();
    if (husbandry_ == nullptr || !reg.valid(animal) || !reg.all_of<Animal, Mover>(animal)) {
        return;
    }
    AnimalCare* care = husbandry_->careFor(animal);
    if (care == nullptr) {
        return;
    }
    care->heldUntil = std::max(care->heldUntil, until);
    // It stops where it is: a walk, a wait for a path or a sleep ends, and it looks round again once let go.
    Animal& a = reg.get<Animal>(animal);
    if (a.mode == AnimalMode::WaitPath) {
        cancelPath(*care);
    }
    if (a.mode != AnimalMode::Idle) {
        Mover& mover = reg.get<Mover>(animal);
        mover.path.clear();
        mover.next = 0;
        mover.vx = 0.0f;
        mover.vy = 0.0f;
        a.mode = AnimalMode::Idle;
        a.until = time_.seconds;
        a.nextThink = std::min(a.nextThink, care->heldUntil);
    }
}

void AnimalSystem::wake(Entity animal) {
    entt::registry& reg = things_.registry();
    if (reg.valid(animal) && reg.all_of<Animal>(animal)) {
        Animal& a = reg.get<Animal>(animal);
        if (a.mode == AnimalMode::Sleep) {
            a.mode = AnimalMode::Idle;
            a.nextThink = time_.seconds;
        }
    }
}

void AnimalSystem::hourly(int64_t now) {
    const float target = targetWeight();
    const float w = wildWeight();
    saved_.over = w > target * rules_.overTarget;
    if (w < target * rules_.underTarget && rng_.unit() < rules_.entryChance) {
        const int kind = pickKind(rng_);
        CellPos c;
        if (kind >= 0 && pickEdgeCell(rng_, c)) {
            const size_t before = livingCount();
            placeGroup(kind, c, rng_, true, true);
            saved_.stats.arrived += livingCount() - before;
        }
    }
    (void)now;
}

bool AnimalSystem::sleepNow(const Kind& kind, const Animal& a, int64_t now) const {
    const int hour = static_cast<int>((now % kSecondsPerDay) / kSecondsPerHour);
    const int start = ((kind.nocturnal ? rules_.nocturnalStartHour : rules_.sleepStartHour) + a.sleepShift) % 24;
    const int end = ((kind.nocturnal ? rules_.nocturnalEndHour : rules_.sleepEndHour) + a.sleepShift) % 24;
    return start < end ? (hour >= start && hour < end) : (hour >= start || hour < end);
}

int64_t AnimalSystem::sleepEnd(const Kind& kind, const Animal& a, int64_t now) const {
    const int end = ((kind.nocturnal ? rules_.nocturnalEndHour : rules_.sleepEndHour) + a.sleepShift) % 24;
    int64_t t = now - now % kSecondsPerHour + kSecondsPerHour;
    while ((t / kSecondsPerHour) % 24 != end) {
        t += kSecondsPerHour;
    }
    return t;
}

void AnimalSystem::begin(Entity e, Animal& a, const Kind& kind) {
    const int64_t now = time_.seconds;
    Rng rng = animalRngFor(seed_, e, now);
    a.started = true;
    a.sleepShift = static_cast<uint8_t>(rng.below(3));
    a.food = capacity(kind, now - a.bornAt) * (0.6f + 0.4f * rng.unit());
    a.foodAt = now;
    a.nextThink = now + 1 + static_cast<int64_t>(rng.below(static_cast<uint32_t>(rules_.idleMax)));
    things_.registry().emplace_or_replace<Faction>(e, Faction{a.wild ? kWildFaction : kPlayerFaction});
}

bool AnimalSystem::walkTo(Entity e, Animal& a, const Kind& kind, float x, float y, AnimalMode mode) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(e);
    const CellPos cell{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), pos.level};
    if (!map_.walkable(cell)) {
        return false;
    }
    const float cx = static_cast<float>(cell.x) + 0.5f;
    const float cy = static_cast<float>(cell.y) + 0.5f;
    if (!straightWalkIsClear(map_, pos.x, pos.y, cx, cy, kPawnRadius, pos.level)) {
        return false;
    }
    Mover& mover = reg.get<Mover>(e);
    mover.metresPerStep = kind.topSpeed * rules_.wanderSpeed;
    mover.walkTo(pos.x, pos.y, std::vector<CellPos>{cell});
    a.goal = cell;
    a.mode = mode;
    return true;
}

bool AnimalSystem::findFood(Entity e, const Kind& kind, Rng& rng, CellPos& out) const {
    (void)kind;
    const Position& pos = things_.registry().get<Position>(e);
    const CellPos here = cellOf(pos);
    const int reach = static_cast<int>(rules_.grazeRadius);
    for (int r = 0; r <= reach; ++r) {
        const int cells = r == 0 ? 1 : 8 * r;
        const int first = r == 0 ? 0 : static_cast<int>(rng.below(static_cast<uint32_t>(cells)));
        for (int i = 0; i < cells; ++i) {
            const CellPos c = r == 0 ? here : animalRingCell(here, r, (first + i) % cells);
            if (!map_.inBounds(c) || !plants_.has(c)) {
                continue;
            }
            const DefId plant = plants_.defAt(c);
            if (nutrition_[plant.value] <= 0.0f || plants_.growth(c) < rules_.minPlantGrowth || !map_.walkable(c)) {
                continue;
            }
            if (r > 0 && !straightWalkIsClear(map_, pos.x, pos.y, static_cast<float>(c.x) + 0.5f,
                                              static_cast<float>(c.y) + 0.5f, kPawnRadius, pos.level)) {
                continue;
            }
            out = c;
            return true;
        }
    }
    return false;
}

void AnimalSystem::finishEating(Entity e, Animal& a, const Kind& kind) {
    const int64_t now = time_.seconds;
    const CellPos c = a.goal;
    if (map_.inBounds(c) && plants_.has(c)) {
        const DefId plant = plants_.defAt(c);
        const float nutrition = nutrition_[plant.value] * plants_.growth(c);
        if (nutrition > 0.0f) {
            a.food = std::min(capacity(kind, now - a.bornAt), a.food + nutrition);
            plants_.remove(c);
            ++saved_.stats.grazed;
            if (events_ != nullptr) {
                Event event;
                event.type = EventType::PlantRemoved;
                event.thing = e;
                event.cell = c;
                event.def = plant;
                event.message = "grazed";
                events_->push_back(std::move(event));
            }
        }
    }
    a.mode = AnimalMode::Idle;
}

bool AnimalSystem::tryLeave(Entity e, Animal& a, const Kind& kind, Rng& rng) {
    (void)rng;
    const Position& pos = things_.registry().get<Position>(e);
    const float w = static_cast<float>(map_.width());
    const float h = static_cast<float>(map_.height());
    const float toLeft = pos.x;
    const float toRight = w - pos.x;
    const float toTop = pos.y;
    const float toBottom = h - pos.y;
    const float nearest = std::min(std::min(toLeft, toRight), std::min(toTop, toBottom));
    if (nearest > rules_.edgeReach) {
        return false;
    }
    float tx = pos.x;
    float ty = pos.y;
    if (nearest == toLeft) {
        tx = 0.5f;
    } else if (nearest == toRight) {
        tx = w - 0.5f;
    } else if (nearest == toTop) {
        ty = 0.5f;
    } else {
        ty = h - 0.5f;
    }
    return walkTo(e, a, kind, tx, ty, AnimalMode::Leave);
}

void AnimalSystem::decide(Entity e, Animal& a, const Kind& kind, Rng& rng) {
    if (!a.wild && husbandry_ != nullptr) {
        decideTamed(e, a, kind, rng);
        return;
    }
    entt::registry& reg = things_.registry();
    const int64_t now = time_.seconds;
    const Position pos = reg.get<Position>(e);
    const float cap = capacity(kind, now - a.bornAt);

    // Hungry: eat what is close.
    if (kind.grazer) {
        if (a.food < rules_.hungryBelow * cap) {
            a.feeding = true;
        } else if (a.food >= rules_.fullAt * cap) {
            a.feeding = false;
        }
        CellPos plant;
        if (a.feeding && findFood(e, kind, rng, plant)) {
            if (cellOf(pos) == plant) {
                a.goal = plant;
                a.mode = AnimalMode::Eat;
                a.until = now + rules_.eatSeconds;
                a.nextThink = a.until;
                return;
            }
            if (walkTo(e, a, kind, static_cast<float>(plant.x) + 0.5f, static_cast<float>(plant.y) + 0.5f, AnimalMode::GoToFood)) {
                a.nextThink = now + kAnimalWalkCheck;
                return;
            }
        }
    }

    // A herd follower keeps near its leader.
    bool followed = false;
    if (a.herd != 0) {
        const auto it = leaders_.find(a.herd);
        if (it != leaders_.end() && it->second != e && reg.valid(it->second) && reg.all_of<Position, Animal>(it->second)) {
            const Position& lp = reg.get<Position>(it->second);
            const Animal& la = reg.get<Animal>(it->second);
            const bool leaderWalking = !reg.get<Mover>(it->second).arrived() &&
                                       (la.mode == AnimalMode::Wander || la.mode == AnimalMode::GoToFood || la.mode == AnimalMode::Leave);
            const float ax = leaderWalking ? static_cast<float>(la.goal.x) + 0.5f : lp.x;
            const float ay = leaderWalking ? static_cast<float>(la.goal.y) + 0.5f : lp.y;
            if (animalDist2(pos.x, pos.y, ax, ay) > rules_.herdRadius * rules_.herdRadius ||
                animalDist2(pos.x, pos.y, lp.x, lp.y) > rules_.herdPull * rules_.herdPull) {
                for (int tries = 0; tries < kAnimalTries && !followed; ++tries) {
                    const float tx = ax + rng.range(-rules_.herdRadius, rules_.herdRadius) * 0.8f;
                    const float ty = ay + rng.range(-rules_.herdRadius, rules_.herdRadius) * 0.8f;
                    followed = walkTo(e, a, kind, tx, ty, AnimalMode::Wander);
                }
                if (followed) {
                    a.nextThink = now + kAnimalWalkCheck;
                    return;
                }
            }
            followed = true;  // close enough: stands about, doesn't lead
        }
    }

    // Wild animals near the map edge may walk off it.
    if (a.wild && now >= a.staysUntil) {
        const float chance = rules_.leaveChance * (saved_.over ? 4.0f : 1.0f);
        if (rng.unit() < chance && tryLeave(e, a, kind, rng)) {
            a.nextThink = now + kAnimalWalkCheck;
            return;
        }
    }

    // A leader or a loner wanders now and then; a follower only shuffles a little.
    const float reachMax = followed ? 2.0f * rules_.wanderMin : (a.feeding ? 2.0f * rules_.wanderMax : rules_.wanderMax);
    if (rng.unit() < (followed ? rules_.wanderChance * 0.5f : rules_.wanderChance)) {
        for (int tries = 0; tries < kAnimalTries; ++tries) {
            const float dx = rng.range(-reachMax, reachMax);
            const float dy = rng.range(-reachMax, reachMax);
            const float d2 = dx * dx + dy * dy;
            if (d2 < rules_.wanderMin * rules_.wanderMin * (followed ? 0.1f : 1.0f) || d2 > reachMax * reachMax) {
                continue;
            }
            if (walkTo(e, a, kind, pos.x + dx, pos.y + dy, AnimalMode::Wander)) {
                a.nextThink = now + kAnimalWalkCheck;
                return;
            }
        }
    }
    a.mode = AnimalMode::Idle;
    a.until = now + rules_.idleMin + static_cast<int64_t>(rng.below(static_cast<uint32_t>(rules_.idleMax - rules_.idleMin + 1)));
    a.nextThink = a.until;
}

void AnimalSystem::think(Entity e) {
    entt::registry& reg = things_.registry();
    const Kind* kindPtr = kindOf(e);
    Animal& a = reg.get<Animal>(e);
    if (kindPtr == nullptr) {
        a.nextThink = time_.seconds + kSecondsPerHour;  // an animal whose def the data no longer has
        return;
    }
    const Kind& kind = *kindPtr;
    const int64_t now = time_.seconds;
    if (!a.started) {
        begin(e, a, kind);
        return;
    }
    Mover& mover = reg.get<Mover>(e);
    const Health* health = reg.try_get<Health>(e);

    // The dead lie where they fell until they rot away.
    if (health != nullptr && health->dead) {
        const int64_t gone = kSecondsPerDay * rules_.corpseDays;
        if (a.diedAt < 0) {
            if (AnimalCare* dying = husbandry_ != nullptr ? reg.try_get<AnimalCare>(e) : nullptr) {
                cancelPath(*dying);
            }
            a.diedAt = now;
            a.mode = AnimalMode::Idle;
            a.nextThink = now + gone;
            return;
        }
        if (now - a.diedAt >= gone) {
            ++saved_.stats.cleared;
            things_.destroy(e);
        } else {
            a.nextThink = a.diedAt + gone;
        }
        return;
    }
    if (health != nullptr && health->downed) {
        a.nextThink = now + kAnimalDownedCheck;
        return;
    }

    // Food falls all the time; it is worked out when the animal looks up.
    const int64_t age = now - a.bornAt;
    // Wild animals that eat meat can't graze and have nothing to hunt yet (N5), so they stay fed. A tame one eats what it
    // is given (N3).
    a.food = kind.grazer || !a.wild ? std::max(0.0f, a.food - foodFallPerSecond(kind, age) * static_cast<float>(now - a.foodAt))
                                    : capacity(kind, age);
    a.foodAt = now;

    // A handler is with it: it stands still. One that works (a trained hauler) is busy with its job.
    AnimalCare* care = husbandry_ != nullptr ? reg.try_get<AnimalCare>(e) : nullptr;
    if (care != nullptr && care->heldUntil > now) {
        if (a.mode == AnimalMode::Sleep) {
            a.mode = AnimalMode::Idle;
        }
        a.nextThink = std::min(care->heldUntil, now + kAnimalWalkCheck);
        return;
    }
    if (const Worker* worker = reg.try_get<Worker>(e); worker != nullptr && worker->job.type != JobType::None && worker->job.type != JobType::Wait) {
        mover.metresPerStep = kind.topSpeed;
        a.nextThink = now + 2;
        return;
    }
    if (!a.wild && husbandry_ != nullptr) {
        if (AnimalCare* tameCare = husbandry_->careFor(e)) {
            starveCheck(e, a, kind, *tameCare);
            if (health != nullptr && health->dead) {
                return;
            }
        }
    }

    const bool hurt = health != nullptr && !health->injuries.empty();
    if (a.mode == AnimalMode::Sleep) {
        if (now < a.until && !hurt) {
            a.nextThink = std::min(a.until, now + kAnimalSleepCheck);
            return;
        }
        a.mode = AnimalMode::Idle;
    }
    if (!hurt && sleepNow(kind, a, now) && a.mode != AnimalMode::Eat) {
        mover.path.clear();
        mover.next = 0;
        mover.vx = 0.0f;
        mover.vy = 0.0f;
        a.mode = AnimalMode::Sleep;
        a.until = sleepEnd(kind, a, now);
        a.nextThink = std::min(a.until, now + kAnimalSleepCheck);
        return;
    }
    if (a.mode == AnimalMode::Eat) {
        if (now < a.until) {
            a.nextThink = a.until;
            return;
        }
        finishEating(e, a, kind);
    }
    if (a.mode == AnimalMode::EatItem) {
        if (now < a.until) {
            a.nextThink = a.until;
            return;
        }
        finishEatingItem(e, a, kind);
    }
    if (a.mode == AnimalMode::WaitPath) {
        AnimalCare* waiting = husbandry_ != nullptr ? husbandry_->careFor(e) : nullptr;
        if (waiting == nullptr) {
            a.mode = AnimalMode::Idle;
        } else if (!pollPath(e, a, kind, *waiting)) {
            return;
        }
    }
    if (a.mode == AnimalMode::Wander || a.mode == AnimalMode::GoToFood || a.mode == AnimalMode::Leave ||
        a.mode == AnimalMode::GoToItem) {
        if (!mover.arrived() && !mover.gaveUp) {
            a.nextThink = now + kAnimalWalkCheck;
            return;
        }
        const bool gaveUp = mover.gaveUp;
        mover.gaveUp = false;
        mover.path.clear();
        mover.next = 0;
        if (!gaveUp && a.mode == AnimalMode::GoToFood && cellOf(reg.get<Position>(e)) == a.goal && plants_.has(a.goal)) {
            a.mode = AnimalMode::Eat;
            a.until = now + rules_.eatSeconds;
            a.nextThink = a.until;
            return;
        }
        if (!gaveUp && a.mode == AnimalMode::GoToItem && cellOf(reg.get<Position>(e)) == a.goal &&
            edibleItemAt(e, a.goal) != kNoEntity) {
            a.mode = AnimalMode::EatItem;
            a.until = now + rules_.eatSeconds;
            a.nextThink = a.until;
            return;
        }
        if (!gaveUp && a.mode == AnimalMode::Leave) {
            const Position& pos = reg.get<Position>(e);
            const float edge = std::min(std::min(pos.x, static_cast<float>(map_.width()) - pos.x),
                                        std::min(pos.y, static_cast<float>(map_.height()) - pos.y));
            if (edge <= 1.5f) {
                ++saved_.stats.left;
                things_.destroy(e);
                return;
            }
        }
        a.mode = AnimalMode::Idle;
    }
    Rng rng = animalRngFor(seed_, e, now);
    decide(e, a, kind, rng);
}

}  // namespace colony

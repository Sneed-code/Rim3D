#include "ColonySim/Husbandry.h"

#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr const char* kHusbandryTrainableNames[kTrainableCount] = {"Obedience", "Release", "Rescue", "Haul"};
constexpr const char* kHusbandryDietNames[3] = {"Herbivore", "Carnivore", "Omnivore"};

int husbandryTrainabilityOf(const std::string& text) {
    if (text == "Simple") return 1;
    if (text == "Intermediate") return 2;
    if (text == "Advanced") return 3;
    return 0;
}

int husbandryDietOf(const std::string& text) {
    for (int i = 0; i < 3; ++i) {
        if (text == kHusbandryDietNames[i]) {
            return i;
        }
    }
    return 2;
}

// Each roll gets a stream of its own from the animal and the second, so no order of handlers changes it.
Rng husbandryRngFor(uint64_t seed, Entity e, int64_t now, uint64_t salt) {
    return Rng(splitMix64(seed ^ (static_cast<uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ULL) ^
                          (static_cast<uint64_t>(now) << 13) ^ salt),
               11);
}

bool husbandryDead(const entt::registry& reg, Entity e) {
    const Health* h = reg.try_get<Health>(e);
    return h != nullptr && h->dead;
}

}  // namespace

const char* trainableName(Trainable t) {
    const int i = static_cast<int>(t);
    return i >= 0 && i < kTrainableCount ? kHusbandryTrainableNames[i] : "?";
}

bool trainableFromName(const std::string& name, Trainable& out) {
    for (int i = 0; i < kTrainableCount; ++i) {
        if (name == kHusbandryTrainableNames[i]) {
            out = static_cast<Trainable>(i);
            return true;
        }
    }
    return false;
}

Husbandry::Husbandry(Things& things, const Map& map, AnimalSystem& animals, HealthSystem& health,
                     const DefDatabase& defs, const GameTime& time, uint64_t seed)
    : things_(things),
      map_(map),
      animals_(animals),
      health_(health),
      defs_(defs),
      time_(time),
      seed_(splitMix64(seed ^ 0x6875736261ULL)) {
    // Fallbacks, the same as the file, for the test defs that don't have one.
    rules_.dietFoods[0] = {"Plant"};
    rules_.dietFoods[1] = {"Meat", "Kibble"};
    rules_.dietFoods[2] = {"Plant", "Meat", "Kibble"};
    rules_.trainables = {{"Obedience", 1, 10, -1}, {"Release", 2, 5, 0}, {"Rescue", 2, 20, 0}, {"Haul", 3, 30, 0}};
    if (const Def* r = defs.find("Rules", "HusbandryRules")) {
        rules_.interactSeconds = r->number("interactSeconds", rules_.interactSeconds);
        rules_.slaughterSeconds = r->number("slaughterSeconds", rules_.slaughterSeconds);
        rules_.feedSeconds = r->number("feedSeconds", rules_.feedSeconds);
        rules_.skillChanceBase = r->number("skillChanceBase", rules_.skillChanceBase);
        rules_.skillChancePerLevel = r->number("skillChancePerLevel", rules_.skillChancePerLevel);
        rules_.feedNutritionPerBodySize = r->number("feedNutritionPerBodySize", rules_.feedNutritionPerBodySize);
        rules_.handReach = r->number("handReach", rules_.handReach);
        rules_.holdRadius = r->number("holdRadius", rules_.holdRadius);
        rules_.attemptIntervalSeconds = r->number("attemptIntervalSeconds", rules_.attemptIntervalSeconds);
        rules_.decayDays = r->number("decayDays", rules_.decayDays);
        rules_.starveDays = r->number("starveDays", rules_.starveDays);
        rules_.handFeedBelow = r->number("handFeedBelow", rules_.handFeedBelow);
        rules_.workMinFood = r->number("workMinFood", rules_.workMinFood);
        rules_.homeRadius = r->number("homeRadius", rules_.homeRadius);
        rules_.itemRadius = r->number("itemRadius", rules_.itemRadius);
        rules_.tameXp = r->number("tameXp", rules_.tameXp);
        rules_.trainXp = r->number("trainXp", rules_.trainXp);
        rules_.slaughterXp = r->number("slaughterXp", rules_.slaughterXp);
        rules_.feedXp = r->number("feedXp", rules_.feedXp);
        rules_.repathSeconds = r->number("repathSeconds", rules_.repathSeconds);
        rules_.huntRangeShare = r->number("huntRangeShare", rules_.huntRangeShare);
        rules_.huntMinRange = r->number("huntMinRange", rules_.huntMinRange);
        rules_.huntMaxSeconds = r->number("huntMaxSeconds", rules_.huntMaxSeconds);
        rules_.huntAvoidManhunterChance = r->number("huntAvoidManhunterChance", rules_.huntAvoidManhunterChance);
        rules_.huntRepathSeconds = r->number("huntRepathSeconds", rules_.huntRepathSeconds);
        rules_.butcherEfficiencyBase = r->number("butcherEfficiencyBase", rules_.butcherEfficiencyBase);
        rules_.butcherEfficiencyPerLevel = r->number("butcherEfficiencyPerLevel", rules_.butcherEfficiencyPerLevel);
        rules_.butcherManipulationImportance = r->number("butcherManipulationImportance", rules_.butcherManipulationImportance);
        rules_.butcherMaxEfficiency = r->number("butcherMaxEfficiency", rules_.butcherMaxEfficiency);
        rules_.butcherXp = r->number("butcherXp", rules_.butcherXp);
        for (int d = 0; d < 3; ++d) {
            const std::string key = std::string("dietFoods.") + kHusbandryDietNames[d];
            if (r->count(key.c_str()) > 0) {
                rules_.dietFoods[d].clear();
                for (size_t i = 0; i < r->count(key.c_str()); ++i) {
                    rules_.dietFoods[d].push_back(r->textAt(key.c_str(), i));
                }
            }
        }
        for (int t = 0; t < kTrainableCount; ++t) {
            const std::string at = "trainables." + std::to_string(t) + ".";
            if (!r->has((at + "name").c_str())) {
                continue;
            }
            TrainableRule& rule = rules_.trainables[static_cast<size_t>(t)];
            rule.trainability = husbandryTrainabilityOf(r->text((at + "trainability").c_str()));
            rule.steps = std::max(1, static_cast<int>(r->number((at + "steps").c_str(), rule.steps)));
            Trainable pre;
            rule.prerequisite = trainableFromName(r->text((at + "prerequisite").c_str()), pre) ? static_cast<int>(pre) : -1;
        }
    }
    for (int t = 0; t < kTrainableCount; ++t) {
        rules_.trainables[static_cast<size_t>(t)].name = kHusbandryTrainableNames[t];
    }

    kindOfDef_.assign(defs.size(), -1);
    nutrition_.assign(defs.size(), 0.0f);
    foodType_.assign(defs.size(), 0);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.text("category") == "Pawn" && def.text("race.intelligence") == "Animal") {
            Kind k;
            k.trainability = husbandryTrainabilityOf(def.text("race.trainability"));
            k.wildness = def.number("race.wildness", 0.5);
            k.diet = husbandryDietOf(def.text("race.diet"));
            k.bodySize = static_cast<float>(def.number("race.baseBodySize", 1.0));
            kindOfDef_[id.value] = static_cast<int>(kinds_.size());
            kinds_.push_back(k);
        } else if (def.has("ingestible.foodType")) {
            nutrition_[id.value] = static_cast<float>(def.number("stats.nutrition", 0.0));
            const std::string type = def.text("ingestible.foodType");
            for (int d = 0; d < 3; ++d) {
                const std::vector<std::string>& list = rules_.dietFoods[d];
                if (std::find(list.begin(), list.end(), type) != list.end()) {
                    foodType_[id.value] = static_cast<uint8_t>(foodType_[id.value] | (1u << d));
                }
            }
        }
    }
    zoneOf_.assign(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), kNoAnimalZone);
}

Husbandry::~Husbandry() = default;

// ---- Small things ---------------------------------------------------------------------------

size_t Husbandry::slot(CellPos c) const {
    return static_cast<size_t>(c.y) * static_cast<size_t>(map_.width()) + static_cast<size_t>(c.x);
}

const Husbandry::Kind* Husbandry::kindOf(Entity animal) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Animal, Thing>(animal)) {
        return nullptr;
    }
    const uint32_t def = reg.get<Thing>(animal).def.value;
    return def < kindOfDef_.size() && kindOfDef_[def] >= 0 ? &kinds_[static_cast<size_t>(kindOfDef_[def])] : nullptr;
}

bool Husbandry::valid(Entity animal) const {
    return kindOf(animal) != nullptr;
}

void Husbandry::emit(EventType type, Entity animal, Entity handler, const char* message, float amount, int part) {
    if (events_ == nullptr) {
        return;
    }
    Event event;
    event.type = type;
    event.thing = animal;
    event.target = handler;
    event.amount = amount;
    event.part = part;
    if (const Position* pos = things_.registry().valid(animal) ? things_.registry().try_get<Position>(animal) : nullptr) {
        event.cell = cellOf(*pos);
    }
    if (message != nullptr) {
        event.message = message;
    }
    events_->push_back(std::move(event));
}

const AnimalCare* Husbandry::care(Entity animal) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(animal) ? reg.try_get<AnimalCare>(animal) : nullptr;
}

AnimalCare* Husbandry::careFor(Entity animal) {
    if (!valid(animal)) {
        return nullptr;
    }
    return &things_.registry().get_or_emplace<AnimalCare>(animal);
}

bool Husbandry::isTame(Entity animal) const {
    return valid(animal) && !things_.registry().get<Animal>(animal).wild;
}

int Husbandry::trainability(Entity animal) const {
    const Kind* k = kindOf(animal);
    return k != nullptr ? k->trainability : 0;
}

double Husbandry::wildness(Entity animal) const {
    const Kind* k = kindOf(animal);
    return k != nullptr ? k->wildness : 1.0;
}

// ---- Commands -------------------------------------------------------------------------------

bool Husbandry::markTame(Entity animal, bool on, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    if (husbandryDead(things_.registry(), animal)) {
        error = "it is dead";
        return false;
    }
    if (on && isTame(animal)) {
        error = "it is tame already";
        return false;
    }
    AnimalCare* c = careFor(animal);
    if (on && c->markHunt) {
        c->markHunt = false;  // a wild animal is to be tamed or hunted, not both
        emit(EventType::DesignationsChanged, animal, kNoEntity);
    }
    if (c->markTame != on) {
        c->markTame = on;
        emit(EventType::DesignationsChanged, animal, kNoEntity);
    }
    return true;
}

bool Husbandry::setName(Entity animal, const std::string& name, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    if (husbandryDead(things_.registry(), animal)) {
        error = "it is dead";
        return false;
    }
    if (!isTame(animal)) {
        error = "only a tame animal can be named";
        return false;
    }
    if (name.size() > kMaxAnimalNameLength) {
        error = "a name is at most 24 characters";
        return false;
    }
    careFor(animal)->name = name;
    return true;
}

bool Husbandry::markInArea(const CellRect& area, const std::string& mark, bool on, std::string& error) {
    if (mark != "Tame" && mark != "Slaughter" && mark != "Hunt") {
        error = "a mark is Tame, Slaughter or Hunt";
        return false;
    }
    std::vector<Entity> found;
    for (const auto [e, a] : things_.registry().view<Animal>().each()) {
        (void)a;
        const Position& pos = things_.registry().get<Position>(e);
        if (area.contains(CellPos{static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)), pos.level})) {
            found.push_back(e);
        }
    }
    std::sort(found.begin(), found.end());
    int marked = 0;
    std::string ignored;
    for (const Entity e : found) {
        if (husbandryDead(things_.registry(), e)) {
            continue;
        }
        const bool tame = isTame(e);
        if (mark == "Slaughter" ? !tame : tame) {
            continue;
        }
        const bool ok = mark == "Tame" ? markTame(e, on, ignored)
                        : mark == "Hunt" ? markHunt(e, on, ignored)
                                         : markSlaughter(e, on, ignored);
        marked += ok ? 1 : 0;
    }
    if (marked == 0) {
        error = mark == "Slaughter" ? "no tame animal in that area" : "no wild animal in that area";
        return false;
    }
    return true;
}

bool Husbandry::markHunt(Entity animal, bool on, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    if (husbandryDead(things_.registry(), animal)) {
        error = "it is dead";
        return false;
    }
    if (on && isTame(animal)) {
        error = "only a wild animal can be hunted; slaughter a tame one";
        return false;
    }
    AnimalCare* c = careFor(animal);
    if (on && c->markTame) {
        c->markTame = false;
        emit(EventType::DesignationsChanged, animal, kNoEntity);
    }
    if (c->markHunt != on) {
        c->markHunt = on;
        emit(EventType::DesignationsChanged, animal, kNoEntity);
    }
    return true;
}

bool Husbandry::markSlaughter(Entity animal, bool on, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    if (husbandryDead(things_.registry(), animal)) {
        error = "it is dead";
        return false;
    }
    if (on && !isTame(animal)) {
        error = "only a tame animal can be slaughtered; hunt a wild one";
        return false;
    }
    AnimalCare* c = careFor(animal);
    if (c->markSlaughter != on) {
        c->markSlaughter = on;
        emit(EventType::DesignationsChanged, animal, kNoEntity);
    }
    return true;
}

void Husbandry::setHauler(Entity animal, bool on) {
    if (haulerHook_) {
        haulerHook_(animal, on);
    }
}

bool Husbandry::release(Entity animal, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    entt::registry& reg = things_.registry();
    Animal& a = reg.get<Animal>(animal);
    if (a.wild) {
        error = "it is wild already";
        return false;
    }
    const bool hauler = learned(animal, Trainable::Haul) || learned(animal, Trainable::Rescue);
    reg.emplace_or_replace<AnimalCare>(animal);
    a.wild = true;
    a.herd = 0;
    a.feeding = false;
    a.mode = AnimalMode::Idle;
    a.nextThink = time_.seconds;
    a.staysUntil = time_.seconds + 2 * kSecondsPerHour;
    reg.emplace_or_replace<Faction>(animal, Faction{kWildFaction});
    if (hauler) {
        setHauler(animal, false);
    }
    emit(EventType::AnimalReleased, animal, kNoEntity);
    emit(EventType::DesignationsChanged, animal, kNoEntity);
    return true;
}

bool Husbandry::prerequisiteLearned(const AnimalCare& c, Trainable what) const {
    const int pre = rules_.trainables[static_cast<size_t>(what)].prerequisite;
    return pre < 0 || c.training[pre].steps >= rules_.trainables[static_cast<size_t>(pre)].steps;
}

bool Husbandry::canLearn(Entity animal, Trainable what) const {
    return isTame(animal) && trainability(animal) >= rules_.trainables[static_cast<size_t>(what)].trainability;
}

int Husbandry::steps(Entity animal, Trainable what) const {
    const AnimalCare* c = care(animal);
    return c != nullptr ? c->training[static_cast<int>(what)].steps : 0;
}

int Husbandry::stepsNeeded(Trainable what) const {
    return rules_.trainables[static_cast<size_t>(what)].steps;
}

bool Husbandry::learned(Entity animal, Trainable what) const {
    return steps(animal, what) >= stepsNeeded(what);
}

bool Husbandry::setTraining(Entity animal, Trainable what, bool wanted, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    if (!isTame(animal)) {
        error = "only a tame animal can be trained";
        return false;
    }
    if (wanted && !canLearn(animal, what)) {
        error = trainability(animal) == 0 ? "this kind of animal can't be trained"
                                          : std::string("this animal is not clever enough to learn ") + trainableName(what);
        return false;
    }
    AnimalCare* c = careFor(animal);
    c->training[static_cast<int>(what)].wanted = wanted;
    if (wanted) {
        const int pre = rules_.trainables[static_cast<size_t>(what)].prerequisite;
        if (pre >= 0) {
            c->training[pre].wanted = true;
        }
    } else if (!learned(animal, what)) {
        for (int t = 0; t < kTrainableCount; ++t) {
            if (rules_.trainables[static_cast<size_t>(t)].prerequisite == static_cast<int>(what)) {
                c->training[t].wanted = false;
            }
        }
    }
    return true;
}

bool Husbandry::nextToTrain(Entity animal, Trainable& out) const {
    const AnimalCare* c = care(animal);
    if (c == nullptr || !isTame(animal) || husbandryDead(things_.registry(), animal)) {
        return false;
    }
    for (int t = 0; t < kTrainableCount; ++t) {
        const Trainable what = static_cast<Trainable>(t);
        if (c->training[t].wanted && canLearn(animal, what) && !learned(animal, what) && prerequisiteLearned(*c, what)) {
            out = what;
            return true;
        }
    }
    return false;
}

bool Husbandry::trainingDue(Entity animal) const {
    const AnimalCare* c = care(animal);
    return c != nullptr && time_.seconds >= c->nextTrainAt;
}

uint32_t Husbandry::learnedMask(Entity animal) const {
    uint32_t mask = 0;
    for (int t = 0; t < kTrainableCount; ++t) {
        mask |= learned(animal, static_cast<Trainable>(t)) ? (1u << t) : 0u;
    }
    return mask;
}

uint32_t Husbandry::wantedMask(Entity animal) const {
    const AnimalCare* c = care(animal);
    uint32_t mask = 0;
    for (int t = 0; c != nullptr && t < kTrainableCount; ++t) {
        mask |= c->training[t].wanted ? (1u << t) : 0u;
    }
    return mask;
}

double Husbandry::skillFactor(int skill) const {
    return std::max(0.0, rules_.skillChanceBase + rules_.skillChancePerLevel * static_cast<double>(skill));
}

double Husbandry::tameChance(int skill, Entity animal) const {
    return std::clamp(skillFactor(skill) * (1.0 - wildness(animal)), 0.0, 1.0);
}

double Husbandry::trainChance(int skill, Entity animal) const {
    return std::clamp(skillFactor(skill) * (1.5 - wildness(animal)), 0.0, 1.0);
}

double Husbandry::feedNutrition(Entity animal) const {
    const Kind* k = kindOf(animal);
    return rules_.feedNutritionPerBodySize * (k != nullptr ? static_cast<double>(k->bodySize) : 1.0);
}

bool Husbandry::eats(DefId animalDef, DefId item) const {
    if (!animalDef.valid() || !item.valid() || animalDef.value >= kindOfDef_.size() || item.value >= foodType_.size() ||
        kindOfDef_[animalDef.value] < 0) {
        return false;
    }
    const Kind& k = kinds_[static_cast<size_t>(kindOfDef_[animalDef.value])];
    return (foodType_[item.value] & (1u << k.diet)) != 0 && nutrition_[item.value] > 0.0f;
}

bool Husbandry::animalEats(Entity animal, DefId item) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(animal) && reg.all_of<Thing>(animal) && eats(reg.get<Thing>(animal).def, item);
}

float Husbandry::nutritionOf(DefId item) const {
    return item.valid() && item.value < nutrition_.size() ? nutrition_[item.value] : 0.0f;
}

// ---- Zones ----------------------------------------------------------------------------------

const AnimalZone* Husbandry::findZone(AnimalZoneId id) const {
    for (const AnimalZone& zone : saved_.zones) {
        if (zone.id == id) {
            return &zone;
        }
    }
    return nullptr;
}

AnimalZoneId Husbandry::zoneAt(CellPos cell) const {
    return map_.inBounds(cell) ? zoneOf_[slot(cell)] : kNoAnimalZone;
}

AnimalZoneId Husbandry::createZone(const CellRect& area) {
    AnimalZone zone;
    zone.id = saved_.nextZone;
    for (int y = area.minY; y < area.minY + area.sizeY; ++y) {
        for (int x = area.minX; x < area.minX + area.sizeX; ++x) {
            const CellPos c{x, y, 0};
            if (area.level == 0 && map_.walkable(c) && zoneOf_[slot(c)] == kNoAnimalZone) {
                zone.cells.push_back(c);
            }
        }
    }
    if (zone.cells.empty()) {
        return kNoAnimalZone;
    }
    for (const CellPos& c : zone.cells) {
        zoneOf_[slot(c)] = zone.id;
    }
    ++saved_.nextZone;
    ++zoneChanges_;
    saved_.zones.push_back(std::move(zone));
    emit(EventType::DesignationsChanged, kNoEntity, kNoEntity);
    return saved_.zones.back().id;
}

int Husbandry::removeZoneCells(const CellRect& area) {
    int removed = 0;
    for (AnimalZone& zone : saved_.zones) {
        const size_t before = zone.cells.size();
        zone.cells.erase(std::remove_if(zone.cells.begin(), zone.cells.end(),
                                        [&](const CellPos& c) {
                                            const bool in = area.level == c.level && c.x >= area.minX && c.x < area.minX + area.sizeX &&
                                                            c.y >= area.minY && c.y < area.minY + area.sizeY;
                                            if (in) {
                                                zoneOf_[slot(c)] = kNoAnimalZone;
                                            }
                                            return in;
                                        }),
                         zone.cells.end());
        removed += static_cast<int>(before - zone.cells.size());
    }
    if (removed == 0) {
        return 0;
    }
    // Animals of a zone that is gone roam freely again.
    std::vector<AnimalZoneId> gone;
    for (const AnimalZone& zone : saved_.zones) {
        if (zone.cells.empty()) {
            gone.push_back(zone.id);
        }
    }
    saved_.zones.erase(std::remove_if(saved_.zones.begin(), saved_.zones.end(),
                                      [](const AnimalZone& z) { return z.cells.empty(); }),
                       saved_.zones.end());
    if (!gone.empty()) {
        for (const auto [e, c] : things_.registry().view<AnimalCare>().each()) {
            if (std::find(gone.begin(), gone.end(), c.zone) != gone.end()) {
                c.zone = kNoAnimalZone;
            }
        }
    }
    ++zoneChanges_;
    emit(EventType::DesignationsChanged, kNoEntity, kNoEntity);
    return removed;
}

bool Husbandry::assignZone(Entity animal, AnimalZoneId zone, std::string& error) {
    if (!valid(animal)) {
        error = "that is no animal";
        return false;
    }
    if (!isTame(animal)) {
        error = "only a tame animal can be kept in a zone";
        return false;
    }
    if (zone != kNoAnimalZone && findZone(zone) == nullptr) {
        error = "no such animal zone";
        return false;
    }
    careFor(animal)->zone = zone;
    return true;
}

bool Husbandry::setZoneBreeding(AnimalZoneId zone, bool allow, std::string& error) {
    for (AnimalZone& z : saved_.zones) {
        if (z.id == zone) {
            z.breedingOff = !allow;
            return true;
        }
    }
    error = "no such animal zone";
    return false;
}

bool Husbandry::isPen(AnimalZoneId id) const {
    const uint64_t stamp[2] = {zoneChanges_, map_.buildingChanges()};
    if (stamp[0] != penStamp_[0] || stamp[1] != penStamp_[1]) {
        penCache_.clear();
        penStamp_[0] = stamp[0];
        penStamp_[1] = stamp[1];
    }
    for (const auto& [zone, pen] : penCache_) {
        if (zone == id) {
            return pen;
        }
    }
    bool pen = false;
    if (const AnimalZone* zone = findZone(id)) {
        pen = true;
        static constexpr int kDx[4] = {1, -1, 0, 0};
        static constexpr int kDy[4] = {0, 0, 1, -1};
        for (const CellPos& c : zone->cells) {
            for (int d = 0; d < 4 && pen; ++d) {
                const CellPos n{c.x + kDx[d], c.y + kDy[d], c.level};
                // A way out: ground an animal can walk onto that is not in the zone and not a door (animals don't open doors).
                if (map_.walkable(n) && !map_.isDoor(n) && zoneOf_[slot(n)] != id) {
                    pen = false;
                }
            }
            if (!pen) {
                break;
            }
        }
    }
    penCache_.emplace_back(id, pen);
    return pen;
}

// ---- Work done by the jobs ------------------------------------------------------------------

void Husbandry::hold(Entity animal, int64_t until) {
    animals_.hold(animal, until);
}

void Husbandry::becomeTame(Entity handler, Entity animal) {
    entt::registry& reg = things_.registry();
    Animal& a = reg.get<Animal>(animal);
    const int64_t now = time_.seconds;
    a.wild = false;
    a.herd = 0;
    a.feeding = false;
    a.staysUntil = 0;
    a.mode = AnimalMode::Idle;
    a.nextThink = now;
    reg.emplace_or_replace<Faction>(animal, Faction{kPlayerFaction});
    AnimalCare* c = careFor(animal);
    c->markTame = false;
    c->hasHome = true;
    if (const Position* pos = reg.try_get<Position>(animal)) {
        c->home = cellOf(*pos);
    }
    c->tamedAt = now;
    ++saved_.stats.tamed;
    emit(EventType::AnimalTamed, animal, handler);
    emit(EventType::DesignationsChanged, animal, kNoEntity);
}

bool Husbandry::joinColony(Entity animal) {
    if (!valid(animal) || !things_.registry().get<Animal>(animal).wild) {
        return false;
    }
    becomeTame(kNoEntity, animal);
    return true;
}

bool Husbandry::finishTame(Entity handler, Entity animal, int skill) {
    if (!valid(animal) || !things_.registry().get<Animal>(animal).wild) {
        return false;
    }
    const double chance = tameChance(skill, animal);
    Rng rng = husbandryRngFor(seed_, animal, time_.seconds, 1);
    if (static_cast<double>(rng.unit()) < chance) {
        becomeTame(handler, animal);
        return true;
    }
    ++saved_.stats.tameFailed;
    emit(EventType::AnimalAttemptFailed, animal, handler, "tame", static_cast<float>(chance));
    return false;
}

bool Husbandry::finishTrain(Entity handler, Entity animal, Trainable what, int skill) {
    AnimalCare* c = careFor(animal);
    if (c == nullptr || !isTame(animal)) {
        return false;
    }
    const int64_t now = time_.seconds;
    c->lastTrainAt = now;
    c->nextTrainAt = now + static_cast<int64_t>(rules_.attemptIntervalSeconds);
    const double chance = trainChance(skill, animal);
    Rng rng = husbandryRngFor(seed_, animal, now, 2 + static_cast<uint64_t>(what));
    if (!(static_cast<double>(rng.unit()) < chance)) {
        ++saved_.stats.trainingFailed;
        emit(EventType::AnimalAttemptFailed, animal, handler, trainableName(what), static_cast<float>(chance));
        return false;
    }
    TrainingState& state = c->training[static_cast<int>(what)];
    const int needed = stepsNeeded(what);
    if (state.steps < needed) {
        ++state.steps;
    }
    ++saved_.stats.trainingSteps;
    emit(EventType::AnimalTrained, animal, handler, trainableName(what), static_cast<float>(state.steps), needed);
    if (state.steps == needed && (what == Trainable::Haul || what == Trainable::Rescue)) {
        setHauler(animal, true);
    }
    return true;
}

bool Husbandry::finishSlaughter(Entity handler, Entity animal) {
    if (!valid(animal) || husbandryDead(things_.registry(), animal)) {
        return false;
    }
    if (AnimalCare* c = careFor(animal)) {
        c->markSlaughter = false;
        c->collect = true;
    }
    if (!health_.kill(animal, DeathCause::Slaughtered)) {
        return false;
    }
    ++saved_.stats.slaughtered;
    emit(EventType::AnimalSlaughtered, animal, handler);
    emit(EventType::DesignationsChanged, animal, kNoEntity);
    return true;
}

void Husbandry::finishHunt(Entity hunter, Entity animal) {
    AnimalCare* c = valid(animal) ? careFor(animal) : nullptr;
    if (c == nullptr || c->collect) {
        return;
    }
    c->markHunt = false;
    c->collect = true;
    ++saved_.stats.hunted;
    emit(EventType::AnimalHunted, animal, hunter);
    emit(EventType::DesignationsChanged, animal, kNoEntity);
}

bool Husbandry::isCorpse(Entity animal) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Thing>(animal) || !husbandryDead(reg, animal) || !things_.spawned(animal)) {
        return false;
    }
    const Def& def = defs_.get(reg.get<Thing>(animal).def);
    return def.has("race.meatDef") || def.has("race.leatherDef");
}

Husbandry::ButcherYield Husbandry::butcherYield(Entity corpse, double efficiency) const {
    ButcherYield out;
    if (!isCorpse(corpse)) {
        return out;
    }
    const Kind* kind = kindOf(corpse);  // none for a person: a full body
    const Def& def = defs_.get(things_.registry().get<Thing>(corpse).def);
    const double share = kind != nullptr && kind->bodySize > 0.0f
                             ? static_cast<double>(animals_.bodySize(corpse)) / static_cast<double>(kind->bodySize)
                             : 1.0;
    const double scale = std::max(0.0, efficiency) * share;
    if (def.has("race.meatDef")) {
        out.meat = defs_.idOf("Things", def.text("race.meatDef"));
        out.meatCount = out.meat.valid() ? static_cast<int>(std::lround(def.number("race.meatAmount", 0.0) * scale)) : 0;
    }
    if (def.has("race.leatherDef")) {
        out.leather = defs_.idOf("Things", def.text("race.leatherDef"));
        out.leatherCount = out.leather.valid() ? static_cast<int>(std::lround(def.number("race.leatherAmount", 0.0) * scale)) : 0;
    }
    return out;
}

bool Husbandry::wantsHauling(Entity animal) const {
    if (!isCorpse(animal)) {
        return false;
    }
    const AnimalCare* c = care(animal);
    return c != nullptr && (c->collect || c->markHunt);
}

void Husbandry::feed(Entity animal, float nutrition) {
    ++saved_.stats.fed;
    animals_.addFood(animal, nutrition);
}

void Husbandry::noteStarved(Entity animal) {
    ++saved_.stats.starved;
    emit(EventType::AnimalStarved, animal, kNoEntity);
}

void Husbandry::step() {
    const int64_t now = time_.seconds;
    if (now <= 0 || now % kSecondsPerHour != 0) {
        return;
    }
    // Training that was started and not finished fades: a step a `decayDays` after the last attempt.
    const int64_t period = std::max<int64_t>(1, static_cast<int64_t>(rules_.decayDays * static_cast<double>(kSecondsPerDay)));
    for (const auto [e, c] : things_.registry().view<AnimalCare>().each()) {
        (void)e;
        for (int t = 0; t < kTrainableCount; ++t) {
            TrainingState& state = c.training[t];
            if (state.steps > 0 && state.steps < rules_.trainables[static_cast<size_t>(t)].steps && now - c.lastTrainAt >= period) {
                --state.steps;
                c.lastTrainAt += period;
            }
        }
    }
}

// ---- Saving ---------------------------------------------------------------------------------

bool Husbandry::restore(HusbandrySaved state, std::string& error) {
    std::fill(zoneOf_.begin(), zoneOf_.end(), kNoAnimalZone);
    for (const AnimalZone& zone : state.zones) {
        if (zone.id == kNoAnimalZone || zone.id >= state.nextZone || zone.cells.empty()) {
            error = "the save has a bad animal zone";
            return false;
        }
        for (const CellPos& c : zone.cells) {
            if (!map_.inBounds(c) || zoneOf_[slot(c)] != kNoAnimalZone) {
                error = "the save has animal zones on the same cell or off the map";
                return false;
            }
            zoneOf_[slot(c)] = zone.id;
        }
    }
    saved_ = std::move(state);
    ++zoneChanges_;
    return true;
}

void Husbandry::hashState(StateHasher& h) const {
    // Nothing at all until something was done, so games that never touch animals hash as they always did.
    const HusbandryStats& s = saved_.stats;
    if (saved_.nextZone == 1 && saved_.zones.empty() && s.tamed == 0 && s.tameFailed == 0 && s.trainingSteps == 0 &&
        s.trainingFailed == 0 && s.slaughtered == 0 && s.fed == 0 && s.starved == 0) {
        return;
    }
    h.add(saved_.nextZone);
    h.add(static_cast<uint64_t>(saved_.zones.size()));
    for (const AnimalZone& zone : saved_.zones) {
        h.add(zone.id);
        if (zone.breedingOff) {
            h.add(uint64_t{1});
        }
        for (const CellPos& c : zone.cells) {
            h.add(c.x);
            h.add(c.y);
        }
    }
    h.add(saved_.stats.tamed);
    h.add(saved_.stats.tameFailed);
    h.add(saved_.stats.trainingSteps);
    h.add(saved_.stats.trainingFailed);
    h.add(saved_.stats.slaughtered);
    h.add(saved_.stats.fed);
    h.add(saved_.stats.starved);
    if (saved_.stats.hunted != 0) {  // only once there was a hunt, so older recordings hash as before
        h.add(saved_.stats.hunted);
    }
}

void Husbandry::hashThing(Entity e, StateHasher& h) const {
    const AnimalCare* c = care(e);
    if (c == nullptr) {
        return;
    }
    h.add(c->markTame);
    h.add(c->markSlaughter);
    if (!c->name.empty()) {  // only once named, so older recordings hash as before
        h.add(c->name.data(), c->name.size());
    }
    if (c->markHunt || c->collect) {
        h.add(c->markHunt);
        h.add(c->collect);
    }
    h.add(c->zone);
    h.add(c->hasHome);
    h.add(c->home.x);
    h.add(c->home.y);
    h.add(c->tamedAt);
    h.add(c->heldUntil);
    h.add(c->starvingSince);
    h.add(c->nextTrainAt);
    h.add(c->lastTrainAt);
    h.add(c->noPathUntil);
    for (const TrainingState& t : c->training) {
        h.add(static_cast<int>(t.steps));
        h.add(t.wanted);
    }
    h.add(c->path.slot);
    h.add(c->path.serial);
    h.add(c->pathGoal.x);
    h.add(c->pathGoal.y);
    h.add(static_cast<int>(c->pathMode));
}

}  // namespace colony

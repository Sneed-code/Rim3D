// Breeding and animal products (task N4, docs/design/breeding.md).
#include "ColonySim/Breeding.h"

#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// Each decision gets a stream of its own from the animal and the second, so the order animals are visited in
// can't change anything.
Rng breedRngFor(uint64_t seed, Entity e, int64_t now, uint64_t what) {
    return Rng(splitMix64(seed ^ (static_cast<uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ULL) ^
                          (static_cast<uint64_t>(now) << 17) ^ (what * 0xD6E8FEB86659FD93ULL)),
               11);
}

float breedDist2(float ax, float ay, float bx, float by) {
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

int64_t breedSeconds(double days) {
    return static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
}

}  // namespace

Breeding::Breeding(Things& things, const Map& map, AnimalSystem& animals, Husbandry& husbandry, const DefDatabase& defs,
                   const GameTime& time, uint64_t seed)
    : things_(things),
      map_(map),
      animals_(animals),
      husbandry_(husbandry),
      defs_(defs),
      time_(time),
      seed_(splitMix64(seed ^ 0x627265656469ULL)) {
    if (const Def* r = defs.find("Rules", "BreedingRules")) {
        rules_.passSeconds = std::max(1, static_cast<int>(r->number("passSeconds", rules_.passSeconds)));
        rules_.mateMeanHours = std::max(0.01, r->number("mateMeanHours", rules_.mateMeanHours));
        rules_.mateChance = r->number("mateChance", rules_.mateChance);
        rules_.mateRadius = r->number("mateRadius", rules_.mateRadius);
        rules_.mateSearchRadius = r->number("mateSearchRadius", rules_.mateSearchRadius);
        rules_.courtSeconds = r->number("courtSeconds", rules_.courtSeconds);
        rules_.mateHoldSeconds = r->number("mateHoldSeconds", rules_.mateHoldSeconds);
        rules_.mateCooldownHours = r->number("mateCooldownHours", rules_.mateCooldownHours);
        rules_.birthCooldownDays = r->number("birthCooldownDays", rules_.birthCooldownDays);
        rules_.minFood = r->number("minFood", rules_.minFood);
        rules_.milkWorkSeconds = r->number("milkWorkSeconds", rules_.milkWorkSeconds);
        rules_.woolWorkSeconds = r->number("woolWorkSeconds", rules_.woolWorkSeconds);
        rules_.gatherXp = r->number("gatherXp", rules_.gatherXp);
        rules_.birthSpread = r->number("birthSpread", rules_.birthSpread);
        rules_.hatchMinTemp = r->number("hatchMinTemp", rules_.hatchMinTemp);
        rules_.hatchMaxTemp = r->number("hatchMaxTemp", rules_.hatchMaxTemp);
    }
    kinds_.assign(defs.size(), Kind{});
    isAnimal_.assign(defs.size(), 0);
    hatches_.assign(defs.size(), Hatch{});
    stackLimits_.assign(defs.size(), 1);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        stackLimits_[id.value] = std::max(1, static_cast<int>(def.integer("stackLimit", 1)));
        if (def.has("hatches.into")) {
            hatches_[id.value].into = defs.idOf("Things", def.text("hatches.into"));
            hatches_[id.value].seconds = def.number("hatches.days", 3.0) * static_cast<double>(kSecondsPerDay);
        }
        if (def.text("category") != "Pawn" || def.text("race.intelligence") != "Animal") {
            continue;
        }
        isAnimal_[id.value] = 1;
        Kind& k = kinds_[id.value];
        if (def.has("race.gestationDays")) {
            k.gestates = true;
            k.gestationSeconds = def.number("race.gestationDays", 10.0) * static_cast<double>(kSecondsPerDay);
            k.litterMin = std::max(1, static_cast<int>(def.number("race.litterSize.min", 1.0)));
            k.litterMax = std::max(k.litterMin, static_cast<int>(def.number("race.litterSize.max", static_cast<double>(k.litterMin))));
        }
        if (def.has("race.eggs")) {
            k.eggUnfertilized = defs.idOf("Things", def.text("race.eggs.unfertilizedThing"));
            k.eggFertilized = defs.idOf("Things", def.text("race.eggs.fertilizedThing"));
            k.eggs = k.eggUnfertilized.valid();
            k.eggIntervalSeconds = def.number("race.eggs.intervalDays", 1.0) * static_cast<double>(kSecondsPerDay);
            k.eggCount = std::max(1, static_cast<int>(def.number("race.eggs.count", 1.0)));
        }
        if (def.has("race.milk")) {
            k.milkThing = defs.idOf("Things", def.text("race.milk.thing"));
            k.milkIntervalSeconds = def.number("race.milk.intervalDays", 1.0) * static_cast<double>(kSecondsPerDay);
            k.milkAmount = static_cast<int>(def.number("race.milk.amount", 0.0));
        }
        if (def.has("race.wool")) {
            k.woolThing = defs.idOf("Things", def.text("race.wool.thing"));
            k.woolIntervalSeconds = def.number("race.wool.intervalDays", 1.0) * static_cast<double>(kSecondsPerDay);
            k.woolAmount = static_cast<int>(def.number("race.wool.amount", 0.0));
        }
    }
}

Breeding::~Breeding() = default;

// ---- Questions ------------------------------------------------------------------------------

const Breeding::Kind* Breeding::kindOf(Entity animal) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Animal, Thing>(animal)) {
        return nullptr;
    }
    const uint32_t def = reg.get<Thing>(animal).def.value;
    return def < isAnimal_.size() && isAnimal_[def] != 0 ? &kinds_[def] : nullptr;
}

AnimalSex Breeding::sexOf(Entity animal) const {
    const uint64_t h = splitMix64(seed_ ^ (static_cast<uint64_t>(entt::to_integral(animal)) * 0x9E3779B97F4A7C15ULL));
    return ((h >> 20) & 1U) != 0 ? AnimalSex::Female : AnimalSex::Male;
}

bool Breeding::upAndAbout(Entity animal) const {
    const Health* health = things_.registry().try_get<Health>(animal);
    return health == nullptr || !health->incapacitated();
}

bool Breeding::matureTame(Entity animal) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr || !husbandry_.isTame(animal)) {
        return false;
    }
    const Health* health = things_.registry().try_get<Health>(animal);
    return (health == nullptr || !health->dead) && animals_.isAdult(animal);
}

const AnimalBreed* Breeding::breed(Entity animal) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(animal) ? reg.try_get<AnimalBreed>(animal) : nullptr;
}

bool Breeding::pregnant(Entity animal) const {
    const AnimalBreed* b = breed(animal);
    return b != nullptr && b->pregnantUntil >= 0;
}

bool Breeding::laysEggs(Entity animal) const {
    const Kind* kind = kindOf(animal);
    return kind != nullptr && kind->eggs;
}

bool Breeding::gives(Entity animal, AnimalProduct what) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr) {
        return false;
    }
    if (what == AnimalProduct::Milk) {
        return kind->milkAmount > 0 && kind->milkThing.valid() && female(animal);
    }
    return kind->woolAmount > 0 && kind->woolThing.valid();
}

DefId Breeding::productDef(Entity animal, AnimalProduct what) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr) {
        return DefId{};
    }
    return what == AnimalProduct::Milk ? kind->milkThing : kind->woolThing;
}

int Breeding::productAmount(Entity animal, AnimalProduct what) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr) {
        return 0;
    }
    return what == AnimalProduct::Milk ? kind->milkAmount : kind->woolAmount;
}

double Breeding::gatherSeconds(AnimalProduct what) const {
    return what == AnimalProduct::Milk ? rules_.milkWorkSeconds : rules_.woolWorkSeconds;
}

float Breeding::fullness(Entity animal, AnimalProduct what) const {
    const Kind* kind = kindOf(animal);
    if (kind == nullptr || !gives(animal, what) || !matureTame(animal)) {
        return -1.0f;
    }
    const AnimalBreed* b = breed(animal);
    if (b == nullptr) {
        return 0.0f;
    }
    const int64_t since = what == AnimalProduct::Milk ? b->milkSince : b->woolSince;
    const double interval = what == AnimalProduct::Milk ? kind->milkIntervalSeconds : kind->woolIntervalSeconds;
    if (since == kBreedNotStarted || interval <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp(static_cast<double>(time_.seconds - since) / interval, 0.0, 1.0));
}

bool Breeding::readyToGather(Entity animal, AnimalProduct what) const {
    return fullness(animal, what) >= 1.0f;
}

// ---- Events and items -----------------------------------------------------------------------

void Breeding::emit(EventType type, Entity thing, Entity target, DefId def, CellPos cell, const char* message, float amount) {
    if (events_ == nullptr) {
        return;
    }
    Event event;
    event.type = type;
    event.thing = thing;
    event.target = target;
    event.def = def;
    event.cell = cell;
    event.amount = amount;
    if (message != nullptr) {
        event.message = message;
    }
    events_->push_back(std::move(event));
}

// Puts `count` of an item def at a cell: onto a stack of the same def there that has room, else as new stacks.
// Returns the last stack touched, or none if nothing could be placed.
static Entity breedDrop(Things& things, const std::vector<int>& limits, DefId def, CellPos cell, int count) {
    entt::registry& reg = things.registry();
    const int limit = def.value < limits.size() ? limits[def.value] : 1;
    Entity last = kNoEntity;
    while (count > 0) {
        const int n = std::min(count, limit);
        Entity into = kNoEntity;
        for (const Entity item : things.itemsAt(cell)) {
            if (reg.valid(item) && reg.all_of<Thing, StackCount>(item) && reg.get<Thing>(item).def == def &&
                reg.get<StackCount>(item).count + n <= limit && !reg.all_of<Hatching>(item)) {
                into = item;
                break;
            }
        }
        if (into != kNoEntity) {
            const Entity from = things.make(def, n);
            if (from == kNoEntity) {
                break;
            }
            things.addToStack(into, from, n);
            things.destroy(from);
            last = into;
        } else {
            const Entity made = things.spawn(def, cell, n);
            if (made == kNoEntity) {
                break;
            }
            last = made;
        }
        count -= n;
    }
    return last;
}

bool Breeding::finishGather(Entity handler, Entity animal, AnimalProduct what) {
    if (!readyToGather(animal, what)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    const DefId def = productDef(animal, what);
    const int amount = productAmount(animal, what);
    const CellPos cell = cellOf(reg.get<Position>(animal));
    breedDrop(things_, stackLimits_, def, cell, amount);
    AnimalBreed& b = reg.get<AnimalBreed>(animal);
    (what == AnimalProduct::Milk ? b.milkSince : b.woolSince) = time_.seconds;
    if (what == AnimalProduct::Milk) {
        ++stats_.milked;
    } else {
        ++stats_.sheared;
    }
    emit(EventType::AnimalGathered, animal, handler, def, cell, what == AnimalProduct::Milk ? "milk" : "wool",
         static_cast<float>(amount));
    return true;
}

// ---- Young ----------------------------------------------------------------------------------

Entity Breeding::spawnYoung(DefId def, Entity mother, CellPos around, int64_t now) {
    entt::registry& reg = things_.registry();
    Entity baby = kNoEntity;
    const int spread = std::max(0, static_cast<int>(std::ceil(rules_.birthSpread)));
    for (int r = 0; r <= spread && baby == kNoEntity; ++r) {
        for (int dy = -r; dy <= r && baby == kNoEntity; ++dy) {
            for (int dx = -r; dx <= r && baby == kNoEntity; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const CellPos c{around.x + dx, around.y + dy, around.level};
                if (map_.inBounds(c) && map_.walkable(c)) {
                    baby = things_.spawn(def, c);
                }
            }
        }
    }
    if (baby == kNoEntity) {
        return kNoEntity;
    }
    Animal& a = reg.get<Animal>(baby);
    a.wild = false;
    a.bornAt = now;
    a.herd = 0;
    reg.emplace_or_replace<Faction>(baby, Faction{kPlayerFaction});
    AnimalCare* care = husbandry_.careFor(baby);
    care->tamedAt = now;
    care->hasHome = true;
    care->home = around;
    const AnimalCare* motherCare = mother != kNoEntity ? husbandry_.care(mother) : nullptr;
    if (motherCare != nullptr) {
        care->zone = motherCare->zone;
        if (motherCare->hasHome) {
            care->home = motherCare->home;
        }
    } else {
        care->zone = husbandry_.zoneAt(around);
    }
    return baby;
}

void Breeding::giveBirth(Entity mother, AnimalBreed& b, const Kind& kind, int64_t now) {
    (void)kind;
    entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(mother).def;
    const CellPos at = cellOf(reg.get<Position>(mother));
    const int litter = std::max(1, static_cast<int>(b.litter));
    b.pregnantUntil = -1;
    b.litter = 0;
    b.nextMateAt = now + breedSeconds(rules_.birthCooldownDays);
    for (int i = 0; i < litter; ++i) {
        const Entity baby = spawnYoung(def, mother, at, now);
        if (baby != kNoEntity) {
            ++stats_.born;
            emit(EventType::AnimalBorn, baby, mother, def, at, nullptr, static_cast<float>(litter));
        }
    }
}

Entity Breeding::placeEgg(DefId def, CellPos cell, int64_t now, bool fertilized) {
    entt::registry& reg = things_.registry();
    const int count = 1;
    if (!fertilized) {
        return breedDrop(things_, stackLimits_, def, cell, count);
    }
    const Entity egg = things_.spawn(def, cell, count);
    if (egg != kNoEntity && def.value < hatches_.size() && hatches_[def.value].into.valid()) {
        reg.emplace_or_replace<Hatching>(egg, Hatching{now + static_cast<int64_t>(hatches_[def.value].seconds)});
    }
    return egg;
}

Entity Breeding::layEgg(Entity hen, bool fertilized) {
    const Kind* kind = kindOf(hen);
    if (kind == nullptr || !kind->eggs || !things_.spawned(hen)) {
        return kNoEntity;
    }
    const int64_t now = time_.seconds;
    const CellPos cell = cellOf(things_.registry().get<Position>(hen));
    const DefId def = fertilized && kind->eggFertilized.valid() ? kind->eggFertilized : kind->eggUnfertilized;
    Entity last = kNoEntity;
    for (int i = 0; i < kind->eggCount; ++i) {
        const Entity egg = placeEgg(def, cell, now, fertilized && kind->eggFertilized.valid());
        if (egg != kNoEntity) {
            last = egg;
            ++stats_.eggsLaid;
        }
    }
    if (last != kNoEntity) {
        emit(EventType::EggLaid, last, hen, def, cell, fertilized ? "fertilized" : nullptr, static_cast<float>(kind->eggCount));
    }
    return last;
}

void Breeding::hatch(Entity egg, int64_t now) {
    entt::registry& reg = things_.registry();
    const DefId eggDef = reg.get<Thing>(egg).def;
    const Hatch& h = hatches_[eggDef.value];
    const CellPos cell = cellOf(reg.get<Position>(egg));
    things_.destroy(egg);
    const Entity baby = spawnYoung(h.into, kNoEntity, cell, now);
    if (baby != kNoEntity) {
        ++stats_.hatched;
        emit(EventType::EggHatched, baby, kNoEntity, eggDef, cell);
    }
}

bool Breeding::makePregnant(Entity mother, int litter, int64_t bornAt) {
    const Kind* kind = kindOf(mother);
    if (kind == nullptr || !kind->gestates || !female(mother) || !matureTame(mother)) {
        return false;
    }
    AnimalBreed& b = things_.registry().get_or_emplace<AnimalBreed>(mother);
    b.pregnantUntil = bornAt;
    b.litter = static_cast<uint8_t>(std::clamp(litter, 1, 255));
    return true;
}

// ---- Mating ---------------------------------------------------------------------------------

void Breeding::mate(Entity male, Entity mother, const Kind& kind, int64_t now) {
    entt::registry& reg = things_.registry();
    AnimalBreed& m = reg.get<AnimalBreed>(male);
    AnimalBreed& f = reg.get<AnimalBreed>(mother);
    const int64_t cooldown = static_cast<int64_t>(rules_.mateCooldownHours * static_cast<double>(kSecondsPerHour));
    m.nextMateAt = now + cooldown;
    f.nextMateAt = now + cooldown;
    ++stats_.matings;
    const char* result = nullptr;
    if (kind.gestates) {
        Rng rng = breedRngFor(seed_, male, now, 2);
        if (static_cast<double>(rng.unit()) < rules_.mateChance) {
            f.pregnantUntil = now + static_cast<int64_t>(kind.gestationSeconds);
            f.litter = static_cast<uint8_t>(kind.litterMin + static_cast<int>(rng.below(static_cast<uint32_t>(kind.litterMax - kind.litterMin + 1))));
            f.nextMateAt = f.pregnantUntil;
            ++stats_.pregnancies;
            result = "pregnant";
        }
    } else if (kind.eggs) {
        f.fertilized = 1;
        result = "fertilized";
    }
    const int64_t until = now + static_cast<int64_t>(rules_.mateHoldSeconds);
    animals_.hold(male, until);
    animals_.hold(mother, until);
    emit(EventType::AnimalMated, male, mother, reg.get<Thing>(male).def, cellOf(reg.get<Position>(male)), result);
}

bool Breeding::fertileNow(Entity animal, const AnimalBreed& b, int64_t now) const {
    const entt::registry& reg = things_.registry();
    if (now < b.nextMateAt || !upAndAbout(animal)) {
        return false;
    }
    const Animal& a = reg.get<Animal>(animal);
    if (a.mode == AnimalMode::Sleep || animals_.foodShare(animal) < static_cast<float>(rules_.minFood)) {
        return false;
    }
    const AnimalCare* care = husbandry_.care(animal);
    return care == nullptr || care->heldUntil <= now;
}

bool Breeding::breedingAllowed(Entity animal) const {
    const AnimalBreed* b = breed(animal);
    if (b != nullptr && b->breedingOff) {
        return false;
    }
    const AnimalCare* care = husbandry_.care(animal);
    const AnimalZone* zone = care != nullptr && care->zone != kNoAnimalZone ? husbandry_.findZone(care->zone) : nullptr;
    return zone == nullptr || !zone->breedingOff;
}

bool Breeding::setBreeding(Entity animal, bool allow, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Thing, Animal>(animal) || kindOf(animal) == nullptr) {
        error = "that is no animal";
        return false;
    }
    if (reg.get<Animal>(animal).wild) {
        error = "only a tame animal can be told not to breed";
        return false;
    }
    reg.get_or_emplace<AnimalBreed>(animal).breedingOff = !allow;
    return true;
}

bool Breeding::willing(Entity female, const AnimalBreed& b, const Kind& kind, int64_t now) const {
    (void)kind;
    (void)female;
    (void)now;
    return b.pregnantUntil < 0 && b.fertilized == 0;
}

// ---- The pass -------------------------------------------------------------------------------

void Breeding::step() {
    const int64_t now = time_.seconds;
    if (now <= 0 || now % rules_.passSeconds != 0) {
        return;
    }
    pass(now, false);
}

void Breeding::pass(int64_t now, bool everyMaleSeeks) {
    entt::registry& reg = things_.registry();
    tame_.clear();
    males_.clear();
    females_.clear();
    for (const auto [e, a] : reg.view<const Animal>().each()) {
        if (!a.wild && a.started) {
            tame_.push_back(e);
        }
    }
    std::sort(tame_.begin(), tame_.end());

    const double perPass = static_cast<double>(rules_.passSeconds) / (rules_.mateMeanHours * static_cast<double>(kSecondsPerHour));
    for (const Entity e : tame_) {
        const Kind* kind = kindOf(e);
        if (kind == nullptr || !matureTame(e) || !reg.all_of<Position>(e)) {
            continue;
        }
        const bool isFemale = female(e);
        AnimalBreed& b = reg.get_or_emplace<AnimalBreed>(e);

        if (isFemale && b.pregnantUntil >= 0 && now >= b.pregnantUntil) {
            giveBirth(e, b, *kind, now);
        }
        // Milk, wool and eggs fill from the moment the pass first sees the animal grown up and tame.
        if (kind->milkAmount > 0 && isFemale && b.milkSince == kBreedNotStarted) {
            b.milkSince = now;
        }
        if (kind->woolAmount > 0 && b.woolSince == kBreedNotStarted) {
            b.woolSince = now;
        }
        if (kind->eggs && isFemale) {
            if (b.eggSince == kBreedNotStarted) {
                b.eggSince = now;
            } else if (static_cast<double>(now - b.eggSince) >= kind->eggIntervalSeconds) {
                const bool fertilized = b.fertilized > 0;
                if (layEgg(e, fertilized) != kNoEntity && fertilized) {
                    --b.fertilized;
                }
                const int64_t next = b.eggSince + static_cast<int64_t>(kind->eggIntervalSeconds);
                b.eggSince = now - next > rules_.passSeconds ? now : next;
            }
        }

        if ((kind->gestates || kind->eggs) && fertileNow(e, b, now) && breedingAllowed(e)) {
            const Position& pos = reg.get<Position>(e);
            const AnimalCare* care = husbandry_.care(e);
            const Candidate c{e, reg.get<Thing>(e).def, care != nullptr ? care->zone : kNoAnimalZone, pos.x, pos.y, pos.level};
            if (!isFemale) {
                males_.push_back(c);
            } else if (willing(e, b, *kind, now)) {
                females_.push_back(c);
            }
        }
    }

    // A mature male looks for a mate now and then: the nearest willing female of his kind within `mateSearchRadius`, in
    // his zone. If she is within `mateRadius` they mate; else he sets out for her and mates once he is there.
    const float reach2 = static_cast<float>(rules_.mateRadius * rules_.mateRadius);
    const float search2 = static_cast<float>(rules_.mateSearchRadius * rules_.mateSearchRadius);
    for (const Candidate& male : males_) {
        AnimalBreed& mb = reg.get<AnimalBreed>(male.animal);
        Candidate* target = nullptr;
        const bool courted = mb.courting != kNoEntity && now < mb.courtUntil;
        if (courted) {
            for (Candidate& f : females_) {
                if (f.animal == mb.courting) {
                    target = &f;
                    break;
                }
            }
        } else {
            mb.courting = kNoEntity;
            Rng rng = breedRngFor(seed_, male.animal, now, 1);
            if (!everyMaleSeeks && static_cast<double>(rng.unit()) >= perPass) {
                continue;
            }
            float bestDist = search2;
            for (Candidate& f : females_) {
                if (f.animal == kNoEntity || f.level != male.level || f.def != male.def || f.zone != male.zone) {
                    continue;
                }
                const float d = breedDist2(male.x, male.y, f.x, f.y);
                if (d <= bestDist && (target == nullptr || d < bestDist)) {
                    target = &f;
                    bestDist = d;
                }
            }
        }
        if (target == nullptr) {
            mb.courting = kNoEntity;  // she is gone, taken, or no longer willing
            continue;
        }
        if (breedDist2(male.x, male.y, target->x, target->y) <= reach2) {
            mb.courting = kNoEntity;
            mate(male.animal, target->animal, kinds_[male.def.value], now);
            target->animal = kNoEntity;
        } else {
            if (!courted) {
                mb.courting = target->animal;
                mb.courtUntil = now + static_cast<int64_t>(rules_.courtSeconds);
            }
            animals_.goTo(male.animal, CellPos{static_cast<int>(std::floor(target->x)), static_cast<int>(std::floor(target->y)), target->level});
        }
    }

    // Fertilized eggs hatch where they lie.
    eggs_.clear();
    for (auto [e, h] : reg.view<Hatching>().each()) {
        if (!reg.all_of<Position>(e)) {
            continue;
        }
        // An egg that is too cold or too hot makes no progress: its hatch time moves on with the clock.
        if (temperature_ != nullptr) {
            const float degrees = temperature_->temperatureAt(cellOf(reg.get<Position>(e)));
            if (degrees < rules_.hatchMinTemp || degrees > rules_.hatchMaxTemp) {
                h.hatchAt += rules_.passSeconds;
                continue;
            }
        }
        if (now >= h.hatchAt) {
            eggs_.push_back(e);
        }
    }
    std::sort(eggs_.begin(), eggs_.end());
    for (const Entity egg : eggs_) {
        hatch(egg, now);
    }
}

// ---- Hash -----------------------------------------------------------------------------------

void Breeding::hashState(StateHasher& h) const {
    if (stats_.total() == 0) {
        return;
    }
    h.add(stats_.matings);
    h.add(stats_.pregnancies);
    h.add(stats_.born);
    h.add(stats_.eggsLaid);
    h.add(stats_.hatched);
    h.add(stats_.milked);
    h.add(stats_.sheared);
}

void Breeding::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const AnimalBreed* b = reg.try_get<AnimalBreed>(e)) {
        if (b->breedingOff) {
            h.add(uint64_t{2});
        }
        h.add(b->pregnantUntil);
        h.add(static_cast<int>(b->litter));
        h.add(static_cast<int>(b->fertilized));
        h.add(b->nextMateAt);
        h.add(b->milkSince);
        h.add(b->woolSince);
        h.add(b->eggSince);
        h.add(static_cast<uint64_t>(entt::to_integral(b->courting)));
        h.add(b->courtUntil);
    }
    if (const Hatching* hatching = reg.try_get<Hatching>(e)) {
        h.add(hatching->hatchAt);
    }
}

}  // namespace colony

#include "ColonySim/Sim.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Breeding.h"
#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Defense.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Health.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Crafting.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Inspirations.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Social.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Research.h"
#include "ColonySim/Talents.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Power.h"
#include "ColonySim/Regions.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/RoofCollapse.h"
#include "ColonySim/UpperFloors.h"
#include "ColonySim/Weather.h"
#include "ColonySim/Zones.h"
#include "ColonySim/Narrator.h"
#include "MovementSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace colony {

namespace {

constexpr int64_t kBillUpkeepSeconds = 60;  // "until you have" bills with a restart stock look at their stock this often

const DefDatabase& simEmptyDefs() {
    static const DefDatabase empty;
    return empty;
}

std::string simCellText(CellPos c) {
    return "(" + std::to_string(c.x) + ", " + std::to_string(c.y) + ", level " + std::to_string(c.level) + ")";
}

// True if a job holds the thing (reserved, or carried).
bool simJobUses(const JobSystem& jobs, const entt::registry& reg, Entity thing) {
    if (jobs.reservedBy(JobTarget{thing, CellPos{}}) != kNoEntity) {
        return true;
    }
    for (const auto [pawn, carrying] : reg.view<const Carrying>().each()) {
        (void)pawn;
        if (carrying.thing == thing) {
            return true;
        }
    }
    return false;
}

void simHashCell(StateHasher& h, CellPos c) {
    h.add(c.x);
    h.add(c.y);
    h.add(c.level);
}

// Times its scope into the profile, if there is one.
class SimLap {
public:
    SimLap(StepProfile* profile, const char* name) : profile_(profile), name_(name) {
        if (profile_ != nullptr) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~SimLap() {
        if (profile_ != nullptr) {
            profile_->add(name_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
        }
    }
    SimLap(const SimLap&) = delete;
    SimLap& operator=(const SimLap&) = delete;

private:
    StepProfile* profile_;
    const char* name_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace

Sim::Sim(const SimSettings& settings) : Sim(settings, nullptr) {}

Sim::Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs)
    : Sim(settings, std::move(defs), EmptyStart{}) {
    placeStockpileAndThings();
    if (!settings_.scenario.empty()) {
        startScenario();
    }
    animals_->populate(settings_.wildAnimals);  // after the colonists, so no herd starts on top of them
    rooms_->update();
    power_->update();
    lights_->update();
    roomStats_->refresh();
}

Sim::Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs, EmptyStart)
    : settings_(settings),
      // No defs: share a static empty database, owned by nobody.
      defs_(defs ? std::move(defs) : std::shared_ptr<const DefDatabase>(std::shared_ptr<const DefDatabase>(), &simEmptyDefs())),
      map_(MapSettings{settings.mapSize, settings.mapSize, splitMix64(settings.seed ^ 0x6D6170ULL), settings.flatMap}),
      pathfinder_(map_),
      zones_(std::make_unique<Zones>(map_)),
      things_(std::make_unique<Things>(map_, defs_)),
      plants_(std::make_unique<Plants>(map_, *defs_, splitMix64(settings.seed ^ 0x706C616E74ULL), "TemperateForest")),
      mineables_(std::make_unique<Mineables>(map_, *defs_, splitMix64(settings.seed ^ 0x6D696E65ULL))),
      designations_(std::make_unique<Designations>(map_)),
      movement_(std::make_unique<MovementSystem>()),
      health_(std::make_unique<HealthSystem>(*things_)),
      combat_(std::make_unique<CombatSystem>(*things_, map_, *health_)),
      time_(),
      rng_(splitMix64(settings.seed ^ 0x7061776EULL)),
      groundHash_(map_.contentHash()) {
    things_->setEventLog(&events_);
    health_->setEventLog(&events_);
    health_->setWorld(&time_.seconds, settings.seed);
    weather_ = std::make_unique<Weather>(map_, *defs_, splitMix64(settings.seed ^ 0x636C696D617465ULL), "TemperateForest");
    weather_->setEventLog(&events_);
    movement_->setWeather(weather_.get());
    combat_->setWeather(weather_.get());
    construction_ = std::make_unique<Construction>(map_, *things_, *plants_, *defs_, rng_, events_);
    needs_ = std::make_unique<NeedsSystem>(*things_, *health_, *defs_);
    mood_ = std::make_unique<MoodSystem>(*things_, *needs_, *defs_);
    apparel_ = std::make_unique<ApparelSystem>(*things_, *combat_, *health_, *defs_);
    apparel_->registerSituations(*mood_);
    health_->registerSituations(*mood_);
    doorNumbers_ = DoorNumbers::read(*defs_);
    movement_->setDoors(doorNumbers_);
    pathfinder_.setDoorCost(doorNumbers_.pathCost);
    rooms_ = std::make_unique<Rooms>(map_, *things_, *defs_);
    mood_->setRooms(rooms_.get());
    floors_ = std::make_unique<Floors>(map_, *defs_);
    filth_ = std::make_unique<Filth>(map_, *things_, *floors_, *defs_, settings.seed);
    roomStats_ = std::make_unique<RoomStats>(map_, *things_, *rooms_, *floors_, *filth_, *plants_, *defs_);
    mood_->setRoomStats(roomStats_.get());
    power_ = std::make_unique<PowerGrid>(map_, *things_, *rooms_, *defs_, events_);
    lights_ = std::make_unique<Lights>(map_, *things_, *rooms_, *defs_, time_);
    // The sun for solar panels is the light grid's; a lamp that draws power glows only while it has it,
    // and the lamps are made again when that changes.
    power_->setWind([this](const GameTime&) { return weather_->wind(); });
    power_->setSun([this](const GameTime&) { return static_cast<double>(lights_->skyGlow()); });
    power_->setOnChange([this] { lights_->markDirty(); });
    // A lamp that burns fuel is dark without it (FuelSystem marks the lamps dirty when that changes).
    lights_->setActive([this](Entity building) { return power_->running(building) && jobs_->fuel().lit(building); });
    movement_->setLights(lights_.get());
    recreation_ = std::make_unique<RecreationSystem>(*things_, *defs_);
    recreation_->setRooms(rooms_.get());
    research_ = std::make_unique<ResearchSystem>(*things_, *defs_);
    medicine_ = std::make_unique<MedicineSystem>(*things_, *defs_);
    talents_ = std::make_unique<TalentSystem>(*things_, *defs_);
    temperature_ = std::make_unique<TemperatureSystem>(map_, *things_, *rooms_, *health_, *defs_,
                                                       splitMix64(settings.seed ^ 0x74656D70ULL));
    temperature_->setOutdoor(outdoorTemperature());
    temperature_->setGround(groundTemperature());
    mood_->setTemperature(temperature_.get());
    // What a pawn wears widens its comfortable range (C15): cold insulation lowers the minimum, heat raises the maximum.
    temperature_->setComfortModifier([this](Entity pawn, float& min, float& max) {
        float cold = 0.0f;
        float heat = 0.0f;
        apparel_->insulation(pawn, cold, heat);
        min -= cold;
        max += heat;
    });
    // A heater or cooler that draws power pushes heat only while it has it (E5); a campfire has no power part.
    temperature_->setSourceRunning([this](Entity building) { return power_->running(building); });
    spoilage_ = std::make_unique<SpoilageSystem>(map_, *things_, *temperature_, *roomStats_, *defs_,
                                                 splitMix64(settings.seed ^ 0x73706F696CULL));
    spoilage_->setEventLog(&events_);
    // Eating a poisoned meal gives the FoodPoisoning disease (H2); a fresh wound's chance of infection follows how
    // clean the room it was taken in is.
    spoilage_->setPoisonHandler([this](Entity pawn) { health_->contract(pawn, health_->diseaseId("FoodPoisoning")); });
    health_->setCleanliness([this](Entity pawn) {
        const Position* pos = things_->registry().try_get<Position>(pawn);
        return pos != nullptr ? roomStats_->cellCleanliness(cellOf(*pos)) : 0.0f;
    });
    farming_ = std::make_unique<Farming>(map_, *plants_, *zones_, *defs_, events_);
    farming_->setClimate(temperature_.get(), lights_.get());
    farming_->setForecast(
        [this](int64_t seconds) { return outdoorOverride_ ? *outdoorOverride_ : outdoorTemperatureAt(seconds); },
        [this]() { return time_.seconds; });
    plants_->setGrowthModifier([this](size_t cell) { return farming_->growthFactor(cell); });
    farming_->setPlantSpots(things_.get(), [this](Entity building) { return power_->running(building); });
    plants_->setFertilityOverride([this](size_t cell) { return farming_->spotFertility(cell); });
    plants_->setGrowsAnytime([this](size_t cell) { return farming_->sunLampLit(cell); });
    zones_->setCellTaken([this](CellPos c) { return farming_->zoneAt(c) != kNoGrowingZone; });
    lights_->setWeather(weather_.get());
    jobs_ = std::make_unique<JobSystem>(map_, *things_, pathfinder_, *zones_, *construction_, *plants_, *mineables_,
                                        *designations_, *defs_, *health_, *needs_, time_, stats_, events_);
    movement_->setSpeedFactor([this](Entity pawn) { return jobs_->loadSpeedFactor(pawn); });
    jobs_->setLights(lights_.get());
    jobs_->setPower(power_.get());
    jobs_->setRooms(rooms_.get());
    jobs_->setResearch(research_.get());
    jobs_->setMedicine(medicine_.get());
    jobs_->setTalents(talents_.get());
    jobs_->setFilth(filth_.get());
    jobs_->setApparel(apparel_.get());
    jobs_->setSpoilage(spoilage_.get());
    crafting_ = std::make_unique<CraftingSystem>(*things_, *apparel_, *zones_, *defs_, settings.seed);
    jobs_->setCrafting(crafting_.get());
    jobs_->setFarming(farming_.get());
    fire_ = std::make_unique<Fire>(map_, *things_, *plants_, *floors_, *filth_, *rooms_, *temperature_, *health_, *weather_, *defs_,
                                   settings.seed);
    fire_->setEventLog(&events_);
    // Fire-resistant skin (a gene's flammability factor) keeps a pawn from catching fire.
    fire_->setPawnFactor([this](Entity pawn) {
        const PawnGenes* genes = genesOf(pawn);
        return genes != nullptr ? geneStat(*genes, "flammability", 1.0f) : 1.0f;
    });
    fire_->setPawnHurt([this](Entity pawn, DefId type, float amount, Rng& rng) { return combat_->hurtPawn(pawn, type, amount, rng); });
    jobs_->setFire(fire_.get());
    jobs_->fuel().setLights(lights_.get());
    // A wood-fired generator makes power only with fuel, and burns it only while it makes power.
    power_->setFuel([this](Entity building) { return jobs_->fuel().lit(building); });
    jobs_->fuel().setBurning([this](Entity building) { return !power_->hasPower(building) || power_->stateOf(building) != PowerState::Off; });
    mood_->setActiveSkill([this](Entity pawn) { return jobs_->activeSkill(pawn); });
    fighters_ = std::make_unique<FighterSystem>(*things_, map_, *combat_, *health_, *jobs_, pathfinder_, *defs_, time_);
    explosives_ = std::make_unique<ExplosiveSystem>(*things_, map_, *combat_, *health_, *defs_);
    fighters_->setExplosives(explosives_.get());
    mentalBreaks_ = std::make_unique<MentalBreakSystem>(*things_, map_, *health_, *fighters_, *jobs_, *mood_, *rooms_,
                                                        pathfinder_, *defs_, time_);
    combat_->setEventLog(&events_);
    combat_->setTalents(talents_.get());
    explosives_->setEventLog(&events_);
    fighters_->setEventLog(&events_);
    // The biome is the one the plants use; both move to the settings together.
    animals_ = std::make_unique<AnimalSystem>(*things_, map_, *plants_, *health_, *defs_, time_,
                                              splitMix64(settings.seed ^ 0x616E696D616C73ULL), "TemperateForest");
    animals_->setEventLog(&events_);
    husbandry_ = std::make_unique<Husbandry>(*things_, map_, *animals_, *health_, *defs_, time_, settings.seed);
    husbandry_->setEventLog(&events_);
    breeding_ = std::make_unique<Breeding>(*things_, map_, *animals_, *husbandry_, *defs_, time_, settings.seed);
    breeding_->setEventLog(&events_);
    breeding_->setClimate(temperature_.get());
    jobs_->setBreeding(breeding_.get());
    animals_->setHusbandry(husbandry_.get());
    animals_->setPathfinder(&pathfinder_);
    combat_->setHurtHook([this](Entity victim, Entity attacker) { animals_->hurt(victim, attacker); });
    jobs_->setHusbandry(husbandry_.get());
    jobs_->setCombat(combat_.get());
    jobs_->setTemperature(temperature_.get());
    husbandry_->setHaulerHook([this](Entity animal, bool on) { jobs_->setAnimalHauler(animal, on); });
    jobs_->setAnimalWorkGate([this](Entity animal) { return animals_->mayWork(animal); });
    raids_ = std::make_unique<RaidSystem>(*things_, map_, *rooms_, *combat_, *health_, *jobs_, *fighters_, *defs_, time_, rng_);
    raids_->setEventLog(&events_);
    raids_->setLooksMaker([this](Entity pawn, uint64_t seed, bool female) { giveLooks(pawn, seed, female, "Baseline"); });
    prisoners_ = std::make_unique<PrisonerSystem>(*things_, map_, *combat_, *health_, *jobs_, *fighters_, *raids_, *defs_, time_, rng_);
    prisoners_->setEventLog(&events_);
    jobs_->setPrisoners(prisoners_.get());
    mood_->addSituation("isPrisoner", [this](Entity pawn) { return prisoners_->isPrisoner(pawn) ? 0 : -1; });
    // Night owls like being up at night and not by day (docs/design/mood.md); only pawns that are awake count.
    const auto awakeAt = [this](Entity pawn, bool night) {
        const Worker* worker = things_->registry().try_get<Worker>(pawn);
        if (worker == nullptr || jobIsLyingDown(worker->job)) {
            return -1;
        }
        return mood_->rules().isNightHour(time_.hourOfDay()) == night ? 0 : -1;
    };
    mood_->addSituation("awakeAtNight", [awakeAt](Entity pawn) { return awakeAt(pawn, true); });
    mood_->addSituation("awakeInDay", [awakeAt](Entity pawn) { return awakeAt(pawn, false); });
    mood_->addSituation("noBedroom", [this](Entity pawn) {
        const Entity bed = jobs_->bedOf(pawn);
        if (bed == kNoEntity || !things_->registry().all_of<Position>(bed)) {
            return 0;
        }
        return rooms_->roleAt(cellOf(things_->registry().get<Position>(bed))) == RoomRole::Bedroom ? -1 : 0;
    });
    combat_->setBeforeDestroy([this](Entity thing) {
        jobs_->forget(thing);
        raids_->forget(thing);
        defense_->onDestroy(thing);
        fire_->onDestroy(thing);
        mentalBreaks_->forget(thing);
    });
    fighters_->setWeather(weather_.get());
    melee_ = std::make_unique<MeleeSystem>(*things_, map_, *combat_, *health_, *defs_, time_,
                                           splitMix64(settings.seed ^ 0x6D656C6565ULL));
    melee_->setEventLog(&events_);
    fighters_->setMelee(melee_.get());
    upperFloors_ = std::make_unique<UpperFloors>(map_, *rooms_, *things_);
    roofCollapse_ = std::make_unique<RoofCollapse>(map_, *rooms_, *things_, *health_, *defs_);
    roofCollapse_->setEventLog(&events_);
    roofCollapse_->setInterruptHandler([this](Entity pawn, const std::string& why) { jobs_->interrupt(pawn, why); });
    construction_->setUpperFloors(upperFloors_.get());
    animals_->setMelee(melee_.get());
    defense_ = std::make_unique<DefenseSystem>(*things_, map_, *combat_, *health_, *power_, *defs_, time_);
    defense_->setEventLog(&events_);
    defense_->setBeforeDestroy([this](Entity thing) {
        jobs_->forget(thing);
        raids_->forget(thing);
        mentalBreaks_->forget(thing);
    });
    fire_->setRunning([this](Entity building) { return power_->running(building); });
    defense_->setOnLearn([this](Entity pawn) { fighters_->replan(pawn); });
    fighters_->setDefense(defense_.get());
    narrator_ = std::make_unique<NarratorSystem>(*raids_, *defs_, time_, settings.seed);
    narrator_->setEventLog(&events_);
    social_ = std::make_unique<SocialSystem>(*things_, *rooms_, *health_, *melee_, *mood_, *jobs_, *defs_, time_,
                                             splitMix64(settings.seed ^ 0x736F6369616CULL));
    social_->setEventLog(&events_);
    mentalBreaks_->setEventLog(&events_);
    mentalBreaks_->setMelee(melee_.get());
    mentalBreaks_->setFire(fire_.get());
    inspirations_ = std::make_unique<InspirationSystem>(*things_, *health_, *defs_, time_);
    inspirations_->setEventLog(&events_);
    jobs_->setInspirations(inspirations_.get());
    combat_->setInspirations(inspirations_.get());
    prisoners_->setInspirations(inspirations_.get());
    map_.regions().update();  // build every region now, not in the first step
}

Sim::~Sim() = default;

ThreatMap& Sim::threatMap(FactionId faction) {
    std::unique_ptr<ThreatMap>& slot = threatMaps_[faction];
    if (!slot) {
        slot = std::make_unique<ThreatMap>(map_);
    }
    return *slot;
}

void Sim::placeStockpileAndThings() {
    // The stockpile's middle: the walkable cell nearest the middle, searched in growing squares.
    const int size = map_.width();
    const int mid = size / 2;
    for (int r = 0; r < size && !stockpileCell_; ++r) {
        for (int y = mid - r; y <= mid + r && !stockpileCell_; ++y) {
            for (int x = mid - r; x <= mid + r; ++x) {
                const bool onRing = std::abs(x - mid) == r || std::abs(y - mid) == r;
                if (onRing && map_.walkable(CellPos{x, y, 0})) {
                    stockpileCell_ = CellPos{x, y, 0};
                    break;
                }
            }
        }
    }
    if (!stockpileCell_ || (settings_.pawns == 0 && settings_.items == 0)) {
        return;
    }

    const DefId pawnDef = defs_->idOf("Things", settings_.pawnDef);
    const DefId itemDef = defs_->idOf("Things", settings_.haulItemDef);
    const DefId hauling = defs_->idOf("WorkTypes", "Hauling");
    if (settings_.pawns > 0 && !pawnDef.valid()) {
        setupErrors_.push_back("hauling test: no Things def '" + settings_.pawnDef + "'");
    }
    if (settings_.items > 0 && !itemDef.valid()) {
        setupErrors_.push_back("hauling test: no Things def '" + settings_.haulItemDef + "'");
    }
    if (settings_.pawns > 0 && pawnDef.valid() && !hauling.valid()) {
        setupErrors_.push_back("hauling test: no WorkTypes def 'Hauling'");
    }
    if (!setupErrors_.empty()) {
        return;
    }

    // The stockpile: a square around its middle with a cell for every pawn to carry to at once
    // (each cell is held by one hauler) plus the full stacks, the cells that can hold one.
    const int stackLimit = itemDef.valid() ? jobs_->stackLimit(itemDef) : 1;
    const int cellsWanted = 2 * settings_.pawns + settings_.items / stackLimit + 1;
    const int side = std::clamp(static_cast<int>(std::ceil(std::sqrt(static_cast<double>(cellsWanted)))), 3, 32);
    const CellRect area{stockpileCell_->x - side / 2, stockpileCell_->y - side / 2, side, side, 0};
    zones_->createStockpile(area);

    // Everything reachable from the stockpile (4-connected, so always pathable).
    std::vector<int> reachable{map_.index(*stockpileCell_)};
    std::vector<uint8_t> visited(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
    visited[static_cast<size_t>(reachable[0])] = 1;
    for (size_t head = 0; head < reachable.size(); ++head) {
        const CellPos c = map_.cellAt(reachable[head]);
        const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
        for (const CellPos& n : around) {
            if (!map_.walkable(n)) {
                continue;
            }
            const int i = map_.index(n);
            if (visited[static_cast<size_t>(i)] == 0) {
                visited[static_cast<size_t>(i)] = 1;
                reachable.push_back(i);
            }
        }
    }
    // Items go outside the stockpile, so every one of them is a haul.
    reachable.erase(std::remove_if(reachable.begin(), reachable.end(),
                                   [&](int i) { return zones_->zoneAt(map_.cellAt(i)) != kNoZone; }),
                    reachable.end());

    for (int i = 0; i < settings_.items && !reachable.empty(); ++i) {
        const uint32_t pick = rng_.below(static_cast<uint32_t>(reachable.size()));
        if (things_->spawn(itemDef, map_.cellAt(reachable[pick])) == kNoEntity) {
            setupErrors_.push_back("hauling test: '" + settings_.haulItemDef + "' is not an item");
            return;
        }
    }
    haulers_.reserve(static_cast<size_t>(settings_.pawns));
    for (int i = 0; i < settings_.pawns; ++i) {
        const Entity pawn = things_->spawn(pawnDef, *stockpileCell_);
        if (pawn == kNoEntity || !things_->registry().all_of<Mover>(pawn)) {
            things_->destroy(pawn);
            setupErrors_.push_back("hauling test: '" + settings_.pawnDef + "' is not a pawn");
            return;
        }
        jobs_->addWorker(pawn, {}, false);  // the benchmark's pawns haul all day: no needs
        haulers_.push_back(pawn);
        const uint64_t looksSeed = splitMix64(settings_.seed ^ 0x6861756C6572ULL) + static_cast<uint64_t>(i);
        giveLooks(pawn, looksSeed, (splitMix64(looksSeed) & 1) != 0, "Baseline");
    }
}

void Sim::giveLooks(Entity pawn, uint64_t seed, bool female, const std::string& lineage) {
    entt::registry& reg = things_->registry();
    const Thing* thing = reg.try_get<Thing>(pawn);
    if (thing == nullptr || thing->category != ThingCategory::Pawn ||
        defs_->get(thing->def).text("race.intelligence", "Humanlike") != "Humanlike") {
        return;
    }
    PawnLooks looks;
    std::string error;
    // Without the looks rules (the test defs) the pawn gets plain looks; that is not an error.
    generateLooks(*defs_, seed, female, lineage, looks, error);
    reg.emplace_or_replace<PawnLooks>(pawn, std::move(looks));
    giveGenes(pawn);
}

void Sim::giveGenes(Entity pawn) {
    entt::registry& reg = things_->registry();
    const PawnLooks* looks = reg.try_get<PawnLooks>(pawn);
    if (looks == nullptr) {
        return;
    }
    PawnGenes genes;
    std::string error;
    // Without the Genes defs (the test defs) the pawn has no genes; that is not an error.
    makeGenes(*defs_, looks->lineage, genes, error);
    reg.emplace_or_replace<PawnGenes>(pawn, std::move(genes));
}

const PawnGenes* Sim::genesOf(Entity pawn) const {
    return things_->exists(pawn) ? things_->registry().try_get<PawnGenes>(pawn) : nullptr;
}

const PawnLooks* Sim::looksOf(Entity pawn) const {
    return things_->exists(pawn) ? things_->registry().try_get<PawnLooks>(pawn) : nullptr;
}

Entity Sim::controlledPawn() const {
    for (const Entity pawn : things_->registry().view<const PlayerControl>()) {
        return pawn;
    }
    return kNoEntity;
}

const Colonist* Sim::colonistOf(Entity pawn) const {
    return things_->exists(pawn) ? things_->registry().try_get<Colonist>(pawn) : nullptr;
}

int Sim::itemsLeft() const {
    int left = 0;
    for (const auto [item, thing] : things_->registry().view<const Thing>().each()) {
        left += thing.category == ThingCategory::Item && jobs_->storedPriority(item) == StoragePriority::Unstored ? 1 : 0;
    }
    return left;
}

uint64_t Sim::submit(Command command) {
    pending_.emplace_back(++submitted_, std::move(command));
    return submitted_;
}

void Sim::commandFailed(uint64_t id, std::string message) {
    Event event;
    event.type = EventType::CommandFailed;
    event.command = id;
    event.message = std::move(message);
    events_.push_back(std::move(event));
}

void Sim::applyCommand(const Command& command, uint64_t id) {
    std::string error;
    // A colonist in a mental break takes no orders: it isn't the player's to command until it is over.
    Entity ordered = kNoEntity;
    if (const auto* draft = std::get_if<DraftCommand>(&command)) {
        ordered = draft->drafted ? draft->pawn : kNoEntity;
    } else if (const auto* moveOrder = std::get_if<MoveOrderCommand>(&command)) {
        ordered = moveOrder->pawn;
    } else if (const auto* attack = std::get_if<AttackOrderCommand>(&command)) {
        ordered = attack->pawn;
    } else if (const auto* haul = std::get_if<OrderHaulCommand>(&command)) {
        ordered = haul->pawn;
    } else if (const auto* control = std::get_if<ControlPawnCommand>(&command)) {
        ordered = control->pawn;
    } else if (const auto* hands = std::get_if<HandsOnCommand>(&command)) {
        ordered = hands->mode == HandsOnMode::Work ? hands->pawn : kNoEntity;
    }
    if (ordered != kNoEntity && mentalBreaks_->active(ordered)) {
        commandFailed(id, "that colonist is in a mental break");
        return;
    }
    if (const auto* spawn = std::get_if<SpawnThingCommand>(&command)) {
        const DefId def = defs_->idOf("Things", spawn->def);
        Entity thing = kNoEntity;
        if (!def.valid()) {
            commandFailed(id, "no Things def '" + spawn->def + "'");
        } else if ((thing = things_->spawn(def, spawn->cell, spawn->count, spawn->rot)) == kNoEntity) {
            commandFailed(id, "can't put " + spawn->def + " at " + simCellText(spawn->cell));
        } else if (things_->registry().all_of<Mover>(thing) && !things_->registry().all_of<Animal>(thing)) {
            jobs_->addWorker(thing);  // until factions: every pawn is a colonist
            const uint64_t looksSeed = splitMix64(settings_.seed ^ 0x7370617776ULL ^ (static_cast<uint64_t>(time_.seconds) << 20) ^
                                                  entt::to_integral(thing));
            giveLooks(thing, looksSeed, (splitMix64(looksSeed) & 1) != 0, "Baseline");
        }
    } else if (const auto* destroy = std::get_if<DestroyThingCommand>(&command)) {
        if (!things_->exists(destroy->thing) || !things_->registry().all_of<Thing>(destroy->thing)) {
            commandFailed(id, "that thing doesn't exist");
        } else if (things_->registry().all_of<EquippedBy>(destroy->thing)) {
            commandFailed(id, "a pawn is holding or wearing that thing");
        } else {
            combat_->dropGear(destroy->thing);
            explosives_->dropBelt(destroy->thing);
            jobs_->forget(destroy->thing);
            fighters_->forget(destroy->thing);
            raids_->forget(destroy->thing);
            mentalBreaks_->forget(destroy->thing);
            things_->destroy(destroy->thing);
        }
    } else if (const auto* damage = std::get_if<DamagePawnCommand>(&command)) {
        const DefId type = health_->damageTypeId(damage->damageType);
        if (!health_->tracks(damage->pawn)) {
            commandFailed(id, "that thing has no body to hurt");
        } else if (!type.valid()) {
            commandFailed(id, "no DamageTypes def '" + damage->damageType + "'");
        } else if (!(damage->amount > 0.0f)) {
            commandFailed(id, "damage must be more than zero");
        } else if (!health_->applyDamage(damage->pawn, type, damage->amount, damage->part, rng_)) {
            commandFailed(id, "can't hurt that pawn there (dead, or the part is gone)");
        }
    } else if (const auto* tend = std::get_if<TendPawnCommand>(&command)) {
        if (!health_->tracks(tend->pawn)) {
            commandFailed(id, "that thing has no body to tend");
        } else if (health_->tendAll(tend->pawn, tend->quality) == 0) {
            commandFailed(id, "nothing to tend");
        }
    } else if (const auto* work = std::get_if<SetWorkPriorityCommand>(&command)) {
        const DefId type = defs_->idOf("WorkTypes", work->workType);
        if (!type.valid()) {
            commandFailed(id, "no WorkTypes def '" + work->workType + "'");
        } else if (!jobs_->setPriority(work->pawn, type, work->priority, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* create = std::get_if<CreateStockpileCommand>(&command)) {
        if (create->priority == StoragePriority::Unstored) {
            commandFailed(id, "a stockpile's priority can't be Unstored");
        } else if (zones_->createStockpile(create->area, create->priority) == kNoZone) {
            commandFailed(id, "no cell there can hold a stockpile");
        }
    } else if (const auto* remove = std::get_if<RemoveZoneCellsCommand>(&command)) {
        if (zones_->removeCells(remove->area) == 0) {
            commandFailed(id, "no zone there");
        }
    } else if (const auto* priority = std::get_if<SetStockpilePriorityCommand>(&command)) {
        if (!zones_->setPriority(priority->zone, priority->priority)) {
            commandFailed(id, zones_->find(priority->zone) ? "a stockpile's priority can't be Unstored" : "no such zone");
        }
    } else if (const auto* allowed = std::get_if<SetStockpileAllowedCommand>(&command)) {
        const DefId def = defs_->idOf("Things", allowed->def);
        if (!def.valid()) {
            commandFailed(id, "no Things def '" + allowed->def + "'");
        } else if (!zones_->setAllowed(allowed->zone, def, allowed->allowed)) {
            commandFailed(id, "no such zone");
        }
    } else if (const auto* haul = std::get_if<OrderHaulCommand>(&command)) {
        if (!jobs_->orderHaul(haul->pawn, haul->thing, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* control = std::get_if<ControlPawnCommand>(&command)) {
        entt::registry& reg = things_->registry();
        if (control->pawn != kNoEntity) {
            if (!things_->spawned(control->pawn) || !reg.all_of<Colonist, Worker, Mover>(control->pawn)) {
                commandFailed(id, "that isn't a colonist on the map");
                return;
            }
            if (health_->incapacitated(control->pawn)) {
                commandFailed(id, "that colonist is downed or dead");
                return;
            }
        }
        // Whoever was controlled before is let go where it stands, and its job system takes over.
        const std::vector<Entity> before(reg.view<PlayerControl>().begin(), reg.view<PlayerControl>().end());
        for (const Entity old : before) {
            reg.remove<PlayerControl>(old);
            // Hands-on work ends and what it carries is put down where it stands (U10).
            jobs_->interrupt(old, "let go by the player");
            jobs_->putDown(old);
        }
        if (control->pawn != kNoEntity) {
            // Its job ends (what it carries is put down) and the job system leaves it alone.
            jobs_->interrupt(control->pawn, "taken over by the player");
            Mover& mover = reg.get<Mover>(control->pawn);
            mover.path.clear();
            mover.next = 0;
            mover.vx = 0.0f;
            mover.vy = 0.0f;
            mover.gaveUp = false;
            reg.emplace<PlayerControl>(control->pawn);
        }
    } else if (const auto* hands = std::get_if<HandsOnCommand>(&command)) {
        entt::registry& reg = things_->registry();
        if (!things_->exists(hands->pawn) || !reg.all_of<PlayerControl, Worker, Position>(hands->pawn)) {
            commandFailed(id, "that pawn isn't under direct control");
        } else if (health_->incapacitated(hands->pawn)) {
            commandFailed(id, "that colonist is downed or dead");
        } else if (hands->mode == HandsOnMode::Stop) {
            jobs_->handsOnStop(hands->pawn);
        } else if (hands->mode == HandsOnMode::PutDown) {
            if (!jobs_->handsOnPutDown(hands->pawn, hands->cell, error)) {
                commandFailed(id, error);
            }
        } else if (!jobs_->handsOn(hands->pawn, hands->thing, hands->cell, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* flick = std::get_if<FlickCommand>(&command)) {
        if (!power_->flick(flick->thing, flick->on, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* circuit = std::get_if<ShortCircuitCommand>(&command)) {
        double released = 0.0;
        if (!power_->shortCircuit(circuit->thing, released, error)) {
            commandFailed(id, error);
        } else if (power_->info(things_->registry().get<Thing>(circuit->thing).def).transmitter) {
            // The conduit or switch burns out.
            combat_->dropGear(circuit->thing);
            explosives_->dropBelt(circuit->thing);
            jobs_->forget(circuit->thing);
            fighters_->forget(circuit->thing);
            things_->destroy(circuit->thing);
        }
    } else if (const auto* contract = std::get_if<ContractDiseaseCommand>(&command)) {
        const DefId disease = health_->diseaseId(contract->disease);
        if (!things_->exists(contract->pawn) || !health_->tracks(contract->pawn)) {
            commandFailed(id, "that thing has no body to fall ill");
        } else if (!disease.valid()) {
            commandFailed(id, "there is no disease '" + contract->disease + "'");
        } else if (!health_->contract(contract->pawn, disease, contract->severity, contract->part)) {
            commandFailed(id, "that pawn can't catch " + contract->disease + " now");
        }
    } else if (const auto* medical = std::get_if<SetMedicalCareCommand>(&command)) {
        MedicalCare care = MedicalCare::Best;
        if (!things_->exists(medical->pawn) || !health_->tracks(medical->pawn)) {
            commandFailed(id, "that thing has no body to care for");
        } else if (!medicalCareFromName(medical->care, care)) {
            commandFailed(id, "no medical care '" + medical->care + "' (NoCare, NoMedicine, Herbal, Normal or Best)");
        } else {
            things_->registry().emplace_or_replace<MedicalSettings>(medical->pawn, MedicalSettings{care});
        }
    } else if (const auto* prisonerMode = std::get_if<SetPrisonerModeCommand>(&command)) {
        PrisonerMode mode = PrisonerMode::NoInteraction;
        std::string why;
        if (!prisonerModeFromName(prisonerMode->mode, mode)) {
            commandFailed(id, "no prisoner mode '" + prisonerMode->mode + "' (NoInteraction, ReduceResistance, AttemptRecruit or Release)");
        } else if (!prisoners_->setMode(prisonerMode->pawn, mode, why)) {
            commandFailed(id, why);
        }
    } else if (const auto* prisonBed = std::get_if<SetPrisonBedCommand>(&command)) {
        std::string why;
        if (!prisoners_->setPrisonBed(prisonBed->bed, prisonBed->on, why)) {
            commandFailed(id, why);
        }
    } else if (const auto* throwCommand = std::get_if<ThrowCommand>(&command)) {
        DefId item;
        if (!throwCommand->item.empty()) {
            item = defs_->idOf("Things", throwCommand->item);
            if (!item.valid()) {
                commandFailed(id, "there is no such grenade");
                return;
            }
        }
        if (!explosives_->throwAt(throwCommand->pawn, item, throwCommand->target, throwCommand->x, throwCommand->y,
                                  throwCommand->player, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* explode = std::get_if<ExplodeCommand>(&command)) {
        const DefId type = defs_->idOf("DamageTypes", explode->damageType);
        const CellPos at{static_cast<int>(std::floor(explode->x)), static_cast<int>(std::floor(explode->y)), 0};
        if (!type.valid()) {
            commandFailed(id, "there is no such damage type");
        } else if (!map_.inBounds(at) || !(explode->radius > 0.0f) || !(explode->damage > 0.0f)) {
            commandFailed(id, "that is not a blast on the map");
        } else {
            Explosion blast;
            blast.x = explode->x;
            blast.y = explode->y;
            blast.z = map_.heightAt(explode->x, explode->y) + 0.04f;
            blast.radius = explode->radius;
            blast.damage = explode->damage;
            blast.damageType = type;
            blast.penetration = explode->penetration;
            blast.seed = splitMix64(settings_.seed ^ static_cast<uint64_t>(time_.seconds) ^ (id << 20));
            combat_->explode(blast);
        }
    } else if (const auto* setResearch = std::get_if<SetResearchCommand>(&command)) {
        if (!research_->setCurrent(setResearch->project, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* unlockResearch = std::get_if<UnlockResearchCommand>(&command)) {
        const bool wasDone = research_->isDone(unlockResearch->project);
        if (!research_->unlock(unlockResearch->project, error)) {
            commandFailed(id, error);
        } else if (!wasDone) {
            Event event;
            event.type = EventType::ResearchDone;
            event.def = defs_->idOf("Research", unlockResearch->project);
            event.message = unlockResearch->project;
            events_.push_back(std::move(event));
        }
    } else if (const auto* setWeather = std::get_if<SetWeatherCommand>(&command)) {
        if (!weather_->force(setWeather->weather, static_cast<double>(setWeather->hours))) {
            commandFailed(id, "there is no weather called " + setWeather->weather);
        }
    } else if (const auto* mark = std::get_if<MarkAnimalCommand>(&command)) {
        bool ok = false;
        if (mark->mark == "Tame") {
            ok = husbandry_->markTame(mark->animal, mark->on, error);
        } else if (mark->mark == "Slaughter") {
            ok = husbandry_->markSlaughter(mark->animal, mark->on, error);
        } else if (mark->mark == "Hunt") {
            ok = husbandry_->markHunt(mark->animal, mark->on, error);
        } else if (mark->mark == "Release") {
            ok = husbandry_->release(mark->animal, error);
        } else {
            error = "a mark is Tame, Slaughter, Hunt or Release";
        }
        if (!ok) {
            commandFailed(id, error);
        }
    } else if (const auto* markArea = std::get_if<MarkAnimalsInAreaCommand>(&command)) {
        if (!husbandry_->markInArea(markArea->area, markArea->mark, markArea->on, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* animalName = std::get_if<SetAnimalNameCommand>(&command)) {
        if (!husbandry_->setName(animalName->animal, animalName->name, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* training = std::get_if<SetTrainingCommand>(&command)) {
        Trainable what = Trainable::Obedience;
        if (!trainableFromName(training->trainable, what)) {
            commandFailed(id, "a trick is Obedience, Release, Rescue or Haul");
        } else if (!husbandry_->setTraining(training->animal, what, training->wanted, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* createAnimalZone = std::get_if<CreateAnimalZoneCommand>(&command)) {
        if (husbandry_->createZone(createAnimalZone->area) == kNoAnimalZone) {
            commandFailed(id, "no free walkable ground in that area for an animal zone");
        }
    } else if (const auto* removeAnimalZone = std::get_if<RemoveAnimalZoneCellsCommand>(&command)) {
        if (husbandry_->removeZoneCells(removeAnimalZone->area) == 0) {
            commandFailed(id, "no animal zone there");
        }
    } else if (const auto* assignAnimalZone = std::get_if<AssignAnimalZoneCommand>(&command)) {
        if (!husbandry_->assignZone(assignAnimalZone->animal, assignAnimalZone->zone, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* setBreeding = std::get_if<SetBreedingCommand>(&command)) {
        const bool ok = setBreeding->animal != kNoEntity ? breeding_->setBreeding(setBreeding->animal, setBreeding->allow, error)
                                                         : husbandry_->setZoneBreeding(setBreeding->zone, setBreeding->allow, error);
        if (!ok) {
            commandFailed(id, error);
        }
    } else if (const auto* createGrowing = std::get_if<CreateGrowingZoneCommand>(&command)) {
        const DefId crop = defs_->idOf("Things", createGrowing->crop);
        if (farming_->createZone(createGrowing->area, crop, error) == kNoGrowingZone) {
            commandFailed(id, error);
        }
    } else if (const auto* removeGrowing = std::get_if<RemoveGrowingZoneCellsCommand>(&command)) {
        if (farming_->removeCells(removeGrowing->area) == 0) {
            commandFailed(id, "no growing zone there");
        }
    } else if (const auto* setGrowingCrop = std::get_if<SetGrowingZoneCropCommand>(&command)) {
        if (!farming_->setCrop(setGrowingCrop->zone, defs_->idOf("Things", setGrowingCrop->crop), error)) {
            commandFailed(id, error);
        }
    } else if (const auto* setNarrator = std::get_if<SetNarratorCommand>(&command)) {
        if (!narrator_->setNarrator(setNarrator->narrator, setNarrator->difficulty, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* fireIncident = std::get_if<FireIncidentCommand>(&command)) {
        if (!narrator_->fire(fireIncident->incident, fireIncident->points, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* spendTalent = std::get_if<SpendTalentPointCommand>(&command)) {
        if (!talents_->spend(spendTalent->pawn, spendTalent->talent, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* ignite = std::get_if<IgniteCommand>(&command)) {
        if (ignite->pawn != kNoEntity) {
            if (!fire_->setPawnAlight(ignite->pawn, time_.seconds, ignite->force)) {
                commandFailed(id, "that pawn can't catch fire");
            }
        } else if (!fire_->ignite(ignite->cell, ignite->size, ignite->force)) {
            commandFailed(id, "nothing there can burn");
        }
    } else if (const auto* home = std::get_if<SetHomeAreaCommand>(&command)) {
        if (fire_->setHome(home->area, home->add) == 0) {
            commandFailed(id, "the home area is already so there");
        }
    } else if (const auto* rearm = std::get_if<RearmTrapCommand>(&command)) {
        if (!defense_->rearm(rearm->trap, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* carry = std::get_if<CarryWeaponCommand>(&command)) {
        if (!combat_->carryWeapon(carry->pawn, carry->item, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* switchWeapon = std::get_if<SwitchWeaponCommand>(&command)) {
        if (!combat_->switchWeapon(switchWeapon->pawn, switchWeapon->slot, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* talentXp = std::get_if<AddTalentXpCommand>(&command)) {
        if (!talents_->addXp(talentXp->pawn, talentXp->amount, events_, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* lineage = std::get_if<SetLineageCommand>(&command)) {
        PawnLooks* looks = things_->exists(lineage->pawn) ? things_->registry().try_get<PawnLooks>(lineage->pawn) : nullptr;
        if (looks == nullptr) {
            commandFailed(id, "that thing has no looks to change");
        } else if (!setLineage(*defs_, lineage->lineage, *looks, error)) {
            commandFailed(id, error);
        } else {
            giveGenes(lineage->pawn);
        }
    } else if (const auto* move = std::get_if<DirectMoveCommand>(&command)) {
        entt::registry& reg = things_->registry();
        if (!std::isfinite(move->x) || !std::isfinite(move->y)) {
            commandFailed(id, "that position isn't a number");
        } else if (!things_->exists(move->pawn) || !reg.all_of<PlayerControl, Position, Mover>(move->pawn)) {
            commandFailed(id, "that pawn isn't under direct control");
        } else if (health_->incapacitated(move->pawn)) {
            commandFailed(id, "that colonist is downed or dead");
        } else if (combat_->rollLocked(move->pawn)) {
            // Rolling (P2): the sim moves the pawn; the player's position is ignored, without a failure
            // because Unreal sends one every step.
        } else {
            Position& pos = reg.get<Position>(move->pawn);
            Mover& mover = reg.get<Mover>(move->pawn);
            float dx = move->x - pos.x;
            float dy = move->y - pos.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length > kDirectMaxStep) {
                dx *= kDirectMaxStep / length;
                dy *= kDirectMaxStep / length;
            }
            // The whole move, or sliding along one axis (a wall), or staying where it is.
            const float tries[3][2] = {{dx, dy}, {dx, 0.0f}, {0.0f, dy}};
            float moveX = 0.0f;
            float moveY = 0.0f;
            for (const auto& t : tries) {
                if ((t[0] != 0.0f || t[1] != 0.0f) && pawnFitsAt(map_, pos.x + t[0], pos.y + t[1], kPawnRadius, pos.level)) {
                    moveX = t[0];
                    moveY = t[1];
                    break;
                }
            }
            pos.x += moveX;
            pos.y += moveY;
            mover.vx = moveX;
            mover.vy = moveY;
        }
    } else if (const auto* roll = std::get_if<DodgeRollCommand>(&command)) {
        if (!combat_->dodgeRoll(roll->pawn, roll->dx, roll->dy, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* schedule = std::get_if<SetScheduleCommand>(&command)) {
        if (!jobs_->setSchedule(schedule->pawn, schedule->hour, schedule->kind, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* assign = std::get_if<AssignBedCommand>(&command)) {
        if (!jobs_->assignBed(assign->bed, assign->pawn, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* place = std::get_if<PlaceBlueprintCommand>(&command)) {
        const DefId def = defs_->idOf("Things", place->def);
        const DefId stuff = place->stuff.empty() ? DefId{} : defs_->idOf("Things", place->stuff);
        if (!def.valid()) {
            commandFailed(id, "no Things def '" + place->def + "'");
        } else if (!place->stuff.empty() && !stuff.valid()) {
            commandFailed(id, "no Things def '" + place->stuff + "'");
        } else if (!research_->unlocked(defs_->get(def), error)) {
            commandFailed(id, error);
        } else if (construction_->placeBlueprint(def, stuff, place->cell, place->rot, error) == kNoEntity) {
            commandFailed(id, error);
        }
    } else if (const auto* cancel = std::get_if<CancelBuildCommand>(&command)) {
        if (!construction_->cancel(cancel->thing, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* deconstruct = std::get_if<DeconstructCommand>(&command)) {
        if (!construction_->markDeconstruct(deconstruct->thing, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* equip = std::get_if<EquipCommand>(&command)) {
        const bool grenade = things_->registry().valid(equip->item) && things_->registry().all_of<Thing>(equip->item) &&
                             explosives_->isThrown(things_->registry().get<Thing>(equip->item).def);
        if (simJobUses(*jobs_, things_->registry(), equip->item)) {
            commandFailed(id, "a job is using that thing");
        } else if (grenade ? !explosives_->take(equip->pawn, equip->item, &error)
                           : !combat_->equip(equip->pawn, equip->item, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* shoot = std::get_if<ShootCommand>(&command)) {
        if (explosives_->throwing(shoot->pawn)) {
            commandFailed(id, "that pawn is throwing");
        } else if (!combat_->shoot(shoot->pawn, shoot->target, shoot->x, shoot->y, shoot->z, shoot->player, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* stance = std::get_if<SetStanceCommand>(&command)) {
        if (static_cast<int>(stance->stance) > static_cast<int>(Stance::Prone) ||
            !combat_->setStance(stance->pawn, stance->stance)) {
            commandFailed(id, "that is not a pawn");
        }
    } else if (const auto* designate = std::get_if<DesignateCommand>(&command)) {
        const DesignationKind kind = designate->kind;
        const CellRect& area = designate->area;
        int marked = 0;
        if (kind == DesignationKind::None) {
            commandFailed(id, "Designate needs Mine, CutPlant or HarvestPlant (CancelDesignations takes marks off)");
            return;
        }
        for (int y = std::max(0, area.minY); y < std::min(map_.height(), area.minY + area.sizeY); ++y) {
            for (int x = std::max(0, area.minX); x < std::min(map_.width(), area.minX + area.sizeX); ++x) {
                const CellPos c{x, y, area.level};
                if (!map_.inBounds(c)) {
                    continue;
                }
                bool suits = false;
                if (kind == DesignationKind::Mine) {
                    suits = mineables_->has(c);
                } else if (plants_->has(c)) {
                    suits = kind == DesignationKind::CutPlant || defs_->get(plants_->defAt(c)).has("plant.harvestedThingDef");
                }
                if (suits) {
                    designations_->set(c, kind);
                    ++marked;
                }
            }
        }
        if (marked == 0) {
            commandFailed(id, kind == DesignationKind::Mine ? "there is no rock to mine there"
                                                            : (kind == DesignationKind::CutPlant ? "there is no plant to cut there"
                                                                                                 : "there is nothing to harvest there"));
        } else {
            Event changed;
            changed.type = EventType::DesignationsChanged;
            events_.push_back(std::move(changed));
        }
    } else if (const auto* cancelMarks = std::get_if<CancelDesignationsCommand>(&command)) {
        std::vector<int> cells;
        for (const auto& [index, kind] : designations_->all()) {
            if ((cancelMarks->kind == DesignationKind::None || cancelMarks->kind == kind) && cancelMarks->area.contains(map_.cellAt(index))) {
                cells.push_back(index);
            }
        }
        for (const int index : cells) {
            designations_->set(map_.cellAt(index), DesignationKind::None);
            jobs_->cancelGatherAt(map_.cellAt(index));
        }
        if (cells.empty()) {
            commandFailed(id, "there is nothing marked there");
        } else {
            Event changed;
            changed.type = EventType::DesignationsChanged;
            events_.push_back(std::move(changed));
        }
    } else if (const auto* draft = std::get_if<DraftCommand>(&command)) {
        if (!fighters_->draft(draft->pawn, draft->drafted, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* moveOrder = std::get_if<MoveOrderCommand>(&command)) {
        if (!fighters_->orderMove(moveOrder->pawn, moveOrder->cell, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* attack = std::get_if<AttackOrderCommand>(&command)) {
        if (!fighters_->orderAttack(attack->pawn, attack->target, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* faction = std::get_if<SetFactionCommand>(&command)) {
        if (!fighters_->setFaction(faction->pawn, faction->faction, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* reload = std::get_if<ReloadCommand>(&command)) {
        if (!combat_->reload(reload->pawn, &error)) {
            commandFailed(id, error);
        }
    } else if (const auto* addBill = std::get_if<AddBillCommand>(&command)) {
        const Def* recipeDef = defs_->find("Recipes", addBill->recipe);
        if (recipeDef != nullptr && !research_->unlocked(*recipeDef, error)) {
            commandFailed(id, error);
        } else if (jobs_->bills().addBill(addBill->building, addBill->recipe, addBill->mode, addBill->count, error) == 0) {
            commandFailed(id, error);
        }
    } else if (const auto* removeBill = std::get_if<RemoveBillCommand>(&command)) {
        if (!jobs_->bills().removeBill(removeBill->building, removeBill->bill, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* melee = std::get_if<MeleeCommand>(&command)) {
        const bool done = melee->mode == MeleeMode::Block
                              ? melee_->block(melee->pawn, &error)
                              : melee_->attack(melee->pawn, melee->target,
                                               things_->registry().valid(melee->pawn) &&
                                                   things_->registry().all_of<PlayerControl>(melee->pawn),
                                               &error);
        if (!done) {
            commandFailed(id, error);
        }
    } else if (const auto* billOptions = std::get_if<SetBillOptionsCommand>(&command)) {
        BillSystem::Options options;
        options.skillMin = billOptions->skillMin;
        options.skillMax = billOptions->skillMax;
        options.radius = billOptions->radius;
        options.storeZone = billOptions->storeZone;
        options.unpauseAt = billOptions->unpauseAt;
        options.disallowed = billOptions->disallowed;
        if (billOptions->storeZone != 0 && zones_->find(billOptions->storeZone) == nullptr) {
            commandFailed(id, "there is no stockpile " + std::to_string(billOptions->storeZone));
        } else if (!jobs_->bills().setOptions(billOptions->building, billOptions->bill, options, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* moveBill = std::get_if<MoveBillCommand>(&command)) {
        if (!jobs_->bills().moveBill(moveBill->building, moveBill->bill, moveBill->index, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* setBill = std::get_if<SetBillCommand>(&command)) {
        if (!jobs_->bills().setBill(setBill->building, setBill->bill, setBill->mode, setBill->count, setBill->suspended,
                                    error)) {
            commandFailed(id, error);
        }
    } else if (const auto* placeFloor = std::get_if<PlaceFloorCommand>(&command)) {
        const DefId floor = placeFloor->floor.empty() ? DefId{} : defs_->idOf("Terrain", placeFloor->floor);
        if (!placeFloor->floor.empty() && !floors_->isFloorDef(floor)) {
            commandFailed(id, "no floor '" + placeFloor->floor + "'");
        } else {
            int changed = 0;
            for (int y = placeFloor->area.minY; y < placeFloor->area.minY + placeFloor->area.sizeY; ++y) {
                for (int x = placeFloor->area.minX; x < placeFloor->area.minX + placeFloor->area.sizeX; ++x) {
                    const CellPos cell{x, y, placeFloor->area.level};
                    const DefId before = floors_->at(cell);
                    if (floors_->set(cell, floor) && floors_->at(cell) != before) {
                        ++changed;
                    }
                }
            }
            if (changed == 0) {
                commandFailed(id, "no floor changed there");
            }
        }
    } else if (const auto* addFilth = std::get_if<AddFilthCommand>(&command)) {
        if (!filth_->add(addFilth->cell, addFilth->kind, addFilth->layers)) {
            commandFailed(id, "no filth could be added there");
        }
    } else if (const auto* raid = std::get_if<StartRaidCommand>(&command)) {
        if (!raids_->startRaid(raid->faction, raid->points, raid->edge, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* alarm = std::get_if<SoundAlarmCommand>(&command)) {
        if (!raids_->soundAlarm(alarm->on, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* role = std::get_if<SetDefenseRoleCommand>(&command)) {
        if (role->role > static_cast<uint8_t>(DefenseRole::KeepWorking)) {
            commandFailed(id, "no such defense role");
        } else if (!raids_->setRole(role->pawn, static_cast<DefenseRole>(role->role), role->hasSpot, role->spot, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* shelter = std::get_if<MarkShelterCommand>(&command)) {
        if (!raids_->markShelter(shelter->cell, shelter->clear, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* setOutfit = std::get_if<SetOutfitCommand>(&command)) {
        if (!apparel_->setOutfit(setOutfit->pawn, setOutfit->outfit, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* editOutfit = std::get_if<EditOutfitCommand>(&command)) {
        if (!apparel_->editOutfit(editOutfit->outfit, editOutfit->disallowed, editOutfit->hitPointsMin,
                                  editOutfit->hitPointsMax, editOutfit->qualityMin, editOutfit->qualityMax, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* addOutfit = std::get_if<AddOutfitCommand>(&command)) {
        if (apparel_->addOutfit(addOutfit->name, error) == 0) {
            commandFailed(id, error);
        }
    } else if (const auto* removeOutfit = std::get_if<RemoveOutfitCommand>(&command)) {
        if (!apparel_->removeOutfit(removeOutfit->outfit, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* strip = std::get_if<StripCommand>(&command)) {
        if (!apparel_->markStrip(strip->thing, strip->strip, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* interact = std::get_if<InteractCommand>(&command)) {
        if (!social_->interact(interact->initiator, interact->recipient, interact->interaction, interact->outcome, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* upper = std::get_if<PlaceUpperFloorCommand>(&command)) {
        int changed = 0;
        std::string why;
        for (int y = upper->area.minY; y < upper->area.minY + upper->area.sizeY; ++y) {
            for (int x = upper->area.minX; x < upper->area.minX + upper->area.sizeX; ++x) {
                const CellPos cell{x, y, upper->area.level};
                const bool before = map_.upperFloor(cell);
                const bool done = upper->remove ? upperFloors_->remove(cell, &why) : upperFloors_->lay(cell, &why);
                if (done && map_.upperFloor(cell) != before) {
                    ++changed;
                }
            }
        }
        if (changed == 0) {
            commandFailed(id, why.empty() ? std::string("no upper floor changed there") : why);
        }
    } else if (const auto* refuel = std::get_if<SetRefuelCommand>(&command)) {
        if (!jobs_->fuel().setRefuel(refuel->building, refuel->autoRefuel, refuel->fillNow, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* startBreak = std::get_if<StartMentalBreakCommand>(&command)) {
        if (!mentalBreaks_->start(startBreak->pawn, startBreak->breakDef, rng_, error)) {
            commandFailed(id, error);
        }
    } else if (const auto* endBreak = std::get_if<EndMentalBreakCommand>(&command)) {
        if (!mentalBreaks_->end(endBreak->pawn, error)) {
            commandFailed(id, error);
        }
    }
}

void Sim::step() {
    SimLap total(profile_, "total");
    if (profile_ != nullptr) {
        ++profile_->steps;
    }
    events_.clear();
    // A controlled pawn only has a velocity in the steps it was moved.
    for (const auto [pawn, mover] : things_->registry().view<Mover, const PlayerControl>().each()) {
        mover.vx = 0.0f;
        mover.vy = 0.0f;
    }
    // Commands first, in submit order. Swap out the queue so its memory is reused.
    std::vector<std::pair<uint64_t, Command>> applying;
    applying.swap(pending_);
    for (auto& [id, command] : applying) {
        applyCommand(command, id);
        commandLog_.push_back(LoggedCommand{time_.seconds, std::move(command)});
    }
    applying.clear();
    pending_.swap(applying);

    temperature_->setOutdoor(outdoorTemperature());
    { SimLap lap(profile_, "health"); health_->step(time_.seconds, settings_.seed); }
    { SimLap lap(profile_, "needs"); needs_->step(); }
    // A controlled colonist who goes down or dies is let go: the player can't walk it any more.
    {
        entt::registry& reg = things_->registry();
        const std::vector<Entity> controlled(reg.view<PlayerControl>().begin(), reg.view<PlayerControl>().end());
        for (const Entity pawn : controlled) {
            if (health_->incapacitated(pawn)) {
                reg.remove<PlayerControl>(pawn);
            }
        }
    }
    const size_t eventsBefore = events_.size();
    { SimLap lap(profile_, "jobs"); jobs_->fuel().step(); }
    { SimLap lap(profile_, "jobs"); jobs_->step(); }
    { SimLap lap(profile_, "raids"); raids_->step(); }
    {
        std::vector<Event> gained;
        talents_->awardJobXp(events_, eventsBefore, gained);
        events_.insert(events_.end(), gained.begin(), gained.end());
    }
    { SimLap lap(profile_, "fighters"); fighters_->step(); }
    { SimLap lap(profile_, "prisoners"); prisoners_->step(); }
    { SimLap lap(profile_, "recreation"); recreation_->step(); }
    { SimLap lap(profile_, "animals"); animals_->step(); }
    { SimLap lap(profile_, "husbandry"); husbandry_->step(); }
    { SimLap lap(profile_, "breeding"); breeding_->step(); }
    { SimLap lap(profile_, "social"); social_->step(time_.seconds); }
    { SimLap lap(profile_, "mentalBreaks"); mentalBreaks_->step(time_.seconds, rng_); }  // before the mood system, which turns its events into memories
    { SimLap lap(profile_, "mood"); mood_->step(time_.seconds, events_); }
    { SimLap lap(profile_, "inspirations"); inspirations_->step(time_.seconds); }
    { SimLap lap(profile_, "movement"); movement_->step(things_->registry(), map_); }
    { SimLap lap(profile_, "melee"); melee_->step(); }
    { SimLap lap(profile_, "combat"); combat_->step(rng_); }
    { SimLap lap(profile_, "explosives"); explosives_->step(rng_); }
    animals_->scare(events_, eventsBefore);  // shots and blows this step frighten the animals near them (N5)
    { SimLap lap(profile_, "defense"); defense_->step(rng_); }
    {
        // Blasts of Burn damage start fires; a pawn that catches fire stops what it was doing.
        fire_->step(time_.seconds, 0);
        for (size_t i = 0; i < events_.size(); ++i) {
            if (events_[i].type == EventType::PawnCaughtFire) {
                jobs_->interrupt(events_[i].thing, "it caught fire");
            }
        }
    }
    { SimLap lap(profile_, "apparel"); apparel_->step(time_.seconds, rng_, events_); }
    { SimLap lap(profile_, "pathfinder"); pathfinder_.update(settings_.pathBudget); }  // paths asked for this step are ready next step at the earliest
    rooms_->update();
    { SimLap lap(profile_, "roofCollapse"); roofCollapse_->step(rng_); }  // roofs and floors that lost their holders fall
    { SimLap lap(profile_, "power"); power_->step(time_); }
    lights_->update();
    { SimLap lap(profile_, "filth"); filth_->step(); }
    { SimLap lap(profile_, "roomStats"); roomStats_->step(time_.seconds); }
    { SimLap lap(profile_, "temperature"); temperature_->step(time_.seconds); }
    { SimLap lap(profile_, "spoilage"); spoilage_->step(time_.seconds); }
    if (time_.seconds % kBillUpkeepSeconds == 0) {
        jobs_->bills().step();
        crafting_->step();
    }
    time_.seconds += 1;
    { SimLap lap(profile_, "weather"); weather_->step(time_.seconds); }
    spoilage_->setOutdoorFactor(weather_->deteriorationFactor());
    if (time_.seconds % kSecondsPerDay == 0) {
        jobs_->skillsNewDay();
    }
    if (time_.seconds % kSecondsPerHour == 0) {
        SimLap lap(profile_, "hourly");
        plants_->growHour(static_cast<int>(((time_.seconds - 1) % kSecondsPerDay) / kSecondsPerHour));
        farming_->growHour(time_.seconds);
        filth_->fadeHour(weather_->effects().rainRate);
        pruneDesignations();
    }
    narrator_->step();  // after the clock: incidents come on the narrator's checks (T1)
    if (settings_.checkpointEvery > 0 && time_.seconds % settings_.checkpointEvery == 0) {
        checkpoints_.push_back(Checkpoint{time_.seconds, stateHash()});
    }
}

// The open air's temperature is E1's: the biome's season, the hour and a slow random drift.
float Sim::outdoorTemperature() const {
    return outdoorOverride_ ? *outdoorOverride_ : outdoorTemperatureAt(time_.seconds);
}

float Sim::outdoorTemperatureAt(int64_t gameSeconds) const {
    return static_cast<float>(weather_->temperatureAt(gameSeconds));
}

void Sim::setOutdoorOverride(std::optional<float> celsius) {
    outdoorOverride_ = celsius;
    temperature_->setOutdoor(outdoorTemperature());
    temperature_->setGround(groundTemperature());
}

float Sim::groundTemperature() const {
    return outdoorOverride_ ? *outdoorOverride_ : static_cast<float>(weather_->yearMeanTemperature());
}

// Marks whose rock or plant is gone (it died of old age): taken off, so they don't pile up.
void Sim::pruneDesignations() {
    std::vector<int> stale;
    for (const auto& [index, kind] : designations_->all()) {
        const CellPos c = map_.cellAt(index);
        if (kind == DesignationKind::Mine ? !mineables_->has(c) : !plants_->has(c)) {
            stale.push_back(index);
        }
    }
    for (const int index : stale) {
        designations_->set(map_.cellAt(index), DesignationKind::None);
    }
    if (!stale.empty()) {
        Event changed;
        changed.type = EventType::DesignationsChanged;
        events_.push_back(std::move(changed));
    }
}

void Sim::writeSnapshot(Snapshot& out) const {
    out.gameSeconds = time_.seconds;
    out.mapWidth = map_.width();
    out.mapHeight = map_.height();
    out.things.clear();
    out.waypoints.clear();
    out.bills.clear();
    out.zones.clear();
    out.zoneCells.clear();
    out.animalZones.clear();
    out.growingZones.clear();
    out.growingZoneCells.clear();
    out.animalZoneCells.clear();
    out.looks.clear();
    out.research.clear();
    out.talents.clear();
    for (const auto& [pawn, state] : talents_->states()) {
        TalentView view;
        view.pawn = pawn;
        view.xp = state.xp;
        const int earned = talents_->pointsEarned(pawn);
        view.xpForNextPoint = earned < talents_->rules().maxPoints ? talents_->xpForPoints(earned + 1) : 0;
        view.pointsFree = talents_->pointsFree(pawn);
        for (const TalentDef& talent : talents_->talents()) {
            if (const int rank = talents_->rank(pawn, talent.defName)) {
                view.ranks.push_back(TalentRankView{talent.def, rank});
            }
        }
        out.talents.push_back(std::move(view));
    }
    for (const ResearchProject& project : research_->projects()) {
        ResearchView view;
        view.project = project.def;
        view.progress = research_->progress(project.defName);
        view.cost = research_->cost(project);
        view.done = research_->isDone(project.defName);
        view.available = research_->available(project);
        view.current = research_->current() == project.defName;
        out.research.push_back(view);
    }
    out.apparel.clear();
    out.outfits.clear();
    for (const OutfitPolicy& outfit : apparel_->outfits()) {
        out.outfits.push_back(OutfitView{outfit.id, outfit.name});
    }
    out.designations.clear();
    for (const auto& [index, kind] : designations_->all()) {
        out.designations.push_back(DesignationView{map_.cellAt(index), kind});
    }
    for (const StockpileZone& zone : zones_->stockpiles()) {
        out.zones.push_back(ZoneView{zone.id, zone.priority, static_cast<uint32_t>(out.zoneCells.size()),
                                     static_cast<uint32_t>(zone.cells.size())});
        out.zoneCells.insert(out.zoneCells.end(), zone.cells.begin(), zone.cells.end());
    }
    for (const AnimalZone& zone : husbandry_->zones()) {
        out.animalZones.push_back(AnimalZoneView{zone.id, static_cast<uint32_t>(out.animalZoneCells.size()),
                                                 static_cast<uint32_t>(zone.cells.size()), husbandry_->isPen(zone.id)});
        out.animalZoneCells.insert(out.animalZoneCells.end(), zone.cells.begin(), zone.cells.end());
    }
    for (const GrowingZone& zone : farming_->zones()) {
        out.growingZones.push_back(GrowingZoneView{zone.id, zone.crop, static_cast<uint32_t>(out.growingZoneCells.size()),
                                                   static_cast<uint32_t>(zone.cells.size())});
        out.growingZoneCells.insert(out.growingZoneCells.end(), zone.cells.begin(), zone.cells.end());
    }
    const entt::registry& reg = things_->registry();
    for (const auto [entity, thing, pos] : reg.view<const Thing, const Position>().each()) {
        ThingView view;
        view.id = entity;
        view.def = thing.def;
        view.category = thing.category;
        view.x = pos.x;
        view.y = pos.y;
        view.z = map_.heightAt(pos.x, pos.y, pos.level);
        view.level = pos.level;
        if (const Mover* mover = reg.try_get<Mover>(entity)) {
            view.vx = mover->vx;
            view.vy = mover->vy;
            // On stairs or a ladder the pawn is drawn the share `climb` of the way from where it
            // stands to the cell at the other end (docs/design/levels.md).
            if (mover->climbing(pos.level)) {
                const CellPos to = mover->path[mover->next];
                if (const LevelLink* link = map_.levelLinkBetween(cellOf(pos), to)) {
                    const float t = std::clamp(mover->climbed / link->length, 0.0f, 1.0f);
                    const float tx = static_cast<float>(to.x) + 0.5f;
                    const float ty = static_cast<float>(to.y) + 0.5f;
                    view.climb = t;
                    view.x = pos.x + (tx - pos.x) * t;
                    view.y = pos.y + (ty - pos.y) * t;
                    view.z += (map_.heightAt(tx, ty, to.level) - view.z) * t;
                }
            }
            view.firstWaypoint = static_cast<uint32_t>(out.waypoints.size());
            for (size_t k = mover->next; k < mover->path.size(); ++k) {
                out.waypoints.push_back(PathPointView{static_cast<float>(mover->path[k].x) + 0.5f,
                                                      static_cast<float>(mover->path[k].y) + 0.5f});
            }
            view.waypointCount = static_cast<uint32_t>(out.waypoints.size()) - view.firstWaypoint;
        }
        if (const Footprint* footprint = reg.try_get<Footprint>(entity)) {
            const CellRect rect = footprint->rect();
            view.rotation = footprint->rotation;
            view.sizeX = rect.sizeX;
            view.sizeY = rect.sizeY;
            view.markedForDeconstruct = reg.all_of<MarkedForDeconstruct>(entity);
            if (const Door* door = reg.try_get<Door>(entity)) {
                view.doorOpen = door->openFraction(doorNumbers_);
            }
            if (const PowerPart* part = reg.try_get<PowerPart>(entity)) {
                view.power = power_->stateOf(entity);
                view.powerNet = part->net;
                view.powerWatts = static_cast<float>(part->watts);
                const double capacity = power_->capacityOf(entity);
                view.batteryFill = capacity > 0.0 ? static_cast<float>(part->stored / capacity) : -1.0f;
            }
        }
        if (const ConstructionSite* site = reg.try_get<ConstructionSite>(entity)) {
            const CellRect rect = site->rect();
            view.rotation = site->rotation;
            view.sizeX = rect.sizeX;
            view.sizeY = rect.sizeY;
            view.stage = site->stage;
            view.stuff = site->stuff;
            view.buildProgress = site->workTotal > 0.0f ? std::min(1.0f, site->workDone / site->workTotal) : 0.0f;
            view.materialsHave = site->materialsHave();
            view.materialsNeeded = site->materialsNeeded();
        } else if (const BuiltOf* made = reg.try_get<BuiltOf>(entity)) {
            view.stuff = made->stuff;
        }
        if (const BillList* list = reg.try_get<BillList>(entity)) {
            view.firstBill = static_cast<uint32_t>(out.bills.size());
            for (const Bill& bill : list->bills) {
                out.bills.push_back(BillView{bill.id, bill.recipe, bill.mode, bill.count, bill.suspended, bill.skillMin, bill.skillMax,
                                             bill.radius, bill.storeZone, bill.unpauseAt, bill.satisfied, bill.disallowed});
            }
            view.billCount = static_cast<uint32_t>(out.bills.size()) - view.firstBill;
        }
        if (const Fuel* fuel = reg.try_get<Fuel>(entity)) {
            const FuelSpec& spec = jobs_->fuel().specOf(entity);
            view.fuel = fuel->amount;
            view.fuelCapacity = spec.capacity;
            view.autoRefuel = fuel->autoRefuel;
            view.fillOrdered = fuel->fillOrdered;
        }
        if (const StackCount* stack = reg.try_get<StackCount>(entity)) {
            view.stackCount = stack->count;
        }
        if (const Spoilage* spoilage = reg.try_get<Spoilage>(entity)) {
            view.rot = spoilage->rot;
        }
        if (const HitPoints* hp = reg.try_get<HitPoints>(entity)) {
            view.hitPoints = hp->current;
            view.maxHitPoints = hp->max;
        }
        if (const Health* health = reg.try_get<Health>(entity)) {
            view.downed = health->downed;
            view.dead = health->dead;
        }
        if (const PawnLooks* looks = reg.try_get<PawnLooks>(entity)) {
            PawnLooksView l;
            l.lineage = defs_->idOf("Lineages", looks->lineage);
            l.female = looks->female;
            l.body = looks->body;
            l.weight = looks->weight;
            l.height = looks->height;
            l.skin = looks->skin;
            l.hair = looks->hair;
            view.looks = static_cast<int32_t>(out.looks.size());
            out.looks.push_back(l);
        }
        if (const Worker* worker = reg.try_get<Worker>(entity)) {
            view.job = worker->job.type;
            view.jobTarget = worker->job.thing;
            view.jobProgress = worker->job.type != JobType::None ? jobs_->jobProgress(entity) : 0.0f;
        }
        if (const Needs* needs = reg.try_get<Needs>(entity)) {
            view.food = needs->food;
            view.rest = needs->rest;
        }
        if (const Joy* joy = reg.try_get<Joy>(entity)) {
            view.joy = joy->level;
            view.comfort = joy->comfort;
            view.outdoors = joy->outdoors;
        }
        if (const Mood* mood = reg.try_get<Mood>(entity)) {
            view.mood = mood->level;
            view.moodBand = static_cast<int>(mood->band);
        }
        if (reg.all_of<Social>(entity)) {
            view.partner = social_->partner(entity);
            view.socialFight = social_->fighting(entity);
        }
        if (const BeautyNeed* beauty = roomStats_->beautyNeed(entity)) {
            view.beautyNeed = beauty->level;
        }
        if (const Worker* worker = reg.try_get<Worker>(entity)) {
            view.asleep = jobIsLyingDown(worker->job);
        }
        if (const MentalBreaker* breaker = reg.try_get<MentalBreaker>(entity); breaker != nullptr && breaker->active) {
            view.mentalBreak = defs_->idOf("MentalBreaks", breaker->def);
        }
        if (const Carrying* carrying = reg.try_get<Carrying>(entity); carrying && reg.valid(carrying->thing)) {
            view.carriedDef = reg.get<Thing>(carrying->thing).def;
            view.carriedCount = reg.get<StackCount>(carrying->thing).count;
        }
        if (const Posture* posture = reg.try_get<Posture>(entity)) {
            view.stance = view.downed || view.dead ? Stance::Prone : posture->stance;
            view.facingX = posture->facingX;
            view.facingY = posture->facingY;
            view.shotReadyIn = combat_->shotReadyIn(entity);
            view.rounds = combat_->roundsLeft(entity);
            view.magazineSize = combat_->magazineSize(entity);
            view.reloading = combat_->reloading(entity);
            combat_->kickOf(entity, view.kickUp, view.kickSide);
            view.rollPhase = static_cast<uint8_t>(combat_->rollPhase(entity));
            if (view.rollPhase != 0) {
                view.rollStepsLeft = combat_->rollStepsLeft(entity);
                combat_->rollDirection(entity, view.rollDirX, view.rollDirY);
            }
            view.hitProof = combat_->hitProof(entity);
            view.grenades = explosives_->count(entity);
            view.throwReadyIn = explosives_->throwReadyIn(entity);
            view.knockedDown = combat_->knockedDownFor(entity) > 0;
        }
        if (const Gear* gear = reg.try_get<Gear>(entity); gear != nullptr && gear->weapon != kNoEntity) {
            view.weapon = reg.get<Thing>(gear->weapon).def;
        }
        if (const Gear* gear = reg.try_get<Gear>(entity); gear != nullptr && !(gear->weapon == kNoEntity && gear->stowed.empty())) {
            for (int slot = 0; slot < kWeaponSlotCount; ++slot) {
                const Entity item = combat_->weaponIn(entity, static_cast<WeaponSlot>(slot));
                if (item != kNoEntity) {
                    view.carried[slot] = reg.get<Thing>(item).def;
                }
            }
            WeaponSlot target = WeaponSlot::Primary;
            if (combat_->switchTarget(entity, target)) {
                view.switchSlot = static_cast<uint8_t>(target);
                view.switchStepsLeft = combat_->switchStepsLeft(entity);
            }
        }
        if (const Gear* gear = reg.try_get<Gear>(entity)) {
            view.firstApparel = static_cast<uint32_t>(out.apparel.size());
            for (const Entity piece : gear->apparel) {
                ApparelView worn;
                worn.item = piece;
                worn.def = reg.get<Thing>(piece).def;
                if (const MadeOf* made = reg.try_get<MadeOf>(piece)) {
                    worn.stuff = made->stuff;
                }
                worn.quality = apparel_->stats().qualityOf(reg, piece);
                worn.hitPointShare = ApparelStats::hitPointShare(reg, piece);
                worn.tainted = reg.all_of<Tainted>(piece);
                out.apparel.push_back(worn);
            }
            view.apparelCount = static_cast<uint32_t>(out.apparel.size()) - view.firstApparel;
        }
        if (view.category == ThingCategory::Pawn) {
            view.outfit = apparel_->outfitOf(entity);
            view.markedForStrip = apparel_->stripMarked(entity);
        } else if (view.category == ThingCategory::Item && apparel_->stats().isApparel(thing.def)) {
            if (const MadeOf* made = reg.try_get<MadeOf>(entity)) {
                view.stuff = made->stuff;
            }
            view.quality = apparel_->stats().qualityOf(reg, entity);
            view.tainted = reg.all_of<Tainted>(entity);
        } else if (view.category == ThingCategory::Item && crafting_->hasQuality(thing.def)) {
            // Weapons and other crafted things with a quality: made of a stuff and of a quality like clothes.
            if (const MadeOf* made = reg.try_get<MadeOf>(entity)) {
                view.stuff = made->stuff;
            }
            view.quality = apparel_->stats().qualityOf(reg, entity);
        }
        if (view.category == ThingCategory::Pawn) {
            view.faction = fighters_->factionOf(entity);
        }
        if (reg.all_of<Animal>(entity)) {
            view.lifeStage = animals_->lifeStage(entity);
            view.bodySize = animals_->bodySize(entity);
            view.food = animals_->foodShare(entity);
            view.asleep = animals_->asleep(entity);
            view.animalTemper = static_cast<int>(reg.get<Animal>(entity).temper);
            view.animalMode = static_cast<int>(reg.get<Animal>(entity).mode);
            if (const AnimalCare* care = husbandry_->care(entity)) {
                view.markTame = care->markTame;
                view.markSlaughter = care->markSlaughter;
                view.markHunt = care->markHunt;
                view.animalZone = care->zone;
                view.animalName = care->name;
                view.learnedTricks = husbandry_->learnedMask(entity);
                view.wantedTricks = husbandry_->wantedMask(entity);
            }
            view.sex = static_cast<int>(breeding_->sexOf(entity));
            view.pregnant = breeding_->pregnant(entity);
            view.milkFullness = breeding_->fullness(entity, AnimalProduct::Milk);
            view.woolFullness = breeding_->fullness(entity, AnimalProduct::Wool);
        }
        view.onFire = reg.all_of<Burning>(entity);
        if (const Fighter* fighter = reg.try_get<Fighter>(entity)) {
            view.drafted = fighter->drafted;
            view.fighting = true;
            view.fightTarget = fighter->target;
            view.fightSpot = fighter->hasSpot ? fighter->spot : cellOf(pos);
            view.fallingBack = fighter->fallingBack;
            view.defending = fighter->defending;
            view.sheltering = fighter->mode == FightMode::Shelter;
            view.fleeing = fighter->mode == FightMode::Flee;
        }
        if (reg.all_of<Colonist>(entity) && view.faction == kPlayerFaction) {
            view.defenseRole = static_cast<uint8_t>(raids_->roleOf(entity));
        }
        if (const Raider* raider = reg.try_get<Raider>(entity)) {
            view.squad = raider->squad;
        }
        if (const Prisoner* prisoner = reg.try_get<Prisoner>(entity)) {
            view.prisoner = true;
            view.prisonerMode = static_cast<uint8_t>(prisoner->mode);
            view.resistance = prisoner->resistance;
        }
        view.prisonBed = reg.all_of<PrisonBed>(entity);
        if (const Melee* melee = reg.try_get<Melee>(entity)) {
            view.meleePhase = melee->phase;
            view.meleeStepsLeft = melee->stepsLeft;
            view.meleeTarget = melee->phase == MeleePhase::Windup ? melee->target : kNoEntity;
            view.blocking = melee_->blocking(entity);
        }
        if (const Turret* turret = reg.try_get<Turret>(entity)) {
            view.turretPhase = static_cast<uint8_t>(turret->phase);
            view.turretTarget = turret->target;
        }
        if (const Trap* trap = reg.try_get<Trap>(entity)) {
            view.trapState = trap->sprung ? 2 : 1;
        }
        out.things.push_back(view);
    }
    std::sort(out.things.begin(), out.things.end(),
              [](const ThingView& a, const ThingView& b) { return a.id < b.id; });

    out.alarm = raids_->state().alarm;
    out.alarmManual = raids_->state().manual;
    out.squads.clear();
    for (const Squad& squad : raids_->state().squads) {
        out.squads.push_back(SquadView{squad.id, squad.side, static_cast<uint8_t>(squad.state),
                                       static_cast<int>(squad.members.size()), squad.started, squad.target});
    }

    out.rooms.clear();
    for (const Room& room : rooms_->rooms()) {
        if (!room.enclosed()) {
            continue;  // the outdoors has no stats worth showing
        }
        RoomView view;
        view.id = room.id;
        view.role = room.role;
        view.cellCount = room.cellCount;
        view.bounds = room.bounds;
        view.outdoors = room.outdoors();
        view.openRoofCount = room.openRoofCount;
        view.beds = room.beds;
        view.tables = room.tables;
        view.chairs = room.chairs;
        view.stoves = room.stoves;
        if (const RoomStatValues* stats = roomStats_->statsOf(room.id)) {
            view.hasStats = true;
            view.beauty = stats->beauty;
            view.space = stats->space;
            view.wealth = stats->wealth;
            view.cleanliness = stats->cleanliness;
            view.impressiveness = stats->impressiveness;
        }
        view.temperature = temperature_->roomTemperature(room.id);
        out.rooms.push_back(view);
    }
    out.powerNets = power_->nets();
    out.outdoorTemperature = temperature_->outdoor();
    if (out.floorChanges != floors_->changes()) {
        out.floorChanges = floors_->changes();
        out.floors.clear();
        for (const auto& [index, def] : floors_->all()) {
            out.floors.push_back(FloorView{map_.cellAt(index), def});
        }
    }
    if (out.filthChanges != filth_->changes()) {
        out.filthChanges = filth_->changes();
        out.filth.clear();
        for (const auto& [index, piles] : filth_->all()) {
            for (const FilthPile& pile : piles) {
                out.filth.push_back(FilthView{map_.cellAt(index), filth_->kinds()[pile.kind].def, pile.thickness});
            }
        }
    }
    if (out.fireChanges != fire_->changes()) {
        out.fireChanges = fire_->changes();
        out.fires.clear();
        for (const auto& [index, fire] : fire_->all()) {
            out.fires.push_back(FireView{map_.cellAt(index), fire.size});
        }
    }
    if (out.burntChanges != fire_->burntChanges()) {
        out.burntChanges = fire_->burntChanges();
        out.burnt.clear();
        for (const auto& [index, at] : fire_->burntCells()) {
            (void)at;
            out.burnt.push_back(map_.cellAt(index));
        }
    }
    if (out.homeChanges != fire_->homeChanges()) {
        out.homeChanges = fire_->homeChanges();
        out.homeArea.clear();
        for (const int index : fire_->homeCells()) {
            out.homeArea.push_back(map_.cellAt(index));
        }
    }
    out.roofCells = rooms_->thinRoofCells();
    out.roofChanges = rooms_->roofChanges();
    out.roofCollapses = roofCollapse_->collapses();
    {
        const WeatherEffects effects = weather_->effects();
        WeatherView& w = out.weather;
        w.year = static_cast<int>(time_.year());
        w.dayOfYear = static_cast<int>(time_.dayOfYear());
        w.season = time_.season();
        w.temperature = static_cast<float>(weather_->outdoorTemperature());
        w.weather = weather_->current();
        w.previous = weather_->previous();
        w.transition = weather_->transition();
        w.skyGlow = effects.skyGlow;
        w.rainRate = effects.rainRate;
        w.snowRate = effects.snowRate;
        w.wetness = effects.wetness;
        w.snowChanges = weather_->snowChanges();
        w.snowCover = static_cast<float>(weather_->snowCellCount()) /
                      static_cast<float>(std::max(1, map_.width() * map_.height()));
    }
    out.projectiles.clear();
    for (const auto [shot, p] : reg.view<const Projectile>().each()) {
        out.projectiles.push_back(ProjectileView{shot, p.def, p.x, p.y, p.z, p.vx, p.vy, p.vz, p.level});
    }
    for (const auto [grenade, g] : reg.view<const Grenade>().each()) {
        out.projectiles.push_back(ProjectileView{grenade, g.def, g.x, g.y, g.z, g.vx, g.vy, g.vz, g.level, g.fuse});
    }
    std::sort(out.projectiles.begin(), out.projectiles.end(),
              [](const ProjectileView& a, const ProjectileView& b) { return a.id < b.id; });
    {
        const NarratorState state = narrator_->state();
        NarratorView& n = out.narrator;
        n.narrator = narrator_->narratorDef();
        n.difficulty = narrator_->difficultyDef();
        n.wealth = state.wealth;
        n.threatPoints = state.points;
        n.incidents = state.incidents;
    }
}

uint64_t Sim::stateHash() const {
    StateHasher h;
    h.add(time_.seconds);
    // The map's ground is fixed after generation, so its hash is taken once. Its building layer
    // follows from the buildings' footprints, hashed below. Map layers that change in play
    // (mining, plants, roofs) must keep their own running hash: hashing a 1 km map takes ~10 ms.
    h.add(groundHash_);
    h.add(rng_.state());
    h.add(rng_.increment());
    h.add(stockpileCell_.has_value());
    if (stockpileCell_) {
        simHashCell(h, *stockpileCell_);
    }
    h.add(static_cast<uint64_t>(haulers_.size()));
    for (const Entity pawn : haulers_) {
        h.add(entt::to_integral(pawn));
    }
    h.add(stats_.hauls);
    h.add(stats_.failedJobs);
    h.add(stats_.repaths);
    h.add(plants_->hash());
    h.add(weather_->hash());
    h.add(mineables_->hash());
    designations_->hashState(h);
    research_->hashState(h);
    raids_->hashState(h);
    talents_->hashState(h);
    pathfinder_.hashState(h);
    zones_->hashState(h);
    floors_->hashState(h);
    filth_->hashState(h);
    roomStats_->hashState(h);
    animals_->hashState(h);
    husbandry_->hashState(h);
    breeding_->hashState(h);
    fire_->hashState(h);
    temperature_->hashState(h);
    h.add(map_.upperFloorHash());
    roofCollapse_->hashState(h);
    spoilage_->hashState(h);
    narrator_->hashState(h);
    crafting_->hashState(h);
    farming_->hashState(h);

    // The entity ids the registry has handed out: the free list in reuse order (it decides the
    // next ids), then every live thing in id order with its components. The registry's own
    // order of live things depends on its history, and a loaded save doesn't repeat that.
    const entt::registry& reg = things_->registry();
    const auto* entities = reg.storage<Entity>();
    const size_t live = entities->free_list();
    h.add(static_cast<uint64_t>(entities->size()));
    h.add(static_cast<uint64_t>(live));
    for (size_t i = live; i < entities->size(); ++i) {
        h.add(entt::to_integral(entities->data()[i]));
    }
    std::vector<Entity> liveIds(entities->data(), entities->data() + live);
    std::sort(liveIds.begin(), liveIds.end());
    for (const Entity e : liveIds) {
        h.add(entt::to_integral(e));
        // Which components it has, then their fields.
        const bool has[] = {reg.all_of<Thing>(e),        reg.all_of<Position>(e),     reg.all_of<Footprint>(e),
                            reg.all_of<StackCount>(e),   reg.all_of<HitPoints>(e),    reg.all_of<Mover>(e),
                            reg.all_of<Worker>(e),       reg.all_of<WorkSettings>(e), reg.all_of<Carrying>(e),
                            reg.all_of<Colonist>(e),     reg.all_of<Health>(e),       reg.all_of<PlayerControl>(e),
                            reg.all_of<Needs>(e),        reg.all_of<Schedule>(e),     reg.all_of<Bed>(e),
                            reg.all_of<ConstructionSite>(e), reg.all_of<BuiltOf>(e),
                            reg.all_of<MarkedForDeconstruct>(e),
                            reg.all_of<Mood>(e),         reg.all_of<Door>(e),
                            reg.all_of<PawnLooks>(e),
                            reg.all_of<BillList>(e),
                            reg.all_of<PowerPart>(e),
                            reg.all_of<Spoilage>(e)};
        uint64_t mask = 0;
        for (size_t bit = 0; bit < sizeof has / sizeof has[0]; ++bit) {
            mask |= has[bit] ? (uint64_t{1} << bit) : 0;
        }
        h.add(mask);
        if (const Thing* thing = reg.try_get<Thing>(e)) {
            h.add(thing->def.value);
            h.add(static_cast<int>(thing->category));
        }
        if (const Position* pos = reg.try_get<Position>(e)) {
            h.add(pos->x);
            h.add(pos->y);
            h.add(pos->level);
        }
        if (const Footprint* f = reg.try_get<Footprint>(e)) {
            h.add(f->sizeX);
            h.add(f->sizeY);
            h.add(f->blocksMovement);
            h.add(f->isDoor);
            h.add(static_cast<int>(f->obstacleDm));
            simHashCell(h, f->origin);
            h.add(static_cast<int>(f->rotation));
        }
        if (const StackCount* stack = reg.try_get<StackCount>(e)) {
            h.add(stack->count);
        }
        if (const Spoilage* spoilage = reg.try_get<Spoilage>(e)) {
            h.add(spoilage->rot);
            h.add(spoilage->decay);
            h.add(spoilage->poisoned);
            for (const DefId ingredient : spoilage->ingredients) {
                h.add(ingredient.value);
            }
        }
        if (const HitPoints* hp = reg.try_get<HitPoints>(e)) {
            h.add(hp->current);
            h.add(hp->max);
        }
        if (const Mover* mover = reg.try_get<Mover>(e)) {
            h.add(mover->metresPerStep);
            h.add(static_cast<uint64_t>(mover->path.size()));
            for (const CellPos& c : mover->path) {
                simHashCell(h, c);
            }
            h.add(static_cast<uint64_t>(mover->next));
            h.add(mover->vx);
            h.add(mover->vy);
            h.add(mover->fromX);
            h.add(mover->fromY);
            h.add(static_cast<uint64_t>(mover->blockedSteps));
            h.add(static_cast<uint64_t>(mover->ghostSteps));
            h.add(static_cast<uint64_t>(mover->heldSteps));
            h.add(mover->gaveUp);
            if (mover->climbed != 0.0f) {
                h.add(mover->climbed);  // M2; only when climbing, so older hashes stay
            }
        }
        if (const Colonist* c = reg.try_get<Colonist>(e)) {
            for (const std::string* text : {&c->firstName, &c->nickName, &c->lastName, &c->childhood, &c->adulthood}) {
                h.add(text->data(), text->size());
            }
            h.add(c->female);
            h.add(c->age);
            h.add(c->protagonist);
            for (const TraitRecord& t : c->traits) {
                h.add(t.def.data(), t.def.size());
                h.add(t.degree);
            }
            for (const SkillRecord& s : c->skills) {
                h.add(s.def.data(), s.def.size());
                h.add(s.level);
                h.add(static_cast<int>(s.passion));
                h.add(s.xp);
                h.add(s.xpToday);
                h.add(s.disabled);
            }
            for (const std::string& tag : c->disabledWorkTags) {
                h.add(tag.data(), tag.size());
            }
        }
        if (const ConstructionSite* site = reg.try_get<ConstructionSite>(e)) {
            h.add(static_cast<int>(site->stage));
            h.add(site->stuff.value);
            simHashCell(h, site->origin);
            h.add(static_cast<int>(site->rotation));
            h.add(site->sizeX);
            h.add(site->sizeY);
            h.add(static_cast<uint64_t>(site->materials.size()));
            for (const MaterialCount& m : site->materials) {
                h.add(m.item.value);
                h.add(m.needed);
                h.add(m.have);
            }
            h.add(site->workTotal);
            h.add(site->workDone);
        }
        if (const BuiltOf* made = reg.try_get<BuiltOf>(e)) {
            h.add(made->stuff.value);
        }
        if (const PawnLooks* looks = reg.try_get<PawnLooks>(e)) {
            h.add(looks->lineage.data(), looks->lineage.size());
            h.add(looks->female);
            h.add(static_cast<int>(looks->body));
            h.add(looks->weight);
            h.add(looks->height);
            h.add(looks->melanin);
            for (const LooksColor* c : {&looks->naturalHair, &looks->skin, &looks->hair}) {
                h.add(c->r);
                h.add(c->g);
                h.add(c->b);
            }
        }
        if (const BillList* list = reg.try_get<BillList>(e)) {
            h.add(list->nextId);
            h.add(static_cast<uint64_t>(list->bills.size()));
            for (const Bill& bill : list->bills) {
                h.add(bill.id);
                h.add(bill.recipe.value);
                h.add(static_cast<int>(bill.mode));
                h.add(bill.count);
                h.add(bill.suspended);
                h.add(bill.skillMin);
                h.add(bill.skillMax);
                h.add(bill.radius);
                h.add(bill.storeZone);
                h.add(bill.unpauseAt);
                h.add(bill.satisfied);
                for (const DefId def : bill.disallowed) {
                    h.add(def.value);
                }
            }
        }
        if (const StoreIn* store = reg.try_get<StoreIn>(e)) {
            h.add(store->zone);
        }
        if (const Fuel* fuel = reg.try_get<Fuel>(e)) {
            h.add(fuel->amount);
            h.add(fuel->autoRefuel);
            h.add(fuel->fillOrdered);
        }
        if (const Worker* worker = reg.try_get<Worker>(e)) {
            const Job& job = worker->job;
            h.add(static_cast<int>(job.type));
            h.add(entt::to_integral(job.thing));
            simHashCell(h, job.cell);
            h.add(job.count);
            h.add(entt::to_integral(job.chair));
            h.add(entt::to_integral(job.site));
            h.add(job.progress);
            h.add(job.bill);
            h.add(static_cast<uint64_t>(job.picks.size()));
            for (const BillPick& pick : job.picks) {
                h.add(entt::to_integral(pick.item));
                h.add(pick.count);
            }
            h.add(static_cast<uint64_t>(job.used.size()));
            for (const BillUsed& used : job.used) {
                h.add(used.def.value);
                h.add(used.count);
            }
            h.add(job.workType.value);
            if (job.activity.valid()) {
                h.add(job.activity.value);
            }
            h.add(job.forced);
            h.add(static_cast<int>(job.handsOn));
            h.add(static_cast<int>(job.step));
            h.add(static_cast<int>(job.walk));
            h.add(static_cast<int>(job.repaths));
            h.add(job.startedAt);
            h.add(job.until);
            h.add(job.path.slot);
            h.add(job.path.serial);
            h.add(static_cast<uint64_t>(worker->avoid.size()));
            for (const AvoidTarget& avoid : worker->avoid) {
                h.add(entt::to_integral(avoid.target.thing));
                simHashCell(h, avoid.target.cell);
                h.add(avoid.until);
            }
        }
        if (const WorkSettings* work = reg.try_get<WorkSettings>(e)) {
            h.add(static_cast<uint64_t>(work->entries.size()));
            for (const WorkPriority& entry : work->entries) {
                h.add(entry.workType.value);
                h.add(static_cast<int>(entry.priority));
                h.add(entry.disabled);
            }
        }
        if (const Carrying* carrying = reg.try_get<Carrying>(e)) {
            h.add(entt::to_integral(carrying->thing));
        }
        if (const Needs* needs = reg.try_get<Needs>(e)) {
            h.add(needs->food);
            h.add(needs->rest);
            h.add(needs->malnutrition);
            h.add(needs->foodSearchAfter);
        }
        if (const Joy* joy = reg.try_get<Joy>(e)) {
            h.add(joy->level);
            h.add(joy->comfort);
            h.add(joy->outdoors);
            h.add(static_cast<uint64_t>(joy->tolerance.size()));
            for (const JoyTolerance& t : joy->tolerance) {
                h.add(t.kind.data(), t.kind.size());
                h.add(t.amount);
            }
        }
        if (const Mood* mood = reg.try_get<Mood>(e)) {
            h.add(mood->level);
            h.add(static_cast<int>(mood->band));
            h.add(static_cast<uint64_t>(mood->memories.size()));
            for (const Memory& memory : mood->memories) {
                h.add(memory.def.data(), memory.def.size());
                h.add(memory.expires);
            }
        }
        if (const Schedule* schedule = reg.try_get<Schedule>(e)) {
            for (const ScheduleKind kind : schedule->hours) {
                h.add(static_cast<int>(kind));
            }
        }
        if (const Door* door = reg.try_get<Door>(e)) {
            h.add(static_cast<uint64_t>(door->progress));
            h.add(static_cast<uint64_t>(door->hold));
            h.add(door->open);
        }
        if (const PowerPart* power = reg.try_get<PowerPart>(e)) {
            h.add(power->on);
            h.add(power->powered);
            h.add(power->stored);
            h.add(power->draw);
        }
        if (const Bed* bed = reg.try_get<Bed>(e)) {
            h.add(static_cast<uint64_t>(bed->owners.size()));
            for (const Entity owner : bed->owners) {
                h.add(entt::to_integral(owner));
            }
        }
        if (const Health* health = reg.try_get<Health>(e)) {
            h.add(health->protagonist);
            h.add(static_cast<int>(health->rest));
            h.add(static_cast<uint64_t>(health->injuries.size()));
            for (const Injury& injury : health->injuries) {
                h.add(static_cast<int>(injury.part));
                h.add(injury.hediff.value);
                h.add(injury.severity);
                h.add(injury.tendQuality);
                h.add(injury.peak);
                h.add(injury.infectAt);
            }
            h.add(static_cast<uint64_t>(health->diseases.size()));
            for (const Disease& disease : health->diseases) {
                h.add(disease.def.value);
                h.add(disease.severity);
                h.add(disease.tendQuality);
                h.add(disease.tendedUntil);
                h.add(disease.tendTotal);
                h.add(static_cast<int>(disease.part));
            }
            h.add(static_cast<uint64_t>(health->immunities.size()));
            for (const Immunity& immunity : health->immunities) {
                h.add(immunity.def.value);
                h.add(immunity.level);
            }
            h.add(static_cast<uint64_t>(health->missing.size()));
            for (const MissingPart& missing : health->missing) {
                h.add(static_cast<int>(missing.part));
                h.add(missing.tendQuality);
            }
            h.add(health->bloodLoss);
            h.add(health->dead);
            h.add(static_cast<int>(health->cause));
        }
        medicine_->hashPawn(e, h);
        combat_->hashThing(e, h);
        explosives_->hashThing(e, h);
        fighters_->hashThing(e, h);
        animals_->hashThing(e, h);
        raids_->hashThing(e, h);
        prisoners_->hashThing(e, h);
        husbandry_->hashThing(e, h);
        breeding_->hashThing(e, h);
        fire_->hashThing(e, h);
        melee_->hashThing(e, h);
        social_->hashThing(e, h);
        defense_->hashThing(e, h);
        apparel_->hashThing(e, h);
        mentalBreaks_->hashThing(e, h);
        inspirations_->hashThing(e, h);
    }
    apparel_->hashState(h);
    return h.value();
}

void Sim::stepMany(int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        step();
    }
}

}  // namespace colony

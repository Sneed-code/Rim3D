#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Random.h"
#include "ColonySim/ThreatMap.h"
#include "ColonySim/World.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class AnimalSystem;
class Breeding;
class Fire;
class ApparelSystem;
class Construction;
class DefDatabase;
class DefenseSystem;
class DrugSystem;
class Filth;
class Floors;
class RoomStats;
struct Colonist;
struct PawnLooks;
struct PawnGenes;
class Designations;
class ExplosiveSystem;
class FighterSystem;
class Areas;
class Husbandry;
class InspirationSystem;
class MentalBreakSystem;
class JobSystem;
class Lights;
class MeleeSystem;
class DeepResources;
class Mineables;
class MovementSystem;
class WoundViews;
class MoodSystem;
class MedicineSystem;
class SurgerySystem;
class RecreationSystem;
class SocialSystem;
class ResearchSystem;
class ResonanceSystem;
class TalentSystem;
class TravelSystem;
class NeedsSystem;
class RaidSystem;
class FieldCare;
class MonsterSystem;
class Plants;
class PowerGrid;
class Regions;
class PrisonerSystem;
class TradeSystem;
class QuestSystem;
class ShipSystem;
class CaravanSystem;
class Rooms;
class TemperatureSystem;
class SpoilageSystem;
class LootSystem;
class CraftingSystem;
class RepairSystem;
class Things;
class UpperFloors;
class RoofCollapse;
class Weather;
class Zones;
class Farming;
class NarratorSystem;
class DisasterSystem;
struct Snapshot;
struct OtherMapView;
class SimSaveAccess;
class StateHasher;

// The planet the game is on (docs/design/world-map.md). Off (the default), there is no world: the map is
// made from SimSettings::seed in the temperate forest, as before W2. On, the sim makes the world from
// these settings and the colony's map is the start tile's: its biome, climate and map seed.
struct WorldSettings {
    bool enabled = false;
    uint64_t seed = 0;   // 0: the sim's seed
    int width = 0;       // tiles; 0 takes the WorldGen def's
    int height = 0;
    // The colony's tile. -1: picked when the sim is made, by the scenario's biome, and then stored
    // here, so a save or replay always names the tile.
    int startTile = -1;
    // This sim is another map (W5, docs/design/other-maps.md): `startTile` is the visited tile, which may
    // hold a settlement, and the colony's own tile is not here.
    bool visit = false;
};

// Sizes a world can have (0 takes the def's) and a start tile that is a tile or -1. Saves and replays
// refuse other settings.
inline bool worldSettingsValid(const WorldSettings& w) {
    return w.width >= 0 && w.width <= 1000 && w.height >= 0 && w.height <= 500 && w.startTile >= -1 &&
           (w.startTile < 0 || w.width == 0 || w.height == 0 || w.startTile < w.width * w.height);
}

struct SimSettings {
    uint64_t seed = 0;
    int mapSize = 512;  // cells per side
    int pawns = 0;      // Hauling test: pawns start at a stockpile zone in the middle
    int items = 0;      // and haul items scattered over the reachable map to it
    bool flatMap = false;               // all soil at height 0, for tests
    std::string pawnDef = "Human";      // Things defs the hauling test spawns
    std::string haulItemDef = "Steel";
    // Record a state hash every this many game seconds (0: never), for replays to compare.
    int64_t checkpointEvery = kSecondsPerHour;
    int pathBudget = Pathfinder::kDefaultBudget;  // path search work per step (docs/design/pathfinding.md)
    // A Scenarios def to start (e.g. "Crashlanded"): colonists made from the seed, with starting
    // items, near the middle of the map (docs/design/colonists.md). Empty: no scenario.
    std::string scenario{};
    // Wild animals when the map is made (docs/design/animal-life.md): 0 for none, else the biome's
    // animal density in percent (100 = as the biome says). Needs the Biomes and animal defs.
    int wildAnimals = 0;
    WorldSettings world{};  // saved in its own section, "world"
    // The game second the sim starts at (another map made in play starts at the colony's time). Not
    // saved: a save has the time it reached.
    int64_t startSecond = 0;
};

struct SimStats {
    uint64_t hauls = 0;
    uint64_t failedJobs = 0;
    uint64_t repaths = 0;  // paths asked for again because a pawn gave up on its walk
};

// The state hash after a step, recorded every SimSettings::checkpointEvery game seconds.
struct Checkpoint {
    int64_t step = 0;  // game seconds
    uint64_t hash = 0;
};

// Wall time spent in each system of a step, summed over the steps run while it is set on a Sim
// (`simrun --perf`, docs/design/sim-cost.md). Names are string literals.
struct StepProfile {
    std::vector<const char*> names;
    std::vector<double> ms;
    int64_t steps = 0;
    void add(const char* name, double elapsedMs) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                ms[i] += elapsedMs;
                return;
            }
        }
        names.push_back(name);
        ms.push_back(elapsedMs);
    }
};

class Sim;

// Another world tile's map (W5, docs/design/other-maps.md): a sim of its own, made the first time a
// caravan visits the tile and kept from then on, stepped with the colony's.
struct OtherMap {
    int tile = -1;
    int64_t madeAt = 0;  // the colony's game second when it was made
    std::unique_ptr<Sim> sim;
};

// The whole game state and its rules. Unreal sends commands, reads events and snapshots, and
// never changes sim state directly (docs/design/bridge.md).
class COLONY_API Sim {
public:
    // A sim with no defs: a map but no things. Enough for terrain work and the Stage 0 tests.
    explicit Sim(const SimSettings& settings);
    // Things are made from `defs`, which every sim made from it shares. The hauling test needs the
    // Things defs it names and the WorkTypes def Hauling.
    Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs);
    ~Sim();
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    // Advances one game second: applies the commands submitted since the last step, in order,
    // then runs every system.
    void step();
    void stepMany(int64_t steps);
    // Times each system of the following steps into `profile` (null: off, the default). Not saved,
    // not hashed.
    void setProfile(StepProfile* profile) { profile_ = profile; }

    // Queues a command for the start of the next step. Returns its id: 1, 2, 3... in submit
    // order, which a CommandFailed event names.
    uint64_t submit(Command command);
    // Every command applied so far, failed ones included.
    const std::vector<LoggedCommand>& commandLog() const { return commandLog_; }

    // What happened during the last step (before the first step: while setting up the map).
    // Cleared when the next step starts, so read it after every step.
    const std::vector<Event>& events() const { return events_; }

    // Fills `out` with what Unreal draws, reusing its memory.
    void writeSnapshot(Snapshot& out) const;

    // Hash of the whole game state: same seed and commands, same hash. Every piece of sim state
    // must go in it (docs/design/bridge.md). Costs about a microsecond per thing.
    uint64_t stateHash() const;
    const std::vector<Checkpoint>& checkpoints() const { return checkpoints_; }

    const SimSettings& settings() const { return settings_; }
    const DefDatabase& defs() const { return *defs_; }
    const GameTime& time() const { return time_; }
    const Map& map() const { return map_; }
    // For map generation and tests that set the world up; the game changes the map through commands.
    Map& editMap() { return map_; }
    // Regions and reachability (docs/design/regions.md). Queries may rebuild, so not const.
    Regions& regions() { return map_.regions(); }
    // Cover from the eight sides and line of sight (docs/design/cover.md).
    Cover& cover() { return map_.cover(); }
    // The threat map of a faction, made empty the first time it is asked for.
    ThreatMap& threatMap(FactionId faction);
    uint64_t seed() const { return settings_.seed; }

    // Every pawn, item and building (docs/design/things.md).
    Things& things() { return *things_; }
    const Things& things() const { return *things_; }
    // Body parts, injuries, bleeding, downed and death (docs/design/health.md). Damage comes in
    // through `health().applyDamage` from other sim systems, or the DamagePawnCommand.
    HealthSystem& health() { return *health_; }
    const HealthSystem& health() const { return *health_; }
    // Food and rest (docs/design/needs.md). Eating and sleeping are jobs, see jobs().
    NeedsSystem& needs() { return *needs_; }
    const NeedsSystem& needs() const { return *needs_; }
    // Mood and thoughts (docs/design/mood.md).
    MoodSystem& mood() { return *mood_; }
    const MoodSystem& mood() const { return *mood_; }
    // Recreation, comfort and the outdoors need (docs/design/recreation.md).
    RecreationSystem& recreation() { return *recreation_; }
    const RecreationSystem& recreation() const { return *recreation_; }
    // Drugs, tolerance, addiction, withdrawal and drug policies (docs/design/drugs.md).
    DrugSystem& drugs() { return *drugs_; }
    const DrugSystem& drugs() const { return *drugs_; }
    // Research projects and what they unlock (docs/design/research.md).
    ResearchSystem& research() { return *research_; }
    const ResearchSystem& research() const { return *research_; }
    // Medicine: tend quality, medical care settings (docs/design/medicine.md).
    MedicineSystem& medicine() { return *medicine_; }
    const MedicineSystem& medicine() const { return *medicine_; }
    // Surgery: operations planned on patients, their success chances (docs/design/surgery.md).
    SurgerySystem& surgery() { return *surgery_; }
    const SurgerySystem& surgery() const { return *surgery_; }
    // The protagonist's talent XP, points and ranks (docs/design/talents.md).
    TalentSystem& talents() { return *talents_; }
    const TalentSystem& talents() const { return *talents_; }
    // Guns, projectiles, hitboxes and armor (docs/design/combat.md).
    CombatSystem& combat() { return *combat_; }
    const CombatSystem& combat() const { return *combat_; }
    // Grenades: belts, throwing, fuses (docs/design/explosives.md). Blasts are `combat().explode`.
    ExplosiveSystem& explosives() { return *explosives_; }
    const ExplosiveSystem& explosives() const { return *explosives_; }
    // Turrets and traps (docs/design/defense.md).
    DefenseSystem& defense() { return *defense_; }
    const DefenseSystem& defense() const { return *defense_; }
    // Clothes and armor, outfits, stripping (docs/design/apparel.md).
    ApparelSystem& apparel() { return *apparel_; }
    const ApparelSystem& apparel() const { return *apparel_; }
    // What a health panel shows for a pawn; false if it has no body.
    bool healthReport(Entity pawn, HealthReport& out) const { return health_->report(pawn, out); }

    // The colonist the player controls directly (ControlPawnCommand), or kNoEntity.
    Entity controlledPawn() const;
    // A pawn's colonist record (name, skills...), or null if it is not a colonist.
    const Colonist* colonistOf(Entity pawn) const;
    // How a humanlike pawn looks (docs/design/looks.md), or null for other things.
    const PawnLooks* looksOf(Entity pawn) const;
    // A humanlike pawn's genes, made from its lineage (docs/design/genes.md), or null for other things.
    const PawnGenes* genesOf(Entity pawn) const;

    // The plant layer: wild plants from map generation, growing hourly (docs/design/plants.md).
    Plants& plants() { return *plants_; }
    const Plants& plants() const { return *plants_; }

    // The world map, or null when SimSettings::world is off (docs/design/world-map.md). It does not
    // change in play: made from its settings when the sim is made and again when a save loads.
    const World* world() const { return world_.get(); }
    // What the world says about the colony's tile (biome, hills, river, road, climate, map seed),
    // or null without a world. The map generator (S8) and other maps (W5) read it.
    const WorldSite* site() const { return site_ ? &*site_ : nullptr; }
    // The biome the plants, animals and weather use: the start tile's, or the temperate forest.
    const std::string& biome() const { return biome_; }

    // Seasons, outdoor temperature, weather and snow (docs/design/weather.md).
    Weather& weather() { return *weather_; }
    const Weather& weather() const { return *weather_; }

    // The rock and ore layer, mined down by jobs (docs/design/mining.md), and the marks the player
    // has put on rock and plants for colonists to work on.
    Mineables& mineables() { return *mineables_; }
    const Mineables& mineables() const { return *mineables_; }
    // Deep deposits, the scanner and the deep drill (docs/design/deep-mining.md).
    DeepResources& deepResources() { return *deepResources_; }
    const DeepResources& deepResources() const { return *deepResources_; }
    Designations& designations() { return *designations_; }
    const Designations& designations() const { return *designations_; }

    // Problems setting up the start (e.g. a def the settings name is missing). Empty when fine.
    const std::vector<std::string>& setupErrors() const { return setupErrors_; }

    // Rooms, roofs and roles (docs/design/rooms.md). Brought up to date at the end of every step.
    Rooms& rooms() { return *rooms_; }
    const Rooms& rooms() const { return *rooms_; }

    // Light and darkness (docs/design/light.md). Brought up to date at the end of every step.
    Farming& farming() { return *farming_; }
    const Farming& farming() const { return *farming_; }
    Lights& lights() { return *lights_; }
    const Lights& lights() const { return *lights_; }
    // Floors, filth, room stats and the beauty need (docs/design/room-stats.md).
    Floors& floors() { return *floors_; }
    const Floors& floors() const { return *floors_; }
    Filth& filth() { return *filth_; }
    const Filth& filth() const { return *filth_; }
    RoomStats& roomStats() { return *roomStats_; }
    const RoomStats& roomStats() const { return *roomStats_; }

    // Room temperature, comfort and exposure (docs/design/temperature.md).
    TemperatureSystem& temperature() { return *temperature_; }
    const TemperatureSystem& temperature() const { return *temperature_; }
    SpoilageSystem& spoilage() { return *spoilage_; }
    const SpoilageSystem& spoilage() const { return *spoilage_; }
    // Quality, stuff and bench speed of what work tables make (docs/design/crafting.md).
    CraftingSystem& crafting() { return *crafting_; }
    const CraftingSystem& crafting() const { return *crafting_; }
    // Repair of clothes, armor and weapons, and upgrade parts on guns (docs/design/repair.md).
    RepairSystem& repair() { return *repair_; }
    const RepairSystem& repair() const { return *repair_; }
    // The temperature in the open right now, in degrees Celsius: the biome's season, the hour of the
    // day and the random drift (Weather::temperatureAt).
    float outdoorTemperature() const;
    // The open air's temperature at any game second, ignoring the override (for tests and previews).
    float outdoorTemperatureAt(int64_t gameSeconds) const;
    // The temperature under rock: the biome's mean over the year.
    float groundTemperature() const;
    // Fixes the outdoor temperature (and so the ground's) for tests and `simrun --outdoor`. Not saved:
    // a loaded game goes back to the biome's own. Pass nullopt to let the biome decide again.
    void setOutdoorOverride(std::optional<float> celsius);

    // Upper floors and the rules for laying them (docs/design/levels.md).
    UpperFloors& upperFloors() { return *upperFloors_; }
    const UpperFloors& upperFloors() const { return *upperFloors_; }
    // Allowed areas: named cell sets a pawn can be limited to (docs/design/areas.md).
    Areas& areas() { return *areas_; }
    const Areas& areas() const { return *areas_; }
    RoofCollapse& roofCollapse() { return *roofCollapse_; }
    const RoofCollapse& roofCollapse() const { return *roofCollapse_; }

    // Stockpile zones (docs/design/jobs.md).
    const Zones& zones() const { return *zones_; }
    // Work types, the think list, jobs and reservations (docs/design/jobs.md).
    JobSystem& jobs() { return *jobs_; }
    const JobSystem& jobs() const { return *jobs_; }

    // Drafted colonists and other factions' pawns: where to stand, whom to shoot (docs/design/fighters.md).
    FighterSystem& fighters() { return *fighters_; }
    const FighterSystem& fighters() const { return *fighters_; }
    // Melee swings, blocks and dodges (docs/design/melee.md).
    MeleeSystem& melee() { return *melee_; }
    const MeleeSystem& melee() const { return *melee_; }

    // Opinions, interactions, bonds and social fights (docs/design/social.md).
    SocialSystem& social() { return *social_; }
    const SocialSystem& social() const { return *social_; }

    // Wild animals: herds, grazing, sleep, coming and going at the edges (docs/design/animal-life.md).
    AnimalSystem& animals() { return *animals_; }
    const AnimalSystem& animals() const { return *animals_; }
    // Raids, raid squads and the colony alarm with its defense roles (docs/design/raids.md).
    RaidSystem& raids() { return *raids_; }
    const RaidSystem& raids() const { return *raids_; }
    // Medics, firefighters and rescue under fire (F10, docs/design/field-care.md).
    FieldCare& fieldCare() { return *fieldCare_; }
    const FieldCare& fieldCare() const { return *fieldCare_; }

    // Brood mounds and Remnant clusters (docs/design/monsters.md).
    MonsterSystem& monsters() { return *monsters_; }

    // The narrator, incidents and threat points (docs/design/narrator.md).
    NarratorSystem& narrator() { return *narrator_; }
    const NarratorSystem& narrator() const { return *narrator_; }
    // Loot tables: what raiders drop, cargo pods, ancient stashes (docs/design/loot.md).
    LootSystem& loot() { return *loot_; }
    const LootSystem& loot() const { return *loot_; }
    // Disasters and bad events: solar flare, toxic fallout, volcanic winter, flashstorm, psychic waves, blight,
    // manhunters, short circuits (docs/design/disasters.md).
    DisasterSystem& disasters() { return *disasters_; }
    const DisasterSystem& disasters() const { return *disasters_; }
    // Prisoners: capture, feeding, warden talks, recruiting and release (docs/design/prisoners.md).
    // The spaceship ending: parts, reactor, siege waves, launch (docs/design/ship.md).
    ShipSystem& ship() { return *ship_; }
    const ShipSystem& ship() const { return *ship_; }
    // Caravans on the world map (docs/design/caravans.md); they need a world.
    CaravanSystem& caravans() { return *caravans_; }
    const CaravanSystem& caravans() const { return *caravans_; }
    PrisonerSystem& prisoners() { return *prisoners_; }
    const PrisonerSystem& prisoners() const { return *prisoners_; }
    // Quests: offers, answers, rewards (docs/design/quests.md). The narrator offers them with `quests().offerRandom`.
    QuestSystem& quests() { return *quests_; }
    const QuestSystem& quests() const { return *quests_; }

    // Faction goodwill, visitors, trade caravans, orbital traders and trade (docs/design/trade.md).
    TradeSystem& trade() { return *trade_; }
    const TradeSystem& trade() const { return *trade_; }

    // Taming, training, animal zones and pens (docs/design/husbandry.md).
    Husbandry& husbandry() { return *husbandry_; }
    // Fire, burning pawns, burnt ground and the home area (docs/design/fire.md).
    Fire& fire() { return *fire_; }
    const Fire& fire() const { return *fire_; }
    Breeding& breeding() { return *breeding_; }
    const Breeding& breeding() const { return *breeding_; }
    const Husbandry& husbandry() const { return *husbandry_; }

    // Colonists who snap when their mood is low: wander, binge, tantrum, berserk, leave (docs/design/mental-breaks.md).
    MentalBreakSystem& mentalBreaks() { return *mentalBreaks_; }
    const MentalBreakSystem& mentalBreaks() const { return *mentalBreaks_; }

    // Good-mood colonists get inspired: a legendary craft, a recruitment, a shooting frenzy (docs/design/inspirations.md).
    InspirationSystem& inspirations() { return *inspirations_; }
    const InspirationSystem& inspirations() const { return *inspirations_; }
    // Night skip and fast travel (docs/design/skip-travel.md).
    TravelSystem& travel() { return *travel_; }
    const TravelSystem& travel() const { return *travel_; }

    // Blueprints, frames and taking buildings down (docs/design/construction.md).
    Construction& construction() { return *construction_; }
    const Construction& construction() const { return *construction_; }

    // Attunement, strain, focus and the powers of resonance (docs/design/resonance.md).
    ResonanceSystem& resonance() { return *resonance_; }
    const ResonanceSystem& resonance() const { return *resonance_; }

    // Power nets, generators, batteries and who has power (docs/design/power.md). Stepped at the end
    // of every step, after rooms (the sun needs roofs).
    PowerGrid& power() { return *power_; }
    const PowerGrid& power() const { return *power_; }

    // Other maps (W5, docs/design/other-maps.md): the tiles caravans have visited, each its own sim with
    // its own things (their ids are that sim's). Pawns there from the colony stay here as entities with
    // only an `AwayPawn` part. Null / empty without a world, and on another map itself.
    const std::vector<OtherMap>& otherMaps() const { return otherMaps_; }
    const Sim* otherMap(int tile) const;
    Sim* otherMap(int tile);

    // Hauling test: the middle of its stockpile zone (empty if no cell is walkable) and its pawns.
    const std::optional<CellPos>& stockpileCell() const { return stockpileCell_; }
    const std::vector<Entity>& haulers() const { return haulers_; }
    // Items not in a stockpile that takes them, carried ones included.
    int itemsLeft() const;
    const SimStats& stats() const { return stats_; }
    // Path requests go through its queue, which runs once per step (docs/design/pathfinding.md).
    Pathfinder& pathfinder() { return pathfinder_; }
    const Pathfinder& pathfinder() const { return pathfinder_; }

private:
    friend class SimSaveAccess;  // saves read and restore the private state (Save.h)

    struct EmptyStart {};
    // A sim with its map and nothing else: no hauling test. Sim(settings, defs) adds the test's
    // stockpile, items and pawns; loading a save restores them from the file instead.
    Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs, EmptyStart);

    void applyCommand(const Command& command, uint64_t id);
    // One game second. `keepEvents`: the events of the steps before it in this Sim::step stay (a skip's batch).
    void stepOnce(bool keepEvents);
    void commandFailed(uint64_t id, std::string message);
    // ClimbCommand (U17): sets the controlled pawn off up or down the stairs or ladder at its cell.
    bool startClimb(Entity pawn, float dx, float dy, std::string& error);
    void placeStockpileAndThings();
    // W5 (OtherMaps.cpp): a caravan on another tile walks onto its map, made the first time.
    bool visitMap(int caravan, std::string& error);
    // A caravan formed by colonists on another map; it comes back here once it sets out.
    bool formOnMap(int tile, const std::vector<Entity>& pawns, const std::vector<Entity>& items, int destination,
                   int& formed, std::string& error);
    // Steps every other map once, then takes over the caravans that left them.
    void stepOtherMaps();
    void takeCaravanFrom(OtherMap& map, int caravan);
    void hashOtherMaps(StateHasher& h) const;
    void writeOtherMaps(std::vector<OtherMapView>& out) const;
    std::string placeName(int tile) const;
    void pruneDesignations();
    void startScenario();
    // Gives a new pawn its looks if it is humanlike (its def names no other intelligence).
    Entity makeJoiner(CellPos cell, uint64_t seed);
    void giveLooks(Entity pawn, uint64_t seed, bool female, const std::string& lineage);
    // Makes (again) the genes of a pawn that has looks, from its lineage.
    void giveGenes(Entity pawn);

    SimSettings settings_;
    std::shared_ptr<const DefDatabase> defs_;
    // The world, its start tile's site and the biome come before the map: the map's seed is the site's.
    std::unique_ptr<World> world_;
    uint64_t worldHash_ = 0;
    std::optional<WorldSite> site_;
    std::string biome_;
    Map map_;
    Pathfinder pathfinder_;
    std::unique_ptr<Zones> zones_;
    std::unique_ptr<Things> things_;
    std::unique_ptr<Plants> plants_;
    std::unique_ptr<Weather> weather_;
    std::unique_ptr<Mineables> mineables_;
    std::unique_ptr<DeepResources> deepResources_;
    std::unique_ptr<Designations> designations_;
    std::unique_ptr<MovementSystem> movement_;
    std::unique_ptr<Rooms> rooms_;
    std::unique_ptr<PowerGrid> power_;
    std::unique_ptr<Lights> lights_;
    std::unique_ptr<TemperatureSystem> temperature_;  // after rooms_ and health_
    std::unique_ptr<Farming> farming_;                // after temperature_, lights_ and zones_
    std::unique_ptr<SpoilageSystem> spoilage_;        // after temperature_ and roomStats_
    std::optional<float> outdoorOverride_;
    StepProfile* profile_ = nullptr;
    DoorNumbers doorNumbers_;  // for the snapshot's door animation
    std::unique_ptr<HealthSystem> health_;
    std::unique_ptr<WoundViews> woundViews_;  // for the snapshot's wounds
    std::unique_ptr<NeedsSystem> needs_;
    std::unique_ptr<MoodSystem> mood_;
    std::unique_ptr<RecreationSystem> recreation_;
    std::unique_ptr<DrugSystem> drugs_;  // after health_, needs_ and mood_
    std::unique_ptr<ResearchSystem> research_;
    std::unique_ptr<MedicineSystem> medicine_;
    std::unique_ptr<SurgerySystem> surgery_;
    std::unique_ptr<TalentSystem> talents_;
    std::unique_ptr<CombatSystem> combat_;
    std::unique_ptr<ExplosiveSystem> explosives_;  // after combat_ and health_
    std::unique_ptr<ApparelSystem> apparel_;  // after combat, which it uses
    std::unique_ptr<CraftingSystem> crafting_;  // after apparel_ and zones_
    std::unique_ptr<RepairSystem> repair_;      // after apparel_ and combat_
    std::map<FactionId, std::unique_ptr<ThreatMap>> threatMaps_;
    GameTime time_;
    Rng rng_;
    uint64_t groundHash_;  // map content hash before anything was built
    std::optional<CellPos> stockpileCell_;
    std::vector<Entity> haulers_;
    SimStats stats_;
    std::unique_ptr<Construction> construction_;
    std::unique_ptr<JobSystem> jobs_;  // after everything it holds a reference to
    std::unique_ptr<FighterSystem> fighters_;  // after the job system
    std::unique_ptr<Floors> floors_;           // C17; filth and room stats read the floors
    std::unique_ptr<Filth> filth_;
    std::unique_ptr<RoomStats> roomStats_;
    std::unique_ptr<AnimalSystem> animals_;
    std::unique_ptr<UpperFloors> upperFloors_;  // M1
    std::unique_ptr<Areas> areas_;                // allowed areas, before the job system uses them
    std::unique_ptr<RoofCollapse> roofCollapse_;  // M3, after the rooms and the job system
    std::unique_ptr<RaidSystem> raids_;        // after the fighters
    std::unique_ptr<MonsterSystem> monsters_;  // after raids_ and animals_
    std::unique_ptr<PrisonerSystem> prisoners_;  // after the raids
    std::unique_ptr<TradeSystem> trade_;         // after the raids
    std::unique_ptr<QuestSystem> quests_;        // after the raids
    std::unique_ptr<ShipSystem> ship_;           // after the raids
    std::unique_ptr<CaravanSystem> caravans_;    // after the job system
    std::unique_ptr<Husbandry> husbandry_;  // after animals_ and the job system
    std::unique_ptr<Breeding> breeding_;    // after husbandry_
    std::unique_ptr<Fire> fire_;            // after the weather, temperature, rooms, floors, filth, plants and health
    std::unique_ptr<MentalBreakSystem> mentalBreaks_;  // after the job and fighter systems
    std::unique_ptr<InspirationSystem> inspirations_;  // after the mood system
    std::vector<std::string> setupErrors_;
    std::vector<std::pair<uint64_t, Command>> pending_;  // id, command
    uint64_t submitted_ = 0;
    std::vector<LoggedCommand> commandLog_;
    std::vector<Event> events_;
    std::vector<Checkpoint> checkpoints_;
    std::unique_ptr<MeleeSystem> melee_;  // after the fighters, which hold a pointer to it
    std::unique_ptr<DefenseSystem> defense_;  // after the fighters, which hold a pointer to it
    std::unique_ptr<FieldCare> fieldCare_;    // after the raids, the fighters and the jobs, which hold a pointer to it
    std::unique_ptr<NarratorSystem> narrator_;  // after raids_
    std::unique_ptr<LootSystem> loot_;          // after the narrator, whose cargo pod it carries out
    std::unique_ptr<DisasterSystem> disasters_;  // T3, after the narrator and everything it touches
    std::unique_ptr<SocialSystem> social_;    // after the mood, melee and job systems it uses
    std::unique_ptr<ResonanceSystem> resonance_;  // after the fighters and the job system it holds
    std::unique_ptr<TravelSystem> travel_;    // after nearly everything: it asks the others about threats
    std::vector<OtherMap> otherMaps_;         // W5, by the order they were made
};

}  // namespace colony

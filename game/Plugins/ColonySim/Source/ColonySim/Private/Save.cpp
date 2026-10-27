#include "ColonySim/Save.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Breeding.h"
#include "ColonySim/Apparel.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Defense.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Drugs.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Power.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Disasters.h"
#include "ColonySim/Narrator.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Health.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Weather.h"
#include "ColonySim/Designations.h"
#include "ColonySim/RoofCollapse.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Crafting.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Inspirations.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Surgery.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Quests.h"
#include "ColonySim/Ship.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Social.h"
#include "ColonySim/Research.h"
#include "ColonySim/Talents.h"
#include "ColonySim/Trade.h"
#include "ColonySim/Things.h"
#include "ColonySim/Version.h"
#include "ColonySim/Zones.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

// The sim builds without exceptions, so the JSON library must not throw.
#ifndef JSON_NOEXCEPTION
#define JSON_NOEXCEPTION
#endif
#include "nlohmann/json.hpp"

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
using SaveJson = nlohmann::ordered_json;

constexpr char kSaveMagic[8] = {'R', 'I', 'M', '3', 'D', 'S', 'A', 'V'};
constexpr size_t kSaveMagicSize = sizeof kSaveMagic;
// The file format. Formats 2 to 19 each added something to one long layout, so two tasks that
// added state at once fought over the next number. Format 20 (S7) splits the save into named
// sections, each with its own version (see `saveSections`): a task that adds or changes state
// bumps or adds its own section and never this number. Older formats still load (the archive's
// version() says which one a file is; docs/design/saves.md lists what each added):
// 2: S3's path queue, pathBudget, a hauler's path ticket; 3: S4's pawn velocity; 4: C8's scenario
// and colonists; 5: F1's pawn health; 6: C1's jobs, work priorities and zones; 7: U9's direct
// control; 8: C6's needs, schedules, bed owners, the eating job's chair; 9: C2's plants; 10: C4's
// building plans, frames and deconstruct marks, a job's site and progress; 11: C9's mood; 12: F2's
// posture, gear, triggers and projectiles; 13: C5's door states; 14: C3's rock layer,
// designations; 15: F4's factions and fighters; 16: A3's pawn looks; 17: F5's magazines,
// reloading, a fighter's reaction and dodge times; 18: C7's bills and the bill job; 19: U10's
// hands-on jobs; 20: S7's sections and commands by name.
constexpr uint32_t kSaveFormatVersion = 20;
constexpr uint32_t kSaveSectionsFormat = 20;
// What sectionVersion() says while reading a format 6 to 19 file: their layout is every
// section's version 1.
constexpr uint32_t kSaveLegacySectionVersion = 1;
constexpr uint32_t kSaveOldestFormat = 6;
constexpr const char* kSaveFormatName = "rim3d-save";
constexpr const char* kSaveThingsKind = "Things";
constexpr const char* kSaveWorkTypesKind = "WorkTypes";

uint64_t saveChecksum(const uint8_t* data, size_t size) {
    StateHasher h;
    h.add(static_cast<uint64_t>(size));
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word = 0;
        for (size_t k = 0; k < 8; ++k) {
            word |= static_cast<uint64_t>(data[i + k]) << (8 * k);
        }
        h.add(word);
    }
    uint64_t tail = 0;
    for (size_t k = 0; i + k < size; ++k) {
        tail |= static_cast<uint64_t>(data[i + k]) << (8 * k);
    }
    h.add(tail);
    return h.value();
}

// What the save holds, as plain values. The three archives below (binary write, binary read,
// JSON write) all walk it with the same `saveXxx(ar, value)` functions, so the file layout and
// the JSON dump can't drift apart.
struct SaveThing {
    std::string def;  // defName of a Things def
    ThingCategory category = ThingCategory::Item;
};

// A pawn's health as saved: the primary state of `Health` (what is derived is worked out again on
// loading), with the injuries' defs by name.
struct SaveInjury {
    uint16_t part = 0;
    std::string hediff;  // defName of a Hediffs def
    float severity = 0.0f;
    float tendQuality = -1.0f;
    float peak = 0.0f;  // health section 3 on
    int64_t infectAt = -1;  // health section 4 on
};

struct SaveDisease {  // health section 4 on
    std::string hediff;  // defName of a Hediffs def of class Disease
    float severity = 0.0f;
    float tendQuality = -1.0f;
    int64_t tendedUntil = 0;
    float tendTotal = 0.0f;
    int32_t part = -1;
};

struct SaveImmunity {  // health section 4 on
    std::string hediff;
    float level = 0.0f;
};

// Section "drugs" (Drugs.h): one record per pawn that has taken a drug or been given a policy.
struct SaveDrugEntry {
    std::string drug;
    bool forAddiction = true;
    bool forJoy = false;
    bool scheduled = false;
    float daysFrequency = 1.0f;
    float moodBelow = 1.01f;
    float joyBelow = 1.01f;
};
struct SaveDrugChemical {
    std::string chemical;  // Chemicals defName
    float tolerance = 0.0f;
    bool addicted = false;
    float need = 0.0f;
    float progress = 0.0f;
    bool armed = false;
};
struct SaveDrugDose {
    std::string drug;  // Things defName
    int64_t at = 0;
};
struct SaveDrugPawn {
    Entity pawn = kNoEntity;
    std::string policy;
    int64_t searchAfter = 0;
    std::vector<SaveDrugEntry> edits;
    std::vector<SaveDrugChemical> chemicals;
    std::vector<SaveDrugDose> last;
};

struct SaveEnvironmentHediff {
    std::string hediff;  // defName of a Hediffs def of class Environment
    float severity = 0.0f;
};

struct SaveAddedPart {  // health section 4 on (H3)
    uint16_t part = 0;
    std::string hediff;  // defName of a Hediffs def of class AddedPart
};

struct SaveOperation {  // medicine section 2 on (H3)
    uint32_t id = 0;
    std::string recipe;  // defName of a surgery Recipes def
    uint16_t part = 0;
};

struct SaveOperations {
    uint32_t nextId = 1;
    std::vector<SaveOperation> list;
};

struct SaveHealth {
    bool protagonist = false;
    RestKind rest = RestKind::Ground;
    std::vector<SaveInjury> injuries;
    std::vector<MissingPart> missing;
    std::vector<SaveAddedPart> added;  // health section 4 on
    float bloodLoss = 0.0f;
    std::vector<SaveEnvironmentHediff> environment;  // health section 2 on
    std::vector<SaveDisease> diseases;               // health section 4 on
    std::vector<SaveImmunity> immunities;            // health section 4 on
    bool dead = false;
    DeathCause cause = DeathCause::None;
    bool downed = false;  // health section 5 on: with `dirty`, so a wound not yet worked out is still pending after a load
    bool dirty = false;
};

// An item's rot and wear and what a meal was cooked from, by defName (section "spoilage").
struct SaveSpoilage {
    float rot = 0.0f;
    float decay = 0.0f;
    bool poisoned = false;
    std::vector<std::string> ingredients;  // Things defNames
};

// A bill job's delivered ingredients, by defName.
struct SaveBillUsed {
    std::string def;  // Things defName
    int count = 0;
};

// A worker's job and memory, with the work type by defName (empty for none).
struct SaveWorker {
    Job job;
    std::string workType;
    std::vector<SaveBillUsed> used;  // the job's `used`, by name (format 18 on)
    std::vector<AvoidTarget> avoid;
};

// A work building's bills, with the recipes by defName (format 18 on).
struct SaveBill {
    uint32_t id = 0;
    std::string recipe;  // Recipes defName
    BillMode mode = BillMode::Count;
    int count = 0;
    bool suspended = false;
    // Section "bills" version 2 (C14); older saves have the defaults.
    int skillMin = 0;
    int skillMax = 20;
    float radius = 0.0f;
    uint32_t storeZone = 0;
    int unpauseAt = 0;
    bool satisfied = false;
    std::vector<std::string> disallowed;  // Things defNames
};

struct SaveBillList {
    uint32_t nextId = 1;
    std::vector<SaveBill> bills;
};

struct SaveWorkPriority {
    std::string workType;  // WorkTypes defName
    uint8_t priority = 0;
    bool disabled = false;
};

struct SaveWorkSettings {
    std::vector<SaveWorkPriority> entries;
};

// A blueprint or frame, with its stuff and materials by defName.
struct SaveMaterial {
    std::string item;  // Things defName
    int needed = 0;
    int have = 0;
};

struct SaveSite {
    ConstructionStage stage = ConstructionStage::Blueprint;
    std::string stuff;  // Things defName, empty for none
    CellPos origin;
    Rot4 rotation = Rot4::North;
    int sizeX = 1;
    int sizeY = 1;
    std::vector<SaveMaterial> materials;
    float workTotal = 0.0f;
    float workDone = 0.0f;
};

struct SaveMark {};  // a building marked for deconstruction

// A projectile as saved: its defs by name.
struct SaveProjectile {
    std::string def;     // Projectiles def
    std::string weapon;  // Things def
    Projectile p;
};

// F9's explosives (section "explosives"): a grenade in flight or lying with its fuse, a pawn's belt
// and throwing arm, and a pawn knocked down by a blast. Defs by name.
struct SaveGrenade {
    std::string def;   // Projectiles def
    std::string item;  // Things def it was thrown as, empty if none
    Grenade g;
};
struct SaveBeltStack {
    std::string item;  // Things def
    int count = 0;
};
struct SaveBelt {
    std::vector<SaveBeltStack> stacks;
};
struct SaveThrower {
    std::string item;  // Things def, empty if none
    Thrower t;
};

// A colonist's recreation, comfort and outdoors needs, with the activity of its recreation job by
// defName (section "recreation").
struct SaveJoy {
    Joy joy;
    std::string activity;  // RecreationActivities defName, empty without a recreation job
};

// What C15 keeps on a thing (section "apparel"): the stuff an item is made of and its quality, taint, a
// strip order, and a pawn's outfit.
struct SaveApparel {
    std::string stuff;  // Things defName, empty for none
    int quality = -1;   // -1: none (normal)
    bool tainted = false;
    bool stripMark = false;
    bool hasOutfit = false;
    uint32_t outfit = 0;
    int64_t nextCheck = 0;
};

struct SaveEntity {
    uint32_t id = 0;
    std::optional<SaveThing> thing;
    std::optional<Position> position;
    std::optional<Footprint> footprint;
    std::optional<StackCount> stack;
    std::optional<HitPoints> hitPoints;
    std::optional<Mover> mover;
    std::optional<Colonist> colonist;  // format 4 on
    std::optional<SaveHealth> health;  // format 5 on
    std::optional<SaveWorker> worker;
    std::optional<SaveWorkSettings> work;
    std::optional<Carrying> carrying;
    bool playerControlled = false;  // format 7 on
    std::optional<Needs> needs;        // format 8 on
    std::optional<Schedule> schedule;  // format 8 on
    std::optional<Bed> bed;            // format 8 on
    std::optional<SaveSite> site;           // format 10 on
    std::optional<std::string> builtOf;     // format 10 on: the stuff a building was made of
    std::optional<SaveMark> marked;         // format 10 on
    std::optional<Mood> mood;          // format 11 on
    std::optional<Posture> posture;    // format 12 on
    std::optional<Gear> gear;
    std::optional<EquippedBy> equippedBy;
    std::optional<Shooter> shooter;
    std::optional<SaveProjectile> projectile;
    std::optional<Door> door;          // format 13 on
    std::optional<Faction> faction;  // format 15 on
    std::optional<Fighter> fighter;  // format 15 on
    std::optional<PawnLooks> looks;  // format 16 on
    std::optional<Magazine> magazine;  // format 17 on
    std::optional<SaveBillList> bills;      // format 18 on
    std::optional<PowerPart> power;         // section "power"
    std::optional<SaveJoy> joy;             // section "recreation"
    std::optional<Animal> animal;           // section "animals"
    std::optional<DefensePlan> defense;     // section "raids"
    std::optional<Raider> raider;           // section "raids"
    std::optional<Kidnapped> kidnapped;     // section "raids" 2 (F12)
    std::optional<AnimalCare> care;         // section "husbandry"
    std::optional<AnimalBreed> breed;       // section "breeding" (N4)
    std::optional<Hatching> hatching;       // section "breeding" (N4)
    std::optional<Young> young;             // section "breeding" version 3
    std::optional<Burning> burning;         // section "fire" (E4)
    std::optional<SaveSpoilage> spoilage;   // section "spoilage"
    std::optional<SaveGrenade> grenade;     // section "explosives"
    std::optional<SaveBelt> belt;           // section "explosives"
    std::optional<SaveThrower> thrower;     // section "explosives"
    std::optional<KnockedDown> knocked;     // section "explosives"
    std::optional<Melee> melee;             // section "melee" (F8)
    std::optional<Social> social;           // section "social" (C20)
    std::optional<SocialFight> socialFight; // section "social" (C20)
    std::optional<SaveApparel> apparel;     // section "apparel" (format 20 on)
    std::optional<RecoilKick> kick;         // section "feel"
    std::optional<DodgeRoll> roll;          // section "feel"
    std::optional<MedicalSettings> medical;  // section "medicine"
    std::optional<SaveOperations> operations;  // section "medicine" (H3, version 2 on)
    std::optional<std::vector<Entity>> stowed;  // section "weapons" (P1): Gear::stowed
    std::optional<WeaponSwitch> weaponSwitch;   // section "weapons"
    std::optional<Turret> turret;           // section "defense" (F11)
    std::optional<Trap> trap;               // section "defense" (F11)
    std::optional<Prisoner> prisoner;       // section "prisoners" (C22)
    std::optional<PrisonBed> prisonBed;     // section "prisoners" (C22)
    std::optional<Fuel> fuel;               // "fuel" section (C11)
    std::optional<StoreIn> storeIn;         // "crafting" section (C14)
    std::optional<MentalBreaker> mentalBreaker;  // section "mentalBreaks" (C19)
    std::optional<Visitor> visitor;         // section "trade" (C23)
    std::optional<Inspired> inspired;            // section "inspirations" (C25)
};

// A stockpile zone, with its filter by defName.
struct SaveZone {
    uint32_t id = 0;
    StoragePriority priority = StoragePriority::Normal;
    std::vector<std::string> disallowed;  // Things defNames
    std::vector<CellPos> cells;
};

// The plant layer as parallel lists, one entry per plant in cell order (format 9 on).
struct SavePlants {
    std::vector<std::string> defs;  // defNames of the plants' Things defs, in the order first met
    std::vector<uint32_t> cells;    // cell index in the map
    std::vector<uint32_t> defIndex;  // into `defs`
    std::vector<uint32_t> growth;
    std::vector<uint32_t> ageHours;
};

// The rock layer as parallel lists, one entry per rock or ore cell in cell order (format 10 on).
// Rock cells missing from it were mined.
struct SaveMineables {
    std::vector<std::string> defs;  // defNames of the rock and ore Things defs, in the order first met
    std::vector<uint32_t> cells;    // cell index in the map
    std::vector<uint32_t> defIndex;  // into `defs`
    std::vector<uint32_t> hitPoints;
};

// The player's marks on rock and plants (format 10 on), in cell order.
struct SaveDesignations {
    std::vector<uint32_t> cells;
    std::vector<uint8_t> kinds;
};

// A floor laid on a cell (section "roomstats").
struct SaveFloor {
    uint32_t cell = 0;
    std::string def;  // Terrain defName
};

// The weather: which kinds it is, when it ends, its random stream, and the snow as parallel lists, one
// entry per cell with snow in cell order (section "weather").
struct SaveWeather {
    std::string current;   // Weathers defNames, empty for none
    std::string previous;
    int64_t startedAt = 0;
    int64_t endsAt = 0;
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    std::vector<uint32_t> snowCells;  // cell index in the map
    std::vector<uint32_t> snowDepth;  // hundredths of a percent
};

struct SavePending {
    uint64_t id = 0;
    Command command;
};

struct SaveData {
    std::string simVersion;
    uint64_t defsHash = 0;
    SimSettings settings;
    int64_t seconds = 0;
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
    uint64_t groundHash = 0;
    std::optional<CellPos> stockpile;
    std::vector<uint32_t> haulers;
    uint64_t hauls = 0;
    uint64_t failedJobs = 0;
    uint64_t repaths = 0;  // format 3 on
    std::vector<std::string> setupErrors;
    uint64_t submitted = 0;
    std::vector<SavePending> pending;
    std::vector<LoggedCommand> commandLog;
    std::vector<Checkpoint> checkpoints;
    // Entity ids no thing uses at the moment, in the order the registry reuses them (the next
    // one first), so a loaded sim hands out the same ids as the one that was saved.
    std::vector<uint32_t> freeIds;
    std::vector<SaveEntity> entities;  // live things, in id order
    PathSavedState paths;              // the path queue (format 2 on)
    uint32_t nextZoneId = 1;           // format 4 on
    std::vector<SaveZone> zones;
    SavePlants plants;                 // format 10 on
    bool hasRock = false;              // the file has the rock layer (format 12 on); older saves keep the generated one
    SaveMineables mineables;           // format 12 on
    std::vector<SaveDrugPawn> drugPawns;  // section "drugs"

    SaveDesignations designations;     // format 12 on
    ResearchState research;            // section "research"
    ShipState ship;                    // section "ship"
    // Section "talents" (P3): per protagonist, parallel lists in pawn order.
    std::vector<Entity> talentPawns;
    std::vector<int64_t> talentXp;
    std::vector<std::vector<std::string>> talentNames;
    std::vector<std::vector<int32_t>> talentRanks;
    // Section "roomstats" (C17): floors, filth, what pawns carry on their shoes, beauty needs.
    bool hasRoomStats = false;
    std::vector<SaveFloor> floors;
    FilthState filth;
    RoomStatsState roomStats;
    int wildAnimals = 0;               // section "animals": SimSettings::wildAnimals
    AnimalSaved animals;               // section "animals": the system's own state
    RaidState raids;                   // section "raids" (F6)
    QuestSystemState quests;           // section "quests" (Q1)
    HusbandrySaved husbandry;          // section "husbandry": animal zones and counters
    BreedingStats breeding;            // section "breeding": the counters (N4)
    FireSaved fire;                    // section "fire": fires, burnt ground, the home area (E4)
    std::vector<RoomTemperature> roomTemperatures;  // section "temperature" (E2)
    std::vector<CellPos> upperFloors;  // section "levels" (M1), by level then row
    RoofCollapseSaved roofs;           // section "roofs" (M3)
    std::vector<OutfitPolicy> outfits;  // section "apparel"; empty: the outfits the defs make
    uint32_t nextOutfitId = 1;
    SpoilageState spoilageState;                    // section "spoilage" (C16)
    CraftingState craftingState;                    // section "crafting" (C14)
    FarmingSaved farming;                           // section "farming" (C13)
    bool hasWeather = false;           // the file has the weather section; older saves start it fresh
    SaveWeather weather;
    TradeState trade;                  // section "trade" (C23)
    bool hasNarrator = false;          // section "narrator" (T1); older saves keep the narrator off
    NarratorState narrator;
    bool hasDisasters = false;         // section "disasters" (T3); older saves have none on
    DisasterState disasters;
    WorldSettings world;               // section "world" (W2): SimSettings::world; older saves have no world
    uint64_t worldHash = 0;            // the world's hash when saved, checked against the one made again
};

// ---- Archives ------------------------------------------------------------------------------
//
// Each archive offers: value (numbers, bools, enums), text, note (JSON only), group, list,
// optional, section, require (checks while reading), kReading, version() (the file's format) and
// sectionVersion() (the version of the section being read or written; 1 in formats 6 to 19).

class SaveBinaryWriter {
public:
    static constexpr bool kReading = false;
    uint32_t version() const { return kSaveFormatVersion; }
    uint32_t sectionVersion() const { return sectionVersion_; }

    template <typename T>
    void value(const char*, T& v) {
        put(v);
    }
    void text(const char*, std::string& s) {
        put(static_cast<uint32_t>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }
    void note(const char*, const char*) {}
    template <typename F>
    void group(const char*, F fn) {
        fn();
    }
    template <typename T, typename F>
    void list(const char*, std::vector<T>& items, F fn) {
        put(static_cast<uint32_t>(items.size()));
        for (T& item : items) {
            fn(item);
        }
    }
    template <typename T, typename F>
    void optional(const char*, std::optional<T>& item, F fn) {
        put(item.has_value());
        if (item) {
            fn(*item);
        }
    }
    void require(bool, const char*) {}
    // A section: its name, its version, its size in bytes, then what `fn` writes.
    template <typename F>
    void section(const char* name, uint32_t current, F fn) {
        std::string title = name;
        text(nullptr, title);
        put(current);
        const size_t sizeAt = bytes_.size();
        put(uint32_t{0});
        const uint32_t outer = sectionVersion_;
        sectionVersion_ = current;
        fn();
        sectionVersion_ = outer;
        const uint32_t size = static_cast<uint32_t>(bytes_.size() - sizeAt - sizeof(uint32_t));
        for (size_t k = 0; k < sizeof(uint32_t); ++k) {
            bytes_[sizeAt + k] = static_cast<uint8_t>((size >> (8 * k)) & 0xFF);
        }
    }

    std::vector<uint8_t>& bytes() { return bytes_; }

private:
    template <typename T>
    void put(T v) {
        if constexpr (std::is_enum_v<T>) {
            put(static_cast<std::underlying_type_t<T>>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
            bytes_.push_back(v ? 1 : 0);
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            Bits bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            put(bits);
        } else {
            static_assert(std::is_integral_v<T>, "save numbers, bools and enums");
            const auto bits = static_cast<std::make_unsigned_t<T>>(v);
            for (size_t k = 0; k < sizeof(T); ++k) {
                bytes_.push_back(static_cast<uint8_t>((static_cast<uint64_t>(bits) >> (8 * k)) & 0xFF));
            }
        }
    }

    std::vector<uint8_t> bytes_;
    uint32_t sectionVersion_ = 0;
};

class SaveBinaryReader {
public:
    static constexpr bool kReading = true;

    SaveBinaryReader(const uint8_t* data, size_t size, uint32_t version) : data_(data), size_(size), version_(version) {}

    uint32_t version() const { return version_; }
    uint32_t sectionVersion() const { return version_ >= kSaveSectionsFormat ? sectionVersion_ : kSaveLegacySectionVersion; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    bool atEnd() const { return pos_ == size_; }
    void fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message;
        }
    }

    // Format 20 on: reads the list of sections (name, version, size, bytes) up to the end, so
    // section() can find each by name whatever order the file has them in.
    void readSections() {
        while (!failed() && !atEnd()) {
            SaveSectionSpan span;
            text(nullptr, span.name);
            get(span.version);
            uint32_t size = 0;
            get(size);
            if (failed() || size > size_ - pos_) {
                fail("the save file is cut short");
                return;
            }
            for (const SaveSectionSpan& other : sections_) {
                if (other.name == span.name) {
                    fail("the save file is damaged (it has two '" + span.name + "' sections)");
                    return;
                }
            }
            span.offset = pos_;
            span.size = size;
            pos_ += size;
            sections_.push_back(std::move(span));
        }
    }
    // Reads one section with `fn`. A section the file doesn't have is skipped and keeps its
    // defaults: the file is from before that section existed.
    template <typename F>
    void section(const char* name, uint32_t newest, F fn) {
        if (failed()) {
            return;
        }
        SaveSectionSpan* span = nullptr;
        for (SaveSectionSpan& s : sections_) {
            if (s.name == name) {
                span = &s;
            }
        }
        if (span == nullptr) {
            return;
        }
        span->read = true;
        if (span->version == 0 || span->version > newest) {
            fail("this save's '" + span->name + "' section is version " + std::to_string(span->version) +
                 ", made by a newer version of the game (this one reads up to " + std::to_string(newest) + ")");
            return;
        }
        const uint8_t* const outerData = data_;
        const size_t outerSize = size_;
        const size_t outerPos = pos_;
        data_ = outerData + span->offset;
        size_ = span->size;
        pos_ = 0;
        sectionVersion_ = span->version;
        fn();
        if (!failed() && !atEnd()) {
            fail("the save file is damaged (its '" + span->name + "' section has extra data)");
        }
        data_ = outerData;
        size_ = outerSize;
        pos_ = outerPos;
        sectionVersion_ = 0;
    }
    // A section no section() call asked for: the file was made by a newer version of the game.
    const std::string* unknownSection() const {
        for (const SaveSectionSpan& s : sections_) {
            if (!s.read) {
                return &s.name;
            }
        }
        return nullptr;
    }

    template <typename T>
    void value(const char*, T& v) {
        get(v);
    }
    void text(const char*, std::string& s) {
        uint32_t length = 0;
        get(length);
        if (failed() || length > size_ - pos_) {
            fail("the save file is cut short");
            s.clear();
            return;
        }
        s.assign(reinterpret_cast<const char*>(data_ + pos_), length);
        pos_ += length;
    }
    void note(const char*, const char*) {}
    template <typename F>
    void group(const char*, F fn) {
        fn();
    }
    template <typename T, typename F>
    void list(const char*, std::vector<T>& items, F fn) {
        uint32_t count = 0;
        get(count);
        // Every element takes at least a byte, so a bigger count is a damaged file; checking
        // keeps a bad count from asking for gigabytes.
        if (failed() || count > size_ - pos_) {
            fail("the save file is cut short");
            items.clear();
            return;
        }
        items.clear();
        items.resize(count);
        for (T& item : items) {
            fn(item);
            if (failed()) {
                return;
            }
        }
    }
    template <typename T, typename F>
    void optional(const char*, std::optional<T>& item, F fn) {
        bool present = false;
        get(present);
        item.reset();
        if (present && !failed()) {
            item.emplace();
            fn(*item);
        }
    }
    void require(bool ok, const char* message) {
        if (!ok) {
            fail(message);
        }
    }

private:
    template <typename T>
    void get(T& v) {
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            get(raw);
            v = static_cast<T>(raw);
        } else if constexpr (std::is_same_v<T, bool>) {
            uint8_t raw = 0;
            get(raw);
            if (raw > 1) {
                fail("the save file is damaged (a yes/no value is neither)");
            }
            v = raw == 1;
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            Bits bits = 0;
            get(bits);
            std::memcpy(&v, &bits, sizeof bits);
        } else {
            static_assert(std::is_integral_v<T>, "save numbers, bools and enums");
            if (sizeof(T) > size_ - pos_) {
                fail("the save file is cut short");
                pos_ = size_;
                v = T{};
                return;
            }
            uint64_t bits = 0;
            for (size_t k = 0; k < sizeof(T); ++k) {
                bits |= static_cast<uint64_t>(data_[pos_ + k]) << (8 * k);
            }
            pos_ += sizeof(T);
            v = static_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
        }
    }

    struct SaveSectionSpan {
        std::string name;
        uint32_t version = 0;
        size_t offset = 0;
        size_t size = 0;
        bool read = false;
    };

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t version_;
    uint32_t sectionVersion_ = 0;
    std::vector<SaveSectionSpan> sections_;
    std::string error_;
};

class SaveJsonWriter {
public:
    static constexpr bool kReading = false;

    explicit SaveJsonWriter(SaveJson& root) : cur_(&root) {}
    uint32_t version() const { return kSaveFormatVersion; }
    uint32_t sectionVersion() const { return sectionVersion_; }

    template <typename T>
    void value(const char* name, T& v) {
        if constexpr (std::is_enum_v<T>) {
            slot(name) = static_cast<std::underlying_type_t<T>>(v);
        } else {
            slot(name) = v;
        }
    }
    void text(const char* name, std::string& s) { slot(name) = s; }
    void note(const char* name, const char* s) { slot(name) = s; }
    template <typename F>
    void group(const char* name, F fn) {
        SaveJson* saved = cur_;
        cur_ = &slot(name);
        if (name != nullptr) {
            *cur_ = SaveJson::object();
        }
        fn();
        cur_ = saved;
    }
    template <typename T, typename F>
    void list(const char* name, std::vector<T>& items, F fn) {
        SaveJson& array = slot(name);
        array = SaveJson::array();
        SaveJson* saved = cur_;
        for (T& item : items) {
            array.push_back(nullptr);
            cur_ = &array.back();
            fn(item);
        }
        cur_ = saved;
    }
    template <typename T, typename F>
    void optional(const char* name, std::optional<T>& item, F fn) {
        if (item) {
            group(name, [&] { fn(*item); });
        }
    }
    void require(bool, const char*) {}
    template <typename F>
    void section(const char* name, uint32_t current, F fn) {
        group(name, [&] {
            slot("version") = current;
            sectionVersion_ = current;
            fn();
            sectionVersion_ = 0;
        });
    }

private:
    SaveJson& slot(const char* name) { return name != nullptr ? (*cur_)[name] : *cur_; }

    SaveJson* cur_;
    uint32_t sectionVersion_ = 0;
};

// ---- What the save holds, field by field ------------------------------------------------

template <typename Ar>
void saveCell(Ar& ar, CellPos& c) {
    ar.value("x", c.x);
    ar.value("y", c.y);
    ar.value("level", c.level);
}

template <typename Ar>
void saveRect(Ar& ar, CellRect& r) {
    ar.value("minX", r.minX);
    ar.value("minY", r.minY);
    ar.value("sizeX", r.sizeX);
    ar.value("sizeY", r.sizeY);
    ar.value("level", r.level);
    ar.require(r.sizeX >= 0 && r.sizeY >= 0 && r.sizeX <= 4096 && r.sizeY <= 4096, "the save file has a bad area");
}

// Commands by name (format 20 on): the commands part of a save starts with the names of the
// command types it uses, and each command gives its type as a place in that list. Formats 6 to 19
// gave the type's place in the `Command` variant, which shifted whenever two tasks added commands
// at once; that table is kept below for them and never grows.
struct SaveCommandNames {
    std::vector<std::string> names;
    std::vector<Command> blanks;        // reading: a command of each named type, fields unset
    std::vector<uint16_t> indexOfType;  // writing: for each variant index, its place in `names`
};

template <typename>
inline constexpr bool kSaveNoSuchCommand = false;

// Second half of the command chain, split out because MSVC stops at 127 nested else-if blocks (C1061).
template <typename Ar, typename Type>
void saveCommandTail(Ar& ar, Type& c) {
    if constexpr (std::is_same_v<Type, StartRaidCommand>) {
        ar.text("faction", c.faction);
        ar.value("points", c.points);
        ar.value("edge", c.edge);
    } else if constexpr (std::is_same_v<Type, SoundAlarmCommand>) {
        ar.value("on", c.on);
    } else if constexpr (std::is_same_v<Type, SetDefenseRoleCommand>) {
        ar.value("pawn", c.pawn);
        ar.value("role", c.role);
        ar.value("hasSpot", c.hasSpot);
        ar.group("spot", [&] { saveCell(ar, c.spot); });
    } else if constexpr (std::is_same_v<Type, MarkShelterCommand>) {
        ar.group("cell", [&] { saveCell(ar, c.cell); });
        ar.value("clear", c.clear);
    } else if constexpr (std::is_same_v<Type, MarkAnimalCommand>) {
        ar.value("animal", c.animal);
        ar.text("mark", c.mark);
        ar.value("on", c.on);
    } else if constexpr (std::is_same_v<Type, MarkAnimalsInAreaCommand>) {
        ar.group("area", [&] { saveRect(ar, c.area); });
        ar.text("mark", c.mark);
        ar.value("on", c.on);
    } else if constexpr (std::is_same_v<Type, SetAnimalNameCommand>) {
        ar.value("animal", c.animal);
        ar.text("name", c.name);
    } else if constexpr (std::is_same_v<Type, SetTrainingCommand>) {
        ar.value("animal", c.animal);
        ar.text("trainable", c.trainable);
        ar.value("wanted", c.wanted);
    } else if constexpr (std::is_same_v<Type, CreateAnimalZoneCommand> || std::is_same_v<Type, RemoveAnimalZoneCellsCommand>) {
        ar.group("area", [&] { saveRect(ar, c.area); });
    } else if constexpr (std::is_same_v<Type, AssignAnimalZoneCommand>) {
        ar.value("animal", c.animal);
        ar.value("zone", c.zone);
    } else if constexpr (std::is_same_v<Type, SetBreedingCommand>) {
        ar.value("animal", c.animal);
        ar.value("zone", c.zone);
        ar.value("allow", c.allow);
    } else if constexpr (std::is_same_v<Type, MeleeCommand>) {
        auto* melee = &c;
        ar.value("pawn", melee->pawn);
        ar.value("target", melee->target);
        ar.value("mode", melee->mode);
        ar.require(melee->mode <= MeleeMode::Block, "the save file has a bad melee mode");
    } else if constexpr (std::is_same_v<Type, SetOutfitCommand>) {
        auto* setOutfit = &c;
        ar.value("pawn", setOutfit->pawn);
        ar.value("outfit", setOutfit->outfit);
    } else if constexpr (std::is_same_v<Type, EditOutfitCommand>) {
        auto* edit = &c;
        ar.value("outfit", edit->outfit);
        ar.list("disallowed", edit->disallowed, [&](std::string& name) { ar.text(nullptr, name); });
        ar.value("hitPointsMin", edit->hitPointsMin);
        ar.value("hitPointsMax", edit->hitPointsMax);
        ar.value("qualityMin", edit->qualityMin);
        ar.value("qualityMax", edit->qualityMax);
    } else if constexpr (std::is_same_v<Type, AddOutfitCommand>) {
        auto* addOutfit = &c;
        ar.text("name", addOutfit->name);
    } else if constexpr (std::is_same_v<Type, RemoveOutfitCommand>) {
        auto* removeOutfit = &c;
        ar.value("outfit", removeOutfit->outfit);
    } else if constexpr (std::is_same_v<Type, StripCommand>) {
        auto* strip = &c;
        ar.value("thing", strip->thing);
        ar.value("strip", strip->strip);
    } else if constexpr (std::is_same_v<Type, InteractCommand>) {
        ar.value("initiator", c.initiator);
        ar.value("recipient", c.recipient);
        ar.text("interaction", c.interaction);
        ar.value("outcome", c.outcome);
    } else if constexpr (std::is_same_v<Type, DodgeRollCommand>) {
        ar.value("pawn", c.pawn);
        ar.value("dx", c.dx);
        ar.value("dy", c.dy);
    } else if constexpr (std::is_same_v<Type, SetMedicalCareCommand>) {
        ar.value("pawn", c.pawn);
        ar.text("care", c.care);
    } else if constexpr (std::is_same_v<Type, ContractDiseaseCommand>) {
        ar.value("pawn", c.pawn);
        ar.text("disease", c.disease);
        ar.value("severity", c.severity);
        ar.value("part", c.part);
    } else if constexpr (std::is_same_v<Type, SetWeatherCommand>) {
        auto* setWeather = &c;
        ar.text("weather", setWeather->weather);
        ar.value("hours", setWeather->hours);
    } else if constexpr (std::is_same_v<Type, SpendTalentPointCommand>) {
        ar.value("pawn", c.pawn);
        ar.text("talent", c.talent);
    } else if constexpr (std::is_same_v<Type, AddTalentXpCommand>) {
        ar.value("pawn", c.pawn);
        ar.value("amount", c.amount);
    } else if constexpr (std::is_same_v<Type, RearmTrapCommand>) {
        ar.value("trap", c.trap);
    } else if constexpr (std::is_same_v<Type, CarryWeaponCommand>) {
        ar.value("pawn", c.pawn);
        ar.value("item", c.item);
    } else if constexpr (std::is_same_v<Type, SwitchWeaponCommand>) {
        ar.value("pawn", c.pawn);
        ar.value("slot", c.slot);
        ar.require(static_cast<int>(c.slot) < kWeaponSlotCount, "the save file has a bad weapon slot");
    } else if constexpr (std::is_same_v<Type, CreateGrowingZoneCommand>) {
        ar.group("area", [&] { saveRect(ar, c.area); });
        ar.text("crop", c.crop);
    } else if constexpr (std::is_same_v<Type, RemoveGrowingZoneCellsCommand>) {
        ar.group("area", [&] { saveRect(ar, c.area); });
    } else if constexpr (std::is_same_v<Type, SetGrowingZoneCropCommand>) {
        ar.value("zone", c.zone);
        ar.text("crop", c.crop);
    } else if constexpr (std::is_same_v<Type, SetNarratorCommand>) {
        ar.text("narrator", c.narrator);
        ar.text("difficulty", c.difficulty);
    } else if constexpr (std::is_same_v<Type, FireIncidentCommand>) {
        ar.text("incident", c.incident);
        ar.value("points", c.points);
    } else if constexpr (std::is_same_v<Type, IgniteCommand>) {
        ar.group("cell", [&] { saveCell(ar, c.cell); });
        ar.value("size", c.size);
        ar.value("pawn", c.pawn);
        ar.value("force", c.force);
    } else if constexpr (std::is_same_v<Type, SetHomeAreaCommand>) {
        ar.group("area", [&] { saveRect(ar, c.area); });
        ar.value("add", c.add);
    } else if constexpr (std::is_same_v<Type, SetPrisonerModeCommand>) {
        ar.value("pawn", c.pawn);
        ar.text("mode", c.mode);
    } else if constexpr (std::is_same_v<Type, SetPrisonBedCommand>) {
        ar.value("bed", c.bed);
        ar.value("on", c.on);
    } else if constexpr (std::is_same_v<Type, PlaceUpperFloorCommand>) {
        ar.group("area", [&] { saveRect(ar, c.area); });
        ar.value("remove", c.remove);
    } else if constexpr (std::is_same_v<Type, StartMentalBreakCommand>) {
        auto* start = &c;
        ar.value("pawn", start->pawn);
        ar.text("breakDef", start->breakDef);
    } else if constexpr (std::is_same_v<Type, EndMentalBreakCommand>) {
        auto* stop = &c;
        ar.value("pawn", stop->pawn);
    } else if constexpr (std::is_same_v<Type, StartStrategyRaidCommand>) {
        ar.text("faction", c.faction);
        ar.value("points", c.points);
        ar.value("edge", c.edge);
        ar.text("strategy", c.strategy);
        ar.value("hour", c.hour);
    } else if constexpr (std::is_same_v<Type, ShipCommand>) {
        ar.text("action", c.action);
        ar.value("thing", c.thing);
        ar.value("pawn", c.pawn);
    } else if constexpr (std::is_same_v<Type, TakeDrugCommand>) {
        ar.value("pawn", c.pawn);
        ar.value("thing", c.thing);
    } else if constexpr (std::is_same_v<Type, SetDrugPolicyCommand>) {
        ar.value("pawn", c.pawn);
        ar.text("policy", c.policy);
    } else if constexpr (std::is_same_v<Type, SetDrugPolicyEntryCommand>) {
        ar.value("pawn", c.pawn);
        ar.text("drug", c.drug);
        ar.value("forAddiction", c.forAddiction);
        ar.value("forJoy", c.forJoy);
        ar.value("scheduled", c.scheduled);
        ar.value("daysFrequency", c.daysFrequency);
        ar.value("moodBelow", c.moodBelow);
        ar.value("joyBelow", c.joyBelow);
    } else if constexpr (std::is_same_v<Type, StartThreatCommand>) {
        ar.text("kind", c.kind);
        ar.value("points", c.points);
        ar.value("edge", c.edge);
    } else if constexpr (std::is_same_v<Type, QuestCommand>) {
        ar.text("action", c.action);
        ar.value("quest", c.quest);
        ar.text("kind", c.kind);
    } else {
        static_assert(kSaveNoSuchCommand<Type>, "save every command type here");
    }
}

template <typename Ar>
void saveTradeLine(Ar& ar, TradeLine& l) {
    ar.text("thing", l.thing);
    ar.value("count", l.count);
}

template <typename Type>
inline constexpr bool kSaveIsTradeCommand = std::is_same_v<Type, StartVisitCommand> || std::is_same_v<Type, TradeCommand> ||
                                            std::is_same_v<Type, AdjustGoodwillCommand> || std::is_same_v<Type, GiftCommand>;

// The trade commands, apart from saveCommand's long chain (MSVC stops at 128 nested else-ifs).
template <typename Ar, typename Type>
void saveTradeCommand(Ar& ar, Type& c) {
    if constexpr (std::is_same_v<Type, StartVisitCommand>) {
        auto* visit = &c;
        ar.text("faction", visit->faction);
        ar.text("kind", visit->kind);
        ar.text("traderKind", visit->traderKind);
        ar.value("edge", visit->edge);
    } else if constexpr (std::is_same_v<Type, TradeCommand>) {
        auto* deal = &c;
        ar.value("visit", deal->visit);
        ar.value("negotiator", deal->negotiator);
        ar.list("buy", deal->buy, [&](TradeLine& l) { saveTradeLine(ar, l); });
        ar.list("sell", deal->sell, [&](TradeLine& l) { saveTradeLine(ar, l); });
    } else if constexpr (std::is_same_v<Type, AdjustGoodwillCommand>) {
        auto* goodwill = &c;
        ar.text("faction", goodwill->faction);
        ar.value("delta", goodwill->delta);
    } else {
        static_assert(std::is_same_v<Type, GiftCommand>, "save every trade command here");
        auto* gift = &c;
        ar.value("visit", gift->visit);
        ar.list("items", gift->items, [&](TradeLine& l) { saveTradeLine(ar, l); });
    }
}

template <typename Ar>
void saveCommand(Ar& ar, Command& command, SaveCommandNames* names) {
    if (names != nullptr) {
        uint16_t type = 0;
        if constexpr (!Ar::kReading) {
            type = names->indexOfType[command.index()];
        }
        ar.value("type", type);
        if constexpr (Ar::kReading) {
            if (type >= names->blanks.size()) {
                ar.require(false, "the save file has a command of a type it doesn't name");
                return;
            }
            command = names->blanks[type];
        }
    } else {
        // Formats 6 to 19, read only: the type's place in the variant as it was then.
        uint8_t type = static_cast<uint8_t>(command.index());
        ar.value("type", type);
        if constexpr (Ar::kReading) {
            switch (type) {
                case 0: command = SpawnThingCommand{}; break;
                case 1: command = DestroyThingCommand{}; break;
                case 2: command = DamagePawnCommand{}; break;
                case 3: command = TendPawnCommand{}; break;
                case 4: command = SetWorkPriorityCommand{}; break;
                case 5: command = CreateStockpileCommand{}; break;
                case 6: command = RemoveZoneCellsCommand{}; break;
                case 7: command = SetStockpilePriorityCommand{}; break;
                case 8: command = SetStockpileAllowedCommand{}; break;
                case 9: command = OrderHaulCommand{}; break;
                case 10: command = ControlPawnCommand{}; break;
                case 11: command = DirectMoveCommand{}; break;
                case 12: command = SetScheduleCommand{}; break;
                case 13: command = AssignBedCommand{}; break;
                case 14: command = PlaceBlueprintCommand{}; break;
                case 15: command = CancelBuildCommand{}; break;
                case 16: command = DeconstructCommand{}; break;
                case 17: command = EquipCommand{}; break;
                case 18: command = ShootCommand{}; break;
                case 19: command = SetStanceCommand{}; break;
                case 20: command = DesignateCommand{}; break;
                case 21: command = CancelDesignationsCommand{}; break;
                case 22: command = DraftCommand{}; break;
                case 23: command = MoveOrderCommand{}; break;
                case 24: command = AttackOrderCommand{}; break;
                case 25: command = SetFactionCommand{}; break;
                case 26: command = SetLineageCommand{}; break;
                case 27: command = ReloadCommand{}; break;
                case 28: command = AddBillCommand{}; break;
                case 29: command = RemoveBillCommand{}; break;
                case 30: command = SetBillCommand{}; break;
                case 31: command = HandsOnCommand{}; break;
                default: ar.require(false, "the save file names a command this build doesn't know"); return;
            }
            // A format older than the command that was added in it can't contain it.
            ar.require((type < 10 || ar.version() >= 7) && (type < 12 || ar.version() >= 8) &&
                           (type < 14 || ar.version() >= 10) && (type < 17 || ar.version() >= 12) && (type < 20 || ar.version() >= 14) &&
                           (type < 22 || ar.version() >= 15) && (type < 26 || ar.version() >= 16) &&
                           (type < 27 || ar.version() >= 17) && (type < 28 || ar.version() >= 18) && (type < 31 || ar.version() >= 19),
                       "the save file names a command this build doesn't know");
        }
    }
    ar.note("name", commandName(command));
    std::visit(
        [&](auto& c) {
            using Type = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<Type, SpawnThingCommand>) {
                auto* spawn = &c;
                ar.text("def", spawn->def);
                ar.group("cell", [&] { saveCell(ar, spawn->cell); });
                ar.value("count", spawn->count);
                ar.value("rot", spawn->rot);
                ar.require(static_cast<int>(spawn->rot) <= static_cast<int>(Rot4::West), "the save file has a bad rotation");
            } else if constexpr (std::is_same_v<Type, DestroyThingCommand>) {
                auto* destroy = &c;
                ar.value("thing", destroy->thing);
            } else if constexpr (std::is_same_v<Type, DamagePawnCommand>) {
                auto* damage = &c;
                ar.value("pawn", damage->pawn);
                ar.text("damageType", damage->damageType);
                ar.value("amount", damage->amount);
                ar.value("part", damage->part);
            } else if constexpr (std::is_same_v<Type, TendPawnCommand>) {
                auto* tend = &c;
                ar.value("pawn", tend->pawn);
                ar.value("quality", tend->quality);
            } else if constexpr (std::is_same_v<Type, SetWorkPriorityCommand>) {
                auto* work = &c;
                ar.value("pawn", work->pawn);
                ar.text("workType", work->workType);
                ar.value("priority", work->priority);
            } else if constexpr (std::is_same_v<Type, CreateStockpileCommand>) {
                auto* create = &c;
                ar.group("area", [&] { saveRect(ar, create->area); });
                ar.value("priority", create->priority);
                ar.require(create->priority <= StoragePriority::Critical, "the save file has a bad storage priority");
            } else if constexpr (std::is_same_v<Type, RemoveZoneCellsCommand>) {
                auto* remove = &c;
                ar.group("area", [&] { saveRect(ar, remove->area); });
            } else if constexpr (std::is_same_v<Type, SetStockpilePriorityCommand>) {
                auto* priority = &c;
                ar.value("zone", priority->zone);
                ar.value("priority", priority->priority);
                ar.require(priority->priority <= StoragePriority::Critical, "the save file has a bad storage priority");
            } else if constexpr (std::is_same_v<Type, SetStockpileAllowedCommand>) {
                auto* allowed = &c;
                ar.value("zone", allowed->zone);
                ar.text("def", allowed->def);
                ar.value("allowed", allowed->allowed);
            } else if constexpr (std::is_same_v<Type, OrderHaulCommand>) {
                auto* haul = &c;
                ar.value("pawn", haul->pawn);
                ar.value("thing", haul->thing);
            } else if constexpr (std::is_same_v<Type, ControlPawnCommand>) {
                auto* control = &c;
                ar.value("pawn", control->pawn);
            } else if constexpr (std::is_same_v<Type, DirectMoveCommand>) {
                auto* move = &c;
                ar.value("pawn", move->pawn);
                ar.value("x", move->x);
                ar.value("y", move->y);
            } else if constexpr (std::is_same_v<Type, SetScheduleCommand>) {
                auto* schedule = &c;
                ar.value("pawn", schedule->pawn);
                ar.value("hour", schedule->hour);
                ar.value("kind", schedule->kind);
                ar.require(schedule->kind <= ScheduleKind::Sleep, "the save file has a bad schedule kind");
            } else if constexpr (std::is_same_v<Type, AssignBedCommand>) {
                auto* assign = &c;
                ar.value("bed", assign->bed);
                ar.value("pawn", assign->pawn);
            } else if constexpr (std::is_same_v<Type, PlaceBlueprintCommand>) {
                auto* place = &c;
                ar.text("def", place->def);
                ar.text("stuff", place->stuff);
                ar.group("cell", [&] { saveCell(ar, place->cell); });
                ar.value("rot", place->rot);
                ar.require(static_cast<int>(place->rot) <= static_cast<int>(Rot4::West), "the save file has a bad rotation");
            } else if constexpr (std::is_same_v<Type, CancelBuildCommand>) {
                auto* cancel = &c;
                ar.value("thing", cancel->thing);
            } else if constexpr (std::is_same_v<Type, DeconstructCommand>) {
                auto* deconstruct = &c;
                ar.value("thing", deconstruct->thing);
            } else if constexpr (std::is_same_v<Type, EquipCommand>) {
                auto* equip = &c;
                ar.value("pawn", equip->pawn);
                ar.value("item", equip->item);
            } else if constexpr (std::is_same_v<Type, ShootCommand>) {
                auto* shoot = &c;
                ar.value("pawn", shoot->pawn);
                ar.value("target", shoot->target);
                ar.value("x", shoot->x);
                ar.value("y", shoot->y);
                ar.value("z", shoot->z);
                ar.value("player", shoot->player);
            } else if constexpr (std::is_same_v<Type, SetStanceCommand>) {
                auto* stance = &c;
                ar.value("pawn", stance->pawn);
                ar.value("stance", stance->stance);
                ar.require(static_cast<int>(stance->stance) <= static_cast<int>(Stance::Prone), "the save file has a bad stance");
            } else if constexpr (std::is_same_v<Type, DesignateCommand>) {
                auto* designate = &c;
                ar.group("area", [&] { saveRect(ar, designate->area); });
                ar.value("kind", designate->kind);
                ar.require(designate->kind >= DesignationKind::Mine && designate->kind <= DesignationKind::HarvestPlant,
                       "the save file has a bad designation kind");
            } else if constexpr (std::is_same_v<Type, CancelDesignationsCommand>) {
                auto* cancelMarks = &c;
                ar.group("area", [&] { saveRect(ar, cancelMarks->area); });
                ar.value("kind", cancelMarks->kind);
                ar.require(cancelMarks->kind <= DesignationKind::HarvestPlant, "the save file has a bad designation kind");
            } else if constexpr (std::is_same_v<Type, DraftCommand>) {
                auto* draft = &c;
                ar.value("pawn", draft->pawn);
                ar.value("drafted", draft->drafted);
            } else if constexpr (std::is_same_v<Type, MoveOrderCommand>) {
                auto* moveOrder = &c;
                ar.value("pawn", moveOrder->pawn);
                ar.group("cell", [&] { saveCell(ar, moveOrder->cell); });
            } else if constexpr (std::is_same_v<Type, AttackOrderCommand>) {
                auto* attack = &c;
                ar.value("pawn", attack->pawn);
                ar.value("target", attack->target);
            } else if constexpr (std::is_same_v<Type, SetFactionCommand>) {
                auto* faction = &c;
                ar.value("pawn", faction->pawn);
                ar.value("faction", faction->faction);
            } else if constexpr (std::is_same_v<Type, SetLineageCommand>) {
                auto* lineage = &c;
                ar.value("pawn", lineage->pawn);
                ar.text("lineage", lineage->lineage);
            } else if constexpr (std::is_same_v<Type, ReloadCommand>) {
                auto* reload = &c;
                ar.value("pawn", reload->pawn);
            } else if constexpr (std::is_same_v<Type, AddBillCommand>) {
                auto* addBill = &c;
                ar.value("building", addBill->building);
                ar.text("recipe", addBill->recipe);
                ar.value("mode", addBill->mode);
                ar.require(addBill->mode <= BillMode::Forever, "the save file has a bad bill mode");
                ar.value("count", addBill->count);
            } else if constexpr (std::is_same_v<Type, RemoveBillCommand>) {
                auto* removeBill = &c;
                ar.value("building", removeBill->building);
                ar.value("bill", removeBill->bill);
            } else if constexpr (std::is_same_v<Type, SetBillCommand>) {
                auto* setBill = &c;
                ar.value("building", setBill->building);
                ar.value("bill", setBill->bill);
                ar.value("mode", setBill->mode);
                ar.require(setBill->mode <= BillMode::Forever, "the save file has a bad bill mode");
                ar.value("count", setBill->count);
                ar.value("suspended", setBill->suspended);
            } else if constexpr (std::is_same_v<Type, PlaceFloorCommand>) {
                auto* place = &c;
                ar.group("area", [&] { saveRect(ar, place->area); });
                ar.text("floor", place->floor);
            } else if constexpr (std::is_same_v<Type, AddFilthCommand>) {
                auto* add = &c;
                ar.group("cell", [&] { saveCell(ar, add->cell); });
                ar.text("kind", add->kind);
                ar.value("layers", add->layers);
            } else if constexpr (std::is_same_v<Type, HandsOnCommand>) {
                auto* hands = &c;
                ar.value("pawn", hands->pawn);
                ar.value("mode", hands->mode);
                ar.require(hands->mode <= HandsOnMode::Stop, "the save file has a bad hands-on mode");
                ar.value("thing", hands->thing);
                ar.group("cell", [&] { saveCell(ar, hands->cell); });
            } else if constexpr (std::is_same_v<Type, FlickCommand>) {
                ar.value("thing", c.thing);
                ar.value("on", c.on);
            } else if constexpr (std::is_same_v<Type, ShortCircuitCommand>) {
                ar.value("thing", c.thing);
            } else if constexpr (std::is_same_v<Type, SetBillOptionsCommand>) {
                ar.value("building", c.building);
                ar.value("bill", c.bill);
                ar.value("skillMin", c.skillMin);
                ar.value("skillMax", c.skillMax);
                ar.value("radius", c.radius);
                ar.value("storeZone", c.storeZone);
                ar.value("unpauseAt", c.unpauseAt);
                ar.list("disallowed", c.disallowed, [&](std::string& name) { ar.text(nullptr, name); });
            } else if constexpr (std::is_same_v<Type, MoveBillCommand>) {
                ar.value("building", c.building);
                ar.value("bill", c.bill);
                ar.value("index", c.index);
            } else if constexpr (std::is_same_v<Type, AddOperationCommand>) {
                ar.value("pawn", c.pawn);
                ar.text("recipe", c.recipe);
                ar.value("part", c.part);
            } else if constexpr (std::is_same_v<Type, RemoveOperationCommand>) {
                ar.value("pawn", c.pawn);
                ar.value("operation", c.operation);
            } else if constexpr (std::is_same_v<Type, SetRefuelCommand>) {
                auto* refuel = &c;
                ar.value("building", refuel->building);
                ar.value("autoRefuel", refuel->autoRefuel);
                ar.value("fillNow", refuel->fillNow);
            } else if constexpr (std::is_same_v<Type, SetResearchCommand> || std::is_same_v<Type, UnlockResearchCommand>) {
                ar.text("project", c.project);
            } else if constexpr (std::is_same_v<Type, ThrowCommand>) {
                ar.value("pawn", c.pawn);
                ar.text("item", c.item);
                ar.value("target", c.target);
                ar.value("x", c.x);
                ar.value("y", c.y);
                ar.value("player", c.player);
            } else if constexpr (std::is_same_v<Type, ExplodeCommand>) {
                ar.value("x", c.x);
                ar.value("y", c.y);
                ar.value("radius", c.radius);
                ar.value("damage", c.damage);
                ar.text("damageType", c.damageType);
                ar.value("penetration", c.penetration);
            } else if constexpr (kSaveIsTradeCommand<Type>) {
                saveTradeCommand(ar, c);
            } else {
                saveCommandTail(ar, c);
            }
        },
        command);
}

template <typename Ar>
void saveSettings(Ar& ar, SimSettings& s) {
    ar.value("seed", s.seed);
    ar.value("mapSize", s.mapSize);
    ar.value("pawns", s.pawns);
    ar.value("items", s.items);
    ar.value("flatMap", s.flatMap);
    ar.text("pawnDef", s.pawnDef);
    ar.text("haulItemDef", s.haulItemDef);
    ar.value("checkpointEvery", s.checkpointEvery);
    if (ar.version() >= 2) {
        ar.value("pathBudget", s.pathBudget);
    }
    if (ar.version() >= 4) {
        ar.text("scenario", s.scenario);
    }
}

template <typename Ar>
void saveColonist(Ar& ar, Colonist& c) {
    ar.text("firstName", c.firstName);
    ar.text("nickName", c.nickName);
    ar.text("lastName", c.lastName);
    ar.value("female", c.female);
    ar.value("age", c.age);
    ar.text("childhood", c.childhood);
    ar.text("adulthood", c.adulthood);
    ar.list("traits", c.traits, [&](TraitRecord& t) {
        ar.text("def", t.def);
        ar.value("degree", t.degree);
    });
    ar.list("skills", c.skills, [&](SkillRecord& s) {
        ar.text("def", s.def);
        ar.value("level", s.level);
        ar.value("passion", s.passion);
        ar.value("xp", s.xp);
        if (ar.sectionVersion() >= 2) {
            ar.value("xpToday", s.xpToday);
        }
        ar.value("disabled", s.disabled);
        ar.require(static_cast<int>(s.passion) <= static_cast<int>(Passion::Major), "the save file has a bad passion");
    });
    ar.list("disabledWorkTags", c.disabledWorkTags, [&](std::string& tag) { ar.text(nullptr, tag); });
    ar.value("protagonist", c.protagonist);
}

template <typename Ar>
void saveLooksColor(Ar& ar, const char* name, LooksColor& c) {
    ar.group(name, [&] {
        ar.value("r", c.r);
        ar.value("g", c.g);
        ar.value("b", c.b);
    });
}

template <typename Ar>
void saveLooks(Ar& ar, PawnLooks& l) {
    ar.text("lineage", l.lineage);
    ar.value("female", l.female);
    ar.value("body", l.body);
    ar.require(static_cast<int>(l.body) <= static_cast<int>(BodyType::Hulk), "the save file has a bad body type");
    ar.value("weight", l.weight);
    ar.value("height", l.height);
    ar.value("melanin", l.melanin);
    ar.require(l.weight >= -1.0f && l.weight <= 1.0f && l.height > 0.0f && l.height < 10.0f, "the save file has impossible looks");
    saveLooksColor(ar, "naturalHair", l.naturalHair);
    saveLooksColor(ar, "skin", l.skin);
    saveLooksColor(ar, "hair", l.hair);
}

template <typename Ar>
void saveThingPart(Ar& ar, SaveThing& t) {
    ar.text("def", t.def);
    ar.value("category", t.category);
    ar.require(static_cast<int>(t.category) <= static_cast<int>(ThingCategory::Pawn),
               "the save file has a bad thing category");

}

template <typename Ar>
void savePositionPart(Ar& ar, Position& p) {
    ar.value("x", p.x);
    ar.value("y", p.y);
    ar.value("level", p.level);

}

template <typename Ar>
void saveFootprintPart(Ar& ar, Footprint& f) {
    ar.value("sizeX", f.sizeX);
    ar.value("sizeY", f.sizeY);
    ar.value("blocksMovement", f.blocksMovement);
    ar.value("isDoor", f.isDoor);
    ar.value("obstacleDm", f.obstacleDm);
    ar.group("origin", [&] { saveCell(ar, f.origin); });
    ar.value("rotation", f.rotation);
    ar.require(f.sizeX >= 1 && f.sizeX <= 256 && f.sizeY >= 1 && f.sizeY <= 256,
               "the save file has a building of impossible size");
    ar.require(static_cast<int>(f.rotation) <= static_cast<int>(Rot4::West), "the save file has a bad rotation");

}

template <typename Ar>
void saveStackPart(Ar& ar, StackCount& s) {
    ar.value("count", s.count);
}

template <typename Ar>
void saveHitPointsPart(Ar& ar, HitPoints& hp) {
    ar.value("current", hp.current);
    ar.value("max", hp.max);

}

template <typename Ar>
void saveMoverPart(Ar& ar, Mover& m, const std::optional<Position>& position) {
    ar.value("metresPerStep", m.metresPerStep);
    ar.list("path", m.path, [&](CellPos& c) { saveCell(ar, c); });
    uint64_t next = m.next;
    ar.value("next", next);
    m.next = static_cast<size_t>(next);
    ar.require(m.next <= m.path.size(), "the save file has a pawn walking past the end of its path");
    if (ar.version() >= 3) {
        ar.value("vx", m.vx);
        ar.value("vy", m.vy);
        ar.value("fromX", m.fromX);
        ar.value("fromY", m.fromY);
        ar.value("blockedSteps", m.blockedSteps);
        ar.value("ghostSteps", m.ghostSteps);
        ar.value("heldSteps", m.heldSteps);
        ar.value("gaveUp", m.gaveUp);
        if (ar.sectionVersion() >= 2) {  // M2: stairs and ladders
            ar.value("climbed", m.climbed);
        }
    } else if (position) {
        // Older saves: a pawn mid-walk sets off along the line from where it stands.
        m.fromX = position->x;
        m.fromY = position->y;
    }

}

template <typename Ar>
void saveWorkerPart(Ar& ar, SaveWorker& w) {
    Job& job = w.job;
    ar.value("job", job.type);
    ar.require(job.type <= JobType::Party, "the save file has a bad job");
    ar.require(job.type <= JobType::HarvestPlant || ar.version() >= 18, "the save file has a bad job");
    ar.require(job.type <= JobType::LayDown || ar.version() >= 10, "the save file has a bad job");
    ar.require(job.type <= JobType::Deconstruct || ar.version() >= 14, "the save file has a bad job");
    ar.value("thing", job.thing);
    ar.group("cell", [&] { saveCell(ar, job.cell); });
    ar.value("count", job.count);
    if (ar.version() >= 8) {
        ar.value("chair", job.chair);
    }
    if (ar.version() >= 10) {
        ar.value("site", job.site);
        ar.value("progress", job.progress);
        ar.require(job.progress >= 0.0f && job.progress < 1.0e9f, "the save file has an impossible job progress");
    }
    if (ar.version() >= 18) {
        ar.value("bill", job.bill);
        ar.list("picks", job.picks, [&](BillPick& pick) {
            ar.value("item", pick.item);
            ar.value("count", pick.count);
            ar.require(pick.count > 0 && pick.count <= 100000, "the save file has an impossible ingredient count");
        });
        ar.list("used", w.used, [&](SaveBillUsed& used) {
            ar.text("def", used.def);
            ar.value("count", used.count);
            ar.require(used.count > 0 && used.count <= 100000, "the save file has an impossible ingredient count");
        });
        ar.require(job.picks.size() < 12, "the save file has a bill job with too many ingredients");
    }
    ar.text("workType", w.workType);
    ar.value("forced", job.forced);
    if (ar.version() >= 19) {
        ar.value("handsOn", job.handsOn);
        ar.require(job.handsOn <= 2, "the save file has a bad hands-on state");
    }
    ar.value("step", job.step);
    ar.require(job.step < std::max(1, jobStepCount(job.type)), "the save file has a job past its last step");
    ar.value("walk", job.walk);
    ar.require(job.walk <= 2, "the save file has a bad walk state");
    ar.value("repaths", job.repaths);
    ar.value("startedAt", job.startedAt);
    ar.value("until", job.until);
    ar.value("pathSlot", job.path.slot);
    ar.value("pathSerial", job.path.serial);
    ar.list("avoid", w.avoid, [&](AvoidTarget& a) {
        ar.value("thing", a.target.thing);
        ar.group("cell", [&] { saveCell(ar, a.target.cell); });
        ar.value("until", a.until);
    });
    ar.require(w.avoid.size() <= JobSystem::kMaxAvoid, "the save file has a worker with too long a memory");

}

template <typename Ar>
void saveColonistPart(Ar& ar, Colonist& c) {
    saveColonist(ar, c);
}

template <typename Ar>
void saveHealthPart(Ar& ar, SaveHealth& h) {
    ar.value("protagonist", h.protagonist);
    ar.value("rest", h.rest);
    ar.require(static_cast<int>(h.rest) <= static_cast<int>(RestKind::HospitalBed), "the save file has a bad rest kind");
    ar.list("injuries", h.injuries, [&](SaveInjury& i) {
        ar.value("part", i.part);
        ar.text("hediff", i.hediff);
        ar.value("severity", i.severity);
        ar.value("tendQuality", i.tendQuality);
        if (ar.sectionVersion() >= 3) {
            ar.value("peak", i.peak);
            ar.require(i.peak >= 0.0f && i.peak < 1.0e6f, "the save file has an impossible injury");
        }
        if (ar.sectionVersion() >= 4) {
            ar.value("infectAt", i.infectAt);
            ar.require(i.infectAt >= -1, "the save file has an impossible infection time");
        }
        ar.require(i.severity >= 0.0f && i.severity < 1.0e6f, "the save file has an impossible injury");
        ar.require(i.tendQuality >= -1.0f && i.tendQuality < 1.0e3f, "the save file has an impossible tend quality");
    });
    ar.list("missing", h.missing, [&](MissingPart& m) {
        ar.value("part", m.part);
        ar.value("tendQuality", m.tendQuality);
        ar.require(m.tendQuality >= -1.0f && m.tendQuality < 1.0e3f, "the save file has an impossible tend quality");
    });
    if (ar.sectionVersion() >= 5) {
        ar.list("added", h.added, [&](SaveAddedPart& a) {
            ar.value("part", a.part);
            ar.text("hediff", a.hediff);
        });
    }
    ar.value("bloodLoss", h.bloodLoss);
    ar.require(h.bloodLoss >= 0.0f && h.bloodLoss <= 1000.0f, "the save file has impossible blood loss");
    if (ar.sectionVersion() >= 2) {
        ar.list("environment", h.environment, [&](SaveEnvironmentHediff& env) {
            ar.text("hediff", env.hediff);
            ar.value("severity", env.severity);
            ar.require(env.severity > 0.0f && env.severity < 1.0e3f, "the save file has an impossible environment hediff");
        });
    }
    if (ar.sectionVersion() >= 4) {
        ar.list("diseases", h.diseases, [&](SaveDisease& d) {
            ar.text("hediff", d.hediff);
            ar.value("severity", d.severity);
            ar.value("tendQuality", d.tendQuality);
            ar.value("tendedUntil", d.tendedUntil);
            ar.value("tendTotal", d.tendTotal);
            ar.value("part", d.part);
            ar.require(d.severity > 0.0f && d.severity <= 1.0f, "the save file has an impossible disease");
            ar.require(d.tendQuality >= -1.0f && d.tendQuality < 1.0e3f, "the save file has an impossible tend quality");
            ar.require(d.part >= -1 && d.part < 4096, "the save file has a disease on an impossible body part");
        });
        ar.list("immunities", h.immunities, [&](SaveImmunity& m) {
            ar.text("hediff", m.hediff);
            ar.value("level", m.level);
            ar.require(m.level >= 0.0f && m.level <= 1.0f, "the save file has an impossible immunity");
        });
    }
    ar.value("dead", h.dead);
    ar.value("cause", h.cause);
    ar.require(static_cast<int>(h.cause) <= static_cast<int>(DeathCause::Withdrawal), "the save file has a bad cause of death");
    if (ar.sectionVersion() >= 6) {
        ar.value("downed", h.downed);
        ar.value("dirty", h.dirty);
    }
}

template <typename Ar>
void saveWorkPart(Ar& ar, SaveWorkSettings& w) {
    ar.list("entries", w.entries, [&](SaveWorkPriority& p) {
        ar.text("workType", p.workType);
        ar.value("priority", p.priority);
        ar.value("disabled", p.disabled);
        ar.require(p.priority <= JobSystem::kMaxPriority, "the save file has a bad work priority");
    });

}

template <typename Ar>
void saveCarryingPart(Ar& ar, Carrying& c) {
    ar.value("thing", c.thing);
}

template <typename Ar>
void saveNeedsPart(Ar& ar, Needs& n) {
    ar.value("food", n.food);
    ar.value("rest", n.rest);
    ar.value("malnutrition", n.malnutrition);
    ar.value("foodSearchAfter", n.foodSearchAfter);
    ar.require(n.food >= 0.0f && n.food <= 1000.0f && n.rest >= 0.0f && n.rest <= 1000.0f &&
                   n.malnutrition >= 0.0f && n.malnutrition <= 1000.0f,
               "the save file has an impossible need level");

}

template <typename Ar>
void saveSchedulePart(Ar& ar, Schedule& s) {
    std::vector<ScheduleKind> hours(s.hours.begin(), s.hours.end());
    ar.list("hours", hours, [&](ScheduleKind& kind) {
        ar.value(nullptr, kind);
        ar.require(kind <= ScheduleKind::Sleep, "the save file has a bad schedule kind");
    });
    ar.require(hours.size() == s.hours.size(), "the save file has a schedule that isn't 24 hours");
    if (hours.size() == s.hours.size()) {
        std::copy(hours.begin(), hours.end(), s.hours.begin());
    }

}

template <typename Ar>
void saveBedPart(Ar& ar, Bed& b) {
    ar.list("owners", b.owners, [&](Entity& owner) { ar.value(nullptr, owner); });

}

template <typename Ar>
void saveApparelPart(Ar& ar, SaveApparel& a) {
    ar.text("stuff", a.stuff);
    ar.value("quality", a.quality);
    ar.require(a.quality >= -1 && a.quality < 256, "the save file has an impossible quality");
    ar.value("tainted", a.tainted);
    ar.value("stripMark", a.stripMark);
    ar.value("hasOutfit", a.hasOutfit);
    ar.value("outfit", a.outfit);
    ar.value("nextCheck", a.nextCheck);
}

template <typename Ar>
void saveOutfitPolicy(Ar& ar, OutfitPolicy& o) {
    ar.value("id", o.id);
    ar.text("name", o.name);
    ar.list("disallowed", o.disallowed, [&](std::string& name) { ar.text(nullptr, name); });
    ar.value("hitPointsMin", o.hitPointsMin);
    ar.value("hitPointsMax", o.hitPointsMax);
    ar.value("qualityMin", o.qualityMin);
    ar.value("qualityMax", o.qualityMax);
    ar.require(o.id != 0 && o.hitPointsMin >= 0.0f && o.hitPointsMax <= 1.0f && o.hitPointsMin <= o.hitPointsMax &&
                   o.qualityMin <= o.qualityMax,
               "the save file has an impossible outfit");
}

template <typename Ar>
void saveFuelPart(Ar& ar, Fuel& fuel) {
    ar.value("amount", fuel.amount);
    ar.value("autoRefuel", fuel.autoRefuel);
    ar.value("fillOrdered", fuel.fillOrdered);
    ar.require(fuel.amount >= 0.0f && fuel.amount <= 1.0e6f, "the save file has an impossible amount of fuel");
}

template <typename Ar>
void saveBillsPart(Ar& ar, SaveBillList& list) {
    ar.value("nextId", list.nextId);
    ar.list("bills", list.bills, [&](SaveBill& bill) {
        ar.value("id", bill.id);
        ar.text("recipe", bill.recipe);
        ar.value("mode", bill.mode);
        ar.require(bill.mode <= BillMode::Forever, "the save file has a bad bill mode");
        ar.value("count", bill.count);
        ar.require(bill.count >= 0 && bill.count <= 1000000, "the save file has an impossible bill count");
        ar.value("suspended", bill.suspended);
        if (ar.sectionVersion() >= 2) {
            ar.value("skillMin", bill.skillMin);
            ar.value("skillMax", bill.skillMax);
            ar.value("radius", bill.radius);
            ar.value("storeZone", bill.storeZone);
            ar.value("unpauseAt", bill.unpauseAt);
            ar.value("satisfied", bill.satisfied);
            ar.list("disallowed", bill.disallowed, [&](std::string& name) { ar.text(nullptr, name); });
            ar.require(bill.skillMin >= 0 && bill.skillMax <= 20 && bill.skillMin <= bill.skillMax && bill.radius >= 0.0f &&
                           bill.unpauseAt >= 0,
                       "the save file has an impossible bill setting");
        }
        ar.require(bill.id != 0 && bill.id < list.nextId, "the save file has a bill with a bad id");
    });

}

template <typename Ar>
void saveSitePart(Ar& ar, SaveSite& s) {
    ar.value("stage", s.stage);
    ar.require(s.stage == ConstructionStage::Blueprint || s.stage == ConstructionStage::Frame,
               "the save file has a bad building stage");
    ar.text("stuff", s.stuff);
    ar.group("origin", [&] { saveCell(ar, s.origin); });
    ar.value("rotation", s.rotation);
    ar.require(static_cast<int>(s.rotation) <= static_cast<int>(Rot4::West), "the save file has a bad rotation");
    ar.value("sizeX", s.sizeX);
    ar.value("sizeY", s.sizeY);
    ar.require(s.sizeX >= 1 && s.sizeX <= 256 && s.sizeY >= 1 && s.sizeY <= 256,
               "the save file has a building plan of impossible size");
    ar.list("materials", s.materials, [&](SaveMaterial& m) {
        ar.text("item", m.item);
        ar.value("needed", m.needed);
        ar.value("have", m.have);
        ar.require(m.needed >= 0 && m.needed < 1000000 && m.have >= 0 && m.have < 1000000,
                   "the save file has an impossible material count");
    });
    ar.value("workTotal", s.workTotal);
    ar.value("workDone", s.workDone);
    ar.require(s.workTotal >= 0.0f && s.workTotal < 1.0e9f && s.workDone >= 0.0f && s.workDone < 1.0e9f,
               "the save file has impossible building work");

}

template <typename Ar>
void saveMoodPart(Ar& ar, Mood& m) {
    ar.value("level", m.level);
    ar.value("band", m.band);
    ar.require(m.level >= 0.0f && m.level <= 100.0f, "the save file has an impossible mood");
    ar.require(m.band <= MoodBand::Extreme, "the save file has a bad mood band");
    ar.list("memories", m.memories, [&](Memory& memory) {
        ar.text("def", memory.def);
        ar.value("expires", memory.expires);
    });

}

template <typename Ar>
void saveJoyPart(Ar& ar, SaveJoy& j) {
    ar.value("level", j.joy.level);
    ar.value("comfort", j.joy.comfort);
    ar.value("outdoors", j.joy.outdoors);
    ar.require(j.joy.level >= 0.0f && j.joy.level <= 1.0f && j.joy.comfort >= 0.0f && j.joy.comfort <= 1.0f &&
                   j.joy.outdoors >= 0.0f && j.joy.outdoors <= 1.0f,
               "the save file has an impossible need");
    ar.list("tolerance", j.joy.tolerance, [&](JoyTolerance& t) {
        ar.text("kind", t.kind);
        ar.value("amount", t.amount);
        ar.require(t.amount >= 0.0f && t.amount <= 1.0e3f, "the save file has an impossible tolerance");
    });
    ar.text("activity", j.activity);
}

template <typename Ar>
void savePosturePart(Ar& ar, Posture& p) {
    ar.value("stance", p.stance);
    ar.value("facingX", p.facingX);
    ar.value("facingY", p.facingY);
    ar.require(static_cast<int>(p.stance) <= static_cast<int>(Stance::Prone), "the save file has a bad stance");

}

template <typename Ar>
void saveAnimalPart(Ar& ar, Animal& a) {
    ar.value("started", a.started);
    ar.value("wild", a.wild);
    ar.value("feeding", a.feeding);
    ar.value("sleepShift", a.sleepShift);
    ar.value("herd", a.herd);
    ar.value("bornAt", a.bornAt);
    ar.value("diedAt", a.diedAt);
    ar.value("food", a.food);
    ar.value("foodAt", a.foodAt);
    ar.value("mode", a.mode);
    ar.value("until", a.until);
    ar.value("nextThink", a.nextThink);
    ar.value("staysUntil", a.staysUntil);
    ar.group("goal", [&] { saveCell(ar, a.goal); });
    if (ar.sectionVersion() >= 2) {  // N5: fights
        ar.value("temper", a.temper);
        ar.value("enemy", a.enemy);
        ar.value("temperUntil", a.temperUntil);
        ar.value("huntAfter", a.huntAfter);
        ar.require(static_cast<int>(a.temper) <= static_cast<int>(AnimalTemper::Manhunter), "the save file has a bad animal temper");
    }
    ar.require(static_cast<int>(a.mode) <= static_cast<int>(AnimalMode::Seized), "the save file has a bad animal mode");
    ar.require(a.sleepShift < 24, "the save file has a bad animal sleep shift");
}

template <typename Ar>
void saveCarePart(Ar& ar, AnimalCare& c) {
    ar.value("markTame", c.markTame);
    ar.value("markSlaughter", c.markSlaughter);
    if (ar.sectionVersion() >= 2) {  // N2: hunting
        ar.value("markHunt", c.markHunt);
        ar.value("collect", c.collect);
    }
    if (ar.sectionVersion() >= 3) {  // animal names
        ar.text("name", c.name);
    }
    ar.value("zone", c.zone);
    ar.value("hasHome", c.hasHome);
    ar.group("home", [&] { saveCell(ar, c.home); });
    ar.value("tamedAt", c.tamedAt);
    ar.value("heldUntil", c.heldUntil);
    ar.value("starvingSince", c.starvingSince);
    ar.value("nextTrainAt", c.nextTrainAt);
    ar.value("lastTrainAt", c.lastTrainAt);
    ar.value("noPathUntil", c.noPathUntil);
    for (int t = 0; t < kTrainableCount; ++t) {
        ar.group(trainableName(static_cast<Trainable>(t)), [&] {
            ar.value("steps", c.training[t].steps);
            ar.value("wanted", c.training[t].wanted);
        });
    }
    ar.value("pathSlot", c.path.slot);
    ar.value("pathSerial", c.path.serial);
    ar.group("pathGoal", [&] { saveCell(ar, c.pathGoal); });
    ar.value("pathMode", c.pathMode);
    ar.require(static_cast<int>(c.pathMode) <= static_cast<int>(AnimalMode::EatItem), "the save file has a bad animal path mode");
}

template <typename Ar>
void saveBreedPart(Ar& ar, AnimalBreed& b) {
    ar.value("pregnantUntil", b.pregnantUntil);
    ar.value("litter", b.litter);
    ar.value("fertilized", b.fertilized);
    ar.value("nextMateAt", b.nextMateAt);
    ar.value("milkSince", b.milkSince);
    ar.value("woolSince", b.woolSince);
    ar.value("eggSince", b.eggSince);
    ar.value("courting", b.courting);
    ar.value("courtUntil", b.courtUntil);
    if (ar.sectionVersion() >= 2) {
        ar.value("breedingOff", b.breedingOff);
    }
}

template <typename Ar>
void saveYoungPart(Ar& ar, Young& y) {
    ar.value("mother", y.mother);
}

template <typename Ar>
void saveHatchingPart(Ar& ar, Hatching& h) {
    ar.value("hatchAt", h.hatchAt);
}

template <typename Ar>
void saveGearPart(Ar& ar, Gear& g) {
    ar.value("weapon", g.weapon);
    ar.list("apparel", g.apparel, [&](Entity& a) { ar.value(nullptr, a); });

}

template <typename Ar>
void saveEquippedByPart(Ar& ar, EquippedBy& b) {
    ar.value("pawn", b.pawn);
}

template <typename Ar>
void saveShooterPart(Ar& ar, Shooter& sh) {
    ar.value("phase", sh.phase);
    ar.require(static_cast<int>(sh.phase) <= static_cast<int>(ar.version() >= 17 ? ShotPhase::Reloading : ShotPhase::Cooling),
               "the save file has a bad trigger phase");
    ar.value("stepsLeft", sh.stepsLeft);
    ar.value("shotsLeft", sh.shotsLeft);
    ar.value("target", sh.target);
    ar.value("aimX", sh.aimX);
    ar.value("aimY", sh.aimY);
    ar.value("aimZ", sh.aimZ);
    ar.value("player", sh.player);
    ar.value("recoil", sh.recoil);

}

template <typename Ar>
void saveKickPart(Ar& ar, RecoilKick& k) {
    ar.value("up", k.up);
    ar.value("side", k.side);
    ar.value("shots", k.shots);
    ar.value("sinceShot", k.sinceShot);
    ar.require(std::isfinite(k.up) && std::isfinite(k.side) && k.shots >= 0 && k.sinceShot >= 0,
               "the save file has a bad gun kick");
}

template <typename Ar>
void saveStowedPart(Ar& ar, std::vector<Entity>& items) {
    ar.list("items", items, [&](Entity& item) { ar.value(nullptr, item); });
}

template <typename Ar>
void saveWeaponSwitchPart(Ar& ar, WeaponSwitch& w) {
    ar.value("to", w.to);
    ar.value("stepsLeft", w.stepsLeft);
    ar.require(w.stepsLeft >= 0 && (w.stepsLeft == 0 || w.to != kNoEntity), "the save file has a bad weapon switch");
}

template <typename Ar>
void saveRollPart(Ar& ar, DodgeRoll& r) {
    ar.value("phase", r.phase);
    ar.require(static_cast<int>(r.phase) <= static_cast<int>(RollPhase::Recovery), "the save file has a bad dodge roll phase");
    ar.value("stepsLeft", r.stepsLeft);
    ar.value("rolled", r.rolled);
    ar.value("dirX", r.dirX);
    ar.value("dirY", r.dirY);
    ar.value("cooldown", r.cooldown);
    ar.value("proof", r.proof);
    ar.require(r.stepsLeft >= 0 && r.rolled >= 0 && r.cooldown >= 0 && std::isfinite(r.dirX) && std::isfinite(r.dirY),
               "the save file has a bad dodge roll");
}

template <typename Ar>
void saveProjectilePart(Ar& ar, SaveProjectile& sp) {
    ar.text("def", sp.def);
    ar.text("weapon", sp.weapon);
    ar.value("launcher", sp.p.launcher);
    ar.value("x", sp.p.x);
    ar.value("y", sp.p.y);
    ar.value("z", sp.p.z);
    ar.value("vx", sp.p.vx);
    ar.value("vy", sp.p.vy);
    ar.value("vz", sp.p.vz);
    ar.value("level", sp.p.level);
    ar.value("age", sp.p.age);
    ar.group("startCell", [&] { saveCell(ar, sp.p.startCell); });
    ar.value("seed", sp.p.seed);
    if (ar.sectionVersion() >= 2) {
        ar.value("damageFactor", sp.p.damageFactor);
    }
}

template <typename Ar>
void saveGrenadePart(Ar& ar, SaveGrenade& sg) {
    ar.text("def", sg.def);
    ar.text("item", sg.item);
    ar.value("thrower", sg.g.thrower);
    ar.value("x", sg.g.x);
    ar.value("y", sg.g.y);
    ar.value("z", sg.g.z);
    ar.value("vx", sg.g.vx);
    ar.value("vy", sg.g.vy);
    ar.value("vz", sg.g.vz);
    ar.value("level", sg.g.level);
    ar.value("age", sg.g.age);
    ar.value("fuse", sg.g.fuse);
    ar.group("startCell", [&] { saveCell(ar, sg.g.startCell); });
    ar.value("seed", sg.g.seed);
}

template <typename Ar>
void saveBeltPart(Ar& ar, SaveBelt& belt) {
    ar.list("stacks", belt.stacks, [&](SaveBeltStack& s) {
        ar.text("item", s.item);
        ar.value("count", s.count);
        ar.require(s.count > 0, "the save file has an empty stack of grenades on a belt");
    });
}

template <typename Ar>
void saveThrowerPart(Ar& ar, SaveThrower& st) {
    ar.value("phase", st.t.phase);
    ar.require(static_cast<int>(st.t.phase) <= static_cast<int>(ThrowPhase::Cooling), "the save file has a bad throwing phase");
    ar.value("stepsLeft", st.t.stepsLeft);
    ar.text("item", st.item);
    ar.value("target", st.t.target);
    ar.value("aimX", st.t.aimX);
    ar.value("aimY", st.t.aimY);
    ar.value("player", st.t.player);
}

template <typename Ar>
void saveKnockedPart(Ar& ar, KnockedDown& k) {
    ar.value("stepsLeft", k.stepsLeft);
    ar.value("before", k.before);
    ar.require(static_cast<int>(k.before) <= static_cast<int>(Stance::Prone), "the save file has a bad stance");
}

template <typename Ar>
void saveDoorPart(Ar& ar, Door& d) {
    ar.value("progress", d.progress);
    ar.value("hold", d.hold);
    ar.value("open", d.open);

}

template <typename Ar>
void savePowerPart(Ar& ar, PowerPart& part) {
    ar.value("on", part.on);
    ar.value("powered", part.powered);
    ar.value("stored", part.stored);
    ar.require(part.stored >= 0.0 && part.stored < 1e9, "the save file has a battery with impossible charge");
    ar.value("draw", part.draw);
    ar.require(part.draw == part.draw && part.draw < 1e9, "the save file has an impossible power draw");
}

template <typename Ar>
void saveFactionPart(Ar& ar, Faction& f) {
    ar.value("id", f.id);
    ar.require(f.id != 0, "the save file has faction 0");

}

template <typename Ar>
void saveFighterPart(Ar& ar, Fighter& f) {
    ar.value("drafted", f.drafted);
    ar.value("advances", f.advances);
    ar.group("anchor", [&] { saveCell(ar, f.anchor); });
    ar.value("leash", f.leash);
    ar.value("moveOrdered", f.moveOrdered);
    ar.value("forcedTarget", f.forcedTarget);
    ar.value("target", f.target);
    ar.value("hasSpot", f.hasSpot);
    ar.group("spot", [&] { saveCell(ar, f.spot); });
    ar.group("fireCell", [&] { saveCell(ar, f.fireCell); });
    ar.value("fireStance", f.fireStance);
    ar.require(static_cast<int>(f.fireStance) <= static_cast<int>(Stance::Prone), "the save file has a bad stance");
    ar.value("canFire", f.canFire);
    ar.value("fallingBack", f.fallingBack);
    ar.value("nextPlan", f.nextPlan);
    ar.value("walk", f.walk);
    ar.value("repaths", f.repaths);
    ar.value("pathSlot", f.path.slot);
    ar.value("pathSerial", f.path.serial);
    ar.list("known", f.known, [&](KnownEnemy& k) {
        ar.value("pawn", k.pawn);
        ar.value("x", k.x);
        ar.value("y", k.y);
        ar.value("level", k.level);
        ar.value("seenAt", k.seenAt);
    });
    if (ar.version() >= 17) {
        ar.value("reactUntil", f.reactUntil);
        ar.value("dodgeUntil", f.dodgeUntil);
    }
    if (ar.sectionVersion() >= 2) {  // F6: what the alarm or a raid has it do
        ar.value("mode", f.mode);
        ar.require(static_cast<int>(f.mode) <= static_cast<int>(FightMode::Seize), "the save file has a bad fighter mode");
        ar.value("defending", f.defending);
        ar.value("squad", f.squad);
        ar.value("hasGoal", f.hasGoal);
        ar.group("goal", [&] { saveCell(ar, f.goal); });
        ar.value("retreat", f.retreat);
    }

}

template <typename Ar>
void saveMeleePart(Ar& ar, Melee& m) {
    ar.value("phase", m.phase);
    ar.require(m.phase <= MeleePhase::Recovery, "the save file has a bad melee phase");
    ar.value("stepsLeft", m.stepsLeft);
    ar.value("target", m.target);
    ar.value("tool", m.tool);
    ar.value("weaponTool", m.weaponTool);
    ar.value("player", m.player);
    ar.value("committed", m.committed);
    ar.value("aimX", m.aimX);
    ar.value("aimY", m.aimY);
    ar.value("aimZ", m.aimZ);
    ar.value("seed", m.seed);
    ar.value("threat", m.threat);
    ar.value("defense", m.defense);
    ar.require(m.defense <= MeleeDefense::Block, "the save file has a bad melee defense");
    ar.value("reactAt", m.reactAt);
    ar.value("blockFrom", m.blockFrom);
    ar.value("blockUntil", m.blockUntil);
    ar.value("dodged", m.dodged);
}

template <typename Ar>
void saveSocialPart(Ar& ar, Social& social) {
    ar.value("nextTalk", social.nextTalk);
    ar.value("mourned", social.mourned);
    ar.list("memories", social.memories, [&](OpinionMemory& m) {
        ar.value("other", m.other);
        ar.text("label", m.label);
        ar.value("offset", m.offset);
        ar.value("expires", m.expires);
        ar.require(m.offset >= -1.0e3f && m.offset <= 1.0e3f, "the save file has an impossible opinion");
    });
    ar.list("bonds", social.bonds, [&](Bond& b) {
        ar.value("kind", b.kind);
        ar.require(b.kind <= BondKind::ExSpouse, "the save file has a bad bond kind");
        ar.value("other", b.other);
        ar.value("since", b.since);
    });
}

template <typename Ar>
void saveSocialFightPart(Ar& ar, SocialFight& f) {
    ar.value("opponent", f.opponent);
    ar.value("startedAt", f.startedAt);
    ar.value("endsBy", f.endsBy);
    ar.value("nextMove", f.nextMove);
}

template <typename Ar>
void saveTurretPart(Ar& ar, Turret& t) {
    ar.value("phase", t.phase);
    ar.require(static_cast<int>(t.phase) <= static_cast<int>(TurretPhase::Cooling), "the save file has a bad turret phase");
    ar.value("stepsLeft", t.stepsLeft);
    ar.value("shotsLeft", t.shotsLeft);
    ar.value("target", t.target);
}

template <typename Ar>
void saveTrapPart(Ar& ar, Trap& t) {
    ar.value("sprung", t.sprung);
    ar.list("known", t.known, [&](FactionId& f) {
        ar.value(nullptr, f);
        ar.require(f != 0, "the save file has faction 0");
    });
    ar.require(std::is_sorted(t.known.begin(), t.known.end()), "the save file has a trap's sides out of order");
}

template <typename Ar>
void saveMentalBreakerPart(Ar& ar, MentalBreaker& b) {
    ar.value("active", b.active);
    ar.text("def", b.def);
    ar.value("state", b.state);
    ar.require(static_cast<int>(b.state) <= static_cast<int>(BreakState::FireSpree), "the save file has a bad mental break state");
    ar.value("startedAt", b.startedAt);
    ar.value("endsAfter", b.endsAfter);
    ar.value("hardEnd", b.hardEnd);
    ar.value("recoverLevel", b.recoverLevel);
    ar.value("lastEnded", b.lastEnded);
    ar.value("walk", b.walk);
    ar.value("repaths", b.repaths);
    ar.value("pathSlot", b.path.slot);
    ar.value("pathSerial", b.path.serial);
    ar.value("hasGoal", b.hasGoal);
    ar.group("goal", [&] { saveCell(ar, b.goal); });
    ar.value("target", b.target);
    ar.value("done", b.done);
    ar.value("nextAct", b.nextAct);
    ar.value("nextHit", b.nextHit);
    ar.list("visited", b.visited, [&](Entity& e) { ar.value(nullptr, e); });
}

template <typename Ar>
void saveMagazinePart(Ar& ar, Magazine& m) {
    ar.value("rounds", m.rounds);
    ar.require(m.rounds >= 0, "the save file has a magazine with fewer than no rounds");

}

template <typename Ar>
void saveLooksPart(Ar& ar, PawnLooks& l) {
    saveLooks(ar, l);
}

template <typename Ar>
void saveLegacyEntity(Ar& ar, SaveEntity& e) {
    ar.value("id", e.id);
    ar.optional("thing", e.thing, [&](SaveThing& t) { saveThingPart(ar, t); });
    ar.optional("position", e.position, [&](Position& p) { savePositionPart(ar, p); });
    ar.optional("footprint", e.footprint, [&](Footprint& f) { saveFootprintPart(ar, f); });
    ar.optional("stackCount", e.stack, [&](StackCount& s) { saveStackPart(ar, s); });
    ar.optional("hitPoints", e.hitPoints, [&](HitPoints& hp) { saveHitPointsPart(ar, hp); });
    ar.optional("mover", e.mover, [&](Mover& m) { saveMoverPart(ar, m, e.position); });
    ar.optional("worker", e.worker, [&](SaveWorker& w) { saveWorkerPart(ar, w); });
    if (ar.version() >= 4) {
        ar.optional("colonist", e.colonist, [&](Colonist& c) { saveColonistPart(ar, c); });
    }
    if (ar.version() >= 5) {
        ar.optional("health", e.health, [&](SaveHealth& h) { saveHealthPart(ar, h); });
    }
    ar.optional("work", e.work, [&](SaveWorkSettings& w) { saveWorkPart(ar, w); });
    ar.optional("carrying", e.carrying, [&](Carrying& c) { saveCarryingPart(ar, c); });
    if (ar.version() >= 7) {
        ar.value("playerControlled", e.playerControlled);
    }
    if (ar.version() >= 8) {
        ar.optional("needs", e.needs, [&](Needs& n) { saveNeedsPart(ar, n); });
        ar.optional("schedule", e.schedule, [&](Schedule& s) { saveSchedulePart(ar, s); });
        ar.optional("bed", e.bed, [&](Bed& b) { saveBedPart(ar, b); });
    }
    if (ar.version() >= 18) {
        ar.optional("bills", e.bills, [&](SaveBillList& list) { saveBillsPart(ar, list); });
    }
    if (ar.version() >= 10) {
        ar.optional("site", e.site, [&](SaveSite& s) { saveSitePart(ar, s); });
        ar.optional("builtOf", e.builtOf, [&](std::string& stuff) { ar.text("stuff", stuff); });
        ar.optional("marked", e.marked, [&](SaveMark&) {});
    }
    if (ar.version() >= 11) {
        ar.optional("mood", e.mood, [&](Mood& m) { saveMoodPart(ar, m); });
    }
    if (ar.version() >= 12) {
        ar.optional("posture", e.posture, [&](Posture& p) { savePosturePart(ar, p); });
        ar.optional("gear", e.gear, [&](Gear& g) { saveGearPart(ar, g); });
        ar.optional("equippedBy", e.equippedBy, [&](EquippedBy& b) { saveEquippedByPart(ar, b); });
        ar.optional("shooter", e.shooter, [&](Shooter& sh) { saveShooterPart(ar, sh); });
        ar.optional("projectile", e.projectile, [&](SaveProjectile& sp) { saveProjectilePart(ar, sp); });
    }
    if (ar.version() >= 13) {
        ar.optional("door", e.door, [&](Door& d) { saveDoorPart(ar, d); });
    }
    if (ar.version() >= 15) {
        ar.optional("faction", e.faction, [&](Faction& f) { saveFactionPart(ar, f); });
        ar.optional("fighter", e.fighter, [&](Fighter& f) { saveFighterPart(ar, f); });
    }
    if (ar.version() >= 17) {
        ar.optional("magazine", e.magazine, [&](Magazine& m) { saveMagazinePart(ar, m); });
    }
    if (ar.version() >= 16) {
        ar.optional("looks", e.looks, [&](PawnLooks& l) { saveLooksPart(ar, l); });
    }
}

template <typename Ar>
void saveZone(Ar& ar, SaveZone& z) {
    ar.value("id", z.id);
    ar.value("priority", z.priority);
    ar.require(z.priority <= StoragePriority::Critical, "the save file has a bad storage priority");
    ar.list("disallowed", z.disallowed, [&](std::string& def) { ar.text(nullptr, def); });
    ar.list("cells", z.cells, [&](CellPos& c) { saveCell(ar, c); });
}

template <typename Ar>
void savePaths(Ar& ar, PathSavedState& p) {
    ar.value("nextSerial", p.nextSerial);
    ar.value("updates", p.updates);
    ar.list("requests", p.requests, [&](PathSavedRequest& r) {
        ar.value("serial", r.serial);
        ar.value("status", r.status);
        ar.require(static_cast<int>(r.status) <= static_cast<int>(PathStatus::Failed),
                   "the save file has a bad path status");
        ar.group("start", [&] { saveCell(ar, r.start); });
        ar.group("goal", [&] { saveCell(ar, r.goal); });
        ar.value("askedAt", r.askedAt);
        ar.list("path", r.path, [&](CellPos& c) { saveCell(ar, c); });
    });
    ar.list("freeSlots", p.freeSlots, [&](uint32_t& slot) { ar.value(nullptr, slot); });
    ar.list("queue", p.queue, [&](uint32_t& slot) { ar.value(nullptr, slot); });
    ar.value("activeWork", p.activeWork);
}

template <typename Ar>
void savePlants(Ar& ar, SavePlants& p) {
    ar.list("defs", p.defs, [&](std::string& s) { ar.text(nullptr, s); });
    ar.list("cells", p.cells, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("defIndex", p.defIndex, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("growth", p.growth, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("ageHours", p.ageHours, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.require(p.cells.size() == p.defIndex.size() && p.cells.size() == p.growth.size() && p.cells.size() == p.ageHours.size(),
               "the save file's plant lists don't match");
}

template <typename Ar>
void saveWeather(Ar& ar, SaveWeather& w) {
    ar.text("current", w.current);
    ar.text("previous", w.previous);
    ar.value("startedAt", w.startedAt);
    ar.value("endsAt", w.endsAt);
    ar.value("rngState", w.rngState);
    ar.value("rngIncrement", w.rngIncrement);
    ar.list("snowCells", w.snowCells, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("snowDepth", w.snowDepth, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.require(w.snowCells.size() == w.snowDepth.size(), "the save file's snow lists don't match");
}

template <typename Ar>
void saveMineables(Ar& ar, SaveMineables& m) {
    ar.list("defs", m.defs, [&](std::string& s) { ar.text(nullptr, s); });
    ar.list("cells", m.cells, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("defIndex", m.defIndex, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("hitPoints", m.hitPoints, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.require(m.cells.size() == m.defIndex.size() && m.cells.size() == m.hitPoints.size(),
               "the save file's rock lists don't match");
}

template <typename Ar>
void saveDesignations(Ar& ar, SaveDesignations& d) {
    ar.list("cells", d.cells, [&](uint32_t& v) { ar.value(nullptr, v); });
    ar.list("kinds", d.kinds, [&](uint8_t& v) { ar.value(nullptr, v); });
    ar.require(d.cells.size() == d.kinds.size(), "the save file's designation lists don't match");
}

template <typename Ar>
void saveResearch(Ar& ar, ResearchState& r) {
    ar.text("current", r.current);
    ar.value("neolithicStart", r.neolithicStart);
    ar.list("done", r.done, [&](std::string& name) { ar.text(nullptr, name); });
    ar.require(std::is_sorted(r.done.begin(), r.done.end()), "the save file's finished research isn't in order");
    // The progress map is saved as parallel lists in name order.
    std::vector<std::string> names;
    std::vector<double> points;
    for (const auto& [name, value] : r.progress) {
        names.push_back(name);
        points.push_back(value);
    }
    ar.list("progressNames", names, [&](std::string& name) { ar.text(nullptr, name); });
    ar.list("progressPoints", points, [&](double& value) { ar.value(nullptr, value); });
    ar.require(names.size() == points.size(), "the save file's research lists don't match");
    if (Ar::kReading) {
        r.progress.clear();
        for (size_t i = 0; i < names.size(); ++i) {
            ar.require(points[i] >= 0.0 && points[i] < 1.0e9, "the save file has impossible research progress");
            r.progress[names[i]] = points[i];
        }
    }
}

template <typename Ar>
void saveShip(Ar& ar, ShipState& s) {
    ar.value("phase", s.phase);
    ar.require(static_cast<int>(s.phase) <= static_cast<int>(ShipPhase::Launched), "the save file has a bad ship phase");
    ar.value("reactor", s.reactor);
    ar.value("startedAt", s.startedAt);
    ar.value("nextWave", s.nextWave);
    ar.value("waves", s.waves);
    ar.value("escaped", s.escaped);
    ar.value("launchedAt", s.launchedAt);
    ar.list("caskets", s.caskets, [&](Entity& e) { ar.value(nullptr, e); });
    ar.list("riders", s.riders, [&](Entity& e) { ar.value(nullptr, e); });
}

template <typename Ar>
void saveDrugs(Ar& ar, SaveData& d) {
    ar.list("pawns", d.drugPawns, [&](SaveDrugPawn& p) {
        ar.value("pawn", p.pawn);
        ar.text("policy", p.policy);
        ar.value("searchAfter", p.searchAfter);
        ar.list("edits", p.edits, [&](SaveDrugEntry& e) {
            ar.text("drug", e.drug);
            ar.value("forAddiction", e.forAddiction);
            ar.value("forJoy", e.forJoy);
            ar.value("scheduled", e.scheduled);
            ar.value("daysFrequency", e.daysFrequency);
            ar.value("moodBelow", e.moodBelow);
            ar.value("joyBelow", e.joyBelow);
            ar.require(e.daysFrequency > 0.0f && e.daysFrequency <= 1000.0f, "the save file has an impossible drug frequency");
        });
        ar.list("chemicals", p.chemicals, [&](SaveDrugChemical& c) {
            ar.text("chemical", c.chemical);
            ar.value("tolerance", c.tolerance);
            ar.value("addicted", c.addicted);
            ar.value("need", c.need);
            ar.value("progress", c.progress);
            ar.value("armed", c.armed);
            ar.require(c.tolerance >= 0.0f && c.tolerance <= 1.0f && c.need >= 0.0f && c.need <= 1.0f && c.progress >= 0.0f &&
                           c.progress <= 1.0f,
                       "the save file has an impossible drug record");
        });
        ar.list("last", p.last, [&](SaveDrugDose& dose) {
            ar.text("drug", dose.drug);
            ar.value("at", dose.at);
        });
    });
}

template <typename Ar>
void saveTalents(Ar& ar, SaveData& d) {
    ar.list("pawns", d.talentPawns, [&](Entity& pawn) { ar.value(nullptr, pawn); });
    ar.list("xp", d.talentXp, [&](int64_t& xp) { ar.value(nullptr, xp); });
    ar.list("names", d.talentNames, [&](std::vector<std::string>& names) {
        ar.list(nullptr, names, [&](std::string& name) { ar.text(nullptr, name); });
    });
    ar.list("ranks", d.talentRanks, [&](std::vector<int32_t>& ranks) {
        ar.list(nullptr, ranks, [&](int32_t& rank) { ar.value(nullptr, rank); });
    });
    ar.require(d.talentPawns.size() == d.talentXp.size() && d.talentPawns.size() == d.talentNames.size() &&
                   d.talentPawns.size() == d.talentRanks.size(),
               "the save file's talent lists don't match");
    for (size_t i = 0; i < d.talentPawns.size(); ++i) {
        ar.require(d.talentNames[i].size() == d.talentRanks[i].size(), "the save file's talent lists don't match");
    }
}

template <typename Ar>
void saveRoomStatsSection(Ar& ar, SaveData& d) {
    d.hasRoomStats = true;
    ar.list("floors", d.floors, [&](SaveFloor& f) {
        ar.value("cell", f.cell);
        ar.text("def", f.def);
    });
    ar.list("filth", d.filth.piles, [&](FilthState::Entry& e) {
        ar.value("cell", e.cell);
        ar.text("kind", e.kind);
        ar.value("thickness", e.thickness);
    });
    ar.list("carriers", d.filth.carriers, [&](FilthState::Carrier& c) {
        ar.value("pawn", c.pawn);
        ar.value("lastCell", c.lastCell);
        ar.list("carried", c.carried, [&](std::pair<std::string, int>& p) {
            ar.text("kind", p.first);
            ar.value("count", p.second);
        });
    });
    ar.value("filthRngState", d.filth.rngState);
    ar.value("filthRngIncrement", d.filth.rngIncrement);
    ar.list("beautyNeeds", d.roomStats.needs, [&](RoomStatsState::Need& n) {
        ar.value("pawn", n.pawn);
        ar.value("level", n.level);
        ar.value("perceived", n.perceived);
        ar.value("sampledAt", n.sampledAt);
    });
}

// Formats 6 to 19, read only: one long layout, each format adding to it.
template <typename Ar>
void saveLegacyData(Ar& ar, SaveData& d) {
    ar.text("simVersion", d.simVersion);
    ar.value("defsHash", d.defsHash);
    ar.group("settings", [&] { saveSettings(ar, d.settings); });
    ar.value("seconds", d.seconds);
    ar.value("rngState", d.rngState);
    ar.value("rngIncrement", d.rngIncrement);
    ar.value("groundHash", d.groundHash);
    ar.optional("stockpile", d.stockpile, [&](CellPos& c) { saveCell(ar, c); });
    ar.list("haulers", d.haulers, [&](uint32_t& id) { ar.value(nullptr, id); });
    ar.value("hauls", d.hauls);
    ar.value("failedJobs", d.failedJobs);
    if (ar.version() >= 3) {
        ar.value("repaths", d.repaths);
    }
    ar.list("setupErrors", d.setupErrors, [&](std::string& s) { ar.text(nullptr, s); });
    ar.value("submitted", d.submitted);
    ar.list("pending", d.pending, [&](SavePending& p) {
        ar.value("id", p.id);
        saveCommand(ar, p.command, nullptr);
    });
    ar.list("commandLog", d.commandLog, [&](LoggedCommand& logged) {
        ar.value("step", logged.step);
        saveCommand(ar, logged.command, nullptr);
    });
    ar.list("checkpoints", d.checkpoints, [&](Checkpoint& c) {
        ar.value("step", c.step);
        ar.value("hash", c.hash);
    });
    ar.list("freeIds", d.freeIds, [&](uint32_t& id) { ar.value(nullptr, id); });
    ar.list("entities", d.entities, [&](SaveEntity& e) { saveLegacyEntity(ar, e); });
    if (ar.version() >= 2) {
        ar.group("paths", [&] { savePaths(ar, d.paths); });
    }
    ar.value("nextZoneId", d.nextZoneId);
    ar.list("zones", d.zones, [&](SaveZone& z) { saveZone(ar, z); });
    if (ar.version() >= 9) {
        ar.group("plants", [&] { savePlants(ar, d.plants); });
    }
    if (ar.version() >= 14) {
        d.hasRock = true;
        ar.group("mineables", [&] { saveMineables(ar, d.mineables); });
        ar.group("designations", [&] { saveDesignations(ar, d.designations); });
    }
}

template <typename Ar>
void saveQuests(Ar& ar, QuestSystemState& q) {
    ar.value("next", q.next);
    ar.list("quests", q.quests, [&](Quest& quest) {
        ar.value("id", quest.id);
        ar.text("kind", quest.kind);
        ar.value("state", quest.state);
        ar.require(static_cast<int>(quest.state) <= static_cast<int>(QuestState::Expired), "the save file has a bad quest state");
        ar.value("offeredAt", quest.offeredAt);
        ar.value("expiresAt", quest.expiresAt);
        ar.value("acceptedAt", quest.acceptedAt);
        ar.value("launchAt", quest.launchAt);
        ar.value("deadline", quest.deadline);
        ar.value("launched", quest.launched);
        ar.value("count", quest.count);
        ar.value("points", quest.points);
        ar.text("faction", quest.faction);
        ar.value("squad", quest.squad);
        ar.list("people", quest.people, [&](Entity& p) { ar.value(nullptr, p); });
        ar.list("rewards", quest.rewards, [&](Quest::Reward& r) {
            ar.text("thing", r.thing);
            ar.value("count", r.count);
        });
    });
    ar.require(q.next > 0, "the save file has a bad quest counter");
}

template <typename Ar>
void saveRaids(Ar& ar, RaidState& r) {
    ar.list("factions", r.factions, [&](std::string& f) { ar.text(nullptr, f); });
    ar.list("squads", r.squads, [&](Squad& s) {
        ar.value("id", s.id);
        ar.text("faction", s.faction);
        ar.value("side", s.side);
        ar.value("points", s.points);
        ar.value("started", s.started);
        ar.value("arrivedAt", s.arrivedAt);
        ar.value("giveUpAt", s.giveUpAt);
        ar.value("state", s.state);
        ar.require(static_cast<int>(s.state) <= static_cast<int>(SquadState::Rout), "the save file has a bad squad state");
        if (ar.sectionVersion() >= 2) {  // F12
            ar.value("strategy", s.strategy);
            ar.require(static_cast<int>(s.strategy) < kRaidStrategyCount, "the save file has a bad raid strategy");
            ar.value("hasBreach", s.hasBreach);
            ar.group("breach", [&] { saveCell(ar, s.breach); });
            ar.group("breachFrom", [&] { saveCell(ar, s.breachFrom); });
        }
        ar.value("gaveUp", s.gaveUp);
        ar.group("entry", [&] { saveCell(ar, s.entry); });
        ar.value("hasTarget", s.hasTarget);
        ar.group("target", [&] { saveCell(ar, s.target); });
        ar.value("nextThink", s.nextThink);
        ar.list("members", s.members, [&](Entity& m) { ar.value(nullptr, m); });
    });
    ar.value("nextSquad", r.nextSquad);
    if (ar.sectionVersion() >= 2) {  // F12: raids waiting for their hour
        ar.list("pending", r.pending, [&](PendingRaid& p) {
            ar.text("faction", p.faction);
            ar.value("points", p.points);
            ar.value("edge", p.edge);
            ar.value("strategy", p.strategy);
            ar.require(static_cast<int>(p.strategy) < kRaidStrategyCount, "the save file has a bad raid strategy");
            ar.value("at", p.at);
        });
    }
    ar.value("alarm", r.alarm);
    ar.value("manual", r.manual);
    ar.value("suppressed", r.suppressed);
    ar.value("hasThreat", r.hasThreat);
    ar.value("threatX", r.threatX);
    ar.value("threatY", r.threatY);
    ar.value("threatSeenAt", r.threatSeenAt);
    ar.value("hasHome", r.hasHome);
    ar.value("homeX", r.homeX);
    ar.value("homeY", r.homeY);
    ar.value("hasShelter", r.hasShelter);
    ar.group("shelter", [&] { saveCell(ar, r.shelter); });
    ar.value("nextAlarmThink", r.nextAlarmThink);
    if (ar.sectionVersion() >= 2) {
        ar.value("adaptDays", r.adaptDays);
        ar.value("nextAdaptGrowth", r.nextAdaptGrowth);
        ar.list("adaptPawns", r.adaptPawns, [&](RaidState::AdaptPawn& p) {
            ar.value("pawn", p.pawn);
            ar.value("downed", p.downed);
        });
    }
}

template <typename Ar>
void saveTrade(Ar& ar, TradeState& t) {
    ar.value("nextVisit", t.nextVisit);
    ar.value("lastDrift", t.lastDrift);
    ar.list("standings", t.standings, [&](Standing& s) {
        ar.text("faction", s.faction);
        ar.value("goodwill", s.goodwill);
        ar.require(std::isfinite(s.goodwill), "the save file has an impossible goodwill");
    });
    ar.list("visits", t.visits, [&](Visit& v) {
        ar.value("id", v.id);
        ar.value("kind", v.kind);
        ar.require(static_cast<int>(v.kind) <= static_cast<int>(VisitKind::Orbital), "the save file has a bad visit kind");
        ar.value("phase", v.phase);
        ar.require(static_cast<int>(v.phase) <= static_cast<int>(VisitPhase::Leaving), "the save file has a bad visit phase");
        ar.text("faction", v.faction);
        ar.text("traderKind", v.traderKind);
        ar.value("side", v.side);
        ar.value("arrivedAt", v.arrivedAt);
        ar.value("leaveAt", v.leaveAt);
        ar.value("nextWander", v.nextWander);
        ar.group("entry", [&] { saveCell(ar, v.entry); });
        ar.group("spot", [&] { saveCell(ar, v.spot); });
        ar.value("silver", v.silver);
        ar.value("started", v.started);
        ar.value("trader", v.trader);
        ar.list("stock", v.stock, [&](TradeLine& l) { saveTradeLine(ar, l); });
        ar.list("members", v.members, [&](Entity& m) { ar.value(nullptr, m); });
    });
}

// ---- Sections (format 20 on) -----------------------------------------------------------------

// A thing's part as saved in a section: the thing's id, then the part.
template <typename T>
struct SaveTagged {
    uint32_t id = 0;
    T value;
};

// One kind of part of things, as a list of (thing id, part) for every thing that has it, in id
// order, named like the part (the JSON dump of formats 6 to 19 used the same names on each thing). Reading puts each back on the thing the "things" section made with that id.
template <typename Ar, typename T, typename F>
void saveParts(Ar& ar, const char* name, std::vector<SaveEntity>& entities, std::optional<T> SaveEntity::*part, F fn) {
    std::vector<SaveTagged<T>> items;
    if constexpr (!Ar::kReading) {
        for (const SaveEntity& e : entities) {
            if (e.*part) {
                items.push_back(SaveTagged<T>{e.id, *(e.*part)});
            }
        }
    }
    ar.list(name, items, [&](SaveTagged<T>& item) {
        ar.value("id", item.id);
        fn(item.value);
    });
    if constexpr (Ar::kReading) {
        for (SaveTagged<T>& item : items) {
            const auto at = std::lower_bound(entities.begin(), entities.end(), item.id,
                                             [](const SaveEntity& e, uint32_t id) { return e.id < id; });
            if (at == entities.end() || at->id != item.id || (*at).*part) {
                ar.require(false, "the save file gives a part to a thing that isn't there");
                return;
            }
            (*at).*part = std::move(item.value);
        }
    }
}

template <typename Ar>
void saveThingsSection(Ar& ar, SaveData& d) {
    ar.list("freeIds", d.freeIds, [&](uint32_t& id) { ar.value(nullptr, id); });
    ar.list("entities", d.entities, [&](SaveEntity& e) {
        ar.value("id", e.id);
        ar.optional("thing", e.thing, [&](SaveThing& t) { saveThingPart(ar, t); });
        ar.optional("position", e.position, [&](Position& p) { savePositionPart(ar, p); });
        ar.optional("footprint", e.footprint, [&](Footprint& f) { saveFootprintPart(ar, f); });
        ar.optional("stackCount", e.stack, [&](StackCount& s) { saveStackPart(ar, s); });
        ar.optional("hitPoints", e.hitPoints, [&](HitPoints& hp) { saveHitPointsPart(ar, hp); });
    });
    for (size_t i = 1; i < d.entities.size(); ++i) {
        ar.require(d.entities[i - 1].id < d.entities[i].id, "the save file's things are out of order");
    }
}

template <typename Ar>
void saveCommandsSection(Ar& ar, SaveData& d) {
    SaveCommandNames names;
    if constexpr (!Ar::kReading) {
        names.indexOfType.assign(std::variant_size_v<Command>, UINT16_MAX);
        const auto see = [&](const Command& command) {
            uint16_t& index = names.indexOfType[command.index()];
            if (index == UINT16_MAX) {
                index = static_cast<uint16_t>(names.names.size());
                names.names.push_back(commandName(command));
            }
        };
        for (const SavePending& p : d.pending) {
            see(p.command);
        }
        for (const LoggedCommand& logged : d.commandLog) {
            see(logged.command);
        }
    }
    ar.list("commandNames", names.names, [&](std::string& name) { ar.text(nullptr, name); });
    if constexpr (Ar::kReading) {
        for (const std::string& name : names.names) {
            Command blank;
            if (!commandFromName(name, blank)) {
                ar.fail("this save has a command this version of the game doesn't know ('" + name +
                        "'), so it was made by a newer version");
                return;
            }
            names.blanks.push_back(std::move(blank));
        }
    }
    ar.value("submitted", d.submitted);
    ar.list("pending", d.pending, [&](SavePending& p) {
        ar.value("id", p.id);
        saveCommand(ar, p.command, &names);
    });
    ar.list("commandLog", d.commandLog, [&](LoggedCommand& logged) {
        ar.value("step", logged.step);
        saveCommand(ar, logged.command, &names);
    });
    ar.list("checkpoints", d.checkpoints, [&](Checkpoint& c) {
        ar.value("step", c.step);
        ar.value("hash", c.hash);
    });
}

// Every section, in the order they are written and read (a section of parts comes after
// "things"). Each has its own version: a task that changes what a section holds bumps that
// number and reads the old layout with `if (ar.sectionVersion() >= 2)`; a task that adds new
// state adds its own section at the end. A file without a section loads it as empty, which is
// how saves from before a section existed load. See docs/design/saves.md.
template <typename Ar>
void saveSections(Ar& ar, SaveData& d) {
    ar.section("sim", 1, [&] {
        ar.text("simVersion", d.simVersion);
        ar.value("defsHash", d.defsHash);
        ar.group("settings", [&] { saveSettings(ar, d.settings); });
        ar.value("seconds", d.seconds);
        ar.value("rngState", d.rngState);
        ar.value("rngIncrement", d.rngIncrement);
        ar.value("groundHash", d.groundHash);
        ar.optional("stockpile", d.stockpile, [&](CellPos& c) { saveCell(ar, c); });
        ar.list("haulers", d.haulers, [&](uint32_t& id) { ar.value(nullptr, id); });
        ar.value("hauls", d.hauls);
        ar.value("failedJobs", d.failedJobs);
        ar.value("repaths", d.repaths);
        ar.list("setupErrors", d.setupErrors, [&](std::string& s) { ar.text(nullptr, s); });
    });
    ar.section("commands", 1, [&] { saveCommandsSection(ar, d); });
    ar.section("things", 1, [&] { saveThingsSection(ar, d); });
    std::vector<SaveEntity>& e = d.entities;
    ar.section("movement", 2, [&] {
        saveParts(ar, "mover", e, &SaveEntity::mover, [&](Mover& m) { saveMoverPart(ar, m, std::nullopt); });
    });
    ar.section("jobs", 1, [&] {
        saveParts(ar, "worker", e, &SaveEntity::worker, [&](SaveWorker& w) { saveWorkerPart(ar, w); });
        saveParts(ar, "work", e, &SaveEntity::work, [&](SaveWorkSettings& w) { saveWorkPart(ar, w); });
        saveParts(ar, "carrying", e, &SaveEntity::carrying, [&](Carrying& c) { saveCarryingPart(ar, c); });
    });
    ar.section("colonists", 2, [&] {
        saveParts(ar, "colonist", e, &SaveEntity::colonist, [&](Colonist& c) { saveColonistPart(ar, c); });
    });
    ar.section("health", 6, [&] {
        saveParts(ar, "health", e, &SaveEntity::health, [&](SaveHealth& h) { saveHealthPart(ar, h); });
    });
    ar.section("control", 1, [&] {
        std::vector<uint32_t> controlled;
        for (const SaveEntity& thing : e) {
            if (thing.playerControlled) {
                controlled.push_back(thing.id);
            }
        }
        ar.list("playerControlled", controlled, [&](uint32_t& id) { ar.value(nullptr, id); });
        if constexpr (Ar::kReading) {
            for (const uint32_t id : controlled) {
                const auto at = std::lower_bound(e.begin(), e.end(), id, [](const SaveEntity& t, uint32_t i) { return t.id < i; });
                ar.require(at != e.end() && at->id == id, "the save file gives a part to a thing that isn't there");
                if (at != e.end() && at->id == id) {
                    at->playerControlled = true;
                }
            }
        }
    });
    ar.section("needs", 1, [&] {
        saveParts(ar, "needs", e, &SaveEntity::needs, [&](Needs& n) { saveNeedsPart(ar, n); });
        saveParts(ar, "schedule", e, &SaveEntity::schedule, [&](Schedule& s) { saveSchedulePart(ar, s); });
        saveParts(ar, "bed", e, &SaveEntity::bed, [&](Bed& b) { saveBedPart(ar, b); });
    });
    ar.section("bills", 2, [&] {
        saveParts(ar, "bills", e, &SaveEntity::bills, [&](SaveBillList& list) { saveBillsPart(ar, list); });
    });
    ar.section("construction", 1, [&] {
        saveParts(ar, "site", e, &SaveEntity::site, [&](SaveSite& s) { saveSitePart(ar, s); });
        saveParts(ar, "builtOf", e, &SaveEntity::builtOf, [&](std::string& stuff) { ar.text("stuff", stuff); });
        saveParts(ar, "marked", e, &SaveEntity::marked, [&](SaveMark&) {});
    });
    ar.section("mood", 1, [&] {
        saveParts(ar, "mood", e, &SaveEntity::mood, [&](Mood& m) { saveMoodPart(ar, m); });
    });
    ar.section("combat", 2, [&] {
        saveParts(ar, "posture", e, &SaveEntity::posture, [&](Posture& p) { savePosturePart(ar, p); });
        saveParts(ar, "gear", e, &SaveEntity::gear, [&](Gear& g) { saveGearPart(ar, g); });
        saveParts(ar, "equippedBy", e, &SaveEntity::equippedBy, [&](EquippedBy& b) { saveEquippedByPart(ar, b); });
        saveParts(ar, "shooter", e, &SaveEntity::shooter, [&](Shooter& sh) { saveShooterPart(ar, sh); });
        saveParts(ar, "projectile", e, &SaveEntity::projectile, [&](SaveProjectile& sp) { saveProjectilePart(ar, sp); });
        saveParts(ar, "magazine", e, &SaveEntity::magazine, [&](Magazine& m) { saveMagazinePart(ar, m); });
    });
    ar.section("doors", 1, [&] {
        saveParts(ar, "door", e, &SaveEntity::door, [&](Door& door) { saveDoorPart(ar, door); });
    });
    ar.section("fighters", 2, [&] {
        saveParts(ar, "faction", e, &SaveEntity::faction, [&](Faction& f) { saveFactionPart(ar, f); });
        saveParts(ar, "fighter", e, &SaveEntity::fighter, [&](Fighter& f) { saveFighterPart(ar, f); });
    });
    ar.section("looks", 1, [&] {
        saveParts(ar, "looks", e, &SaveEntity::looks, [&](PawnLooks& l) { saveLooksPart(ar, l); });
    });
    ar.section("paths", 1, [&] { savePaths(ar, d.paths); });
    ar.section("zones", 1, [&] {
        ar.value("nextZoneId", d.nextZoneId);
        ar.list("zones", d.zones, [&](SaveZone& z) { saveZone(ar, z); });
    });
    ar.section("plants", 1, [&] { savePlants(ar, d.plants); });
    ar.section("mining", 1, [&] {
        d.hasRock = true;
        saveMineables(ar, d.mineables);
        saveDesignations(ar, d.designations);
    });
    ar.section("power", 1, [&] {
        saveParts(ar, "power", e, &SaveEntity::power, [&](PowerPart& part) { savePowerPart(ar, part); });
    });
    ar.section("recreation", 1, [&] {
        saveParts(ar, "joy", e, &SaveEntity::joy, [&](SaveJoy& j) { saveJoyPart(ar, j); });
    });
    ar.section("research", 1, [&] { saveResearch(ar, d.research); });
    ar.section("ship", 1, [&] { saveShip(ar, d.ship); });
    ar.section("medicine", 2, [&] {
        saveParts(ar, "medical", e, &SaveEntity::medical, [&](MedicalSettings& m) {
            ar.value("care", m.care);
            ar.require(static_cast<int>(m.care) <= static_cast<int>(MedicalCare::Best), "the save file has a bad medical care setting");
        });
        if (ar.sectionVersion() >= 2) {
            saveParts(ar, "operations", e, &SaveEntity::operations, [&](SaveOperations& ops) {
                ar.value("nextId", ops.nextId);
                ar.list("list", ops.list, [&](SaveOperation& op) {
                    ar.value("id", op.id);
                    ar.text("recipe", op.recipe);
                    ar.value("part", op.part);
                    ar.require(op.id > 0 && op.id < ops.nextId, "the save file has an operation with a bad number");
                });
            });
        }
    });
    ar.section("talents", 1, [&] { saveTalents(ar, d); });
    ar.section("drugs", 1, [&] { saveDrugs(ar, d); });
    ar.section("roomstats", 1, [&] { saveRoomStatsSection(ar, d); });
    ar.section("animals", 2, [&] {
        ar.value("wildAnimals", d.wildAnimals);
        ar.value("populated", d.animals.populated);
        ar.value("percent", d.animals.percent);
        ar.value("rngState", d.animals.rngState);
        ar.value("rngIncrement", d.animals.rngIncrement);
        ar.value("nextHerd", d.animals.nextHerd);
        ar.value("over", d.animals.over);
        ar.value("arrived", d.animals.stats.arrived);
        ar.value("left", d.animals.stats.left);
        ar.value("grazed", d.animals.stats.grazed);
        ar.value("cleared", d.animals.stats.cleared);
        if (ar.sectionVersion() >= 2) {  // N5
            ar.value("manhunters", d.animals.stats.manhunters);
            ar.value("revenges", d.animals.stats.revenges);
            ar.value("hunts", d.animals.stats.hunts);
            ar.value("kills", d.animals.stats.kills);
            ar.value("meals", d.animals.stats.meals);
            ar.value("scares", d.animals.stats.scares);
        }
        ar.require(d.wildAnimals >= 0 && d.animals.percent >= 0, "the save file has a bad wild animal density");
        saveParts(ar, "animal", e, &SaveEntity::animal, [&](Animal& a) { saveAnimalPart(ar, a); });
    });
    ar.section("husbandry", 3, [&] {
        ar.value("nextZone", d.husbandry.nextZone);
        ar.list("zones", d.husbandry.zones, [&](AnimalZone& z) {
            ar.value("id", z.id);
            ar.list("cells", z.cells, [&](CellPos& c) { saveCell(ar, c); });
            if (ar.sectionVersion() >= 2) {
                ar.value("breedingOff", z.breedingOff);
            }
        });
        ar.value("tamed", d.husbandry.stats.tamed);
        ar.value("tameFailed", d.husbandry.stats.tameFailed);
        ar.value("trainingSteps", d.husbandry.stats.trainingSteps);
        ar.value("trainingFailed", d.husbandry.stats.trainingFailed);
        ar.value("slaughtered", d.husbandry.stats.slaughtered);
        ar.value("fed", d.husbandry.stats.fed);
        ar.value("starved", d.husbandry.stats.starved);
        if (ar.sectionVersion() >= 2) {
            ar.value("hunted", d.husbandry.stats.hunted);
        }
        saveParts(ar, "care", e, &SaveEntity::care, [&](AnimalCare& c) { saveCarePart(ar, c); });
    });
    ar.section("farming", 1, [&] {
        ar.value("nextZone", d.farming.nextZone);
        ar.list("zones", d.farming.zones, [&](FarmingSaved::Zone& z) {
            ar.value("id", z.id);
            ar.text("crop", z.crop);
            ar.list("cells", z.cells, [&](CellPos& c) { saveCell(ar, c); });
        });
        ar.value("sown", d.farming.stats.sown);
        ar.value("harvested", d.farming.stats.harvested);
        ar.value("frostKilled", d.farming.stats.frostKilled);
        ar.value("heatKilled", d.farming.stats.heatKilled);
    });
    ar.section("temperature", 1, [&] {
        ar.list("roomTemperatures", d.roomTemperatures, [&](RoomTemperature& t) {
            ar.group("anchor", [&] { saveCell(ar, t.anchor); });
            ar.value("celsius", t.celsius);
            ar.require(t.celsius > -500.0f && t.celsius < 1000.0f, "the save file has an impossible room temperature");
        });
    });
    ar.section("raids", 2, [&] {
        saveRaids(ar, d.raids);
        saveParts(ar, "defense", e, &SaveEntity::defense, [&](DefensePlan& p) {
            ar.value("role", p.role);
            ar.require(static_cast<int>(p.role) <= static_cast<int>(DefenseRole::KeepWorking), "the save file has a bad defense role");
            ar.value("hasSpot", p.hasSpot);
            ar.group("spot", [&] { saveCell(ar, p.spot); });
        });
        saveParts(ar, "raider", e, &SaveEntity::raider, [&](Raider& r) {
            ar.value("squad", r.squad);
            ar.text("kind", r.kind);
            if (ar.sectionVersion() >= 2) {  // F12
                ar.value("goalThing", r.goalThing);
                ar.value("loot", r.loot);
                ar.value("sparesDowned", r.sparesDowned);
            }
        });
        if (ar.sectionVersion() >= 2) {
            saveParts(ar, "kidnapped", e, &SaveEntity::kidnapped, [&](Kidnapped& k) { ar.value("by", k.by); });
        }
    });
    ar.section("spoilage", 1, [&] {
        ar.value("rngState", d.spoilageState.rngState);
        ar.value("rngIncrement", d.spoilageState.rngIncrement);
        saveParts(ar, "spoilage", e, &SaveEntity::spoilage, [&](SaveSpoilage& s) {
            ar.value("rot", s.rot);
            ar.value("decay", s.decay);
            ar.value("poisoned", s.poisoned);
            ar.list("ingredients", s.ingredients, [&](std::string& name) { ar.text(nullptr, name); });
            ar.require(s.rot >= 0.0f && s.rot < 1.0f && s.decay >= 0.0f && s.decay < 1.0f, "the save file has an impossible rot or wear");
        });
    });
    ar.section("melee", 1, [&] {
        saveParts(ar, "melee", e, &SaveEntity::melee, [&](Melee& m) { saveMeleePart(ar, m); });
    });
    ar.section("social", 1, [&] {
        saveParts(ar, "social", e, &SaveEntity::social, [&](Social& social) { saveSocialPart(ar, social); });
        saveParts(ar, "fight", e, &SaveEntity::socialFight, [&](SocialFight& f) { saveSocialFightPart(ar, f); });
    });
    ar.section("apparel", 1, [&] {
        saveParts(ar, "apparel", e, &SaveEntity::apparel, [&](SaveApparel& a) { saveApparelPart(ar, a); });
        ar.value("nextOutfitId", d.nextOutfitId);
        ar.list("outfits", d.outfits, [&](OutfitPolicy& o) { saveOutfitPolicy(ar, o); });
    });
    ar.section("weapons", 1, [&] {
        saveParts(ar, "stowed", e, &SaveEntity::stowed, [&](std::vector<Entity>& items) { saveStowedPart(ar, items); });
        saveParts(ar, "switch", e, &SaveEntity::weaponSwitch, [&](WeaponSwitch& w) { saveWeaponSwitchPart(ar, w); });
    });
    ar.section("feel", 1, [&] {
        saveParts(ar, "kick", e, &SaveEntity::kick, [&](RecoilKick& k) { saveKickPart(ar, k); });
        saveParts(ar, "roll", e, &SaveEntity::roll, [&](DodgeRoll& r) { saveRollPart(ar, r); });
    });
    ar.section("weather", 1, [&] {
        d.hasWeather = true;
        saveWeather(ar, d.weather);
    });
    ar.section("explosives", 1, [&] {
        saveParts(ar, "belt", e, &SaveEntity::belt, [&](SaveBelt& b) { saveBeltPart(ar, b); });
        saveParts(ar, "thrower", e, &SaveEntity::thrower, [&](SaveThrower& t) { saveThrowerPart(ar, t); });
        saveParts(ar, "grenade", e, &SaveEntity::grenade, [&](SaveGrenade& g) { saveGrenadePart(ar, g); });
        saveParts(ar, "knockedDown", e, &SaveEntity::knocked, [&](KnockedDown& k) { saveKnockedPart(ar, k); });
    });
    ar.section("breeding", 3, [&] {
        ar.value("matings", d.breeding.matings);
        ar.value("pregnancies", d.breeding.pregnancies);
        ar.value("born", d.breeding.born);
        ar.value("eggsLaid", d.breeding.eggsLaid);
        ar.value("hatched", d.breeding.hatched);
        ar.value("milked", d.breeding.milked);
        ar.value("sheared", d.breeding.sheared);
        saveParts(ar, "breed", e, &SaveEntity::breed, [&](AnimalBreed& b) { saveBreedPart(ar, b); });
        saveParts(ar, "hatching", e, &SaveEntity::hatching, [&](Hatching& h) { saveHatchingPart(ar, h); });
        if (ar.sectionVersion() >= 3) {
            saveParts(ar, "young", e, &SaveEntity::young, [&](Young& y) { saveYoungPart(ar, y); });
        }
    });
    ar.section("defense", 1, [&] {
        saveParts(ar, "turret", e, &SaveEntity::turret, [&](Turret& t) { saveTurretPart(ar, t); });
        saveParts(ar, "trap", e, &SaveEntity::trap, [&](Trap& t) { saveTrapPart(ar, t); });
    });
    ar.section("narrator", 1, [&] {
        d.hasNarrator = true;
        NarratorState& n = d.narrator;
        ar.text("narrator", n.narrator);
        ar.text("difficulty", n.difficulty);
        ar.value("rngState", n.rngState);
        ar.value("rngIncrement", n.rngIncrement);
        ar.value("cycleStart", n.cycleStart);
        ar.list("threats", n.threats, [&](int64_t& t) { ar.value(nullptr, t); });
        ar.list("fired", n.fired, [&](IncidentFired& f) {
            ar.text("incident", f.incident);
            ar.value("at", f.at);
        });
        ar.value("incidents", n.incidents);
        ar.value("wealth", n.wealth);
        ar.value("points", n.points);
    });
    ar.section("disasters", 1, [&] {
        d.hasDisasters = true;
        DisasterState& s = d.disasters;
        ar.value("rngState", s.rngState);
        ar.value("rngIncrement", s.rngIncrement);
        ar.value("started", s.started);
        ar.list("active", s.active, [&](DisasterCondition& c) {
            uint8_t kind = static_cast<uint8_t>(c.kind);
            ar.value("kind", kind);
            ar.require(kind < kDisasterKindCount, "the save file has a disaster that does not exist");
            c.kind = static_cast<DisasterKind>(kind);
            ar.value("startedAt", c.startedAt);
            ar.value("endsAt", c.endsAt);
            ar.value("level", c.level);
            ar.value("female", c.female);
            ar.value("centreX", c.centre.x);
            ar.value("centreY", c.centre.y);
            ar.value("centreLevel", c.centre.level);
            int radius = static_cast<int>(c.radius);  // metres, whole
            ar.value("radius", radius);
            c.radius = static_cast<float>(radius);
            ar.value("nextStrike", c.nextStrike);
        });
    });
    ar.section("fire", 1, [&] {
        ar.value("started", d.fire.started);
        ar.value("burntUp", d.fire.burntUp);
        ar.value("pawnsBurnt", d.fire.pawnsBurnt);
        ar.value("autoHome", d.fire.autoHome);
        ar.value("homeStarted", d.fire.homeStarted);
        ar.list("fires", d.fire.fires, [&](FireRecord& r) {
            ar.value("cell", r.cell);
            ar.value("size", r.fire.size);
            ar.value("age", r.fire.age);
            ar.value("plantDamage", r.fire.plantDamage);
            ar.require(r.fire.size > 0.0f && r.fire.size < 100.0f, "the save file has an impossible fire");
        });
        ar.list("burnt", d.fire.burnt, [&](BurntRecord& r) {
            ar.value("cell", r.cell);
            ar.value("at", r.at);
        });
        ar.list("home", d.fire.home, [&](int& cell) { ar.value("cell", cell); });
        saveParts(ar, "burning", e, &SaveEntity::burning, [&](Burning& b) { ar.value("until", b.until); });
    });
    ar.section("prisoners", 2, [&] {
        saveParts(ar, "prisoner", e, &SaveEntity::prisoner, [&](Prisoner& p) {
            ar.value("mode", p.mode);
            ar.value("resistance", p.resistance);
            ar.value("capturedAt", p.capturedAt);
            ar.value("nextChat", p.nextChat);
            if (ar.sectionVersion() >= 2) {  // executions
                ar.value("guiltyUntil", p.guiltyUntil);
            }
            ar.require(static_cast<int>(p.mode) <= static_cast<int>(PrisonerMode::Execute), "the save file has a bad prisoner mode");
        });
        saveParts(ar, "prisonBed", e, &SaveEntity::prisonBed, [&](PrisonBed& b) { ar.value("marked", b.marked); });
    });
    ar.section("levels", 1, [&] {
        ar.list("upperFloors", d.upperFloors, [&](CellPos& c) {
            saveCell(ar, c);
            ar.require(c.level >= 1, "the save file has an upper floor on the ground");
        });
    });
    ar.section("fuel", 1, [&] {
        saveParts(ar, "fuel", e, &SaveEntity::fuel, [&](Fuel& f) { saveFuelPart(ar, f); });
    });
    ar.section("roofs", 1, [&] {
        ar.value("collapses", d.roofs.collapses);
        ar.list("thin", d.roofs.thin, [&](CellPos& c) { saveCell(ar, c); });
        ar.list("fallenThick", d.roofs.fallenThick, [&](int& cell) { ar.value("cell", cell); });
        ar.list("pending", d.roofs.pending, [&](CellPos& c) { saveCell(ar, c); });
    });
    ar.section("crafting", 1, [&] {
        ar.value("rngState", d.craftingState.rngState);
        ar.value("rngIncrement", d.craftingState.rngIncrement);
        saveParts(ar, "storeIn", e, &SaveEntity::storeIn, [&](StoreIn& s) { ar.value("zone", s.zone); });
    });
    ar.section("mentalBreaks", 1, [&] {
        saveParts(ar, "mentalBreaker", e, &SaveEntity::mentalBreaker, [&](MentalBreaker& b) { saveMentalBreakerPart(ar, b); });
    });
    ar.section("trade", 1, [&] {
        saveTrade(ar, d.trade);
        saveParts(ar, "visitor", e, &SaveEntity::visitor, [&](Visitor& v) {
            ar.value("visit", v.visit);
            ar.value("role", v.role);
            ar.require(static_cast<int>(v.role) <= static_cast<int>(VisitorRole::Guard), "the save file has a bad visitor role");
        });
    });
    ar.section("quests", 1, [&] { saveQuests(ar, d.quests); });
    ar.section("inspirations", 1, [&] {
        saveParts(ar, "inspired", e, &SaveEntity::inspired, [&](Inspired& i) {
            ar.text("def", i.def);
            ar.value("startedAt", i.startedAt);
            ar.value("endsAt", i.endsAt);
        });
    });
    ar.section("world", 1, [&] {
        ar.value("enabled", d.world.enabled);
        ar.value("seed", d.world.seed);
        ar.value("width", d.world.width);
        ar.value("height", d.world.height);
        ar.value("startTile", d.world.startTile);
        ar.value("hash", d.worldHash);
    });
    // New sections go here, at the end.
}

// The file: magic, format version, the sections, then a checksum of everything before it.
std::vector<uint8_t> saveEncode(SaveData& data) {
    SaveBinaryWriter writer;
    std::vector<uint8_t>& bytes = writer.bytes();
    bytes.insert(bytes.end(), kSaveMagic, kSaveMagic + kSaveMagicSize);
    uint32_t version = kSaveFormatVersion;
    writer.value(nullptr, version);
    saveSections(writer, data);
    uint64_t checksum = saveChecksum(bytes.data(), bytes.size());
    writer.value(nullptr, checksum);
    return std::move(bytes);
}

bool saveDecode(const std::vector<uint8_t>& bytes, SaveData& out, std::string& error) {
    const size_t headerSize = kSaveMagicSize + sizeof(uint32_t);
    if (bytes.size() < headerSize + sizeof(uint64_t) || std::memcmp(bytes.data(), kSaveMagic, kSaveMagicSize) != 0) {
        error = "this is not a Rim3D save file";
        return false;
    }
    uint32_t version = 0;
    for (size_t k = 0; k < 4; ++k) {
        version |= static_cast<uint32_t>(bytes[kSaveMagicSize + k]) << (8 * k);
    }
    if (version == 0 || version > kSaveFormatVersion) {
        error = "this save is format " + std::to_string(version) + ", made by a newer version of the game (this one reads up to " +
                std::to_string(kSaveFormatVersion) + ")";
        return false;
    }
    if (version < kSaveOldestFormat) {
        error = "this save is format " + std::to_string(version) + ", made before the job system; this version reads format " +
                std::to_string(kSaveOldestFormat) + " and later";
        return false;
    }
    const size_t bodyEnd = bytes.size() - sizeof(uint64_t);
    uint64_t stored = 0;
    for (size_t k = 0; k < 8; ++k) {
        stored |= static_cast<uint64_t>(bytes[bodyEnd + k]) << (8 * k);
    }
    if (saveChecksum(bytes.data(), bodyEnd) != stored) {
        error = "the save file is damaged (its checksum doesn't match)";
        return false;
    }
    SaveBinaryReader reader(bytes.data() + headerSize, bodyEnd - headerSize, version);
    out = SaveData();
    if (version >= kSaveSectionsFormat) {
        reader.readSections();
        saveSections(reader, out);
        const std::string* unknown = reader.failed() ? nullptr : reader.unknownSection();
        if (unknown != nullptr) {
            error = "this save has a '" + *unknown + "' section this version of the game doesn't know, so it was made by a newer version";
            return false;
        }
    } else {
        saveLegacyData(reader, out);
    }
    if (reader.failed()) {
        error = reader.error();
        return false;
    }
    if (!reader.atEnd()) {
        error = "the save file has extra data after the end";
        return false;
    }
    return true;
}

std::string saveJsonText(SaveData& data) {
    SaveJson root = SaveJson::object();
    root["format"] = kSaveFormatName;
    root["formatVersion"] = kSaveFormatVersion;
    SaveJsonWriter writer(root);
    saveSections(writer, data);
    return root.dump(1) + "\n";
}

}  // namespace

// Reads and restores the sim's private state. A friend of Sim.
class SimSaveAccess {
public:
    static SaveData collect(const Sim& sim);
    static bool restore(Sim& sim, SaveData& data, std::string& error);
    // A sim with the saved settings and a regenerated map, ready for restore.
    static std::unique_ptr<Sim> makeEmpty(const SaveData& data, std::shared_ptr<const DefDatabase> defs,
                                          std::string& error);
};

std::unique_ptr<Sim> SimSaveAccess::makeEmpty(const SaveData& data, std::shared_ptr<const DefDatabase> defs,
                                              std::string& error) {
    SimSettings settings = data.settings;
    settings.wildAnimals = data.wildAnimals;  // saved in the animals section, not the sim section
    settings.world = data.world;              // and the world in its own
    auto sim = std::unique_ptr<Sim>(new Sim(settings, std::move(defs), Sim::EmptyStart{}));
    if (sim->worldHash_ != data.worldHash) {
        error = "the world generator gives a different world for this seed than when the save was made";
        return nullptr;
    }
    if (data.world.enabled && sim->world_->startTile() != data.world.startTile) {
        error = "the save's start tile can't start a colony on this world";
        return nullptr;
    }
    if (sim->groundHash_ != data.groundHash) {
        error = "the map generator gives a different map for this seed than when the save was made";
        return nullptr;
    }
    return sim;
}

SaveData SimSaveAccess::collect(const Sim& sim) {
    SaveData d;
    d.simVersion = kSimVersion;
    d.defsHash = sim.defs_->contentHash();
    d.settings = sim.settings_;
    d.wildAnimals = sim.settings_.wildAnimals;
    d.world = sim.settings_.world;
    d.worldHash = sim.worldHash_;
    d.animals = sim.animals_->saved();
    d.husbandry = sim.husbandry_->saved();
    d.farming = sim.farming_->saved();
    d.breeding = sim.breeding_->saved();
    d.narrator = sim.narrator_->state();
    d.disasters = sim.disasters_->state();
    d.fire = sim.fire_->saved();
    d.seconds = sim.time_.seconds;
    d.rngState = sim.rng_.state();
    d.rngIncrement = sim.rng_.increment();
    d.groundHash = sim.groundHash_;
    d.stockpile = sim.stockpileCell_;
    for (const Entity pawn : sim.haulers_) {
        d.haulers.push_back(entt::to_integral(pawn));
    }
    d.hauls = sim.stats_.hauls;
    d.failedJobs = sim.stats_.failedJobs;
    d.repaths = sim.stats_.repaths;
    d.setupErrors = sim.setupErrors_;
    d.submitted = sim.submitted_;
    for (const auto& [id, command] : sim.pending_) {
        d.pending.push_back(SavePending{id, command});
    }
    d.commandLog = sim.commandLog_;
    d.checkpoints = sim.checkpoints_;
    d.outfits = sim.apparel_->outfits();
    d.nextOutfitId = sim.apparel_->nextOutfitId();

    const entt::registry& reg = sim.things_->registry();
    const auto* entities = reg.storage<Entity>();
    const size_t live = entities->free_list();
    for (size_t i = live; i < entities->size(); ++i) {
        d.freeIds.push_back(entt::to_integral(entities->data()[i]));
    }
    std::vector<Entity> ids(entities->data(), entities->data() + live);
    std::sort(ids.begin(), ids.end());
    d.entities.reserve(ids.size());
    for (const Entity e : ids) {
        SaveEntity out;
        out.id = entt::to_integral(e);
        if (const Thing* thing = reg.try_get<Thing>(e)) {
            out.thing = SaveThing{sim.defs_->get(thing->def).defName(), thing->category};
        }
        if (const Position* position = reg.try_get<Position>(e)) {
            out.position = *position;
        }
        if (const Footprint* footprint = reg.try_get<Footprint>(e)) {
            out.footprint = *footprint;
        }
        if (const StackCount* stack = reg.try_get<StackCount>(e)) {
            out.stack = *stack;
        }
        if (const HitPoints* hitPoints = reg.try_get<HitPoints>(e)) {
            out.hitPoints = *hitPoints;
        }
        if (const Mover* mover = reg.try_get<Mover>(e)) {
            out.mover = *mover;
        }
        if (const Worker* worker = reg.try_get<Worker>(e)) {
            SaveWorker w;
            w.job = worker->job;
            w.workType = worker->job.workType.valid() ? sim.defs_->get(worker->job.workType).defName() : std::string();
            for (const BillUsed& used : worker->job.used) {
                w.used.push_back(SaveBillUsed{sim.defs_->get(used.def).defName(), used.count});
            }
            w.job.used.clear();
            w.avoid = worker->avoid;
            out.worker = std::move(w);
        }
        if (const Spoilage* spoilage = reg.try_get<Spoilage>(e)) {
            SaveSpoilage saved;
            saved.rot = spoilage->rot;
            saved.decay = spoilage->decay;
            saved.poisoned = spoilage->poisoned;
            for (const DefId ingredient : spoilage->ingredients) {
                saved.ingredients.push_back(sim.defs_->get(ingredient).defName());
            }
            out.spoilage = std::move(saved);
        }
        if (const Joy* joy = reg.try_get<Joy>(e)) {
            SaveJoy saved;
            saved.joy = *joy;
            const Worker* worker = reg.try_get<Worker>(e);
            if (worker != nullptr && worker->job.activity.valid()) {
                saved.activity = sim.defs_->get(worker->job.activity).defName();
            }
            out.joy = std::move(saved);
        }
        if (const WorkSettings* work = reg.try_get<WorkSettings>(e)) {
            SaveWorkSettings w;
            for (const WorkPriority& entry : work->entries) {
                w.entries.push_back(SaveWorkPriority{sim.defs_->get(entry.workType).defName(), entry.priority, entry.disabled});
            }
            out.work = std::move(w);
        }
        if (const Carrying* carrying = reg.try_get<Carrying>(e)) {
            out.carrying = *carrying;
        }
        if (const Needs* needs = reg.try_get<Needs>(e)) {
            out.needs = *needs;
        }
        if (const Door* door = reg.try_get<Door>(e)) {
            out.door = *door;
        }
        if (const PowerPart* part = reg.try_get<PowerPart>(e)) {
            out.power = *part;
            out.power->net = 0;  // worked out again on loading
            out.power->watts = 0.0;
        }
        if (const Mood* mood = reg.try_get<Mood>(e)) {
            out.mood = *mood;
        }
        if (const Schedule* schedule = reg.try_get<Schedule>(e)) {
            out.schedule = *schedule;
        }
        if (const Bed* bed = reg.try_get<Bed>(e)) {
            out.bed = *bed;
        }
        if (const Colonist* colonist = reg.try_get<Colonist>(e)) {
            out.colonist = *colonist;
        }
        if (const PawnLooks* looks = reg.try_get<PawnLooks>(e)) {
            out.looks = *looks;
        }
        if (const MentalBreaker* breaker = reg.try_get<MentalBreaker>(e)) {
            out.mentalBreaker = *breaker;
        }
        if (const Visitor* visitor = reg.try_get<Visitor>(e)) {
            out.visitor = *visitor;
        }
        if (const Inspired* inspired = reg.try_get<Inspired>(e)) {
            out.inspired = *inspired;
        }
        out.playerControlled = reg.all_of<PlayerControl>(e);
        if (const ConstructionSite* site = reg.try_get<ConstructionSite>(e)) {
            SaveSite s;
            s.stage = site->stage;
            s.stuff = site->stuff.valid() ? sim.defs_->get(site->stuff).defName() : std::string();
            s.origin = site->origin;
            s.rotation = site->rotation;
            s.sizeX = site->sizeX;
            s.sizeY = site->sizeY;
            for (const MaterialCount& m : site->materials) {
                s.materials.push_back(SaveMaterial{sim.defs_->get(m.item).defName(), m.needed, m.have});
            }
            s.workTotal = site->workTotal;
            s.workDone = site->workDone;
            out.site = std::move(s);
        }
        if (const BuiltOf* made = reg.try_get<BuiltOf>(e)) {
            out.builtOf = sim.defs_->get(made->stuff).defName();
        }
        if (reg.all_of<MarkedForDeconstruct>(e)) {
            out.marked = SaveMark{};
        }
        {
            const MadeOf* made = reg.try_get<MadeOf>(e);
            const Quality* quality = reg.try_get<Quality>(e);
            const PawnOutfit* outfit = reg.try_get<PawnOutfit>(e);
            if (made != nullptr || quality != nullptr || outfit != nullptr || reg.all_of<Tainted>(e) || reg.all_of<StripMark>(e)) {
                SaveApparel a;
                a.stuff = made != nullptr && made->stuff.valid() ? sim.defs_->get(made->stuff).defName() : std::string();
                a.quality = quality != nullptr ? static_cast<int>(quality->level) : -1;
                a.tainted = reg.all_of<Tainted>(e);
                a.stripMark = reg.all_of<StripMark>(e);
                a.hasOutfit = outfit != nullptr;
                a.outfit = outfit != nullptr ? outfit->outfit : 0;
                a.nextCheck = outfit != nullptr ? outfit->nextCheck : 0;
                out.apparel = std::move(a);
            }
        }
        if (const BillList* list = reg.try_get<BillList>(e)) {
            SaveBillList saved;
            saved.nextId = list->nextId;
            for (const Bill& bill : list->bills) {
                SaveBill b;
                b.id = bill.id;
                b.recipe = sim.defs_->get(bill.recipe).defName();
                b.mode = bill.mode;
                b.count = bill.count;
                b.suspended = bill.suspended;
                b.skillMin = bill.skillMin;
                b.skillMax = bill.skillMax;
                b.radius = bill.radius;
                b.storeZone = bill.storeZone;
                b.unpauseAt = bill.unpauseAt;
                b.satisfied = bill.satisfied;
                for (const DefId def : bill.disallowed) {
                    b.disallowed.push_back(sim.defs_->get(def).defName());
                }
                saved.bills.push_back(std::move(b));
            }
            out.bills = std::move(saved);
        }
        if (const Fuel* fuel = reg.try_get<Fuel>(e)) {
            out.fuel = *fuel;
        }
        if (const StoreIn* store = reg.try_get<StoreIn>(e)) {
            out.storeIn = *store;
        }
        if (const Health* health = reg.try_get<Health>(e)) {
            SaveHealth saved;
            saved.protagonist = health->protagonist;
            saved.rest = health->rest;
            for (const Injury& injury : health->injuries) {
                saved.injuries.push_back(SaveInjury{injury.part, sim.defs_->get(injury.hediff).defName(), injury.severity,
                                                    injury.tendQuality, injury.peak, injury.infectAt});
            }
            for (const Disease& disease : health->diseases) {
                saved.diseases.push_back(SaveDisease{sim.defs_->get(disease.def).defName(), disease.severity, disease.tendQuality,
                                                     disease.tendedUntil, disease.tendTotal, disease.part});
            }
            for (const Immunity& immunity : health->immunities) {
                saved.immunities.push_back(SaveImmunity{sim.defs_->get(immunity.def).defName(), immunity.level});
            }
            saved.missing = health->missing;
            for (const AddedPart& added : health->added) {
                saved.added.push_back(SaveAddedPart{added.part, sim.defs_->get(added.hediff).defName()});
            }
            saved.bloodLoss = health->bloodLoss;
            for (const EnvironmentHediff& env : health->environment) {
                saved.environment.push_back(SaveEnvironmentHediff{sim.defs_->get(env.def).defName(), env.severity});
            }
            saved.dead = health->dead;
            saved.cause = health->cause;
            saved.downed = health->downed;
            saved.dirty = health->dirty;
            out.health = std::move(saved);
        }
        if (const Posture* posture = reg.try_get<Posture>(e)) {
            out.posture = *posture;
        }
        if (const Gear* gear = reg.try_get<Gear>(e)) {
            out.gear = *gear;
        }
        if (const EquippedBy* by = reg.try_get<EquippedBy>(e)) {
            out.equippedBy = *by;
        }
        if (const Shooter* shooter = reg.try_get<Shooter>(e)) {
            out.shooter = *shooter;
        }
        if (const Faction* faction = reg.try_get<Faction>(e)) {
            out.faction = *faction;
        }
        if (const Fighter* fighter = reg.try_get<Fighter>(e)) {
            out.fighter = *fighter;
        }
        if (const Magazine* magazine = reg.try_get<Magazine>(e)) {
            out.magazine = *magazine;
        }
        if (const DefensePlan* plan = reg.try_get<DefensePlan>(e)) {
            out.defense = *plan;
        }
        if (const Raider* raider = reg.try_get<Raider>(e)) {
            out.raider = *raider;
        }
        if (const Kidnapped* kidnapped = reg.try_get<Kidnapped>(e)) {
            out.kidnapped = *kidnapped;
        }
        if (const RecoilKick* kick = reg.try_get<RecoilKick>(e)) {
            out.kick = *kick;
        }
        if (const DodgeRoll* roll = reg.try_get<DodgeRoll>(e)) {
            out.roll = *roll;
        }
        if (const Gear* gear = reg.try_get<Gear>(e); gear != nullptr && !gear->stowed.empty()) {
            out.stowed = gear->stowed;
        }
        if (const WeaponSwitch* change = reg.try_get<WeaponSwitch>(e)) {
            out.weaponSwitch = *change;
        }
        if (const Animal* animal = reg.try_get<Animal>(e)) {
            out.animal = *animal;
        }
        if (const Prisoner* prisoner = reg.try_get<Prisoner>(e)) {
            out.prisoner = *prisoner;
        }
        if (const PrisonBed* prisonBed = reg.try_get<PrisonBed>(e)) {
            out.prisonBed = *prisonBed;
        }
        if (const Operations* operations = reg.try_get<Operations>(e)) {
            SaveOperations saved;
            saved.nextId = operations->nextId;
            for (const Operation& op : operations->list) {
                saved.list.push_back(SaveOperation{op.id, sim.defs_->get(op.recipe).defName(), op.part});
            }
            out.operations = std::move(saved);
        }
        if (const MedicalSettings* medical = reg.try_get<MedicalSettings>(e)) {
            out.medical = *medical;
        }
        if (const AnimalCare* care = reg.try_get<AnimalCare>(e)) {
            out.care = *care;
        }
        if (const AnimalBreed* breed = reg.try_get<AnimalBreed>(e)) {
            out.breed = *breed;
        }
        if (const Hatching* hatching = reg.try_get<Hatching>(e)) {
            out.hatching = *hatching;
        }
        if (const Young* young = reg.try_get<Young>(e)) {
            out.young = *young;
        }
        if (const Burning* burning = reg.try_get<Burning>(e)) {
            out.burning = *burning;
        }
        if (const Turret* turret = reg.try_get<Turret>(e)) {
            out.turret = *turret;
        }
        if (const Trap* trap = reg.try_get<Trap>(e)) {
            out.trap = *trap;
        }
        if (const Melee* melee = reg.try_get<Melee>(e)) {
            out.melee = *melee;
        }
        if (const Social* social = reg.try_get<Social>(e)) {
            out.social = *social;
        }
        if (const SocialFight* fight = reg.try_get<SocialFight>(e)) {
            out.socialFight = *fight;
        }
        if (const Projectile* p = reg.try_get<Projectile>(e)) {
            out.projectile = SaveProjectile{sim.defs_->get(p->def).defName(), sim.defs_->get(p->weapon).defName(), *p};
        }
        if (const Grenade* g = reg.try_get<Grenade>(e)) {
            out.grenade = SaveGrenade{sim.defs_->get(g->def).defName(), g->item.valid() ? sim.defs_->get(g->item).defName() : std::string(), *g};
        }
        if (const Belt* belt = reg.try_get<Belt>(e)) {
            SaveBelt saved;
            for (const BeltStack& s : belt->stacks) {
                saved.stacks.push_back(SaveBeltStack{sim.defs_->get(s.item).defName(), s.count});
            }
            out.belt = std::move(saved);
        }
        if (const Thrower* t = reg.try_get<Thrower>(e)) {
            out.thrower = SaveThrower{t->item.valid() ? sim.defs_->get(t->item).defName() : std::string(), *t};
        }
        if (const KnockedDown* k = reg.try_get<KnockedDown>(e)) {
            out.knocked = *k;
        }
        d.entities.push_back(std::move(out));
    }
    d.paths = sim.pathfinder_.saveState();
    d.nextZoneId = sim.zones_->nextId();
    for (const StockpileZone& zone : sim.zones_->stockpiles()) {
        SaveZone z;
        z.id = zone.id;
        z.priority = zone.priority;
        for (const DefId def : zone.disallowed) {
            z.disallowed.push_back(sim.defs_->get(def).defName());
        }
        z.cells = zone.cells;
        d.zones.push_back(std::move(z));
    }
    for (const auto& [pawn, s] : sim.drugs_->states()) {
        SaveDrugPawn saved;
        saved.pawn = pawn;
        saved.policy = s.policy;
        saved.searchAfter = s.searchAfter;
        for (const DrugPolicyEntry& e : s.edits) {
            saved.edits.push_back(SaveDrugEntry{e.drug, e.forAddiction, e.forJoy, e.scheduled, e.daysFrequency, e.moodBelow, e.joyBelow});
        }
        for (const ChemicalState& c : s.chemicals) {
            saved.chemicals.push_back(SaveDrugChemical{c.chemical, c.tolerance, c.addicted, c.need, c.progress, c.armed});
        }
        for (const DrugDose& dose : s.last) {
            saved.last.push_back(SaveDrugDose{dose.drug, dose.at});
        }
        d.drugPawns.push_back(std::move(saved));
    }
    std::vector<uint32_t> defIndexOf(sim.defs_->size(), UINT32_MAX);
    for (const PlantRecord& r : sim.plants_->records()) {
        if (defIndexOf[r.def.value] == UINT32_MAX) {
            defIndexOf[r.def.value] = static_cast<uint32_t>(d.plants.defs.size());
            d.plants.defs.push_back(sim.defs_->get(r.def).defName());
        }
        d.plants.cells.push_back(static_cast<uint32_t>(r.cell));
        d.plants.defIndex.push_back(defIndexOf[r.def.value]);
        d.plants.growth.push_back(r.growth);
        d.plants.ageHours.push_back(r.ageHours);
    }
    std::vector<uint32_t> rockIndexOf(sim.defs_->size(), UINT32_MAX);
    for (const MineableRecord& r : sim.mineables_->records()) {
        if (rockIndexOf[r.def.value] == UINT32_MAX) {
            rockIndexOf[r.def.value] = static_cast<uint32_t>(d.mineables.defs.size());
            d.mineables.defs.push_back(sim.defs_->get(r.def).defName());
        }
        d.mineables.cells.push_back(static_cast<uint32_t>(r.cell));
        d.mineables.defIndex.push_back(rockIndexOf[r.def.value]);
        d.mineables.hitPoints.push_back(r.hitPoints);
    }
    for (const auto& [cell, kind] : sim.designations_->all()) {
        d.designations.cells.push_back(static_cast<uint32_t>(cell));
        d.designations.kinds.push_back(static_cast<uint8_t>(kind));
    }
    d.research = sim.research_->state();
    d.ship = sim.ship_->state();
    for (const auto& [pawn, s] : sim.talents_->states()) {
        d.talentPawns.push_back(pawn);
        d.talentXp.push_back(s.xp);
        d.talentNames.emplace_back();
        d.talentRanks.emplace_back();
        for (const auto& [name, rank] : s.ranks) {
            d.talentNames.back().push_back(name);
            d.talentRanks.back().push_back(rank);
        }
    }
    d.hasRoomStats = true;
    for (const auto& [cell, def] : sim.floors_->all()) {
        d.floors.push_back(SaveFloor{static_cast<uint32_t>(cell), sim.defs_->get(def).defName()});
    }
    d.filth = sim.filth_->state();
    d.roomStats = sim.roomStats_->state();
    d.roomTemperatures = sim.temperature_->roomTemperatures();
    d.upperFloors = sim.map_.upperFloorCells();
    d.roofs = sim.roofCollapse_->state();
    d.raids = sim.raids_->state();
    d.quests = sim.quests_->state();
    d.spoilageState = sim.spoilage_->state();
    d.craftingState = sim.crafting_->state();
    d.trade = sim.trade_->state();
    {
        const WeatherState state = sim.weather_->state();
        const auto nameOf = [&](DefId def) { return def.valid() ? sim.defs_->get(def).defName() : std::string(); };
        d.weather.current = nameOf(state.current);
        d.weather.previous = nameOf(state.previous);
        d.weather.startedAt = state.startedAt;
        d.weather.endsAt = state.endsAt;
        d.weather.rngState = state.rngState;
        d.weather.rngIncrement = state.rngIncrement;
        for (const WeatherSnowRecord& r : sim.weather_->snowRecords()) {
            d.weather.snowCells.push_back(static_cast<uint32_t>(r.cell));
            d.weather.snowDepth.push_back(r.depth);
        }
    }
    return d;
}

bool SimSaveAccess::restore(Sim& sim, SaveData& d, std::string& error) {
    using Traits = entt::entt_traits<Entity>;
    Things& things = *sim.things_;
    entt::registry& reg = things.registry();

    // The ids: every slot from 0 up is either a live thing or a free id.
    const size_t slots = d.entities.size() + d.freeIds.size();
    std::vector<uint32_t> idOfSlot(slots, 0);
    std::vector<uint8_t> slotUsed(slots, 0);
    std::vector<uint8_t> slotFree(slots, 0);
    const auto claim = [&](uint32_t id, bool isFree) {
        const size_t slot = static_cast<size_t>(Traits::to_entity(static_cast<Entity>(id)));
        if (slot >= slots || slotUsed[slot] != 0) {
            return false;
        }
        slotUsed[slot] = 1;
        slotFree[slot] = isFree ? 1 : 0;
        idOfSlot[slot] = id;
        return true;
    };
    for (const SaveEntity& e : d.entities) {
        if (!claim(e.id, false)) {
            error = "the save file's thing ids don't add up";
            return false;
        }
    }
    for (const uint32_t id : d.freeIds) {
        if (!claim(id, true)) {
            error = "the save file's thing ids don't add up";
            return false;
        }
    }
    // Free ids are made one version back and destroyed again, which brings them to the saved
    // version, and destroying them last-first puts them back in the saved reuse order.
    for (size_t slot = 0; slot < slots; ++slot) {
        Entity hint = static_cast<Entity>(idOfSlot[slot]);
        if (slotFree[slot] != 0) {
            hint = Traits::construct(Traits::to_entity(hint), static_cast<Traits::version_type>(Traits::to_version(hint) - 1));
        }
        if (reg.create(hint) != hint) {
            error = "could not restore the thing ids";
            return false;
        }
    }
    for (size_t i = d.freeIds.size(); i-- > 0;) {
        const Entity saved = static_cast<Entity>(d.freeIds[i]);
        reg.destroy(Traits::construct(Traits::to_entity(saved), static_cast<Traits::version_type>(Traits::to_version(saved) - 1)));
    }
    const auto* entities = std::as_const(reg).storage<Entity>();
    bool idsRestored = entities->free_list() == d.entities.size() && entities->size() == slots;
    for (size_t i = 0; idsRestored && i < d.freeIds.size(); ++i) {
        idsRestored = entt::to_integral(entities->data()[d.entities.size() + i]) == d.freeIds[i];
    }
    if (!idsRestored) {
        error = "could not restore the thing ids";
        return false;
    }

    // Components. A thing's def is looked up by name; the rest is copied back.
    for (SaveEntity& e : d.entities) {
        const Entity entity = static_cast<Entity>(e.id);
        if (e.thing) {
            const Def* def = sim.defs_->find(kSaveThingsKind, e.thing->def);
            if (def == nullptr) {
                error = "the save has a thing made from '" + e.thing->def + "', which the data files no longer have";
                return false;
            }
            reg.emplace<Thing>(entity, Thing{def->id(), e.thing->category});
        }
        if (e.position) {
            if (!sim.map_.inBounds(cellOf(*e.position))) {
                error = "the save has a thing outside the map";
                return false;
            }
            reg.emplace<Position>(entity, *e.position);
        }
        if (e.footprint) {
            reg.emplace<Footprint>(entity, *e.footprint);
        }
        if (e.stack) {
            reg.emplace<StackCount>(entity, *e.stack);
        }
        if (e.hitPoints) {
            reg.emplace<HitPoints>(entity, *e.hitPoints);
        }
        if (e.mover) {
            reg.emplace<Mover>(entity, std::move(*e.mover));
        }
        if (e.worker) {
            Worker worker;
            worker.job = e.worker->job;
            worker.job.workType = DefId{};
            worker.job.activity = DefId{};  // from the recreation section below
            if (!e.worker->workType.empty()) {
                // A work type the data files dropped leaves the job without one; it runs on.
                worker.job.workType = sim.defs_->idOf(kSaveWorkTypesKind, e.worker->workType);
            }
            for (const SaveBillUsed& used : e.worker->used) {
                const DefId def = sim.defs_->idOf(kSaveThingsKind, used.def);
                if (!def.valid()) {
                    error = "the save has a bill job with '" + used.def + "', which the data files no longer have";
                    return false;
                }
                worker.job.used.push_back(BillUsed{def, used.count});
            }
            worker.avoid = std::move(e.worker->avoid);
            reg.emplace<Worker>(entity, std::move(worker));
        }
        if (e.work) {
            // Matched to today's work types by name: one the save doesn't know gets the default
            // priority, one the data files dropped is forgotten.
            WorkSettings work = sim.jobs_->defaultWorkSettings();
            for (WorkPriority& entry : work.entries) {
                for (const SaveWorkPriority& saved : e.work->entries) {
                    if (saved.workType == sim.defs_->get(entry.workType).defName()) {
                        entry.priority = saved.priority;
                        entry.disabled = saved.disabled;
                    }
                }
            }
            reg.emplace<WorkSettings>(entity, std::move(work));
        }
        if (e.carrying) {
            reg.emplace<Carrying>(entity, *e.carrying);
        }
        if (e.needs) {
            reg.emplace<Needs>(entity, *e.needs);
        }
        if (e.mood) {
            reg.emplace<Mood>(entity, *e.mood);
        }
        if (e.footprint && e.footprint->isDoor) {
            reg.emplace<Door>(entity, e.door.value_or(Door{}));  // saves before format 12: closed
        }
        if (e.power) {
            reg.emplace<PowerPart>(entity, *e.power);
        } else if (e.footprint && e.thing && sim.defs_->get(reg.get<Thing>(entity).def).has("power")) {
            reg.emplace<PowerPart>(entity);  // saves from before the section: switched on, unpowered until the first step
        }
        if (e.schedule) {
            reg.emplace<Schedule>(entity, *e.schedule);
        }
        if (e.bed) {
            reg.emplace<Bed>(entity, *e.bed);
        }
        if (e.colonist) {
            reg.emplace<Colonist>(entity, std::move(*e.colonist));
        }
        if (e.site) {
            ConstructionSite site;
            site.stage = e.site->stage;
            if (!e.site->stuff.empty()) {
                site.stuff = sim.defs_->idOf(kSaveThingsKind, e.site->stuff);
                if (!site.stuff.valid()) {
                    error = "the save has a building plan made of '" + e.site->stuff + "', which the data files no longer have";
                    return false;
                }
            }
            site.origin = e.site->origin;
            site.rotation = e.site->rotation;
            site.sizeX = e.site->sizeX;
            site.sizeY = e.site->sizeY;
            for (const SaveMaterial& m : e.site->materials) {
                const DefId item = sim.defs_->idOf(kSaveThingsKind, m.item);
                if (!item.valid()) {
                    error = "the save has a building plan that needs '" + m.item + "', which the data files no longer have";
                    return false;
                }
                site.materials.push_back(MaterialCount{item, m.needed, m.have});
            }
            site.workTotal = e.site->workTotal;
            site.workDone = e.site->workDone;
            if (!e.thing || !e.position || e.thing->category != ThingCategory::Building) {
                error = "the save has a building plan that isn't a building on the map";
                return false;
            }
            reg.emplace<ConstructionSite>(entity, std::move(site));
        }
        if (e.builtOf) {
            const DefId stuff = sim.defs_->idOf(kSaveThingsKind, *e.builtOf);
            if (!stuff.valid()) {
                error = "the save has a building made of '" + *e.builtOf + "', which the data files no longer have";
                return false;
            }
            reg.emplace<BuiltOf>(entity, BuiltOf{stuff});
        }
        if (e.marked) {
            reg.emplace<MarkedForDeconstruct>(entity);
        }
        if (e.apparel) {
            const SaveApparel& a = *e.apparel;
            if (!a.stuff.empty()) {
                const DefId stuff = sim.defs_->idOf(kSaveThingsKind, a.stuff);
                if (!stuff.valid()) {
                    error = "the save has apparel made of '" + a.stuff + "', which the data files no longer have";
                    return false;
                }
                reg.emplace<MadeOf>(entity, MadeOf{stuff});
            }
            if (a.quality >= 0) {
                reg.emplace<Quality>(entity, Quality{static_cast<uint8_t>(a.quality)});
            }
            if (a.tainted) {
                reg.emplace<Tainted>(entity);
            }
            if (a.stripMark) {
                reg.emplace<StripMark>(entity);
            }
            if (a.hasOutfit) {
                reg.emplace<PawnOutfit>(entity, PawnOutfit{a.outfit, a.nextCheck});
            }
        }
        if (e.looks) {
            reg.emplace<PawnLooks>(entity, std::move(*e.looks));
            sim.giveGenes(entity);  // genes follow the lineage and are not saved (docs/design/genes.md)
        }
        if (e.mentalBreaker) {
            reg.emplace<MentalBreaker>(entity, std::move(*e.mentalBreaker));
        }
        if (e.visitor) {
            reg.emplace<Visitor>(entity, *e.visitor);
        }
        if (e.inspired) {
            reg.emplace<Inspired>(entity, std::move(*e.inspired));
        }
        if (e.bills) {
            BillList list;
            list.nextId = e.bills->nextId;
            for (const SaveBill& saved : e.bills->bills) {
                const Def* recipe = sim.defs_->find("Recipes", saved.recipe);
                if (recipe == nullptr) {
                    error = "the save has a bill for '" + saved.recipe + "', which the data files no longer have";
                    return false;
                }
                Bill bill;
                bill.id = saved.id;
                bill.recipe = recipe->id();
                bill.mode = saved.mode;
                bill.count = saved.count;
                bill.suspended = saved.suspended;
                bill.skillMin = saved.skillMin;
                bill.skillMax = saved.skillMax;
                bill.radius = saved.radius;
                bill.storeZone = saved.storeZone;
                bill.unpauseAt = saved.unpauseAt;
                bill.satisfied = saved.satisfied;
                for (const std::string& name : saved.disallowed) {
                    const Def* item = sim.defs_->find("Things", name);
                    if (item == nullptr) {
                        error = "the save has a bill that refuses '" + name + "', which the data files no longer have";
                        return false;
                    }
                    bill.disallowed.push_back(item->id());
                }
                std::sort(bill.disallowed.begin(), bill.disallowed.end(), [](DefId a, DefId b) { return a.value < b.value; });
                list.bills.push_back(std::move(bill));
            }
            reg.emplace<BillList>(entity, std::move(list));
        }
        if (e.storeIn) {
            reg.emplace<StoreIn>(entity, *e.storeIn);
        }
        if (e.fuel) {
            reg.emplace<Fuel>(entity, *e.fuel);
        } else if (e.thing && e.thing->category == ThingCategory::Building && !e.site) {
            // A building saved before fuel existed: it burns fuel now, and starts full (a stove that
            // cooked for free keeps cooking until it first runs low).
            const FuelSpec fuelSpec = readFuelSpec(*sim.defs_, *sim.defs_->find(kSaveThingsKind, e.thing->def));
            if (fuelSpec.present) {
                reg.emplace<Fuel>(entity, Fuel{fuelSpec.capacity});
            }
        }
        if (e.health) {
            Health health;
            health.protagonist = e.health->protagonist;
            health.rest = e.health->rest;
            for (const SaveInjury& injury : e.health->injuries) {
                const Def* hediff = sim.defs_->find("Hediffs", injury.hediff);
                if (hediff == nullptr) {
                    error = "the save has an injury of '" + injury.hediff + "', which the data files no longer have";
                    return false;
                }
                health.injuries.push_back(Injury{injury.part, hediff->id(), injury.severity, injury.tendQuality, injury.peak, injury.infectAt});
            }
            for (const SaveDisease& disease : e.health->diseases) {
                const Def* hediff = sim.defs_->find("Hediffs", disease.hediff);
                if (hediff == nullptr || !sim.health_->isDisease(hediff->id())) {
                    error = "the save has the disease '" + disease.hediff + "', which the data files no longer have";
                    return false;
                }
                health.diseases.push_back(Disease{hediff->id(), disease.severity, disease.tendQuality, disease.tendedUntil, disease.tendTotal, disease.part});
            }
            for (const SaveImmunity& immunity : e.health->immunities) {
                const Def* hediff = sim.defs_->find("Hediffs", immunity.hediff);
                if (hediff == nullptr || !sim.health_->isDisease(hediff->id())) {
                    error = "the save has an immunity to '" + immunity.hediff + "', which the data files no longer have";
                    return false;
                }
                health.immunities.push_back(Immunity{hediff->id(), immunity.level});
            }
            std::sort(health.diseases.begin(), health.diseases.end(), [](const Disease& a, const Disease& b) {
                return a.def.value != b.def.value ? a.def.value < b.def.value : a.part < b.part;
            });
            std::sort(health.immunities.begin(), health.immunities.end(),
                      [](const Immunity& a, const Immunity& b) { return a.def.value < b.def.value; });
            health.missing = e.health->missing;
            for (const SaveAddedPart& added : e.health->added) {
                const Def* hediff = sim.defs_->find("Hediffs", added.hediff);
                std::string partDef;
                DefId item;
                float efficiency = 1.0f;
                if (hediff == nullptr || !sim.health_->addedInfo(hediff->id(), partDef, item, efficiency)) {
                    error = "the save has the artificial part '" + added.hediff + "', which the data files no longer have";
                    return false;
                }
                health.added.push_back(AddedPart{added.part, hediff->id()});
            }
            health.bloodLoss = e.health->bloodLoss;
            for (const SaveEnvironmentHediff& env : e.health->environment) {
                const Def* hediff = sim.defs_->find("Hediffs", env.hediff);
                if (hediff == nullptr) {
                    error = "the save has the illness '" + env.hediff + "', which the data files no longer have";
                    return false;
                }
                health.environment.push_back(EnvironmentHediff{hediff->id(), env.severity});
            }
            std::sort(health.environment.begin(), health.environment.end(),
                      [](const EnvironmentHediff& a, const EnvironmentHediff& b) { return a.def.value < b.def.value; });
            health.dead = e.health->dead;
            health.cause = e.health->cause;
            health.downed = e.health->downed;  // with `dirty`: a wound not yet worked out stays pending (refreshAll)
            health.dirty = e.health->dirty;
            const int parts = e.thing ? sim.health_->partCount(reg.get<Thing>(entity).def) : 0;
            for (const Injury& injury : health.injuries) {
                if (injury.part >= parts) {
                    error = "the save has an injury on a body part its pawn doesn't have";
                    return false;
                }
            }
            for (const MissingPart& missing : health.missing) {
                if (missing.part >= parts) {
                    error = "the save has a lost body part its pawn doesn't have";
                    return false;
                }
            }
            for (const AddedPart& added : health.added) {
                if (added.part >= parts) {
                    error = "the save has an artificial part on a body part its pawn doesn't have";
                    return false;
                }
            }
            for (const Disease& disease : health.diseases) {
                if (disease.part >= parts) {
                    error = "the save has a disease on a body part its pawn doesn't have";
                    return false;
                }
            }
            reg.emplace<Health>(entity, std::move(health));
        } else if (e.thing && e.thing->category == ThingCategory::Pawn &&
                   sim.defs_->get(reg.get<Thing>(entity).def).has("race.body")) {
            reg.emplace<Health>(entity);  // saved before pawns had health
        }
        if (e.playerControlled) {
            reg.emplace<PlayerControl>(entity);
        }
        if (e.posture) {
            reg.emplace<Posture>(entity, *e.posture);
        } else if (e.thing && e.thing->category == ThingCategory::Pawn) {
            reg.emplace<Posture>(entity);  // saved before pawns had a posture
        }
        if (e.gear) {
            reg.emplace<Gear>(entity, std::move(*e.gear));
        }
        if (e.equippedBy) {
            reg.emplace<EquippedBy>(entity, *e.equippedBy);
        }
        if (e.shooter) {
            reg.emplace<Shooter>(entity, *e.shooter);
        }
        if (e.projectile) {
            Projectile p = e.projectile->p;
            p.def = sim.defs_->idOf("Projectiles", e.projectile->def);
            p.weapon = sim.defs_->idOf(kSaveThingsKind, e.projectile->weapon);
            if (!p.def.valid() || !p.weapon.valid()) {
                error = "the save has a projectile of '" + e.projectile->def + "', which the data files no longer have";
                return false;
            }
            reg.emplace<Projectile>(entity, p);
        }
        if (e.grenade) {
            Grenade g = e.grenade->g;
            g.def = sim.defs_->idOf("Projectiles", e.grenade->def);
            g.item = e.grenade->item.empty() ? DefId{} : sim.defs_->idOf(kSaveThingsKind, e.grenade->item);
            if (!g.def.valid() || (!e.grenade->item.empty() && !g.item.valid())) {
                error = "the save has a grenade of '" + e.grenade->def + "', which the data files no longer have";
                return false;
            }
            reg.emplace<Grenade>(entity, g);
        }
        if (e.belt) {
            Belt belt;
            for (const SaveBeltStack& s : e.belt->stacks) {
                const DefId item = sim.defs_->idOf(kSaveThingsKind, s.item);
                if (!item.valid()) {
                    error = "the save has a grenade '" + s.item + "' on a belt, which the data files no longer have";
                    return false;
                }
                belt.stacks.push_back(BeltStack{item, s.count});
            }
            reg.emplace<Belt>(entity, std::move(belt));
        }
        if (e.thrower) {
            Thrower t = e.thrower->t;
            t.item = e.thrower->item.empty() ? DefId{} : sim.defs_->idOf(kSaveThingsKind, e.thrower->item);
            if (!e.thrower->item.empty() && !t.item.valid()) {
                error = "the save has a throw of '" + e.thrower->item + "', which the data files no longer have";
                return false;
            }
            reg.emplace<Thrower>(entity, t);
        }
        if (e.knocked) {
            reg.emplace<KnockedDown>(entity, *e.knocked);
        }
        if (e.faction) {
            reg.emplace<Faction>(entity, *e.faction);
        }
        if (e.fighter) {
            reg.emplace<Fighter>(entity, *e.fighter);
        }
        if (e.magazine) {
            reg.emplace<Magazine>(entity, *e.magazine);
        }
        if (e.kick) {
            reg.emplace<RecoilKick>(entity, *e.kick);
        }
        if (e.roll) {
            reg.emplace<DodgeRoll>(entity, *e.roll);
        }
        if (e.stowed) {
            if (!reg.all_of<Gear>(entity)) {
                error = "the save has carried weapons on a pawn without gear";
                return false;
            }
            reg.get<Gear>(entity).stowed = *e.stowed;
        }
        if (e.weaponSwitch) {
            reg.emplace<WeaponSwitch>(entity, *e.weaponSwitch);
        }
        if (e.joy) {
            reg.emplace<Joy>(entity, e.joy->joy);
            if (!e.joy->activity.empty()) {
                // An activity the data files dropped leaves the job without one; it ends at once.
                if (Worker* worker = reg.try_get<Worker>(entity)) {
                    worker->job.activity = sim.defs_->idOf("RecreationActivities", e.joy->activity);
                }
            }
        }
        if (e.animal) {
            reg.emplace<Animal>(entity, *e.animal);
        }
        if (e.operations) {
            Operations operations;
            operations.nextId = e.operations->nextId;
            const int parts = e.thing ? sim.health_->partCount(reg.get<Thing>(entity).def) : 0;
            for (const SaveOperation& saved : e.operations->list) {
                const SurgeryRecipe* recipe = sim.surgery_->recipe(saved.recipe);
                if (recipe == nullptr) {
                    error = "the save has an operation '" + saved.recipe + "', which the data files no longer have";
                    return false;
                }
                if (saved.part >= parts) {
                    error = "the save has an operation on a body part its patient doesn't have";
                    return false;
                }
                operations.list.push_back(Operation{saved.id, recipe->def, saved.part});
            }
            reg.emplace<Operations>(entity, std::move(operations));
        }
        if (e.medical) {
            reg.emplace<MedicalSettings>(entity, *e.medical);
        }
        if (e.prisoner) {
            reg.emplace<Prisoner>(entity, *e.prisoner);
        }
        if (e.prisonBed) {
            reg.emplace<PrisonBed>(entity, *e.prisonBed);
        }
        if (e.defense) {
            reg.emplace<DefensePlan>(entity, *e.defense);
        }
        if (e.kidnapped) {
            reg.emplace<Kidnapped>(entity, *e.kidnapped);
        }
        if (e.raider) {
            reg.emplace<Raider>(entity, *e.raider);
        }
        if (e.care) {
            reg.emplace<AnimalCare>(entity, *e.care);
        }
        if (e.breed) {
            reg.emplace<AnimalBreed>(entity, *e.breed);
        }
        if (e.hatching) {
            reg.emplace<Hatching>(entity, *e.hatching);
        }
        if (e.young) {
            reg.emplace<Young>(entity, *e.young);
        }
        if (e.burning) {
            reg.emplace<Burning>(entity, *e.burning);
        }
        if (e.spoilage) {
            Spoilage spoilage;
            spoilage.rot = e.spoilage->rot;
            spoilage.decay = e.spoilage->decay;
            spoilage.poisoned = e.spoilage->poisoned;
            for (const std::string& name : e.spoilage->ingredients) {
                const DefId ingredient = sim.defs_->idOf(kSaveThingsKind, name);
                if (ingredient.valid()) {  // an ingredient the data files dropped is forgotten
                    spoilage.ingredients.push_back(ingredient);
                }
            }
            std::sort(spoilage.ingredients.begin(), spoilage.ingredients.end(),
                      [](DefId a, DefId b) { return a.value < b.value; });
            reg.emplace_or_replace<Spoilage>(entity, std::move(spoilage));
        } else if (e.thing && e.thing->category == ThingCategory::Item && thingHasSpoilage(sim.defs_->get(reg.get<Thing>(entity).def))) {
            reg.emplace<Spoilage>(entity);  // saved before items could spoil
        }
        if (e.melee) {
            reg.emplace<Melee>(entity, *e.melee);
        }
        if (e.social) {
            reg.emplace<Social>(entity, *e.social);
        }
        if (e.socialFight) {
            reg.emplace<SocialFight>(entity, *e.socialFight);
        }
        if (e.turret) {
            reg.emplace<Turret>(entity, *e.turret);
        } else if (e.footprint && e.thing && sim.defs_->get(reg.get<Thing>(entity).def).has("turret")) {
            reg.emplace<Turret>(entity);  // saved before turrets existed
        }
        if (e.trap) {
            reg.emplace<Trap>(entity, *e.trap);
        } else if (e.footprint && e.thing && sim.defs_->get(reg.get<Thing>(entity).def).has("trap")) {
            reg.emplace<Trap>(entity);
        }
    }
    sim.animals_->restore(d.animals);
    if (!sim.husbandry_->restore(d.husbandry, error)) {
        return false;
    }
    sim.breeding_->restore(d.breeding);
    if (d.hasNarrator && !sim.narrator_->restore(d.narrator, error)) {
        return false;
    }
    if (d.hasDisasters) {
        sim.disasters_->restore(d.disasters);
    }
    if (!sim.farming_->restore(d.farming, error)) {
        return false;
    }
    sim.health_->refreshAll();
    // Upper floors before the buildings that stand on them.
    for (const CellPos& c : d.upperFloors) {
        if (!sim.map_.setUpperFloor(c, true)) {
            error = "the save has an upper floor outside the map";
            return false;
        }
    }
    for (const SaveEntity& e : d.entities) {
        if (e.position && !things.relink(static_cast<Entity>(e.id))) {
            error = "the save has a building that no longer fits on the map";
            return false;
        }
    }

    // Everything a thing names must exist.
    const auto exists = [&](Entity e) { return e == kNoEntity || reg.valid(e); };
    for (const SaveEntity& e : d.entities) {
        bool gearExists = true;
        if (e.gear) {
            const Gear& gear = reg.get<Gear>(static_cast<Entity>(e.id));
            gearExists = exists(gear.weapon);
            for (const Entity a : gear.apparel) {
                gearExists = gearExists && a != kNoEntity && reg.valid(a);
            }
            for (const Entity a : gear.stowed) {
                gearExists = gearExists && a != kNoEntity && reg.valid(a);
            }
        }
        if ((e.worker && (!exists(e.worker->job.thing) || !exists(e.worker->job.chair) ||
                          !exists(e.worker->job.site))) ||
            (e.carrying && !exists(e.carrying->thing)) || !gearExists || (e.equippedBy && !exists(e.equippedBy->pawn))) {
            error = "the save has a thing that names another thing that doesn't exist";
            return false;
        }
        if (e.bed) {
            for (const Entity owner : e.bed->owners) {
                if (!reg.valid(owner)) {
                    error = "the save has a bed owned by a pawn that doesn't exist";
                    return false;
                }
            }
        }
    }
    // At most one controlled pawn, and it must be one a command could have taken over.
    int controlled = 0;
    for (const SaveEntity& e : d.entities) {
        if (e.playerControlled) {
            ++controlled;
            if (!e.position || !e.mover || !e.colonist || !e.worker) {
                error = "the save has a controlled pawn that isn't a colonist on the map";
                return false;
            }
        }
    }
    if (controlled > 1) {
        error = "the save has more than one controlled pawn";
        return false;
    }
    sim.haulers_.clear();
    for (const uint32_t id : d.haulers) {
        const Entity pawn = static_cast<Entity>(id);
        if (!reg.valid(pawn) || !reg.all_of<Position, Mover, Worker>(pawn)) {
            error = "the save lists a hauler that isn't a pawn who works";
            return false;
        }
        sim.haulers_.push_back(pawn);
    }
    // Zones, then the path queue once every building is back, then the jobs that wait on it.
    std::vector<StockpileZone> zones;
    for (SaveZone& z : d.zones) {
        StockpileZone zone;
        zone.id = z.id;
        zone.priority = z.priority;
        for (const std::string& name : z.disallowed) {
            const DefId def = sim.defs_->idOf(kSaveThingsKind, name);
            if (def.valid()) {  // a def the data files dropped can't be stored anyway
                zone.disallowed.push_back(def);
            }
        }
        zone.cells = std::move(z.cells);
        zones.push_back(std::move(zone));
    }
    if (!sim.zones_->restore(std::move(zones), d.nextZoneId, error)) {
        return false;
    }
    if (!sim.pathfinder_.loadState(std::move(d.paths), error)) {
        return false;
    }
    if (!sim.construction_->afterLoad(error)) {
        return false;
    }
    if (!sim.jobs_->afterLoad(error)) {
        return false;
    }
    if (!sim.fighters_->afterLoad(error)) {
        return false;
    }
    if (!sim.mentalBreaks_->afterLoad(error)) {
        return false;
    }
    if (!sim.inspirations_->afterLoad(error)) {
        return false;
    }
    if (d.stockpile && !sim.map_.inBounds(*d.stockpile)) {
        error = "the save has a stockpile outside the map";
        return false;
    }
    for (const SavePending& p : d.pending) {
        if (p.id == 0 || p.id > d.submitted) {
            error = "the save has a queued command with an impossible number";
            return false;
        }
    }

    {
        // Older saves have no plants: the map starts bare.
        std::vector<PlantRecord> records;
        for (size_t i = 0; i < d.plants.cells.size(); ++i) {
            if (d.plants.defIndex[i] >= d.plants.defs.size()) {
                error = "the save has a plant of an unknown kind";
                return false;
            }
            const DefId def = sim.defs_->idOf("Things", d.plants.defs[d.plants.defIndex[i]]);
            records.push_back(PlantRecord{static_cast<int>(d.plants.cells[i]), def, d.plants.growth[i], d.plants.ageHours[i]});
        }
        if (!sim.plants_->restore(records)) {
            error = "the save has a plant the data files can't place";
            return false;
        }
    }
    if (d.hasRock) {
        std::vector<MineableRecord> records;
        for (size_t i = 0; i < d.mineables.cells.size(); ++i) {
            if (d.mineables.defIndex[i] >= d.mineables.defs.size() || d.mineables.hitPoints[i] > 65535) {
                error = "the save has rock of an unknown kind";
                return false;
            }
            const DefId def = sim.defs_->idOf("Things", d.mineables.defs[d.mineables.defIndex[i]]);
            records.push_back(MineableRecord{static_cast<int>(d.mineables.cells[i]), def, static_cast<uint16_t>(d.mineables.hitPoints[i])});
        }
        if (!sim.mineables_->restore(records)) {
            error = "the save has rock the data files can't place";
            return false;
        }
        std::vector<std::pair<int, DesignationKind>> marks;
        for (size_t i = 0; i < d.designations.cells.size(); ++i) {
            if (d.designations.kinds[i] > static_cast<uint8_t>(DesignationKind::HarvestPlant)) {
                error = "the save has a bad designation";
                return false;
            }
            marks.emplace_back(static_cast<int>(d.designations.cells[i]), static_cast<DesignationKind>(d.designations.kinds[i]));
        }
        if (!sim.designations_->restore(marks)) {
            error = "the save has a designation off the map";
            return false;
        }
    }
    {
        // A project the data files dropped is forgotten; a save from before research starts with none done.
        ResearchState research = d.research;
        const ResearchSystem& known = *sim.research_;
        research.done.erase(std::remove_if(research.done.begin(), research.done.end(),
                                           [&](const std::string& n) { return known.find(n) == nullptr; }),
                            research.done.end());
        for (auto it = research.progress.begin(); it != research.progress.end();) {
            it = known.find(it->first) == nullptr ? research.progress.erase(it) : std::next(it);
        }
        if (known.find(research.current) == nullptr) {
            research.current.clear();
        }
        sim.research_->state() = std::move(research);
    }
    for (const Squad& squad : d.raids.squads) {
        for (const Entity m : squad.members) {
            if (!reg.valid(m) || !reg.all_of<Raider>(m)) {
                error = "the save has a raid squad with a pawn that isn't there";
                return false;
            }
            const Raider& raider = reg.get<Raider>(m);
            if ((raider.loot != kNoEntity && !reg.valid(raider.loot)) || (raider.goalThing != kNoEntity && !reg.valid(raider.goalThing))) {
                error = "the save has a raider after something that isn't there";
                return false;
            }
        }
    }
    if (!sim.raids_->restore(std::move(d.raids), error)) {
        return false;
    }
    for (const Visit& visit : d.trade.visits) {
        for (const Entity m : visit.members) {
            if (!reg.valid(m) || !reg.all_of<Visitor>(m)) {
                error = "the save has a visit with a pawn that isn't there";
                return false;
            }
        }
        if (visit.trader != kNoEntity && !reg.valid(visit.trader)) {
            error = "the save has a trader that isn't there";
            return false;
        }
    }
    if (!sim.trade_->restore(std::move(d.trade), error)) {
        return false;
    }
    for (Quest& quest : d.quests.quests) {
        quest.people.erase(std::remove_if(quest.people.begin(), quest.people.end(), [&](Entity p) { return !reg.valid(p); }),
                           quest.people.end());
    }
    sim.quests_->state() = std::move(d.quests);
    if (!sim.ship_->restore(std::move(d.ship), error)) {
        return false;
    }
    sim.apparel_->restoreOutfits(std::move(d.outfits), d.nextOutfitId);
    if (d.hasWeather) {
        WeatherState state;
        state.current = d.weather.current.empty() ? DefId{} : sim.defs_->idOf("Weathers", d.weather.current);
        state.previous = d.weather.previous.empty() ? DefId{} : sim.defs_->idOf("Weathers", d.weather.previous);
        state.startedAt = d.weather.startedAt;
        state.endsAt = d.weather.endsAt;
        state.rngState = d.weather.rngState;
        state.rngIncrement = d.weather.rngIncrement;
        std::vector<WeatherSnowRecord> snow;
        for (size_t i = 0; i < d.weather.snowCells.size(); ++i) {
            if (d.weather.snowDepth[i] > 65535) {
                error = "the save has snow that is too deep";
                return false;
            }
            snow.push_back(WeatherSnowRecord{static_cast<int>(d.weather.snowCells[i]), static_cast<uint16_t>(d.weather.snowDepth[i])});
        }
        if ((!d.weather.current.empty() && !state.current.valid()) || (!d.weather.previous.empty() && !state.previous.valid()) ||
            !sim.weather_->restore(state, snow, d.seconds)) {
            error = "the save has weather the data files can't place";
            return false;
        }
    } else {
        // A save from before the weather: it begins clear at the time the game had reached.
        sim.weather_->restore(sim.weather_->state(), {}, d.seconds);
    }
    {
        // Drug records of a pawn that is gone are forgotten; so is a policy, chemical or drug the data files dropped.
        sim.drugs_->states().clear();
        for (const SaveDrugPawn& saved : d.drugPawns) {
            if (!sim.things_->exists(saved.pawn)) {
                continue;
            }
            DrugState state;
            state.policy = sim.drugs_->policyNamed(saved.policy) != nullptr ? saved.policy : std::string();
            state.searchAfter = saved.searchAfter;
            for (const SaveDrugEntry& e : saved.edits) {
                if (sim.drugs_->kindOf(sim.defs_->idOf("Things", e.drug)) != nullptr) {
                    state.edits.push_back(DrugPolicyEntry{e.drug, e.forAddiction, e.forJoy, e.scheduled, e.daysFrequency, e.moodBelow, e.joyBelow});
                }
            }
            for (const SaveDrugChemical& c : saved.chemicals) {
                if (sim.drugs_->chemicalIndex(c.chemical) >= 0) {
                    state.chemicals.push_back(ChemicalState{c.chemical, c.tolerance, c.addicted, c.need, c.progress, c.armed});
                }
            }
            for (const SaveDrugDose& dose : saved.last) {
                if (sim.drugs_->kindOf(sim.defs_->idOf("Things", dose.drug)) != nullptr) {
                    state.last.push_back(DrugDose{dose.drug, dose.at});
                }
            }
            std::sort(state.edits.begin(), state.edits.end(), [](const DrugPolicyEntry& a, const DrugPolicyEntry& b) { return a.drug < b.drug; });
            std::sort(state.chemicals.begin(), state.chemicals.end(), [](const ChemicalState& a, const ChemicalState& b) { return a.chemical < b.chemical; });
            std::sort(state.last.begin(), state.last.end(), [](const DrugDose& a, const DrugDose& b) { return a.drug < b.drug; });
            sim.drugs_->states()[saved.pawn] = std::move(state);
        }
    }
    {
        // Talents of a pawn that is gone, or of a talent the data files dropped, are forgotten.
        sim.talents_->states().clear();
        for (size_t i = 0; i < d.talentPawns.size(); ++i) {
            if (!sim.things_->exists(d.talentPawns[i])) {
                continue;
            }
            TalentState state;
            state.xp = std::max<int64_t>(0, d.talentXp[i]);
            for (size_t k = 0; k < d.talentNames[i].size(); ++k) {
                const TalentDef* talent = sim.talents_->find(d.talentNames[i][k]);
                if (talent != nullptr && d.talentRanks[i][k] > 0) {
                    state.ranks[talent->defName] = std::min(d.talentRanks[i][k], talent->maxRank);
                }
            }
            sim.talents_->states()[d.talentPawns[i]] = std::move(state);
        }
    }
    sim.time_.seconds = d.seconds;
    sim.rng_.restore(d.rngState, d.rngIncrement);
    sim.stockpileCell_ = d.stockpile;
    sim.stats_.hauls = d.hauls;
    sim.stats_.failedJobs = d.failedJobs;
    sim.stats_.repaths = d.repaths;
    sim.setupErrors_ = d.setupErrors;
    sim.submitted_ = d.submitted;
    sim.pending_.clear();
    for (SavePending& p : d.pending) {
        sim.pending_.emplace_back(p.id, std::move(p.command));
    }
    sim.commandLog_ = std::move(d.commandLog);
    sim.checkpoints_ = std::move(d.checkpoints);
    // A door that was open when the game was saved doesn't block sight.
    for (const auto [entity, door, footprint] : reg.view<const Door, const Footprint>().each()) {
        if (door.open) {
            setDoorSight(sim.map_, footprint, true);
        }
    }
    if (d.hasRoomStats) {
        std::vector<std::pair<int, DefId>> floors;
        for (const SaveFloor& f : d.floors) {
            floors.emplace_back(static_cast<int>(f.cell), sim.defs_->idOf("Terrain", f.def));
        }
        if (!sim.floors_->restore(floors)) {
            error = "the save has a floor the data files can't place";
            return false;
        }
        if (!sim.filth_->restore(d.filth, error) || !sim.roomStats_->restore(d.roomStats, error)) {
            return false;
        }
    }
    if (!sim.rooms_->restoreThinRoofs(d.roofs.thin)) {
        error = "the save has a roof outside the map";
        return false;
    }
    sim.rooms_->update();
    sim.power_->markDirty();
    sim.power_->update();
    sim.roomStats_->refresh();
    sim.temperature_->setOutdoor(sim.outdoorTemperature());
    sim.temperature_->restore(d.roomTemperatures);
    sim.spoilage_->restore(d.spoilageState);
    if (!sim.roofCollapse_->restore(d.roofs)) {
        error = "the save has a fallen roof outside the map";
        return false;
    }
    sim.crafting_->restore(d.craftingState);
    if (!sim.fire_->restore(d.fire, error)) {
        return false;
    }
    sim.lights_->update();  // derived from the buildings and roofs, so not saved: the first step must already see it
    sim.events_.clear();
    return true;
}

std::vector<uint8_t> saveSim(const Sim& sim) {
    SaveData data = SimSaveAccess::collect(sim);
    return saveEncode(data);
}

SaveLoadResult loadSim(const std::vector<uint8_t>& bytes, std::shared_ptr<const DefDatabase> defs) {
    SaveLoadResult result;
    SaveData data;
    if (!saveDecode(bytes, data, result.error)) {
        return result;
    }
    const SimSettings& s = data.settings;
    if (s.mapSize <= 0 || s.mapSize > 4096 || s.pawns < 0 || s.items < 0 || s.checkpointEvery < 0 || s.pathBudget <= 0 ||
        data.seconds < 0 || !worldSettingsValid(data.world)) {
        result.error = "the save file has impossible settings";
        return result;
    }
    const uint64_t defsHash = defs ? defs->contentHash() : DefDatabase().contentHash();
    if (data.defsHash != defsHash) {
        result.warnings.push_back("the data files differ from the ones this save was made with");
    }
    if (data.simVersion != kSimVersion) {
        result.warnings.push_back("this save was made by sim " + data.simVersion + ", this is " + kSimVersion);
    }

    std::unique_ptr<Sim> sim = SimSaveAccess::makeEmpty(data, std::move(defs), result.error);
    if (!sim || !SimSaveAccess::restore(*sim, data, result.error)) {
        return result;
    }
    result.sim = std::move(sim);
    return result;
}

std::string saveToJson(const Sim& sim) {
    SaveData data = SimSaveAccess::collect(sim);
    return saveJsonText(data);
}

bool saveFileToJson(const std::vector<uint8_t>& bytes, std::string& json, std::string& error) {
    SaveData data;
    if (!saveDecode(bytes, data, error)) {
        return false;
    }
    json = saveJsonText(data);
    return true;
}

bool readSaveFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot read file";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeSaveFile(const std::string& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = path + ": cannot write file";
        return false;
    }
    return true;
}

}  // namespace colony

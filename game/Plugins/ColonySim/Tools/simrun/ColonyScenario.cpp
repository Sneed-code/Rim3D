#include "ColonyScenario.h"

#include <chrono>

#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Map.h"
#include "ColonySim/Health.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Regions.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Social.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colonyscenario {

namespace {

constexpr int64_t kFieldDay = 13;  // year run: the rice field is laid out on this game day

using namespace colony;

// The room: walls round a 9 by 7 footprint, a door in the south wall, and inside it six beds, a
// table with stools and a stove. All of it is made of steel except the door, table and stools (wood).
constexpr int kRoomWidth = 9;
constexpr int kRoomHeight = 7;
constexpr int kRoomDoorX = 1;  // the door's column in the south wall
constexpr int kBeds = 6;
constexpr int kMarksEach = 12;  // rock cells, trees and berry bushes the script marks
constexpr int kBerryMarks = 24;
constexpr int64_t kStuckSamples = 60;  // one sample a game minute: a game hour without a change is stuck
constexpr int kSearchRadius = 40;
constexpr int kMarkRadius = 120;  // how far from the landing the script looks for rock, trees and bushes

struct Plan {
    DefId def;
    std::string defName;
    std::string stuff;
    CellPos cell;
    Rot4 rot = Rot4::North;
};

struct Stuck {
    uint64_t fingerprint = 0;
    int64_t samples = 0;     // samples in a row with the same fingerprint
    int64_t worst = 0;       // the longest such run
    int64_t idleSamples = 0; // samples in a row without a job
    int64_t worstIdle = 0;
    Entity pawn = kNoEntity;
    JobType worstJob = JobType::None;
    CellPos worstCell;
};

uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ULL + 0x9E3779B97F4A7C15ULL; }

uint64_t floatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

bool isTree(const Sim& sim, DefId def) { return def.valid() && sim.defs().get(def).flag("plant.isTree"); }

// A bush that gives something when picked (not a tree).
bool isBerryBush(const Sim& sim, DefId def) {
    return def.valid() && !isTree(sim, def) && sim.defs().get(def).integer("plant.harvestYield", 0) > 0;
}

// A walkable cell next to `c` that a pawn at `from` can walk to, or false.
bool touchable(Sim& sim, CellPos from, CellPos c) {
    const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
    for (const CellPos& n : around) {
        if (sim.map().walkable(n) && sim.regions().canReach(from, n)) {
            return true;
        }
    }
    return false;
}

float distSq(CellPos a, CellPos b) {
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    return dx * dx + dy * dy;
}

// The room's origin: the spot near the landing with the fewest trees in its footprint (they are cleared
// when the building goes up, and cost the colonists time) where all the ground is fit to build on and
// the cell outside the door can be reached from the landing, or false.
bool findRoomSite(Sim& sim, CellPos landing, CellPos& out) {
    bool found = false;
    float best = 0.0f;
    for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx) {
            const CellPos origin{landing.x + dx, landing.y + dy, 0};
            int trees = 0;
            bool ok = sim.map().walkable(CellPos{origin.x + kRoomDoorX, origin.y - 1, 0}) &&
                      sim.regions().canReach(landing, CellPos{origin.x + kRoomDoorX, origin.y - 1, 0});
            for (int y = origin.y - 1; y <= origin.y + kRoomHeight && ok; ++y) {
                for (int x = origin.x - 1; x <= origin.x + kRoomWidth && ok; ++x) {
                    const CellPos c{x, y, 0};
                    ok = sim.map().groundWalkable(c) && sim.map().buildingAt(c) == kNoEntity;
                    trees += ok && isTree(sim, sim.plants().defAt(c)) ? 1 : 0;
                }
            }
            const float score = distSq(origin, landing) + 400.0f * static_cast<float>(trees);
            if (ok && (!found || score < best)) {
                found = true;
                best = score;
                out = origin;
            }
        }
    }
    return found;
}

// The best 14 x 10 patch of ground for the rice field within 40 cells of the landing: the most fertile soil
// with no rock, water or building in it, outside the room (poor ground grows crops too slowly for a year run).
CellPos findFieldSite(const Sim& sim, CellPos landing, const CellRect& room) {
    CellPos best{landing.x + 10, landing.y, 0};
    float bestScore = -1.0f;
    for (int dy = -40; dy <= 40; dy += 2) {
        for (int dx = -40; dx <= 40; dx += 2) {
            const CellPos origin{landing.x + dx, landing.y + dy, 0};
            float score = 0.0f;
            bool ok = sim.map().inBounds(CellPos{origin.x + 13, origin.y + 9, 0}) && sim.map().inBounds(origin);
            for (int y = 0; y < 10 && ok; ++y) {
                for (int x = 0; x < 14 && ok; ++x) {
                    const CellPos c{origin.x + x, origin.y + y, 0};
                    ok = sim.map().groundWalkable(c) && sim.map().buildingAt(c) == kNoEntity && !room.contains(c) &&
                         sim.map().terrain(c) == Terrain::Soil;
                    score += ok ? sim.plants().fertility(c) : 0.0f;
                }
            }
            if (ok && score > bestScore) {
                bestScore = score;
                best = origin;
            }
        }
    }
    return best;
}

size_t ripeCrops(const Sim& sim) {
    size_t ripe = 0;
    for (const GrowingZone& zone : sim.farming().zones()) {
        for (const CellPos& c : zone.cells) {
            ripe += sim.farming().readyToHarvest(c) ? 1 : 0;
        }
    }
    return ripe;
}

}  // namespace

int run(uint64_t seed, int mapSize, double days, std::shared_ptr<const DefDatabase> defs, bool verbose, bool year) {
    SimSettings settings{seed, mapSize, 0, 0};
    settings.scenario = "Crashlanded";
    settings.wildAnimals = year ? 100 : 0;
    Sim sim(settings, defs);
    if (!sim.setupErrors().empty()) {
        for (const std::string& e : sim.setupErrors()) {
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        return 2;
    }
    if (!sim.stockpileCell()) {
        std::fprintf(stderr, "colony scenario: no landing site\n");
        return 2;
    }
    const CellPos landing = *sim.stockpileCell();
    CellPos origin;
    if (!findRoomSite(sim, landing, origin)) {
        std::fprintf(stderr, "colony scenario: no room for the building near the landing site\n");
        return 2;
    }

    if (verbose) {
        std::fprintf(stderr, "landing %d,%d  room origin %d,%d\n", landing.x, landing.y, origin.x, origin.y);
    }

    // The plans: wall ring with the door in the south wall, then the furniture inside.
    std::vector<Plan> plans;
    const auto plan = [&](const char* def, const char* stuff, int x, int y, Rot4 rot = Rot4::North) {
        plans.push_back(Plan{defs->idOf("Things", def), def, stuff, CellPos{origin.x + x, origin.y + y, 0}, rot});
    };
    for (int x = 0; x < kRoomWidth; ++x) {
        for (const int y : {0, kRoomHeight - 1}) {
            if (y == 0 && x == kRoomDoorX) {
                plan("Door", "WoodLog", x, y);
            } else {
                plan("Wall", "Steel", x, y);
            }
        }
    }
    for (int y = 1; y < kRoomHeight - 1; ++y) {
        plan("Wall", "Steel", 0, y);
        plan("Wall", "Steel", kRoomWidth - 1, y);
    }
    for (int i = 0; i < kBeds; ++i) {
        plan("Bed", "Steel", 1 + i, kRoomHeight - 3);  // 1 x 2: rows kRoomHeight-3 and kRoomHeight-2 hold the beds
    }
    plan("Table1x2", "WoodLog", 3, 2);
    plan("Stool", "WoodLog", 2, 2);
    plan("Stool", "WoodLog", 2, 3);
    plan("Stool", "WoodLog", 4, 3);
    plan("FueledStove", "", 4, 1);
    plan("HorseshoesPin", "WoodLog", 6, 3);  // something to do in the evening hours (C18)
    plan("CraftingSpot", "", 7, 1);          // a club to make from the chopped wood (C14)
    if (year) {
        plan("ButcherTable", "WoodLog", -2, 6);  // outside the west wall, beside the stockpile: hunted animals are cut up here (N2)
    }
    for (const Plan& p : plans) {
        sim.submit(PlaceBlueprintCommand{p.defName, p.stuff, p.cell, p.rot});
    }
    const CellRect roomRect{origin.x, origin.y, kRoomWidth, kRoomHeight, 0};
    if (year) {
        // The year run: a rice field beside the room and a stockpile for its harvest, so the colony can live on.
        sim.submit(CreateStockpileCommand{CellRect{origin.x - 8, origin.y, 6, 6, 0}});
        // Food to last until the first harvest (the script has no hunting yet): 32 stacks of survival meals.
        for (int i = 0; i < 32; ++i) {
            sim.submit(SpawnThingCommand{"MealSurvivalPack", CellPos{origin.x - 8 + i % 6, origin.y + 1 + i / 6, 0}, 20});
        }
    }
    // A wood floor inside the room (C17): dirt tracked in from the bare ground gets dropped on it,
    // and the colonists clean it.
    sim.submit(PlaceFloorCommand{CellRect{origin.x + 1, origin.y + 1, kRoomWidth - 2, kRoomHeight - 2, 0}, "WoodFloor"});

    // The marks: the nearest rock, trees and berry bushes the colonists can walk up to.
    struct Candidate {
        CellPos cell;
        float d = 0.0f;
    };
    std::vector<Candidate> rock;
    std::vector<Candidate> trees;
    std::vector<Candidate> bushes;
    for (int dy = -kMarkRadius; dy <= kMarkRadius; ++dy) {
        for (int dx = -kMarkRadius; dx <= kMarkRadius; ++dx) {
            const CellPos c{landing.x + dx, landing.y + dy, 0};
            if (!sim.map().inBounds(c) || roomRect.contains(c)) {
                continue;
            }
            const DefId plant = sim.plants().defAt(c);
            const bool isRock = sim.map().terrain(c) == Terrain::Rock && sim.mineables().defAt(c).valid();
            const bool mature = plant.valid() && sim.plants().growth(c) >= 0.95f;
            if (isRock) {
                if (touchable(sim, landing, c)) {
                    rock.push_back({c, distSq(c, landing)});
                }
            } else if (mature && isTree(sim, plant)) {
                if (touchable(sim, landing, c)) {
                    trees.push_back({c, distSq(c, landing)});
                }
            } else if (mature && isBerryBush(sim, plant) && touchable(sim, landing, c)) {
                bushes.push_back({c, distSq(c, landing)});
            }
        }
    }
    const auto nearest = [](std::vector<Candidate>& list) {
        std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
            return a.d != b.d ? a.d < b.d : (a.cell.y != b.cell.y ? a.cell.y < b.cell.y : a.cell.x < b.cell.x);
        });
    };
    nearest(rock);
    nearest(trees);
    nearest(bushes);
    if (rock.empty() || trees.empty() || bushes.empty()) {
        std::fprintf(stderr, "colony scenario: nothing to mine (%zu), chop (%zu) or pick (%zu) within %d cells of the landing\n",
                     rock.size(), trees.size(), bushes.size(), kMarkRadius);
        return 2;
    }
    const auto mark = [&](const std::vector<Candidate>& list, size_t limit, DesignationKind kind) {
        for (size_t i = 0; i < list.size() && i < limit; ++i) {
            sim.submit(DesignateCommand{CellRect{list[i].cell.x, list[i].cell.y, 1, 1, 0}, kind});
        }
        return std::min(list.size(), limit);
    };
    const size_t markedRock = mark(rock, kMarksEach, DesignationKind::Mine);
    const size_t markedTrees = mark(trees, kMarksEach, DesignationKind::CutPlant);
    const size_t markedBushes = mark(bushes, kBerryMarks, DesignationKind::HarvestPlant);

    std::vector<Entity> colonists;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        colonists.push_back(pawn);
    }

    if (year) {
        // The player arms the best shot with the landing's rifle and revolver (colonists do not equip loose weapons
        // themselves): it hunts for the colony and defends it, and the rest run for shelter when the alarm sounds.
        std::vector<Entity> byShooting = colonists;
        const auto shooting = [&](Entity pawn) {
            const Colonist* colonist = sim.colonistOf(pawn);
            for (size_t i = 0; colonist != nullptr && i < colonist->skills.size(); ++i) {
                if (colonist->skills[i].def == "Shooting") {
                    return colonist->skills[i].level;
                }
            }
            return 0;
        };
        std::stable_sort(byShooting.begin(), byShooting.end(), [&](Entity a, Entity b) { return shooting(a) > shooting(b); });
        // The best shot carries both (the rifle for range, the revolver when something is close).
        const char* const guns[2] = {"Gun_BoltActionRifle", "Gun_Revolver"};
        for (size_t g = 0; g < 2 && g < byShooting.size(); ++g) {
            const DefId gun = defs->idOf("Things", guns[g]);
            for (const auto [item, thing] : sim.things().registry().view<const Thing>().each()) {
                if (thing.def == gun) {
                    sim.submit(CarryWeaponCommand{byShooting[0], item});
                    break;
                }
            }
        }
    }

    // Skills under level 10 do not decay: their total experience (levels counted in) shows that the
    // work taught something.
    std::vector<std::pair<Entity, size_t>> learners;
    for (const Entity pawn : colonists) {
        const Colonist* colonist = sim.colonistOf(pawn);
        for (size_t i = 0; colonist != nullptr && i < colonist->skills.size(); ++i) {
            if (colonist->skills[i].level < 10) {
                learners.emplace_back(pawn, i);
            }
        }
    }
    const auto skillXpTotal = [&]() {
        double total = 0.0;
        for (const auto& [pawn, index] : learners) {
            const Colonist* colonist = sim.colonistOf(pawn);
            if (colonist == nullptr || index >= colonist->skills.size()) {
                continue;  // died and gone
            }
            const SkillRecord& skill = colonist->skills[index];
            total += static_cast<double>(skill.xp);
            for (int level = 0; level < skill.level; ++level) {
                total += sim.jobs().skillRules().xpToNextLevel[static_cast<size_t>(level)];
            }
        }
        return total;
    };
    const double skillXpStart = skillXpTotal();
    // The run.
    std::map<Entity, Stuck> stuck;
    uint64_t died = 0;
    uint64_t ate = 0;
    uint64_t atTable = 0;
    uint64_t slept = 0;
    uint64_t sleptInBed = 0;
    uint64_t mined = 0;
    uint64_t felled = 0;
    uint64_t picked = 0;
    uint64_t mealsMade = 0;
    uint64_t cleaned = 0;
    uint64_t woodIn = 0;     // wood colonists put into buildings (Refueled events)
    uint64_t outOfFuel = 0;  // times a building ran out
    uint64_t weatherChanges = 0;  // WeatherChanged events (E1): the colony works through rain, fog and cold
    uint64_t commandsFailed = 0;
    uint64_t recreations = 0;  // recreation jobs started
    uint64_t pinGames = 0;     // ... of them at the horseshoes pin
    uint64_t talks = 0;        // interactions between colonists (C20)
    uint64_t fights = 0;       // fights with fists begun
    float minJoy = 1.0f;
    float minComfort = 1.0f;
    float minOutdoors = 1.0f;
    uint64_t mentalBreaks = 0;
    float minFood = 1.0f;
    float minRest = 1.0f;
    float maxMalnutrition = 0.0f;
    float minMood = 100.0f;
    int frostShown = 0;
    bool fieldMade = false;
    bool billAdded = false;
    bool butcherBillAdded = false;
    uint64_t butchered = 0;
    bool clubBillAdded = false;
    uint64_t clubsMade = 0;
    bool bedsAssigned = false;
    int64_t stoveBuiltAt = -1;
    int64_t allBuiltAt = -1;
    int64_t designationsDoneAt = -1;
    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    Snapshot snapshot;

    std::map<std::string, int> animalDeaths;  // deaths of anything that is not a colonist, by cause
    double worstStepMs = 0.0;
    int64_t worstStepAt = 0;
    for (int64_t i = 0; i < steps; ++i) {
        const auto stepStart = std::chrono::steady_clock::now();
        sim.step();
        const double stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stepStart).count();
        if (stepMs > worstStepMs) {
            worstStepMs = stepMs;
            worstStepAt = i;
        }
        for (const Event& e : sim.events()) {
            switch (e.type) {
                case EventType::PawnDied:
                    if (sim.things().registry().valid(e.thing) && sim.things().registry().all_of<Colonist>(e.thing)) {
                        ++died;
                        std::fprintf(stderr, "day %lld: a colonist died: %s\n", static_cast<long long>(sim.time().day()), e.message.c_str());
                    } else {
                        ++animalDeaths[e.message];
                    }
                    break;
                case EventType::PawnAte: ++ate; atTable += e.message == "table" ? 1 : 0; break;
                case EventType::PawnWokeUp: ++slept; sleptInBed += e.message == "bed" ? 1 : 0; break;
                case EventType::Interaction: ++talks; break;
                case EventType::SocialFight: fights += e.message == "started" ? 1 : 0; break;
                case EventType::JobStarted:
                    if (e.job == JobType::Recreate) {
                        ++recreations;
                        pinGames += e.target != kNoEntity ? 1 : 0;
                    }
                    break;
                case EventType::JobEnded:
                    if (e.message.rfind("failed", 0) == 0) {
                        std::fprintf(stderr, "step %lld: %s job failed for a colonist at %d,%d: %s\n", static_cast<long long>(i),
                                     jobTypeName(e.job), e.cell.x, e.cell.y, e.message.c_str());
                    }
                    break;
                case EventType::PawnInjured:
                    if (verbose && sim.things().registry().valid(e.thing) && sim.things().registry().all_of<Colonist>(e.thing)) {
                        std::fprintf(stderr, "day %lld %02d:%02d: colonist %u hurt at %d,%d, part %d, %.1f damage (%s)\n", static_cast<long long>(sim.time().day()),
                                     sim.time().hourOfDay(), static_cast<int>((sim.time().seconds / 60) % 60), static_cast<unsigned>(entt::to_integral(e.thing)), e.cell.x,
                                     e.cell.y, e.part, static_cast<double>(e.amount), e.message.c_str());
                        // What the colonist and the animals round it were doing, to see why nobody fought back.
                        const entt::registry& reg = sim.things().registry();
                        const Fighter* fighter = reg.try_get<Fighter>(e.thing);
                        const Gear* gear = reg.try_get<Gear>(e.thing);
                        std::fprintf(stderr, "    alarm %d, fighter %s%s, weapon %s", sim.raids().state().alarm ? 1 : 0, fighter != nullptr ? "yes" : "no",
                                     fighter != nullptr && fighter->defending ? " (defending)" : "",
                                     gear != nullptr && gear->weapon != kNoEntity ? "yes" : "no");
                        if (fighter != nullptr && gear != nullptr && gear->weapon != kNoEntity) {
                            const Thing& weaponThing = reg.get<Thing>(gear->weapon);
                            std::fprintf(stderr, " [%s mode %d target %u canFire %d falling %d walk %d stowed %zu known %zu]", defs->get(weaponThing.def).defName().c_str(),
                                         static_cast<int>(fighter->mode), static_cast<unsigned>(entt::to_integral(fighter->target)), fighter->canFire ? 1 : 0,
                                         fighter->fallingBack ? 1 : 0, static_cast<int>(fighter->walk), gear->stowed.size(), fighter->known.size());
                        }
                        const Position& at = reg.get<Position>(e.thing);
                        for (const auto [animal, wild, pos] : reg.view<const Animal, const Position>().each()) {
                            const float d = std::hypot(pos.x - at.x, pos.y - at.y);
                            if (wild.temper != AnimalTemper::Calm && d < 8.0f) {
                                std::fprintf(stderr, "; animal %u temper %d at %.1f m", static_cast<unsigned>(entt::to_integral(animal)), static_cast<int>(wild.temper),
                                             static_cast<double>(d));
                            }
                        }
                        std::fprintf(stderr, "\n");
                    }
                    break;
                case EventType::WeatherChanged: ++weatherChanges; break;
                case EventType::PawnMentalBreak:
                    ++mentalBreaks;
                    std::fprintf(stderr, "day %lld: a colonist had a mental break: %s\n", static_cast<long long>(sim.time().day()), e.message.c_str());
                    break;
                case EventType::CellMined: ++mined; break;
                case EventType::PlantRemoved:
                    ++felled;
                    if (e.message == "frost" && frostShown++ < 5) {
                        std::fprintf(stderr, "day %lld %02d:00: frost killed a crop at %d,%d (%.1f C there)\n", static_cast<long long>(sim.time().day()),
                                     sim.time().hourOfDay(), e.cell.x, e.cell.y, static_cast<double>(sim.temperature().temperatureAt(e.cell)));
                    }
                    break;
                case EventType::PlantHarvested: ++picked; break;
                case EventType::PawnAction: cleaned += e.action == PawnAction::Clean ? 1 : 0; break;
                case EventType::RecipeDone:
                    if (defs->get(e.def).defName() == "MakeClub") {
                        clubsMade += static_cast<uint64_t>(std::max(0.0f, e.amount));
                    } else if (defs->get(e.def).defName() == "ButcherCorpse") {
                        ++butchered;
                    } else {
                        mealsMade += static_cast<uint64_t>(std::max(0.0f, e.amount));
                    }
                    break;
                case EventType::Refueled: woodIn += static_cast<uint64_t>(std::max(0.0f, e.amount)); break;
                case EventType::OutOfFuel: ++outOfFuel; break;
                case EventType::CommandFailed:
                    ++commandsFailed;
                    std::fprintf(stderr, "step %lld: command %llu failed: %s\n", static_cast<long long>(i),
                                 static_cast<unsigned long long>(e.command), e.message.c_str());
                    break;
                default: break;
            }
        }

        const int64_t now = sim.time().seconds;
        if (now % 60 == 0) {
            // Stuck: a pawn that has a job (or is lying down) but nothing about it changes for a game hour
            // (its cell, job, job step, work done); and idle time, which is only reported.
            for (const auto [pawn, pos, worker, needs] :
                 sim.things().registry().view<const Position, const Worker, const Needs>().each()) {
                Stuck& s = stuck[pawn];
                s.pawn = pawn;
                const CellPos cell = cellOf(pos);
                const Job& job = worker.job;
                uint64_t fp = mix(0, static_cast<uint64_t>(cell.x) << 20 | static_cast<uint64_t>(cell.y));
                fp = mix(fp, static_cast<uint64_t>(job.type));
                fp = mix(fp, job.step);
                fp = mix(fp, job.walk);
                fp = mix(fp, floatBits(job.progress));
                fp = mix(fp, static_cast<uint64_t>(job.thing));
                if (job.type == JobType::BuildFrame) {
                    // The work done is kept on the frame, not in the job.
                    const ConstructionSite* site = sim.things().registry().valid(job.thing) ? sim.things().registry().try_get<ConstructionSite>(job.thing) : nullptr;
                    fp = mix(fp, site != nullptr ? floatBits(site->workDone) : 0);
                }
                // Lying down and playing (sitting, watching the clouds) are meant to be still.
                const bool working = job.type != JobType::None && job.type != JobType::Wait && !jobIsLyingDown(job) &&
                                     !(job.type == JobType::Recreate && job.step > 0) && !(job.type == JobType::BedRest && job.step > 0);
                if (working && fp == s.fingerprint) {
                    ++s.samples;
                    if (s.samples > s.worst) {
                        s.worst = s.samples;
                        s.worstJob = job.type;
                        s.worstCell = cell;
                    }
                    if (verbose && s.samples == kStuckSamples / 2) {
                        const Mover& m = sim.things().registry().get<Mover>(pawn);
                        std::fprintf(stderr, "t=%lld colonist %u: %s step %d, no change for 30 game minutes at %.2f,%.2f (path %zu/%zu)\n",
                                     static_cast<long long>(now), static_cast<unsigned>(entt::to_integral(pawn)), jobTypeName(job.type),
                                     job.step, pos.x, pos.y, m.next, m.path.size());
                    }
                } else {
                    s.samples = 0;
                }
                s.fingerprint = fp;
                if (job.type == JobType::None || job.type == JobType::Wait) {
                    s.worstIdle = std::max(s.worstIdle, ++s.idleSamples);
                } else {
                    s.idleSamples = 0;
                }
                minFood = std::min(minFood, needs.food);
                minRest = std::min(minRest, needs.rest);
                maxMalnutrition = std::max(maxMalnutrition, needs.malnutrition);
            }
            for (const auto [pawn, joy] : sim.things().registry().view<const Joy>().each()) {
                (void)pawn;
                minJoy = std::min(minJoy, joy.level);
                minComfort = std::min(minComfort, joy.comfort);
                minOutdoors = std::min(minOutdoors, joy.outdoors);
            }
            for (const auto [pawn, mood] : sim.things().registry().view<const Mood>().each()) {
                (void)pawn;
                minMood = std::min(minMood, mood.level);
            }
        }

        if (now % kSecondsPerHour == 0) {
            if (year && !fieldMade && sim.time().day() >= kFieldDay) {
                // Sown at the start, the crops die in the early spring frosts: the field goes in with the warm weather.
                const CellPos site = findFieldSite(sim, landing, roomRect);
                if (verbose) {
                    std::fprintf(stderr, "rice field at %d,%d\n", site.x, site.y);
                }
                sim.submit(CreateGrowingZoneCommand{CellRect{site.x, site.y, 14, 10, 0}, "Plant_Rice"});
                fieldMade = true;
            }
            sim.writeSnapshot(snapshot);
            int built = 0;
            int stoveId = 0;
            Entity stove = kNoEntity;
            Entity craftingSpot = kNoEntity;
            std::vector<Entity> beds;
            for (const ThingView& t : snapshot.things) {
                if (t.category != ThingCategory::Building || t.stage != ConstructionStage::Built) {
                    continue;
                }
                const std::string& name = defs->get(t.def).defName();
                ++built;
                if (name == "FueledStove") {
                    stove = t.id;
                    ++stoveId;
                } else if (name == "CraftingSpot") {
                    craftingSpot = t.id;
                } else if (name == "Bed") {
                    beds.push_back(t.id);
                }
            }
            if (built >= static_cast<int>(plans.size()) && allBuiltAt < 0) {
                allBuiltAt = now;
            }
            if (stoveId > 0 && stoveBuiltAt < 0) {
                stoveBuiltAt = now;
            }
            if (year) {
                for (const ThingView& t : snapshot.things) {
                    if (!butcherBillAdded && t.category == ThingCategory::Building && t.stage == ConstructionStage::Built &&
                        defs->get(t.def).defName() == "ButcherTable") {
                        sim.submit(AddBillCommand{t.id, "ButcherCorpse", BillMode::Forever, 1});
                        // Only corpses within 40 m: with no limit the butcher walks across the map to a wolf's kill.
                        sim.submit(SetBillOptionsCommand{t.id, 1, 0, 20, 40.0f, 0, 0, {}});
                        butcherBillAdded = true;
                    }
                }
                // Hunting: keep a few game animals marked, nearest to the landing first (not the predators or the boars).
                if (now >= kFieldDay * kSecondsPerDay) {
                    int marked = 0;
                    std::vector<std::pair<float, Entity>> prey;
                    const entt::registry& reg = sim.things().registry();
                    for (const auto [animal, wild, thing, pos] : reg.view<const Animal, const Thing, const Position>().each()) {
                        const Health* health = reg.try_get<Health>(animal);
                        const std::string& name = defs->get(thing.def).defName();
                        if (!wild.wild || (health != nullptr && health->dead)) {
                            continue;
                        }
                        const AnimalCare* care = sim.husbandry().care(animal);
                        const float away = std::hypot(pos.x - static_cast<float>(landing.x), pos.y - static_cast<float>(landing.y));
                        if (care != nullptr && care->markHunt) {
                            if (away > 80.0f) {
                                sim.submit(MarkAnimalCommand{animal, "Hunt", false});  // it ran off: the hunter would follow it for good
                            } else {
                                ++marked;
                            }
                        } else if (name == "Deer" || name == "Hare" || name == "Muffalo" || name == "Alpaca") {
                            const float d = std::hypot(pos.x - static_cast<float>(landing.x), pos.y - static_cast<float>(landing.y));
                            if (d < 45.0f) {
                                prey.emplace_back(d, animal);
                            }
                        }
                    }
                    std::sort(prey.begin(), prey.end());
                    for (size_t k = 0; marked < 3 && k < prey.size(); ++k, ++marked) {
                        sim.submit(MarkAnimalCommand{prey[k].second, "Hunt", true});
                    }
                }
            }
            if (stove != kNoEntity && !billAdded) {
                // Cook until there are 8 meals on the map, so the stove keeps the colony fed.
                sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::UntilHave, year ? 30 : 8});
                billAdded = true;
            }
            if (craftingSpot != kNoEntity && !clubBillAdded) {
                // One club, made of chopped wood by whoever has Crafting work.
                sim.submit(AddBillCommand{craftingSpot, "MakeClub", BillMode::Count, 1});
                clubBillAdded = true;
            }
            if (!bedsAssigned && static_cast<int>(beds.size()) >= kBeds && colonists.size() >= static_cast<size_t>(kBeds)) {
                for (int b = 0; b < kBeds; ++b) {
                    sim.submit(AssignBedCommand{beds[static_cast<size_t>(b)], colonists[static_cast<size_t>(b)]});
                }
                bedsAssigned = true;
            }
            if (designationsDoneAt < 0 && sim.designations().count() == 0 && now > 0) {
                designationsDoneAt = now;
            }
            if (verbose) {
                std::fprintf(stderr, "day %lld %02d:00  built %d/%zu  marks %zu  meals made %llu  ate %llu\n",
                             static_cast<long long>(sim.time().day()), sim.time().hourOfDay(), built, plans.size(),
                             sim.designations().count(), static_cast<unsigned long long>(mealsMade),
                             static_cast<unsigned long long>(ate));
            }
            if (verbose && year && now % (6 * kSecondsPerHour) == 0) {
                int rice = 0;
                int meat = 0;
                int meals = 0;
                for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
                    const std::string& name = defs->get(thing.def).defName();
                    rice += name == "RawRice" ? stack.count : 0;
                    meat += name.rfind("Meat", 0) == 0 ? stack.count : 0;
                    meals += name.rfind("Meal", 0) == 0 ? stack.count : 0;
                }
                std::fprintf(stderr, "    food: rice %d, meat %d, meals %d; crops ripe %zu; hunted %llu\n", rice, meat, meals, ripeCrops(sim),
                             static_cast<unsigned long long>(sim.husbandry().stats().hunted));
            }
        }
    }

    // What stands at the end.
    sim.writeSnapshot(snapshot);
    std::map<std::string, int> builtBy;
    int unfinished = 0;
    for (const ThingView& t : snapshot.things) {
        if (t.category != ThingCategory::Building) {
            continue;
        }
        if (t.stage == ConstructionStage::Built) {
            ++builtBy[defs->get(t.def).defName()];
        } else {
            ++unfinished;
        }
    }
    int bedroomsRoofed = 0;
    for (const Room& room : sim.rooms().rooms()) {
        if (room.enclosed() && !room.outdoors() && room.beds == kBeds && room.stoves == 1 && room.tables >= 1) {
            ++bedroomsRoofed;
        }
    }
    int64_t worstStuck = 0;
    int64_t worstIdle = 0;
    const Stuck* worst = nullptr;
    for (const auto& [pawn, s] : stuck) {
        (void)pawn;
        worstIdle = std::max(worstIdle, s.worstIdle);
        if (s.worst > worstStuck) {
            worstStuck = s.worst;
            worst = &s;
        }
    }
    int expectedWalls = 0;
    int expectedDoors = 0;
    for (const Plan& p : plans) {
        expectedWalls += p.defName == "Wall" ? 1 : 0;
        expectedDoors += p.defName == "Door" ? 1 : 0;
    }

    const double skillXpGained = skillXpTotal() - skillXpStart;
    int friendPairs = 0;
    int rivalPairs = 0;
    for (size_t i = 0; i < colonists.size(); ++i) {
        for (size_t j = 0; j < colonists.size(); ++j) {
            if (i < j && sim.social().friends(colonists[i], colonists[j])) {
                ++friendPairs;
            }
            if (i != j && sim.social().rival(colonists[i], colonists[j])) {
                ++rivalPairs;
            }
        }
    }

    const RoomStatValues* roomStat = sim.roomStats().statsAt(CellPos{origin.x + 2, origin.y + 2, 0});
    std::printf("{\"colony\":{\"seed\":%llu,\"size\":%d,\"days\":%.2f,\"colonists\":%zu,\"died\":%llu,"
                "\"minFood\":%.3f,\"maxMalnutrition\":%.3f,\"minRest\":%.3f,\"minMood\":%.1f,"
                "\"stuckMinutesWorst\":%lld,\"idleMinutesWorst\":%lld,"
                "\"minJoy\":%.3f,\"minComfort\":%.3f,\"minOutdoors\":%.3f,\"recreations\":%llu,\"pinGames\":%llu,\"talks\":%llu,\"fights\":%llu,\"friendPairs\":%d,\"rivalPairs\":%d,"
                "\"marked\":{\"rock\":%zu,\"trees\":%zu,\"bushes\":%zu},\"marksLeft\":%zu,\"marksDoneAtSecond\":%lld,"
                "\"mined\":%llu,\"felled\":%llu,\"picked\":%llu,"
                "\"planned\":%zu,\"walls\":%d,\"doors\":%d,\"beds\":%d,\"tables\":%d,\"stools\":%d,\"stoves\":%d,\"unfinished\":%d,"
                "\"allBuiltAtSecond\":%lld,\"stoveBuiltAtSecond\":%lld,\"roomOk\":%s,"
                "\"skillXpGained\":%.0f,\"woodIn\":%llu,\"outOfFuel\":%llu,\"mealsCooked\":%llu,\"clubsMade\":%llu,\"ate\":%llu,\"atTable\":%llu,\"woke\":%llu,\"wokeInBed\":%llu,"
                "\"cleaned\":%llu,\"filthLeft\":%zu,\"roomBeauty\":%.2f,\"roomImpressiveness\":%.1f,"
                "\"weatherChanges\":%llu,\"commandsFailed\":%llu,\"failedJobs\":%llu,\"mentalBreaks\":%llu,\"stateHash\":\"%016llx\"}}\n",
                static_cast<unsigned long long>(seed), mapSize, days, colonists.size(), static_cast<unsigned long long>(died),
                static_cast<double>(minFood), static_cast<double>(maxMalnutrition), static_cast<double>(minRest),
                static_cast<double>(minMood), static_cast<long long>(worstStuck), static_cast<long long>(worstIdle),
                static_cast<double>(minJoy), static_cast<double>(minComfort), static_cast<double>(minOutdoors),
                static_cast<unsigned long long>(recreations), static_cast<unsigned long long>(pinGames),
                static_cast<unsigned long long>(talks), static_cast<unsigned long long>(fights), friendPairs, rivalPairs,
                markedRock, markedTrees, markedBushes, sim.designations().count(),
                static_cast<long long>(designationsDoneAt), static_cast<unsigned long long>(mined),
                static_cast<unsigned long long>(felled), static_cast<unsigned long long>(picked), plans.size(),
                builtBy["Wall"], builtBy["Door"], builtBy["Bed"], builtBy["Table1x2"], builtBy["Stool"], builtBy["FueledStove"], unfinished,
                static_cast<long long>(allBuiltAt), static_cast<long long>(stoveBuiltAt), bedroomsRoofed == 1 ? "true" : "false",
                skillXpGained,
                static_cast<unsigned long long>(woodIn), static_cast<unsigned long long>(outOfFuel),
                static_cast<unsigned long long>(mealsMade), static_cast<unsigned long long>(clubsMade),
                static_cast<unsigned long long>(ate), static_cast<unsigned long long>(atTable), static_cast<unsigned long long>(slept),
                static_cast<unsigned long long>(sleptInBed), static_cast<unsigned long long>(cleaned), sim.filth().totalLayers(),
                roomStat ? static_cast<double>(roomStat->beauty) : 0.0, roomStat ? static_cast<double>(roomStat->impressiveness) : 0.0,
                static_cast<unsigned long long>(weatherChanges), static_cast<unsigned long long>(commandsFailed), static_cast<unsigned long long>(sim.stats().failedJobs),
                static_cast<unsigned long long>(mentalBreaks),
                static_cast<unsigned long long>(sim.stateHash()));

    if (year) {
        std::printf("{\"year\":{\"worstStepMs\":%.2f,\"worstStepAt\":%lld,\"day\":%lld}}\n", worstStepMs, static_cast<long long>(worstStepAt), static_cast<long long>(worstStepAt / kSecondsPerDay));
        const FarmingStats& f = sim.farming().stats();
        double growthSum = 0.0;
        size_t crops = 0;
        size_t mature = 0;
        for (const GrowingZone& zone : sim.farming().zones()) {
            for (const CellPos& c : zone.cells) {
                if (sim.plants().has(c) && sim.plants().defAt(c) == zone.crop) {
                    ++crops;
                    growthSum += static_cast<double>(sim.plants().growth(c));
                    mature += sim.farming().readyToHarvest(c) ? 1 : 0;
                }
            }
        }
        std::printf("{\"crops\":{\"standing\":%zu,\"meanGrowth\":%.3f,\"ready\":%zu}}\n", crops, crops > 0 ? growthSum / static_cast<double>(crops) : 0.0, mature);
        std::printf("{\"farming\":{\"zones\":%zu,\"sown\":%llu,\"harvested\":%llu,\"frostKilled\":%llu,\"heatKilled\":%llu}}\n", sim.farming().zones().size(),
                    static_cast<unsigned long long>(f.sown), static_cast<unsigned long long>(f.harvested), static_cast<unsigned long long>(f.frostKilled), static_cast<unsigned long long>(f.heatKilled));
        std::printf("{\"hunting\":{\"hunted\":%llu,\"butchered\":%llu}}\n", static_cast<unsigned long long>(sim.husbandry().stats().hunted),
                    static_cast<unsigned long long>(butchered));
        for (const auto& [cause, n] : animalDeaths) {
            std::printf("{\"otherDeaths\":{\"cause\":\"%s\",\"count\":%d}}\n", cause.c_str(), n);
        }
    }

    // The checks.
    int failures = 0;
    const auto fail = [&failures](const char* format, auto... args) {
        std::fprintf(stderr, "check failed: ");
        std::fprintf(stderr, format, args...);
        std::fprintf(stderr, "\n");
        ++failures;
    };
    if (colonists.size() != 6) {
        fail("%zu colonists, expected 6", colonists.size());
    }
    if (died > 0) {
        fail("%llu colonist(s) died", static_cast<unsigned long long>(died));
    }
    if (maxMalnutrition > 0.0f || minFood <= 0.0f) {
        fail("a colonist starved (lowest food %.3f, malnutrition up to %.3f)", static_cast<double>(minFood),
             static_cast<double>(maxMalnutrition));
    }
    if (worstStuck >= kStuckSamples && worst != nullptr) {
        fail("a colonist was stuck for %lld game minutes (job %s at cell %d,%d)", static_cast<long long>(worstStuck),
             jobTypeName(worst->worstJob), worst->worstCell.x, worst->worstCell.y);
    }
    if (sim.designations().count() > 0) {
        fail("%zu of %zu marks still not worked off", sim.designations().count(), markedRock + markedTrees + markedBushes);
    }
    if (unfinished > 0 || builtBy["Wall"] != expectedWalls || builtBy["Door"] != expectedDoors || builtBy["Bed"] != kBeds ||
        builtBy["Table1x2"] != 1 || builtBy["Stool"] != 3 || builtBy["FueledStove"] != 1 || builtBy["HorseshoesPin"] != 1) {
        fail("the building is not finished (walls %d/%d, doors %d/%d, beds %d/%d, table %d/1, stools %d/3, stove %d/1, pin %d/1, %d sites left)",
             builtBy["Wall"], expectedWalls, builtBy["Door"], expectedDoors, builtBy["Bed"], kBeds, builtBy["Table1x2"], builtBy["Stool"],
             builtBy["FueledStove"], builtBy["HorseshoesPin"], unfinished);
    }
    if (bedroomsRoofed != 1) {
        fail("no roofed room with %d beds, a table and a stove", kBeds);
    }
    if (sleptInBed == 0) {
        fail("nobody slept in a bed (%llu times asleep)", static_cast<unsigned long long>(slept));
    }
    if (weatherChanges == 0) {
        fail("the weather never changed in %.1f days", days);
    }
    if (mealsMade == 0 || ate == 0) {
        fail("the stove made %llu meals and colonists ate %llu times", static_cast<unsigned long long>(mealsMade),
             static_cast<unsigned long long>(ate));
    }
    if (clubsMade != 1) {
        fail("the crafting spot made %llu clubs, not the 1 the bill asked for", static_cast<unsigned long long>(clubsMade));
    }
    if (recreations == 0 || pinGames == 0) {
        fail("colonists started %llu recreation jobs, %llu at the horseshoes pin", static_cast<unsigned long long>(recreations),
             static_cast<unsigned long long>(pinGames));
    }
    if (talks == 0) {
        fail("%s", "the colonists never talked to each other");
    }
    size_t homeFilth = 0;
    for (int y = origin.y - 15; y < origin.y + kRoomHeight + 15; ++y) {
        for (int x = origin.x - 15; x < origin.x + kRoomWidth + 15; ++x) {
            if (year && sim.map().inBounds(CellPos{x, y, 0})) {
                for (const FilthPile& pile : sim.filth().at(CellPos{x, y, 0})) {
                    homeFilth += static_cast<size_t>(pile.thickness);
                }
            }
        }
    }
    if (year ? homeFilth > 60 : sim.filth().totalLayers() > 12) {
        fail("%zu layers of filth lie about (cleaned %llu): nobody keeps up with the dirt", year ? homeFilth : sim.filth().totalLayers(),
             static_cast<unsigned long long>(cleaned));
    }
    if (roomStat == nullptr) {
        fail("%s", "the room has no stats");
    }
    if (woodIn == 0) {
        fail("%s", "nobody put wood into the stove");
    }
    if (!(skillXpGained > 0.0)) {
        fail("the work taught nobody anything (%.1f experience gained)", skillXpGained);
    }
    if (commandsFailed > 0 || sim.stats().failedJobs > 0) {
        fail("%llu commands and %llu jobs failed", static_cast<unsigned long long>(commandsFailed),
             static_cast<unsigned long long>(sim.stats().failedJobs));
    }
    return failures > 0 ? 1 : 0;
}

}  // namespace colonyscenario

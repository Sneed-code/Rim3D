#include "ColonyScenario.h"

#include <chrono>

#include "ColonySim/Animals.h"
#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Map.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Health.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Prisoners.h"
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
#include "ColonySim/Trade.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colonyscenario {

namespace {

constexpr int64_t kFieldDay = 2;   // year run: the rice fields are laid out on this game day (sowing waits for frost-free weather)
constexpr int64_t kHuntDay = 13;   // year run: hunting starts on this game day
// ... with this many game animals marked at a time within kHuntRadius metres of the landing, a mark taken off one that runs
// kHuntLeash away. The fields feed the colony now: hunters that chased six marks up to 80 m out met wolf packs and manhunters
// far from help (two dead on seed 7), so they stay nearer home.
constexpr int kHuntMarks = 3;
constexpr float kHuntRadius = 50.0f;
constexpr float kHuntLeash = 60.0f;
constexpr int64_t kWinterDay = 33; // year run: the campfire, the warm clothes and the extra wood come on this game day
constexpr int kFields = 4;         // year run: rice fields
constexpr int kWinterTrees = 40;   // ... trees marked then, for the fire
constexpr int64_t kGreenhouseDay = 15;  // year run: the heated greenhouse is planned on this game day
// The greenhouse: wood walls round a 10 by 6 footprint with a door in the south wall. Inside (x 1..8, y 1..4):
// a row of conduits at y = 3 joining four hydroponics basins above it (y = 2), a heater and two standing lamps
// below it (y = 4) and a wood-fired generator (2 by 2, x 7..8, y 3..4) at the row's east end.
constexpr int kGreenWidth = 10;
constexpr int kGreenHeight = 6;
constexpr int kGreenBasins = 4;  // six took more steel than the crash site has, and then none got built
// The storeroom (year run, day 1): steel walls round a 10 by 8 footprint with a door in the south wall, and a stockpile for food
// only over its inside (8 by 6). Under a roof rice and meals don't weather away (rain wears a meal out in the open in about a
// day), and in winter the unheated room freezes and keeps them: the colony's winter larder.
constexpr int64_t kShedDay = 1;
constexpr int kFirebreak = 4;
constexpr size_t kSpecialists = 2;  // year run: colonists who doctor, and who cook
constexpr int kHealrootWidth = 6;  // year run: the healroot patch (herbal medicine), 6 by 4
constexpr int kHealrootHeight = 4;  // year run: cells cleared of plants round the buildings
constexpr int kShedWidth = 10;
constexpr int kShedHeight = 8;

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

// Dry ground to build or sow on: walkable, and not shallow water, marsh or mud (map generation 2).
bool dryGround(const Sim& sim, CellPos c) {
    if (!sim.map().inBounds(c) || !sim.map().groundWalkable(c)) {
        return false;
    }
    const Terrain t = sim.map().terrain(c);
    return t != Terrain::ShallowWater && t != Terrain::Marsh && t != Terrain::Mud;
}

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
                    ok = dryGround(sim, c) && sim.map().buildingAt(c) == kNoEntity;
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
CellPos findFieldSite(const Sim& sim, CellPos landing, const CellRect& room, const std::vector<CellRect>& taken = {}) {
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
                    ok = dryGround(sim, c) && sim.map().buildingAt(c) == kNoEntity && !room.contains(c) &&
                         sim.map().terrain(c) == Terrain::Soil &&
                         std::none_of(taken.begin(), taken.end(), [&](const CellRect& t) { return t.contains(c); });
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

// A walkable cell with nothing built on it, as near the centre as can be found.
CellPos freeCellNear(const Sim& sim, CellPos centre) {
    for (int ring = 0; ring < 30; ++ring) {
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                const CellPos c{centre.x + dx, centre.y + dy, 0};
                if (std::max(std::abs(dx), std::abs(dy)) == ring && dryGround(sim, c) &&
                    sim.map().buildingAt(c) == kNoEntity && sim.map().terrain(c) != Terrain::Rock) {
                    return c;
                }
            }
        }
    }
    return centre;
}

// The nearest spot to the landing for the greenhouse or the storeroom (with a ring of free ground round it), outside the room.
bool findGreenhouseSite(Sim& sim, CellPos landing, const CellRect& room, CellPos& out, int width = kGreenWidth, int height = kGreenHeight,
                        const std::vector<CellRect>& taken = {}) {
    bool found = false;
    float best = 0.0f;
    for (int dy = -30; dy <= 30; ++dy) {
        for (int dx = -30; dx <= 30; ++dx) {
            const CellPos origin{landing.x + dx, landing.y + dy, 0};
            bool ok = true;
            int trees = 0;
            for (int y = origin.y - 1; y <= origin.y + height && ok; ++y) {
                for (int x = origin.x - 1; x <= origin.x + width && ok; ++x) {
                    const CellPos c{x, y, 0};
                    ok = dryGround(sim, c) && sim.map().buildingAt(c) == kNoEntity &&
                         sim.farming().zoneAt(c) == kNoGrowingZone && sim.zones().stockpileAt(c) == nullptr &&
                         std::none_of(taken.begin(), taken.end(), [&](const CellRect& t) { return t.contains(c); }) &&
                         !(x >= room.minX - 2 && x < room.minX + room.sizeX + 2 && y >= room.minY - 2 && y < room.minY + room.sizeY + 2);
                    trees += ok && isTree(sim, sim.plants().defAt(c)) ? 1 : 0;  // cleared when the walls go up, at a cost
                }
            }
            const float score = distSq(origin, landing) + 400.0f * static_cast<float>(trees);
            ok = ok && sim.regions().canReach(landing, CellPos{origin.x + 1, origin.y - 1, 0});
            if (ok && (!found || score < best)) {
                found = true;
                best = score;
                out = origin;
            }
        }
    }
    return found;
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

// A colonist's illnesses and what ails it, on one line (year run, verbose): to see what an illness death came from.
void printIllness(const Sim& sim, Entity pawn, const char* prefix) {
    HealthReport report;
    if (!sim.healthReport(pawn, report) || (report.diseases.empty() && report.environment.empty() && !report.dead)) {
        return;
    }
    std::string line;
    char part[160];
    for (const HealthReport::DiseaseLine& d : report.diseases) {
        std::snprintf(part, sizeof(part), " %s%s%s (%s) severity %.2f immunity %.2f%s;", d.name.c_str(), d.part.empty() ? "" : " in the ",
                      d.part.c_str(), d.stage.c_str(), static_cast<double>(d.severity), static_cast<double>(d.immunity), d.tended ? " tended" : " untended");
        line += part;
    }
    for (const HealthReport::EnvironmentLine& env : report.environment) {
        std::snprintf(part, sizeof(part), " %s (%s) %.2f;", env.name.c_str(), env.stage.c_str(), static_cast<double>(env.severity));
        line += part;
    }
    std::snprintf(part, sizeof(part), " blood loss %.2f, %zu injuries, %s", static_cast<double>(report.bloodLoss), report.injuries.size(),
                  report.downed ? "down" : "up");
    line += part;
    std::fprintf(stderr, "%s colonist %u:%s\n", prefix, static_cast<unsigned>(entt::to_integral(pawn)), line.c_str());
}

}  // namespace

int run(uint64_t seed, int mapSize, double days, std::shared_ptr<const DefDatabase> defs, bool verbose, bool year, const std::string& narrator) {
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
        // 1 x 2: rows kRoomHeight-3 and kRoomHeight-2 hold the beds. The year run makes them of wood: the steel goes into the
        // storeroom's walls, which a wildfire must not burn.
        plan("Bed", year ? "WoodLog" : "Steel", 1 + i, kRoomHeight - 3);
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
    if (!narrator.empty()) {
        sim.submit(SetNarratorCommand{narrator, ""});  // the year scenario (C25): the narrator sends its incidents
    }
    if (year) {
        // The player has the colony's research done at the start: hydroponics (and the electricity it needs) is
        // what the greenhouse is made of.
        sim.submit(UnlockResearchCommand{"Electricity"});
        sim.submit(UnlockResearchCommand{"Hydroponics"});
    }
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
    // F10 (docs/design/field-care.md): one colonist is the alarm's Medic and one its Firefighter. With no raid the
    // roles change nothing: they work as everyone does.
    {
        std::vector<Entity> crew = colonists;
        std::sort(crew.begin(), crew.end());
        if (year) {
            // The year run makes its best doctor the Medic: the one who tends the sick while the alarm is on (plague and a
            // raid together left the plague untended on seed 1).
            const auto medicine = [&](Entity pawn) {
                const Colonist* colonist = sim.colonistOf(pawn);
                for (size_t i = 0; colonist != nullptr && i < colonist->skills.size(); ++i) {
                    if (colonist->skills[i].def == "Medicine") {
                        return colonist->skills[i].disabled ? -1 : colonist->skills[i].level;
                    }
                }
                return 0;
            };
            std::stable_sort(crew.begin(), crew.end(), [&](Entity a, Entity b) { return medicine(a) > medicine(b); });
            if (verbose) {
                for (const Entity pawn : crew) {
                    std::string line;
                    const Colonist* colonist = sim.colonistOf(pawn);
                    for (size_t i = 0; colonist != nullptr && i < colonist->skills.size(); ++i) {
                        line += " " + colonist->skills[i].def + " " + (colonist->skills[i].disabled ? std::string("-") : std::to_string(colonist->skills[i].level));
                    }
                    std::fprintf(stderr, "colonist %u:%s\n", static_cast<unsigned>(entt::to_integral(pawn)), line.c_str());
                }
            }
        }
        for (size_t c = 0; c < crew.size() && c < 2; ++c) {
            const DefenseRole role = c == 0 ? DefenseRole::Medic : DefenseRole::Firefighter;
            sim.submit(SetDefenseRoleCommand{crew[c], static_cast<uint8_t>(role), false, CellPos{}});
        }
    }

    // The year run arms everyone, as a player would: the best shot gets the rifle, the next the shotgun, the rest a revolver
    // each (the landing's two guns and three the script leaves there), and the last the club once the crafting spot has made
    // it. With one gun in the colony the others ran from every mad squirrel and wolf, and died once its owner fell.
    // A colonist's level in a skill, -1 if it can't do that work (or is gone).
    const auto skillOf = [&](Entity pawn, const char* skill) {
        const Colonist* colonist = sim.things().registry().valid(pawn) ? sim.colonistOf(pawn) : nullptr;
        for (size_t i = 0; colonist != nullptr && i < colonist->skills.size(); ++i) {
            if (colonist->skills[i].def == skill) {
                return colonist->skills[i].disabled ? -1 : colonist->skills[i].level;
            }
        }
        return colonist != nullptr ? 0 : -1;
    };
    std::vector<Entity> byShooting = colonists;
    if (year) {
        std::stable_sort(byShooting.begin(), byShooting.end(), [&](Entity a, Entity b) { return skillOf(a, "Shooting") > skillOf(b, "Shooting"); });
        const char* const loose[] = {"Gun_PumpShotgun", "Gun_Revolver", "Gun_Revolver"};
        int n = 0;
        for (const char* def : loose) {
            sim.submit(SpawnThingCommand{def, freeCellNear(sim, CellPos{origin.x + 2 + n, origin.y + kRoomHeight + 2, 0}), 1});
            ++n;
        }
    }
    // Hands one weapon to each colonist without one, best shots first: the guns lying about in order of `kGunOrder`, then
    // melee weapons. Returns the weapons handed out.
    const auto armColonists = [&]() {
        static const char* const kGunOrder[] = {"Gun_BoltActionRifle", "Gun_PumpShotgun", "Gun_Revolver", "Melee_Club"};
        const entt::registry& reg = sim.things().registry();
        std::vector<std::pair<int, Entity>> loose;
        for (const auto [item, thing] : reg.view<const Thing>().each()) {
            if (thing.category != ThingCategory::Item || reg.all_of<EquippedBy>(item) || !sim.things().spawned(item)) {
                continue;
            }
            const std::string& name = defs->get(thing.def).defName();
            for (int k = 0; k < static_cast<int>(std::size(kGunOrder)); ++k) {
                if (name == kGunOrder[k]) {
                    loose.emplace_back(k, item);
                }
            }
        }
        std::sort(loose.begin(), loose.end());
        size_t next = 0;
        int handed = 0;
        for (const Entity pawn : byShooting) {
            if (next >= loose.size()) {
                break;
            }
            const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
            const Gear* gear = reg.valid(pawn) ? reg.try_get<Gear>(pawn) : nullptr;
            if (health == nullptr || health->dead || gear == nullptr || gear->weapon != kNoEntity || !gear->stowed.empty()) {
                continue;
            }
            sim.submit(CarryWeaponCommand{pawn, loose[next++].second});
            ++handed;
        }
        return handed;
    };

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
    uint64_t starvedOrFroze = 0;  // deaths C25 never allows, even with the storyteller on
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
    uint64_t letters = 0;  // narrator letters (C25)
    uint64_t weatherChanges = 0;  // WeatherChanged events (E1): the colony works through rain, fog and cold
    uint64_t commandsFailed = 0;
    uint64_t recreations = 0;  // recreation jobs started
    uint64_t pinGames = 0;     // ... of them at the horseshoes pin
    uint64_t warmthSeeks = 0;   // times a colonist went to a warm room because of the cold
    uint64_t weaponsTaken = 0;  // loose weapons colonists picked up
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
    bool winterReady = false;
    bool greenhousePlanned = false;
    bool greenZoneMade = false;
    CellPos greenOrigin{0, 0, 0};
    std::vector<Plan> greenPlans;
    bool shedPlanned = false;
    bool shedFiltered = false;
    CellRect shedRect{0, 0, 0, 0, 0};
    std::vector<Plan> shedPlans;
    float greenMinTemp = 100.0f;
    float greenMaxTemp = -100.0f;
    int greenMostCrops = 0;  // most basins with a crop in them at once
    uint64_t greenCrops = 0;  // hours x crops standing in the basins
    uint64_t greenRipe = 0;   // hours x ripe crops in the basins
    std::map<Entity, bool> bestCare;  // year run: whether each colonist's medical care is Best (ill) or Herbal (wounds only)
    uint64_t firebreakMarks = 0;
    CellRect healrootRect{0, 0, 0, 0, 0};  // year run: the healroot patch, if one was made      // year run: plants marked for the firebreak
    uint64_t weaponsHanded = 0;
    std::map<std::pair<Entity, std::string>, int> workSet;  // year run: the work priority the script last set, plus 1 (0: none yet)       // year run: weapons the script handed to colonists
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
    std::map<Entity, int> shotsBy;            // year run, verbose: shots each pawn fired
    std::map<Entity, Entity> shooterOf;       // verbose: who fired each projectile in flight
    std::map<Entity, std::array<int, 8>> hitsBy;  // verbose: what each shooter's projectiles stopped on, by ProjectileHitKind
    std::map<Entity, std::array<float, 3>> lastGroundHit;  // verbose: where each shooter's last shot into the ground stopped
    std::map<std::string, int> spoiled;
    std::map<std::string, int> huntFailures;  // year run: hunts given up, by the animal's def       // year run: stacks lost to rot or the weather, by def and how
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
                    // The colony's own people: not raiders, visitors, traders or quest guests (made as colonists too).
                    if (sim.things().registry().valid(e.thing) && sim.things().registry().all_of<Colonist>(e.thing) &&
                        !sim.things().registry().all_of<Visitor>(e.thing) && sim.fighters().factionOf(e.thing) == kPlayerFaction) {
                        ++died;
                        starvedOrFroze += e.message == "starvation" || e.message == "hypothermia" ? 1 : 0;
                        std::fprintf(stderr, "day %lld: a colonist died: %s\n", static_cast<long long>(sim.time().day()), e.message.c_str());
                        printIllness(sim, e.thing, "    at death:");
                    } else {
                        const bool person = sim.things().registry().valid(e.thing) && sim.things().registry().all_of<Colonist>(e.thing);
                        ++animalDeaths[person ? "outsider: " + e.message : e.message];
                        if (verbose && person) {
                            std::fprintf(stderr, "day %lld: an outsider died: %s\n", static_cast<long long>(sim.time().day()), e.message.c_str());
                        }
                    }
                    break;
                case EventType::PawnAte: ++ate; atTable += e.message == "table" ? 1 : 0; break;
                case EventType::PawnWokeUp: ++slept; sleptInBed += e.message == "bed" ? 1 : 0; break;
                case EventType::Interaction: ++talks; break;
                case EventType::SocialFight: fights += e.message == "started" ? 1 : 0; break;
                case EventType::JobStarted:
                    if (e.job == JobType::SeekWarmth) {
                        ++warmthSeeks;
                    } else if (e.job == JobType::EquipWeapon) {
                        ++weaponsTaken;
                    }
                    if (e.job == JobType::Recreate) {
                        ++recreations;
                        pinGames += e.target != kNoEntity ? 1 : 0;
                    }
                    break;
                case EventType::JobEnded:
                    if (e.message.rfind("failed", 0) == 0) {
                        const entt::registry& reg = sim.things().registry();
                        const bool known = e.target != kNoEntity && reg.valid(e.target) && reg.all_of<Thing>(e.target);
                        std::fprintf(stderr, "step %lld: %s job failed for a colonist at %d,%d%s%s: %s\n", static_cast<long long>(i),
                                     jobTypeName(e.job), e.cell.x, e.cell.y, known ? ", target " : "",
                                     known ? defs->get(reg.get<Thing>(e.target).def).defName().c_str() : "", e.message.c_str());
                        if (e.job == JobType::Hunt && known) {
                            ++huntFailures[defs->get(reg.get<Thing>(e.target).def).defName()];
                        }
                    }
                    break;
                case EventType::MeleeSwing:
                    if (verbose && sim.things().registry().valid(e.target) && sim.things().registry().all_of<Colonist>(e.target) &&
                        sim.fighters().factionOf(e.target) == kPlayerFaction) {
                        const entt::registry& reg = sim.things().registry();
                        const Fighter* of = reg.try_get<Fighter>(e.thing);
                        std::fprintf(stderr, "    swing at colonist %u by %u side %u%s%s%s mode %d target %u\n", static_cast<unsigned>(entt::to_integral(e.target)),
                                     static_cast<unsigned>(entt::to_integral(e.thing)), static_cast<unsigned>(sim.fighters().factionOf(e.thing)),
                                     reg.all_of<Visitor>(e.thing) ? " visitor" : "", reg.all_of<Prisoner>(e.thing) ? " prisoner" : "",
                                     sim.mentalBreaks().active(e.thing) ? " mental-break" : "", of != nullptr ? static_cast<int>(of->mode) : -1,
                                     of != nullptr ? static_cast<unsigned>(entt::to_integral(of->target)) : 0u);
                    }
                    break;
                case EventType::PawnInjured:
                    if (verbose && sim.things().registry().valid(e.thing) && sim.things().registry().all_of<Colonist>(e.thing)) {
                        std::fprintf(stderr, "day %lld %02d:%02d: colonist %u hurt at %d,%d, part %d, %.1f damage (%s)\n", static_cast<long long>(sim.time().day()),
                                     sim.time().hourOfDay(), static_cast<int>((sim.time().seconds / 60) % 60), static_cast<unsigned>(entt::to_integral(e.thing)), e.cell.x,
                                     e.cell.y, e.part, static_cast<double>(e.amount), e.def.valid() ? sim.defs().get(e.def).defName().c_str() : e.message.c_str());
                        // What the colonist and the animals round it were doing, to see why nobody fought back.
                        const entt::registry& reg = sim.things().registry();
                        const Fighter* fighter = reg.try_get<Fighter>(e.thing);
                        const Gear* gear = reg.try_get<Gear>(e.thing);
                        std::fprintf(stderr, "    shots fired %d, alarm %d, fighter %s%s, weapon %s", shotsBy[e.thing], sim.raids().state().alarm ? 1 : 0,
                                     fighter != nullptr ? "yes" : "no",
                                     fighter != nullptr && fighter->defending ? " (defending)" : "",
                                     gear != nullptr && gear->weapon != kNoEntity ? "yes" : "no");
                        if (const Worker* w = reg.try_get<Worker>(e.thing)) {
                            std::fprintf(stderr, ", job %s%s%s%s", jobTypeName(w->job.type), sim.health().incapacitated(e.thing) ? ", down" : "",
                                         sim.raids().state().suppressed ? ", alarm held off" : "", sim.raids().state().hasThreat ? ", threat known" : "");
                        }
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
                        for (const auto [other, mover, pos] : reg.view<const Mover, const Position>().each()) {
                            const float d = std::hypot(pos.x - at.x, pos.y - at.y);
                            if (other != e.thing && !reg.all_of<Animal>(other) && d < 3.0f) {
                                const Fighter* of = reg.try_get<Fighter>(other);
                                std::fprintf(stderr, "; person %u side %u%s%s mode %d at %.1f m", static_cast<unsigned>(entt::to_integral(other)),
                                             static_cast<unsigned>(sim.fighters().factionOf(other)), reg.all_of<Visitor>(other) ? " visitor" : "",
                                             sim.mentalBreaks().active(other) ? " mental-break" : "", of != nullptr ? static_cast<int>(of->mode) : -1,
                                             static_cast<double>(d));
                            }
                        }
                        std::fprintf(stderr, "\n");
                    }
                    break;
                case EventType::PawnTended:
                    if (verbose && sim.things().registry().valid(e.target) && sim.things().registry().all_of<Colonist>(e.target)) {
                        std::fprintf(stderr, "    tended colonist %u: quality %.2f with %s%s\n", static_cast<unsigned>(entt::to_integral(e.target)),
                                     static_cast<double>(e.amount), e.def.valid() ? sim.defs().get(e.def).defName().c_str() : "no medicine",
                                     e.message.find("self") != std::string::npos ? " (self)" : "");
                    }
                    break;
                case EventType::ShotFired:
                    ++shotsBy[e.thing];
                    if (verbose) {
                        shooterOf[e.target] = e.thing;
                    }
                    break;
                case EventType::ProjectileHit:
                    if (verbose) {
                        // Shots colonists fire at animals: what each one hit, to see where friendly fire comes from.
                        const auto it = shooterOf.find(e.thing);
                        const entt::registry& reg = sim.things().registry();
                        const Entity shooter = it != shooterOf.end() ? it->second : kNoEntity;
                        if (shooter != kNoEntity) {
                            ++hitsBy[shooter][std::min<size_t>(static_cast<size_t>(e.hit), 7)];
                            if (e.hit == ProjectileHitKind::Ground) {
                                lastGroundHit[shooter] = {e.x, e.y, e.z};
                            }
                        }
                        const Fighter* f = shooter != kNoEntity && reg.valid(shooter) ? reg.try_get<Fighter>(shooter) : nullptr;
                        if (f != nullptr && reg.all_of<Colonist>(shooter) && reg.valid(f->target) && reg.all_of<Animal>(f->target)) {
                            const Position& from = reg.get<Position>(shooter);
                            const Position& to = reg.get<Position>(f->target);
                            std::fprintf(stderr, "    shot by %u at animal %u (%.1f m): hit kind %d thing %u at %.2f,%.2f,%.2f%s\n",
                                         static_cast<unsigned>(entt::to_integral(shooter)), static_cast<unsigned>(entt::to_integral(f->target)),
                                         static_cast<double>(std::hypot(to.x - from.x, to.y - from.y)), static_cast<int>(e.hit),
                                         static_cast<unsigned>(entt::to_integral(e.target)), static_cast<double>(e.x), static_cast<double>(e.y),
                                         static_cast<double>(e.z), e.target == f->target ? " (the target)" : "");
                        }
                        if (it != shooterOf.end()) {
                            shooterOf.erase(it);
                        }
                    }
                    break;
                case EventType::WeatherChanged: ++weatherChanges; break;
                case EventType::Letter:
                    ++letters;
                    if (verbose) {
                        std::fprintf(stderr, "day %lld: letter %s (%.0f points): %s\n", static_cast<long long>(sim.time().day()),
                                     e.def.valid() ? sim.defs().get(e.def).defName().c_str() : "?", static_cast<double>(e.amount), e.message.c_str());
                    }
                    break;
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
                case EventType::ItemSpoiled:
                    if (year) {
                        ++spoiled[defs->get(e.def).defName() + " " + e.message];
                    }
                    break;
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
                                     !(job.type == JobType::Recreate && job.step > 0) && !(job.type == JobType::BedRest && job.step > 0) &&
                                     !(job.type == JobType::SeekWarmth && job.step > 0);
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
                        // Why: the walk left, the job's cells and who stands close.
                        std::string why;
                        char part[160];
                        for (size_t k = m.next; k < m.path.size() && k < m.next + 6; ++k) {
                            std::snprintf(part, sizeof(part), " %d,%d%s", m.path[k].x, m.path[k].y, sim.map().walkable(m.path[k]) ? "" : "(blocked)");
                            why += part;
                        }
                        std::snprintf(part, sizeof(part), "; job cell %d,%d walk %d repaths %d; mover blocked %u held %u ghost %u gave up %d", job.cell.x,
                                      job.cell.y, static_cast<int>(job.walk), static_cast<int>(job.repaths), m.blockedSteps, m.heldSteps, m.ghostSteps,
                                      m.gaveUp ? 1 : 0);
                        why += part;
                        for (const auto [other, opos, omover] : sim.things().registry().view<const Position, const Mover>().each()) {
                            if (other != pawn && std::hypot(opos.x - pos.x, opos.y - pos.y) < 2.0f) {
                                const entt::registry& oreg = sim.things().registry();
                                const Worker* ow = oreg.try_get<Worker>(other);
                                std::snprintf(part, sizeof(part), "; pawn %u at %.2f,%.2f (%s%s%s%s)", static_cast<unsigned>(entt::to_integral(other)), opos.x,
                                              opos.y, ow != nullptr ? jobTypeName(ow->job.type) : (oreg.all_of<Animal>(other) ? "animal" : "-"),
                                              sim.health().incapacitated(other) ? ", down" : "", omover.hold ? ", held" : "",
                                              omover.arrived() ? "" : ", walking");
                                why += part;
                            }
                        }
                        std::fprintf(stderr, "    path%s\n", why.c_str());
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

        if (verbose && year && now % 600 == 0 && sim.raids().state().alarm) {
            // The fight, every ten game minutes while the alarm is on: each fighter, its side, place, mode, target and shots.
            const entt::registry& reg = sim.things().registry();
            std::string line;
            char part[200];
            for (const auto [pawn, f, pos] : reg.view<const Fighter, const Position>().each()) {
                const Health* h = reg.try_get<Health>(pawn);
                if (h != nullptr && h->dead) {
                    continue;
                }
                const Position* tp = reg.valid(f.target) ? reg.try_get<Position>(f.target) : nullptr;
                std::snprintf(part, sizeof(part), " [%u side %u at %.0f,%.0f mode %d%s%s target %u%s%s%s shots %d]",
                              static_cast<unsigned>(entt::to_integral(pawn)), static_cast<unsigned>(sim.fighters().factionOf(pawn)), pos.x, pos.y,
                              static_cast<int>(f.mode), f.defending ? " def" : "", f.drafted ? " drafted" : "", static_cast<unsigned>(entt::to_integral(f.target)),
                              tp != nullptr ? (" " + std::to_string(static_cast<int>(std::hypot(tp->x - pos.x, tp->y - pos.y))) + " m").c_str() : "",
                              f.canFire ? " can fire" : "", h != nullptr && h->downed ? " DOWN" : "", shotsBy[pawn]);
                line += part;
                const auto hit = hitsBy.find(pawn);
                if (hit != hitsBy.end()) {
                    // ground, rock, building, pawn, armor, gone
                    const std::array<int, 8>& k = hit->second;
                    std::snprintf(part, sizeof(part), " (stopped on ground %d rock %d building %d pawn %d armor %d gone %d)",
                                  k[static_cast<size_t>(ProjectileHitKind::Ground)], k[static_cast<size_t>(ProjectileHitKind::Rock)],
                                  k[static_cast<size_t>(ProjectileHitKind::Building)], k[static_cast<size_t>(ProjectileHitKind::Pawn)],
                                  k[static_cast<size_t>(ProjectileHitKind::Armor)], k[static_cast<size_t>(ProjectileHitKind::Gone)]);
                    line.insert(line.size() - 1, part);
                }
                const auto ground = lastGroundHit.find(pawn);
                if (ground != lastGroundHit.end()) {
                    const std::array<float, 3>& g = ground->second;
                    std::snprintf(part, sizeof(part), " (z %.2f, target z %.2f, last into the ground at %.1f,%.1f,%.2f)",
                                  static_cast<double>(sim.map().heightAt(pos.x, pos.y)),
                                  tp != nullptr ? static_cast<double>(sim.map().heightAt(tp->x, tp->y)) : 0.0, static_cast<double>(g[0]),
                                  static_cast<double>(g[1]), static_cast<double>(g[2]));
                    line.insert(line.size() - 1, part);
                }
            }
            std::fprintf(stderr, "  fight t=%lld:%s\n", static_cast<long long>(now), line.c_str());
        }
        if (now % kSecondsPerHour == 0) {
            if (year && !fieldMade && sim.time().day() >= kFieldDay) {
                // Sown at the start, the crops die in the early spring frosts: the field goes in with the warm weather.
                // Three fields: one feeds the colony for about five days, the stock of survival meals is gone by day 40.
                std::vector<CellRect> fields;
                const CellRect shedRing{shedRect.minX - 1, shedRect.minY - 1, shedRect.sizeX + 2, shedRect.sizeY + 2, 0};
                for (int f = 0; f < kFields; ++f) {
                    std::vector<CellRect> taken = fields;
                    taken.push_back(shedRing);
                    const CellPos site = findFieldSite(sim, landing, roomRect, taken);
                    if (verbose) {
                        std::fprintf(stderr, "rice field at %d,%d\n", site.x, site.y);
                    }
                    fields.push_back(CellRect{site.x, site.y, 14, 10, 0});
                    sim.submit(CreateGrowingZoneCommand{fields.back(), "Plant_Rice"});
                }
                // A healroot patch for herbal medicine, if anyone can sow it (Plants 8): what the wounds are tended with.
                bool sower = false;
                for (const Entity pawn : colonists) {
                    const Colonist* colonist = sim.colonistOf(pawn);
                    for (size_t k = 0; colonist != nullptr && k < colonist->skills.size(); ++k) {
                        sower = sower || (colonist->skills[k].def == "Plants" &&
                                          colonist->skills[k].level >= defs->get(defs->idOf("Things", "Plant_Healroot")).integer("plant.sowMinSkill", 0));
                    }
                }
                CellPos herbs;
                std::vector<CellRect> taken = fields;
                taken.push_back(shedRing);
                if (sower && findGreenhouseSite(sim, landing, roomRect, herbs, kHealrootWidth, kHealrootHeight, taken)) {
                    sim.submit(CreateGrowingZoneCommand{CellRect{herbs.x, herbs.y, kHealrootWidth, kHealrootHeight, 0}, "Plant_Healroot"});
                    healrootRect = CellRect{herbs.x, herbs.y, kHealrootWidth, kHealrootHeight, 0};
                    if (verbose) {
                        std::fprintf(stderr, "healroot at %d,%d\n", herbs.x, herbs.y);
                    }
                }
                fieldMade = true;
            }
            if (year && !winterReady && sim.time().day() >= kWinterDay) {
                // Winter coming: a campfire in the room (colonists with Hauling keep it in wood), more trees marked for it, and the
                // stock of warm clothes the script has no way to make yet (cloth is not grown): a parka and a tuque for each.
                winterReady = true;
                sim.submit(PlaceBlueprintCommand{"Campfire", "", CellPos{origin.x + 1, origin.y + 1, 0}, Rot4::North});
                int marksNow = 0;
                for (const Candidate& tree : trees) {
                    const DefId plant = sim.plants().defAt(tree.cell);
                    if (marksNow < kWinterTrees && plant.valid() && isTree(sim, plant) && sim.plants().growth(tree.cell) >= 0.95f) {
                        sim.submit(DesignateCommand{CellRect{tree.cell.x, tree.cell.y, 1, 1, 0}, DesignationKind::CutPlant});
                        ++marksNow;
                    }
                }
                const DefId cloth = defs->idOf("Things", "Cloth");
                for (size_t k = 0; k < colonists.size() * 2; ++k) {
                    std::string why;
                    sim.apparel().make(defs->idOf("Things", k % 2 == 0 ? "Apparel_Parka" : "Apparel_Tuque"), cloth, -1,
                                       freeCellNear(sim, CellPos{origin.x + 2 + static_cast<int>(k), origin.y - 2, 0}), &why);
                }
            }
            if (year && !shedPlanned && sim.time().day() >= kShedDay) {
                // The winter larder: a roofed room of steel near the landing with a food stockpile inside.
                shedPlanned = true;
                CellPos shedOrigin;
                if (!findGreenhouseSite(sim, landing, roomRect, shedOrigin, kShedWidth, kShedHeight)) {
                    std::fprintf(stderr, "colony scenario: no room for the storeroom\n");
                    return 2;
                }
                if (verbose) {
                    std::fprintf(stderr, "storeroom at %d,%d\n", shedOrigin.x, shedOrigin.y);
                }
                shedRect = CellRect{shedOrigin.x, shedOrigin.y, kShedWidth, kShedHeight, 0};
                // Steel walls and a steel door: on seed 7 a flashstorm's grass fire burned the wooden one with every meal in it.
                const auto shed = [&](const char* def, int x, int y) {
                    shedPlans.push_back(Plan{defs->idOf("Things", def), def, "Steel", CellPos{shedOrigin.x + x, shedOrigin.y + y, 0}, Rot4::North});
                };
                for (int x = 0; x < kShedWidth; ++x) {
                    shed(x == 1 ? "Door" : "Wall", x, 0);
                    shed("Wall", x, kShedHeight - 1);
                }
                for (int y = 1; y < kShedHeight - 1; ++y) {
                    shed("Wall", 0, y);
                    shed("Wall", kShedWidth - 1, y);
                }
                for (const Plan& p : shedPlans) {
                    sim.submit(PlaceBlueprintCommand{p.defName, p.stuff, p.cell, p.rot});
                }
                sim.submit(CreateStockpileCommand{CellRect{shedOrigin.x + 1, shedOrigin.y + 1, kShedWidth - 2, kShedHeight - 2, 0},
                                                  StoragePriority::Important});
            } else if (shedPlanned && !shedFiltered) {
                // Food, medicine and components only (in the open the crash site's medicine and components weather away in a
                // few weeks, and a doctor with no medicine tends too badly to stop an infection): every other item and every
                // corpse stays out (the hauled game goes to the butcher).
                const StockpileZone* zone = sim.zones().stockpileAt(CellPos{shedRect.minX + 1, shedRect.minY + 1, 0});
                if (zone != nullptr) {
                    for (const DefId d : defs->idsOfKind("Things")) {
                        const Def& def = defs->get(d);
                        const bool keep = def.has("ingestible") || def.defName().rfind("Medicine", 0) == 0 || def.defName().rfind("Component", 0) == 0;
                        if ((def.text("category") == "Item" && !keep) || def.text("category") == "Pawn") {
                            sim.submit(SetStockpileAllowedCommand{zone->id, def.defName(), false});
                        }
                    }
                    shedFiltered = true;
                }
            }
            if (year && !greenhousePlanned && sim.time().day() >= kGreenhouseDay) {
                greenhousePlanned = true;
                if (!findGreenhouseSite(sim, landing, roomRect, greenOrigin, kGreenWidth, kGreenHeight, {shedRect})) {
                    std::fprintf(stderr, "colony scenario: no room for the greenhouse\n");
                    return 2;
                }
                if (verbose) {
                    std::fprintf(stderr, "greenhouse at %d,%d\n", greenOrigin.x, greenOrigin.y);
                }
                const auto green = [&](const char* def, const char* stuff, int x, int y) {
                    greenPlans.push_back(Plan{defs->idOf("Things", def), def, stuff, CellPos{greenOrigin.x + x, greenOrigin.y + y, 0}, Rot4::North});
                };
                for (int x = 0; x < kGreenWidth; ++x) {
                    for (const int y : {0, kGreenHeight - 1}) {
                        if (y == 0 && x == 1) {
                            green("Door", "WoodLog", x, y);
                        } else {
                            green("Wall", "WoodLog", x, y);
                        }
                    }
                }
                for (int y = 1; y < kGreenHeight - 1; ++y) {
                    green("Wall", "WoodLog", 0, y);
                    green("Wall", "WoodLog", kGreenWidth - 1, y);
                }
                for (int x = 1; x <= 6; ++x) {
                    green("PowerConduit", "", x, 3);  // up to the generator
                }
                for (int x = 1; x <= kGreenBasins; ++x) {
                    green("HydroponicsBasin", "", x, 2);
                }
                green("WoodFiredGenerator", "", 7, 3);
                green("Heater", "", 1, 4);
                green("StandingLamp", "", 3, 4);
                green("StandingLamp", "", 5, 4);
                for (const Plan& p : greenPlans) {
                    sim.submit(PlaceBlueprintCommand{p.defName, p.stuff, p.cell, p.rot});
                }
            }
            int treesMarked = 0;
            for (const auto& [index, kind] : sim.designations().all()) {
                treesMarked += kind == DesignationKind::CutPlant && isTree(sim, sim.plants().defAt(sim.map().cellAt(index))) ? 1 : 0;
            }
            if (year && now % (6 * kSecondsPerHour) == 0 && treesMarked < 6) {
                // Wood runs out (stove, walls, the greenhouse's generator): keep a few of the nearest trees marked.
                int wood = 0;
                for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
                    wood += defs->get(thing.def).defName() == "WoodLog" ? stack.count : 0;
                }
                if (wood < 400) {
                    std::vector<Candidate> near;
                    for (int dy = -kMarkRadius; dy <= kMarkRadius; ++dy) {
                        for (int dx = -kMarkRadius; dx <= kMarkRadius; ++dx) {
                            const CellPos c{landing.x + dx, landing.y + dy, 0};
                            const DefId plant = sim.map().inBounds(c) ? sim.plants().defAt(c) : DefId();
                            if (plant.valid() && sim.plants().growth(c) >= 0.95f && isTree(sim, plant) && touchable(sim, landing, c)) {
                                near.push_back({c, distSq(c, landing)});
                            }
                        }
                    }
                    nearest(near);
                    mark(near, 10, DesignationKind::CutPlant);
                }
            }
            if (year && now % kSecondsPerDay == 6 * kSecondsPerHour) {
                // A firebreak, as a player clears one: every plant within kFirebreak cells of the room, the storeroom and the
                // greenhouse is marked for cutting each morning (grass grows back), crops in a growing zone left alone. Fire
                // reaches two cells (embers), and walls don't stop embers.
                std::vector<CellRect> base{roomRect};
                if (shedPlanned) {
                    base.push_back(shedRect);
                }
                if (greenhousePlanned) {
                    base.push_back(CellRect{greenOrigin.x, greenOrigin.y, kGreenWidth, kGreenHeight, 0});
                }
                for (const CellRect& r : base) {
                    for (int y = r.minY - kFirebreak; y < r.minY + r.sizeY + kFirebreak; ++y) {
                        for (int x = r.minX - kFirebreak; x < r.minX + r.sizeX + kFirebreak; ++x) {
                            const CellPos c{x, y, 0};
                            if (sim.map().inBounds(c) && sim.plants().has(c) && sim.farming().zoneAt(c) == kNoGrowingZone &&
                                sim.designations().at(c) == DesignationKind::None) {
                                sim.submit(DesignateCommand{CellRect{x, y, 1, 1, 0}, DesignationKind::CutPlant});
                                ++firebreakMarks;
                            }
                        }
                    }
                }
            }
            if (year) {
                // Medicine is kept for illness: a colonist is tended with the best there is while it has a disease (plague,
                // flu, an infection...), and with herbal medicine or none for wounds alone. With Best for all, the crash
                // site's 30 went on scratches, and plague was then tended by hand at quality 0.25 to 0.55.
                for (const Entity pawn : colonists) {
                    HealthReport report;
                    if (!sim.things().registry().valid(pawn) || !sim.healthReport(pawn, report) || report.dead) {
                        continue;
                    }
                    const bool ill = std::any_of(report.diseases.begin(), report.diseases.end(),
                                                 [](const HealthReport::DiseaseLine& d) { return d.visible; });
                    const auto it = bestCare.find(pawn);
                    if (it == bestCare.end() || it->second != ill) {
                        sim.submit(SetMedicalCareCommand{pawn, ill ? "Best" : "Herbal"});
                        bestCare[pawn] = ill;
                    }
                }
                weaponsHanded += static_cast<uint64_t>(armColonists());
                // Doctoring and cooking for the two best at them, as a player sets the work tab: tends by a doctor of level 2
                // were 0.14 to 0.4 and lost plague races, and poor cooks poison the food. Checked again every hour, so the next
                // best take over when one dies.
                for (const auto& [work, skill] : {std::pair<const char*, const char*>{"Doctor", "Medicine"}, {"Cooking", "Cooking"}}) {
                    std::vector<Entity> able;
                    for (const Entity pawn : colonists) {
                        const Health* health = sim.things().registry().valid(pawn) ? sim.things().registry().try_get<Health>(pawn) : nullptr;
                        if (health != nullptr && !health->dead && skillOf(pawn, skill) >= 0) {
                            able.push_back(pawn);
                        }
                    }
                    std::stable_sort(able.begin(), able.end(), [&](Entity a, Entity b) { return skillOf(a, skill) > skillOf(b, skill); });
                    for (size_t k = 0; k < able.size(); ++k) {
                        // The others still doctor last of all (rescue, feeding the downed), and never cook.
                        const int priority = k < kSpecialists ? 1 : (std::strcmp(work, "Doctor") == 0 ? 4 : 0);
                        int& last = workSet[{able[k], work}];
                        if (last != priority + 1) {
                            sim.submit(SetWorkPriorityCommand{able[k], work, priority});
                            last = priority + 1;
                        }
                    }
                }
            }
            if (greenhousePlanned && !greenZoneMade) {
                int basins = 0;
                for (int x = 1; x <= kGreenBasins; ++x) {
                    const Entity b = sim.map().buildingAt(CellPos{greenOrigin.x + x, greenOrigin.y + 2, 0});
                    basins += b != kNoEntity ? 1 : 0;  // blueprints and frames are not on the building layer
                }
                if (basins == kGreenBasins) {
                    sim.submit(CreateGrowingZoneCommand{CellRect{greenOrigin.x + 1, greenOrigin.y + 2, kGreenBasins, 1, 0}, "Plant_Rice"});
                    greenZoneMade = true;
                    if (verbose) {
                        std::fprintf(stderr, "    greenhouse basins built, rice zone made\n");
                    }
                }
            }
            if (greenZoneMade) {
                const float t = sim.temperature().temperatureAt(CellPos{greenOrigin.x + 3, greenOrigin.y + 2, 0});
                greenMinTemp = std::min(greenMinTemp, t);
                greenMaxTemp = std::max(greenMaxTemp, t);
                int grown = 0;
                for (int x = 1; x <= kGreenBasins; ++x) {
                    const CellPos b{greenOrigin.x + x, greenOrigin.y + 2, 0};
                    if (sim.plants().has(b)) {
                        ++greenCrops;
                        greenRipe += sim.farming().readyToHarvest(b) ? 1 : 0;
                        ++grown;
                    }
                }
                greenMostCrops = std::max(greenMostCrops, grown);
                if (verbose && now % (12 * kSecondsPerHour) == 0) {
                    std::fprintf(stderr, "    greenhouse: %.1f C (outside %.1f), %d crops in the basins\n", static_cast<double>(t),
                                 static_cast<double>(sim.outdoorTemperature()), grown);
                    const Room* room = sim.rooms().roomOf(CellPos{greenOrigin.x + 3, greenOrigin.y + 2, 0});
                    const Entity heater = sim.map().buildingAt(CellPos{greenOrigin.x + 1, greenOrigin.y + 4, 0});
                    const Entity gen = sim.map().buildingAt(CellPos{greenOrigin.x + 7, greenOrigin.y + 3, 0});
                    int wood = 0;
                    int steel = 0;
                    for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
                        wood += defs->get(thing.def).defName() == "WoodLog" ? stack.count : 0;
                        steel += defs->get(thing.def).defName() == "Steel" ? stack.count : 0;
                    }
                    const Fuel* genFuel = gen != kNoEntity ? sim.things().registry().try_get<Fuel>(gen) : nullptr;
                    std::fprintf(stderr, "      wood on the map %d, steel %d, generator holds %.1f\n", wood, steel, genFuel != nullptr ? static_cast<double>(genFuel->amount) : -1.0);
                    std::fprintf(stderr, "      room enclosed %d, open roof cells %d; heater powered %d running %d; generator lit %d powered %d\n",
                                 room != nullptr && room->enclosed(), room != nullptr ? room->openRoofCount : -1,
                                 heater != kNoEntity && sim.power().powered(heater), heater != kNoEntity && sim.power().running(heater),
                                 gen != kNoEntity && sim.jobs().fuel().lit(gen), gen != kNoEntity && sim.power().powered(gen));
                }
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
                        // Only corpses within 40 m: with no limit the butcher walks across the map to a wolf's kill. People stay
                        // out of the filter, as by default.
                        sim.submit(SetBillOptionsCommand{t.id, 1, 0, 20, 40.0f, 0, 0, 0, {"Human"}});
                        butcherBillAdded = true;
                    }
                }
                // Hunting: keep a few game animals marked, nearest to the landing first (not the predators or the boars).
                if (now >= kHuntDay * kSecondsPerDay) {
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
                            if (away > kHuntLeash) {
                                sim.submit(MarkAnimalCommand{animal, "Hunt", false});  // it ran off: the hunter would follow it for good
                            } else {
                                ++marked;
                            }
                        } else if (name == "Deer" || name == "Hare" || name == "Muffalo" || name == "Alpaca") {
                            const float d = std::hypot(pos.x - static_cast<float>(landing.x), pos.y - static_cast<float>(landing.y));
                            if (d < kHuntRadius) {
                                prey.emplace_back(d, animal);
                            }
                        }
                    }
                    std::sort(prey.begin(), prey.end());
                    for (size_t k = 0; marked < kHuntMarks && k < prey.size(); ++k, ++marked) {
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
                int steel = 0;
                int components = 0;
                for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
                    const std::string& name = defs->get(thing.def).defName();
                    steel += name == "Steel" ? stack.count : 0;
                    components += name == "ComponentIndustrial" ? stack.count : 0;
                    rice += name == "RawRice" ? stack.count : 0;
                    meat += name.rfind("Meat", 0) == 0 ? stack.count : 0;
                    meals += name.rfind("Meal", 0) == 0 ? stack.count : 0;
                }
                std::fprintf(stderr, "    food: rice %d, meat %d, meals %d; crops ripe %zu; hunted %llu; steel %d, components %d\n", rice, meat, meals,
                             ripeCrops(sim), static_cast<unsigned long long>(sim.husbandry().stats().hunted), steel, components);
            }
            if (verbose && year && now % (6 * kSecondsPerHour) == 3 * kSecondsPerHour) {
                // Each colonist's mood, what makes it and what it is doing, four times a day.
                for (const Entity pawn : colonists) {
                    if (!sim.things().registry().valid(pawn)) {
                        continue;
                    }
                    const Mood* mood = sim.things().registry().try_get<Mood>(pawn);
                    const Health* health = sim.things().registry().try_get<Health>(pawn);
                    if (mood == nullptr || health == nullptr || health->dead) {
                        continue;
                    }
                    std::string why;
                    for (const ThoughtView& t : sim.mood().thoughts(pawn, now)) {
                        char part[96];
                        std::snprintf(part, sizeof(part), " %s %+.0f", t.def.c_str(), static_cast<double>(t.offset));
                        why += part;
                    }
                    printIllness(sim, pawn, "    illness");
                    const Worker& worker = sim.things().registry().get<Worker>(pawn);
                    const Position& at = sim.things().registry().get<Position>(pawn);
                    std::fprintf(stderr, "    mood %u: %.0f (food %.2f, %s at %.0f,%.0f, %s)%s\n", static_cast<unsigned>(entt::to_integral(pawn)),
                                 static_cast<double>(mood->level), static_cast<double>(sim.things().registry().get<Needs>(pawn).food),
                                 jobTypeName(worker.job.type), static_cast<double>(at.x), static_cast<double>(at.y),
                                 health->downed ? "down" : "up", why.c_str());
                }
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
    for (const Plan& p : shedPlans) {
        expectedWalls += p.defName == "Wall" ? 1 : 0;
        expectedDoors += p.defName == "Door" ? 1 : 0;
    }
    for (const Plan& p : greenPlans) {
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
        std::printf("{\"year\":{\"worstStepMs\":%.2f,\"worstStepAt\":%lld,\"day\":%lld,\"warmthSeeks\":%llu,\"weaponsTaken\":%llu,\"weaponsHanded\":%llu,\"firebreakMarks\":%llu}}\n", worstStepMs, static_cast<long long>(worstStepAt), static_cast<long long>(worstStepAt / kSecondsPerDay), static_cast<unsigned long long>(warmthSeeks), static_cast<unsigned long long>(weaponsTaken), static_cast<unsigned long long>(weaponsHanded), static_cast<unsigned long long>(firebreakMarks));
        std::printf("{\"year\":{\"worstStepMs\":%.2f,\"worstStepAt\":%lld,\"day\":%lld,\"narrator\":\"%s\",\"letters\":%llu}}\n", worstStepMs, static_cast<long long>(worstStepAt), static_cast<long long>(worstStepAt / kSecondsPerDay), narrator.c_str(), static_cast<unsigned long long>(letters));
        if (greenhousePlanned) {
            std::printf("{\"greenhouse\":{\"at\":\"%d,%d\",\"basins\":%d,\"generators\":%d,\"heaters\":%d,\"lamps\":%d,\"zoneMade\":%s,"
                        "\"minTemp\":%.1f,\"maxTemp\":%.1f,\"mostCrops\":%d,\"cropHours\":%llu,\"ripeHours\":%llu,\"unbuilt\":%d}}\n",
                        greenOrigin.x, greenOrigin.y, builtBy["HydroponicsBasin"], builtBy["WoodFiredGenerator"], builtBy["Heater"],
                        builtBy["StandingLamp"], greenZoneMade ? "true" : "false", static_cast<double>(greenMinTemp),
                        static_cast<double>(greenMaxTemp), greenMostCrops, static_cast<unsigned long long>(greenCrops),
                        static_cast<unsigned long long>(greenRipe), unfinished);
        }
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
        for (const auto& [what, n] : huntFailures) {
            std::printf("{\"huntFailed\":{\"animal\":\"%s\",\"count\":%d}}\n", what.c_str(), n);
        }
        for (const auto& [what, n] : spoiled) {
            std::printf("{\"spoiled\":{\"stacks\":\"%s\",\"count\":%d}}\n", what.c_str(), n);
        }
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
    // With the storyteller on (C25) raids, fires, disease and mad animals may kill colonists and burn what they built,
    // as in RimWorld: the year run then only has to keep the colony alive, fed, warm and unstuck. The checks that
    // follow the script's plan (the building, the marks, the dirt, failed jobs) are reported, not failed.
    const bool storyteller = !narrator.empty();
    int notes = 0;
    const auto planCheck = [&](const char* format, auto... args) {
        if (!storyteller) {
            fail(format, args...);
            return;
        }
        std::fprintf(stderr, "note: ");
        std::fprintf(stderr, format, args...);
        std::fprintf(stderr, "\n");
        ++notes;
    };
    if (colonists.size() != 6) {
        fail("%zu colonists, expected 6", colonists.size());
    }
    if (died > 0) {
        planCheck("%llu colonist(s) died", static_cast<unsigned long long>(died));
    }
    if (starvedOrFroze > 0) {
        fail("%llu colonist(s) starved or froze to death", static_cast<unsigned long long>(starvedOrFroze));
    }
    size_t alive = 0;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        const Health* health = sim.things().registry().try_get<Health>(pawn);
        alive += sim.fighters().factionOf(pawn) == kPlayerFaction && !sim.things().registry().all_of<Visitor>(pawn) &&
                         (health == nullptr || !health->dead)
                     ? 1
                     : 0;
    }
    if (alive == 0) {
        fail("%s", "the colony died out");
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
        planCheck("%zu of %zu marks still not worked off", sim.designations().count(), markedRock + markedTrees + markedBushes);
    }
    if (unfinished > 0 || builtBy["Wall"] != expectedWalls || builtBy["Door"] != expectedDoors || builtBy["Bed"] != kBeds ||
        builtBy["Table1x2"] != 1 || builtBy["Stool"] != 3 || builtBy["FueledStove"] != 1 || builtBy["HorseshoesPin"] != 1) {
        planCheck("the building is not finished (walls %d/%d, doors %d/%d, beds %d/%d, table %d/1, stools %d/3, stove %d/1, pin %d/1, %d sites left)",
             builtBy["Wall"], expectedWalls, builtBy["Door"], expectedDoors, builtBy["Bed"], kBeds, builtBy["Table1x2"], builtBy["Stool"],
             builtBy["FueledStove"], builtBy["HorseshoesPin"], unfinished);
    }
    if (bedroomsRoofed != 1) {
        planCheck("no roofed room with %d beds, a table and a stove", kBeds);
    }
    if (sleptInBed == 0) {
        planCheck("nobody slept in a bed (%llu times asleep)", static_cast<unsigned long long>(slept));
    }
    if (weatherChanges == 0) {
        fail("the weather never changed in %.1f days", days);
    }
    if (mealsMade == 0 || ate == 0) {
        fail("the stove made %llu meals and colonists ate %llu times", static_cast<unsigned long long>(mealsMade),
             static_cast<unsigned long long>(ate));
    }
    if (clubsMade != 1) {
        planCheck("the crafting spot made %llu clubs, not the 1 the bill asked for", static_cast<unsigned long long>(clubsMade));
    }
    if (recreations == 0 || pinGames == 0) {
        planCheck("colonists started %llu recreation jobs, %llu at the horseshoes pin", static_cast<unsigned long long>(recreations),
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
        planCheck("%zu layers of filth lie about (cleaned %llu): nobody keeps up with the dirt", year ? homeFilth : sim.filth().totalLayers(),
             static_cast<unsigned long long>(cleaned));
    }
    if (roomStat == nullptr) {
        planCheck("%s", "the room has no stats");
    }
    if (woodIn == 0) {
        planCheck("%s", "nobody put wood into the stove");
    }
    if (!(skillXpGained > 0.0)) {
        planCheck("the work taught nobody anything (%.1f experience gained)", skillXpGained);
    }
    if (year && days >= static_cast<double>(kGreenhouseDay) + 12.0 && !greenZoneMade) {
        planCheck("%s", "the greenhouse basins were not all built 12 days after it was planned");
    }
    if (commandsFailed > 0 || sim.stats().failedJobs > 0) {
        planCheck("%llu commands and %llu jobs failed", static_cast<unsigned long long>(commandsFailed),
             static_cast<unsigned long long>(sim.stats().failedJobs));
    }
    if (notes > 0) {
        std::fprintf(stderr, "%d note(s) above: the storyteller's events undid part of the plan; %zu colonists alive\n", notes, alive);
    }
    return failures > 0 ? 1 : 0;
}

}  // namespace colonyscenario

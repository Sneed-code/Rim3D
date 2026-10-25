#include "ColonyScenario.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
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

}  // namespace

int run(uint64_t seed, int mapSize, double days, std::shared_ptr<const DefDatabase> defs, bool verbose) {
    SimSettings settings{seed, mapSize, 0, 0};
    settings.scenario = "Crashlanded";
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
    for (const Plan& p : plans) {
        sim.submit(PlaceBlueprintCommand{p.defName, p.stuff, p.cell, p.rot});
    }
    const CellRect roomRect{origin.x, origin.y, kRoomWidth, kRoomHeight, 0};

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
            const SkillRecord& skill = sim.colonistOf(pawn)->skills[index];
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
    uint64_t commandsFailed = 0;
    float minFood = 1.0f;
    float minRest = 1.0f;
    float maxMalnutrition = 0.0f;
    float minMood = 100.0f;
    bool billAdded = false;
    bool bedsAssigned = false;
    int64_t stoveBuiltAt = -1;
    int64_t allBuiltAt = -1;
    int64_t designationsDoneAt = -1;
    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    Snapshot snapshot;

    for (int64_t i = 0; i < steps; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            switch (e.type) {
                case EventType::PawnDied: ++died; std::fprintf(stderr, "day %lld: a colonist died: %s\n", static_cast<long long>(sim.time().day()), e.message.c_str()); break;
                case EventType::PawnAte: ++ate; atTable += e.message == "table" ? 1 : 0; break;
                case EventType::PawnWokeUp: ++slept; sleptInBed += e.message == "bed" ? 1 : 0; break;
                case EventType::JobEnded:
                    if (e.message.rfind("failed", 0) == 0) {
                        std::fprintf(stderr, "step %lld: %s job failed for a colonist at %d,%d: %s\n", static_cast<long long>(i),
                                     jobTypeName(e.job), e.cell.x, e.cell.y, e.message.c_str());
                    }
                    break;
                case EventType::CellMined: ++mined; break;
                case EventType::PlantRemoved: ++felled; break;
                case EventType::PlantHarvested: ++picked; break;
                case EventType::RecipeDone: mealsMade += static_cast<uint64_t>(std::max(0.0f, e.amount)); break;
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
                const bool working = job.type != JobType::None && job.type != JobType::Wait && !jobIsLyingDown(job);
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
            for (const auto [pawn, mood] : sim.things().registry().view<const Mood>().each()) {
                (void)pawn;
                minMood = std::min(minMood, mood.level);
            }
        }

        if (now % kSecondsPerHour == 0) {
            sim.writeSnapshot(snapshot);
            int built = 0;
            int stoveId = 0;
            Entity stove = kNoEntity;
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
            if (stove != kNoEntity && !billAdded) {
                // Cook until there are 8 meals on the map, so the stove keeps the colony fed.
                sim.submit(AddBillCommand{stove, "CookMealSimple", BillMode::UntilHave, 8});
                billAdded = true;
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
    std::printf("{\"colony\":{\"seed\":%llu,\"size\":%d,\"days\":%.2f,\"colonists\":%zu,\"died\":%llu,"
                "\"minFood\":%.3f,\"maxMalnutrition\":%.3f,\"minRest\":%.3f,\"minMood\":%.1f,"
                "\"stuckMinutesWorst\":%lld,\"idleMinutesWorst\":%lld,"
                "\"marked\":{\"rock\":%zu,\"trees\":%zu,\"bushes\":%zu},\"marksLeft\":%zu,\"marksDoneAtSecond\":%lld,"
                "\"mined\":%llu,\"felled\":%llu,\"picked\":%llu,"
                "\"planned\":%zu,\"walls\":%d,\"doors\":%d,\"beds\":%d,\"tables\":%d,\"stools\":%d,\"stoves\":%d,\"unfinished\":%d,"
                "\"allBuiltAtSecond\":%lld,\"stoveBuiltAtSecond\":%lld,\"roomOk\":%s,"
                "\"skillXpGained\":%.0f,\"mealsCooked\":%llu,\"ate\":%llu,\"atTable\":%llu,\"woke\":%llu,\"wokeInBed\":%llu,"
                "\"commandsFailed\":%llu,\"failedJobs\":%llu,\"stateHash\":\"%016llx\"}}\n",
                static_cast<unsigned long long>(seed), mapSize, days, colonists.size(), static_cast<unsigned long long>(died),
                static_cast<double>(minFood), static_cast<double>(maxMalnutrition), static_cast<double>(minRest),
                static_cast<double>(minMood), static_cast<long long>(worstStuck), static_cast<long long>(worstIdle),
                markedRock, markedTrees, markedBushes, sim.designations().count(),
                static_cast<long long>(designationsDoneAt), static_cast<unsigned long long>(mined),
                static_cast<unsigned long long>(felled), static_cast<unsigned long long>(picked), plans.size(),
                builtBy["Wall"], builtBy["Door"], builtBy["Bed"], builtBy["Table1x2"], builtBy["Stool"], builtBy["FueledStove"], unfinished,
                static_cast<long long>(allBuiltAt), static_cast<long long>(stoveBuiltAt), bedroomsRoofed == 1 ? "true" : "false",
                skillXpGained, static_cast<unsigned long long>(mealsMade), static_cast<unsigned long long>(ate),
                static_cast<unsigned long long>(atTable), static_cast<unsigned long long>(slept),
                static_cast<unsigned long long>(sleptInBed), static_cast<unsigned long long>(commandsFailed),
                static_cast<unsigned long long>(sim.stats().failedJobs), static_cast<unsigned long long>(sim.stateHash()));

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
        builtBy["Table1x2"] != 1 || builtBy["Stool"] != 3 || builtBy["FueledStove"] != 1) {
        fail("the building is not finished (walls %d/%d, doors %d/%d, beds %d/%d, table %d/1, stools %d/3, stove %d/1, %d sites left)",
             builtBy["Wall"], expectedWalls, builtBy["Door"], expectedDoors, builtBy["Bed"], kBeds, builtBy["Table1x2"], builtBy["Stool"],
             builtBy["FueledStove"], unfinished);
    }
    if (bedroomsRoofed != 1) {
        fail("no roofed room with %d beds, a table and a stove", kBeds);
    }
    if (sleptInBed == 0) {
        fail("nobody slept in a bed (%llu times asleep)", static_cast<unsigned long long>(slept));
    }
    if (mealsMade == 0 || ate == 0) {
        fail("the stove made %llu meals and colonists ate %llu times", static_cast<unsigned long long>(mealsMade),
             static_cast<unsigned long long>(ate));
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

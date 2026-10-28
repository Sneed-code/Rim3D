#include "HouseScenario.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace housescenario {

namespace {

using namespace colony;

// The house: walls round a 9 by 7 footprint with a door in the south wall. Inside, stairs at x = 1 climb
// north from the cell in front of their foot (1, 1) to (1, 5) upstairs; the upper floor covers the whole
// footprint, the walls included, and the upper walls stand on it. Six beds upstairs.
constexpr int kWidth = 9;
constexpr int kHeight = 7;
constexpr int kDoorX = 1;
constexpr int kStairsX = 1;
constexpr int kStairsY = 2;  // the stairs cover y = 2 to 4
constexpr int kBeds = 6;
constexpr int kSearchRadius = 40;
constexpr int64_t kStuckSamples = 60;  // one sample a game minute: a game hour without a change is stuck
constexpr int kDropHours = 4;          // a drop of cloth and blocks to haul every four game hours ...
constexpr int kDropEndHours = 10;      // ... until ten game hours before the end, so the last ones are stored by then
constexpr int kDropCount = 20;
constexpr int kPixels = 14;            // pixels a cell is wide in the pictures

struct Stuck {
    uint64_t fingerprint = 0;
    int64_t samples = 0;
    int64_t worst = 0;
    JobType worstJob = JobType::None;
    CellPos worstCell;
};

uint64_t houseMix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ULL + 0x9E3779B97F4A7C15ULL; }

uint64_t houseFloatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

bool houseIsTree(const Sim& sim, DefId def) { return def.valid() && sim.defs().get(def).flag("plant.isTree"); }

float houseDistSq(CellPos a, CellPos b) {
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    return dx * dx + dy * dy;
}

// The footprint's origin: the spot near the landing with the fewest trees in it where all the ground is
// fit to build on and the cell outside the door can be reached from the landing, or false.
bool houseFindSite(Sim& sim, CellPos landing, CellPos& out) {
    bool found = false;
    float best = 0.0f;
    for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx) {
            const CellPos origin{landing.x + dx, landing.y + dy, 0};
            int trees = 0;
            bool ok = sim.map().walkable(CellPos{origin.x + kDoorX, origin.y - 1, 0}) &&
                      sim.regions().canReach(landing, CellPos{origin.x + kDoorX, origin.y - 1, 0});
            // One ring of margin: the stockpile drops and the walking round the house need ground.
            for (int y = origin.y - 2; y <= origin.y + kHeight + 1 && ok; ++y) {
                for (int x = origin.x - 3; x <= origin.x + kWidth + 2 && ok; ++x) {
                    const CellPos c{x, y, 0};
                    ok = sim.map().groundWalkable(c) && sim.map().buildingAt(c) == kNoEntity;
                    trees += ok && houseIsTree(sim, sim.plants().defAt(c)) ? 1 : 0;
                }
            }
            const float score = houseDistSq(origin, landing) + 400.0f * static_cast<float>(trees);
            if (ok && (!found || score < best)) {
                found = true;
                best = score;
                out = origin;
            }
        }
    }
    return found;
}

// The house seen from above on one level, cropped round it: ground (or the upper floor, or open air)
// under the buildings, walls dark, the door brown, stairs orange, beds blue, items yellow, colonists red.
bool houseWritePng(Sim& sim, const Snapshot& snapshot, CellPos origin, int level, const std::string& path) {
    const int x0 = origin.x - 4;
    const int y0 = origin.y - 4;
    const int cellsW = kWidth + 8;
    const int cellsH = kHeight + 8;
    const int w = cellsW * kPixels;
    const int h = cellsH * kPixels;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    const auto paint = [&](int cx, int cy, unsigned char r, unsigned char g, unsigned char b, int inset) {
        if (cx < 0 || cy < 0 || cx >= cellsW || cy >= cellsH) {
            return;
        }
        for (int py = inset; py < kPixels - inset; ++py) {
            for (int px = inset; px < kPixels - inset; ++px) {
                // North (+y) is up in the picture.
                const size_t i = (static_cast<size_t>((cellsH - 1 - cy) * kPixels + py) * static_cast<size_t>(w) +
                                  static_cast<size_t>(cx * kPixels + px)) * 3;
                pixels[i] = r;
                pixels[i + 1] = g;
                pixels[i + 2] = b;
            }
        }
    };
    for (int cy = 0; cy < cellsH; ++cy) {
        for (int cx = 0; cx < cellsW; ++cx) {
            const CellPos c{x0 + cx, y0 + cy, level};
            unsigned char r = 20;
            unsigned char g = 20;
            unsigned char b = 30;  // open air
            switch (sim.map().terrain(c)) {
                case Terrain::Air: break;
                case Terrain::UpperFloor: r = 205; g = 180; b = 130; break;
                case Terrain::Water: r = 60; g = 100; b = 170; break;
                case Terrain::ShallowWater: r = 90; g = 140; b = 195; break;
                case Terrain::Marsh: r = 70; g = 110; b = 80; break;
                case Terrain::Mud: r = 105; g = 85; b = 55; break;
                case Terrain::Sand: r = 215; g = 200; b = 150; break;
                case Terrain::Rock:
                case Terrain::RoughStone: r = g = b = 120; break;
                case Terrain::Soil: r = 100; g = 140; b = 70; break;
            }
            paint(cx, cy, r, g, b, 0);
            paint(cx, cy, static_cast<unsigned char>(r - 12), static_cast<unsigned char>(g - 12), static_cast<unsigned char>(b - 12), kPixels - 1);
        }
    }
    const auto cellsOf = [&](const ThingView& t, int& ox, int& oy) {
        ox = static_cast<int>(std::lround(t.x - static_cast<float>(t.sizeX) * 0.5f));
        oy = static_cast<int>(std::lround(t.y - static_cast<float>(t.sizeY) * 0.5f));
    };
    for (int pass = 0; pass < 3; ++pass) {  // buildings, then items, then colonists on top
        for (const ThingView& t : snapshot.things) {
            if (t.level != level) {
                continue;
            }
            if (pass == 0 && t.category == ThingCategory::Building) {
                const std::string& name = sim.defs().get(t.def).defName();
                if (name == "UpperFloor") {
                    continue;  // the floor is already the terrain once laid
                }
                unsigned char r = 70;
                unsigned char g = 70;
                unsigned char b = 75;
                if (name == "Door") { r = 130; g = 85; b = 40; }
                if (name == "Stairs" || name == "Ladder") { r = 230; g = 140; b = 40; }
                if (name == "Bed") { r = 70; g = 110; b = 200; }
                if (t.stage != ConstructionStage::Built) {  // a plan: a paler cell
                    r = static_cast<unsigned char>((r + 255) / 2);
                    g = static_cast<unsigned char>((g + 255) / 2);
                    b = static_cast<unsigned char>((b + 255) / 2);
                }
                int ox = 0;
                int oy = 0;
                cellsOf(t, ox, oy);
                for (int dy = 0; dy < t.sizeY; ++dy) {
                    for (int dx = 0; dx < t.sizeX; ++dx) {
                        paint(ox + dx - x0, oy + dy - y0, r, g, b, 1);
                    }
                }
            } else if (pass == 1 && t.category == ThingCategory::Item) {
                paint(static_cast<int>(std::floor(t.x)) - x0, static_cast<int>(std::floor(t.y)) - y0, 255, 220, 0, 4);
            } else if (pass == 2 && t.category == ThingCategory::Pawn) {
                paint(static_cast<int>(std::floor(t.x)) - x0, static_cast<int>(std::floor(t.y)) - y0, 230, 20, 20, 3);
            }
        }
    }
    // Stockpile cells: a thin pale-blue frame.
    for (const StockpileZone& zone : sim.zones().stockpiles()) {
        for (const CellPos& c : zone.cells) {
            if (c.level == level) {
                paint(c.x - x0, c.y - y0, 150, 190, 230, kPixels / 2 - 1);
            }
        }
    }
    return stbi_write_png(path.c_str(), w, h, 3, pixels.data(), w * 3) != 0;
}

}  // namespace

int run(uint64_t seed, int mapSize, double days, std::shared_ptr<const DefDatabase> defs, bool verbose,
        const std::string& pngBase) {
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
        std::fprintf(stderr, "house scenario: no landing site\n");
        return 2;
    }
    const CellPos landing = *sim.stockpileCell();
    CellPos origin;
    if (!houseFindSite(sim, landing, origin)) {
        std::fprintf(stderr, "house scenario: no room for the house near the landing site\n");
        return 2;
    }
    if (verbose) {
        std::fprintf(stderr, "landing %d,%d  house origin %d,%d\n", landing.x, landing.y, origin.x, origin.y);
    }
    const auto at = [&](int x, int y, int level) { return CellPos{origin.x + x, origin.y + y, level}; };
    const auto onRing = [](int x, int y) { return x == 0 || y == 0 || x == kWidth - 1 || y == kHeight - 1; };

    // Materials: Crashlanded drops 450 steel and 300 logs by the landing, which is not enough for two
    // storeys (walls 56 x 5 steel, beds 6 x 45, stairs 30; floor 63 x 4 logs, door 25), so more come
    // down beside the house. Plus meals for three days.
    const auto drop = [&](const char* def, int x, int y, int level, int count) {
        sim.submit(SpawnThingCommand{def, at(x, y, level), count});
    };
    for (int i = 0; i < 4; ++i) {
        drop("Steel", kWidth + 1, i, 0, 75);
        drop("Steel", kWidth + 2, i, 0, 75);
        drop("WoodLog", -2, i, 0, 75);
    }
    for (int i = 0; i < 4; ++i) {
        drop("MealSurvivalPack", -3, i, 0, 10);
    }

    // Ground floor first: the ring, the door and the stairs. The rest follows as each part stands.
    int wallsPlanned = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (!onRing(x, y)) {
                continue;
            }
            if (y == 0 && x == kDoorX) {
                sim.submit(PlaceBlueprintCommand{"Door", "WoodLog", at(x, y, 0), Rot4::North});
            } else {
                sim.submit(PlaceBlueprintCommand{"Wall", "Steel", at(x, y, 0), Rot4::North});
            }
            ++wallsPlanned;
        }
    }
    sim.submit(PlaceBlueprintCommand{"Stairs", "Steel", at(kStairsX, kStairsY, 0), Rot4::North});

    std::vector<Entity> colonists;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        colonists.push_back(pawn);
    }

    enum class Phase { Walls, Floor, Upstairs, Living } phase = Phase::Walls;
    int64_t wallsBuiltAt = -1;
    int64_t floorBuiltAt = -1;
    int64_t upstairsBuiltAt = -1;
    int64_t lastDropHour = -1;
    int drops = 0;
    bool bedsAssigned = false;
    bool edgesPlanned = false;
    ZoneId groundZone = kNoZone;
    ZoneId upperZone = kNoZone;

    std::map<Entity, Stuck> stuck;
    std::set<Entity> sleptUpstairs;  // colonists who woke from a bed upstairs
    uint64_t died = 0;
    uint64_t ate = 0;
    uint64_t sleptDownstairs = 0;  // woke up in a bed on the ground, after the beds were upstairs
    uint64_t sleptOnGround = 0;
    uint64_t commandsFailed = 0;
    uint64_t haulStarts = 0;
    uint64_t haulStepsUp = 0;    // steps spent climbing the stairs on a haul job, going up
    uint64_t haulStepsDown = 0;  // ... going down
    uint64_t buildStepsUp = 0;   // steps spent climbing on a construction job
    uint64_t climbStepsOther = 0;
    float minFood = 1.0f;
    float minRest = 1.0f;
    float maxMalnutrition = 0.0f;

    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    Snapshot snapshot;

    // What stands, counted from a snapshot: built and planned things by def name and level.
    struct Count {
        int built = 0;
        int sites = 0;
    };
    const auto tally = [&](std::map<std::pair<std::string, int>, Count>& out) {
        sim.writeSnapshot(snapshot);
        out.clear();
        for (const ThingView& t : snapshot.things) {
            if (t.category != ThingCategory::Building) {
                continue;
            }
            Count& c = out[{defs->get(t.def).defName(), t.level}];
            (t.stage == ConstructionStage::Built ? c.built : c.sites) += 1;
        }
    };
    std::map<std::pair<std::string, int>, Count> counts;
    const auto built = [&](const char* def, int level) {
        const auto it = counts.find({def, level});
        return it == counts.end() ? 0 : it->second.built;
    };

    int floorsLaid = 0;
    for (int64_t i = 0; i < steps; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            switch (e.type) {
                case EventType::PawnDied:
                    ++died;
                    std::fprintf(stderr, "day %lld: a colonist died: %s\n", static_cast<long long>(sim.time().day()), e.message.c_str());
                    break;
                case EventType::PawnAte: ++ate; break;
                case EventType::PawnWokeUp:
                    if (e.message == "bed" && e.target != kNoEntity && sim.things().registry().valid(e.target)) {
                        const Position* bed = sim.things().registry().try_get<Position>(e.target);
                        if (bed != nullptr && bed->level == 1) {
                            sleptUpstairs.insert(e.thing);
                        } else {
                            ++sleptDownstairs;
                        }
                    } else {
                        ++sleptOnGround;
                    }
                    break;
                case EventType::JobStarted: haulStarts += e.job == JobType::HaulToCell ? 1 : 0; break;
                case EventType::JobEnded:
                    if (e.message.rfind("failed", 0) == 0) {
                        std::fprintf(stderr, "step %lld: %s job failed for a colonist at %d,%d,%d: %s\n", static_cast<long long>(i),
                                     jobTypeName(e.job), e.cell.x, e.cell.y, e.cell.level, e.message.c_str());
                    }
                    break;
                case EventType::CommandFailed:
                    ++commandsFailed;
                    std::fprintf(stderr, "step %lld: command %llu failed: %s\n", static_cast<long long>(i),
                                 static_cast<unsigned long long>(e.command), e.message.c_str());
                    break;
                default: break;
            }
        }

        // Every step: who climbs the stairs, carrying what job.
        for (const auto [pawn, pos, worker, mover] : sim.things().registry().view<const Position, const Worker, const Mover>().each()) {
            (void)pawn;
            if (!mover.climbing(pos.level) || mover.climbed <= 0.0f) {
                continue;
            }
            const bool up = mover.path[mover.next].level > pos.level;
            switch (worker.job.type) {
                case JobType::HaulToCell: (up ? haulStepsUp : haulStepsDown) += 1; break;
                case JobType::DeliverResources:
                case JobType::BuildFrame: buildStepsUp += 1; break;
                default: climbStepsOther += 1; break;
            }
        }

        const int64_t now = sim.time().seconds;
        if (now % 60 != 0) {
            continue;
        }
        // The house is about building and hauling, not joy: with the wiki's recreation moods (-10 under
        // 15%, -20 at nothing) a colonist with no games would break down on day two, so keep the need up.
        for (const auto [pawn, joy] : sim.things().registry().view<Joy>().each()) {
            (void)pawn;
            joy.level = std::max(joy.level, 0.5f);
        }
        // Stuck: a pawn that has a job but nothing about it changes for a game hour.
        for (const auto [pawn, pos, worker, needs] : sim.things().registry().view<const Position, const Worker, const Needs>().each()) {
            Stuck& s = stuck[pawn];
            const CellPos cell = cellOf(pos);
            const Job& job = worker.job;
            uint64_t fp = houseMix(0, static_cast<uint64_t>(cell.x) << 20 | static_cast<uint64_t>(cell.y) << 2 | static_cast<uint64_t>(cell.level));
            fp = houseMix(fp, static_cast<uint64_t>(job.type));
            fp = houseMix(fp, job.step);
            fp = houseMix(fp, job.walk);
            fp = houseMix(fp, houseFloatBits(job.progress));
            fp = houseMix(fp, static_cast<uint64_t>(job.thing));
            if (job.type == JobType::BuildFrame && sim.things().registry().valid(job.thing)) {
                const ConstructionSite* site = sim.things().registry().try_get<ConstructionSite>(job.thing);
                fp = houseMix(fp, site != nullptr ? houseFloatBits(site->workDone) : 0);
            }
            const bool working = job.type != JobType::None && job.type != JobType::Wait && !jobIsLyingDown(job) &&
                                 !(job.type == JobType::Recreate && job.step > 0);
            if (working && fp == s.fingerprint) {
                if (++s.samples > s.worst) {
                    s.worst = s.samples;
                    s.worstJob = job.type;
                    s.worstCell = cell;
                }
            } else {
                s.samples = 0;
            }
            s.fingerprint = fp;
            minFood = std::min(minFood, needs.food);
            minRest = std::min(minRest, needs.rest);
            maxMalnutrition = std::max(maxMalnutrition, needs.malnutrition);
        }

        // The script, every game minute.
        tally(counts);
        switch (phase) {
            case Phase::Walls: {
                // Once the ring and the door stand, plan the upper floor over all of it.
                if (built("Wall", 0) + built("Door", 0) >= wallsPlanned) {
                    for (int y = 0; y < kHeight; ++y) {
                        for (int x = 0; x < kWidth; ++x) {
                            sim.submit(PlaceBlueprintCommand{"UpperFloor", "WoodLog", at(x, y, 1), Rot4::North});
                        }
                    }
                    wallsBuiltAt = now;
                    phase = Phase::Floor;
                }
                break;
            }
            case Phase::Floor: {
                floorsLaid = 0;
                for (int y = 0; y < kHeight; ++y) {
                    for (int x = 0; x < kWidth; ++x) {
                        floorsLaid += sim.map().upperFloor(at(x, y, 1)) ? 1 : 0;
                    }
                }
                // With the floor laid: the upper corner walls (an edge wall beside a corner would shut it in: the
                // outside of the upper ring is open air, so a corner can only be built from the two cells
                // that are walls by then), the beds, and a stockpile on each level that takes only one thing.
                if (floorsLaid == kWidth * kHeight) {
                    for (const int y : {0, kHeight - 1}) {
                        for (const int x : {0, kWidth - 1}) {
                            sim.submit(PlaceBlueprintCommand{"Wall", "Steel", at(x, y, 1), Rot4::North});
                        }
                    }
                    for (int b = 0; b < kBeds; ++b) {
                        sim.submit(PlaceBlueprintCommand{"Bed", "Steel", at(2 + b, 1, 1), Rot4::North});  // rows 1 and 2
                    }
                    // Zone ids count up from nextId() and are never reused, so the filters can go in the same step.
                    groundZone = sim.zones().nextId();
                    upperZone = groundZone + 1;
                    sim.submit(CreateStockpileCommand{CellRect{origin.x + 3, origin.y + 3, 4, 3, 0}});  // on the ground, inside
                    sim.submit(CreateStockpileCommand{CellRect{origin.x + 3, origin.y + 4, 4, 2, 1}});  // upstairs, by the beds
                    // Cloth goes up, blocks come down; nothing else is stored.
                    const DefId cloth = defs->idOf("Things", "Cloth");
                    const DefId blocks = defs->idOf("Things", "BlocksGranite");
                    for (const DefId id : defs->idsOfKind("Things")) {
                        if (id != blocks) {
                            sim.submit(SetStockpileAllowedCommand{groundZone, defs->get(id).defName(), false});
                        }
                        if (id != cloth) {
                            sim.submit(SetStockpileAllowedCommand{upperZone, defs->get(id).defName(), false});
                        }
                    }
                    floorBuiltAt = now;
                    phase = Phase::Upstairs;
                }
                break;
            }
            case Phase::Upstairs: {
                // The corners stand: now the edges between them.
                if (!edgesPlanned && built("Wall", 1) >= 4) {
                    for (int y = 0; y < kHeight; ++y) {
                        for (int x = 0; x < kWidth; ++x) {
                            const bool corner = (x == 0 || x == kWidth - 1) && (y == 0 || y == kHeight - 1);
                            if (onRing(x, y) && !corner) {
                                sim.submit(PlaceBlueprintCommand{"Wall", "Steel", at(x, y, 1), Rot4::North});
                            }
                        }
                    }
                    edgesPlanned = true;
                }
                // The beds are theirs once they stand.
                if (!bedsAssigned && built("Bed", 1) >= kBeds && colonists.size() >= static_cast<size_t>(kBeds)) {
                    std::vector<Entity> beds;
                    for (const ThingView& t : snapshot.things) {
                        if (t.category == ThingCategory::Building && t.stage == ConstructionStage::Built && t.level == 1 &&
                            defs->get(t.def).defName() == "Bed") {
                            beds.push_back(t.id);
                        }
                    }
                    for (size_t b = 0; b < beds.size() && b < colonists.size(); ++b) {
                        sim.submit(AssignBedCommand{beds[b], colonists[b]});
                    }
                    bedsAssigned = true;
                }
                if (bedsAssigned && edgesPlanned && built("Stairs", 0) >= 1 && built("Wall", 1) >= wallsPlanned) {
                    upstairsBuiltAt = now;
                    phase = Phase::Living;
                }
                break;
            }
            case Phase::Living: {
                // Every few game hours: cloth outside the door, to go up; blocks on the upper floor, to come down.
                const int64_t hour = now / kSecondsPerHour;
                const int64_t endHour = steps / kSecondsPerHour;
                if (now % kSecondsPerHour == 0 && hour != lastDropHour && hour % kDropHours == 0 && hour < endHour - kDropEndHours) {
                    lastDropHour = hour;
                    sim.submit(SpawnThingCommand{"Cloth", at(kDoorX + 3, -2, 0), kDropCount});
                    sim.submit(SpawnThingCommand{"BlocksGranite", at(6, 3, 1), kDropCount});
                    ++drops;
                }
                break;
            }
        }
        if (verbose && now % kSecondsPerHour == 0) {
            std::fprintf(stderr, "day %lld %02d:00  phase %d  walls %d/%d  floors %d/%d  upper walls %d  beds %d  hauls %llu (up %llu, down %llu)  woke upstairs %zu\n",
                         static_cast<long long>(sim.time().day()), sim.time().hourOfDay(), static_cast<int>(phase),
                         built("Wall", 0) + built("Door", 0), wallsPlanned, floorsLaid, kWidth * kHeight, built("Wall", 1),
                         built("Bed", 1), static_cast<unsigned long long>(haulStarts), static_cast<unsigned long long>(haulStepsUp),
                         static_cast<unsigned long long>(haulStepsDown), sleptUpstairs.size());
        }
    }

    // What stands at the end.
    tally(counts);
    int unfinished = 0;
    for (const auto& [key, c] : counts) {
        (void)key;
        unfinished += c.sites;
    }
    for (const ThingView& t : snapshot.things) {
        if (t.category == ThingCategory::Building && t.stage != ConstructionStage::Built) {
            std::fprintf(stderr, "unfinished: %s at %.1f,%.1f level %d, %s, materials %d/%d, progress %.2f\n", defs->get(t.def).defName().c_str(),
                         static_cast<double>(t.x), static_cast<double>(t.y), t.level, t.stage == ConstructionStage::Frame ? "frame" : "blueprint",
                         t.materialsHave, t.materialsNeeded, static_cast<double>(t.buildProgress));
        }
    }
    int floorCells = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            floorCells += sim.map().upperFloor(at(x, y, 1)) ? 1 : 0;
        }
    }
    int upstairsRooms = 0;
    {
        const Room* room = sim.rooms().roomOf(at(4, 4, 1));
        if (room != nullptr && room->enclosed() && !room->outdoors() && room->beds == kBeds) {
            upstairsRooms = 1;
        }
    }
    const Room* groundRoom = sim.rooms().roomOf(at(4, 3, 0));
    const bool groundIndoors = groundRoom != nullptr && groundRoom->enclosed() && !groundRoom->outdoors();

    // Where the hauled things lie: cloth should all be in the upstairs stockpile, blocks in the ground one.
    const DefId clothDef = defs->idOf("Things", "Cloth");
    const DefId blocksDef = defs->idOf("Things", "BlocksGranite");
    int clothUp = 0;
    int clothElsewhere = 0;
    int blocksDown = 0;
    int blocksElsewhere = 0;
    for (const auto [item, thing, pos, stack] : sim.things().registry().view<const Thing, const Position, const StackCount>().each()) {
        (void)item;
        if (thing.category != ThingCategory::Item || (thing.def != clothDef && thing.def != blocksDef)) {
            continue;
        }
        const ZoneId zone = sim.zones().zoneAt(cellOf(pos));
        if (thing.def == clothDef) {
            (zone == upperZone && zone != kNoZone ? clothUp : clothElsewhere) += stack.count;
        } else {
            (zone == groundZone && zone != kNoZone ? blocksDown : blocksElsewhere) += stack.count;
        }
    }

    int64_t worstStuck = 0;
    const Stuck* worst = nullptr;
    for (const auto& [pawn, s] : stuck) {
        (void)pawn;
        if (s.worst > worstStuck) {
            worstStuck = s.worst;
            worst = &s;
        }
    }

    // The pictures: one per level, the house from above.
    if (!pngBase.empty()) {
        for (int level = 0; level <= 1; ++level) {
            const std::string path = pngBase + "-level" + std::to_string(level) + ".png";
            if (!houseWritePng(sim, snapshot, origin, level, path)) {
                std::fprintf(stderr, "could not write %s\n", path.c_str());
                return 2;
            }
        }
    }

    std::printf("{\"house\":{\"seed\":%llu,\"size\":%d,\"days\":%.2f,\"colonists\":%zu,\"died\":%llu,"
                "\"minFood\":%.3f,\"maxMalnutrition\":%.3f,\"minRest\":%.3f,\"stuckMinutesWorst\":%lld,"
                "\"groundWalls\":%d,\"groundDoors\":%d,\"stairs\":%d,\"upperFloors\":%d,\"upperWalls\":%d,\"upperBeds\":%d,\"unfinished\":%d,"
                "\"wallsBuiltAtSecond\":%lld,\"floorBuiltAtSecond\":%lld,\"houseDoneAtSecond\":%lld,"
                "\"upstairsRoomOk\":%s,\"groundRoomIndoors\":%s,\"sleptUpstairs\":%zu,\"sleptDownstairs\":%llu,\"sleptOnGround\":%llu,\"ate\":%llu,"
                "\"drops\":%d,\"haulJobs\":%llu,\"haulClimbStepsUp\":%llu,\"haulClimbStepsDown\":%llu,\"buildClimbSteps\":%llu,\"otherClimbSteps\":%llu,"
                "\"clothUpstairs\":%d,\"clothElsewhere\":%d,\"blocksDownstairs\":%d,\"blocksElsewhere\":%d,"
                "\"commandsFailed\":%llu,\"failedJobs\":%llu,\"stateHash\":\"%016llx\"}}\n",
                static_cast<unsigned long long>(seed), mapSize, days, colonists.size(), static_cast<unsigned long long>(died),
                static_cast<double>(minFood), static_cast<double>(maxMalnutrition), static_cast<double>(minRest),
                static_cast<long long>(worstStuck), built("Wall", 0), built("Door", 0), built("Stairs", 0), floorCells,
                built("Wall", 1), built("Bed", 1), unfinished, static_cast<long long>(wallsBuiltAt), static_cast<long long>(floorBuiltAt),
                static_cast<long long>(upstairsBuiltAt), upstairsRooms == 1 ? "true" : "false", groundIndoors ? "true" : "false",
                sleptUpstairs.size(), static_cast<unsigned long long>(sleptDownstairs), static_cast<unsigned long long>(sleptOnGround),
                static_cast<unsigned long long>(ate), drops, static_cast<unsigned long long>(haulStarts),
                static_cast<unsigned long long>(haulStepsUp), static_cast<unsigned long long>(haulStepsDown),
                static_cast<unsigned long long>(buildStepsUp), static_cast<unsigned long long>(climbStepsOther), clothUp, clothElsewhere,
                blocksDown, blocksElsewhere, static_cast<unsigned long long>(commandsFailed),
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
        fail("a colonist was stuck for %lld game minutes (job %s at cell %d,%d,%d)", static_cast<long long>(worstStuck),
             jobTypeName(worst->worstJob), worst->worstCell.x, worst->worstCell.y, worst->worstCell.level);
    }
    if (unfinished > 0 || built("Wall", 0) != wallsPlanned - 1 || built("Door", 0) != 1 || built("Stairs", 0) != 1 ||
        floorCells != kWidth * kHeight || built("Wall", 1) != wallsPlanned || built("Bed", 1) != kBeds) {
        fail("the house is not finished (ground walls %d/%d, door %d/1, stairs %d/1, floor cells %d/%d, upper walls %d/%d, upper beds %d/%d, %d sites left)",
             built("Wall", 0), wallsPlanned - 1, built("Door", 0), built("Stairs", 0), floorCells, kWidth * kHeight, built("Wall", 1),
             wallsPlanned, built("Bed", 1), kBeds, unfinished);
    }
    if (upstairsRooms != 1) {
        fail("%s", "upstairs is not an enclosed, roofed room with six beds");
    }
    if (!groundIndoors) {
        fail("%s", "the ground floor is not an enclosed, roofed room");
    }
    if (sleptUpstairs.size() != static_cast<size_t>(kBeds)) {
        fail("%zu of %d colonists woke up from a bed upstairs", sleptUpstairs.size(), kBeds);
    }
    if (sleptDownstairs > 0) {
        fail("%llu times a colonist woke in a bed downstairs", static_cast<unsigned long long>(sleptDownstairs));
    }
    if (ate == 0) {
        fail("%s", "nobody ate");
    }
    if (drops < 3) {
        fail("only %d drops of things to haul", drops);
    }
    if (haulStepsUp == 0 || haulStepsDown == 0) {
        fail("hauling did not climb both ways (steps up %llu, down %llu)", static_cast<unsigned long long>(haulStepsUp),
             static_cast<unsigned long long>(haulStepsDown));
    }
    if (buildStepsUp == 0) {
        fail("%s", "nobody climbed the stairs to build");
    }
    if (clothElsewhere > 0 || clothUp != drops * kDropCount) {
        fail("cloth: %d in the upstairs stockpile, %d elsewhere, %d dropped", clothUp, clothElsewhere, drops * kDropCount);
    }
    if (blocksElsewhere > 0 || blocksDown != drops * kDropCount) {
        fail("blocks: %d in the ground stockpile, %d elsewhere, %d dropped", blocksDown, blocksElsewhere, drops * kDropCount);
    }
    if (commandsFailed > 0 || sim.stats().failedJobs > 0) {
        fail("%llu commands and %llu jobs failed", static_cast<unsigned long long>(commandsFailed),
             static_cast<unsigned long long>(sim.stats().failedJobs));
    }
    return failures > 0 ? 1 : 0;
}

}  // namespace housescenario

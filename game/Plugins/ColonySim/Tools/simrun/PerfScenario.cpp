#include "PerfScenario.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace perfscenario {

namespace {

using namespace colony;

constexpr int kTameAnimals = 300;
constexpr int kRoomWidth = 12;
constexpr int kRoomHeight = 8;
constexpr int kBeds = 8;
constexpr int kSearchRadius = 60;
constexpr int kMarkRadius = 35;

// The origin of a width by height patch of bare, buildable ground (with a one cell margin) in the ring
// round `landing` that no earlier site took, nearest first, or false.
bool perfFindSite(const Sim& sim, CellPos landing, int width, int height, const std::vector<CellRect>& taken, CellPos& out) {
    bool found = false;
    int best = 0;
    for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx) {
            const int dist = dx * dx + dy * dy;
            if (dist < 100 || (found && dist >= best)) {
                continue;  // not on the landing itself, and no better than what we have
            }
            const CellPos origin{landing.x + dx, landing.y + dy, 0};
            bool ok = true;
            for (int y = origin.y - 1; y <= origin.y + height && ok; ++y) {
                for (int x = origin.x - 1; x <= origin.x + width && ok; ++x) {
                    const CellPos c{x, y, 0};
                    ok = sim.map().inBounds(c) && sim.map().groundWalkable(c) && sim.map().buildingAt(c) == kNoEntity &&
                         std::none_of(taken.begin(), taken.end(), [&](const CellRect& r) {
                             return x >= r.minX - 2 && x < r.minX + r.sizeX + 2 && y >= r.minY - 2 && y < r.minY + r.sizeY + 2;
                         });
                }
            }
            if (ok) {
                found = true;
                best = dist;
                out = origin;
            }
        }
    }
    return found;
}

}  // namespace

int run(uint64_t seed, int mapSize, double days, double budgetMs, std::shared_ptr<const DefDatabase> defs) {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };

    SimSettings settings{seed, mapSize, 0, 0};
    settings.scenario = "PerfColony";
    settings.wildAnimals = 100;
    const auto start = Clock::now();
    Sim sim(settings, defs);
    const auto generated = Clock::now();
    if (!sim.setupErrors().empty() || !sim.stockpileCell()) {
        for (const std::string& e : sim.setupErrors()) {
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        std::fprintf(stderr, "perf: the sim could not be set up (does the data folder have the PerfColony scenario?)\n");
        return 2;
    }
    const CellPos landing = *sim.stockpileCell();

    // Two walled rooms, each with a door in the south wall, beds and a stove: room temperature and
    // the construction work for the colonists.
    std::vector<CellRect> taken;
    for (int room = 0; room < 2; ++room) {
        CellPos origin;
        if (!perfFindSite(sim, landing, kRoomWidth, kRoomHeight, taken, origin)) {
            std::fprintf(stderr, "perf: no room for building %d near the landing site\n", room + 1);
            return 2;
        }
        taken.push_back(CellRect{origin.x, origin.y, kRoomWidth, kRoomHeight, 0});
        for (int x = 0; x < kRoomWidth; ++x) {
            for (const int y : {0, kRoomHeight - 1}) {
                const bool door = y == 0 && x == 2;
                sim.submit(PlaceBlueprintCommand{door ? "Door" : "Wall", door ? "WoodLog" : "Steel", CellPos{origin.x + x, origin.y + y, 0}});
            }
        }
        for (int y = 1; y < kRoomHeight - 1; ++y) {
            sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{origin.x, origin.y + y, 0}});
            sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{origin.x + kRoomWidth - 1, origin.y + y, 0}});
        }
        for (int bed = 0; bed < kBeds; ++bed) {
            sim.submit(PlaceBlueprintCommand{"Bed", "Steel", CellPos{origin.x + 1 + bed, origin.y + kRoomHeight - 3, 0}});
        }
        sim.submit(PlaceBlueprintCommand{"FueledStove", "", CellPos{origin.x + 5, origin.y + 1, 0}});
        sim.submit(PlaceFloorCommand{CellRect{origin.x + 1, origin.y + 1, kRoomWidth - 2, kRoomHeight - 2, 0}, "WoodFloor"});
    }

    // A growing zone of rice, and everything round the landing to mine, fell and pick.
    CellPos farm;
    if (perfFindSite(sim, landing, 24, 24, taken, farm)) {
        sim.submit(CreateGrowingZoneCommand{CellRect{farm.x, farm.y, 24, 24, 0}, "Plant_Rice"});
        taken.push_back(CellRect{farm.x, farm.y, 24, 24, 0});
    }
    const CellRect marks{landing.x - kMarkRadius, landing.y - kMarkRadius, 2 * kMarkRadius, 2 * kMarkRadius, 0};
    bool rock = false;  // a Mine mark over no rock fails
    for (int y = marks.minY; y < marks.minY + marks.sizeY && !rock; ++y) {
        for (int x = marks.minX; x < marks.minX + marks.sizeX && !rock; ++x) {
            rock = sim.map().inBounds(CellPos{x, y, 0}) && sim.map().terrain(CellPos{x, y, 0}) == Terrain::Rock;
        }
    }
    if (rock) {
        sim.submit(DesignateCommand{marks, DesignationKind::Mine});
    }
    sim.submit(DesignateCommand{marks, DesignationKind::CutPlant});
    sim.submit(DesignateCommand{marks, DesignationKind::HarvestPlant});

    // 300 tame, grown animals on a grid in a patch of open ground, fed every game hour.
    CellPos pen;
    const int side = 20;  // 20 by 20 cells, one animal to every cell and a bit
    if (!perfFindSite(sim, landing, side, side, taken, pen)) {
        std::fprintf(stderr, "perf: no room for the animals near the landing site\n");
        return 2;
    }
    static const char* const kKinds[] = {"Chicken", "Chicken", "Cow", "Muffalo", "Alpaca"};
    int penned = 0;
    for (int i = 0; i < kTameAnimals; ++i) {
        const CellPos cell{pen.x + i % side, pen.y + i / side, 0};
        const Entity e = sim.things().spawn(kKinds[i % 5], cell);
        if (e == kNoEntity) {
            continue;
        }
        Animal& a = sim.things().registry().get<Animal>(e);
        a.wild = false;
        a.bornAt = -400 * kSecondsPerDay;
        sim.things().registry().emplace_or_replace<Faction>(e, Faction{kPlayerFaction});
        AnimalCare* care = sim.husbandry().careFor(e);
        care->hasHome = true;
        care->home = cell;
        care->tamedAt = 0;
        ++penned;
    }

    int colonists = 0;
    int wild = 0;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        (void)pawn;
        (void)colonist;
        ++colonists;
    }
    for (const auto [pawn, animal] : sim.things().registry().view<const Animal>().each()) {
        (void)pawn;
        wild += animal.wild ? 1 : 0;
    }

    StepProfile profile;
    sim.setProfile(&profile);
    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    std::vector<float> stepMs(static_cast<size_t>(steps));
    uint64_t commandsFailed = 0;
    float worstMs = 0.0f;
    int64_t worstStep = -1;
    const char* worstSystem = "?";
    const auto run0 = Clock::now();
    for (int64_t i = 0; i < steps; ++i) {
        if (sim.time().seconds % kSecondsPerHour == 0) {
            std::vector<Entity> animals(sim.things().registry().view<const Animal>().begin(), sim.things().registry().view<const Animal>().end());
            for (const Entity e : animals) {
                if (!sim.things().registry().get<Animal>(e).wild) {
                    sim.animals().addFood(e, 50.0f);
                }
            }
        }
        const std::vector<double> lapsBefore = profile.ms;
        const auto before = Clock::now();
        sim.step();
        stepMs[static_cast<size_t>(i)] = static_cast<float>(ms(before, Clock::now()));
        if (stepMs[static_cast<size_t>(i)] > worstMs) {
            // Which system took the time in the dearest step so far.
            worstMs = stepMs[static_cast<size_t>(i)];
            worstStep = i;
            worstSystem = "?";
            double most = 0.0;
            for (size_t k = 0; k < profile.names.size(); ++k) {
                const double spent = profile.ms[k] - (k < lapsBefore.size() ? lapsBefore[k] : 0.0);
                if (std::string(profile.names[k]) != "total" && spent > most) {
                    most = spent;
                    worstSystem = profile.names[k];
                }
            }
        }
        for (const Event& e : sim.events()) {
            if (e.type == EventType::CommandFailed) {
                ++commandsFailed;
                std::fprintf(stderr, "step %lld: command %llu failed: %s\n", static_cast<long long>(i),
                             static_cast<unsigned long long>(e.command), e.message.c_str());
            }
        }
    }
    const double runMs = ms(run0, Clock::now());
    sim.setProfile(nullptr);

    double total = 0.0;
    for (const float v : stepMs) {
        total += v;
    }
    const double avg = steps > 0 ? total / static_cast<double>(steps) : 0.0;
    std::vector<float> sorted = stepMs;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        return sorted.empty() ? 0.0 : static_cast<double>(sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]);
    };
    // At 3x the sim runs 90 steps per real second; at 60 fps that is 1.5 steps per frame.
    const double msPerFrameAt3x = avg * 3.0 * kStepsPerRealSecondAt1x / 60.0;

    // The table: systems by cost, with their share of the whole step.
    std::vector<std::pair<double, const char*>> rows;
    for (size_t i = 0; i < profile.names.size(); ++i) {
        if (std::string(profile.names[i]) != "total") {
            rows.emplace_back(profile.ms[i], profile.names[i]);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "%-12s %10s %7s\n", "system", "ms/step", "share");
    for (const auto& [cost, name] : rows) {
        std::fprintf(stderr, "%-12s %10.4f %6.1f%%\n", name, cost / static_cast<double>(steps), total > 0.0 ? 100.0 * cost / total : 0.0);
    }

    std::printf("{\"perf\":{\"seed\":%llu,\"size\":%d,\"days\":%.2f,\"colonists\":%d,\"tameAnimals\":%d,\"wildAnimals\":%d,"
                "\"things\":%zu,\"mapGenMs\":%.1f,\"runMs\":%.1f,\"avgStepMs\":%.4f,\"p99StepMs\":%.3f,\"maxStepMs\":%.3f,"
                "\"msPerFrameAt3x\":%.3f,\"worstStep\":%lld,\"worstSystem\":\"%s\",\"commandsFailed\":%llu,\"stateHash\":\"%016llx\",\"systems\":{",
                static_cast<unsigned long long>(seed), mapSize, days, colonists, penned, wild, sim.things().count(),
                ms(start, generated), runMs, avg, percentile(0.99), percentile(1.0), msPerFrameAt3x,
                static_cast<long long>(worstStep), worstSystem, static_cast<unsigned long long>(commandsFailed),
                static_cast<unsigned long long>(sim.stateHash()));
    for (size_t i = 0; i < rows.size(); ++i) {
        std::printf("%s\"%s\":%.4f", i == 0 ? "" : ",", rows[i].second, rows[i].first / static_cast<double>(steps));
    }
    std::printf("}}}\n");

    if (budgetMs > 0.0 && avg > budgetMs) {
        std::fprintf(stderr, "perf: the average step costs %.4f ms, over the budget of %.4f ms\n", avg, budgetMs);
        return 1;
    }
    return 0;
}

}  // namespace perfscenario

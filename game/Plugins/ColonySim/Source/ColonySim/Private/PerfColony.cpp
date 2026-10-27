#include "ColonySim/PerfColony.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <vector>

namespace colony {

namespace {

constexpr int kPerfTameAnimals = 300;
constexpr int kPerfRoomWidth = 12;
constexpr int kPerfRoomHeight = 8;
constexpr int kPerfBeds = 8;
constexpr int kPerfSearchRadius = 60;
constexpr int kPerfMarkRadius = 35;

// The origin of a width by height patch of bare, buildable ground (with a one cell margin) in the ring
// round `landing` that no earlier site took, nearest first, or false.
bool perfFindSite(const Sim& sim, CellPos landing, int width, int height, const std::vector<CellRect>& taken, CellPos& out) {
    bool found = false;
    int best = 0;
    for (int dy = -kPerfSearchRadius; dy <= kPerfSearchRadius; ++dy) {
        for (int dx = -kPerfSearchRadius; dx <= kPerfSearchRadius; ++dx) {
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

std::string setUpPerfColony(Sim& sim, PerfColonyCounts& counts) {
    counts = PerfColonyCounts{};
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        (void)pawn;
        (void)colonist;
        ++counts.colonists;
    }
    if (!sim.setupErrors().empty() || !sim.stockpileCell() || counts.colonists == 0) {
        return "the sim could not be set up (does the data folder have the PerfColony scenario?)";
    }
    const CellPos landing = *sim.stockpileCell();

    // Two walled rooms, each with a door in the south wall, beds and a stove: room temperature and
    // the construction work for the colonists.
    std::vector<CellRect> taken;
    for (int room = 0; room < 2; ++room) {
        CellPos origin;
        if (!perfFindSite(sim, landing, kPerfRoomWidth, kPerfRoomHeight, taken, origin)) {
            return "no room for building " + std::to_string(room + 1) + " near the landing site";
        }
        taken.push_back(CellRect{origin.x, origin.y, kPerfRoomWidth, kPerfRoomHeight, 0});
        for (int x = 0; x < kPerfRoomWidth; ++x) {
            for (const int y : {0, kPerfRoomHeight - 1}) {
                const bool door = y == 0 && x == 2;
                sim.submit(PlaceBlueprintCommand{door ? "Door" : "Wall", door ? "WoodLog" : "Steel", CellPos{origin.x + x, origin.y + y, 0}});
            }
        }
        for (int y = 1; y < kPerfRoomHeight - 1; ++y) {
            sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{origin.x, origin.y + y, 0}});
            sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{origin.x + kPerfRoomWidth - 1, origin.y + y, 0}});
        }
        for (int bed = 0; bed < kPerfBeds; ++bed) {
            sim.submit(PlaceBlueprintCommand{"Bed", "Steel", CellPos{origin.x + 1 + bed, origin.y + kPerfRoomHeight - 3, 0}});
        }
        sim.submit(PlaceBlueprintCommand{"FueledStove", "", CellPos{origin.x + 5, origin.y + 1, 0}});
        sim.submit(PlaceFloorCommand{CellRect{origin.x + 1, origin.y + 1, kPerfRoomWidth - 2, kPerfRoomHeight - 2, 0}, "WoodFloor"});
    }

    // A growing zone of rice, and everything round the landing to mine, fell and pick.
    CellPos farm;
    if (perfFindSite(sim, landing, 24, 24, taken, farm)) {
        sim.submit(CreateGrowingZoneCommand{CellRect{farm.x, farm.y, 24, 24, 0}, "Plant_Rice"});
        taken.push_back(CellRect{farm.x, farm.y, 24, 24, 0});
    }
    const CellRect marks{landing.x - kPerfMarkRadius, landing.y - kPerfMarkRadius, 2 * kPerfMarkRadius, 2 * kPerfMarkRadius, 0};
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
        return "no room for the animals near the landing site";
    }
    static const char* const kKinds[] = {"Chicken", "Chicken", "Cow", "Muffalo", "Alpaca"};
    for (int i = 0; i < kPerfTameAnimals; ++i) {
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
        ++counts.tameAnimals;
    }

    for (const auto [pawn, animal] : sim.things().registry().view<const Animal>().each()) {
        (void)pawn;
        counts.wildAnimals += animal.wild ? 1 : 0;
    }
    return {};
}

void feedPerfColonyAnimals(Sim& sim) {
    std::vector<Entity> animals(sim.things().registry().view<const Animal>().begin(), sim.things().registry().view<const Animal>().end());
    for (const Entity e : animals) {
        if (!sim.things().registry().get<Animal>(e).wild) {
            sim.animals().addFood(e, 50.0f);
        }
    }
}

int armPerfColonists(Sim& sim) {
    const DefId rifle = sim.defs().idOf("Things", "Gun_AssaultRifle");
    std::vector<Entity> colonists(sim.things().registry().view<const Colonist>().begin(), sim.things().registry().view<const Colonist>().end());
    int armed = 0;
    for (const Entity pawn : colonists) {
        armed += sim.combat().equip(pawn, sim.things().make(rifle)) ? 1 : 0;
    }
    return armed;
}

}  // namespace colony

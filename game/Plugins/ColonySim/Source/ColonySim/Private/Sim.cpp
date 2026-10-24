#include "ColonySim/Sim.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Things.h"

#include <cmath>
#include <cstdlib>

namespace colony {

// Stage 0 hauling placeholder: a pawn that only hauls items to one stockpile cell. The job
// system (task C1) replaces these components and the code that uses them.
enum class HaulStage : uint8_t { Idle, GoToItem, CarryToStockpile };

struct HaulJob {
    HaulStage stage = HaulStage::Idle;
    Entity item = kNoEntity;
};

// On an item a pawn has claimed, so nobody else goes for it.
struct HaulReservedBy {
    Entity pawn = kNoEntity;
};

// On an item while a pawn carries it (the item is off the map).
struct HaulCarriedBy {
    Entity pawn = kNoEntity;
};

// On an item that has reached the stockpile.
struct HaulStored {};

namespace {

// Moves the pawn along its path by one step's worth of walking. Returns true
// once the end of the path is reached.
bool haulAdvance(Position& pos, Mover& mover) {
    float budget = mover.metresPerStep;
    while (budget > 0.0f && mover.next < mover.path.size()) {
        const CellPos c = mover.path[mover.next];
        const float tx = static_cast<float>(c.x) + 0.5f;
        const float ty = static_cast<float>(c.y) + 0.5f;
        const float dx = tx - pos.x;
        const float dy = ty - pos.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= budget) {
            pos.x = tx;
            pos.y = ty;
            pos.level = c.level;
            budget -= dist;
            ++mover.next;
        } else {
            pos.x += dx / dist * budget;
            pos.y += dy / dist * budget;
            budget = 0.0f;
        }
    }
    return mover.next >= mover.path.size();
}

const DefDatabase& simEmptyDefs() {
    static const DefDatabase empty;
    return empty;
}

}  // namespace

Sim::Sim(const SimSettings& settings) : Sim(settings, nullptr) {}

Sim::Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs)
    : settings_(settings),
      // No defs: share a static empty database, owned by nobody.
      defs_(defs ? std::move(defs) : std::shared_ptr<const DefDatabase>(std::shared_ptr<const DefDatabase>(), &simEmptyDefs())),
      map_(MapSettings{settings.mapSize, settings.mapSize, splitMix64(settings.seed ^ 0x6D6170ULL), settings.flatMap}),
      pathfinder_(map_),
      things_(std::make_unique<Things>(map_, defs_)),
      time_(),
      rng_(splitMix64(settings.seed ^ 0x7061776EULL)) {
    placeStockpileAndThings();
}

Sim::~Sim() = default;

void Sim::placeStockpileAndThings() {
    // Stockpile: the walkable cell nearest the middle, searched in growing squares.
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
    if (settings_.pawns > 0 && !pawnDef.valid()) {
        setupErrors_.push_back("hauling test: no Things def '" + settings_.pawnDef + "'");
    }
    if (settings_.items > 0 && !itemDef.valid()) {
        setupErrors_.push_back("hauling test: no Things def '" + settings_.haulItemDef + "'");
    }
    if (!setupErrors_.empty()) {
        return;
    }

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

    for (int i = 0; i < settings_.items; ++i) {
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
        things_->registry().emplace<HaulJob>(pawn);
        haulers_.push_back(pawn);
    }
}

int Sim::itemsLeft() const {
    int left = 0;
    things_->registry().view<const Thing>(entt::exclude<HaulStored>).each([&left](const Thing& thing) {
        left += thing.category == ThingCategory::Item ? 1 : 0;
    });
    return left;
}

void Sim::startNextHaul(Entity pawn) {
    // Nearest free item in a straight line, ties to the lowest entity id. A linear scan is fine
    // for the Stage 0 test; the real job system keeps ready-made haul lists per chunk.
    entt::registry& reg = things_->registry();
    const Position& pos = reg.get<Position>(pawn);
    Entity best = kNoEntity;
    float bestDist = 0.0f;
    for (const auto [item, thing, itemPos] :
         reg.view<const Thing, const Position>(entt::exclude<HaulStored, HaulReservedBy>).each()) {
        if (thing.category != ThingCategory::Item || itemPos.level != pos.level) {
            continue;
        }
        const float dx = itemPos.x - pos.x;
        const float dy = itemPos.y - pos.y;
        const float dist = dx * dx + dy * dy;
        if (best == kNoEntity || dist < bestDist || (dist == bestDist && item < best)) {
            best = item;
            bestDist = dist;
        }
    }
    if (best == kNoEntity) {
        return;
    }
    reg.emplace<HaulReservedBy>(best, HaulReservedBy{pawn});
    Mover& mover = reg.get<Mover>(pawn);
    mover.next = 0;
    if (!pathfinder_.findPath(cellOf(pos), cellOf(reg.get<Position>(best)), mover.path)) {
        ++stats_.failedJobs;  // item stays reserved so nobody retries it
        return;
    }
    HaulJob& job = reg.get<HaulJob>(pawn);
    job.item = best;
    job.stage = HaulStage::GoToItem;
}

void Sim::updateHauler(Entity pawn) {
    entt::registry& reg = things_->registry();
    HaulJob& job = reg.get<HaulJob>(pawn);
    switch (job.stage) {
        case HaulStage::Idle:
            startNextHaul(pawn);
            break;
        case HaulStage::GoToItem: {
            Position& pos = reg.get<Position>(pawn);
            Mover& mover = reg.get<Mover>(pawn);
            if (haulAdvance(pos, mover)) {
                mover.next = 0;
                if (pathfinder_.findPath(cellOf(pos), *stockpileCell_, mover.path)) {
                    things_->despawn(job.item);
                    reg.emplace<HaulCarriedBy>(job.item, HaulCarriedBy{pawn});
                    job.stage = HaulStage::CarryToStockpile;
                } else {
                    ++stats_.failedJobs;  // item stays where it is, reserved
                    job.stage = HaulStage::Idle;
                    job.item = kNoEntity;
                }
            }
            break;
        }
        case HaulStage::CarryToStockpile:
            if (haulAdvance(reg.get<Position>(pawn), reg.get<Mover>(pawn))) {
                reg.remove<HaulCarriedBy, HaulReservedBy>(job.item);
                things_->spawnAt(job.item, *stockpileCell_);
                reg.emplace<HaulStored>(job.item);
                ++stats_.hauls;
                job.stage = HaulStage::Idle;
                job.item = kNoEntity;
            }
            break;
    }
}

void Sim::step() {
    for (const Entity pawn : haulers_) {
        updateHauler(pawn);
    }
    time_.seconds += 1;
}

void Sim::stepMany(int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        step();
    }
}

}  // namespace colony

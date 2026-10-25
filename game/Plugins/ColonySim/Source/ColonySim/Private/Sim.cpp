#include "ColonySim/Sim.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "HaulJob.h"
#include "MovementSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace colony {

namespace {

// Starts the pawn walking the path the pathfinder found: straightened where the ground allows.
void simStartWalk(const Map& map, const Position& pos, Mover& mover) {
    std::vector<CellPos> route = std::move(mover.path);
    smoothPath(map, pos.x, pos.y, route);
    mover.walkTo(pos.x, pos.y, std::move(route));
}

const DefDatabase& simEmptyDefs() {
    static const DefDatabase empty;
    return empty;
}

std::string simCellText(CellPos c) {
    return "(" + std::to_string(c.x) + ", " + std::to_string(c.y) + ", level " + std::to_string(c.level) + ")";
}

void simHashCell(StateHasher& h, CellPos c) {
    h.add(c.x);
    h.add(c.y);
    h.add(c.level);
}

}  // namespace

Sim::Sim(const SimSettings& settings) : Sim(settings, nullptr) {}

Sim::Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs)
    : Sim(settings, std::move(defs), EmptyStart{}) {
    placeStockpileAndThings();
}

Sim::Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs, EmptyStart)
    : settings_(settings),
      // No defs: share a static empty database, owned by nobody.
      defs_(defs ? std::move(defs) : std::shared_ptr<const DefDatabase>(std::shared_ptr<const DefDatabase>(), &simEmptyDefs())),
      map_(MapSettings{settings.mapSize, settings.mapSize, splitMix64(settings.seed ^ 0x6D6170ULL), settings.flatMap}),
      pathfinder_(map_),
      things_(std::make_unique<Things>(map_, defs_)),
      movement_(std::make_unique<MovementSystem>()),
      time_(),
      rng_(splitMix64(settings.seed ^ 0x7061776EULL)),
      groundHash_(map_.contentHash()) {
    things_->setEventLog(&events_);
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
    HaulJob& job = reg.get<HaulJob>(pawn);
    job.item = best;
    job.path = pathfinder_.request(cellOf(pos), cellOf(reg.get<Position>(best)));
    job.stage = HaulStage::WaitPathToItem;
}

void Sim::updateHauler(Entity pawn) {
    entt::registry& reg = things_->registry();
    HaulJob& job = reg.get<HaulJob>(pawn);
    switch (job.stage) {
        case HaulStage::Idle:
            startNextHaul(pawn);
            break;
        case HaulStage::WaitPathToItem: {
            if (pathfinder_.status(job.path) == PathStatus::Queued) {
                break;
            }
            Mover& mover = reg.get<Mover>(pawn);
            mover.next = 0;
            if (pathfinder_.takePath(job.path, mover.path)) {
                simStartWalk(map_, reg.get<Position>(pawn), mover);
                job.stage = HaulStage::GoToItem;
            } else {
                ++stats_.failedJobs;  // item stays reserved so nobody retries it
                job.stage = HaulStage::Idle;
                job.item = kNoEntity;
            }
            break;
        }
        case HaulStage::GoToItem:
            if (reg.get<Mover>(pawn).gaveUp) {
                // Held up for good (a rock in the way of the line it was on): ask for a path from here.
                reg.get<Mover>(pawn).gaveUp = false;
                ++stats_.repaths;
                job.path = pathfinder_.request(cellOf(reg.get<Position>(pawn)), cellOf(reg.get<Position>(job.item)));
                job.stage = HaulStage::WaitPathToItem;
            } else if (reg.get<Mover>(pawn).arrived()) {
                job.path = pathfinder_.request(cellOf(reg.get<Position>(pawn)), *stockpileCell_);
                job.stage = HaulStage::WaitPathToStockpile;
            }
            break;
        case HaulStage::WaitPathToStockpile: {
            if (pathfinder_.status(job.path) == PathStatus::Queued) {
                break;
            }
            Mover& mover = reg.get<Mover>(pawn);
            mover.next = 0;
            if (pathfinder_.takePath(job.path, mover.path)) {
                simStartWalk(map_, reg.get<Position>(pawn), mover);
                things_->despawn(job.item);
                reg.emplace<HaulCarriedBy>(job.item, HaulCarriedBy{pawn});
                job.stage = HaulStage::CarryToStockpile;
            } else {
                ++stats_.failedJobs;  // item stays where it is, reserved
                job.stage = HaulStage::Idle;
                job.item = kNoEntity;
            }
            break;
        }
        case HaulStage::WaitPathCarrying: {
            if (pathfinder_.status(job.path) == PathStatus::Queued) {
                break;
            }
            Mover& mover = reg.get<Mover>(pawn);
            if (pathfinder_.takePath(job.path, mover.path)) {
                simStartWalk(map_, reg.get<Position>(pawn), mover);
                job.stage = HaulStage::CarryToStockpile;
            } else {
                // No way to the stockpile from here: puts the item down where it stands.
                reg.remove<HaulCarriedBy, HaulReservedBy>(job.item);
                things_->spawnAt(job.item, cellOf(reg.get<Position>(pawn)));
                ++stats_.failedJobs;
                job.stage = HaulStage::Idle;
                job.item = kNoEntity;
            }
            break;
        }
        case HaulStage::CarryToStockpile:
            if (reg.get<Mover>(pawn).gaveUp) {
                reg.get<Mover>(pawn).gaveUp = false;
                ++stats_.repaths;
                job.path = pathfinder_.request(cellOf(reg.get<Position>(pawn)), *stockpileCell_);
                job.stage = HaulStage::WaitPathCarrying;
            } else if (reg.get<Mover>(pawn).arrived()) {
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
    if (const auto* spawn = std::get_if<SpawnThingCommand>(&command)) {
        const DefId def = defs_->idOf("Things", spawn->def);
        if (!def.valid()) {
            commandFailed(id, "no Things def '" + spawn->def + "'");
        } else if (things_->spawn(def, spawn->cell, spawn->count, spawn->rot) == kNoEntity) {
            commandFailed(id, "can't put " + spawn->def + " at " + simCellText(spawn->cell));
        }
    } else if (const auto* destroy = std::get_if<DestroyThingCommand>(&command)) {
        if (!things_->exists(destroy->thing)) {
            commandFailed(id, "that thing doesn't exist");
        } else if (things_->registry().any_of<HaulJob, HaulReservedBy, HaulCarriedBy>(destroy->thing)) {
            commandFailed(id, "the hauling test is using that thing");
        } else {
            things_->destroy(destroy->thing);
        }
    }
}

void Sim::step() {
    events_.clear();
    // Commands first, in submit order. Swap out the queue so its memory is reused.
    std::vector<std::pair<uint64_t, Command>> applying;
    applying.swap(pending_);
    for (auto& [id, command] : applying) {
        applyCommand(command, id);
        commandLog_.push_back(LoggedCommand{time_.seconds, std::move(command)});
    }
    applying.clear();
    pending_.swap(applying);

    for (const Entity pawn : haulers_) {
        updateHauler(pawn);
    }
    movement_->step(things_->registry(), map_);
    pathfinder_.update(settings_.pathBudget);  // paths asked for this step are ready next step at the earliest
    time_.seconds += 1;
    if (settings_.checkpointEvery > 0 && time_.seconds % settings_.checkpointEvery == 0) {
        checkpoints_.push_back(Checkpoint{time_.seconds, stateHash()});
    }
}

void Sim::writeSnapshot(Snapshot& out) const {
    out.gameSeconds = time_.seconds;
    out.mapWidth = map_.width();
    out.mapHeight = map_.height();
    out.things.clear();
    out.waypoints.clear();
    const entt::registry& reg = things_->registry();
    for (const auto [entity, thing, pos] : reg.view<const Thing, const Position>().each()) {
        ThingView view;
        view.id = entity;
        view.def = thing.def;
        view.category = thing.category;
        view.x = pos.x;
        view.y = pos.y;
        view.z = map_.heightAt(pos.x, pos.y);
        view.level = pos.level;
        if (const Mover* mover = reg.try_get<Mover>(entity)) {
            view.vx = mover->vx;
            view.vy = mover->vy;
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
        }
        if (const StackCount* stack = reg.try_get<StackCount>(entity)) {
            view.stackCount = stack->count;
        }
        if (const HitPoints* hp = reg.try_get<HitPoints>(entity)) {
            view.hitPoints = hp->current;
            view.maxHitPoints = hp->max;
        }
        out.things.push_back(view);
    }
    std::sort(out.things.begin(), out.things.end(),
              [](const ThingView& a, const ThingView& b) { return a.id < b.id; });
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
    pathfinder_.hashState(h);

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
        const bool has[] = {reg.all_of<Thing>(e),         reg.all_of<Position>(e),       reg.all_of<Footprint>(e),
                            reg.all_of<StackCount>(e),    reg.all_of<HitPoints>(e),      reg.all_of<Mover>(e),
                            reg.all_of<HaulJob>(e),       reg.all_of<HaulReservedBy>(e), reg.all_of<HaulCarriedBy>(e),
                            reg.all_of<HaulStored>(e)};
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
        }
        if (const HaulJob* job = reg.try_get<HaulJob>(e)) {
            h.add(static_cast<int>(job->stage));
            h.add(entt::to_integral(job->item));
            h.add(job->path.slot);
            h.add(job->path.serial);
        }
        if (const HaulReservedBy* by = reg.try_get<HaulReservedBy>(e)) {
            h.add(entt::to_integral(by->pawn));
        }
        if (const HaulCarriedBy* by = reg.try_get<HaulCarriedBy>(e)) {
            h.add(entt::to_integral(by->pawn));
        }
    }
    return h.value();
}

void Sim::stepMany(int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        step();
    }
}

}  // namespace colony

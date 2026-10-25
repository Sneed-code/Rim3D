#include "ColonySim/Pathfinder.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"
#include "ColonySim/Regions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr float kPathDiagonalCost = 1.41421356f;
constexpr uint32_t kPathNone = 0xFFFFFFFFu;

// Octile distance: the cost of the shortest 8-connected walk over open ground.
float pathOctile(float dx, float dy) {
    dx = std::fabs(dx);
    dy = std::fabs(dy);
    return dx + dy + (kPathDiagonalCost - 2.0f) * std::min(dx, dy);
}

float pathOctileCells(int ax, int ay, int bx, int by) {
    return pathOctile(static_cast<float>(ax - bx), static_cast<float>(ay - by));
}

// Min-heap order. Equal f goes to the smaller h, then the lower tie so searches are deterministic.
// Region ids aren't used to break ties: a loaded save builds its regions afresh, and their ids may
// differ from the saved game's, but their anchor cells don't.
struct PathHeapAfter {
    bool operator()(const Pathfinder::OpenNode& a, const Pathfinder::OpenNode& b) const {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        if (a.h != b.h) {
            return a.h > b.h;
        }
        return a.tie > b.tie;
    }
};

}  // namespace

Pathfinder::Pathfinder(Map& map) : map_(map) {
    const size_t cells = static_cast<size_t>(map.width()) * static_cast<size_t>(map.height());
    g_.assign(cells, 0.0f);
    parent_.assign(cells, -1);
    seen_.assign(cells, 0);
    closed_.assign(cells, 0);
}

// ---------------------------------------------------------------------------------------------
// Queue

PathTicket Pathfinder::request(CellPos start, CellPos goal) {
    ++stats_.requests;
    uint32_t slot;
    if (freeSlots_.empty()) {
        slot = static_cast<uint32_t>(requests_.size());
        requests_.emplace_back();
    } else {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
    }
    Request& r = requests_[slot];
    r.serial = nextSerial_++;
    if (nextSerial_ == 0) {
        nextSerial_ = 1;
    }
    r.start = start;
    r.goal = goal;
    r.askedAt = updates_;
    r.path.clear();
    if (start == goal) {
        r.status = PathStatus::Found;
    } else if (!map_.regions().canReach(start, goal)) {
        r.status = PathStatus::Failed;
        ++stats_.refused;
    } else {
        r.status = PathStatus::Queued;
        queue_.push_back(slot);
        stats_.maxQueue = std::max(stats_.maxQueue, static_cast<uint32_t>(queue_.size()));
    }
    return PathTicket{slot, r.serial};
}

Pathfinder::Request* Pathfinder::find(PathTicket ticket) {
    if (ticket.slot >= requests_.size()) {
        return nullptr;
    }
    Request& r = requests_[ticket.slot];
    return r.serial == ticket.serial && r.status != PathStatus::Unknown ? &r : nullptr;
}

const Pathfinder::Request* Pathfinder::find(PathTicket ticket) const {
    return const_cast<Pathfinder*>(this)->find(ticket);
}

PathStatus Pathfinder::status(PathTicket ticket) const {
    const Request* r = find(ticket);
    return r ? r->status : PathStatus::Unknown;
}

bool Pathfinder::takePath(PathTicket ticket, std::vector<CellPos>& out) {
    out.clear();
    Request* r = find(ticket);
    if (!r || r->status == PathStatus::Queued) {
        return false;
    }
    const bool found = r->status == PathStatus::Found;
    if (found) {
        out.swap(r->path);
    }
    freeSlot(ticket.slot);
    return found;
}

void Pathfinder::cancel(PathTicket ticket) {
    Request* r = find(ticket);
    if (!r) {
        return;
    }
    if (r->status == PathStatus::Queued) {
        if (queue_.front() == ticket.slot) {
            phase_ = Phase::Idle;  // it was being searched
            activeWork_ = 0;
        }
        queue_.erase(std::find(queue_.begin(), queue_.end(), ticket.slot));
    }
    freeSlot(ticket.slot);
}

void Pathfinder::freeSlot(uint32_t slot) {
    Request& r = requests_[slot];
    r.status = PathStatus::Unknown;
    r.path.clear();
    freeSlots_.push_back(slot);
}

void Pathfinder::finish(uint32_t slot, bool found) {
    Request& r = requests_[slot];
    r.status = found ? PathStatus::Found : PathStatus::Failed;
    if (found) {
        buildPath(r.path);
    }
    stats_.maxWaitSteps = std::max(stats_.maxWaitSteps, static_cast<uint32_t>(updates_ - r.askedAt));
}

void Pathfinder::update(int budget) {
    ++updates_;
    Regions& regions = map_.regions();
    regions.update();
    if (phase_ != Phase::Idle && regions.changeCount() != regionsChangeCount_) {
        phase_ = Phase::Idle;  // its region ids are stale; start it again below
        activeWork_ = 0;
        ++stats_.restarts;
    }
    int64_t work = 0;
    while (!queue_.empty() && work < budget) {
        const uint32_t slot = queue_.front();
        if (phase_ == Phase::Idle) {
            const Request& r = requests_[slot];
            if (!beginSearch(r.start, r.goal)) {
                // The map changed since the request and the goal can't be reached any more.
                queue_.pop_front();
                ++stats_.refused;
                finish(slot, false);
                continue;
            }
            activeWork_ = 0;
        }
        const int64_t before = work;
        const Outcome outcome = runSearch(work, budget);
        activeWork_ += work - before;
        if (outcome == Outcome::Running) {
            break;  // goes on next step
        }
        queue_.pop_front();
        phase_ = Phase::Idle;
        activeWork_ = 0;
        ++stats_.searches;
        stats_.failedSearches += outcome == Outcome::Failed ? 1 : 0;
        finish(slot, outcome == Outcome::Found);
    }
    stats_.maxWorkInStep = std::max(stats_.maxWorkInStep, static_cast<uint64_t>(work));
}

void Pathfinder::hashState(StateHasher& h) const {
    h.add(nextSerial_);
    h.add(updates_);
    h.add(static_cast<uint64_t>(requests_.size()));
    for (const Request& r : requests_) {
        h.add(r.serial);
        h.add(static_cast<int>(r.status));
        if (r.status == PathStatus::Unknown) {
            continue;
        }
        for (const CellPos& c : {r.start, r.goal}) {
            h.add(c.x);
            h.add(c.y);
            h.add(c.level);
        }
        h.add(r.askedAt);
        h.add(static_cast<uint64_t>(r.path.size()));
    }
    h.add(static_cast<uint64_t>(freeSlots_.size()));
    for (const uint32_t slot : freeSlots_) {
        h.add(slot);
    }
    h.add(static_cast<uint64_t>(queue_.size()));
    for (const uint32_t slot : queue_) {
        h.add(slot);
    }
    // A half-done search is fully decided by its request, the map and the work done so far.
    h.add(static_cast<int>(phase_));
    h.add(activeWork_);
}

PathSavedState Pathfinder::saveState() const {
    PathSavedState out;
    out.nextSerial = nextSerial_;
    out.updates = updates_;
    for (const Request& r : requests_) {
        out.requests.push_back(PathSavedRequest{r.serial, r.status, r.start, r.goal, r.askedAt, r.path});
    }
    out.freeSlots = freeSlots_;
    out.queue.assign(queue_.begin(), queue_.end());
    out.activeWork = phase_ == Phase::Idle ? 0 : activeWork_;
    return out;
}

bool Pathfinder::loadState(PathSavedState state, std::string& error) {
    // Every slot is either free (Unknown, in freeSlots once) or live; the Queued ones are exactly
    // the queue, each once.
    const size_t slots = state.requests.size();
    std::vector<uint8_t> listed(slots, 0);
    for (const uint32_t slot : state.freeSlots) {
        if (slot >= slots || listed[slot] != 0 || state.requests[slot].status != PathStatus::Unknown) {
            error = "the save's path queue is damaged";
            return false;
        }
        listed[slot] = 1;
    }
    for (const uint32_t slot : state.queue) {
        if (slot >= slots || listed[slot] != 0 || state.requests[slot].status != PathStatus::Queued) {
            error = "the save's path queue is damaged";
            return false;
        }
        listed[slot] = 1;
    }
    for (size_t slot = 0; slot < slots; ++slot) {
        const PathSavedRequest& r = state.requests[slot];
        const bool bad = static_cast<int>(r.status) > static_cast<int>(PathStatus::Failed) ||
                         (r.status == PathStatus::Unknown && listed[slot] == 0) ||
                         (r.status == PathStatus::Queued && listed[slot] == 0) ||
                         (r.status != PathStatus::Found && !r.path.empty());
        if (bad) {
            error = "the save's path queue is damaged";
            return false;
        }
    }
    if (state.activeWork < 0 || (state.activeWork > 0 && state.queue.empty()) || state.nextSerial == 0) {
        error = "the save's path queue is damaged";
        return false;
    }

    requests_.clear();
    for (PathSavedRequest& r : state.requests) {
        Request& to = requests_.emplace_back();
        to.serial = r.serial;
        to.status = r.status;
        to.start = r.start;
        to.goal = r.goal;
        to.askedAt = r.askedAt;
        to.path = std::move(r.path);
    }
    freeSlots_ = std::move(state.freeSlots);
    queue_.assign(state.queue.begin(), state.queue.end());
    nextSerial_ = state.nextSerial;
    updates_ = state.updates;
    phase_ = Phase::Idle;
    activeWork_ = 0;

    // Redo the half-done search: the same request, map and amount of work give the same state.
    if (state.activeWork > 0) {
        const Request& front = requests_[queue_.front()];
        if (!map_.inBounds(front.start) || !map_.inBounds(front.goal) || !beginSearch(front.start, front.goal)) {
            error = "the save has a path search the map can't have";
            return false;
        }
        int64_t work = 0;
        if (runSearch(work, state.activeWork) != Outcome::Running || work != state.activeWork) {
            error = "the save has a path search the map can't have";
            phase_ = Phase::Idle;
            return false;
        }
        activeWork_ = work;
    }
    stats_ = PathStats{};
    return true;
}

bool Pathfinder::findPath(CellPos start, CellPos goal, std::vector<CellPos>& out) {
    return searchNow(start, goal, out, true);
}

bool Pathfinder::findPathFullGrid(CellPos start, CellPos goal, std::vector<CellPos>& out) {
    return searchNow(start, goal, out, false);
}

bool Pathfinder::searchNow(CellPos start, CellPos goal, std::vector<CellPos>& out, bool hierarchical) {
    out.clear();
    if (phase_ != Phase::Idle) {
        phase_ = Phase::Idle;  // the queued search's state is about to be overwritten
        activeWork_ = 0;
        ++stats_.restarts;
    }
    if (start == goal) {
        return true;
    }
    if (!beginSearch(start, goal)) {
        ++stats_.refused;
        return false;
    }
    if (!hierarchical) {
        beginCells(false);
    }
    int64_t work = 0;
    const Outcome outcome = runSearch(work, std::numeric_limits<int64_t>::max());
    phase_ = Phase::Idle;
    ++stats_.searches;
    if (outcome != Outcome::Found) {
        ++stats_.failedSearches;
        return false;
    }
    buildPath(out);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Search

bool Pathfinder::beginSearch(CellPos start, CellPos goal) {
    Regions& regions = map_.regions();
    if (!regions.canReach(start, goal)) {  // rebuilds dirty blocks first
        return false;
    }
    regionsChangeCount_ = regions.changeCount();
    start_ = start;
    goal_ = goal;

    // Two generations per search: the route and corridor use the first, the guide and the cell
    // pass the second.
    generation_ += 2;
    if (generation_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0u);
        std::fill(closed_.begin(), closed_.end(), 0u);
        std::fill(rSeen_.begin(), rSeen_.end(), 0u);
        std::fill(rClosed_.begin(), rClosed_.end(), 0u);
        std::fill(corridor_.begin(), corridor_.end(), 0u);
        generation_ = 2;
    }
    routeGen_ = generation_ - 1;
    goalNode_ = static_cast<uint32_t>(regions.idLimit());
    const size_t nodes = regions.idLimit() + 1;
    if (rG_.size() < nodes) {
        rG_.resize(nodes, 0.0f);
        rParent_.resize(nodes, kPathNone);
        rX_.resize(nodes, 0.0f);
        rY_.resize(nodes, 0.0f);
        rSeen_.resize(nodes, 0u);
        rClosed_.resize(nodes, 0u);
        corridor_.resize(nodes, 0u);
    }

    // Route pass from the start cell's middle.
    const RegionId from = regions.regionAtNow(start);
    goalRegion_ = regions.regionAtNow(goal);
    const float sx = static_cast<float>(start.x) + 0.5f;
    const float sy = static_cast<float>(start.y) + 0.5f;
    rG_[from] = 0.0f;
    rParent_[from] = kPathNone;
    rX_[from] = sx;
    rY_[from] = sy;
    rSeen_[from] = routeGen_;
    const float h = pathOctile(sx - (static_cast<float>(goal.x) + 0.5f), sy - (static_cast<float>(goal.y) + 0.5f));
    open_.clear();
    open_.push_back(OpenNode{h, h, regionTie(from), from});
    phase_ = Phase::Route;
    return true;
}

Pathfinder::Outcome Pathfinder::runSearch(int64_t& work, int64_t budget) {
    if (phase_ == Phase::Route) {
        const Outcome route = runRoute(work, budget);
        if (route != Outcome::Found) {
            return route;
        }
        beginGuide();
    }
    if (phase_ == Phase::Guide) {
        const Outcome guide = runGuide(work, budget);
        if (guide != Outcome::Found) {
            return guide;
        }
        beginCells(true);
    }
    return runCells(work, budget);
}

void Pathfinder::pushRegion(uint32_t node, uint32_t parent, float g, float x, float y, float h, uint32_t gen) {
    if (rSeen_[node] == gen && g >= rG_[node]) {
        return;
    }
    rSeen_[node] = gen;
    rG_[node] = g;
    rParent_[node] = parent;
    rX_[node] = x;
    rY_[node] = y;
    open_.push_back(OpenNode{g + h, h, regionTie(node), node});
    std::push_heap(open_.begin(), open_.end(), PathHeapAfter{});
}

uint32_t Pathfinder::regionTie(uint32_t node) const {
    if (node == goalNode_) {
        return 0xFFFFFFFFu;
    }
    const CellPos anchor = map_.regions().region(node).anchor;
    return static_cast<uint32_t>((anchor.level * map_.height() + anchor.y) * map_.width() + anchor.x);
}

// Route pass: A* over the region graph. A region is entered at the middle of the border it was
// reached through, and each hop costs the octile distance between entry points. The goal is a
// node of its own, reached from the goal region at the cost of the walk from its entry point.
Pathfinder::Outcome Pathfinder::runRoute(int64_t& work, int64_t budget) {
    const Regions& regions = map_.regions();
    const uint32_t gen = routeGen_;
    const float gx = static_cast<float>(goal_.x) + 0.5f;
    const float gy = static_cast<float>(goal_.y) + 0.5f;
    while (!open_.empty()) {
        if (work >= budget) {
            return Outcome::Running;
        }
        std::pop_heap(open_.begin(), open_.end(), PathHeapAfter{});
        const uint32_t node = open_.back().node;
        open_.pop_back();
        if (node == goalNode_) {
            return Outcome::Found;
        }
        if (rClosed_[node] == gen) {
            continue;  // stale duplicate
        }
        rClosed_[node] = gen;
        ++work;
        ++stats_.regionsExpanded;

        const float px = rX_[node];
        const float py = rY_[node];
        const float g = rG_[node];
        if (node == goalRegion_) {
            pushRegion(goalNode_, node, g + pathOctile(gx - px, gy - py), gx, gy, 0.0f, gen);
        }
        for (const RegionLink& link : regions.region(node).links) {
            if (rClosed_[link.other] != gen) {
                const float enter = regions.region(link.other).kind == RegionKind::Door ? doorCost_ : 0.0f;
                pushRegion(link.other, node, g + pathOctile(link.x - px, link.y - py) + enter, link.x, link.y,
                           pathOctile(gx - link.x, gy - link.y), gen);
            }
        }
    }
    return Outcome::Failed;  // can't happen while reachability groups are right
}

// The corridor is the route's regions and every region linked to one of them; the extra ring
// lets the path cut across a neighbour where the route's border middles zig-zag. The guide pass
// then walks the corridor backwards from the goal (Dijkstra, same costs as the route pass), so
// every corridor region knows the point it leaves through towards the goal and the cost from
// there. The cell pass uses that as its heuristic and so follows the corridor instead of filling
// it.
void Pathfinder::beginGuide() {
    const Regions& regions = map_.regions();
    for (uint32_t r = rParent_[goalNode_]; r != kPathNone; r = rParent_[r]) {
        corridor_[r] = routeGen_;
        for (const RegionLink& link : regions.region(r).links) {
            corridor_[link.other] = routeGen_;
        }
    }
    open_.clear();
    pushRegion(goalRegion_, kPathNone, 0.0f, static_cast<float>(goal_.x) + 0.5f, static_cast<float>(goal_.y) + 0.5f,
               0.0f, generation_);
    phase_ = Phase::Guide;
}

Pathfinder::Outcome Pathfinder::runGuide(int64_t& work, int64_t budget) {
    const Regions& regions = map_.regions();
    const uint32_t gen = generation_;
    while (!open_.empty()) {
        if (work >= budget) {
            return Outcome::Running;
        }
        std::pop_heap(open_.begin(), open_.end(), PathHeapAfter{});
        const uint32_t node = open_.back().node;
        open_.pop_back();
        if (rClosed_[node] == gen) {
            continue;
        }
        rClosed_[node] = gen;
        ++work;
        ++stats_.regionsExpanded;
        const float px = rX_[node];
        const float py = rY_[node];
        const float g = rG_[node];
        for (const RegionLink& link : regions.region(node).links) {
            if (corridor_[link.other] == routeGen_ && rClosed_[link.other] != gen) {
                pushRegion(link.other, node, g + pathOctile(link.x - px, link.y - py), link.x, link.y, 0.0f, gen);
            }
        }
    }
    return Outcome::Found;
}

void Pathfinder::beginCells(bool corridorOnly) {
    corridorOnly_ = corridorOnly;
    const int start = map_.index(start_);
    g_[static_cast<size_t>(start)] = 0.0f;
    parent_[static_cast<size_t>(start)] = -1;
    seen_[static_cast<size_t>(start)] = generation_;
    const float h = cellHeuristic(start_.x, start_.y, map_.regions().regionAtNow(start_));
    open_.clear();
    open_.push_back(OpenNode{h, h, static_cast<uint32_t>(start), static_cast<uint32_t>(start)});
    phase_ = Phase::Cells;
}

float Pathfinder::cellHeuristic(int x, int y, RegionId region) const {
    if (!corridorOnly_) {
        return pathOctileCells(x, y, goal_.x, goal_.y);
    }
    // Walk to where the region is left towards the goal, then the guide's cost from there.
    return rG_[region] + pathOctile(rX_[region] - (static_cast<float>(x) + 0.5f), rY_[region] - (static_cast<float>(y) + 0.5f));
}

// Cell pass: A* over the 8-connected grid, with no corner cutting. With the corridor, only cells
// in corridor regions are used and the heuristic is the guide's.
Pathfinder::Outcome Pathfinder::runCells(int64_t& work, int64_t budget) {
    const uint32_t gen = generation_;
    const int w = map_.width();
    const int h = map_.height();
    const int goal = map_.index(goal_);
    const RegionId* regionOf = map_.regions().regionIdsOnLevel(goal_.level);  // kNoRegion: can't be walked
    const Passage* passage = map_.passageOnLevel(goal_.level);
    const auto at = [](auto& v, int i) -> auto& { return v[static_cast<size_t>(i)]; };

    static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    while (!open_.empty()) {
        if (work >= budget) {
            return Outcome::Running;
        }
        std::pop_heap(open_.begin(), open_.end(), PathHeapAfter{});
        const int c = static_cast<int>(open_.back().node);
        open_.pop_back();
        if (at(closed_, c) == gen) {
            continue;  // stale duplicate
        }
        at(closed_, c) = gen;
        ++work;
        ++stats_.cellsExpanded;
        if (c == goal) {
            return Outcome::Found;
        }

        const int cx = c % w;
        const int cy = c / w;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + kDx[d];
            const int ny = cy + kDy[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            const int n = ny * w + nx;
            const RegionId region = regionOf[n];
            if (region == kNoRegion || (corridorOnly_ && corridor_[region] != routeGen_)) {
                continue;
            }
            const bool diagonal = d >= 4;
            if (diagonal && (regionOf[cy * w + nx] == kNoRegion || regionOf[ny * w + cx] == kNoRegion)) {
                continue;  // no corner cutting
            }
            if (at(closed_, n) == gen) {
                continue;
            }
            const float ng = at(g_, c) + (diagonal ? kPathDiagonalCost : 1.0f) + (passage[n] == Passage::Door ? doorCost_ : 0.0f);
            if (at(seen_, n) != gen || ng < at(g_, n)) {
                at(seen_, n) = gen;
                at(g_, n) = ng;
                at(parent_, n) = c;
                const float nh = cellHeuristic(nx, ny, region);
                open_.push_back(OpenNode{ng + nh, nh, static_cast<uint32_t>(n), static_cast<uint32_t>(n)});
                std::push_heap(open_.begin(), open_.end(), PathHeapAfter{});
            }
        }
    }
    return Outcome::Failed;  // can't happen: the corridor joins start and goal
}

void Pathfinder::buildPath(std::vector<CellPos>& out) const {
    out.clear();
    const int start = map_.index(start_);
    for (int i = map_.index(goal_); i != start; i = parent_[static_cast<size_t>(i)]) {
        out.push_back(map_.cellAt(i, goal_.level));
    }
    std::reverse(out.begin(), out.end());
}

}  // namespace colony

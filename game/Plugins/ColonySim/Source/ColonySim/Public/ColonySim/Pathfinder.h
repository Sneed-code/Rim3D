#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace colony {

class Map;
class StateHasher;

// Hierarchical pathfinding. See docs/design/pathfinding.md.
//
// A path is found in passes: A* over the region graph (docs/design/regions.md) gives a route of
// regions; a short walk back from the goal over those regions and the regions next to them (the
// corridor) gives each one the cost left to the goal; then cell A* over the 8-connected grid,
// with no corner cutting, finds the path inside the corridor, using those costs as its guide.
// Requests whose start and goal are in different reachability groups are refused at once,
// without a search.
//
// The game asks for paths through a queue: `request` returns a ticket, `update` runs once per
// step and does at most a fixed budget of work (regions plus cells expanded, never time), so a
// burst of long searches spreads over a few steps and a pawn waits a step or two for its path.
// A search left half done at the end of a step goes on in the next one; if the map's regions
// change in between, it starts again.
//
// For now every walkable cell costs the same, and cells with a blocking building can't be
// walked. Terrain, slope, door and danger costs come later.

// A request in the queue. Copy it freely; a ticket for a freed request is simply unknown.
struct PathTicket {
    uint32_t slot = 0xFFFFFFFFu;
    uint32_t serial = 0;

    bool valid() const { return slot != 0xFFFFFFFFu; }
    friend bool operator==(const PathTicket&, const PathTicket&) = default;
};

enum class PathStatus : uint8_t {
    Unknown,  // not a ticket this pathfinder knows (never issued, taken or cancelled)
    Queued,   // waiting for, or in the middle of, its search
    Found,    // the path is ready for `takePath`
    Failed,   // no path: unreachable, refused at once or found so by the search
};

struct PathStats {
    uint64_t requests = 0;
    uint64_t refused = 0;           // different reachability groups (or off the map), no search
    uint64_t searches = 0;          // searches finished, found or not, queued or direct
    uint64_t failedSearches = 0;    // searches that found no path (a bug if regions say reachable)
    uint64_t restarts = 0;          // half-done searches started again after the regions changed
    uint64_t regionsExpanded = 0;
    uint64_t cellsExpanded = 0;
    uint64_t maxWorkInStep = 0;     // most work `update` did in one step
    uint32_t maxQueue = 0;          // most requests waiting at once
    uint32_t maxWaitSteps = 0;      // most `update` calls a request waited for its answer
};

// The queue as plain values, for saves (docs/design/saves.md). Counters are not part of it.
struct PathSavedRequest {
    uint32_t serial = 0;
    PathStatus status = PathStatus::Unknown;
    CellPos start;
    CellPos goal;
    uint64_t askedAt = 0;
    std::vector<CellPos> path;
};

struct PathSavedState {
    uint32_t nextSerial = 1;
    uint64_t updates = 0;
    std::vector<PathSavedRequest> requests;  // by ticket slot
    std::vector<uint32_t> freeSlots;         // in the order they are used again (the last first)
    std::vector<uint32_t> queue;             // in request order
    int64_t activeWork = 0;                  // work done on the front request's search so far
};

class COLONY_API Pathfinder {
public:
    // Work per step when the sim doesn't say otherwise: regions plus cells expanded. Measured at
    // 1 to 1.5 ms in the cloud (docs/design/pathfinding.md); sneed's PC should be faster.
    static constexpr int kDefaultBudget = 3000;

    struct OpenNode {
        float f;
        float h;      // ties on f go to the node nearer the goal, which keeps open ground cheap
        uint32_t tie;  // then the lower of these: the cell, or a region's anchor cell (never its id)
        uint32_t node;
    };

    // Not const: region queries rebuild dirty blocks.
    explicit Pathfinder(Map& map);

    // Queues a path search from `start` to `goal`. A request that can't succeed (different
    // reachability groups, off the map, another level) is Failed at once; start == goal is Found
    // at once with an empty path. Every ticket must be taken or cancelled to free its slot.
    PathTicket request(CellPos start, CellPos goal);
    PathStatus status(PathTicket ticket) const;
    // Moves a Found path into `out` (the cells after the start up to and including the goal) and
    // frees the ticket. Returns false, leaving `out` empty, for any other status; a Failed ticket
    // is freed too.
    bool takePath(PathTicket ticket, std::vector<CellPos>& out);
    // Drops a request in any state and frees its ticket.
    void cancel(PathTicket ticket);

    // Runs queued searches in request order until `budget` work is done or the queue is empty.
    // The sim calls this once per step.
    void update(int budget = kDefaultBudget);
    size_t queued() const { return queue_.size(); }

    // Finds a path now, with no budget. For tests and tools: it throws away the progress of a
    // half-done queued search, which then starts again on the next update.
    bool findPath(CellPos start, CellPos goal, std::vector<CellPos>& out);
    // Plain cell A* over the whole level, without regions: the best path, for tests to compare.
    bool findPathFullGrid(CellPos start, CellPos goal, std::vector<CellPos>& out);

    const PathStats& stats() const { return stats_; }
    // Adds the queue and the progress of the search in hand to a state hash (not the stats).
    void hashState(StateHasher& h) const;
    PathSavedState saveState() const;
    // Puts a saved queue back and redoes the half-done search, on a map that already has every
    // building. Returns false, with `error`, if the state doesn't add up.
    bool loadState(PathSavedState state, std::string& error);

private:
    enum class Phase : uint8_t { Idle, Route, Guide, Cells };
    enum class Outcome : uint8_t { Running, Found, Failed };

    struct Request {
        uint32_t serial = 0;
        PathStatus status = PathStatus::Unknown;
        CellPos start;
        CellPos goal;
        uint64_t askedAt = 0;  // update() count when requested
        std::vector<CellPos> path;
    };

    Request* find(PathTicket ticket);
    const Request* find(PathTicket ticket) const;
    void freeSlot(uint32_t slot);
    void finish(uint32_t slot, bool found);
    bool searchNow(CellPos start, CellPos goal, std::vector<CellPos>& out, bool hierarchical);

    // One search at a time, kept in the members below so it can stop and go on next step.
    bool beginSearch(CellPos start, CellPos goal);  // false if it can't succeed
    void pushRegion(uint32_t node, uint32_t parent, float g, float x, float y, float h, uint32_t gen);
    uint32_t regionTie(uint32_t node) const;
    Outcome runRoute(int64_t& work, int64_t budget);
    void beginGuide();
    Outcome runGuide(int64_t& work, int64_t budget);
    void beginCells(bool corridorOnly);
    float cellHeuristic(int x, int y, uint32_t region) const;
    Outcome runCells(int64_t& work, int64_t budget);
    Outcome runSearch(int64_t& work, int64_t budget);
    void buildPath(std::vector<CellPos>& out) const;

    Map& map_;

    // Queue.
    std::vector<Request> requests_;  // indexed by ticket slot
    std::vector<uint32_t> freeSlots_;
    std::deque<uint32_t> queue_;     // slots in request order; the front one is being searched
    uint32_t nextSerial_ = 1;
    uint64_t updates_ = 0;

    // The search in progress; while phase_ isn't Idle it belongs to queue_.front().
    Phase phase_ = Phase::Idle;
    int64_t activeWork_ = 0;  // work done on it since it (re)started; the search is a function of this
    uint64_t regionsChangeCount_ = 0;
    CellPos start_;
    CellPos goal_;
    bool corridorOnly_ = true;

    // Region passes, indexed by region id, plus the goal node at goalNode_. The route pass fills
    // them with the cost from the start and the entry point, the guide pass then with the cost to
    // the goal and the exit point.
    std::vector<float> rG_;
    std::vector<uint32_t> rParent_;
    std::vector<float> rX_;  // metres
    std::vector<float> rY_;
    std::vector<uint32_t> rSeen_;
    std::vector<uint32_t> rClosed_;
    std::vector<uint32_t> corridor_;  // == routeGen_ for regions the cell pass may use
    uint32_t routeGen_ = 0;           // generation_ - 1: the route pass and the corridor
    uint32_t goalRegion_ = 0;
    uint32_t goalNode_ = 0;  // == the region id limit when the search began

    // Cell pass, indexed by cell.
    std::vector<float> g_;
    std::vector<int> parent_;
    std::vector<uint32_t> seen_;    // == generation_ when g_ and parent_ are valid
    std::vector<uint32_t> closed_;  // == generation_ when the cell is done

    std::vector<OpenNode> open_;
    uint32_t generation_ = 0;
    PathStats stats_;
};

}  // namespace colony

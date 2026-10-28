// Transport pods, the sim's side (docs/design/transport-pods.md): PodCommand, and what pods do where
// they come down: land on the colony's map or another map, form a caravan on the tile, or be a gift
// to a settlement. Loading and flying are TransportPodSystem's (TransportPods.cpp).
#include "ColonySim/Caravans.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/OtherMaps.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"
#include "ColonySim/TransportPods.h"
#include "ColonySim/World.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace colony {

void Sim::applyPodCommand(const PodCommand& command, uint64_t id) {
    std::string error;
    bool ok = false;
    if (command.action == "Load") {
        int made = 0;
        ok = pods_->load(command.pods, command.pawns, command.items, made, error);
    } else if (command.action == "Cancel") {
        ok = pods_->cancel(command.group, error);
    } else if (command.action == "Launch") {
        ok = launchPods(command, error);
    } else {
        error = "no pod action '" + command.action + "' (Load, Cancel or Launch)";
    }
    if (!ok) {
        commandFailed(id, error);
    }
}

bool Sim::launchPods(const PodCommand& command, std::string& error) {
    PodArrival arrival = PodArrival::Land;
    if (!podArrivalFromName(command.arrival, arrival)) {
        error = "no arrival '" + command.arrival + "' (Land, Caravan or Gift)";
        return false;
    }
    const PodGroup* group = pods_->find(command.group);
    if (group == nullptr) {
        error = "no such pod group";
        return false;
    }
    const bool home = command.tile < 0 || (world_ != nullptr && command.tile == world_->startTile());
    if (!home && (world_ == nullptr || settings_.world.visit || world_->startTile() < 0)) {
        error = "there is no world map to fly to";
        return false;
    }
    if (!home && command.tile >= world_->tileCount()) {
        error = "there is no such tile";
        return false;
    }
    const CellPos cell{command.x, command.y, 0};
    const int settlement = home ? kNoSettlement : world_->tile(command.tile).settlement;
    if (arrival == PodArrival::Land) {
        const Sim* there = home ? this : otherMap(command.tile);
        if (!home && settlement != kNoSettlement) {
            error = "landing on a settlement is an attack, which isn't in the game yet: send a caravan or a gift";
            return false;
        }
        if (there == nullptr) {
            error = "pods can only land on a map a caravan has been to";
            return false;
        }
        if (cell.x >= 0 && !there->map().inBounds(cell)) {
            error = "that cell is off the map";
            return false;
        }
    } else if (arrival == PodArrival::Caravan) {
        if (home) {
            error = "the pods are at the colony: land them instead";
            return false;
        }
        if (!world_->canVisit(command.tile)) {
            error = "there is no ground to come down on there";
            return false;
        }
        if (!pods_->hasPassengersAboard(*group)) {
            error = "a caravan needs a colonist in the pods";
            return false;
        }
    } else {
        if (settlement == kNoSettlement) {
            error = "only a settlement takes a gift";
            return false;
        }
        if (pods_->hasPassengersAboard(*group)) {
            error = "a gift can't carry colonists";
            return false;
        }
        if (group->loaded.empty()) {
            error = "nothing is in the pods to give";
            return false;
        }
    }
    return pods_->launch(command.group, home ? -1 : command.tile, arrival, cell, error);
}

void Sim::landPods() {
    std::vector<PodFlight> arrived = pods_->takeArrived();
    for (PodFlight& f : arrived) {
        landFlight(f);
    }
}

void Sim::landFlight(PodFlight& f) {
    entt::registry& reg = things_->registry();
    const auto arrived = [&](const char* what, CellPos cell, int caravan) {
        Event e;
        e.type = EventType::PodsArrived;
        e.count = f.id;
        e.amount = static_cast<float>(f.to);
        e.message = what;
        e.cell = cell;
        e.part = caravan;
        events_.push_back(std::move(e));
    };
    const bool home = f.to < 0 || (world_ != nullptr && f.to == world_->startTile());
    const int settlement = home || world_ == nullptr ? kNoSettlement : world_->tile(f.to).settlement;
    // What the place allows now (a map can't go away yet, but the check is cheap); otherwise passengers form a
    // caravan, and goods nobody rides with are lost.
    PodArrival arrival = f.arrival;
    if (arrival == PodArrival::Land && !home && (settlement != kNoSettlement || otherMap(f.to) == nullptr)) {
        arrival = PodArrival::Caravan;
    }
    if (arrival == PodArrival::Gift && (settlement == kNoSettlement || !f.pawns.empty())) {
        arrival = PodArrival::Caravan;
    }
    if (arrival == PodArrival::Caravan && (home || f.pawns.empty() || world_ == nullptr || !world_->canVisit(f.to))) {
        arrival = home ? PodArrival::Land : PodArrival::Gift;
        if (arrival == PodArrival::Gift && (settlement == kNoSettlement || !f.pawns.empty())) {
            for (const Entity e : f.items) {
                things_->destroy(e);
            }
            for (const Entity e : f.pawns) {
                things_->destroy(e);
            }
            arrived("lost", CellPos{}, -1);
            return;
        }
    }
    if (arrival == PodArrival::Land && home) {
        const CellPos at = pods_->landHere(f.pawns, f.items, f.cell);
        arrived("landed", at, -1);
        return;
    }
    if (arrival == PodArrival::Land) {
        Sim& there = *otherMap(f.to);
        std::vector<Entity> roots = f.pawns;
        roots.insert(roots.end(), f.items.begin(), f.items.end());
        std::vector<Entity> made;
        std::string error;
        if (moveOffMapThings(*this, roots, there, std::vector<Entity>(roots.size(), kNoEntity), true, made, error)) {
            for (size_t i = 0; i < f.pawns.size(); ++i) {
                AwayPawn& away = reg.get<AwayPawn>(roots[i]);
                away.tile = f.to;
                away.there = made[i];
            }
            const std::vector<Entity> pawns(made.begin(), made.begin() + static_cast<std::ptrdiff_t>(f.pawns.size()));
            const std::vector<Entity> items(made.begin() + static_cast<std::ptrdiff_t>(f.pawns.size()), made.end());
            const CellPos at = there.pods_->landHere(pawns, items, f.cell);
            arrived("landed", at, -1);
            return;
        }
        arrival = f.pawns.empty() ? PodArrival::Gift : PodArrival::Caravan;  // can't happen: the things are off the map
    }
    if (arrival == PodArrival::Gift) {
        double value = 0.0;
        for (const Entity e : f.items) {
            const Thing& thing = reg.get<Thing>(e);
            const Def& def = defs_->get(thing.def);
            const StackCount* stack = reg.try_get<StackCount>(e);
            value += def.number("stats.marketValue", def.defName() == "Silver" ? 1.0 : 0.0) * (stack != nullptr ? stack->count : 1);
        }
        for (const Entity e : f.items) {
            things_->destroy(e);
        }
        std::string ignored;
        trade_->adjustGoodwill(world_->settlements()[static_cast<size_t>(settlement)].faction,
                               static_cast<float>(value / trade_->rules().giftSilverPerPoint), ignored);
        arrived("gift", CellPos{}, -1);
        return;
    }
    // A caravan waiting on the tile: what came in the pods, ready to move on or walk onto the map there.
    Caravan c;
    c.state = CaravanState::Waiting;
    c.tile = f.to;
    c.cameFrom = f.from >= 0 ? f.from : f.to;
    for (const Entity pawn : f.pawns) {
        CaravanWalker w;
        w.pawn = pawn;
        w.walk = 3;
        c.members.push_back(w);
    }
    c.items = f.items;
    c.formedAt = time_.seconds;
    const int caravan = caravans_->adopt(std::move(c));
    arrived("caravan", CellPos{}, caravan);
}

}  // namespace colony

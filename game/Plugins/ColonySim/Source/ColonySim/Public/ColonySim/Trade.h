#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class CombatSystem;
class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class RaidSystem;
class Rng;
class StateHasher;
class TalentSystem;
class Things;
struct GameTime;

// Faction goodwill, visitors, traders and trade (task C23). See docs/design/trade.md.
//
// Other factions' people come to the colony in groups ("visits"): visitors who walk in, stay a while and leave; a trade
// caravan, a trader with guards who walks in, waits for the colony to trade and leaves; and an orbital trader, a ship
// that has no pawns, only a stock, for a few hours. A visit is started by `startVisit` (T1's incidents call it; the
// StartVisitCommand is the dev entry point). The colony trades with `TradeCommand`: goods it buys and sells at RimWorld's
// prices, paid in silver items. Unreal does not include this header: it reads `visits` and `goodwill` from the snapshot
// and sends TradeCommand.

enum class VisitKind : uint8_t {
    Visitors,  // a group of the faction's villagers
    Caravan,   // a trader and guards on foot
    Orbital,   // a trade ship: no pawns, a stock for a few hours
    Settlement,  // the people of a faction's settlement, on its own map (docs/design/settlements.md): they live there
};

enum class VisitPhase : uint8_t {
    Staying,  // here: the colony may trade with a trader
    Leaving,  // walking off the map
    Defending,  // a settlement whose faction is hostile: its people fight for it and nobody trades
};

COLONY_API const char* visitKindName(VisitKind kind);
COLONY_API bool visitKindFromName(const std::string& name, VisitKind& out);
COLONY_API const char* visitPhaseName(VisitPhase phase);

// What a visiting pawn is.
enum class VisitorRole : uint8_t { Visitor, Trader, Guard };

// On every pawn of a visit. These pawns keep their faction but are nobody's enemy: fighters, turrets, the alarm and
// wardens leave them alone. They walk as fighters in the Flee mode (so they never shoot), and the trade system gives their
// goals. Every field is saved.
struct Visitor {
    uint32_t visit = 0;
    VisitorRole role = VisitorRole::Visitor;
};

// A thing for sale or sold, by Things defName.
struct TradeLine {
    std::string thing;
    int count = 0;
};

// One visit. Saved as a whole.
struct Visit {
    uint32_t id = 0;
    VisitKind kind = VisitKind::Visitors;
    VisitPhase phase = VisitPhase::Staying;
    std::string faction;      // Factions defName
    std::string traderKind;   // TraderKinds defName; empty for visitors
    uint32_t side = 0;        // its pawns' Faction id
    int64_t arrivedAt = 0;    // game second
    int64_t leaveAt = 0;      // it starts to leave at this game second
    int64_t nextWander = 0;   // visitors move to a new spot at this game second
    CellPos entry;            // where it walked in
    CellPos spot;             // where it gathers (the trader stands here; a ship drops goods here)
    int silver = 0;           // what the trader can pay with
    std::vector<TradeLine> stock;  // for sale, in defName order; what the colony sells is added to it
    Entity trader = kNoEntity;     // the pawn to trade with; none for a ship, or once it is dead
    std::vector<Entity> members;   // in id order; the dead and those that left are dropped
    int started = 0;
    // A settlement's (saved from trade section 2): when its trader's stock and silver are rolled again, and how far from the
    // spot its people walk (half the base).
    int64_t restockAt = 0;
    int range = 0;
};

// What one faction thinks of the colony: goodwill from -100 to 100. Only factions whose goodwill has changed are listed;
// the others sit at their start (the middle of their `startingGoodwill` range).
struct Standing {
    std::string faction;
    float goodwill = 0.0f;
};

// A trade caravan the colony asked a faction for over the comms console (docs/design/comms-console.md). Saved in section "comms".
struct CaravanRequest {
    std::string faction;     // Factions defName
    std::string traderKind;  // TraderKinds defName; empty for one of the faction's by chance
    int64_t arriveAt = 0;    // game second it walks in
};

// When the colony last asked a faction for a caravan (the request's cooldown). Saved in section "comms".
struct CommsCall {
    std::string faction;
    int64_t at = 0;
};

struct TradeState {
    std::vector<Visit> visits;       // in id order
    uint32_t nextVisit = 1;
    std::vector<Standing> standings; // in faction name order
    int64_t lastDrift = 0;           // the game day goodwill last drifted back toward its start
    std::vector<CaravanRequest> requests;  // in arrival order (then faction name)
    std::vector<CommsCall> calls;          // in faction name order
};

// What a faction is to the colony, by goodwill (FactionRules).
enum class Relation : uint8_t { Hostile, Neutral, Ally };

// The numbers from TradeRules and FactionRules (data/defs/Rules). `present` is false when the defs have none.
struct COLONY_API TradeRules {
    bool present = false;
    float colonySellsAt = 0.6f;   // the share of market value a trader pays for what the colony sells (RimWorld wiki)
    float colonyBuysAt = 1.4f;    // and charges for what it sells
    std::map<std::string, float> sellCategoryFactors{{"Furniture", 0.7f}, {"Sculptures", 1.1f}, {"Weapons", 0.2f}, {"Techprints", 0.1f}};
    std::vector<std::string> alwaysAccepted{"Silver", "ComponentIndustrial", "MealSurvivalPack"};
    // Social skill against the price improvement (wiki: 1.5% per level, 30% at 20), and the most any pawn can reach.
    std::vector<std::pair<float, float>> improvementBySocial{{0.0f, 0.0f}, {20.0f, 0.3f}};
    float improvementCap = 0.395f;
    float improvementFloor = -0.5f;   // ours
    float orbitalStayHours = 16.0f;
    float caravanStayHoursMin = 12.0f;   // ours
    float caravanStayHoursMax = 20.0f;
    float visitorStayHoursMin = 6.0f;    // ours
    float visitorStayHoursMax = 14.0f;
    int visitorCountMin = 2;             // ours
    int visitorCountMax = 5;
    int guardCountMin = 2;               // ours
    int guardCountMax = 4;
    float wanderRadius = 9.0f;           // visitors drift this far from the gathering spot
    int wanderEverySeconds = 900;        // ours
    int edgeDepth = 2;                   // cells in from the map edge a visit walks in at
    int leaveTimeoutSeconds = 21600;     // pawns still on the map this long after leaving are gone
    int dropRadius = 6;                  // goods are put down within this many cells of the spot
    float settlementDiscount = 0.02f;    // trading at a settlement adds this to the price improvement (RimWorld wiki)
    float settlementRestockDays = 30.0f; // a settlement's trader rolls its stock again this often (recalled from RimWorld)
    // The comms console's caravan request (TradeRules.comms; wiki: an ally only, 19 goodwill, 4 days between).
    float requestGoodwillCost = 19.0f;
    float requestCooldownDays = 4.0f;
    float requestArrivalDays = 2.0f;     // recalled from RimWorld's code (120,000 ticks), unverified
    float beaconRadius = 7.9f;           // a beacon whose def has no building.tradeRadius
    // FactionRules.
    float hostileAtOrBelow = -75.0f;
    float alliedAtOrAbove = 75.0f;
    float goodwillMin = -100.0f;
    float goodwillMax = 100.0f;
    float driftPerDay = 0.2f;
    float releaseGoodwill = 12.0f;        // a released prisoner's faction likes the colony this much more (recalled)
    float giftSilverPerPoint = 40.0f;     // a gift gives 1 goodwill per this much market value (recalled)
    float visitorKilledGoodwill = -25.0f; // a visitor dying in the colony costs this (ours)
    float executedGoodwill = -10.0f;      // an executed prisoner costs its faction's goodwill this (ours)

    static TradeRules read(const DefDatabase& defs);
};

class COLONY_API TradeSystem {
public:
    TradeSystem(Things& things, Map& map, CombatSystem& combat, FighterSystem& fighters, HealthSystem& health, JobSystem& jobs, RaidSystem& raids,
                const DefDatabase& defs, const GameTime& time, Rng& rng, uint64_t seed);

    const TradeRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Gives a new visitor its looks (Sim::giveLooks): pawn, seed, female.
    void setLooksMaker(std::function<void(Entity, uint64_t, bool)> maker) { looks_ = std::move(maker); }
    // The protagonist's talents make a better haggler (stat "tradePrice"). Without them nobody has any.
    void setTalents(const TalentSystem* talents) { talents_ = talents; }
    // The difficulty's trade price disadvantage (0 to 0.2): taken off every negotiator's price improvement.
    void setPriceDisadvantage(float disadvantage) { disadvantage_ = disadvantage; }
    // Whether a building does its job (PowerGrid::running): a comms console and a trade beacon need power. Without it
    // every built one counts.
    void setRunning(std::function<bool(Entity)> running) { running_ = std::move(running); }

    // ---- Goodwill ----
    // A faction's goodwill with the colony now.
    float goodwill(const std::string& faction) const;
    // Changes it by `delta`, within the limits; a permanent enemy stays at its lowest. False for an unknown faction.
    bool adjustGoodwill(const std::string& faction, float delta, std::string& error);
    Relation relation(const std::string& faction) const;
    // Whether the faction is the colony's enemy now (a permanent enemy, or goodwill at -75 or less): raids may come from it.
    bool hostile(const std::string& faction) const { return relation(faction) == Relation::Hostile; }
    // The Factions defName a pawn belongs to (empty for the player's side and for sides that are not a faction).
    std::string factionOf(Entity pawn) const;
    // The colony gives the faction of visit `visit` the `items` lying on the map; its goodwill rises by their market value over
    // `giftSilverPerPoint`. A caravan's trader or a ship adds the goods to its stock. All or nothing.
    bool gift(uint32_t visit, const std::vector<TradeLine>& items, std::string& error);
    // A prisoner was put to death: its faction's goodwill drops (`executedGoodwill`). The executions task calls it.
    void noteExecuted(Entity prisoner);

    // ---- Visits ----
    // A visit of `faction` walks in at `edge` (0 north, 1 east, 2 south, 3 west, -1 any) or, for a ship, arrives.
    // `traderKind` names a TraderKinds def for a caravan or ship (empty: one of the faction's kinds for that way, by
    // chance). Fails, with a reason in plain words, for a faction that can't visit or trade, is hostile to the colony or has
    // no such kind. `id` is the new visit's.
    bool startVisit(const std::string& faction, VisitKind kind, const std::string& traderKind, int edge, uint32_t& id,
                    std::string& error);
    const Visit* find(uint32_t id) const;
    const TradeState& state() const { return state_; }
    // The people of a faction's settlement (docs/design/settlements.md), made with its map: `residents` pawns around `spot`, who
    // walk within `range` cells of it for good. A faction at peace with the colony has a trader of one of its `traders.base`
    // kinds standing at the spot (when it trades), guards and villagers; a hostile one has fighters of its `pawnKinds`
    // who defend the base. `id` is the new visit's.
    bool startSettlement(const std::string& faction, CellPos spot, int range, int residents, uint32_t& id, std::string& error);
    // Another map's trade system asks the colony's for goodwill: the colony's standings are the only ones, so gifts, a
    // killed visitor and hostility on that map are the colony's. Not saved: the colony sets it when the map is made or loaded.
    void setGoodwillOwner(TradeSystem* owner) { owner_ = owner; }

    // ---- Comms console and trade beacons (docs/design/comms-console.md) ----
    // A built, powered comms console. With `user`, one that pawn can walk up to; without (kNoEntity), any. kNoEntity if none.
    Entity commsConsole(Entity user = kNoEntity) const;
    // The built, powered orbital trade beacons, in id order.
    std::vector<Entity> tradeBeacons() const;
    // The cells an orbital trader buys from: within each powered beacon's radius and not cut off from it by a wall or a
    // door. Sorted.
    std::vector<CellPos> beaconCells() const;
    // How many of a thing lie on the beacons' cells (what a ship can be sold or given).
    int beaconHas(const std::string& thing) const;
    // `negotiator` uses a comms console to ask `faction` for a trade caravan (`traderKind`, or empty for any of its own). The
    // faction must be an ally and not asked in the last `requestCooldownDays`; it costs `requestGoodwillCost` goodwill and
    // the caravan walks in `requestArrivalDays` later. False, with a reason in plain words, when it can't be done.
    bool requestCaravan(const std::string& faction, const std::string& traderKind, Entity negotiator, std::string& error);

    // ---- Trade ----
    // How many silver the colony has: the silver lying on the map.
    int silverOnMap() const;
    // How many of a thing the colony has to sell: the stacks lying on the map.
    int colonyHas(const std::string& thing) const;
    // The share a negotiator's skill and talents take off what the colony pays and add to what it gets (a pawn without a
    // Social skill, or none, counts as level 0).
    float priceImprovement(Entity negotiator) const;
    // Whether this visit's trader takes `def` from the colony.
    bool accepts(const Visit& visit, const Def& def) const;
    // The price of one unit: what the colony pays a trader for it (`colonyBuys`), or gets (not rounded).
    float unitPrice(const Def& def, bool colonyBuys, float improvement) const;
    // The trade, as the TradeCommand does it. The colony buys `buy` from the trader's stock and sells `sell` from what lies
    // on the map; the difference is paid in silver items (a trader with less silver than it owes takes the goods that are
    // left over for nothing). All or nothing. `negotiator` is a colonist of the player.
    bool trade(uint32_t visit, Entity negotiator, const std::vector<TradeLine>& buy, const std::vector<TradeLine>& sell,
               std::string& error);

    // Visitors move about, groups leave when their time is up, pawns that reach the edge are gone, goodwill drifts. Runs once a step.
    // `events` are this step's: a released prisoner pleases its faction, a visitor's death angers it.
    void step(const std::vector<Event>& events);

    // Call before a thing is destroyed: it leaves its visit.
    void forget(Entity thing);
    // After loading a save. False if it doesn't fit the defs.
    bool restore(TradeState state, std::string& error);
    void hashState(StateHasher& h) const;
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct KindInfo {
        std::string defName;
        std::string race;
        std::vector<std::string> weapons;
        struct Wear {
            std::string thing;
            float chance = 1.0f;
        };
        std::vector<Wear> apparel;
    };

    int64_t now() const;
    // After goodwill fell: a faction that is hostile now sends its visits away.
    void sendAwayIfHostile(const std::string& faction);
    float startGoodwill(const std::string& faction) const;
    bool readKind(const std::string& kindName, KindInfo& out, std::string& error) const;
    bool pickEntry(const CellPos& target, int edge, CellPos& entry) const;
    // Adds `count` pawn kinds from the faction's `list` (names, or {kind, weight}) to `out`, by weight.
    bool pickKinds(const Def& faction, const char* list, VisitorRole role, int count, std::vector<std::pair<std::string, VisitorRole>>& out,
                   std::string& error) const;
    // Open cells a metre apart (every other cell) reachable from `from`, nearest first.
    std::vector<CellPos> cellsAround(CellPos from, size_t count) const;
    // Makes the pawns of `bring` in `cells` and joins them to the visit; `defending` makes hostile fighters, not visitors.
    void spawnGroup(Visit& visit, const std::vector<std::pair<std::string, VisitorRole>>& bring, const std::vector<KindInfo>& infos,
                    const std::vector<CellPos>& cells, bool defending);
    bool spawnPawn(Visit& visit, const KindInfo& kind, VisitorRole role, CellPos cell, uint64_t base, bool defending = false);
    // A trader's silver and stock, rolled from its kind (a settlement's again every `settlementRestockDays`).
    void rollStock(Visit& visit);
    void leave(Visit& visit);
    void wander(Visit& visit);
    // A settlement's people fight for it from now on: they are no longer visitors, and nobody trades there.
    void defend(Visit& visit);
    // A cell for a settlement's pawn within its range of the spot, by chance.
    bool settlementCell(const Visit& visit, CellPos& out) const;
    CellPos edgeGoal(Entity pawn) const;
    bool nearestFree(float x, float y, CellPos from, int radius, CellPos& out) const;
    void dropGoods(CellPos at, const std::string& thing, int count);
    // Where a visit's bought goods go down: its spot, or for a ship the first beacon.
    CellPos dropSpot(const Visit& visit) const;
    // Up to `count` of a thing from the stacks lying on `cells` (all the map when null), lowest id first. Returns how many.
    int takeFromMap(const std::string& thing, int count, const std::vector<CellPos>* cells = nullptr);
    int countOnMap(const std::string& thing, const std::vector<CellPos>* cells) const;
    // A ship needs a powered console (one `negotiator` can reach, if given) and a powered beacon. Empty when it may trade.
    std::string orbitalBlocker(Entity negotiator) const;
    bool builtAndRunning(Entity building) const;
    void arriveRequested();
    std::vector<std::string> wordsOf(const Def& def) const;
    float sellFactor(const Def& def) const;
    void emit(EventType type, uint32_t visit, Entity thing, float amount, const std::string& message) const;

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    FighterSystem& fighters_;
    HealthSystem& health_;
    JobSystem& jobs_;
    RaidSystem& raids_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Rng& rng_;
    uint64_t seed_;
    TradeRules rules_;
    TradeState state_;
    const TalentSystem* talents_ = nullptr;
    float disadvantage_ = 0.0f;
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity, uint64_t, bool)> looks_;
    std::function<bool(Entity)> running_;
    TradeSystem* owner_ = nullptr;
};

}  // namespace colony

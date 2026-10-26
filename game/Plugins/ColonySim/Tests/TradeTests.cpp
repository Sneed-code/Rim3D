#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Talents.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure. The expression is
// evaluated twice, so it must have no effects.
#define TRD_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/trade.md are what is tested.
std::shared_ptr<const DefDatabase> trdDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim trdSim() { return Sim(SimSettings{7, 128, 0, 0, true}, trdDefs()); }

Entity trdColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    return pawn;
}

// Gives the pawn a Social skill of this level (a plain spawned human has no skills).
void trdSocial(Sim& sim, Entity pawn, int level) {
    Colonist colonist;
    SkillRecord social;
    social.def = "Social";
    social.level = level;
    colonist.skills.push_back(social);
    sim.things().registry().emplace_or_replace<Colonist>(pawn, std::move(colonist));
}

// Puts `count` of a thing on the map, in stacks, near the cell.
void trdPutDown(Sim& sim, const char* def, int count, CellPos cell) {
    const int limit = static_cast<int>(trdDefs()->find("Things", def)->integer("stackLimit", 1));
    for (int i = 0; count > 0; ++i) {
        const int n = std::min(count, limit);
        sim.things().spawn(def, CellPos{cell.x + i % 8, cell.y + i / 8, 0}, n);
        count -= n;
    }
}

// Start an orbital trader of the Fray Compact and return the visit.
const Visit* trdShip(Sim& sim, const char* kind = "BulkGoods") {
    uint32_t id = 0;
    std::string error;
    if (!sim.trade().startVisit("FrayCompact", VisitKind::Orbital, kind, -1, id, error)) {
        MESSAGE(error);
        return nullptr;
    }
    return sim.trade().find(id);
}

int trdStock(const Visit& visit, const std::string& thing) {
    for (const TradeLine& l : visit.stock) {
        if (l.thing == thing) {
            return l.count;
        }
    }
    return 0;
}

// Replaces the visit's trader silver (a test setup the game has no command for).
void trdSetSilver(Sim& sim, uint32_t visit, int silver) {
    TradeState state = sim.trade().state();
    for (Visit& v : state.visits) {
        if (v.id == visit) {
            v.silver = silver;
        }
    }
    std::string error;
    sim.trade().restore(state, error);
}

}  // namespace

TEST_CASE("Trade: goodwill starts inside each faction's range, moves within the limits and sets the relation") {
    Sim sim = trdSim();
    TradeSystem& trade = sim.trade();
    for (const DefId id : trdDefs()->idsOfKind("Factions")) {
        const Def& f = trdDefs()->get(id);
        const float g = trade.goodwill(f.defName());
        CHECK_MESSAGE(g >= f.number("startingGoodwill.0") - 0.01, f.defName());
        CHECK_MESSAGE(g <= f.number("startingGoodwill.1") + 0.01, f.defName());
    }
    CHECK(trade.goodwill("Pirate") == -100.0f);
    CHECK(trade.relation("Pirate") == Relation::Hostile);
    // The same seed gives the same starts.
    Sim other = trdSim();
    CHECK(other.trade().goodwill("FrayCompact") == trade.goodwill("FrayCompact"));

    std::string error;
    CHECK(trade.adjustGoodwill("FrayCompact", -500.0f, error));
    CHECK(trade.goodwill("FrayCompact") == -100.0f);
    CHECK(trade.relation("FrayCompact") == Relation::Hostile);
    CHECK(trade.adjustGoodwill("FrayCompact", 500.0f, error));
    CHECK(trade.goodwill("FrayCompact") == 100.0f);
    CHECK(trade.relation("FrayCompact") == Relation::Ally);
    CHECK(trade.adjustGoodwill("Pirate", 50.0f, error));
    CHECK(trade.goodwill("Pirate") == -100.0f);  // a permanent enemy stays there
    CHECK_FALSE(trade.adjustGoodwill("Nobody", 1.0f, error));
    CHECK(error.find("Nobody") != std::string::npos);
}

TEST_CASE("Trade: goodwill drifts back toward where the faction started") {
    Sim sim = trdSim();
    TradeSystem& trade = sim.trade();
    const float start = trade.goodwill("FrayCompact");
    std::string error;
    trade.adjustGoodwill("FrayCompact", 30.0f, error);
    const float raised = trade.goodwill("FrayCompact");
    CHECK(raised > start);
    sim.stepMany(86400 * 2 + 5);
    const float later = trade.goodwill("FrayCompact");
    CHECK(later < raised);
    CHECK(later == doctest::Approx(raised - 0.4f).epsilon(0.01));
}

TEST_CASE("Trade: factions that are hostile or don't trade or visit send nobody") {
    Sim sim = trdSim();
    uint32_t id = 0;
    std::string error;
    CHECK_FALSE(sim.trade().startVisit("Pirate", VisitKind::Visitors, "", -1, id, error));
    CHECK_FALSE(sim.trade().startVisit("Pirate", VisitKind::Orbital, "", -1, id, error));
    CHECK_FALSE(sim.trade().startVisit("Nobody", VisitKind::Visitors, "", -1, id, error));
    sim.trade().adjustGoodwill("FrayCompact", -200.0f, error);
    CHECK_FALSE(sim.trade().startVisit("FrayCompact", VisitKind::Orbital, "", -1, id, error));
    CHECK(error.find("hostile") != std::string::npos);
    CHECK(sim.trade().state().visits.empty());
    // Through a command the failure is reported.
    sim.submit(StartVisitCommand{"FrayCompact", "Orbital", "", -1});
    sim.submit(StartVisitCommand{"FreeHold", "Sailing", "", -1});
    bool failed = false;
    for (int i = 0; i < 3; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            failed = failed || e.type == EventType::CommandFailed;
        }
    }
    CHECK(failed);
}

TEST_CASE("Trade: prices are RimWorld's 140% to buy and 60% to sell, bettered by Social skill, talents and the category") {
    Sim sim = trdSim();
    const TradeSystem& trade = sim.trade();
    const Def& steel = *trdDefs()->find("Things", "Steel");
    const Def& sword = *trdDefs()->find("Things", "Melee_Longsword");
    const float mv = static_cast<float>(steel.number("stats.marketValue"));
    CHECK(trade.unitPrice(steel, true, 0.0f) == doctest::Approx(mv * 1.4f));
    CHECK(trade.unitPrice(steel, false, 0.0f) == doctest::Approx(mv * 0.6f));
    CHECK(trade.unitPrice(steel, true, 0.3f) == doctest::Approx(mv * 1.4f * 0.7f));
    CHECK(trade.unitPrice(steel, false, 0.3f) == doctest::Approx(mv * 0.6f * 1.3f));
    // Weapons fetch a fifth of that.
    CHECK(trade.unitPrice(sword, false, 0.0f) == doctest::Approx(static_cast<float>(sword.number("stats.marketValue")) * 0.6f * 0.2f));
    // The negotiator: more Social is a better price; the ceiling holds.
    const Entity dull = trdColonist(sim, CellPos{20, 20, 0});
    trdSocial(sim, dull, 0);
    const Entity smooth = trdColonist(sim, CellPos{22, 20, 0});
    trdSocial(sim, smooth, 20);
    CHECK(trade.priceImprovement(dull) == doctest::Approx(0.0f));
    CHECK(trade.priceImprovement(smooth) == doctest::Approx(0.3f));
    CHECK(trade.priceImprovement(kNoEntity) <= 0.0f);
}

TEST_CASE("Trade: a ship arrives with stock and silver, stays 16 hours and leaves") {
    Sim sim = trdSim();
    trdColonist(sim, CellPos{60, 60, 0});
    const Visit* ship = trdShip(sim);
    TRD_REQUIRE(ship != nullptr);
    const uint32_t id = ship->id;
    CHECK(ship->kind == VisitKind::Orbital);
    CHECK(ship->members.empty());
    CHECK(ship->silver >= 750);
    CHECK(ship->silver <= 1200);
    CHECK(trdStock(*ship, "Steel") >= 250);
    CHECK(trdStock(*ship, "Steel") <= 400);
    CHECK(ship->leaveAt - ship->arrivedAt == 16 * 3600);
    // Unreal sees it.
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    TRD_REQUIRE(snapshot.visits.size() == 1);
    CHECK(snapshot.visits[0].faction == "FrayCompact");
    CHECK(snapshot.visits[0].kind == static_cast<uint8_t>(VisitKind::Orbital));
    CHECK_FALSE(snapshot.visits[0].stock.empty());
    CHECK(snapshot.goodwill.size() == trdDefs()->idsOfKind("Factions").size());
    // It goes when its time is up.
    sim.stepMany(16 * 3600 + 30);
    CHECK(sim.trade().find(id) == nullptr);
    CHECK(sim.trade().state().visits.empty());
}

TEST_CASE("Trade: the colony buys goods with silver at the quoted price") {
    Sim sim = trdSim();
    const Entity colonist = trdColonist(sim, CellPos{60, 60, 0});
    trdSocial(sim, colonist, 0);
    const Visit* ship = trdShip(sim);
    TRD_REQUIRE(ship != nullptr);
    const uint32_t id = ship->id;
    const int stockBefore = trdStock(*ship, "Steel");
    const int traderSilver = ship->silver;
    trdPutDown(sim, "Silver", 300, CellPos{50, 50, 0});
    TRD_REQUIRE(sim.trade().silverOnMap() == 300);
    const float mv = static_cast<float>(trdDefs()->find("Things", "Steel")->number("stats.marketValue"));
    const int cost = static_cast<int>(std::ceil(mv * 1.4f * 100.0f - 1e-4f));

    std::string error;
    CHECK_FALSE(sim.trade().trade(id, colonist, {TradeLine{"Steel", stockBefore + 1}}, {}, error));  // more than it has
    CHECK(error.find("hasn't") != std::string::npos);
    CHECK_FALSE(sim.trade().trade(id, colonist, {TradeLine{"Silver", 5}}, {}, error));  // money is not goods
    CHECK_FALSE(sim.trade().trade(id, colonist, {TradeLine{"Nothing", 5}}, {}, error));
    CHECK_FALSE(sim.trade().trade(id, kNoEntity, {TradeLine{"Steel", 1}}, {}, error));  // nobody to haggle
    CHECK_FALSE(sim.trade().trade(id, colonist, {}, {}, error));
    CHECK(sim.trade().silverOnMap() == 300);  // all or nothing

    sim.submit(TradeCommand{id, colonist, {TradeLine{"Steel", 100}}, {}});
    sim.step();
    CHECK(sim.trade().silverOnMap() == 300 - cost);
    CHECK(sim.trade().colonyHas("Steel") == 100);
    const Visit* after = sim.trade().find(id);
    TRD_REQUIRE(after != nullptr);
    CHECK(trdStock(*after, "Steel") == stockBefore - 100);
    CHECK(after->silver == traderSilver + cost);

    // Not enough silver: refused, nothing changes.
    CHECK_FALSE(sim.trade().trade(id, colonist, {TradeLine{"Steel", 100}}, {}, error));
    CHECK(error.find("silver") != std::string::npos);
    CHECK(sim.trade().colonyHas("Steel") == 100);
}

TEST_CASE("Trade: the colony sells what the trader takes, paid up to the silver it has; the rest goes free") {
    Sim sim = trdSim();
    const Entity colonist = trdColonist(sim, CellPos{60, 60, 0});
    trdSocial(sim, colonist, 0);
    const Visit* ship = trdShip(sim);
    TRD_REQUIRE(ship != nullptr);
    const uint32_t id = ship->id;
    trdPutDown(sim, "Steel", 200, CellPos{50, 50, 0});
    sim.things().spawn("Melee_Longsword", CellPos{56, 56, 0});
    const float mv = static_cast<float>(trdDefs()->find("Things", "Steel")->number("stats.marketValue"));
    const int worth = static_cast<int>(std::floor(mv * 0.6f * 100.0f + 1e-4f));
    std::string error;

    // A bulk goods trader buys metals but not swords.
    CHECK_FALSE(sim.trade().trade(id, colonist, {}, {TradeLine{"Melee_Longsword", 1}}, error));
    CHECK(error.find("won't take") != std::string::npos);
    CHECK_FALSE(sim.trade().trade(id, colonist, {}, {TradeLine{"Steel", 500}}, error));  // more than the colony has
    CHECK(sim.trade().colonyHas("Steel") == 200);

    const int traderSilver = sim.trade().find(id)->silver;
    CHECK(sim.trade().trade(id, colonist, {}, {TradeLine{"Steel", 100}}, error));
    CHECK(sim.trade().colonyHas("Steel") == 100);
    CHECK(sim.trade().silverOnMap() == worth);
    CHECK(sim.trade().find(id)->silver == traderSilver - worth);
    CHECK(trdStock(*sim.trade().find(id), "Steel") >= 100);

    // A trader with only 20 silver left pays 20 for the lot and takes the rest free.
    trdSetSilver(sim, id, 20);
    CHECK(sim.trade().trade(id, colonist, {}, {TradeLine{"Steel", 100}}, error));
    CHECK(sim.trade().colonyHas("Steel") == 0);
    CHECK(sim.trade().silverOnMap() == worth + 20);
    CHECK(sim.trade().find(id)->silver == 0);
}

TEST_CASE("Trade: a good negotiator pays less and is paid more") {
    Sim sim = trdSim();
    const Entity dull = trdColonist(sim, CellPos{60, 60, 0});
    trdSocial(sim, dull, 0);
    const Entity smooth = trdColonist(sim, CellPos{62, 60, 0});
    trdSocial(sim, smooth, 20);
    const Visit* ship = trdShip(sim);
    TRD_REQUIRE(ship != nullptr);
    const uint32_t id = ship->id;
    trdPutDown(sim, "Silver", 5000, CellPos{50, 50, 0});
    std::string error;
    CHECK(sim.trade().trade(id, dull, {TradeLine{"Steel", 100}}, {}, error));
    const int dullCost = 5000 - sim.trade().silverOnMap();
    CHECK(sim.trade().trade(id, smooth, {TradeLine{"Steel", 100}}, {}, error));
    const int smoothCost = 5000 - dullCost - sim.trade().silverOnMap();
    CHECK(smoothCost < dullCost);
    CHECK(smoothCost == doctest::Approx(dullCost * 0.7).epsilon(0.01));
}

TEST_CASE("Trade: a trade caravan walks in with a trader and guards, nobody shoots them, and they leave") {
    Sim sim = trdSim();
    const Entity colonist = trdColonist(sim, CellPos{64, 64, 0});
    trdSocial(sim, colonist, 0);
    uint32_t id = 0;
    std::string error;
    const bool started = sim.trade().startVisit("FrayCompact", VisitKind::Caravan, "BulkGoods", 0, id, error);
    TRD_REQUIRE(started);
    const Visit* caravan = sim.trade().find(id);
    TRD_REQUIRE(caravan != nullptr);
    CHECK(caravan->members.size() >= 3);  // the trader and 2 to 4 guards
    CHECK(caravan->members.size() <= 5);
    TRD_REQUIRE(caravan->trader != kNoEntity);
    entt::registry& reg = sim.things().registry();
    int guards = 0;
    for (const Entity m : caravan->members) {
        const Visitor* v = reg.try_get<Visitor>(m);
        TRD_REQUIRE(v != nullptr);
        CHECK(v->visit == id);
        CHECK(sim.fighters().factionOf(m) != kPlayerFaction);
        guards += v->role == VisitorRole::Guard ? 1 : 0;
    }
    CHECK(guards == static_cast<int>(caravan->members.size()) - 1);
    CHECK(reg.get<Visitor>(caravan->trader).role == VisitorRole::Trader);
    const Entity trader = caravan->trader;

    // They reach the colony without a shot or an alarm.
    int shots = 0;
    bool alarm = false;
    for (int i = 0; i < 1800; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            shots += e.type == EventType::ShotFired ? 1 : 0;
        }
        alarm = alarm || sim.raids().state().alarm;
    }
    CHECK(shots == 0);
    CHECK_FALSE(alarm);
    const Position& at = reg.get<Position>(trader);
    const CellPos spot = sim.trade().find(id)->spot;
    CHECK(std::abs(at.x - (spot.x + 0.5f)) < 6.0f);
    CHECK(std::abs(at.y - (spot.y + 0.5f)) < 6.0f);

    // The colony trades with the trader pawn.
    trdPutDown(sim, "Silver", 500, CellPos{50, 50, 0});
    CHECK(sim.trade().trade(id, colonist, {TradeLine{"Steel", 50}}, {}, error));
    CHECK(sim.trade().colonyHas("Steel") == 50);

    // Time is up: they walk to the edge and are gone.
    TradeState state = sim.trade().state();
    state.visits[0].leaveAt = sim.time().seconds + 30;
    const bool restored = sim.trade().restore(state, error);
    TRD_REQUIRE(restored);
    bool left = false;
    for (int i = 0; i < 4000 && sim.trade().find(id) != nullptr; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            left = left || e.type == EventType::VisitLeft;
        }
    }
    CHECK(left);
    CHECK(sim.trade().find(id) == nullptr);
    CHECK_FALSE(reg.valid(trader));
    CHECK(reg.view<Visitor>().empty());
}

TEST_CASE("Trade: a group of visitors stays a while, wanders near the spot and goes, and can't be traded with") {
    Sim sim = trdSim();
    const Entity colonist = trdColonist(sim, CellPos{64, 64, 0});
    uint32_t id = 0;
    std::string error;
    const bool started = sim.trade().startVisit("FrayCompact", VisitKind::Visitors, "", 3, id, error);
    TRD_REQUIRE(started);
    const Visit* group = sim.trade().find(id);
    TRD_REQUIRE(group != nullptr);
    CHECK(group->members.size() >= 2);
    CHECK(group->members.size() <= 5);
    CHECK(group->trader == kNoEntity);
    CHECK(group->leaveAt - group->arrivedAt >= 6 * 3600);
    CHECK(group->leaveAt - group->arrivedAt <= 14 * 3600);
    CHECK_FALSE(sim.trade().trade(id, colonist, {}, {TradeLine{"Steel", 1}}, error));
    CHECK(error.find("nothing to trade") != std::string::npos);
    // Visitors are on the map and the snapshot marks them.
    sim.stepMany(60);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    int marked = 0;
    for (const ThingView& t : snapshot.things) {
        marked += t.visitor && t.visit == id ? 1 : 0;
    }
    CHECK(marked == static_cast<int>(group->members.size()));
    // Nobody harms them: a gun-toting colonist next to them with the alarm raised leaves them alone.
    sim.stepMany(1500);
    for (const Entity m : sim.trade().find(id)->members) {
        CHECK(sim.things().registry().get<Position>(m).x > 1.0f);
    }
    // Gone after the stay.
    TradeState state = sim.trade().state();
    state.visits[0].leaveAt = sim.time().seconds + 15;
    const bool restored = sim.trade().restore(state, error);
    TRD_REQUIRE(restored);
    sim.stepMany(4000);
    CHECK(sim.trade().find(id) == nullptr);
}

TEST_CASE("Trade: a visit, its stock and goodwill save and load, and the loaded game goes on the same") {
    Sim sim = trdSim();
    const Entity colonist = trdColonist(sim, CellPos{64, 64, 0});
    uint32_t id = 0;
    std::string error;
    const bool started = sim.trade().startVisit("FrayCompact", VisitKind::Caravan, "CombatSupplier", 1, id, error);
    TRD_REQUIRE(started);
    sim.trade().adjustGoodwill("FreeHold", 12.0f, error);
    sim.stepMany(400);
    trdPutDown(sim, "Silver", 3000, CellPos{50, 50, 0});
    CHECK(sim.trade().trade(id, colonist, {TradeLine{"MedicineIndustrial", 5}}, {}, error));
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, trdDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    TRD_REQUIRE(loaded.error.empty());
    TRD_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    const Visit* a = sim.trade().find(id);
    const Visit* b = copy.trade().find(id);
    TRD_REQUIRE(a != nullptr);
    TRD_REQUIRE(b != nullptr);
    CHECK(b->silver == a->silver);
    CHECK(b->stock.size() == a->stock.size());
    CHECK(b->members == a->members);
    CHECK(b->trader == a->trader);
    CHECK(copy.trade().goodwill("FreeHold") == sim.trade().goodwill("FreeHold"));
    CHECK(saveSim(copy) == bytes);
    sim.stepMany(3000);
    copy.stepMany(3000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(saveSim(copy) == saveSim(sim));
}

TEST_CASE("Trade: a game that never meets a trader hashes as before the system existed") {
    Sim a = trdSim();
    Sim b = trdSim();
    a.stepMany(100);
    b.stepMany(100);
    CHECK(a.stateHash() == b.stateHash());
    // The hash moves once a visit comes.
    const Visit* ship = trdShip(b);
    TRD_REQUIRE(ship != nullptr);
    CHECK(a.stateHash() != b.stateHash());
}

TEST_CASE("Trade: the commands are written into a replay and read back") {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, trdDefs());
    Entity colonist = kNoEntity;
    for (const auto [pawn, c] : recorded.things().registry().view<const Colonist>().each()) {
        (void)c;
        colonist = colonist == kNoEntity || pawn < colonist ? pawn : colonist;
    }
    TRD_REQUIRE(colonist != kNoEntity);
    recorded.stepMany(5);
    recorded.submit(AdjustGoodwillCommand{"FrayCompact", 20.0f});
    recorded.submit(StartVisitCommand{"FrayCompact", "Orbital", "BulkGoods", -1});
    recorded.stepMany(2);
    recorded.submit(TradeCommand{1, colonist, {TradeLine{"Steel", 10}}, {TradeLine{"Silver", 1}}});
    recorded.stepMany(600);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("StartVisit") != std::string::npos);
    CHECK(text.find("AdjustGoodwill") != std::string::npos);
    CHECK(text.find("\"Trade\"") != std::string::npos);
    Replay parsed;
    std::string error;
    const bool read = readReplay(text, parsed, error);
    TRD_REQUIRE(read);
    Sim again(parsed.settings, trdDefs());
    playReplay(parsed, again, replay.endStep);
    const ReplayCheck check = checkReplay(parsed, again);
    CHECK(check.matched());
    CHECK(again.stateHash() == recorded.stateHash());
    CHECK(again.trade().find(1) != nullptr);
}

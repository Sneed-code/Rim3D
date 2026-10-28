#include "ColonySim/Artillery.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Health.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Evaluates its argument once: the tests pass it calls that step the sim.
#define SIEGE_REQUIRE(...)                  \
    do {                                    \
        const bool siegeOk = (__VA_ARGS__); \
        CHECK(siegeOk);                     \
        if (!siegeOk) {                     \
            return;                         \
        }                                   \
    } while (0)

// These tests check docs/design/sieges.md.
namespace {

std::shared_ptr<const DefDatabase> siegeDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> siegeSim(uint64_t seed) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return std::make_unique<Sim>(settings, siegeDefs());
}

std::vector<Entity> siegeColonists(Sim& sim) {
    std::vector<Entity> out;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        if (sim.fighters().factionOf(pawn) == kPlayerFaction) {
            out.push_back(pawn);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The colonists keep out of it (they shelter), so the siege plays out by its own rules.
void siegeShelterAll(Sim& sim) {
    for (const Entity c : siegeColonists(sim)) {
        sim.submit(SetDefenseRoleCommand{c, static_cast<uint8_t>(DefenseRole::Shelter), false, CellPos{}});
    }
}

bool siegeStart(Sim& sim, float points) {
    sim.submit(StartStrategyRaidCommand{"Pirate", points, 3, "Siege", -1.0f});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            MESSAGE("the siege could not start: " << e.message);
            return false;
        }
    }
    return sim.raids().state().squads.size() == 1 && sim.raids().state().squads[0].strategy == RaidStrategy::Siege;
}

const Squad* siegeSquad(const Sim& sim) {
    return sim.raids().state().squads.empty() ? nullptr : &sim.raids().state().squads[0];
}

// Steps until the squad's siege reaches `stage` (or the squad is gone), collecting SiegeChanged messages.
bool siegeRunTo(Sim& sim, SiegeStage stage, int64_t limit, std::vector<std::string>* messages = nullptr) {
    for (int64_t i = 0; i < limit; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::SiegeChanged && messages != nullptr) {
                messages->push_back(e.message);
            }
        }
        const Squad* s = siegeSquad(sim);
        if (s == nullptr) {
            return false;
        }
        if (static_cast<int>(s->siege.stage) >= static_cast<int>(stage)) {
            return true;
        }
    }
    return false;
}

std::vector<Entity> siegeBuilt(const Sim& sim, const std::string& def) {
    std::vector<Entity> out;
    const Squad* s = siegeSquad(sim);
    if (s == nullptr) {
        return out;
    }
    for (const SiegeBuild& b : s->siege.builds) {
        if (b.def == def && b.built != kNoEntity && sim.things().spawned(b.built)) {
            out.push_back(b.built);
        }
    }
    return out;
}

bool siegeSameAfterLoad(Sim& sim, int64_t steps) {
    SaveLoadResult loaded = loadSim(saveSim(sim), siegeDefs());
    if (loaded.sim == nullptr) {
        MESSAGE("the save did not load: " << loaded.error);
        return false;
    }
    if (loaded.sim->stateHash() != sim.stateHash()) {
        MESSAGE("the loaded game differs at once");
        return false;
    }
    for (int64_t i = 0; i < steps; ++i) {
        sim.step();
        loaded.sim->step();
        if (loaded.sim->stateHash() != sim.stateHash()) {
            MESSAGE("the loaded game drifts after " << (i + 1) << " steps");
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Sieges: the strategy has a name, and Auto picks it only for big raids of a faction that can build") {
    RaidStrategy s = RaidStrategy::Assault;
    CHECK(raidStrategyFromName("Siege", s));
    CHECK(s == RaidStrategy::Siege);
    CHECK(std::string(siegeStageName(SiegeStage::Shell)) == "Shell");

    auto sim = siegeSim(5);
    int pirates = 0;
    int small = 0;
    int clan = 0;
    for (int i = 0; i < 300; ++i) {
        pirates += sim->raids().pickStrategy(800.0f, "Pirate") == RaidStrategy::Siege ? 1 : 0;
        small += sim->raids().pickStrategy(100.0f, "Pirate") == RaidStrategy::Siege ? 1 : 0;
        clan += sim->raids().pickStrategy(800.0f, "ClanSavage") == RaidStrategy::Siege ? 1 : 0;
    }
    CHECK(pirates > 0);  // 8 of 108 weight: 300 draws miss it with chance under 1 in 10^9
    CHECK(pirates < 80);
    CHECK(small == 0);   // under siegeMinPoints
    CHECK(clan == 0);    // tribes don't siege
}

TEST_CASE("Sieges: raiders dig in out of reach, build mortars and sandbags, and shell the colony") {
    auto sim = siegeSim(21);
    siegeShelterAll(*sim);
    sim->stepMany(60);
    const uint64_t firedBefore = sim->artillery().shotsFired();
    SIEGE_REQUIRE(siegeStart(*sim, 400.0f));
    const Squad& squad = *siegeSquad(*sim);
    const RaidRules& rules = sim->raids().rules();
    // The camp is planned at once, out of the colonists' reach and inside the mortar's.
    CHECK(squad.siege.stage == SiegeStage::March);
    int mortars = 0;
    int sandbags = 0;
    for (const SiegeBuild& b : squad.siege.builds) {
        mortars += b.def == "Turret_Mortar" ? 1 : 0;
        sandbags += b.def == "Sandbags" ? 1 : 0;
    }
    CHECK(mortars == 2);  // round(400 / 60), at most 2
    CHECK(sandbags >= 2);
    const CellPos camp = squad.siege.camp;
    const entt::registry& reg = sim->things().registry();
    for (const Entity c : siegeColonists(*sim)) {
        const Position& p = reg.get<Position>(c);
        const float d = std::sqrt((p.x - (camp.x + 0.5f)) * (p.x - (camp.x + 0.5f)) + (p.y - (camp.y + 0.5f)) * (p.y - (camp.y + 0.5f)));
        CHECK(d >= rules.campDistanceMin - 15.0f);  // the colonists stand round the target the camp is measured from
    }

    std::vector<std::string> messages;
    SIEGE_REQUIRE(siegeRunTo(*sim, SiegeStage::Build, 2 * kSecondsPerHour, &messages));
    CHECK(std::find(messages.begin(), messages.end(), "camp") != messages.end());
    // Their first shells came down in the camp.
    int shells = 0;
    for (const auto [e, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        if (sim->defs().get(thing.def).defName() == "Shell_HighExplosive") {
            shells += stack.count;
        }
    }
    CHECK(shells == rules.shellsPerMortar * mortars);

    SIEGE_REQUIRE(siegeRunTo(*sim, SiegeStage::Shell, 6 * kSecondsPerHour, &messages));
    CHECK(std::find(messages.begin(), messages.end(), "shelling") != messages.end());
    const std::vector<Entity> guns = siegeBuilt(*sim, "Turret_Mortar");
    CHECK(guns.size() == 2);
    // Manned by raiders (they have fired since the first gun went up), and their ammunition left alone by the colony's
    // haulers.
    bool manned = false;
    for (int i = 0; i < 2 * kSecondsPerHour && (sim->artillery().shotsFired() == firedBefore || !manned); ++i) {
        sim->step();
        for (const Entity g : siegeBuilt(*sim, "Turret_Mortar")) {
            manned = manned || sim->artillery().manned(g);
        }
    }
    CHECK(manned);
    CHECK(sim->artillery().shotsFired() > firedBefore);
    for (const Entity g : siegeBuilt(*sim, "Turret_Mortar")) {
        const Artillery& a = reg.get<Artillery>(g);
        if (a.crew != kNoEntity) {
            CHECK(reg.all_of<Raider>(a.crew));
        }
        CHECK_FALSE(reg.get<Fuel>(g).autoRefuel);
    }
    // The builders get the sandbags up too.
    for (int i = 0; i < 2 * kSecondsPerHour && siegeBuilt(*sim, "Sandbags").empty(); ++i) {
        sim->step();
    }
    CHECK_FALSE(siegeBuilt(*sim, "Sandbags").empty());
    // The snapshot shows it.
    Snapshot snap;
    sim->writeSnapshot(snap);
    SIEGE_REQUIRE(!snap.squads.empty());
    CHECK(snap.squads[0].strategy == static_cast<uint8_t>(RaidStrategy::Siege));
    CHECK(snap.squads[0].siegeStage == static_cast<uint8_t>(SiegeStage::Shell));
    CHECK(snap.squads[0].camp == camp);
}

TEST_CASE("Sieges: when the shells run out and no more come, they assault") {
    auto sim = siegeSim(22);
    siegeShelterAll(*sim);
    sim->stepMany(60);
    SIEGE_REQUIRE(siegeStart(*sim, 400.0f));
    SIEGE_REQUIRE(siegeRunTo(*sim, SiegeStage::Shell, 8 * kSecondsPerHour));
    // Every shell they get is taken away: the drops run out after resupplyMax.
    std::vector<std::string> messages;
    entt::registry& reg = sim->things().registry();
    const DefId shell = sim->defs().idOf("Things", "Shell_HighExplosive");
    bool assaulted = false;
    for (int i = 0; i < kSecondsPerHour && !assaulted; ++i) {
        std::vector<Entity> stacks;
        for (const auto [e, thing, pos] : reg.view<const Thing, const Position>().each()) {
            if (thing.def == shell) {
                stacks.push_back(e);
            }
        }
        for (const Entity e : stacks) {
            sim->things().destroy(e);
        }
        for (const Entity g : siegeBuilt(*sim, "Turret_Mortar")) {
            reg.get<Fuel>(g).amount = 0.0f;
        }
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::SiegeChanged) {
                messages.push_back(e.message);
            }
        }
        const Squad* s = siegeSquad(*sim);
        assaulted = s != nullptr && s->siege.stage == SiegeStage::Assault;
    }
    SIEGE_REQUIRE(assaulted);
    CHECK(siegeSquad(*sim)->siege.resupplies == sim->raids().rules().resupplyMax);
    CHECK(std::find(messages.begin(), messages.end(), "assault: out of shells") != messages.end());
    // The crews left the guns, and the squad goes for the colony.
    for (const Entity g : siegeBuilt(*sim, "Turret_Mortar")) {
        CHECK(reg.get<Artillery>(g).crew == kNoEntity);
    }
    for (const Entity m : siegeSquad(*sim)->members) {
        if (const Fighter* f = reg.try_get<Fighter>(m); f != nullptr && !sim->health().incapacitated(m)) {
            CHECK(f->mode == FightMode::Fight);
            CHECK(f->leash == 0.0f);
        }
    }
}

TEST_CASE("Sieges: losing their mortars, or too many of their own, sends them in") {
    {
        auto sim = siegeSim(23);
        siegeShelterAll(*sim);
        sim->stepMany(60);
        SIEGE_REQUIRE(siegeStart(*sim, 400.0f));
        SIEGE_REQUIRE(siegeRunTo(*sim, SiegeStage::Shell, 8 * kSecondsPerHour));
        for (const Entity g : siegeBuilt(*sim, "Turret_Mortar")) {
            sim->submit(DestroyThingCommand{g});
        }
        std::vector<std::string> messages;
        CHECK(siegeRunTo(*sim, SiegeStage::Assault, 120, &messages));
        CHECK(std::find(messages.begin(), messages.end(), "assault: no mortars") != messages.end());
    }
    {
        auto sim = siegeSim(24);
        siegeShelterAll(*sim);
        sim->stepMany(60);
        SIEGE_REQUIRE(siegeStart(*sim, 400.0f));
        SIEGE_REQUIRE(siegeRunTo(*sim, SiegeStage::Build, 2 * kSecondsPerHour));
        const Squad& squad = *siegeSquad(*sim);
        const int started = squad.started;
        const int kill = static_cast<int>(std::ceil(sim->raids().rules().siegeAssaultLost * static_cast<float>(started)));
        if (static_cast<float>(kill) >= sim->raids().rules().fleeLostFraction * static_cast<float>(started)) {
            MESSAGE("the squad is too small to lose 40% without routing: " << started);
            return;
        }
        const std::vector<Entity> members = squad.members;
        for (int i = 0; i < kill; ++i) {
            sim->submit(DestroyThingCommand{members[static_cast<size_t>(i)]});
        }
        std::vector<std::string> messages;
        CHECK(siegeRunTo(*sim, SiegeStage::Assault, 120, &messages));
        CHECK(std::find(messages.begin(), messages.end(), "assault: losses") != messages.end());
    }
}

TEST_CASE("Sieges: a save while they build and while they shell loads and plays on the same") {
    auto sim = siegeSim(25);
    sim->stepMany(60);
    SIEGE_REQUIRE(siegeStart(*sim, 400.0f));
    SIEGE_REQUIRE(siegeRunTo(*sim, SiegeStage::Build, 2 * kSecondsPerHour));
    sim->stepMany(300);
    CHECK(siegeSameAfterLoad(*sim, 600));
    if (siegeRunTo(*sim, SiegeStage::Shell, 8 * kSecondsPerHour)) {
        CHECK(siegeSameAfterLoad(*sim, 1200));
    } else {
        MESSAGE("the siege never got to shelling (the colonists fought it off first)");
    }
}

TEST_CASE("Sieges: a map with no room for a camp refuses the siege") {
    SimSettings settings;
    settings.seed = 26;
    settings.mapSize = 48;  // the camp must be 35 m out and 6 cells in from the edge: no room
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    Sim sim(settings, siegeDefs());
    sim.submit(StartStrategyRaidCommand{"Pirate", 400.0f, -1, "Siege", -1.0f});
    sim.step();
    bool failed = false;
    for (const Event& e : sim.events()) {
        failed = failed || e.type == EventType::CommandFailed;
    }
    CHECK(failed);
    CHECK(sim.raids().state().squads.empty());
}

#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define EXPLO_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data. These tests check docs/design/explosives.md.
std::shared_ptr<const DefDatabase> exploDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<DefSource> sources;
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), text.str()});
        }
        std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
        std::stringstream tuning;
        tuning << tuningFile.rdbuf();
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, tuning.str(), *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> exploSim(uint64_t seed = 11) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, exploDefs());
}

Entity exploPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

DefId exploItem(Sim& sim) { return sim.defs().idOf("Things", "Grenade_Frag"); }

float exploHurt(Sim& sim, Entity pawn) {
    float total = 0.0f;
    const Health& h = sim.things().registry().get<Health>(pawn);
    for (const Injury& i : h.injuries) {
        total += i.severity;
    }
    return total + static_cast<float>(h.missing.size()) * 50.0f;
}

void exploHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

bool exploSaw(const Sim& sim, EventType type) {
    return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; });
}

void exploBlast(Sim& sim, float x, float y, float radius = 1.9f, float damage = 50.0f) {
    sim.submit(ExplodeCommand{x, y, radius, damage, "Bomb", 0.1f});
    sim.step();
}

void exploWall(Sim& sim, int x0, int y0, int x1, int y1, const char* def = "Wall") {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            sim.things().spawn(def, CellPos{x, y, 0});
        }
    }
}

}  // namespace

TEST_CASE("Explosives: the frag grenade has the wiki's numbers") {
    const DefDatabase& defs = *exploDefs();
    const Def* bomb = defs.find("DamageTypes", "Bomb");
    const Def* proj = defs.find("Projectiles", "Proj_GrenadeFrag");
    const Def* item = defs.find("Things", "Grenade_Frag");
    EXPLO_REQUIRE(bomb != nullptr);
    EXPLO_REQUIRE(proj != nullptr);
    EXPLO_REQUIRE(item != nullptr);
    CHECK(proj->number("explosion.damage") == doctest::Approx(50.0));
    CHECK(proj->number("explosion.radius") == doctest::Approx(1.9));
    CHECK(proj->number("explosion.armorPenetration") == doctest::Approx(0.1));
    CHECK(proj->integer("thrown.fuse") == 50);  // 100 ticks
    CHECK(proj->number("thrown.speed") == doctest::Approx(0.4));  // 12 m/s
    CHECK(item->number("thrown.range") == doctest::Approx(12.9));
    CHECK(item->number("thrown.warmupTime") == doctest::Approx(45.0));  // 90 ticks
    CHECK(item->number("thrown.cooldown") == doctest::Approx(80.0));    // 160 ticks
    CHECK(bomb->number("explosion.wallFactor") * bomb->number("explosion.impassableFactor") * bomb->number("explosion.buildingFactor") ==
          doctest::Approx(8.0));
    CHECK(defs.get(defs.idOf("DamageTypes", "Bomb")).text("hediff") == "Shredded");
}

TEST_CASE("Explosives: a blast hurts by distance and leaves those outside alone") {
    float near = 0.0f;
    float edge = 0.0f;
    float outside = 0.0f;
    for (uint64_t seed = 1; seed <= 12; ++seed) {
        auto sim = exploSim(seed);
        const Entity a = exploPawn(*sim, 40, 40);  // at the centre
        const Entity b = exploPawn(*sim, 42, 40);  // 2 m out of a 3 m blast
        const Entity c = exploPawn(*sim, 45, 40);  // 5 m out
        exploBlast(*sim, 40.5f, 40.5f, 3.0f);
        near += exploHurt(*sim, a);
        edge += exploHurt(*sim, b);
        outside += exploHurt(*sim, c);
    }
    CHECK(near > edge);
    CHECK(edge > 0.0f);
    CHECK(outside == 0.0f);
}

TEST_CASE("Explosives: a blast comes as fragments on different parts") {
    auto sim = exploSim(3);
    const Entity a = exploPawn(*sim, 40, 40);
    exploBlast(*sim, 40.5f, 40.5f);
    EXPLO_REQUIRE(exploSaw(*sim, EventType::Explosion));
    const Health& h = sim->things().registry().get<Health>(a);
    CHECK(h.injuries.size() + h.missing.size() >= 2);
    CHECK(h.injuries.size() + h.missing.size() <= 4);
    int events = 0;
    for (const Event& e : sim->events()) {
        events += (e.type == EventType::PawnInjured || e.type == EventType::PawnPartLost) ? 1 : 0;
    }
    CHECK(events >= 2);
}

TEST_CASE("Explosives: a wall shields, and a frag grenade brings down a wall next to it") {
    {
        auto sim = exploSim();
        const Entity behind = exploPawn(*sim, 44, 40);
        const Entity open = exploPawn(*sim, 40, 44);
        exploWall(*sim, 42, 38, 42, 42);  // a wall between the blast and the first pawn
        exploBlast(*sim, 40.5f, 40.5f, 5.0f);
        CHECK(exploHurt(*sim, behind) == 0.0f);
        CHECK(exploHurt(*sim, open) > 0.0f);
    }
    {
        auto sim = exploSim();
        const Entity wall = sim->things().spawn("Wall", CellPos{42, 40, 0});
        const Entity bed = sim->things().spawn("Bed", CellPos{40, 36, 0});
        EXPLO_REQUIRE(wall != kNoEntity);
        EXPLO_REQUIRE(bed != kNoEntity);
        const int bedHp = sim->things().registry().get<HitPoints>(bed).current;
        exploBlast(*sim, 41.6f, 40.5f);  // 0.4 m from the wall
        CHECK_FALSE(sim->things().exists(wall));  // 50 x 8 x about 0.9 is over its 300
        // The bed is 3.5 m away: out of the 1.9 m radius.
        CHECK(sim->things().registry().get<HitPoints>(bed).current == bedHp);
    }
    {
        auto sim = exploSim();
        const Entity bed = sim->things().spawn("Bed", CellPos{40, 41, 0});
        EXPLO_REQUIRE(bed != kNoEntity);
        const int hp = sim->things().registry().get<HitPoints>(bed).current;
        exploBlast(*sim, 40.5f, 40.5f);  // beside a bed (furniture takes x2)
        const int left = sim->things().registry().get<HitPoints>(bed).current;
        CHECK(left < hp);
    }
}

TEST_CASE("Explosives: lying flat takes less, armor takes some away") {
    float standing = 0.0f;
    float prone = 0.0f;
    float bare = 0.0f;
    float armored = 0.0f;
    for (uint64_t seed = 1; seed <= 24; ++seed) {
        {
            auto sim = exploSim(seed);
            const Entity a = exploPawn(*sim, 40, 40);
            const Entity b = exploPawn(*sim, 41, 40);
            sim->submit(SetStanceCommand{b, Stance::Prone});
            sim->step();
            exploBlast(*sim, 40.9f, 40.5f);
            standing += exploHurt(*sim, a);
            prone += exploHurt(*sim, b);
        }
        {
            auto sim = exploSim(seed);
            const Entity a = exploPawn(*sim, 40, 40);
            const Entity b = exploPawn(*sim, 41, 40);
            for (const char* piece : {"Apparel_FlakVest", "Apparel_FlakPants", "Apparel_SimpleHelmet"}) {
                const Entity item = sim->things().make(sim->defs().idOf("Things", piece));
                if (item != kNoEntity) {
                    sim->combat().equip(b, item);
                }
            }
            exploBlast(*sim, 40.9f, 40.5f);
            bare += exploHurt(*sim, a);
            armored += exploHurt(*sim, b);
        }
    }
    CHECK(prone < standing);
    CHECK(armored < bare);
}

TEST_CASE("Explosives: a hard blast knocks a pawn down and away, and it gets up again") {
    auto sim = exploSim();
    const Entity pawn = exploPawn(*sim, 40, 40, "Gun_Revolver");
    exploHeal(*sim, pawn);
    const Position before = sim->things().registry().get<Position>(pawn);
    exploBlast(*sim, 39.7f, 40.5f);
    EXPLO_REQUIRE(sim->combat().knockedDownFor(pawn) > 0);
    EXPLO_REQUIRE(exploSaw(*sim, EventType::PawnKnockedDown));
    exploHeal(*sim, pawn);
    const Position after = sim->things().registry().get<Position>(pawn);
    CHECK(after.x > before.x);  // thrown away from the blast, to the east
    Snapshot snap;
    sim->writeSnapshot(snap);
    const auto view = std::find_if(snap.things.begin(), snap.things.end(), [&](const ThingView& t) { return t.id == pawn; });
    EXPLO_REQUIRE(view != snap.things.end());
    CHECK(view->knockedDown);
    CHECK(view->stance == Stance::Prone);
    // It can't shoot while down.
    sim->submit(ShootCommand{pawn, kNoEntity, 60.0f, 40.0f, 1.0f, true});
    sim->step();
    CHECK(exploSaw(*sim, EventType::CommandFailed));
    exploHeal(*sim, pawn);
    sim->stepMany(120);
    CHECK(sim->combat().knockedDownFor(pawn) == 0);
    CHECK(sim->things().registry().get<Posture>(pawn).stance == Stance::Standing);
}

TEST_CASE("Explosives: a stack of grenades goes on the belt, up to the limit") {
    auto sim = exploSim();
    const Entity pawn = exploPawn(*sim, 40, 40);
    const Entity four = sim->things().spawn("Grenade_Frag", CellPos{41, 40, 0}, 4);
    EXPLO_REQUIRE(four != kNoEntity);
    sim->submit(EquipCommand{pawn, four});
    sim->step();
    CHECK(sim->explosives().count(pawn) == 4);
    CHECK_FALSE(sim->things().exists(four));
    const Entity many = sim->things().spawn("Grenade_Frag", CellPos{42, 40, 0}, 10);
    sim->submit(EquipCommand{pawn, many});
    sim->step();
    CHECK(sim->explosives().count(pawn) == 10);  // the belt holds ten
    EXPLO_REQUIRE(sim->things().exists(many));
    CHECK(sim->things().registry().get<StackCount>(many).count == 4);
    sim->submit(EquipCommand{pawn, many});
    sim->step();
    CHECK(exploSaw(*sim, EventType::CommandFailed));  // the belt is full
    // A pawn taken off the map drops them.
    sim->submit(DestroyThingCommand{pawn});
    sim->step();
    int lying = 0;
    for (const auto [e, thing, stack] : sim->things().registry().view<const Thing, const StackCount>().each()) {
        lying += thing.def == exploItem(*sim) ? stack.count : 0;
    }
    CHECK(lying == 14);
}

TEST_CASE("Explosives: a thrown grenade arcs, lands near the target and goes off when its fuse is out") {
    auto sim = exploSim();
    const Entity thrower = exploPawn(*sim, 40, 40);
    exploHeal(*sim, thrower);
    const int given = sim->explosives().give(thrower, exploItem(*sim), 2);
    EXPLO_REQUIRE(given == 2);
    const float tx = 48.5f;
    const float ty = 40.5f;
    sim->submit(ThrowCommand{thrower, "", kNoEntity, tx, ty, false});
    sim->step();
    EXPLO_REQUIRE(std::any_of(sim->events().begin(), sim->events().end(), [&](const Event& e) {
        return e.type == EventType::PawnAction && e.action == PawnAction::Throw && e.thing == thrower;
    }));
    CHECK(sim->explosives().throwing(thrower));
    int thrownAt = -1;
    int landedAt = -1;
    int blastAt = -1;
    float landX = 0.0f;
    float landY = 0.0f;
    float blastRadius = 0.0f;
    bool sawFlying = false;
    for (int i = 0; i < 400 && blastAt < 0; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            if (e.type == EventType::GrenadeThrown) {
                thrownAt = i;
            } else if (e.type == EventType::GrenadeLanded) {
                landedAt = i;
                landX = e.x;
                landY = e.y;
            } else if (e.type == EventType::Explosion) {
                blastAt = i;
                blastRadius = e.amount;
            }
        }
        if (thrownAt >= 0 && landedAt < 0) {
            Snapshot snap;
            sim->writeSnapshot(snap);
            for (const ProjectileView& p : snap.projectiles) {
                sawFlying = sawFlying || (p.fuse < 0 && p.def == sim->defs().idOf("Projectiles", "Proj_GrenadeFrag"));
            }
        }
    }
    EXPLO_REQUIRE(thrownAt >= 0);
    EXPLO_REQUIRE(landedAt > thrownAt);
    EXPLO_REQUIRE(blastAt > landedAt);
    CHECK(thrownAt >= 40);  // the wind-up is 45 steps
    CHECK(sawFlying);
    CHECK(blastAt - landedAt == 50);  // the fuse
    CHECK(blastRadius == doctest::Approx(1.9));
    // The miss radius is 1.9 m scaled by distance (8 m of 9) and skill (0.9 at skill 4 here).
    CHECK(std::hypot(landX - tx, landY - ty) <= 1.9f * 8.0f / 9.0f + 0.05f);
    CHECK(sim->explosives().count(thrower) == 1);
    CHECK(sim->things().registry().view<Grenade>().empty());
    // Cooling down: a second throw is refused until 80 steps after the release.
    sim->submit(ThrowCommand{thrower, "", kNoEntity, tx, ty, false});
    sim->step();
    CHECK(exploSaw(*sim, EventType::CommandFailed));
    sim->stepMany(40);
    CHECK(sim->explosives().throwReadyIn(thrower) == 0);
}

TEST_CASE("Explosives: a player's throw has no wind-up") {
    auto sim = exploSim();
    const Entity thrower = exploPawn(*sim, 40, 40);
    sim->explosives().give(thrower, exploItem(*sim), 1);
    sim->submit(ThrowCommand{thrower, "Grenade_Frag", kNoEntity, 46.5f, 40.5f, true});
    bool thrown = false;
    for (int i = 0; i < 5 && !thrown; ++i) {
        sim->step();
        thrown = exploSaw(*sim, EventType::GrenadeThrown);
    }
    CHECK(thrown);
    // No grenade, no throw.
    sim->stepMany(100);
    sim->submit(ThrowCommand{thrower, "", kNoEntity, 46.5f, 40.5f, true});
    sim->step();
    CHECK(exploSaw(*sim, EventType::CommandFailed));
}

TEST_CASE("Explosives: walls stop a throw short, low cover is cleared") {
    auto sim = exploSim();
    const Entity thrower = exploPawn(*sim, 40, 40);
    sim->explosives().give(thrower, exploItem(*sim), 2);
    ThrowPlan plan;
    EXPLO_REQUIRE(sim->explosives().planThrow(thrower, exploItem(*sim), 48.5f, 40.5f, plan));
    CHECK(plan.reaches);
    exploWall(*sim, 44, 20, 44, 60, "Wall");  // a long tall wall
    EXPLO_REQUIRE(sim->explosives().planThrow(thrower, exploItem(*sim), 48.5f, 40.5f, plan));
    CHECK_FALSE(plan.reaches);
    CHECK(plan.landX < 44.0f);  // it falls on this side
    // Out of range of the arm: no arc reaches.
    auto far = exploSim();
    const Entity weak = exploPawn(*far, 20, 20);
    far->explosives().give(weak, exploItem(*far), 1);
    EXPLO_REQUIRE(far->explosives().planThrow(weak, exploItem(*far), 60.5f, 20.5f, plan));
    CHECK_FALSE(plan.reaches);
    // Sandbags are 1.1 m: the arc goes over.
    auto low = exploSim();
    const Entity lobber = exploPawn(*low, 40, 40);
    low->explosives().give(lobber, exploItem(*low), 1);
    exploWall(*low, 43, 30, 43, 50, "Sandbags");
    EXPLO_REQUIRE(low->explosives().planThrow(lobber, exploItem(*low), 48.5f, 40.5f, plan));
    CHECK(plan.reaches);
    CHECK(plan.landX > 44.0f);
}

TEST_CASE("Explosives: a grenadier throws at a group, dives from a grenade, and none of its own side is hurt") {
    auto sim = exploSim(9);
    const Entity colonist = exploPawn(*sim, 30, 40, "Gun_AssaultRifle");
    const Entity friendlyA = exploPawn(*sim, 28, 38, "Gun_Revolver");
    std::vector<Entity> raiders;
    for (const CellPos c : {CellPos{41, 40, 0}, CellPos{42, 41, 0}, CellPos{41, 42, 0}}) {
        const Entity raider = exploPawn(*sim, c.x, c.y, "Gun_Revolver");
        sim->submit(SetFactionCommand{raider, 2});
        raiders.push_back(raider);
    }
    sim->explosives().give(colonist, exploItem(*sim), 3);
    sim->submit(DraftCommand{colonist, true});
    sim->submit(DraftCommand{friendlyA, true});
    bool threw = false;
    bool exploded = false;
    bool raiderDived = false;
    int friendlyShredded = 0;
    const DefId shredded = sim->defs().idOf("Hediffs", "Shredded");
    for (int i = 0; i < 900 && !exploded; ++i) {
        exploHeal(*sim, colonist);
        exploHeal(*sim, friendlyA);
        sim->step();
        for (const Event& e : sim->events()) {
            threw = threw || (e.type == EventType::GrenadeThrown && e.thing == colonist);
            exploded = exploded || e.type == EventType::Explosion;
            friendlyShredded += (e.type == EventType::PawnInjured && e.def == shredded && (e.thing == colonist || e.thing == friendlyA)) ? 1 : 0;
            raiderDived = raiderDived || (e.type == EventType::PawnAction && e.action == PawnAction::Dive &&
                                          std::find(raiders.begin(), raiders.end(), e.thing) != raiders.end());
        }
    }
    CHECK(threw);
    CHECK(exploded);
    CHECK(raiderDived);
    CHECK(friendlyShredded == 0);
}

TEST_CASE("Explosives: a fighter runs from a grenade at its feet and lies flat when there is nowhere to go") {
    {
        auto sim = exploSim();
        const Entity fighter = exploPawn(*sim, 40, 40, "Gun_Revolver");
        sim->submit(DraftCommand{fighter, true});
        sim->stepMany(20);
        Rng rng(1);
        const Entity grenade = sim->explosives().place(sim->defs().idOf("Projectiles", "Proj_GrenadeFrag"), 40.9f, 40.5f, 0, kNoEntity, rng);
        EXPLO_REQUIRE(grenade != kNoEntity);
        bool dived = false;
        bool blew = false;
        float dist = 0.0f;
        for (int i = 0; i < 80 && !blew; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                dived = dived || (e.type == EventType::PawnAction && e.action == PawnAction::Dive && e.thing == fighter);
                if (e.type == EventType::Explosion) {
                    blew = true;
                    const Position& p = sim->things().registry().get<Position>(fighter);
                    dist = std::hypot(p.x - e.x, p.y - e.y);
                }
            }
        }
        CHECK(dived);
        CHECK(blew);
        CHECK(dist >= 1.9f);  // out of the blast by the time it goes off
        CHECK(exploHurt(*sim, fighter) == 0.0f);
    }
    {
        auto sim = exploSim();
        exploWall(*sim, 40, 40, 44, 40);
        exploWall(*sim, 40, 44, 44, 44);
        exploWall(*sim, 40, 41, 40, 43);
        exploWall(*sim, 44, 41, 44, 43);
        const Entity fighter = exploPawn(*sim, 41, 41, "Gun_Revolver");
        sim->submit(DraftCommand{fighter, true});
        sim->stepMany(20);
        Rng rng(1);
        sim->explosives().place(sim->defs().idOf("Projectiles", "Proj_GrenadeFrag"), 42.5f, 42.5f, 0, kNoEntity, rng);
        bool dived = false;
        for (int i = 0; i < 20; ++i) {
            sim->step();
            for (const Event& e : sim->events()) {
                dived = dived || (e.type == EventType::PawnAction && e.action == PawnAction::Dive && e.thing == fighter);
            }
        }
        CHECK(dived);
        CHECK(sim->things().registry().get<Posture>(fighter).stance == Stance::Prone);
    }
}

TEST_CASE("Explosives: belts, flying grenades, fuses and knock-downs survive a save and load") {
    auto sim = exploSim();
    const Entity thrower = exploPawn(*sim, 40, 40);
    const Entity near = exploPawn(*sim, 60, 60);
    sim->explosives().give(thrower, exploItem(*sim), 3);
    sim->explosives().give(near, exploItem(*sim), 1);
    sim->submit(ThrowCommand{thrower, "", kNoEntity, 48.5f, 40.5f, false});
    sim->stepMany(60);  // released and in the air
    EXPLO_REQUIRE(!sim->things().registry().view<Grenade>().empty());
    exploBlast(*sim, 60.2f, 60.5f);  // knocks the other pawn down
    EXPLO_REQUIRE(sim->combat().knockedDownFor(near) > 0);
    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"explosives\"") != std::string::npos);
    CHECK(json.find("\"grenade\"") != std::string::npos);
    CHECK(json.find("\"belt\"") != std::string::npos);
    CHECK(json.find("\"knockedDown\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), exploDefs());
    EXPLO_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    CHECK(loaded.sim->explosives().count(thrower) == sim->explosives().count(thrower));
    // Through the landing and the blast.
    for (int i = 0; i < 160; ++i) {
        sim->step();
        loaded.sim->step();
        EXPLO_REQUIRE(loaded.sim->stateHash() == sim->stateHash());
    }
    CHECK(sim->things().registry().view<Grenade>().empty());
}

TEST_CASE("Explosives: the throw and the blast go through a replay file and play back the same") {
    SimSettings settings;
    settings.seed = 21;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.checkpointEvery = 100;
    Sim original(settings, exploDefs());
    original.submit(SpawnThingCommand{"Human", CellPos{40, 40, 0}});
    original.submit(SpawnThingCommand{"Human", CellPos{49, 40, 0}});
    original.submit(SpawnThingCommand{"Grenade_Frag", CellPos{41, 40, 0}, 2});
    original.step();
    std::vector<Entity> spawned;
    for (const Event& e : original.events()) {
        if (e.type == EventType::ThingSpawned) {
            spawned.push_back(e.thing);
        }
    }
    EXPLO_REQUIRE(spawned.size() == 3);
    original.submit(EquipCommand{spawned[0], spawned[2]});
    original.step();
    original.submit(ThrowCommand{spawned[0], "Grenade_Frag", spawned[1], 0.0f, 0.0f, false});
    original.stepMany(200);
    original.submit(ExplodeCommand{49.5f, 40.5f, 2.5f, 40.0f, "Bomb", 0.1f});
    original.stepMany(10);
    const Replay recorded = recordReplay(original);
    Replay loaded;
    std::string error;
    EXPLO_REQUIRE(readReplay(writeReplay(recorded), loaded, error));
    CHECK(writeReplay(loaded) == writeReplay(recorded));
    CHECK(writeReplay(loaded).find("\"Throw\"") != std::string::npos);
    CHECK(writeReplay(loaded).find("\"Explode\"") != std::string::npos);
    Sim replayed(loaded.settings, exploDefs());
    playReplay(loaded, replayed, loaded.endStep);
    CHECK(replayed.stateHash() == original.stateHash());
    CHECK(checkReplay(loaded, replayed).matched());
}

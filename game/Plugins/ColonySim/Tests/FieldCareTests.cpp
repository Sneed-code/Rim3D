#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/FieldCare.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define CARE_REQUIRE(...)      \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> careDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A flat 96 m map with no plants and clear, mild weather. An enemy stands still at (60, 32) (it is held with
// PlayerControl, which the fighter AI leaves alone) and a wall from (40, 24) to (40, 40) hides the ground just west of
// it from that enemy: (37, 28) is in its shadow, (37, 48) is not. docs/design/field-care.md.
struct CareFixture {
    std::unique_ptr<Sim> simPtr;
    Sim& sim;
    std::vector<Event> seen;
    Entity enemy = kNoEntity;

    static constexpr CellPos kHidden{37, 28, 0};
    static constexpr CellPos kOpen{37, 48, 0};

    CareFixture() : simPtr(std::make_unique<Sim>(SimSettings{5, 96, 0, 0, true, "Human", "Steel", 0}, careDefs())), sim(*simPtr) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.setOutdoorOverride(20.0f);
        sim.weather().force("Clear", 400.0);
        for (int y = 24; y <= 40; ++y) {
            sim.things().spawn("Wall", CellPos{40, y, 0});
        }
        enemy = sim.things().spawn("Human", CellPos{60, 32, 0});
        reg().emplace<PlayerControl>(enemy);
        sim.submit(SetFactionCommand{enemy, 2});
        sim.step();
    }

    entt::registry& reg() { return sim.things().registry(); }

    Entity colonist(CellPos cell) {
        const Entity pawn = sim.things().spawn("Human", cell);
        reg().emplace<Colonist>(pawn, Colonist{});
        sim.jobs().addWorker(pawn, {}, false);
        return pawn;
    }

    // Four bad bruises put a pawn down with pain, and a cut on the arm makes it bleed (MedicineTests).
    void downBleeding(Entity pawn) {
        const std::vector<std::string> labels = sim.health().partLabels(reg().get<Thing>(pawn).def);
        const auto hit = [&](const char* type, float amount, const char* part) {
            const auto it = std::find(labels.begin(), labels.end(), part);
            Rng rng(99);
            sim.health().applyDamage(pawn, sim.health().damageTypeId(type), amount, static_cast<int>(it - labels.begin()), rng);
        };
        for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
            hit("Blunt", 20.0f, part);
        }
        hit("Cut", 6.0f, "right arm");
    }

    void run(int64_t seconds) {
        for (int64_t i = 0; i < seconds; ++i) {
            sim.step();
            seen.insert(seen.end(), sim.events().begin(), sim.events().end());
        }
    }
    bool runUntil(int64_t limit, const std::function<bool()>& done) {
        for (int64_t i = 0; i < limit; ++i) {
            run(1);
            if (done()) {
                return true;
            }
        }
        return false;
    }
    const Event* find(const std::function<bool(const Event&)>& test) const {
        const auto it = std::find_if(seen.begin(), seen.end(), test);
        return it == seen.end() ? nullptr : &*it;
    }
    const Event* tended(Entity doctor, Entity patient) const {
        return find([&](const Event& e) { return e.type == EventType::PawnTended && e.thing == doctor && e.target == patient; });
    }
    const Event* jobOn(Entity pawn, JobType type, Entity target) const {
        return find([&](const Event& e) { return e.type == EventType::JobStarted && e.thing == pawn && e.job == type && e.target == target; });
    }
    bool failed() const {
        return find([](const Event& e) { return e.type == EventType::CommandFailed; }) != nullptr;
    }
    CellPos cell(Entity pawn) { return cellOf(reg().get<Position>(pawn)); }
    void killEnemy() { sim.submit(DamagePawnCommand{enemy, "Bullet", 500.0f, 0}); }
};

}  // namespace

TEST_CASE("Field care: the Medic and Firefighter roles are set by command, never for a colonist incapable of the work") {
    CareFixture f;
    const Entity medic = f.colonist(CellPos{30, 32, 0});
    const Entity unable = f.colonist(CellPos{30, 34, 0});
    f.sim.submit(SetDefenseRoleCommand{medic, static_cast<uint8_t>(DefenseRole::Medic), false, CellPos{}});
    f.sim.submit(SetDefenseRoleCommand{unable, static_cast<uint8_t>(DefenseRole::Firefighter), false, CellPos{}});
    f.run(1);
    CHECK_FALSE(f.failed());
    CHECK(f.sim.raids().roleOf(medic) == DefenseRole::Medic);
    CHECK(f.sim.raids().roleOf(unable) == DefenseRole::Firefighter);
    CHECK(std::string(defenseRoleName(DefenseRole::Medic)) == "Medic");

    // One whose backstory forbids Doctor work can't be a Medic; there is no sixth role; a spot is a Defender's only.
    const int doctor = f.sim.jobs().workTypeIndex(f.sim.defs().idOf("WorkTypes", "Doctor"));
    CARE_REQUIRE(doctor >= 0);
    f.reg().get<WorkSettings>(unable).entries[static_cast<size_t>(doctor)].disabled = true;
    f.sim.submit(SetDefenseRoleCommand{unable, static_cast<uint8_t>(DefenseRole::Medic), false, CellPos{}});
    f.sim.submit(SetDefenseRoleCommand{medic, 5, false, CellPos{}});
    f.sim.submit(SetDefenseRoleCommand{medic, static_cast<uint8_t>(DefenseRole::Medic), true, CellPos{30, 30, 0}});
    f.run(1);
    int failures = 0;
    for (const Event& e : f.seen) {
        failures += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failures == 3);
    CHECK(f.sim.raids().roleOf(unable) == DefenseRole::Firefighter);
    CHECK(f.sim.raids().roleOf(medic) == DefenseRole::Medic);

    Snapshot snap;
    f.sim.writeSnapshot(snap);
    CHECK(snap.find(medic)->defenseRole == 3);
    CHECK(snap.find(unable)->defenseRole == 4);
}

TEST_CASE("Field care: the enemies' sight decides which cells are safe") {
    CareFixture f;
    FieldCare& care = f.sim.fieldCare();
    CHECK(care.exposure(37.5f, 28.5f, 0) == 0);
    CHECK(care.exposure(37.5f, 48.5f, 0) == 1);
    CHECK(care.safe(CareFixture::kHidden));
    CHECK_FALSE(care.safe(CareFixture::kOpen));
    CHECK_FALSE(care.safe(CellPos{55, 50, 0}));  // within keepAway: never safe, seen or not
    // A downed enemy sees nobody.
    f.killEnemy();
    f.run(1);
    CHECK(care.safe(CareFixture::kOpen));
}

TEST_CASE("Field care: a medic waits behind the line and tends the downed it can reach out of sight, the rest once the enemy is gone") {
    CareFixture f;
    const Entity medic = f.colonist(CellPos{30, 32, 0});
    const Entity hidden = f.colonist(CareFixture::kHidden);
    const Entity open = f.colonist(CareFixture::kOpen);
    f.sim.things().spawn("MedicineIndustrial", CellPos{30, 30, 0}, 5);
    f.sim.submit(SetDefenseRoleCommand{medic, static_cast<uint8_t>(DefenseRole::Medic), false, CellPos{}});
    f.run(1);
    f.downBleeding(hidden);
    f.downBleeding(open);
    f.run(1);
    CARE_REQUIRE(f.reg().get<Health>(hidden).downed);
    CARE_REQUIRE(f.reg().get<Health>(open).downed);
    f.sim.submit(SoundAlarmCommand{true});
    f.run(1);
    CARE_REQUIRE(f.sim.raids().state().alarm);
    CHECK(f.sim.fieldCare().dutyWorkType(medic) == f.sim.jobs().workTypeIndex(f.sim.defs().idOf("WorkTypes", "Doctor")));
    CHECK_FALSE(f.reg().all_of<Fighter>(medic));  // a medic doesn't fight

    CHECK(f.runUntil(3000, [&] { return f.tended(medic, hidden) != nullptr; }));
    CHECK(f.jobOn(medic, JobType::TendPatient, open) == nullptr);
    CHECK(f.sim.health().needsTending(open));
    // With nothing it can reach it stands by out of the enemy's sight.
    CHECK(f.runUntil(300, [&] { return f.reg().get<Worker>(medic).job.type == JobType::StandBy; }));
    CHECK(f.sim.fieldCare().safe(f.reg().get<Worker>(medic).job.cell));
    f.run(200);
    CHECK(f.jobOn(medic, JobType::TendPatient, open) == nullptr);

    // The enemy goes down: the open ground is safe and the medic goes out.
    f.killEnemy();
    CHECK(f.runUntil(3000, [&] { return f.tended(medic, open) != nullptr; }));
    CHECK_FALSE(f.failed());

    // The alarm off, it is an ordinary colonist again.
    f.sim.submit(SoundAlarmCommand{false});
    f.run(2);
    CHECK(f.sim.fieldCare().dutyWorkType(medic) < 0);
    CHECK(f.reg().get<Worker>(medic).job.type != JobType::StandBy);
}

TEST_CASE("Field care: a firefighter beats out a fire out of the enemy's sight, and one in its sight only once the enemy is gone") {
    CareFixture f;
    const Entity firefighter = f.colonist(CellPos{30, 32, 0});
    f.sim.submit(SetDefenseRoleCommand{firefighter, static_cast<uint8_t>(DefenseRole::Firefighter), false, CellPos{}});
    f.sim.submit(SetWorkPriorityCommand{firefighter, "Firefighter", 0});  // the role does the work whatever the Work tab says
    f.sim.submit(SetHomeAreaCommand{CellRect{20, 20, 30, 34, 0}, true});
    f.sim.things().spawn("WoodLog", CareFixture::kHidden, 50);
    f.sim.things().spawn("WoodLog", CareFixture::kOpen, 50);
    f.run(1);
    f.sim.submit(SoundAlarmCommand{true});
    f.run(1);
    CARE_REQUIRE(f.sim.fire().ignite(CareFixture::kHidden));
    CARE_REQUIRE(f.sim.fire().ignite(CareFixture::kOpen));
    const auto beaten = [&](CellPos c) {
        return f.find([&](const Event& e) { return e.type == EventType::FireOut && e.cell == c && e.message == "beaten"; }) != nullptr;
    };
    const auto jobAt = [&](CellPos c) {
        return f.find([&](const Event& e) {
            return e.type == EventType::JobStarted && e.thing == firefighter && e.job == JobType::ExtinguishFire && e.cell == c;
        }) != nullptr;
    };
    CHECK(f.runUntil(1500, [&] { return beaten(CareFixture::kHidden); }));
    f.run(300);
    CHECK_FALSE(jobAt(CareFixture::kOpen));
    CARE_REQUIRE(f.sim.fire().burning(CareFixture::kOpen));

    f.killEnemy();
    CHECK(f.runUntil(1500, [&] { return beaten(CareFixture::kOpen); }));
    CHECK_FALSE(f.failed());
}

TEST_CASE("Field care: a fighter drags a downed friend out of the enemy's sight, and the medic tends it there") {
    CareFixture f;
    const Entity medic = f.colonist(CellPos{30, 32, 0});
    const Entity fighter = f.colonist(CellPos{34, 44, 0});
    const Entity friendDown = f.colonist(CareFixture::kOpen);
    f.sim.things().spawn("MedicineIndustrial", CellPos{30, 30, 0}, 5);
    f.sim.submit(SetDefenseRoleCommand{medic, static_cast<uint8_t>(DefenseRole::Medic), false, CellPos{}});
    f.run(1);
    f.downBleeding(friendDown);
    f.run(1);
    CARE_REQUIRE(f.reg().get<Health>(friendDown).downed);
    f.sim.submit(SoundAlarmCommand{true});

    const auto action = [&](PawnAction a) {
        return f.find([&](const Event& e) { return e.type == EventType::PawnAction && e.thing == fighter && e.action == a && e.target == friendDown; });
    };
    CARE_REQUIRE(f.runUntil(600, [&] { return action(PawnAction::LiftPawn) != nullptr; }));
    CHECK(rescuerOf(f.reg(), friendDown) == fighter);
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    CHECK(snap.find(fighter)->rescuing == friendDown);
    CHECK(f.sim.fieldCare().drags() == 1);
    CARE_REQUIRE(f.runUntil(600, [&] { return action(PawnAction::LayPawn) != nullptr; }));
    CHECK(action(PawnAction::LayPawn)->message == "safe");
    CHECK(f.sim.fieldCare().safe(f.cell(friendDown)));
    CHECK(f.reg().get<Health>(friendDown).downed);
    CARE_REQUIRE(f.reg().all_of<Fighter>(fighter));
    CHECK(f.reg().get<Fighter>(fighter).mode == FightMode::Fight);
    CHECK(rescuerOf(f.reg(), friendDown) == kNoEntity);

    // Out of sight, the medic reaches it.
    CHECK(f.runUntil(3000, [&] { return f.tended(medic, friendDown) != nullptr; }));
    CHECK_FALSE(f.failed());
}

TEST_CASE("Field care: a drag and a duty in the middle save and load, and the loaded game goes on the same") {
    CareFixture f;
    const Entity medic = f.colonist(CellPos{30, 32, 0});
    const Entity fighter = f.colonist(CellPos{34, 44, 0});
    const Entity friendDown = f.colonist(CareFixture::kOpen);
    f.sim.things().spawn("MedicineIndustrial", CellPos{30, 30, 0}, 5);
    f.sim.submit(SetDefenseRoleCommand{medic, static_cast<uint8_t>(DefenseRole::Medic), false, CellPos{}});
    f.run(1);
    f.downBleeding(friendDown);
    f.run(1);
    f.sim.submit(SoundAlarmCommand{true});
    CARE_REQUIRE(f.runUntil(600, [&] {
        const Fighter* fi = f.reg().try_get<Fighter>(fighter);
        return fi != nullptr && fi->mode == FightMode::Rescue && fi->lifted;
    }));
    CHECK(f.reg().get<Worker>(medic).job.type == JobType::StandBy);

    // (Saves keep PlayerControl for the player's colonist only: from here the enemy is an ordinary fighter.)
    f.reg().remove<PlayerControl>(f.enemy);
    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, careDefs());
    CARE_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == f.sim.stateHash());
    loaded.sim->setOutdoorOverride(20.0f);  // a test aid, not saved
    const Fighter& copy = loaded.sim->things().registry().get<Fighter>(fighter);
    CHECK(copy.mode == FightMode::Rescue);
    CHECK(copy.rescuing == friendDown);
    CHECK(copy.lifted);
    for (int i = 0; i < 1500; ++i) {
        f.sim.step();
        loaded.sim->step();
        if (f.sim.stateHash() != loaded.sim->stateHash()) {
            MESSAGE("hashes differ " << i << " steps after loading");
            CHECK(false);
            break;
        }
    }
}

TEST_CASE("Field care: a recorded raid with medics and firefighters replays to the same state") {
    SimSettings settings;
    settings.seed = 13;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, careDefs());
    std::vector<Entity> colonists;
    for (const auto [pawn, colonist] : recorded.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        colonists.push_back(pawn);
    }
    std::sort(colonists.begin(), colonists.end());
    CARE_REQUIRE(colonists.size() >= 4);
    recorded.submit(SetDefenseRoleCommand{colonists[0], static_cast<uint8_t>(DefenseRole::Medic), false, CellPos{}});
    recorded.submit(SetDefenseRoleCommand{colonists[1], static_cast<uint8_t>(DefenseRole::Firefighter), false, CellPos{}});
    recorded.submit(StartRaidCommand{"Pirate", 200.0f, -1});
    recorded.stepMany(5);
    recorded.submit(DamagePawnCommand{colonists[2], "Blunt", 30.0f, -1});
    recorded.submit(DamagePawnCommand{colonists[3], "Cut", 8.0f, -1});
    const Position& at = recorded.things().registry().get<Position>(colonists[1]);
    recorded.submit(IgniteCommand{CellPos{static_cast<int>(at.x) + 2, static_cast<int>(at.y), 0}, 0.5f, kNoEntity, true});
    recorded.stepMany(3000);
    const Replay replay = recordReplay(recorded);
    Replay parsed;
    std::string error;
    CARE_REQUIRE(readReplay(writeReplay(replay), parsed, error));
    Sim again(parsed.settings, careDefs());
    playReplay(parsed, again, replay.endStep);
    const ReplayCheck check = checkReplay(parsed, again);
    CHECK(check.matched());
    CHECK(again.stateHash() == recorded.stateHash());
}

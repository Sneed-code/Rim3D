#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Random.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Social.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// These tests check the animation hooks (task AN1, docs/plans/animation.md "Sim hooks"): what the snapshot and
// events tell Unreal so it can pick the right animation.

#define ANIM_REQUIRE(...)                    \
    do {                                     \
        const bool animChecked = (__VA_ARGS__); \
        CHECK(animChecked);                  \
        if (!animChecked) {                  \
            return;                          \
        }                                    \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> animDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> animSim(uint64_t seed = 21) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, animDefs());
}

Entity animPawn(Sim& sim, int x, int y, const char* weapon = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (weapon != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", weapon)));
    }
    return pawn;
}

// A colonist that works all day, so it is awake and may talk (as SocialTests' socPawn).
Entity animColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.age = 25;
    colonist.skills.push_back(SkillRecord{"Social", 0, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    return pawn;
}

// Two colonists side by side who have just said `interaction`; false if it was refused.
bool animTalk(Sim& sim, Entity& a, Entity& b, const char* interaction) {
    a = animColonist(sim, CellPos{20, 20, 0});
    b = animColonist(sim, CellPos{21, 20, 0});
    sim.step();
    sim.submit(InteractCommand{a, b, interaction, 1});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            MESSAGE(e.message);
            return false;
        }
    }
    return true;
}

const ThingView* animView(const Snapshot& snap, Entity e) {
    for (const ThingView& v : snap.things) {
        if (v.id == e) {
            return &v;
        }
    }
    return nullptr;
}

// Fires one dead-straight shot from `shooter` at `target`'s chest and steps until it lands; true if it hit flesh or armor.
bool animShoot(Sim& sim, Entity shooter, Entity target, std::vector<Event>* staggers = nullptr) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    sim.combat().aimPoint(target, x, y, z);
    Rng rng(3);
    sim.combat().fire(shooter, x, y, z, 0.0f, rng);
    bool hit = false;
    for (int i = 0; i < 60 && !sim.things().registry().view<Projectile>().empty(); ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ProjectileHit && e.target == target &&
                (e.hit == ProjectileHitKind::Pawn || e.hit == ProjectileHitKind::Armor)) {
                hit = true;
            }
            if (e.type == EventType::PawnStaggered && staggers != nullptr) {
                staggers->push_back(e);
            }
        }
    }
    return hit;
}

}  // namespace

TEST_CASE("Animation hooks: a bullet with the stopping power staggers a man and slows him; a weaker one does not") {
    auto sim = animSim();
    const Entity shooter = animPawn(*sim, 10, 10, "Gun_Revolver");     // stopping power 1
    const Entity target = animPawn(*sim, 16, 10);
    std::vector<Event> staggers;
    ANIM_REQUIRE(animShoot(*sim, shooter, target, &staggers));
    ANIM_REQUIRE(staggers.size() == 1);
    CHECK(staggers[0].thing == target);
    CHECK(staggers[0].amount == doctest::Approx(48.0f));
    const int left = sim->combat().staggeredFor(target);
    CHECK(left > 0);
    CHECK(left <= 48);
    CHECK(sim->combat().staggerSpeedFactor(target) == doctest::Approx(0.17f));
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* v = animView(snap, target);
    ANIM_REQUIRE(v != nullptr);
    CHECK(v->staggeredFor == left);
    // It wears off by itself.
    for (int i = 0; i < 50; ++i) {
        sim->step();
    }
    CHECK(sim->combat().staggeredFor(target) == 0);
    CHECK(sim->combat().staggerSpeedFactor(target) == doctest::Approx(1.0f));

    // An autopistol bullet (stopping power 0.5) does not stagger a man (body size 1).
    auto other = animSim();
    const Entity pistol = animPawn(*other, 10, 10, "Gun_Autopistol");
    const Entity man = animPawn(*other, 16, 10);
    std::vector<Event> none;
    ANIM_REQUIRE(animShoot(*other, pistol, man, &none));
    CHECK(none.empty());
    CHECK(other->combat().staggeredFor(man) == 0);
}

TEST_CASE("Animation hooks: a staggered walker covers a sixth of the ground") {
    auto sim = animSim();
    const Entity a = animPawn(*sim, 10, 20);
    const Entity b = animPawn(*sim, 10, 30);
    sim->things().registry().emplace<Staggered>(b, Staggered{100000});
    entt::registry& reg = sim->things().registry();
    reg.get<Mover>(a).walkTo(reg.get<Position>(a).x, reg.get<Position>(a).y, {CellPos{40, 20, 0}});
    reg.get<Mover>(b).walkTo(reg.get<Position>(b).x, reg.get<Position>(b).y, {CellPos{40, 30, 0}});
    for (int i = 0; i < 30; ++i) {
        sim->step();
        sim->things().registry().get<Staggered>(b).stepsLeft = 100000;
    }
    const float da = sim->things().registry().get<Position>(a).x - 10.5f;
    const float db = sim->things().registry().get<Position>(b).x - 10.5f;
    ANIM_REQUIRE(da > 1.0f);
    CHECK(db == doctest::Approx(da * 0.17f).epsilon(0.15));
}

TEST_CASE("Animation hooks: an interaction shows on both pawns for its time, then comes off") {
    auto sim = animSim();
    Entity a = kNoEntity, b = kNoEntity;
    ANIM_REQUIRE(animTalk(*sim, a, b, "Chitchat"));
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* va = animView(snap, a);
    const ThingView* vb = animView(snap, b);
    ANIM_REQUIRE(va != nullptr && vb != nullptr);
    CHECK(va->talkDef == sim->defs().idOf("Interactions", "Chitchat"));
    CHECK(va->talkWith == b);
    CHECK(va->talkStarted);
    CHECK(vb->talkWith == a);
    CHECK_FALSE(vb->talkStarted);
    CHECK(va->talkSecondsLeft >= 118);
    CHECK(va->talkSecondsLeft <= 120);
    for (int i = 0; i < 121; ++i) {
        sim->step();
    }
    sim->writeSnapshot(snap);
    CHECK_FALSE(animView(snap, a)->talkDef.valid());
    CHECK_FALSE(sim->things().registry().all_of<Talking>(a));
    CHECK_FALSE(sim->things().registry().all_of<Talking>(b));
}

TEST_CASE("Animation hooks: posture says sitting in a chair, lying in bed and lying downed") {
    auto sim = animSim();
    const Entity pawn = animPawn(*sim, 20, 20);
    sim->jobs().addWorker(pawn);
    const Entity chair = sim->things().spawn("Stool", CellPos{20, 20, 0});
    ANIM_REQUIRE(chair != kNoEntity);
    Job& job = sim->things().registry().get<Worker>(pawn).job;
    job = Job{};
    job.type = JobType::Ingest;
    job.chair = chair;
    job.step = jobStepCount(JobType::Ingest) - 1;
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(animView(snap, pawn)->posture == PawnPosture::Sit);
    CHECK(animView(snap, pawn)->seat == chair);

    job.step = 0;  // still walking to it
    sim->writeSnapshot(snap);
    CHECK(animView(snap, pawn)->posture == PawnPosture::Stand);

    job = Job{};
    job.type = JobType::LayDown;
    job.step = kLayDownLieStep;
    sim->writeSnapshot(snap);
    CHECK(animView(snap, pawn)->posture == PawnPosture::Lie);

    job = Job{};
    Health& h = sim->things().registry().get<Health>(pawn);
    h.downed = true;
    sim->writeSnapshot(snap);
    CHECK(animView(snap, pawn)->posture == PawnPosture::Lie);
    CHECK(animView(snap, pawn)->seat == kNoEntity);
}

TEST_CASE("Animation hooks: a longsword's blade is a strong swing, its handle and fists are not") {
    auto sim = animSim();
    const Entity pawn = animPawn(*sim, 20, 20, "Melee_Longsword");
    Melee& m = sim->things().registry().emplace<Melee>(pawn);
    m.weaponTool = true;
    m.tool = 0;  // blade, 19.4
    CHECK(sim->melee().strongSwing(pawn));
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(animView(snap, pawn)->meleeStrong);
    CHECK(animView(snap, pawn)->meleeTool == 0);
    m.tool = 1;  // handle, 12
    CHECK_FALSE(sim->melee().strongSwing(pawn));

    const Entity boxer = animPawn(*sim, 30, 20);
    Melee& f = sim->things().registry().emplace<Melee>(boxer);
    f.weaponTool = false;
    for (uint8_t t = 0; t < 3; ++t) {  // left fist, right fist, head
        f.tool = t;
        CHECK_FALSE(sim->melee().strongSwing(boxer));
    }
}

TEST_CASE("Animation hooks: cover toward the fight target and walking over sandbags") {
    auto sim = animSim();
    const Entity pawn = animPawn(*sim, 20, 20, "Gun_Revolver");
    const Entity enemy = animPawn(*sim, 30, 20);
    ANIM_REQUIRE(sim->things().spawn("Sandbags", CellPos{21, 20, 0}) != kNoEntity);
    Fighter fighter;
    fighter.target = enemy;
    sim->things().registry().emplace_or_replace<Fighter>(pawn, fighter);
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* v = animView(snap, pawn);
    ANIM_REQUIRE(v != nullptr);
    CHECK(v->coverDir == static_cast<uint8_t>(Dir8::E));
    CHECK(v->coverHeight == doctest::Approx(1.1f).epsilon(0.05));

    // Walking: in the sandbags' cell the pawn is over a low obstacle.
    const Entity walker = animPawn(*sim, 21, 22);
    sim->things().registry().get<Position>(walker) = Position{21.5f, 20.5f, 0};
    Mover& mover = sim->things().registry().get_or_emplace<Mover>(walker);
    mover.vx = 0.1f;
    sim->writeSnapshot(snap);
    CHECK(animView(snap, walker)->vaultHeight == doctest::Approx(1.1f).epsilon(0.05));
    CHECK(animView(snap, enemy)->vaultHeight == 0.0f);
}

TEST_CASE("Animation hooks: a stagger and an interaction survive a save") {
    auto sim = animSim();
    Entity a = kNoEntity, b = kNoEntity;
    ANIM_REQUIRE(animTalk(*sim, a, b, "DeepTalk"));
    sim->combat().stagger(a);
    const int stagger = sim->combat().staggeredFor(a);
    ANIM_REQUIRE(stagger > 0);
    const uint64_t hash = sim->stateHash();
    SaveLoadResult loaded = loadSim(saveSim(*sim), animDefs());
    ANIM_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == hash);
    CHECK(loaded.sim->combat().staggeredFor(a) == stagger);
    const Talking* t = loaded.sim->social().talking(b);
    ANIM_REQUIRE(t != nullptr);
    CHECK(t->def == animDefs()->idOf("Interactions", "DeepTalk"));
    CHECK(t->with == a);
}

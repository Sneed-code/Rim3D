#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Resonance.h"
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

// Evaluates once: some checks cast a power.
#define RESO_REQUIRE(...)                                  \
    do {                                                   \
        const bool resoOk = static_cast<bool>(__VA_ARGS__);  \
        CHECK_MESSAGE(resoOk, #__VA_ARGS__);               \
        if (!resoOk) {                                     \
            return;                                        \
        }                                                  \
    } while (0)

namespace {

// The game's real data. These tests check docs/design/resonance.md.
std::shared_ptr<const DefDatabase> resoDefs() {
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

std::unique_ptr<Sim> resoSim(uint64_t seed = 5) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, resoDefs());
}

Entity resoPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

// An attuned pawn at `level` that knows every power of its level or lower.
void resoAttune(Sim& sim, Entity pawn, int level) {
    sim.submit(AttuneCommand{pawn, level});
    for (const char* power : {"Stun", "Numb", "Burden", "Jump"}) {
        sim.submit(LearnPowerCommand{pawn, power});
    }
    sim.step();
}

bool resoFailed(const Sim& sim, const char* text) {
    return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) {
        return e.type == EventType::CommandFailed && e.message.find(text) != std::string::npos;
    });
}

bool resoSaw(const Sim& sim, EventType type, Entity thing = kNoEntity) {
    return std::any_of(sim.events().begin(), sim.events().end(),
                       [&](const Event& e) { return e.type == type && (thing == kNoEntity || e.thing == thing); });
}

// Casts and steps until the power goes off; true if it did.
bool resoCast(Sim& sim, Entity pawn, const char* power, Entity target, CellPos cell = CellPos{}) {
    sim.submit(CastPowerCommand{pawn, power, target, cell, false});
    for (int i = 0; i < 20; ++i) {
        sim.step();
        if (resoSaw(sim, EventType::CommandFailed)) {
            return false;
        }
        if (resoSaw(sim, EventType::PowerCast, pawn)) {
            return true;
        }
    }
    return false;
}

// Bruises on the limbs until the pawn's pain is at least `pain` (bruises don't bleed).
void resoHurt(Sim& sim, Entity pawn, float pain) {
    const DefId blunt = sim.health().damageTypeId("Blunt");
    Rng rng(3);
    for (int i = 0; i < 200 && sim.things().registry().get<Health>(pawn).pain < pain; ++i) {
        sim.health().applyDamage(pawn, blunt, 3.0f, -1, rng);
        sim.health().refresh(pawn);
    }
}

}  // namespace

TEST_CASE("Resonance: the powers and rules have RimWorld's numbers") {
    const DefDatabase& defs = *resoDefs();
    const Def* stun = defs.find("Powers", "Stun");
    const Def* numb = defs.find("Powers", "Numb");
    const Def* burden = defs.find("Powers", "Burden");
    const Def* jump = defs.find("Powers", "Jump");
    RESO_REQUIRE(stun != nullptr);
    RESO_REQUIRE(numb != nullptr);
    RESO_REQUIRE(burden != nullptr);
    RESO_REQUIRE(jump != nullptr);
    CHECK(stun->number("strain") == doctest::Approx(12.0));
    CHECK(stun->number("focusCost") == doctest::Approx(0.01));
    CHECK(stun->integer("effect.duration") == 90);  // 180 ticks
    CHECK(numb->number("strain") == doctest::Approx(8.0));
    CHECK(numb->number("effect.value") == doctest::Approx(0.1));
    CHECK(numb->integer("effect.duration") == 3600);  // 7,200 ticks
    CHECK(burden->number("effect.value") == doctest::Approx(0.4));
    CHECK(burden->number("range") == doctest::Approx(29.9));
    CHECK(jump->integer("level") == 4);  // RimWorld's skip is a level 4 psycast
    CHECK(jump->number("strain") == doctest::Approx(25.0));
    CHECK(jump->number("range") == doctest::Approx(27.9));

    auto sim = resoSim();
    const Entity pawn = resoPawn(*sim, 40, 40);
    resoAttune(*sim, pawn, 1);
    CHECK(sim->resonance().strainLimit(pawn) == doctest::Approx(30.0f));
    sim->submit(AttuneCommand{pawn, 4});
    sim->step();
    CHECK(sim->resonance().strainLimit(pawn) == doctest::Approx(60.0f));
}

TEST_CASE("Resonance: attuning teaches a power of each new level, a level 4 power waits for level 4") {
    auto sim = resoSim();
    const Entity pawn = resoPawn(*sim, 40, 40);
    sim->submit(AttuneCommand{pawn, 1});
    sim->step();
    const Attunement* a = sim->resonance().attunement(pawn);
    RESO_REQUIRE(a != nullptr);
    CHECK(a->level == 1);
    CHECK(a->powers.size() == 1);  // one of stun, numb, burden at random
    CHECK(a->focus == doctest::Approx(sim->resonance().rules().startFocus));
    sim->submit(LearnPowerCommand{pawn, "Jump"});
    sim->step();
    CHECK(resoFailed(*sim, "needs attunement level 4"));
    sim->submit(AttuneCommand{pawn, 4});
    sim->step();
    CHECK(sim->resonance().knows(pawn, sim->defs().idOf("Powers", "Jump")));  // the only level 4 power so far
    sim->submit(AttuneCommand{pawn, 0});
    sim->step();
    CHECK(sim->resonance().attunement(pawn) == nullptr);
}

TEST_CASE("Resonance: a stun stops a pawn walking and shooting for 3 seconds, and costs strain and focus") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    const Entity target = resoPawn(*sim, 50, 40, "Gun_Revolver");
    resoAttune(*sim, caster, 1);
    sim->submit(DraftCommand{target, true});
    sim->submit(MoveOrderCommand{target, CellPos{50, 60, 0}});
    const float focusBefore = sim->resonance().attunement(caster)->focus;
    RESO_REQUIRE(resoCast(*sim, caster, "Stun", target));
    const Attunement* a = sim->resonance().attunement(caster);
    CHECK(a->strain == doctest::Approx(12.0f).epsilon(0.01));
    CHECK(a->focus == doctest::Approx(focusBefore - 0.01f).epsilon(0.001));
    RESO_REQUIRE(sim->things().registry().all_of<Stunned>(target));
    CHECK(sim->things().registry().get<Stunned>(target).stepsLeft == 90);
    const Position before = sim->things().registry().get<Position>(target);
    sim->submit(ShootCommand{target, caster});
    sim->step();
    CHECK(resoFailed(*sim, "stunned"));
    sim->stepMany(60);
    const Position during = sim->things().registry().get<Position>(target);
    CHECK(during.x == doctest::Approx(before.x));
    CHECK(during.y == doctest::Approx(before.y));
    Snapshot snap;
    sim->writeSnapshot(snap);
    RESO_REQUIRE(snap.find(target) != nullptr);
    CHECK(snap.find(target)->stunnedFor > 0);
    sim->stepMany(40);
    CHECK(!sim->things().registry().all_of<Stunned>(target));
    sim->stepMany(60);
    const Position after = sim->things().registry().get<Position>(target);
    CHECK(std::hypot(after.x - before.x, after.y - before.y) > 1.0f);  // walking again
}

TEST_CASE("Resonance: strain falls 0.54 a real second at level 1, faster in pain, and the limit refuses a cast") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    const Entity target = resoPawn(*sim, 45, 40);
    resoAttune(*sim, caster, 1);
    RESO_REQUIRE(resoCast(*sim, caster, "Stun", target));
    const float s0 = sim->resonance().attunement(caster)->strain;
    sim->stepMany(30);
    CHECK(s0 - sim->resonance().attunement(caster)->strain == doctest::Approx(0.54f).epsilon(0.01));
    RESO_REQUIRE(resoCast(*sim, caster, "Stun", target));  // 24 or so
    sim->submit(CastPowerCommand{caster, "Stun", target, CellPos{}, false});
    sim->step();
    CHECK(resoFailed(*sim, "too much strain"));
}

TEST_CASE("Resonance: numb takes a pawn's pain away, so one down from pain gets up, and it wears off") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    const Entity hurt = resoPawn(*sim, 44, 40);
    resoAttune(*sim, caster, 1);
    resoHurt(*sim, hurt, 0.85f);
    sim->step();
    const Health& h = sim->things().registry().get<Health>(hurt);
    RESO_REQUIRE(!h.dead);
    RESO_REQUIRE(h.downed);
    const float pain = h.pain;
    RESO_REQUIRE(resoCast(*sim, caster, "Numb", hurt));
    bool gotUp = false;
    for (int i = 0; i < 3 && !gotUp; ++i) {
        sim->step();
        gotUp = resoSaw(*sim, EventType::PawnGotUp, hurt);
    }
    CHECK(gotUp);
    CHECK(sim->things().registry().get<Health>(hurt).pain == doctest::Approx(pain * 0.1f).epsilon(0.05));
    sim->stepMany(3600);
    CHECK(sim->things().registry().get<Health>(hurt).downed);  // the pain is back
    // Oneself too.
    resoHurt(*sim, caster, 0.4f);
    sim->step();
    RESO_REQUIRE(resoCast(*sim, caster, "Numb", caster));
    sim->step();
    CHECK(sim->things().registry().get<Health>(caster).pain < 0.1f);
}

TEST_CASE("Resonance: burden caps Moving at 40% for 20 seconds") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    const Entity target = resoPawn(*sim, 60, 40);
    resoAttune(*sim, caster, 1);
    sim->submit(SetFactionCommand{target, 2});
    RESO_REQUIRE(resoCast(*sim, caster, "Burden", target));
    sim->step();
    CHECK(sim->health().capacityValue(target, "Moving") == doctest::Approx(0.4f));
    CHECK(sim->things().registry().get<Health>(target).movingFactor == doctest::Approx(0.4f));
    Snapshot snap;
    sim->writeSnapshot(snap);
    CHECK(snap.find(target)->burdened);
    sim->stepMany(600);
    CHECK(sim->things().registry().get<Health>(target).movingFactor == doctest::Approx(1.0f));
}

TEST_CASE("Resonance: a jump moves the pawn at once to a cell it can see in range, not through a wall") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    resoAttune(*sim, caster, 4);
    for (int y = 30; y <= 50; ++y) {
        sim->things().spawn("Wall", CellPos{55, y, 0});
    }
    sim->submit(CastPowerCommand{caster, "Jump", caster, CellPos{60, 40, 0}, false});
    sim->step();
    CHECK(resoFailed(*sim, "can't see"));
    sim->submit(CastPowerCommand{caster, "Jump", caster, CellPos{75, 40, 0}, false});
    sim->step();
    CHECK(resoFailed(*sim, "out of range"));
    RESO_REQUIRE(resoCast(*sim, caster, "Jump", caster, CellPos{50, 45, 0}));
    const Position& p = sim->things().registry().get<Position>(caster);
    CHECK(p.x == doctest::Approx(50.5f));
    CHECK(p.y == doctest::Approx(45.5f));
    // Another pawn can be moved too.
    const Entity other = resoPawn(*sim, 48, 45);
    sim->stepMany(100);  // strain down to the limit's reach again
    RESO_REQUIRE(resoCast(*sim, caster, "Jump", other, CellPos{48, 52, 0}));
    CHECK(cellOf(sim->things().registry().get<Position>(other)) == CellPos{48, 52, 0});
}

TEST_CASE("Resonance: a power needs focus; a jump needs 25% of it") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    const Entity target = resoPawn(*sim, 45, 40);
    resoAttune(*sim, caster, 4);
    sim->things().registry().get<Attunement>(caster).focus = 0.2f;
    sim->submit(CastPowerCommand{caster, "Jump", caster, CellPos{45, 45, 0}, false});
    sim->step();
    CHECK(resoFailed(*sim, "not enough focus"));
    CHECK(resoCast(*sim, caster, "Stun", target));  // a level 1 power needs only its cost
    sim->things().registry().get<Attunement>(caster).focus = 0.005f;
    sim->stepMany(200);
    sim->submit(CastPowerCommand{caster, "Stun", target, CellPos{}, false});
    sim->step();
    CHECK(resoFailed(*sim, "not enough focus"));
}

TEST_CASE("Resonance: an attuned fighter stuns an enemy at a friend's side and numbs a friend in pain") {
    auto sim = resoSim(7);
    const Entity caster = resoPawn(*sim, 30, 40, "Gun_Revolver");
    const Entity friendly = resoPawn(*sim, 40, 40, "Gun_Revolver");
    const Entity raider = resoPawn(*sim, 42, 40, "Gun_Revolver");
    resoAttune(*sim, caster, 1);
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(DraftCommand{caster, true});
    sim->submit(DraftCommand{friendly, true});
    bool stunned = false;
    for (int i = 0; i < 120 && !stunned; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            stunned = stunned || (e.type == EventType::PowerCast && e.thing == caster && e.target == raider &&
                                  e.def == sim->defs().idOf("Powers", "Stun"));
        }
    }
    CHECK(stunned);
    sim->submit(DestroyThingCommand{raider});
    sim->stepMany(200);
    resoHurt(*sim, friendly, 0.5f);
    bool numbed = false;
    for (int i = 0; i < 120 && !numbed; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            numbed = numbed || (e.type == EventType::PowerCast && e.thing == caster && e.target == friendly);
        }
    }
    CHECK(numbed);
}

TEST_CASE("Resonance: a fighter falling back from an enemy at arm's length jumps to its spot") {
    auto sim = resoSim(3);
    const Entity caster = resoPawn(*sim, 40, 40, "Gun_Revolver");
    std::vector<Entity> raiders;
    for (const CellPos c : {CellPos{42, 40, 0}, CellPos{42, 41, 0}, CellPos{43, 39, 0}}) {
        const Entity raider = resoPawn(*sim, c.x, c.y, "Melee_Club");  // they charge
        sim->submit(SetFactionCommand{raider, 2});
        raiders.push_back(raider);
    }
    sim->submit(AttuneCommand{caster, 4});  // a level 1 power at random, and the jump
    sim->step();
    sim->submit(SetFactionCommand{caster, 3});  // an AI fighter of its own side, outnumbered: it falls back
    bool jumped = false;
    for (int i = 0; i < 200 && !jumped; ++i) {
        for (const Entity r : raiders) {
            sim->things().registry().replace<Health>(r, Health{});
            sim->health().refresh(r);
        }
        sim->step();
        for (const Event& e : sim->events()) {
            jumped = jumped || (e.type == EventType::PowerCast && e.thing == caster && e.def == sim->defs().idOf("Powers", "Jump"));
        }
    }
    CHECK(jumped);
}

TEST_CASE("Resonance: attunement, a cast under way, stuns and effects survive a save and load") {
    auto sim = resoSim();
    const Entity caster = resoPawn(*sim, 40, 40);
    const Entity a = resoPawn(*sim, 46, 40);
    const Entity b = resoPawn(*sim, 40, 46);
    resoAttune(*sim, caster, 4);
    RESO_REQUIRE(resoCast(*sim, caster, "Burden", a));
    RESO_REQUIRE(resoCast(*sim, caster, "Stun", b));
    sim->submit(CastPowerCommand{caster, "Numb", a, CellPos{}, false});
    sim->stepMany(3);  // mid warm-up
    RESO_REQUIRE(sim->resonance().casting(caster));
    const std::string json = saveToJson(*sim);
    CHECK(json.find("\"resonance\"") != std::string::npos);
    CHECK(json.find("\"attunement\"") != std::string::npos);
    CHECK(json.find("\"casting\"") != std::string::npos);
    CHECK(json.find("\"stunned\"") != std::string::npos);
    SaveLoadResult loaded = loadSim(saveSim(*sim), resoDefs());
    RESO_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
    for (int i = 0; i < 200; ++i) {
        sim->step();
        loaded.sim->step();
        RESO_REQUIRE(loaded.sim->stateHash() == sim->stateHash());
    }
    CHECK(loaded.sim->health().capacityValue(a, "Moving") == doctest::Approx(sim->health().capacityValue(a, "Moving")));
}

TEST_CASE("Resonance: the commands go through a replay file and play back the same") {
    SimSettings settings;
    settings.seed = 23;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.checkpointEvery = 50;
    Sim original(settings, resoDefs());
    original.submit(SpawnThingCommand{"Human", CellPos{40, 40, 0}});
    original.submit(SpawnThingCommand{"Human", CellPos{48, 40, 0}});
    original.step();
    std::vector<Entity> spawned;
    for (const Event& e : original.events()) {
        if (e.type == EventType::ThingSpawned) {
            spawned.push_back(e.thing);
        }
    }
    RESO_REQUIRE(spawned.size() == 2);
    original.submit(AttuneCommand{spawned[0], 4});
    original.submit(LearnPowerCommand{spawned[0], "Stun"});
    original.submit(LearnPowerCommand{spawned[0], "Burden"});
    original.step();
    original.submit(CastPowerCommand{spawned[0], "Stun", spawned[1], CellPos{}, false});
    original.stepMany(20);
    original.submit(CastPowerCommand{spawned[0], "Jump", spawned[0], CellPos{44, 47, 0}, true});
    original.stepMany(100);
    const Replay recorded = recordReplay(original);
    Replay loaded;
    std::string error;
    RESO_REQUIRE(readReplay(writeReplay(recorded), loaded, error));
    CHECK(writeReplay(loaded) == writeReplay(recorded));
    CHECK(writeReplay(loaded).find("\"CastPower\"") != std::string::npos);
    Sim replayed(loaded.settings, resoDefs());
    playReplay(loaded, replayed, loaded.endStep);
    CHECK(replayed.stateHash() == original.stateHash());
    CHECK(checkReplay(loaded, replayed).matched());
    CHECK(cellOf(original.things().registry().get<Position>(spawned[0])) == CellPos{44, 47, 0});
}

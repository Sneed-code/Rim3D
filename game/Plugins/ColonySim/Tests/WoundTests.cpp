#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Wounds.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
// The expression is evaluated once, so it may change the sim.
#define WOUND_REQUIRE(...)              \
    do {                                \
        const bool woundOk = (__VA_ARGS__); \
        CHECK(woundOk);                 \
        if (!woundOk) {                 \
            return;                     \
        }                               \
    } while (0)

namespace {

// The game's real data: the Human body, the injuries and the skin regions of docs/design/wounds.md.
std::shared_ptr<const DefDatabase> woundTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> woundTestSim() {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.pawns = 0;
    settings.items = 0;
    return std::make_unique<Sim>(settings, woundTestDefs());
}

Entity woundTestPawn(Sim& sim, int x = 10, int y = 10) {
    return sim.things().spawn("Human", CellPos{x, y, 0});
}

int woundTestPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == label) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool woundTestHit(Sim& sim, Entity pawn, const char* type, float amount, const std::string& partLabel) {
    Rng rng(99);
    return sim.health().applyDamage(pawn, sim.health().damageTypeId(type), amount, woundTestPart(sim, pawn, partLabel), rng);
}

bool woundTestInjure(Sim& sim, Entity pawn, const char* hediff, float severity, const std::string& partLabel) {
    return sim.health().addInjury(pawn, woundTestPart(sim, pawn, partLabel), sim.defs().idOf("Hediffs", hediff), severity);
}

// The pawn's wounds in the snapshot, in order.
std::vector<WoundView> woundTestWounds(Sim& sim, Entity pawn, float* bloodLoss = nullptr, float* bleedPerDay = nullptr) {
    Snapshot snap;
    sim.writeSnapshot(snap);
    std::vector<WoundView> out;
    const ThingView* view = snap.find(pawn);
    if (view == nullptr) {
        return out;
    }
    CHECK(view->firstWound + view->woundCount <= snap.wounds.size());
    for (uint32_t i = 0; i < view->woundCount; ++i) {
        out.push_back(snap.wounds[view->firstWound + i]);
    }
    if (bloodLoss != nullptr) {
        *bloodLoss = view->bloodLoss;
    }
    if (bleedPerDay != nullptr) {
        *bleedPerDay = view->bleedPerDay;
    }
    return out;
}

}  // namespace

TEST_CASE("Wounds: a gunshot shows on its part with its kind, region and share of the part") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    const Entity bystander = woundTestPawn(*sim, 14, 10);
    CHECK(woundTestWounds(*sim, pawn).empty());
    WOUND_REQUIRE(woundTestHit(*sim, pawn, "Bullet", 6.0f, "left arm"));
    sim->step();
    float bloodLoss = -1.0f;
    float bleedPerDay = -1.0f;
    const std::vector<WoundView> wounds = woundTestWounds(*sim, pawn, &bloodLoss, &bleedPerDay);
    WOUND_REQUIRE(wounds.size() == 1);
    const WoundView& w = wounds[0];
    CHECK(w.part == woundTestPart(*sim, pawn, "left arm"));
    CHECK(w.kind == WoundKind::Gunshot);
    CHECK(w.region == WoundRegion::LeftArm);
    CHECK(sim->defs().get(w.partDef).defName() == "Arm");
    CHECK(w.severity == doctest::Approx(6.0f / 30.0f).epsilon(0.05));  // the arm has 30 hit points; a second of healing is allowed
    CHECK(w.bleeding);
    CHECK_FALSE(w.tended);
    CHECK(w.tendQuality < 0.0f);
    CHECK(bleedPerDay > 0.0f);
    CHECK(bloodLoss >= 0.0f);
    CHECK(woundTestWounds(*sim, bystander).empty());
}

TEST_CASE("Wounds: tending wraps the wound in a bandage and stops it bleeding") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    WOUND_REQUIRE(woundTestHit(*sim, pawn, "Cut", 5.0f, "right leg"));
    sim->step();
    float bleedPerDay = 0.0f;
    std::vector<WoundView> wounds = woundTestWounds(*sim, pawn, nullptr, &bleedPerDay);
    WOUND_REQUIRE(wounds.size() == 1);
    CHECK(wounds[0].bleeding);
    CHECK(bleedPerDay > 0.0f);
    CHECK(sim->health().tendAll(pawn, 0.7f) == 1);
    sim->step();
    wounds = woundTestWounds(*sim, pawn, nullptr, &bleedPerDay);
    WOUND_REQUIRE(wounds.size() == 1);
    CHECK(wounds[0].tended);
    CHECK(wounds[0].tendQuality == doctest::Approx(0.7f));
    CHECK_FALSE(wounds[0].bleeding);
    CHECK(bleedPerDay == doctest::Approx(0.0f));
    CHECK(wounds[0].region == WoundRegion::RightLeg);
}

TEST_CASE("Wounds: each injury def draws as its kind, scars as scars") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    struct Case {
        const char* hediff;
        WoundKind kind;
    };
    const Case cases[] = {{"Cut", WoundKind::Cut},         {"Stab", WoundKind::Cut},       {"Scratch", WoundKind::Cut},
                          {"Gunshot", WoundKind::Gunshot}, {"Bite", WoundKind::Bite},      {"Shredded", WoundKind::Bite},
                          {"Bruise", WoundKind::Bruise},   {"Crush", WoundKind::Bruise},   {"Burn", WoundKind::Burn},
                          {"Frostbite", WoundKind::Burn},  {"Scar", WoundKind::Scar},      {"OldWound", WoundKind::Scar}};
    for (const Case& c : cases) {
        WOUND_REQUIRE(woundTestInjure(*sim, pawn, c.hediff, 1.0f, "torso"));
    }
    const std::vector<WoundView> wounds = woundTestWounds(*sim, pawn);
    WOUND_REQUIRE(wounds.size() == sizeof(cases) / sizeof(cases[0]));
    for (size_t i = 0; i < wounds.size(); ++i) {
        CHECK_MESSAGE(wounds[i].kind == cases[i].kind, cases[i].hediff);
        CHECK(wounds[i].region == WoundRegion::Torso);
    }
}

TEST_CASE("Wounds: the regions follow the part and its side, down to fingers and toes") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    struct Case {
        const char* part;
        WoundRegion region;
    };
    const Case cases[] = {{"torso", WoundRegion::Torso},        {"neck", WoundRegion::Head},         {"head", WoundRegion::Head},
                          {"left eye", WoundRegion::Head},      {"nose", WoundRegion::Head},         {"jaw", WoundRegion::Head},
                          {"left shoulder", WoundRegion::LeftArm},  {"right arm", WoundRegion::RightArm},
                          {"left hand", WoundRegion::LeftArm},  {"right pinky", WoundRegion::RightArm},
                          {"left leg", WoundRegion::LeftLeg},   {"right foot", WoundRegion::RightLeg},
                          {"left big toe", WoundRegion::LeftLeg}};
    for (const Case& c : cases) {
        CHECK_MESSAGE(woundTestPart(*sim, pawn, c.part) >= 0, c.part);
        WOUND_REQUIRE(woundTestInjure(*sim, pawn, "Cut", 1.0f, c.part));
    }
    const std::vector<WoundView> wounds = woundTestWounds(*sim, pawn);
    WOUND_REQUIRE(wounds.size() == sizeof(cases) / sizeof(cases[0]));
    for (size_t i = 0; i < wounds.size(); ++i) {
        CHECK_MESSAGE(wounds[i].region == cases[i].region, cases[i].part);
        CHECK(wounds[i].partDef.valid());
    }
}

TEST_CASE("Wounds: injuries to organs and bones inside the body are not drawn, the skin over them is") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    WOUND_REQUIRE(woundTestInjure(*sim, pawn, "Cut", 2.0f, "heart"));
    WOUND_REQUIRE(woundTestInjure(*sim, pawn, "Crack", 2.0f, "left humerus"));
    CHECK(woundTestWounds(*sim, pawn).empty());
    // A bullet that goes in to the heart also hurts the torso over it.
    Rng rng(5);
    const int heart = woundTestPart(*sim, pawn, "heart");
    WOUND_REQUIRE(sim->health().applyHit(pawn, sim->health().damageTypeId("Bullet"), 4.0f, heart));
    const std::vector<WoundView> wounds = woundTestWounds(*sim, pawn);
    WOUND_REQUIRE(!wounds.empty());
    for (const WoundView& w : wounds) {
        CHECK(w.region == WoundRegion::Torso);
        CHECK(sim->defs().get(w.partDef).defName() == "Torso");
    }
}

TEST_CASE("Wounds: a lost part shows as a stump, without its children, and a bandage once tended") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    WOUND_REQUIRE(woundTestHit(*sim, pawn, "Cut", 200.0f, "left hand"));
    std::vector<WoundView> wounds = woundTestWounds(*sim, pawn);
    int missing = 0;
    for (const WoundView& w : wounds) {
        if (w.kind == WoundKind::Missing) {
            ++missing;
            CHECK(w.part == woundTestPart(*sim, pawn, "left hand"));
            CHECK(w.region == WoundRegion::LeftArm);
            CHECK(sim->defs().get(w.partDef).defName() == "Hand");
            CHECK(w.severity == doctest::Approx(1.0f));
            CHECK(w.bleeding);
            CHECK_FALSE(w.tended);
        }
    }
    CHECK(missing == 1);  // the fingers went with the hand and are not listed
    CHECK(sim->health().tendAll(pawn, 1.0f) >= 1);
    wounds = woundTestWounds(*sim, pawn);
    for (const WoundView& w : wounds) {
        if (w.kind == WoundKind::Missing) {
            CHECK(w.tended);
            CHECK_FALSE(w.bleeding);
        }
    }
}

TEST_CASE("Wounds: every injury and scar def names a kind, and every outside part of the Human lands on a region") {
    const DefDatabase& defs = *woundTestDefs();
    const char* names[] = {"Cut", "Bruise", "Burn", "Bite", "Gunshot", "Infection", "Scar", "Missing"};
    int checked = 0;
    for (const DefId id : defs.idsOfKind("Hediffs")) {
        const Def& def = defs.get(id);
        const std::string cls = def.text("hediffClass");
        if (cls != "Injury" && cls != "Scar") {
            continue;
        }
        ++checked;
        const std::string kind = def.text("woundKind");
        bool known = false;
        for (const char* name : names) {
            known = known || kind == name;
        }
        CHECK_MESSAGE(known, def.defName());
    }
    CHECK(checked >= 12);
    CHECK(std::string(woundKindName(WoundKind::Infection)) == "Infection");
    CHECK(std::string(woundRegionName(WoundRegion::RightLeg)) == "RightLeg");
    for (const char* part : {"Torso", "Neck", "Head", "Eye", "Ear", "Nose", "Jaw", "Shoulder", "Arm", "Hand", "Finger", "Leg", "Foot", "Toe"}) {
        const Def* def = defs.find("BodyParts", part);
        WOUND_REQUIRE(def != nullptr);
        CHECK_MESSAGE(!def->text("skinRegion").empty(), part);
    }
}

TEST_CASE("Wounds: the same game gives the same wounds") {
    auto a = woundTestSim();
    auto b = woundTestSim();
    const Entity pa = woundTestPawn(*a);
    const Entity pb = woundTestPawn(*b);
    for (Sim* sim : {a.get(), b.get()}) {
        const Entity pawn = sim == a.get() ? pa : pb;
        woundTestHit(*sim, pawn, "Bullet", 5.0f, "torso");
        woundTestHit(*sim, pawn, "Bite", 4.0f, "right arm");
        sim->step();
    }
    const std::vector<WoundView> wa = woundTestWounds(*a, pa);
    const std::vector<WoundView> wb = woundTestWounds(*b, pb);
    WOUND_REQUIRE(wa.size() == wb.size());
    CHECK(wa.size() >= 2);
    for (size_t i = 0; i < wa.size(); ++i) {
        CHECK(wa[i].part == wb[i].part);
        CHECK(wa[i].kind == wb[i].kind);
        CHECK(wa[i].severity == wb[i].severity);
    }
}

TEST_CASE("Wounds: a septic wound shows as an infection, in place of its wound or on its own") {
    auto sim = woundTestSim();
    const Entity pawn = woundTestPawn(*sim);
    const DefId infection = sim->health().diseaseId("Infection");
    WOUND_REQUIRE(infection.valid());
    WOUND_REQUIRE(woundTestHit(*sim, pawn, "Cut", 5.0f, "left arm"));
    WOUND_REQUIRE(woundTestHit(*sim, pawn, "Cut", 2.0f, "right leg"));
    const int arm = woundTestPart(*sim, pawn, "left arm");
    // The wound on that part turns into the infection; the other wound stays a cut.
    WOUND_REQUIRE(sim->health().contract(pawn, infection, 0.4f, arm));
    std::vector<WoundView> wounds = woundTestWounds(*sim, pawn);
    WOUND_REQUIRE(wounds.size() == 2);
    CHECK(wounds[0].part == arm);
    CHECK(wounds[0].kind == WoundKind::Infection);
    CHECK(wounds[1].kind == WoundKind::Cut);
    // A whole-body sickness shows nothing on the skin.
    WOUND_REQUIRE(sim->health().contract(pawn, sim->health().diseaseId("Flu"), 0.5f));
    CHECK(woundTestWounds(*sim, pawn).size() == 2);
    // An infection whose wound has gone stands on its own.
    const Entity other = woundTestPawn(*sim, 14, 10);
    const int foot = woundTestPart(*sim, other, "right foot");
    WOUND_REQUIRE(sim->health().contract(other, infection, 0.4f, foot));
    wounds = woundTestWounds(*sim, other);
    WOUND_REQUIRE(wounds.size() == 1);
    CHECK(wounds[0].kind == WoundKind::Infection);
    CHECK(wounds[0].part == foot);
    CHECK(wounds[0].region == WoundRegion::RightLeg);
}

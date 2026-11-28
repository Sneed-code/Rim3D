#include "doctest.h"

#include "ColonySim/BodyMotion.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Hold.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Random.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace colony;

#define HOLD_REQUIRE(...)      \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> holdTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) MESSAGE(e);
        CHECK(ok);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

WeaponHold holdTestOf(const DefDatabase& defs, const char* weapon) {
    WeaponHold hold;
    CHECK(holdFor(defs, weapon != nullptr ? defs.idOf("Things", weapon) : DefId{}, hold));
    return hold;
}

}  // namespace

TEST_CASE("Weapon hold: every weapon in the game is held through a style, with its grip points") {
    const auto defs = holdTestDefs();
    // Bare hands are fists.
    const WeaponHold fists = holdTestOf(*defs, nullptr);
    CHECK(fists.styleName == "Hold_Unarmed");
    CHECK(fists.hands == 0);
    CHECK(fists.grip == HoldGrip::None);
    // The spear's shaft runs through both fists (the Pole grip), not across one fist like a sword's hilt.
    const WeaponHold spearHold = holdTestOf(*defs, "Melee_Spear");
    CHECK(spearHold.grip == HoldGrip::Pole);
    CHECK(spearHold.hasOff);
    CHECK(std::string(holdGripName(HoldGrip::Pole)) == "Pole");
    CHECK(fists.attackStyle == "Unarmed");

    // A one-handed blade: one hand, the palm on the middle of the hilt, no second hand.
    const WeaponHold gladius = holdTestOf(*defs, "Melee_Gladius");
    CHECK(gladius.styleName == "Hold_OneHandBlade");
    CHECK(gladius.hands == 1);
    CHECK(gladius.grip == HoldGrip::Blade);
    CHECK(gladius.attackStyle == "OneHand");
    CHECK(gladius.main.x == doctest::Approx(-6.0f));
    CHECK_FALSE(gladius.hasOff);

    // The longsword: two hands on the hilt, the left below the right.
    const WeaponHold longsword = holdTestOf(*defs, "Melee_Longsword");
    CHECK(longsword.styleName == "Hold_TwoHandBlade");
    CHECK(longsword.hands == 2);
    CHECK(longsword.hasOff);
    CHECK(longsword.off.x < longsword.main.x);
    CHECK(longsword.attackStyle == "TwoHand");

    // A spear: hands far apart on the shaft. A mace swings like a club.
    const WeaponHold spear = holdTestOf(*defs, "Melee_Spear");
    CHECK(spear.styleName == "Hold_Spear");
    CHECK(spear.off.x - spear.main.x >= 40.0f);
    CHECK(holdTestOf(*defs, "Melee_Mace").attackStyle == "Club");

    // Guns: a pistol in one hand, raked back; a rifle with the other hand on the fore-end; a bow in the left hand.
    const WeaponHold pistol = holdTestOf(*defs, "Gun_Autopistol");
    CHECK(pistol.styleName == "Hold_Pistol");
    CHECK(pistol.grip == HoldGrip::Gun);
    CHECK(pistol.rake == doctest::Approx(22.0f));
    CHECK(pistol.attackStyle.empty());
    const WeaponHold rifle = holdTestOf(*defs, "Gun_AssaultRifle");
    CHECK(rifle.styleName == "Hold_Rifle");
    CHECK(rifle.hands == 2);
    CHECK(rifle.hasOff);
    CHECK(rifle.off.x > 10.0f);
    const WeaponHold bow = holdTestOf(*defs, "Gun_RecurveBow");
    CHECK(bow.grip == HoldGrip::Bow);
    CHECK(bow.hand == HoldHand::Left);

    // Every weapon def has a style of its own in the data, and every melee style's attacks are a list MeleeRules has.
    const Def* rules = defs->find("Rules", "MeleeRules");
    HOLD_REQUIRE(rules != nullptr);
    const std::vector<std::string> attackStyles = rules->keys("styleAttacks");
    const std::set<std::string> known(attackStyles.begin(), attackStyles.end());
    int weapons = 0;
    for (const DefId id : defs->idsOfKind("Things")) {
        const Def& def = defs->get(id);
        const bool gun = def.has("verbs") && !def.flag("turretGun");  // a turret's own gun is never held
        const bool melee = def.count("tools") > 0 && !def.has("race") && def.has("hold");
        if (!gun && !melee) {
            continue;
        }
        ++weapons;
        WeaponHold hold;
        CHECK(holdFor(*defs, id, hold));
        INFO(def.defName());
        CHECK(def.has("hold.style"));
        CHECK(hold.hands >= 1);
        if (hold.hands == 2 && hold.grip != HoldGrip::Bow) {
            CHECK(hold.hasOff);
        }
        if (!hold.attackStyle.empty()) {
            CHECK(known.count(hold.attackStyle) == 1);
        }
    }
    CHECK(weapons >= 30);

    // Shields go through the same system: on the left forearm.
    WeaponHold shield;
    CHECK(shieldHoldFor(*defs, defs->idOf("Things", "Apparel_ShieldMedium"), shield));
    CHECK(shield.grip == HoldGrip::Shield);
    CHECK(shield.hand == HoldHand::Left);
    CHECK_FALSE(shieldHoldFor(*defs, defs->idOf("Things", "Melee_Gladius"), shield));
}

TEST_CASE("Weapon hold: a new weapon needs only a hold block, and one without gets a style by its kind") {
    std::vector<DefSource> sources;
    sources.push_back(DefSource{"HoldStyles", "styles.json", R"([
        {"defName": "Hold_Unarmed", "hands": 0, "grip": "None", "attackStyle": "Unarmed"},
        {"defName": "Hold_OneHandBlade", "hands": 1, "grip": "Blade", "attackStyle": "OneHand", "main": [-5, 0, 0]},
        {"defName": "Hold_TwoHandBlade", "hands": 2, "grip": "Blade", "attackStyle": "TwoHand", "main": [-4, 0, 0], "off": [-13, 0, 0]},
        {"defName": "Hold_Pistol", "hands": 1, "grip": "Gun", "rake": 22},
        {"defName": "Hold_Rifle", "hands": 2, "grip": "Gun", "rake": 18, "off": [34, 0, -2]},
        {"defName": "Hold_Halberd", "hands": 2, "grip": "Blade", "attackStyle": "TwoHand", "main": [0, 0, 0], "off": [50, 0, 0]}
    ])"});
    sources.push_back(DefSource{"Things", "things.json", R"([
        {"defName": "TestHalberd", "hold": {"style": "Hold_Halberd", "main": [-30, 0, 0]}, "tools": [{"label": "blade"}]},
        {"defName": "TestDagger", "tools": [{"label": "point"}]},
        {"defName": "TestGreatAxe", "twoHanded": true, "tools": [{"label": "edge"}]},
        {"defName": "TestPistol", "verbs": [{"range": 20}]},
        {"defName": "TestRifle", "twoHanded": true, "verbs": [{"range": 30}]},
        {"defName": "TestOddStyle", "hold": {"style": "Hold_Missing"}, "tools": [{"label": "edge"}]},
        {"defName": "TestRock"}
    ])"});
    DefDatabase defs;
    std::vector<std::string> errors;
    HOLD_REQUIRE(DefDatabase::build(sources, "", defs, errors));

    // Its own style and grip; the style's off point stays.
    const WeaponHold halberd = holdTestOf(defs, "TestHalberd");
    CHECK(halberd.styleName == "Hold_Halberd");
    CHECK(halberd.main.x == doctest::Approx(-30.0f));
    CHECK(halberd.off.x == doctest::Approx(50.0f));
    CHECK(halberd.hasOff);
    // No hold: by kind and twoHanded, with the style's grip points.
    CHECK(holdTestOf(defs, "TestDagger").styleName == "Hold_OneHandBlade");
    CHECK(holdTestOf(defs, "TestDagger").main.x == doctest::Approx(-5.0f));
    CHECK(holdTestOf(defs, "TestGreatAxe").styleName == "Hold_TwoHandBlade");
    CHECK(holdTestOf(defs, "TestPistol").styleName == "Hold_Pistol");
    const WeaponHold rifle = holdTestOf(defs, "TestRifle");
    CHECK(rifle.styleName == "Hold_Rifle");
    CHECK(rifle.off.x == doctest::Approx(34.0f));
    // A style that isn't there falls back by kind; a thing that is no weapon is held as fists.
    CHECK(holdTestOf(defs, "TestOddStyle").styleName == "Hold_OneHandBlade");
    CHECK(holdTestOf(defs, "TestRock").styleName == "Hold_Unarmed");

    // Without any styles there is nothing to hold by.
    DefDatabase empty;
    WeaponHold none;
    CHECK_FALSE(holdFor(empty, DefId{}, none));
}

TEST_CASE("Weapon hold: a melee weapon swings the attack clips of its hold style") {
    const auto defs = holdTestDefs();
    Sim sim(SimSettings{3, 64, 0, 0, true}, defs);
    const BodyMotionSystem& motion = sim.combat().motion();
    const auto clipsOf = [&](const char* weapon) {
        std::vector<std::string> names;
        for (const DefId clip : sim.melee().attackClipsOf(defs->idOf("Things", weapon))) {
            const MotionClip* c = motion.clip(clip);
            names.push_back(c != nullptr ? c->name : std::string());
        }
        return names;
    };
    const auto allStartWith = [](const std::vector<std::string>& names, const std::string& prefix) {
        for (const std::string& n : names) {
            if (n.rfind(prefix, 0) != 0) {
                return false;
            }
        }
        return true;
    };
    const std::vector<std::string> mace = clipsOf("Melee_Mace");
    const std::vector<std::string> spear = clipsOf("Melee_Spear");
    const std::vector<std::string> gladius = clipsOf("Melee_Gladius");
    CHECK_FALSE(mace.empty());
    CHECK_FALSE(spear.empty());
    CHECK_FALSE(gladius.empty());
    CHECK(allStartWith(mace, "Motion_Club_"));
    // The spear's own thrusts once they are measured (MeleeRules.styleAttacks.Melee_Spear), else its two-hand style's.
    CHECK((allStartWith(spear, "Motion_Spear_") || allStartWith(spear, "Motion_TwoHand_")));
    CHECK(allStartWith(gladius, "Motion_OneHand_"));
}

TEST_CASE("Weapon hold: a body carried to bed is on the carrier's back in the snapshot until it is laid down") {
    const auto defs = holdTestDefs();
    Sim sim(SimSettings{7, 64, 0, 0, true}, defs);
    sim.mentalBreaks().setNatural(false);
    const Entity bed = sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 5);
    HOLD_REQUIRE(bed != kNoEntity);
    const auto colonist = [&](CellPos cell) {
        const Entity pawn = sim.things().spawn("Human", cell);
        sim.jobs().addWorker(pawn);
        for (int h = 0; h < 24; ++h) {
            sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
        }
        return pawn;
    };
    const Entity doctor = colonist(CellPos{10, 10, 0});
    const Entity patient = colonist(CellPos{30, 30, 0});
    sim.submit(SetWorkPriorityCommand{patient, "Doctor", 0});
    sim.step();
    // Down with a broken leg, not bleeding (so it is carried before it is tended).
    Rng rng(99);
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(patient).def);
    int leg = -1;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == "left leg") {
            leg = static_cast<int>(i);
        }
    }
    HOLD_REQUIRE(leg >= 0);
    sim.health().applyDamage(patient, sim.health().damageTypeId("Blunt"), 60.0f, leg, rng);
    sim.step();
    HOLD_REQUIRE(sim.things().registry().get<Health>(patient).downed);

    bool carried = false;
    bool laid = false;
    Snapshot snap;
    for (int i = 0; i < 6000 && !laid; ++i) {
        sim.step();
        sim.writeSnapshot(snap);
        const ThingView* d = snap.find(doctor);
        const ThingView* p = snap.find(patient);
        HOLD_REQUIRE(d != nullptr);
        HOLD_REQUIRE(p != nullptr);
        if (d->carryingPawn == patient) {
            carried = true;
            CHECK(p->carriedBy == doctor);
            CHECK(sim.jobs().carryingPawn(doctor) == patient);
        } else {
            CHECK(p->carriedBy == kNoEntity);
        }
        for (const Event& e : sim.events()) {
            laid = laid || (e.type == EventType::PawnAction && e.action == PawnAction::LayPawn && e.target == patient);
        }
    }
    CHECK(carried);
    CHECK(laid);
    sim.step();
    sim.writeSnapshot(snap);
    CHECK(snap.find(doctor)->carryingPawn == kNoEntity);
    CHECK(snap.find(patient)->carriedBy == kNoEntity);
}

TEST_CASE("Weapon hold: every held weapon has a place on the body, and the hands follow it, in both views") {
    const auto defs = holdTestDefs();
    // Every weapon a pawn can hold (guns and melee) is put somewhere by its style, first person and third, so its hands
    // close on its grips (docs/design/weapon-hold.md, "Weapon first, then the hands").
    int weapons = 0;
    for (size_t i = 0; i < defs->size(); ++i) {
        const Def& def = defs->get(DefId{static_cast<uint32_t>(i)});
        if (def.kind() != "Things" || def.flag("abstract") || !(def.has("verbs") || (def.count("tools") > 0 && !def.has("race")))) {
            continue;
        }
        if (def.has("race") || def.has("building") || def.has("apparel")) {
            continue;
        }
        WeaponHold hold;
        HOLD_REQUIRE(holdFor(*defs, def.id(), hold));
        if (hold.grip == HoldGrip::None || hold.grip == HoldGrip::Bow) {
            continue;  // fists keep their guard; bows draw with their own clip
        }
        ++weapons;
        INFO(def.defName());
        CHECK(holdPlaceFor(hold, false, false, false) != nullptr);
        CHECK(holdPlaceFor(hold, true, false, false) != nullptr);
        CHECK(holdPlaceFor(hold, true, false, false)->anchor == HoldAnchor::View);
        if (hold.grip == HoldGrip::Gun) {
            const HoldPlace* aimed = holdPlaceFor(hold, false, true, false);
            HOLD_REQUIRE(aimed != nullptr);
            // Aimed: the sight before the eye, or the stock in the shoulder (rifles' cheek weld, the big guns low on
            // the shoulder: sneed 2026-11-23).
            const bool sighted = aimed->point == HoldPlacePoint::Sight && aimed->anchor == HoldAnchor::Head;
            const bool shouldered = aimed->point == HoldPlacePoint::Stock && aimed->anchor == HoldAnchor::Shoulder;
            CHECK((sighted || shouldered));
            CHECK(holdPlaceFor(hold, true, true, false)->point == HoldPlacePoint::Sight);
        } else {
            CHECK(holdPlaceFor(hold, false, false, true) != nullptr);
        }
    }
    CHECK(weapons > 20);

    // A rifle: aimed, the sight goes before the eye; ready, a low ready with the grip before the belly and the stock
    // tucked under the arm, muzzle down and across the body (both hands within reach: PC run 2026-11-19); the support
    // hand's palm lies under the fore-end.
    const WeaponHold rifle = holdTestOf(*defs, "Gun_AssaultRifle");
    CHECK(rifle.aim.set);
    CHECK(rifle.aim.point == HoldPlacePoint::Stock);  // the stock in the shoulder, cheek on it (sneed's picture 2026-11-23)
    CHECK(rifle.aim.anchor == HoldAnchor::Shoulder);
    CHECK(rifle.ready.pitch < -10.0f);
    CHECK(rifle.ready.yaw < 0.0f);
    CHECK(rifle.hasOff);
    CHECK(rifle.offRoll > 45.0f);
    // A pistol is one-handed for the sim, with a support hand for drawing.
    const WeaponHold pistol = holdTestOf(*defs, "Gun_Autopistol");
    CHECK(pistol.hands == 1);
    CHECK_FALSE(pistol.hasOff);
    CHECK(pistol.hasSupport);
    // Melee: blocking has its own place; first person falls back to third only when it has none.
    const WeaponHold sword = holdTestOf(*defs, "Melee_Longsword");
    CHECK(holdPlaceFor(sword, false, false, true) == &sword.block);
    CHECK(holdPlaceFor(sword, true, false, true) == &sword.fpBlock);
    CHECK(holdPlaceFor(sword, false, false, false) == &sword.ready);
    // With a shield on the arm (sneed 2026-11-28): the one-handed blade's own shield places, beside the shield and held up
    // behind its edge in a guard; a style without them keeps its own; first person keeps its own.
    const WeaponHold gladius = holdTestOf(*defs, "Melee_Gladius");
    CHECK(gladius.shieldBlock.set);
    CHECK(gladius.shieldReady.set);
    CHECK(holdPlaceFor(gladius, false, false, true, true) == &gladius.shieldBlock);
    CHECK(holdPlaceFor(gladius, false, false, false, true) == &gladius.shieldReady);
    CHECK(holdPlaceFor(gladius, false, false, true, false) == &gladius.block);
    CHECK(holdPlaceFor(gladius, true, false, true, true) == &gladius.fpBlock);
    CHECK(holdPlaceFor(sword, false, false, true, true) == &sword.block);
}

#include "ColonySim/Defs.h"
#include "ColonySim/Sound.h"

#include <doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace colony;

namespace {

bool soundTestLoad(DefDatabase& db) {
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    return ok;
}

std::vector<SoundPlay> soundTestPlay(const DefDatabase& db, const Event& e, const SoundDefOf& defOf = {}) {
    std::vector<SoundPlay> out;
    soundForEvent(db, e, defOf, out);
    return out;
}

}  // namespace

// V20: cues in the defs and the event mapping (docs/design/sound.md).
TEST_CASE("sound: every cue a def names is a SoundCues def, with a placeholder tone") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    size_t named = 0;
    for (const std::string& kind : db.kinds()) {
        for (DefId id : db.idsOfKind(kind)) {
            const Def& def = db.get(id);
            for (const std::string& slot : def.keys("visual.sound")) {
                if (slot == "material" || slot == "impactSet") continue;  // what it is made of, a family: not cues
                const std::string cue = soundSlot(def, slot.c_str());
                if (cue.empty()) continue;  // an empty slot clears the parent's (a bow throws no spent case)
                const Def* c = db.find("SoundCues", cue);
                CHECK_MESSAGE(c != nullptr, def.defName() << " " << slot << " -> " << cue);
                ++named;
            }
        }
    }
    CHECK(named > 80);
    for (DefId id : db.idsOfKind("SoundCues")) {
        const Def& c = db.get(id);
        CHECK_MESSAGE(c.number("volume") > 0.0, c.defName());
        CHECK_MESSAGE((c.number("tone.hz") > 0.0 && c.number("tone.seconds") > 0.0), c.defName());
        CHECK_MESSAGE((c.number("priority", -1) >= 0 && c.number("priority") <= 3), c.defName());
        // A loop with no range is a bed heard everywhere; a one-shot with no range is a global chime.
        CHECK_MESSAGE(c.number("range") >= 0.0, c.defName());
    }
}

TEST_CASE("sound: guns, animals, melee weapons, terrain and doors have their slots") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    for (const char* gun : {"Gun_Revolver", "Gun_Autopistol", "Gun_BoltActionRifle", "Gun_PumpShotgun", "Gun_AssaultRifle", "Gun_MiniTurret"}) {
        const Def* d = db.find("Things", gun);
        if (d == nullptr) { CHECK_MESSAGE(false, gun); continue; }
        CHECK_MESSAGE(!soundSlot(*d, "shot").empty(), gun);
    }
    // Different guns, different shots.
    CHECK(soundSlot(*db.find("Things", "Gun_BoltActionRifle"), "shot") != soundSlot(*db.find("Things", "Gun_Revolver"), "shot"));
    CHECK(soundSlot(*db.find("Things", "Gun_Revolver"), "reload") == "Gun_Reload");  // inherited from GunBase
    for (DefId id : db.idsOfKind("Things")) {
        const Def& d = db.get(id);
        if (d.text("category") == "Pawn" && d.has("race.body") && d.defName() != "Human") {
            CHECK_MESSAGE((!soundSlot(d, "idle").empty() && !soundSlot(d, "hurt").empty() && !soundSlot(d, "death").empty()), d.defName());
        }
    }
    CHECK(soundSlot(*db.find("Things", "Melee_Spear"), "swing") == "Swing_Spear");
    CHECK(soundSlot(*db.find("Things", "Melee_Knife"), "swing") == "Swing_Blade");  // inherited
    CHECK(soundSlot(*db.find("Things", "Door"), "open") == "Door_Open");
    CHECK(soundSlot(*db.find("Things", "Door"), "close") == "Door_Close");
    // The door's mesh and parts are still there beside the sound.
    CHECK(db.find("Things", "Door")->has("visual.mesh"));
    CHECK(soundStepCue(db, db.idOf("Terrain", "WoodFloor")) == "Step_Wood");
    CHECK(soundStepCue(db, db.idOf("Terrain", "Granite_Rough")) == "Step_Stone");
    CHECK(soundStepCue(db, db.idOf("Terrain", "Soil")) == "Step_Grass");
    CHECK(soundStepCue(db, db.idOf("Terrain", "Sand")) == "Step_Sand");
    CHECK(soundStepCue(db, db.idOf("Terrain", "WaterShallow")) == "Step_Water");
    CHECK(soundStepCue(db, DefId{}) == "Step_Dirt");
    for (DefId id : db.idsOfKind("Terrain")) CHECK_MESSAGE(!soundStepCue(db, id).empty(), db.get(id).defName());
    for (DefId id : db.idsOfKind("Biomes")) {
        const Def& b = db.get(id);
        CHECK_MESSAGE((!soundSlot(b, "day").empty() && !soundSlot(b, "night").empty()), b.defName());
    }
    CHECK(soundSlot(*db.find("Weathers", "RainyThunderstorm"), "thunder") == "Weather_Thunder");
    CHECK(soundSlot(*db.find("Weathers", "Clear"), "loop").empty());
}

TEST_CASE("sound: events turn into cues") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }

    Event shot;
    shot.type = EventType::ShotFired;
    shot.def = db.idOf("Things", "Gun_BoltActionRifle");
    shot.x = 10; shot.y = 20; shot.z = 1.5f;
    auto plays = soundTestPlay(db, shot);
    if (plays.size() != 1) { CHECK(plays.size() == 1); return; }
    CHECK(plays[0].cue == "Shot_Rifle");
    CHECK(plays[0].hasPos);
    CHECK(plays[0].x == doctest::Approx(10.0f));
    CHECK(plays[0].z == doctest::Approx(1.5f));

    Event hit;
    hit.type = EventType::ProjectileHit;
    hit.def = db.idOf("Projectiles", "Bullet_Revolver");
    hit.hit = ProjectileHitKind::Pawn;
    CHECK(soundTestPlay(db, hit).at(0).cue == "Flesh_Bullet");
    hit.hit = ProjectileHitKind::Armor;
    CHECK(soundTestPlay(db, hit).at(0).cue == "Impact_Steel");
    hit.hit = ProjectileHitKind::Rock;
    CHECK(soundTestPlay(db, hit).at(0).cue == "Impact_Stone");
    hit.hit = ProjectileHitKind::Gone;
    CHECK(soundTestPlay(db, hit).empty());

    Event swing;
    swing.type = EventType::MeleeSwing;
    swing.def = db.idOf("Things", "Melee_Club");
    CHECK(soundTestPlay(db, swing).at(0).cue == "Swing_Blunt");
    Event mh;
    mh.type = EventType::MeleeHit;
    mh.def = db.idOf("Things", "Melee_Club");
    mh.message = "hit";
    mh.hit = ProjectileHitKind::Pawn;
    CHECK(soundTestPlay(db, mh).at(0).cue == "Hit_Wood_Flesh");
    mh.message = "deflected";
    mh.hit = ProjectileHitKind::Armor;
    CHECK(soundTestPlay(db, mh).at(0).cue == "Hit_Wood_Steel");
    mh.message = "missed";
    CHECK(soundTestPlay(db, mh).empty());
    mh.message = "hit";
    mh.hit = ProjectileHitKind::Pawn;
    mh.def = DefId{};
    mh.message = "left fist";
    CHECK(soundTestPlay(db, mh).at(0).cue == "Punch");

    Event boom;
    boom.type = EventType::Explosion;
    boom.def = db.idOf("DamageTypes", "Bomb");
    CHECK(soundTestPlay(db, boom).at(0).cue == "Explosion_Blast");
    boom.def = db.idOf("DamageTypes", "Burn");
    CHECK(soundTestPlay(db, boom).at(0).cue == "Explosion_Fire");

    // Fire starts a loop at its cell and ends it with the same identity.
    Event fire;
    fire.type = EventType::FireStarted;
    fire.cell = {4, 5, 0};
    plays = soundTestPlay(db, fire);
    CHECK(plays.size() == 2);
    CHECK(plays.at(1).mode == SoundMode::LoopStart);
    fire.type = EventType::FireOut;
    plays = soundTestPlay(db, fire);
    CHECK(plays.at(0).mode == SoundMode::LoopStop);
    CHECK(plays.at(0).cell == CellPos{4, 5, 0});

    // The colony alarm is a loop heard everywhere.
    Event alarm;
    alarm.type = EventType::AlarmChanged;
    alarm.message = "on: raiders";
    plays = soundTestPlay(db, alarm);
    CHECK(plays.at(0).mode == SoundMode::LoopStart);
    CHECK(!plays.at(0).hasPos);
    alarm.message = "off: calm";
    CHECK(soundTestPlay(db, alarm).at(0).mode == SoundMode::LoopStop);

    // Events that name only an entity find its def through the callback; no callback, no sound.
    const Entity pawn = static_cast<Entity>(7);
    const DefId human = db.idOf("Things", "Human");
    const SoundDefOf defOf = [&](Entity e) { return e == pawn ? human : DefId{}; };
    Event hurt;
    hurt.type = EventType::PawnInjured;
    hurt.thing = pawn;
    CHECK(soundTestPlay(db, hurt, defOf).at(0).cue == "Pawn_Hurt");
    CHECK(soundTestPlay(db, hurt).empty());
    hurt.type = EventType::PawnDied;
    CHECK(soundTestPlay(db, hurt, defOf).at(0).cue == "Pawn_Death");

    // A smithy rings, a stove sizzles: the bench's `work` slot beats the default.
    Event act;
    act.type = EventType::PawnAction;
    act.action = PawnAction::Cook;
    act.thing = pawn;
    act.target = static_cast<Entity>(9);
    const DefId smithy = db.idOf("Things", "FueledSmithy");
    const SoundDefOf benchOf = [&](Entity e) { return e == act.target ? smithy : DefId{}; };
    CHECK(soundTestPlay(db, act, benchOf).at(0).cue == "Work_Smith");
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Cook");
    act.action = PawnAction::Mine;
    act.cell = {3, 3, 0};
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Mine");
    CHECK(soundTestPlay(db, act).at(0).hasPos);
    act.action = PawnAction::Roll;
    CHECK(soundTestPlay(db, act).at(0).cue == "Dodge");

    // Stealth: a sting when a fighter grows suspicious or is alerted, none when it calms down or is ordered.
    act.action = PawnAction::Alert;
    act.amount = 1.0f;
    act.message = "heard steps";
    CHECK(soundTestPlay(db, act).at(0).cue == "Alert_Suspicious");
    act.amount = 2.0f;
    act.message = "saw";
    CHECK(soundTestPlay(db, act).at(0).cue == "Alert_Spotted");
    act.message = "ordered";
    CHECK(soundTestPlay(db, act).empty());
    act.amount = 1.0f;
    act.message = "lost them";
    CHECK(soundTestPlay(db, act).empty());
    act.amount = 0.0f;
    act.message = "calm";
    CHECK(soundTestPlay(db, act).empty());
}

TEST_CASE("sound: same event, same cues") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    Event shot;
    shot.type = EventType::ShotFired;
    shot.def = db.idOf("Things", "Gun_Revolver");
    const auto a = soundTestPlay(db, shot);
    const auto b = soundTestPlay(db, shot);
    CHECK(a.size() == b.size());
    CHECK(a.at(0).cue == b.at(0).cue);
}

TEST_CASE("sound: a woman grunts at some hard melee hits and screams when downed or killed") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const Entity woman = static_cast<Entity>(11);
    const Entity man = static_cast<Entity>(12);
    const Entity beast = static_cast<Entity>(13);
    const DefId human = db.idOf("Things", "Human");
    const SoundDefOf defOf = [&](Entity e) { return e == woman || e == man ? human : DefId{}; };
    const SoundGenderOf genderOf = [&](Entity e) {
        return e == woman ? SoundGender::Female : e == man ? SoundGender::Male : SoundGender::Unknown;
    };
    const auto voices = [&](const Event& e) {
        std::vector<SoundPlay> out;
        soundForEvent(db, e, defOf, out, genderOf);
        std::vector<SoundPlay> v;
        for (const SoundPlay& p : out) if (p.voice != SoundVoice::None) v.push_back(p);
        return v;
    };

    Event mh;
    mh.type = EventType::MeleeHit;
    mh.def = db.idOf("Things", "Melee_Club");
    mh.message = "hit";
    mh.hit = ProjectileHitKind::Pawn;
    mh.target = woman;
    mh.amount = 8.0f;
    // About half the hard hits, decided by the event alone (the same event always gives the same answer).
    int grunts = 0;
    for (int i = 0; i < 400; ++i) {
        mh.x = static_cast<float>(i) * 0.37f;
        mh.y = static_cast<float>(i % 17);
        const auto v = voices(mh);
        CHECK(v.size() == voices(mh).size());
        if (!v.empty()) {
            ++grunts;
            CHECK(v[0].cue == "Voice_FemHit");
            CHECK(v[0].voice == SoundVoice::Hit);
            CHECK(v[0].entity == woman);
            CHECK(v[0].hasPos);
        }
    }
    CHECK(grunts > 160);
    CHECK(grunts < 240);
    // The club's own hit sound (wood on flesh) still plays with it.
    CHECK(soundTestPlay(db, mh, defOf).at(0).cue == "Hit_Wood_Flesh");

    // 3 damage or less, a blocked or deflected blow, an animal, or no gender callback: no grunt.
    bool any = false;
    for (int i = 0; i < 100; ++i) {
        mh.x = static_cast<float>(i);
        mh.amount = 3.0f;
        any = any || !voices(mh).empty();
        mh.amount = 8.0f;
        mh.target = beast;
        any = any || !voices(mh).empty();
        mh.target = woman;
        mh.message = "blocked";
        any = any || !voices(mh).empty();
        mh.message = "hit";
        std::vector<SoundPlay> out;
        soundForEvent(db, mh, defOf, out);
        for (const SoundPlay& p : out) any = any || p.voice != SoundVoice::None;
    }
    CHECK(!any);

    // Downed: always a scream, at the pawn and following it.
    Event down;
    down.type = EventType::PawnDowned;
    down.thing = woman;
    down.cell = {5, 6, 0};
    auto v = voices(down);
    if (v.size() != 1) { CHECK(v.size() == 1); return; }
    CHECK(v[0].cue == "Voice_FemScream");
    CHECK(v[0].voice == SoundVoice::Scream);
    CHECK(v[0].entity == woman);
    CHECK(v[0].x == doctest::Approx(5.5f));
    down.thing = man;  // men have recordings now too (2026-11-20)
    CHECK(voices(down).at(0).cue == "Voice_MaleScream");

    // Killed in a fight screams, bleeding out from it too; cold or illness do not.
    Event died;
    died.type = EventType::PawnDied;
    died.thing = woman;
    died.message = "a vital part destroyed";
    CHECK(voices(died).size() == 1);
    died.message = "executed";
    CHECK(voices(died).size() == 1);
    died.message = "blood loss";
    CHECK(voices(died).size() == 1);
    died.message = "hypothermia";
    CHECK(voices(died).empty());
    died.message = "illness";
    CHECK(voices(died).empty());

    // Killed while she lay downed (awake or knocked out): she screams again, every time.
    died.message = "a vital part destroyed";
    for (const float amount : {0.0f, 1.0f, 2.0f}) {
        died.amount = amount;
        CHECK(voices(died).size() == 1);
    }
}

TEST_CASE("sound: a woman grunts at some hard shots too") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const Entity woman = static_cast<Entity>(21);
    const DefId human = db.idOf("Things", "Human");
    const SoundDefOf defOf = [&](Entity e) { return e == woman ? human : DefId{}; };
    const SoundGenderOf genderOf = [&](Entity e) { return e == woman ? SoundGender::Female : SoundGender::Unknown; };
    Event shot;
    shot.type = EventType::ProjectileHit;
    shot.def = db.idOf("Projectiles", "Bullet_Revolver");
    shot.hit = ProjectileHitKind::Pawn;
    shot.target = woman;
    int grunts = 0;
    bool weak = false;
    bool armour = false;
    for (int i = 0; i < 400; ++i) {
        shot.x = static_cast<float>(i) * 0.41f;
        shot.y = static_cast<float>(i % 13);
        const auto count = [&](float amount, ProjectileHitKind kind) {
            shot.amount = amount;
            shot.hit = kind;
            std::vector<SoundPlay> out;
            soundForEvent(db, shot, defOf, out, genderOf);
            int n = 0;
            for (const SoundPlay& p : out) {
                if (p.voice == SoundVoice::Hit) {
                    ++n;
                    CHECK(p.cue == "Voice_FemHit");
                    CHECK(p.entity == woman);
                }
            }
            return n;
        };
        grunts += count(12.0f, ProjectileHitKind::Pawn);
        weak = weak || count(3.0f, ProjectileHitKind::Pawn) != 0;
        armour = armour || count(12.0f, ProjectileHitKind::Armor) != 0;
    }
    CHECK(grunts > 160);
    CHECK(grunts < 240);
    CHECK(!weak);
    CHECK(!armour);
}

TEST_CASE("sound: voice cues carry their rules") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const Def* hit = db.find("SoundCues", "Voice_FemHit");
    const Def* scream = db.find("SoundCues", "Voice_FemScream");
    if (hit == nullptr || scream == nullptr) { CHECK(false); return; }
    CHECK(hit->number("chance", 0.0) == doctest::Approx(0.5));
    CHECK(hit->number("minDamage", 0.0) == doctest::Approx(3.0));
    CHECK(hit->number("cooldown", 0.0) == doctest::Approx(5.0));
    CHECK(scream->number("cooldown", 0.0) == doctest::Approx(0.0));
    CHECK(scream->integer("priority", 3) == 0);
}

TEST_CASE("sound: lovin sounds by who is in the bed") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    using G = SoundGender;
    CHECK(soundLovinCue(G::Male, G::Female) == "Voice_LovinCouple");
    CHECK(soundLovinCue(G::Female, G::Male) == "Voice_LovinCouple");
    CHECK(soundLovinCue(G::Female, G::Female) == "Voice_LovinFem");
    CHECK(soundLovinCue(G::Female, G::Unknown) == "Voice_LovinFem");  // a woman alone
    CHECK(soundLovinCue(G::Male, G::Male).empty());
    CHECK(soundLovinCue(G::Male, G::Unknown).empty());
    CHECK(soundLovinCue(G::Unknown, G::Unknown).empty());
    for (const char* cue : {"Voice_LovinCouple", "Voice_LovinFem", "Amb_Birds_Tropical", "Amb_Tropical_Day", "Amb_Tropical_Night"}) {
        CHECK_MESSAGE(db.find("SoundCues", cue) != nullptr, cue);
    }
    // Through a closed room it must not carry far: a short range.
    CHECK(db.find("SoundCues", "Voice_LovinCouple")->number("range", 0.0) <= 15.0);
    const Def* rain = db.find("Biomes", "TropicalRainforest");
    if (rain == nullptr) { CHECK(false); return; }
    CHECK(soundSlot(*rain, "birds") == "Amb_Birds_Tropical");
    CHECK(soundSlot(*rain, "night") == "Amb_Tropical_Night");
}

TEST_CASE("sound: melee blows sound by what strikes what") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const DefId club = db.idOf("Things", "Melee_Club");
    const DefId mace = db.idOf("Things", "Melee_Mace");
    const DefId knife = db.idOf("Things", "Melee_Knife");
    const DefId steel = db.idOf("Things", "Steel");
    const DefId wood = db.idOf("Things", "WoodLog");
    CHECK(soundMaterialOf(db, steel) == "Steel");
    CHECK(soundMaterialOf(db, wood) == "Wood");
    CHECK(soundMaterialOf(db, club) == "Wood");
    CHECK(soundMaterialOf(db, mace) == "Steel");
    Event e;
    e.type = EventType::MeleeHit;
    e.hit = ProjectileHitKind::Pawn;
    e.message = "hit";
    // Blunt blows on flesh by the weapon's stuff (or its def): a wooden club thuds, a steel one or a mace smacks.
    e.def = club;
    CHECK(soundMeleeCue(db, e) == "Hit_Wood_Flesh");
    e.stuff = steel;
    CHECK(soundMeleeCue(db, e) == "Hit_Steel_Flesh");
    e.stuff = DefId{};
    e.def = mace;
    CHECK(soundMeleeCue(db, e) == "Hit_Steel_Flesh");
    // A blade sounds like itself.
    e.def = knife;
    CHECK(soundMeleeCue(db, e) == "Flesh_Cut");
    // Parried by a wooden club, blocked by a steel shield, turned by armour, into a wall or rock.
    e.def = mace;
    e.message = "blocked";
    e.struckDef = club;
    e.struckStuff = wood;
    CHECK(soundMeleeCue(db, e) == "Hit_Steel_Wood");
    e.def = club;
    e.struckStuff = steel;
    CHECK(soundMeleeCue(db, e) == "Hit_Wood_Steel");
    e.message = "deflected";
    e.hit = ProjectileHitKind::Armor;
    e.struckDef = DefId{};
    e.struckStuff = DefId{};
    CHECK(soundMeleeCue(db, e) == "Hit_Wood_Steel");
    e.message = "missed";
    e.hit = ProjectileHitKind::None;
    CHECK(soundMeleeCue(db, e).empty());  // a swing through air
    // Into rock or a wall the sim says "missed" (no pawn struck) with what it hit.
    e.hit = ProjectileHitKind::Rock;
    e.def = knife;
    e.stuff = steel;
    CHECK(soundMeleeCue(db, e) == "Hit_Steel_Stone");
    e.hit = ProjectileHitKind::Building;
    e.struckDef = db.idOf("Things", "Wall");
    e.struckStuff = wood;
    CHECK(soundMeleeCue(db, e) == "Hit_Steel_Wood");
    // Every pairing has its cue.
    for (const char* w : {"Wood", "Steel"}) {
        for (const char* s2 : {"Flesh", "Wood", "Steel", "Stone"}) {
            CHECK_MESSAGE(db.find("SoundCues", std::string("Hit_") + w + "_" + s2) != nullptr, w << " " << s2);
        }
    }
    // Fists: a punch on a body, a clang on plate.
    e = Event();
    e.type = EventType::MeleeHit;
    e.def = db.idOf("Things", "Human");
    e.hit = ProjectileHitKind::Pawn;
    e.message = "hit";
    CHECK(soundMeleeCue(db, e) == "Punch");
    e.message = "deflected";
    e.hit = ProjectileHitKind::Armor;
    CHECK(soundMeleeCue(db, e) == "Melee_Clang");
}

TEST_CASE("sound: shots and blasts have a far layer and carry like real ones") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    for (const char* shot : {"Shot_Revolver", "Shot_Pistol", "Shot_Rifle", "Shot_Shotgun", "Shot_AssaultRifle", "Shot_Turret",
                             "Explosion_Blast", "Explosion_Fire"}) {
        const Def* c = db.find("SoundCues", shot);
        if (c == nullptr) { CHECK_MESSAGE(false, shot); continue; }
        CHECK_MESSAGE(db.find("SoundCues", c->text("far")) != nullptr, shot);
        CHECK_MESSAGE(c->number("farFrom") > 0.0, shot);
        CHECK_MESSAGE(c->number("range") >= 1000.0, shot);  // a gunshot carries a kilometre and more
    }
    CHECK(db.find("SoundCues", "Shot_Rifle")->number("range") > db.find("SoundCues", "Shot_Pistol")->number("range"));
    CHECK(db.find("SoundCues", "Explosion_Blast")->number("range") > db.find("SoundCues", "Shot_Rifle")->number("range"));
}

// sneed, 2026-11-20 (docs/design/sound.md "Every weapon, surface and job").
TEST_CASE("sound: every gun sounds like itself") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const auto shotOf = [&](const char* gun) {
        Event shot;
        shot.type = EventType::ShotFired;
        shot.def = db.idOf("Things", gun);
        return soundTestPlay(db, shot).at(0).cue;
    };
    CHECK(shotOf("Gun_Autopistol") == "Shot_Pistol");
    CHECK(shotOf("Gun_MachinePistol") == "Shot_MachinePistol");
    CHECK(shotOf("Gun_HeavySMG") == "Shot_HeavySMG");
    CHECK(shotOf("Gun_AssaultRifle") == "Shot_AssaultRifle");
    CHECK(shotOf("Gun_LMG") == "Shot_LMG");
    CHECK(shotOf("Gun_Minigun") == "Shot_Minigun");
    CHECK(shotOf("Gun_SniperRifle") == "Shot_SniperRifle");
    CHECK(shotOf("Gun_ChainShotgun") == "Shot_ChainShotgun");
    CHECK(shotOf("Gun_ChargeRifle") == "Shot_Charge");
    CHECK(shotOf("Gun_ShortBow") == "Shot_Bow");
    CHECK(shotOf("Gun_Greatbow") == "Shot_Greatbow");
    CHECK(shotOf("Gun_Pila") == "Throw_Pila");
    CHECK(shotOf("Gun_TripleRocketLauncher") == "Shot_Rocket");
    CHECK(shotOf("Gun_GrenadeLauncher") == "Shot_Launcher");
    CHECK(shotOf("Gun_AutocannonTurret") == "Shot_Autocannon");
    CHECK(shotOf("Gun_Mortar") == "Shot_Mortar");
    // No two hand guns of different kinds share a cue.
    std::vector<std::string> seen;
    for (const char* g : {"Gun_Revolver", "Gun_Autopistol", "Gun_MachinePistol", "Gun_HeavySMG", "Gun_BoltActionRifle", "Gun_PumpShotgun",
                          "Gun_AssaultRifle", "Gun_LMG", "Gun_Minigun", "Gun_SniperRifle", "Gun_ChainShotgun", "Gun_ChargeRifle",
                          "Gun_ShortBow", "Gun_Greatbow", "Gun_Pila"}) {
        const std::string cue = shotOf(g);
        CHECK_MESSAGE(std::find(seen.begin(), seen.end(), cue) == seen.end(), g << " shares " << cue);
        seen.push_back(cue);
    }

    // A firearm's spent case tinkles half a second later; a bow throws none.
    Event shot;
    shot.type = EventType::ShotFired;
    shot.thing = static_cast<Entity>(3);
    shot.def = db.idOf("Things", "Gun_AssaultRifle");
    auto plays = soundTestPlay(db, shot);
    if (plays.size() != 2) { CHECK(plays.size() == 2); return; }
    CHECK(plays[1].cue == "Gun_Shell");
    CHECK(plays[1].delay > 0.2f);
    shot.def = db.idOf("Things", "Gun_RecurveBow");
    CHECK(soundTestPlay(db, shot).size() == 1);
}

TEST_CASE("sound: bullets and arrows sound by what they strike") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    Event hit;
    hit.type = EventType::ProjectileHit;
    hit.def = db.idOf("Projectiles", "Bullet_AssaultRifle");
    hit.hit = ProjectileHitKind::Ground;
    CHECK(soundImpactCue(db, hit) == "Impact_Dirt");
    const SoundStepAt water = [](CellPos) { return std::string("Step_Water"); };
    const SoundStepAt sand = [](CellPos) { return std::string("Step_Sand"); };
    const SoundStepAt floor = [](CellPos) { return std::string("Step_Wood"); };
    CHECK(soundImpactCue(db, hit, water) == "Impact_Water");
    CHECK(soundImpactCue(db, hit, sand) == "Impact_Sand");
    CHECK(soundImpactCue(db, hit, floor) == "Impact_Wood");
    // A wall by its stuff: wood, steel, stone; a wall that says nothing is stone.
    hit.hit = ProjectileHitKind::Building;
    hit.struckDef = db.idOf("Things", "Wall");
    hit.struckStuff = db.idOf("Things", "WoodLog");
    CHECK(soundImpactCue(db, hit) == "Impact_Wood");
    hit.struckStuff = db.idOf("Things", "Steel");
    CHECK(soundImpactCue(db, hit) == "Impact_Steel");
    hit.struckStuff = db.idOf("Things", "BlocksGranite");
    CHECK(soundImpactCue(db, hit) == "Impact_Stone");
    hit.struckStuff = DefId{};
    hit.struckDef = DefId{};
    CHECK(soundImpactCue(db, hit) == "Impact_Stone");
    // A tree trunk is wood.
    hit.hit = ProjectileHitKind::Plant;
    CHECK(soundImpactCue(db, hit) == "Impact_Wood");
    hit.hit = ProjectileHitKind::Pawn;
    CHECK(soundImpactCue(db, hit) == "Flesh_Bullet");

    // Arrows thunk into flesh and wood; steel rings like stone.
    hit.def = db.idOf("Projectiles", "Arrow_Recurve");
    CHECK(soundImpactCue(db, hit) == "Arrow_Flesh");
    hit.hit = ProjectileHitKind::Ground;
    CHECK(soundImpactCue(db, hit) == "Arrow_Dirt");
    hit.hit = ProjectileHitKind::Plant;
    CHECK(soundImpactCue(db, hit) == "Arrow_Wood");
    hit.hit = ProjectileHitKind::Ground;
    hit.hit = ProjectileHitKind::Armor;
    CHECK(soundImpactCue(db, hit) == "Arrow_Stone");
    hit.hit = ProjectileHitKind::Building;
    hit.struckStuff = db.idOf("Things", "WoodLog");
    CHECK(soundImpactCue(db, hit) == "Arrow_Wood");

    // A bullet off rock whines away a quarter of the time, decided by the hit itself.
    hit.def = db.idOf("Projectiles", "Bullet_AssaultRifle");
    hit.hit = ProjectileHitKind::Rock;
    int ricochets = 0;
    for (int i = 0; i < 400; ++i) {
        hit.x = static_cast<float>(i) * 0.37f;
        hit.y = static_cast<float>(i % 17);
        const auto plays = soundTestPlay(db, hit);
        ricochets += (plays.size() == 2 && plays[1].cue == "Ricochet") ? 1 : 0;
    }
    CHECK(ricochets > 60);
    CHECK(ricochets < 140);
    // Never off flesh or an arrow.
    hit.hit = ProjectileHitKind::Pawn;
    CHECK(soundTestPlay(db, hit).size() == 1);
}

TEST_CASE("sound: blasts by what burst, with debris after a big one") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    Event boom;
    boom.type = EventType::Explosion;
    boom.def = db.idOf("DamageTypes", "Bomb");
    boom.amount = 1.9f;
    auto plays = soundTestPlay(db, boom);
    CHECK(plays.at(0).cue == "Explosion_Blast");
    CHECK(plays.size() == 1);  // a small one throws no debris
    boom.struckDef = db.idOf("Projectiles", "Bullet_Shell_HighExplosive");
    boom.amount = 2.9f;
    plays = soundTestPlay(db, boom);
    CHECK(plays.at(0).cue == "Explosion_Mortar");
    if (plays.size() != 2) { CHECK(plays.size() == 2); return; }
    CHECK(plays[1].cue == "Explosion_Debris");
    CHECK(plays[1].delay > 0.0f);
    boom.struckDef = db.idOf("Projectiles", "Rocket_Doomsday");
    boom.amount = 7.8f;
    CHECK(soundTestPlay(db, boom).at(0).cue == "Explosion_Large");
    boom.struckDef = DefId{};
    CHECK(soundTestPlay(db, boom).at(0).cue == "Explosion_Large");  // big by its size alone
    boom.struckDef = db.idOf("Projectiles", "Shell_EMP");
    boom.amount = 3.5f;
    plays = soundTestPlay(db, boom);
    CHECK(plays.at(0).cue == "Explosion_EMP");
    CHECK(plays.size() == 1);
    boom.struckDef = db.idOf("Projectiles", "Shell_Smoke");
    CHECK(soundTestPlay(db, boom).at(0).cue == "Explosion_Smoke");
}

TEST_CASE("sound: gore, a felled tree and the work by the job") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const Entity pawn = static_cast<Entity>(7);
    const DefId human = db.idOf("Things", "Human");
    const SoundDefOf defOf = [&](Entity e) { return e == pawn ? human : DefId{}; };
    Event lost;
    lost.type = EventType::PawnPartLost;
    lost.thing = pawn;
    lost.message = "Leg";
    auto plays = soundTestPlay(db, lost, defOf);
    const auto has = [&](const char* cue) {
        return std::any_of(plays.begin(), plays.end(), [&](const SoundPlay& p) { return p.cue == cue; });
    };
    CHECK(has("Gore_Sever"));
    CHECK(has("Gore_Splat"));
    lost.message = "Head";
    plays = soundTestPlay(db, lost, defOf);
    CHECK(has("Gore_Decap"));

    Event felled;
    felled.type = EventType::PlantRemoved;
    felled.message = "felled";
    felled.cell = {2, 3, 0};
    CHECK(soundTestPlay(db, felled).at(0).cue == "Tree_Fall");
    felled.message = "cut";
    CHECK(soundTestPlay(db, felled).empty());

    Event act;
    act.type = EventType::PawnAction;
    act.thing = pawn;
    act.action = PawnAction::Cut;
    act.cell = {4, 4, 0};
    act.struckDef = db.idOf("Things", "Plant_TreeOak");
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Chop");
    act.struckDef = db.idOf("Things", "Plant_Grass");
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_PlantCut");
    act.action = PawnAction::Harvest;
    act.message = "sow";
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Sow");
    act.message.clear();
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Harvest");
    act.action = PawnAction::Deconstruct;
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Deconstruct");
    act.action = PawnAction::Repair;
    CHECK(soundTestPlay(db, act).at(0).cue == "Work_Repair");
    act.action = PawnAction::LieDown;
    CHECK(soundTestPlay(db, act).empty());  // on the ground: no creak
    act.target = static_cast<Entity>(11);
    CHECK(soundTestPlay(db, act).at(0).cue == "Bed_Creak");
}

TEST_CASE("sound: a man grunts and screams too") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const Entity pawn = static_cast<Entity>(7);
    const DefId human = db.idOf("Things", "Human");
    const SoundDefOf defOf = [&](Entity e) { return e == pawn ? human : DefId{}; };
    const SoundGenderOf man = [](Entity) { return SoundGender::Male; };
    Event down;
    down.type = EventType::PawnDowned;
    down.thing = pawn;
    std::vector<SoundPlay> out;
    soundForEvent(db, down, defOf, out, man);
    CHECK(std::any_of(out.begin(), out.end(), [](const SoundPlay& p) { return p.cue == "Voice_MaleScream" && p.voice == SoundVoice::Scream; }));
}

TEST_CASE("sound: what a step says about the ground") {
    CHECK(soundSurfaceOfStep("Step_Grass") == "Dirt");
    CHECK(soundSurfaceOfStep("Step_Mud") == "Dirt");
    CHECK(soundSurfaceOfStep("Step_Sand") == "Sand");
    CHECK(soundSurfaceOfStep("Step_Water") == "Water");
    CHECK(soundSurfaceOfStep("Step_Tile") == "Stone");
    CHECK(soundSurfaceOfStep("Step_Wood") == "Wood");
}

TEST_CASE("sound: a corpse makes no vocals when shot or hit, but the blow itself still sounds (sneed 2026-11-22)") {
    DefDatabase db;
    if (!soundTestLoad(db)) { CHECK(false); return; }
    const Entity woman = static_cast<Entity>(11);
    const DefId human = db.idOf("Things", "Human");
    const SoundDefOf defOf = [&](Entity e) { return e == woman ? human : DefId{}; };
    const SoundGenderOf genderOf = [&](Entity) { return SoundGender::Female; };
    bool alive = true;
    const SoundAliveOf aliveOf = [&](Entity) { return alive; };
    Event mh;
    mh.type = EventType::MeleeHit;
    mh.def = db.idOf("Things", "Melee_Club");
    mh.message = "hit";
    mh.hit = ProjectileHitKind::Pawn;
    mh.target = woman;
    mh.amount = 8.0f;
    int livingGrunts = 0;
    int deadGrunts = 0;
    int deadOther = 0;
    for (int i = 0; i < 100; ++i) {
        mh.x = static_cast<float>(i) * 0.37f;
        for (const bool a : {true, false}) {
            alive = a;
            std::vector<SoundPlay> out;
            soundForEvent(db, mh, defOf, out, genderOf, {}, aliveOf);
            for (const SoundPlay& p : out) {
                if (p.voice != SoundVoice::None) {
                    (a ? livingGrunts : deadGrunts) += 1;
                } else if (!a) {
                    ++deadOther;
                }
            }
        }
    }
    CHECK(livingGrunts > 20);
    CHECK(deadGrunts == 0);
    CHECK(deadOther >= 100);  // the club on flesh still sounds
    // A part torn off a corpse: the gore sounds, no hurt cry.
    Event lost;
    lost.type = EventType::PawnPartLost;
    lost.thing = woman;
    lost.message = "Arm";
    alive = false;
    std::vector<SoundPlay> out;
    soundForEvent(db, lost, defOf, out, genderOf, {}, aliveOf);
    for (const SoundPlay& p : out) {
        CHECK(p.voice == SoundVoice::None);
        CHECK(p.cue.rfind("Voice_", 0) != 0);
    }
}

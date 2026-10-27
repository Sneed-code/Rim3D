#include "ColonySim/Defs.h"
#include "ColonySim/Sound.h"

#include <doctest.h>

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
                const std::string cue = soundSlot(def, slot.c_str());
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
    CHECK(soundTestPlay(db, hit).at(0).cue == "Impact_Armor");
    hit.hit = ProjectileHitKind::Rock;
    CHECK(soundTestPlay(db, hit).at(0).cue == "Impact_Rock");
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
    CHECK(soundTestPlay(db, mh).at(0).cue == "Flesh_Blunt");
    mh.message = "deflected";
    mh.hit = ProjectileHitKind::Armor;
    CHECK(soundTestPlay(db, mh).at(0).cue == "Melee_Clang");
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

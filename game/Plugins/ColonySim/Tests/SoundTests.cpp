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
    // The club's own hit sound still plays with it.
    CHECK(soundTestPlay(db, mh, defOf).at(0).cue == "Flesh_Blunt");

    // 3 damage or less, a blocked or deflected blow, a man (no male voice yet), an animal, or no gender callback: no grunt.
    bool any = false;
    for (int i = 0; i < 100; ++i) {
        mh.x = static_cast<float>(i);
        mh.amount = 3.0f;
        any = any || !voices(mh).empty();
        mh.amount = 8.0f;
        mh.target = man;
        any = any || !voices(mh).empty();
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
    down.thing = man;
    CHECK(voices(down).empty());

    // Killed by violence screams; bleeding out, cold or illness do not.
    Event died;
    died.type = EventType::PawnDied;
    died.thing = woman;
    died.message = "a vital part destroyed";
    CHECK(voices(died).size() == 1);
    died.message = "executed";
    CHECK(voices(died).size() == 1);
    died.message = "blood loss";
    CHECK(voices(died).empty());
    died.message = "hypothermia";
    CHECK(voices(died).empty());

    // Killed while she lay downed: awake, a last scream about half the time (by the event alone); knocked out, none.
    died.message = "a vital part destroyed";
    int last = 0;
    bool knockedOut = false;
    for (int i = 0; i < 400; ++i) {
        died.thing = static_cast<Entity>(100 + i);
        died.cell = {i % 23, i / 23, 0};
        const Entity who = died.thing;
        const SoundGenderOf allFemale = [](Entity) { return SoundGender::Female; };
        const SoundDefOf allHuman = [&](Entity) { return human; };
        const auto count = [&](float amount) {
            died.amount = amount;
            std::vector<SoundPlay> out;
            soundForEvent(db, died, allHuman, out, allFemale);
            int n = 0;
            for (const SoundPlay& p : out) n += p.voice == SoundVoice::Scream && p.entity == who ? 1 : 0;
            return n;
        };
        CHECK(count(0.0f) == 1);  // was up: killed outright, always
        last += count(1.0f);
        knockedOut = knockedOut || count(2.0f) != 0;
    }
    CHECK(last > 160);
    CHECK(last < 240);
    CHECK(!knockedOut);
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

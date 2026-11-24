#include "ColonySim/Sound.h"

#include "ColonySim/Map.h"
#include "ColonySim/Random.h"

#include <cstring>
#include <initializer_list>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

SoundPlay soundAt(const char* cue, float x, float y, float z) {
    SoundPlay p;
    p.cue = cue;
    p.hasPos = true;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

SoundPlay soundAtCell(const std::string& cue, CellPos cell) {
    SoundPlay p;
    p.cue = cue;
    p.hasPos = true;
    p.x = static_cast<float>(cell.x) + 0.5f;
    p.y = static_cast<float>(cell.y) + 0.5f;
    p.z = static_cast<float>(cell.level) * Map::kLevelHeightM;
    p.cell = cell;
    return p;
}

SoundPlay soundOn(const std::string& cue, Entity entity) {
    SoundPlay p;
    p.cue = cue;
    p.entity = entity;
    return p;
}

const Def* soundDefOfEntity(const DefDatabase& db, const SoundDefOf& defOf, Entity e) {
    if (!defOf || e == kNoEntity) return nullptr;
    const DefId id = defOf(e);
    return id.valid() ? &db.get(id) : nullptr;
}

const Def* soundDef(const DefDatabase& db, DefId id) { return id.valid() ? &db.get(id) : nullptr; }

std::string soundSlotOr(const Def* def, const char* slot, const char* fallback) {
    std::string cue = def != nullptr ? soundSlot(*def, slot) : std::string();
    return cue.empty() ? std::string(fallback) : cue;
}

// What a hit on flesh sounds like: the damage type's `hit`.
std::string soundFleshCue(const DefDatabase& db, const Def* damageType) {
    (void)db;
    return soundSlotOr(damageType, "hit", "Flesh_Bullet");
}

const Def* soundDamageTypeOfProjectile(const DefDatabase& db, DefId projectile) {
    const Def* p = soundDef(db, projectile);
    if (p == nullptr) return nullptr;
    return db.find("DamageTypes", p->text("projectile.damageDef"));
}

// The work sound of a PawnAction that has one; null for the rest.
const char* soundWorkCue(PawnAction action) {
    switch (action) {
        case PawnAction::Mine: return "Work_Mine";
        case PawnAction::Cut: return "Work_Chop";
        case PawnAction::Harvest: return "Work_Harvest";
        case PawnAction::Build: return "Work_Build";
        case PawnAction::Deconstruct: return "Work_Deconstruct";
        case PawnAction::Repair: return "Work_Repair";
        case PawnAction::Rearm: return "Trap_Rearm";
        case PawnAction::Operate:
        case PawnAction::Patch: return "Work_Tend";
        case PawnAction::Drill:
        case PawnAction::Scan: return "Work_Drill";
        case PawnAction::DropCorpse: return "Pawn_Death";
        case PawnAction::LieDown: return "Bed_Creak";
        case PawnAction::Cook: return "Work_Cook";
        case PawnAction::Research: return "Work_Research";
        case PawnAction::Clean: return "Work_Clean";
        case PawnAction::Tend: return "Work_Tend";
        case PawnAction::Butcher: return "Work_Butcher";
        case PawnAction::Refuel: return "Work_Refuel";
        case PawnAction::Handle:
        case PawnAction::Slaughter: return "Work_Handle";
        case PawnAction::Chat:
        case PawnAction::FeedPawn: return "Work_Chat";
        case PawnAction::Extinguish: return "Work_Extinguish";
        case PawnAction::Eat: return "Work_Eat";
        case PawnAction::PickUp: return "Item_PickUp";
        case PawnAction::PutDown: return "Item_PutDown";
        case PawnAction::Dodge:
        case PawnAction::Roll: return "Dodge";
        default: return nullptr;
    }
}

void soundPawnAction(const DefDatabase& db, const Event& e, const SoundDefOf& defOf, std::vector<SoundPlay>& out) {
    if (e.action == PawnAction::Reload || e.action == PawnAction::Switch) {
        const char* slot = e.action == PawnAction::Reload ? "reload" : "switch";
        const char* fallback = e.action == PawnAction::Reload ? "Gun_Reload" : "Gun_Switch";
        out.push_back(soundOn(soundSlotOr(soundDefOfEntity(db, defOf, e.target), slot, fallback), e.thing));
        return;
    }
    if (e.action == PawnAction::Alert) {
        // Stealth (docs/design/stealth.md): a sting when a fighter grows suspicious and a sharper one when it is
        // alerted by what it saw, heard or was called to. Calming down, an order and a whole site raised are silent.
        const std::string& why = e.message;
        if (why == "calm" || why == "lost them" || why == "ordered" || why == "site raised") return;
        if (e.amount >= 1.5f) out.push_back(soundOn("Alert_Spotted", e.thing));
        else if (e.amount >= 0.5f) out.push_back(soundOn("Alert_Suspicious", e.thing));
        return;
    }
    if (e.action == PawnAction::Patch && e.message != "patching") return;  // the bandage sounds once, as it starts
    if (e.action == PawnAction::LieDown && e.target == kNoEntity) return;  // the ground does not creak
    const char* cue = soundWorkCue(e.action);
    if (cue == nullptr) return;
    std::string name = cue;
    // Plant work by the plant (`struckDef`): a tree is chopped with an axe, the rest cut; its `visual.sound.cut` says
    // otherwise. Sowing (`message` "sow") digs the soil.
    if (e.action == PawnAction::Cut) {
        const Def* plant = soundDef(db, e.struckDef);
        const char* fallback = plant != nullptr && !plant->flag("plant.isTree") ? "Work_PlantCut" : "Work_Chop";
        name = soundSlotOr(plant, "cut", fallback);
    } else if (e.action == PawnAction::Harvest && e.message == "sow") {
        name = "Work_Sow";
    }
    if (db.find("SoundCues", name) == nullptr) name = cue;
    // A bench names its own work sound ("work"): a smithy rings, a stove sizzles.
    if (e.action == PawnAction::Cook || e.action == PawnAction::Research || e.action == PawnAction::Butcher) {
        name = soundSlotOr(soundDefOfEntity(db, defOf, e.target), "work", cue);
    }
    if (e.action == PawnAction::Mine || e.action == PawnAction::Cut || e.action == PawnAction::Harvest ||
        e.action == PawnAction::Extinguish || e.action == PawnAction::DropCorpse) {
        // At the cell worked, naming the worker too: Unreal repeats a cue with a `repeat` while that pawn keeps at it.
        SoundPlay p = soundAtCell(name, e.cell);
        p.entity = e.thing;
        out.push_back(p);
    } else {
        out.push_back(soundOn(name, e.thing));
    }
}

}  // namespace

std::string soundMaterialOf(const DefDatabase& db, DefId def) {
    const Def* d = soundDef(db, def);
    if (d == nullptr) return {};
    const std::string said = d->text("visual.sound.material");
    if (!said.empty()) return said;
    for (size_t i = 0; i < d->count("stuffProps.categories"); ++i) {
        const std::string c = d->textAt("stuffProps.categories", i);
        if (c == "Metallic" || c == "Composite") return "Steel";
        if (c == "Woody") return "Wood";
        if (c == "Stony") return "Stone";
        if (c == "Fabric" || c == "Leathery") return "Flesh";  // cloth and leather give like a body
    }
    const std::string surface = d->text("visual.surface");
    if (surface == "wood") return "Wood";
    if (surface == "steel" || surface == "plasteel" || surface == "uranium") return "Steel";
    if (!surface.empty()) return "Stone";
    return {};
}

namespace {
std::string soundFirstCue(const DefDatabase& db, std::initializer_list<std::string> cues);
}  // namespace

std::string soundMeleeCue(const DefDatabase& db, const Event& e) {
    const std::string& m = e.message;
    // A blow into a wall or rock comes as "missed" (no pawn was struck) with `hit` Building or Rock.
    if (m == "missed" && e.hit != ProjectileHitKind::Building && e.hit != ProjectileHitKind::Rock &&
        e.hit != ProjectileHitKind::Plant && e.hit != ProjectileHitKind::Ground)
        return {};
    if (m == "dodged") return "Dodge";
    // The weapon's material (its stuff, else its def), wood or steel: a stone club rings hard like steel, a cloth one is wood.
    std::string weapon = soundMaterialOf(db, e.stuff);
    if (weapon.empty()) weapon = soundMaterialOf(db, e.def);
    if (weapon == "Stone") weapon = "Steel";
    if (weapon == "Flesh") weapon = "Wood";
    const bool flesh = m == "hit" && e.hit == ProjectileHitKind::Pawn;
    const Def* weaponDef = soundDef(db, e.def);
    const std::string slot = weaponDef != nullptr ? soundSlot(*weaponDef, "hit") : std::string();
    if (flesh) {
        // A blade sounds like itself in flesh (its `hit`: Flesh_Cut, Flesh_Stab); a blunt blow by what it is made of.
        if (!slot.empty() && slot != "Flesh_Blunt") return slot;
        if (!weapon.empty() && !slot.empty()) return "Hit_" + weapon + "_Flesh";
        if (!slot.empty()) return slot;
        return weaponDef != nullptr && weaponDef->text("race.intelligence") == "Humanlike" ? "Punch" : "Flesh_Bite";
    }
    // Struck something else: the parrying weapon, a shield, armour, a wall, or rock.
    std::string surface = soundMaterialOf(db, e.struckStuff);
    if (surface.empty()) surface = soundMaterialOf(db, e.struckDef);
    if (surface.empty()) {
        if (e.hit == ProjectileHitKind::Rock) surface = "Stone";
        else if (e.hit == ProjectileHitKind::Plant) surface = "Wood";  // a tree trunk
        else if (e.hit == ProjectileHitKind::Ground) surface = "Dirt";  // the earth under a body
        else if (e.hit == ProjectileHitKind::Building) surface = "Stone";
        else if (m == "deflected" || e.hit == ProjectileHitKind::Armor) surface = "Steel";  // plate turns a blow
        else if (m == "blocked") surface = "Steel";
    }
    if (weapon.empty() || slot.empty()) {
        // Fists, teeth and claws.
        return surface == "Steel" || surface == "Stone" ? "Melee_Clang" : "Punch";
    }
    if (surface.empty()) return {};
    // A weapon that sounds the same on everything (the plasma sword's `hitAll`).
    if (const std::string all = soundSlot(*weaponDef, "hitAll"); !all.empty()) return all;
    // sneed 2026-11-23: an edge sounds sharp on every surface, a club or hammer blunt, whatever it is made of
    // (Hit_Sharp_<surface>, Hit_Blunt_<surface>); the older weapon-material pairs are the fallback.
    const bool sharp = slot == "Flesh_Cut" || slot == "Flesh_Stab";
    return soundFirstCue(db, {std::string(sharp ? "Hit_Sharp_" : "Hit_Blunt_") + surface, "Hit_" + weapon + "_" + surface});
}

std::string soundSurfaceOfStep(const std::string& step) {
    if (step == "Step_Sand") return "Sand";
    if (step == "Step_Water") return "Water";
    if (step == "Step_Stone" || step == "Step_Tile") return "Stone";
    if (step == "Step_Wood") return "Wood";
    if (step == "Step_Metal") return "Steel";
    return "Dirt";  // grass, soil, carpet, leaves, mud, snow
}

namespace {

// The first of the cues the defs have; empty if none.
std::string soundFirstCue(const DefDatabase& db, std::initializer_list<std::string> cues) {
    for (const std::string& c : cues) {
        if (!c.empty() && db.find("SoundCues", c) != nullptr) return c;
    }
    return {};
}

}  // namespace

std::string soundImpactCue(const DefDatabase& db, const Event& e, const SoundStepAt& stepAt) {
    const Def* projectile = soundDef(db, e.def);
    std::string set = projectile != nullptr ? soundSlot(*projectile, "impactSet") : std::string();
    if (set.empty()) set = "Impact";
    std::string surface;
    std::string old;  // the general cue from before surfaces, the last fallback
    switch (e.hit) {
        case ProjectileHitKind::Pawn: {
            const std::string flesh = soundFleshCue(db, soundDamageTypeOfProjectile(db, e.def));
            return set == "Impact" ? flesh : soundFirstCue(db, {set + "_Flesh", flesh});
        }
        case ProjectileHitKind::Armor: surface = "Steel"; old = "Impact_Armor"; break;
        case ProjectileHitKind::Rock: surface = "Stone"; old = "Impact_Rock"; break;
        case ProjectileHitKind::Building: {
            std::string m = soundMaterialOf(db, e.struckStuff);
            if (m.empty()) m = soundMaterialOf(db, e.struckDef);
            surface = m == "Wood" ? "Wood" : m == "Steel" ? "Steel" : m == "Flesh" ? "Dirt" : "Stone";  // sandbags thud like earth
            old = "Impact_Building";
            break;
        }
        case ProjectileHitKind::Plant: surface = "Wood"; old = "Impact_Building"; break;  // a tree trunk
        case ProjectileHitKind::Ground:
            surface = soundSurfaceOfStep(stepAt ? stepAt(e.cell) : std::string("Step_Dirt"));
            old = "Impact_Ground";
            break;
        default: return {};
    }
    // An arrow has fewer recordings: steel rings like stone, sand and water like... sand and water, else the ground.
    if (set == "Arrow") {
        const std::string arrow = surface == "Steel" ? "Stone" : surface == "Sand" ? "Dirt" : surface;
        return soundFirstCue(db, {set + "_" + surface, set + "_" + arrow, "Impact_" + surface, old});
    }
    return soundFirstCue(db, {set + "_" + surface, "Impact_" + surface, old});
}

namespace {

// A hash of the event's place and amount, 0 to 1, for sound choices that must not touch the game's random numbers.
double soundEventRoll(const Event& e, uint64_t salt);

void soundMeleeHit(const DefDatabase& db, const Event& e, std::vector<SoundPlay>& out) {
    std::string cue = soundMeleeCue(db, e);
    if (cue.empty()) return;
    if (db.find("SoundCues", cue) == nullptr) {
        // A pairing with no cue of its own falls back to the old general sounds.
        const std::string& m = e.message;
        cue = m == "blocked" ? "Melee_Block" : (m == "deflected" || e.hit == ProjectileHitKind::Armor) ? "Melee_Clang"
              : e.hit == ProjectileHitKind::Building || e.hit == ProjectileHitKind::Plant ? "Impact_Building" : "Flesh_Blunt";
    }
    out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
}

// A weapon's own sound on top of every blow that lands on something (its `hitExtra` slot: the zeushammer's electric
// burst, sneed 2026-11-23), `hitExtraMech` instead on a mechanoid (a far bigger discharge).
void soundMeleeExtra(const DefDatabase& db, const SoundDefOf& defOf, const Event& e, std::vector<SoundPlay>& out) {
    const bool landed = e.message == "hit" || e.message == "blocked" || e.message == "deflected" ||
                        (e.message == "missed" && e.hit != ProjectileHitKind::None);
    const Def* weapon = landed ? soundDef(db, e.def) : nullptr;
    if (weapon == nullptr) return;
    std::string cue;
    if (e.hit == ProjectileHitKind::Pawn || e.hit == ProjectileHitKind::Armor) {
        const Def* target = soundDefOfEntity(db, defOf, e.target);
        if (target != nullptr && target->text("race.intelligence") == "Mechanoid") cue = soundSlot(*weapon, "hitExtraMech");
    }
    if (cue.empty()) cue = soundSlot(*weapon, "hitExtra");
    if (!cue.empty() && db.find("SoundCues", cue) != nullptr) out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
}

// The voice slot of a pawn's gender ("femaleHit"), or null when it has no voice.
const char* soundVoiceSlot(SoundGender gender, SoundVoice voice) {
    if (gender == SoundGender::Female) return voice == SoundVoice::Hit ? "femaleHit" : "femaleScream";
    if (gender == SoundGender::Male) return voice == SoundVoice::Hit ? "maleHit" : "maleScream";
    return nullptr;
}

uint64_t soundFloatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

double soundEventRoll(const Event& e, uint64_t salt) {
    const uint64_t roll = splitMix64(salt ^ static_cast<uint64_t>(e.target) ^ (soundFloatBits(e.amount) << 32) ^
                                     splitMix64(soundFloatBits(e.x) ^ (soundFloatBits(e.y) << 32)));
    return static_cast<double>(roll >> 11) * 0x1.0p-53;
}

// The pawn's voice cue for this kind of cry; empty if it has none.
std::string soundVoiceCue(const DefDatabase& db, const SoundDefOf& defOf, const SoundGenderOf& genderOf, Entity pawn,
                          SoundVoice voice) {
    if (!genderOf || pawn == kNoEntity) return {};
    const char* slot = soundVoiceSlot(genderOf(pawn), voice);
    const Def* def = soundDefOfEntity(db, defOf, pawn);
    if (slot == nullptr || def == nullptr) return {};
    return soundSlot(*def, slot);
}

SoundPlay soundVoiceOn(const std::string& cue, Entity pawn, SoundVoice voice, float x, float y, float z, bool hasPos) {
    SoundPlay p = soundOn(cue, pawn);
    p.voice = voice;
    p.hasPos = hasPos;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

// A grunt from the one struck by a hard hit (melee or shot), some of the time.
void soundHitVoice(const DefDatabase& db, const Event& e, const SoundDefOf& defOf, const SoundGenderOf& genderOf,
                   std::vector<SoundPlay>& out) {
    // A melee blow that landed ("hit"), or a bullet, arrow or shard that struck the pawn (ProjectileHit on a Pawn).
    if (e.hit != ProjectileHitKind::Pawn) return;
    if (e.type == EventType::MeleeHit && e.message != "hit") return;
    const std::string cue = soundVoiceCue(db, defOf, genderOf, e.target, SoundVoice::Hit);
    const Def* cueDef = cue.empty() ? nullptr : db.find("SoundCues", cue);
    if (cueDef == nullptr || e.amount <= static_cast<float>(cueDef->number("minDamage", 3.0))) return;
    const uint64_t roll = splitMix64(static_cast<uint64_t>(e.target) ^ (soundFloatBits(e.amount) << 32) ^
                                     splitMix64(soundFloatBits(e.x) ^ (soundFloatBits(e.y) << 32)));
    const double chance = cueDef->number("chance", 0.5);
    if (static_cast<double>(roll >> 11) * 0x1.0p-53 >= chance) return;
    out.push_back(soundVoiceOn(cue, e.target, SoundVoice::Hit, e.x, e.y, e.z, true));
}

}  // namespace

std::string soundDownedVoice(const Def& pawnDef, SoundGender gender) {
    if (gender == SoundGender::Female) return soundSlot(pawnDef, "femaleDowned");
    if (gender == SoundGender::Male) return soundSlot(pawnDef, "maleDowned");
    return {};
}

std::string soundLovinCue(SoundGender a, SoundGender b) {
    const int women = (a == SoundGender::Female ? 1 : 0) + (b == SoundGender::Female ? 1 : 0);
    const int men = (a == SoundGender::Male ? 1 : 0) + (b == SoundGender::Male ? 1 : 0);
    if (women == 1 && men == 1) return "Voice_LovinCouple";
    if (women >= 1 && men == 0) return "Voice_LovinFem";
    return {};
}

bool soundViolentDeath(const std::string& cause) {
    return cause == "a vital part destroyed" || cause == "organ failure" || cause == "executed" || cause == "blood loss" ||
           cause == "wounds" || cause == "slaughtered";
}

std::string soundSlot(const Def& def, const char* slot) {
    return def.text((std::string("visual.sound.") + slot).c_str());
}

std::string soundStepCue(const DefDatabase& db, DefId terrain) {
    return soundSlotOr(soundDef(db, terrain), "step", "Step_Dirt");
}

void soundForEvent(const DefDatabase& db, const Event& e, const SoundDefOf& defOf, std::vector<SoundPlay>& out,
                   const SoundGenderOf& genderOf, const SoundStepAt& stepAt, const SoundAliveOf& aliveOf) {
    const auto voiced = [&](Entity pawn) { return !aliveOf || pawn == kNoEntity || aliveOf(pawn); };
    switch (e.type) {
        case EventType::ShotFired: {
            const Def* gun = soundDef(db, e.def);
            const std::string cue = soundSlotOr(gun, "shot", "Shot_Pistol");
            out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
            // A firearm throws its spent case, which tinkles on the ground half a second later (the gun's `shell`).
            const std::string shell = gun != nullptr ? soundSlot(*gun, "shell") : std::string();
            if (!shell.empty() && e.thing != kNoEntity) {
                SoundPlay p = soundOn(shell, e.thing);
                p.delay = 0.45f;
                out.push_back(p);
            }
            // A pump gun is racked after each shot (its `pump`, sneed 2026-11-24: "pump shotguns don't have the shotgun sounds").
            const std::string pump = gun != nullptr ? soundSlot(*gun, "pump") : std::string();
            if (!pump.empty() && e.thing != kNoEntity && db.find("SoundCues", pump) != nullptr) {
                SoundPlay p = soundOn(pump, e.thing);
                p.delay = 0.35f;
                out.push_back(p);
            }
            break;
        }
        case EventType::ProjectileHit: {
            const std::string cue = soundImpactCue(db, e, stepAt);
            if (!cue.empty()) out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
            if (e.hit == ProjectileHitKind::Pawn) {
                if (voiced(e.target)) soundHitVoice(db, e, defOf, genderOf, out);
            } else if (e.hit == ProjectileHitKind::Rock || e.hit == ProjectileHitKind::Armor ||
                       (e.hit == ProjectileHitKind::Building && cue.find("_Wood") == std::string::npos)) {
                // A bullet off stone or steel whines away now and then (a quarter of the time, by a hash of the hit).
                const Def* projectile = soundDef(db, e.def);
                const bool bullet = projectile == nullptr || soundSlot(*projectile, "impactSet").empty();
                if (bullet && db.find("SoundCues", "Ricochet") != nullptr && soundEventRoll(e, 0x52494343ull) < 0.25) {
                    SoundPlay p = soundAt("Ricochet", e.x, e.y, e.z);
                    p.delay = 0.05f;
                    out.push_back(p);
                }
            }
            break;
        }
        case EventType::MeleeSwing: {
            const Def* weapon = soundDef(db, e.def);
            const std::string cue = weapon != nullptr ? soundSlot(*weapon, "swing") : std::string();
            if (!cue.empty()) out.push_back(soundOn(cue, e.thing));
            break;
        }
        case EventType::MeleeHit:
            soundMeleeHit(db, e, out);
            soundMeleeExtra(db, defOf, e, out);
            if (voiced(e.target)) soundHitVoice(db, e, defOf, genderOf, out);
            break;
        // A hit softened by a raised shield (docs/design/shields.md); one it stopped already sounds as a blocked blow or an
        // armour impact.
        case EventType::ShieldBlocked:
            if (e.message == "softened") out.push_back(soundOn("Melee_Block", e.thing));
            break;
        case EventType::Explosion: {
            // The grenade, shell or rocket's own `explosion` (EMP, smoke, a mortar shell); else a big blast (radius 4.5 m
            // and more: the doomsday rocket) `Explosion_Large`; else the damage type's (a frag's crack, fire's whoosh).
            const Def* damage = soundDef(db, e.def);
            std::string cue = soundSlotOr(soundDef(db, e.struckDef), "explosion", "");
            const std::string byType = soundSlotOr(damage, "explosion", "Explosion_Blast");
            if (cue.empty() && byType == "Explosion_Blast" && e.amount >= 4.5f) cue = soundFirstCue(db, {"Explosion_Large"});
            if (cue.empty()) cue = byType;
            out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
            // Earth and stones rain down after a real blast (2.5 m and more), not after fire, smoke or EMP.
            const bool blast = cue == "Explosion_Blast" || cue == "Explosion_Large" || cue == "Explosion_Mortar";
            if (blast && e.amount >= 2.5f && db.find("SoundCues", "Explosion_Debris") != nullptr) {
                SoundPlay p = soundAt("Explosion_Debris", e.x, e.y, e.z);
                p.delay = 0.6f;
                out.push_back(p);
            }
            break;
        }
        case EventType::GrenadeThrown: out.push_back(soundAt("Grenade_Throw", e.x, e.y, e.z)); break;
        case EventType::GrenadeLanded: {
            // The Projectiles def's `landed` (a molotov's bottle shatters), else a grenade's clunk.
            const std::string cue = soundSlotOr(soundDef(db, e.def), "landed", "Grenade_Landed");
            out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
            break;
        }
        case EventType::TrapSprung: out.push_back(soundAt("Trap_Spring", e.x, e.y, e.z)); break;
        case EventType::TrapRearmed: out.push_back(soundAtCell("Trap_Rearm", e.cell)); break;
        case EventType::WindowBroken: out.push_back(soundAt("Window_Shatter", e.x, e.y, e.z)); break;
        case EventType::PawnAction: soundPawnAction(db, e, defOf, out); break;
        case EventType::WeaponSwitched: break;  // the PawnAction Switch already sounded
        case EventType::PawnInjured:
        case EventType::PawnPartLost: {
            const Def* pawn = soundDefOfEntity(db, defOf, e.thing);
            const std::string cue = soundSlotOr(pawn, "hurt", "");
            if (!cue.empty() && voiced(e.thing)) out.push_back(soundOn(cue, e.thing));
            if (e.type == EventType::PawnPartLost) {
                // Gore (sneed, 2026-11-20): a limb comes off with a wet tear and a crack of bone, a head with a heavier
                // chop; the piece slaps down on the ground a moment later. Mechanoids (a `gore` slot of "none") don't.
                const std::string gore = soundSlotOr(pawn, "gore", "");
                if (gore != "none") {
                    const bool head = e.message == "Head" || e.message == "Neck";
                    const std::string sever = soundFirstCue(db, {head ? "Gore_Decap" : "Gore_Sever", "Gore_Sever"});
                    if (!sever.empty()) out.push_back(soundOn(sever, e.thing));
                    if (db.find("SoundCues", "Gore_Splat") != nullptr) {
                        SoundPlay p = soundOn("Gore_Splat", e.thing);
                        p.delay = 0.5f;
                        out.push_back(p);
                    }
                }
            }
            break;
        }
        case EventType::PawnGibbed: {
            // The trunk bursts (sneed, 2026-11-22): its tear came with the PawnPartLost; the chunks slap down round about
            // over the next second.
            if (db.find("SoundCues", "Gore_Splat") != nullptr && soundSlotOr(soundDefOfEntity(db, defOf, e.thing), "gore", "") != "none") {
                for (const float delay : {0.35f, 0.6f, 0.9f}) {
                    SoundPlay p = soundOn("Gore_Splat", e.thing);
                    p.delay = delay;
                    out.push_back(p);
                }
            }
            break;
        }
        case EventType::PlantRemoved:
            if (e.message == "felled") {
                const std::string fall = soundSlotOr(soundDef(db, e.def), "fall", "Tree_Fall");
                if (db.find("SoundCues", fall) != nullptr) out.push_back(soundAtCell(fall, e.cell));
            }
            break;
        case EventType::PawnDowned:
        case EventType::PawnDied: {
            const char* slot = e.type == EventType::PawnDied ? "death" : "downed";
            const std::string cue = soundSlotOr(soundDefOfEntity(db, defOf, e.thing), slot, "");
            if (!cue.empty()) out.push_back(soundOn(cue, e.thing));
            // A woman screams every time she goes down, and every time a fight kills her: outright, or again while she
            // lies downed (sneed, 2026-11-17: "when downed - they will scream again if killed"). Cold, hunger or illness: no.
            const bool screams = e.type == EventType::PawnDowned || soundViolentDeath(e.message);
            if (screams) {
                const std::string scream = soundVoiceCue(db, defOf, genderOf, e.thing, SoundVoice::Scream);
                if (!scream.empty()) out.push_back(soundVoiceOn(scream, e.thing, SoundVoice::Scream, static_cast<float>(e.cell.x) + 0.5f,
                                                      static_cast<float>(e.cell.y) + 0.5f,
                                                      static_cast<float>(e.cell.level) * Map::kLevelHeightM, true));
            }
            break;
        }
        case EventType::PawnCaughtFire:
        case EventType::PawnFireOut: {
            SoundPlay p = soundOn(soundSlotOr(soundDefOfEntity(db, defOf, e.thing), "burning", "Pawn_Burning"), e.thing);
            p.mode = e.type == EventType::PawnCaughtFire ? SoundMode::LoopStart : SoundMode::LoopStop;
            out.push_back(p);
            break;
        }
        case EventType::FireStarted: {
            out.push_back(soundAtCell("Fire_Ignite", e.cell));
            SoundPlay p = soundAtCell("Fire_Burning", e.cell);
            p.mode = SoundMode::LoopStart;
            out.push_back(p);
            break;
        }
        case EventType::FireOut: {
            SoundPlay p = soundAtCell("Fire_Burning", e.cell);
            p.mode = SoundMode::LoopStop;
            out.push_back(p);
            out.push_back(soundAtCell("Fire_Out", e.cell));
            break;
        }
        case EventType::PowerChanged: out.push_back(soundOn(e.message == "on" ? "Power_On" : "Power_Off", e.thing)); break;
        case EventType::ShortCircuit: out.push_back(soundAtCell("Short_Circuit", e.cell)); break;
        case EventType::RoofCollapsed: out.push_back(soundAtCell("Roof_Collapse", e.cell)); break;
        case EventType::ResearchDone: {
            SoundPlay p;
            p.cue = "Research_Done";
            out.push_back(p);
            break;
        }
        case EventType::RaidArrived: {
            SoundPlay p;
            p.cue = "Raid_Horn";
            out.push_back(p);
            break;
        }
        case EventType::AlarmChanged: {
            SoundPlay p;
            p.cue = "Alarm";
            p.mode = e.message.rfind("on", 0) == 0 ? SoundMode::LoopStart : SoundMode::LoopStop;
            out.push_back(p);
            break;
        }
        default: break;
    }
}

}  // namespace colony

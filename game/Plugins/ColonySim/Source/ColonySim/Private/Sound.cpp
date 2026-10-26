#include "ColonySim/Sound.h"

#include "ColonySim/Map.h"

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
        case PawnAction::Build:
        case PawnAction::Deconstruct: return "Work_Build";
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
    const char* cue = soundWorkCue(e.action);
    if (cue == nullptr) return;
    std::string name = cue;
    // A bench names its own work sound ("work"): a smithy rings, a stove sizzles.
    if (e.action == PawnAction::Cook || e.action == PawnAction::Research || e.action == PawnAction::Butcher) {
        name = soundSlotOr(soundDefOfEntity(db, defOf, e.target), "work", cue);
    }
    if (e.action == PawnAction::Mine || e.action == PawnAction::Cut || e.action == PawnAction::Harvest ||
        e.action == PawnAction::Extinguish) {
        out.push_back(soundAtCell(name, e.cell));
    } else {
        out.push_back(soundOn(name, e.thing));
    }
}

void soundMeleeHit(const DefDatabase& db, const Event& e, std::vector<SoundPlay>& out) {
    const std::string& m = e.message;
    const char* special = nullptr;
    if (m == "missed") return;
    if (m == "deflected" || e.hit == ProjectileHitKind::Armor) special = "Melee_Clang";
    else if (m == "blocked") special = "Melee_Block";
    else if (m == "dodged") special = "Dodge";
    else if (e.hit == ProjectileHitKind::Building) special = "Impact_Building";
    if (special != nullptr) {
        out.push_back(soundAt(special, e.x, e.y, e.z));
        return;
    }
    // A wielded weapon says what it sounds like; fists, teeth and claws are told by the tool's name.
    std::string cue;
    if (const Def* weapon = soundDef(db, e.def)) cue = soundSlot(*weapon, "hit");
    if (cue.empty()) {
        if (m.find("fist") != std::string::npos) cue = "Punch";
        else cue = "Flesh_Bite";
    }
    out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
}

}  // namespace

std::string soundSlot(const Def& def, const char* slot) {
    return def.text((std::string("visual.sound.") + slot).c_str());
}

std::string soundStepCue(const DefDatabase& db, DefId terrain) {
    return soundSlotOr(soundDef(db, terrain), "step", "Step_Dirt");
}

void soundForEvent(const DefDatabase& db, const Event& e, const SoundDefOf& defOf, std::vector<SoundPlay>& out) {
    switch (e.type) {
        case EventType::ShotFired: {
            const std::string cue = soundSlotOr(soundDef(db, e.def), "shot", "Shot_Pistol");
            out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
            break;
        }
        case EventType::ProjectileHit: {
            switch (e.hit) {
                case ProjectileHitKind::Pawn: {
                    const std::string cue = soundFleshCue(db, soundDamageTypeOfProjectile(db, e.def));
                    out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
                    break;
                }
                case ProjectileHitKind::Armor: out.push_back(soundAt("Impact_Armor", e.x, e.y, e.z)); break;
                case ProjectileHitKind::Building: out.push_back(soundAt("Impact_Building", e.x, e.y, e.z)); break;
                case ProjectileHitKind::Ground: out.push_back(soundAt("Impact_Ground", e.x, e.y, e.z)); break;
                case ProjectileHitKind::Rock: out.push_back(soundAt("Impact_Rock", e.x, e.y, e.z)); break;
                default: break;
            }
            break;
        }
        case EventType::MeleeSwing: {
            const Def* weapon = soundDef(db, e.def);
            const std::string cue = weapon != nullptr ? soundSlot(*weapon, "swing") : std::string();
            if (!cue.empty()) out.push_back(soundOn(cue, e.thing));
            break;
        }
        case EventType::MeleeHit: soundMeleeHit(db, e, out); break;
        case EventType::Explosion: {
            const std::string cue = soundSlotOr(soundDef(db, e.def), "explosion", "Explosion_Blast");
            out.push_back(soundAt(cue.c_str(), e.x, e.y, e.z));
            break;
        }
        case EventType::GrenadeThrown: out.push_back(soundAt("Grenade_Throw", e.x, e.y, e.z)); break;
        case EventType::GrenadeLanded: out.push_back(soundAt("Grenade_Landed", e.x, e.y, e.z)); break;
        case EventType::TrapSprung: out.push_back(soundAt("Trap_Spring", e.x, e.y, e.z)); break;
        case EventType::TrapRearmed: out.push_back(soundAtCell("Trap_Rearm", e.cell)); break;
        case EventType::PawnAction: soundPawnAction(db, e, defOf, out); break;
        case EventType::WeaponSwitched: break;  // the PawnAction Switch already sounded
        case EventType::PawnInjured:
        case EventType::PawnPartLost: {
            const std::string cue = soundSlotOr(soundDefOfEntity(db, defOf, e.thing), "hurt", "");
            if (!cue.empty()) out.push_back(soundOn(cue, e.thing));
            break;
        }
        case EventType::PawnDowned:
        case EventType::PawnDied: {
            const char* slot = e.type == EventType::PawnDied ? "death" : "downed";
            const std::string cue = soundSlotOr(soundDefOfEntity(db, defOf, e.thing), slot, "");
            if (!cue.empty()) out.push_back(soundOn(cue, e.thing));
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

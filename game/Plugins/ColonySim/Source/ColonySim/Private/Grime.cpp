#include "ColonySim/Grime.h"

#include "ColonySim/Events.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"
#include "ColonySim/Water.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// A step longer than this is a jump (a load, a teleport, a carried body put down), not walking.
constexpr float kGrimeMaxStepM = 3.0f;

// Raises `level` towards `cap` at `perSecond` (a level already over the cap stays where it is).
void grimeRaise(float& level, float perSecond, float cap) {
    if (level < cap && perSecond > 0.0f) {
        level = std::min(cap, level + perSecond);
    }
}

void grimeLower(float& level, float amount) {
    if (amount > 0.0f) {
        level = std::max(0.0f, level - amount);
    }
}

float grimePer(float seconds) {
    return seconds > 0.0f ? 1.0f / seconds : 0.0f;
}

bool grimeDirtyWork(JobType type) {
    switch (type) {
        case JobType::Mine:
        case JobType::CutPlant:
        case JobType::HarvestPlant:
        case JobType::SowCrop:
        case JobType::BuildFrame:
        case JobType::Deconstruct:
        case JobType::RemoveFloor:
        case JobType::Earthworks:
        case JobType::WindowWork:
        case JobType::Butcher:
        case JobType::ExtinguishFire:
            return true;
        default:
            return false;
    }
}

}  // namespace

GrimeNumbers GrimeNumbers::read(const DefDatabase& defs) {
    GrimeNumbers n;
    const Def* def = defs.find("Rules", "GrimeRules");
    if (def == nullptr) {
        return n;
    }
    auto num = [&](const char* path, float fallback) { return static_cast<float>(def->number(path, fallback)); };
    n.soilFullSeconds = num("soilFullSeconds", n.soilFullSeconds);
    n.soilMax = std::clamp(num("soilMax", n.soilMax), 0.0f, 1.0f);
    n.mudFullSeconds = num("mudFullSeconds", n.mudFullSeconds);
    n.workFullSeconds = num("workFullSeconds", n.workFullSeconds);
    n.workMax = std::clamp(num("workMax", n.workMax), 0.0f, 1.0f);
    n.groundFullSeconds = num("groundFullSeconds", n.groundFullSeconds);
    n.bloodPerDamage = num("bloodPerDamage", n.bloodPerDamage);
    n.spatterPerDamage = num("spatterPerDamage", n.spatterPerDamage);
    n.weaponPerDamage = num("weaponPerDamage", n.weaponPerDamage);
    n.rainWashSeconds = num("rainWashSeconds", n.rainWashSeconds);
    n.ankleWashSeconds = num("ankleWashSeconds", n.ankleWashSeconds);
    n.waistWashSeconds = num("waistWashSeconds", n.waistWashSeconds);
    n.swimWashSeconds = num("swimWashSeconds", n.swimWashSeconds);
    n.wearOffSeconds = num("wearOffSeconds", n.wearOffSeconds);
    return n;
}

Grime::Grime(const Map& map, Things& things, const Floors& floors, const Filth& filth, const DefDatabase& defs)
    : map_(map), things_(things), floors_(floors), filth_(filth), defs_(defs), numbers_(GrimeNumbers::read(defs)) {
    bleeds_.assign(defs.size(), 0);
    for (const DefId id : defs.idsOfKind("Hediffs")) {
        bleeds_[id.value] = defs.get(id).number("injuryProps.bleedRate", 0.0) > 0.0 ? 1 : 0;
    }
}

Grime::GroundKind Grime::groundAt(float x, float y, int level) const {
    const CellPos c{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), level};
    if (!map_.onGround(c) || floors_.at(c).valid()) {
        return GroundKind::Clean;
    }
    switch (map_.terrain(c)) {
        case Terrain::Soil:
        case Terrain::Sand:
        case Terrain::RoughStone:
            return GroundKind::Soil;
        case Terrain::Mud:
        case Terrain::Marsh:
            return GroundKind::Mud;
        default:
            return GroundKind::Clean;
    }
}

void Grime::addBlood(PawnGrime& g, float amount, int kind, int64_t now) {
    if (kind < 0 || amount <= 0.0f) {
        return;
    }
    g.blood = std::min(1.0f, g.blood + amount);
    g.bloodKind = kind;
    g.bloodAt = now;
}

void Grime::addWeaponBlood(PawnGrime& g, float amount, int kind, int64_t now) {
    if (kind < 0 || amount <= 0.0f) {
        return;
    }
    g.weaponBlood = std::min(1.0f, g.weaponBlood + amount);
    g.weaponKind = kind;
    g.weaponAt = now;
}

void Grime::step(int64_t now, const std::vector<Event>& events, float rainRate) {
    entt::registry& reg = things_.registry();
    const GrimeNumbers& n = numbers_;

    // Blood from this step's blows first, so a body hit in the rain still shows it for a while.
    for (const Event& e : events) {
        if (e.type == EventType::PawnInjured) {
            if (!reg.valid(e.thing) || !e.def.valid() || e.def.value >= bleeds_.size() || bleeds_[e.def.value] == 0) {
                continue;
            }
            const Thing* thing = reg.try_get<Thing>(e.thing);
            if (thing == nullptr) {
                continue;
            }
            addBlood(pawns_[entt::to_integral(e.thing)], n.bloodPerDamage * e.amount, filth_.bloodKindOf(thing->def), now);
        } else if (e.type == EventType::MeleeHit && e.hit == ProjectileHitKind::Pawn && e.amount > 0.0f) {
            if (!reg.valid(e.thing) || !reg.valid(e.target)) {
                continue;
            }
            const Thing* attacker = reg.try_get<Thing>(e.thing);
            const Thing* victim = reg.try_get<Thing>(e.target);
            if (attacker == nullptr || victim == nullptr) {
                continue;
            }
            const int kind = filth_.bloodKindOf(victim->def);
            PawnGrime& g = pawns_[entt::to_integral(e.thing)];
            addBlood(g, n.spatterPerDamage * e.amount, kind, now);
            // A weapon, not its own fists or teeth (whose def is the pawn's own).
            if (e.def.valid() && e.def != attacker->def) {
                addWeaponBlood(g, n.weaponPerDamage * e.amount, kind, now);
            }
        }
    }

    // Every pawn on the map, in id order (the map is sorted), the ones never seen before added.
    for (const auto [entity, thing, pos] : reg.view<const Thing, const Position>().each()) {
        (void)pos;
        if (thing.category == ThingCategory::Pawn) {
            pawns_.try_emplace(entt::to_integral(entity));
        }
    }

    const float rain = std::clamp(rainRate, 0.0f, 1.5f);
    const float wear = grimePer(n.wearOffSeconds);
    for (auto it = pawns_.begin(); it != pawns_.end();) {
        const Entity entity = static_cast<Entity>(it->first);
        if (!reg.valid(entity)) {
            it = pawns_.erase(it);
            continue;
        }
        PawnGrime& g = it->second;
        const Position* pos = reg.try_get<Position>(entity);
        if (pos == nullptr) {  // carried, in a pod, off the map: nothing changes
            g.lastLevel = -1;
            ++it;
            continue;
        }
        const Health* health = reg.try_get<Health>(entity);
        const bool lying = health != nullptr && (health->dead || health->downed);
        const GroundKind ground = groundAt(pos->x, pos->y, pos->level);

        // Walking: metres moved since the last step.
        float walked = 0.0f;
        if (g.lastLevel == pos->level) {
            walked = std::hypot(pos->x - g.lastX, pos->y - g.lastY);
            if (walked > kGrimeMaxStepM) {
                walked = 0.0f;
            }
        }
        g.lastX = pos->x;
        g.lastY = pos->y;
        g.lastLevel = pos->level;
        if (walked > 0.0f && !lying) {
            // A metre of walking counts as about a second (people walk 1 to 2 m/s).
            if (ground == GroundKind::Soil) {
                grimeRaise(g.dirtLegs, walked * grimePer(n.soilFullSeconds) * n.soilMax, n.soilMax);
            } else if (ground == GroundKind::Mud) {
                grimeRaise(g.dirtLegs, walked * grimePer(n.mudFullSeconds), 1.0f);
            }
        }
        // Lying or crawling on bare ground.
        if (lying && ground != GroundKind::Clean) {
            const float per = grimePer(n.groundFullSeconds);
            grimeRaise(g.dirtBody, per, 1.0f);
            grimeRaise(g.dirtLegs, per, 1.0f);
        }
        // Dirty work, while at it (standing at the job, not walking to it).
        if (const Worker* worker = reg.try_get<Worker>(entity); worker != nullptr && !lying && walked <= 0.01f &&
                                                                 grimeDirtyWork(worker->job.type)) {
            const float per = grimePer(n.workFullSeconds) * n.workMax;
            grimeRaise(g.dirtBody, per, n.workMax);
            grimeRaise(g.dirtLegs, per, n.workMax);
        }

        // Water.
        const float height = bodyHeight_ ? bodyHeight_(entity) : kPersonHeightM;
        const WaterContact water = pos->level == 0 ? waterContactAt(map_, pos->x, pos->y, 0, height) : WaterContact{};
        switch (water.band) {
            case WaterBand::Ankle:
                grimeLower(g.dirtLegs, grimePer(n.ankleWashSeconds));
                break;
            case WaterBand::Waist:
            case WaterBand::Chest:
                grimeLower(g.dirtLegs, grimePer(n.ankleWashSeconds));
                grimeLower(g.dirtBody, grimePer(n.waistWashSeconds));
                grimeLower(g.blood, grimePer(n.waistWashSeconds));
                break;
            case WaterBand::Swimming: {
                const float per = grimePer(n.swimWashSeconds);
                grimeLower(g.dirtLegs, per);
                grimeLower(g.dirtBody, per);
                grimeLower(g.blood, per);
                grimeLower(g.weaponBlood, per);
                break;
            }
            default:
                break;
        }
        // Rain under the open sky.
        if (rain > 0.001f && map_.inBounds(cellOf(*pos)) && map_.roof(cellOf(*pos)) == Roof::None) {
            const float per = rain * grimePer(n.rainWashSeconds);
            grimeLower(g.dirtLegs, per);
            grimeLower(g.dirtBody, per);
            grimeLower(g.blood, per);
            grimeLower(g.weaponBlood, per);
        }
        // It all wears off in time (not off a corpse lying in the dirt).
        if (!(health != nullptr && health->dead)) {
            grimeLower(g.dirtLegs, wear);
            grimeLower(g.dirtBody, wear);
            grimeLower(g.blood, wear);
            grimeLower(g.weaponBlood, wear);
        }
        if (g.blood <= 0.0f) {
            g.bloodKind = -1;
        }
        if (g.weaponBlood <= 0.0f) {
            g.weaponKind = -1;
        }
        ++it;
    }
}

const PawnGrime* Grime::of(Entity pawn) const {
    const auto it = pawns_.find(entt::to_integral(pawn));
    return it == pawns_.end() ? nullptr : &it->second;
}

void Grime::set(Entity pawn, float dirtLegs, float dirtBody, float blood, int bloodKind, int64_t now) {
    PawnGrime& g = pawns_[entt::to_integral(pawn)];
    g.dirtLegs = std::clamp(dirtLegs, 0.0f, 1.0f);
    g.dirtBody = std::clamp(dirtBody, 0.0f, 1.0f);
    g.blood = std::clamp(blood, 0.0f, 1.0f);
    g.bloodKind = g.blood > 0.0f ? bloodKind : -1;
    g.bloodAt = now;
}

void Grime::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(pawns_.size()));
    for (const auto& [id, g] : pawns_) {
        h.add(id);
        h.add(g.dirtLegs);
        h.add(g.dirtBody);
        h.add(g.blood);
        h.add(g.weaponBlood);
        h.add(g.bloodKind);
        h.add(g.weaponKind);
        h.add(g.bloodAt);
        h.add(g.weaponAt);
        h.add(g.lastX);
        h.add(g.lastY);
        h.add(g.lastLevel);
    }
}

GrimeState Grime::state() const {
    GrimeState out;
    const auto kindName = [&](int kind) {
        return kind >= 0 && static_cast<size_t>(kind) < filth_.kinds().size() ? filth_.kinds()[static_cast<size_t>(kind)].name : std::string();
    };
    for (const auto& [id, g] : pawns_) {
        GrimeState::Entry e;
        e.pawn = id;
        e.dirtLegs = g.dirtLegs;
        e.dirtBody = g.dirtBody;
        e.blood = g.blood;
        e.weaponBlood = g.weaponBlood;
        e.bloodKind = kindName(g.bloodKind);
        e.weaponKind = kindName(g.weaponKind);
        e.bloodAt = g.bloodAt;
        e.weaponAt = g.weaponAt;
        e.lastX = g.lastX;
        e.lastY = g.lastY;
        e.lastLevel = g.lastLevel;
        out.pawns.push_back(std::move(e));
    }
    return out;
}

bool Grime::restore(const GrimeState& state, std::string& error) {
    std::map<uint32_t, PawnGrime> pawns;
    const auto kindOf = [&](const std::string& name, int& out) {
        out = name.empty() ? -1 : filth_.kindIndex(name);
        if (!name.empty() && out < 0) {
            error = "grime: unknown filth kind " + name;
            return false;
        }
        return true;
    };
    for (const GrimeState::Entry& e : state.pawns) {
        PawnGrime g;
        g.dirtLegs = std::clamp(e.dirtLegs, 0.0f, 1.0f);
        g.dirtBody = std::clamp(e.dirtBody, 0.0f, 1.0f);
        g.blood = std::clamp(e.blood, 0.0f, 1.0f);
        g.weaponBlood = std::clamp(e.weaponBlood, 0.0f, 1.0f);
        if (!kindOf(e.bloodKind, g.bloodKind) || !kindOf(e.weaponKind, g.weaponKind)) {
            return false;
        }
        g.bloodAt = e.bloodAt;
        g.weaponAt = e.weaponAt;
        g.lastX = e.lastX;
        g.lastY = e.lastY;
        g.lastLevel = e.lastLevel;
        pawns[e.pawn] = g;
    }
    pawns_ = std::move(pawns);
    return true;
}

}  // namespace colony

#include "ColonySim/Spoilage.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float spoilFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

}  // namespace

SpoilageNumbers SpoilageNumbers::read(const DefDatabase& defs) {
    SpoilageNumbers n;
    const Def* def = defs.find("Rules", "SpoilageRules");
    if (def == nullptr) {
        return n;
    }
    n.intervalSeconds = std::max(1, static_cast<int>(def->integer("intervalSeconds", n.intervalSeconds)));
    n.rotFullAboveC = spoilFloat(*def, "rot.fullSpeedAboveC", n.rotFullAboveC);
    n.rotNoneBelowC = spoilFloat(*def, "rot.stopsBelowC", n.rotNoneBelowC);
    n.outdoorFactor = spoilFloat(*def, "outdoorDeteriorationFactor", n.outdoorFactor);
    n.poisonOnIngest = spoilFloat(*def, "poison.onIngestChance", n.poisonOnIngest);
    n.dirtyChance = spoilFloat(*def, "poison.dirtyKitchenChance", n.dirtyChance);
    n.cleanAtLeast = spoilFloat(*def, "poison.cleanAtLeast", n.cleanAtLeast);
    n.dirtiest = spoilFloat(*def, "poison.dirtiestAt", n.dirtiest);
    n.unskilledChance = spoilFloat(*def, "poison.unskilledChance", n.unskilledChance);
    n.skillSafe = std::max(1, static_cast<int>(def->integer("poison.skillSafeAt", n.skillSafe)));
    n.corpseRotStartDays = spoilFloat(*def, "corpse.daysToRotStart", n.corpseRotStartDays);
    n.corpseDessicatedDays = std::max(n.corpseRotStartDays, spoilFloat(*def, "corpse.daysToDessicated", n.corpseDessicatedDays));
    n.fliesFromDays = spoilFloat(*def, "corpse.flies.fromDays", n.fliesFromDays);
    n.fliesFullDays = std::max(n.fliesFromDays, spoilFloat(*def, "corpse.flies.fullDays", n.fliesFullDays));
    n.fliesFadeFromDays = std::min(n.corpseDessicatedDays, spoilFloat(*def, "corpse.flies.fadeFromDays", n.fliesFadeFromDays));
    n.fliesColdC = spoilFloat(*def, "corpse.flies.stillAtOrBelowC", n.fliesColdC);
    n.fliesWarmC = std::max(n.fliesColdC, spoilFloat(*def, "corpse.flies.allFlyingAtC", n.fliesWarmC));
    n.maggotsFromDays = spoilFloat(*def, "corpse.maggots.fromDays", n.maggotsFromDays);
    n.maggotsFullDays = std::max(n.maggotsFromDays, spoilFloat(*def, "corpse.maggots.fullDays", n.maggotsFullDays));
    n.maggotsGoneDays = std::max(n.maggotsFullDays, spoilFloat(*def, "corpse.maggots.goneDays", n.maggotsGoneDays));
    return n;
}

SpoilageSystem::SpoilageSystem(Map& map, Things& things, TemperatureSystem& temperature, RoomStats& roomStats, const DefDatabase& defs,
                   uint64_t seed)
    : map_(map), things_(things), temperature_(temperature), roomStats_(roomStats), defs_(defs),
      numbers_(SpoilageNumbers::read(defs)), rng_(seed, 0x5B011A6Eull) {
    rotDays_.assign(defs.size(), 0.0f);
    deterioration_.assign(defs.size(), 0.0f);
    isMeal_.assign(defs.size(), 0);
    shelters_.assign(defs.size(), 0);
    noCorpseRot_.assign(defs.size(), 0);
    for (uint32_t i = 0; i < defs.size(); ++i) {
        const Def& d = defs.get(DefId{i});
        if (d.kind() != "Things") {
            continue;
        }
        rotDays_[i] = static_cast<float>(thingRotDays(d));
        deterioration_[i] = spoilFloat(d, "stats.deteriorationRate", 0.0f);
        isMeal_[i] = d.text("ingestible.foodType") == "Meal" ? 1 : 0;
        shelters_[i] = d.flag("building.preventDeteriorationOnTop") ? 1 : 0;
        noCorpseRot_[i] = d.flag("race.mechanical", false) ? 1 : 0;
    }
}

float SpoilageSystem::rotSpeedAt(float celsius) const {
    if (celsius >= numbers_.rotFullAboveC) {
        return 1.0f;
    }
    if (celsius <= numbers_.rotNoneBelowC) {
        return 0.0f;
    }
    return (celsius - numbers_.rotNoneBelowC) / (numbers_.rotFullAboveC - numbers_.rotNoneBelowC);
}

namespace {

// 0 at or below `from`, 1 at or above `to`, linear between (1 from `from` on if they are the same).
float spoilRamp(float x, float from, float to) {
    if (x >= to) {
        return x >= from ? 1.0f : 0.0f;
    }
    if (x <= from) {
        return 0.0f;
    }
    return (x - from) / (to - from);
}

}  // namespace

CorpseLook SpoilageSystem::corpseLook(float rotDays, float celsius) const {
    const SpoilageNumbers& n = numbers_;
    CorpseLook look;
    if (rotDays >= n.corpseDessicatedDays) {
        look.stage = CorpseStage::Dessicated;
        return look;
    }
    look.stage = rotDays >= n.corpseRotStartDays ? CorpseStage::Rotting : CorpseStage::Fresh;
    const float swarm = spoilRamp(rotDays, n.fliesFromDays, n.fliesFullDays) *
                        (1.0f - spoilRamp(rotDays, n.fliesFadeFromDays, n.corpseDessicatedDays));
    look.flies = swarm;
    // In the cold the flies sit on the body rather than fly.
    look.flying = spoilRamp(celsius, n.fliesColdC, n.fliesWarmC);
    look.maggots = spoilRamp(rotDays, n.maggotsFromDays, n.maggotsFullDays) * (1.0f - spoilRamp(rotDays, n.maggotsFullDays, n.maggotsGoneDays));
    return look;
}

bool SpoilageSystem::corpseRots(DefId pawnDef) const {
    return pawnDef.valid() && pawnDef.value < noCorpseRot_.size() && noCorpseRot_[pawnDef.value] == 0;
}

void SpoilageSystem::rotCorpses(float days) {
    entt::registry& reg = things_.registry();
    for (const Entity e : reg.view<Health, Position>()) {
        if (!reg.get<Health>(e).dead) {
            continue;
        }
        const Thing* thing = reg.try_get<Thing>(e);
        if (thing == nullptr || !corpseRots(thing->def)) {
            continue;
        }
        const CellPos cell = cellOf(reg.get<Position>(e));
        if (!map_.inBounds(cell)) {
            continue;
        }
        CorpseRot& rot = reg.get_or_emplace<CorpseRot>(e);
        if (rot.days < numbers_.corpseDessicatedDays) {
            rot.days = std::min(numbers_.corpseDessicatedDays, rot.days + rotSpeedAt(temperature_.temperatureAt(cell)) * days);
        }
    }
}

float SpoilageSystem::poisonChance(Entity cook, CellPos cell) {
    entt::registry& reg = things_.registry();
    float dirt = 0.0f;
    if (map_.inBounds(cell)) {
        const RoomStatValues* room = roomStats_.statsAt(cell);
        const float cleanliness = room != nullptr ? room->cleanliness : roomStats_.cellCleanliness(cell);
        dirt = std::clamp((numbers_.cleanAtLeast - cleanliness) / (numbers_.cleanAtLeast - numbers_.dirtiest), 0.0f, 1.0f);
    }
    float unskilled = 0.0f;
    if (const Colonist* colonist = reg.valid(cook) ? reg.try_get<Colonist>(cook) : nullptr) {
        const SkillRecord* skill = colonist->skill("Cooking");
        const int level = skill != nullptr ? skill->level : 0;
        unskilled = std::clamp(static_cast<float>(numbers_.skillSafe - level) / static_cast<float>(numbers_.skillSafe), 0.0f, 1.0f);
    }
    return std::clamp(numbers_.dirtyChance * dirt + numbers_.unskilledChance * unskilled, 0.0f, 1.0f);
}

void SpoilageSystem::madeBy(Entity item, const std::vector<BillUsed>& used, Entity cook, CellPos cell) {
    entt::registry& reg = things_.registry();
    Spoilage* spoilage = reg.valid(item) ? reg.try_get<Spoilage>(item) : nullptr;
    if (spoilage == nullptr || !isMeal_[reg.get<Thing>(item).def.value]) {
        return;
    }
    spoilage->ingredients.clear();
    for (const BillUsed& u : used) {
        spoilage->ingredients.push_back(u.def);
    }
    std::sort(spoilage->ingredients.begin(), spoilage->ingredients.end(), [](DefId a, DefId b) { return a.value < b.value; });
    spoilage->ingredients.erase(std::unique(spoilage->ingredients.begin(), spoilage->ingredients.end()),
                                spoilage->ingredients.end());
    const float chance = poisonChance(cook, cell);
    spoilage->poisoned = chance > 0.0f && rng_.unit() < chance;
}

bool SpoilageSystem::eaten(Entity pawn, Entity item) {
    entt::registry& reg = things_.registry();
    const Spoilage* spoilage = reg.valid(item) ? reg.try_get<Spoilage>(item) : nullptr;
    if (spoilage == nullptr || !spoilage->poisoned || rng_.unit() >= numbers_.poisonOnIngest) {
        return false;
    }
    if (events_ != nullptr) {
        Event event;
        event.type = EventType::PawnPoisoned;
        event.thing = pawn;
        event.def = reg.get<Thing>(item).def;
        if (const Position* pos = reg.try_get<Position>(pawn)) {
            event.cell = cellOf(*pos);
        }
        event.target = item;
        event.message = "food poisoning";
        events_->push_back(std::move(event));
    }
    if (poisoned_) {
        poisoned_(pawn);
    }
    return true;
}

bool SpoilageSystem::humanlikeIngredient(Entity item) const {
    const entt::registry& reg = things_.registry();
    const Spoilage* spoilage = reg.valid(item) ? reg.try_get<Spoilage>(item) : nullptr;
    if (spoilage == nullptr) {
        return false;
    }
    return std::any_of(spoilage->ingredients.begin(), spoilage->ingredients.end(),
                       [&](DefId ingredient) { return defs_.get(ingredient).flag("humanlike"); });
}

void SpoilageSystem::step(int64_t now) {
    if (now % numbers_.intervalSeconds != 0) {
        return;
    }
    entt::registry& reg = things_.registry();
    const float days = static_cast<float>(numbers_.intervalSeconds) / static_cast<float>(kSecondsPerDay);
    // In id order, so what is destroyed (and the ids freed) is the same on every platform and after a load.
    scratch_.clear();
    for (const Entity e : reg.view<Spoilage, Position>()) {
        scratch_.push_back(e);
    }
    std::sort(scratch_.begin(), scratch_.end());
    // Bodies first: nothing is destroyed there, so the order does not matter.
    rotCorpses(days);
    for (const Entity e : scratch_) {
        const DefId def = reg.get<Thing>(e).def;
        const CellPos cell = cellOf(reg.get<Position>(e));
        if (!map_.inBounds(cell)) {
            continue;
        }
        Spoilage& spoilage = reg.get<Spoilage>(e);
        const float rotDays = rotDays_[def.value];
        if (rotDays > 0.0f) {
            const float speed = rotSpeedAt(temperature_.temperatureAt(cell));
            spoilage.rot += speed * days / rotDays;
            if (spoilage.rot >= 1.0f) {
                report(e, def, cell, "rotted");
                things_.destroy(e);
                continue;
            }
        }
        const float rate = deterioration_[def.value];
        if (rate > 0.0f && !sheltered(cell)) {
            HitPoints* hp = reg.try_get<HitPoints>(e);
            if (hp == nullptr) {
                continue;
            }
            spoilage.decay += rate * numbers_.outdoorFactor * days;
            const float whole = std::floor(spoilage.decay);
            if (whole >= 1.0f) {
                spoilage.decay -= whole;
                hp->current -= static_cast<int>(whole);
            }
            if (hp->current <= 0) {
                report(e, def, cell, "weathered");
                things_.destroy(e);
            }
        }
    }
}

bool SpoilageSystem::sheltered(CellPos cell) const {
    if (map_.roof(cell) != Roof::None) {
        return true;
    }
    const Entity building = map_.buildingAt(cell);
    return building != kNoEntity && shelters_[things_.registry().get<Thing>(building).def.value] != 0;
}

void SpoilageSystem::report(Entity item, DefId def, CellPos cell, const char* how) {
    if (events_ == nullptr) {
        return;
    }
    Event event;
    event.type = EventType::ItemSpoiled;
    event.thing = item;
    event.def = def;
    event.cell = cell;
    event.message = how;
    events_->push_back(std::move(event));
}

void SpoilageSystem::hashState(StateHasher& hasher) const {
    hasher.add(rng_.state());
    hasher.add(rng_.increment());
}

}  // namespace colony

#include "ColonySim/Fuel.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

FuelSpec readFuelSpec(const DefDatabase& defs, const Def& def) {
    FuelSpec spec;
    if (!def.has("fuel.capacity")) {
        return spec;
    }
    spec.item = defs.idOf("Things", def.text("fuel.item"));
    spec.capacity = static_cast<float>(def.number("fuel.capacity"));
    spec.present = spec.item.valid() && spec.capacity > 0.0f;
    double perDay = def.number("fuel.perDay");
    const double hoursPerItem = def.number("fuel.burnHoursPerWood");
    if (perDay <= 0.0 && hoursPerItem > 0.0) {
        perDay = 24.0 / hoursPerItem;
    }
    spec.perSecond = static_cast<float>(perDay / static_cast<double>(kSecondsPerDay));
    spec.perUse = static_cast<float>(def.number("fuel.perUse"));
    spec.autoRefuelBelow = static_cast<float>(def.number("fuel.autoRefuelPercent", spec.autoRefuelBelow));
    spec.initial = static_cast<float>(std::clamp(def.number("fuel.initialPercent", 0.0), 0.0, 1.0));
    return spec;
}

FuelSystem::FuelSystem(Things& things, const DefDatabase& defs, std::vector<Event>& events)
    : things_(things), defs_(defs), events_(events) {
    specs_.assign(defs_.size(), FuelSpec{});
    for (const DefId id : defs_.idsOfKind("Things")) {
        specs_[id.value] = readFuelSpec(defs_, defs_.get(id));
    }
}

const FuelSpec& FuelSystem::spec(DefId def) const {
    return def.valid() && def.value < specs_.size() ? specs_[def.value] : none_;
}

const FuelSpec& FuelSystem::specOf(Entity building) const {
    const Thing* thing = things_.registry().try_get<Thing>(building);
    return thing != nullptr ? spec(thing->def) : none_;
}

void FuelSystem::step() {
    entt::registry& reg = things_.registry();
    for (const auto [building, fuel, thing] : reg.view<Fuel, const Thing>().each()) {
        const FuelSpec& s = spec(thing.def);
        if (s.perSecond <= 0.0f || fuel.amount <= 0.0f || (burning_ && !burning_(building))) {
            continue;
        }
        use(building, s.perSecond);
    }
}

bool FuelSystem::lit(Entity building) const {
    const Fuel* fuel = things_.registry().try_get<Fuel>(building);
    return fuel == nullptr || fuel->amount > 0.0f;
}

bool FuelSystem::canUse(Entity building, float amount) const {
    const Fuel* fuel = things_.registry().try_get<Fuel>(building);
    return fuel == nullptr || fuel->amount + 1.0e-4f >= amount;
}

void FuelSystem::use(Entity building, float amount) {
    Fuel* fuel = things_.registry().try_get<Fuel>(building);
    if (fuel == nullptr || fuel->amount <= 0.0f || amount <= 0.0f) {
        return;
    }
    fuel->amount -= amount;
    if (fuel->amount > 1.0e-4f) {
        return;
    }
    fuel->amount = 0.0f;
    if (lights_ != nullptr) {
        lights_->markDirty();
    }
    Event event;
    event.type = EventType::OutOfFuel;
    event.thing = building;
    event.def = things_.registry().get<Thing>(building).def;
    if (const Position* pos = things_.registry().try_get<Position>(building)) {
        event.cell = cellOf(*pos);
    }
    events_.push_back(std::move(event));
}

float FuelSystem::fraction(Entity building) const {
    const Fuel* fuel = things_.registry().try_get<Fuel>(building);
    const FuelSpec& s = specOf(building);
    if (fuel == nullptr || !s.present) {
        return -1.0f;
    }
    return std::clamp(fuel->amount / s.capacity, 0.0f, 1.0f);
}

int FuelSystem::roomFor(Entity building) const {
    const Fuel* fuel = things_.registry().try_get<Fuel>(building);
    const FuelSpec& s = specOf(building);
    if (fuel == nullptr || !s.present) {
        return 0;
    }
    return std::max(0, static_cast<int>(std::floor(s.capacity - fuel->amount + 1.0e-4f)));
}

bool FuelSystem::wantsFuel(Entity building) const {
    const Fuel* fuel = things_.registry().try_get<Fuel>(building);
    const FuelSpec& s = specOf(building);
    if (fuel == nullptr || !s.present || roomFor(building) < 1) {
        return false;
    }
    return fuel->fillOrdered || (fuel->autoRefuel && fuel->amount <= s.capacity * s.autoRefuelBelow + 1.0e-4f);
}

int FuelSystem::add(Entity building, int count) {
    Fuel* fuel = things_.registry().try_get<Fuel>(building);
    const int taken = std::min(count, roomFor(building));
    if (fuel == nullptr || taken <= 0) {
        return 0;
    }
    if (fuel->amount <= 0.0f && lights_ != nullptr) {
        lights_->markDirty();
    }
    fuel->amount += static_cast<float>(taken);
    if (roomFor(building) < 1) {
        fuel->fillOrdered = false;
    }
    Event event;
    event.type = EventType::Refueled;
    event.thing = building;
    event.def = things_.registry().get<Thing>(building).def;
    event.amount = static_cast<float>(taken);
    if (const Position* pos = things_.registry().try_get<Position>(building)) {
        event.cell = cellOf(*pos);
    }
    events_.push_back(std::move(event));
    return taken;
}

bool FuelSystem::setRefuel(Entity building, bool autoRefuel, bool fillNow, std::string& error) {
    entt::registry& reg = things_.registry();
    Fuel* fuel = things_.spawned(building) ? reg.try_get<Fuel>(building) : nullptr;
    if (fuel == nullptr || reg.all_of<ConstructionSite>(building)) {
        error = "that building doesn't burn fuel";
        return false;
    }
    fuel->autoRefuel = autoRefuel;
    fuel->fillOrdered = fillNow && roomFor(building) >= 1;
    return true;
}

}  // namespace colony

#include "ColonySim/Ship.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

ShipPart shipPartFromName(const std::string& name) {
    if (name == "Beam") return ShipPart::Beam;
    if (name == "Casket") return ShipPart::Casket;
    if (name == "Reactor") return ShipPart::Reactor;
    if (name == "Engine") return ShipPart::Engine;
    if (name == "Core") return ShipPart::Core;
    if (name == "Sensor") return ShipPart::Sensor;
    return ShipPart::None;
}

// Two footprints on one level that share a stretch of edge.
bool shipRectsTouch(const CellRect& a, const CellRect& b) {
    if (a.level != b.level) {
        return false;
    }
    const int xGap = std::max(a.minX - (b.minX + b.sizeX), b.minX - (a.minX + a.sizeX));
    const int yGap = std::max(a.minY - (b.minY + b.sizeY), b.minY - (a.minY + a.sizeY));
    const int xOverlap = std::min(a.minX + a.sizeX, b.minX + b.sizeX) - std::max(a.minX, b.minX);
    const int yOverlap = std::min(a.minY + a.sizeY, b.minY + b.sizeY) - std::max(a.minY, b.minY);
    return (xGap == 0 && yOverlap > 0) || (yGap == 0 && xOverlap > 0);
}

}  // namespace

const char* shipPhaseName(ShipPhase phase) {
    switch (phase) {
        case ShipPhase::Idle: return "Idle";
        case ShipPhase::Activating: return "Activating";
        case ShipPhase::Ready: return "Ready";
        case ShipPhase::Launched: return "Launched";
    }
    return "?";
}

ShipRules ShipRules::read(const DefDatabase& defs) {
    ShipRules r;
    const Def* rules = defs.find("Rules", "ShipRules");
    if (rules == nullptr) {
        return r;
    }
    r.activationDays = rules->number("activationDays", r.activationDays);
    r.waveFirstHours = rules->number("waveFirstHours", r.waveFirstHours);
    r.waveIntervalHours = std::max(1.0, rules->number("waveIntervalHours", r.waveIntervalHours));
    r.wavePointsFactor = static_cast<float>(rules->number("wavePointsFactor", r.wavePointsFactor));
    r.enginesNeeded = static_cast<int>(rules->integer("enginesNeeded", r.enginesNeeded));
    if (const size_t n = rules->count("waveFactions")) {
        r.waveFactions.clear();
        for (size_t i = 0; i < n; ++i) {
            r.waveFactions.push_back(rules->textAt("waveFactions", i));
        }
    }
    return r;
}

ShipSystem::ShipSystem(Things& things, RaidSystem& raids, const DefDatabase& defs, const GameTime& time)
    : things_(things), raids_(raids), defs_(defs), time_(time) {
    rules_ = ShipRules::read(defs);
    partByDef_.assign(defs.size(), ShipPart::None);
    for (const DefId id : defs.idsOfKind("Things")) {
        partByDef_[id.value] = shipPartFromName(defs.get(id).text("ship.part"));
    }
}

ShipPart ShipSystem::partOf(DefId def) const {
    return def.valid() && def.value < partByDef_.size() ? partByDef_[def.value] : ShipPart::None;
}

bool ShipSystem::alive(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Position>(pawn)) {
        return false;
    }
    const Health* health = reg.try_get<Health>(pawn);
    return health != nullptr && !health->dead;
}

void ShipSystem::emit(EventType type, Entity thing, float amount, const char* message) const {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.thing = thing;
    e.amount = amount;
    e.message = message;
    const entt::registry& reg = things_.registry();
    if (reg.valid(thing)) {
        if (const Thing* t = reg.try_get<Thing>(thing)) {
            e.def = t->def;
        }
        if (const Position* p = reg.try_get<Position>(thing)) {
            e.cell = cellOf(*p);
        }
    }
    events_->push_back(std::move(e));
}

ShipParts ShipSystem::parts() const {
    ShipParts out;
    const entt::registry& reg = things_.registry();
    std::vector<CellRect> beams;
    std::vector<std::pair<Entity, CellRect>> caskets;
    for (const auto [e, thing, foot] : reg.view<const Thing, const Footprint>().each()) {
        switch (partOf(thing.def)) {
            case ShipPart::Beam: ++out.beams; beams.push_back(foot.rect()); break;
            case ShipPart::Casket: ++out.caskets; caskets.emplace_back(e, foot.rect()); break;
            case ShipPart::Reactor: ++out.reactors; break;
            case ShipPart::Engine: ++out.engines; break;
            case ShipPart::Core: ++out.cores; break;
            case ShipPart::Sensor: ++out.sensors; break;
            case ShipPart::None: break;
        }
    }
    for (const auto& [casket, rect] : caskets) {
        const bool connected = std::any_of(beams.begin(), beams.end(), [&](const CellRect& b) { return shipRectsTouch(rect, b); });
        if (!connected) {
            ++out.unconnectedCaskets;
            continue;
        }
        for (size_t i = 0; i < state_.caskets.size(); ++i) {
            if (state_.caskets[i] == casket && alive(state_.riders[i])) {
                ++out.ridersAboard;
            }
        }
    }
    return out;
}

std::string ShipSystem::launchProblem() const {
    if (state_.phase == ShipPhase::Launched) {
        return "the ship has already launched";
    }
    if (state_.phase != ShipPhase::Ready) {
        return "the reactor is not active yet";
    }
    const ShipParts p = parts();
    if (p.engines < rules_.enginesNeeded) {
        return "the ship needs " + std::to_string(rules_.enginesNeeded) + " engines (it has " + std::to_string(p.engines) + ")";
    }
    if (p.cores < 1) {
        return "the ship needs a computer core";
    }
    if (p.sensors < 1) {
        return "the ship needs a sensor cluster";
    }
    if (p.ridersAboard < 1) {
        return "no one rides in a cryptosleep casket that touches a structural beam";
    }
    return std::string();
}

int64_t ShipSystem::activationLeft() const {
    if (state_.phase != ShipPhase::Activating) {
        return 0;
    }
    const int64_t total = static_cast<int64_t>(rules_.activationDays * static_cast<double>(kSecondsPerDay));
    return std::max<int64_t>(0, state_.startedAt + total - time_.seconds);
}

bool ShipSystem::startReactor(Entity reactor, std::string& error) {
    const entt::registry& reg = things_.registry();
    if (state_.phase == ShipPhase::Launched) {
        error = "the ship has already launched";
        return false;
    }
    if (state_.phase != ShipPhase::Idle) {
        error = "the reactor is already running";
        return false;
    }
    const Thing* t = reg.valid(reactor) ? reg.try_get<Thing>(reactor) : nullptr;
    if (t == nullptr || partOf(t->def) != ShipPart::Reactor || !reg.all_of<Footprint>(reactor)) {
        error = "that is not a finished ship reactor";
        return false;
    }
    state_.phase = ShipPhase::Activating;
    state_.reactor = reactor;
    state_.startedAt = time_.seconds;
    state_.waves = 0;
    state_.nextWave = time_.seconds + static_cast<int64_t>(rules_.waveFirstHours * static_cast<double>(kSecondsPerHour));
    emit(EventType::ShipReactorStarted, reactor, static_cast<float>(rules_.activationDays), "started");
    return true;
}

bool ShipSystem::board(Entity casket, Entity pawn, std::string& error) {
    const entt::registry& reg = things_.registry();
    const Thing* t = reg.valid(casket) ? reg.try_get<Thing>(casket) : nullptr;
    if (t == nullptr || partOf(t->def) != ShipPart::Casket) {
        error = "that is not a ship cryptosleep casket";
        return false;
    }
    if (state_.phase == ShipPhase::Launched) {
        error = "the ship has already launched";
        return false;
    }
    if (pawn != kNoEntity) {
        if (!alive(pawn)) {
            error = "that pawn is not alive";
            return false;
        }
        const Faction* f = reg.try_get<Faction>(pawn);
        if (f != nullptr && f->id != kPlayerFaction && !reg.all_of<Prisoner>(pawn)) {
            error = "that pawn is not part of the colony";
            return false;
        }
    }
    for (size_t i = 0; i < state_.caskets.size();) {
        if (state_.caskets[i] == casket || (pawn != kNoEntity && state_.riders[i] == pawn)) {
            state_.caskets.erase(state_.caskets.begin() + static_cast<std::ptrdiff_t>(i));
            state_.riders.erase(state_.riders.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (pawn != kNoEntity) {
        state_.caskets.push_back(casket);
        state_.riders.push_back(pawn);
    }
    return true;
}

bool ShipSystem::launch(std::string& error) {
    error = launchProblem();
    if (!error.empty()) {
        return false;
    }
    // The riders in connected caskets leave; a rider in a casket that touches no beam stays behind.
    std::vector<CellRect> beams;
    for (const auto [e, thing, foot] : things_.registry().view<const Thing, const Footprint>().each()) {
        if (partOf(thing.def) == ShipPart::Beam) {
            beams.push_back(foot.rect());
        }
    }
    std::vector<Entity> leaving;
    for (size_t i = 0; i < state_.caskets.size(); ++i) {
        const Footprint* foot = things_.registry().try_get<Footprint>(state_.caskets[i]);
        if (foot == nullptr || !alive(state_.riders[i])) {
            continue;
        }
        const CellRect rect = foot->rect();
        if (std::any_of(beams.begin(), beams.end(), [&](const CellRect& b) { return shipRectsTouch(rect, b); })) {
            leaving.push_back(state_.riders[i]);
        }
    }
    state_.escaped = static_cast<int>(leaving.size());
    state_.phase = ShipPhase::Launched;
    state_.launchedAt = time_.seconds;
    state_.caskets.clear();
    state_.riders.clear();
    emit(EventType::ShipLaunched, state_.reactor, static_cast<float>(state_.escaped), "launched");
    for (const Entity pawn : leaving) {
        raids_.leaveMap(pawn);
    }
    return true;
}

bool ShipSystem::sendWave() {
    if (rules_.waveFactions.empty()) {
        return false;
    }
    const float points = raids_.threatPoints() * rules_.wavePointsFactor;
    for (size_t k = 0; k < rules_.waveFactions.size(); ++k) {
        const std::string& faction = rules_.waveFactions[(static_cast<size_t>(state_.waves) + k) % rules_.waveFactions.size()];
        std::string why;
        if (raids_.startRaid(faction, points, -1, why)) {
            return true;
        }
    }
    return false;
}

void ShipSystem::step() {
    if (state_.phase == ShipPhase::Launched) {
        return;
    }
    const entt::registry& reg = things_.registry();
    if (time_.seconds % 15 == 0 && !state_.caskets.empty()) {
        // Riders who died and caskets that are gone drop out.
        for (size_t i = 0; i < state_.caskets.size();) {
            if (!reg.valid(state_.caskets[i]) || !reg.all_of<Footprint>(state_.caskets[i]) || !alive(state_.riders[i])) {
                state_.caskets.erase(state_.caskets.begin() + static_cast<std::ptrdiff_t>(i));
                state_.riders.erase(state_.riders.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
    if (state_.phase == ShipPhase::Idle) {
        return;
    }
    if (!reg.valid(state_.reactor) || !reg.all_of<Footprint>(state_.reactor)) {
        // Destroyed (or taken down): the activation is lost and the raids stop.
        emit(EventType::ShipReactorLost, kNoEntity, 0.0f, state_.phase == ShipPhase::Ready ? "active reactor lost" : "reactor lost");
        state_.phase = ShipPhase::Idle;
        state_.reactor = kNoEntity;
        return;
    }
    if (state_.phase != ShipPhase::Activating) {
        return;
    }
    if (activationLeft() == 0) {
        state_.phase = ShipPhase::Ready;
        emit(EventType::ShipReactorReady, state_.reactor, 0.0f, "active");
        return;
    }
    if (time_.seconds >= state_.nextWave) {
        if (sendWave()) {
            ++state_.waves;
            emit(EventType::ShipWave, state_.reactor, static_cast<float>(state_.waves), "wave");
        }
        state_.nextWave = time_.seconds + static_cast<int64_t>(rules_.waveIntervalHours * static_cast<double>(kSecondsPerHour));
    }
}

void ShipSystem::forget(Entity thing) {
    for (size_t i = 0; i < state_.caskets.size();) {
        if (state_.caskets[i] == thing || state_.riders[i] == thing) {
            state_.caskets.erase(state_.caskets.begin() + static_cast<std::ptrdiff_t>(i));
            state_.riders.erase(state_.riders.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

bool ShipSystem::restore(ShipState state, std::string& error) {
    const entt::registry& reg = things_.registry();
    if (state.caskets.size() != state.riders.size()) {
        error = "the save's ship riders don't match its caskets";
        return false;
    }
    if (state.phase == ShipPhase::Activating || state.phase == ShipPhase::Ready) {
        if (!reg.valid(state.reactor) || !reg.all_of<Footprint>(state.reactor)) {
            error = "the save has a running ship reactor that isn't there";
            return false;
        }
    }
    for (size_t i = 0; i < state.caskets.size(); ++i) {
        if (!reg.valid(state.caskets[i]) || !reg.valid(state.riders[i])) {
            error = "the save has a ship rider or casket that isn't there";
            return false;
        }
    }
    state_ = std::move(state);
    return true;
}

void ShipSystem::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(state_.phase));
    h.add(static_cast<uint64_t>(entt::to_integral(state_.reactor)));
    h.add(state_.startedAt);
    h.add(state_.nextWave);
    h.add(state_.waves);
    h.add(state_.escaped);
    h.add(state_.launchedAt);
    h.add(static_cast<uint64_t>(state_.caskets.size()));
    for (size_t i = 0; i < state_.caskets.size(); ++i) {
        h.add(static_cast<uint64_t>(entt::to_integral(state_.caskets[i])));
        h.add(static_cast<uint64_t>(entt::to_integral(state_.riders[i])));
    }
}

}  // namespace colony

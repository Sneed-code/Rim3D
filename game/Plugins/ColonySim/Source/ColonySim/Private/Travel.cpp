#include "ColonySim/Travel.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// Whether `hour` (0 to 23) lies in the hours sleepFrom..sleepTo, which may wrap past midnight.
bool travelInHours(int hour, int from, int to) {
    return from <= to ? (hour >= from && hour <= to) : (hour >= from || hour <= to);
}

}  // namespace

const char* skipKindName(SkipKind kind) {
    switch (kind) {
        case SkipKind::None: return "None";
        case SkipKind::Night: return "Night";
        case SkipKind::Travel: return "Travel";
    }
    return "?";
}

TravelRules TravelRules::read(const DefDatabase& defs) {
    TravelRules r;
    const Def* rules = defs.find("Rules", "TravelRules");
    if (rules == nullptr) {
        return r;
    }
    r.visitedBlock = std::max(1, static_cast<int>(rules->integer("visitedBlock", r.visitedBlock)));
    r.visitedRadius = static_cast<float>(rules->number("visitedRadius", r.visitedRadius));
    r.minDistance = static_cast<float>(rules->number("minDistance", r.minDistance));
    r.detour = std::max(1.0f, static_cast<float>(rules->number("detour", r.detour)));
    r.defaultSpeed = std::max(0.5f, static_cast<float>(rules->number("defaultSpeed", r.defaultSpeed)));
    r.nightBatch = std::max(1, static_cast<int>(rules->integer("nightBatch", r.nightBatch)));
    r.travelBatch = std::max(1, static_cast<int>(rules->integer("travelBatch", r.travelBatch)));
    r.threatCheckSeconds = std::max(1, static_cast<int>(rules->integer("threatCheckSeconds", r.threatCheckSeconds)));
    r.freezeAnimals = rules->flag("freezeAnimals", r.freezeAnimals);
    return r;
}

TravelSystem::TravelSystem(Things& things, Map& map, JobSystem& jobs, RaidSystem& raids, Fire& fire, AnimalSystem& animals,
                           FighterSystem& fighters, MentalBreakSystem& breaks, HealthSystem& health, NeedsSystem& needs,
                           const DefDatabase& defs, const GameTime& time)
    : things_(things),
      map_(map),
      jobs_(jobs),
      raids_(raids),
      fire_(fire),
      animals_(animals),
      fighters_(fighters),
      breaks_(breaks),
      health_(health),
      needs_(needs),
      defs_(defs),
      time_(time) {
    rules_ = TravelRules::read(defs);
}

TravelSystem::~TravelSystem() = default;

int TravelSystem::batch() const {
    switch (state_.kind) {
        case SkipKind::Night: return rules_.nightBatch;
        case SkipKind::Travel: return rules_.travelBatch;
        case SkipKind::None: break;
    }
    return 0;
}

int TravelSystem::blocksWide() const { return (map_.width() + rules_.visitedBlock - 1) / rules_.visitedBlock; }
int TravelSystem::blocksHigh() const { return (map_.height() + rules_.visitedBlock - 1) / rules_.visitedBlock; }

bool TravelSystem::markBlock(int bx, int by) {
    if (bx < 0 || by < 0 || bx >= blocksWide() || by >= blocksHigh()) {
        return false;
    }
    if (grid_.empty()) {
        grid_.assign(static_cast<size_t>(blocksWide()) * static_cast<size_t>(blocksHigh()), 0);
    }
    uint8_t& block = grid_[static_cast<size_t>(by) * static_cast<size_t>(blocksWide()) + static_cast<size_t>(bx)];
    if (block != 0) {
        return false;
    }
    block = 1;
    ++visitedCount_;
    return true;
}

bool TravelSystem::visited(CellPos cell) const {
    if (grid_.empty() || !map_.inBounds(cell)) {
        return false;
    }
    const int bx = cell.x / rules_.visitedBlock;
    const int by = cell.y / rules_.visitedBlock;
    return grid_[static_cast<size_t>(by) * static_cast<size_t>(blocksWide()) + static_cast<size_t>(bx)] != 0;
}

size_t TravelSystem::visitedBlocks() const { return visitedCount_; }

void TravelSystem::noteVisits() {
    entt::registry& reg = things_.registry();
    for (const auto [pawn, pos] : reg.view<const PlayerControl, const Position>().each()) {
        if (pos.level != 0) {
            continue;
        }
        const int bx = static_cast<int>(std::floor(pos.x)) / rules_.visitedBlock;
        const int by = static_cast<int>(std::floor(pos.y)) / rules_.visitedBlock;
        if (bx == lastBlockX_ && by == lastBlockY_) {
            continue;
        }
        lastBlockX_ = bx;
        lastBlockY_ = by;
        // The blocks marked depend only on the pawn's block (its middle, not where in the block it stands), so marking
        // twice, or again after a load, changes nothing.
        const int reach = static_cast<int>(std::ceil(rules_.visitedRadius / static_cast<float>(rules_.visitedBlock)));
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                if (std::hypot(static_cast<float>(dx), static_cast<float>(dy)) * static_cast<float>(rules_.visitedBlock) <=
                    rules_.visitedRadius) {
                    markBlock(bx + dx, by + dy);
                }
            }
        }
    }
}

std::string TravelSystem::threat() const {
    const entt::registry& reg = things_.registry();
    const RaidState& raids = raids_.state();
    if (raids.alarm) {
        return "the alarm is sounding";
    }
    if (!raids.squads.empty()) {
        return "a raid is on the map";
    }
    if (fire_.count() > 0 || fire_.burningPawns() > 0) {
        return "something is on fire";
    }
    for (const auto [pawn, faction] : reg.view<const Faction>().each()) {
        if (faction.id == kPlayerFaction || !things_.spawned(pawn) || reg.all_of<Prisoner>(pawn) || reg.all_of<Animal>(pawn)) {
            continue;
        }
        const Health* health = reg.try_get<Health>(pawn);
        if (health == nullptr || !health->dead) {
            return "an enemy is on the map";
        }
    }
    for (const auto [animal, state] : reg.view<const Animal>().each()) {
        if (things_.spawned(animal) && animals_.hostile(animal)) {
            return "a hostile animal is about";
        }
    }
    for (const auto [pawn, worker] : reg.view<const Worker>().each()) {
        if (!things_.spawned(pawn) || reg.all_of<Animal>(pawn) || fighters_.factionOf(pawn) != kPlayerFaction) {
            continue;
        }
        if (breaks_.active(pawn)) {
            return "a colonist is in a mental break";
        }
        if (reg.all_of<Fighter>(pawn)) {
            return "colonists are under arms";
        }
    }
    return std::string();
}

int64_t TravelSystem::morningAfter() const {
    const NeedRules& needs = needs_.rules();
    const int64_t end = static_cast<int64_t>((needs.sleepToHour + 1) % 24) * kSecondsPerHour;
    const int64_t dayStart = time_.day() * kSecondsPerDay;
    int64_t morning = dayStart + end;
    if (morning <= time_.seconds) {
        morning += kSecondsPerDay;
    }
    return morning;
}

bool TravelSystem::canSkipNight(std::string& why) const {
    const NeedRules& needs = needs_.rules();
    if (!travelInHours(time_.hourOfDay(), needs.sleepFromHour, needs.sleepToHour)) {
        why = "it is not night: sleeping hours are " + std::to_string(needs.sleepFromHour) + ":00 to " +
              std::to_string((needs.sleepToHour + 1) % 24) + ":00";
        return false;
    }
    const std::string threatNow = threat();
    if (!threatNow.empty()) {
        why = "can't skip the night: " + threatNow;
        return false;
    }
    const entt::registry& reg = things_.registry();
    int asleep = 0;
    int waiting = 0;
    for (const auto [pawn, worker] : reg.view<const Worker>().each()) {
        if (!things_.spawned(pawn) || reg.all_of<Animal>(pawn) || fighters_.factionOf(pawn) != kPlayerFaction ||
            health_.incapacitated(pawn) || reg.all_of<PlayerControl>(pawn) || reg.all_of<Prisoner>(pawn)) {
            continue;
        }
        const Needs* need = reg.try_get<Needs>(pawn);
        if (need == nullptr) {
            continue;
        }
        if (jobIsLyingDown(worker.job) || worker.job.type == JobType::BedRest) {
            ++asleep;
            continue;
        }
        // Someone on a Sleep hour who is tired and not in bed yet holds the night back; someone rested, or on
        // another shift, stays up while the rest sleep.
        const Schedule* schedule = reg.try_get<Schedule>(pawn);
        const bool sleepHour = schedule != nullptr && schedule->hours[static_cast<size_t>(time_.hourOfDay())] == ScheduleKind::Sleep;
        if (sleepHour && need->rest < needs.fallAsleepBelow) {
            ++waiting;
        }
    }
    if (waiting > 0) {
        why = std::to_string(waiting) + (waiting == 1 ? " colonist isn't" : " colonists aren't") + " in bed yet";
        return false;
    }
    if (asleep == 0) {
        why = "nobody is asleep";
        return false;
    }
    return true;
}

int64_t TravelSystem::travelSeconds(Entity pawn, float x, float y, std::string& why) const {
    const entt::registry& reg = things_.registry();
    if (!std::isfinite(x) || !std::isfinite(y)) {
        why = "that place isn't a number";
        return 0;
    }
    if (!things_.spawned(pawn) || !reg.all_of<PlayerControl, Worker, Mover>(pawn)) {
        why = "only the colonist you control can travel";
        return 0;
    }
    if (health_.incapacitated(pawn) || fire_.pawnBurning(pawn)) {
        why = "that colonist can't travel now";
        return 0;
    }
    const Position& pos = reg.get<Position>(pawn);
    if (pos.level != 0) {
        why = "go outside first";
        return 0;
    }
    const CellPos to{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), 0};
    if (!map_.inBounds(to) || !pawnFitsAt(map_, x, y, kPawnRadius, 0)) {
        why = "nobody can stand there";
        return 0;
    }
    if (!visited(to)) {
        why = "you haven't been there";
        return 0;
    }
    const float distance = std::hypot(x - pos.x, y - pos.y);
    if (distance < rules_.minDistance) {
        why = "that is close enough to walk";
        return 0;
    }
    if (!jobs_.canReach(cellOf(pos), to)) {
        why = "there is no way there";
        return 0;
    }
    const std::string threatNow = threat();
    if (!threatNow.empty()) {
        why = "can't travel: " + threatNow;
        return 0;
    }
    // Metres per game second (one step): its own walking speed, slowed by what hurts it.
    float speed = reg.get<Mover>(pawn).metresPerStep;
    if (const Health* health = reg.try_get<Health>(pawn)) {
        speed *= health->movingFactor;
    }
    if (!(speed > 0.0f)) {
        speed = rules_.defaultSpeed;
    }
    return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(distance * rules_.detour / speed)));
}

void TravelSystem::emit(EventType type, const std::string& message, float amount) {
    if (events_ == nullptr) {
        return;
    }
    Event event;
    event.type = type;
    event.thing = state_.pawn;
    event.message = message;
    event.amount = amount;
    if (const Position* pos = state_.pawn != kNoEntity && things_.registry().valid(state_.pawn)
                                  ? things_.registry().try_get<Position>(state_.pawn)
                                  : nullptr) {
        event.x = pos->x;
        event.y = pos->y;
        event.cell = cellOf(*pos);
    }
    events_->push_back(std::move(event));
}

bool TravelSystem::startNight(std::string& error) {
    if (skipping()) {
        error = "time is already being skipped";
        return false;
    }
    if (!canSkipNight(error)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    state_.kind = SkipKind::Night;
    state_.startedAt = time_.seconds;
    state_.endsAt = morningAfter();
    state_.pawn = kNoEntity;
    // The colonist under control goes to bed with the rest: it is let go where it stands.
    const std::vector<Entity> controlled(reg.view<PlayerControl>().begin(), reg.view<PlayerControl>().end());
    for (const Entity pawn : controlled) {
        state_.pawn = pawn;
        reg.remove<PlayerControl>(pawn);
        jobs_.interrupt(pawn, "went to sleep");
        jobs_.putDown(pawn);
    }
    emit(EventType::SkipStarted, "night", static_cast<float>(state_.endsAt - state_.startedAt));
    return true;
}

bool TravelSystem::startTravel(Entity pawn, float x, float y, std::string& error) {
    if (skipping()) {
        error = "time is already being skipped";
        return false;
    }
    const int64_t seconds = travelSeconds(pawn, x, y, error);
    if (seconds <= 0) {
        return false;
    }
    state_.kind = SkipKind::Travel;
    state_.startedAt = time_.seconds;
    state_.endsAt = time_.seconds + seconds;
    state_.pawn = pawn;
    state_.destX = x;
    state_.destY = y;
    Mover& mover = things_.registry().get<Mover>(pawn);
    mover.vx = 0.0f;
    mover.vy = 0.0f;
    emit(EventType::SkipStarted, "travel", static_cast<float>(seconds));
    return true;
}

bool TravelSystem::cancel(const std::string& why, std::string& error) {
    if (!skipping()) {
        error = "no time is being skipped";
        return false;
    }
    finish(why, false);
    return true;
}

void TravelSystem::finish(const std::string& why, bool arrived) {
    entt::registry& reg = things_.registry();
    const SkipKind kind = state_.kind;
    if (kind == SkipKind::Travel && arrived && state_.pawn != kNoEntity && things_.spawned(state_.pawn)) {
        Position& pos = reg.get<Position>(state_.pawn);
        pos.x = state_.destX;
        pos.y = state_.destY;
        pos.level = 0;
        if (Mover* mover = reg.try_get<Mover>(state_.pawn)) {
            mover->path.clear();
            mover->next = 0;
            mover->vx = 0.0f;
            mover->vy = 0.0f;
            mover->gaveUp = false;
        }
        ++state_.travels;
    } else if (kind == SkipKind::Night && arrived) {
        ++state_.nights;
    }
    const int64_t skipped = time_.seconds - state_.startedAt;
    state_.skipped += skipped;
    hold_ += skipped;
    emit(EventType::SkipEnded, std::string(kind == SkipKind::Night ? "night: " : "travel: ") + why, static_cast<float>(skipped));
    state_.kind = SkipKind::None;
    state_.pawn = kNoEntity;
}

void TravelSystem::afterStep() {
    if (!skipping()) {
        return;
    }
    const int64_t startedAt = state_.startedAt;
    if (time_.seconds >= state_.endsAt) {
        finish(state_.kind == SkipKind::Night ? "morning" : "arrived", true);
        return;
    }
    if ((time_.seconds - startedAt) % rules_.threatCheckSeconds == 0) {
        const std::string threatNow = threat();
        if (!threatNow.empty()) {
            finish(threatNow, false);
            return;
        }
        // A traveller who goes down or dies on the way never arrives.
        if (state_.kind == SkipKind::Travel &&
            (!things_.spawned(state_.pawn) || health_.incapacitated(state_.pawn) || !things_.registry().all_of<PlayerControl>(state_.pawn))) {
            finish("the colonist can't go on", false);
        }
    }
}

bool TravelSystem::restore(TravelState state, std::string& error) {
    const size_t blocks = static_cast<size_t>(blocksWide()) * static_cast<size_t>(blocksHigh());
    for (const uint32_t index : state.visited) {
        if (index >= blocks) {
            error = "the save has a visited place off the map";
            return false;
        }
    }
    if (state.kind != SkipKind::None && state.endsAt < state.startedAt) {
        error = "the save has a skip that ends before it began";
        return false;
    }
    grid_.clear();
    visitedCount_ = 0;
    lastBlockX_ = -1;
    lastBlockY_ = -1;
    if (!state.visited.empty()) {
        grid_.assign(blocks, 0);
        for (const uint32_t index : state.visited) {
            if (grid_[index] == 0) {
                grid_[index] = 1;
                ++visitedCount_;
            }
        }
    }
    state.visited.clear();
    state_ = std::move(state);
    return true;
}

TravelState TravelSystem::state() const {
    TravelState out = state_;
    out.visited.clear();
    for (size_t i = 0; i < grid_.size(); ++i) {
        if (grid_[i] != 0) {
            out.visited.push_back(static_cast<uint32_t>(i));
        }
    }
    return out;
}

void TravelSystem::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(state_.kind));
    h.add(state_.startedAt);
    h.add(state_.endsAt);
    h.add(static_cast<uint64_t>(entt::to_integral(state_.pawn)));
    h.add(state_.destX);
    h.add(state_.destY);
    h.add(state_.nights);
    h.add(state_.travels);
    h.add(state_.skipped);
    h.add(static_cast<uint64_t>(visitedCount_));
    for (size_t i = 0; i < grid_.size(); ++i) {
        if (grid_[i] != 0) {
            h.add(static_cast<uint64_t>(i));
        }
    }
}

}  // namespace colony

#include "ColonySim/MentalBreaks.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// Walks given up this many times in a row end the walk (the same limit as the job system's).
constexpr uint8_t kMentalMaxRepaths = 5;
// How far from a colonist an insulting spree looks for the next one to insult, in metres.
constexpr float kMentalInsultRadius = 60.0f;
// How many targets a search tries to reach before giving up.
constexpr size_t kMentalReachTries = 24;

float mentalDistSq(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

float mentalCellDistSq(CellPos a, CellPos b) {
    return mentalDistSq(static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(b.x), static_cast<float>(b.y));
}

// Metres from a point to a rectangle of cells (0 inside it).
float mentalDistanceToRect(float x, float y, const CellRect& rect) {
    const float dx = std::max({static_cast<float>(rect.minX) - x, 0.0f, x - static_cast<float>(rect.minX + rect.sizeX)});
    const float dy = std::max({static_cast<float>(rect.minY) - y, 0.0f, y - static_cast<float>(rect.minY + rect.sizeY)});
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

const char* breakStateName(BreakState state) {
    switch (state) {
        case BreakState::Wander: return "Wander";
        case BreakState::Binge: return "Binge";
        case BreakState::HideInRoom: return "HideInRoom";
        case BreakState::Tantrum: return "Tantrum";
        case BreakState::InsultSpree: return "InsultSpree";
        case BreakState::Berserk: return "Berserk";
        case BreakState::LeaveColony: return "LeaveColony";
        case BreakState::FireSpree: return "FireSpree";
    }
    return "?";
}

bool breakStateFromName(const std::string& name, BreakState& out) {
    for (const BreakState state : {BreakState::Wander, BreakState::Binge, BreakState::HideInRoom, BreakState::Tantrum,
                                   BreakState::InsultSpree, BreakState::Berserk, BreakState::LeaveColony,
                                   BreakState::FireSpree}) {
        if (name == breakStateName(state)) {
            out = state;
            return true;
        }
    }
    return false;
}

MentalBreakRules MentalBreakRules::read(const DefDatabase& defs) {
    MentalBreakRules r;
    if (const Def* when = defs.find("Rules", "MentalBreakRules")) {
        r.present = true;
        r.meanDays[0] = when->number("meanDaysBetweenBreaks.Minor", r.meanDays[0]);
        r.meanDays[1] = when->number("meanDaysBetweenBreaks.Major", r.meanDays[1]);
        r.meanDays[2] = when->number("meanDaysBetweenBreaks.Extreme", r.meanDays[2]);
        const size_t points = when->count("moodCurve");
        if (points >= 2) {
            r.curve.clear();
            for (size_t i = 0; i < points; ++i) {
                const std::string path = "moodCurve." + std::to_string(i);
                r.curve.emplace_back(static_cast<float>(when->numberAt(path.c_str(), 0)),
                                     static_cast<float>(when->numberAt(path.c_str(), 1)));
            }
        }
        r.minSecondsBetween = static_cast<int64_t>(std::llround(when->number("minHoursBetweenBreaks", 24.0) * 3600.0));
        r.recoveryMargin = static_cast<float>(when->number("recoveryMoodMargin", r.recoveryMargin));
        r.restOnlyWhenAwake = when->flag("restOnlyWhenAwake", r.restOnlyWhenAwake);
    }
    if (const Def* how = defs.find("Rules", "MentalBreakBehaviour")) {
        const auto whole = [&](const char* path, int fallback) { return static_cast<int>(how->integer(path, fallback)); };
        r.checkEvery = std::max(1, whole("checkEverySeconds", r.checkEvery));
        r.wanderRadius = whole("wanderRadius", r.wanderRadius);
        r.wanderWaitMin = static_cast<int>(how->numberAt("wanderWaitSeconds", 0, r.wanderWaitMin));
        r.wanderWaitMax = std::max(r.wanderWaitMin, static_cast<int>(how->numberAt("wanderWaitSeconds", 1, r.wanderWaitMax)));
        r.hideRadius = whole("hideRadius", r.hideRadius);
        r.smashRadius = whole("smashRadius", r.smashRadius);
        r.berserkRadius = whole("berserkRadius", r.berserkRadius);
        r.reach = static_cast<float>(how->number("reachMetres", r.reach));
        r.talkReach = static_cast<float>(how->number("talkReachMetres", r.talkReach));
        r.fireRadius = whole("fireRadius", r.fireRadius);
        r.fireEvery = std::max(1, whole("fireEverySeconds", r.fireEvery));
        r.punchEvery = std::max(1, whole("punchEverySeconds", r.punchEvery));
        r.punchDamage = static_cast<float>(how->number("punchDamage", r.punchDamage));
        r.punchDamageType = how->text("punchDamageType", r.punchDamageType);
        r.smashDamage = std::max(1, whole("smashDamage", r.smashDamage));
        r.tantrumMaxSmashes = std::max(1, whole("tantrumMaxSmashes", r.tantrumMaxSmashes));
        r.bingeMeals = std::max(1, whole("bingeMeals", r.bingeMeals));
        r.eatSeconds = std::max(1, whole("eatSeconds", r.eatSeconds));
        r.repathEvery = std::max(1, whole("repathEverySeconds", r.repathEvery));
        r.searchEvery = std::max(1, whole("targetSearchEverySeconds", r.searchEvery));
        r.hardEndFactor = how->number("hardEndFactor", r.hardEndFactor);
        r.hardEndMinHours = how->number("hardEndMinHours", r.hardEndMinHours);
    }
    return r;
}

MentalBreakSystem::MentalBreakSystem(Things& things, Map& map, HealthSystem& health, FighterSystem& fighters,
                                     JobSystem& jobs, MoodSystem& mood, Rooms& rooms, Pathfinder& pathfinder,
                                     const DefDatabase& defs, const GameTime& time)
    : things_(things),
      map_(map),
      health_(health),
      fighters_(fighters),
      jobs_(jobs),
      mood_(mood),
      rooms_(rooms),
      pathfinder_(pathfinder),
      defs_(defs),
      time_(time),
      rules_(MentalBreakRules::read(defs)) {
    for (const DefId id : defs.idsOfKind("MentalBreaks")) {
        const Def& def = defs.get(id);
        Kind kind;
        kind.def = id;
        kind.name = def.defName();
        const std::string band = def.text("band");
        kind.band = band == "Minor" ? 0 : band == "Major" ? 1 : band == "Extreme" ? 2 : -1;
        kind.commonality = def.number("baseCommonality", 1.0);
        kind.hoursMin = def.numberAt("durationHours", 0, 0.0);
        kind.hoursMax = std::max(kind.hoursMin, def.numberAt("durationHours", 1, kind.hoursMin));
        kind.count = static_cast<int>(def.integer("count", 0));
        for (size_t i = 0; i < def.count("requires"); ++i) {
            kind.needs.push_back(def.textAt("requires", i));
        }
        if (kind.band < 0 || !breakStateFromName(def.text("state"), kind.state)) {
            continue;  // a break the sim can't act out yet
        }
        kinds_.push_back(std::move(kind));
    }
    // Defs are walked in id order, which is the load order: not something a save may depend on, but
    // the weighted pick below is made over this list, so sort it by name to be independent of it.
    std::sort(kinds_.begin(), kinds_.end(), [](const Kind& a, const Kind& b) { return a.name < b.name; });
    nutrition_.assign(defs.size(), 0.0f);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.has("ingestible")) {
            nutrition_[id.value] = static_cast<float>(def.number("stats.nutrition", 0.0));
        }
    }
}

void MentalBreakSystem::setFire(Fire* fire) {
    fire_ = fire;
    if (fire != nullptr) {
        provide("fire");
    }
}

void MentalBreakSystem::provide(const std::string& feature) {
    if (std::find(features_.begin(), features_.end(), feature) == features_.end()) {
        features_.push_back(feature);
    }
}

const MentalBreakSystem::Kind* MentalBreakSystem::kindNamed(const std::string& name) const {
    for (const Kind& kind : kinds_) {
        if (kind.name == name) {
            return &kind;
        }
    }
    return nullptr;
}

bool MentalBreakSystem::provided(const Kind& kind) const {
    for (const std::string& need : kind.needs) {
        if (std::find(features_.begin(), features_.end(), need) == features_.end()) {
            return false;
        }
    }
    return true;
}

bool MentalBreakSystem::active(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const MentalBreaker* b = reg.valid(pawn) ? reg.try_get<MentalBreaker>(pawn) : nullptr;
    return b != nullptr && b->active;
}

DefId MentalBreakSystem::breakOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const MentalBreaker* b = reg.valid(pawn) ? reg.try_get<MentalBreaker>(pawn) : nullptr;
    return b != nullptr && b->active ? defs_.idOf("MentalBreaks", b->def) : DefId{};
}

// ---- Starting ---------------------------------------------------------------------------------

bool MentalBreakSystem::eligible(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<Thing, Position, Mover, Worker>(pawn) && !reg.all_of<PlayerControl>(pawn) &&
           fighters_.factionOf(pawn) == kPlayerFaction && !health_.incapacitated(pawn);
}

bool MentalBreakSystem::feasible(Entity pawn, const Kind& kind, Rng& rng) {
    const MentalBreaker none;
    switch (kind.state) {
        case BreakState::Wander:
        case BreakState::Berserk:
            return true;
        case BreakState::FireSpree: {
            CellPos cell, stand;
            return fireGoal(pawn, rng, cell, stand);
        }
        case BreakState::Binge: return findFood(pawn, none) != kNoEntity;
        case BreakState::HideInRoom: {
            CellPos spot;
            return hideSpot(pawn, spot);
        }
        case BreakState::Tantrum: return findSmashTarget(pawn, none) != kNoEntity;
        case BreakState::InsultSpree: return findPawnTarget(pawn, none, kMentalInsultRadius, true) != kNoEntity;
        case BreakState::LeaveColony: {
            CellPos edge;
            return edgeGoal(pawn, edge);
        }
    }
    return false;
}

// The chance of a break is RimWorld's: a pawn under a threshold gets a roll every few minutes of
// game time, with a mean wait per band that shrinks as the mood sinks below the threshold.
void MentalBreakSystem::natural(Entity pawn, int64_t now, Rng& rng) {
    entt::registry& reg = things_.registry();
    const Mood& mood = reg.get<Mood>(pawn);
    if (mood.band == MoodBand::Stable || (now + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.checkEvery != 0 ||
        !eligible(pawn)) {
        return;
    }
    const MentalBreaker* history = reg.try_get<MentalBreaker>(pawn);
    if (history != nullptr && history->lastEnded >= 0 && now - history->lastEnded < rules_.minSecondsBetween) {
        return;
    }
    if (rules_.restOnlyWhenAwake) {
        const Worker& worker = reg.get<Worker>(pawn);
        if (jobIsLyingDown(worker.job)) {
            return;
        }
    }
    float levels[3] = {0.0f, 0.0f, 0.0f};
    mood_.breakLevels(pawn, levels[0], levels[1], levels[2]);
    const int band = static_cast<int>(mood.band) - 1;
    const float threshold = levels[band];
    const float ratio = threshold > 0.0f ? std::clamp(mood.level / threshold, 0.0f, 1.0f) : 0.0f;
    float factor = rules_.curve.back().second;
    for (size_t i = 1; i < rules_.curve.size(); ++i) {
        if (ratio <= rules_.curve[i].first) {
            const auto& [x0, y0] = rules_.curve[i - 1];
            const auto& [x1, y1] = rules_.curve[i];
            factor = x1 > x0 ? y0 + (y1 - y0) * (ratio - x0) / (x1 - x0) : y1;
            break;
        }
    }
    const double meanSeconds = rules_.meanDays[band] * static_cast<double>(kSecondsPerDay) * static_cast<double>(factor);
    // Plain arithmetic, no exp(): the same on every platform.
    const double chance = meanSeconds > 0.0 ? std::min(1.0, static_cast<double>(rules_.checkEvery) / meanSeconds) : 1.0;
    if (static_cast<double>(rng.below(1000000)) >= chance * 1000000.0) {
        return;
    }
    // Pick one of the band's breaks that can happen now, by commonality.
    std::vector<const Kind*> options;
    double total = 0.0;
    // A pyromaniac's only extreme break is the fire-starting spree (docs/design/traits.md).
    const bool pyromaniac = band == 2 && mood_.hasTrait(pawn, "Pyromaniac");
    for (const Kind& kind : kinds_) {
        if (pyromaniac && kind.state != BreakState::FireSpree) {
            continue;
        }
        if (kind.band == band && provided(kind) && kind.commonality > 0.0 && feasible(pawn, kind, rng)) {
            options.push_back(&kind);
            total += kind.commonality;
        }
    }
    if (options.empty()) {
        return;
    }
    double pick = static_cast<double>(rng.below(1000000)) / 1000000.0 * total;
    const Kind* chosen = options.back();
    for (const Kind* kind : options) {
        if (pick < kind->commonality) {
            chosen = kind;
            break;
        }
        pick -= kind->commonality;
    }
    begin(pawn, *chosen, band, now, rng);
}

bool MentalBreakSystem::start(Entity pawn, const std::string& breakDef, Rng& rng, std::string& error) {
    const Kind* kind = kindNamed(breakDef);
    if (kind == nullptr) {
        error = "no MentalBreaks def '" + breakDef + "' the sim can act out";
        return false;
    }
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing, Position, Mover, Worker>(pawn)) {
        error = "that isn't a colonist on the map";
        return false;
    }
    if (reg.all_of<PlayerControl>(pawn)) {
        error = "the player is walking that colonist";
        return false;
    }
    if (fighters_.factionOf(pawn) != kPlayerFaction) {
        error = "only the player's own colonists have mental breaks";
        return false;
    }
    if (health_.incapacitated(pawn)) {
        error = "that colonist is downed or dead";
        return false;
    }
    if (active(pawn)) {
        error = "that colonist is in a mental break already";
        return false;
    }
    if (!provided(*kind)) {
        error = "'" + breakDef + "' needs " + kind->needs.front() + ", which the game doesn't have yet";
        return false;
    }
    if (!feasible(pawn, *kind, rng)) {
        error = "there is nothing for the colonist to do in a '" + breakDef + "' break";
        return false;
    }
    begin(pawn, *kind, kind->band, time_.seconds, rng);
    return true;
}

bool MentalBreakSystem::end(Entity pawn, std::string& error) {
    entt::registry& reg = things_.registry();
    MentalBreaker* b = reg.valid(pawn) ? reg.try_get<MentalBreaker>(pawn) : nullptr;
    if (b == nullptr || !b->active) {
        error = "that colonist isn't in a mental break";
        return false;
    }
    finish(pawn, *b, time_.seconds, "stopped");
    return true;
}

void MentalBreakSystem::begin(Entity pawn, const Kind& kind, int band, int64_t now, Rng& rng) {
    entt::registry& reg = things_.registry();
    jobs_.interrupt(pawn, "mental break");
    if (const Fighter* fighter = reg.try_get<Fighter>(pawn); fighter != nullptr && fighter->drafted) {
        std::string ignored;
        fighters_.draft(pawn, false, ignored);
    }
    MentalBreaker& b = reg.get_or_emplace<MentalBreaker>(pawn);
    stopWalk(pawn, b);
    b.active = true;
    b.def = kind.name;
    b.state = kind.state;
    b.startedAt = now;
    const double u = static_cast<double>(rng.below(10001)) / 10000.0;
    b.endsAfter = now + static_cast<int64_t>(std::llround((kind.hoursMin + (kind.hoursMax - kind.hoursMin) * u) * 3600.0));
    b.hardEnd = now + static_cast<int64_t>(std::llround(std::max(kind.hoursMax * rules_.hardEndFactor, rules_.hardEndMinHours) * 3600.0));
    float levels[3] = {0.0f, 0.0f, 0.0f};
    mood_.breakLevels(pawn, levels[0], levels[1], levels[2]);
    const Mood* mood = reg.try_get<Mood>(pawn);
    b.recoverLevel = mood != nullptr ? std::min(100.0f, levels[band] + rules_.recoveryMargin * 100.0f) : 0.0f;
    b.hasGoal = false;
    b.target = kNoEntity;
    b.done = 0;
    b.nextAct = now;
    b.nextHit = 0;
    b.visited.clear();
    emit(EventType::PawnMentalBreak, pawn, kind.name, mood != nullptr ? mood->level : 0.0f);
}

void MentalBreakSystem::finish(Entity pawn, MentalBreaker& b, int64_t now, const char* reason) {
    stopWalk(pawn, b);
    b.active = false;
    b.lastEnded = now;
    b.hasGoal = false;
    b.target = kNoEntity;
    b.visited.clear();
    emit(EventType::PawnMentalBreakEnded, pawn, reason, static_cast<float>(now - b.startedAt));
}

// ---- Each step ----------------------------------------------------------------------------------

void MentalBreakSystem::step(int64_t now, Rng& rng) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> acting;
    for (const Entity pawn : reg.view<MentalBreaker>()) {
        if (reg.get<MentalBreaker>(pawn).active) {
            acting.push_back(pawn);
        }
    }
    std::sort(acting.begin(), acting.end());
    for (const Entity pawn : acting) {
        if (reg.valid(pawn)) {  // an earlier pawn's break can't destroy another, but stay safe
            tick(pawn, reg.get<MentalBreaker>(pawn), now, rng);
        }
    }
    if (!natural_ || !rules_.present || kinds_.empty()) {
        return;
    }
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<Mood>()) {
        const MentalBreaker* b = reg.try_get<MentalBreaker>(pawn);
        if (b == nullptr || !b->active) {
            pawns.push_back(pawn);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    for (const Entity pawn : pawns) {
        natural(pawn, now, rng);
    }
}

void MentalBreakSystem::tick(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng) {
    entt::registry& reg = things_.registry();
    const Health* health = reg.try_get<Health>(pawn);
    if (health != nullptr && health->dead) {
        finish(pawn, b, now, "died");
        return;
    }
    if (health_.incapacitated(pawn)) {
        finish(pawn, b, now, "downed");
        return;
    }
    if (!things_.spawned(pawn)) {
        finish(pawn, b, now, "stopped");
        return;
    }
    // A pawn that is on fire runs about, whatever its break was doing.
    if (fire_ != nullptr && reg.all_of<Burning>(pawn) && fire_->panicStep(pawn, now)) {
        stopWalk(pawn, b);
        b.target = kNoEntity;
    } else switch (b.state) {
        case BreakState::Wander: tickWander(pawn, b, now, rng); break;
        case BreakState::HideInRoom: tickHide(pawn, b, now, rng); break;
        case BreakState::Binge: tickBinge(pawn, b, now, rng); break;
        case BreakState::Tantrum: tickAttack(pawn, b, now, rng, false); break;
        case BreakState::Berserk: tickAttack(pawn, b, now, rng, true); break;
        case BreakState::InsultSpree: tickInsult(pawn, b, now, rng); break;
        case BreakState::LeaveColony: tickLeave(pawn, b, now, rng); break;
        case BreakState::FireSpree: tickFireSpree(pawn, b, now, rng); break;
    }
    if (!reg.valid(pawn) || !b.active) {
        return;  // (a pawn that left the colony has no break component to look at)
    }
    // Wanders, hiding, tantrums, berserk and the like last their time and then until the mood is
    // back. Binges, sprees and giving up end themselves.
    const bool endsItself = b.state == BreakState::Binge || b.state == BreakState::InsultSpree || b.state == BreakState::LeaveColony;
    const Mood* mood = reg.try_get<Mood>(pawn);
    const bool calm = mood == nullptr || mood->level >= b.recoverLevel;
    if (now >= b.hardEnd || (!endsItself && now >= b.endsAfter && calm)) {
        finish(pawn, b, now, "recovered");
    }
}

// ---- Walking ------------------------------------------------------------------------------------

void MentalBreakSystem::beginWalk(Entity pawn, MentalBreaker& b, CellPos goal) {
    if (b.walk == 1) {
        pathfinder_.cancel(b.path);
    }
    b.goal = goal;
    b.hasGoal = true;
    b.path = pathfinder_.request(cellOf(things_.registry().get<Position>(pawn)), goal);
    b.walk = 1;
    b.repaths = 0;
}

MentalBreakSystem::Walk MentalBreakSystem::walkStep(Entity pawn, MentalBreaker& b) {
    entt::registry& reg = things_.registry();
    Mover& mover = reg.get<Mover>(pawn);
    const Position& pos = reg.get<Position>(pawn);
    if (b.walk == 1) {
        if (pathfinder_.status(b.path) == PathStatus::Queued) {
            return Walk::Going;
        }
        std::vector<CellPos> route;
        const bool found = pathfinder_.takePath(b.path, route);
        b.path = PathTicket{};
        if (!found) {
            b.walk = 0;
            return Walk::Failed;
        }
        smoothPath(map_, pos.x, pos.y, route);
        mover.walkTo(pos.x, pos.y, std::move(route));
        b.walk = 2;
        return Walk::Going;
    }
    if (b.walk == 2) {
        if (mover.gaveUp) {
            mover.gaveUp = false;
            if (b.repaths >= kMentalMaxRepaths) {
                b.walk = 0;
                return Walk::Failed;
            }
            ++b.repaths;
            b.path = pathfinder_.request(cellOf(pos), b.goal);
            b.walk = 1;
            return Walk::Going;
        }
        if (mover.arrived()) {
            b.walk = 0;
            return Walk::Arrived;
        }
        return Walk::Going;
    }
    return Walk::None;
}

void MentalBreakSystem::stopWalk(Entity pawn, MentalBreaker& b) {
    if (b.walk == 1) {
        pathfinder_.cancel(b.path);
    }
    b.path = PathTicket{};
    b.walk = 0;
    if (Mover* mover = things_.registry().valid(pawn) ? things_.registry().try_get<Mover>(pawn) : nullptr) {
        mover->path.clear();
        mover->next = 0;
        mover->gaveUp = false;
    }
}

// ---- Sad wander, hiding ---------------------------------------------------------------------------

bool MentalBreakSystem::wanderGoal(Entity pawn, Rng& rng, CellPos& out) const {
    const CellPos here = cellOf(things_.registry().get<Position>(pawn));
    const uint32_t span = static_cast<uint32_t>(rules_.wanderRadius * 2 + 1);
    for (int tries = 0; tries < 12; ++tries) {
        const int dx = static_cast<int>(rng.below(span)) - rules_.wanderRadius;
        const int dy = static_cast<int>(rng.below(span)) - rules_.wanderRadius;
        const CellPos c{here.x + dx, here.y + dy, here.level};
        if ((dx != 0 || dy != 0) && map_.walkable(c) && jobs_.canReach(here, c)) {
            out = c;
            return true;
        }
    }
    return false;
}

void MentalBreakSystem::tickWander(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng) {
    if (b.walk != 0) {
        const Walk w = walkStep(pawn, b);
        if (w == Walk::Arrived) {
            b.hasGoal = false;
            b.nextAct = now + rules_.wanderWaitMin +
                        static_cast<int64_t>(rng.below(static_cast<uint32_t>(rules_.wanderWaitMax - rules_.wanderWaitMin + 1)));
        } else if (w == Walk::Failed) {
            b.hasGoal = false;
            b.nextAct = now + rules_.searchEvery;
        }
        return;
    }
    if (now < b.nextAct) {
        return;
    }
    CellPos goal;
    if (wanderGoal(pawn, rng, goal)) {
        beginWalk(pawn, b, goal);
    } else {
        b.nextAct = now + rules_.searchEvery;
    }
}

// Its own bed if it has one, else the nearest cell under a roof it can reach.
bool MentalBreakSystem::hideSpot(Entity pawn, CellPos& out) {
    entt::registry& reg = things_.registry();
    const CellPos here = cellOf(reg.get<Position>(pawn));
    for (const auto [bed, owners, footprint] : reg.view<const Bed, const Footprint>().each()) {
        if (std::find(owners.owners.begin(), owners.owners.end(), pawn) == owners.owners.end()) {
            continue;
        }
        const CellRect rect = footprint.rect();
        const CellPos cell{rect.minX, rect.minY, rect.level};
        if (map_.walkable(cell) && jobs_.canReach(here, cell)) {
            out = cell;
            return true;
        }
    }
    bool found = false;
    float best = 0.0f;
    const int r = rules_.hideRadius;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const CellPos c{here.x + dx, here.y + dy, here.level};
            const float d = static_cast<float>(dx * dx + dy * dy);
            if ((found && d >= best) || d > static_cast<float>(r * r) || !map_.walkable(c) || !rooms_.indoors(c) ||
                !jobs_.canReach(here, c)) {
                continue;
            }
            found = true;
            best = d;
            out = c;
        }
    }
    return found;
}

void MentalBreakSystem::tickHide(Entity pawn, MentalBreaker& b, int64_t now, Rng&) {
    if (b.walk != 0) {
        if (walkStep(pawn, b) == Walk::Failed) {
            b.hasGoal = false;
            b.nextAct = now + rules_.searchEvery;
        }
        return;
    }
    if (b.hasGoal || now < b.nextAct) {
        return;  // there; it stays
    }
    CellPos spot;
    if (hideSpot(pawn, spot)) {
        beginWalk(pawn, b, spot);
    } else {
        b.nextAct = now + rules_.searchEvery;
    }
}

// ---- Food binge -------------------------------------------------------------------------------------

Entity MentalBreakSystem::findFood(Entity pawn, const MentalBreaker& b) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos here = cellOf(at);
    Entity best = kNoEntity;
    float bestDist = 0.0f;
    for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        (void)stack;
        if (thing.category != ThingCategory::Item || thing.def.value >= nutrition_.size() || nutrition_[thing.def.value] <= 0.0f ||
            pos.level != at.level || std::find(b.visited.begin(), b.visited.end(), item) != b.visited.end()) {
            continue;
        }
        const float d = mentalDistSq(at.x, at.y, pos.x, pos.y);
        if (best != kNoEntity && (d > bestDist || (d == bestDist && item > best))) {
            continue;
        }
        if (jobs_.reservedBy(JobTarget{item, CellPos{}}) != kNoEntity || !jobs_.canReach(here, cellOf(pos))) {
            continue;
        }
        best = item;
        bestDist = d;
    }
    return best;
}

void MentalBreakSystem::tickBinge(Entity pawn, MentalBreaker& b, int64_t now, Rng&) {
    entt::registry& reg = things_.registry();
    if (b.done >= rules_.bingeMeals) {
        finish(pawn, b, now, "recovered");
        return;
    }
    if (b.target != kNoEntity && !(things_.spawned(b.target) && reg.all_of<StackCount>(b.target))) {
        b.target = kNoEntity;  // someone took it
        b.nextHit = 0;
        stopWalk(pawn, b);
    }
    if (b.target == kNoEntity) {
        if (now < b.nextAct) {
            return;
        }
        const Entity food = findFood(pawn, b);
        if (food == kNoEntity) {
            finish(pawn, b, now, "recovered");  // nothing left to eat
            return;
        }
        b.target = food;
        b.nextHit = 0;
        beginWalk(pawn, b, cellOf(reg.get<Position>(food)));
        return;
    }
    const CellPos foodCell = cellOf(reg.get<Position>(b.target));
    if (cellOf(reg.get<Position>(pawn)) != foodCell) {
        if (b.walk != 0) {
            if (walkStep(pawn, b) == Walk::Failed) {
                b.visited.push_back(b.target);
                b.target = kNoEntity;
            }
        } else {
            beginWalk(pawn, b, foodCell);  // nudged off its way
        }
        return;
    }
    if (b.walk != 0) {
        stopWalk(pawn, b);
    }
    if (b.nextHit == 0) {
        b.nextHit = now + rules_.eatSeconds;
        action(pawn, b.target, PawnAction::Eat);
        return;
    }
    if (now < b.nextHit) {
        return;
    }
    // One helping, past full if need be: the stomach only holds so much, the rest is wasted.
    const Entity food = b.target;
    const DefId def = reg.get<Thing>(food).def;
    StackCount& stack = reg.get<StackCount>(food);
    if (stack.count > 1) {
        --stack.count;
    } else {
        jobs_.forget(food);
        things_.destroy(food);
    }
    float gained = 0.0f;
    if (Needs* needs = reg.try_get<Needs>(pawn)) {
        const float before = needs->food;
        needs->food = std::min(1.0f, needs->food + nutrition_[def.value]);
        gained = needs->food - before;
    }
    if (events_ != nullptr) {
        Event event;
        event.type = EventType::PawnAte;
        event.thing = pawn;
        event.def = def;
        event.cell = foodCell;
        event.amount = gained;
        event.message = "binge";
        events_->push_back(std::move(event));
    }
    ++b.done;
    b.target = kNoEntity;
    b.nextHit = 0;
    b.nextAct = now;
}

// ---- Tantrum and berserk --------------------------------------------------------------------------

float MentalBreakSystem::distanceTo(Entity pawn, Entity target) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const Position& there = reg.get<Position>(target);
    if (const Footprint* footprint = reg.try_get<Footprint>(target)) {
        return mentalDistanceToRect(at.x, at.y, footprint->rect());
    }
    if (reg.all_of<Mover>(target)) {
        return std::sqrt(mentalDistSq(at.x, at.y, there.x, there.y));
    }
    return mentalDistanceToRect(at.x, at.y, CellRect{static_cast<int>(std::floor(there.x)), static_cast<int>(std::floor(there.y)), 1, 1, there.level});
}

namespace {

// A cell to stand in to reach `target`: the nearest walkable, reachable one within a cell of it.
bool mentalStandCell(const Map& map, const JobSystem& jobs, const entt::registry& reg, Entity pawn, Entity target, CellPos& out) {
    const CellPos here = cellOf(reg.get<Position>(pawn));
    CellRect rect;
    if (const Footprint* footprint = reg.try_get<Footprint>(target)) {
        rect = footprint->rect();
    } else {
        const CellPos c = cellOf(reg.get<Position>(target));
        rect = CellRect{c.x, c.y, 1, 1, c.level};
    }
    bool found = false;
    float best = 0.0f;
    for (int y = rect.minY - 1; y <= rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX - 1; x <= rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            const float d = mentalCellDistSq(here, c);
            if ((found && d >= best) || !map.walkable(c) || !jobs.canReach(here, c)) {
                continue;
            }
            found = true;
            best = d;
            out = c;
        }
    }
    return found;
}

}  // namespace

Entity MentalBreakSystem::findSmashTarget(Entity pawn, const MentalBreaker& b) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const float radiusSq = static_cast<float>(rules_.smashRadius * rules_.smashRadius);
    std::vector<std::pair<float, Entity>> near;
    for (const auto [thing, def, pos, hp] : reg.view<const Thing, const Position, const HitPoints>().each()) {
        if (def.category == ThingCategory::Pawn || hp.current <= 0 || pos.level != at.level || reg.all_of<ConstructionSite>(thing) ||
            std::find(b.visited.begin(), b.visited.end(), thing) != b.visited.end()) {
            continue;
        }
        // Walls and doors hold the roof up: a tantrum is about the furniture.
        if (def.category == ThingCategory::Building && defs_.get(def.def).flag("building.holdsRoof")) {
            continue;
        }
        const float d = mentalDistSq(at.x, at.y, pos.x, pos.y);
        if (d <= radiusSq) {
            near.emplace_back(d, thing);
        }
    }
    std::sort(near.begin(), near.end());
    for (size_t i = 0; i < near.size() && i < kMentalReachTries; ++i) {
        CellPos stand;
        if (mentalStandCell(map_, jobs_, reg, pawn, near[i].second, stand)) {
            return near[i].second;
        }
    }
    return kNoEntity;
}

Entity MentalBreakSystem::findPawnTarget(Entity pawn, const MentalBreaker& b, float radius, bool colonistsOnly) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos here = cellOf(at);
    std::vector<std::pair<float, Entity>> near;
    for (const auto [other, thing, pos, health] : reg.view<const Thing, const Position, const Health>().each()) {
        if (other == pawn || thing.category != ThingCategory::Pawn || health.incapacitated() || pos.level != at.level ||
            (colonistsOnly && !reg.all_of<Colonist>(other)) ||
            std::find(b.visited.begin(), b.visited.end(), other) != b.visited.end()) {
            continue;
        }
        const float d = mentalDistSq(at.x, at.y, pos.x, pos.y);
        if (d <= radius * radius) {
            near.emplace_back(d, other);
        }
    }
    std::sort(near.begin(), near.end());
    for (size_t i = 0; i < near.size() && i < kMentalReachTries; ++i) {
        if (jobs_.canReach(here, cellOf(reg.get<Position>(near[i].second)))) {
            return near[i].second;
        }
    }
    return kNoEntity;
}

// Walks to within `reach` of `target`, planning again as it moves (a pawn walks off). True once close
// enough. A target it can't get to is put on the visited list and dropped.
bool MentalBreakSystem::approach(Entity pawn, MentalBreaker& b, Entity target, float reach, int64_t now) {
    entt::registry& reg = things_.registry();
    if (distanceTo(pawn, target) <= reach) {
        if (b.walk != 0) {
            stopWalk(pawn, b);
        }
        return true;
    }
    const bool moves = reg.all_of<Mover>(target);
    if (b.walk != 0) {
        if (walkStep(pawn, b) == Walk::Failed) {
            b.visited.push_back(target);
            b.target = kNoEntity;
            b.nextAct = now + rules_.searchEvery;
            return false;
        }
        if (!moves || now < b.nextAct) {
            return false;
        }
    }
    CellPos stand;
    if (!mentalStandCell(map_, jobs_, reg, pawn, target, stand)) {
        b.visited.push_back(target);
        b.target = kNoEntity;
        b.nextAct = now + rules_.searchEvery;
        return false;
    }
    beginWalk(pawn, b, stand);
    b.nextAct = now + rules_.repathEvery;
    return false;
}

void MentalBreakSystem::tickAttack(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng, bool pawnsFirst) {
    entt::registry& reg = things_.registry();
    if (b.target != kNoEntity) {
        const bool pawnTarget = reg.valid(b.target) && reg.all_of<Mover>(b.target);
        const HitPoints* hp = reg.valid(b.target) ? reg.try_get<HitPoints>(b.target) : nullptr;
        const bool alive = things_.spawned(b.target) &&
                           (pawnTarget ? !health_.incapacitated(b.target) : (hp != nullptr && hp->current > 0));
        if (!alive) {
            stopWalk(pawn, b);
            b.target = kNoEntity;
        }
    }
    if (b.target == kNoEntity) {
        if (now < b.nextAct) {
            return;
        }
        Entity target = pawnsFirst ? findPawnTarget(pawn, b, static_cast<float>(rules_.berserkRadius), false) : kNoEntity;
        if (target == kNoEntity) {
            target = findSmashTarget(pawn, b);
        }
        if (target == kNoEntity) {
            b.nextAct = now + rules_.searchEvery;
            return;
        }
        b.target = target;
        b.nextAct = now;
    }
    if (melee_ != nullptr && reg.all_of<Mover, Posture>(b.target)) {
        if (!chase(pawn, b, b.target, now)) {
            return;
        }
        std::string why;
        if (melee_->ready(pawn)) {
            melee_->attack(pawn, b.target, false, &why);
        }
        return;
    }
    if (!approach(pawn, b, b.target, rules_.reach, now) || now < b.nextHit) {
        return;
    }
    b.nextHit = now + rules_.punchEvery;
    const Entity target = b.target;
    action(pawn, target, PawnAction::Punch);
    if (reg.all_of<Mover>(target)) {
        const DefId type = health_.damageTypeId(rules_.punchDamageType);
        if (type.valid()) {
            health_.applyDamage(target, type, rules_.punchDamage, -1, rng);
        }
        return;
    }
    HitPoints& hp = reg.get<HitPoints>(target);
    hp.current -= rules_.smashDamage;
    if (hp.current <= 0) {
        jobs_.forget(target);
        things_.destroy(target);
        b.target = kNoEntity;
        ++b.done;
        if (b.state == BreakState::Tantrum && b.done >= rules_.tantrumMaxSmashes) {
            finish(pawn, b, now, "recovered");
        }
    }
}

// Walks at a pawn it means to hit, planning again as it moves, until a swing would reach it. Holds still
// once there, so the wind-up isn't spent walking. A target it can't get to is dropped.
bool MentalBreakSystem::chase(Entity pawn, MentalBreaker& b, Entity target, int64_t now) {
    entt::registry& reg = things_.registry();
    if (melee_->inReach(pawn, target)) {
        if (b.walk != 0) {
            stopWalk(pawn, b);
        }
        return true;
    }
    if (melee_->winding(pawn)) {
        return false;  // mid-swing: let it finish
    }
    if (b.walk != 0) {
        if (walkStep(pawn, b) == Walk::Failed) {
            b.visited.push_back(target);
            b.target = kNoEntity;
            b.nextAct = now + rules_.searchEvery;
            return false;
        }
        if (now < b.nextAct) {
            return false;
        }
    }
    beginWalk(pawn, b, cellOf(reg.get<Position>(target)));
    b.nextAct = now + rules_.repathEvery;
    return false;
}

// ---- Fire-starting spree --------------------------------------------------------------------------------

// Something near that would burn and isn't burning yet, picked among the nearest few (so a cell that
// won't take a flame, wet ground say, is not tried for ever), and a cell to light it from.
bool MentalBreakSystem::fireGoal(Entity pawn, Rng& rng, CellPos& cell, CellPos& stand) const {
    if (fire_ == nullptr) {
        return false;
    }
    const entt::registry& reg = things_.registry();
    const CellPos here = cellOf(reg.get<Position>(pawn));
    std::vector<std::pair<float, CellPos>> near;
    for (int dy = -rules_.fireRadius; dy <= rules_.fireRadius; ++dy) {
        for (int dx = -rules_.fireRadius; dx <= rules_.fireRadius; ++dx) {
            const CellPos c{here.x + dx, here.y + dy, here.level};
            const float d = mentalCellDistSq(here, c);
            if (d > static_cast<float>(rules_.fireRadius * rules_.fireRadius) || !map_.inBounds(c) || fire_->burning(c) ||
                fire_->flammabilityAt(c) <= 0.0f) {
                continue;
            }
            near.emplace_back(d, c);
        }
    }
    std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : (a.second.y != b.second.y ? a.second.y < b.second.y : a.second.x < b.second.x);
    });
    std::vector<CellPos> options, stands;
    for (size_t i = 0; i < near.size() && options.size() < 5 && i < kMentalReachTries * 4; ++i) {
        // Stand in the cell itself, else in a walkable one beside it.
        const CellPos c = near[i].second;
        CellPos best;
        bool found = false;
        float bestD = 0.0f;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const CellPos s{c.x + dx, c.y + dy, c.level};
                const float d = mentalCellDistSq(here, s);
                if ((found && d >= bestD) || !map_.inBounds(s) || !map_.walkable(s) || !jobs_.canReach(here, s)) {
                    continue;
                }
                found = true;
                bestD = d;
                best = s;
            }
        }
        if (found) {
            options.push_back(c);
            stands.push_back(best);
        }
    }
    if (options.empty()) {
        return false;
    }
    const size_t pick = rng.below(static_cast<uint32_t>(options.size()));
    cell = options[pick];
    stand = stands[pick];
    return true;
}

void MentalBreakSystem::tickFireSpree(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng) {
    if (fire_ == nullptr) {
        return;
    }
    if (b.hasGoal && b.walk != 0) {
        const Walk w = walkStep(pawn, b);
        if (w == Walk::Going) {
            return;
        }
        if (w == Walk::Failed) {
            b.hasGoal = false;
            b.nextAct = now + rules_.searchEvery;
            return;
        }
    }
    if (b.hasGoal) {
        // Arrived: light the cell the goal was picked for (the neighbours of the stand cell include it).
        if (now < b.nextHit) {
            return;
        }
        b.hasGoal = false;
        b.nextHit = now + rules_.fireEvery;
        const CellPos here = cellOf(things_.registry().get<Position>(pawn));
        // The cell to light is stored in the target's place as nothing: find the nearest flammable one in reach.
        CellPos best;
        bool found = false;
        float bestD = 0.0f;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const CellPos c{here.x + dx, here.y + dy, here.level};
                const float d = mentalCellDistSq(here, c);
                if ((found && d >= bestD) || !map_.inBounds(c) || fire_->burning(c) || fire_->flammabilityAt(c) <= 0.0f) {
                    continue;
                }
                found = true;
                bestD = d;
                best = c;
            }
        }
        if (found && fire_->ignite(best)) {
            ++b.done;
        }
        b.nextAct = now + rules_.fireEvery;
        return;
    }
    if (now < b.nextAct) {
        return;
    }
    CellPos cell, stand;
    if (!fireGoal(pawn, rng, cell, stand)) {
        b.nextAct = now + rules_.searchEvery;
        return;
    }
    beginWalk(pawn, b, stand);
    b.nextHit = now;
}

// ---- Insulting spree ------------------------------------------------------------------------------------

void MentalBreakSystem::tickInsult(Entity pawn, MentalBreaker& b, int64_t now, Rng&) {
    entt::registry& reg = things_.registry();
    const Kind* kind = kindNamed(b.def);
    const int count = kind != nullptr && kind->count > 0 ? kind->count : 3;
    if (b.done >= count) {
        finish(pawn, b, now, "recovered");
        return;
    }
    if (b.target != kNoEntity && !(things_.spawned(b.target) && !health_.incapacitated(b.target))) {
        stopWalk(pawn, b);
        b.target = kNoEntity;
    }
    if (b.target == kNoEntity) {
        if (now < b.nextAct) {
            return;
        }
        const Entity target = findPawnTarget(pawn, b, kMentalInsultRadius, true);
        if (target == kNoEntity) {
            finish(pawn, b, now, "recovered");  // nobody left to insult
            return;
        }
        b.target = target;
        b.nextAct = now;
    }
    if (!approach(pawn, b, b.target, rules_.talkReach, now)) {
        return;
    }
    if (events_ != nullptr) {
        Event event;
        event.type = EventType::PawnInsulted;
        event.thing = pawn;
        event.def = reg.get<Thing>(pawn).def;
        event.cell = cellOf(reg.get<Position>(pawn));
        event.target = b.target;
        event.message = "insult";
        events_->push_back(std::move(event));
    }
    b.visited.push_back(b.target);
    b.target = kNoEntity;
    ++b.done;
}

// ---- Giving up -------------------------------------------------------------------------------------------

// The nearest walkable cell on the edge of the map that the pawn can get to.
bool MentalBreakSystem::edgeGoal(Entity pawn, CellPos& out) const {
    const CellPos here = cellOf(things_.registry().get<Position>(pawn));
    const int w = map_.width();
    const int h = map_.height();
    bool found = false;
    float best = 0.0f;
    const auto consider = [&](int x, int y) {
        const CellPos c{x, y, here.level};
        const float d = mentalCellDistSq(here, c);
        if ((found && d >= best) || !map_.walkable(c) || !jobs_.canReach(here, c)) {
            return;
        }
        found = true;
        best = d;
        out = c;
    };
    for (int x = 0; x < w; x += 2) {
        consider(x, 0);
        consider(x, h - 1);
    }
    for (int y = 0; y < h; y += 2) {
        consider(0, y);
        consider(w - 1, y);
    }
    return found;
}

void MentalBreakSystem::tickLeave(Entity pawn, MentalBreaker& b, int64_t now, Rng&) {
    if (!b.hasGoal) {
        CellPos edge;
        if (!edgeGoal(pawn, edge)) {
            finish(pawn, b, now, "recovered");  // no way out
            return;
        }
        beginWalk(pawn, b, edge);
        return;
    }
    const Walk w = walkStep(pawn, b);
    if (w == Walk::Arrived) {
        leaveColony(pawn, b);
    } else if (w == Walk::Failed) {
        b.hasGoal = false;
        if (++b.done > 3) {
            finish(pawn, b, now, "recovered");  // held up for good: it stays after all
        }
    }
}

// The pawn walks off the map for good with everything it carries, wears and holds.
void MentalBreakSystem::leaveColony(Entity pawn, MentalBreaker& b) {
    entt::registry& reg = things_.registry();
    emit(EventType::PawnLeftColony, pawn, b.def, 0.0f);
    std::vector<Entity> belongings;
    if (const Carrying* carrying = reg.try_get<Carrying>(pawn)) {
        belongings.push_back(carrying->thing);
        reg.remove<Carrying>(pawn);
    }
    if (const Gear* gear = reg.try_get<Gear>(pawn)) {
        belongings.insert(belongings.end(), gear->apparel.begin(), gear->apparel.end());
        if (gear->weapon != kNoEntity) {
            belongings.push_back(gear->weapon);
        }
        reg.remove<Gear>(pawn);
    }
    for (const Entity item : belongings) {
        if (reg.valid(item)) {
            reg.remove<EquippedBy>(item);
            things_.destroy(item);
        }
    }
    for (const auto [bed, owners] : reg.view<Bed>().each()) {
        owners.owners.erase(std::remove(owners.owners.begin(), owners.owners.end(), pawn), owners.owners.end());
    }
    stopWalk(pawn, b);
    jobs_.forget(pawn);
    fighters_.forget(pawn);
    forget(pawn);
    things_.destroy(pawn);
}

// ---- Helpers --------------------------------------------------------------------------------------------

void MentalBreakSystem::action(Entity pawn, Entity target, PawnAction what) {
    if (events_ == nullptr) {
        return;
    }
    const entt::registry& reg = things_.registry();
    Event event;
    event.type = EventType::PawnAction;
    event.thing = pawn;
    event.def = reg.get<Thing>(pawn).def;
    event.action = what;
    event.target = target;
    event.cell = cellOf(reg.get<Position>(pawn));
    events_->push_back(std::move(event));
}

void MentalBreakSystem::emit(EventType type, Entity pawn, const std::string& message, float amount) {
    if (events_ == nullptr) {
        return;
    }
    const entt::registry& reg = things_.registry();
    Event event;
    event.type = type;
    event.thing = pawn;
    event.def = reg.get<Thing>(pawn).def;
    if (const Position* pos = reg.try_get<Position>(pawn)) {
        event.cell = cellOf(*pos);
    }
    event.message = message;
    event.amount = amount;
    events_->push_back(std::move(event));
}

// ---- Destroyed things, loading, hashing ---------------------------------------------------------------------

void MentalBreakSystem::forget(Entity thing) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(thing)) {
        return;
    }
    if (MentalBreaker* own = reg.try_get<MentalBreaker>(thing); own != nullptr && own->walk == 1) {
        pathfinder_.cancel(own->path);
        own->path = PathTicket{};
        own->walk = 0;
    }
    for (const Entity pawn : reg.view<MentalBreaker>()) {
        MentalBreaker& b = reg.get<MentalBreaker>(pawn);
        if (b.target == thing) {
            b.target = kNoEntity;
        }
        b.visited.erase(std::remove(b.visited.begin(), b.visited.end(), thing), b.visited.end());
    }
}

bool MentalBreakSystem::afterLoad(std::string& error) {
    entt::registry& reg = things_.registry();
    for (const Entity pawn : reg.view<MentalBreaker>()) {
        MentalBreaker& b = reg.get<MentalBreaker>(pawn);
        if (b.walk > 2 || (b.walk != 0 && !reg.all_of<Mover, Position>(pawn))) {
            error = "the save has a colonist in a mental break that can't walk";
            return false;
        }
        if (b.walk == 1 && pathfinder_.status(b.path) == PathStatus::Unknown) {
            error = "the save has a colonist in a mental break waiting for a path nobody is looking for";
            return false;
        }
        if (b.active && (kindNamed(b.def) == nullptr || !reg.all_of<Thing, Position, Mover>(pawn))) {
            // The data no longer has this break (or the pawn is off the map): the break just ends.
            stopWalk(pawn, b);
            b.active = false;
        }
        if (b.target != kNoEntity && !reg.valid(b.target)) {
            b.target = kNoEntity;
        }
        b.visited.erase(std::remove_if(b.visited.begin(), b.visited.end(), [&](Entity e) { return !reg.valid(e); }), b.visited.end());
    }
    return true;
}

void MentalBreakSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    const MentalBreaker* b = reg.try_get<MentalBreaker>(e);
    if (b == nullptr) {
        return;
    }
    h.add(uint64_t{0x3E47A1});
    h.add(b->active);
    h.add(b->def.data(), b->def.size());
    h.add(static_cast<int>(b->state));
    h.add(b->startedAt);
    h.add(b->endsAfter);
    h.add(b->hardEnd);
    h.add(b->recoverLevel);
    h.add(b->lastEnded);
    h.add(static_cast<int>(b->walk));
    h.add(static_cast<int>(b->repaths));
    h.add(b->path.slot);
    h.add(b->path.serial);
    h.add(b->hasGoal);
    h.add(b->goal.x);
    h.add(b->goal.y);
    h.add(b->goal.level);
    h.add(entt::to_integral(b->target));
    h.add(b->done);
    h.add(b->nextAct);
    h.add(b->nextHit);
    h.add(static_cast<uint64_t>(b->visited.size()));
    for (const Entity v : b->visited) {
        h.add(entt::to_integral(v));
    }
}

}  // namespace colony

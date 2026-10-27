#include "ColonySim/Inspirations.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

InspirationRules InspirationRules::read(const DefDatabase& defs) {
    InspirationRules r;
    const Def* rules = defs.find("Rules", "InspirationRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    r.checkEvery = std::max(1, static_cast<int>(rules->integer("checkEverySeconds", r.checkEvery)));
    r.minMood = static_cast<float>(rules->number("minMood", r.minMood));
    r.defaultDays = rules->number("defaultDurationDays", r.defaultDays);
    if (const size_t points = rules->count("meanDaysByMood"); points >= 2) {
        r.meanDays.clear();
        for (size_t i = 0; i < points; ++i) {
            const std::string at = "meanDaysByMood." + std::to_string(i);
            r.meanDays.emplace_back(static_cast<float>(rules->number((at + ".0").c_str())),
                                    static_cast<float>(rules->number((at + ".1").c_str())));
        }
        std::sort(r.meanDays.begin(), r.meanDays.end());
    }
    return r;
}

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float inspirationMeanDays(const std::vector<std::pair<float, float>>& points, float mood) {
    if (mood <= points.front().first) {
        return points.front().second;
    }
    for (size_t i = 1; i < points.size(); ++i) {
        if (mood <= points[i].first) {
            const float span = points[i].first - points[i - 1].first;
            const float t = span > 0.0f ? (mood - points[i - 1].first) / span : 1.0f;
            return points[i - 1].second + (points[i].second - points[i - 1].second) * t;
        }
    }
    return points.back().second;
}

}  // namespace

InspirationSystem::InspirationSystem(Things& things, HealthSystem& health, const DefDatabase& defs, const GameTime& time)
    : things_(things), health_(health), defs_(defs), time_(time), rules_(InspirationRules::read(defs)) {
    for (const DefId id : defs.idsOfKind("Inspirations")) {
        const Def& def = defs.get(id);
        Kind kind;
        kind.def = id;
        kind.name = def.defName();
        kind.commonality = def.number("baseCommonality", 1.0);
        kind.days = def.number("durationDays", rules_.defaultDays);
        for (size_t i = 0; i < def.count("anySkill"); ++i) {
            kind.anySkill.push_back(def.textAt("anySkill", i));
        }
        for (const std::string& stat : def.keys("statFactors")) {
            kind.statFactors.emplace_back(stat, static_cast<float>(def.number(("statFactors." + stat).c_str(), 1.0)));
        }
        kind.qualityShift = static_cast<int>(def.integer("qualityShift", 0));
        kind.freeMedicine = def.flag("freeMedicine", false);
        kind.recruitFactor = static_cast<float>(def.number("recruitChanceFactor", 1.0));
        kinds_.push_back(std::move(kind));
    }
    // The weighted pick walks this list, so sort it by name to be independent of the defs' load order.
    std::sort(kinds_.begin(), kinds_.end(), [](const Kind& a, const Kind& b) { return a.name < b.name; });
}

const InspirationSystem::Kind* InspirationSystem::kindNamed(const std::string& name) const {
    for (const Kind& kind : kinds_) {
        if (kind.name == name) {
            return &kind;
        }
    }
    return nullptr;
}

const InspirationSystem::Kind* InspirationSystem::activeKind(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn)) {
        return nullptr;
    }
    const Inspired* inspired = reg.try_get<Inspired>(pawn);
    return inspired != nullptr ? kindNamed(inspired->def) : nullptr;
}

std::string InspirationSystem::defOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Inspired* inspired = reg.valid(pawn) ? reg.try_get<Inspired>(pawn) : nullptr;
    return inspired != nullptr ? inspired->def : std::string();
}

bool InspirationSystem::colonist(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<Thing, Position, Colonist, Mood>(pawn) && !reg.all_of<Prisoner>(pawn);
}

bool InspirationSystem::eligible(Entity pawn, const std::string& def) const {
    const Kind* kind = kindNamed(def);
    if (kind == nullptr || !colonist(pawn)) {
        return false;
    }
    if (kind->anySkill.empty()) {
        return true;
    }
    const Colonist& c = things_.registry().get<Colonist>(pawn);
    for (const std::string& skill : kind->anySkill) {
        const SkillRecord* record = c.skill(skill);
        if (record == nullptr || !record->disabled) {
            return true;  // a pawn without a record for it has the skill at its default
        }
    }
    return false;
}

void InspirationSystem::emit(EventType type, Entity pawn, const std::string& message, float amount) {
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

void InspirationSystem::begin(Entity pawn, const Kind& kind, int64_t now) {
    Inspired inspired;
    inspired.def = kind.name;
    inspired.startedAt = now;
    inspired.endsAt = now + static_cast<int64_t>(std::llround(kind.days * static_cast<double>(kSecondsPerDay)));
    things_.registry().emplace_or_replace<Inspired>(pawn, std::move(inspired));
    emit(EventType::InspirationStarted, pawn, kind.name, static_cast<float>(kind.days));
}

bool InspirationSystem::start(Entity pawn, const std::string& def, std::string& error) {
    const Kind* kind = kindNamed(def);
    if (kind == nullptr) {
        error = "there is no inspiration called " + def;
    } else if (!colonist(pawn)) {
        error = "only a colonist can be inspired";
    } else if (has(pawn)) {
        error = "it is inspired already";
    } else if (!eligible(pawn, def)) {
        error = "it can't do the work this inspiration is for";
    } else {
        begin(pawn, *kind, time_.seconds);
        return true;
    }
    return false;
}

bool InspirationSystem::end(Entity pawn, const char* why) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Inspired>(pawn)) {
        return false;
    }
    const Inspired inspired = reg.get<Inspired>(pawn);
    reg.remove<Inspired>(pawn);
    emit(EventType::InspirationEnded, pawn, std::string(why) + ":" + inspired.def,
         static_cast<float>(time_.seconds - inspired.startedAt));
    return true;
}

float InspirationSystem::statFactor(Entity pawn, const char* stat) const {
    const Kind* kind = activeKind(pawn);
    if (kind == nullptr) {
        return 1.0f;
    }
    for (const auto& [name, factor] : kind->statFactors) {
        if (name == stat) {
            return factor;
        }
    }
    return 1.0f;
}

int InspirationSystem::takeQualityShift(Entity pawn) {
    const Kind* kind = activeKind(pawn);
    if (kind == nullptr || kind->qualityShift <= 0) {
        return 0;
    }
    const int shift = kind->qualityShift;
    end(pawn, "used");
    return shift;
}

bool InspirationSystem::takeFreeMedicine(Entity pawn) {
    const Kind* kind = activeKind(pawn);
    if (kind == nullptr || !kind->freeMedicine) {
        return false;
    }
    end(pawn, "used");
    return true;
}

float InspirationSystem::takeRecruitFactor(Entity pawn) {
    const Kind* kind = activeKind(pawn);
    if (kind == nullptr || kind->recruitFactor == 1.0f) {
        return 1.0f;
    }
    const float factor = kind->recruitFactor;
    end(pawn, "used");
    return factor;
}

void InspirationSystem::natural(Entity pawn, int64_t now) {
    entt::registry& reg = things_.registry();
    const Mood& mood = reg.get<Mood>(pawn);
    if (mood.level < rules_.minMood || has(pawn) || health_.incapacitated(pawn)) {
        return;
    }
    if (const MentalBreaker* breaker = reg.try_get<MentalBreaker>(pawn); breaker != nullptr && breaker->active) {
        return;
    }
    const double meanSeconds = static_cast<double>(inspirationMeanDays(rules_.meanDays, mood.level)) * static_cast<double>(kSecondsPerDay);
    if (meanSeconds <= 0.0) {
        return;
    }
    // The rolls come from the second and the pawn, so they don't depend on the order pawns are walked in.
    Rng roll(splitMix64(static_cast<uint64_t>(now) ^ (static_cast<uint64_t>(entt::to_integral(pawn)) << 32) ^ 0x1257AULL));
    if (roll.unit() >= 1.0 - std::exp(-static_cast<double>(rules_.checkEvery) / meanSeconds)) {
        return;
    }
    std::vector<const Kind*> options;
    double total = 0.0;
    for (const Kind& kind : kinds_) {
        if (kind.commonality > 0.0 && eligible(pawn, kind.name)) {
            options.push_back(&kind);
            total += kind.commonality;
        }
    }
    if (options.empty()) {
        return;
    }
    double pick = static_cast<double>(roll.unit()) * total;
    for (const Kind* kind : options) {
        pick -= kind->commonality;
        if (pick < 0.0) {
            begin(pawn, *kind, now);
            return;
        }
    }
    begin(pawn, *options.back(), now);
}

void InspirationSystem::step(int64_t now) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> done;
    for (const Entity pawn : reg.view<Inspired>()) {
        if (now >= reg.get<Inspired>(pawn).endsAt || health_.incapacitated(pawn)) {
            done.push_back(pawn);
        }
    }
    std::sort(done.begin(), done.end());
    for (const Entity pawn : done) {
        end(pawn, "expired");
    }
    if (!natural_ || !rules_.present || kinds_.empty()) {
        return;
    }
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<Colonist, Mood>()) {
        if ((now + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.checkEvery == 0 && colonist(pawn)) {
            pawns.push_back(pawn);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    for (const Entity pawn : pawns) {
        natural(pawn, now);
    }
}

bool InspirationSystem::afterLoad(std::string& error) {
    const entt::registry& reg = things_.registry();
    for (const Entity pawn : reg.view<Inspired>()) {
        const Inspired& inspired = reg.get<Inspired>(pawn);
        if (kindNamed(inspired.def) == nullptr) {
            error = "the save file has an inspiration (" + inspired.def + ") the game does not know";
            return false;
        }
    }
    return true;
}

void InspirationSystem::hashThing(Entity e, StateHasher& h) const {
    const Inspired* inspired = things_.registry().try_get<Inspired>(e);
    if (inspired == nullptr) {
        return;
    }
    h.add(uint64_t{0x1257A3});
    h.add(inspired->def.data(), inspired->def.size());
    h.add(inspired->startedAt);
    h.add(inspired->endsAt);
}

}  // namespace colony

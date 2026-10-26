#include "ColonySim/Recreation.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float recFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

}  // namespace

float Joy::toleranceOf(const std::string& kind) const {
    for (const JoyTolerance& t : tolerance) {
        if (t.kind == kind) {
            return t.amount;
        }
    }
    return 0.0f;
}

void Joy::addTolerance(const std::string& kind, float amount, float max) {
    auto it = std::lower_bound(tolerance.begin(), tolerance.end(), kind,
                               [](const JoyTolerance& t, const std::string& k) { return t.kind < k; });
    if (it == tolerance.end() || it->kind != kind) {
        it = tolerance.insert(it, JoyTolerance{kind, 0.0f});
    }
    it->amount = std::min(max, it->amount + amount);
}

RecreationRules RecreationRules::read(const DefDatabase& defs) {
    RecreationRules r;
    const Def* joy = defs.find("Rules", "JoyRules");
    if (joy == nullptr) {
        return r;
    }
    r.present = true;
    r.joyFallPerDay = recFloat(*joy, "fallPerDay", r.joyFallPerDay);
    r.toleranceMinGainFactor = recFloat(*joy, "toleranceMinGainFactor", r.toleranceMinGainFactor);
    r.toleranceMax = recFloat(*joy, "toleranceMax", r.toleranceMax);
    if (joy->count("joyHours") >= 2) {
        r.joyFromHour = std::clamp(static_cast<int>(joy->numberAt("joyHours", 0, r.joyFromHour)), 0, 23);
        r.joyToHour = std::clamp(static_cast<int>(joy->numberAt("joyHours", 1, r.joyToHour)), 0, 23);
    }
    if (const Def* rules = defs.find("Rules", "RecreationRules")) {
        r.startJoy = recFloat(*rules, "startJoy", r.startJoy);
        r.seekBelow = recFloat(*rules, "seekBelow", r.seekBelow);
        r.joyHoursUntil = recFloat(*rules, "joyHoursUntil", r.joyHoursUntil);
        r.dayFromHour = std::clamp(static_cast<int>(rules->integer("dayFromHour", r.dayFromHour)), 0, 23);
        r.dayToHour = std::clamp(static_cast<int>(rules->integer("dayToHour", r.dayToHour)), 0, 24);
        r.walkRadius = recFloat(*rules, "walkRadius", r.walkRadius);
        r.spotRadius = recFloat(*rules, "spotRadius", r.spotRadius);
    }
    if (const Def* rules = defs.find("Rules", "ComfortRules")) {
        r.comfortStart = recFloat(*rules, "start", r.comfortStart);
        r.comfortFallPerDay = recFloat(*rules, "fallPerDay", r.comfortFallPerDay);
        r.comfortRisePerHour = recFloat(*rules, "risePerHour", r.comfortRisePerHour);
        r.groundComfort = recFloat(*rules, "groundComfort", r.groundComfort);
        r.defaultBedComfort = recFloat(*rules, "defaultBedComfort", r.defaultBedComfort);
    }
    if (const Def* rules = defs.find("Rules", "OutdoorsRules")) {
        r.outdoorsStart = recFloat(*rules, "start", r.outdoorsStart);
        r.outdoorsFallPerDay = recFloat(*rules, "fallPerDay", r.outdoorsFallPerDay);
        r.outdoorsRisePerDay = recFloat(*rules, "risePerDay", r.outdoorsRisePerDay);
    }
    for (const DefId id : defs.idsOfKind("RecreationKinds")) {
        const Def& def = defs.get(id);
        RecreationKind kind;
        kind.defName = def.defName();
        kind.gainPerUse = recFloat(def, "toleranceGainPerUse", kind.gainPerUse);
        kind.decayPerDay = recFloat(def, "toleranceDecayPerDay", kind.decayPerDay);
        r.kinds.push_back(std::move(kind));
    }
    for (const DefId id : defs.idsOfKind("RecreationActivities")) {
        const Def& def = defs.get(id);
        RecreationActivity a;
        a.def = id;
        a.defName = def.defName();
        a.kind = r.kindIndex(def.text("kind"));
        if (a.kind < 0) {
            continue;  // check_defs.py catches the bad name; the sim just doesn't offer it
        }
        a.building = def.text("building");
        a.buildingFlag = def.text("buildingFlag");
        if (def.count("users") >= 2) {
            a.usersMin = std::max(1, static_cast<int>(def.numberAt("users", 0, 1)));
            a.usersMax = std::max(a.usersMin, static_cast<int>(def.numberAt("users", 1, 1)));
        }
        a.joyPerHour = recFloat(def, "joyPerHour", a.joyPerHour);
        if (def.count("durationHours") >= 2) {
            a.hoursMin = std::max(0.05, def.numberAt("durationHours", 0, a.hoursMin));
            a.hoursMax = std::max(a.hoursMin, def.numberAt("durationHours", 1, a.hoursMax));
        }
        a.outdoors = def.flag("outdoors");
        a.daytimeOnly = def.flag("daytimeOnly");
        a.walks = def.flag("walks");
        a.lies = def.flag("lies");
        if (a.building.empty() && a.buildingFlag.empty() && !a.outdoors) {
            continue;  // needs a place
        }
        r.activities.push_back(std::move(a));
    }
    return r;
}

int RecreationRules::kindIndex(const std::string& defName) const {
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (kinds[i].defName == defName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

float RecreationRules::gainPerHour(const RecreationActivity& activity, const Joy& joy) const {
    const float tolerance = joy.toleranceOf(kinds[static_cast<size_t>(activity.kind)].defName);
    return activity.joyPerHour * std::max(toleranceMinGainFactor, 1.0f - tolerance);
}

RecreationSystem::RecreationSystem(Things& things, const DefDatabase& defs)
    : things_(things), defs_(defs), rules_(RecreationRules::read(defs)) {}

// Beds and chairs give their def's `stats.comfort`; the ground, a sleeper's `groundComfort`.
float RecreationSystem::comfortInUse(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Worker* worker = reg.try_get<Worker>(pawn);
    if (worker == nullptr) {
        return -1.0f;
    }
    const Job& job = worker->job;
    const auto comfortOf = [&](Entity furniture, float fallback) {
        if (furniture == kNoEntity || !reg.valid(furniture) || !reg.all_of<Thing>(furniture)) {
            return fallback;
        }
        return recFloat(defs_.get(reg.get<Thing>(furniture).def), "stats.comfort", fallback);
    };
    if (jobIsLyingDown(job)) {
        return job.thing != kNoEntity ? comfortOf(job.thing, rules_.defaultBedComfort) : rules_.groundComfort;
    }
    if (job.type == JobType::Ingest && job.chair != kNoEntity && job.step + 1 == jobStepCount(job.type)) {
        return comfortOf(job.chair, rules_.groundComfort);
    }
    if (job.type == JobType::Recreate && job.chair != kNoEntity && job.step + 1 == jobStepCount(job.type)) {
        return comfortOf(job.chair, rules_.groundComfort);
    }
    return -1.0f;
}

void RecreationSystem::step() {
    if (!rules_.present) {
        return;
    }
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<Needs>()) {
        pawns.push_back(pawn);
    }
    std::sort(pawns.begin(), pawns.end());

    // New colonists start with the needs half full.
    for (const Entity pawn : pawns) {
        if (!reg.all_of<Joy>(pawn)) {
            Joy& joy = reg.emplace<Joy>(pawn);
            joy.level = rules_.startJoy;
            joy.comfort = rules_.comfortStart;
            joy.outdoors = rules_.outdoorsStart;
        }
    }

    constexpr float kPerSecond = 1.0f / static_cast<float>(kSecondsPerDay);
    constexpr float kPerSecondOfHour = 1.0f / static_cast<float>(kSecondsPerHour);
    for (const Entity pawn : pawns) {
        const Health* health = reg.try_get<Health>(pawn);
        if (health != nullptr && health->dead) {
            continue;
        }
        Joy& joy = reg.get<Joy>(pawn);
        const Worker* worker = reg.try_get<Worker>(pawn);
        const bool asleep = worker != nullptr && jobIsLyingDown(worker->job);
        const bool playing = worker != nullptr && worker->job.type == JobType::Recreate && worker->job.step > 0;

        // Joy falls while the pawn is awake; the activity's job raises it (Jobs.h) while it plays.
        if (!asleep && !playing) {
            joy.level = std::max(0.0f, joy.level - rules_.joyFallPerDay * kPerSecond);
        }
        for (JoyTolerance& t : joy.tolerance) {
            const int index = rules_.kindIndex(t.kind);
            const float decay = index >= 0 ? rules_.kinds[static_cast<size_t>(index)].decayPerDay : 0.1f;
            t.amount = std::max(0.0f, t.amount - decay * kPerSecond);
        }
        joy.tolerance.erase(std::remove_if(joy.tolerance.begin(), joy.tolerance.end(),
                                           [](const JoyTolerance& t) { return t.amount <= 0.0f; }),
                            joy.tolerance.end());

        // Comfort moves towards the furniture's comfort while it is used, else falls.
        const float used = comfortInUse(pawn);
        if (used >= 0.0f) {
            if (joy.comfort < used) {
                joy.comfort = std::min(used, joy.comfort + rules_.comfortRisePerHour * kPerSecondOfHour);
            } else {
                joy.comfort = std::max(used, joy.comfort - rules_.comfortFallPerDay * 4.0f * kPerSecond);
            }
        } else {
            joy.comfort = std::max(0.0f, joy.comfort - rules_.comfortFallPerDay * kPerSecond);
        }

        // Outdoors rises under the open sky and falls indoors.
        const Position* position = reg.try_get<Position>(pawn);
        const bool indoors = rooms_ != nullptr && position != nullptr && rooms_->indoors(cellOf(*position));
        if (indoors) {
            joy.outdoors = std::max(0.0f, joy.outdoors - rules_.outdoorsFallPerDay * kPerSecond);
        } else {
            joy.outdoors = std::min(1.0f, joy.outdoors + rules_.outdoorsRisePerDay * kPerSecond);
        }
    }
}

}  // namespace colony

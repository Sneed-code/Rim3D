#include "ColonySim/Mood.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float moodFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

const char* moodFoodBandName(FoodBand band) {
    switch (band) {
        case FoodBand::Fed: return "Fed";
        case FoodBand::Hungry: return "Hungry";
        case FoodBand::Ravenous: return "Ravenous";
        case FoodBand::Malnourished: return "Malnourished";
    }
    return "";
}

const char* moodRestBandName(RestBand band) {
    switch (band) {
        case RestBand::Rested: return "Rested";
        case RestBand::Drowsy: return "Drowsy";
        case RestBand::Tired: return "Tired";
        case RestBand::Exhausted: return "Exhausted";
    }
    return "";
}

bool moodIsMeal(const std::string& preferability) {
    return preferability == "MealAwful" || preferability == "MealSimple" || preferability == "MealFine" ||
           preferability == "MealLavish";
}

// The trait degree's def path ("degreeDatas.N") for a colonist's trait, or empty.
std::string moodDegreePath(const Def& trait, int degree) {
    const size_t degrees = trait.count("degreeDatas");
    for (size_t i = 0; i < degrees; ++i) {
        const std::string path = "degreeDatas." + std::to_string(i);
        if (trait.integer((path + ".degree").c_str(), 0) == degree) {
            return path;
        }
    }
    return std::string();
}

}  // namespace

const char* moodBandName(MoodBand band) {
    switch (band) {
        case MoodBand::Stable: return "stable";
        case MoodBand::Minor: return "minor";
        case MoodBand::Major: return "major";
        case MoodBand::Extreme: return "extreme";
    }
    return "?";
}

MoodRules MoodRules::read(const DefDatabase& defs) {
    MoodRules r;
    const Def* def = defs.find("Rules", "MoodRules");
    if (def == nullptr) {
        return r;
    }
    r.present = true;
    r.baseMood = moodFloat(*def, "baseMood", r.baseMood);
    r.minorBreakThreshold = moodFloat(*def, "breakThreshold.minor", r.minorBreakThreshold);
    r.majorBreakThreshold = moodFloat(*def, "breakThreshold.major", r.majorBreakThreshold);
    r.extremeBreakThreshold = moodFloat(*def, "breakThreshold.extreme", r.extremeBreakThreshold);
    r.risePerHour = moodFloat(*def, "risePerHour", r.risePerHour);
    r.fallPerHour = moodFloat(*def, "fallPerHour", r.fallPerHour);
    r.memoryDays = def->number("memoryDays", r.memoryDays);
    return r;
}

MoodSystem::MoodSystem(Things& things, const NeedsSystem& needs, const DefDatabase& defs)
    : things_(things), needs_(needs), defs_(defs), rules_(MoodRules::read(defs)) {
    for (const DefId id : defs.idsOfKind("Thoughts")) {
        const Def& def = defs.get(id);
        Thought thought;
        thought.def = id;
        thought.offset = moodFloat(def, "moodOffset", 0.0f);
        thought.durationDays = def.number("durationDays", rules_.memoryDays);
        thought.trigger = def.text("trigger");
        thought.need = def.text("situation.need");
        thought.band = def.text("situation.band");
        thoughts_.push_back(std::move(thought));
    }
}

float MoodSystem::traitMoodOffset(Entity pawn) const {
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    float total = 0.0f;
    if (colonist == nullptr) {
        return total;
    }
    for (const TraitRecord& record : colonist->traits) {
        if (const Def* trait = defs_.find("Traits", record.def)) {
            const std::string path = moodDegreePath(*trait, record.degree);
            if (!path.empty()) {
                total += moodFloat(*trait, (path + ".moodOffset").c_str(), 0.0f);
            }
        }
    }
    return total;
}

float MoodSystem::traitBreakOffset(Entity pawn) const {
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    float total = 0.0f;
    if (colonist == nullptr) {
        return total;
    }
    for (const TraitRecord& record : colonist->traits) {
        if (const Def* trait = defs_.find("Traits", record.def)) {
            const std::string path = moodDegreePath(*trait, record.degree);
            if (!path.empty()) {
                total += moodFloat(*trait, (path + ".statOffsets.mentalBreakThreshold").c_str(), 0.0f);
            }
        }
    }
    return total;
}

void MoodSystem::breakLevels(Entity pawn, float& minor, float& major, float& extreme) const {
    const float offset = traitBreakOffset(pawn);
    minor = std::clamp(rules_.minorBreakThreshold + offset, 0.0f, 1.0f) * 100.0f;
    major = std::clamp(rules_.majorBreakThreshold + offset, 0.0f, 1.0f) * 100.0f;
    extreme = std::clamp(rules_.extremeBreakThreshold + offset, 0.0f, 1.0f) * 100.0f;
}

std::vector<ThoughtView> MoodSystem::thoughts(Entity pawn, int64_t now) const {
    std::vector<ThoughtView> out;
    const entt::registry& reg = things_.registry();
    const Needs* needs = reg.try_get<Needs>(pawn);
    if (needs != nullptr) {
        const std::string foodBand = moodFoodBandName(foodBandOf(needs_.rules(), needs->food));
        const std::string restBand = moodRestBandName(restBandOf(needs_.rules(), needs->rest));
        for (const Thought& thought : thoughts_) {
            if (thought.need.empty()) {
                continue;
            }
            const std::string& band = thought.need == "food" ? foodBand : restBand;
            if (thought.need != "food" && thought.need != "rest") {
                continue;
            }
            if (band == thought.band) {
                const Def& def = defs_.get(thought.def);
                out.push_back(ThoughtView{def.defName(), def.label(), thought.offset, true, 0});
            }
        }
    }
    if (const Mood* mood = reg.try_get<Mood>(pawn)) {
        for (const Memory& memory : mood->memories) {
            if (memory.expires <= now) {
                continue;
            }
            const Def* def = defs_.find("Thoughts", memory.def);
            if (def != nullptr) {
                out.push_back(ThoughtView{def->defName(), def->label(), moodFloat(*def, "moodOffset", 0.0f), false, memory.expires});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const ThoughtView& a, const ThoughtView& b) {
        const float aa = std::fabs(a.offset);
        const float bb = std::fabs(b.offset);
        return aa != bb ? aa > bb : a.def < b.def;
    });
    return out;
}

float MoodSystem::target(Entity pawn, int64_t now) const {
    float total = rules_.baseMood + traitMoodOffset(pawn);
    for (const ThoughtView& thought : thoughts(pawn, now)) {
        total += thought.offset;
    }
    return total;
}

void MoodSystem::addMemory(Mood& mood, const std::string& trigger, int64_t now) {
    for (const Thought& thought : thoughts_) {
        if (thought.trigger != trigger) {
            continue;
        }
        const std::string& name = defs_.get(thought.def).defName();
        const int64_t expires = now + static_cast<int64_t>(std::llround(thought.durationDays * static_cast<double>(kSecondsPerDay)));
        // One of each: a second one starts the fading over.
        const auto it = std::find_if(mood.memories.begin(), mood.memories.end(), [&](const Memory& m) { return m.def == name; });
        if (it != mood.memories.end()) {
            it->expires = expires;
        } else {
            mood.memories.push_back(Memory{name, expires});
        }
    }
}

void MoodSystem::step(int64_t now, std::vector<Event>& events) {
    if (!rules_.present) {
        return;
    }
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<Needs>()) {
        pawns.push_back(pawn);
    }
    std::sort(pawns.begin(), pawns.end());

    // New colonists start at the mood they are heading for.
    for (const Entity pawn : pawns) {
        if (!reg.all_of<Mood>(pawn)) {
            Mood& mood = reg.emplace<Mood>(pawn);
            mood.level = std::clamp(target(pawn, now), 0.0f, 100.0f);
        }
    }

    // What happened this step starts memories. (The events added below are only read next step,
    // and none of them is one of these.)
    const size_t eventCount = events.size();
    for (size_t i = 0; i < eventCount; ++i) {
        const Event& event = events[i];
        // A break that ended in calm (or by the pawn going down) is a relief; an insult stings its target.
        if (event.type == EventType::PawnMentalBreakEnded || event.type == EventType::PawnInsulted) {
            const bool ended = event.type == EventType::PawnMentalBreakEnded;
            const Entity who = ended ? event.thing : event.target;
            Mood* felt = reg.valid(who) ? reg.try_get<Mood>(who) : nullptr;
            if (felt != nullptr && (!ended || event.message == "recovered" || event.message == "downed")) {
                addMemory(*felt, ended ? "MentalBreakEnded" : "Insulted", now);
            }
            continue;
        }
        Mood* mood = (event.type == EventType::PawnAte || event.type == EventType::PawnWokeUp) && reg.valid(event.thing)
                         ? reg.try_get<Mood>(event.thing)
                         : nullptr;
        if (mood == nullptr) {
            continue;
        }
        if (event.type == EventType::PawnAte) {
            const std::string preferability = defs_.get(event.def).text("ingestible.preferability");
            if (preferability == "RawBad") {
                addMemory(*mood, "AteRawFood", now);
            }
            if (event.message == "ground" && moodIsMeal(preferability)) {
                addMemory(*mood, "AteWithoutTable", now);
            }
            if (preferability == "MealFine") {
                addMemory(*mood, "AteFineMeal", now);
            }
            if (preferability == "MealLavish") {
                addMemory(*mood, "AteLavishMeal", now);
            }
        } else {
            if (event.message == "ground") {
                addMemory(*mood, "SleptOnGround", now);
            }
            if (rooms_ != nullptr && !rooms_->indoors(event.cell)) {
                addMemory(*mood, "SleptOutside", now);
            }
        }
    }

    constexpr float kPerSecond = 1.0f / static_cast<float>(kSecondsPerHour);
    for (const Entity pawn : pawns) {
        const Health* health = reg.try_get<Health>(pawn);
        if (health != nullptr && health->dead) {
            continue;
        }
        Mood& mood = reg.get<Mood>(pawn);
        mood.memories.erase(std::remove_if(mood.memories.begin(), mood.memories.end(),
                                           [&](const Memory& m) { return m.expires <= now; }),
                            mood.memories.end());

        // The level moves towards the target, but stands still while the pawn sleeps.
        const Worker* worker = reg.try_get<Worker>(pawn);
        if (worker == nullptr || !jobIsLyingDown(worker->job)) {
            const float goal = std::clamp(target(pawn, now), 0.0f, 100.0f);
            if (goal > mood.level) {
                mood.level = std::min(goal, mood.level + rules_.risePerHour * kPerSecond);
            } else {
                mood.level = std::max(goal, mood.level - rules_.fallPerHour * kPerSecond);
            }
        }

        float minor = 0.0f;
        float major = 0.0f;
        float extreme = 0.0f;
        breakLevels(pawn, minor, major, extreme);
        MoodBand band = MoodBand::Stable;
        if (mood.level < extreme) {
            band = MoodBand::Extreme;
        } else if (mood.level < major) {
            band = MoodBand::Major;
        } else if (mood.level < minor) {
            band = MoodBand::Minor;
        }
        if (band != mood.band) {
            mood.band = band;
            Event event;
            event.type = EventType::PawnMoodBand;
            event.thing = pawn;
            event.def = reg.get<Thing>(pawn).def;
            event.message = moodBandName(band);
            event.amount = mood.level;
            events.push_back(std::move(event));
        }
    }
}

}  // namespace colony

#include "ColonySim/Mood.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/RoomStats.h"
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
    r.nightFromHour = std::clamp(static_cast<int>(def->integer("nightHours.from", r.nightFromHour)), 0, 23);
    r.nightToHour = std::clamp(static_cast<int>(def->integer("nightHours.to", r.nightToHour)), 0, 23);
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
        thought.condition = def.text("situation.condition");
        if (!thought.condition.empty()) {
            for (size_t i = 0; i < def.count("stageThresholds"); ++i) {
                thought.thresholds.push_back(static_cast<float>(def.numberAt("stageThresholds", i)));
            }
            for (size_t i = 0; i < def.count("roomRoles"); ++i) {
                thought.roomRoles.push_back(def.textAt("roomRoles", i));
            }
            for (size_t i = 0; i < def.count("stages"); ++i) {
                const std::string path = "stages." + std::to_string(i);
                Thought::Stage stage;
                stage.offset = moodFloat(def, (path + ".moodOffset").c_str(), 0.0f);
                stage.label = def.text((path + ".label").c_str());
                for (size_t r = 0; r < def.count((path + ".roles").c_str()); ++r) {
                    stage.roles.push_back(def.textAt((path + ".roles").c_str(), r));
                }
                thought.stages.push_back(std::move(stage));
            }
            thought.trait = def.text("situation.trait");
            thought.notTrait = def.text("situation.notTrait");
            for (size_t i = 0; i < def.count("situation.notTraits"); ++i) {
                thought.notTraits.push_back(def.textAt("situation.notTraits", i));
            }
            for (size_t i = 0; i < def.count("situation.anyTraits"); ++i) {
                thought.anyTraits.push_back(def.textAt("situation.anyTraits", i));
            }
        }
        thought.passion = def.text("situation.passion");
        if (!thought.passion.empty()) {
            // The size of the lift is SkillRules.passionMood, not a number of the thought's own.
            const SkillRules skills = SkillRules::read(defs);
            thought.offset = static_cast<float>(skills.passionMood[thought.passion == "Major" ? 2 : 1]);
        }
        thoughts_.push_back(std::move(thought));

        StagedThought staged;
        staged.def = id;
        staged.condition = def.text("situation.condition");
        const size_t stages = def.count("stages");
        if (staged.condition.empty() || stages == 0) {
            continue;
        }
        // The thresholds sit in a list named after the condition: "joyNeed" has "joyThresholds".
        std::string key = staged.condition;
        if (key.size() > 4 && key.compare(key.size() - 4, 4, "Need") == 0) {
            key.resize(key.size() - 4);
        }
        key += "Thresholds";
        for (size_t i = 0; i < def.count(key.c_str()); ++i) {
            staged.thresholds.push_back(static_cast<float>(def.numberAt(key.c_str(), i)));
        }
        for (size_t i = 0; i < stages; ++i) {
            const std::string path = "stages." + std::to_string(i);
            staged.labels.push_back(def.text((path + ".label").c_str()));
            staged.offsets.push_back(moodFloat(def, (path + ".moodOffset").c_str(), 0.0f));
        }
        staged_.push_back(std::move(staged));
    }
}

// What each staged condition measures. A task that adds a condition adds its case here.
bool MoodSystem::conditionValue(Entity pawn, const std::string& condition, float& value) const {
    const entt::registry& reg = things_.registry();
    if (const Joy* joy = reg.try_get<Joy>(pawn)) {
        if (condition == "joyNeed") {
            value = joy->level;
            return true;
        }
        if (condition == "comfortNeed") {
            value = joy->comfort;
            return true;
        }
        if (condition == "outdoorsNeed") {
            value = joy->outdoors;
            return true;
        }
    }
    if (temperature_ != nullptr && (condition == "coldBelowComfort" || condition == "hotAboveComfort")) {
        value = condition == "coldBelowComfort" ? temperature_->degreesBelowComfort(pawn)
                                                : temperature_->degreesAboveComfort(pawn);
        return value > 0.0f;
    }
    return false;
}

void MoodSystem::addSituation(const std::string& condition, SituationStage stage) {
    situations_.push_back(Situation{condition, std::move(stage)});
}

bool MoodSystem::hasTrait(Entity pawn, const std::string& trait) const {
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    if (colonist == nullptr) {
        return false;
    }
    return std::any_of(colonist->traits.begin(), colonist->traits.end(),
                       [&](const TraitRecord& record) { return record.def == trait; });
}

float MoodSystem::traitMoodOffset(Entity pawn) const {
    const Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    float total = 0.0f;
    if (const PawnGenes* genes = things_.registry().try_get<PawnGenes>(pawn)) {
        total += genes->effects.moodOffset;
    }
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
    for (const StagedThought& staged : staged_) {
        float value = 0.0f;
        if (!conditionValue(pawn, staged.condition, value)) {
            continue;
        }
        size_t stage = 0;
        while (stage < staged.thresholds.size() && stage + 1 < staged.offsets.size() && value >= staged.thresholds[stage]) {
            ++stage;
        }
        const Def& def = defs_.get(staged.def);
        out.push_back(ThoughtView{def.defName(), staged.labels[stage].empty() ? def.label() : staged.labels[stage],
                                  staged.offsets[stage], true, 0});
    }
    // Staged situations that read the pawn's room or its beauty need (C17).
    if (roomStats_ != nullptr) {
        for (const Thought& thought : thoughts_) {
            double value = 0.0;
            RoomRole role = RoomRole::None;
            if (thought.condition.empty() || thought.stages.empty() || !roomStats_->conditionValue(pawn, thought.condition, value, role)) {
                continue;
            }
            const auto listed = [](const std::vector<std::string>& roles, RoomRole r) {
                return roles.empty() || std::find(roles.begin(), roles.end(), roomRoleName(r)) != roles.end();
            };
            if (!listed(thought.roomRoles, role)) {
                continue;
            }
            size_t stage = 0;
            while (stage < thought.thresholds.size() && value >= static_cast<double>(thought.thresholds[stage])) {
                ++stage;
            }
            stage = std::min(stage, thought.stages.size() - 1);
            const Thought::Stage& picked = thought.stages[stage];
            if (picked.offset == 0.0f || !listed(picked.roles, role)) {
                continue;
            }
            const Def& def = defs_.get(thought.def);
            out.push_back(ThoughtView{def.defName(), def.label() + ": " + picked.label, picked.offset, true, 0});
        }
    }
    // Situations judged by other systems (clothes, and later temperature and light).
    for (const Thought& thought : thoughts_) {
        if (thought.condition.empty() || thought.stages.empty()) {
            continue;
        }
        if ((!thought.trait.empty() && !hasTrait(pawn, thought.trait)) ||
            (!thought.notTrait.empty() && hasTrait(pawn, thought.notTrait))) {
            continue;
        }
        const auto has = [&](const std::string& trait) { return hasTrait(pawn, trait); };
        if (std::any_of(thought.notTraits.begin(), thought.notTraits.end(), has) ||
            (!thought.anyTraits.empty() && std::none_of(thought.anyTraits.begin(), thought.anyTraits.end(), has))) {
            continue;
        }
        for (const Situation& situation : situations_) {
            if (situation.condition != thought.condition) {
                continue;
            }
            const int stage = situation.stage(pawn);
            if (stage >= 0 && static_cast<size_t>(stage) < thought.stages.size()) {
                const Thought::Stage& s = thought.stages[static_cast<size_t>(stage)];
                const Def& def = defs_.get(thought.def);
                out.push_back(ThoughtView{def.defName(), s.label.empty() ? def.label() : s.label, s.offset, true, 0});
            }
        }
    }
    // Working at a skill the pawn is passionate about.
    const Colonist* colonist = activeSkill_ ? reg.try_get<Colonist>(pawn) : nullptr;
    const std::string skill = colonist != nullptr ? activeSkill_(pawn) : std::string();
    const SkillRecord* record = skill.empty() ? nullptr : colonist->skill(skill);
    if (record != nullptr && !record->disabled && record->passion != Passion::None) {
        const char* passion = record->passion == Passion::Major ? "Major" : "Minor";
        for (const Thought& thought : thoughts_) {
            if (thought.passion == passion) {
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

void MoodSystem::remember(Entity pawn, const std::string& trigger, int64_t now) {
    entt::registry& reg = things_.registry();
    if (Mood* mood = reg.valid(pawn) ? reg.try_get<Mood>(pawn) : nullptr) {
        addMemory(*mood, trigger, now);
    }
}

namespace {
// A pawn of the player's colony (not a prisoner, guest or enemy) that has a mood.
bool moodIsColonist(entt::registry& reg, Entity pawn) {
    if (!reg.valid(pawn) || !reg.all_of<Colonist, Mood>(pawn) || reg.all_of<Prisoner>(pawn)) {
        return false;
    }
    const Faction* faction = reg.try_get<Faction>(pawn);
    return faction == nullptr || faction->id == kPlayerFaction;
}
}  // namespace

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
        if (event.type == EventType::PawnDied) {
            // A colonist died: every other living colonist knows. (Psychopaths shrug, as in RimWorld.)
            if (moodIsColonist(reg, event.thing)) {
                for (const Entity other : pawns) {
                    const Health* health = reg.try_get<Health>(other);
                    if (other != event.thing && moodIsColonist(reg, other) && (health == nullptr || !health->dead) &&
                        !hasTrait(other, "Psychopath")) {
                        addMemory(reg.get<Mood>(other), "KnowColonistDied", now);
                    }
                }
            }
            continue;
        }
        if (event.type == EventType::PrisonerExecuted) {
            // The colony knows. Cold-hearted, bloodthirsty and cannibal colonists shrug (RimWorld wiki).
            const char* trigger = event.message == "guilty" ? "KnowPrisonerExecutedGuilty" : "KnowPrisonerExecuted";
            for (const Entity other : pawns) {
                if (moodIsColonist(reg, other) && !hasTrait(other, "Psychopath") && !hasTrait(other, "Bloodlust") &&
                    !hasTrait(other, "Cannibal")) {
                    addMemory(reg.get<Mood>(other), trigger, now);
                }
            }
            continue;
        }
        if (event.type == EventType::OrganHarvested) {
            // The victim, if one of ours, is scarred by it; everyone else hears. A bloodthirsty colonist enjoys it.
            const bool ours = event.message == "colonist";
            if (ours && moodIsColonist(reg, event.thing)) {
                addMemory(reg.get<Mood>(event.thing), "MyOrganHarvested", now);
            }
            for (const Entity other : pawns) {
                if (other == event.thing || !moodIsColonist(reg, other)) {
                    continue;
                }
                if (hasTrait(other, "Bloodlust")) {
                    addMemory(reg.get<Mood>(other), "BloodlustOrganHarvested", now);
                } else if (!hasTrait(other, "Psychopath") && (ours || !hasTrait(other, "Cannibal"))) {
                    addMemory(reg.get<Mood>(other), ours ? "KnowColonistOrganHarvested" : "KnowGuestOrganHarvested", now);
                }
            }
            continue;
        }
        if (event.type == EventType::AnimalButchered) {
            // Cutting up a person: only the cold-hearted and the cannibals do not mind.
            Mood* butcher = reg.valid(event.thing) ? reg.try_get<Mood>(event.thing) : nullptr;
            if (butcher != nullptr && defs_.get(event.def).text("race.intelligence") == "Humanlike" && !eatsPeople(event.thing)) {
                addMemory(*butcher, "ButcheredHumanlikeCorpse", now);
                // The rest of the colony hears of it, except the butcher and those who do not mind.
                for (const Entity other : pawns) {
                    if (other != event.thing && moodIsColonist(reg, other) && !eatsPeople(other)) {
                        addMemory(reg.get<Mood>(other), "KnowButcheredHumanlikeCorpse", now);
                    }
                }
            }
            continue;
        }
        Mood* mood = (event.type == EventType::PawnAte || event.type == EventType::PawnWokeUp || event.type == EventType::PrisonerCaptured ||
                      event.type == EventType::PrisonerChatted) &&
                             reg.valid(event.type == EventType::PrisonerChatted ? event.target : event.thing)
                         ? reg.try_get<Mood>(event.type == EventType::PrisonerChatted ? event.target : event.thing)
                         : nullptr;
        if (mood == nullptr) {
            continue;
        }
        if (event.type == EventType::PrisonerCaptured) {
            addMemory(*mood, "PrisonerCaptured", now);
        } else if (event.type == EventType::PrisonerChatted) {
            addMemory(*mood, "PrisonerChatted", now);
        } else if (event.type == EventType::PawnAte) {
            const std::string preferability = defs_.get(event.def).text("ingestible.preferability");
            // Human flesh, raw or as an ingredient (`part` 1): a horror to most, a treat to a cannibal, nothing to a psychopath.
            const bool rawHumanFlesh = defs_.get(event.def).flag("humanlike");
            if ((rawHumanFlesh || event.part == 1) && !hasTrait(event.thing, "Psychopath")) {
                const bool cannibal = hasTrait(event.thing, "Cannibal");
                addMemory(*mood, rawHumanFlesh ? (cannibal ? "AteHumanlikeMeatCannibal" : "AteHumanlikeMeat")
                                               : (cannibal ? "AteHumanlikeMeatAsIngredientCannibal" : "AteHumanlikeMeatAsIngredient"),
                          now);
            }
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

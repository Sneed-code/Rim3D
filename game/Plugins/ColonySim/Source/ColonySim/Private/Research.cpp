#include "ColonySim/Research.h"

#include "ColonySim/Events.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Things.h"
#include "ColonySim/Traits.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// The projects a def needs: `researchPrerequisite` is one name or a list of them.
std::vector<std::string> researchNeeded(const Def& def) {
    std::vector<std::string> out;
    if (const size_t n = def.count("researchPrerequisite")) {
        for (size_t i = 0; i < n; ++i) {
            out.push_back(def.textAt("researchPrerequisite", i));
        }
    } else if (def.has("researchPrerequisite")) {
        out.push_back(def.text("researchPrerequisite"));
    }
    return out;
}

}  // namespace

ResearchRules ResearchRules::read(const DefDatabase& defs) {
    ResearchRules r;
    const Def* rules = defs.find("Rules", "ResearchRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    r.pointsPerTick = rules->number("pointsPerTick", r.pointsPerTick);
    r.secondsPerTick = std::max(1.0e-6, rules->number("secondsPerTick", r.secondsPerTick));
    r.skillSpeedBase = rules->number("skillSpeedBase", r.skillSpeedBase);
    r.skillSpeedPerLevel = rules->number("skillSpeedPerLevel", r.skillSpeedPerLevel);
    r.defaultSkillLevel = static_cast<int>(rules->integer("defaultSkillLevel", r.defaultSkillLevel));
    return r;
}

ResearchSystem::ResearchSystem(Things& things, const DefDatabase& defs) : things_(things), defs_(defs) {
    rules_ = ResearchRules::read(defs_);
    for (const DefId id : defs_.idsOfKind("Research")) {
        const Def& def = defs_.get(id);
        ResearchProject p;
        p.def = id;
        p.defName = def.defName();
        p.techLevel = def.text("techLevel");
        p.baseCost = def.number("baseCost");
        p.benchTier = std::max(1, static_cast<int>(def.integer("benchTier", 1)));
        for (const char* key : {"prerequisites", "hiddenPrerequisites"}) {
            for (size_t i = 0; i < def.count(key); ++i) {
                p.prerequisites.push_back(def.textAt(key, i));
            }
        }
        projects_.push_back(std::move(p));
    }
    for (const DefId id : defs_.idsOfKind("Things")) {
        const Def& def = defs_.get(id);
        if (def.has("researchBench")) {
            ResearchBenchInfo info;
            info.tier = std::max(1, static_cast<int>(def.integer("researchBench.tier", 1)));
            info.speedFactor = def.number("researchBench.speedFactor", 1.0);
            benches_[id.value] = info;
        }
    }
}

const ResearchProject* ResearchSystem::find(const std::string& defName) const {
    for (const ResearchProject& p : projects_) {
        if (p.defName == defName) {
            return &p;
        }
    }
    return nullptr;
}

bool ResearchSystem::isDone(const std::string& project) const {
    return std::binary_search(state_.done.begin(), state_.done.end(), project);
}

double ResearchSystem::progress(const std::string& project) const {
    if (const ResearchProject* p = find(project); p != nullptr && isDone(project)) {
        return cost(*p);
    }
    const auto it = state_.progress.find(project);
    return it == state_.progress.end() ? 0.0 : it->second;
}

double ResearchSystem::cost(const ResearchProject& project) const {
    double factor = 1.0;
    if (state_.neolithicStart) {
        if (const Def* level = defs_.find("TechLevels", project.techLevel)) {
            factor = level->number("costFactorForNeolithicStart", 1.0);
        }
    }
    return project.baseCost * factor;
}

bool ResearchSystem::available(const ResearchProject& project) const {
    if (isDone(project.defName)) {
        return false;
    }
    return std::all_of(project.prerequisites.begin(), project.prerequisites.end(),
                       [this](const std::string& name) { return isDone(name); });
}

bool ResearchSystem::setCurrent(const std::string& project, std::string& error) {
    if (project.empty()) {
        state_.current.clear();
        return true;
    }
    const ResearchProject* p = find(project);
    if (p == nullptr) {
        error = "no Research def '" + project + "'";
        return false;
    }
    if (isDone(project)) {
        error = p->defName + " is already researched";
        return false;
    }
    for (const std::string& need : p->prerequisites) {
        if (!isDone(need)) {
            error = p->defName + " needs " + need + " first";
            return false;
        }
    }
    state_.current = project;
    return true;
}

bool ResearchSystem::unlock(const std::string& project, std::string& error) {
    if (find(project) == nullptr) {
        error = "no Research def '" + project + "'";
        return false;
    }
    if (!isDone(project)) {
        state_.done.insert(std::upper_bound(state_.done.begin(), state_.done.end(), project), project);
    }
    state_.progress.erase(project);
    if (state_.current == project) {
        state_.current.clear();
    }
    return true;
}

bool ResearchSystem::unlocked(const Def& def, std::string& error) const {
    // Without research defs (the test defs) nothing is gated.
    if (projects_.empty()) {
        return true;
    }
    for (const std::string& need : researchNeeded(def)) {
        if (!isDone(need)) {
            error = def.label() + " needs the research " + need;
            return false;
        }
    }
    return true;
}

bool ResearchSystem::unlocked(const Def& def) const {
    std::string ignored;
    return unlocked(def, ignored);
}

const ResearchBenchInfo* ResearchSystem::benchInfo(DefId thingDef) const {
    const auto it = benches_.find(thingDef.value);
    return it == benches_.end() ? nullptr : &it->second;
}

bool ResearchSystem::benchCanWork(const ResearchBenchInfo& bench) const {
    if (state_.current.empty() || !rules_.present) {
        return false;
    }
    const ResearchProject* p = find(state_.current);
    return p != nullptr && available(*p) && bench.tier >= p->benchTier;
}

double ResearchSystem::speedAt(Entity pawn, const ResearchBenchInfo& bench) const {
    int level = rules_.defaultSkillLevel;
    if (const Colonist* colonist = things_.registry().try_get<Colonist>(pawn)) {
        if (const SkillRecord* record = colonist->skill("Intellectual")) {
            level = record->level;
        }
    }
    const double skill = std::max(0.0, rules_.skillSpeedBase + rules_.skillSpeedPerLevel * level);
    return rules_.pointsPerSecond() * skill * bench.speedFactor * (traits_ != nullptr ? traits_->of(pawn).workSpeed : 1.0f);
}

bool ResearchSystem::addProgress(double points, Entity pawn, std::vector<Event>& events) {
    const ResearchProject* p = find(state_.current);
    if (p == nullptr || points <= 0.0) {
        return false;
    }
    double& have = state_.progress[p->defName];
    have += points;
    const double need = cost(*p);
    if (have < need) {
        return false;
    }
    const std::string name = p->defName;
    std::string ignored;
    unlock(name, ignored);
    Event event;
    event.type = EventType::ResearchDone;
    event.thing = pawn;
    event.def = p->def;
    event.message = name;
    event.amount = static_cast<float>(need);
    events.push_back(std::move(event));
    return true;
}

void ResearchSystem::hashState(StateHasher& h) const {
    h.add(state_.current.data(), state_.current.size());
    h.add(state_.neolithicStart);
    h.add(static_cast<uint64_t>(state_.done.size()));
    for (const std::string& name : state_.done) {
        h.add(name.data(), name.size());
    }
    h.add(static_cast<uint64_t>(state_.progress.size()));
    for (const auto& [name, points] : state_.progress) {
        h.add(name.data(), name.size());
        h.add(points);
    }
}

}  // namespace colony

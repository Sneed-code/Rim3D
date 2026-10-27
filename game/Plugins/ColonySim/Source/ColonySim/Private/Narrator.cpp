#include "ColonySim/Narrator.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Raids.h"

#include <algorithm>
#include <utility>

namespace colony {

namespace {

float narratorCurve(const std::vector<NarratorRules::Point>& curve, float x) {
    if (curve.empty()) {
        return 0.0f;
    }
    if (x <= curve.front().x) {
        return curve.front().y;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (x <= curve[i].x) {
            const float span = curve[i].x - curve[i - 1].x;
            const float t = span > 0.0f ? (x - curve[i - 1].x) / span : 1.0f;
            return curve[i - 1].y + (curve[i].y - curve[i - 1].y) * t;
        }
    }
    return curve.back().y;
}

void narratorReadCurve(const Def& def, const char* key, std::vector<NarratorRules::Point>& out) {
    const size_t n = def.count(key);
    if (n == 0) {
        return;
    }
    out.clear();
    for (size_t i = 0; i < n; ++i) {
        const std::string base = std::string(key) + "." + std::to_string(i);
        out.push_back(NarratorRules::Point{static_cast<float>(def.number((base + ".0").c_str())),
                                           static_cast<float>(def.number((base + ".1").c_str()))});
    }
}

bool narratorCategoryFrom(const std::string& name, IncidentCategory& out) {
    for (int i = 0; i < kIncidentCategoryCount; ++i) {
        if (name == incidentCategoryName(static_cast<IncidentCategory>(i))) {
            out = static_cast<IncidentCategory>(i);
            return true;
        }
    }
    return false;
}

LetterKind narratorLetterFrom(const std::string& name) {
    for (int i = 0; i <= static_cast<int>(LetterKind::PositiveEvent); ++i) {
        if (name == letterKindName(static_cast<LetterKind>(i))) {
            return static_cast<LetterKind>(i);
        }
    }
    return LetterKind::NeutralEvent;
}

}  // namespace

const char* letterKindName(LetterKind kind) {
    switch (kind) {
        case LetterKind::ThreatBig: return "ThreatBig";
        case LetterKind::NegativeEvent: return "NegativeEvent";
        case LetterKind::NeutralEvent: return "NeutralEvent";
        case LetterKind::PositiveEvent: return "PositiveEvent";
    }
    return "NeutralEvent";
}

const char* incidentCategoryName(IncidentCategory category) {
    switch (category) {
        case IncidentCategory::ThreatBig: return "ThreatBig";
        case IncidentCategory::ThreatSmall: return "ThreatSmall";
        case IncidentCategory::Misc: return "Misc";
        case IncidentCategory::FactionArrival: return "FactionArrival";
        case IncidentCategory::OrbitalVisitor: return "OrbitalVisitor";
        case IncidentCategory::Disease: return "Disease";
    }
    return "Misc";
}

NarratorSystem::NarratorSystem(RaidSystem& raids, const DefDatabase& defs, const GameTime& time, uint64_t seed)
    : raids_(raids), defs_(defs), time_(time), rng_(splitMix64(seed ^ 0x6E61727261746F72ULL)) {
    if (const Def* r = defs_.find("Rules", "NarratorRules")) {
        rules_.defaultNarrator = r->text("defaultNarrator", rules_.defaultNarrator);
        rules_.defaultDifficulty = r->text("defaultDifficulty", rules_.defaultDifficulty);
        narratorReadCurve(*r, "populationIntent", rules_.populationIntent);
        narratorReadCurve(*r, "populationIntentByDay", rules_.populationIntentByDay);
        rules_.minIncidentChanceFactor = static_cast<float>(r->number("minIncidentChanceFactor", rules_.minIncidentChanceFactor));
        rules_.checkSeconds = static_cast<int>(r->integer("checkSeconds", rules_.checkSeconds));
        rules_.checkSeconds = std::max(1, rules_.checkSeconds);
    }
    if (const Def* r = defs_.find("Rules", "RaidRules")) {
        minPoints_ = static_cast<float>(r->number("minPoints", minPoints_));
        maxPoints_ = static_cast<float>(r->number("maxPoints", maxPoints_));
    }
    for (const DefId id : defs_.idsOfKind("Incidents")) {
        const Def& def = defs_.get(id);
        Incident inc;
        inc.def = id;
        inc.name = def.defName();
        inc.label = def.label();
        if (!narratorCategoryFrom(def.text("category"), inc.category)) {
            continue;
        }
        inc.letter = narratorLetterFrom(def.text("letter"));
        inc.baseChance = static_cast<float>(def.number("baseChance", 1.0));
        inc.earliestDay = static_cast<float>(def.number("earliestDay", 0.0));
        inc.cooldownDays = static_cast<float>(def.number("cooldownDays", 0.0));
        inc.minColonists = static_cast<int>(def.integer("minColonists", 0));
        inc.hasPoints = def.has("points");
        inc.pointsFactor = static_cast<float>(def.number("points.factor", 1.0));
        inc.pointsMax = static_cast<float>(def.number("points.max", 0.0));
        inc.populationScaled = def.flag("populationScaled");
        incidents_.push_back(std::move(inc));
    }
    const Def* rulesDef = defs_.find("Rules", "NarratorRules");
    for (const DefId id : defs_.idsOfKind("Narrators")) {
        const Def& def = defs_.get(id);
        Narrator n;
        n.def = id;
        n.name = def.defName();
        n.cycle = def.has("threatCycle.onDays");
        n.onDays = static_cast<float>(def.number("threatCycle.onDays", 0.0));
        n.offDays = static_cast<float>(def.number("threatCycle.offDays", 0.0));
        n.perCycleMin = static_cast<int>(def.integer("threatCycle.incidentsPerCycle.0", 1));
        n.perCycleMax = std::max(n.perCycleMin, static_cast<int>(def.integer("threatCycle.incidentsPerCycle.1", n.perCycleMin)));
        n.firstThreatDay = static_cast<float>(def.number("threatCycle.firstThreatDay", 0.0));
        n.cycle = n.cycle && n.onDays + n.offDays > 0.0f;
        for (int c = 0; c < kIncidentCategoryCount; ++c) {
            const std::string key = std::string("mtbDays.") + incidentCategoryName(static_cast<IncidentCategory>(c));
            n.mtbDays[c] = static_cast<float>(def.number(key.c_str(), 0.0));
        }
        n.pointsFactor = static_cast<float>(def.number("pointsFactor", 1.0));
        n.randomPoints = def.count("randomPointsFactor") == 2;
        n.randomPointsMin = static_cast<float>(def.number("randomPointsFactor.0", 1.0));
        n.randomPointsMax = static_cast<float>(def.number("randomPointsFactor.1", 1.0));
        n.minChanceFactor = rules_.minIncidentChanceFactor;
        if (rulesDef != nullptr) {
            const std::string key = "minIncidentChanceFactorByNarrator." + n.name;
            n.minChanceFactor = static_cast<float>(rulesDef->number(key.c_str(), n.minChanceFactor));
        }
        narrators_.push_back(std::move(n));
    }
    if (defs_.find("Difficulties", rules_.defaultDifficulty) != nullptr) {
        state_.difficulty = rules_.defaultDifficulty;
    }
    setWorker("RaidEnemy", [this](IncidentFiring& f) { return raidWorker(f); });
}

NarratorSystem::~NarratorSystem() = default;

int64_t NarratorSystem::now() const {
    return time_.seconds;
}

float NarratorSystem::day() const {
    return static_cast<float>(static_cast<double>(now()) / static_cast<double>(kSecondsPerDay));
}

void NarratorSystem::setWorker(const std::string& incident, IncidentWorker worker) {
    for (auto& [name, w] : workers_) {
        if (name == incident) {
            w = std::move(worker);
            return;
        }
    }
    workers_.emplace_back(incident, std::move(worker));
}

bool NarratorSystem::hasWorker(const std::string& incident) const {
    return std::any_of(workers_.begin(), workers_.end(), [&](const auto& w) { return w.first == incident && w.second; });
}

const NarratorSystem::Narrator* NarratorSystem::narrator() const {
    for (const Narrator& n : narrators_) {
        if (n.name == state_.narrator) {
            return &n;
        }
    }
    return nullptr;
}

DefId NarratorSystem::narratorDef() const {
    const Narrator* n = narrator();
    return n != nullptr ? n->def : DefId{};
}

DefId NarratorSystem::difficultyDef() const {
    return state_.difficulty.empty() ? DefId{} : defs_.idOf("Difficulties", state_.difficulty);
}

const NarratorSystem::Incident* NarratorSystem::incident(const std::string& name) const {
    for (const Incident& inc : incidents_) {
        if (inc.name == name) {
            return &inc;
        }
    }
    return nullptr;
}

float NarratorSystem::threatScale() const {
    const Def* d = state_.difficulty.empty() ? nullptr : defs_.find("Difficulties", state_.difficulty);
    return d != nullptr ? static_cast<float>(d->number("threatScale", 1.0)) : 1.0f;
}

float NarratorSystem::diseaseFrequency() const {
    const Def* d = state_.difficulty.empty() ? nullptr : defs_.find("Difficulties", state_.difficulty);
    return d != nullptr ? static_cast<float>(d->number("diseaseFrequency", 1.0)) : 1.0f;
}

float NarratorSystem::threatPoints() const {
    const Narrator* n = narrator();
    return raids_.threatPoints(threatScale() * (n != nullptr ? n->pointsFactor : 1.0f));
}

float NarratorSystem::populationFactor() const {
    const Narrator* n = narrator();
    const float floor = n != nullptr ? n->minChanceFactor : rules_.minIncidentChanceFactor;
    const float intent = narratorCurve(rules_.populationIntent, static_cast<float>(raids_.colonistCount()));
    // The push fades in over the first days: neutral (1) at the start, the whole curve later.
    const float fade = narratorCurve(rules_.populationIntentByDay, day());
    return std::max(floor, 1.0f + (intent - 1.0f) * fade);
}

int64_t NarratorSystem::lastFired(const std::string& name) const {
    for (const IncidentFired& f : state_.fired) {
        if (f.incident == name) {
            return f.at;
        }
    }
    return -1;
}

bool NarratorSystem::canHappen(const Incident& inc) const {
    if (!hasWorker(inc.name) || day() < inc.earliestDay || raids_.colonistCount() < inc.minColonists) {
        return false;
    }
    const int64_t last = lastFired(inc.name);
    const auto cooldown = static_cast<int64_t>(static_cast<double>(inc.cooldownDays) * static_cast<double>(kSecondsPerDay));
    return last < 0 || now() - last >= cooldown;
}

float NarratorSystem::weight(const std::string& name) const {
    const Incident* inc = incident(name);
    if (inc == nullptr || !canHappen(*inc)) {
        return 0.0f;
    }
    return std::max(0.0f, inc->baseChance * (inc->populationScaled ? populationFactor() : 1.0f));
}

float NarratorSystem::pointsFor(const Incident& inc, float points, bool roll) {
    if (points > 0.0f) {
        return points;
    }
    if (!inc.hasPoints) {
        return 0.0f;
    }
    float p = threatPoints();
    const Narrator* n = narrator();
    if (roll && n != nullptr && n->randomPoints) {
        p *= rng_.range(n->randomPointsMin, n->randomPointsMax);
    }
    p *= inc.pointsFactor;
    if (inc.pointsMax > 0.0f) {
        p = std::min(p, inc.pointsMax);
    }
    return std::clamp(p, minPoints_, maxPoints_);
}

bool NarratorSystem::happen(const Incident& inc, float points, bool forced, std::string& error) {
    IncidentWorker* worker = nullptr;
    for (auto& [name, w] : workers_) {
        if (name == inc.name && w) {
            worker = &w;
        }
    }
    if (worker == nullptr) {
        error = "nothing can make a " + inc.label + " happen yet";
        return false;
    }
    IncidentFiring firing;
    firing.incident = inc.name;
    firing.points = pointsFor(inc, points, true);
    firing.forced = forced;
    if (!(*worker)(firing)) {
        error = firing.error.empty() ? "the " + inc.label + " could not happen" : firing.error;
        return false;
    }
    bool found = false;
    for (IncidentFired& f : state_.fired) {
        if (f.incident == inc.name) {
            f.at = now();
            found = true;
        }
    }
    if (!found) {
        state_.fired.push_back(IncidentFired{inc.name, now()});
        std::sort(state_.fired.begin(), state_.fired.end(),
                  [](const IncidentFired& a, const IncidentFired& b) { return a.incident < b.incident; });
    }
    ++state_.incidents;
    if (events_ != nullptr) {
        Event e;
        e.type = EventType::Letter;
        e.def = inc.def;
        e.message = firing.text.empty() ? inc.label : firing.text;
        e.count = static_cast<int>(inc.letter);
        e.amount = firing.points;
        e.squad = firing.squad;
        if (firing.hasCell) {
            e.cell = firing.cell;
        }
        events_->push_back(std::move(e));
    }
    return true;
}

bool NarratorSystem::fireCategory(IncidentCategory category) {
    std::vector<const Incident*> candidates;
    std::vector<float> weights;
    for (const Incident& inc : incidents_) {
        if (inc.category == category) {
            const float w = weight(inc.name);
            if (w > 0.0f) {
                candidates.push_back(&inc);
                weights.push_back(w);
            }
        }
    }
    // By weight; one that can't happen after all is dropped and another drawn.
    for (;;) {
        float total = 0.0f;
        for (const float w : weights) {
            total += w;
        }
        if (total <= 0.0f) {
            return false;
        }
        float r = rng_.unit() * total;
        size_t pick = 0;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (weights[i] <= 0.0f) {
                continue;
            }
            pick = i;
            if (r < weights[i]) {
                break;
            }
            r -= weights[i];
        }
        std::string error;
        if (happen(*candidates[pick], 0.0f, false, error)) {
            return true;
        }
        weights[pick] = 0.0f;
    }
}

void NarratorSystem::planCycle(int64_t start) {
    const Narrator* n = narrator();
    state_.cycleStart = start;
    state_.threats.clear();
    if (n == nullptr || !n->cycle) {
        return;
    }
    const int count = n->perCycleMin + static_cast<int>(rng_.below(static_cast<uint32_t>(n->perCycleMax - n->perCycleMin + 1)));
    const double onSeconds = static_cast<double>(n->onDays) * static_cast<double>(kSecondsPerDay);
    for (int i = 0; i < count; ++i) {
        state_.threats.push_back(start + static_cast<int64_t>(static_cast<double>(rng_.unit()) * onSeconds));
    }
    std::sort(state_.threats.begin(), state_.threats.end());
}

void NarratorSystem::check() {
    const Narrator* n = narrator();
    if (n == nullptr) {
        return;
    }
    const int64_t t = now();
    state_.wealth = raids_.colonyWealth();
    state_.points = threatPoints();
    // Orrin and Maren: the big threats come on a cycle, `onDays` with threats then `offDays` calm.
    if (n->cycle) {
        const auto length = static_cast<int64_t>(static_cast<double>(n->onDays + n->offDays) * static_cast<double>(kSecondsPerDay));
        if (state_.cycleStart < 0) {
            const auto first = static_cast<int64_t>(static_cast<double>(n->firstThreatDay) * static_cast<double>(kSecondsPerDay));
            planCycle(std::max(first, t));
        }
        while (!state_.threats.empty() && state_.threats.front() <= t) {
            state_.threats.erase(state_.threats.begin());
            fireCategory(IncidentCategory::ThreatBig);  // none that can happen: this one is missed
        }
        if (state_.threats.empty() && t >= state_.cycleStart + length) {
            planCycle(std::max(state_.cycleStart + length, t));
        }
    }
    // Every category by chance: RimWorld's MTB roll, check length over the mean days between.
    const float checkDays = static_cast<float>(rules_.checkSeconds) / static_cast<float>(kSecondsPerDay);
    for (int c = 0; c < kIncidentCategoryCount; ++c) {
        const auto category = static_cast<IncidentCategory>(c);
        float mtb = n->mtbDays[c];
        if ((n->cycle && category == IncidentCategory::ThreatBig) || mtb <= 0.0f) {
            continue;
        }
        if (category == IncidentCategory::Disease) {
            const float frequency = diseaseFrequency();
            if (frequency <= 0.0f) {
                continue;
            }
            mtb /= frequency;
        }
        if (rng_.unit() < checkDays / mtb) {
            fireCategory(category);
        }
    }
}

void NarratorSystem::step() {
    if (!state_.narrator.empty() && now() % rules_.checkSeconds == 0) {
        check();
    }
}

bool NarratorSystem::setNarrator(const std::string& name, const std::string& difficulty, std::string& error) {
    if (!name.empty() && defs_.find("Narrators", name) == nullptr) {
        error = "there is no narrator called " + name;
        return false;
    }
    if (!difficulty.empty() && defs_.find("Difficulties", difficulty) == nullptr) {
        error = "there is no difficulty called " + difficulty;
        return false;
    }
    if (!difficulty.empty()) {
        state_.difficulty = difficulty;
    }
    if (name != state_.narrator) {
        state_.narrator = name;
        state_.cycleStart = -1;  // the new narrator plans its own cycle at its next check
        state_.threats.clear();
    }
    if (!name.empty()) {
        state_.wealth = raids_.colonyWealth();
        state_.points = threatPoints();
    }
    return true;
}

bool NarratorSystem::fire(const std::string& name, float points, std::string& error) {
    const Incident* inc = incident(name);
    if (inc == nullptr) {
        error = "there is no incident called " + name;
        return false;
    }
    if (!(points >= 0.0f) || points > 1e6f) {
        error = "points must be 0 (from the colony) or more";
        return false;
    }
    return happen(*inc, points, true, error);
}

bool NarratorSystem::raidWorker(IncidentFiring& f) {
    // A faction that is always the colony's enemy and has raiders to send. Goodwill (C23) will let
    // others turn hostile.
    std::vector<const Def*> factions;
    for (const DefId id : defs_.idsOfKind("Factions")) {
        const Def& def = defs_.get(id);
        if (def.flag("permanentEnemy") && def.flag("canRaid") && def.count("pawnKinds") > 0) {
            factions.push_back(&def);
        }
    }
    while (!factions.empty()) {
        const size_t pick = rng_.below(static_cast<uint32_t>(factions.size()));
        const Def& faction = *factions[pick];
        std::string error;
        // F12: the way it comes is drawn by weight (sappers when walls shut the colony in, pods, thieves, kidnappers).
        const RaidStrategy strategy = raids_.pickStrategy(f.points);
        if (raids_.startRaid(faction.defName(), f.points, -1, strategy, -1.0f, error)) {
            const Squad& squad = raids_.state().squads.back();
            f.squad = squad.id;
            f.hasCell = true;
            f.cell = squad.entry;
            const bool one = squad.started == 1;
            const char* how = "";
            switch (strategy) {
                case RaidStrategy::Sappers: how = " They are digging through the walls."; break;
                case RaidStrategy::DropPods: how = " They are dropping in by pod."; break;
                case RaidStrategy::Thieves: how = " They have come to steal."; break;
                case RaidStrategy::Kidnappers: how = " They are after your wounded."; break;
                case RaidStrategy::Assault: break;
            }
            f.text = "Raid: " + std::to_string(squad.started) + (one ? " raider from the " : " raiders from the ") + faction.label() +
                     (one ? " has" : " have") + " come to attack the colony." + how;
            return true;
        }
        f.error = error;
        factions.erase(factions.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    if (f.error.empty()) {
        f.error = "no hostile faction can raid";
    }
    return false;
}

NarratorState NarratorSystem::state() const {
    NarratorState s = state_;
    s.rngState = rng_.state();
    s.rngIncrement = rng_.increment();
    return s;
}

bool NarratorSystem::restore(NarratorState s, std::string& error) {
    if (!s.narrator.empty() && defs_.find("Narrators", s.narrator) == nullptr) {
        error = "the save's narrator " + s.narrator + " is not in the data files";
        return false;
    }
    if (!s.difficulty.empty() && defs_.find("Difficulties", s.difficulty) == nullptr) {
        error = "the save's difficulty " + s.difficulty + " is not in the data files";
        return false;
    }
    for (const IncidentFired& f : s.fired) {
        if (incident(f.incident) == nullptr) {
            error = "the save names an incident the data files don't have: " + f.incident;
            return false;
        }
    }
    std::sort(s.fired.begin(), s.fired.end(), [](const IncidentFired& a, const IncidentFired& b) { return a.incident < b.incident; });
    std::sort(s.threats.begin(), s.threats.end());
    rng_.restore(s.rngState, s.rngIncrement);
    state_ = std::move(s);
    return true;
}

void NarratorSystem::hashState(StateHasher& h) const {
    h.add(state_.narrator.data(), state_.narrator.size());
    h.add(state_.difficulty.data(), state_.difficulty.size());
    h.add(rng_.state());
    h.add(rng_.increment());
    h.add(state_.cycleStart);
    h.add(static_cast<uint64_t>(state_.threats.size()));
    for (const int64_t t : state_.threats) {
        h.add(t);
    }
    h.add(static_cast<uint64_t>(state_.fired.size()));
    for (const IncidentFired& f : state_.fired) {
        h.add(f.incident.data(), f.incident.size());
        h.add(f.at);
    }
    h.add(state_.incidents);
    h.add(state_.wealth);
    h.add(state_.points);
}

}  // namespace colony
